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
 * @file      ExplicitFvSolver.hpp
 * @brief     Serial/OpenMP CPU reference implementation of the explicit FV 1D solver.
 *
 * @details Godunov-type explicit finite volume on the conduit cell mesh:
 *          hydrostatic (Audusse) reconstruction → HLLC face flux → positivity
 *          limiting → flux divergence with the bed-slope correction →
 *          semi-implicit friction → explicit zero-D node continuity. Substeps
 *          at the CFL limit to fill each routing step.
 *
 *          This is the REFERENCE implementation in the plan's sense: the Kokkos
 *          plugin compiles the same FvKernels.hpp bodies, and the §6.8 parity
 *          harness diffs the two element-wise.
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §3, §5
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_EXPLICIT_FV_SOLVER_HPP
#define OPENSWMM_ENGINE_FV_EXPLICIT_FV_SOLVER_HPP

#include "AcceptedWaterVolumes.hpp"
#include <array>
#include <cstdint>
#include <vector>

#include "FvKernels.hpp"
#include "FvOptions.hpp"
#include "INetworkSolver.hpp"
#include "NetworkMeshData.hpp"
#include "PressurizedHeadSolver.hpp"

namespace openswmm::transport::fvkernels {
struct SpeciesKernelView; // transport/fvkernels/SpeciesTransportKernels.hpp
}

namespace openswmm::fv {

class ExplicitFvSolver : public INetworkSolver {
public:
    /**
     * @brief Construct an unbound solver with no borrowed mesh/state.
     *
     * Call initialize before using geometry/state helpers or advancing physical time.
     */
    ExplicitFvSolver() = default;
    /**
     * @brief Destroy owned solver scratch without deleting borrowed mesh/state.
     *
     * All returned member references and raw views become invalid; no final advance, report publication
     * or external callback is performed.
     */
    ~ExplicitFvSolver() override = default;

    /**
     * @brief Disallow copying a solver and its borrowed state/scratch ownership.
     */
    ExplicitFvSolver(const ExplicitFvSolver&) = delete;
    /**
     * @brief Disallow copy assignment of a live solver.
     */
    ExplicitFvSolver& operator=(const ExplicitFvSolver&) = delete;

    /**
     * @brief Enable the restricted native nonlinear pressure-storage experiment.
     *
     * Sets a persistent enable flag and copies controls; no immediate feature validation or solve
     * occurs. advance rejects unsupported options/forcing. There is no disable method on this instance;
     * initialize does not clear this flag. Serialize configuration with solver execution.
     *
     * @param[in] controls Copied nonlinear stopping/work controls; no referenced caller storage is
     *     retained.
     */
    void enablePressureStorageCandidate(const NonlinearPressureStorage::Controls& controls = {}) {
        pressure_candidate_ = true;
        pressure_controls_ = controls;
    }
    /**
     * @brief Borrow the most recently attempted nonlinear pressure candidate diagnostics.
     * @return Const member reference; content changes when a later candidate is attempted and expires
     *     at solver destruction.
     */
    const NonlinearPressureStorage::Result& pressureCandidateResult() const noexcept {
        return pressure_result_;
    }
    /**
     * @brief Read the current restricted-candidate routing-window rejection count.
     * @return Retry count reset at entry to a nonempty enabled-candidate advance; may retain history
     *     when that path is not entered.
     */
    long pressureCandidateRejections() const noexcept { return pressure_rejections_; }

    /**
     * @brief Bind a mesh/state and initialize native solver scratch and static eligibility.
     *
     * Mesh/state must remain alive at stable addresses and retain coherent dimensions/topology until
     * finalize or rebinding. Initializes owned face/cell/node/LTS/transport arrays, resets major run
     * statistics and builds algebraic/pass-through/rim caches. Does not validate every required
     * array/index; callers provide a constructed mesh and sized state. Allocation can throw after
     * binding and partial mutation, so is_initialized alone cannot certify successful completion.
     * Concurrent use of one solver or its borrowed state is unsupported.
     *
     * @param[in,out] mesh Borrowed validated mesh/topology and geometry; fills short optional node
     *     surcharge/ponding and face gate/culvert arrays, and derives missing face geometry.
     * @param[in,out] state Borrowed dimension-aligned conserved/node/species state in native ft units.
     *     Refreshes derived depths/velocities/areas, floors negative cell area and clears dry Q;
     *     initializes/clears TPA regime flags.
     * @param[in] opts Copied FV options; development environment switches are read synchronously and
     *     may override selected options.
     */
    void initialize(NetworkMeshData& mesh, NetworkStateData& state, const FvOptions& opts) override;
    /**
     * @brief Advance the borrowed native state over one requested routing window.
     *
     * Resets per-window receipts, reseeds nonalgebraic node volume from heads, subcycles globally or by
     * eligible LTS tiers and publishes current state/telemetry. Ordinary retry exhaustion or minimum
     * substep accepts the last state; inspect divergence() and reached time. General
     * allocation/callback failures can leave partial advance. The enabled restricted pressure candidate
     * instead validates supported water-only Euler options and restores its routing-window physical
     * state/accepted receipts on exceptions, then rethrows; work diagnostics remain attempted-work
     * telemetry. Callbacks must be synchronous and non-reentrant. External edits to cell A/Q require
     * reinitialize to invalidate derived-state caching.
     *
     * @param[in] t_current Finite current simulation time in seconds.
     * @param[in] t_target Finite target seconds, normally >=t_current; an unbound solver or nonforward
     *     interval returns t_target without advancing.
     * @param[in] forcing Borrowed step arrays/callback context, dimension-aligned and live for the
     *     entire call; values may be synchronously refreshed by the documented callback.
     * @return Reached time in seconds; can stop short after the ordinary-path two-million-operator-step
     *     backstop.
     */
    double advance(double t_current, double t_target, const FvStepForcing& forcing) override;
    /**
     * @brief Invalidate stepping caches and refresh derived state after external restoration.
     *
     * No-op when unbound. Invalidates LTS/active-list/CFL caches, refreshes depths/velocity/closure and
     * node areas. Does not clear run statistics, numerical carry, borrowed topology, existing TPA
     * history or all report receipts; use initialize for a new run. Does not rebuild static
     * topology/eligibility caches.
     *
     * @param[in] t0 Compatibility time in seconds; ignored by this implementation.
     */
    void reinitialize(double t0) override;
    /**
     * @brief Detach the borrowed mesh and state.
     *
     * Sets only mesh_/state_ to null. Does not delete external storage, clear owned
     * capacities/statistics/receipts, invoke callbacks or advance time. Reinitialize via initialize
     * before accessing state-dependent helpers.
     */
    void finalize() override;

    long last_num_steps() const noexcept override { return last_nsteps_; }
    double last_step_size() const noexcept override { return last_h_; }
    double suggested_step() const noexcept override { return suggested_h_; }
    RunStats run_stats() const noexcept override;
    bool is_initialized() const noexcept override { return mesh_ != nullptr; }
    bool divergence(Divergence& d) const noexcept override {
        if (div_link_ < 0)
            return false;
        d.link = div_link_;
        d.value = div_value_;
        d.what = div_what_;
        return true;
    }

    /// Per-conduit local-step sink for the "Conduit Time Step Summary"
    /// ([REPORT] LINK_STEPS). When set, every accepted global substep and LTS
    /// macro cycle calls `fn(user, conduit_row, dt, duration)` once per
    /// conduit row, with `dt` the conduit's governing (finest-cell) step.
    /// Reporting only: never read by the integrator. Null (default) = off.
    using ConduitStepSink = void (*)(void* user, int conduit, double dt, double dur);
    /**
     * @brief Set an optional synchronous per-conduit accepted-step reporting observer.
     *
     * Every accepted global step/LTS macro cycle invokes once per conduit. The sink is reporting-only,
     * must not throw, retain transient arguments, reenter the solver or mutate its mesh/state.
     * Restricted nonlinear candidate validation disallows the sink. Replacing a sink does not release
     * user storage.
     *
     * @param[in] fn Callback(user,conduit_row,governing_dt,duration), or nullptr to disable;
     *     dt/duration are seconds and conduit row is mesh ConduitData indexing.
     * @param[in] user Opaque borrowed callback context retained alongside fn; caller keeps it live
     *     until replaced or solver destruction.
     */
    void setConduitStepSink(ConduitStepSink fn, void* user) noexcept {
        step_sink_ = fn;
        step_sink_user_ = user;
    }

    /**
     * @brief Borrow the last-window signed node exchange receipt.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return Node-indexed ft3 positive into each node; storage is solver-owned and can change on
     *     advance/init.
     */
    const std::vector<double>& node_exchange() const noexcept { return node_exch_; }

    /// Enable optional water-transfer observations after initialize(). Disabled
    /// runs allocate/record nothing. Changing this invalidates borrowed rows.
    void trackAcceptedWater(bool enabled) {
        water_tracking_ = enabled;
        if (enabled && mesh_)
            water_.resize(mesh_->n_faces(), mesh_->struct_link.size(), mesh_->n_cells(), mesh_->n_nodes());
        else water_ = {};
        save_water_ = water_; rk_water_ = water_;
    }
    const AcceptedWaterVolumes& acceptedWater() const noexcept { return water_; }
    void requireConservativeNodeBalance(bool enabled) noexcept {conservative_node_balance_=enabled;}
    std::vector<double>& withdrawalRates() noexcept { return withdrawal_rates_; }
    const std::vector<double>& withdrawalRates() const noexcept { return withdrawal_rates_; }
    const std::vector<double>& unmetWithdrawalVolumes() const noexcept { return unmet_withdrawal_; }



