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
 * @file      SWMMEngine.hpp
 * @brief     Lifecycle manager for the new openswmm.engine.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_SWMM_ENGINE_HPP
#define OPENSWMM_ENGINE_SWMM_ENGINE_HPP

#include "SimulationContext.hpp"
#include "HotStartManager.hpp"
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
#include "../2d/coupling/RuntimeWaterDriverCheckpoint.hpp"
#include "../hydrology/SourceWaterDeliveryBridge.hpp"
namespace openswmm::twoD {
class Default2DOutputPlugin;
class EngineSourceResultHistory;
class EngineSourceResultStream;
}
#endif

#include <array>
#include <functional>
#include <memory>
#include <string>

// Forward-declare the C callback types without pulling in the C header here
// (avoids polluting C++ translation units with extern "C" names)
extern "C" {
typedef void (*SWMM_ProgressCallback)(void*, double, double, void*);
typedef void (*SWMM_WarningCallback)(void*, int, const char*, void*);
typedef void (*SWMM_StepBeginCallback)(void*, double, double, void*);
typedef void (*SWMM_StepEndCallback)(void*, double, double, void*);
}

namespace openswmm {

class DefaultOutputPlugin;
struct CheckpointInspection;
struct CheckpointSessionResources;

/**
 * @brief Registered callback bundle (all optional).
 * @ingroup engine_core
 */
struct EngineCallbacks {
    SWMM_ProgressCallback on_progress = nullptr;
    void* progress_ud = nullptr;
    SWMM_WarningCallback on_warning = nullptr;
    void* warning_ud = nullptr;
    SWMM_StepBeginCallback on_step_begin = nullptr;
    void* step_begin_ud = nullptr;
    SWMM_StepEndCallback on_step_end = nullptr;
    void* step_end_ud = nullptr;
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
    /**
     * @brief Construct one independent CREATED engine and register process components once.
     *
     * Owns context, solvers, plugin factory and IO thread. With 2D enabled, wires context IO pointers
     * to this engine's router. Allocation/registration exceptions propagate. Caller serializes all
     * operations on each engine.
     */
    SWMMEngine();
    /**
     * @brief Destroy engine-owned subsystems and close active RUNNING/ENDED sessions.
     *
     * Explicit close() remains the lifecycle completion path for other states. Borrowed
     * context/solver/callback handles become invalid. Nonowned participants must remain alive until
     * engine cleanup completes.
     */
    ~SWMMEngine();

    // Non-copyable, non-movable (owns resources)
    /**
     * @brief Disallow copying simulation resources and engine identity.
     */
    SWMMEngine(const SWMMEngine&) = delete;
    /**
     * @brief Disallow copy replacement of engine resources.
     */
    SWMMEngine& operator=(const SWMMEngine&) = delete;
    /**
     * @brief Disallow moving an engine whose internal/nonowning pointers depend on stable identity.
     */
    SWMMEngine(SWMMEngine&&) = delete;
    /**
     * @brief Disallow move replacement of engine resources and identity.
     */
    SWMMEngine& operator=(SWMMEngine&&) = delete;

    // =========================================================================
    // Lifecycle operations (map 1:1 to C API)
    // =========================================================================

    /**
         * @brief Open an input file and parse the simulation model.
         *
         * @details Reads and resolves the input model and transitions to OPENED.
         *          Runtime initial conditions are applied by initialize().
         *
         * @param inp_path          Path to the input file.
         * @param rpt_path          Path for the report output file (NULL = no report).
         * @param out_path          Path for the binary output file (NULL = no binary out).
         * @param input_plugin_lib  Path to shared library providing an IInputPlugin,
         *                          or NULL to use the built-in .inp reader.
         * @returns SWMM_OK or an error code.

         * @param[in] metadata_only True performs parse/metadata preparation and disallows initialize(); false prepares an executable handle.
         */
    int open(const char* inp_path, const char* rpt_path, const char* out_path,
             const char* input_plugin_lib = nullptr, bool metadata_only = false) noexcept;

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
     * @brief Advance a RUNNING engine toward an accepted elapsed-second boundary.
     *
     * Sets a temporary step target and repeatedly steps until reached, failed or no elapsed progress.
     * With 2D, flushes coupling on normal success and refuses unsent driver updates. Does not roll back
     * already accepted steps after a later failure.
     *
     * @param[in] seconds Finite target seconds in [current accepted time,total duration]; zero-distance
     *     advance is permitted subject to participant readiness.
     * @param[out] actual Non-null accepted-seconds destination. Preserved for null pointer/nonfinite
     *     target; otherwise initialized to current accepted time before lifecycle/range checks and
     *     updated after stepping.
     * @return SWMM_OK or step error; CFFI_ERR_BADPARAM for invalid target/output, CFFI_ERR_LIFECYCLE
     *     for nonrunning/reentrant or due-participant refusal, or WRONG_STATE after a caught exception.
     */
    int advanceTo(double seconds, double* actual) noexcept;
    /**
     * @brief Read the accepted clock used by private runtime source transactions.
     * @return Elapsed seconds: ctx.current_time for qualified runtime catchment forcing, otherwise
     *     elapsed_ms/1000.
     */
    double runtimeCouplingTime() const noexcept;
    /**
     * @brief Report whether the engine is outside its internal runtime pipeline phase.
     * @return !runtime_pipeline_; this hint alone does not validate lifecycle, reentrancy or a source
     *     transaction.
     */
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

