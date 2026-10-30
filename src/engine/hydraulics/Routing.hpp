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
 * @file      Routing.hpp
 * @brief     Top-level routing dispatcher — KW / DW / Steady-state.
 *
 * @details Orchestrates the routing step:
 *   1. Save old hydraulic states
 *   2. Initialize node flows (lateral inflows)
 *   3. Dispatch to KinematicWave or DynamicWave solver
 *   4. Update node/link final states
 *   5. Update mass balance accumulators
 *
 * @note Legacy reference: src/legacy/engine/routing.c, flowrout.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ROUTING_HPP
#define OPENSWMM_ROUTING_HPP

#include "XSectBatch.hpp"
#include "../quality/FvTransportWater.hpp"
#include "KinematicWave.hpp"
#include "DynamicWave.hpp"
#include "Divider.hpp"
#include "fv/FvOptions.hpp"
#include "fv/INetworkSolver.hpp"
#include "fv/NetworkMeshData.hpp"
#include <array>

#include <memory>
#include <string>
#include <functional>
#include <vector>

namespace openswmm {
namespace twoD {
class SurfaceRouter2D;
}

struct SimulationContext;
struct OutfallData;

// ============================================================================
// Routing model enum
// ============================================================================

enum class RouteModel : int {
    STEADY = 0,  ///< Steady-state (pass-through)
    KINWAVE = 1, ///< Kinematic wave
    DYNWAVE = 2, ///< Dynamic wave (St. Venant, implicit Picard)
    FV = 3       ///< Explicit conservative finite volume (Godunov)
};

/**
 * @brief Refresh routing lengths and apply dynamic-wave/FV Courant lengthening.
 *
 * Uses true floodplain length for irregular sections and effective Manning n/force-main
 * conventions. Recomputes factor from stored base values; when lengthened adjusts slope/n-derived
 * beta, rough_factor, q_full/q_max and FORCE_MAIN s_bot. Call before initial normal depths and
 * solver initialization. Does not rewrite authored lengths; caller requires aligned valid subtype
 * rows and finite positive geometry/roughness. Allocation may throw after earlier length updates;
 * no rollback.
 *
 * @param[in,out] ctx Borrowed resolved native geometry/parameters; updates conduit true/mod
 *     lengths, selected conveyance terms and force-main roughness factor.
 * @param[in] model DYNWAVE/FV enable positive LENGTHENING_STEP; other route models retain authored
 *     mod_length.
 */
void applyConduitLengthening(SimulationContext& ctx, RouteModel model);

// ============================================================================
// Routing orchestrator
// ============================================================================

/**
 * @brief Top-level routing orchestrator.
 *
 * @details Owns the XSectGroups (shape-grouped batch xsect), KWSolver, and
 *          DWSolver. Initialised once after the model is built; then
 *          `step()` is called each routing timestep.
 */
class Router {
public:
    /// Project classic restart volumes/flows onto the existing FV mesh once.
    /// Preserves conduit inventory; does not restore subgrid transient history.
    void restoreFvClassicState(SimulationContext& ctx);
    /**
     * @brief Initialize routing groups, solver state and the selected model dispatch.
     *
     * Resets checkpoint attachment, builds complete scalar/grouped native geometry with borrowed stable
     * profile tables, applies conduit lengthening and initializes model-specific work. KW/STEADY use
     * topological order/cycle diagnostics; DW configures iteration/surcharge/UF state; FV builds its
     * own mesh/backend. Allocations/solver failures can leave partially initialized context/router. FV
     * mesh validation diagnostics are retained for the caller to inspect; init does not itself turn
     * every report error into an exception. Reinitialize after topology/model edits and serialize
     * execution on an instance.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] model Requested STEADY, KINWAVE, DYNWAVE or FV route model; must agree with relevant
     *     ctx.options routing settings.
     */
    void init(SimulationContext& ctx, RouteModel model);

