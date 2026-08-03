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
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP

#include "data/MeshData.hpp"
#include "data/SurfaceStateData.hpp"
#include "data/SolverOptions2D.hpp"
#include "data/BoundaryData.hpp"
#include "data/PendingRows2D.hpp"
#include "coupling/NodeCoupling.hpp"
#include "mesh/RainfallInterpolator.hpp"

#include <memory>
#include <unordered_map>
#include <vector>
#ifdef OPENSWMM_HAS_2D
#include "solver/ISurfaceSolver.hpp"
#include "uncertainty/DeviationOperator2D.hpp"
#include "uncertainty/MeshEigenBasis.hpp"
#include "uncertainty/SpectralROM.hpp"
#endif

namespace openswmm {
struct SimulationContext;
}

// Forward declaration — the 2D ROM holds a non-owning pointer to the 1D ROM for
// per-member heads at coupling points; no 1D uncertainty headers needed here.
namespace openswmm::uncertainty { struct SpectralROM1D; }

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

    // ------------------------------------------------------------------------
    // Surface uncertainty ROM
    // ------------------------------------------------------------------------
    // The ROM lives here rather than on a solver: this class already owns the
    // mesh, the state, and the solver, and the ROM consumes exactly those —
    // mesh geometry for its basis, post-step depth as the deterministic anchor,
    // and the solver's published coupling exchange. Both marcher backends get
    // it with no backend-side code, and nothing about it is integrator-specific.

#ifdef OPENSWMM_HAS_2D
    /// The ensemble ROM, or null when [2D_ROM] is off / the mesh is too small
    /// for an eigenbasis. Null is the normal disabled state, not an error.
    const SpectralROM* rom() const noexcept { return rom_.get(); }
    SpectralROM*       rom()       noexcept { return rom_.get(); }

    /// The eigenbasis the ROM projects onto (null until initialize()).
    const MeshEigenBasis* romBasis() const noexcept { return rom_basis_.get(); }

    /// Register the 1D network ROM so coupling fluxes use each member's own
    /// reconstructed 1D head instead of the shared deterministic head.
    /// Non-owning; the 1D ROM must outlive this router. Safe to call with null.
    void setROM1D(const openswmm::uncertainty::SpectralROM1D* r) noexcept {
        rom1d_ = r;
    }

    /// Per-point ensemble exchange-flow bounds, or an empty (invalid) struct
    /// when the ROM is off or has not applied a coupling flux yet.
    const CouplingUncertaintyOutput& couplingUncertainty() const noexcept;
#else
    void setROM1D(const openswmm::uncertainty::SpectralROM1D*) noexcept {}
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

    /// V-E3 — parse-time scratch for [2D_BOUNDARY_CONDITIONS] rows.
    std::vector<PendingBoundaryRow> pending_bc_rows_;

    /// §11A — parse-time scratch for [2D_EDGE_CONVEYANCE] rows.
    /// Drained in initialize() into mesh_.edge_conveyance after
    /// buildMeshTopology populates the neighbour table.
    std::vector<PendingEdgeConveyanceRow> pending_edge_conveyance_rows_;

    std::vector<CouplingPoint> coupling_points_;
    /// Non-outfall coupling points only — the live in-marcher exchange list.
    /// Stable storage that state_.node_coupling points at; built once in
    /// initialize().
    std::vector<CouplingPoint> node_coupling_points_;

    /// Sim time since the last co-advance output refresh (report-scale cadence).
    double co_refresh_elapsed_ = 0.0;
    /// Sim time since the last rainfall/forcing refresh (gage-scale cadence).
    double co_forcing_elapsed_ = 0.0;
    bool   co_forcing_first_   = true;
    /// One windowless routing-step co-advance: outfall inject + rainfall + BC
    /// resolve + solver advance over exactly [t, t+dt] + ledger booking +
    /// mass balance + output refresh. Replaces the whole window state machine
    /// on the marcher path.
    void coAdvanceStep(SimulationContext& ctx, double dt, double t);

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
    /// registry indices on the first advance (ctx.table_names is populated by
    /// then), not at parse time.
    bool boundary_names_resolved_ = false;
