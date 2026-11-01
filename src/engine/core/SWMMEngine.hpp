// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
 * @license  Apache-2.0
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
#include "../quality/lard/LagrangianSolver.hpp"
#include "../transport/components/EulerianArdComponent/ArdEngine.hpp"
#include "../quality/Landuse.hpp"
#include "../controls/Controls.hpp"
#include "../hydraulics/Exfiltration.hpp"
#include "../hydraulics/Inlet.hpp"
#include "../hydraulics/Culvert.hpp"
#include "../hydraulics/HydStructures.hpp"
#include "InterfaceFile.hpp"

#ifdef OPENSWMM_HAS_2D
#include "../2d/SurfaceRouter2D.hpp"
#include "../uncertainty/UncertaintyConfig.hpp"
#include "../uncertainty/GraphEigenBasis.hpp"
#include "../uncertainty/NetworkLaplacian1D.hpp"
#include "../uncertainty/SpectralROM1D.hpp"
#include "../uncertainty/RomThreshold.hpp"
#include "../uncertainty/RomSurchargeAttenuation.hpp"
#include "../uncertainty/RomPhaseCoordinate.hpp"
#include "../uncertainty/RomDirectionalOperator.hpp"
#include "../uncertainty/RomFlowResponse.hpp"
#include "../uncertainty/RomAdditiveChannel.hpp"
#include "../uncertainty/UncertaintyEnsemble.hpp"
#include "../uncertainty/SoftSpatialField.hpp"
#include "../uncertainty/SpdeSpatialBasis.hpp"
#include "../uncertainty/RunoffElasticity.hpp"
#include "../uncertainty/WQUncertaintyBounds.hpp"
#ifdef OPENSWMM_HAS_2D
#include "../uncertainty/GridFileReader.hpp"
#endif
#include "../2d/coupling/RuntimeWaterDriverCheckpoint.hpp"
#include "../hydrology/SourceWaterDeliveryBridge.hpp"
namespace openswmm::twoD { class Default2DOutputPlugin; class EngineSourceResultHistory; }
#endif

#include <array>
#include <fstream>
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

class DefaultOutputPlugin;

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
    int advanceTo(double seconds, double* actual) noexcept;
    /// Internal coupling clock: accepted seconds for the explicit private profile.
    double runtimeCouplingTime() const noexcept;
    bool runtimeUpdateAllowed() const noexcept { return !runtime_pipeline_; }

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

    // Internal, same-platform water-only hydraulic session restart. Enable at
    // start()'s initial boundary; restore into an identically initialized fresh
    // session. No public C API; external water drivers require their participant.
    // Private binary-output qualification: initialize(), prepare this mode,
    // start(1), then enable checkpointing. Requires a new output destination.
    std::string prepareOutputCheckpointing();
    std::string enableCheckpointing();
#ifdef OPENSWMM_HAS_2D
    // Separate single-file mesh output qualification; initialize(), prepare,
    // start(0), then attach the common source checkpoint stage.
    std::string prepareSurfaceOutputCheckpointing();
    /// Internal water-only joint binary/mesh continuation with recoverable publication.
    std::string prepareCombinedOutputCheckpointing();
    // Qualification attachment only: mesh-owned common sources, with ordinary
    // engine runoff outside the mesh. Stage lifetime must cover the session.
    std::string enableCheckpointing(twoD::CommonSourceStage& stage);
    /// Explicit private spatial-source ownership; no authored/public activation.
    std::string enableSpatialSourceCheckpointing(twoD::CommonSourceStage& stage);
    /// Attach at the initial checkpoint boundary after the mesh water stage.
    /// Caller owns the driver through close; it must send due updates itself.
    std::string attachRuntimeWaterDriverCheckpoint(twoD::RuntimeWaterDriverCheckpoint&);
    const runoff::SourceWaterDeliveryBridge* sourceDelivery() const { return source_delivery_.get(); }
    /// Settle past surface work and publish newly completed private outlets
    /// before adopting a future runtime prescription at the accepted engine time.
    void flushRuntimeCouplingBoundary(bool changing_prescription=false);
    // Accepted-engine water history, separate from source-interval recording.
    // Caller keeps the recorder alive through end/close. Restore first, then resume.
    std::string setEngineSourceResultHistory(twoD::EngineSourceResultHistory&,bool detailed_source_intervals=false);
    std::string resumeEngineSourceResultHistory(twoD::EngineSourceResultHistory&);
#endif
    std::string saveCheckpoint(const std::string& path) const;
    std::string loadCheckpoint(const std::string& path);

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

    /// A4: LID solver access. The per-layer age block is keyed on a FLAT unit
    /// index while the units themselves live in per-type groups, so anything
    /// reading that block needs the manager to resolve the two.
    const lid::LIDSolver& lid() const noexcept { return lid_; }

    /// Groundwater solver access (for C API state injection).
    groundwater::GWSolver&       gwSolver()       noexcept { return groundwater_; }
    const groundwater::GWSolver& gwSolver() const noexcept { return groundwater_; }

    /// Snow solver access (for C API state injection).
    snow::SnowSolver&       snowSolver()       noexcept { return snow_; }
    const snow::SnowSolver& snowSolver() const noexcept { return snow_; }

    /// Control engine access (for C API rule compilation after initialize()).
    controls::ControlEngine&       controlEngine()       noexcept { return controls_; }
    const controls::ControlEngine& controlEngine() const noexcept { return controls_; }

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

    /**
     * @brief Enable/disable lenient (permissive) open.
     *
     * @details When enabled, open() still records post-parse validation errors
     *          (undefined objects, missing curves, etc.) into ctx.errors and the
     *          report, but does NOT fail the open — it leaves the engine in the
     *          OPENED state with all parsed objects intact and editable. This
     *          lets an editor/GUI load as many objects as feasible from a broken
     *          model. Hard reader failures (unreadable/malformed file) still
     *          fail. The default (false) preserves strict, legacy-matching
     *          behavior for the CLI and simulation. Callers should inspect the
     *          error list after a lenient open; running such a model still
     *          requires a fresh, strict open.
     */
    void set_lenient_open(bool on) noexcept { lenient_open_ = on; }

    /// BW-MSX gate access: the pollutant surface-quality store (read-only).
    const landuse::SurfaceQualitySoA& surfaceQuality() const noexcept { return surface_quality_; }

