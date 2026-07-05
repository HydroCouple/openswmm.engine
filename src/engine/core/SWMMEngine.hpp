/**
 * @file SWMMEngine.hpp
 * @brief Lifecycle manager for the new openswmm.engine.
 *
 * @details SWMMEngine is the C++ façade that owns all sub-systems and
 *          drives the simulation from open → initialize → run → end.
 *          The C API in openswmm_engine.h delegates every call into here.
 *
 * ### Lifecycle state machine
 *
 * ```
 * CREATED → OPENED → INITIALIZED → RUNNING ──┐
 *                                    ↑         │ step loop
 *                                    └─────────┘
 *                                    ↓
 *                                  ENDED → REPORTED → CLOSED
 * Any state → ERROR_STATE (on fatal error)
 * ```
 *
 * ### Callback registration
 *
 * All callbacks are optional. If not registered, the corresponding events
 * are silently dropped. Callbacks are called on the main simulation thread.
 *
 * @see SimulationContext.hpp — owns all simulation data
 * @see InputReader.hpp       — parses .inp input file
 * @see TimestepController.hpp — explicit dt_next computation
 * @see include/openswmm/engine/openswmm_engine.h — C API surface
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_SWMM_ENGINE_HPP
#define OPENSWMM_ENGINE_SWMM_ENGINE_HPP

#include "SimulationContext.hpp"
#include "../plugins/PluginFactory.hpp"
#include "../output/IOThread.hpp"
#include "../hydraulics/Routing.hpp"
#include "../hydrology/Runoff.hpp"
#include "../hydrology/Gage.hpp"
#include "../hydrology/Climate.hpp"
#include "../hydrology/ClimateFile.hpp"
#include "../hydrology/Snow.hpp"
#include "../hydrology/Groundwater.hpp"
#include "../hydrology/LID.hpp"
#include "../hydrology/Inflow.hpp"
#include "../hydrology/RDII.hpp"
#include "../hydrology/RunoffInterface.hpp"
#include "../hydrology/RdiiInterface.hpp"
#include "../quality/QualityRouting.hpp"
#include "../quality/Landuse.hpp"
#include "../controls/Controls.hpp"
#include "../hydraulics/Exfiltration.hpp"
#include "../hydraulics/Inlet.hpp"
#include "../hydraulics/Culvert.hpp"
#include "../hydraulics/HydStructures.hpp"
#include "InterfaceFile.hpp"

#ifdef OPENSWMM_HAS_2D
#include "../2d/SurfaceRouter2D.hpp"
namespace openswmm::twoD { class Default2DOutputPlugin; }
#endif

#include <functional>
#include <memory>
#include <string>

// Forward-declare the C callback types without pulling in the C header here
// (avoids polluting C++ translation units with extern "C" names)
extern "C" {
    typedef void (*SWMM_ProgressCallback)(void*, double, double, void*);
    typedef void (*SWMM_WarningCallback) (void*, int, const char*, void*);
    typedef void (*SWMM_StepBeginCallback)(void*, double, double, void*);
    typedef void (*SWMM_StepEndCallback)  (void*, double, double, void*);
}

namespace openswmm {

/**
 * @brief Registered callback bundle (all optional).
 * @ingroup engine_core
 */
struct EngineCallbacks {
    SWMM_ProgressCallback  on_progress   = nullptr; void* progress_ud  = nullptr;
    SWMM_WarningCallback   on_warning    = nullptr; void* warning_ud   = nullptr;
    SWMM_StepBeginCallback on_step_begin = nullptr; void* step_begin_ud= nullptr;
    SWMM_StepEndCallback   on_step_end   = nullptr; void* step_end_ud  = nullptr;
};

/**
 * @brief Primary lifecycle manager for one simulation run.
 *
 * @details One SWMMEngine per concurrent simulation. The C API creates a
 *          SWMMEngine via `new`, wraps it in a `void*` handle, and deletes
 *          it via `swmm_engine_destroy()`.
 *
 * @ingroup engine_core
 */