    /**
     * @brief Dispatch one prepared hydraulic routing interval and publish its current state.
     *
     * Caller snapshots old node/link state and assembles lateral/external forcing before entry; this
     * routine deliberately does not save old state a second time. Initializes node/storage loss flows,
     * evaluates non-DW conduit losses/outfall boundaries, executes the model and publishes final
     * states. FV preserves signed piezometric head. Caller advances clocks, finishes LID/postprocessing
     * and updates global mass/report ledgers. Tracked checkpoints require beginCheckpointStep
     * beforehand and acceptance afterward. General failures/nonconvergence do not roll back ordinary
     * routing state; inspect lastStepConverged and upstream error handling.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] dt Positive finite routing interval in seconds, consistent with saved old state and
     *     forcing.
     * @param[in] evap_rate Finite nonnegative evaporation rate in native ft/s.
     * @param[in] non_conduit_fn Optional synchronous structure callback(ctx,dt,Picard index). DW
     *     invokes per Picard iteration; FV invokes initially and optionally each substep with index
     *     zero. Must not throw/reenter, change topology or retain transient borrowed state.
     * @return DW Picard iteration count, KW effort, one for steady flow, or FV internal operator-step
     *     count; zero can indicate an absent FV solver.
     */
    /// Publish an accepted native CPU FV water graph for conservative quality.
    void acceptedQualityWater(const SimulationContext& ctx, double dt, quality::FvTransportWater& out) const;

    int step(SimulationContext& ctx, double dt, double evap_rate = 0.0,
             dynwave::DWSolver::NonConduitFlowFunc non_conduit_fn = nullptr);

    /**
     * @brief Attach the internal accepted-boundary hydraulic checkpoint profile at initial time.
     *
     * Requires no prior attached context, validates supported model/state and stores serialized model
     * identity, then marks time-zero boundary accepted. Ordinary standalone scope is native DW
     * water-only conduits, ordinary junctions/non-tabular storage including initialized node LIDs, and
     * FREE/NORMAL/FIXED outfalls. Engine-owned sessions can qualify paired
     * structure/control/quality/heat or aquifer state via their separate ownership flags. Unsupported
     * profile throws; external producers/writers require their paired state. Attachment requires the
     * context address/lifetime to remain stable; init resets it.
     *
     * @param[in,out] ctx Initialized matching router/LID context with current_time exactly zero,
     *     retained by nonowning identity for the checkpoint session.
     */
    void enableCheckpointing(SimulationContext& ctx);
    /**
     * @brief Reserve the next chronological tracked routing boundary before preparation mutates state.
     *
     * Revalidates supported profile and exact serialized model identity. Sets scheduled end, clears
     * ready/finished and marks prepared before old-state/LID/forcing preparation. A later preparation
     * exception therefore cannot leave a falsely saveable boundary. Throws on mismatched
     * identity/time/model/limits; there is no automatic rollback here.
     *
     * @param[in,out] ctx Same attached context at its currently accepted time.
     * @param[in] dt Finite positive seconds with a representably later finite endpoint within
     *     configured duration.
     */
    void beginCheckpointStep(SimulationContext& ctx, double dt);
    /**
     * @brief Validate and accept a fully postprocessed tracked step at its advanced clock.
     *
     * Requires step_finished and not-ready, validates profile/model and complete RestartState
     * invariants, then marks the boundary saveable and updates accepted time. A DW return with Picard
     * nonconvergence is still an accepted legacy interval. Throws on any mismatch and leaves an
     * unaccepted boundary; caller owns all paired producer/clock state.
     *
     * @param[in,out] ctx Same attached context after successful Router::step, LID finish/postprocessing
     *     and caller clock advance to scheduled end.
     */
    void acceptCheckpointBoundary(SimulationContext& ctx);
    /**
     * @brief Serialize and publish an accepted internal hydraulic checkpoint file.
     *
     * Writes version 5 framed state/model/checksum bytes with a 64-MiB limit. Existing targets must
     * have hydraulic magic before replacement; stages bytes then renames to publish. No fsync
     * durability guarantee or Windows UTF-8/long-path adaptation is supplied. This component file does
     * not replace paired runoff/control/2D/output checkpoints.
     *
     * @param[in] ctx Same attached context at an accepted boundary, unchanged model/profile and valid
     *     current state.
     * @param[in] path Nonempty native narrow filesystem path; temporary exclusive staging is placed
     *     alongside the target.
     * @return Empty string on success, otherwise a caught std::exception diagnostic; no hydraulic state
     *     changes.
     */
    std::string saveCheckpoint(const SimulationContext& ctx, const std::string& path) const;
    /**
     * @brief Validate a stored hydraulic checkpoint privately, then install its selected state.
     *
     * Reads a bounded complete file, validates framing/version/model/checksum and native invariants
     * before no-throw installation. Accepts supported versions 3..5 subject to missing
     * thermal/ground-contact-state restrictions. Validation/read failure preserves live hydraulic
     * state. Restore may recover an unaccepted boundary; no current-ready requirement is imposed.
     * External paired session state is the caller responsibility.
     *
     * @param[in,out] ctx Same checkpoint-enabled matching context/model; installation replaces selected
     *     native runtime state, clocks and checkpoint readiness.
     * @param[in] path Native narrow readable checkpoint filename.
     * @return Empty on success, otherwise caught std::exception diagnostic.
     */
    std::string loadCheckpoint(SimulationContext& ctx, const std::string& path);

