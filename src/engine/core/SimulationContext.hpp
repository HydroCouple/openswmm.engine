/**
 * @file SimulationContext.hpp
 * @brief The central, reentrant simulation context for the new engine.
 *
 * @details SimulationContext is the single owner of all simulation state.
 *          It replaces the ~70 global variables in src/solver/globals.h with
 *          a value type that is thread-safe (one context per thread) and
 *          trivially clonable for parallel runs.
 *
 * ### Architecture summary
 *
 * | Legacy (globals.h) | New location |
 * |--------------------|-------------|
 * | `FlowUnits`, `RouteModel`, … | `ctx.options` (SimulationOptions) |
 * | `StartDate`, `EndDate`, `ElapsedTime`, … | `ctx.current_time`, `ctx.options.*` |
 * | `Node[]`, `Nnodes[]` | `ctx.nodes` (NodeData) + `ctx.node_names` (NameIndex) |
 * | `Link[]`, `Nlinks[]` | `ctx.links` (LinkData) + `ctx.link_names` (NameIndex) |
 * | `Subcatch[]`, `Nsubcatch` | `ctx.subcatches` (SubcatchData) + `ctx.subcatch_names` |
 * | `Gage[]`, `Ngages` | `ctx.gages` (GageData) + `ctx.gage_names` |
 * | `Pollut[]`, `Npolluts` + quality state | `ctx.pollutants` (PollutantData) |
 * | `Tseries[]` / `Curve[]` | `ctx.tables` (TableData) + `ctx.table_names` |
 * | `Coord[]` | `ctx.spatial` (SpatialFrame) |
 * | — (new) | `ctx.user_flags` (UserFlags) |
 *
 * ### Lifecycle
 *
 * ```
 * ctx.reset()          — full cold reset (call before re-running)
 * ctx.save_state()     — snapshot current state into old-step arrays
 * ctx.advance(dt)      — advance simulation clock by dt seconds
 * ctx.output_due()     — true when dt_output_remaining <= EPSILON
 * ctx.is_complete()    — true when current_time >= end_time
 * ```
 *
 * ### Thread safety
 *
 * A SimulationContext must not be shared between threads without external
 * synchronization. The IO thread receives a **deep copy** (SimulationSnapshot)
 * posted through the ring queue — not a pointer to the live context.
 *
 * @see Legacy reference: src/solver/globals.h
 * @see src/engine/data/NodeData.hpp
 * @see src/engine/data/LinkData.hpp
 * @see src/engine/data/SubcatchData.hpp
 * @see src/engine/data/GageData.hpp
 * @see src/engine/data/PollutantData.hpp
 * @see src/engine/data/TableData.hpp
 * @see src/engine/data/NameIndex.hpp
 * @see src/engine/core/SimulationOptions.hpp
 * @see src/engine/core/SpatialFrame.hpp
 * @see src/engine/core/UserFlags.hpp
 * @see include/openswmm/plugin_sdk/SimulationSnapshot.hpp
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_SIMULATION_CONTEXT_HPP
#define OPENSWMM_ENGINE_SIMULATION_CONTEXT_HPP

#include <cmath>
#include "../data/GageData.hpp"
#include "../data/LinkData.hpp"
#include "../data/NameIndex.hpp"
#include "../data/NodeData.hpp"
#include "../data/PollutantData.hpp"
#include "../data/SubcatchData.hpp"
#include "../data/TableData.hpp"
#include "SimulationOptions.hpp"
#include "SpatialFrame.hpp"
#include "UserFlags.hpp"
#include "../data/QualityData.hpp"
#include "../data/InflowData.hpp"
#include "../data/InfraData.hpp"
#include "../data/HydrologyData.hpp"
#include "../data/ForcingData.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace openswmm {

// ============================================================================
// Plugin specification (from [PLUGINS] section)
// ============================================================================

/**
 * @brief One plugin entry from the [PLUGINS] section.
 *
 * @details path is the shared library path (.so/.dylib/.dll).
 *          init_args are the remaining tokens on the same line, passed
 *          verbatim to IOutputPlugin::initialize() / IReportPlugin::initialize().
 *
 * @ingroup engine_core
 */
