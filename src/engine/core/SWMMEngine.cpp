/**
 * @file SWMMEngine.cpp
 * @brief Implementation of the SWMMEngine lifecycle manager.
 *
 * @see SWMMEngine.hpp
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "SWMMEngine.hpp"
#include "DateTime.hpp"
#include "SimulationContext.hpp"
#include "UnitConversion.hpp"
#include "../hydraulics/Link.hpp"
#include "../hydraulics/XSectBatch.hpp"
#include "../hydraulics/Node.hpp"
#include "../hydraulics/ForceMain.hpp"
#include <cmath>
#include <algorithm>
#include <vector>
#include "../hydraulics/TimestepController.hpp"
#include "../input/PostParseResolver.hpp"
#include "../plugins/DefaultInputPlugin.hpp"
#include "../../../include/openswmm/plugin_sdk/IPluginComponentInfo.hpp"
#include "../plugins/DefaultOutputPlugin.hpp"
#include "../plugins/DefaultReportPlugin.hpp"

#include <cstring>

// OpenMP support — graceful degradation when not available
#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#else
static inline int omp_get_max_threads() { return 1; }
static inline void omp_set_num_threads(int) {}
#endif

// Error codes (matches openswmm_engine.h SWMM_ErrorCode)
static constexpr int SWMM_OK                 = 0;
static constexpr int SWMM_ERR_MEMORY         = 1;
static constexpr int SWMM_ERR_FILE_NOT_FOUND = 2;
static constexpr int SWMM_ERR_WRONG_STATE    = 3;
static constexpr int SWMM_ERR_PARSE          = 4;
static constexpr int SWMM_ERR_PLUGIN         = 10;

namespace openswmm {

// ============================================================================
// Constructor / Destructor
// ============================================================================

SWMMEngine::SWMMEngine()
    : io_thread_(plugins_)   // IOThread needs a PluginFactory& at construction
{
    ctx_.state = EngineState::CREATED;
}

SWMMEngine::~SWMMEngine() {
    if (ctx_.state == EngineState::RUNNING ||
        ctx_.state == EngineState::ENDED) {
        close();
    }
}

// ============================================================================
// open()
// ============================================================================

int SWMMEngine::open(const char* inp_path,
                     const char* rpt_path,
                     const char* out_path,
                     const char* input_plugin_lib) noexcept {
    if (ctx_.state != EngineState::CREATED &&
        ctx_.state != EngineState::CLOSED) {
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_open: engine is not in CREATED or CLOSED state");
        return SWMM_ERR_WRONG_STATE;
    }

    // Reset context for a fresh run
    ctx_.reset();

    rpt_path_ = rpt_path ? rpt_path : "";
    out_path_ = out_path ? out_path : "";

    // Resolve input plugin: path, id:version, or fall back to default
    if (plugins_.input_plugins().empty()) {
        if (input_plugin_lib && input_plugin_lib[0] != '\0') {
            auto warn_cb = [this](const std::string& msg) {
                emit_warning(SWMM_ERR_PLUGIN, msg.c_str());
            };
            IPluginComponentInfo* info = plugins_.find_component(
                input_plugin_lib, warn_cb);
            if (info && info->has_input()) {
                IInputPlugin* ip = info->create_input_plugin();
                if (ip) {
                    ip->initialize({}, info);
                    plugins_.add_input_plugin(ip);
                }
            }
        }
        // Fall back to built-in .inp reader
        if (plugins_.input_plugins().empty()) {
            auto* ip = new DefaultInputPlugin();
            ip->initialize({}, nullptr);
            plugins_.add_input_plugin(ip);
        }
    }

    auto* input_plugin = plugins_.input_plugins().front();
    if (input_plugin->read(inp_path ? inp_path : "", ctx_) != 0) {
        return ctx_.error_code != 0 ? ctx_.error_code : SWMM_ERR_PARSE;
    }

    // Warn about unknown/skipped sections
    for (const auto& tag : input_plugin->skipped_sections()) {
        emit_warning(100,
            ("Unknown input section [" + tag + "] — skipped").c_str());
    }

    // Resolve cross-references (forward refs, final array sizing, head init)
    input::resolve_cross_references(ctx_);

    // Phase 4: load plugins listed in [PLUGINS]
    if (!ctx_.plugin_specs.empty()) {
        plugins_.load_plugins(ctx_.plugin_specs, [this](const std::string& msg) {
            emit_warning(SWMM_ERR_PLUGIN, msg.c_str());
        });
    }

    // Inject built-in output/report plugins when paths are configured.
    // These are registered AFTER user-defined [PLUGINS] so that user plugins
    // have the opportunity to override default output.
    if (!out_path_.empty()) {
        auto* op = new DefaultOutputPlugin(out_path_);
        op->initialize({}, nullptr);
        op->validate(ctx_);
        plugins_.add_output_plugin(op);
    }
    if (!rpt_path_.empty()) {
        auto* rp = new DefaultReportPlugin(rpt_path_);
        rp->initialize({}, nullptr);
        rp->validate(ctx_);
        plugins_.add_report_plugin(rp);
    }

    ctx_.state = EngineState::OPENED;
    return SWMM_OK;
}

// ============================================================================
// initialize()
// ============================================================================

int SWMMEngine::initialize() noexcept {
    if (ctx_.state != EngineState::OPENED) {
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_initialize: must call open() first");
        return SWMM_ERR_WRONG_STATE;
    }

    // Apply initial depths/flows from input (all defaults already in NodeData etc.)
    // reset_state() applies init_depth to depth/old_depth/head but volumes need
    // separate computation using node geometry tables.
    ctx_.reset_state();

    // Compute initial volumes from init_depth (matching legacy node_initState)
    for (int i = 0; i < ctx_.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double d = ctx_.nodes.init_depth[ui];
        if (d > 0.0) {
            double vol = node::getVolume(ctx_.nodes, i, d, &ctx_.tables);
            ctx_.nodes.volume[ui] = vol;
            ctx_.nodes.old_volume[ui] = vol;
        }
        // Compute full volume for surcharge detection
        double fd = ctx_.nodes.full_depth[ui];
        if (fd > 0.0) {
            ctx_.nodes.full_volume[ui] = node::getVolume(ctx_.nodes, i, fd, &ctx_.tables);
        }
    }

    // Initialize output timer
    ctx_.dt_output_remaining = ctx_.options.report_step;
    ctx_.current_date        = ctx_.options.start_date;
    ctx_.current_time        = 0.0;

    // Initialize all computational modules (batch SoA setup)
    init_modules();

    ctx_.state = EngineState::INITIALIZED;
    return SWMM_OK;
}

// ============================================================================
// start()
// ============================================================================

int SWMMEngine::start(int save_results) noexcept {
    if (ctx_.state != EngineState::INITIALIZED) {
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_start: must call initialize() first");
        return SWMM_ERR_WRONG_STATE;
    }

    save_results_ = save_results;

    // Phase 4: call prepare() on all plugins (opens output files/headers)
    if (!plugins_.empty()) {
        const int rc = plugins_.prepare_all(ctx_);
        if (rc != 0) {
            set_error(SWMM_ERR_PLUGIN, "swmm_engine_start: plugin prepare() failed");
            return SWMM_ERR_PLUGIN;
        }
    }

    // Phase 5: start the IO writer thread
    io_thread_.start();

    // Initialize averaging accumulators if rpt_averages is enabled
    if (ctx_.options.rpt_averages) {
        avg_.resize(ctx_.n_nodes(), ctx_.n_links());
    }

    ctx_.state = EngineState::RUNNING;
    return SWMM_OK;
}

// ============================================================================
// step()
// ============================================================================

int SWMMEngine::step(double* elapsed_time) noexcept {
    if (ctx_.state != EngineState::RUNNING) {
        if (elapsed_time) *elapsed_time = 0.0;
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_step: engine is not running");
        return SWMM_ERR_WRONG_STATE;
    }

    // Check if simulation is complete
    if (hydraulics::TimestepController::simulation_complete(ctx_)) {
        if (elapsed_time) *elapsed_time = 0.0;
        ctx_.state = EngineState::ENDED;
        return SWMM_OK;
    }

    // Compute next explicit timestep using CFL-based adaptive stepping
    double dt_cfl = ctx_.options.routing_step;
    if (ctx_.options.variable_step > 0.0) {
        dt_cfl = router_.getAdaptiveStep(ctx_, ctx_.options.routing_step,
                                          ctx_.options.variable_step);
        // Track max Courant number: ratio of fixed step to CFL-limited step
        if (dt_cfl > 0.0 && dt_cfl < ctx_.options.routing_step) {
            double courant = ctx_.options.routing_step / dt_cfl;
            ctx_.routing_stats.max_courant =
                std::max(ctx_.routing_stats.max_courant, courant);
        }
    }
#ifdef OPENSWMM_HAS_2D
    // Optionally constrain dt by 2D CFL hint (prevents coupling interval too large)
    if (surface_router_.isActive()) {
        double dt_cfl_2d = surface_router_.computeCflHint(ctx_);
        dt_cfl = std::min(dt_cfl, dt_cfl_2d);
    }
#endif
    const double dt_next = hydraulics::TimestepController::compute_next(ctx_, dt_cfl);

    // Fire step-begin callback
    emit_progress();
    if (callbacks_.on_step_begin) {
        callbacks_.on_step_begin(
            static_cast<void*>(this),
            ctx_.current_date,
            dt_next,
            callbacks_.step_begin_ud
        );
    }

    // Snapshot state before solving
    ctx_.save_state();

    // Reset mass balance accumulators (matching legacy massbal_initTimeStepTotals)
    resetStepMassBalance();

    // ---- Apply user-injected runtime forcings ----
    applyForcings(dt_next);

    // ---- Full simulation pipeline (matching legacy swmm_step order) ----
    // Reference: swmm5.c::execRouting() → runoff_execute() + routing_execute()

    stepRunoff(dt_next);
    stepRouting(dt_next);
    updateStatistics(dt_next);
    updateRoutingMassBalance(dt_next);
    computeFinalStorage();
    computeFinalQualityMassBalance();

    // Accumulate node/link results for time-step averaging (legacy RptFlags.averages)
    if (ctx_.options.rpt_averages) {
        accumulateAvgResults();
    }

    // ---- Clear auto-reset forcings ----
    ctx_.forcing.clear_reset_entries();

    // Advance clock (must happen before output_due check)
    hydraulics::TimestepController::advance(ctx_, dt_next);

    // Post snapshot after advance so output_due() fires correctly.
    // All subcatch/node/link state arrays still reflect the end of the
    // just-completed routing step — advance only updates timers, not state.
    postOutputSnapshot(dt_next);

    // Fire step-end callback
    if (callbacks_.on_step_end) {
        callbacks_.on_step_end(
            static_cast<void*>(this),
            ctx_.current_date,
            dt_next,
            callbacks_.step_end_ud
        );
    }

    if (elapsed_time) *elapsed_time = ctx_.current_time / hydraulics::TimestepController::SEC_PER_DAY;
    return SWMM_OK;
}

// ============================================================================
// stepRunoff() — Phase A: runoff sub-stepping
// ============================================================================

/**
 * @brief Execute Phase A: runoff sub-stepping.
 *
 * @details Runs multiple runoff substeps per routing step using variable
 *          timestep control (matching legacy runoff_getTimeStep).
 *          Updates rain gages, climate, snowmelt, runoff, infiltration,
 *          groundwater, LIDs, quality buildup/washoff.
 *
 * @param dt_routing  Routing timestep (seconds).
 */