    /**
     * @brief Select an outer routing interval from the active model telemetry.
     *
     * A model with no links always returns fixed_step. DW may update critical-element counters and
     * first-call minimum-step state. FV subcycles internally, so this is a forcing/reporting cadence
     * hint rather than its actual stability timestep. Requires successful matching init.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] fixed_step Positive nominal interval in seconds.
     * @param[in] courant Positive variable-step factor or <=0 for fixed cadence.
     * @return Seconds: DW CFL selection, FV min(fixed_step,max(fixed_step*courant,suggested_substep))
     *     when enabled, otherwise fixed_step.
     */
    double getAdaptiveStep(SimulationContext& ctx, double fixed_step, double courant);

    /// Access the shape-grouped xsect manager.
    const XSectGroups& xsectGroups() const { return groups_; }

    /**
     * @brief Report the topological cycle diagnostic from the most recent initialization.
     * @return True if KW/STEADY topological ordering detected a cycle; DW does not use this tree-order
     *     check.
     */
    bool hasCycle() const { return cycle_detected_; }

    /**
     * @brief Configure the owned dynamic-wave solver worker count.
     *
     * Delegates to DWSolver::setNumThreads. Configure outside an active routing call; affects DW loops,
     * not the FV backend thread policy.
     *
     * @param[in] n Requested count; zero selects its automatic policy and environment overrides may
     *     apply.
     * @param[in,out] warnings Optional vector receiving policy warnings, or nullptr.
     */
    void setDWNumThreads(int n, std::vector<std::string>* warnings = nullptr) {
        dw_solver_.setNumThreads(n, warnings);
    }

    /// Access the DW solver (for non-conduit node state scatter).
    dynwave::DWSolver& dwSolver() { return dw_solver_; }

    /**
     * @brief Attach the borrowed structure-equation provider for tree routing.
     *
     * Must be set before KW/STEADY stepping when structures are present. DW/FV receive their structure
     * work through the callback instead. Provider/model lifetime must cover all routing calls;
     * replacing it does not release its memory. Checkpoint qualification accounts for enclosing engine
     * ownership separately.
     *
     * @param[in] s Live initialized StructureSolver or nullptr; never deleted by Router.
     */
    void setStructureSolver(hydstruct::StructureSolver* s) { structures_ = s; }

    // Internal water-only conduit window: copy persistent DW history and batch
    // geometry without retaining pointers into the live router. Checkpoint and
    // external structure sessions are not part of this transaction profile.
    /**
     * @brief Create private DW numerical history/group scratch for a restricted internal routing trial.
     *
     * Requires initialized DYNWAVE outside a checkpoint session; otherwise throws runtime_error. Does
     * not copy external structure/control sessions or a full SimulationContext. Geometry profile table
     * references still borrow the underlying stable model storage; no pointer into the source router
     * own group object is retained. Allocations can throw without mutating the source. Caller validates
     * the restricted transaction profile and supplies a compatible private context.
     * @return Owning Router containing copied DW/group and selected link-step scratch, with its DW
     *     group pointer rebound to the copy.
     */
    std::unique_ptr<Router> privateDynamicWaveWindow() const;
    /**
     * @brief Exchange accepted private DW history and grouped scratch into this router.
     *
     * No-throw swaps only groups_ and dw_solver_, clears their bypass masks and rebinds each solver to
     * its own group object. The donor receives the previous numerical history. Does not publish context
     * state, checkpoint/session fields or every copied reporting buffer; caller owns the surrounding
     * transaction. No validity check is performed.
     *
     * @param[in,out] r Compatible private DW window created from this same initialized model; must be
     *     exclusively owned and validated before publication.
     */
    void publishDynamicWaveWindow(Router& r) noexcept;

    /**
     * @brief Report the selected routing model legacy convergence statistic.
     *
     * FV true means no Picard convergence loop is used, not a certificate against short
     * advance/divergence/backend failure. Those are checked by the FV advance wrapper/error path.
     * @return DW final Picard flag for DYNWAVE; true for every other model.
     */
    bool lastStepConverged() const {
        return (model_ == RouteModel::DYNWAVE) ? dw_solver_.lastConverged() : true;
    }