    // Internal same-platform engine-session restart. Engine sessions preserve
    // shared 1D node/link temperature and bed state. A qualified 2D
    // CommonSourceStage attachment also preserves mesh/ground thermal state
    // and routed channel-ground thermal receipts. Direct hydraulic component
    // checkpoints do not preserve thermal state. Enable at start()'s initial
    // boundary; restore into an identically initialized fresh session.
    // Session preparation is internal; public operations require this
    // attachment. External water drivers require their participant.
    // Private binary-output qualification: initialize(), prepare this mode,
    // start(1), then enable checkpointing. Requires a new output destination.
    /**
     * @brief Qualify built-in binary output continuation before start(1).
     *
     * Requires INITIALIZED at time zero, inactive pipeline, configured built-in binary writer and
     * supported native plugins. Destination must be new; no surface writer may be configured for this
     * binary-only preparation. Marks the binary writer fresh without opening the checkpoint itself.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string prepareOutputCheckpointing();
    /**
     * @brief Attach a supported numerical checkpoint profile at the initial running boundary.
     *
     * Delegates enableCheckpointingImpl() without a common-source stage. Requires a supported native
     * model, time zero and no active pipeline/previous attachment. Snapshots canonical model identity
     * for subsequent save/restore checks; does not itself create an on-disk checkpoint.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string enableCheckpointing();
    /**
     * @brief Queue owned native engine-checkpoint bytes for start-time restore.
     *
     * Requires INITIALIZED and no existing queued checkpoint. Does not decode/adopt state during this
     * call; start() performs the restoration.
     *
     * @param[in] checkpoint Nonempty byte string passed by value and moved into pending storage; not a
     *     path.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string queueHotstartCheckpoint(std::string checkpoint);
    /**
     * @brief Rebase mass-balance initial inventories on the current restored state.
     *
     * Delegates initMassBalance() after hotstart adoption. Requires already initialized module state;
     * no public lifecycle guard or exception rollback here.
     */
    void reopenMassBalance() noexcept;
    /**
     * @brief Capture a qualified accepted boundary and write a native full-engine hotstart.
     *
     * Uses checkpointBytes(...,include_outputs=false), then
     * HotStartManager::save_with_engine_checkpoint with current runoff/GW state. Does not package
     * output continuations; returns HotStartManager IO text on save refusal.
     *
     * @param[in] path Borrowed checkpoint/native-hotstart filename; synchronous file operations can be
     *     substantial.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string saveNativeHotstart(const std::string& path) const;
    /**
     * @brief Read whether a full-engine checkpoint attachment exists.
     * @return checkpoint_enabled_; does not revalidate current boundary readiness or model
     *     compatibility.
     */
    bool hotstartCheckpointAttached() const noexcept { return checkpoint_enabled_; }
#ifdef OPENSWMM_HAS_2D
    // Separate single-file mesh output qualification; initialize(), prepare,
    // start(0), then attach the common source checkpoint stage.
    /**
     * @brief Qualify a new mesh-only output destination before start(0).
     *
     * Requires INITIALIZED/time zero, no active/attached checkpoint and the sole built-in mesh writer
     * with native plugins; no binary writer may be configured. Destination must be absent. Marks mesh
     * output fresh; unavailable HDF5 builds return a diagnostic.
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string prepareSurfaceOutputCheckpointing();
    /**
     * @brief Qualify two distinct fresh binary and mesh output destinations before start(1).
     *
     * Requires INITIALIZED/time zero, active surface, exactly the built-in binary/mesh writers and
     * supported native plugins. Both destinations must be absent and weakly canonical paths distinct.
     * Does not itself attach a source stage or write a checkpoint.
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string prepareCombinedOutputCheckpointing();
    // Qualification attachment only: mesh-owned common sources, with ordinary
    // engine runoff outside the mesh. Stage lifetime must cover the session.
    /**
     * @brief Attach an initial engine checkpoint with a borrowed common-source stage.
     *
     * Delegates enableCheckpointingImpl(&stage,false). The stage must be mesh-only unless explicit
     * private spatial-source ownership is selected by enableSpatialSourceCheckpointing(). Requires
     * initial RUNNING accepted boundary.
     *
     * @param[in,out] stage Borrowed initialized stage matching this router mesh/state/subsurface;
     *     caller keeps it alive until engine close/detachment.
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string enableCheckpointing(twoD::CommonSourceStage& stage);
    /**
     * @brief Attach explicit private spatial-source ownership and its delivery bridge.
     *
     * Delegates enableCheckpointingImpl(&stage,true). Requires selected catchment groups, matching
     * receiver/router state and compatible conservative return receipts; stage remains externally
     * owned.
     *
     * @param[in,out] stage Borrowed initialized stage matching this router mesh/state/subsurface;
     *     caller keeps it alive until engine close/detachment.
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string enableSpatialSourceCheckpointing(twoD::CommonSourceStage& stage);
    /**
     * @brief Bind a caller-owned replay/checkpoint participant to the initial mesh water profile.
     *
     * Requires enabled accepted time-zero checkpoint, no active pipeline/other driver/history/stream
     * and qualified runtime-water checkpointing. Captures initial participant state and appends
     * identity to model binding before storing its pointer. Does not drive participant updates itself.
     *
     * @param[in,out] driver Borrowed participant with immutable nonempty identity <=4096 bytes matching
     *     the common-stage runtime-water identity. Caller keeps it alive through close and sends due
     *     updates.
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string attachRuntimeWaterDriverCheckpoint(twoD::RuntimeWaterDriverCheckpoint& driver);
    /**
     * @brief Borrow the private source-to-network delivery bridge.
     *
     * Copy/read only under engine serialization; pointer invalid after detach/close/destruction.
     * @return Nonowning pointer to owned bridge, or nullptr if no explicit spatial-source attachment.
     */
    const runoff::SourceWaterDeliveryBridge* sourceDelivery() const {
        return source_delivery_.get();
    }
    /**
     * @brief Borrow the stage source driver only when source delivery is attached.
     *
     * Participant lifetime is external and must cover the engine session.
     * @return Nonowning stage-owned source pointer when both bridge and stage exist, otherwise nullptr.
     */
    const runoff::SourceWaterDriver* completedSourceDriver() const;
    /**
     * @brief Complete pending surface work and publish source delivery at accepted engine time.
     *
     * Refuses unsupported full-history barriers before flushing. Preflights attached stream, flushes
     * surface batch, captures delivery and updates runoff mass-balance deltas/storage; stream may then
     * append a CouplingBarrier event. Does not roll back already completed surface work on a later
     * recording refusal.
     * @throws std::invalid_argument For a history profile unable to represent the
     * requested barrier.
     * @throws std::runtime_error For stream/delivery capture refusal.
     *
     * @param[in] changing_prescription True also requests a coupling-barrier stream frame before a
     *     future prescription update; false flushes pending work only.
     */
    void flushRuntimeCouplingBoundary(bool changing_prescription = false);
    // Accepted-engine water history, separate from source-interval recording.
    // Caller keeps the recorder alive through end/close. Restore first, then resume.
    /**
     * @brief Attach an empty in-memory accepted-engine source history at time zero.
     *
     * Requires no active pipeline/other recorder and an unbound CPU explicit-inertial marcher. Stages
     * the initial append in a copy then retains the recorder pointer; no source journal is created
     * here.
     *
     * @param[in,out] history Borrowed recorder; caller owns it through engine end/close and prevents
     *     concurrent mutation.
     * @param[in] detailed_source_intervals True also binds the recorder to CPU-marcher source-interval
     *     events.
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string setEngineSourceResultHistory(twoD::EngineSourceResultHistory& history,
                                             bool detailed_source_intervals = false);
    /**
     * @brief Resume an existing caller-owned source history after checkpoint restore.
     *
     * Requires no active pipeline/other recorder and unbound CPU marcher. history.resume() checks
     * continuation against this engine before pointer attachment; detailed mode rebinds interval
     * events.
     *
     * @param[in,out] history Borrowed recorder; caller owns it through engine end/close and prevents
     *     concurrent mutation.
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string resumeEngineSourceResultHistory(twoD::EngineSourceResultHistory& history);
    /**
     * @brief Initialize and attach a caller-owned full-history source journal stream.
     *
     * Requires time zero, inactive pipeline, no existing recorder and an unbound CPU marcher.
     * Initializes stream first, then attaches engine/marcher pointers; does not expose a public GUI
     * activation path.
     *
     * @param[in,out] stream Borrowed stream participant that must outlive engine recording through
     *     close/detachment.
     * @param[in] path Borrowed journal destination forwarded to stream.initialize(); participant
     *     enforces its file/publication policy.
     * @param[in] detailed True records source intervals as well as accepted engine events.
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string setEngineSourceResultStream(twoD::EngineSourceResultStream& stream,
                                            const std::string& path, bool detailed = false);
    /**
     * @brief Append an accepted runtime-prescription update event when a source stream exists.
     *
     * No-op without a stream. Captures at ctx.current_time without advancing physics.
     * @throws
     * std::runtime_error When stream capture refuses the event.
     */
    void recordRuntimeCouplingPrescription();
    /**
     * @brief Select file-backed companion publication at an initial combined-output journal boundary.
     *
     * Requires time zero, inactive pipeline, attached source stream, both prepared binary/mesh outputs
     * and an accepted checkpoint boundary. Sets the file-profile flag; no file checkpoint is published
     * yet. Requires HDF5 output support.
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string enableFileOutputCheckpointing();
#endif
    /**
     * @brief Save a qualified accepted checkpoint using replace-permitted publication.
     *
     * Delegates saveCheckpointImpl(path,false); validates profile/model/physical boundary and may
     * serialize bound source/output companions. Does not roll back already accepted simulation time on
     * IO refusal.
     *
     * @param[in] path Borrowed checkpoint/native-hotstart filename; synchronous file operations can be
     *     substantial.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string saveCheckpoint(const std::string& path) const;
    /**
     * @brief Save a qualified accepted checkpoint to an exclusively new destination.
     *
     * Delegates saveCheckpointImpl(path,true); path must be nonempty/NUL-free, destination absent and
     * exclusive-publication support available. Refuses existing entries rather than replacing.
     *
     * @param[in] path Borrowed checkpoint/native-hotstart filename; synchronous file operations can be
     *     substantial.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string saveCheckpointNew(const std::string& path) const;
    /**
     * @brief Initialize, start and own a managed checkpoint session from OPENED.
     *
     * Performs preflight before starting native modules/writers; refuses registered callbacks,
     * unsupported providers/components and ancillary file outputs. A failure after initialization
     * begins closes the engine; preflight refusal leaves OPENED. Owns resources needed to keep private
     * stage/recorders alive.
     * @see swmm_checkpoint_start_session().
     *
     * @param[in] output SWMM_CHECKPOINT_OUTPUT_* profile from NONE through FILES; determines required
     *     fresh binary/mesh companions.
     * @param[in] source_path Borrowed UTF-8 source-journal path; empty disables journaling. Detailed
     *     intervals and FILES require a nonempty journal.
     * @param[in] detailed True records detailed source intervals and requires a source path.
     * @param[in] ownership_token Borrowed current reviewed spatial-source ownership token; validated
     *     against requested ownership when present.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string startCheckpointSession(int output, const std::string& source_path, bool detailed,
                                       const std::string& ownership_token);
    /**
     * @brief Read inexpensive checkpoint attachment/lifecycle hints.
     *
     * BOUNDARY/FRESH require RUNNING, zero error, do_routing, no active pipeline/target and ready
     * attachment; fresh additionally means time zero. Does not perform full model/physics validation.
     * @return SWMM_CHECKPOINT_SESSION_* bitmask for attached, managed, accepted boundary and fresh
     *     status.
     */
    unsigned long checkpointSessionStatus() const noexcept;
    /**
     * @brief Restore an on-disk checkpoint into a matching fresh prepared session.
     *
     * Delegates loadCheckpointPath(path,nullptr), requiring time-zero accepted profile and compatible
     * model/participants/output bindings. Does not prepare a new session automatically.
     *
     * @param[in] path Borrowed checkpoint/native-hotstart filename; synchronous file operations can be
     *     substantial.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string loadCheckpoint(const std::string& path);
    /**
     * @brief Validate a checkpoint against a fresh prepared session without installing state.
     *
     * Uses a temporary candidate and loadCheckpointPath() inspection mode. Temporary output copies are
     * discarded; actual restore separately checks publication eligibility. Does not certify external
     * adapter availability.
     *
     * @param[in] path Borrowed checkpoint/native-hotstart filename; synchronous file operations can be
     *     substantial.
     * @param[out] inspection Caller-owned metadata replaced only after a fully successful inspection;
     *     preserved on refusal.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string inspectCheckpoint(const std::string& path, CheckpointInspection& inspection);
    /**
     * @brief Package accepted file-backed checkpoint prefixes into a new transport directory.
     *
     * Requires file-backed HDF5 output support in the build. Copies accepted prefixes, preserving
     * external model/provider initialization requirements; not an engine-state restore.
     *
     * @param[in] checkpoint Nonempty NUL-free saved file-backed checkpoint path; bundle
     *     physical/source/output bindings and checksums are verified.
     * @param[in] directory Nonempty NUL-free new destination directory; exclusive bundle publication is
     *     required.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    static std::string packageCheckpointBundle(const std::string& checkpoint,
                                               const std::string& directory);

    // =========================================================================
    // Callback registration
    // =========================================================================

    /**
     * @brief Replace the progress callback and borrowed user pointer.
     *
     * Stores pointers only; no ownership, lifecycle check or synchronization. Callbacks must not throw
     * across noexcept engine operations and should follow the public callback reentrancy rules.
     *
     * @param[in] cb Function pointer, or NULL to disable this callback.
     * @param[in] user_data Opaque borrowed pointer forwarded unchanged; caller keeps referenced state
     *     alive through every callback.
     */
    void set_progress_callback(SWMM_ProgressCallback cb, void* user_data) noexcept;
    /**
     * @brief Replace the warning callback and borrowed user pointer.
     *
     * Stores pointers only; no ownership, lifecycle check or synchronization. Callbacks must not throw
     * across noexcept engine operations and should follow the public callback reentrancy rules.
     *
     * @param[in] cb Function pointer, or NULL to disable this callback.
     * @param[in] user_data Opaque borrowed pointer forwarded unchanged; caller keeps referenced state
     *     alive through every callback.
     */
    void set_warning_callback(SWMM_WarningCallback cb, void* user_data) noexcept;
    /**
     * @brief Replace the step begin callback and borrowed user pointer.
     *
     * Stores pointers only; no ownership, lifecycle check or synchronization. Callbacks must not throw
     * across noexcept engine operations and should follow the public callback reentrancy rules.
     *
     * @param[in] cb Function pointer, or NULL to disable this callback.
     * @param[in] user_data Opaque borrowed pointer forwarded unchanged; caller keeps referenced state
     *     alive through every callback.
     */
    void set_step_begin_callback(SWMM_StepBeginCallback cb, void* user_data) noexcept;
    /**
     * @brief Replace the step end callback and borrowed user pointer.
     *
     * Stores pointers only; no ownership, lifecycle check or synchronization. Callbacks must not throw
     * across noexcept engine operations and should follow the public callback reentrancy rules.
     *
     * @param[in] cb Function pointer, or NULL to disable this callback.
     * @param[in] user_data Opaque borrowed pointer forwarded unchanged; caller keeps referenced state
     *     alive through every callback.
     */
    void set_step_end_callback(SWMM_StepEndCallback cb, void* user_data) noexcept;

