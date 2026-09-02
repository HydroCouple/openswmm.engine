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
 * @file SurfaceRouter2D.hpp
 * @brief Top-level orchestrator for the optional 2D surface routing module.
 *
 * @details Manages the full 2D workflow within the engine lifecycle:
 *          - Mesh topology construction (after parse)
 *          - Explicit-marcher solver initialization
 *          - Windowless co-advance coupling (sync batches), rainfall update
 *          - Statistics and finalization
 *
 *          Integrates with SWMMEngine via lifecycle hooks:
 *          initialize() → step() → finalize()
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §4.1, §8.3
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP

#include "data/MeshData.hpp"
#include "data/SurfaceStateData.hpp"
#include "data/SolverOptions2D.hpp"
#include "data/BoundaryData.hpp"
#include "data/PendingRows2D.hpp"
#include "coupling/NodeCoupling.hpp"
#include "infil/Infil2D.hpp"
#include "mesh/RainfallInterpolator.hpp"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef OPENSWMM_HAS_2D
#include "solver/ISurfaceSolver.hpp"
#endif

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::twoD {

/**
 * @brief Top-level orchestrator for the 2D surface routing module.
 */
class SurfaceRouter2D {
public:
    SurfaceRouter2D() = default;
    ~SurfaceRouter2D() = default;

    // Non-copyable, movable
    SurfaceRouter2D(const SurfaceRouter2D&) = delete;
    SurfaceRouter2D& operator=(const SurfaceRouter2D&) = delete;
    SurfaceRouter2D(SurfaceRouter2D&&) = default;
    SurfaceRouter2D& operator=(SurfaceRouter2D&&) = default;

    /**
     * @brief Initialize the 2D module after input parsing is complete.
     *
     * Builds mesh topology, vertex stencils, resolves coupling names,
     * and initializes the explicit 2D solver.
     *
     * @param ctx Simulation context (must have mesh_2d populated from parsing).
     */
    void initialize(SimulationContext& ctx);

    /**
     * @brief Advance the 2D surface routing by one SWMM routing step.
     *
     * Sequence:
     * 1. Update outfall boundary heads from 2D state (before 1D routing)
     * 2. After 1D routing: compute coupling exchange flows
     * 3. Update 2D rainfall from system gages
     * 4. Advance the explicit 2D solver by dt_swmm
     * 5. Transfer outfall discharges into 2D cells
     * 6. Update statistics
     *
     * @param ctx  Simulation context.
     * @param dt   SWMM routing timestep (seconds).
     * @param t    Current simulation time (seconds from start).
     */
    void step(SimulationContext& ctx, double dt, double t);

    /**
     * @brief Pre-routing hook: update outfall boundaries from 2D surface heads.
     *
     * Must be called BEFORE the 1D routing step, after setOutfallDepths().
     *
     * @param ctx Simulation context.
     */
    void updateOutfallsPreRouting(SimulationContext& ctx);

    /**
     * @brief Post-routing hook: compute coupling exchange and advance 2D solver.
     *
     * Must be called AFTER the 1D routing step.
     *
     * @param ctx  Simulation context.
     * @param dt   SWMM routing timestep (seconds).
     * @param t    Current simulation time (seconds from start).
     */
    void advancePostRouting(SimulationContext& ctx, double dt, double t);

    /**
     * @brief Prepare a RESET 2D forcing to apply on the next routing step.
     *
     * Flushes any already accumulated macro-window before the new one-shot is
     * recorded, then forces the next routing step to fire a 2D advance so the
     * prescription applies to future time only and is cleared after use.
     */
    void prepareOneShotForcing(SimulationContext& ctx);

    /**
     * @brief Finalize the 2D module at simulation end.
     *
     * Flushes any partial macro-step window (routing time accumulated since
     * the last 2D advance) so the 2D clock ends at the simulation end instead
     * of up to one window short, then releases the solver.
     *
     * @param ctx  Simulation context (needed for the flush advance).
     */
    void finalize(SimulationContext& ctx);

    /// Check if the 2D module is active.
    bool isActive() const noexcept { return active_; }

    /// Windowless-coupling stabilizer: per coupled node, the exchange head
    /// sensitivity G = Σ_points −∂Q/∂h_1d ≥ 0 converted to 1D units (ft³/s per
    /// ft), for the caller to add into the dynamic-wave `sumdqdh` each Picard
    /// iteration. Appends (node_idx, G) pairs; cheap (O(points)).
    void computeCouplingConductances(
        const SimulationContext& ctx,
        std::vector<std::pair<int, double>>& out) const;

    /**
     * @brief Make the parsed mesh editable without a full initialize().
     *
     * The GUI keeps the engine in OPENED (not INITIALIZED) state so 1D
     * property edits stay legal. In that state the 2D mesh is parsed but the
     * [2D_BOUNDARY_CONDITIONS] / [2D_EDGE_CONVEYANCE] rows still live in the
     * pending-row buffers, which the serializer prefers over live state. This
     * drains those rows into BoundaryData / mesh edge slots (the same drain
     * initialize() performs) so per-edge API edits take effect and are written
     * on save. No-op once drained or when no mesh is loaded.
     */
    void prepareForEdit();

    /// Access mesh data (read-only).
    const MeshData& mesh() const noexcept { return mesh_; }

    /// Access mesh data (mutable, for input parsing).
    MeshData& mesh() noexcept { return mesh_; }

    /// Access surface state (read-only).
    const SurfaceStateData& state() const noexcept { return state_; }

    /// Access surface state (mutable, for forcing).
    SurfaceStateData& state() noexcept { return state_; }

    /// Access solver options (read-only).
    const SolverOptions2D& options() const noexcept { return options_; }

    /// Access solver options (mutable).
    SolverOptions2D& options() noexcept { return options_; }

    /// Access per-edge boundary-condition data (read-only).
    const BoundaryData& boundary() const noexcept { return boundary_; }

    /// Access per-edge boundary-condition data (mutable, for forcing/parsing).
    BoundaryData& boundary() noexcept { return boundary_; }

    /// Announce that a boundary slot's TYPE changed (the API BC-type setter),
    /// so the compact non-WALL slot list the per-step boundary passes walk is
    /// rebuilt before it is next used.
    void invalidateBoundaryIndex() noexcept { bc_nonwall_dirty_ = true; }

    /// Bring the render fields (vertex heads, output gradients, vertex render
    /// depths) up to date with the current solver state if a batch has run
    /// since they were last computed. The API getters that expose those fields
    /// call this so a reader arriving between reports is never served a stale
    /// field; it is a no-op at a report instant, where the refresh already ran.
    void refreshRenderFieldsIfStale() {
        if (render_dirty_) refreshRenderFields();
    }

    /**
     * @brief Access the per-cell infiltration model (plan §5.5, track I).
     *
     * Mutable so SWMMEngine can publish it on `ctx.twod_io.infil` — the
     * [2D_INFILTRATION*] section handlers and the InpWriter reach it there.
     * The router owns the object, resolves it against the mesh in
     * initialize(), and drives its INFIL_STEP cadence in coAdvanceStep().
     */
    Infil2D& infil() noexcept { return infil_; }
    const Infil2D& infil() const noexcept { return infil_; }

    /**
     * @brief Ledger-consistent cumulative infiltrated depth per cell (m).
     *
     * @details Drains the SAME `SurfaceStateData::infil_applied` accumulator
     *          that `MassBalance2D::infil_out` books — the depth the marcher
     *          actually removed, summed as it was removed — so
     *          `sum(infilCumulative()[i] * tri_area[i]) == infil_out` holds by
     *          construction. Prefer this over `Infil2D::cumulative()`, which is
     *          the unramped capacity the kernels offered and therefore exceeds
     *          the applied loss on drying cells. Empty when no
     *          `[2D_INFILTRATION*]` model resolved.
     */
    const std::vector<double>& infilCumulative() const noexcept {
        return infil_cum_applied_;
    }

    /**
     * @brief Ledger-consistent cumulative rainfall volume per cell (m³).
     *
     * @details Booked in accumulateMassBalance() from the SAME per-cell
     *          rainfall term that feeds `MassBalance2D::rainfall_in`, so
     *          `sum(rainCumulative()[i]) == rainfall_in` holds by construction.
     *          Sized [n_triangles] from initialize(); zero-filled when no rain.
     */
    const std::vector<double>& rainCumulative() const noexcept {
        return rain_cum_;
    }

    /**
     * @brief Per-row buffer for `[2D_BOUNDARY_CONDITIONS]` parse output.
     *
     * V-E3. Populated by the input parser during reading (before the
     * mesh is finalized), drained into `boundary_` during `initialize()`
     * after `boundary_.resize()` allocates the per-edge slots. Retained
     * after the drain so serialization (InpWriter / GeoPackage) can
     * re-emit the authored rows (group label, TS-vs-constant choice).
     * Hoisted to data/PendingRows2D.hpp; alias kept for call sites.
     */
    using PendingBoundaryRow = twoD::PendingBoundaryRow;
    using PendingInitialQualityRow = twoD::PendingInitialQualityRow;
    using PendingBoundaryQualityRow = twoD::PendingBoundaryQualityRow;
    /// S1/S2: raw [2D_INITIAL_QUALITY] rows, resolved at initialize().
    std::vector<PendingInitialQualityRow>& pendingInitialQualityRows() noexcept {
        return pending_iq_rows_;
    }
    std::vector<PendingBoundaryQualityRow>& pendingBoundaryQualityRows() noexcept {
        return pending_bq_rows_;
    }
    std::vector<PendingBoundaryRow>& pendingBCRows() noexcept { return pending_bc_rows_; }
    const std::vector<PendingBoundaryRow>& pendingBCRows() const noexcept { return pending_bc_rows_; }

    /**
     * @brief Per-row buffer for `[2D_EDGE_CONVEYANCE]` parse output (§11A).
     *
     * Populated by the input parser during reading (vertices known but
     * mesh topology not yet built), drained into `mesh_.edge_conveyance`
     * during `initialize()` after `buildMeshTopology` has populated the
     * neighbour table. Mirrored to both slots of an interior edge so
     * antisymmetric FV flux integration stays mass-conservative.
     * Retained after the drain for faithful re-serialization.
     */
    using PendingEdgeConveyanceRow = twoD::PendingEdgeConveyanceRow;
    std::vector<PendingEdgeConveyanceRow>& pendingEdgeConveyanceRows() noexcept {
        return pending_edge_conveyance_rows_;
    }
    const std::vector<PendingEdgeConveyanceRow>& pendingEdgeConveyanceRows() const noexcept {
        return pending_edge_conveyance_rows_;
    }

    /// Get total 2D surface volume (sum of depth * area).
    double totalVolume() const;

    /// Get total exchange flow (sum of coupling flows, m³/s).
    double totalExchangeFlow() const;
#ifdef OPENSWMM_HAS_2D
    /// Access explicit-marcher sub-step statistics.
    long lastSolverSteps() const {
        return solver_ ? solver_->last_num_steps() : 0;
    }
    double lastSolverStepSize() const {
        return solver_ ? solver_->last_step_size() : 0.0;
    }
#else
    long lastSolverSteps() const { return 0; }
    double lastSolverStepSize() const { return 0.0; }
#endif

private:
    /// Drain pending [2D_BOUNDARY_CONDITIONS] / [2D_EDGE_CONVEYANCE] rows into
    /// BoundaryData / mesh edge slots and flip pending_rows_drained. Shared by
    /// initialize() and prepareForEdit(); idempotent.
    void drainPendingRows();

    MeshData         mesh_;
    SurfaceStateData state_;
    SolverOptions2D  options_;
    BoundaryData     boundary_;

    /// §5.5 track I — per-cell infiltration parameters, kernel state and the
    /// held rates published into state_.infil_rate. Inert (no allocation, no
    /// per-step work) until a [2D_INFILTRATION*] section resolves a model.
    Infil2D infil_;

    /// Sim time since the last Infil2D::updateRates call (D-I1 INFIL_STEP
    /// cadence). Advanced on the routing/co-advance cadence, NEVER per
    /// marcher substep.
    double infil_elapsed_ = 0.0;

    /// Per-cell APPLIED cumulative infiltrated depth (m) — see
    /// infilCumulative(). Filled in accumulateMassBalance() by draining the
    /// same SurfaceStateData::infil_applied accumulator that feeds
    /// MassBalance2D::infil_out; empty when no [2D_INFILTRATION*] model
    /// resolved.
    std::vector<double> infil_cum_applied_;

    /// Per-cell cumulative rainfall volume (m³) — see rainCumulative().
    /// Filled in accumulateMassBalance() alongside MassBalance2D::rainfall_in.
    std::vector<double> rain_cum_;

    /// V-E3 — parse-time scratch for [2D_BOUNDARY_CONDITIONS] rows.
    std::vector<PendingBoundaryRow> pending_bc_rows_;
    std::vector<PendingInitialQualityRow> pending_iq_rows_;   ///< S1/S2
    std::vector<PendingBoundaryQualityRow> pending_bq_rows_;  ///< S2

    /// §11A — parse-time scratch for [2D_EDGE_CONVEYANCE] rows.
    /// Drained in initialize() into mesh_.edge_conveyance after
    /// buildMeshTopology populates the neighbour table.
    std::vector<PendingEdgeConveyanceRow> pending_edge_conveyance_rows_;

    std::vector<CouplingPoint> coupling_points_;
    /// Non-outfall coupling points only — the live in-marcher exchange list.
    /// Stable storage that state_.node_coupling points at; built once in
    /// initialize().
    std::vector<CouplingPoint> node_coupling_points_;

    /// Sim time since the last statistics/continuity sample (30 s cadence).
    double co_refresh_elapsed_ = 0.0;
    /// Sim time since the last RENDER-field refresh (vertex heads, gradients,
    /// render depths). Those fields have exactly two consumers — the output
    /// snapshot and the bulk API getters — so they are refreshed on the
    /// REPORT_STEP grid, and on demand for an API reader that arrives between
    /// reports (render_dirty_).
    double co_render_elapsed_ = 0.0;
    bool   render_dirty_      = false;
    /// Per-cell free-surface scratch for the render-depth reconstruction,
    /// held across refreshes so the pass allocates nothing.
    std::vector<double> render_eta_;
    /// Flat mesh edge slots (3*i+e) carrying a non-WALL boundary condition.
    /// The BC TYPE of a slot is fixed once the model is resolved, so the list
    /// is built once in initialize(); the per-step boundary ledger and the
    /// per-step TS/rating resolve walk it instead of all 3*n_triangles slots.
    std::vector<int> bc_nonwall_slots_;
    /// Set whenever a slot's BC TYPE changes (parse-time drain, API setter);
    /// bc_nonwall_slots_ is rebuilt on the next resolve.
    bool bc_nonwall_dirty_ = true;
    /// Per-node dedupe for the coupling term of the per-batch mass balance;
    /// a member so the batch does not build and destroy a hash set each time.
    std::unordered_set<int> mb_seen_nodes_;
    /// Sim time since the last rainfall/forcing refresh (gage-scale cadence).
    double co_forcing_elapsed_ = 0.0;
    bool   co_forcing_first_   = true;
    /// One windowless routing-step co-advance: outfall inject + rainfall + BC
    /// resolve + solver advance over exactly [t, t+dt] + ledger booking +
    /// mass balance + output refresh. Replaces the whole window state machine
    /// on the marcher path.
    void coAdvanceStep(SimulationContext& ctx, double dt, double t);
    /// True when the batch about to run (span @p dt) is the one that closes
    /// the current output-refresh window — decided BEFORE the advance so the
    /// continuity snapshot is only taken on batches that will use it.
    bool refreshDue(const SimulationContext& ctx, double dt) const;
    /// Recompute the render fields from the current state.
    void refreshRenderFields();

    bool   active_           = false;
    double sim_time_         = 0.0;
    /// Routing time accumulated since the last co-advance sync batch.
    double pending_dt_       = 0.0;

    /// OPENSWMM_2D_HEAD_RAMP experiment: per-coupling-point 1D head at the
    /// previous batch (2D metre frame) + that batch's span, for the
    /// batch-over-batch trend slope handed to the marcher.
    std::vector<double> ramp_prev_head_;
    double              ramp_prev_span_ = 0.0;

    /// Previous cumulative boundary flux (Σ edge_bc_cum_flux, m³), for the
    /// per-step delta in the global mass balance.
    double prev_boundary_cum_ = 0.0;

    /// Per-coupling-point outfall exchange volume (m³, + = 1D discharge onto
    /// the surface, − = withdrawal) accumulated per sync batch. Indexed like
    /// coupling_points_ (non-outfall slots stay 0).
    std::vector<double> window_outfall_accum_;
    /// Per-cell withdrawal budget (m³) for the CURRENT sync batch, seeded from
    /// max(0, cell volume) at the last batch boundary. Outfall withdrawals
    /// draw it down so their batch-cumulative total can never overdraw the
    /// state the batch started from.
    std::vector<double> window_avail_budget_;

    /// Seed window_avail_budget_ from the current state and zero the outfall
    /// accumulator — called at initialize() and after every sync batch.
    void resetWindowAccumulators();
    /// Batches in which at least one outfall withdrawal hit the availability
    /// cap; reported once at finalize().
    long outfall_clamp_windows_ = 0;

    /// Simulation time of the most recent routing step (for the finalize flush).
    double last_t_ = 0.0;

    /// One-shot guard: resolve deferred boundary timeseries/curve NAMES to
    /// registry indices on the first advance (ctx.tables is populated by
    /// then), not at parse time.
    bool boundary_names_resolved_ = false;

    /// Project-display-units → SI factor for SPECIFIED_STAGE boundary heads
    /// (0.3048 for US FLOW_UNITS with a non-SI mesh file, else 1.0). Set in
    /// initialize() alongside the mesh scaling; applied once to constant
    /// heads after drainPendingRows() and at every TS lookup in
    /// resolveBoundaryValues(). Also converts the SI head back to display
    /// units for rating-curve stage-axis queries.
    double bc_stage_scale_ = 1.0;

    /// Display flow units → m³/s factor for SPECIFIED_FLOW / TS_FLOW /
    /// RATING_CURVE per-metre discharges (from FLOW_UNITS; 1.0 for CMS).
    /// Applied once to constant flows after drainPendingRows() and at every
    /// TS / rating-curve lookup.
    double bc_flow_scale_ = 1.0;
#ifdef OPENSWMM_HAS_2D
    /// Time integrator, chosen at runtime: the serial ExplicitInertialSolver,
    /// or the Kokkos marcher plugin when installed/eligible (constructed in
    /// initialize() via makeSurfaceSolver).
    std::unique_ptr<ISurfaceSolver> solver_;
#endif

    /// Update rainfall from the rain gages (natural-neighbour interpolation or
    /// the uniform SYSTEM mean, per options_.rainfall_mode).
    void updateRainfall(SimulationContext& ctx);

    /// Static per-cell rainfall-interpolation weights. Built once in
    /// initialize() (gage positions are fixed for a run); applied each step in
    /// updateRainfall() for RainfallMode::NATURAL_NEIGHBOUR.
    RainfallInterpolator interp_;

    /// Per-step scratch: each gage's current rainfall converted to m/s, indexed
    /// by global gage index. Reused across steps to avoid per-step allocation.
    std::vector<double> rain_si_;

    /// Resolve per-step boundary driving values: evaluate SPECIFIED_STAGE /
    /// SPECIFIED_FLOW timeseries at time @p t and RATING_CURVE from the boundary
    /// cell stage into edge_bc_head / edge_bc_flow (which the flux kernels read).
    /// Resolves deferred timeseries/curve names to registry indices once. No-op
    /// for WALL / NORMAL_FLOW and for constant SPECIFIED_* edges.
    void resolveBoundaryValues(SimulationContext& ctx, double t);

    /// Accumulate the global 2D mass-balance terms for one executed step
    /// into ctx.mass_balance_2d (rainfall, coupling, outfall, boundary,
    /// infiltration, latest storage) and the evaporation loss into
    /// state_.evap_loss_total.
    /// All terms in the 2D solver's SI internal units (m³).
    void accumulateMassBalance(SimulationContext& ctx, double dt);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