    /**
     * @brief Enable conduit-step reporting when the selected solver can provide it.
     *
     * Call after successful init. Stores nominal routing-step bin edges and, on native FV,
     * allocates/zeroes pending per-link sample buffers and installs a synchronous sink retaining this
     * Router address. Router must therefore remain at a stable address while the sink is active.
     * Allocation exceptions propagate; no hydraulic state changes.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @return True for DW or the native ExplicitFvSolver backend; false for KW/STEADY/unsupported FV
     *     backends.
     */
    bool enableLinkStepStats(SimulationContext& ctx);

    /// Whether enableLinkStepStats() succeeded.
    bool linkStepStatsEnabled() const { return lstep_enabled_; }

    /**
     * @brief Keep or discard the just-completed interval conduit-step report samples.
     *
     * No-op unless enabled. DW samples current final convergence/CFL state only in-window. FV merges
     * pending per-link min/max/time-weighted/bin totals when in-window, then clears them either way.
     * Invoke once per completed routing interval to avoid double-counting or mixing windows; no
     * hydraulic changes.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] dt Completed outer interval seconds; DW weights samples by this value.
     * @param[in] in_window True only within the active reporting/event window; false discards pending
     *     FV samples.
     */
    void accumulateLinkStepStats(SimulationContext& ctx, double dt, bool in_window);

    /**
     * @brief Borrow the currently owned FV backend interface.
     *
     * No ownership transfers. Read diagnostics only outside mutation/advance; caller checks successful
     * FV initialization before treating a non-null backend as usable.
     * @return Const backend pointer, possibly nullptr; valid until backend replacement or Router
     *     destruction.
     */
    const fv::INetworkSolver* fvSolver() const { return fv_solver_.get(); }

    /// The FV mesh built at init (empty unless FLOW_ROUTING FV).
    const fv::NetworkMeshData& fvMesh() const { return fv_mesh_; }

    /// Backend label chosen by the FV factory ("cpu (...)", "omp (...)", ...).
    const std::string& fvBackend() const { return fv_backend_; }