void SWMMEngine::stepRunoff(double dt_routing) noexcept {
    // ================================================================
    // PHASE A: RUNOFF WITH INDEPENDENT CLOCK (matching legacy)
    //
    // Legacy architecture (runoff.c + routing.c):
    //   1. runoff_execute() advances on its OWN clock (300s wet / 3600s dry)
    //   2. At each routing step, addWetWeatherInflows() INTERPOLATES:
    //        f = (routingTime - OldRunoffTime) / (NewRunoffTime - OldRunoffTime)
    //        q = (1 - f) * oldRunoff + f * newRunoff
    //   3. This produces smooth lateral inflows that ramp linearly between
    //      runoff evaluation boundaries.
    //
    // Without this, computing runoff at every 5-sec routing step produces
    // 15-18% higher peak rates (same volume, sharper peaks).
    // ================================================================

    double routing_time = ctx_.current_time;  // seconds from simulation start

    // --- Phase 1: Advance runoff clock if needed ---
    // Legacy: while (NewRunoffTime < nextRoutingTime) runoff_execute();
    // Runoff must catch up past the END of this routing step so that
    // infil/evap/runoff reflect the interval STARTING at the report
    // boundary. Legacy achieves this accidentally via variable timestep
    // Legacy uses strict < (runoff.c line 164: while(NewRunoffTime < nextRoutingTime))
    double next_routing_time = routing_time + dt_routing;
    while (new_runoff_time_ < next_routing_time) {
        // Save old runoff, runon, and GW state for interpolation
        // (matching legacy subcatch_setOldState + gw oldFlow/newFlow)
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            ctx_.subcatches.old_runoff[ui]       = ctx_.subcatches.runoff[ui];
            ctx_.subcatches.old_runon_inflow[ui]  = ctx_.subcatches.runon_inflow[ui];
            ctx_.subcatches.old_gw_flow[ui]      = ctx_.subcatches.gw_flow[ui];
        }

        // Advance runoff clock
        old_runoff_time_ = new_runoff_time_;
        double abs_time = datetime::addSeconds(ctx_.options.start_date, old_runoff_time_);

        // Runoff state flags (matching legacy globals)
        bool is_raining = false;
        bool has_runoff = false;
        bool has_snow = false;

        // A1. Update rain gages and detect rainfall
        gage::updateAllGages(ctx_, abs_time);
        for (int g = 0; g < ctx_.n_gages(); ++g) {
            if (ctx_.gages.rainfall[static_cast<std::size_t>(g)] > 0.0)
                is_raining = true;
        }

        // A2. Update climate state
        int doy = datetime::dayOfYear(abs_time);
        int mon = datetime::monthOfYear(abs_time) - 1;
        int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));

        // A2a. Temperature source (before updateDailyClimate so
        //       Hargreaves/gamma/ea use the current temperature)
        if (climate_.temp_ts_index >= 0) {
            // Temperature from timeseries
            auto& tbl = ctx_.tables.tables[static_cast<std::size_t>(climate_.temp_ts_index)];
            climate_.temperature = table_lookup_cursor(tbl, abs_time);
            climate_.temperature += climate_.adjust_temp[mon];
        } else if (ctx_.options.temp_source == 2 && climate_file_.isOpen()) {
            // Temperature from climate file
            climate::DailyClimateRecord rec;
            if (climate_file_.getRecord(abs_time, rec)) {
                if (!std::isnan(rec.tmin) && !std::isnan(rec.tmax)) {
                    double tmin = rec.tmin + climate_.adjust_temp[mon];
                    double tmax = rec.tmax + climate_.adjust_temp[mon];
                    climate_.temperature = (tmin + tmax) / 2.0;
                    climate_.temp_range  = tmax - tmin;
                }
            }
        }

        climate::updateDailyClimate(climate_, doy, mon);

        // A2b. Evaporation from timeseries or climate file
        if (climate_.evap_method == climate::EvapMethod::TIMESERIES &&
            climate_.evap_ts_index >= 0) {
            auto& tbl = ctx_.tables.tables[static_cast<std::size_t>(climate_.evap_ts_index)];
            double evap_user = table_lookup_cursor(tbl, abs_time);
            climate_.evap_rate = evap_user / ucf::Ucf[ucf::EVAPRATE][unit_sys];
            climate_.evap_rate *= climate_.adjust_evap[mon];
        }
        else if (climate_.evap_method == climate::EvapMethod::PAN &&
                 climate_file_.isOpen()) {
            // Pan evaporation from climate file × monthly pan coefficient
            climate::DailyClimateRecord rec;
            if (climate_file_.getRecord(abs_time, rec) && !std::isnan(rec.evap)) {
                // rec.evap is in user units (in/day US, mm/day SI)
                climate_.evap_rate = rec.evap / ucf::Ucf[ucf::EVAPRATE][unit_sys];
                climate_.evap_rate *= ctx_.options.pan_coeff[mon];
                climate_.evap_rate *= climate_.adjust_evap[mon];
            }
        }

        // A2c. Wind speed lookup
        if (ctx_.options.wind_type == 0) {
            climate_.wind_speed = ctx_.options.wind_speed[mon];
        } else if (ctx_.options.wind_type == 1 && climate_file_.isOpen()) {
            // Wind from climate file
            climate::DailyClimateRecord rec;
            if (climate_file_.getRecord(abs_time, rec) && !std::isnan(rec.wind)) {
                climate_.wind_speed = rec.wind;
            }
        }

        // A2d. Monthly adjustment factors
        climate_.infil_factor = ctx_.adjust_hydcon[mon];

        // A2e. Recovery pattern lookup (monthly pattern for soil recovery)
        if (climate_.recovery_pat_index >= 0) {
            auto ui = static_cast<std::size_t>(climate_.recovery_pat_index);
            if (ui < ctx_.patterns.factors.size()) {
                const auto& facs = ctx_.patterns.factors[ui];
                auto umon = static_cast<std::size_t>(mon);
                climate_.recovery_factor = (umon < facs.size()) ? facs[umon] : 1.0;
            }
        }

        // A2f. Apply rainfall adjustment to all gages
        {
            double rain_factor = ctx_.adjust_rain[mon];
            if (rain_factor != 1.0) {
                for (int g = 0; g < ctx_.n_gages(); ++g)
                    ctx_.gages.rainfall[static_cast<std::size_t>(g)] *= rain_factor;
            }
        }

        // Compute variable runoff timestep
        double dt_runoff = computeRunoffTimestep(abs_time, is_raining, has_runoff, has_snow);
        if (dt_runoff <= 0.0) dt_runoff = 1.0;

        // Update runoff clock
        new_runoff_time_ = old_runoff_time_ + dt_runoff;
        // Total simulation duration in seconds
        double total_sec = (ctx_.options.end_date - ctx_.options.start_date)
                           * 86400.0;
        if (new_runoff_time_ > total_sec) {
            dt_runoff = total_sec - old_runoff_time_;
            new_runoff_time_ = total_sec;
        }

        // A2a. RDII convolution at wet weather step (matching legacy RdiiStep = WetStep).
        //      Legacy pre-computes RDII in createRdiiFile() at WetStep cadence;
        //      we compute here using each UH group's assigned rain gage.
        //      Results are buffered in rdii_ and applied during routing.
        if (!ctx_.options.ignore_rdii) {
            int rdii_month = datetime::monthOfYear(abs_time) - 1;
            rdii_.computeAll(ctx_, rdii_month, dt_runoff);
        }

        // A3. Snowmelt (pass gamma and ea for rain-on-snow calculation)
        snow_.execute(ctx_, dt_runoff, climate_.temperature,
                      climate_.wind_speed, 0.0,
                      climate_.gamma, climate_.ea);

        // A3b. Apply N-PERV/DSTORE pattern adjustments (before runoff)
        if (ctx_.has_subcatch_adj_patterns) {
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                // N-PERV pattern
                if (ui < ctx_.subcatch_n_perv_pattern.size()) {
                    int pi = ctx_.subcatch_n_perv_pattern[ui];
                    if (pi >= 0 && static_cast<std::size_t>(pi) < ctx_.patterns.factors.size()) {
                        const auto& facs = ctx_.patterns.factors[static_cast<std::size_t>(pi)];
                        auto umon = static_cast<std::size_t>(mon);
                        double f = (umon < facs.size()) ? facs[umon] : 1.0;
                        ctx_.subcatches.n_perv[ui] = ctx_.base_n_perv[ui] * f;
                    }
                }
                // DSTORE pattern
                if (ui < ctx_.subcatch_d_store_pattern.size()) {
                    int pi = ctx_.subcatch_d_store_pattern[ui];
                    if (pi >= 0 && static_cast<std::size_t>(pi) < ctx_.patterns.factors.size()) {
                        const auto& facs = ctx_.patterns.factors[static_cast<std::size_t>(pi)];
                        auto umon = static_cast<std::size_t>(mon);
                        double f = (umon < facs.size()) ? facs[umon] : 1.0;
                        ctx_.subcatches.ds_perv[ui] = ctx_.base_ds_perv[ui] * f;
                    }
                }
                // INFIL pattern: scales infil_factor for this subcatchment
                // (applied globally via climate_.infil_factor already;
                //  per-subcatchment INFIL pattern would require per-subcatch
                //  infil_factor which is a deeper refactor — noted for future)
            }
        }

        // A4. Runoff (computes subcatches.runoff[i] = newRunoff rate)
        //     Runoff solver is self-contained; output is subcatches.runoff[i].
        //     Routing picks it up via Phase 2 interpolation → nodes.runoff_inflow[].
        runoff_.execute(ctx_, dt_runoff, climate_.evap_rate,
                        climate_.infil_factor, climate_.recovery_factor, mon);

        // Update state flags for next timestep selection
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            if (ctx_.subcatches.runoff[static_cast<std::size_t>(i)] > 0.0)
                has_runoff = true;
        }

        // A4b. Accumulate runoff mass balance totals
        accumulateRunoffMassBalance(dt_runoff);

        // A4c. Surface quality: buildup + washoff
        stepSurfaceQuality(dt_runoff);

        // A5. Assemble GW coupling (pre-compute sw_head from routing state)
        assembleGWCoupling(dt_runoff);

        // A5b. Groundwater solver (reads subcatches.gw_sw_head, not nodes directly)
        stepGroundwater(dt_runoff);

        // A6. LID performance
        // A6a. Compute per-unit LID inflow from subcatchment runoff + rainfall
        for (int t = 0; t < lid_.numGroups(); ++t) {
            auto& g = lid_.group(t);
            if (g.count == 0) continue;
            for (int u = 0; u < g.count; ++u) {
                auto uu = static_cast<std::size_t>(u);
                int sc = g.subcatch_idx[uu];
                if (sc < 0 || sc >= ctx_.n_subcatches()) {
                    g.inflow[uu] = 0.0;
                    continue;
                }
                auto usc = static_cast<std::size_t>(sc);
                // Gage rainfall for this subcatchment
                int gage = ctx_.subcatches.gage[usc];
                double rain = (gage >= 0) ? ctx_.gages.rainfall[static_cast<std::size_t>(gage)] : 0.0;
                // Subcatchment runoff rate (ft/sec over subcatch area)
                double q_runoff = ctx_.subcatches.runoff[usc];
                double sc_area  = ctx_.subcatches.area[usc] * 43560.0; // acres → ft2
                double lid_area = g.area[uu];
                // Inflow = rainfall on LID + fraction of subcatch runoff routed to LID
                double q_from_sc = 0.0;
                if (sc_area > 0.0 && lid_area > 0.0) {
                    q_from_sc = q_runoff * (g.from_imperv[uu] + g.from_perv[uu])
                              * sc_area / lid_area;
                }
                g.inflow[uu] = rain + q_from_sc;
            }
        }

        lid_.execute(ctx_, dt_runoff, 0.0, climate_.evap_rate);

        // A6b. Route LID outputs back to subcatchment runoff totals
        for (int t = 0; t < lid_.numGroups(); ++t) {
            const auto& g = lid_.group(t);
            if (g.count == 0) continue;
            for (int u = 0; u < g.count; ++u) {
                auto uu = static_cast<std::size_t>(u);
                int sc = g.subcatch_idx[uu];
                if (sc < 0 || sc >= ctx_.n_subcatches()) continue;
                auto usc = static_cast<std::size_t>(sc);
                double lid_area = g.area[uu];
                double sc_area  = ctx_.subcatches.area[usc] * 43560.0;
                if (sc_area <= 0.0) continue;
                double area_ratio = lid_area / sc_area;

                // Surface runoff from LID → subcatchment runoff
                // (converted from LID rate to subcatch rate via area ratio)
                if (!g.to_perv[uu]) {
                    ctx_.subcatches.runoff[usc] += g.surface_runoff[uu] * area_ratio;
                }
                // Drain flow → subcatchment runoff (or to specific node/subcatch)
                if (g.drain_node[uu] >= 0) {
                    // Route drain to a specific node — add as external inflow
                    auto un = static_cast<std::size_t>(g.drain_node[uu]);
                    if (un < ctx_.nodes.ext_inflow.size()) {
                        ctx_.nodes.ext_inflow[un] += g.drain_flow[uu] * lid_area;
                    }
                } else {
                    // Route drain to subcatchment runoff
                    int target_sc = (g.drain_subcatch[uu] >= 0)
                                    ? g.drain_subcatch[uu] : sc;
                    auto utsc = static_cast<std::size_t>(target_sc);
                    if (utsc < ctx_.subcatches.runoff.size()) {
                        double tgt_area = ctx_.subcatches.area[utsc] * 43560.0;
                        if (tgt_area > 0.0)
                            ctx_.subcatches.runoff[utsc] += g.drain_flow[uu]
                                                          * lid_area / tgt_area;
                    }
                }

                // Apply pollutant removal to drain quality
                // drain_rmvl reduces the subcatch washoff concentration for
                // the fraction of flow passing through LID drain
                int np_lid = g.n_pollutants;
                if (np_lid > 0 && g.drain_flow[uu] > 0.0) {
                    for (int p = 0; p < np_lid; ++p) {
                        auto rmvl_idx = static_cast<std::size_t>(u * np_lid + p);
                        if (rmvl_idx >= g.drain_rmvl.size()) break;
                        double rmvl = g.drain_rmvl[rmvl_idx];
                        if (rmvl <= 0.0) continue;
                        // Reduce washoff load for this subcatchment-pollutant
                        auto conc_idx = static_cast<std::size_t>(sc * np_lid + p);
                        if (conc_idx < ctx_.subcatches.washoff_load.size()) {
                            double drain_vol = g.drain_flow[uu] * lid_area * dt_runoff;
                            double load_removed = drain_vol * rmvl;
                            // Reduce the subcatch washoff by the removed load
                            ctx_.subcatches.washoff_load[conc_idx] =
                                std::max(0.0, ctx_.subcatches.washoff_load[conc_idx]
                                             - load_removed);
                        }
                    }
                }
            }
        }

        // A7. Street sweeping buildup removal
        {
            int np = ctx_.n_pollutants();
            int nlu = ctx_.n_landuses();
            if (np > 0 && nlu > 0 && !is_raining) {
                int sweep_doy = datetime::dayOfYear(abs_time);
                int ss = ctx_.options.sweep_start;
                int se = ctx_.options.sweep_end;
                bool in_season = (ss <= se)
                    ? (sweep_doy >= ss && sweep_doy <= se)
                    : (sweep_doy >= ss || sweep_doy <= se);

                if (in_season) {
                    double dt_days = dt_runoff / ucf::SEC_PER_DAY;
                    for (int lu = 0; lu < nlu; ++lu) {
                        auto ulu = static_cast<std::size_t>(lu);
                        double interval = ctx_.landuses.sweep_interval[ulu];
                        if (interval <= 0.0) continue;
                        ctx_.landuses.last_swept[ulu] += dt_days;
                        if (ctx_.landuses.last_swept[ulu] < interval) continue;
                        ctx_.landuses.last_swept[ulu] = 0.0;

                        double removal_frac = ctx_.landuses.sweep_removal[ulu] / 100.0;
                        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                            auto ui = static_cast<std::size_t>(i);
                            auto cov_idx = ui * static_cast<std::size_t>(nlu) + ulu;
                            double frac = (cov_idx < ctx_.subcatches.coverage.size())
                                          ? ctx_.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                            if (frac <= 0.0) continue;

                            for (int p = 0; p < np; ++p) {
                                auto k = static_cast<std::size_t>(lu * np + p);
                                double effic = landuse_solver_.washoff_params[k].sweep_effic / 100.0;
                                auto bu = surface_quality_.bu_idx(i, lu, p);
                                const auto& bp = landuse_solver_.buildup_params[k];
                                double norm = (bp.normalizer == 0)
                                    ? frac * ctx_.subcatches.area[ui]
                                    : frac * ctx_.subcatches.curb_length[ui];
                                double removed_per_unit = surface_quality_.buildup[bu]
                                                          * removal_frac * effic;
                                surface_quality_.buildup[bu] =
                                    std::max(surface_quality_.buildup[bu] - removed_per_unit, 0.0);
                                ctx_.mass_balance.qual_sweeping[
                                    static_cast<std::size_t>(p)] += removed_per_unit * norm;
                            }
                        }
                    }
                }
            }
        }

        // A8. Subcatchment-to-subcatchment routing → runon_inflow[]
        //     Upstream subcatch runoff is accumulated into the receiving
        //     subcatch's runon_inflow field (does NOT mutate runoff[receiver]).
        assembleRunon();
    }

    // --- Phase 2: Interpolate runoff & GW to decomposed node inflow arrays ---
    // Matching legacy addWetWeatherInflows():
    //   f = (routingTime - OldRunoffTime) / (NewRunoffTime - OldRunoffTime)
    //   q = (1 - f) * oldRunoff + f * newRunoff
    // Writes to nodes.runoff_inflow[] and nodes.gw_inflow[] (assembled later).

    double span = new_runoff_time_ - old_runoff_time_;
    double f = (span > 0.0) ? (routing_time - old_runoff_time_) / span : 1.0;
    f = std::max(0.0, std::min(1.0, f));

    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (ctx_.subcatches.area[ui] <= 0.0) continue;

        // Interpolated runoff rate including runon from upstream subcatchments.
        // (matching legacy subcatch_getWtdOutflow)
        double q_runoff = (1.0 - f) * ctx_.subcatches.old_runoff[ui]
                        +        f  * ctx_.subcatches.runoff[ui];
        double q_runon  = (1.0 - f) * ctx_.subcatches.old_runon_inflow[ui]
                        +        f  * ctx_.subcatches.runon_inflow[ui];
        double q = q_runoff + q_runon;

        // Scatter to decomposed runoff inflow array
        int out_node = ctx_.subcatches.outlet_node[ui];
        if (out_node >= 0 && out_node < ctx_.n_nodes()) {
            ctx_.nodes.runoff_inflow[static_cast<std::size_t>(out_node)] += q;
        }

        // Interpolated GW flow (matching legacy addGroundwaterInflows)
        int gw_node = ctx_.subcatches.gw_node[ui];
        if (gw_node < 0) gw_node = ctx_.subcatches.outlet_node[ui];
        if (gw_node >= 0 && gw_node < ctx_.n_nodes()) {
            double gw_q = (1.0 - f) * ctx_.subcatches.old_gw_flow[ui]
                        +        f  * ctx_.subcatches.gw_flow[ui];
            if (std::fabs(gw_q) > 1.0e-6) {
                ctx_.nodes.gw_inflow[static_cast<std::size_t>(gw_node)] += gw_q;
            }
        }
    }
}

// ============================================================================
// computeRunoffTimestep() — variable runoff timestep matching legacy
// ============================================================================

/**
 * @brief Compute variable runoff timestep matching legacy runoff_getTimeStep().
 *
 * @details Selects wet_step or dry_step based on current conditions, then
 *          shortens to align with next rain gage boundary.
 *
 * @param abs_time     Current absolute Julian date.
 * @param is_raining   True if any gage has rainfall > 0.
 * @param has_runoff   True if any subcatchment produces runoff > 0.
 * @param has_snow     True if any subcatchment has snow depth > 0.
 * @returns Runoff timestep in seconds.
 */
double SWMMEngine::computeRunoffTimestep(double abs_time, bool is_raining,
                                         bool has_runoff, bool has_snow) noexcept {
    // Matches legacy runoff_getTimeStep() exactly:
    //   1. Start with maxStep = dry_step
    //   2. Shorten to next evaporation change date
    //   3. Shorten to next rain gage boundary
    //   4. Choose wet_step or dry_step based on conditions
    //   5. Return min(wet/dry step, maxStep)
    long max_step = static_cast<long>(ctx_.options.dry_step);

    // Shorten to next rain gage boundary
    // (matching legacy gage_getNextRainDate)
    for (int g = 0; g < ctx_.n_gages(); ++g) {
        auto ug = static_cast<std::size_t>(g);
        int ts_idx = ctx_.gages.ts_index[ug];
        if (ts_idx < 0 || ts_idx >= static_cast<int>(ctx_.tables.tables.size()))
            continue;
        auto& tbl = ctx_.tables.tables[static_cast<std::size_t>(ts_idx)];
        int idx = tbl.cursor.index;
        int n = static_cast<int>(tbl.x.size());
        if (idx < 0 || idx >= n) continue;

        // gage_getNextRainDate logic:
        // - If before startDate: return startDate
        // - If before endDate: return endDate
        // - Otherwise: return nextDate
        double interval_sec = ctx_.gages.interval_sec[ug];
        double entry_start = tbl.x[static_cast<std::size_t>(idx)];
        double entry_end = datetime::addSeconds(entry_start, interval_sec);
        double t_shifted = abs_time + datetime::OneSecond;

        double next_rain_date;
        if (t_shifted < entry_start) {
            next_rain_date = entry_start;
        } else if (t_shifted < entry_end) {
            next_rain_date = entry_end;
        } else if (idx + 1 < n) {
            next_rain_date = tbl.x[static_cast<std::size_t>(idx + 1)];
        } else {
            continue; // No more data
        }

        long secs_to_change = datetime::timeDiff(next_rain_date, abs_time);
        if (secs_to_change > 0 && secs_to_change < max_step)
            max_step = secs_to_change;
    }

    // Choose wet or dry step based on conditions
    long time_step;
    if (is_raining || has_snow || has_runoff)
        time_step = static_cast<long>(ctx_.options.wet_step);
    else
        time_step = static_cast<long>(ctx_.options.dry_step);

    // Limit to max_step (alignment constraint)
    if (time_step > max_step)
        time_step = max_step;

    return static_cast<double>(time_step);
}