struct PluginSpec {
    std::string              path;       ///< Shared library path
    std::vector<std::string> init_args;  ///< Extra tokens from the [PLUGINS] row
};

// ============================================================================
// Engine state enumeration
// ============================================================================

/**
 * @brief High-level engine lifecycle state.
 *
 * @details Mirrors SWMM_EngineState in include/openswmm/engine/openswmm_engine.h
 *          but lives in the C++ domain (scoped enum).
 *
 * @ingroup engine_core
 */
enum class EngineState : int32_t {
    CREATED      = 0,  ///< Context allocated, no input loaded
    OPENED       = 1,  ///< Input file parsed, objects allocated
    INITIALIZED  = 2,  ///< Initial conditions applied
    RUNNING      = 3,  ///< Simulation loop in progress
    PAUSED       = 4,  ///< Simulation paused (future hot-swap support)
    ENDED        = 5,  ///< Simulation loop completed
    REPORTED     = 6,  ///< Summary report written
    CLOSED       = 7,  ///< Resources released
    ERROR_STATE  = 8,  ///< Fatal error; call swmm_engine_last_error()
    BUILDING     = 9   ///< Programmatic model construction in progress (no .inp)
};

// ============================================================================
// SimulationContext
// ============================================================================

/**
 * @brief Central, reentrant simulation context.
 *
 * @details Owns all simulation data. Passed by (non-const) reference to every
 *          solver function. One context per concurrent simulation.
 *
 * @ingroup engine_core
 */
struct SimulationContext {

    // =========================================================================
    // Engine state
    // =========================================================================

    /** @brief Current lifecycle state of the engine. */
    EngineState state = EngineState::CREATED;

    // =========================================================================
    // Project title / notes
    // =========================================================================

    /**
     * @brief Project title and notes (from [TITLE] section).
     *
     * @details Each entry is one line of free-form text from the [TITLE]
     *          section of the .inp file. Lines beginning with ";;" (comments)
     *          are stripped by the input reader before reaching the handler.
     *
     * @see Legacy: Title[MAXTITLE] in globals.h (limited to 3 lines)
     */
    std::vector<std::string> title_notes;

    // =========================================================================
    // Options & configuration
    // =========================================================================

    /**
     * @brief Parsed simulation options (from [OPTIONS] section).
     * @see Legacy: FlowUnits, RouteModel, RouteStep, … in globals.h
     */
    SimulationOptions options;

    // =========================================================================
    // Simulation clock
    // =========================================================================

    /**
     * @brief Current simulation time (decimal days from start_date).
     *
     * @details Updated by TimestepController::advance(). The value 0.0
     *          corresponds to options.start_date.
     *
     * @see Legacy: ElapsedTime in globals.h
     */
    double current_time = 0.0;

    /**
     * @brief Absolute current date/time (decimal days, Julian date).
     *
     * @details = options.start_date + current_time
     *
     * @see Legacy: StartDateTime + ElapsedTime in globals.h
     */
    double current_date = 0.0;

    /**
     * @brief Time remaining until the next output boundary (seconds).
     *
     * @details Managed by TimestepController. Decremented each step;
     *          reset to options.report_step by reset_output_timer().
     *
     * @see TimestepController::advance(), TimestepController::output_due()
     */
    double dt_output_remaining = 0.0;

    /**
     * @brief Time remaining until the next control rule event (seconds).
     *
     * @details 0.0 means no control step scheduled; updated by the control
     *          rule processor.
     */
    double dt_controls_remaining = 0.0;

    // =========================================================================
    // Object data stores (Structure-of-Arrays)
    // =========================================================================

    /**
     * @brief All node state and properties.
     * @see Legacy: Node[], NodeStats[] in globals.h + TNode in objects.h
     */
    NodeData nodes;

    /**
     * @brief All link state and properties.
     * @see Legacy: Link[], LinkStats[] in globals.h + TLink in objects.h
     */
    LinkData links;