class SWMMEngine {
public:
    SWMMEngine();
    ~SWMMEngine();

    // Non-copyable, non-movable (owns resources)
    SWMMEngine(const SWMMEngine&)            = delete;
    SWMMEngine& operator=(const SWMMEngine&) = delete;
    SWMMEngine(SWMMEngine&&)                 = delete;
    SWMMEngine& operator=(SWMMEngine&&)      = delete;

    // =========================================================================
    // Lifecycle operations (map 1:1 to C API)
    // =========================================================================

    /**
     * @brief Open an input file and parse the simulation model.
     *
     * @details Reads the .inp file, allocates all object arrays, and applies
     *          initial conditions. Transitions to OPENED state on success.
     *
     * @param inp_path          Path to the input file.
     * @param rpt_path          Path for the report output file (NULL = no report).
     * @param out_path          Path for the binary output file (NULL = no binary out).
     * @param input_plugin_lib  Path to shared library providing an IInputPlugin,
     *                          or NULL to use the built-in .inp reader.
     * @returns SWMM_OK or an error code.
     */
    int open(const char* inp_path,
             const char* rpt_path,
             const char* out_path,
             const char* input_plugin_lib = nullptr) noexcept;

    /**
     * @brief Apply initial conditions and prepare the engine for stepping.
     *
     * @details Initializes state variables to initial depths and flows from
     *          input, starts the IO thread, and initializes plugins.
     * @returns SWMM_OK or an error code.
     */
    int initialize() noexcept;

    /**
     * @brief Signal the start of the simulation step loop.
     *
     * @param save_results  If non-zero, results are written to the output file.
     * @returns SWMM_OK or an error code.
     */
    int start(int save_results) noexcept;

    /**
     * @brief Advance the simulation by one explicit timestep.
     *
     * @details Computes dt_next via TimestepController, calls save_state(),
     *          steps hydrology / hydraulics / quality, advances timers, and
     *          posts a snapshot to the IO thread if output is due.
     *
     * @param[out] elapsed_time  Current elapsed time in decimal days after the step.
     *                           Set to 0.0 when the simulation is complete.
     * @returns SWMM_OK or an error code.
     */
    int step(double* elapsed_time) noexcept;

    /**
     * @brief End the simulation loop and flush output.
     *
     * @details Finalizes plugins and joins the IO thread.
     * @returns SWMM_OK or an error code.
     */
    int end() noexcept;

    /**
     * @brief Write the summary report file.
     * @returns SWMM_OK or an error code.
     */
    int report() noexcept;

    /**
     * @brief Release all resources (does NOT free this object).
     *
     * @details After close(), the engine transitions to CLOSED. Call
     *          `delete engine` (via swmm_engine_destroy()) to free memory.
     */
    int close() noexcept;

    // =========================================================================
    // Callback registration
    // =========================================================================

    void set_progress_callback(SWMM_ProgressCallback cb, void* user_data) noexcept;
    void set_warning_callback (SWMM_WarningCallback  cb, void* user_data) noexcept;
    void set_step_begin_callback(SWMM_StepBeginCallback cb, void* user_data) noexcept;
    void set_step_end_callback  (SWMM_StepEndCallback   cb, void* user_data) noexcept;

    // =========================================================================
    // Context access (for C API wrappers)
    // =========================================================================

    SimulationContext&       context()       noexcept { return ctx_; }
    const SimulationContext& context() const noexcept { return ctx_; }

    /// Groundwater solver access (for C API state injection).
    groundwater::GWSolver&       gwSolver()       noexcept { return groundwater_; }
    const groundwater::GWSolver& gwSolver() const noexcept { return groundwater_; }

    /// Snow solver access (for C API state injection).
    snow::SnowSolver&       snowSolver()       noexcept { return snow_; }
    const snow::SnowSolver& snowSolver() const noexcept { return snow_; }