    // =========================================================================
    // Context access (for C API wrappers)
    // =========================================================================

    /**
     * @brief Borrow this engine's simulation context.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned simulation context storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    SimulationContext& context() noexcept { return ctx_; }
    /**
     * @brief Borrow this engine's simulation context.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned simulation context storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const SimulationContext& context() const noexcept { return ctx_; }

    /**
     * @brief Borrow this engine's LID solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned LID solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const lid::LIDSolver& lid() const noexcept { return lid_; }

    /**
     * @brief Borrow this engine's groundwater solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned groundwater solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    groundwater::GWSolver& gwSolver() noexcept { return groundwater_; }
    /**
     * @brief Borrow this engine's groundwater solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned groundwater solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const groundwater::GWSolver& gwSolver() const noexcept { return groundwater_; }

    /**
     * @brief Borrow this engine's snow solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned snow solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    snow::SnowSolver& snowSolver() noexcept { return snow_; }
    /**
     * @brief Borrow this engine's snow solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned snow solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const snow::SnowSolver& snowSolver() const noexcept { return snow_; }

    /**
     * @brief Borrow this engine's control engine.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned control engine storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    controls::ControlEngine& controlEngine() noexcept { return controls_; }
    /**
     * @brief Borrow this engine's control engine.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned control engine storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const controls::ControlEngine& controlEngine() const noexcept { return controls_; }

    /**
     * @brief Borrow this engine's inflow solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned inflow solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    inflow::InflowSolver& inflowSolver() noexcept { return inflow_; }
    /**
     * @brief Borrow this engine's inflow solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned inflow solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const inflow::InflowSolver& inflowSolver() const noexcept { return inflow_; }

    /// Re-derive the land-use buildup/washoff parameter cache from the live
    /// context (for C API runtime edits via swmm_buildup_set/_washoff_set).
    /// Leaves the accumulated buildup pool untouched.
    void refreshLanduseParams() noexcept;

    /**
     * @brief Recompile one node/pollutant treatment and refresh its treatment-presence flag.
     *
     * Resizes the treatment store if dimensions differ, parses the current expression and clears
     * compiled cell on parse refusal. Does not throw a C ABI error code for an invalid index.
     *
     * @param[in] node_idx Zero-based node index; an invalid index is a successful no-op in this
     *     internal helper.
     * @param[in] pollut_idx Zero-based pollutant index; invalid or zero-sized model is a successful
     *     no-op.
     * @return Zero for success/no-op, or treatment parser code for an invalid expression.
     */
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
     * @brief Open a runoff interface stream for SAVE against current model dimensions.
     *
     * Owns a new interface object; discards it after open refusal. No lifecycle validation here. Runoff
     * substeps automatically save while active. Because this method is noexcept, an uncaught allocation
     * exception terminates.
     *
     * @param[in] path Borrowed binary output filename; opening may create/truncate according to the
     *     interface implementation.
     * @return Zero on success, -10 if a stream is already open, or underlying interface-open error.
     */
    int openRunoffIfaceWrite(const std::string& path) noexcept;