#ifdef OPENSWMM_HAS_2D
    /** @brief Access the 2D surface router (for C API delegation). */
    twoD::SurfaceRouter2D&       surfaceRouter2D()       noexcept { return surface_router_; }
    const twoD::SurfaceRouter2D& surfaceRouter2D() const noexcept { return surface_router_; }

    /** @brief Read the [UNCERTAINTY]-parsed configuration (see uncertainty_config_). */
    const uncertainty::UncertaintyConfig& uncertaintyConfig() const noexcept {
        return uncertainty_config_;
    }

    /** @brief Access the hydraulic router (for statistics / diagnostics). */
    Router&       router()       noexcept { return router_; }
    const Router& router() const noexcept { return router_; }

    /** @brief 1D spectral ROM (null if not built, or the network is too small). */
    const uncertainty::SpectralROM1D* rom1d() const noexcept { return rom1d_.get(); }

    /** @brief True when an [UNCERTAINTY] QUALITY source is active (PR 13b WQ layer). */
    bool wq_unc_active() const noexcept { return wq_unc_active_; }

    /**
     * @brief PR H5: per-active-node surcharge-attenuation factor from the
     * most recent stepRouting() call (active-node space, same indexing as
     * rom1d_active_map_/rom1d()'s buffers). Empty before the first step or
     * when the 1D ROM isn't built. Test/diagnostic accessor.
     */
    const std::vector<double>& rom1dAlphaBuffer() const noexcept { return rom1d_alpha_buf_; }
    /// H5b: mutable access to the surcharge-attenuation dials (ramp band,
    /// floor). Read every routing step by computeSurchargeAlpha(), so it can be
    /// set any time before stepping. Not parser-exposed; test/calibration knob.
    uncertainty::SurchargeAttenuationConfig& rom1dSurchargeConfig() noexcept { return rom1d_surcharge_cfg_; }
    /// H14: mutable access to the Froude-gate dials of the directional 1D
    /// operator (enabled, fr_lo, fr_hi). Read every routing step. Not
    /// parser-exposed; test/calibration knob.
    uncertainty::DirectionalOperatorConfig& rom1dDirectionalConfig() noexcept { return rom1d_directional_cfg_; }
    /// H14: fraction of conduits whose upstream coupling was gated (> 0) at
    /// the most recent operator assembly; 0 when no operator is installed.
    double rom1dDirectionalGatedFraction() const noexcept { return rom1d_dir_gated_frac_; }

    /**
     * @brief PR H11: mutable access to the phase-coordinate config. Intended
     * for the open()->initialize() window (same pattern as
     * surfaceRouter2D().options()) so tests/callers can flip
     * PhaseConfig::enabled off, or retune the physical dials, before the
     * first stepRouting() call. No .inp parser key exists for this (matches
     * H5's SurchargeAttenuationConfig, an internal knob only).
     */
    uncertainty::PhaseConfig& rom1dPhaseConfig() noexcept { return rom1d_phase_cfg_; }

    /**
     * @brief PR H11: per-active-node travel-time field T̄(x) from the most
     * recent refreshRom1dTravelTime() call (active-node space, same indexing
     * as rom1d_active_map_/rom1d()'s buffers). Empty before the first
     * refresh or when the 1D ROM isn't built. Test/diagnostic accessor.
     */
    const std::vector<double>& rom1dTravelTimeBuffer() const noexcept { return rom1d_tbar_buf_; }
#endif


private:
    // -----------------------------------------------------------------------
    // Sub-systems
    // -----------------------------------------------------------------------

    SimulationContext      ctx_;        ///< All simulation data (SoA + options)
    bool                   lenient_open_ = false;  ///< permissive open (see setter)
    bool                   plugins_prepare_attempted_ = false;  ///< start() reached prepare_all (report file may be open)
    PluginFactory          plugins_;   ///< Phase 4: plugin loader + lifecycle
    IOThread               io_thread_; ///< Phase 5: writer thread

    // Computational modules (batch-oriented, SoA)
    Router                       router_;       ///< Hydraulic routing (owns XSectGroups)
    runoff::RunoffSolver         runoff_;       ///< Subcatchment runoff (batch nonlinear reservoir)
    climate::ClimateFileReader   climate_file_; ///< Climate file reader (temp/evap/wind from file)
    snow::SnowSolver             snow_;         ///< Snowmelt (batch over subcatch×subareas)
    int                          last_melt_doy_ = -1;  ///< Day the seasonal melt coeffs were last set (legacy climate.c LastDay)
    groundwater::GWSolver        groundwater_;  ///< Groundwater (batch ODE per subcatchment)
    lid::LIDSolver               lid_;          ///< LID (batch by type group)
    quality::QualitySolver       quality_;      ///< Quality routing (batch link-load + mixing)
    transport::ArdEngine         ard_;          ///< QUALITY_SOLVER EULERIAN_ARD engine (phase E1)
    bool                         ard_init_attempted_ = false;  ///< lazy init after Router::init
    lard::LagrangianSolver       lard_;         ///< QUALITY_SOLVER LAGRANGIAN (X1 skeleton — no-op)
    landuse::LanduseSolver       landuse_solver_; ///< Buildup/washoff computation
    landuse::SurfaceQualitySoA   surface_quality_; ///< Per-subcatch surface quality state
    controls::ControlEngine      controls_;     ///< Control rule evaluation
    inflow::InflowSolver         inflow_;       ///< External + DWF inflows
    rdii::RDIISolver             rdii_;         ///< RDII (unit hydrograph convolution)
    exfil::ExfilSolver           exfil_;        ///< Storage node exfiltration
    inlet::InletSolver           inlet_;        ///< Street inlet capture
    std::vector<double>          gw_frac_perv_; ///< Per-subcatch pervious fraction for GW evap
    std::vector<double>          gw_perv_evap_; ///< Per-subcatch pervious evap rate (ft/sec)
    /// U3 (track I-b, 2026-09-07): the 2D surface's per-subcatchment
    /// infiltration recharge, drained from SurfaceRouter2D once per runoff
    /// step (m³) and converted to the ft/sec-over-full-area rate the GW
    /// solver's `infil_rate` argument expects.
    std::vector<double>          gw_2d_recharge_vol_;
    std::vector<double>          gw_infil_with_2d_;
    std::vector<double>          gw_infil_rate_;      ///< (Vinfil + VlidInfil) / area / dt, legacy's `infil` (ft/s)
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
    bool between_events_ = false;               ///< legacy BetweenEvents, as the last stepRouting evaluated it
    bool isBetweenEvents(double current_date) const; ///< Check if between routing events
    bool isInSteadyState(int action_count) const;    ///< Check if system is in steady state
    std::vector<gage::GageState> gage_states_;  ///< Per-gage state (SoA)