// ============================================================================
// accumulateRunoffMassBalance() — runoff mass balance for one substep
// ============================================================================

/**
 * @brief Accumulate runoff mass balance totals for one substep.
 *
 * @details Sums rainfall, evaporation, infiltration, and runoff volumes
 *          over all subcatchments for a single runoff substep. Also updates
 *          per-subcatchment statistics (max runoff, precipitation volume,
 *          runoff volume).
 *
 * @param dt_runoff  Runoff substep duration (seconds).
 */
void SWMMEngine::accumulateRunoffMassBalance(double dt_runoff) noexcept {
    //      Sum over all subcatchments for this runoff substep
    //
    //      Units:
    //        rainfall   = in/hr (project rain units)
    //        evap_loss  = ft/sec (depth rate on pervious)
    //        infil_loss = ft/sec (depth rate on pervious)
    //        runoff     = cfs (flow)
    //        area       = acres
    //
    //      Volume conversions:
    //        rain_vol (ft³) = rainfall(in/hr) / UCF(RAINFALL) * area(ac) * ACRES_TO_FT2 * dt(sec)
    //        infil_vol(ft³) = infil_rate(ft/sec) * area(ac) * ACRES_TO_FT2 * dt(sec)
    //        runoff_vol(ft³)= runoff(cfs) * dt(sec)
    //
    const double RAIN_TO_FTSEC = 1.0 / ucf::UCF(ucf::RAINFALL, ctx_.options);
    constexpr double ACRES_TO_FT2  = ucf::ACRES_TO_FT2;

    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        double area_ft2 = ctx_.subcatches.area[ui] * ACRES_TO_FT2;

        // Rainfall volume (ft³) — rainfall is already in ft/sec (internal units)
        double rain_ftsec = ctx_.subcatches.rainfall[ui];
        ctx_.mass_balance.runoff_rainfall += rain_ftsec * area_ft2 * dt_runoff;

        // Evaporation volume (ft³) — evap_loss is ft/sec
        ctx_.mass_balance.runoff_evap +=
            ctx_.subcatches.evap_loss[ui] * area_ft2 * dt_runoff;

        // Infiltration volume (ft³) — infil_loss is area-averaged rate (ft/sec)
        // Already accounts for pervious fraction (Vinfil / dt / total_area)
        ctx_.mass_balance.runoff_infil +=
            ctx_.subcatches.infil_loss[ui] * area_ft2 * dt_runoff;

        // Surface runoff volume (ft³) — runoff is cfs
        ctx_.mass_balance.runoff_runoff +=
            ctx_.subcatches.runoff[ui] * dt_runoff;

        // Subcatchment-level statistics
        if (ctx_.subcatches.runoff[ui] > ctx_.subcatches.stat_max_runoff[ui])
            ctx_.subcatches.stat_max_runoff[ui] = ctx_.subcatches.runoff[ui];
        ctx_.subcatches.stat_precip_vol[ui] +=
            rain_ftsec * area_ft2 * dt_runoff;
        ctx_.subcatches.stat_runoff_vol[ui] +=
            ctx_.subcatches.runoff[ui] * dt_runoff;
    }
}

// ============================================================================
// stepSurfaceQuality() — surface quality buildup + washoff for one substep
// ============================================================================

/**
 * @brief Compute surface quality buildup and washoff for one substep.
 *
 * @details Matching legacy surfqual_getBuildup + surfqual_getWashoff.
 *          Iterates over subcatchments, pollutants, and land uses to
 *          accumulate buildup and compute washoff loads.
 *
 * @param dt_runoff  Runoff substep duration (seconds).
 */
void SWMMEngine::stepSurfaceQuality(double dt_runoff) noexcept {
    //      Matching legacy surfqual_getBuildup + surfqual_getWashoff
    if (ctx_.n_pollutants() > 0 && ctx_.n_landuses() > 0) {
        int np = ctx_.n_pollutants();
        int nlu = ctx_.n_landuses();
        constexpr double MIN_RUNOFF_RATE = 1.0e-9; // ft/sec threshold
        double dt_days = dt_runoff / ucf::SEC_PER_DAY;

        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            double q = ctx_.subcatches.runoff[ui];  // cfs
            double area_ac = ctx_.subcatches.area[ui]; // acres

            for (int p = 0; p < np; ++p) {
                auto sq_idx = ui * static_cast<std::size_t>(np)
                              + static_cast<std::size_t>(p);
                double total_washoff_load = 0.0; // mass/sec

                // Iterate over land uses weighted by coverage
                // Buildup is now stored PER LAND USE: bu_idx(i, lu, p)
                for (int lu = 0; lu < nlu; ++lu) {
                    auto cov_idx = ui * static_cast<std::size_t>(nlu)
                                   + static_cast<std::size_t>(lu);
                    double frac = (cov_idx < ctx_.subcatches.coverage.size())
                                  ? ctx_.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                    if (frac <= 0.0) continue;

                    auto k = static_cast<std::size_t>(lu * np + p);
                    const auto& bp = landuse_solver_.buildup_params[k];
                    const auto& wp = landuse_solver_.washoff_params[k];
                    auto bu = surface_quality_.bu_idx(i, lu, p);

                    // Normalizer for this land use (absolute mass = buildup * norm)
                    double norm = (bp.normalizer == 0)
                        ? frac * area_ac : frac * ctx_.subcatches.curb_length[ui];

                    // --- Buildup accumulation (matching legacy surfqual_getBuildup)
                    if (bp.type != landuse::BuildupType::NONE) {
                        double mass = surface_quality_.buildup[bu];  // per-normalizer-unit
                        double days = 0.0;
                        double c0 = bp.coeff[0], c1 = bp.coeff[1], c2 = bp.coeff[2];
                        // Inverse: mass → equivalent days
                        if (mass > 0.0) {
                            switch (bp.type) {
                                case landuse::BuildupType::POWER:
                                    days = (c1 * c2 > 0.0) ? std::pow(mass / c1, 1.0 / c2) : 0.0;
                                    break;
                                case landuse::BuildupType::EXPON:
                                    days = (c0 * c1 > 0.0 && mass < c0) ? -std::log(1.0 - mass / c0) / c1 : 0.0;
                                    break;
                                case landuse::BuildupType::SATUR:
                                    days = (c0 > mass) ? mass * c2 / (c0 - mass) : bp.max_days;
                                    break;
                                default: break;
                            }
                        }
                        days += dt_days;
                        // Forward: days → new mass
                        double new_mass = 0.0;
                        if (days > 0.0) {
                            switch (bp.type) {
                                case landuse::BuildupType::POWER:
                                    new_mass = std::min(c1 * std::pow(days, c2), c0);
                                    break;
                                case landuse::BuildupType::EXPON:
                                    new_mass = c0 * (1.0 - std::exp(-c1 * days));
                                    break;
                                case landuse::BuildupType::SATUR:
                                    new_mass = (c2 + days > 0.0) ? c0 * days / (c2 + days) : 0.0;
                                    break;
                                default: break;
                            }
                        }
                        double buildup_change = new_mass - mass;
                        surface_quality_.buildup[bu] = new_mass;

                        // Track net buildup increase for mass balance
                        if (buildup_change > 0.0) {
                            ctx_.mass_balance.qual_surface_buildup[
                                static_cast<std::size_t>(p)] += buildup_change * norm;
                        }
                    }

                    // --- Washoff computation (matching legacy surfqual_getWashoff)
                    if (wp.type != landuse::WashoffType::NONE && q > MIN_RUNOFF_RATE) {
                        double buildup = surface_quality_.buildup[bu];

                        double load = 0.0; // mass/sec (absolute)
                        switch (wp.type) {
                            case landuse::WashoffType::EMC:
                                load = wp.coeff * q * frac;
                                break;
                            case landuse::WashoffType::EXPON:
                                if (buildup > 0.0)
                                    load = wp.coeff * std::pow(q, wp.expon) * buildup * norm;
                                break;
                            case landuse::WashoffType::RATING:
                                load = wp.coeff * std::pow(q, wp.expon) * frac;
                                break;
                            default: break;
                        }

                        // Cap washoff to available buildup (mass/sec <= total_mass / dt)
                        double avail = buildup * norm;
                        double max_load = (dt_runoff > 0.0) ? avail / dt_runoff : 0.0;
                        if (load > max_load && wp.type != landuse::WashoffType::EMC)
                            load = max_load;

                        // Reduce per-landuse buildup by washoff amount
                        if (norm > 0.0) {
                            double washed = load * dt_runoff / norm;
                            surface_quality_.buildup[bu] =
                                std::max(surface_quality_.buildup[bu] - washed, 0.0);
                        }

                        // Apply BMP removal
                        load *= (1.0 - wp.bmp_effic / 100.0);

                        total_washoff_load += load;
                    }
                } // end land use loop

                // Ponded quality tracking (matching legacy findPondedLoads)
                // Tracks pollutant mass in standing water between events
                if (sq_idx < ctx_.subcatches.ponded_qual.size()) {
                    double rainfall_rate = ctx_.gages.rainfall.empty() ? 0.0
                        : (ctx_.subcatches.gage[ui] >= 0 &&
                           ctx_.subcatches.gage[ui] < static_cast<int>(ctx_.gages.rainfall.size()))
                          ? ctx_.gages.rainfall[static_cast<size_t>(ctx_.subcatches.gage[ui])]
                          : 0.0;
                    double c_rain = ctx_.pollutants.c_rain[static_cast<size_t>(p)];
                    double v_rain = rainfall_rate * area_ac * dt_runoff;
                    double w_rain = c_rain * v_rain;

                    // Total ponded mass = previous + rain deposition
                    double w_ponded = ctx_.subcatches.ponded_qual[sq_idx] + w_rain;

                    if (w_ponded > 0.0 && q > MIN_RUNOFF_RATE) {
                        double v_outflow = q * dt_runoff;
                        double v_total = v_rain + v_outflow;
                        double c_ponded = (v_total > 0.0) ? w_ponded / v_total : 0.0;

                        // Runoff carries ponded mass out
                        double w_outflow = c_ponded * v_outflow;
                        w_ponded -= w_outflow;
                        total_washoff_load += w_outflow / dt_runoff;
                    }

                    // Store remaining ponded mass
                    ctx_.subcatches.ponded_qual[sq_idx] = std::max(w_ponded, 0.0);
                }

                // Convert washoff load to concentration (mass/ft3)
                double conc = 0.0;
                if (q > MIN_RUNOFF_RATE && total_washoff_load > 0.0)
                    conc = total_washoff_load / q;

                ctx_.subcatches.conc[sq_idx] = conc;

                // Update quality mass balance and per-subcatch total load
                if (total_washoff_load > 0.0) {
                    double mass = total_washoff_load * dt_runoff;
                    ctx_.mass_balance.qual_runoff_load[
                        static_cast<std::size_t>(p)] += mass;
                    // Accumulate per-subcatchment washoff load
                    ctx_.subcatches.total_load[sq_idx] += mass;
                }
            } // end pollutant loop
        } // end subcatch loop
    }
}

// ============================================================================
// stepGroundwater() — groundwater computation for one substep
// ============================================================================

/**
 * @brief Execute groundwater computation for one substep.
 *
 * @details Runs the groundwater ODE solver for all subcatchments, then
 *          scatters GW lateral flow contributions to receiving nodes
 *          and accumulates groundwater inflow mass balance.
 *
 * @param dt_runoff  Runoff substep duration (seconds).
 */
void SWMMEngine::stepGroundwater(double dt_runoff) noexcept {
    int ns = ctx_.n_subcatches();

    // GW surface water head and available node flow are pre-computed by
    // assembleGWCoupling() before this function is called.

    // Build per-subcatchment FracPerv and pervious evap rate
    // Legacy: FracPerv = subcatch_getFracPerv(j)
    //         MaxEvap = Evap.rate * FracPerv
    //         AvailEvap = max(MaxEvap - evap, 0)
    gw_frac_perv_.assign(static_cast<std::size_t>(ns), 0.0);
    gw_perv_evap_.assign(static_cast<std::size_t>(ns), 0.0);
    for (int i = 0; i < ns; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double total_area = ctx_.subcatches.area[ui] * ucf::ACRES_TO_FT2;
        if (total_area <= 0.0) continue;
        double frac_perv = 1.0 - ctx_.subcatches.frac_imperv[ui];
        gw_frac_perv_[ui] = std::max(frac_perv, 0.0);
        gw_perv_evap_[ui] = ctx_.subcatches.evap_loss[ui];
    }

    // Pass actual infiltration rate to groundwater (upper zone percolation input).
    // sw_head is read from the pre-assembled subcatches.gw_sw_head[].
    groundwater_.execute(ctx_, dt_runoff, climate_.evap_rate,
                         ctx_.subcatches.infil_loss.data(),
                         ctx_.subcatches.gw_sw_head.data(),
                         gw_frac_perv_.data(), gw_perv_evap_.data());

    // Store GW flow rates in subcatch state for Phase 2 interpolation.
    // GW flow is in ft/sec (rate per unit area).
    // Convert to CFS: q_cfs = gw_flow * area_ft2
    // Note: do NOT scatter to lat_flow here — Phase 2 does interpolated scatter.
    // Note: do NOT accumulate routing_gw_inflow here — updateRoutingMassBalance does it.
    for (int i = 0; i < ns; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double gw_rate = groundwater_.state().gw_flow[ui]; // ft/sec
        double area_ft2 = ctx_.subcatches.area[ui] * ucf::ACRES_TO_FT2;
        ctx_.subcatches.gw_flow[ui] = gw_rate * area_ft2; // CFS
    }
}

// ============================================================================
// stepRouting() — Phase B: hydraulic and quality routing
// ============================================================================

/**
 * @brief Execute Phase B: hydraulic and quality routing.
 *
 * @details Evaluates controls, computes inflows (external, DWF, RDII),
 *          runs hydraulic routing, inlet capture, culvert control,
 *          exfiltration, and quality transport.
 *
 * @param dt_routing  Routing timestep (seconds).
 */
void SWMMEngine::stepRouting(double dt_routing) noexcept {
    // Track routing time-step statistics
    ctx_.routing_stats.update(dt_routing);
    ctx_.routing_stats.record_step_bin(dt_routing);

    // ================================================================
    // PHASE B: ROUTING (once per routing step)
    // Reference: routing_execute() in legacy routing.c
    // ================================================================

    // B0a. Check if between routing events — skip routing if so
    //       (matching legacy isBetweenEvents() in routing.c)
    if (isBetweenEvents(ctx_.current_date)) {
        // Advance next_event_ index past expired events
        while (next_event_ < static_cast<int>(ctx_.events.size()) &&
               ctx_.current_date > ctx_.events[static_cast<size_t>(next_event_)].end) {
            next_event_++;
        }
        return;
    }

    // B0. Half-step mass balance update (P8-G12)
    // Legacy: massbal_updateRoutingTotals(routingStep/2) at start of routing
    // (mass balance accumulators updated with half the step's contribution)

    // B1. Evaluate control rules (P8-G18: orifice gradual open/close)
    //     Pump on/off hysteresis applied via target_setting
    controls_.evaluate(ctx_, ctx_.current_time, dt_routing);
    // Apply setting transitions: move setting toward target_setting
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double target = ctx_.links.target_setting[uj];
        double current = ctx_.links.setting[uj];
        if (target != current) {
            // Gradual transition (P8-G18): use orifice open/close rate
            // Legacy: link_setSetting() applies orate for time-based ramp
            double rate = ctx_.links.orate[uj];
            if (rate > 0.0 && dt_routing > 0.0) {
                double delta = rate * dt_routing;
                if (target > current) {
                    ctx_.links.setting[uj] = std::min(current + delta, target);
                } else {
                    ctx_.links.setting[uj] = std::max(current - delta, target);
                }
            } else {
                // Instantaneous transition (rate == 0 or non-orifice links)
                ctx_.links.setting[uj] = target;
            }
        }
    }

    // B2. Compute all inflow sources into decomposed arrays, then assemble.
    //     Each source writes to its own per-node buffer; assembleLateralInflows()
    //     sums them all into nodes.lat_flow[] for the routing solver.
    ctx_.nodes.clearInflowSources();

    // Legacy getDateTime() adds +1ms offset to avoid boundary rounding issues
    // with pattern lookups at exact hour/day boundaries.
    double routing_date = datetime::addSeconds(ctx_.options.start_date,
                                               ctx_.current_time + 0.001);
    inflow_.computeAll(ctx_, routing_date, dt_routing);

    // B2a. RDII inflows — apply pre-computed values from wet weather step.
    if (!ctx_.options.ignore_rdii) {
        rdii_.applyRdiiInflows(ctx_);
    }

    // B2b. Interface file inflows (from upstream model coupling)
    iface_.readInflows(ctx_, ctx_.current_date);

    // B2c. Assemble all decomposed sources into nodes.lat_flow[]
    assembleLateralInflows();