    /**
     * @brief Open a runoff interface stream for USE and verify model compatibility.
     *
     * Owns the stream and discards failed opens. The runoff USE path can consume records automatically
     * in stepRunoff(); explicit reads use readRunoffIfaceStep(). No lifecycle validation here.
     *
     * @param[in] path Borrowed input filename whose subcatchment/pollutant/flow-unit header must match
     *     current model.
     * @return Zero on success, -10 if already open, or underlying read/header error.
     */
    int openRunoffIfaceRead(const std::string& path) noexcept;

    /**
     * @brief Ask the runoff interface to append the current state record.
     *
     * No-op without an interface or outside SAVE mode. This void wrapper does not expose write failure
     * to its caller.
     *
     * @param[in] dt Runoff substep duration in seconds, passed unchanged to saveResults().
     */
    void saveRunoffIfaceStep(double dt) noexcept;

    /**
     * @brief Read one interface record into the current runoff/quality stores.
     *
     * No lifecycle check and no independent distinction between exhausted and unavailable stream.
     * @return True for a successfully adopted record; false for EOF/read refusal or no available USE
     *     stream.
     */
    bool readRunoffIfaceStep() noexcept;

    /**
     * @brief Close and release the owned runoff interface stream.
     *
     * Idempotent when absent. Does not return a close/flush diagnostic or reset authored FilesSpec
     * tokens.
     */
    void closeRunoffIface() noexcept;

    /**
     * @brief Read the authored mode associated with an open runoff interface.
     *
     * This is not queried from the interface object; callers opening directly must keep FilesSpec
     * coherent.
     * @return NONE when no interface is open; otherwise ctx.files.runoff_mode.
     */
    FileMode runoffIfaceMode() const noexcept;

    /**
     * @brief Borrow this engine's runoff solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned runoff solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    runoff::RunoffSolver& runoff_solver() noexcept { return runoff_; }
    /**
     * @brief Borrow this engine's runoff solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned runoff solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const runoff::RunoffSolver& runoff_solver() const noexcept { return runoff_; }

    /**
     * @brief Borrow this engine's groundwater solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned groundwater solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    groundwater::GWSolver& gw_solver() noexcept { return groundwater_; }
    /**
     * @brief Borrow this engine's groundwater solver.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned groundwater solver storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const groundwater::GWSolver& gw_solver() const noexcept { return groundwater_; }

    /**
     * @brief Borrow this engine's plugin factory.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned plugin factory storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    PluginFactory& plugin_factory() noexcept { return plugins_; }
    /**
     * @brief Borrow this engine's plugin factory.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned plugin factory storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const PluginFactory& plugin_factory() const noexcept { return plugins_; }

    /**
     * @brief Read the context fatal-error code.
     * @return ctx.error_code; zero means no currently recorded fatal error.
     */
    int last_error() const noexcept { return ctx_.error_code; }

    /**
     * @brief Borrow the context fatal-error message.
     * @return Engine-owned NUL-terminated bytes; copy before another error/context reset or engine
     *     destruction.
     */
    const char* last_error_message() const noexcept { return ctx_.error_message.c_str(); }

    /**
     * @brief Select whether post-parse validation errors prevent a subsequent open.
     *
     * Does not make unreadable/malformed input acceptable or authorize running an invalid model. Set
     * before open; a strict valid open is required for execution.
     *
     * @param[in] on True retains an editable OPENED model despite soft validation errors; false uses
     *     strict open behavior.
     */
    void set_lenient_open(bool on) noexcept { lenient_open_ = on; }

    /** @brief Access the hydraulic router (for C API delegation — Phase 7
     *         run-time geometry change needs the FV mesh and solver). */
    Router& router() noexcept { return router_; }
    const Router& router() const noexcept { return router_; }

    /**
     * @brief Borrow this engine's surface-quality store.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned surface-quality store storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const landuse::SurfaceQualitySoA& surfaceQuality() const noexcept { return surface_quality_; }

#ifdef OPENSWMM_HAS_2D
    /**
     * @brief Borrow this engine's 2D surface router.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned 2D surface router storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    twoD::SurfaceRouter2D& surfaceRouter2D() noexcept { return surface_router_; }
    /**
     * @brief Borrow this engine's 2D surface router.
     *
     * Caller serializes access. A mutable reference bypasses C API validation; caller must preserve
     * model/solver invariants and avoid mutation during stepping or output access.
     * @return Reference to owned 2D surface router storage, valid only while the engine owns it; no
     *     lifecycle/readiness check.
     */
    const twoD::SurfaceRouter2D& surfaceRouter2D() const noexcept { return surface_router_; }
#endif

private:
    // -----------------------------------------------------------------------
    // Sub-systems
    // -----------------------------------------------------------------------

    // Declared first so resources outlive context/router/plugin destruction,
    // including error teardown. shared_ptr supports an out-of-line resource type.
    std::shared_ptr<CheckpointSessionResources> checkpoint_session_resources_;
    SimulationContext ctx_;      ///< All simulation data (SoA + options)
    bool lenient_open_ = false;  ///< permissive open (see setter)
    bool metadata_only_ = false; ///< parse-only handle cannot initialize a run
    bool plugins_prepare_attempted_ =
        false; ///< start() reached prepare_all (report file may be open)
    bool hotstart_deferred_ =
        false; ///< initialize(): a native USE HOTSTART waits for the end of initialize()
    std::vector<char>
        hotstart_save_done_; ///< per SAVE HOTSTART row: its dated save was written this run
    PluginFactory plugins_;  ///< Phase 4: plugin loader + lifecycle
    IOThread io_thread_;     ///< Phase 5: writer thread