#ifdef OPENSWMM_HAS_2D
    twoD::SurfaceRouter2D        surface_router_; ///< Optional 2D surface routing solver
    /// S4b: per-row unit labels for the 2D species snapshot (pollutant units,
    /// MSX units, "hours", "degC"), rebuilt by fillSurfaceSnapshot() and
    /// pointed to by SimulationSnapshot::surface_species_units.
    mutable std::vector<std::string> surface_species_units_;

    /// Populated from the [UNCERTAINTY] section by register2DSections's
    /// parseUncertaintyLine. 2D-layer specs also set fields directly on
    /// surface_router_.options() (see that function). 1D specs drive
    /// buildROM1D() below; QUALITY specs drive the analytic WQ decay-rate
    /// band (PR 13b, ported 2026-10-10 as part of H6 — see wq_* members).
    uncertainty::UncertaintyConfig uncertainty_config_;

#ifdef OPENSWMM_HAS_2D
    /**
     * @brief SP4 (SR-2d re-port): deterministic gridded forcing for the RUNOFF
     *        and INFLOWS targets of [SOFT_RAINFALL_GRID].
     *
     * One runtime per FORCE_LOCATION source: its own GridFileReader, the
     * target subcatchment/node indices, the pixel per target (nearest pixel
     * centre) or, for RUNOFF with AREA_MEAN and [POLYGONS], a polygon-pixel
     * area-weight CSR. Staged every step into ctx_.forcing (RUNOFF: rainfall
     * OVERRIDE in user units; INFLOWS: lateral inflow ADD) with RESET
     * persistence, so applyForcings() and the runoff solver consume them like
     * any API forcing. Only the deterministic /location plane is used; the
     * /spread plane of these two targets has no ROM consumer on either branch.
     */
    struct SoftGridRuntime {
        uncertainty::GridTarget target = uncertainty::GridTarget::TWO_D;
        bool force_location = false;
        std::string file_path;
        std::string nodes_file;
        GridFileReader reader;
        std::vector<int> target_indices;       ///< subcatch/node indices in parse order
        std::vector<uint32_t> pixel_indices;   ///< nearest grid-cell center per target index
        uncertainty::GridMapping mapping = uncertainty::GridMapping::CENTROID;
        std::vector<int> csr_off;              ///< CSR row offsets (n_targets+1); empty => CENTROID
        std::vector<uint32_t> csr_px;          ///< CSR pixel indices
        std::vector<float> csr_w;              ///< CSR area-fraction weights (row-sum 1)
    };
    std::vector<SoftGridRuntime> soft_grid_runtimes_; ///< RUNOFF/INFLOWS grid forcing runtimes
    void initSoftGridRuntimes() noexcept;
    void stageSoftGridForcings() noexcept;