#ifdef OPENSWMM_HAS_2D
    // B2c. Pre-routing: update outfall boundary heads from 2D surface state.
    //      Must happen after setOutfallDepths() (called inside router_.step)
    //      but the 2D pre-routing hook modifies outfall heads from 2D state.
    surface_router_.updateOutfallsPreRouting(ctx_);
#endif

    // B2d. Check if system is in steady state — skip routing if so
    //       (matching legacy isInSteadyState() in routing.c)
    int action_count = controls_.lastActionCount();
    if (isInSteadyState(action_count)) {
        return;
    }

    // B3. Hydraulic routing (batch xsect geometry → batch momentum)
    //     Includes: conduit flow, pump/orifice/weir/outlet flow,
    //     divider logic, outfall boundary conditions
    // Pass non-conduit flow callback so pumps/orifices/weirs/outlets are
    // computed INSIDE the DW Picard iteration loop (matching legacy findLinkFlows).
    // The callback applies under-relaxation for iterations > 0, matching legacy
    // findNonConduitFlow() lines 435-438 in dynwave.c.
    constexpr double OMEGA_NC = 0.5; // under-relaxation for non-conduit flows
    // Pre-fetch the non-conduit index list (built once at init)
    const auto& nc_idx = hydstruct_.nonConduitIndices();
    auto& dw = router_.dwSolver();
    auto non_conduit_fn = [this, &nc_idx, &dw](SimulationContext& ctx, double dt, int step) {
        auto& links = ctx.links;

        // Save previous iteration flows for under-relaxation
        // Only save non-conduit flows (much smaller than iterating all links)
        thread_local std::vector<double> q_prev;
        q_prev.resize(nc_idx.size());
        for (std::size_t k = 0; k < nc_idx.size(); ++k) {
            q_prev[k] = links.flow[static_cast<std::size_t>(nc_idx[k])];
        }

        // Compute all non-conduit link flows (sets links.flow for non-conduits)
        hydstruct_.computeAllFlows(ctx, dt);

        // Apply under-relaxation and scatter to nodes
        for (std::size_t k = 0; k < nc_idx.size(); ++k) {
            int j = nc_idx[k];
            auto uj = static_cast<std::size_t>(j);

            double q_new = links.flow[uj];
            double q_last = q_prev[k];

            // Under-relaxation for iterations > 0 (matching legacy dynwave.c:435-438)
            // Pumps are exempt from under-relaxation
            if (step > 0 && links.type[uj] != LinkType::PUMP) {
                q_new = (1.0 - 0.5) * q_last + 0.5 * q_new;
                // Don't allow flow to change direction without first being ~0
                if (q_new * q_last < 0.0)
                    q_new = 0.001 * (q_new >= 0.0 ? 1.0 : -1.0);
            }
            links.flow[uj] = q_new;

            // Scatter to node inflow/outflow (matching legacy updateNodeFlows)
            int n1 = links.node1[uj], n2 = links.node2[uj];
            if (n1 < 0 || n2 < 0) continue;
            auto un1 = static_cast<std::size_t>(n1);
            auto un2 = static_cast<std::size_t>(n2);
            if (q_new >= 0.0) {
                ctx.nodes.outflow[un1] += q_new;
                ctx.nodes.inflow[un2]  += q_new;
            } else {
                ctx.nodes.inflow[un1]  -= q_new;
                ctx.nodes.outflow[un2] -= q_new;
            }

             // Non-conduit dqdh — orifices compute their own in computeOrificeFlows;
            // for other types, approximate as Q/(2*head).
            double dqdh = links.dqdh[uj];
            if (dqdh == 0.0 && std::fabs(q_new) > 0.0001) {
                double h1_abs = ctx.nodes.depth[un1] + ctx.nodes.invert_elev[un1];
                double h2_abs = ctx.nodes.depth[un2] + ctx.nodes.invert_elev[un2];
                double dh = std::fabs(h1_abs - h2_abs);
                if (dh > 0.001) dqdh = std::fabs(q_new) / (2.0 * dh);
            }

            // Scatter dqdh to nodes (matching legacy updateNodeFlows lines 566-575)
            dw.nodeState(n1).sumdqdh += dqdh;
            dw.nodeState(n2).sumdqdh += dqdh;

            // Non-conduit surface area (matching legacy findNonConduitSurfArea)
            // Orifices: surfArea = equivalent_length * width_at_depth / 2
            // Weirs/outlets: 0 (SWMM4 compatibility)
            double sa = 0.0;
            if (links.type[uj] == LinkType::ORIFICE) {
                // Legacy: surfArea = xsect_getAofY(xsect, yFull*setting) for BOTTOM
                //         or width*length for SIDE. Approximate:
                sa = links.xsect_a_full[uj] * links.setting[uj];
            }
            double sa1 = sa / 2.0, sa2 = sa / 2.0;
            if (ctx.nodes.type[un1] == NodeType::STORAGE) sa1 = 0.0;
            if (ctx.nodes.type[un2] == NodeType::STORAGE) sa2 = 0.0;
            dw.nodeState(n1).new_surf_area += sa1;
            dw.nodeState(n2).new_surf_area += sa2;
        }
    };
    int iters = router_.step(ctx_, dt_routing, climate_.evap_rate, non_conduit_fn);
    ctx_.routing_stats.update_iterations(iters, iters < ctx_.options.max_trials);

#ifdef OPENSWMM_HAS_2D
    // B3+. Post-routing: compute 2D↔1D coupling exchange, update rainfall,
    //      advance CVODE solver, transfer outfall discharges to 2D cells.
    surface_router_.advancePostRouting(ctx_, dt_routing, ctx_.current_time);
#endif

    // B3a. Inlet capture (street inlet HEC-22 calculations)
    inlet_.computeAll(ctx_, dt_routing);

    // B3b. Culvert inlet control (FHWA HEC-5 equations)
    //      Uses pre-built culvert_links_ (populated in initHydraulics)
    if (!culvert_links_.empty()) {
        culvert::batchComputeInletControl(
            culvert_links_.data(),
            static_cast<int>(culvert_links_.size()),
            ctx_);
    }

    // B3c. Exfiltration (storage node Green-Ampt seepage)
    exfil_.computeAll(ctx_, dt_routing);

    // B4. Non-conduit link flows are now computed inside the DW Picard loop
    //     via the non_conduit_fn callback passed to router_.step().

    // B4a. Write outfall results to interface file (if output is active)
    iface_.writeOutfallResults(ctx_, ctx_.current_date);

    // B5. Water quality routing (P8-G13: fill stub bodies)
    if (ctx_.n_pollutants() > 0) {
        quality_.execute(ctx_, dt_routing);
    }
}

// ============================================================================
// updateStatistics() — update node and link statistics after routing
// ============================================================================

/**
 * @brief Update node and link statistics after routing.
 *
 * @details Updates max depth, max overflow, flooding duration/volume for
 *          nodes, and max flow, max velocity, max filling, surcharge
 *          duration, and volume conveyed for links.
 *
 * @param dt_routing  Routing timestep (seconds).
 */
void SWMMEngine::updateStatistics(double dt_routing) noexcept {
    const int np = ctx_.n_pollutants();

    // B6. Update statistics (P8-G11)
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        // Depth statistics
        double cur_depth = ctx_.nodes.depth[uj];
        ctx_.nodes.stat_sum_depth[uj] += cur_depth;
        if (cur_depth > ctx_.nodes.stat_max_depth[uj]) {
            ctx_.nodes.stat_max_depth[uj] = cur_depth;
            ctx_.nodes.stat_max_depth_date[uj] = ctx_.current_date;
        }
        if (cur_depth > ctx_.nodes.stat_max_rpt_depth[uj])
            ctx_.nodes.stat_max_rpt_depth[uj] = cur_depth;
        if (ctx_.nodes.overflow[uj] > ctx_.nodes.stat_max_overflow[uj]) {
            ctx_.nodes.stat_max_overflow[uj] = ctx_.nodes.overflow[uj];
            ctx_.nodes.stat_max_overflow_date[uj] = ctx_.current_date;
        }
        if (ctx_.nodes.overflow[uj] > 0.0) {
            ctx_.nodes.stat_time_flooded[uj] += dt_routing;
            ctx_.nodes.stat_vol_flooded[uj] += ctx_.nodes.overflow[uj] * dt_routing;
        }

        // Node surcharge tracking
        double full_d = ctx_.nodes.full_depth[uj];
        if (full_d > 0.0 && cur_depth > full_d) {
            ctx_.nodes.stat_time_surcharged[uj] += dt_routing;
            double surcharge_h = cur_depth - full_d;
            if (surcharge_h > ctx_.nodes.stat_max_surcharge_height[uj])
                ctx_.nodes.stat_max_surcharge_height[uj] = surcharge_h;
        }

        // Node inflow statistics (matching legacy stats_updateNodeStats)
        double lat = ctx_.nodes.lat_flow[uj];
        double total_inflow = ctx_.nodes.inflow[uj];
        if (std::fabs(lat) > ctx_.nodes.stat_max_lat_inflow[uj])
            ctx_.nodes.stat_max_lat_inflow[uj] = std::fabs(lat);
        if (total_inflow > ctx_.nodes.stat_max_total_inflow[uj]) {
            ctx_.nodes.stat_max_total_inflow[uj] = total_inflow;
            ctx_.nodes.stat_max_inflow_date[uj] = ctx_.current_date;
        }
        ctx_.nodes.stat_lat_inflow_vol[uj]   += std::fabs(lat) * dt_routing;
        ctx_.nodes.stat_total_inflow_vol[uj] += total_inflow * dt_routing;
        // For outfall nodes, outflow = inflow by definition (matching legacy
        // massbal.c line 587: NodeOutflow[j] += Node[j].inflow * tStep).
        // For non-storage terminal nodes (degree==0), same treatment.
        if (ctx_.nodes.type[uj] == NodeType::OUTFALL ||
            (ctx_.nodes.degree[uj] == 0 &&
             ctx_.nodes.type[uj] != NodeType::STORAGE)) {
            ctx_.nodes.stat_total_outflow_vol[uj] += total_inflow * dt_routing;
        } else {
            ctx_.nodes.stat_total_outflow_vol[uj] += ctx_.nodes.outflow[uj] * dt_routing;
            if (ctx_.nodes.volume[uj] <= ctx_.nodes.full_volume[uj])
                ctx_.nodes.stat_total_outflow_vol[uj] += ctx_.nodes.overflow[uj] * dt_routing;
        }

        // Outfall statistics
        if (ctx_.nodes.type[uj] == NodeType::OUTFALL) {
            double qi = ctx_.nodes.inflow[uj];
            if (qi > 0.001) { // MIN_RUNOFF_FLOW threshold
                ctx_.nodes.stat_outfall_avg_flow[uj] += qi;
                if (qi > ctx_.nodes.stat_outfall_max_flow[uj])
                    ctx_.nodes.stat_outfall_max_flow[uj] = qi;
                ctx_.nodes.stat_outfall_periods[uj]++;
            }
            // Outfall pollutant loads: load += inflow * conc * dt
            if (np > 0) {
                auto base = uj * static_cast<std::size_t>(np);
                for (int p = 0; p < np; ++p) {
                    auto idx = base + static_cast<std::size_t>(p);
                    if (idx < ctx_.nodes.stat_total_load.size() &&
                        idx < ctx_.nodes.conc.size()) {
                        ctx_.nodes.stat_total_load[idx] +=
                            qi * ctx_.nodes.conc[idx] * dt_routing;
                    }
                }
            }
        }
    }
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double q = std::fabs(ctx_.links.flow[uj]);
        if (q > ctx_.links.stat_max_flow[uj]) {
            ctx_.links.stat_max_flow[uj] = q;
            ctx_.links.stat_max_flow_date[uj] = ctx_.current_date;
        }

        // Volume conveyed
        ctx_.links.stat_vol_flow[uj] += q * dt_routing;

        // Velocity (matching legacy link_getVelocity in link.c)
        // Uses geometric area from cross-section shape at current depth,
        // guards on depth <= 0.01, and divides flow by barrels.
        double vel = 0.0;
        if (ctx_.links.type[uj] == LinkType::CONDUIT) {
            XSectParams xs = link::buildXSectParams(ctx_.links, uj);
            vel = link::getVelocity(xs, q, ctx_.links.depth[uj],
                                     ctx_.links.barrels[uj]);
        }
        if (vel > ctx_.links.stat_max_veloc[uj])
            ctx_.links.stat_max_veloc[uj] = vel;

        // Filling ratio (depth / full depth)
        double y_full = ctx_.links.xsect_y_full[uj];
        double d = ctx_.links.depth[uj];
        double filling = (y_full > 0.0) ? d / y_full : 0.0;
        if (filling > ctx_.links.stat_max_filling[uj])
            ctx_.links.stat_max_filling[uj] = filling;

        // Surcharge duration tracking
        if (d >= y_full && y_full > 0.0)
            ctx_.links.stat_time_surcharged[uj] += dt_routing;

        // Conduit surcharge detail tracking (upstream/downstream/both)
        if (ctx_.links.type[uj] == LinkType::CONDUIT && y_full > 0.0) {
            int n1 = ctx_.links.node1[uj];
            int n2 = ctx_.links.node2[uj];
            bool up_full = false, dn_full = false;
            if (n1 >= 0 && n1 < ctx_.n_nodes()) {
                auto un1 = static_cast<std::size_t>(n1);
                double elev1 = ctx_.nodes.invert_elev[un1] + ctx_.links.offset1[uj] + y_full;
                up_full = (ctx_.nodes.head[un1] >= elev1);
            }
            if (n2 >= 0 && n2 < ctx_.n_nodes()) {
                auto un2 = static_cast<std::size_t>(n2);
                double elev2 = ctx_.nodes.invert_elev[un2] + ctx_.links.offset2[uj] + y_full;
                dn_full = (ctx_.nodes.head[un2] >= elev2);
            }
            if (up_full) ctx_.links.stat_time_full_upstream[uj] += dt_routing;
            if (dn_full) ctx_.links.stat_time_full_dnstream[uj] += dt_routing;
            if (up_full && dn_full) ctx_.links.stat_time_full_both[uj] += dt_routing;
            // Capacity limited: flow exceeds full normal flow
            if (q > ctx_.links.q_full[uj] && ctx_.links.q_full[uj] > 0.0)
                ctx_.links.stat_time_capacity_limited[uj] += dt_routing;
        }

        // Flow classification counter
        int fc = static_cast<int>(ctx_.links.flow_class[uj]);
        if (fc >= 0 && fc < LinkData::N_FLOW_CLASSES) {
            auto fc_idx = uj * LinkData::N_FLOW_CLASSES + static_cast<std::size_t>(fc);
            if (fc_idx < ctx_.links.stat_flow_class.size())
                ++ctx_.links.stat_flow_class[fc_idx];
        }

        // Normal flow limited / inlet control counters
        if (ctx_.links.normal_flow_limited[uj]) {
            ++ctx_.links.stat_norm_ltd[uj];
            ctx_.links.normal_flow_limited[uj] = false;  // reset for next step
        }
        if (ctx_.links.inlet_control[uj]) {
            ++ctx_.links.stat_inlet_ctrl[uj];
            ctx_.links.inlet_control[uj] = false;
        }

        // Pump utilization statistics
        if (ctx_.links.type[uj] == LinkType::PUMP) {
            bool is_on = (ctx_.links.setting[uj] > 0.0 && q > 0.0);
            bool was_on = ctx_.links.stat_pump_was_on[uj];

            if (is_on != was_on) {
                ctx_.links.stat_pump_cycles[uj]++;
                ctx_.links.stat_pump_was_on[uj] = is_on;
            }
            if (is_on) {
                ctx_.links.stat_pump_on_time[uj] += dt_routing;
                ctx_.links.stat_pump_volume[uj] += q * dt_routing;
            }
        }

        // Link pollutant loads: load += |flow| * conc * dt
        if (np > 0 && q > 0.0) {
            auto base = uj * static_cast<std::size_t>(np);
            for (int p = 0; p < np; ++p) {
                auto idx = base + static_cast<std::size_t>(p);
                if (idx < ctx_.links.stat_total_load.size() &&
                    idx < ctx_.links.conc.size()) {
                    ctx_.links.stat_total_load[idx] +=
                        q * ctx_.links.conc[idx] * dt_routing;
                }
            }
        }
    }
}