    /**
     * @brief Borrow the signed numerical node-continuity carry.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return Node-indexed ft3. Positive carry is already in node_volume; inventory audits add only
     *     negative carry, avoiding double counting.
     */
    const std::vector<double>& node_continuity_carry() const noexcept { return node_carry_; }

    /**
     * @brief Borrow the most recent pressure-classification folded-node mask.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return Node-indexed flags. Historical if no current pressure pass ran; combine with current
     *     regime/forcing before interpreting.
     */
    const std::vector<std::uint8_t>& pressure_folded_nodes() const noexcept {
        return press_.foldedNodes();
    }

    /**
     * @brief Borrow accumulated arriving conduit-face volume magnitudes.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return Node-indexed nonnegative ft3 over the last advance; do not reconstruct this split from
     *     net exchange.
     */
    const std::vector<double>& node_inflow_volume() const noexcept { return node_in_; }
    /**
     * @brief Borrow accumulated departing conduit-face/diverted-lateral volume magnitudes.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return Node-indexed ft3 over the last advance, kept separately from arrival for report/mass
     *     ledgers.
     */
    const std::vector<double>& node_outflow_volume() const noexcept { return node_out_; }
    /**
     * @brief Book positive node lateral volume that entered cells without crossing node faces.
     *
     * If node_lat_div is set and lateral is positive, adds lateral*dt to node_out. Does not inject
     * water again, change forcing or handle negative lateral withdrawals. Caller invokes exactly once
     * per node update on either stepping path.
     *
     * @param[in] un Valid initialized global node index, unchecked when the diversion mask is present.
     * @param[in] dt Nonnegative accepted substep duration in seconds.
     * @param[in] forcing Borrowed aligned node_lateral ft3/s array or a null lateral pointer.
     */
    void bookDivertedLateral(std::size_t un, double dt, const FvStepForcing& forcing) noexcept {
        if (node_lat_div_.empty() || !node_lat_div_[un] || !forcing.node_lateral)
            return;
        const double lat = forcing.node_lateral[un];
        if (lat > 0.0)
            node_out_[un] += lat * dt;
    }

    /**
     * @brief Borrow culvert inlet-control occurrence flags for the last window.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return Conduit-row-indexed flags set when any substep inlet cap controlled; reset at advance
     *     entry.
     */
    const std::vector<uint8_t>& inlet_control() const noexcept { return inlet_control_; }

    /// Per-node flooding VOLUME (ft³) over the last advance().
    const std::vector<double>& node_flood_volume() const noexcept { return flood_vol_; }

    /**
     * @brief Borrow per-structure-entry DUMMY pass-through receipts.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return ft3 over the last window, aligned with mesh.struct_link; real structure entries remain
     *     zero.
     */
    const std::vector<double>& dummy_volume() const noexcept { return dummy_vol_; }

    /**
     * @brief Borrow the current direct-face algebraic-node bypass mask.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return Node-indexed flags refreshed with structure/lateral/carry/pressure state; solver head
     *     remains a stable diagnostic wet mean.
     */
    const std::vector<std::uint8_t>& node_passthrough() const noexcept { return node_pass_; }

    /**
     * @brief Borrow eligibility for reconstructing reported node stage from adjacent wet cells.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return Node-indexed reporting mask, broader than degree-two solver pass-through eligibility.
     *     Demoted real ponds are excluded before return from advance.
     */
    const std::vector<std::uint8_t>& node_publish_stage() const noexcept { return node_pub_; }

    /**
     * @brief Borrow the last-window time integral of mean face through-flow per cell.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return Cell-indexed aggregate ft3 used for routing-window mean discharge, rather than a final
     *     snapshot.
     */
    const std::vector<double>& cell_flow_integral() const noexcept { return cell_q_int_; }

    /**
     * @brief Evaluate native node storage volume from depth.
     *
     * Pondable depth above a positive full_depth uses its rim volume plus
     * ponded_area*(depth-full_depth). A non-storage-table node uses its current node_surf_area*depth; a
     * table node interpolates 129 uniform samples and extends above table top using node_vol_atop.
     * Requires positive table spacing and valid nondegenerate geometry. No allocation, state mutation
     * or independent validation.
     *
     * @param[in] node Valid initialized global node index with coherent storage arrays.
     * @param[in] depth Finite depth in ft; <=0 returns zero.
     * @return Volume in ft3.
     */
    double nodeVolumeFromDepth(int node, double depth) const;

    /**
     * @brief Read the most recent local-time-stepping tier count.
     * @return One means no separated tiering; otherwise the last assigned macro-cycle tier count.
     */
    int lts_tiers() const noexcept { return lts_tiers_; }

    /**
     * @brief Borrow the run tier-occupancy histogram.
     *
     * Const references remain valid only while this solver lives; their underlying element
     * pointers/contents can change on initialization or later stepping. Serialize reads with mutation.
     * @return Counts cells on every re-tier, including assignments whose macro cycle did not fit. It
     *     does not by itself count accepted macro cycles.
     */
    const std::array<long, kMaxLtsTiers>& tier_occupancy() const noexcept {
        return tier_occupancy_;
    }

private:
    /**
     * @brief Execute the shared mutable routing-window stepping implementation.
     *
     * Implements receipt reset, state reseeding, forcing refresh, active/CFL census, LTS/global
     * retries, state publication and divergence telemetry described by advance. Has no outer
     * routing-window transaction; enabled pressure-candidate advance supplies that wrapper. Requires
     * exclusive initialized mesh/state/scratch. Exceptions can leave partial accepted state, except
     * where the wrapper restores it.
     *
     * @param[in] t_current Current seconds; caller supplies finite values.
     * @param[in] t_target Target seconds; unbound/nonforward calls return target unchanged.
     * @param[in] forcing Borrowed forcing retained internally for the in-flight call; arrays/context
     *     must stay valid through all substeps.
     * @return Time reached in seconds, possibly short after the ordinary step-count backstop.
     */
    double advanceImpl(double t_current, double t_target, const FvStepForcing& forcing);
    /**
     * @brief Reject unsupported nonlinear pressure-storage experiment combinations.
     *
     * Requires initialized closed SLOT, pressure-implicit, first-order global Euler, water-only state
     * with no LTS/compaction/UF, structures, gates, culverts, physical storage/ponding, conduit loss,
     * finite source caps, refresh callback or reporting sink. Throws invalid_argument if any
     * restriction fails; does not validate numeric candidate controls or change state.
     *
     * @param[in] forcing Current aligned forcing and callback descriptors inspected without mutation.
     */
    void validatePressureCandidate(const FvStepForcing& forcing) const;
    // -- substep pipeline ---------------------------------------------------
    /**
     * @brief Rebuild every cell derived state and closure cache from conserved area/discharge.
     *
     * Requires coherent initialized geometry/state arrays. Floors negative area to zero, derives
     * SLOT/TPA depth and stage, refreshes closure cache, zeros non-TPA dry discharge/velocity and
     * computes wet velocity (TPA uses a tiny area floor). Per-cell writes are independently
     * parallelizable. Does not advance time or alter report receipts; it can change externally supplied
     * invalid/dry conserved values.
     */
    void refreshDepths();
    /**
     * @brief Refresh native node area diagnostics/storage coefficients.
     *
     * Virtual nodes receive zero area; table storage reads a finite difference over one table-depth
     * increment with MIN_SURFAREA floor; other nodes copy the fixed mesh.node_area. Requires positive
     * table spacing and coherent head/geometry arrays. Does not advance node volume or change topology;
     * called during initialize/reinitialize.
     */
    void refreshNodeAreas();
    /**
     * @brief Rebuild active cell/face work lists and invalidate cached tier assignment.
     *
     * Requires current depths/velocities/regime/lateral forcing. Clears held mass/momentum/hydrostatic
     * face arrays whenever the list changes. Without compaction includes the whole mesh; otherwise
     * seeds wet/moving/TPA/lateral-fed cells and wet node boundaries, grows an eight-cell
     * double-buffered halo and collects incident faces. Resets rebuild cadence and accumulates
     * active-fraction telemetry. Allocations may throw; no physical time advances.
     */
    void rebuildActiveLists();
    /**
     * @brief Reduce the current face/ghost and applicable node timestep constraints.
     *
     * Reads initialized active lists/current geometry, velocities, node heads, forcing and regime.
     * Includes ghost/far pass-through wave speeds, local cell lengths and applicable algebraic-feedback
     * or node-storage accuracy bounds. Eligible LTS supplies separate node cadence, so its global
     * census omits node accuracy bounds. Const numerical state is preserved, but mutable
     * argmin/performance telemetry and optional OPENSWMM_FV_DT_TRACE diagnostics update. No minimum
     * timestep floor or time advance is applied here.
     *
     * @param[in] press_edit True only when the synchronous pressure pass will run: full eligible
     *     pressure faces lose their acoustic bound and folded nodes lose explicit feedback limits.
     * @return Stable/accuracy interval in seconds; 1e30 when no finite constraint is found.
     */
    double censusDt(bool press_edit = false) const;
    void reconstructState();
    /**
     * @brief Evaluate one native shared hydraulic face receipt and side corrections.
     *
     * Reconstructs both sides in common face geometry, chooses far-cell ghosts for active pass-through
     * nodes, computes shared HLL flux and applies flap-gate reflection, culvert inlet and optional node
     * donor caps. Mass-only uses identical scalar mass decisions. Native volume flux is ft3/s and
     * momentum flux/corrections ft4/s2. Requires live forcing_ when source caps are used. Each face
     * must have one writer; this routine performs no node/cell update or positivity scaling.
     *
     * @param[in] face Valid active face index with initialized geometry/topology and current
     *     reconstruction.
     * @param[in] mass_only True for algebraic trial residuals: writes f_mass only among hydraulic face
     *     arrays; can still latch inlet_control telemetry. False also stores momentum, states, side
     *     corrections and optional species fan records.
     */
    void computeFaceFlux(int face, bool mass_only = false);
    /**
     * @brief Forward shared scalar reconstruction/FCT preparation using current limited water flux.
     *
     * Requires initialized species state and the final water face ledger after node/pressure/positivity
     * corrections. Passes a borrowed SpeciesKernelView synchronously to
     * transport::fvkernels::reconstructScalars. Scalar layouts are species-major; allocation/failure
     * behavior follows the shared kernel. Does not independently recompute water flux.
     *
     * @param[in] dt Positive finite substep duration in seconds, consistent with the current flux/state
     *     snapshot.
     */
    void reconstructScalars(double dt);
    /**
     * @brief Create a nonowning view of this solver hydraulic/scalar state for shared transport
     * kernels.
     *
     * Valid only while this initialized solver and its buffers remain stable. Kernels mutate their
     * designated scalar scratch/state synchronously; do not retain the view across initialization,
     * finalize, reallocation or destruction. No allocation or ownership transfer occurs here.
     * @return View by value containing borrowed mesh/state and member-vector pointers plus copied
     *     scalar options.
     */
    transport::fvkernels::SpeciesKernelView speciesKernelView();
    /**
     * @brief Evaluate every current active face hydraulic receipt.
     *
     * Requires reconstructed state, live forcing_ and initialized active lists. Runs disjoint face
     * writers, parallel above its work threshold, then increments evaluated-face work count. Inactive
     * face hydraulic arrays must already be zero/held consistently by rebuildActiveLists. Does not
     * apply positivity or update conserved state.
     */
    void computeFluxes();
    /**
     * @brief Bound current shared outgoing water fluxes by donor volume.
     *
     * Runs after node/pressure flux replacement. Reapplies finite source-array donor limits, totals
     * outgoing cell/node volume rates, skips artificial algebraic-node bucket limits, and scales each
     * final mass/momentum face flux by the donor factor. Both adjacent water ledgers consume that same
     * scaled mass receipt. Uses thread-local temporary vectors and can allocate; does not update
     * A/volume or time. Scalar reconstruction follows this pass so transport consumes the final water
     * ledger.
     *
     * @param[in] dt Positive finite substep duration in seconds, consistent with the current flux/state
     *     snapshot.
     */
    void limitPositivity(double dt);