#endif

    // ---- 1D network spectral ROM (owned) ----
    // Deviation-form ensemble propagating uncertainty alongside the
    // deterministic DynWave solve. Built once in initHydraulics() when
    // uncertainty_config_.has_1d() or the 2D ROM is active; advanced every
    // routing step in stepRouting(); its quantiles are computed and flushed
    // to <rpt>.uncertainty.csv at report boundaries in postOutputSnapshot().
    // Forwarded to surface_router_.setROM1D() so 2D coupling can read
    // per-member 1D heads (SpectralROM::applyCouplingFlux).
    std::unique_ptr<uncertainty::GraphEigenBasis> rom1d_basis_;
    std::unique_ptr<uncertainty::SpectralROM1D>   rom1d_;
    std::vector<int>    rom1d_active_map_; ///< active_idx -> full_node_idx (for head extraction)
    std::vector<double> rom1d_h_buf_;      ///< per-active-node deterministic head buffer (reused each step)
    std::vector<double> rom1d_invert_buf_; ///< per-active-node invert elevations (filled once at build)
    std::vector<double> rom1d_sens_buf_;   ///< per-active-node depth (head - invert): Manning-sensitivity reference (PR 10)
    std::vector<double> rom1d_alpha_buf_;  ///< per-active-node PR H5 surcharge-attenuation factor (reused each step)
    // PR H14 — Froude-gated directional operator
    uncertainty::DirectionalOperatorConfig rom1d_directional_cfg_;
    std::vector<int>    rom1d_conduit_link_;   ///< edge -> link index: conduits, then (PR H17) structures, each group in link order
    // PR H17 — the ROM graph's edge view: every conduit, then every pump,
    // orifice, weir and outlet, filled from the H snapshot each step.
    std::vector<double> rom1d_edge_off_;
    std::vector<int>    rom1d_edge_n1_, rom1d_edge_n2_;
    /// Fill rom1d_edge_* from a snapshot. Returns the edge count, or 0 when
    /// the snapshot does not match the edges buildROM1D() collected.
    int rom1dEdgesFromSnapshot(const dynwave::HSnapshot& snap) noexcept;
    /// ROM regime inputs for this step, whatever the routing method: edge
    /// conductances into rom1d_edge_* plus per-link Froude and per-node
    /// surcharge flags. Uses the dynamic-wave snapshot when it is valid, and
    /// otherwise (FLOW_ROUTING FV, KINWAVE) derives the same quantities from
    /// the published hydraulic state with the solver's own Picard formula.
    /// Returns the edge count, 0 when nothing is available.
    int rom1dRegime(double dt, const double** link_froude, const std::uint8_t** surcharged) noexcept;
    std::vector<char>   rom1d_fs_pinned_;   ///< nodes overflowing at the rim (phi = 0)
    std::vector<double> rom1d_floor_buf_;   ///< per-node member floor min(invert, h_det)
    std::vector<double>       rom1d_state_froude_;
    std::vector<std::uint8_t> rom1d_state_surch_;
    bool rom1d_flow_source_ = true;
    /// PR H17 step 4: per-active-node band validity flag, refreshed at each
    /// report boundary (see refreshRom1dBandFlags()).
    std::vector<std::uint8_t> rom1d_flag_;
    void refreshRom1dBandFlags() noexcept;
    std::vector<uncertainty::FlowEdge> rom1d_fs_edges_;   ///< flowing links, oriented (scratch)
    std::vector<double> rom1d_fs_phi_;                    ///< phi per active node (scratch)
    /// Fill rom1d_sens_buf_ with phi = dh/d(ln n): the head shift that lets
    /// every conduit carry its deterministic flow again after its roughness
    /// rises. Returns false (buffer untouched) when the solve is singular.
    bool computeRom1dFlowSource() noexcept;
    std::vector<int>    rom1d_dir_a1_, rom1d_dir_a2_, rom1d_dir_sign_;   ///< per-conduit scratch
    std::vector<double> rom1d_dir_w_, rom1d_dir_fr_, rom1d_dir_drop_, rom1d_dir_M_;
    double rom1d_dir_gated_frac_ = 0.0;
    /// Assemble + install the directional operator from the live H snapshot
    /// (every routing step; O(E·k²)). Installs nothing — leaving the diagonal
    /// path bit-identical — when disabled or when no conduit is gated.
    void refreshRom1dDirectionalOperator(double dt) noexcept;

    // ---- SP1: gage-level soft rainfall -> 1D additive channel (H19) ----
    // The spread is split into TWO disjoint planes by coefficient family (H7):
    // plane A is NORMAL/LOGNORMAL gages (probit coefficient), plane B is
    // UNIFORM gages (2u-1 coefficient). They sum to the single-plane spread,
    // so each gage's spread meets exactly its own family's member coefficient
    // and there is no first-family fallback.
    std::vector<int>    rom1d_soft_off_;       ///< CSR offsets (n_active+1)
    std::vector<int>    rom1d_soft_gage_;      ///< CSR gage index per configured subcatchment contribution
    std::vector<char>   rom1d_soft_is_b_;      ///< CSR: 1 when the contribution's gage is UNIFORM (plane B)
    std::vector<int>    rom1d_soft_sub_;       ///< CSR: subcatchment index per contribution (SR-6 elasticity lookup)
    // ---- SR-6: rain->runoff elasticity for the soft-rain spread mapping ----
    uncertainty::RunoffElasticityProbe rom1d_runoff_probe_;
    bool rom1d_runoff_elasticity_enabled_ = true;   ///< Internal knob (A/B measurement), default on
    bool soft_rain_1d_active_ = false;         ///< True when any configured gage feeds an active node
    bool soft_rain_1d_has_a_  = false;         ///< Any NORMAL/LOGNORMAL contribution exists
    bool soft_rain_1d_has_b_  = false;         ///< Any UNIFORM contribution exists
    uncertainty::DistType soft_rain_1d_family_a_ = uncertainty::DistType::NORMAL; ///< Plane A family (NORMAL or LOGNORMAL)

    // ---- SP3: COHERENCE CORR_LEN for the gage path (CL-1c/CL-2c re-port) ----
    // One correlation length per network (max over contributing CORR_LEN gages,
    // the pre-port v1 policy). The per-member coefficient field depends only on
    // the fixed c_i, node geometry and corr_len, so it is built ONCE on the
    // first soft-forcing step. K_s < M -> reduced projection (psi/a, CL-2c);
    // K_s >= M -> materialized SoftSpatialField (CL-1c shape, SPDE-generated).
    double rom1d_soft_corr_len_ = 0.0;        ///< 0 => comonotone (FULL)
    uncertainty::SoftSpatialField rom1d_soft_field_; ///< Materialized M x n_active field (K_s >= M only)
    uncertainty::SpdeSpatialBasis rom1d_soft_basis_; ///< Whittle-Matern nu=2 basis over active nodes
    std::vector<double> rom1d_soft_psi_;      ///< psi_m(t), K_s x n_active row-major (ROM holds a pointer)
    std::vector<double> rom1d_soft_a_;        ///< a_im, M x K_s row-major (ROM holds a pointer)
    bool rom1d_soft_field_built_ = false;     ///< Build attempted (success or fallback) -- never retried
    bool rom1d_soft_reduced_ = false;         ///< Reduced path active (K_s < M)
    bool rom1d_soft_corr_warned_ = false;     ///< One-shot "no [COORDINATES]" fallback warning
    uncertainty::SurchargeAttenuationConfig rom1d_surcharge_cfg_; ///< PR H5 ramp band (defaults [0.9,1.1]); not
                                            ///< parser-exposed in this PR, internal knob only
    // ---- PR H11: per-member phase coordinate ------------------------------
    std::vector<double> rom1d_tbar_buf_;   ///< per-active-node travel time T̄(x) (s); refreshed on its own cadence
    uncertainty::PhaseConfig rom1d_phase_cfg_; ///< dials (enabled/celerity_factor/u_min/caps); not parser-exposed,
                                            ///< internal knob only -- matches H5's SurchargeAttenuationConfig
    double rom1d_tbar_last_refresh_ = -1.0e9;    ///< sim time (s) of the last refreshRom1dTravelTime() call
    double rom1d_tbar_refresh_interval_ = 60.0;  ///< min sim time (s) between T̄ refreshes
    /// Recompute rom1d_tbar_buf_ from the current conduit flow/velocity
    /// state (see the definition for the full-node scratch construction and
    /// why this must never be called from buildROM1D()).
    void refreshRom1dTravelTime() noexcept;
    std::ofstream rom1d_csv_;              ///< 1D ROM quantile CSV, written at report intervals

    // ---- PR 13b / H6: water-quality decay-rate uncertainty layer ----------
    // Analytic post-processing (WQ_LAYER_DESIGN.md): at every report
    // boundary, for each configured pollutant and every node, bound
    // c_prev*exp(-k*km_i*dt_report) over M multipliers km_i drawn from the
    // spec's distribution family. Independent of every ROM; v1 semantics =
    // the most recent report interval only (no parcel age in routing).
    std::vector<double> wq_conc_prev_;            ///< node concentrations at the previous report boundary (node*nPollut + p)
    std::ofstream       wq_csv_;                  ///< <rpt>.wq_uncertainty.csv (time_s,node_name,pollutant,q05,q50,q95)
    uncertainty::WQUncertaintyBounds wq_bounds_;  ///< the analytic bounds calculator
    std::vector<int>    wq_pollut_indices_;       ///< per QUALITY spec: pollutant index (-1 = unresolved, skipped)
    std::vector<double> wq_pollut_perturbations_; ///< per QUALITY spec: fractional decay-rate perturbation
    std::vector<uncertainty::DistType> wq_pollut_dists_; ///< per QUALITY spec: multiplier family (H6)
    bool                wq_unc_active_ = false;   ///< true when uncertainty_config_.has_quality()
    std::ofstream rom_diag_csv_;           ///< PR H3: <rpt>.rom_diag.csv (fr_trust/surcharge_frac/...)

    // ---- PR H10: threshold-crossing probability + modality flag ----------
    /// Per-active-node exceedance threshold, resolved ONCE at build time.
    /// `head` is an ABSOLUTE head in internal units, directly comparable to
    /// the reconstructed member values (which are absolute heads), so the
    /// per-boundary hot path is a plain comparison with no unit arithmetic.
    struct Rom1dThreshold {
        uncertainty::ThresholdKind kind = uncertainty::ThresholdKind::NONE;
        double head = 0.0;      ///< Absolute head at which the node crosses
        bool   has_ctrl = false;///< Node is a sensor in a [CONTROLS] premise
        double ctrl_head = 0.0; ///< That rule's setpoint as an absolute head
        bool   has_rim = false; ///< Node has a MaxDepth (rim) above its invert
        double rim_head = 0.0;  ///< Absolute head at which the node floods
    };
    std::vector<Rom1dThreshold> rom1d_thresholds_;
    std::ofstream rom_threshold_csv_;      ///< PR H10 <rpt>.rom_threshold.csv

    /// PR H10 modality criteria (spec defaults). Config fields rather than
    /// literals so H5's and H11's fixtures can exercise the boundaries, and
    /// so the G54 statistical review has one place to retune.
    double rom1d_modality_gap_ratio_ = 0.5;  ///< g_max/IQR must exceed this
    double rom1d_modality_min_side_  = 0.20; ///< min share on EACH side of the gap

    /// Resolve each active node's exceedance threshold and control setpoint.
    /// Called once from buildROM1D(); see the definition for the priority
    /// order and the display-vs-internal unit handling of setpoints.
    void buildRom1dThresholds() noexcept;

    /** @brief Build + seed the 1D spectral ROM from conduit connectivity and node heads. */
    void buildROM1D() noexcept;
    /// SP1: build the per-active-node gage CSR for gage-level soft rainfall.
    void initSoftRain1D(const std::vector<int>& active_map) noexcept;
    /// SP3: build the SPDE spatial basis / coefficient field for CORR_LEN gages.
    void buildRom1DSoftField() noexcept;