    /**
     * @brief Borrow accumulated FV initialization warnings.
     *
     * Messages/storage can change on subsequent initialization and expire at Router destruction. Caller
     * surfaces them through context/UI diagnostics; this accessor does not clear or throw them.
     * @return Const owned message vector; includes mesh/backend/inert-option diagnostics.
     */
    const std::vector<std::string>& fvWarnings() const { return fv_warnings_; }
    /**
     * @brief Borrow accumulated FV mesh initialization errors.
     *
     * Messages/storage can change on subsequent initialization and expire at Router destruction. Caller
     * surfaces them through context/UI diagnostics; this accessor does not clear or throw them.
     * @return Const owned error vector; nonempty means the built FV state was not initialized for
     *     stepping.
     */
    const std::vector<std::string>& fvErrors() const { return fv_errors_; }

private:
    friend class twoD::SurfaceRouter2D;
    friend class SWMMEngine;
    struct RestartState;
    /**
     * @brief Encode the current accepted hydraulic component boundary into framed bytes.
     *
     * Validates the supported profile and selected runtime invariants, archives model/payload plus
     * checksum and enforces the 64-MiB bound. Does not publish a file or mutate live hydraulic state.
     * Non-standard exceptions may propagate.
     *
     * @param[in] c Same attached accepted context with current_time equal to checkpoint_time and
     *     unchanged serialized model.
     * @param[out] result Receives complete version-5 checkpoint bytes only after serialization/size
     *     checks succeed.
     * @return Empty on success; otherwise the caught std::exception message and prior result is
     *     preserved.
     */
    std::string checkpointBytes(const SimulationContext& c, std::string& result) const;
    /**
     * @brief Prepare a fully validated replacement hydraulic state without publication.
     *
     * Throws on format/version/model/checksum/trailing-byte, dimensional, finite/physical or
     * joint-session mismatch. Runtime state is decoded into a private copy seeded from target geometry;
     * versions predating thermal/ground-contact receipts are rejected if those live fields need
     * restoration. No live state changes; callers validate other paired producers before
     * installCheckpoint.
     *
     * @param[in] c Same checkpoint-enabled target context with unchanged model/profile.
     * @param[in] bytes Owned framed checkpoint bytes, consumed by the reader; nonempty and at most 64
     *     MiB.
     * @param[in] boundary Optional exact shared boundary tuple {time seconds,date days,elapsed
     *     ms,old_elapsed ms,next_report ms,evap ft/s}, or nullptr.
     * @param[in] delivery Optional shared queued-delivery duration in seconds for coupled-node receipt
     *     validation, or nullptr.
     * @param[in] feedback Optional borrowed outfall 2D feedback for exact shape/wetness/head agreement,
     *     or nullptr.
     * @return Owning shared prepared state; no pointer into input bytes or live context data is
     *     retained by the decoded snapshot.
     */
    std::shared_ptr<RestartState>
    prepareCheckpointBytes(const SimulationContext& c, std::string bytes,
                           const std::array<double, 6>* boundary = nullptr,
                           const double* delivery = nullptr,
                           const OutfallData* feedback = nullptr) const;
    /**
     * @brief Validate queued node exchange against a paired delivery window.
     *
     * Throws unless arrays align, current coupling_volume is zero, queues are finite, unpaired nodes
     * have no queued/current exchange, and a zero delivery window has no queue. No state changes or
     * delivery occurs.
     *
     * @param[in] nodes Decoded node coupling queue/volume/inflow arrays to inspect.
     * @param[in] coupled Exact node-count 0/1 coupling mask.
     * @param[in] delivery Finite nonnegative shared delivery interval in seconds.
     */
    static void validateJointDelivery(const NodeData& nodes, const std::vector<uint8_t>& coupled,
                                      double delivery);
    /**
     * @brief Check decoded hydraulic water inventory against its enclosing engine session.
     *
     * Sums legacy reported node storage plus retained LID water and link volumes, using real ponded
     * volume above eligible rims. Throws unless the inventory matches expected within relative/absolute
     * 1e-11 tolerance; no mutation or publication.
     *
     * @param[in] s Non-null prepared runtime state with decoded node/link/storage arrays.
     * @param[in] c Matching immutable target model, coupling/ponding options and subtype joins.
     * @param[in] expected Finite enclosing continuity water inventory in native ft3.
     * @param[in] routed True additionally requires previous-water quality receipt shape for every LID
     *     cell stack.
     */
    static void validateSessionStorage(const std::shared_ptr<RestartState>& s,
                                       const SimulationContext& c, double expected, bool routed);
    /**
     * @brief Publish a previously validated replacement component state without throwing.
     *
     * Delegates to RestartState::install, moving selected runtime containers and clocks/readiness into
     * live storage. Preconditions are mandatory: no validation/null check is performed here. Other
     * owners/producers must install their paired accepted state as one session operation.
     *
     * @param[in,out] c Same target context/model used during preparation; no concurrent
     *     readers/writers.
     * @param[in] s Non-null owning prepared state, already checked against this router/context and all
     *     paired session constraints.
     */
    void installCheckpoint(SimulationContext& c, std::shared_ptr<RestartState> s) noexcept;
    SimulationContext* checkpoint_context_ = nullptr; // non-owning, stable lifetime
    bool checkpoint_owned_session_ = false;           // engine owns structure/control memory
    bool checkpoint_aquifer_session_ =
        false; // paired engine/surface recovery owns node-bed receipts
    std::string checkpoint_model_;
    double checkpoint_time_ = 0.0, checkpoint_step_end_ = 0.0;
    bool checkpoint_ready_ = false, checkpoint_prepared_ = false, checkpoint_step_finished_ = false;
    /**
     * @brief Serialize the immutable identity of the qualified hydraulic checkpoint model.
     *
     * Copies/archives the selected options, authored/resolved geometry/topology and qualified
     * material/provider layout. Allocations can throw; no hydraulic state mutation. Exact comparison
     * prevents restoration after unpaired model edits.
     *
     * @param[in] ctx Initialized matching context with coherent geometry/options/subtype/LID material
     *     arrays.
     * @return Owned binary model-identity bytes used for exact equality, not a human-readable model
     *     name.
     */
    std::string checkpointModel(const SimulationContext& ctx) const;
    /**
     * @brief Reject hydraulic features outside the currently owned checkpoint qualification.
     *
     * Requires DW solver/group/count consistency, supported ordinary nodes/storage/outfalls and
     * nonexternal section geometry. Structures/quality/heat/coupling/aquifer receipts require the
     * corresponding owning-session qualifications; divider/virtual/inlet nodes and tabular storage are
     * rejected. Validates initialized LID cell/material/thermal shapes and joins. Throws a descriptive
     * runtime error; does not repair or mutate model/state.
     *
     * @param[in] ctx Matching initialized context whose complete profile/topology/LID material shape is
     *     inspected.
     */
    void checkCheckpointProfile(const SimulationContext& ctx) const;
    /**
     * @brief Consume a prepared tracked boundary before the hydraulic step can mutate state.
     *
     * Requires prepared, unaccepted and unfinished timing state; external callbacks are not qualified.
     * Revalidates profile/model, then clears prepared so any subsequent exception remains unaccepted.
     * Does not advance time or accept/save a boundary. Throws on mismatch.
     *
     * @param[in,out] ctx Same attached context still at its prior accepted time and unchanged
     *     model/profile.
     * @param[in] dt Exact positive seconds scheduled by beginCheckpointStep.
     * @param[in] evap_rate Finite nonnegative evaporation in native ft/s.
     * @param[in] callback Whether a non-conduit callback is supplied.
     * @param[in] owned_callback True only for a trusted enclosing engine-owned callback; required when
     *     callback is present.
     */
    void checkCheckpointStep(SimulationContext& ctx, double dt, double evap_rate, bool callback,
                             bool owned_callback = false);
    /**
     * @brief Run the common routing step with explicit checkpoint callback ownership.
     *
     * Validates mesh-Richards qualification (native DW only), consumes tracked checkpoint preparation
     * before mutation, initializes flow/loss/outfall inputs, dispatches and updates non-FV final heads.
     * Marks pending mesh-Richards state routed and tracked step_finished only after successful
     * completion; neither advances the caller clock nor accepts the checkpoint boundary. Exceptions can
     * leave partial state and an unaccepted tracked boundary.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] dt Positive finite routing interval in seconds, consistent with saved old state and
     *     forcing.
     * @param[in] evap_rate Finite nonnegative evaporation rate in native ft/s.
     * @param[in] non_conduit_fn Optional synchronous structure callback(ctx,dt,Picard index). DW
     *     invokes per Picard iteration; FV invokes initially and optionally each substep with index
     *     zero. Must not throw/reenter, change topology or retain transient borrowed state.
     * @param[in] owned_callback True only when an enclosing engine session owns the callback/state
     *     included in its checkpoint profile.
     * @return Selected solver effort count as described by step.
     */
    int stepImpl(SimulationContext& ctx, double dt, double evap_rate,
                 dynwave::DWSolver::NonConduitFlowFunc non_conduit_fn, bool owned_callback);