    /// Inflow solver access (for C API runtime pattern-cache refresh).
    inflow::InflowSolver&       inflowSolver()       noexcept { return inflow_; }
    const inflow::InflowSolver& inflowSolver() const noexcept { return inflow_; }

    /// Re-derive the land-use buildup/washoff parameter cache from the live
    /// context (for C API runtime edits via swmm_buildup_set/_washoff_set).
    /// Leaves the accumulated buildup pool untouched.
    void refreshLanduseParams() noexcept;

    /// Recompile one (node, pollutant) treatment expression from the live
    /// context and refresh the per-node has-treatment flag (for C API runtime
    /// edits via swmm_treatment_set/_clear — the step loop evaluates the
    /// compiled cache, not the expression string). Returns 0 on success or a
    /// nonzero parse-error code; a failed parse leaves the cell cleared.
    int refreshTreatment(int node_idx, int pollut_idx) noexcept;

    /// Re-copy the drain-layer coefficients from the live context into the
    /// LID solver's per-unit parameter columns (for C API runtime edits via
    /// swmm_lid_set_drain). Touches no per-unit state, so it is safe mid-run;
    /// a no-op before the solver is initialized.
    void refreshLIDDrainParams() noexcept;

    /// Re-derive the groundwater solver's per-subcatchment flux-coefficient
    /// columns (conductivity, slopes, evap/loss coefficients) from the live
    /// context aquifers (for C API runtime edits via swmm_aquifer_set_param).
    /// Leaves the structural columns (porosity/field capacity/wilting point/
    /// total depth) and the GW state (theta, lower_depth) untouched.
    void refreshAquiferParams() noexcept;

    /**
     * @brief Area-weighted snow depth (SWE, ft) on a subcatchment.
     *
     * Combines the snow pack's plowable/impervious/pervious subarea SWE
     * weighted by subarea fractions (matching the report snapshot
     * computation). Returns 0 for subcatchments without a snow pack.
     *
     * @param idx Subcatchment index (caller-validated).
     * @return Snow water equivalent depth (ft).
     */
    double subcatchSnowDepth(int idx) const noexcept;

    /**
     * @brief Open the runoff interface file in SAVE mode (Phase 1b).
     *
     * @details Allocates a fresh @ref runoff_iface::RunoffInterfaceFile,
     *          opens @p path for binary writing, and stamps the header
     *          with the current subcatchment count, pollutant count,
     *          and flow units.  Subsequent runoff substeps emit one
     *          record each via the auto-save hook in
     *          @ref SWMMEngine::stepRunoff.
     *
     * @returns 0 on success, non-zero on failure (file could not be
     *          opened, or a runoff interface file was already open and
     *          must be closed first).
     */
    int openRunoffIfaceWrite(const std::string& path) noexcept;

    /**
     * @brief Open the runoff interface file in USE mode.
     *
     * @details Opens @p path for binary reading and verifies that the
     *          header matches the current model's subcatchment count,
     *          pollutant count, and flow units. The engine does not
     *          auto-read records yet — callers invoke
     *          @ref swmm_runoff_iface_read_step manually between
     *          @c step calls. USE-mode auto-skip is a follow-up.
     */
    int openRunoffIfaceRead(const std::string& path) noexcept;

    /// Write one record to the open runoff interface file (no-op when
    /// the file is not open in SAVE mode).  Called automatically once
    /// per runoff substep; also callable explicitly via the C API.
    void saveRunoffIfaceStep(double dt) noexcept;

    /// Read one record from the open runoff interface file into the
    /// subcatchment runoff/quality vectors.  Returns @c true on
    /// success, @c false on EOF or when no file is open in READ mode.
    bool readRunoffIfaceStep() noexcept;

    /// Close and reset the runoff interface file (safe to call multiple
    /// times; safe to call when no file was opened).
    void closeRunoffIface() noexcept;

    /// Accessor used by tests / diagnostics — returns the current mode
    /// of the runoff interface file, or @c FileMode::NONE if no file is
    /// open.
    FileMode runoffIfaceMode() const noexcept;