// ============================================================================
// updateRoutingMassBalance() — routing mass balance totals after routing
// ============================================================================

/**
 * @brief Update routing mass balance totals after routing.
 *
 * @details Accumulates flooding, outfall outflow, evaporation/seepage losses,
 *          link seepage losses, and wet weather inflow volumes for the
 *          routing mass balance.
 *
 * @param dt_routing  Routing timestep (seconds).
 */
void SWMMEngine::updateRoutingMassBalance(double dt_routing) noexcept {
    // B7. Mass balance update (P8-G12: routing totals)
    //     Accumulate ALL flow paths matching legacy massbal_updateRoutingTotals
    ctx_.mass_balance.step_flooding  = 0.0;
    ctx_.mass_balance.step_outflow   = 0.0;

    // Accumulate DWF, GW, RDII, and external inflow volumes from step accumulators
    // (step accumulators are set during Inflow::computeAll / RDIISolver::computeAll
    //  and stepRunoff Phase 2 for GW)
    ctx_.mass_balance.routing_dry_weather += ctx_.mass_balance.step_dw_inflow * dt_routing;
    ctx_.mass_balance.routing_gw_inflow   += ctx_.mass_balance.step_gw_inflow * dt_routing;
    ctx_.mass_balance.routing_rdii        += ctx_.mass_balance.step_rdii_inflow * dt_routing;
    ctx_.mass_balance.routing_external    += ctx_.mass_balance.step_ext_inflow * dt_routing;

    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);

        // Flooding (overflow at non-outfall nodes)
        if (ctx_.nodes.overflow[uj] > 0.0 &&
            ctx_.nodes.type[uj] != NodeType::OUTFALL) {
            ctx_.mass_balance.routing_flooding += ctx_.nodes.overflow[uj] * dt_routing;
            ctx_.mass_balance.step_flooding    += ctx_.nodes.overflow[uj];
        }

        // Outfall outflow: at outfall nodes, inflow becomes system outflow
        if (ctx_.nodes.type[uj] == NodeType::OUTFALL) {
            double q_outfall = ctx_.nodes.inflow[uj];
            ctx_.mass_balance.routing_outflow += q_outfall * dt_routing;
            ctx_.mass_balance.step_outflow    += q_outfall;

            // Route outfall discharge to subcatchment if configured
            // (matching legacy getOutfallRunon() in subcatch.c)
            int sc = ctx_.nodes.outfall_route_to[uj];
            if (sc >= 0 && sc < ctx_.n_subcatches() && q_outfall > 0.0) {
                // Add as runon to target subcatchment (converted to depth/sec)
                auto usc = static_cast<std::size_t>(sc);
                double area = ctx_.subcatches.area[usc];
                if (area > 0.0) {
                    ctx_.subcatches.runon_inflow[usc] += q_outfall / area;
                }
            }
        }
        // Non-storage terminal nodes (degree == 0) also count as outflow
        else if (ctx_.nodes.degree[uj] == 0 &&
                 ctx_.nodes.type[uj] != NodeType::STORAGE) {
            ctx_.mass_balance.routing_outflow += ctx_.nodes.inflow[uj] * dt_routing;
            ctx_.mass_balance.step_outflow    += ctx_.nodes.inflow[uj];
        }

        // Node evaporation and seepage losses
        ctx_.mass_balance.routing_evap_loss += ctx_.nodes.losses[uj] * dt_routing;
    }

    // Accumulate link evaporation and seepage losses
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx_.links.type[uj] == LinkType::CONDUIT) {
            int barrels = std::max(ctx_.links.barrels[uj], 1);
            ctx_.mass_balance.routing_evap_loss +=
                ctx_.links.evap_loss_rate[uj] * barrels * dt_routing;
            ctx_.mass_balance.routing_seep_loss +=
                ctx_.links.seep_loss_rate[uj] * barrels * dt_routing;
        }
    }

    // Wet weather inflow: computed directly from decomposed runoff_inflow array
    // (replaces the previous back-calculation: total_lat - DWF - GW - RDII - ext).
    {
        double runoff_q = 0.0;
        double user_q_total = 0.0;
        for (int j = 0; j < ctx_.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx_.nodes.runoff_inflow[uj] > 0.0)
                runoff_q += ctx_.nodes.runoff_inflow[uj];
            if (ctx_.nodes.user_lat_flow[uj] > 0.0)
                user_q_total += ctx_.nodes.user_lat_flow[uj];
        }
        if (runoff_q > 0.0) {
            ctx_.mass_balance.routing_wet_weather += runoff_q * dt_routing;
        }
        if (user_q_total > 0.0) {
            ctx_.mass_balance.routing_forcing_inflow += user_q_total * dt_routing;
        }
    }

    // Quality routing mass balance (matching legacy massbal_updateRoutingTotals)
    int np = ctx_.n_pollutants();
    if (np > 0) {
        for (int j = 0; j < ctx_.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);

            // Wet weather quality inflow: lateral flow × concentration
            if (ctx_.nodes.lat_flow[uj] > 0.0) {
                for (int p = 0; p < np; ++p) {
                    auto qi = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
                    if (qi < ctx_.nodes.conc.size()) {
                        double load = ctx_.nodes.lat_flow[uj] *
                                      ctx_.nodes.conc[qi] * dt_routing;
                        if (load > 0.0)
                            ctx_.mass_balance.qual_routing_wet[static_cast<std::size_t>(p)] += load;
                    }
                }
            }

            // Quality outflow at outfalls: inflow × concentration
            if (ctx_.nodes.type[uj] == NodeType::OUTFALL && ctx_.nodes.inflow[uj] > 0.0) {
                for (int p = 0; p < np; ++p) {
                    auto qi = uj * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
                    if (qi < ctx_.nodes.conc.size()) {
                        double load = ctx_.nodes.inflow[uj] *
                                      ctx_.nodes.conc[qi] * dt_routing;
                        if (load > 0.0)
                            ctx_.mass_balance.qual_routing_outflow[static_cast<std::size_t>(p)] += load;
                    }
                }
            }
        }
    }
}

// ============================================================================
// computeFinalStorage() — final storage volumes for mass balance
// ============================================================================

/**
 * @brief Compute final storage volumes for runoff and routing mass balance.
 *
 * @details Computes runoff final storage from ponded depth on subareas,
 *          and routing final storage from node + link volumes.
 */
void SWMMEngine::computeFinalStorage() noexcept {
    // B8a. Compute runoff final storage: ponded depth × subarea area (ft³)
    //      Matches legacy subcatch_getStorage()
    {
        const auto& soa = runoff_.soa();
        ctx_.mass_balance.runoff_final_store = 0.0;
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            double fi = soa.imperv_pct[ui];
            double fp = 1.0 - fi;
            double f0 = fi * soa.imperv0_pct[ui];
            double f1 = fi * (1.0 - soa.imperv0_pct[ui]);
            double area = soa.area[ui]; // ft²
            ctx_.mass_balance.runoff_final_store +=
                (soa.depth_imperv0[ui] * f0
                 + soa.depth_imperv1[ui] * f1
                 + soa.depth_perv[ui] * fp) * area;
        }
    }

    // B8. Compute routing final storage for mass balance
    //     Sum node volumes + link volumes (matching legacy)
    ctx_.mass_balance.routing_final_storage = 0.0;
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        ctx_.mass_balance.routing_final_storage +=
            ctx_.nodes.volume[static_cast<std::size_t>(j)];
    }
    for (int j = 0; j < ctx_.n_links(); ++j) {
        ctx_.mass_balance.routing_final_storage +=
            ctx_.links.volume[static_cast<std::size_t>(j)];
    }
}

// ============================================================================
// computeFinalQualityMassBalance() — final quality buildup mass balance
// ============================================================================

/**
 * @brief Compute final quality buildup mass for quality mass balance.
 *
 * @details Sums the current buildup across all subcatchments for each
 *          pollutant and stores it in the mass balance final buildup array.
 */
void SWMMEngine::computeFinalQualityMassBalance() noexcept {
    // B9. Quality mass balance: compute final buildup (sum over land uses)
    // Matches legacy massbal_getBuildup: sum landFactor[lu].buildup[p] * norm
    if (ctx_.n_pollutants() > 0) {
        int np = ctx_.n_pollutants();
        int nlu = std::max(ctx_.n_landuses(), 1);
        for (int p = 0; p < np; ++p) {
            double total = 0.0;
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                double area_ac = ctx_.subcatches.area[ui];
                for (int lu = 0; lu < nlu; ++lu) {
                    auto cov_idx = ui * static_cast<std::size_t>(nlu)
                                   + static_cast<std::size_t>(lu);
                    double frac = (cov_idx < ctx_.subcatches.coverage.size())
                                  ? ctx_.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                    if (frac <= 0.0) continue;

                    auto k = static_cast<std::size_t>(lu * np + p);
                    const auto& bp = landuse_solver_.buildup_params[k];
                    double norm = (bp.normalizer == 0)
                        ? frac * area_ac : frac * ctx_.subcatches.curb_length[ui];

                    auto bu = surface_quality_.bu_idx(i, lu, p);
                    if (bu < surface_quality_.buildup.size())
                        total += surface_quality_.buildup[bu] * norm;
                }
            }
            ctx_.mass_balance.qual_final_buildup[static_cast<std::size_t>(p)] = total;
        }
    }
}

// ============================================================================
// postOutputSnapshot() — post snapshot to IO thread if output is due
// ============================================================================

/**
 * @brief Post a snapshot to the IO thread if output is due.
 *
 * @details Checks the output timer and, if a report interval has elapsed,
 *          builds a SimulationSnapshot from the current context state and
 *          posts it to the IO thread for asynchronous writing.
 */
void SWMMEngine::postOutputSnapshot(double /*dt_step*/) noexcept {
    // Post snapshot to IO thread if output is due (Phase 5)
    // Called AFTER advance(). State arrays (lat_flow, depth, flow, infil etc.)
    // still reflect the just-completed routing step. current_date is at the
    // report boundary.
    if (hydraulics::TimestepController::output_due(ctx_)) {
        if (save_results_ && !plugins_.empty()) {
            // Build a SimulationSnapshot from the current context.
            // Vector assignments are O(n) memcpy; the I/O thread consumes
            // and destroys the snapshot after processing.
            SimulationSnapshot snap;
            snap.sim_time         = ctx_.current_date;
            snap.node_count       = ctx_.n_nodes();
            snap.link_count       = ctx_.n_links();
            snap.subcatch_count   = ctx_.n_subcatches();
            snap.gage_count       = ctx_.n_gages();
            snap.pollut_count     = ctx_.n_pollutants();
            snap.flow_units_code  = static_cast<int>(ctx_.options.flow_units);

            // Copy node state
            snap.nodes.depth          = ctx_.nodes.depth;
            snap.nodes.head           = ctx_.nodes.head;
            snap.nodes.volume         = ctx_.nodes.volume;
            snap.nodes.lateral_inflow = ctx_.nodes.lat_flow;
            snap.nodes.total_inflow   = ctx_.nodes.inflow;
            snap.nodes.overflow       = ctx_.nodes.overflow;

            // Copy link state (apply direction to flow/velocity for display)
            snap.links.flow     = ctx_.links.flow;
            snap.links.depth    = ctx_.links.depth;
            snap.links.volume   = ctx_.links.volume;

            // Compute link velocity, capacity, and apply direction
            // Matching legacy link_getResults() + link_getVelocity():
            //   velocity = flow / barrels / xsect_getAofY(depth)
            //   capacity = xsect_getAofY(depth) / aFull  (area ratio, not depth ratio)
            {
                const int nL = ctx_.n_links();
                snap.links.velocity.resize(static_cast<std::size_t>(nL));
                snap.links.capacity.resize(static_cast<std::size_t>(nL));
                for (int j = 0; j < nL; ++j) {
                    auto uj = static_cast<std::size_t>(j);
                    double q = ctx_.links.flow[uj];
                    double d = ctx_.links.depth[uj];
                    double veloc = 0.0;
                    double cap   = 0.0;

                    auto lt = ctx_.links.type[uj];
                    if (lt == LinkType::CONDUIT) {
                        XSectParams xs = link::buildXSectParams(ctx_.links, uj);
                        veloc = link::getVelocity(xs, q, d, ctx_.links.barrels[uj]);
                        cap   = link::getCapacity(xs, d);
                    } else {
                        cap = ctx_.links.setting[uj];
                    }

                    // Apply link direction (matching legacy link_getResults)
                    int dir = ctx_.links.direction[uj];
                    snap.links.flow[uj]     = q * dir;
                    snap.links.velocity[uj] = veloc * dir;
                    snap.links.capacity[uj] = cap;
                }
            }

            // When rpt_averages is enabled, overwrite node/link snapshot arrays
            // with time-step-averaged values (subcatchments stay point-in-time,
            // matching legacy behavior).
            if (ctx_.options.rpt_averages) {
                applyAvgResults(snap);
            }

            // Copy subcatchment state
            // Rainfall, infil, and evap all come from the most recent runoff
            // evaluation and are self-consistent. Using ctx_.subcatches.rainfall
            // (set during runoff) rather than querying the gage at report time
            // ensures rain and infil are time-aligned.
            snap.subcatch.rainfall = ctx_.subcatches.rainfall;
            snap.subcatch.evap     = ctx_.subcatches.evap_loss;
            snap.subcatch.infil    = ctx_.subcatches.infil_loss;
            snap.subcatch.runoff   = ctx_.subcatches.runoff;
            snap.subcatch.gw_flow  = ctx_.subcatches.gw_flow;

            // GW elevation and soil moisture (matching legacy subcatch_getResults):
            //   gw_elev   = (bottomElev + lowerDepth) * UCF(LENGTH)
            //   soil_moist = theta (upper zone moisture content)
            {
                const int nS = ctx_.n_subcatches();
                snap.subcatch.gw_elev.resize(static_cast<std::size_t>(nS), 0.0);
                snap.subcatch.soil_moist.resize(static_cast<std::size_t>(nS), 0.0);
                const auto& gw = groundwater_.state();
                for (int s = 0; s < nS; ++s) {
                    auto us = static_cast<std::size_t>(s);
                    int aq = ctx_.subcatches.gw_aquifer[us];
                    if (aq >= 0) {
                        auto uaq = static_cast<std::size_t>(aq);
                        double bot = ctx_.aquifers.bottom_elev[uaq];
                        snap.subcatch.gw_elev[us] = bot + gw.lower_depth[us];
                        snap.subcatch.soil_moist[us] = gw.theta[us];
                    }
                }
            }

            // Per-subcatch snow depth (from snow pack state)
            {
                const int nS = ctx_.n_subcatches();
                snap.subcatch.snow_depth.resize(static_cast<std::size_t>(nS), 0.0);
                const auto& soa = snow_.state();
                for (int s = 0; s < nS; ++s) {
                    auto us = static_cast<std::size_t>(s);
                    if (ctx_.subcatches.snowpack[us] < 0) continue;
                    double fi = ctx_.subcatches.frac_imperv[us];
                    double sn = (us < soa.snn.size()) ? soa.snn[us] : 0.0;
                    double fArea[3] = { sn * fi, (1.0 - sn) * fi, 1.0 - fi };
                    int base = s * snow::N_SUBAREAS;
                    double sd = 0.0;
                    for (int k = 0; k < snow::N_SUBAREAS; ++k) {
                        auto uk = static_cast<std::size_t>(base + k);
                        if (uk < soa.wsnow.size())
                            sd += soa.wsnow[uk] * fArea[k];
                    }
                    snap.subcatch.snow_depth[us] = sd;
                }
            }

            // System-level results
            snap.sys_temperature = climate_.temperature;

            // Area-weighted average rainfall across subcatchments
            // (matching legacy output_saveSubcatchResults accumulation)
            // Uses snap.subcatch.rainfall (queried at report time above)
            {
                double total_rain = 0.0;
                double total_area = 0.0;
                for (int s = 0; s < ctx_.n_subcatches(); ++s) {
                    auto us = static_cast<std::size_t>(s);
                    double a = ctx_.subcatches.area[us];
                    total_rain += snap.subcatch.rainfall[us] * a;
                    total_area += a;
                }
                snap.sys_rainfall = (total_area > 0.0)
                    ? total_rain / total_area : 0.0;
            }

            // Area-weighted average snow depth across all subcatchments
            {
                double total_snow = 0.0;
                double total_area = 0.0;
                for (int s = 0; s < ctx_.n_subcatches(); ++s) {
                    auto us = static_cast<std::size_t>(s);
                    double a = ctx_.subcatches.area[us];
                    total_snow += snap.subcatch.snow_depth[us] * a;
                    total_area += a;
                }
                snap.sys_snow_depth = (total_area > 0.0) ? total_snow / total_area : 0.0;
            }
            snap.sys_pet = climate_.evap_rate;

            // Area-weighted averages of evap and infil; total runoff
            // Legacy adds GW evaporation (gw->evapLoss) to system evap
            // (output.c:612-613): SYS_EVAP += gw->evapLoss * UCF(EVAPRATE) * area
            {
                double tot_evap = 0.0, tot_infil = 0.0, tot_runoff = 0.0;
                double total_area = 0.0;
                const auto& gw = groundwater_.state();
                for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                    auto ui = static_cast<std::size_t>(i);
                    double a = ctx_.subcatches.area[ui];
                    tot_evap   += ctx_.subcatches.evap_loss[ui] * a;
                    // Add GW evaporation (matching legacy output.c:612-613)
                    if (ctx_.subcatches.gw_aquifer[ui] >= 0 &&
                        ui < gw.upper_evap.size()) {
                        double gw_evap = gw.upper_evap[ui] + gw.lower_evap[ui];
                        tot_evap += gw_evap * a;
                    }
                    tot_infil  += ctx_.subcatches.infil_loss[ui] * a;
                    tot_runoff += ctx_.subcatches.runoff[ui];
                    total_area += a;
                }
                snap.sys_evap   = (total_area > 0.0) ? tot_evap / total_area : 0.0;
                snap.sys_infil  = (total_area > 0.0) ? tot_infil / total_area : 0.0;
                snap.sys_runoff = tot_runoff;
            }

            snap.sys_dw_inflow  = ctx_.mass_balance.step_dw_inflow;
            snap.sys_gw_inflow  = ctx_.mass_balance.step_gw_inflow;
            snap.sys_ii_inflow  = ctx_.mass_balance.step_rdii_inflow;
            snap.sys_ext_inflow = ctx_.mass_balance.step_ext_inflow;
            snap.sys_flooding   = ctx_.mass_balance.step_flooding;
            snap.sys_outflow    = ctx_.mass_balance.step_outflow;

            // Total storage volume (sum of node + link volumes)
            {
                double tot_store = 0.0;
                for (int j = 0; j < ctx_.n_nodes(); ++j)
                    tot_store += ctx_.nodes.volume[static_cast<std::size_t>(j)];
                for (int j = 0; j < ctx_.n_links(); ++j)
                    tot_store += ctx_.links.volume[static_cast<std::size_t>(j)];
                snap.sys_storage = tot_store;
            }

            // Attach name table pointers (valid for lifetime of ctx_)
            snap.node_ids     = &ctx_.node_names.names();
            snap.link_ids     = &ctx_.link_names.names();
            snap.subcatch_ids = &ctx_.subcatch_names.names();
            snap.gage_ids     = &ctx_.gage_names.names();
            snap.pollut_names = &ctx_.pollutant_names.names();

            io_thread_.post(std::move(snap));
        }
        hydraulics::TimestepController::reset_output_timer(ctx_);
    }
}

