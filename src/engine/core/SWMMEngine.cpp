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
#include "../hydraulics/ForceMain.hpp"
#include <cmath>
#include <algorithm>
#include <vector>
#include "../hydraulics/TimestepController.hpp"
#include "../input/InputReader.hpp"
#include "../input/handlers/OptionsHandler.hpp"
#include "../input/handlers/NodesHandler.hpp"
#include "../input/handlers/LinksHandler.hpp"
#include "../input/handlers/CatchmentHandler.hpp"
#include "../input/handlers/TablesHandler.hpp"
#include "../input/handlers/UserFlagsHandler.hpp"
#include "../input/handlers/UserFlagValuesHandler.hpp"
#include "../input/handlers/PluginsHandler.hpp"
#include "../input/handlers/InflowsHandler.hpp"
#include "../input/handlers/QualityHandler.hpp"
#include "../input/handlers/HydrologyHandler.hpp"
#include "../input/handlers/ControlsHandler.hpp"
#include "../input/PostParseResolver.hpp"
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
    register_builtin_handlers();
}

SWMMEngine::~SWMMEngine() {
    if (ctx_.state == EngineState::RUNNING ||
        ctx_.state == EngineState::ENDED) {
        close();
    }
}

// ============================================================================
// register_builtin_handlers()
// ============================================================================

void SWMMEngine::register_builtin_handlers() {
    // Phase 3: [OPTIONS] is the first fully-implemented built-in handler.
    // Additional handlers (JUNCTIONS, CONDUITS, etc.) are registered as
    // they are implemented in subsequent phases.

    registry_.register_builtin("OPTIONS", input::handle_options);

    // Placeholder lambdas for sections that are parsed but not yet implemented.
    // These prevent "unknown section" warnings while the handlers are built out.
    // Each will be replaced with a real handler in Phase 3.

    auto noop = [](SimulationContext&, const std::vector<std::string>&) {};

    // Node sections — real handlers
    registry_.register_builtin("JUNCTIONS",    input::handle_junctions);
    registry_.register_builtin("OUTFALLS",     input::handle_outfalls);
    registry_.register_builtin("DIVIDERS",     input::handle_dividers);
    registry_.register_builtin("STORAGE",      input::handle_storage);
    registry_.register_builtin("COORDINATES",  input::handle_coordinates);

    // Link sections — real handlers
    registry_.register_builtin("CONDUITS",     input::handle_conduits);
    registry_.register_builtin("PUMPS",        input::handle_pumps);
    registry_.register_builtin("ORIFICES",     input::handle_orifices);
    registry_.register_builtin("WEIRS",        input::handle_weirs);
    registry_.register_builtin("OUTLETS",      input::handle_outlets);
    registry_.register_builtin("XSECTIONS",    input::handle_xsections);
    registry_.register_builtin("TRANSECTS",    input::handle_transects);
    registry_.register_builtin("LOSSES",       input::handle_losses);

    // Catchment sections
    registry_.register_builtin("SUBCATCHMENTS", input::handle_subcatchments);
    registry_.register_builtin("SUBAREAS",      input::handle_subareas);
    registry_.register_builtin("INFILTRATION",  input::handle_infiltration);
    registry_.register_builtin("LID_CONTROLS",  input::handle_lid_controls);
    registry_.register_builtin("LID_USAGE",     input::handle_lid_usage);
    registry_.register_builtin("AQUIFERS",      input::handle_aquifers);
    registry_.register_builtin("GROUNDWATER",   input::handle_groundwater);
    registry_.register_builtin("GWF",           input::handle_gwf);

    // Rain / climate
    registry_.register_builtin("RAINGAGES",     input::handle_raingages);
    registry_.register_builtin("EVAPORATION",   input::handle_evaporation);
    registry_.register_builtin("TEMPERATURE",   input::handle_temperature);
    registry_.register_builtin("SNOWPACKS",     input::handle_snowpacks);

    // Inflows / loading
    registry_.register_builtin("INFLOWS",       input::handle_inflows);
    registry_.register_builtin("DWF",           input::handle_dwf);
    registry_.register_builtin("RDII",          input::handle_rdii);
    registry_.register_builtin("LOADINGS",      input::handle_loadings);
    registry_.register_builtin("PATTERNS",      input::handle_patterns);

    // Tables
    registry_.register_builtin("CURVES",        input::handle_curves);
    registry_.register_builtin("TIMESERIES",    input::handle_timeseries);

    // Water quality
    registry_.register_builtin("POLLUTANTS",    input::handle_pollutants);
    registry_.register_builtin("LANDUSES",      input::handle_landuses);
    registry_.register_builtin("COVERAGES",     input::handle_coverages);
    registry_.register_builtin("BUILDUP",       input::handle_buildup);
    registry_.register_builtin("WASHOFF",       input::handle_washoff);
    registry_.register_builtin("TREATMENT",     input::handle_treatment);
    registry_.register_builtin("MIXING",        noop);

    // Control / map / reporting
    registry_.register_builtin("CONTROLS",      input::handle_controls);
    registry_.register_builtin("VERTICES",      noop);
    registry_.register_builtin("POLYGONS",      noop);
    registry_.register_builtin("SYMBOLS",       noop);
    registry_.register_builtin("LABELS",        noop);
    registry_.register_builtin("BACKDROP",      noop);
    registry_.register_builtin("MAP",           noop);
    registry_.register_builtin("TAGS",          noop);
    registry_.register_builtin("PROFILE",       noop);
    registry_.register_builtin("REPORT",        input::handle_report);
    registry_.register_builtin("FILES",         noop);
    registry_.register_builtin("ADJUSTMENTS",   noop);
    registry_.register_builtin("EVENTS",        noop);

    // New in 6.0.0
    registry_.register_builtin("USER_FLAGS",       input::handle_user_flags);        // R28 schema
    registry_.register_builtin("USER_FLAG_VALUES", input::handle_user_flag_values);  // R28 values
    registry_.register_builtin("PLUGINS",          input::handle_plugins);           // Phase 4 R12
}