    /**
     * @brief Solve/relax global-step node coupling before donor positivity limiting.
     *
     * Marks every face live, creates the global node list excluding nodes folded into this substep
     * pressure system, then dispatches solveNodes at shared dt. Requires current reconstructed face
     * fluxes and structure/dummy sources. Mutates node heads or shared mass faces through those
     * routines; no conservative volume integration occurs here.
     *
     * @param[in] dt Positive finite substep duration in seconds, consistent with the current flux/state
     *     snapshot.
     * @param[in] forcing Borrowed aligned current step forcing in native units, with synchronous
     *     non-reentrant callback context live for the call.
     */
    void relaxNodeFluxes(double dt, const FvStepForcing& forcing);
    /**
     * @brief Dispatch independent algebraic and bucket node flux-response updates.
     *
     * Splits cheap fixed/pass-through/storage cases from heavy bracketed junctions. Heavy cases run in
     * a dynamic OpenMP loop above threshold, each writing only its own head/incident faces; light cases
     * run serially. Requires current live-face stamps and all source/eligibility arrays. Scratch
     * allocation can throw; callable must not reenter or mutate shared solver structures.
     *
     * @param[in] nodes Unique valid node indices; incident node boundary faces must be exclusively
     *     owned by these nodes.
     * @param[in] dt_of Synchronous callable(node)->positive seconds; may run concurrently for heavy
     *     node solves and must be safe for those reads.
     * @param[in] forcing Borrowed aligned current step forcing in native units, with synchronous
     *     non-reentrant callback context live for the call.
     */
    template <class DtOf>
    void solveNodes(const std::vector<int>& nodes, DtOf&& dt_of, const FvStepForcing& forcing);
    std::vector<int> node_solve_list_; ///< global-path node list (scratch)
    std::vector<int>
        node_heavy_; ///< junctions that run the bracketed solve (shared by the parallel loop)
    std::vector<int> node_light_; ///< pass-through / storage nodes (serial)

    /**
     * @brief Apply one frozen linearized bucket-node correction to incident mass fluxes.
     *
     * Freezes original head, incident fluxes and characteristic sqrt(g*A*T) resistances, then corrects
     * f_mass by the response of the node continuity equation. Does not move node head, momentum flux or
     * conserved volume. The identical corrected mass array feeds both cell/node updates; nonpositive
     * total resistance is a no-op. One node owns its incident boundary faces.
     *
     * @param[in] node Valid initialized node index; virtual, fixed-head, zero-area or DUMMY-drain nodes
     *     return unchanged.
     * @param[in] dt Positive finite substep duration in seconds, consistent with the current flux/state
     *     snapshot.
     * @param[in] forcing Borrowed aligned current step forcing in native units, with synchronous
     *     non-reentrant callback context live for the call.
     */
    void relaxOneNode(int node, double dt, const FvStepForcing& forcing);

    /**
     * @brief Balance a zero-storage junction head and recompute its live face fluxes.
     *
     * Fixed heads are unchanged. Clean pass-through nodes take the wet-neighbor mean stage (or lowest
     * bed if all dry), without a root solve. Other nodes balance signed face
     * flow+lateral+structure+carry/dt using up to 64 bracket expansions and 16 guarded
     * quasi-Newton/bisection iterations, with 1e-12-ft3/s residual and 1e-6-ft movement targets.
     * Physical ceiling/floor and a guarded degree-one prescribed-discharge fallback handle unbracketed
     * cases; exhaustion uses the best sampled head without an error status. Trial passes write
     * node_head and f_mass; finalization recomputes full live hydraulic receipts. Held LTS faces remain
     * constants. No conserved volume integration occurs here; updateNodes settles the residual.
     *
     * @param[in] node Valid initialized node index with coherent topology/eligibility/storage arrays,
     *     unchecked.
     * @param[in] dt Positive finite timestep in seconds matching the current flux/source window.
     * @param[in] forcing Borrowed aligned current forcing in native units; matching structure/lateral
     *     arrays and callback context remain live for this call.
     */
    void solveAlgebraicNode(int node, double dt, const FvStepForcing& forcing);

    /**
     * @brief Dispose of a junction continuity residual without introducing artificial bucket storage.
     *
     * Folds any published water above the previous positive carry back into the ledger. Positive
     * surplus at ponding rim becomes real pond volume/head; at a sealed physical ceiling becomes
     * flooding. A free-surface pass-through can redistribute residual water evenly to adjacent cells
     * without momentum, leaving unabsorbed remainder as carry. Pressure-adjacent residuals stay banked.
     * Otherwise stores signed carry and publishes only its positive water volume; solved head is
     * retained except when real pond volume determines it. May update adjacent cell
     * A/depth/velocity/cache, so invoke in the serialized node stage.
     *
     * @param[in] node Valid initialized node index with coherent topology/eligibility/storage arrays,
     *     unchecked.
     * @param[in] carry Signed accumulated residual water volume in ft3, including current substep
     *     sources/faces.
     */
    void settleAlgebraicNode(int node, double carry);