#ifdef OPENSWMM_HAS_2D
    /// Time integrator, chosen at runtime: the serial ExplicitInertialSolver,
    /// or the Kokkos marcher plugin when installed/eligible (constructed in
    /// initialize() via makeSurfaceSolver).
    std::unique_ptr<ISurfaceSolver> solver_;

    // ---- Surface uncertainty ROM (see the accessors above) ----

    /// Eigenbasis of the mesh geometric graph-Laplacian. Built once in
    /// initialize() from mesh geometry alone — it does not depend on the flow
    /// field, so it is never re-solved during the run.
    std::unique_ptr<MeshEigenBasis> rom_basis_;

    /// Deviation-form ensemble. Allocated in initialize() when
    /// options_.enable_rom and the basis built; null otherwise.
    std::unique_ptr<SpectralROM> rom_;

    /// Non-owning 1D network ROM, registered by SWMMEngine via setROM1D().
    const openswmm::uncertainty::SpectralROM1D* rom1d_ = nullptr;

    /// False until the first co-advance seeds the ROM from the initial depth
    /// field. Seeding is deferred to the first advance so the ROM anchors on
    /// state the solver has actually stepped, matching h_det's definition.
    bool rom_seeded_ = false;

    /// Sim time since the last quantile computation (report-scale cadence).
    /// Quantiles are an O(M·n log M) sort — computing them every step would
    /// dwarf the ROM's own O(M·k) advance, and nothing reads them between
    /// report boundaries.
    double rom_quantile_elapsed_ = 0.0;

    /// Scratch: deterministic coupling flow (m³/s) per live coupling point,
    /// derived from the solver's published ∫Q dt. Reused across steps.
    std::vector<double> rom_coupling_q_det_;

    /// Scratch: 1D node heads converted to the 2D metre frame. Reused.
    std::vector<double> rom_node_head_buf_;

    /// Per-cell open-boundary grounding conductance (dimensionless len/d
    /// convention), computed once in initROM() from boundary_'s non-WALL
    /// faces. Zero everywhere for a fully closed domain — grounding then
    /// contributes nothing and the basis/operator are the plain Neumann ones.
    /// Unused when options_.rom_legacy_operator is set.
    std::vector<double> rom_ground_w_;

    /// The reduced deviation operator (calibrated production rung: flow-
    /// aligned anisotropic diffusion + upwind advection). Reassembled from
    /// readable state on the report-scale cadence; never touched when
    /// options_.rom_legacy_operator is set.
    DeviationOperator2D rom_operator_;

    /// Build the eigenbasis and allocate the ensemble. No-op when the ROM is
    /// disabled or the mesh has too few cells for an eigenbasis.
    void initROM(SimulationContext& ctx);

    /// Fill rom_ground_w_ from boundary_'s non-WALL faces, scaled by
    /// options_.rom_ground_scale. See MeshEigenBasis::build's ground_w note
    /// for why deviations need a drain path at open boundaries.
    void computeGroundWeights();

    /// (Re-)assemble the reduced operator from the current mesh, basis, and
    /// readable flow state, and install it on rom_. No-op on the legacy path.
    /// Reads mesh_/state_ only.
    void refreshROMOperator();

    /// Advance the ensemble over one co-advance batch and, on the report
    /// cadence, refresh the depth quantiles. Reads state_/mesh_ only.
    void advanceROM(SimulationContext& ctx, double dt);

    /// Deviation decay coefficient K_eff [m^(4/3)/s] from mesh Manning/slope
    /// and the current depth field, or options_.rom_k_eff when that is pinned.
    /// A physical quantity — independent of which backend integrated the step.
    double computeKEff() const;

    /// (Re-)seed the ensemble from the current depth field: refit the
    /// depth-weighted eigenmodes, zero the deviations, redraw the spatial
    /// parameter fields, and record the wet-cell count.
    void seedROM();

    /// Re-seed if the wet domain has grown or shrunk past
    /// options_.rom_wet_reseed_fraction since the last seed. Basis coverage
    /// only — see that option's note.
    void maybeReseedROM(double t);

    /// Wet-cell count at the last seed, and the time of that seed.
    int    rom_wet_count_at_seed_ = 0;
    double rom_last_seed_t_       = 0.0;

    /// Apply the ensemble's per-member coupling fluxes for one batch, using the
    /// solver's published deterministic exchange as the reference the member
    /// deviations are taken against.
    void applyROMCoupling(SimulationContext& ctx, double dt);
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
    /// latest storage) and the evaporation loss into state_.evap_loss_total.
    /// All terms in the 2D solver's SI internal units (m³).
    void accumulateMassBalance(SimulationContext& ctx, double dt);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