    // Computational modules (batch-oriented, SoA)
    Router router_;               ///< Hydraulic routing (owns XSectGroups)
    runoff::RunoffSolver runoff_; ///< Subcatchment runoff (batch nonlinear reservoir)
    std::vector<double>
        hotstart_runoff_; ///< USE HOTSTART runoff section, applied in init_modules()
    LegacySpeciesTrailer
        hotstart_species_; ///< USE HOTSTART species surface block, applied in init_modules()
    climate::ClimateFileReader climate_file_; ///< Climate file reader (temp/evap/wind from file)
    snow::SnowSolver snow_;                   ///< Snowmelt (batch over subcatch×subareas)
    int last_melt_doy_ =
        -1; ///< Day the seasonal melt coeffs were last set (legacy climate.c LastDay)
    groundwater::GWSolver groundwater_;     ///< Groundwater (batch ODE per subcatchment)
    lid::LIDSolver lid_;                    ///< LID (batch by type group)
    quality::QualitySolver quality_;        ///< Quality routing (batch link-load + mixing)
    transport::ArdEngine ard_;              ///< QUALITY_SOLVER EULERIAN_ARD engine (phase E1)
    bool ard_init_attempted_ = false;       ///< lazy init after Router::init
    lard::LagrangianSolver lard_;           ///< QUALITY_SOLVER LAGRANGIAN (X1 skeleton — no-op)
    landuse::LanduseSolver landuse_solver_; ///< Buildup/washoff computation
    landuse::SurfaceQualitySoA surface_quality_; ///< Per-subcatch surface quality state
    controls::ControlEngine controls_;           ///< Control rule evaluation
    inflow::InflowSolver inflow_;                ///< External + DWF inflows
    rdii::RDIISolver rdii_;                      ///< RDII (unit hydrograph convolution)
    exfil::ExfilSolver exfil_;                   ///< Storage node exfiltration
    inlet::InletSolver inlet_;                   ///< Street inlet capture
    std::vector<double> gw_frac_perv_;           ///< Per-subcatch pervious fraction for GW evap
    std::vector<double> gw_perv_evap_;           ///< Per-subcatch pervious evap rate (ft/sec)
    /// U3 (track I-b, 2026-09-07): the 2D surface's per-subcatchment
    /// infiltration recharge, drained from SurfaceRouter2D once per runoff
    /// step (m³) and converted to the ft/sec-over-full-area rate the GW
    /// solver's `infil_rate` argument expects.
    std::vector<double> gw_2d_recharge_vol_;
    std::vector<double> gw_infil_with_2d_;
    std::vector<double>
        gw_infil_rate_;             ///< (Vinfil + VlidInfil) / area / dt, legacy's `infil` (ft/s)
    std::vector<double> snow_rain_; ///< Per-subcatch rainfall into snow step (ft/sec)
    std::vector<double> snow_snow_; ///< Per-subcatch snowfall into snow step (ft/sec)
    hydstruct::StructureSolver hydstruct_; ///< Pumps, orifices, weirs, outlets
    iface::InterfaceManager iface_;        ///< Routing interface file I/O

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
    int next_event_ = 0;          ///< Index of next event in ctx_.events
    bool between_events_ = false; ///< legacy BetweenEvents, as the last stepRouting evaluated it
    /**
     * @brief Test the supplied date against the next configured routing event.
     *
     * Uses next_event_ without advancing it. Caller preserves sorted/valid event data.
     *
     * @param[in] current_date Absolute SWMM DateTime, decimal days relative to 1899-12-30.
     * @return False without events; true before/after the current event or after all events, false
     *     inside its inclusive boundaries.
     */
    bool isBetweenEvents(double current_date) const; ///< Check if between routing events
    /**
     * @brief Determine whether current routing can be skipped under steady-state tolerances.
     *
     * Compares relative lateral/terminal inflow changes using configured tolerances and internal cfs
     * tiny threshold. Does not advance any state.
     *
     * @param[in] action_count Number of control actions; a positive value prevents a steady
     *     classification.
     * @return True only after initial time, with skip_steady_state enabled, no active 2D router,
     *     acceptable prior flow error and unchanged node inflows.
     */
    bool isInSteadyState(int action_count) const; ///< Check if system is in steady state
    std::vector<gage::GageState> gage_states_;    ///< Per-gage state (SoA)

#ifdef OPENSWMM_HAS_2D
    twoD::SurfaceRouter2D surface_router_; ///< Optional 2D surface routing solver
    /// S4b: per-row unit labels for the 2D species snapshot (pollutant units,
    /// MSX units, "hours", "degC"), rebuilt by fillSurfaceSnapshot() and
    /// pointed to by SimulationSnapshot::surface_species_units.
    mutable std::vector<std::string> surface_species_units_;
    /// Non-owning pointer to the 2D HDF5 output plugin (lifetime owned by
    /// PluginFactory's output_plugins_). Set in open() when [2D_OPTIONS]
    /// OUTPUT_FILE is configured; used in start() to call prepareMeshAndDatasets
    /// once the mesh is built.
    twoD::Default2DOutputPlugin* surface_output_plugin_ = nullptr;
    bool checkpoint_surface_output_ = false;
    bool checkpoint_file_outputs_ = false;
    twoD::RuntimeWaterDriverCheckpoint* runtime_water_driver_ = nullptr;

    /**
     * @brief Wire context serialization views to owned 2D router components.
     *
     * Sets nonowning mesh/options/boundary/pending-row/infiltration/groundwater/quality/router
     * pointers. Constructor invokes once; stable nonmovable engine identity and reset-preserved views
     * keep them valid until engine destruction. No allocation or ownership transfer.
     */
    void wire2DModelIO() noexcept;
#endif

    std::string rpt_path_; ///< Report file path
    std::string out_path_; ///< Binary output file path