    /// Static eligibility for the algebraic junction treatment: plain
    /// junction (no storage table), at least one incident conduit face.
    std::vector<std::uint8_t> node_alg_;
    /// Static half of the pass-through test: algebraic, exactly two incident
    /// faces, neither face carrying a culvert inlet or flap gate.
    std::vector<std::uint8_t> node_pass_static_;
    /// Full pass-through test, refreshed with the forcing (refreshStructFlows):
    /// static half AND no lateral, no structure flow, no unbled carry this
    /// routing step. When set, the node's faces present the far cells to each
    /// other directly (faceSide/computeFaceFlux) — the direct spliced face —
    /// which transmits both momentum and depth across the interface exactly.
    std::vector<std::uint8_t> node_pass_;
    /// Static/full halves of the PUBLISH-stage test — see
    /// node_publish_stage(). Deliberately separate from node_pass_, which the
    /// SOLVER reads: widening node_pass_ instead would change the direct-face
    /// bypass and with it the routing, which this is explicitly not allowed to
    /// do (solver-internal heads are load-bearing ghost states under LTS tier
    /// holds).
    std::vector<std::uint8_t> node_pub_static_;
    std::vector<std::uint8_t> node_pub_;
    /// Residual-volume carry (ft³) for algebraic junctions: root-solve
    /// tolerance and any cell-side positivity scaling land here and are bled
    /// back into the next solve's forcing. ≈0 in a converged steady state.
    std::vector<double> node_carry_;
    /// Lateral inflow diverted from a clean degree-2 junction into its two
    /// incident cells (half each) as a zero-momentum area source, so the node
    /// KEEPS the pass-through splice. Head-solving such junctions costs the
    /// split-Riemann ~1 mm of head per junction that pass-through exists to
    /// avoid — with rain on every junction of a 400-conduit channel that
    /// integrated into an 18-25% deep bias with exact q (SWASHES §3.3).
    /// cell_qlat_ is native ft3/s per cell; node_lat_div_ flags the nodes whose
    /// forcing.node_lateral must read as zero on every node path.
    std::vector<double> cell_qlat_;
    std::vector<std::uint8_t> node_lat_div_;
    /**
     * @brief Read the effective node lateral source after direct-cell diversion.
     *
     * @param[in] forcing Borrowed aligned current step forcing in native units, with synchronous
     *     non-reentrant callback context live for the call.
     * @param[in] un Valid initialized node index, unchecked when the diversion mask/forcing array is
     *     present.
     * @return Zero for a diverted node or missing lateral array; otherwise the signed lateral rate in
     *     ft3/s.
     */
    double nodeLateral(const FvStepForcing& forcing, std::size_t un) const noexcept {
        if (!node_lat_div_.empty() && node_lat_div_[un])
            return 0.0;
        return forcing.node_lateral ? forcing.node_lateral[un] : 0.0;
    }
    /// The diverted lateral's RIM. A pass-through junction has no head state,
    /// so the injection above had no ceiling: once the incident cells were
    /// full, every m³ the pipe could not convey went into the Preissmann
    /// slot, whose storage is t_slot·L ≈ 1e-3 m² per pipe — about a kilometre
    /// of head per m³ — and the divergence guard (ERROR 14) fired within a
    /// routing step of the first overload (Bellinge, 2026-09-05: junction
    /// depths of 90–200 m alternating with 0.1 m as the 2D drain toggled
    /// sign, then 3,346 m). A solved junction clamps at its rim and books the
    /// surplus as flooding or ponded volume; the diverted path now does the
    /// same at the cell. face_a_rim_ is the receiving cell's area at the node
    /// rim (invert + full depth, + surcharge depth when sealed; +inf where no
    /// rim applies: open sections, no full depth on record, faces of
    /// non-pass-through nodes). The injection stops at that area and the
    /// remainder is banked per FACE in face_lat_spill_ (ft³) — per face, not
    /// per cell, because a one-cell conduit can receive from a pass-through
    /// junction at each end — and drained by bookLateralSpills at the node
    /// update into flood_vol_ (sealed) or the ponded-volume ledger (pondable,
    /// which demotes the node to the bucket path exactly as
    /// settleAlgebraicNode's ponding branch does). Only the owning cell writes
    /// a boundary face's slot, so the cell update stays race-free.
    std::vector<double> face_a_rim_;
    std::vector<double> face_lat_spill_;
    /**
     * @brief Apply a direct-cell lateral source while banking rim-rejected inflow.
     *
     * For positive inflow uses the lowest finite incident node rim, retains existing water even if
     * already above rim, admits only remaining area room and banks refused ft3 in that boundary face
     * spill slot. Uncapped or withdrawing sources add directly. Only the owning cell writes its
     * incident boundary spill; caller later floors area and books spills at nodes exactly once.
     *
     * @param[in] uc Valid owned cell index with aligned cell_qlat and incident-face/rim arrays.
     * @param[in] a_new Post-flux candidate aggregate area in ft2.
     * @param[in] dt Positive finite timestep in seconds matching the current flux/source window.
     * @param[in] inv_dx Positive inverse cell length in 1/ft.
     * @return Candidate area plus admitted lateral contribution in ft2; negative sources are not
     *     floored here.
     */
    double injectDivertedLateral(std::size_t uc, double a_new, double dt, double inv_dx);
    /**
     * @brief Drain rejected direct-cell lateral volume into node ponding/flooding ledgers.
     *
     * For statically eligible pass-through nodes, sums and zeroes incident face spill slots. Positive
     * spill goes to real ponded volume at least full capacity, or to flood_vol for sealed nodes. Does
     * not directly refresh pond head; the ensuing node bucket update does so. Must follow corresponding
     * cell updates and precede node ledger integration, with exclusive spill ownership.
     *
     * @param[in] nodes Optional unique valid due-node list; nullptr visits all nodes.
     */
    void bookLateralSpills(const std::vector<int>* nodes);
    /// Cached V(full_depth) per node, for the ponding demote test.
    std::vector<double> node_vfull_;

    /**
     * @brief Test whether a node currently uses zero-storage algebraic continuity.
     *
     * Demoted ponds follow the real bucket relation until their water drains below the threshold; no
     * state is changed.
     *
     * @param[in] n Valid initialized node index, unchecked.
     * @return True for static algebraic eligibility unless enabled ponding has volume strictly above
     *     cached full volume.
     */
    bool algebraicActive(int n) const noexcept {
        const auto un = static_cast<std::size_t>(n);
        if (!node_alg_[un])
            return false;
        if (mesh_->node_can_pond[un] && state_->node_volume[un] > node_vfull_[un])
            return false;
        return true;
    }

    /**
     * @brief Inspect adjacent cells for pressure-band or latched TPA state.
     *
     * Unlike the narrower anyPressurizedCell early-exit scan, this explicitly recognizes TPA flags
     * below crown. Used when refreshing bypass/forcing eligibility; no state changes.
     *
     * @param[in] un Valid initialized node index with coherent node-face CSR.
     * @return True for any adjacent flagged TPA cell, or closed geometry whose depth>=y_crown; false
     *     otherwise.
     */
    bool incidentPressurized(std::size_t un) const noexcept {
        for (int p = mesh_->node_face_ptr[un]; p < mesh_->node_face_ptr[un + 1]; ++p) {
            const auto uf =
                static_cast<std::size_t>(mesh_->node_face_idx[static_cast<std::size_t>(p)]);
            const int c = (mesh_->face_cl[uf] >= 0) ? mesh_->face_cl[uf] : mesh_->face_cr[uf];
            if (c < 0)
                continue;
            const auto uc = static_cast<std::size_t>(c);
            if (tpaCell(uc))
                return true;
            const FvGeometry& g = mesh_->geom[static_cast<std::size_t>(mesh_->cell_geom[uc])];
            if (!g.is_open && state_->cell_h[uc] >= g.y_crown)
                return true;
        }
        return false;
    }
    /**
     * @brief Integrate global-step cell water, momentum and scalar fields from final face receipts.
     *
     * Requires positivity-limited water fluxes, reconstructed/FCT scalar fluxes, current HO2 slopes and
     * old-snapshot UF gradients. Each active cell gathers two oriented mass/momentum receipts plus
     * hydrostatic corrections, adds centered bed and diverted lateral sources, integrates species
     * conservatively, applies distributed loss and semi-implicit friction/local/UF terms, and refreshes
     * A/Q/depth/stage/velocity/closure. Dry states zero Q; reports integrate mean face through-flow
     * rather than collocated momentum. Cell writes are disjoint; rim spill is booked per uniquely owned
     * boundary face. No clock advance or node volume update occurs here.
     *
     * @param[in] dt Positive finite timestep in seconds matching the current flux/source window.
     * @param[in] forcing Borrowed aligned current forcing in native units; matching structure/lateral
     *     arrays and callback context remain live for this call.
     */
    void updateCells(double dt, const FvStepForcing& forcing);
    /**
     * @brief Integrate global-step node receipts and physical/algebraic continuity.
     *
     * First derives DUMMY pass-through from the same face-arrival volume, then books cell-rejected
     * lateral spills. Accumulates net/in/out face receipts and diverted-lateral outflow. Fixed heads
     * are imposed without owning their volume ledger; active algebraic nodes settle carry, while real
     * buckets integrate signed source/face water, floor volume, invert depth and apply
     * pond/surcharge/flood capacity. Virtual nodes have no node update. Uses the identical final f_mass
     * array as cells.
     *
     * @param[in] dt Positive finite timestep in seconds matching the current flux/source window.
     * @param[in] forcing Borrowed aligned current forcing in native units; matching structure/lateral
     *     arrays and callback context remain live for this call.
     */
    void updateNodes(double dt, const FvStepForcing& forcing);
    /**
     * @brief Forward the synchronous implicit scalar dispersion pass.
     *
     * Uses speciesKernelView and transport::fvkernels::dispersionSolve after hydraulic/scalar
     * advection. Requires initialized species-major arrays and coherent chain geometry/dispersion
     * options. Shared kernel owns scratch/failure behavior; no independent hydraulic flux or clock
     * update occurs.
     *
     * @param[in] dt Positive finite timestep in seconds matching the current flux/source window.
     */
    void dispersionSolve(double dt);

    /**
     * @brief Refresh node structure sources and current direct-cell lateral/bypass eligibility.
     *
     * At a settled cycle boundary, clears/scatters supplied non-DUMMY aggregate structure flow by
     * global link index, rebuilds per-cell lateral and node-diversion arrays, and refreshes
     * pass-through/report masks. Clean unpressurized degree-two laterals split half to each adjacent
     * cell; virtual-node laterals split at the spliced face. Structure flow, DUMMY drainage, carry or
     * pressure revoke solver bypass as applicable. Newly fed dry cells invalidate active lists. Can
     * allocate; no lateral volume is integrated until cell/node update.
     *
     * @param[in] forcing Borrowed aligned current forcing in native units; matching structure/lateral
     *     arrays and callback context remain live for this call.
     */
    void refreshStructFlows(const FvStepForcing& forcing);
    std::vector<double> node_qstruct_;

    /// Pass-through source/sink from DUMMY links, kept separate from
    /// node_qstruct_ because it is derived, not forced: a dummy carries
    /// whatever arrives at its upstream node, so its value depends on the very
    /// fluxes the node update is about to integrate. Computed by
    /// refreshDummyFlows() immediately before each node update, from the same
    /// arrival volume that update will use, which makes the drain exact rather
    /// than lagged. Folded into q_lat alongside node_qstruct_ at all three
    /// integration sites (relaxOneNode, updateNodes, fireNodes).
    std::vector<double> node_qdummy_;