// ============================================================================
// accumulateAvgResults() — accumulate node/link state for time-step averaging
// ============================================================================

void SWMMEngine::accumulateAvgResults() noexcept {
    // Node accumulators: depth, head, volume, lat_inflow, total_inflow, overflow
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        avg_.node_depth[uj]        += ctx_.nodes.depth[uj];
        avg_.node_head[uj]         += ctx_.nodes.head[uj];
        avg_.node_volume[uj]       += ctx_.nodes.volume[uj];
        avg_.node_lat_inflow[uj]   += ctx_.nodes.lat_flow[uj];
        avg_.node_total_inflow[uj] += ctx_.nodes.inflow[uj];
        avg_.node_overflow[uj]     += ctx_.nodes.overflow[uj];
    }

    // Link accumulators: flow, depth, velocity, volume, capacity
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double q = ctx_.links.flow[uj];
        double d = ctx_.links.depth[uj];

        avg_.link_flow[uj]   += q;
        avg_.link_depth[uj]  += d;
        avg_.link_volume[uj] += ctx_.links.volume[uj];

        auto lt = ctx_.links.type[uj];
        if (lt == LinkType::CONDUIT) {
            XSectParams xs = link::buildXSectParams(ctx_.links, uj);
            avg_.link_velocity[uj] += link::getVelocity(xs, q, d, ctx_.links.barrels[uj]);
            avg_.link_capacity[uj] += link::getCapacity(xs, d);
        } else {
            // Non-conduit capacity (pump speed, regulator opening):
            // Legacy preserves last value — multiply by (n_steps+1) so that
            // division by n_steps in applyAvgResults yields the last value.
            avg_.link_capacity[uj] = ctx_.links.setting[uj] * (avg_.n_steps + 1);
        }
    }

    ++avg_.n_steps;
}

// ============================================================================
// applyAvgResults() — write averaged node/link values into a snapshot
// ============================================================================

void SWMMEngine::applyAvgResults(SimulationSnapshot& snap) noexcept {
    if (avg_.n_steps <= 0) return;
    double inv = 1.0 / avg_.n_steps;

    // Average node results
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        snap.nodes.depth[uj]          = avg_.node_depth[uj] * inv;
        snap.nodes.head[uj]           = avg_.node_head[uj] * inv;
        snap.nodes.volume[uj]         = avg_.node_volume[uj] * inv;
        snap.nodes.lateral_inflow[uj] = avg_.node_lat_inflow[uj] * inv;
        snap.nodes.total_inflow[uj]   = avg_.node_total_inflow[uj] * inv;
        snap.nodes.overflow[uj]       = avg_.node_overflow[uj] * inv;
    }

    // Average link results (direction applied here, not during accumulation)
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        int dir = ctx_.links.direction[uj];
        snap.links.flow[uj]     = avg_.link_flow[uj] * inv * dir;
        snap.links.depth[uj]    = avg_.link_depth[uj] * inv;
        snap.links.velocity[uj] = avg_.link_velocity[uj] * inv * dir;
        snap.links.volume[uj]   = avg_.link_volume[uj] * inv;
        snap.links.capacity[uj] = avg_.link_capacity[uj] * inv;
    }

    // Reset for next report period
    avg_.reset();
}

int SWMMEngine::end() noexcept {
    if (ctx_.state != EngineState::RUNNING &&
        ctx_.state != EngineState::ENDED) {
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_end: engine must be running or ended");
        return SWMM_ERR_WRONG_STATE;
    }

    // Compute final GW storage for mass balance reporting
    {
        auto& gw = groundwater_.state();
        ctx_.mass_balance.gw_final_storage = 0.0;
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (gw.total_depth[ui] <= 0.0) continue;
            double upper_d = gw.total_depth[ui] - gw.lower_depth[ui];
            double vol = gw.theta[ui] * upper_d + gw.porosity[ui] * gw.lower_depth[ui];
            double area = ctx_.subcatches.area[ui] * 43560.0;
            ctx_.mass_balance.gw_final_storage += vol * area;
        }
    }

    // Build routing time step histogram for report
    ctx_.routing_stats.build_histogram();

#ifdef OPENSWMM_HAS_2D
    // Finalize 2D surface routing module (release CVODE resources)
    surface_router_.finalize();
#endif

    // Phase 5: drain and join the IO thread (all writes must complete first)
    io_thread_.stop();

    // Phase 4: finalize plugins (flush/close output files)
    if (!plugins_.empty()) {
        plugins_.finalize_all(ctx_);
    }

    ctx_.state = EngineState::ENDED;
    return SWMM_OK;
}

// ============================================================================
// report()
// ============================================================================

int SWMMEngine::report() noexcept {
    if (ctx_.state != EngineState::ENDED) {
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_report: must call end() first");
        return SWMM_ERR_WRONG_STATE;
    }

    // Phase 4: write summary reports via all report plugins
    if (!plugins_.empty()) {
        plugins_.write_summary_all(ctx_);
    }

    ctx_.state = EngineState::REPORTED;
    return SWMM_OK;
}

// ============================================================================
// close()
// ============================================================================

int SWMMEngine::close() noexcept {
    // Stop IO thread if still running (safe to call even if already stopped)
    io_thread_.stop();

    // Close routing interface files
    iface_.closeFiles();

    // Unload all dynamically loaded plugin libraries
    plugins_.unload_all();

    ctx_.state = EngineState::CLOSED;
    return SWMM_OK;
}

// ============================================================================
// applyForcings — inject user-specified runtime forcing values
// ============================================================================

void SWMMEngine::applyForcings(double dt) noexcept {
    auto& f = ctx_.forcing;

    // ---- Node lateral inflow forcing → write to user_lat_flow ----
    // Transient ForcingData lateral inflows are merged into user_lat_flow
    // so that the existing stepRunoff application path and mass balance
    // tracking in updateRoutingMassBalance handle them uniformly.
    for (int i = 0; i < ctx_.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (f.node_lat_inflow_mode[ui] == ForcingMode::OVERRIDE) {
            ctx_.nodes.user_lat_flow[ui] = f.node_lat_inflow_value[ui];
        } else if (f.node_lat_inflow_mode[ui] == ForcingMode::ADD) {
            ctx_.nodes.user_lat_flow[ui] += f.node_lat_inflow_value[ui];
        }
    }

    // ---- Node head boundary forcing (outfalls only) ----
    for (int i = 0; i < ctx_.n_nodes(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (f.node_head_boundary_mode[ui] == ForcingMode::NONE) continue;
        if (ctx_.nodes.type[ui] != NodeType::OUTFALL) continue;
        ctx_.nodes.outfall_param[ui] = f.node_head_boundary_value[ui];
        ctx_.nodes.outfall_type[ui]  = OutfallType::FIXED;
    }

    // ---- Gage rainfall forcing (before runoff substeps read gages) ----
    for (int g = 0; g < ctx_.n_gages(); ++g) {
        auto ug = static_cast<std::size_t>(g);
        if (f.gage_rainfall_mode[ug] == ForcingMode::OVERRIDE) {
            ctx_.gages.rainfall[ug] = f.gage_rainfall_value[ug];
        } else if (f.gage_rainfall_mode[ug] == ForcingMode::ADD) {
            ctx_.gages.rainfall[ug] += f.gage_rainfall_value[ug];
        }
    }

    // ---- Subcatchment rainfall forcing (bypasses gage lookup) ----
    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (f.subcatch_rainfall_mode[ui] == ForcingMode::OVERRIDE) {
            ctx_.subcatches.rainfall[ui] = f.subcatch_rainfall_value[ui];
        } else if (f.subcatch_rainfall_mode[ui] == ForcingMode::ADD) {
            ctx_.subcatches.rainfall[ui] += f.subcatch_rainfall_value[ui];
        }
    }

    // ---- Subcatchment evaporation forcing ----
    // (applied here; runoff solver will use subcatches.evap_rate if set)
    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (f.subcatch_evap_mode[ui] == ForcingMode::OVERRIDE) {
            ctx_.subcatches.evap_loss[ui] = f.subcatch_evap_value[ui];
        } else if (f.subcatch_evap_mode[ui] == ForcingMode::ADD) {
            ctx_.subcatches.evap_loss[ui] += f.subcatch_evap_value[ui];
        }
    }

    // ---- Link setting forcing (pump/orifice/weir control override) ----
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (f.link_setting_mode[uj] == ForcingMode::OVERRIDE) {
            ctx_.links.setting[uj]        = f.link_setting_value[uj];
            ctx_.links.target_setting[uj] = f.link_setting_value[uj];
        } else if (f.link_setting_mode[uj] == ForcingMode::ADD) {
            ctx_.links.setting[uj]        += f.link_setting_value[uj];
            ctx_.links.target_setting[uj]  = ctx_.links.setting[uj];
        }
    }

    // ---- Link flow forcing ----
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (f.link_flow_mode[uj] == ForcingMode::OVERRIDE) {
            ctx_.links.flow[uj] = f.link_flow_value[uj];
        } else if (f.link_flow_mode[uj] == ForcingMode::ADD) {
            ctx_.links.flow[uj] += f.link_flow_value[uj];
        }
    }

    // ---- Node quality mass flux forcing (transient via ForcingData) ----
    // For OVERRIDE mode, the concentration is set directly.
    // For ADD mode, mass_rate (mass/sec) is added as concentration delta.
    int np = ctx_.n_pollutants();
    if (np > 0) {
        for (int i = 0; i < ctx_.n_nodes(); ++i) {
            for (int p = 0; p < np; ++p) {
                auto flat = static_cast<std::size_t>(i) * static_cast<std::size_t>(np)
                          + static_cast<std::size_t>(p);
                if (f.node_quality_mode[flat] == ForcingMode::OVERRIDE) {
                    ctx_.nodes.conc[flat] = f.node_quality_value[flat];
                } else if (f.node_quality_mode[flat] == ForcingMode::ADD) {
                    ctx_.nodes.conc[flat] += f.node_quality_value[flat];
                    ctx_.mass_balance.routing_forcing_qual_inflow[
                        static_cast<std::size_t>(p)] +=
                        f.node_quality_value[flat] * dt;
                }
            }
        }

        // ---- Persistent user quality mass flux (user_conc_mass_flux) ----
        // Applied as additive mass source each step, analogous to user_lat_flow.
        // mass_rate is in mass/sec; converted to concentration delta via volume.
        if (!ctx_.nodes.user_conc_mass_flux.empty()) {
            for (int i = 0; i < ctx_.n_nodes(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                double vol = ctx_.nodes.volume[ui];
                for (int p = 0; p < np; ++p) {
                    auto flat = ui * static_cast<std::size_t>(np)
                              + static_cast<std::size_t>(p);
                    double mass_rate = ctx_.nodes.user_conc_mass_flux[flat];
                    if (mass_rate == 0.0) continue;

                    // Convert mass flux to concentration: C += (mass_rate * dt) / volume
                    if (vol > 0.0) {
                        ctx_.nodes.conc[flat] += mass_rate * dt / vol;
                    }
                    // Track cumulative forced quality mass (mass = rate * dt)
                    ctx_.mass_balance.routing_forcing_qual_inflow[
                        static_cast<std::size_t>(p)] += mass_rate * dt;
                }
            }
        }
    }
}

// ============================================================================
// Callback registration
// ============================================================================

void SWMMEngine::set_progress_callback(SWMM_ProgressCallback cb, void* ud) noexcept {
    callbacks_.on_progress  = cb;
    callbacks_.progress_ud  = ud;
}

void SWMMEngine::set_warning_callback(SWMM_WarningCallback cb, void* ud) noexcept {
    callbacks_.on_warning  = cb;
    callbacks_.warning_ud  = ud;
}

void SWMMEngine::set_step_begin_callback(SWMM_StepBeginCallback cb, void* ud) noexcept {
    callbacks_.on_step_begin   = cb;
    callbacks_.step_begin_ud   = ud;
}

void SWMMEngine::set_step_end_callback(SWMM_StepEndCallback cb, void* ud) noexcept {
    callbacks_.on_step_end  = cb;
    callbacks_.step_end_ud  = ud;
}

// ============================================================================
// Private helpers
// ============================================================================

void SWMMEngine::set_error(int code, const char* message) noexcept {
    ctx_.error_code    = code;
    ctx_.error_message = message ? message : "";
    ctx_.state         = EngineState::ERROR_STATE;
    emit_warning(code, message);
}

void SWMMEngine::emit_warning(int code, const char* message) noexcept {
    if (callbacks_.on_warning) {
        callbacks_.on_warning(
            static_cast<void*>(this),
            code,
            message,
            callbacks_.warning_ud
        );
    }
}

void SWMMEngine::emit_progress() noexcept {
    if (!callbacks_.on_progress) return;
    const double total = (ctx_.options.end_date - ctx_.options.start_date)
                         * hydraulics::TimestepController::SEC_PER_DAY;
    const double frac  = (total > 0.0) ? (ctx_.current_time / total) : 0.0;
    callbacks_.on_progress(
        static_cast<void*>(this),
        frac,
        ctx_.current_date,
        callbacks_.progress_ud
    );
}

// ============================================================================
// init_modules() — initialize all computational modules from SimulationContext
// ============================================================================

void SWMMEngine::init_modules() noexcept {
    initHydraulics();
    initHydrology();
    initQuality();
    initGeometry();
    initMassBalance();

    // Allocate forcing arrays to match object counts
    ctx_.forcing.resize(ctx_.n_nodes(), ctx_.n_links(),
                        ctx_.n_subcatches(), ctx_.n_gages(),
                        ctx_.n_pollutants());
}

// ============================================================================
// initHydraulics() — initialize hydraulic routing: router, exfiltration,
//                     inlets, culverts, controls, inflows, RDII, interface
// ============================================================================

/**
 * @brief Initialize hydraulic routing: router, exfiltration, inlets, culverts.
 *
 * @details Sets up the hydraulic router (with routing model selection),
 *          configures OpenMP threading, computes conduit conveyance,
 *          initializes controls, inflows, RDII, exfiltration, inlets,
 *          and interface files.
 */
