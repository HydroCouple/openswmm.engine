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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_SWMM_ENGINE_HPP
#define OPENSWMM_ENGINE_SWMM_ENGINE_HPP

#include "SimulationContext.hpp"
#include "../input/SectionRegistry.hpp"
#include "../plugins/PluginFactory.hpp"
#include "../output/IOThread.hpp"
#include "../hydraulics/Routing.hpp"
#include "../hydrology/Runoff.hpp"
#include "../hydrology/Gage.hpp"
#include "../hydrology/Climate.hpp"
#include "../hydrology/Snow.hpp"
#include "../hydrology/Groundwater.hpp"
#include "../hydrology/LID.hpp"
#include "../hydrology/Inflow.hpp"
#include "../hydrology/RDII.hpp"
#include "../quality/QualityRouting.hpp"
#include "../quality/Landuse.hpp"
#include "../controls/Controls.hpp"
#include "../hydraulics/Exfiltration.hpp"
#include "../hydraulics/Inlet.hpp"
#include "../hydraulics/Culvert.hpp"
#include "../hydraulics/HydStructures.hpp"
#include "InterfaceFile.hpp"

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
     * @param inp_path  Path to the .inp input file.
     * @param rpt_path  Path for the report output file (empty = no report).
     * @param out_path  Path for the binary output file (empty = no binary out).
     * @returns SWMM_OK or an error code.
     */
    int open(const char* inp_path,
             const char* rpt_path,
             const char* out_path) noexcept;

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

    /** @brief Last error code (0 = no error). */
    int last_error() const noexcept { return ctx_.error_code; }

    /** @brief Last error message (empty if no error). */
    const char* last_error_message() const noexcept {
        return ctx_.error_message.c_str();
    }

    // =========================================================================
    // SectionRegistry access (for custom section registration via C API)
    // =========================================================================

    input::SectionRegistry& registry() noexcept { return registry_; }

private:
    // -----------------------------------------------------------------------
    // Sub-systems
    // -----------------------------------------------------------------------

    SimulationContext      ctx_;        ///< All simulation data (SoA + options)
    input::SectionRegistry registry_;  ///< Input section dispatch table
    PluginFactory          plugins_;   ///< Phase 4: plugin loader + lifecycle
    IOThread               io_thread_; ///< Phase 5: writer thread

    // Computational modules (batch-oriented, SoA)
    Router                       router_;       ///< Hydraulic routing (owns XSectGroups)
    runoff::RunoffSolver         runoff_;       ///< Subcatchment runoff (batch nonlinear reservoir)
    climate::ClimateState        climate_;      ///< Daily climate state (broadcast to subcatchments)
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
    hydstruct::StructureSolver  hydstruct_;    ///< Pumps, orifices, weirs, outlets
    iface::InterfaceManager      iface_;        ///< Routing interface file I/O
    std::vector<gage::GageState> gage_states_;  ///< Per-gage state (SoA)

    std::string rpt_path_;  ///< Report file path
    std::string out_path_;  ///< Binary output file path

    EngineCallbacks callbacks_;   ///< Registered callback bundle
    int save_results_ = 0;        ///< Whether to save binary results

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
     * @param abs_time     Current absolute Julian date.
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
    void postOutputSnapshot() noexcept;

    // -----------------------------------------------------------------------
    // General helpers
    // -----------------------------------------------------------------------

    /** @brief Register all built-in SWMM section handlers. */
    void register_builtin_handlers();

    /** @brief Set a fatal error on the context and transition to ERROR_STATE. */
    void set_error(int code, const char* message) noexcept;

    /** @brief Fire the warning callback (if registered). */
    void emit_warning(int code, const char* message) noexcept;

    /** @brief Fire the progress callback (if registered). */
    void emit_progress() noexcept;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_SWMM_ENGINE_HPP */