    /// Volume each DUMMY link has passed through so far this routing step
    /// (ft³), parallel to mesh.struct_link. Router divides by dt to publish the
    /// step-mean discharge, matching how real structure flows are reported.
    std::vector<double> dummy_vol_;

    /**
     * @brief Derive one-way DUMMY transfer from the arrival volume about to be consumed.
     *
     * No-op with no DUMMY mask or nonpositive dt. Clears node_qdummy then computes
     * max(arrived/dt+effective lateral+structure,0), applies optional link cap and node source
     * availability, scatters the paired ft3/s source/sink and accumulates DUMMY ft3 receipt. Requires
     * topology validation that a DUMMY is its upstream node sole outlet with no chained DUMMY
     * dependency; capped transfers can leave upstream storage. Callback is not retained and must not
     * reenter/mutate source arrays.
     *
     * @param[in] dt Positive finite timestep in seconds matching the current flux/source window.
     * @param[in] forcing Borrowed aligned current forcing in native units; matching structure/lateral
     *     arrays and callback context remain live for this call.
     * @param[in] arrived Synchronous callable(node_index)->net accepted face arrival volume in ft3 over
     *     dt; must match the imminent node update.
     */
    template <class ArrivedFn>
    void refreshDummyFlows(double dt, const FvStepForcing& forcing, ArrivedFn&& arrived, bool book_receipts = true);

    /**
     * @brief Read a DUMMY-derived node source with an empty-array fast path.
     *
     * @param[in] un Valid node index when dummy source storage is present.
     * @return Signed DUMMY rate in ft3/s, or zero when no array is allocated.
     */
    double nodeQDummy(std::size_t un) const noexcept {
        return node_qdummy_.empty() ? 0.0 : node_qdummy_[un];
    }

    /**
     * @brief Invert the initialized node volume relation.
     *
     * Table-top extension uses node_vol_atop when positive; a zero slope maps surplus
     * to the flooding branch. Pond extension requires positive ponded_area. Plateaus use the lower interpolation
     * endpoint. No input validation, mutation or allocation.
     *
     * @param[in] node Valid initialized global node index.
     * @param[in] volume Finite volume in ft3; <=0 returns zero.
     * @return Depth in ft using pond tail, positive-area linear bucket or monotone table bisection.
     */
    double nodeDepthFromVolume(int node, double volume) const;

    /**
     * @brief Test option/state prerequisites for the tiered stepping path.
     *
     * Does not test current pressure activity or whether a useful multi-tier cycle fits the window;
     * advance applies those further gates.
     * @return True when LTS is enabled, integration is not RK2, state is bound and n_species==0.
     */
    bool ltsEligible() const noexcept {
        return opts_.lts && opts_.time_integration != TimeIntegration::RK2 && state_ &&
               state_->n_species == 0;
    }

    /**
     * @brief Check whether one face belongs to the flux window currently being processed.
     *
     * Algebraic trial recomputation must skip non-live faces to preserve their previously booked
     * cadence; this query does not advance a stamp.
     *
     * @param[in] f Valid face index when all_faces_live_ is false; unchecked.
     * @return True on the global path or for a matching current LTS generation stamp.
     */
    bool faceIsLive(int f) const noexcept {
        return all_faces_live_ ||
               (!live_stamp_.empty() && live_stamp_[static_cast<std::size_t>(f)] == live_gen_);
    }

    /// Faces whose flux is being computed on this base step. Stamped rather
    /// than cleared so marking a due set is O(due), not O(faces).
    std::vector<std::uint32_t> live_stamp_;
    std::uint32_t live_gen_ = 0;
    /// The global (non-LTS) path recomputes every face each step, so there is
    /// no window to violate and the re-solve is unrestricted there. A run can
    /// use BOTH paths — a rejected macro cycle falls through to the global one
    /// — so this is set per step rather than once.
    bool all_faces_live_ = true;

    /**
     * @brief Compute the explicit pressure-junction ghost-feedback bound.
     *
     * TPA flags count pressure at any head. Ignores compaction masks so optional work omission cannot
     * change the integration schedule. No bucket volume or ordinary node accuracy rule is added here.
     *
     * @param[in] n Valid initialized algebraic node index with incident cell geometry.
     * @return cfl*min_pressure(0.5*dx*T)/sum_all_wet(T*c), in seconds, or 1e30 without a pressure
     *     storage contribution/admittance.
     */
    double algebraicNodeStableDt(int n) const noexcept;

    /**
     * @brief Apply the native steady-friction discharge update for one geometry/regime.
     *
     * At full FORCE_MAIN uses q/(1+dt*g*Sf/abs(u)); otherwise delegates to Manning frictionUpdate. Does
     * not add local loss/UF, change state or validate every coefficient. Increments geometry
     * performance telemetry.
     *
     * @param[in] g Initialized section closure with positive wet friction radius.
     * @param[in] roughness Full FORCE_MAIN parameter: <1 means Darcy-Weisbach roughness ft, >=1 means
     *     Hazen-Williams C.
     * @param[in] rough_factor Native Manning g*(n/1.486)^2 for ordinary/part-full flow.
     * @param[in] q Candidate aggregate discharge in ft3/s.
     * @param[in] u Consistent conveyance velocity in ft/s.
     * @param[in] h Friction depth in ft; TPA callers use y_full to retain the full-pipe law.
     * @param[in] dt Positive finite timestep in seconds matching the current flux/source window.
     * @param[in] radius Hydraulic radius evaluated at h in ft, from the current closure.
     * @return Damped discharge in ft3/s; force-main zero velocity is unchanged.
     */
    double frictionFor(const FvGeometry& g, double roughness, double rough_factor, double q,
                       double u, double h, double dt, double radius) const;

    /**
     * @brief Apply ponding or sealed surcharge/flood capacity to an integrated node bucket.
     *
     * No-op without positive full depth or while at/below the rim. Pondable nodes keep water and add
     * only max(vol-max(v_prev,Vfull),0) to flooding-report volume. Sealed nodes may rise through
     * surcharge depth, then excess volume is removed to flood_vol and volume/depth clamp together. Does
     * not publish node_head itself or independently validate the supplied pair.
     *
     * @param[in] node Valid initialized node index with coherent topology/eligibility/storage arrays,
     *     unchecked.
     * @param[in] v_prev Pre-update water volume in ft3, used to report only newly crossed pond-rim
     *     volume.
     * @param[in,out] vol Nonnegative integrated volume in ft3; clipped to sealed maximum when spilling.
     * @param[in,out] depth Depth in ft from the same volume relation; clipped with sealed spilling.
     */
    void applyNodeCapacity(int node, double v_prev, double& vol, double& depth);

    // -- local time stepping (plan §3.3) ------------------------------------
    /**
     * @brief Compute one cell explicit stability interval including its presented boundary states.
     *
     * Uses regime-consistent cell/TPA celerity plus node-head ghosts or the actual far cell through a
     * pass-through ghost. Does not apply the global implicit-pressure census exemption; used to assign
     * local tiers. No physical state mutation.
     *
     * @param[in] cell Valid initialized cell index, irrespective of compaction activity.
     * @return opts.cfl*dx/(maximum abs(u)+c) in seconds, or 1e30 when no nontrivial speed exists.
     */
    double cellStableDt(int cell) const;
    /**
     * @brief Compute a bucket node storage/accuracy interval for local tiering.
     *
     * Includes wet/TPA adjacent geometry without compaction gating. Algebraic feedback is instead
     * applied to incident cells before tier-base selection. No state change or physical timestep floor.
     *
     * @param[in] node Valid initialized node index.
     * @return Minimum face CFL interval using effective length node_surf_area/T in seconds; 1e30 for
     *     virtual, algebraic, outfall, nonpositive-area or unconstrained nodes.
     */
    double nodeStableDt(int node) const;

    /**
     * @brief Assign synchronized power-of-two cell/face/node cadences and due sets.
     *
     * Requires settled accumulators, current state and active lists. Includes all-cell bounds
     * regardless of compaction, node accuracy and optional algebraic-feedback limits, applies adjacency
     * grading, pins algebraic nodes to their finest incident cell and structure endpoints to tier zero,
     * then assigns faces to the finer side. Builds active per-tier/nested due lists, resets pending
     * accumulators and adds tier occupancy/argmin telemetry. Species force K=1. Allocations can throw;
     * never re-tier while flux is booked but undrained.
     *
     * @param[out] dt0 Finest admissible native interval in seconds when a finite requirement exists;
     *     early unsupported/unconstrained returns can leave the prior value.
     * @param[in] k_cap_override Positive optional cap on tier count; <=0 uses the configured cap,
     *     itself limited to kMaxLtsTiers.
     * @return Tier count K; one means the caller should take the global path.
     */
    int assignTiers(double& dt0, int k_cap_override = 0);