    /** @brief Access the runoff solver (for hot start infil state save/restore). */
    runoff::RunoffSolver&       runoff_solver()       noexcept { return runoff_; }
    const runoff::RunoffSolver& runoff_solver() const noexcept { return runoff_; }

    /** @brief Access the GW solver (for hot start GW state save/restore). */
    groundwater::GWSolver&       gw_solver()       noexcept { return groundwater_; }
    const groundwater::GWSolver& gw_solver() const noexcept { return groundwater_; }

    /** @brief Access the plugin factory (for C-API dispatch through plugins). */
    PluginFactory&       plugin_factory()       noexcept { return plugins_; }
    const PluginFactory& plugin_factory() const noexcept { return plugins_; }

    /** @brief Last error code (0 = no error). */
    int last_error() const noexcept { return ctx_.error_code; }

    /** @brief Last error message (empty if no error). */
    const char* last_error_message() const noexcept {
        return ctx_.error_message.c_str();
    }

#ifdef OPENSWMM_HAS_2D
    /** @brief Access the 2D surface router (for C API delegation). */
    twoD::SurfaceRouter2D&       surfaceRouter2D()       noexcept { return surface_router_; }
    const twoD::SurfaceRouter2D& surfaceRouter2D() const noexcept { return surface_router_; }
#endif


private:
    // -----------------------------------------------------------------------
    // Sub-systems
    // -----------------------------------------------------------------------

    SimulationContext      ctx_;        ///< All simulation data (SoA + options)
    PluginFactory          plugins_;   ///< Phase 4: plugin loader + lifecycle
    IOThread               io_thread_; ///< Phase 5: writer thread

    // Computational modules (batch-oriented, SoA)
    Router                       router_;       ///< Hydraulic routing (owns XSectGroups)
    runoff::RunoffSolver         runoff_;       ///< Subcatchment runoff (batch nonlinear reservoir)
    climate::ClimateFileReader   climate_file_; ///< Climate file reader (temp/evap/wind from file)
    snow::SnowSolver             snow_;         ///< Snowmelt (batch over subcatch×subareas)
    groundwater::GWSolver        groundwater_;  ///< Groundwater (batch ODE per subcatchment)
    lid::LIDSolver               lid_;          ///< LID (batch by type group)
    quality::QualitySolver       quality_;      ///< Quality routing (batch link-load + mixing)
    landuse::LanduseSolver       landuse_solver_; ///< Buildup/washoff computation
    landuse::SurfaceQualitySoA   surface_quality_; ///< Per-subcatch surface quality state
    controls::ControlEngine      controls_;     ///< Control rule evaluation
    inflow::InflowSolver         inflow_;       ///< External + DWF inflows
    rdii::RDIISolver             rdii_;         ///< RDII (unit hydrograph convolution)
    exfil::ExfilSolver           exfil_;        ///< Storage node exfiltration
    inlet::InletSolver           inlet_;        ///< Street inlet capture
    std::vector<int>             culvert_links_;///< Pre-built culvert link indices (avoid per-timestep alloc)
    std::vector<double>          gw_frac_perv_; ///< Per-subcatch pervious fraction for GW evap
    std::vector<double>          gw_perv_evap_; ///< Per-subcatch pervious evap rate (ft/sec)
    std::vector<double>          snow_rain_;    ///< Per-subcatch rainfall into snow step (ft/sec)
    std::vector<double>          snow_snow_;    ///< Per-subcatch snowfall into snow step (ft/sec)
    hydstruct::StructureSolver  hydstruct_;    ///< Pumps, orifices, weirs, outlets
    iface::InterfaceManager      iface_;        ///< Routing interface file I/O