    bool step_active_ = false, runtime_pipeline_ = false;
    struct RestartState;
    bool checkpoint_enabled_ = false, checkpoint_ready_ = false;
    bool checkpoint_output_ = false;
    DefaultOutputPlugin* binary_output_plugin_ = nullptr; // owned by PluginFactory
    std::string checkpoint_model_;
    std::string pending_hotstart_checkpoint_;
    std::string hotstart_checkpoint_error_;
    /**
     * @brief Attach native restart resources at the initial RUNNING boundary.
     *
     * Requires time zero and no existing checkpoint. With an active surface, owns initialized
     * common-source/stage resources; otherwise enables numerical checkpointing. Does not write a
     * hotstart file itself.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string prepareNativeHotstartCheckpointing();
    /**
     * @brief Reject a session whose execution/output/provider profile is not restart-qualified.
     *
     * Reads engine and participant state, requiring RUNNING/error-free routing and prepared output mode
     * plus operation-specific supported components/inputs.
     * @throws std::runtime_error On any
     * unsupported profile invariant.
     *
     * @param[in] attaching_surface True selects profile checks while a surface attachment is being
     *     established; false validates an existing attachment.
     */
    void checkCheckpointProfile(bool attaching_surface = false) const;
    /**
     * @brief Qualify and attach the initial checkpoint profile and optional source ownership.
     *
     * Requires inactive pipeline/target, no previous attachment and time zero. Validates supported
     * model/physical state and snapshots canonical model identity before marking enabled/ready.
     * Temporary router flags are restored on handled refusal where applicable; no checkpoint file is
     * written.
     *
     * @if OPENSWMM_HAS_2D
     * @param[in,out] stage 2D build only: optional borrowed matching CommonSourceStage; NULL selects no
     *     common-stage attachment. Caller keeps any stage alive through session end.
     * @param[in] own_sources 2D build only: true requires explicit catchment graphs and initializes a
     *     delivery bridge; false accepts only mesh-only common stages.
     * @endif
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string enableCheckpointingImpl(
#ifdef OPENSWMM_HAS_2D
        twoD::CommonSourceStage* stage, bool own_sources = false
#endif
    );
    /**
     * @brief Serialize the canonical model/compiled-input identity for restart compatibility.
     *
     * Captures supported configuration and resolved caches without advancing clocks;
     * allocation/validation exceptions propagate. Later model changes are detected by byte comparison
     * against this attachment identity.
     * @return Owned same-platform model-identity bytes, not a user-readable hash or portable exchange
     *     format.
     */
    std::string checkpointModel() const;
    /**
     * @brief Validate an accepted attachment, unchanged model and consistent restart inventory.
     *
     * Requires enabled/ready profile, matching canonical model, valid restart state and qualified
     * surface/source boundaries. Does not publish a checkpoint.
     * @throws std::runtime_error On
     * readiness, compatibility or physical/state refusal.
     */
    void checkCheckpointBoundary() const;
    /**
     * @brief Capture a complete checked engine envelope into owned output bytes.
     *
     * Requires checkCheckpointBoundary(). Captures engine/router/source/driver state using
     * version/profile-specific native encoding and checksum; enforces maxBytes. Flushing output is an
     * observable IO side effect even when later envelope construction fails.
     *
     * @param[out] result Caller-owned byte string replaced only after complete successful envelope
     *     construction; preserved on refusal.
     * @param[in] include_outputs True includes qualified output continuation state and flushes IO;
     *     false captures physical/native-hotstart state only.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string checkpointBytes(std::string& result, bool include_outputs = true) const;
    /**
     * @brief Capture and publish a checked boundary with optional source/output contracts.
     *
     * Refuses active pipeline/target. File-backed profiles delegate their companion-aware save; other
     * profiles serialize checkpoint bytes and source-stream contract when attached, then publish.
     * Source-history/stream state may be brought to the accepted boundary before publication.
     *
     * @param[in] path Borrowed checkpoint/native-hotstart filename; synchronous file operations can be
     *     substantial.
     * @param[in] exclusive True requires a new destination and available no-replace publication; false
     *     permits replacement policy.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string saveCheckpointImpl(const std::string& path, bool exclusive) const;
    /**
     * @brief Validate and optionally adopt a physical engine envelope and companion bindings.
     *
     * Requires fresh accepted time-zero engine session, compatible model and participant profile;
     * checks envelope/version/checksum, restored state and companion eligibility before normal
     * adoption. Does not initialize missing external model/provider objects.
     *
     * @param[in] checkpoint Owned serialized physical checkpoint bytes consumed during decode; not a
     *     filename.
     * @param[in] stream_contract Borrowed source-stream continuation contract; empty for an unbound
     *     numerical checkpoint.
     * @param[in] binary_file Borrowed binary-output companion contract/token, empty for no file
     *     profile.
     * @param[in] mesh_file Borrowed mesh-output companion contract/token, empty for no file profile.
     * @param[in] whole_binding Expected whole-file envelope fingerprint, zero for non-file-bound
     *     profiles.
     * @param[out] inspection Optional metadata output selects validation-only mode; NULL installs a
     *     valid restored state.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string loadCheckpointBytes(std::string checkpoint, const std::string& stream_contract = {},
                                    const std::string& binary_file = {},
                                    const std::string& mesh_file = {}, uint64_t whole_binding = 0,
                                    CheckpointInspection* inspection = nullptr);
    /**
     * @brief Decode file/source wrappers and dispatch checkpoint validation or restore.
     *
     * Reads bounded file bytes, resolves supported file-bundle/source-stream envelopes, verifies
     * wrapper checksums/bindings and calls loadCheckpointBytes(). File/source support depends on build
     * capabilities.
     *
     * @param[in] path Borrowed checkpoint/native-hotstart filename; synchronous file operations can be
     *     substantial.
     * @param[out] inspection Optional metadata destination selects inspection mode; NULL requests
     *     actual adoption. Facade inspectCheckpoint() uses a temporary candidate to preserve caller
     *     output on refusal.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string loadCheckpointPath(const std::string& path, CheckpointInspection* inspection);
#ifdef OPENSWMM_HAS_2D
    /**
     * @brief Publish physical checkpoint plus retained binary/mesh file companions.
     *
     * Requires selected combined file profile and source journal. Flushes IO, snapshots physical state,
     * captures source-stream boundary when needed, creates owned output-prefix copies and binds
     * their/source fingerprints into the envelope. Retains companions only after successful
     * publication; staged copies clean up on handled refusal. HDF5 output support is required.
     *
     * @param[in] path Borrowed checkpoint destination; companion filenames append .binary and .mesh.
     * @param[in] exclusive True requests no-replace checkpoint publication; false uses replacement
     *     policy.
     * @return Empty string on success; otherwise an owned refusal/IO/compatibility diagnostic.
     */
    std::string saveFileOutputCheckpoint(const std::string& path, bool exclusive) const;
#endif
    /**
     * @brief Borrow the explicit source-group ownership map when a delivery bridge exists.
     *
     * Do not retain past participant detachment, close or engine destruction.
     * @return Bridge-owned sourceGroups() pointer, or nullptr without bridge/2D support.
     */
    const std::vector<int>* ordinarySourceOwners() const noexcept;
    /**
     * @brief Test whether an ordinary subcatchment source has a private nonnegative owner.
     *
     * @param[in] source Zero-based ordinary source index; invalid/out-of-range indices yield false.
     * @return True when the ownership map exists and its selected entry is nonnegative.
     */
    bool ownsSpatialSource(int source) const noexcept;
#ifdef OPENSWMM_HAS_2D
    friend class twoD::EngineSourceResultHistory;
    friend class twoD::EngineSourceResultStream;
    twoD::EngineSourceResultStream* engine_source_stream_ = nullptr;
    twoD::EngineSourceResultHistory* engine_source_history_ = nullptr;
    /**
     * @brief Clear borrowed engine/marcher recorder links before participants can be destroyed.
     *
     * Unbinds CPU history/stream pointers, clears stream.engine_, nulls engine recorder pointers and
     * disables file-output profile. Does not delete caller-owned recorder objects or erase their
     * captured history.
     */
    void detachEngineSourceHistory() noexcept;
    std::unique_ptr<runoff::SourceWaterDeliveryBridge> source_delivery_;
    twoD::CommonSourceStage* source_stage_ = nullptr; // caller keeps the stage alive
    std::array<double, 6> source_totals_{}; // rain, ET, infiltration, return, runoff, drain (ft3)
    /**
     * @brief Compute cumulative private-source water totals in reporting order.
     *
     * Includes snow precipitation/liquid/sublimation adjustments and floors computed rainfall at zero
     * for cancellation roundoff. Does not mutate ledgers.
     *
     * @param[in] source Borrowed initialized source driver aligned with model subcatchments and owning
     *     snow state.
     * @param[in] bridge Borrowed delivery bridge selecting owned subcatchments and accumulated receiver
     *     receipts.
     * @return Array of cumulative internal ft3: rainfall, evaporation, infiltration, aquifer+outfall
     *     return, delivered runoff, delivered LID drain.
     */
    std::array<double, 6> sourceTotals(const runoff::SourceWaterDriver& source,
                                       const runoff::SourceWaterDeliveryBridge& bridge) const;
    /**
     * @brief Sum owned source water storage and unsettled group volumes.
     *
     * No range/empty-group checks; caller supplies validated group membership.
     *
     * @param[in] source Borrowed initialized driver whose clock groups are nonempty and valid; each
     *     group must contain a subcatchment.
     * @return Internal cubic feet over each selected subcatchment storedVolume plus one pendingVolume
     *     per group.
     */
    double spatialSourceStorage(const runoff::SourceWaterDriver& source) const;
#endif
    double exchange_target_ = -1.0;