    RouteModel model_ = RouteModel::DYNWAVE;
    XSectGroups groups_;

    /// Non-owning; set by SWMMEngine. Null in standalone Router unit tests,
    /// where structures then carry no flow.
    hydstruct::StructureSolver* structures_ = nullptr;

    /// Per-node "already converged this step" flag for storage units under
    /// STEADY (the KW solver keeps its own).
    std::vector<char> steady_storage_updated_;
    /// Per-link end-of-step flow depth under STEADY routing (uniform along
    /// the conduit); consumed by kinwave::finishRouting's node-depth raises.
    std::vector<double> steady_y_;

    // --- Explicit finite-volume solver (FLOW_ROUTING FV) -------------------
    // The mesh and state are owned here, not by the solver, exactly as
    // SurfaceRouter2D owns MeshData/SurfaceStateData: the solver is swappable
    // (CPU or Kokkos plugin) and must not own what survives a backend change.
    fv::NetworkMeshData fv_mesh_;
    fv::NetworkStateData fv_state_;
    fv::FvOptions fv_opts_; ///< internal units
    std::unique_ptr<fv::INetworkSolver> fv_solver_;
    std::string fv_backend_;
    std::vector<std::string> fv_warnings_, fv_errors_;

    // Per-conduit time step summary ([REPORT] LINK_STEPS). FV samples arrive
    // through the solver's sink during advance() and wait here, per link,
    // until accumulateLinkStepStats keeps or discards them.
    bool lstep_enabled_ = false;
    double lstep_rs_ = 0.0; ///< routing step (bin edges)
    std::vector<double> lstep_pmin_, lstep_pmax_, lstep_pdt_time_, lstep_ptime_, lstep_pbin_;
    /**
     * @brief Accumulate one native FV accepted-step sample into pending Router report storage.
     *
     * Updates pending per-link min/max, dt*time, time and histogram bins only. Requires preallocated
     * buffers and coherent conduit mapping; no validation, ownership transfer or hydraulic mutation.
     * The enclosing caller later keeps/discards this interval with accumulateLinkStepStats.
     *
     * @param[in] user Non-null stable Router pointer installed by enableLinkStepStats.
     * @param[in] conduit Valid FV conduit row; mapped negative global link index is skipped.
     * @param[in] dt Positive governing conduit step in seconds, capped to nominal routing interval.
     * @param[in] dur Nonnegative accepted duration weight in seconds.
     */
    static void fvStepSink(void* user, int conduit, double dt, double dur);