    // Phase 1b: optional runoff interface file (legacy "Frunoff").
    // SAVE mode: the engine auto-emits one record per runoff substep from
    // inside stepRunoff(). USE mode: stepRunoff() replaces each runoff
    // substep with the next file record (legacy runoff_readFromFile).
    // Auto-opened from [FILES] SAVE/USE RUNOFF in start(); also reachable
    // via the swmm_runoff_iface_* C API.
    std::unique_ptr<runoff_iface::RunoffInterfaceFile> runoff_iface_file_;

    // RDII interface file (legacy "Frdii"). USE mode bypasses the internal
    // unit-hydrograph computation (legacy rdii_openRdii skips
    // createRdiiFile); SAVE mode exports the computed RDII flows. Opened
    // from [FILES] USE/SAVE RDII in start().
    rdii_iface::RdiiInterfaceFile rdii_iface_file_;

    // Event and steady-state tracking
    int next_event_ = 0;                        ///< Index of next event in ctx_.events
    bool isBetweenEvents(double current_date) const; ///< Check if between routing events
    bool isInSteadyState(int action_count) const;    ///< Check if system is in steady state
    std::vector<gage::GageState> gage_states_;  ///< Per-gage state (SoA)

#ifdef OPENSWMM_HAS_2D
    twoD::SurfaceRouter2D        surface_router_; ///< Optional 2D surface routing solver
    /// Non-owning pointer to the 2D HDF5 output plugin (lifetime owned by
    /// PluginFactory's output_plugins_). Set in open() when [2D_OPTIONS]
    /// OUTPUT_FILE is configured; used in start() to call prepareMeshAndDatasets
    /// once the mesh is built.
    twoD::Default2DOutputPlugin* surface_output_plugin_ = nullptr;

    /// Point ctx_.twod_io at surface_router_'s mesh/options/boundary and
    /// pending parse rows so serialization consumers (InpWriter, GeoPackage
    /// reader/writer) can access the 2D model through the context alone.
    /// Called once from the constructor; SWMMEngine is non-copyable and
    /// non-movable, so the pointers stay valid for the engine's lifetime,
    /// and SimulationContext::reset() intentionally preserves them.
    void wire2DModelIO() noexcept;
#endif

    std::string rpt_path_;  ///< Report file path
    std::string out_path_;  ///< Binary output file path

    // Runoff clock (matching legacy OldRunoffTime / NewRunoffTime)
    // Runoff advances on its own timestep (300 sec wet, 3600 sec dry);
    // lateral flows are linearly interpolated between runoff boundaries.
    double old_runoff_time_ = 0.0;  ///< Previous runoff boundary (seconds from start)
    double new_runoff_time_ = 0.0;  ///< Next runoff boundary (seconds from start)
    // PARITY: millisecond mirrors of the runoff clock, accumulated with the
    // exact legacy ops (runoff.c:229-237 — NewRunoffTime += 1000*step, clamp
    // to TotalDuration ms). The wet-weather/GW interpolation weight f
    // (routing.c:703) and the report-instant runoff weight (output.c) must be
    // formed from MILLISECOND quantities to round identically to legacy.
    double old_runoff_ms_ = 0.0;    ///< legacy OldRunoffTime (msec)
    double new_runoff_ms_ = 0.0;    ///< legacy NewRunoffTime (msec)

    // PARITY: per-subcatchment interpolated wet-weather / GW inflows saved by
    // the Phase-2 interpolation so assembleLateralInflows() can replay
    // legacy's exact per-node accumulation ORDER — routing.c:466-473 adds
    // ext, dwf, then PER-SUBCATCHMENT wet-weather q (routing.c:716), then GW,
    // RDII, iface into Node.newLatFlow. Pre-summing per node and adding in a
    // different source order rounds differently (1-ULP lat-flow drift).
    std::vector<double> wet_q_interp_;  ///< per-subcatch interpolated runoff+runon (cfs)
    std::vector<double> gw_q_interp_;   ///< per-subcatch interpolated GW flow (cfs)
    std::vector<int>    gw_q_node_;     ///< receiving node for gw_q_interp_ (-1 = skip)