    /**
     * @brief Open due local-tier face windows and book their conservative transfers.
     *
     * Stamps only due faces live, evaluates their receipts, solves touched nodes at each node own tier
     * window, applies outgoing positivity against stored plus pending water and books identical signed
     * F*dt into both incident accumulators. Updates node receipt and cell flow-integral ledgers at face
     * cadence. Requires live forcing_ and current reconstruction/tier arrays; no volume is drained yet.
     * Does not run the synchronized pressure-implicit path or species transport.
     *
     * @param[in] faces Unique valid due-face indices from current nested tier lists; empty is a no-op.
     * @param[in] dt0 Positive finite base interval in seconds; each face uses 2^face_tier*dt0.
     */
    void fireFaces(const std::vector<int>& faces, double dt0);
    /**
     * @brief Close due local-tier cell windows and drain their booked transfers.
     *
     * Requires matching tier assignment, pending oriented mass/momentum and fresh old UF snapshot.
     * Drains/zeros its accumulators once, adds centered-bed/diverted-lateral/distributed-loss sources,
     * floors area, derives regime depth/cache and applies steady/local/UF friction. Species are
     * unsupported on this path. TPA wet cells require positive admitted area for velocity division;
     * inadmissible state is not separately diagnosed here.
     *
     * @param[in] cells Unique valid due active-cell indices; each worker exclusively owns
     *     cell/accumulator/incident boundary-spill writes.
     * @param[in] dt0 Positive finite base seconds; each cell source/friction window is 2^cell_tier*dt0.
     * @param[in] forcing Borrowed aligned current forcing in native units; matching structure/lateral
     *     arrays and callback context remain live for this call.
     */
    void fireCells(const std::vector<int>& cells, double dt0, const FvStepForcing& forcing);
    /**
     * @brief Close due local-tier node windows from booked face water.
     *
     * Computes DUMMY transfers from pending arrivals (structure endpoints are pinned to base tier),
     * drains cell spill and node accumulators, books diverted lateral outflow, imposes fixed heads or
     * integrates algebraic carry/real buckets and capacity. Head solve already occurred at face firing.
     * No pending face transfer may be reinterpreted with a different tier; re-tier only after
     * synchronization/settle.
     *
     * @param[in] nodes Unique valid nonvirtual due-node indices from current nested tier lists.
     * @param[in] dt0 Positive base seconds; each node integrates over 2^node_tier*dt0.
     * @param[in] forcing Borrowed aligned current forcing in native units; matching structure/lateral
     *     arrays and callback context remain live for this call.
     */
    void fireNodes(const std::vector<int>& nodes, double dt0, const FvStepForcing& forcing);
    /**
     * @brief Execute one synchronized conservative local-tier macro cycle.
     *
     * TPA transitions update once at cycle entry. Face windows open at multiples of 2^tier and
     * cell/node windows close at their matching end, so sources and booked transfers cover the same
     * span. Reconstructs HO2 and UF snapshots as required, then fires due faces/cells/nodes. With a
     * complete cycle all accumulators drain; caller owns outer save/reject/clock/stat acceptance and
     * must keep forcing unchanged mid-cycle.
     *
     * @param[in] dt0 Positive finite base interval in seconds.
     * @param[in] nsub Required cycle base-step count 2^(K-1), matching current tier/due-set arrays.
     * @param[in] forcing Borrowed aligned current forcing in native units; matching structure/lateral
     *     arrays and callback context remain live for this call.
     */
    void runMacroCycle(double dt0, int nsub, const FvStepForcing& forcing);

    /**
     * @brief Transfer pending local-tier water/momentum into state before synchronization/re-tiering.
     *
     * No-op without accumulator storage or clean non-TPA ledgers. Drains cell mass/momentum and node
     * water/carry, floors invalid negative water, zeroes pending arrays and refreshes regime/derived
     * cell state. Applies no time advance, friction, fresh source or extra flow-integral receipt.
     * Requires exclusive initialized state and settled-source semantics; do not discard booked water by
     * clearing accumulators without either this transfer or a matching rejected-state restoration.
     */
    void settleAccumulators();

    /**
     * @brief Snapshot selected prognostic state and accepted receipts for one retry.
     *
     * Copies cell A/Q/scalars, node volume/head, net/in/out/flood/flow-integral/carry receipts and
     * enabled TPA flags. Allocates as needed. Does not capture every diagnostic, inlet/DUMMY flag or
     * pending LTS accumulator; this is a selected internal retry snapshot rather than a complete
     * external restart.
     */
    void saveState();
    /**
     * @brief Restore the selected retry snapshot and regenerate derived cell state.
     *
     * Requires a matching saveState for the same dimensions. Restores saved conserved/node/receipt
     * arrays and TPA flags, zeroes pending diverted spill and refreshes depth/stage/velocity/closure.
     * Does not rewind work counters or every diagnostic/DUMMY/inlet flag, nor clear LTS accumulators
     * (macro rejection caller clears those). No physical time advances; vector assignments may
     * allocate.
     */
    void restoreState();

    /**
     * @brief Apply one full forward Euler operator evaluation to hydraulic/scalar state.
     *
     * Updates TPA flags, reconstruction, active face fluxes, pressure classification and node response;
     * computes old UF gradients, optional pressure predictor, donor limiter/scalars, conservative
     * cells/nodes, pressure momentum finalization and same-step TPA separation, then dispersion. A
     * false return may follow partial physical mutation; caller must restore its prior snapshot before
     * retrying. No clock/stat acceptance is committed here.
     *
     * @param[in] dt Positive finite substep duration in seconds, consistent with the current flux/state
     *     snapshot.
     * @param[in] forcing Borrowed aligned current step forcing in native units, with synchronous
     *     non-reentrant callback context live for the call.
     * @return False only for restricted pressure-candidate solve/endpoint rejection; true otherwise,
     *     without a general convergence certificate.
     */
    bool takeSubstep(double dt, const FvStepForcing& forcing);

    // -- implicit pressurized head update (slot program R2a) ----------------
    /**
     * @brief Check whether the global pressure-implicit scan finds active geometric band entry.
     *
     * This implementation does not consult sub-crown TPA flags in this early-exit scan; classification
     * predicates may recognize them separately. Reads current bound mesh/state/lists and changes no
     * state.
     * @return False when pressure-implicit is disabled; otherwise true for an active closed cell with
     *     cell_h>=y_crown.
     */
    bool anyPressurizedCell() const;
    /**
     * @brief Predict pressure-system ownership for an algebraic junction.
     *
     * Uses current forcing_ and state so the CFL feedback exemption matches the pressure fold rule.
     * Does not itself classify or solve a component.
     *
     * @param[in] n Valid initialized global node index.
     * @return True for active algebraic, non-fixed-head nodes with at least one face and every incident
     *     face kFull by PressurizedHeadSolver::faceModeOf.
     */
    bool nodePressFolded(int n) const;
    /**
     * @brief Create the synchronous nonowning pressure-pass view over current solver buffers.
     *
     * Caller must not retain it across buffer mutation/reinitialization or beyond the borrowed forcing
     * lifetime. Pressure kernels own only their documented writes; no storage ownership transfers.
     *
     * @param[in] forcing Borrowed aligned current step forcing in native units, with synchronous
     *     non-reentrant callback context live for the call.
     * @return View by value with pointers into mesh/state/options, hydraulic face/cell buffers, masks,
     *     sources and optional UF scratch.
     */
    PressurizedView pressView(const FvStepForcing& forcing);
    PressurizedHeadSolver press_;
    /// Did classify() find implicit work for the substep in flight?
    bool press_step_ = false;
    bool pressure_candidate_ = false;
    NonlinearPressureStorage::Controls pressure_controls_;
    NonlinearPressureStorage::Result pressure_result_;
    long pressure_rejections_ = 0;

    /**
     * @brief Capture the independent SSP-RK2 stage-zero state and receipt baselines.
     *
     * Copies cell/node prognostic arrays, water receipts/carry and enabled TPA flags into RK-specific
     * storage. Separate from rejection snapshot so two forward operator stages can still roll back to
     * the outer step state. Allocations can throw; no state is averaged or time advanced.
     */
    void rkSave();
    /**
     * @brief Form the SSP-RK2 accepted average and average its water receipt increments.
     *
     * Requires matching rkSave followed by two full forward steps at the same dt. Averages cell
     * A/Q/scalar fields; real node depths follow averaged volume while fixed/algebraic heads average
     * directly. Halves receipt/carry changes relative to the stage-zero baseline, restores old TPA
     * flags for regime-consistent inversion and refreshes derived depths. Does not use a midpoint
     * forcing refresh or independently certify endpoint stability; outer retry/census does that.
     *
     * @param[in] forcing Borrowed aligned current step forcing in native units, with synchronous
     *     non-reentrant callback context live for the call.
     */
    void rkAverage(const FvStepForcing& forcing);

    /**
     * @brief Reconstruct one cell or boundary ghost in the common face section.
     *
     * Requires current caches, slopes/HO2 flags, node/pass-through/regime data. Cell reconstruction
     * uses limited eta/u with exact linear bed; ghosts use prescribed node head, far pass-through cell,
     * or reflective wall. Final geometry includes dry safeguards, TPA and configured mixed-wave rules.
     * Pure reads of solver state; output references must be distinct and not alias inputs/state. No
     * ownership transfers.
     *
     * @param[in] face Valid global face index with coherent common geometry.
     * @param[in] cell Valid adjacent cell index, or negative to select the node/wall ghost.
     * @param[in] node Valid coupled node index or negative closed-wall sentinel when cell<0.
     * @param[in] zstar Common reconstructed bed elevation in ft for the final hydrostatic pass.
     * @param[in] dir Incident cell axis relative to the face, +1 or -1.
     * @param[in] u_interior Face-oriented interior/far velocity in ft/s for a ghost.
     * @param[out] out Native aggregate face state; reset to zero on measure_only.
     * @param[out] i1_unreconstructed Own-side pre-hydrostatic pressure moment in ft3, or zero on
     *     measure_only.
     * @param[out] z_side Side reconstructed bed elevation in ft, always assigned.
     * @param[in] measure_only True probes bed only and returns before closure evaluation; false fills
     *     the complete state.
     */
    void faceSide(int face, int cell, int node, double zstar, int dir, double u_interior,
                  kernels::FaceState& out, double& i1_unreconstructed, double& z_side,
                  bool measure_only) const;