    /**
     * @brief All subcatchment state and properties.
     * @see Legacy: Subcatch[], SubcatchStats[] in globals.h + TSubcatch
     */
    SubcatchData subcatches;

    /**
     * @brief All rain gage state and properties.
     * @see Legacy: Gage[] in globals.h + TGage in objects.h
     */
    GageData gages;

    /**
     * @brief Pollutant definitions and per-object quality state.
     * @see Legacy: Pollut[], node.newQual[], link.newQual[] in globals.h
     */
    PollutantData pollutants;

    /**
     * @brief All time series and rating curves.
     * @see Legacy: Tseries[], Curve[] in globals.h + TTable in objects.h
     */
    TableData tables;

    // =========================================================================
    // Name-to-index lookup (O(1))
    // =========================================================================

    /**
     * @brief Node name → node index.
     * @see Legacy: project_findObject(NODE, name) in project.c
     */
    NameIndex node_names;

    /**
     * @brief Link name → link index.
     * @see Legacy: project_findObject(LINK, name) in project.c
     */
    NameIndex link_names;

    /**
     * @brief Subcatchment name → subcatchment index.
     * @see Legacy: project_findObject(SUBCATCH, name) in project.c
     */
    NameIndex subcatch_names;

    /**
     * @brief Rain gage name → gage index.
     * @see Legacy: project_findObject(GAGE, name) in project.c
     */
    NameIndex gage_names;

    /**
     * @brief Pollutant name → pollutant index.
     * @see Legacy: project_findObject(POLLUT, name) in project.c
     */
    NameIndex pollutant_names;

    /**
     * @brief Time series / curve name → table index.
     * @see Legacy: project_findObject(TIMESERIES, name) in project.c
     */
    NameIndex table_names;

    // =========================================================================
    // Quality data (landuse, buildup, washoff, treatment)
    // =========================================================================

    NameIndex        landuse_names;
    LanduseData      landuses;
    BuildupData      buildup;
    WashoffData      washoff;
    TreatmentData    treatment;

    // =========================================================================
    // Inflow data (external, DWF, RDII, patterns)
    // =========================================================================

    ExtInflowData    ext_inflows;
    DwfData          dwf_inflows;
    RDIIAssignData   rdii_assigns;
    UnitHydData      unit_hyds;      ///< Parsed [HYDROGRAPHS] data
    PatternData      patterns;

    // =========================================================================
    // Infrastructure data (transects, streets, inlets, controls)
    // =========================================================================

    TransectStore    transects;
    StreetStore      streets;
    InletStore       inlets;
    InletUsageStore  inlet_usages;
    ControlRuleStore control_rules;

    // =========================================================================
    // Hydrology data (snowpacks, aquifers, LID)
    // =========================================================================

    SnowpackStore    snowpacks;
    NameIndex        snowpack_names;
    AquiferStore     aquifers;
    NameIndex        aquifer_names;
    LidControlStore  lid_controls;
    NameIndex        lid_names;
    LidUsageStore    lid_usage;

    // =========================================================================
    // Spatial data
    // =========================================================================

    /**
     * @brief Coordinate reference system and georeferenced coordinates.
     * @see Legacy: Coord[] in globals.h
     */
    SpatialFrame spatial;

    // =========================================================================
    // User-defined flags
    // =========================================================================

    /**
     * @brief User-defined flags from [USER_FLAGS] section.
     * @details New in 6.0.0. No legacy equivalent.
     * @see UserFlags.hpp
     */
    UserFlags user_flags;

    // =========================================================================
    // Runtime forcing data
    // =========================================================================

    /**
     * @brief Per-element runtime forcing state (lateral inflows, head
     *        boundaries, rainfall, evap, link settings, quality mass fluxes).
     * @details Set via swmm_forcing_*() API. Applied by applyForcings() in
     *          the step loop. Auto-cleared by clear_reset_entries() at end
     *          of each step (for RESET-persistence entries).
     * @see ForcingData.hpp, openswmm_forcing.h
     */
    ForcingData forcing;

    // =========================================================================
    // Plugin specifications (from [PLUGINS] section)
    // =========================================================================