    // Persistent runoff-state flags read by computeRunoffTimestep() on the NEXT
    // runoff step (one-step lag), matching legacy globals HasRunoff/HasSnow in
    // runoff.c. They must persist across substeps so the engine keeps wet_step
    // through the hydrograph recession (rain stopped, ponded water still
    // draining); otherwise dry_step coarsens the falling limb and the
    // end-of-step-rate×dt bookkeeping under-counts runoff volume.
    bool has_runoff_ = false;  ///< Prev step generated runoff (legacy HasRunoff)
    bool has_snow_   = false;  ///< Prev step had snow cover   (legacy HasSnow)

    EngineCallbacks callbacks_;   ///< Registered callback bundle
    int save_results_ = 0;        ///< Whether to save binary results

    // -----------------------------------------------------------------------
    // Report averaging accumulator (legacy RptFlags.averages)
    // -----------------------------------------------------------------------
    // When rpt_averages is true, node and link results are accumulated over
    // each routing step and averaged at report boundaries.  Subcatchment
    // results are always point-in-time (matching legacy).
    struct AvgAccumulator {
        // Node accumulators (6 variables per node)
        std::vector<double> node_depth;
        std::vector<double> node_head;
        std::vector<double> node_volume;
        std::vector<double> node_lat_inflow;
        std::vector<double> node_total_inflow;
        std::vector<double> node_overflow;

        // Link accumulators (5 variables per link)
        std::vector<double> link_flow;
        std::vector<double> link_depth;
        std::vector<double> link_velocity;
        std::vector<double> link_volume;
        std::vector<double> link_capacity;

        int n_steps = 0;  ///< Number of routing steps accumulated

        void resize(int n_nodes, int n_links) {
            auto un = static_cast<std::size_t>(n_nodes);
            auto ul = static_cast<std::size_t>(n_links);
            node_depth.assign(un, 0.0);
            node_head.assign(un, 0.0);
            node_volume.assign(un, 0.0);
            node_lat_inflow.assign(un, 0.0);
            node_total_inflow.assign(un, 0.0);
            node_overflow.assign(un, 0.0);
            link_flow.assign(ul, 0.0);
            link_depth.assign(ul, 0.0);
            link_velocity.assign(ul, 0.0);
            link_volume.assign(ul, 0.0);
            link_capacity.assign(ul, 0.0);
            n_steps = 0;
        }

        void reset() {
            std::fill(node_depth.begin(), node_depth.end(), 0.0);
            std::fill(node_head.begin(), node_head.end(), 0.0);
            std::fill(node_volume.begin(), node_volume.end(), 0.0);
            std::fill(node_lat_inflow.begin(), node_lat_inflow.end(), 0.0);
            std::fill(node_total_inflow.begin(), node_total_inflow.end(), 0.0);
            std::fill(node_overflow.begin(), node_overflow.end(), 0.0);
            std::fill(link_flow.begin(), link_flow.end(), 0.0);
            std::fill(link_depth.begin(), link_depth.end(), 0.0);
            std::fill(link_velocity.begin(), link_velocity.end(), 0.0);
            std::fill(link_volume.begin(), link_volume.end(), 0.0);
            std::fill(link_capacity.begin(), link_capacity.end(), 0.0);
            n_steps = 0;
        }
    };

    AvgAccumulator avg_;  ///< Averaging accumulator (only used when rpt_averages == true)

    // -----------------------------------------------------------------------
    // Reporting-path XSectParams cache
    // -----------------------------------------------------------------------
    // updateStatistics / postOutputSnapshot / accumulateAvgResults each
    // rebuilt the full XSectParams gather per conduit per routing step just
    // to call link::getVelocity. The params are static during a run except
    // through the C-API editing path, which bumps ctx.xsect_generation via
    // recompute_conduit_flow_properties — ensureXspCache() compares the
    // generation and rebuilds only then. Values are verbatim copies of the
    // same SoA fields, so consumers receive bit-identical inputs.
    std::vector<XSectParams> xsp_cache_;
    std::uint64_t xsp_cache_gen_ = ~0ULL;   ///< generation the cache was built at