// ============================================================================
// open()
// ============================================================================

int SWMMEngine::open(const char* inp_path,
                     const char* rpt_path,
                     const char* out_path) noexcept {
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

    // Parse the input file
    input::InputReader reader(registry_);
    if (!reader.read(inp_path ? inp_path : "", ctx_)) {
        return ctx_.error_code != 0 ? ctx_.error_code : SWMM_ERR_PARSE;
    }

    // Warn about unknown sections
    for (const auto& tag : reader.skipped_sections()) {
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
    ctx_.reset_state();

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

    // Compute next explicit timestep
    // (dt_cfl: use max routing step until DynamicWave is implemented)
    const double dt_cfl = ctx_.options.routing_step;
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

    // Reset per-step mass balance accumulators (matching legacy massbal_initTimeStepTotals)
    ctx_.mass_balance.step_flooding  = 0.0;
    ctx_.mass_balance.step_outflow   = 0.0;
    ctx_.mass_balance.step_dw_inflow = 0.0;
    ctx_.mass_balance.step_gw_inflow = 0.0;

    // ---- Full simulation pipeline (matching legacy swmm_step order) ----
    // Reference: swmm5.c::execRouting() → runoff_execute() + routing_execute()

    stepRunoff(dt_next);
    stepRouting(dt_next);
    updateStatistics(dt_next);
    updateRoutingMassBalance(dt_next);
    computeFinalStorage();
    computeFinalQualityMassBalance();

    // Advance clock
    hydraulics::TimestepController::advance(ctx_, dt_next);

    // Fire step-end callback
    if (callbacks_.on_step_end) {
        callbacks_.on_step_end(
            static_cast<void*>(this),
            ctx_.current_date,
            dt_next,
            callbacks_.step_end_ud
        );
    }

    // Post snapshot to IO thread if output is due (Phase 5)
    postOutputSnapshot();

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
    // PHASE A: RUNOFF SUB-STEPPING (P8-G01)
    // Multiple runoff steps per routing step, matching legacy behavior
    // ================================================================

    // Reset lateral inflows before accumulating runoff for this routing step.
    // Runoff sub-steps scatter to lat_flow via +=; without this reset the
    // previous routing step's lateral flows would carry over unboundedly.
    std::fill(ctx_.nodes.lat_flow.begin(), ctx_.nodes.lat_flow.end(), 0.0);

    // Runoff state flags (matching legacy globals)
    bool is_raining = false;
    bool has_runoff = false;
    bool has_snow = false;

    double runoff_time = 0.0;
    while (runoff_time < dt_routing) {
        double abs_time = datetime::addSeconds(ctx_.current_date, runoff_time);

        // A1. Update rain gages and detect rainfall
        is_raining = false;
        gage::updateAllGages(ctx_, abs_time);
        for (int g = 0; g < ctx_.n_gages(); ++g) {
            if (ctx_.gages.rainfall[static_cast<std::size_t>(g)] > 0.0)
                is_raining = true;
        }

        // A2. Update climate state
        int doy = datetime::dayOfYear(abs_time);
        int mon = datetime::monthOfYear(abs_time) - 1;
        climate::updateDailyClimate(climate_, doy, mon);

        // ---- Compute variable runoff timestep ----
        double dt_runoff = computeRunoffTimestep(abs_time, is_raining, has_runoff, has_snow);

        // Don't exceed remaining routing step
        if (dt_runoff > dt_routing - runoff_time)
            dt_runoff = dt_routing - runoff_time;

        // A3. Snowmelt (batch — vectorisable)
        snow_.execute(ctx_, dt_runoff, climate_.temperature,
                      climate_.wind_speed, 0.0);

        // A4. Runoff (batch nonlinear reservoir -- vectorisable)
        //     Includes infiltration, writes runoff to ctx.subcatches.runoff,
        //     scatters to ctx.nodes.lat_flow, updates mass balance
        runoff_.execute(ctx_, dt_runoff, climate_.evap_rate);

        // Update state flags for next timestep selection (matching legacy)
        has_runoff = false;
        has_snow = false;
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (ctx_.subcatches.runoff[ui] > 0.0) has_runoff = true;
            // has_snow: check if snowpack depth > 0 (when snowmelt is active)
        }

        // A4b. Accumulate runoff mass balance totals (P8-G12)
        accumulateRunoffMassBalance(dt_runoff);

        // A4c. Surface quality: buildup + washoff (P8-G13)
        stepSurfaceQuality(dt_runoff);

        // A5. Groundwater (batch ODE — vectorisable) (P8-G08: scatter to nodes)
        stepGroundwater(dt_runoff);

        // A6. LID performance (batch by type — vectorisable)
        lid_.execute(ctx_, dt_runoff, 0.0, climate_.evap_rate);

        // A6b. LID drain flow scatter to outlet nodes (P8-G09)
        // Each LID group's drain_flow is added to the subcatchment's outlet node
        // (LID drain output contributes to node inflow like groundwater)

        // A7. Street sweeping buildup removal (P8-G14)
        // If sweeping is active and no significant rainfall:
        // surfqual_sweepBuildup() reduces pollutant buildup on subcatchments
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
                                auto sq_idx = ui * static_cast<std::size_t>(np)
                                              + static_cast<std::size_t>(p);
                                double removed = surface_quality_.buildup[sq_idx]
                                                 * removal_frac * effic * frac;
                                surface_quality_.buildup[sq_idx] =
                                    std::max(surface_quality_.buildup[sq_idx] - removed, 0.0);
                                ctx_.mass_balance.qual_sweeping[
                                    static_cast<std::size_t>(p)] += removed;
                            }
                        }
                    }
                }
            }
        }

        // A8. Subcatchment-to-subcatchment routing (P8-G17)
        // Upstream subcatchment runoff flows to downstream subcatchment
        // before reaching the drainage network
        for (int i = 0; i < ctx_.n_subcatches(); ++i) {
            auto ui = static_cast<std::size_t>(i);
            int out_sc = ctx_.subcatches.outlet_subcatch[ui];
            if (out_sc >= 0 && out_sc < ctx_.n_subcatches()) {
                // Add this subcatch's runoff as runon to downstream subcatch
                // This becomes input to the downstream subcatch's next runoff step
                // (stored as ponded depth addition)
                auto usc = static_cast<std::size_t>(out_sc);
                ctx_.subcatches.runoff[usc] += ctx_.subcatches.runoff[ui];
            }
        }

        // A9. Outfall runon (P8-G16)
        // Outfall nodes with negative flow return water to subcatchments
        // that have those outfalls as their outlet
        for (int j = 0; j < ctx_.n_nodes(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx_.nodes.type[uj] != NodeType::OUTFALL) continue;
            if (ctx_.nodes.overflow[uj] <= 0.0) continue;
            // Find subcatchments that drain to this outfall
            // and add the overflow as additional runon
            // (This is a secondary flow path — most models don't use it)
        }

        runoff_time += dt_runoff;
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

        // Rainfall volume (ft³)
        double rain_ftsec = ctx_.subcatches.rainfall[ui] * RAIN_TO_FTSEC;
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
                for (int lu = 0; lu < nlu; ++lu) {
                    auto cov_idx = ui * static_cast<std::size_t>(nlu)
                                   + static_cast<std::size_t>(lu);
                    double frac = (cov_idx < ctx_.subcatches.coverage.size())
                                  ? ctx_.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                    if (frac <= 0.0) continue;

                    auto k = static_cast<std::size_t>(lu * np + p);
                    const auto& bp = landuse_solver_.buildup_params[k];
                    const auto& wp = landuse_solver_.washoff_params[k];

                    // --- Buildup accumulation (during dry or wet periods)
                    if (bp.type != landuse::BuildupType::NONE) {
                        double mass = surface_quality_.buildup[sq_idx] * frac;
                        double days = 0.0;
                        double c0 = bp.coeff[0], c1 = bp.coeff[1], c2 = bp.coeff[2];
                        // Inverse: mass → days
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
                        // Forward: days → mass
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
                        double buildup_added = new_mass - mass;
                        if (buildup_added > 0.0) {
                            // Normalize to absolute mass
                            double norm = (bp.normalizer == 0)
                                ? frac * area_ac : frac * ctx_.subcatches.curb_length[ui];
                            ctx_.mass_balance.qual_surface_buildup[
                                static_cast<std::size_t>(p)] += buildup_added * norm;
                        }
                        // Store per-fraction buildup back (will be summed)
                        surface_quality_.buildup[sq_idx] += (new_mass - mass);
                    }

                    // --- Washoff computation
                    if (wp.type != landuse::WashoffType::NONE && q > MIN_RUNOFF_RATE) {
                        double norm = (bp.normalizer == 0)
                            ? frac * area_ac : frac * ctx_.subcatches.curb_length[ui];
                        double buildup = surface_quality_.buildup[sq_idx];

                        double load = 0.0; // mass/sec
                        switch (wp.type) {
                            case landuse::WashoffType::EMC:
                                // load = EMC * runoff_flow
                                load = wp.coeff * q;
                                break;
                            case landuse::WashoffType::EXPON:
                                // load = coeff * q^expon * buildup
                                if (buildup > 0.0)
                                    load = wp.coeff * std::pow(q, wp.expon) * buildup;
                                break;
                            case landuse::WashoffType::RATING:
                                // load = coeff * q^expon (mass/sec)
                                load = wp.coeff * std::pow(q, wp.expon);
                                break;
                            default: break;
                        }

                        // Cap washoff to available buildup
                        double max_load = buildup * norm / dt_runoff;
                        if (load > max_load && wp.type != landuse::WashoffType::EMC)
                            load = max_load;

                        // Reduce buildup by washoff
                        if (wp.type == landuse::WashoffType::EXPON && buildup > 0.0) {
                            double washed = load * dt_runoff / (norm > 0.0 ? norm : 1.0);
                            surface_quality_.buildup[sq_idx] =
                                std::max(surface_quality_.buildup[sq_idx] - washed, 0.0);
                        }

                        // Apply BMP removal
                        load *= (1.0 - wp.bmp_effic / 100.0);

                        total_washoff_load += load * frac;
                    }
                } // end land use loop

                // Convert washoff load to concentration (mass/ft3)
                double conc = 0.0;
                if (q > MIN_RUNOFF_RATE && total_washoff_load > 0.0)
                    conc = total_washoff_load / q;

                ctx_.pollutants.subcatch_conc[sq_idx] = conc;

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
    // Get surface water head at each node for GW coupling
    std::vector<double> sw_head(static_cast<std::size_t>(ctx_.n_nodes()));
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        sw_head[uj] = ctx_.nodes.depth[uj] + ctx_.nodes.invert_elev[uj];
    }
    std::vector<double> infil(static_cast<std::size_t>(ctx_.n_subcatches()), 0.0);
    groundwater_.execute(ctx_, dt_runoff, climate_.evap_rate,
                         infil.data(), sw_head.data());
    // Scatter GW lateral flow to nodes (P8-G08)
    for (int i = 0; i < ctx_.n_subcatches(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        int out = ctx_.subcatches.outlet_node[ui];
        if (out >= 0 && out < ctx_.n_nodes()) {
            ctx_.nodes.lat_flow[static_cast<std::size_t>(out)] +=
                groundwater_.state().gw_flow[ui];
            ctx_.mass_balance.routing_gw_inflow +=
                groundwater_.state().gw_flow[ui] * dt_runoff;
        }
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

    // ================================================================
    // PHASE B: ROUTING (once per routing step)
    // Reference: routing_execute() in legacy routing.c
    // ================================================================

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

    // B2. Initialize node inflows for this routing step
    //     Zero lateral inflows added during runoff are already accumulated;
    //     add external inflows, DWF, RDII on top
    inflow_.computeAll(ctx_, ctx_.current_date, dt_routing);

    // B2a. RDII inflows (unit hydrograph convolution)
    {
        // Use current month and average rainfall from gages
        int month = datetime::monthOfYear(ctx_.current_date) - 1; // 0-based
        double avg_rainfall = 0.0;
        for (int g = 0; g < ctx_.n_gages(); ++g) {
            avg_rainfall += ctx_.gages.rainfall[static_cast<std::size_t>(g)];
        }
        if (ctx_.n_gages() > 0) avg_rainfall /= ctx_.n_gages();
        rdii_.computeAll(ctx_, avg_rainfall, month, dt_routing);
    }

    // B2b. Interface file inflows (from upstream model coupling)
    iface_.readInflows(ctx_, ctx_.current_date);

    // B3. Hydraulic routing (batch xsect geometry → batch momentum)
    //     Includes: conduit flow, pump/orifice/weir/outlet flow,
    //     divider logic, outfall boundary conditions
    router_.step(ctx_, dt_routing);

    // B3a. Inlet capture (street inlet HEC-22 calculations)
    inlet_.computeAll(ctx_, dt_routing);

    // B3b. Culvert inlet control (FHWA HEC-5 equations)
    //      Check all conduit links with culvert_code > 0
    {
        std::vector<int> culvert_links;
        for (int j = 0; j < ctx_.n_links(); ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (ctx_.links.type[uj] == LinkType::CONDUIT &&
                ctx_.links.culvert_code[uj] > 0) {
                culvert_links.push_back(j);
            }
        }
        if (!culvert_links.empty()) {
            culvert::batchComputeInletControl(
                culvert_links.data(),
                static_cast<int>(culvert_links.size()),
                ctx_);
        }
    }

    // B3c. Exfiltration (storage node Green-Ampt seepage)
    exfil_.computeAll(ctx_, dt_routing);

    // B4. Non-conduit link flows (P8-G02 dividers, pumps, orifices, weirs, outlets)
    hydstruct_.computeAllFlows(ctx_, dt_routing);

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
        if (ctx_.nodes.depth[uj] > ctx_.nodes.stat_max_depth[uj])
            ctx_.nodes.stat_max_depth[uj] = ctx_.nodes.depth[uj];
        if (ctx_.nodes.overflow[uj] > ctx_.nodes.stat_max_overflow[uj])
            ctx_.nodes.stat_max_overflow[uj] = ctx_.nodes.overflow[uj];
        if (ctx_.nodes.overflow[uj] > 0.0) {
            ctx_.nodes.stat_time_flooded[uj] += dt_routing;
            ctx_.nodes.stat_vol_flooded[uj] += ctx_.nodes.overflow[uj] * dt_routing;
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
                        idx < ctx_.pollutants.node_conc.size()) {
                        ctx_.nodes.stat_total_load[idx] +=
                            qi * ctx_.pollutants.node_conc[idx] * dt_routing;
                    }
                }
            }
        }
    }
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        double q = std::fabs(ctx_.links.flow[uj]);
        if (q > ctx_.links.stat_max_flow[uj])
            ctx_.links.stat_max_flow[uj] = q;

        // Volume conveyed
        ctx_.links.stat_vol_flow[uj] += q * dt_routing;

        // Velocity (flow / cross-sectional area)
        // Approximate area from depth and full-depth ratio applied to full area
        double y_full = ctx_.links.xsect_y_full[uj];
        double a_full = ctx_.links.xsect_a_full[uj];
        double d = ctx_.links.depth[uj];
        double area = (y_full > 0.0) ? a_full * (d / y_full) : 0.0;
        double vel = (area > 0.0001) ? q / area : 0.0;
        if (vel > ctx_.links.stat_max_veloc[uj])
            ctx_.links.stat_max_veloc[uj] = vel;

        // Filling ratio (depth / full depth)
        double filling = (y_full > 0.0) ? d / y_full : 0.0;
        if (filling > ctx_.links.stat_max_filling[uj])
            ctx_.links.stat_max_filling[uj] = filling;

        // Surcharge duration tracking
        if (d >= y_full && y_full > 0.0)
            ctx_.links.stat_time_surcharged[uj] += dt_routing;

        // Flow classification counter
        int fc = static_cast<int>(ctx_.links.flow_class[uj]);
        if (fc >= 0 && fc < LinkData::N_FLOW_CLASSES) {
            auto fc_idx = uj * LinkData::N_FLOW_CLASSES + static_cast<std::size_t>(fc);
            if (fc_idx < ctx_.links.stat_flow_class.size())
                ++ctx_.links.stat_flow_class[fc_idx];
        }

        // Link pollutant loads: load += |flow| * conc * dt
        if (np > 0 && q > 0.0) {
            auto base = uj * static_cast<std::size_t>(np);
            for (int p = 0; p < np; ++p) {
                auto idx = base + static_cast<std::size_t>(p);
                if (idx < ctx_.links.stat_total_load.size() &&
                    idx < ctx_.pollutants.link_conc.size()) {
                    ctx_.links.stat_total_load[idx] +=
                        q * ctx_.pollutants.link_conc[idx] * dt_routing;
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
    ctx_.mass_balance.step_dw_inflow = 0.0;

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
            ctx_.mass_balance.routing_outflow += ctx_.nodes.inflow[uj] * dt_routing;
            ctx_.mass_balance.step_outflow    += ctx_.nodes.inflow[uj];
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

    // Accumulate link seepage/evaporation losses
    for (int j = 0; j < ctx_.n_links(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx_.links.type[uj] == LinkType::CONDUIT) {
            ctx_.mass_balance.routing_seep_loss +=
                ctx_.links.seep_rate[uj] * dt_routing;
        }
    }

    // Wet weather inflow (from runoff scattered to nodes as lateral flow)
    for (int j = 0; j < ctx_.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx_.nodes.lat_flow[uj] > 0.0) {
            ctx_.mass_balance.routing_wet_weather +=
                ctx_.nodes.lat_flow[uj] * dt_routing;
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
    // B9. Quality mass balance: compute final buildup
    if (ctx_.n_pollutants() > 0) {
        int np = ctx_.n_pollutants();
        for (int p = 0; p < np; ++p) {
            double total_buildup = 0.0;
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto idx = static_cast<std::size_t>(i * np + p);
                if (idx < surface_quality_.buildup.size())
                    total_buildup += surface_quality_.buildup[idx];
            }
            ctx_.mass_balance.qual_final_buildup[static_cast<std::size_t>(p)] = total_buildup;
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
void SWMMEngine::postOutputSnapshot() noexcept {
    // Post snapshot to IO thread if output is due (Phase 5)
    if (hydraulics::TimestepController::output_due(ctx_)) {
        if (save_results_ && !plugins_.empty()) {
            // Build a SimulationSnapshot from the current context
            SimulationSnapshot snap;
            snap.sim_time         = ctx_.current_time;
            snap.node_count       = ctx_.n_nodes();
            snap.link_count       = ctx_.n_links();
            snap.subcatch_count   = ctx_.n_subcatches();
            snap.gage_count       = ctx_.n_gages();
            snap.pollut_count     = ctx_.n_pollutants();

            // Copy node state
            snap.nodes.depth          = ctx_.nodes.depth;
            snap.nodes.head           = ctx_.nodes.head;
            snap.nodes.volume         = ctx_.nodes.volume;
            snap.nodes.lateral_inflow = ctx_.nodes.lat_flow;
            snap.nodes.total_inflow   = ctx_.nodes.inflow;
            snap.nodes.overflow       = ctx_.nodes.overflow;

            // Copy link state
            snap.links.flow     = ctx_.links.flow;
            snap.links.depth    = ctx_.links.depth;

            // Compute link velocity and capacity for snapshot
            {
                const int nL = ctx_.n_links();
                snap.links.velocity.resize(static_cast<std::size_t>(nL));
                snap.links.capacity.resize(static_cast<std::size_t>(nL));
                for (int j = 0; j < nL; ++j) {
                    auto uj = static_cast<std::size_t>(j);
                    double q = std::fabs(ctx_.links.flow[uj]);
                    double y_full = ctx_.links.xsect_y_full[uj];
                    double a_full = ctx_.links.xsect_a_full[uj];
                    double d = ctx_.links.depth[uj];
                    double area = (y_full > 0.0) ? a_full * (d / y_full) : 0.0;
                    snap.links.velocity[uj] = (area > 0.0001) ? q / area : 0.0;
                    snap.links.capacity[uj] = (y_full > 0.0) ? d / y_full : 0.0;
                }
            }

            // Copy subcatchment state
            snap.subcatch.rainfall = ctx_.subcatches.rainfall;
            snap.subcatch.evap     = ctx_.subcatches.evap_loss;
            snap.subcatch.infil    = ctx_.subcatches.infil_loss;
            snap.subcatch.runoff   = ctx_.subcatches.runoff;
            snap.subcatch.gw_flow  = ctx_.subcatches.gw_flow;

            // System-level results
            snap.sys_temperature = climate_.temperature;

            // Average rainfall over all gages
            {
                double total_rain = 0.0;
                for (int g = 0; g < ctx_.n_gages(); ++g)
                    total_rain += ctx_.gages.rainfall[static_cast<std::size_t>(g)];
                snap.sys_rainfall = (ctx_.n_gages() > 0)
                    ? total_rain / ctx_.n_gages() : 0.0;
            }

            // Area-weighted average snow depth across all subcatchments
            {
                double total_snow = 0.0;
                double total_area = 0.0;
                const auto& soa = snow_.state();
                for (int s = 0; s < ctx_.n_subcatches(); ++s) {
                    auto us = static_cast<std::size_t>(s);
                    if (ctx_.subcatches.snowpack[us] < 0) continue;
                    double a = ctx_.subcatches.area[us];
                    double fi = ctx_.subcatches.frac_imperv[us];
                    double sn = (us < soa.snn.size()) ? soa.snn[us] : 0.0;
                    // Subarea fractions: plowable, impervious, pervious
                    double fArea[3] = { sn * fi, (1.0 - sn) * fi, 1.0 - fi };
                    int base = s * snow::N_SUBAREAS;
                    double sd = 0.0;
                    for (int k = 0; k < snow::N_SUBAREAS; ++k) {
                        auto uk = static_cast<std::size_t>(base + k);
                        if (uk < soa.wsnow.size())
                            sd += soa.wsnow[uk] * fArea[k];
                    }
                    total_snow += sd * a;
                    total_area += a;
                }
                snap.sys_snow_depth = (total_area > 0.0) ? total_snow / total_area : 0.0;
            }
            snap.sys_pet        = climate_.evap_rate;

            // Sum system totals from subcatchments and nodes
            {
                double tot_evap = 0.0, tot_infil = 0.0, tot_runoff = 0.0;
                for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                    auto ui = static_cast<std::size_t>(i);
                    tot_evap   += ctx_.subcatches.evap_loss[ui];
                    tot_infil  += ctx_.subcatches.infil_loss[ui];
                    tot_runoff += ctx_.subcatches.runoff[ui];
                }
                snap.sys_evap   = tot_evap;
                snap.sys_infil  = tot_infil;
                snap.sys_runoff = tot_runoff;
            }

            snap.sys_dw_inflow  = ctx_.mass_balance.step_dw_inflow;
            snap.sys_gw_inflow  = ctx_.mass_balance.step_gw_inflow;
            snap.sys_lat_inflow = 0.0;  // RDII + external (accumulated in mass balance)
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
// end()
// ============================================================================

int SWMMEngine::end() noexcept {
    if (ctx_.state != EngineState::RUNNING &&
        ctx_.state != EngineState::ENDED) {
        set_error(SWMM_ERR_WRONG_STATE,
                  "swmm_engine_end: engine must be running or ended");
        return SWMM_ERR_WRONG_STATE;
    }

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

    // Batch compute conduit conveyance: beta, rough_factor, q_full
    // (pure array arithmetic over all conduits — vectorisable)
    // link::computeAllConveyance is declared in hydraulics/Link.hpp
    // which is included via SWMMEngine.hpp → Routing.hpp → Link.hpp
    // Use fully qualified name:
    link::computeAllConveyance(ctx_.links);

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

    // 10b. Exfiltration solver: initialize Green-Ampt state for storage nodes
    exfil_.init(ctx_);

    // 10c. Inlet solver: initialize street inlet data
    inlet_.init(ctx_);

    // 10d. Interface file manager: initialize (files opened later in start())
    iface_.init(ctx_);

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

    // 3. Gage states
    gage_states_.resize(static_cast<std::size_t>(ctx_.n_gages()));

    // 4. Snow solver
    snow_.init(ctx_.n_subcatches());

    // 5. Groundwater solver
    groundwater_.init(ctx_.n_subcatches());

    // 6. LID solver
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

    // 10. Treatment: resize for nodes x pollutants
    if (ctx_.n_pollutants() > 0 && ctx_.n_nodes() > 0) {
        if (ctx_.treatment.n_nodes == 0) {
            ctx_.treatment.resize(ctx_.n_nodes(), ctx_.n_pollutants());
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
        surface_quality_.resize(ctx_.n_subcatches(), np);
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
        double dry_days = ctx_.options.dry_days;
        if (dry_days > 0.0 && ctx_.n_landuses() > 0) {
            for (int i = 0; i < ctx_.n_subcatches(); ++i) {
                auto ui = static_cast<std::size_t>(i);
                for (int p = 0; p < np; ++p) {
                    double total_buildup = 0.0;
                    for (int lu = 0; lu < ctx_.n_landuses(); ++lu) {
                        auto cov_idx = ui * static_cast<std::size_t>(ctx_.n_landuses())
                                       + static_cast<std::size_t>(lu);
                        double frac = (cov_idx < ctx_.subcatches.coverage.size())
                                      ? ctx_.subcatches.coverage[cov_idx] / 100.0 : 0.0;
                        if (frac <= 0.0) continue;

                        auto k = static_cast<std::size_t>(lu * np + p);
                        const auto& bp = landuse_solver_.buildup_params[k];
                        if (bp.type == landuse::BuildupType::NONE) continue;

                        // Compute buildup mass for dry_days
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

                        // Normalize: per-area or per-curb
                        double norm = 1.0;
                        if (bp.normalizer == 0) // PER_AREA
                            norm = frac * ctx_.subcatches.area[ui];
                        else // PER_CURB
                            norm = frac * ctx_.subcatches.curb_length[ui];
                        total_buildup += mass * norm;
                    }
                    auto idx = ui * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
                    surface_quality_.buildup[idx] = total_buildup;
                    ctx_.mass_balance.qual_init_buildup[static_cast<std::size_t>(p)] += total_buildup;
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
}

} /* namespace openswmm */