public:
    /// Measurement hook: 1D ROM mode count (0 = built-in rule). Set before initialize().
    int& rom1dModeCountOverride() noexcept { return rom1d_k_modes_override_; }
    /// Internal A/B hook: quasi-static residual of the Manning channel (default on). Set before initialize().
    bool& rom1dQuasiStatic() noexcept { return rom1d_qsr_; }
    /// Diagnostic: solver passes of the last additive-channel step (1 since the direct solve).
    int rom1dAdditiveSweeps() const noexcept { return rom1d_add_sweeps_; }
    /// Diagnostic: number of additive-channel planes on the last step (H19).
    int rom1dAdditivePlanes() const noexcept { return rom1d_add_np_; }
    /// Diagnostic: the per-mode rates (1/s) used on the last step (H18,
    /// docs/uncertainty/RELAXATION_RATES.md); empty before the first
    /// conductance/storage basis exists.
    const std::vector<double>& rom1dModeRates() const noexcept { return rom1d_h18_rates_; }
    /// Diagnostic: node storage S (ft^2) of the last H18 assembly, active space.
    const std::vector<double>& rom1dStorage() const noexcept { return rom1d_h18_S_; }
    /// Diagnostics: the oriented flow-response edges (H17/H19) and the additive
    /// channel state (P planes × active nodes) of the last step.
    const std::vector<uncertainty::FlowEdge>& rom1dFlowEdges() const noexcept { return rom1d_fs_edges_; }
    const std::vector<double>& rom1dAdditiveState() const noexcept { return rom1d_add_state_; }
    /// Diagnostics: the storage nodes whose members carry their own level
    /// (active indices), and those levels relative to the main run
    /// (nodes x members, ft) after the last step. ADDITIVE_CHANNEL.md §7.
    const std::vector<int>& rom1dStorageMemberNodes() const noexcept { return rom1d_nl_nodes_; }
    const std::vector<double>& rom1dStorageMemberLevels() const noexcept { return rom1d_nl_y_; }
    /// Diagnostic: per-member correction field (members x active nodes, ft).
    const std::vector<double>& rom1dMemberCorrection() const noexcept { return rom1d_corr_; }
    /// Per-member storage and rim levels (default OFF: stable on the threshold gate, not yet on
    /// Bellinge's coupled storage, 2026-10-24). Set before the first step.
    bool& rom1dStorageMembersEnabled() noexcept { return rom1d_nl_enabled_; }
    /// Diagnostic: conductance Laplacian Lc (ft^2/s) of the last H18 assembly, active space.
    const uncertainty::CsrGraph& rom1dConductance() const noexcept { return rom1d_h18_Lc_; }
    /// Diagnostic: the last flow-response field phi per active node (H17).
    const std::vector<double>& rom1dFlowResponse() const noexcept { return rom1d_fs_phi_; }
    /// PR H17: Manning sensitivity from the steady linearized flow response
    /// instead of depth x a fixed elasticity (internal hook while measured).
    bool& rom1dFlowSource() noexcept { return rom1d_flow_source_; }
    /// Band validity flag per active node at the last report boundary:
    /// 0 steady, 1 no_flow, 2 transient (rom1dBandFlagName()).
    const std::vector<std::uint8_t>& rom1dBandFlags() const noexcept { return rom1d_flag_; }
    static const char* rom1dBandFlagName(std::uint8_t f) noexcept {
        switch (f) { case 1: return "no_flow"; case 2: return "transient"; default: return "steady"; }
    }
    /// SR-6: enable/disable the runoff-elasticity correction of the soft-rain
    /// spread (default on). Off reproduces the pre-SR-6 mapping bit-for-bit.
    bool& rom1dRunoffElasticityEnabled() noexcept { return rom1d_runoff_elasticity_enabled_; }
    const uncertainty::RunoffElasticityProbe& rom1dRunoffProbe() const noexcept { return rom1d_runoff_probe_; }