    /** @brief Rebuild xsp_cache_ if links/xsect state changed (cheap check). */
    void ensureXspCache() noexcept;

    /// Legacy-convention full volume per node for .out NODE_VOLUME reporting:
    /// 0 for plain junctions/outfalls/dividers (legacy node_getVolume returns 0
    /// when fullVolume==0), pump wet-well xMax for Type-1 pump inlets. STORAGE
    /// reports its curve volume directly. This DECOUPLES the reported node volume
    /// from the internal volume-state (which keeps MIN_SURFAREA*depth for the
    /// volume-based solver + surcharge detection). See postOutputSnapshot().
    std::vector<double> report_full_volume_;

    /// Legacy-convention reported node volume (mirrors legacy node_getVolume):
    /// STORAGE → curve volume (ctx_.nodes.volume); junction/outfall/divider →
    /// report_full_volume_·(depth/fullDepth) = 0 for plain junctions. Used for
    /// the .out NODE_VOLUME and the routing mass-balance storage sums so both
    /// match legacy, while the internal volume-state (MIN_SURFAREA) is preserved.
    double reportedNodeVolume(int i) const noexcept;

    /// Same legacy-convention mapping evaluated at an arbitrary (depth, volume)
    /// state — used to map the PREVIOUS step's state for output interpolation
    /// (legacy node_getResults interpolates old/new volumes, node.c:487).
    double reportedNodeVolume(int i, double depth, double volume) const noexcept;

    // -----------------------------------------------------------------------
    // Initialization sub-functions (called by init_modules)
    // -----------------------------------------------------------------------

    /** @brief Initialize all computational modules after model is loaded. */
    void init_modules() noexcept;

    /** @brief Initialize hydrology solvers: runoff, snow, groundwater, LID. */
    void initHydrology() noexcept;

    /** @brief Initialize hydraulic routing: router, exfiltration, inlets, culverts. */
    void initHydraulics() noexcept;

    /** @brief Initialize water quality: landuse solver, surface quality, mass balance. */
    void initQuality() noexcept;

    /** @brief Initialize node/link geometry: crown elevations, full volumes. */
    void initGeometry() noexcept;

    /** @brief Initialize mass balance: record initial storage volumes. */
    void initMassBalance() noexcept;

    /** @brief Reset per-step mass balance accumulators (matching legacy massbal_initTimeStepTotals). */
    void resetStepMassBalance() noexcept;

    // -----------------------------------------------------------------------
    // Step sub-functions (called by step)
    // -----------------------------------------------------------------------

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
    void stepRunoff(double dt_routing) noexcept;

    /**
     * @brief Compute variable runoff timestep matching legacy runoff_getTimeStep().
     *
     * @details Selects wet_step or dry_step based on current conditions, then
     *          shortens to align with next rain gage boundary.
     *
     * @param abs_time     Current absolute OADate (days since 12/30/1899).
     * @param is_raining   True if any gage has rainfall > 0.
     * @param has_runoff   True if any subcatchment produces runoff > 0.
     * @param has_snow     True if any subcatchment has snow depth > 0.
     * @returns Runoff timestep in seconds.
     */
    double computeRunoffTimestep(double abs_time, bool is_raining,
                                 bool has_runoff, bool has_snow) noexcept;

    /**
     * @brief Accumulate runoff mass balance totals for one substep.
     *
     * @param dt_runoff  Runoff substep duration (seconds).
     */
    void accumulateRunoffMassBalance(double dt_runoff) noexcept;

    /**
     * @brief Compute surface quality buildup and washoff for one substep.
     *
     * @param dt_runoff  Runoff substep duration (seconds).
     */
    void stepSurfaceQuality(double dt_runoff) noexcept;

    /**
     * @brief Execute groundwater computation for one substep.
     *
     * @param dt_runoff  Runoff substep duration (seconds).
     */
    void stepGroundwater(double dt_runoff) noexcept;