    NetworkMeshData* mesh_ = nullptr;
    NetworkStateData* state_ = nullptr;

    /// The forcing for the advance() currently in flight. Held so the tiered
    /// path's face pass can apply the semi-implicit node coupling without
    /// threading it through fireFaces, which is otherwise forcing-free.
    const FvStepForcing* forcing_ = nullptr;
    FvOptions opts_{};

    // Face scratch. corr_l/corr_r are the Audusse well-balanced corrections
    // g·(I₁(h_K) − I₁(h*_K)) — per-cell, not per-face, which is why they are
    // stored separately from the shared flux.
    std::vector<double> f_mass_, f_mom_, f_sstar_, f_corr_l_, f_corr_r_;

    /// Reconstructed species values on each side of each face, species-major
    /// [s * n_faces + f]. Filled by reconstructScalars() — this is the
    /// anti-diffusion layer, kept strictly separate from the Riemann solver:
    /// HLLC decides WHICH side is upwind (sign of S*), reconstruction decides
    /// WHAT value that side presents (plan §3.2).
    std::vector<double> f_phi_l_, f_phi_r_;

    /// Final, Zalesak-limited species flux per face, species-major.
    std::vector<double> f_phi_flux_;

    /// Zalesak scratch: low-order and antidiffusive face fluxes, the
    /// transported-diffused cell state, and the per-cell blend limits.
    std::vector<double> lo_flux_, anti_flux_, td_, anew_, rplus_, rminus_;

    /// Reconstructed face states and the raw flux result, kept so the species
    /// flux can be evaluated with the SAME wave speeds the water used. Under
    /// HLL the scalar needs sl/sr and both states, not just the mass flux.
    std::vector<kernels::FaceState> f_state_l_, f_state_r_;
    std::vector<kernels::FaceFlux> f_flux_;
    bool hllc_ = true;

    /// Limited scalar slope per cell in the cell's OWN axis (dφ/dx).
    std::vector<double> cell_slope_;

    /// Second-order (FV_ORDER 2) reconstruction slopes in each cell's OWN axis.
    /// The FREE SURFACE is reconstructed, not the depth: with η limited and the
    /// bed taken from its exact per-cell gradient, a lake at rest has zero
    /// slopes by construction and the scheme stays well balanced at second
    /// order. Reconstructing h instead would put a spurious slope in every cell
    /// sitting on a sloping bed.
    std::vector<double> cell_eta_slope_, cell_u_slope_;

    /// Per-cell "second order is admissible here" flag. Linear reconstruction
    /// assumes the cell is small enough that its state varies little across it;
    /// a cell spanning a bed FALL comparable to its own depth violates that
    /// outright (COARSE mode on a long conduit), and reconstructing the bed to
    /// the faces then produces a negative depth at the upstream end.
    std::vector<char> cell_ho2_;

    // Cell scratch.
    std::vector<double> cell_eta_; ///< z_b + h
    std::vector<double> cell_u_;   ///< Q/A, dry-guarded

    /// Closure at the CELL depth, refreshed by cacheClosure() at every write
    /// of cell_h: A(h), T(h) and I₁(h) of the cell's own section. Zero for a
    /// dry or TPA-flagged cell (neither consumer reads it then). faceSide, the
    /// census and the LTS stability bounds evaluated exactly these at the
    /// stored depth, several times per substep and once per node-solve trial;
    /// read from here the value is the same call on the same inputs, so the
    /// results are bit-identical (plan Phase 1b).
    std::vector<double> cell_ah_; ///< areaOfDepth(g, cell_h)
    std::vector<double> cell_t_;  ///< widthOfDepth(g, cell_h)
    std::vector<double> cell_i1_; ///< i1OfDepth(g, cell_h, cell_ah_)
    /**
     * @brief Cache one cell native area/width/pressure integral at its current depth.
     *
     * Wet non-TPA cells evaluate one shared closure triple into cell_ah_/cell_t_/cell_i1_; dry or TPA
     * cells cache zeros because consumers take their dedicated branches. Increments batched performance
     * counters; no heap allocation or cross-cell writes.
     *
     * @param[in] uc Valid initialized cell index owned by the calling worker.
     * @param[in] g The cell immutable initialized geometry.
     * @param[in] h Current native depth in ft, consistent with state.cell_h and regime.
     * @param[out] radius Optional non-aliasing output for the current friction radius;
     *     TPA uses full depth. Null leaves the existing closure-only path unchanged.
     */
    void cacheClosure(std::size_t uc, const FvGeometry& g, double h,
                      double* radius = nullptr) noexcept;

    /// Unsteady-friction convective term c·sgn(Vⁿ)·|∂V/∂x|ⁿ per cell
    /// (issue #156). Precomputed from a consistent old-state snapshot before
    /// each (parallel) cell-update loop so no update reads a mid-update
    /// neighbor; sized only when FvOptions::unsteady_friction != 0.
    std::vector<double> uf_grad_;

    /**
     * @brief Compute old-snapshot unsteady-friction acceleration gradients.
     *
     * Writes c*sign(u)*abs(dV/dx) in ft/s2 from the current pre-update velocity snapshot. Uses
     * same-conduit neighbors only, one-sided/own fallback at node/virtual splices, and current TPA slot
     * width for flagged celerity. Inactive, dry or zero-velocity cells receive zero. Call before any
     * cell momentum update or pressure pass that changes velocity; selected-cell mode leaves other
     * gradients unchanged.
     *
     * @param[in] cells Optional unique valid cell list; nullptr processes all cells. UF scratch must be
     *     allocated by enabled initialization.
     */
    void computeUfGradients(const std::vector<int>* cells);

    // -- TPA pressure closure (issue #156 Phase 4) ---------------------------
    bool tpa_ = false;       ///< FV_PRESSURE_CLOSURE == TPA
    bool mixed_wave_ = true; ///< slot/free interface wave bound (dev switch)
    bool mw_rh_ = true;      ///< bore-speed bound at interior slot/free faces
    bool deg1_guard_slot_ =
        true; ///< degree-1 prescribed discharge skips a slot interior (dev: DEG1=1 off)
    bool census_pass_far_ =
        true; ///< census bounds a pass-through ghost by the FAR cell it fluxes (dev: CENSUS_PASS=0 off)
    bool lts_fit_ =
        false; ///< 4c experiment: a shorter macro cycle when the full one does not fit (OPENSWMM_FV_LTS_FIT=1)
    /**
     * @brief Find the opposite cell presented through an active degree-two node ghost.
     *
     * Assumes static pass-through validation guarantees a degree-two topology; no allocation or state
     * change.
     *
     * @param[in] face Valid one-of-two incident face index.
     * @param[in] nd Valid initialized node index or a negative absent-node sentinel.
     * @return Far incident cell index, or -1 when the node is absent, not active
     *     pass-through/algebraic, or no other face is present.
     */
    int passThroughFarCell(int face, int nd) const noexcept;
    std::vector<uint8_t> tpa_scratch_; ///< previous-flag snapshot (sweep)
    std::vector<uint8_t> save_tpa_;    ///< step-rejection snapshot

    /**
     * @brief Update active cell pressure history from crown filling, venting and column separation.
     *
     * Requires enabled TPA and aligned flags/scratch/current eta. Copies old flags so transition
     * decisions do not depend on sweep order. Open cells clear, inactive cells retain history, free
     * cells enter at a_crown, and flagged cells exit below the 30-ft pressure bound or after hysteresis
     * plus an unsubmerged atmospheric connection. Transitions refresh regime-consistent
     * depth/stage/closure without changing conserved area. Called at each takeSubstep invocation,
     * including each forward Euler stage of RK2. Snapshot allocation may throw.
     */
    void updateTpaFlags();

    /**
     * @brief Test the current per-cell two-component-pressure regime.
     *
     * @param[in] uc Valid cell index; enabled TPA requires an aligned state.cell_tpa array.
     * @return True only when TPA is configured and the corresponding nonzero flag is set; false in SLOT
     *     mode.
     */
    bool tpaCell(std::size_t uc) const noexcept { return tpa_ && state_->cell_tpa[uc] != 0; }
    std::vector<double> cell_q_int_; ///< ∫(mean face mass flux) dt over the
                                     ///< routing step — the DISCHARGE the
                                     ///< report publishes; equals ∫Q dt in
                                     ///< open channels but not in the slot

    bool water_tracking_ = false;
    bool conservative_node_balance_ = false;
    std::vector<double> withdrawal_rates_, unmet_withdrawal_, save_unmet_, rk_unmet_;
    void bookFaceWater(int face, double dt);
    void bookSpliceReceipt(std::size_t vj, double flux, double dt);
    double capWithdrawal(std::size_t node, double balance, double dt, const FvStepForcing& forcing);

    AcceptedWaterVolumes water_, save_water_, rk_water_;
    void bookStructureWater(double dt, const FvStepForcing& forcing);
    double algebraicAvailableWater(std::size_t node, double dt, double pending,
                                   const std::vector<double>& cell_scales) const;

    // Node scratch.
    std::vector<double> node_exch_; ///< ∫(net inflow) dt over the routing step
    std::vector<double> node_in_, node_out_;
    std::vector<double> flood_vol_;
    std::vector<uint8_t> inlet_control_;