private:
    int rom1d_k_modes_override_ = 0;
    bool rom1d_qsr_ = true;   ///< H17 quasi-static residual (internal A/B hook)
    int rom1d_add_sweeps_ = 0;
    int rom1d_add_np_ = 0;
    uncertainty::AdditiveSystem rom1d_add_sys_;
    std::vector<double> rom1d_add_qa_, rom1d_add_qb_;           ///< soft-rain extra inflow per unit coefficient (cfs)
    std::vector<double> rom1d_add_state_, rom1d_add_kappa_, rom1d_add_q_;
    std::vector<std::vector<double>> rom1d_vec_cols_;          ///< FORCING_VECTOR columns (theta) under H19
    std::vector<std::vector<double>> rom1d_fmult_cols_;        ///< FORCING_MULT columns (theta): scale the runoff plane
    /** @brief Node storage S (ft^2, active space) into rom1d_h18_S_. */
    void computeRom1dStorage() noexcept;
    /** @brief H19: advance every additive plane one backward-Euler step and hand them to the ROM. */
    void stepRom1dAdditiveChannel(double dt) noexcept;
    /** @brief Per-member levels at storage nodes with real outlet ratings, and
     *  one correction field per member for what the shared planes mis-deliver
     *  downstream (ADDITIVE_CHANNEL.md §7). Called by stepRom1dAdditiveChannel. */
    void stepRom1dStorageMembers(double dt, const std::vector<std::vector<double>>& q,
                                 const std::vector<std::vector<double>>& kap) noexcept;
    std::vector<uncertainty::FlowEdge> rom1d_fs_dry_edges_;   ///< dry pipes with a member-range secant (rain channel only)
    std::vector<uncertainty::FlowEdge> rom1d_add_edges_;      ///< the rain channel's edges: flowing links + dry pipes
    bool rom1d_nl_init_ = false;
    bool rom1d_nl_enabled_ = false;  ///< per-member storage/rim levels: off until stable on Bellinge (ADDITIVE_CHANNEL.md §7)
    std::vector<int> rom1d_nl_nodes_;                     ///< active indices of storage nodes
    std::vector<double> rom1d_nl_y_;                      ///< nodes x members, level above the main run (ft)
    std::vector<std::vector<double>> rom1d_nl_link_dq_;   ///< per link: members' extra outflow (cfs), storage out-links only
    std::vector<double> rom1d_corr_;                      ///< members x active nodes (ft)
    std::vector<char> rom1d_nl_struct_;                   ///< storage node with a structure link
    std::vector<int> rom1d_cp_of_active_;                 ///< coupling point per active node (-1 none)
    std::vector<char> rom1d_rom_pinned_;                  ///< ROM pin mask: spilling, minus per-member nodes
    std::vector<char> rom1d_corr_pinned_;
    uncertainty::AdditiveSystem rom1d_corr_sys_;
    std::vector<double> rom1d_fs_relay_;   ///< H18: per-link head relay (1 - Froude gate) from the flow response
    std::vector<double> rom1d_h18_wsym_, rom1d_h18_diag_, rom1d_h18_S_, rom1d_h18_rates_, rom1d_h18_tmp_;
    uncertainty::CsrGraph rom1d_h18_Lc_;
    /** @brief H18: conductance Laplacian and storage from the current state (needs rom1dRegime() and the flow response of this step). */
    bool assembleRom1dConductance(double dt) noexcept;
    /** @brief H18: per-mode generalized Rayleigh rates on the current basis into rom1d_h18_rates_. */
    void refreshRom1dModeRates() noexcept;

    /// Non-owning pointer to the 2D HDF5 output plugin (lifetime owned by
    /// PluginFactory's output_plugins_). Set in open() when [2D_OPTIONS]
    /// OUTPUT_FILE is configured; used in start() to call prepareMeshAndDatasets
    /// once the mesh is built.
    twoD::Default2DOutputPlugin* surface_output_plugin_ = nullptr;
    bool checkpoint_surface_output_ = false;
    twoD::RuntimeWaterDriverCheckpoint* runtime_water_driver_ = nullptr;

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

    bool step_active_ = false, runtime_pipeline_ = false;
    struct RestartState;
    bool checkpoint_enabled_ = false, checkpoint_ready_ = false;
    bool checkpoint_output_ = false;
    DefaultOutputPlugin* binary_output_plugin_ = nullptr; // owned by PluginFactory
    std::string checkpoint_model_;
    void checkCheckpointProfile(bool attaching_surface = false) const;
    std::string enableCheckpointingImpl(
#ifdef OPENSWMM_HAS_2D
        twoD::CommonSourceStage* stage, bool own_sources = false
#endif
    );
    std::string checkpointModel() const;
    void checkCheckpointBoundary() const;
    std::string checkpointBytes(std::string&) const;
    const std::vector<int>* ordinarySourceOwners() const noexcept;
    bool ownsSpatialSource(int source) const noexcept;