    /**
     * @brief Plugin library specs parsed from [PLUGINS].
     * @details Consumed by PluginFactory::load_plugins() during open().
     *          New in 6.0.0 — no legacy equivalent.
     */
    std::vector<PluginSpec> plugin_specs;

    // =========================================================================
    // Error / warning tracking
    // =========================================================================

    /**
     * @brief Most recent error code (0 = no error).
     * @see Legacy: ErrorCode in globals.h
     */
    int error_code = 0;

    /**
     * @brief Most recent warning code (0 = no warning).
     * @see Legacy: WarningCode in globals.h
     */
    int warning_code = 0;

    /**
     * @brief Human-readable message for the last error/warning.
     */
    std::string error_message;

    // =========================================================================
    // Mass balance accumulators (SoA — vectorisable batch updates)
    // =========================================================================

    /**
     * @brief Cumulative mass balance totals for runoff and routing.
     *
     * @details Updated every timestep by the runoff and routing modules.
     *          Stored as contiguous doubles for batch accumulation.
     *          All volumes in ft3, all flows integrated to volumes.
     *
     * @see Legacy: RunoffTotals, RoutingTotals in massbal.c
     */
    struct MassBalance {
        // Runoff totals
        double runoff_rainfall   = 0.0;  ///< Total rainfall volume (ft3)
        double runoff_evap       = 0.0;  ///< Total evaporation volume (ft3)
        double runoff_infil      = 0.0;  ///< Total infiltration volume (ft3)
        double runoff_runoff     = 0.0;  ///< Total surface runoff volume (ft3)
        double runoff_snowremov  = 0.0;  ///< Total snow removal volume (ft3)
        double runoff_init_store = 0.0;  ///< Initial surface storage (ft3)
        double runoff_final_store= 0.0;  ///< Final surface storage (ft3)

        // Routing totals
        double routing_dry_weather   = 0.0;
        double routing_wet_weather   = 0.0;
        double routing_gw_inflow     = 0.0;
        double routing_rdii          = 0.0;
        double routing_external      = 0.0;
        double routing_flooding      = 0.0;
        double routing_outflow       = 0.0;
        double routing_evap_loss     = 0.0;
        double routing_seep_loss     = 0.0;
        double routing_init_storage  = 0.0;
        double routing_final_storage = 0.0;

        // User-forced volumes (diagnostic — subset of routing_external)
        double routing_forcing_inflow = 0.0; ///< Cumulative user-forced lateral inflow (ft3)

        // Groundwater mass balance totals (all in feet — depth per unit area)
        // Matches legacy TGwaterTotals
        double gw_infil         = 0.0; ///< Cumulative infiltration to GW (ft)
        double gw_upper_evap    = 0.0; ///< Cumulative upper zone evaporation (ft)
        double gw_lower_evap    = 0.0; ///< Cumulative lower zone evaporation (ft)
        double gw_lower_perc    = 0.0; ///< Cumulative deep percolation (ft)
        double gw_lateral_flow  = 0.0; ///< Cumulative lateral GW flow (ft)
        double gw_init_storage  = 0.0; ///< Initial GW storage (ft)
        double gw_final_storage = 0.0; ///< Final GW storage (ft)

        // Per-step accumulators (reset each step for reporting)
        double step_flooding     = 0.0;
        double step_outflow      = 0.0;
        double step_dw_inflow    = 0.0;
        double step_gw_inflow    = 0.0;
        double step_rdii_inflow  = 0.0;
        double step_ext_inflow   = 0.0;

        // Quality mass balance (per-pollutant, in mass units)
        std::vector<double> qual_init_buildup;   ///< Initial buildup mass
        std::vector<double> qual_final_buildup;  ///< Final buildup mass
        std::vector<double> qual_surface_buildup; ///< Accumulated buildup during sim
        std::vector<double> qual_wet_deposition; ///< Wet deposition mass
        std::vector<double> qual_sweeping;       ///< Mass removed by sweeping
        std::vector<double> qual_bmp_removal;    ///< BMP treatment removal
        std::vector<double> qual_infil_loss;     ///< Mass lost to infiltration
        std::vector<double> qual_runoff_load;    ///< Mass load in surface runoff
        std::vector<double> qual_routing_wet;    ///< Wet weather quality inflow to routing
        std::vector<double> qual_routing_outflow;///< Quality mass leaving at outfalls
        std::vector<double> qual_routing_flood;  ///< Quality mass lost to flooding
        std::vector<double> qual_routing_init;   ///< Initial stored quality mass
        std::vector<double> qual_routing_final;  ///< Final stored quality mass
        std::vector<double> qual_routing_reacted;///< Quality mass lost to decay