    // Runoff clock (matching legacy OldRunoffTime / NewRunoffTime)
    // Runoff advances on its own timestep (300 sec wet, 3600 sec dry);
    // lateral flows are linearly interpolated between runoff boundaries.
    double old_runoff_time_ = 0.0; ///< Previous runoff boundary (seconds from start)
    double new_runoff_time_ = 0.0; ///< Next runoff boundary (seconds from start)
    // PARITY: millisecond mirrors of the runoff clock, accumulated with the
    // exact legacy ops (runoff.c:229-237 — NewRunoffTime += 1000*step, clamp
    // to TotalDuration ms). The wet-weather/GW interpolation weight f
    // (routing.c:703) and the report-instant runoff weight (output.c) must be
    // formed from MILLISECOND quantities to round identically to legacy.
    double old_runoff_ms_ = 0.0; ///< legacy OldRunoffTime (msec)
    double prev_runoff_step_sec_ =
        0.0;                     ///< span of the runoff step just completed (legacy oldRunoffStep)
    double new_runoff_ms_ = 0.0; ///< legacy NewRunoffTime (msec)
    double new_rule_time_ms_ = 0.0; ///< legacy NewRuleTime (msec, routing.c:58)

    // Previous cumulative LID exfiltration / evaporation volumes (ft³), so the
    // per-step delta can be folded into the runoff-continuity infil / evap
    // outflow terms once per runoff step (issue #102 — legacy VlidInfil/VlidEvap).
    double prev_lid_infil_vol_ = 0.0;
    double prev_lid_evap_vol_ = 0.0;

    // PARITY: per-subcatchment interpolated wet-weather / GW inflows saved by
    // the Phase-2 interpolation so assembleLateralInflows() can replay
    // legacy's exact per-node accumulation ORDER — routing.c:466-473 adds
    // ext, dwf, then PER-SUBCATCHMENT wet-weather q (routing.c:716), then GW,
    // RDII, iface into Node.newLatFlow. Pre-summing per node and adding in a
    // different source order rounds differently (1-ULP lat-flow drift).
    std::vector<double>
        wet_q_interp_; ///< per-subcatch interpolated runoff (cfs; NOT runon — it is already inside runoff[])
    std::vector<double>
        lid_drain_q_interp_; ///< per-node interpolated LID drain inflow this routing step (cfs; legacy lid_addDrainInflow)
    /// The same drain flows PER UNIT as (node, q), in legacy's order
    /// (subcatchments by index, each one's lidList). Legacy adds each unit's q
    /// to Node.newLatFlow individually; adding the per-node sum rounds
    /// differently once two units drain to one node.
    std::vector<std::pair<int, double>> lid_drain_q_units_;
    std::vector<double> gw_q_interp_; ///< per-subcatch interpolated GW flow (cfs)
    std::vector<int> gw_q_node_;      ///< receiving node for gw_q_interp_ (-1 = skip)
    // Legacy lid_getRunoff volume accumulators, per subcatchment per runoff
    // step: VlidIn (= sum over units of (captured/area)*area*tStep, ft3) and
    // qRunoff (= sum of unit surface outflow*area, cfs; *tStep = VlidOut).
    // subcatch.c:746-751 rebuilds newRunoff from these in the VOLUME domain;
    // adjusting the runoff RATE incrementally instead rounds differently
    // (1-ULP runoff drift on every LID deck).
    std::vector<double> lid_vlidin_vol_; ///< per-subcatch VlidIn this runoff step (ft3)
    std::vector<double> lid_qsurf_cfs_;  ///< per-subcatch LID surface outflow to outlet (cfs)
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
    bool has_runoff_ = false; ///< Prev step generated runoff (legacy HasRunoff)
    bool has_snow_ = false;   ///< Prev step had snow cover   (legacy HasSnow)

    EngineCallbacks callbacks_; ///< Registered callback bundle
    int save_results_ = 0;      ///< Whether to save binary results

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

        int n_steps = 0; ///< Number of routing steps accumulated

        /**
         * @brief Allocate zeroed display-unit report-sum arrays and reset sample count.
         *
         * Replaces all accumulator vectors and n_steps; allocation failure can leave a partially resized
         * set.
         *
         * @param[in] n_nodes Nonnegative node count; caller validates before conversion to size_t.
         * @param[in] n_links Nonnegative link count; caller validates before conversion to size_t.
         */
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

        /**
         * @brief Zero existing report-sum arrays and sample count without resizing.
         *
         * Preserves capacities/dimensions; caller prevents concurrent accumulation.
         */
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

    AvgAccumulator avg_; ///< Averaging accumulator (only used when rpt_averages == true)

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
    std::uint64_t xsp_cache_gen_ = ~0ULL; ///< generation the cache was built at

    /** @brief Rebuild xsp_cache_ if links/xsect state changed (cheap check). */
    void ensureXspCache() noexcept;

    /**
     * @brief Compute current node volume using the reporting convention.
     *
     * Delegates the explicit-depth/volume overload with current state.
     *
     * @param[in] i Caller-validated node index in aligned state arrays.
     * @return Reported current volume in internal cubic feet, including retained storage-LID water.
     */
    double reportedNodeVolume(int i) const noexcept;

    /**
     * @brief Map a node state into report/mass-balance volume convention.
     *
     * @param[in] i Caller-validated node index.
     * @param[in] depth Node depth above invert, internal feet.
     * @param[in] volume Mobile node water volume, internal cubic feet.
     * @return Cubic feet: storage volume plus held LID water; KINWAVE/STEADY volume; ponded/coupled
     *     overflow volume when applicable; otherwise rpt_full_volume*depth/full_depth, or zero for
     *     nonpositive full depth.
     */
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
    void stepRunoff(double dt_routing);

    /**
     * @brief Advance climate evaporation-change scheduling when the current boundary is due.
     *
     * Preserves a future next_evap_date. CONSTANT/default schedules one year ahead, MONTHLY the next
     * month, PAN next midnight; TIMESERIES advances its cursor to the next qualifying row within run
     * end and copies its rate. Caller supplies initialized climate/table state.
     *
     * @param[in] the_date Absolute SWMM calendar DateTime in decimal days.
     */
    void setNextEvapDate(double the_date) noexcept; ///< legacy climate.c setNextEvapDate
    /**
     * @brief Select wet/dry runoff duration aligned to climate and rain boundaries.
     *
     * @param[in] abs_time Current absolute SWMM DateTime, decimal days.
     * @param[in] is_raining Whether current gage rain is active.
     * @param[in] has_runoff Previous runoff-step activity flag used to retain wet stepping during
     *     recession.
     * @param[in] has_snow Previous snow-cover flag used in wet/dry timestep selection.
     * @return Selected runoff step in seconds, shortened for next evaporation/rain-gage changes.
     */
    double computeRunoffTimestep(double abs_time, bool is_raining, bool has_runoff,
                                 bool has_snow) noexcept;

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
     * @brief Book surface constituent transfers using completed post-LID volumes.
     *
     * Builds per-subcatchment LID infiltration volumes in ft3 then updates post-LID washoff balances.
     * No-op for ignored quality or absent applicable constituents/subcatchments. Does not recompute the
     * hydraulic LID solve.
     *
     * @param[in] dt_runoff Runoff substep duration in seconds; supplied to pollutant/reaction-species
     *     surface bookkeeping.
     */
    void bookWashoffLoads(double dt_runoff) noexcept;

    /**
     * @brief Execute groundwater computation for one substep.
     *
     * @param dt_runoff  Runoff substep duration (seconds).
     */
    void stepGroundwater(double dt_runoff) noexcept;