void SWMMEngine::initHydraulics() noexcept {
    // 1. Router: build XSectGroups from link cross-section data,
    //    compute Manning conveyance for all conduits (batch)
    RouteModel rm = RouteModel::DYNWAVE;
    if (ctx_.options.routing_model == RoutingModel::KINWAVE) rm = RouteModel::KINWAVE;
    else if (ctx_.options.routing_model == RoutingModel::STEADY) rm = RouteModel::STEADY;
    router_.init(ctx_, rm);

#ifdef OPENSWMM_HAS_2D
    // 1a. Initialize optional 2D surface routing module.
    //     Builds mesh topology, vertex stencils, resolves coupling maps,
    //     suppresses ponding at coupled nodes, and initializes CVODE.
    surface_router_.initialize(ctx_);
#endif

    // 1b. Configure OpenMP thread count from THREADS option.
    //     0 = use all available; N = use min(N, available).
    //     DWSolver applies its own threshold (< 4*nThreads links → 1 thread).
    //     The global OMP thread count is also set for Runoff/Quality modules.
    {
        int nt = ctx_.options.num_threads;
        if (nt == 0)
            nt = omp_get_max_threads();
        else
            nt = std::min(nt, omp_get_max_threads());
        omp_set_num_threads(nt);

        // DWSolver gets its own thread count with link-count threshold
        if (rm == RouteModel::DYNWAVE) {
            router_.setDWNumThreads(ctx_.options.num_threads);
        }
    }

    // Initialize routing time-step histogram bins (log-scale from RouteStep
    // down to MinRouteStep, matching legacy stats.c stats_open)
    ctx_.routing_stats.init_histogram(ctx_.options.routing_step,
                                       ctx_.options.min_routing_step);

    // NOTE: Conduit conveyance (beta, rough_factor, q_full) is computed in
    // PostParseResolver and then adjusted for conduit lengthening in
    // Routing::init(). Do not recompute here as it would overwrite the
    // lengthening adjustments.

    // 8. Controls: rules can be added via:
    //    a) Parsed from [CONTROLS] section text (needs rule parser)
    //    b) Built programmatically via C API (swmm_link_set_target_setting etc.)
    //    c) Registered via controls_.init(rules) before start()
    // The ControlEngine is ready to evaluate rules; rules vector may be empty
    // if all control is done via API runtime calls instead of rule text.

    // 9. Inflow solver: populate from ext_inflows + dwf_inflows data
    inflow_.init(ctx_);

    // 10a. RDII solver: initialize unit hydrograph groups
    rdii_.init(ctx_);

    // 10b. Non-conduit hydraulic structures (pumps, orifices, weirs, outlets)
    hydstruct_.init(ctx_);

    // 10c. Exfiltration solver: initialize Green-Ampt state for storage nodes
    exfil_.init(ctx_);

    // 10d. Inlet solver: initialize street inlet data
    inlet_.init(ctx_);

    // 10d-1. Pre-build culvert link index list (avoids per-timestep heap alloc)
    culvert_links_.clear();
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx_.links.type[uj] == LinkType::CONDUIT &&
            ctx_.links.culvert_code[uj] > 0) {
            culvert_links_.push_back(j);
        }
    }

    // 10d. Interface file manager: initialize (files opened later in start())
    iface_.init(ctx_);

    // 10d-2. Sort routing events chronologically and resolve overlaps
    //        (matching legacy sortEvents() in routing.c)
    if (!ctx_.events.empty()) {
        std::sort(ctx_.events.begin(), ctx_.events.end(),
                  [](const SimulationContext::Event& a, const SimulationContext::Event& b) {
                      return a.start < b.start;
                  });
        // Resolve overlapping events
        for (size_t i = 0; i + 1 < ctx_.events.size(); ++i) {
            if (ctx_.events[i].end > ctx_.events[i + 1].start)
                ctx_.events[i].end = ctx_.events[i + 1].start;
        }
        next_event_ = 0;
    }

    // 10e. Control rule parsing from [CONTROLS] section text
    if (ctx_.control_rules.count() > 0) {
        for (const auto& text : ctx_.control_rules.rule_text) {
            controls_.parseRuleText(text, ctx_);
        }
    }
}

// ============================================================================
// initHydrology() — initialize hydrology solvers: runoff, snow, GW, LID
// ============================================================================

/**
 * @brief Initialize hydrology solvers: runoff, snow, groundwater, LID.
 *
 * @details Populates RunoffSoA from subcatchment properties, resizes gage
 *          states, and initializes snow, groundwater, and LID solvers.
 */
void SWMMEngine::initHydrology() noexcept {
    // 2. Runoff solver: populate RunoffSoA from subcatchment properties
    runoff_.init(ctx_);

    // 2b. Store base values for pattern-adjusted subcatchment parameters
    {
        int ns = ctx_.n_subcatches();
        auto uns = static_cast<std::size_t>(ns);
        ctx_.base_n_perv.resize(uns);
        ctx_.base_ds_perv.resize(uns);
        for (int i = 0; i < ns; ++i) {
            auto ui = static_cast<std::size_t>(i);
            ctx_.base_n_perv[ui]  = ctx_.subcatches.n_perv[ui];
            ctx_.base_ds_perv[ui] = ctx_.subcatches.ds_perv[ui];
        }
        // Check if any pattern assignments exist
        ctx_.has_subcatch_adj_patterns = false;
        for (size_t i = 0; i < ctx_.subcatch_n_perv_pattern.size(); ++i)
            if (ctx_.subcatch_n_perv_pattern[i] >= 0)
            { ctx_.has_subcatch_adj_patterns = true; break; }
        if (!ctx_.has_subcatch_adj_patterns) {
            for (size_t i = 0; i < ctx_.subcatch_d_store_pattern.size(); ++i)
                if (ctx_.subcatch_d_store_pattern[i] >= 0)
                { ctx_.has_subcatch_adj_patterns = true; break; }
        }
        if (!ctx_.has_subcatch_adj_patterns) {
            for (size_t i = 0; i < ctx_.subcatch_infil_pattern.size(); ++i)
                if (ctx_.subcatch_infil_pattern[i] >= 0)
                { ctx_.has_subcatch_adj_patterns = true; break; }
        }
    }

    // 3. Gage states
    gage_states_.resize(static_cast<std::size_t>(ctx_.n_gages()));

    // 4. Snow solver: init and populate from parsed snowpack data
    snow_.init(ctx_.n_subcatches());
    {
        auto& soa = snow_.state();
        int unit_sys_snow = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));

        // Transfer global snowmelt parameters
        soa.tipm = ctx_.options.snow_ati_wt;
        soa.rnm  = ctx_.options.snow_nrg_ratio;

        // Transfer ADC curves from options
        for (int k = 0; k < 10; ++k) {
            soa.adc_imperv[k] = ctx_.options.adc_imperv[k];
            soa.adc_perv[k]   = ctx_.options.adc_perv[k];
        }

        // Transfer per-subcatchment snowpack parameters
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            int sp_idx = ctx_.subcatches.snowpack[ui];
            if (sp_idx < 0) continue;
            auto usp = static_cast<std::size_t>(sp_idx);

            // Surface types: 0=PLOWABLE, 1=IMPERVIOUS, 2=PERVIOUS
            const std::array<double, 7>* surfaces[3] = {nullptr, nullptr, nullptr};
            if (usp < ctx_.snowpacks.plowable.size())   surfaces[0] = &ctx_.snowpacks.plowable[usp];
            if (usp < ctx_.snowpacks.impervious.size())  surfaces[1] = &ctx_.snowpacks.impervious[usp];
            if (usp < ctx_.snowpacks.pervious.size())    surfaces[2] = &ctx_.snowpacks.pervious[usp];

            for (int k = 0; k < snow::N_SUBAREAS; ++k) {
                auto idx = static_cast<std::size_t>(i * snow::N_SUBAREAS + k);
                if (!surfaces[k]) continue;
                const auto& p = *surfaces[k];
                // p[0]=Cmin, p[1]=Cmax, p[2]=Tbase, p[3]=FWF, p[4]=SD0, p[5]=FW0, p[6]=SNN0
                soa.dhmin[idx]  = p[0] / ucf::Ucf[ucf::RAINFALL][unit_sys_snow];
                soa.dhmax[idx]  = p[1] / ucf::Ucf[ucf::RAINFALL][unit_sys_snow];
                soa.tbase[idx]  = p[2];
                if (unit_sys_snow == 1) // SI: convert °C to °F
                    soa.tbase[idx] = soa.tbase[idx] * 9.0 / 5.0 + 32.0;
                soa.fwfrac[idx] = p[3];
                soa.wsnow[idx]  = p[4] / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                soa.fw[idx]     = p[5] / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                soa.si[idx]     = soa.wsnow[idx]; // depth at 100% cover = initial depth
                if (k == snow::SNOW_PLOWABLE)
                    soa.snn[ui] = p[6];
            }

            // Transfer plowing/removal parameters
            if (usp < ctx_.snowpacks.removal.size()) {
                const auto& r = ctx_.snowpacks.removal[usp];
                auto sf = static_cast<std::size_t>(i * 5);
                soa.weplow[ui]  = r[0] / ucf::Ucf[ucf::RAINDEPTH][unit_sys_snow];
                soa.sfrac[sf+0] = r[1]; // fraction removed
                soa.sfrac[sf+1] = r[2]; // fraction to impervious
                soa.sfrac[sf+2] = r[3]; // fraction to pervious
                soa.sfrac[sf+3] = r[4]; // fraction immediate melt
                soa.sfrac[sf+4] = r[5]; // fraction to other subcatchment

                // Resolve target subcatchment for plowed snow
                if (usp < ctx_.snowpacks.removal_subcatch.size() &&
                    !ctx_.snowpacks.removal_subcatch[usp].empty()) {
                    soa.to_subcatch[ui] = ctx_.subcatch_names.find(
                        ctx_.snowpacks.removal_subcatch[usp]);
                }
            }
        }
    }

    // 5. Climate: transfer evaporation data from options to climate state
    {
        int evap_type = ctx_.options.evap_type;
        if (evap_type == 0) {
            // CONSTANT
            climate_.evap_method = climate::EvapMethod::CONSTANT;
            climate_.evap_rate = ctx_.options.evap_values[0]
                               / ucf::Ucf[ucf::EVAPRATE][0];
        } else if (evap_type == 1) {
            // MONTHLY
            climate_.evap_method = climate::EvapMethod::MONTHLY;
            for (int i = 0; i < 12; ++i)
                climate_.monthly_evap[i] = ctx_.options.evap_values[i];
        } else if (evap_type == 2) {
            climate_.evap_method = climate::EvapMethod::TIMESERIES;
        } else if (evap_type == 3) {
            climate_.evap_method = climate::EvapMethod::TEMPERATURE;
        } else if (evap_type == 4) {
            climate_.evap_method = climate::EvapMethod::PAN;
        }

        // Transfer latitude for Hargreaves ET calculation
        climate_.latitude = ctx_.options.snow_lat;

        // Transfer monthly adjustment arrays
        for (int i = 0; i < 12; ++i) {
            climate_.adjust_temp[i] = ctx_.adjust_temp[i];
            climate_.adjust_evap[i] = ctx_.adjust_evap[i];
            climate_.adjust_rain[i] = ctx_.adjust_rain[i];
            climate_.adjust_hydcon[i] = ctx_.adjust_hydcon[i];
        }

        // Resolve timeseries names to table indices
        if (ctx_.options.temp_source == 1 && !ctx_.options.temp_ts_name.empty()) {
            climate_.temp_ts_index = ctx_.table_names.find(ctx_.options.temp_ts_name);
        }
        if (evap_type == 2 && !ctx_.options.evap_ts_name.empty()) {
            climate_.evap_ts_index = ctx_.table_names.find(ctx_.options.evap_ts_name);
        }

        // Resolve recovery pattern name to pattern index
        if (!ctx_.options.evap_recovery_pat.empty()) {
            int np = ctx_.patterns.count();
            for (int i = 0; i < np; ++i) {
                if (ctx_.patterns.names[static_cast<std::size_t>(i)] == ctx_.options.evap_recovery_pat) {
                    climate_.recovery_pat_index = i;
                    break;
                }
            }
        }

        // Open climate file if temperature or evaporation uses FILE source
        if ((ctx_.options.temp_source == 2 || evap_type == 4) &&
            !ctx_.options.temp_file.empty()) {
            int us = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));
            climate_file_.open(ctx_.options.temp_file,
                               ctx_.options.temp_file_start, us);
        }
    }

    // 6. Groundwater solver: init and populate from parsed aquifer/GW data
    groundwater_.init(ctx_.n_subcatches());
    {
        auto& gw = groundwater_.state();
        int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx_.options.flow_units));
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            int aq_idx = ctx_.subcatches.gw_aquifer[ui];
            if (aq_idx < 0) continue;
            auto uaq = static_cast<std::size_t>(aq_idx);

            // Copy aquifer properties
            gw.porosity[ui]         = ctx_.aquifers.porosity[uaq];
            gw.field_cap[ui]        = ctx_.aquifers.field_capacity[uaq];
            gw.wilt_point[ui]       = ctx_.aquifers.wilting_point[uaq];
            // All conversions match legacy gwater.c line 170-182
            gw.k_sat[ui]            = ctx_.aquifers.conductivity[uaq]
                                      / ucf::Ucf[ucf::RAINFALL][unit_sys];
            gw.k_slope[ui]          = ctx_.aquifers.conduct_slope[uaq];
            gw.tension_slope[ui]    = ctx_.aquifers.tension_slope[uaq]
                                      / ucf::Ucf[ucf::LENGTH][unit_sys];
            gw.upper_evap_frac[ui]  = ctx_.aquifers.upper_evap[uaq];
            // Resolve upper evaporation pattern name to index
            gw.upper_evap_pat[ui] = -1;
            const auto& pat_name = ctx_.aquifers.upper_evap_pat[uaq];
            if (!pat_name.empty()) {
                for (int p = 0; p < ctx_.patterns.count(); ++p) {
                    if (ctx_.patterns.names[static_cast<std::size_t>(p)] == pat_name) {
                        gw.upper_evap_pat[ui] = p;
                        break;
                    }
                }
            }
            gw.lower_evap_depth[ui] = ctx_.aquifers.lower_evap[uaq]
                                      / ucf::Ucf[ucf::LENGTH][unit_sys];
            gw.lower_loss_coeff[ui] = ctx_.aquifers.lower_loss[uaq]
                                      / ucf::Ucf[ucf::RAINFALL][unit_sys];
            gw.total_depth[ui]      = (ctx_.subcatches.gw_surf_elev[ui]
                                       - ctx_.aquifers.bottom_elev[uaq])
                                      / ucf::Ucf[ucf::LENGTH][unit_sys];

            // Copy GW lateral flow coefficients
            gw.a1[ui]     = ctx_.subcatches.gw_a1[ui];
            gw.b1[ui]     = ctx_.subcatches.gw_b1[ui];
            gw.a2[ui]     = ctx_.subcatches.gw_a2[ui];
            gw.b2[ui]     = ctx_.subcatches.gw_b2[ui];
            gw.a3[ui]     = ctx_.subcatches.gw_a3[ui];
            gw.h_star[ui] = ctx_.subcatches.gw_hstar[ui]
                            / ucf::Ucf[ucf::LENGTH][unit_sys];

            // Initial conditions from aquifer
            gw.theta[ui]      = ctx_.aquifers.upper_moist[uaq];
            gw.lower_depth[ui] = ctx_.aquifers.water_table_elev[uaq]
                                 / ucf::Ucf[ucf::LENGTH][unit_sys];
        }

        // Parse [GWF] custom expressions (stored in ext_options during input parsing).
        // Format: ext_options["GWF:<subcatch_name>:LATERAL"] = expression string
        //         ext_options["GWF:<subcatch_name>:DEEP"]    = expression string
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ctx_.subcatches.gw_aquifer[ui] < 0) continue;

            const std::string& sc_name = ctx_.subcatch_names.names()[ui];

            auto lat_it = ctx_.options.ext_options.find("GWF:" + sc_name + ":LATERAL");
            if (lat_it != ctx_.options.ext_options.end() && !lat_it->second.empty()) {
                mathexpr::Expression expr;
                if (mathexpr::parse(lat_it->second, expr) == 0 && expr.valid) {
                    mathexpr::bind_variables(expr,
                        groundwater::GW_VAR_NAMES, groundwater::GWV_MAX);
                    gw.lateral_expr[ui] = std::move(expr);
                }
            }

            auto deep_it = ctx_.options.ext_options.find("GWF:" + sc_name + ":DEEP");
            if (deep_it != ctx_.options.ext_options.end() && !deep_it->second.empty()) {
                mathexpr::Expression expr;
                if (mathexpr::parse(deep_it->second, expr) == 0 && expr.valid) {
                    mathexpr::bind_variables(expr,
                        groundwater::GW_VAR_NAMES, groundwater::GWV_MAX);
                    gw.deep_expr[ui] = std::move(expr);
                }
            }
        }
    }

    // 7. LID solver
    lid_.init(ctx_);
}