    /**
     * @brief Execute Phase B: hydraulic and quality routing.
     *
     * @details Evaluates controls, computes inflows (external, DWF, RDII),
     *          runs hydraulic routing, inlet capture, culvert control,
     *          exfiltration, and quality transport.
     *
     * @param dt_routing  Routing timestep (seconds).
     */
    /** @brief Assemble subcatch-to-subcatch and outfall runon into subcatches.runon_inflow[].
     *  @param dt_runoff  Runoff timestep (sec) — used to convert outfall_runon_vol to CFS. */
    void assembleRunon(double dt_runoff) noexcept;

    /** @brief Pre-compute GW surface water head and available node flow from routing state. */
    void assembleGWCoupling(double dt_runoff) noexcept;

    /** @brief Assemble all decomposed inflow sources into nodes.lat_flow and compute step mass balance.
     *  @param dt_routing  Current routing step (s); used to re-derive the 1D↔2D
     *                     coupling rate from the carried per-window exchange volume. */
    void assembleLateralInflows(double dt_routing) noexcept;

    void stepRouting(double dt_routing) noexcept;

    /**
     * @brief Update node and link statistics after routing.
     *
     * @param dt_routing  Routing timestep (seconds).
     */
    void updateStatistics(double dt_routing) noexcept;

    /**
     * @brief Update routing mass balance totals after routing.
     *
     * @param dt_routing  Routing timestep (seconds).
     */
    void updateRoutingMassBalance(double dt_routing) noexcept;

    /**
     * @brief Compute final storage volumes for runoff and routing mass balance.
     */
    void computeFinalStorage() noexcept;

    /**
     * @brief Compute final quality buildup mass for quality mass balance.
     */
    void computeFinalQualityMassBalance() noexcept;

    /**
     * @brief Post a snapshot to the IO thread if output is due.
     */
    void postOutputSnapshot(double dt_step) noexcept;

    /**
     * @brief Deep-copy the active 2D surface state into a snapshot.
     * @details No-op when the 2D module is inactive. Shared by the
     *          full-results path and the 2D-only path in postOutputSnapshot,
     *          so 2D HDF5 output can be driven independently of save_results_.
     * @param snap  The snapshot to fill with surface_* fields.
     */
    void fillSurfaceSnapshot(SimulationSnapshot& snap) const noexcept;

    /**
     * @brief Accumulate current node/link results into the averaging accumulators.
     * @details Called every routing step when rpt_averages is true.
     */
    void accumulateAvgResults() noexcept;

    /**
     * @brief Apply time-averaged node/link values to a snapshot.
     * @details Divides accumulated sums by n_steps and writes into snap.
     *          Non-conduit capacity (pump/regulator) uses the last value, not averaged.
     * @param snap  The snapshot to fill with averaged values.
     */
    void applyAvgResults(SimulationSnapshot& snap) noexcept;

    // -----------------------------------------------------------------------
    // General helpers
    // -----------------------------------------------------------------------

    /**
     * @brief Apply user-injected runtime forcings to SoA arrays.
     *
     * @details Called at the start of each routing step (after save_state,
     *          before computations). Writes forcing values into lat_flow,
     *          outfall_param, rainfall, setting, etc. per the forcing mode
     *          (OVERRIDE or ADD). Also accumulates forcing volumes into the
     *          mass balance diagnostic accumulator.
     *
     * @param dt  Routing timestep (seconds) — for mass balance accumulation.
     */
    void applyForcings(double dt) noexcept;

    /** @brief Set a fatal error on the context and transition to ERROR_STATE. */
    void set_error(int code, const char* message) noexcept;

    /** @brief Fire the warning callback (if registered). */
    void emit_warning(int code, const char* message) noexcept;

    /** @brief Fire the progress callback (if registered). */
    void emit_progress() noexcept;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_SWMM_ENGINE_HPP */