    // Per-step forcing buffers, allocated once at init.
    std::vector<double> fv_lateral_, fv_fixed_head_, fv_struct_flow_, fv_cond_loss_;

    /// Per-link cap on a DUMMY link's pass-through discharge: the control
    /// setting and FLOW_LIMIT, which the solver cannot see. Negative means
    /// unlimited; only DUMMY entries are ever read. Refreshed with the
    /// structures, because a control rule can close the link mid-step.
    std::vector<double> fv_link_q_cap_;

    /// Time integral of each structure's discharge over the routing step
    /// (ft³), and the elapsed time the last segment started at. Under
    /// per-substep coupling the discharge MOVES within the step, so the
    /// reported flow has to be the mean that was actually applied, not
    /// whichever substep happened to be last.
    std::vector<double> fv_struct_int_;
    double fv_struct_t_prev_ = 0.0;

    /**
     * @brief Build owned FV mesh/state, seed native history and initialize the selected backend.
     *
     * Converts display FV length/celerity/dispersion to ft units, records inert/DW-option warnings and
     * mesh build errors. On mesh errors returns with fv_errors populated before state/backend
     * initialization. Otherwise seeds aggregate barrel cell A/Q, virtual-junction/shoreline-consistent
     * depth and initial volume, selects a backend and binds it to owned mesh/state. Non-native backends
     * downgrade unsupported SUBSTEP structure callbacks with a warning. Allocates forcing buffers;
     * context initialization/reporting fields can change. Requires prior conduit lengthening;
     * exceptions propagate without rollback.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     */
    void initFv(SimulationContext& ctx);
    /**
     * @brief Assemble forcing, advance the FV backend and publish one routing interval.
     *
     * Subtracts node losses from lateral forcing, sets outfall fixed stages, evaluates structures/DUMMY
     * caps and converts aggregate conduit volumetric loss to area-loss rate per meshed length. Uses a
     * synchronous refresh hook only for native SUBSTEP coupling; control rules are not re-evaluated
     * here. advanceNetworkChecked requires the full [0,dt] interval and no reported divergence before
     * publishFv. A thrown backend/callback/check failure can leave partially advanced owned state or
     * interim context heads; no general rollback is supplied.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] dt Positive finite routing interval in seconds, consistent with saved old state and
     *     forcing.
     * @param[in] non_conduit_fn Optional synchronous structure callback(ctx,dt,Picard index). DW
     *     invokes per Picard iteration; FV invokes initially and optionally each substep with index
     *     zero. Must not throw/reenter, change topology or retain transient borrowed state.
     * @return Backend last_num_steps narrowed to int, or zero when no FV solver exists.
     */
    int stepFv(SimulationContext& ctx, double dt,
               dynwave::DWSolver::NonConduitFlowFunc non_conduit_fn);
    /**
     * @brief Map accepted FV state/receipts to engine reporting and node ledgers.
     *
     * Requires a complete checked backend advance. Aggregate cell A/Q already include barrels. Links
     * publish length-weighted time-mean face through-flow (native backend), instantaneous crown-capped
     * water depth/volume, slot volume and derived flow diagnostics. Native node receipts supply
     * separate inflow/outflow/flood rates; reported stage reconstruction never overwrites
     * solver-internal heads and preserves signed TPA piezometric levels. Banks mean structure/DUMMY
     * flow, scatters their node ledgers and reconstructs virtual-node reports. Does not advance clocks
     * or run-level mass balance, and native-only diagnostics are conditional on backend support.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] dt Positive accepted outer interval seconds for mean flow/overflow/receipt rates;
     *     instantaneous fields can still update for nonpositive dt.
     */
    void publishFv(SimulationContext& ctx, double dt);
    kinwave::KWSolver kw_solver_;
    dynwave::DWSolver dw_solver_;
    // Divider data moved to ctx.node_subtypes.dividers (relational side-table).
    std::vector<int> steady_sorted_links_; ///< Topological link order for STEADY routing
    bool cycle_detected_ = false;          ///< Gap #44: set true when toposort detects a loop