    // Step-rejection snapshot. A cell can cross the crown INSIDE a substep,
    // taking its celerity from the free-surface value to the slot value — a
    // factor of ~20 on a 3 ft pipe — so a step sized on the pre-step state can
    // violate CFL by more than an order of magnitude exactly when the model is
    // doing something interesting. Re-censusing the POST-step state and rolling
    // back when it disagrees is the principled fix; a heuristic fill-rate cap
    // is not, because the stiffness ratio depends on FV_SLOT_CELERITY.
    std::vector<double> save_dummy_, rk_dummy_;
    std::vector<double> save_cell_a_, save_cell_q_, save_cell_phi_;
    std::vector<double> save_node_vol_, save_node_head_;
    std::vector<double> save_exch_, save_in_, save_out_, save_flood_, save_qint_, save_carry_;

    // SSP-RK2 stage-1 snapshot. Separate from the rejection snapshot above:
    // a rejected RK2 step has to roll back to Uⁿ, and Uⁿ must survive the
    // stage-1 save that the rejection machinery performs.
    std::vector<double> rk_cell_a_, rk_cell_q_, rk_cell_phi_;
    std::vector<double> rk_node_vol_, rk_node_head_;
    std::vector<double> rk_exch_, rk_in_, rk_out_, rk_flood_, rk_qint_, rk_carry_;
    std::vector<uint8_t> rk_tpa_; ///< Uⁿ regime latch (issue #156 R4c)

    /// Accept a substep when the post-step stable step is at least this
    /// fraction of the step actually taken.
    static constexpr double kStepAcceptRatio = 0.5;
    static constexpr int kMaxStepRetries = 8;

    /// Substep floor — a loop guard only, far below any stable step this
    /// solver legitimately computes. NOT constants::MIN_TIMESTEP (0.001 s,
    /// legacy DYNWAVE routing granularity): clamping an EXPLICIT scheme's
    /// census up to that value violates CFL wherever a cell demands
    /// dt < 1 ms, which is routine at acoustic slot celerities on sub-foot
    /// cells — and was the root cause of the issue-#156 "high-celerity TPA
    /// instability" class (see the comment at the clamp site in advance()).
    static constexpr double kMinSubstep = 1.0e-6; ///< s

    // Divergence guard (issue #156 R3). The retry loop above is sound for CFL
    // violations, which shrink away with dt — but a dt-INDEPENDENT
    // amplification (measured: RK2 x TPA on the study's e3 siphon grows the
    // crown head x20-40 per millisecond at FV_CFL 0.9, 0.45 and 0.25 alike)
    // fails all 8 retries identically and the loop then ACCEPTS the diverged
    // step silently. These bounds are physical absurdities, not tolerances: no
    // water transient carries particle velocity past 1000 ft/s (celerity is
    // not particle speed), and 10,000 ft of pressure head is 300 bar. A run
    // that crosses either has left the physics and must say so instead of
    // scoring OK (P6 finding F3). Checked once per routing step, O(cells).
    static constexpr double kDivergedVelocity = 1000.0; ///< ft/s
    static constexpr double kDivergedDepth = 10000.0;   ///< ft
    int div_link_ = -1;                                 ///< engine link index, -1 = clean
    double div_value_ = 0.0;
    const char* div_what_ = nullptr;

    // Work lists (plan §5.2.1). `halo_` is the compaction safety margin: a wet
    // front advances at most CFL cells per substep, so a halo of `rebuild_
    // interval_` cells keeps a stale list conservative between rebuilds.
    std::vector<int> active_faces_;
    std::vector<char> cell_active_;
    std::vector<char> halo_prev_; ///< previous halo level (double buffer)
    bool lists_valid_ = false;
    int since_rebuild_ = 0;
    static constexpr int kRebuildInterval = 8;

    // Local time stepping (plan §3.3). Tiers are assigned only at a
    // synchronisation point — never mid-cycle — because a cell re-tiered
    // between firings would either skip a flux it owes or drain one twice.
    std::vector<std::uint8_t> cell_tier_, face_tier_, node_tier_;

    ConduitStepSink step_sink_ = nullptr; ///< see setConduitStepSink
    void* step_sink_user_ = nullptr;
    /**
     * @brief Invoke the reporting sink once for each conduit after accepted work.
     *
     * Requires non-null step_sink_ and coherent conduit ranges/tier arrays; caller checks the callback
     * before entry. Passes borrowed user context, conduit row, governing seconds and duration
     * synchronously. Sink must not throw/reenter or mutate solver state. No hydraulic changes occur.
     *
     * @param[in] dt0 Global governing dt or LTS base dt in seconds.
     * @param[in] dur Accepted duration weighting in seconds.
     * @param[in] tiered True selects the finest incident cell tier step per conduit; false uses dt0
     *     directly.
     */
    void emitConduitSteps(double dt0, double dur, bool tiered) const;
    std::vector<std::vector<int>> cells_by_tier_, faces_by_tier_, nodes_by_tier_;

    /// The K NESTED due-sets, `[j]` holding tiers 0..j concatenated. Built once
    /// per re-tier rather than per substep: the sets are a function of the tier
    /// assignment alone, and rebuilding them inside the macro cycle cost more
    /// than tiering saved — measured 64 % slower than global stepping on the
    /// reference model at Δx = 20 ft before this was hoisted.
    std::vector<std::vector<int>> due_f_upto_, due_c_upto_, due_n_upto_;

    /// Tier assignment is valid until this many macro cycles have elapsed.
    /// Re-tiering is O(cells + faces) with a grading sweep on top, so doing it
    /// every cycle dominates a small model; between re-tiers dt₀ may only
    /// SHRINK, which is what keeps a cached assignment admissible.
    bool lts_valid_ = false;
    int lts_countdown_ = 0;
    double lts_dt0_ = 0.0;
    static constexpr int kRetierEveryCycles = 8;

    /// Pending ∫F dt per control volume, in the cell's OWN axis for momentum.
    /// These are the tier-interface flux accumulators: a face books into them
    /// at ITS cadence and the owning volume drains them at its own.
    std::vector<double> acc_a_, acc_q_, acc_nvol_;

    /// Face flux already committed to an accumulator but not yet drained. The
    /// total invariant Σ(volume) + Σ(pending) is what the conservation gate
    /// checks mid-cycle; without it a snapshot taken between firings looks
    /// like a leak.
    int lts_tiers_ = 1;
    std::array<long, kMaxLtsTiers> tier_occupancy_{};

    // Statistics.
    long last_nsteps_ = 0;
    double last_h_ = 0.0;
    double suggested_h_ = 0.0;
    long total_steps_ = 0;
    long total_flux_ = 0;
    double min_h_ = 0.0;
    double sim_time_ = 0.0;
    /// LTS macro cycles that ran / were rejected by the post-cycle census.
    /// The tier histogram is filled whether or not a cycle ever fits the
    /// routing step; these say whether tiering ENGAGED.
    long n_macro_cycles_ = 0;
    long n_macro_rejected_ = 0;

    /// True while cell_h / cell_eta_ / cell_u_ are exactly what the solver's
    /// own last update derived from cell_a. advance() then skips its entry
    /// refreshDepths() — a full-mesh depth inversion that was 27-32 % of a
    /// reach deck's step in the 2026-09-11 baseline — bit-identically, since
    /// the refresh would recompute the same values from the same areas. The
    /// Router writes cell_a only before initialize() (the initial seed), so
    /// initialize()/reinitialize() are the only places that clear it; the
    /// pressurized-implicit and TPA paths keep the refresh unconditional.
    bool state_clean_ = false;
    /// True while acc_a_/acc_q_/acc_nvol_ hold flux fireFaces booked and no
    /// volume has drained. Lets settleAccumulators() skip its transfer AND the
    /// two full-mesh refreshDepths() it ends with when no macro cycle has run
    /// since the last settle — which, on every deck of the 2026-09-11
    /// baseline, was always (LTS fired 0 cycles) while costing 25-50 % of wall.
    bool acc_dirty_ = false;
    /// Per-face √(g·A·T) scratch for relaxOneNode: pass 1 evaluates it into
    /// here and pass 2 reads it, instead of evaluating the same geometry twice.
    std::vector<double> relax_beta_;

    // dt-argmin attribution (slot program R0): who owned the binding CFL
    // element, counted once per censusDt (global path) / assignTiers (LTS
    // path). `mutable` because censusDt is const; these are telemetry.
    mutable long dt_argmin_pressurized_ = 0;
    mutable long dt_argmin_band_ = 0;
    mutable long dt_argmin_free_ = 0;
    mutable long dt_argmin_node_ = 0;
    double active_sum_ = 0.0;
    double active_min_ = -1.0;
    double active_max_ = -1.0;
    long active_n_ = 0;

    /// The step the last accepted substep actually TOOK. Published as
    /// suggested_step() and therefore what Router::getAdaptiveStep offers the
    /// engine as the next routing step.
    double dt_cache_ = 0.0;

    /// The CFL bound the last census returned — deliberately distinct from
    /// dt_cache_. `dt_cache_` is clamped to the time remaining in the routing
    /// step, so caching it as the Courant bound would drag every later substep
    /// down to whatever fragment closed the previous step. Only this one may be
    /// carried across a skipped census (FV_CFL_CENSUS_INTERVAL > 1).
    double dt_census_ = 0.0;

    /// Substeps remaining before the next full Courant census. Reloaded from
    /// FV_CFL_CENSUS_INTERVAL; zero forces a census on the next substep.
    int census_count_ = 0;
};

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_EXPLICIT_FV_SOLVER_HPP