        void resize_quality(int n_pollutants) {
            auto np = static_cast<std::size_t>(n_pollutants);
            qual_init_buildup.assign(np, 0.0);
            qual_final_buildup.assign(np, 0.0);
            qual_surface_buildup.assign(np, 0.0);
            qual_wet_deposition.assign(np, 0.0);
            qual_sweeping.assign(np, 0.0);
            qual_bmp_removal.assign(np, 0.0);
            qual_infil_loss.assign(np, 0.0);
            qual_runoff_load.assign(np, 0.0);
            qual_routing_wet.assign(np, 0.0);
            qual_routing_outflow.assign(np, 0.0);
            qual_routing_flood.assign(np, 0.0);
            qual_routing_init.assign(np, 0.0);
            qual_routing_final.assign(np, 0.0);
            qual_routing_reacted.assign(np, 0.0);
        }

        void reset() {
            // Save quality vectors, reset scalars, restore vectors
            auto qi = std::move(qual_init_buildup);
            auto qf = std::move(qual_final_buildup);
            auto qsb = std::move(qual_surface_buildup);
            auto qwd = std::move(qual_wet_deposition);
            auto qsw = std::move(qual_sweeping);
            auto qbmp = std::move(qual_bmp_removal);
            auto qil = std::move(qual_infil_loss);
            auto qrl = std::move(qual_runoff_load);
            auto qrw = std::move(qual_routing_wet);
            auto qro = std::move(qual_routing_outflow);
            auto qrf = std::move(qual_routing_flood);
            auto qri = std::move(qual_routing_init);
            auto qrfi = std::move(qual_routing_final);
            auto qrr = std::move(qual_routing_reacted);
            *this = MassBalance{};
            qual_init_buildup = std::move(qi);
            qual_final_buildup = std::move(qf);
            qual_surface_buildup = std::move(qsb);
            qual_wet_deposition = std::move(qwd);
            qual_sweeping = std::move(qsw);
            qual_bmp_removal = std::move(qbmp);
            qual_infil_loss = std::move(qil);
            qual_runoff_load = std::move(qrl);
            qual_routing_wet = std::move(qrw);
            qual_routing_outflow = std::move(qro);
            qual_routing_flood = std::move(qrf);
            qual_routing_init = std::move(qri);
            qual_routing_final = std::move(qrfi);
            qual_routing_reacted = std::move(qrr);
            // Zero out the quality vectors (except init_buildup which is
            // computed once during initQuality and must survive reset)
            for (auto* v : {&qual_final_buildup,
                            &qual_surface_buildup, &qual_wet_deposition,
                            &qual_sweeping, &qual_bmp_removal,
                            &qual_infil_loss, &qual_runoff_load,
                            &qual_routing_wet, &qual_routing_outflow,
                            &qual_routing_flood, &qual_routing_init,
                            &qual_routing_final, &qual_routing_reacted}) {
                std::fill(v->begin(), v->end(), 0.0);
            }
        }

        /// Runoff continuity error (fraction).
        double runoff_error() const {
            double total_in = runoff_rainfall + runoff_init_store;
            double total_out = runoff_evap + runoff_infil + runoff_runoff + runoff_final_store;
            return (total_in > 0.0) ? (total_in - total_out) / total_in : 0.0;
        }