    /** @brief Assemble the run-on for the runoff step about to be taken —
     *  outfall return (over the previous step's span), upstream runoff and
     *  LID drains — into subcatches.runon_rate[] (ft/s, legacy Subcatch.runon)
     *  and runon_inflow[] (CFS). Legacy runoff.c:247-253 order. */
    void assembleRunon() noexcept;

    /**
     * @brief Compute groundwater receiving-head and available-flow inputs from current node state.
     *
     * Uses configured GW node or runoff outlet fallback, aquifer bottom and optional fixed receiving
     * depth. Writes per-subcatchment head in internal feet and availability in internal flow units;
     * caller has initialized GW stores.
     *
     * @param[in] dt_runoff Runoff substep duration in seconds, used for available node-volume-to-flow
     *     conversion.
     */
    void assembleGWCoupling(double dt_runoff) noexcept;

    /** @brief Assemble all decomposed inflow sources into nodes.lat_flow and compute step mass balance.
     *  @param dt_routing  Current routing step (s); used to re-derive the 1D↔2D
     *                     coupling rate from the carried per-window exchange volume. */
    void assembleLateralInflows(double dt_routing) noexcept;

    /**
     * @brief Evaluate controls/inflows and advance hydraulic/quality routing.
     *
     * Updates structures, inlet capture, exfiltration and transport using the current decomposed
     * lateral sources; internal helper does not perform public handle/lifecycle validation.
     *
     * @param[in] dt_routing Routing step duration in seconds; caller establishes a valid running solver
     *     state.
     */
    void stepRouting(double dt_routing);

    /**
     * @brief Update node and link statistics after routing.
     *
     * @param dt_routing  Routing timestep (seconds).
     */
    void updateStatistics(double dt_routing) noexcept;

    /**
     * @brief Integrate per-node routed inflow/outflow over a half step.
     *
     * Called before/after routing to accumulate trapezoidal totals in internal cubic feet, including
     * between-event intervals. Caller has initialized routed node stores.
     *
     * @param[in] half_step Nonnegative duration in seconds, normally half the routing timestep.
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
     * @brief Copy initialized runoff/soil/GW/snow/quality state into legacy hotstart field order.
     *
     * Uses internal legacy-compatible units and aligned initialized solver state. No public
     * lifecycle/array guard or portability conversion; allocation exceptions propagate.
     * @return Owned double vector with layout/size specified by HotStartManager::legacy_runoff_size().
     */
    std::vector<double> legacyRunoffSection() const;
    /**
     * @brief Serialize named subcatchment surface state for native hotstart records.
     *
     * Includes constituent/landuse/species identity and supported initialized surface/runoff/GW data.
     * Uses native scalar representation and internal units; caller prepares aligned module state.
     * @return Owned tagged native bytes, empty for zero subcatchments.
     */
    std::string nativeSurfaceState() const;
    /**
     * @brief Validate or apply named native subcatchment surface-state bytes.
     *
     * Maps saved names into current pollutant/landuse/species/subcatchment identities, skipping
     * unsupported/missing named rows with warnings in apply mode. Does not create missing model
     * objects. Caller uses validation before application when a malformed late row must not leave
     * partial changes.
     *
     * @param[in] blob Borrowed tagged native surface-state record; unknown/malformed layout is refused.
     * @param[in] apply False performs mapping/decode validation without storing rows or warnings; true
     *     applies matching rows and appends skipped-name warnings.
     * @return Empty string on success; otherwise an owned refusal/IO/validation diagnostic.
     */
    std::string applyNativeSurfaceState(const std::string& blob, bool apply);
    /**
     * @brief Apply pending legacy runoff hotstart state to initialized modules.
     *
     * Requires exact pending vector length; mismatch clears pending bytes and returns. Applies runoff
     * depths/infiltration/GW/snow/quality and restates initial buildup. Caller has initialized aligned
     * module state; no public handle/lifecycle checks.
     */
    void applyLegacyRunoffSection() noexcept;

    /**
     * @brief Copy/interpolate a due report snapshot and enqueue asynchronous writing.
     *
     * Uses report timers/current accepted state, honors report-start gate and advances output
     * scheduling. Snapshot owns copied arrays; IO completion/failure is managed separately. Does not
     * synchronously guarantee bytes reached disk.
     *
     * @param[in] dt_step Legacy routing-step-duration argument; unused by this implementation.
     */
    void postOutputSnapshot(double dt_step) noexcept;

    /**
     * @brief Write one authored SAVE HOTSTART entry from the current state.
     *
     * AUTO selects native for assigned node LIDs or heat, otherwise legacy. Converts normal save
     * refusals into warnings; full-engine checkpoint capture is used for qualified heat/native restart.
     * Filesystem/allocation exceptions can propagate from this internal helper.
     *
     * @param[in] entry Borrowed authored format/path/date row; resolved path is preferred, original
     *     token fallback, empty path is a no-op.
     */
    void writeHotstartSave(const HotstartSaveEntry& entry);

    /**
     * @brief Attempt each dated hotstart save once when the accepted clock reaches its date.
     *
     * Uses elapsed milliseconds with a one-millisecond due tolerance. Marks a row done BEFORE calling
     * its save, so a failed save is not retried by later steps. Undated rows are skipped here.
     */
    void writeDueHotstartSaves();

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

    /*! Stop a routing solve that has gone non-physical (non-finite or absurd
     *  head/flow) instead of letting it run on and write NaN into the report.
     *  Returns SWMM_OK when the state is sane. */
    /**
     * @brief Stop grossly nonphysical FV/node/link routing state and record a fatal numerical error.
     *
     * Checks FV solver divergence and nonfinite/absolute>1e9 node head (ft) or link flow (cfs). This is
     * a runaway guard, not a convergence/continuity qualification.
     * @return SWMM_OK for accepted bounds; CFFI_ERR_NUMERICAL after appending a diagnostic and
     *     transitioning ERROR_STATE.
     */
    [[nodiscard]] int checkRoutingDiverged() noexcept;

    /**
     * @brief Write accumulated diagnostics when open fails before report-plugin preparation.
     *
     * No-op without a report path. Attempts the legacy failure-report echo without requiring a
     * successful engine start.
     */
    void write_open_failure_report() noexcept;

    /**
     * @brief Record a fatal context error and synchronously notify the warning callback.
     *
     * Sets ERROR_STATE then emit_warning(). No callback exception handling; callbacks must not throw.
     * noexcept also requires successful diagnostic string allocation.
     *
     * @param[in] code Stored fatal error code.
     * @param[in] message Borrowed NUL-terminated text or NULL; context stores an empty string for NULL,
     *     callback receives the original pointer.
     */
    void set_error(int code, const char* message) noexcept;

    /**
     * @brief Synchronously notify the registered warning/error sink.
     *
     * Does not append report warnings or change error state. Callback executes on the invoking thread
     * and must not throw/reenter unsafe engine operations.
     *
     * @param[in] code Warning or fatal-error code forwarded unchanged.
     * @param[in] message Borrowed message pointer valid for the call; may be NULL when the caller
     *     supplies NULL.
     */
    void emit_warning(int code, const char* message) noexcept;

    /**
     * @brief Append a report warning when memory permits and notify the callback.
     *
     * Suppresses allocation exceptions while appending the report list, but still invokes the callback
     * with borrowed message bytes. Callback must not throw.
     *
     * @param[in] message Borrowed text copied to ctx.warnings when allocation succeeds.
     * @param[in] code Callback warning code; forwarded unchanged.
     */
    void push_report_warning(const std::string& message, int code) noexcept;

    /** @brief Fire the progress callback (if registered). */
    void emit_progress() noexcept;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_SWMM_ENGINE_HPP */