// ============================================================================
// initQuality() — initialize water quality: landuse, surface quality, mass bal
// ============================================================================

/**
 * @brief Initialize water quality: landuse solver, surface quality, mass balance.
 *
 * @details Initializes the quality routing solver, treatment arrays,
 *          buildup/washoff data, landuse solver parameters, surface quality
 *          state, quality mass balance vectors, and initial buildup from
 *          antecedent dry days.
 */
void SWMMEngine::initQuality() noexcept {
    // 7. Quality solver
    quality_.init(ctx_.n_nodes(), ctx_.n_links(), ctx_.n_pollutants());

    // 10. Treatment: resize for nodes x pollutants + compile expressions
    if (ctx_.n_pollutants() > 0 && ctx_.n_nodes() > 0) {
        if (ctx_.treatment.n_nodes == 0) {
            ctx_.treatment.resize(ctx_.n_nodes(), ctx_.n_pollutants());
        }
        // Compile treatment expressions
        int np = ctx_.n_pollutants();
        for (int n = 0; n < ctx_.n_nodes(); ++n) {
            for (int p = 0; p < np; ++p) {
                auto idx = static_cast<std::size_t>(n * np + p);
                const auto& expr_str = ctx_.treatment.expressions[idx];
                if (expr_str.empty()) continue;
                treatment::TreatExpr te;
                if (treatment::parse(expr_str, te) == 0) {
                    te.pollutant_idx = p;
                    ctx_.treatment.compiled[idx] = std::move(te);
                    ctx_.treatment.has_treatment[static_cast<std::size_t>(n)] = true;
                }
            }
        }
    }

    // 11. Buildup/washoff: resize for landuses x pollutants
    if (ctx_.n_landuses() > 0 && ctx_.n_pollutants() > 0) {
        if (ctx_.buildup.n_landuses == 0) {
            ctx_.buildup.resize(ctx_.n_landuses(), ctx_.n_pollutants());
            ctx_.washoff.resize(ctx_.n_landuses(), ctx_.n_pollutants());
        }
    }

    // 11b. Initialize landuse quality solver and surface quality state
    if (ctx_.n_pollutants() > 0) {
        int nlu = std::max(ctx_.n_landuses(), 1);
        int np  = ctx_.n_pollutants();
        landuse_solver_.init(nlu, np);
        surface_quality_.resize(ctx_.n_subcatches(), nlu, np);
        ctx_.subcatches.resize_total_load(ctx_.n_subcatches(), np);

        // Transfer parsed BuildupData/WashoffData into LanduseSolver params
        for (int lu = 0; lu < ctx_.n_landuses(); ++lu) {
            for (int p = 0; p < np; ++p) {
                auto k = static_cast<std::size_t>(lu * np + p);
                auto& bp = landuse_solver_.buildup_params[k];
                bp.type = static_cast<landuse::BuildupType>(ctx_.buildup.func_type[k]);
                bp.coeff[0] = ctx_.buildup.coeff1[k];  // max buildup
                bp.coeff[1] = ctx_.buildup.coeff2[k];  // rate constant
                bp.coeff[2] = ctx_.buildup.coeff3[k];  // exponent/half-sat
                bp.normalizer = ctx_.buildup.normalizer[k];
                // Compute max_days: time to reach 99.9% of max buildup
                if (bp.type == landuse::BuildupType::EXPON && bp.coeff[1] > 0.0)
                    bp.max_days = -std::log(0.001) / bp.coeff[1];
                else if (bp.type == landuse::BuildupType::POWER && bp.coeff[1] > 0.0
                         && bp.coeff[2] > 0.0 && bp.coeff[0] > 0.0)
                    bp.max_days = std::pow(bp.coeff[0] / bp.coeff[1], 1.0 / bp.coeff[2]);
                else if (bp.type == landuse::BuildupType::SATUR && bp.coeff[2] > 0.0
                         && bp.coeff[0] > 0.0)
                    bp.max_days = 999.0 * bp.coeff[2];  // asymptotic

                auto& wp = landuse_solver_.washoff_params[k];
                wp.type = static_cast<landuse::WashoffType>(ctx_.washoff.func_type[k]);
                wp.coeff = ctx_.washoff.coeff[k];
                wp.expon = ctx_.washoff.expon[k];
                wp.sweep_effic = ctx_.washoff.sweep_effic[k];
                wp.bmp_effic = ctx_.washoff.bmp_effic[k];
            }
        }

        // Initialize quality mass balance vectors
        ctx_.mass_balance.resize_quality(np);

        // Compute initial buildup from antecedent dry days
        // (matching legacy landuse_getInitBuildup)
        // Buildup is stored PER LAND USE: buildup[bu_idx(sc,lu,p)] = mass/normalizer
        double dry_days = ctx_.options.dry_days;
        if (dry_days > 0.0 && ctx_.n_landuses() > 0) {
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                for (int lu = 0; lu < nlu; ++lu) {
                    auto cov_idx = ui * static_cast<std::size_t>(nlu)
                                   + static_cast<std::size_t>(lu);
                    double frac = (cov_idx < ctx_.subcatches.coverage.size())
                                  ? ctx_.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                    if (frac <= 0.0) continue;

                    for (int p = 0; p < np; ++p) {
                        auto k = static_cast<std::size_t>(lu * np + p);
                        const auto& bp = landuse_solver_.buildup_params[k];
                        if (bp.type == landuse::BuildupType::NONE) continue;

                        double mass = 0.0;
                        double c0 = bp.coeff[0], c1 = bp.coeff[1], c2 = bp.coeff[2];
                        switch (bp.type) {
                            case landuse::BuildupType::POWER:
                                mass = std::min(c1 * std::pow(dry_days, c2), c0);
                                break;
                            case landuse::BuildupType::EXPON:
                                mass = c0 * (1.0 - std::exp(-c1 * dry_days));
                                break;
                            case landuse::BuildupType::SATUR:
                                mass = (c2 + dry_days > 0.0) ? c0 * dry_days / (c2 + dry_days) : 0.0;
                                break;
                            default: break;
                        }

                        // Store per-landuse buildup (per normalizer unit)
                        auto bu = surface_quality_.bu_idx(i, lu, p);
                        surface_quality_.buildup[bu] = mass;

                        // Normalize to absolute mass for mass balance
                        double norm = (bp.normalizer == 0)
                            ? frac * ctx_.subcatches.area[ui]
                            : frac * ctx_.subcatches.curb_length[ui];
                        ctx_.mass_balance.qual_init_buildup[static_cast<std::size_t>(p)] += mass * norm;
                    }
                }
            }
        }
    }
}

// ============================================================================
// initGeometry() — initialize node/link geometry: crown elevations, full vols
// ============================================================================

/**
 * @brief Initialize node/link geometry: crown elevations, full volumes.
 *
 * @details Computes node crown elevations from connecting link crowns and
 *          tracks node degree (connectivity count). Initializes node full
 *          volumes from full depth using the node volume function.
 */
void SWMMEngine::initGeometry() noexcept {
    // 12. Initialize node crown elevations (matching legacy dynwave_init)
    //     Crown elevation = highest connecting link crown
    for (int i = 0; i < ctx_.n_nodes(); ++i) {
        ctx_.nodes.crown_elev[static_cast<std::size_t>(i)] =
            ctx_.nodes.invert_elev[static_cast<std::size_t>(i)];
    }
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx_.links.type[uj] != LinkType::CONDUIT) continue;
        int n1 = ctx_.links.node1[uj];
        int n2 = ctx_.links.node2[uj];
        auto un1 = static_cast<std::size_t>(n1);
        auto un2 = static_cast<std::size_t>(n2);
        double z1 = ctx_.nodes.invert_elev[un1] + ctx_.links.offset1[uj]
                     + ctx_.links.xsect_y_full[uj];
        double z2 = ctx_.nodes.invert_elev[un2] + ctx_.links.offset2[uj]
                     + ctx_.links.xsect_y_full[uj];
        if (z1 > ctx_.nodes.crown_elev[un1])
            ctx_.nodes.crown_elev[un1] = z1;
        if (z2 > ctx_.nodes.crown_elev[un2])
            ctx_.nodes.crown_elev[un2] = z2;
        // Track node degree (connectivity count)
        ctx_.nodes.degree[un1]++;
        ctx_.nodes.degree[un2]++;
    }

    // 13. Initialize node full volumes
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        ctx_.nodes.full_volume[uj] =
            node::getVolume(ctx_.nodes, j, ctx_.nodes.full_depth[uj], &ctx_.tables);
    }
}

// ============================================================================
// initMassBalance() — initialize mass balance: record initial storage volumes
// ============================================================================

/**
 * @brief Initialize mass balance: record initial storage volumes.
 *
 * @details Resets mass balance state, then records initial routing storage
 *          (sum of node + link volumes) and initial runoff storage
 *          (ponded depth * area for all subcatchments).
 */
void SWMMEngine::resetStepMassBalance() noexcept {
    ctx_.mass_balance.step_flooding     = 0.0;
    ctx_.mass_balance.step_outflow      = 0.0;
    ctx_.mass_balance.step_dw_inflow    = 0.0;
    ctx_.mass_balance.step_gw_inflow    = 0.0;
    ctx_.mass_balance.step_rdii_inflow  = 0.0;
    ctx_.mass_balance.step_ext_inflow   = 0.0;
}

// ============================================================================
// assembleGWCoupling — pre-compute GW surface water head from routing state
// ============================================================================

void SWMMEngine::assembleGWCoupling(double dt_runoff) noexcept {
    int ns = ctx_.n_subcatches();
    // Build per-subcatchment sw_head from each subcatch's GW receiving node.
    // Legacy: Hsw = Node[n].newDepth + Node[n].invertElev - GW->bottomElev
    for (int i = 0; i < ns; ++i) {
        auto ui = static_cast<std::size_t>(i);
        ctx_.subcatches.gw_sw_head[ui] = 0.0;
        ctx_.subcatches.gw_node_avail_flow[ui] = 0.0;

        int aq_idx = ctx_.subcatches.gw_aquifer[ui];
        if (aq_idx < 0) continue;
        int gw_node = ctx_.subcatches.gw_node[ui];
        if (gw_node < 0) gw_node = ctx_.subcatches.outlet_node[ui];
        if (gw_node >= 0 && gw_node < ctx_.n_nodes()) {
            auto un = static_cast<std::size_t>(gw_node);
            double bottom_elev = ctx_.aquifers.bottom_elev[static_cast<std::size_t>(aq_idx)];
            ctx_.subcatches.gw_sw_head[ui] =
                ctx_.nodes.depth[un] + ctx_.nodes.invert_elev[un] - bottom_elev;

            // Available node flow for GW negative flow limit
            double area_ft2 = ctx_.subcatches.area[ui] * ucf::ACRES_TO_FT2;
            if (area_ft2 > 0.0 && dt_runoff > 0.0) {
                ctx_.subcatches.gw_node_avail_flow[ui] =
                    (ctx_.nodes.inflow[un] + ctx_.nodes.volume[un] / dt_runoff) / area_ft2;
            }
        }
    }
}

// ============================================================================
// assembleRunon — subcatch-to-subcatch routing into runon_inflow[]
// ============================================================================

void SWMMEngine::assembleRunon() noexcept {
    std::fill(ctx_.subcatches.runon_inflow.begin(),
              ctx_.subcatches.runon_inflow.end(), 0.0);

    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        int out_sc = ctx_.subcatches.outlet_subcatch[ui];
        if (out_sc >= 0 && out_sc < ctx_.n_subcatches()) {
            auto usc = static_cast<std::size_t>(out_sc);
            ctx_.subcatches.runon_inflow[usc] += ctx_.subcatches.runoff[ui];
        }
    }
}

// ============================================================================
// assembleLateralInflows — sum decomposed inflow sources into lat_flow
// ============================================================================

void SWMMEngine::assembleLateralInflows() noexcept {
    // Sum all decomposed inflow source arrays into nodes.lat_flow[].
    // Each source process writes to its own buffer; this is the single
    // assembly point where they are combined for the routing solver.
    // Also computes step mass balance accumulators from component sums.
    double sum_dw   = 0.0;
    double sum_gw   = 0.0;
    double sum_rdii = 0.0;
    double sum_ext  = 0.0;

    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        ctx_.nodes.lat_flow[uj] = ctx_.nodes.runoff_inflow[uj]
                                + ctx_.nodes.gw_inflow[uj]
                                + ctx_.nodes.ext_inflow[uj]
                                + ctx_.nodes.dwf_inflow[uj]
                                + ctx_.nodes.rdii_inflow[uj]
                                + ctx_.nodes.iface_inflow[uj]
                                + ctx_.nodes.user_lat_flow[uj];

        sum_dw   += ctx_.nodes.dwf_inflow[uj];
        sum_gw   += ctx_.nodes.gw_inflow[uj];
        sum_rdii += ctx_.nodes.rdii_inflow[uj];
        sum_ext  += ctx_.nodes.ext_inflow[uj];
    }

    ctx_.mass_balance.step_dw_inflow   = sum_dw;
    ctx_.mass_balance.step_gw_inflow   = sum_gw;
    ctx_.mass_balance.step_rdii_inflow = sum_rdii;
    ctx_.mass_balance.step_ext_inflow  = sum_ext;
}

// ============================================================================
// isBetweenEvents — check if routing should be skipped between event windows
// Matches legacy routing.c:isBetweenEvents()
// ============================================================================

bool SWMMEngine::isBetweenEvents(double current_date) const {
    if (ctx_.events.empty()) return false;
    if (next_event_ >= static_cast<int>(ctx_.events.size())) return true;

    const auto& ev = ctx_.events[static_cast<size_t>(next_event_)];

    // Past current event → between events
    if (current_date > ev.end) return true;

    // Inside current event → not between
    if (current_date >= ev.start) return false;

    // Before current event → between events
    return true;
}

// ============================================================================
// isInSteadyState — check if routing can be skipped (steady conditions)
// Matches legacy routing.c:isInSteadyState() + inflowHasChanged()
// ============================================================================

bool SWMMEngine::isInSteadyState(int action_count) const {
    if (!ctx_.options.skip_steady_state) return false;
    if (ctx_.current_time == 0.0) return false;
    if (action_count > 0) return false;

    // Check flow error exceeds tolerance
    double flow_error = std::abs(ctx_.mass_balance.routing_error());
    if (flow_error > ctx_.options.sys_flow_tol) return false;

    // Check if any lateral inflow has changed significantly
    constexpr double TINY = 1e-6;
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double qOld = ctx_.nodes.old_lat_flow[uj];
        double qNew = ctx_.nodes.lat_flow[uj];
        double diff;
        if (std::abs(qOld) > TINY)      diff = (qNew / qOld) - 1.0;
        else if (std::abs(qNew) > TINY)  diff = 1.0;
        else                              diff = 0.0;
        if (std::abs(diff) > ctx_.options.lat_flow_tol) return false;
    }
    return true;
}

void SWMMEngine::initMassBalance() noexcept {
    // 14. Mass balance: record initial storage (nodes + links, matching legacy)
    ctx_.mass_balance.reset();
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        ctx_.mass_balance.routing_init_storage +=
            ctx_.nodes.volume[static_cast<std::size_t>(j)];
    }
    for (int j = 0; j < ctx_.n_links(); ++j) {
        ctx_.mass_balance.routing_init_storage +=
            ctx_.links.volume[static_cast<std::size_t>(j)];
    }

    // Record initial runoff storage
    for (int j = 0; j < ctx_.n_subcatches(); ++j) {
        // Initial surface storage approximated from initial depth * area
        auto uj = static_cast<std::size_t>(j);
        ctx_.mass_balance.runoff_init_store +=
            ctx_.subcatches.ponded_depth[uj] * ctx_.subcatches.area[uj];
    }

    // Record initial groundwater storage
    // Legacy: GwaterTotals.initStorage += gwater_getVolume(j) * Subcatch[j].area
    // gwater_getVolume = theta * upperDepth + porosity * lowerDepth
    {
        auto& gw = groundwater_.state();
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (gw.total_depth[ui] <= 0.0) continue;
            double upper_d = gw.total_depth[ui] - gw.lower_depth[ui];
            double vol = gw.theta[ui] * upper_d + gw.porosity[ui] * gw.lower_depth[ui];
            double area = ctx_.subcatches.area[ui] * ucf::ACRES_TO_FT2;
            ctx_.mass_balance.gw_init_storage += vol * area;
        }
    }
}

} /* namespace openswmm */