        /// Routing continuity error (fraction).
        double routing_error() const {
            double total_in = routing_dry_weather + routing_wet_weather +
                              routing_gw_inflow + routing_rdii + routing_external +
                              routing_init_storage;
            double total_out = routing_flooding + routing_outflow +
                               routing_evap_loss + routing_seep_loss +
                               routing_final_storage;
            return (total_in > 0.0) ? (total_in - total_out) / total_in : 0.0;
        }
    } mass_balance;

    // =========================================================================
    // Routing time-step statistics
    // =========================================================================

    struct RoutingStepStats {
        double min_step  = 1.0e30; ///< Minimum routing time step used (sec)
        double max_step  = 0.0;    ///< Maximum routing time step used (sec)
        double sum_step  = 0.0;    ///< Sum of all routing time steps (sec)
        long   n_steps   = 0;      ///< Total number of routing steps
        double steady_pct = 0.0;   ///< Percent of time in steady state

        /// Number of time step histogram bins (matching legacy TIMELEVELS=5).
        static constexpr int N_TIME_BINS = 5;
        long   step_counts[N_TIME_BINS + 1] = {};   ///< Histogram bin counts
        double step_intervals[N_TIME_BINS + 1] = {}; ///< Histogram bin edges

        /// Non-convergence and iteration tracking
        long   n_non_converged = 0;   ///< Steps that didn't converge
        double sum_iterations  = 0.0; ///< Sum of iterations for averaging

        void update(double dt) {
            if (dt < min_step) min_step = dt;
            if (dt > max_step) max_step = dt;
            sum_step += dt;
            ++n_steps;
        }

        /// Record iteration count for a routing step
        void update_iterations(int iters, bool converged) {
            sum_iterations += iters;
            if (!converged) ++n_non_converged;
        }

        /// Build histogram bin edges (call once after simulation, before report)
        void build_histogram() {
            if (n_steps == 0 || max_step <= 0.0) return;
            double hi = max_step;
            double lo = (min_step < 1.0e30) ? min_step : 0.0;
            if (lo <= 0.0) lo = hi;
            step_intervals[0] = hi;
            for (int i = 1; i <= N_TIME_BINS; ++i)
                step_intervals[i] = step_intervals[i-1] / std::pow(hi/lo, 1.0/N_TIME_BINS);
            step_intervals[N_TIME_BINS] = lo;
        }

        /// Add a step to the histogram (call during simulation or post-process)
        void record_step_bin(double dt) {
            for (int i = 0; i < N_TIME_BINS; ++i) {
                if (dt >= step_intervals[i+1]) {
                    step_counts[i]++;
                    return;
                }
            }
            step_counts[N_TIME_BINS - 1]++;
        }

        double avg_step() const {
            return (n_steps > 0) ? sum_step / static_cast<double>(n_steps) : 0.0;
        }

        double pct_non_converged() const {
            return (n_steps > 0) ? 100.0 * static_cast<double>(n_non_converged) / static_cast<double>(n_steps) : 0.0;
        }

        double computed_avg_iterations() const {
            return (n_steps > 0) ? sum_iterations / static_cast<double>(n_steps) : 0.0;
        }
    } routing_stats;

    // =========================================================================
    // Input file path (for model write / hot start)
    // =========================================================================

    /**
     * @brief Path to the input .inp file (empty if not opened from a file).
     * @see Legacy: InpFile in globals.h
     */
    std::string inp_file_path;

    // =========================================================================
    // Context-level operations
    // =========================================================================

    /**
     * @brief Fully reset the context to a CREATED state.
     *
     * @details Clears all object data and name indices. Does NOT free the
     *          option structs (they are re-parsed from the input file).
     *          Call this before re-running or re-opening a simulation.
     */
    void reset() {
        state = EngineState::CREATED;
        current_time = 0.0;
        current_date = 0.0;
        dt_output_remaining = 0.0;
        dt_controls_remaining = 0.0;
        error_code = 0;
        warning_code = 0;
        error_message.clear();
        title_notes.clear();

        // Clear SoA stores
        nodes      = NodeData{};
        links      = LinkData{};
        subcatches = SubcatchData{};
        gages      = GageData{};
        pollutants = PollutantData{};
        tables     = TableData{};

        // Clear name indices
        node_names.clear();
        link_names.clear();
        subcatch_names.clear();
        gage_names.clear();
        pollutant_names.clear();
        table_names.clear();

        // Clear spatial, flags, and forcing
        spatial    = SpatialFrame{};
        user_flags.clear();
        forcing    = ForcingData{};
    }