#ifdef OPENSWMM_HAS_2D
    friend class twoD::EngineSourceResultHistory;
    twoD::EngineSourceResultHistory* engine_source_history_ = nullptr;
    void detachEngineSourceHistory() noexcept;
    std::unique_ptr<runoff::SourceWaterDeliveryBridge> source_delivery_;
    twoD::CommonSourceStage* source_stage_ = nullptr; // caller keeps the stage alive
    std::array<double,6> source_totals_{}; // rain, ET, infiltration, return, runoff, drain (ft3)
    std::array<double,6> sourceTotals(const runoff::SourceWaterDriver&, const runoff::SourceWaterDeliveryBridge&) const;
    double spatialSourceStorage(const runoff::SourceWaterDriver&) const;
#endif
    double exchange_target_ = -1.0;

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
    double prev_runoff_step_sec_ = 0.0;  ///< span of the runoff step just completed (legacy oldRunoffStep)
    double new_runoff_ms_ = 0.0;    ///< legacy NewRunoffTime (msec)
    double new_rule_time_ms_ = 0.0; ///< legacy NewRuleTime (msec, routing.c:58)

    // Previous cumulative LID exfiltration / evaporation volumes (ft³), so the
    // per-step delta can be folded into the runoff-continuity infil / evap
    // outflow terms once per runoff step (issue #102 — legacy VlidInfil/VlidEvap).
    double prev_lid_infil_vol_ = 0.0;
    double prev_lid_evap_vol_  = 0.0;

    // PARITY: per-subcatchment interpolated wet-weather / GW inflows saved by
    // the Phase-2 interpolation so assembleLateralInflows() can replay
    // legacy's exact per-node accumulation ORDER — routing.c:466-473 adds
    // ext, dwf, then PER-SUBCATCHMENT wet-weather q (routing.c:716), then GW,
    // RDII, iface into Node.newLatFlow. Pre-summing per node and adding in a
    // different source order rounds differently (1-ULP lat-flow drift).
    std::vector<double> wet_q_interp_;  ///< per-subcatch interpolated runoff (cfs; NOT runon — it is already inside runoff[])
    std::vector<double> lid_drain_q_interp_; ///< per-node interpolated LID drain inflow this routing step (cfs; legacy lid_addDrainInflow)
    /// The same drain flows PER UNIT as (node, q), in legacy's order
    /// (subcatchments by index, each one's lidList). Legacy adds each unit's q
    /// to Node.newLatFlow individually; adding the per-node sum rounds
    /// differently once two units drain to one node.
    std::vector<std::pair<int, double>> lid_drain_q_units_;
    std::vector<double> gw_q_interp_;   ///< per-subcatch interpolated GW flow (cfs)
    std::vector<int>    gw_q_node_;     ///< receiving node for gw_q_interp_ (-1 = skip)
    // Legacy lid_getRunoff volume accumulators, per subcatchment per runoff
    // step: VlidIn (= sum over units of (captured/area)*area*tStep, ft3) and
    // qRunoff (= sum of unit surface outflow*area, cfs; *tStep = VlidOut).
    // subcatch.c:746-751 rebuilds newRunoff from these in the VOLUME domain;
    // adjusting the runoff RATE incrementally instead rounds differently
    // (1-ULP runoff drift on every LID deck).
    std::vector<double> lid_vlidin_vol_;  ///< per-subcatch VlidIn this runoff step (ft3)
    std::vector<double> lid_qsurf_cfs_;   ///< per-subcatch LID surface outflow to outlet (cfs)
    /// Previous runoff step's GW flow RATE per subcatch (ft/s per unit area,
    /// legacy TGroundwater::oldFlow). Legacy interpolates the RATE and
    /// multiplies by the area afterwards (routing.c:766, subcatch.c:912);
    /// interpolating the cfs product instead is a 1-ULP different number.
    std::vector<double> old_gw_rate_;

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

    /// Legacy DoRouting analog (swmm5.c:748): routing executes only when the
    /// model has nodes and IGNORE_ROUTING is off. Computed once in start();
    /// gates stepRouting(), statistics, routing mass balance, the routing
    /// step-size selection, and the outfall interface write.
    bool do_routing_ = true;

    // -----------------------------------------------------------------------
    // Report averaging accumulator (legacy RptFlags.averages)
    // -----------------------------------------------------------------------
    // When rpt_averages is true, node and link results are accumulated over
    // each routing step and averaged at report boundaries.  Subcatchment
    // results are always interpolated point values (matching legacy).
    // PARITY: the accumulators are float32 DISPLAY-unit sums — legacy's xAvg
    // slots are REAL4 fed by node_getResults/link_getResults at f = 1.0
    // (output.c:72, 853-901), so the whole accumulate/divide chain is float
    // arithmetic on display values. Accumulating doubles (or internal units)
    // diverges by ~1e-5 rel over a report period.
    struct AvgAccumulator {
        // Node accumulators (6 variables per node)
        std::vector<float> node_depth;
        std::vector<float> node_head;
        std::vector<float> node_volume;
        std::vector<float> node_lat_inflow;
        std::vector<float> node_total_inflow;
        std::vector<float> node_overflow;

        // Link accumulators (5 variables per link)
        std::vector<float> link_flow;
        std::vector<float> link_depth;
        std::vector<float> link_velocity;
        std::vector<float> link_volume;
        std::vector<float> link_capacity;

        int n_steps = 0;  ///< Number of routing steps accumulated

        void resize(int n_nodes, int n_links) {
            auto un = static_cast<std::size_t>(n_nodes);
            auto ul = static_cast<std::size_t>(n_links);
            node_depth.assign(un, 0.0f);
            node_head.assign(un, 0.0f);
            node_volume.assign(un, 0.0f);
            node_lat_inflow.assign(un, 0.0f);
            node_total_inflow.assign(un, 0.0f);
            node_overflow.assign(un, 0.0f);
            link_flow.assign(ul, 0.0f);
            link_depth.assign(ul, 0.0f);
            link_velocity.assign(ul, 0.0f);
            link_volume.assign(ul, 0.0f);
            link_capacity.assign(ul, 0.0f);
            n_steps = 0;
        }

        void reset() {
            std::fill(node_depth.begin(), node_depth.end(), 0.0f);
            std::fill(node_head.begin(), node_head.end(), 0.0f);
            std::fill(node_volume.begin(), node_volume.end(), 0.0f);
            std::fill(node_lat_inflow.begin(), node_lat_inflow.end(), 0.0f);
            std::fill(node_total_inflow.begin(), node_total_inflow.end(), 0.0f);
            std::fill(node_overflow.begin(), node_overflow.end(), 0.0f);
            std::fill(link_flow.begin(), link_flow.end(), 0.0f);
            std::fill(link_depth.begin(), link_depth.end(), 0.0f);
            std::fill(link_velocity.begin(), link_velocity.end(), 0.0f);
            std::fill(link_volume.begin(), link_volume.end(), 0.0f);
            std::fill(link_capacity.begin(), link_capacity.end(), 0.0f);
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


    /// Legacy-convention reported node volume (mirrors legacy node_getVolume):
    /// STORAGE → curve volume (ctx_.nodes.volume); junction/outfall/divider →
    /// nodes.rpt_full_volume·(depth/fullDepth) = 0 for plain junctions. Used for
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
    void setNextEvapDate(double the_date) noexcept;   ///< legacy climate.c setNextEvapDate
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

    /// Book the washoff mass against the POST-LID volumes (legacy
    /// surfqual_getWashoff's three bookings), after the units have run.
    void bookWashoffLoads(double dt_runoff) noexcept;

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
    /** @brief Assemble the run-on for the runoff step about to be taken —
     *  outfall return (over the previous step's span), upstream runoff and
     *  LID drains — into subcatches.runon_rate[] (ft/s, legacy Subcatch.runon)
     *  and runon_inflow[] (CFS). Legacy runoff.c:247-253 order. */
    void assembleRunon() noexcept;

    /** @brief Pre-compute GW surface water head and available node flow from routing state. */
    void assembleGWCoupling(double dt_runoff) noexcept;

    /** @brief Assemble all decomposed inflow sources into nodes.lat_flow and compute step mass balance.
     *  @param dt_routing  Current routing step (s); used to re-derive the 1D↔2D
     *                     coupling rate from the carried per-window exchange volume. */
    void assembleLateralInflows(double dt_routing) noexcept;

    void stepRouting(double dt_routing);

    /**
     * @brief Update node and link statistics after routing.
     *
     * @param dt_routing  Routing timestep (seconds).
     */
    void updateStatistics(double dt_routing) noexcept;

    /**
     * @brief Per-node routed volume totals (legacy massbal NodeInflow /
     *        NodeOutflow), accumulated over a half routing step.
     * @details Legacy routing_execute calls massbal_updateRoutingTotals(dt/2)
     *          before and after routing on every step, between events too.
     *          The report's Node Inflow Summary volumes, flow-balance error and
     *          Highest Continuity Errors read these totals.
     */
    void accumulateNodeRoutingTotals(double half_step) noexcept;

    /// True when the last stepRouting() skipped routing as steady state.
    bool last_step_steady_ = false;

    /// Previous routing step's system flow RATES (cfs) for the eleven routing
    /// continuity terms — legacy StepFlowTotals as they stood when the next
    /// step's first massbal_updateRoutingTotals(dt/2) read them.
    std::array<double, 11> routing_prev_rates_{};

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

    /**
     * @brief Project-level validation + step-clamp warnings (legacy project_validate).
     *
     * @details Clamps the dry/routing/wet time steps to legal relative ordering
     *          and to rain-gage recording intervals, recording legacy WARNING
     *          01/06/07 into ctx_.warnings (so they reach the .rpt). Called after
     *          resolve_cross_references and before the fatal-error gate in open().
     */
    void validate_project() noexcept;

    /**
     * @brief Write a report file containing the accumulated errors/warnings when
     *        open() fails before the report plugin is prepared.
     *
     * @details Matches legacy behavior where a failed swmm_open still leaves a
     *          .rpt with the ERROR/WARNING lines. No-op if no report path is set.
     */
    /*! Stop a routing solve that has gone non-physical (non-finite or absurd
     *  head/flow) instead of letting it run on and write NaN into the report.
     *  Returns SWMM_OK when the state is sane. */
    [[nodiscard]] int checkRoutingDiverged() noexcept;

    void write_open_failure_report() noexcept;

    /** @brief Set a fatal error on the context and transition to ERROR_STATE. */
    void set_error(int code, const char* message) noexcept;

    /** @brief Fire the warning callback (if registered). */
    void emit_warning(int code, const char* message) noexcept;

    /**
     * @brief Record a warning so it reaches BOTH the report (.rpt) and the API.
     *
     * @details Pushes @p message onto ctx_.warnings (the accumulator the report
     *          writer flushes, matching legacy report_writeWarningMsg) AND fires
     *          the on_warning callback. Use for engine-level warnings that would
     *          otherwise be callback-only (e.g. unknown/skipped input sections)
     *          and never appear in the report.
     */
    void push_report_warning(const std::string& message, int code) noexcept;

    /** @brief Fire the progress callback (if registered). */
    void emit_progress() noexcept;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_SWMM_ENGINE_HPP */