    /**
     * @brief Prepare node lateral/loss inflow-outflow baseline and storage loss receipts.
     *
     * For storage jointly caps evaporation and precomputed exfiltration against existing volume,
     * avoiding duplicate retained-moisture LID ET and leaving Richards ET to its ODE. Stores
     * evaporation/exfiltration ft3 receipts and node loss ft3/s. Initializes lateral inflow/loss
     * outflow and old excess-volume overflow using model-specific legacy/FV full-volume convention.
     * Requires precomputed exfiltration and initialized subtype/LID arrays; no geometry/topology
     * repair.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] dt Positive finite routing interval in seconds, consistent with saved old state and
     *     forcing.
     * @param[in] evap_rate Finite nonnegative evaporation rate in native ft/s.
     */
    void initNodeFlows(SimulationContext& ctx, double dt, double evap_rate);

    /**
     * @brief Compute native per-barrel conduit evaporation and signed seepage rates.
     *
     * Non-DW step preparation uses mean old/current depth, true routing length and shape-specific
     * widths; IRREGULAR/CUSTOM evaporation retains the implemented linear-width approximation. Applies
     * monthly conductivity and aquifer signed conductance/gain cap. FV losing components share a
     * stored-volume/dt bound; KW/STEADY defer their per-solve inflow cap. Host seepage forcing applies
     * last, beyond physical caps. Writes mapped ConduitData ft3/s rates; DW normally computes its own
     * per-Picard rates instead.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] dt Positive finite routing interval in seconds, consistent with saved old state and
     *     forcing.
     * @param[in] evap_rate Finite nonnegative evaporation rate in native ft/s.
     */
    void computeConduitLosses(SimulationContext& ctx, double dt, double evap_rate);

    /**
     * @brief Refresh native FV head-dependent forcing at a settled substep boundary.
     *
     * No-op for a non-native FV backend. Banks preceding structure flow duration, publishes only
     * current nonvirtual solver depth/head, reevaluates outfall stages at the substep time and refills
     * fixed-head array, then invokes structure equations. Current volumes/flow/node receipt ledgers
     * wait for publishFv. No global clock is advanced.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] t_elapsed Monotone elapsed seconds from the current outer routing-window start.
     * @param[in] eval_structures Synchronous borrowed no-argument callable evaluating structures and
     *     refilling forcing arrays; must remain live, not throw/reenter or change topology.
     */
    void refreshFvBoundaryFlows(SimulationContext& ctx, double t_elapsed,
                                const std::function<void()>& eval_structures);

    /**
     * @brief Bank current structure discharge over the elapsed forcing segment.
     *
     * Adds fv_struct_flow*(t_now-previous) to every non-conduit ft3 integral and advances the segment
     * anchor. DUMMY conduits use the backend own pass-through receipts instead. Requires sized
     * integral/forcing arrays and a fixed structure layout.
     *
     * @param[in] ctx Matching context supplying link types/count; no context fields change.
     * @param[in] t_now Monotone elapsed seconds within the current routing window; <=previous segment
     *     time is a no-op.
     */
    void accumulateStructureFlows(const SimulationContext& ctx, double t_now);

    /**
     * @brief Refresh final node heads from solver-published depth.
     *
     * Despite the historical name, current implementation only writes node head=invert+depth; solvers
     * already wrote link depth/volume. Must not run after FV publication because its piezometric head
     * can be below invert while displayed water depth is nonnegative. No old-state save, time advance
     * or mass-ledger update.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     */
    void updateLinkStates(SimulationContext& ctx);

    /**
     * @brief Route one steady tree-ordered pass using native conveyance and structure equations.
     *
     * Uses stored topological links or natural-order fallback. Updates each upstream storage before its
     * outgoing links, evaluates structures/DUMMY through legacy inflow dispatch, caps losing conduit
     * rates on current per-barrel inflow, limits normal flow to full capacity and publishes uniform
     * depth/volume/full-state. Then completes node volume/overflow and link endpoint depth raises via
     * kinwave::finishRouting. Requires prepared lateral/loss/outfall state and optional live
     * structures_ provider; allocations/exceptions can leave partial state.
     *
     * @param[in,out] ctx Resolved matching native-ft simulation context with stable
     *     node/link/subtype/profile dimensions; caller owns storage, clocks and run-level ledgers.
     * @param[in] dt Positive finite routing interval in seconds, consistent with saved old state and
     *     forcing.
     * @return One after the completed pass; there is no Picard convergence loop.
     */
    int executeSteadyFlow(SimulationContext& ctx, double dt);
};

} // namespace openswmm

#endif // OPENSWMM_ROUTING_HPP