    /**
     * @brief Snapshot current state into old-step arrays before solving.
     *
     * @details Must be called at the start of each timestep, before the
     *          hydraulic and quality solvers update the state.
     */
    void save_state() noexcept {
        nodes.save_state();
        links.save_state();
        subcatches.save_state();
        if (options.quality_routing) {
            pollutants.save_quality_state();
        }
    }

    /**
     * @brief Reset all state variables to initial conditions (cold start).
     *
     * @details Sets all current and old-step arrays to zero. Invert elevations
     *          and static properties are NOT changed.
     */
    void reset_state() noexcept {
        nodes.reset_state();
        links.reset_state();
        subcatches.reset_state();
        gages.reset_state();
        pollutants.reset_quality_state();
        tables.reset_cursors();
        current_time = 0.0;
        current_date = options.start_date;
        dt_output_remaining = options.report_step;
        dt_controls_remaining = 0.0;
    }

    /**
     * @brief Allocate all object arrays after input parsing is complete.
     *
     * @details Called by InputReader after all [JUNCTIONS], [CONDUITS], etc.
     *          sections have been parsed and the final object counts are known.
     *          Resizes SoA arrays and pollutant quality matrices.
     *
     * @note Name indices must already be populated (resize() uses node_names.size(),
     *       etc. to determine array length).
     */
    void allocate_objects() {
        nodes.resize(node_names.size());
        links.resize(link_names.size());
        subcatches.resize(subcatch_names.size());
        gages.resize(gage_names.size());
        pollutants.resize_pollutants(pollutant_names.size());
        pollutants.resize_quality(
            node_names.size(),
            link_names.size(),
            subcatch_names.size()
        );

        // Resize spatial coordinate arrays
        spatial.node_x.assign(static_cast<std::size_t>(node_names.size()), 0.0);
        spatial.node_y.assign(static_cast<std::size_t>(node_names.size()), 0.0);
        spatial.link_x.assign(static_cast<std::size_t>(link_names.size()), 0.0);
        spatial.link_y.assign(static_cast<std::size_t>(link_names.size()), 0.0);
        spatial.subcatch_x.assign(static_cast<std::size_t>(subcatch_names.size()), 0.0);
        spatial.subcatch_y.assign(static_cast<std::size_t>(subcatch_names.size()), 0.0);

        // Resize link vertex and subcatchment polygon arrays
        spatial.link_vertices_x.resize(static_cast<std::size_t>(link_names.size()));
        spatial.link_vertices_y.resize(static_cast<std::size_t>(link_names.size()));
        spatial.subcatch_polygon_x.resize(static_cast<std::size_t>(subcatch_names.size()));
        spatial.subcatch_polygon_y.resize(static_cast<std::size_t>(subcatch_names.size()));

        // Resize gage coordinates
        spatial.gage_x.assign(static_cast<std::size_t>(gage_names.size()), 0.0);
        spatial.gage_y.assign(static_cast<std::size_t>(gage_names.size()), 0.0);
    }

    // =========================================================================
    // Convenience accessors
    // =========================================================================

    /** @brief Number of nodes. */
    int n_nodes()      const noexcept { return node_names.size(); }

    /** @brief Number of links. */
    int n_links()      const noexcept { return link_names.size(); }

    /** @brief Number of subcatchments. */
    int n_subcatches() const noexcept { return subcatch_names.size(); }

    /** @brief Number of rain gages. */
    int n_gages()      const noexcept { return gage_names.size(); }

    /** @brief Number of pollutants. */
    int n_pollutants() const noexcept { return pollutant_names.size(); }
    int n_landuses()   const noexcept { return landuse_names.size(); }

    /** @brief Number of tables (time series + curves). */
    int n_tables()     const noexcept { return table_names.size(); }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_SIMULATION_CONTEXT_HPP */
