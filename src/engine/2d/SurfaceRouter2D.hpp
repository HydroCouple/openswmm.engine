/**
 * @file SurfaceRouter2D.hpp
 * @brief Top-level orchestrator for the optional 2D surface routing module.
 *
 * @details Manages the full 2D workflow within the engine lifecycle:
 *          - Mesh topology construction (after parse)
 *          - CVODE solver initialization
 *          - Per-step coupling, rainfall update, solver advance
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

#include "data/ActiveSetData.hpp"
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
     * and initializes the CVODE solver.
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
     * 4. Advance CVODE by dt_swmm
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

    /**
     * @brief Compute a CFL-like stability hint for the 2D domain.
     *
     * Returns an advisory maximum dt based on mesh resolution and wave speeds.
     * CVODE handles its own sub-stepping, but this prevents the coupling
     * interval from being too large. Returns a value cached at the last 2D
     * advance (the state cannot change between advances), so the per-routing-
     * step call is O(1).
     *
     * @param ctx Simulation context.
     * @return Advisory maximum timestep (seconds).
     */
    double computeCflHint(const SimulationContext& ctx) const;

    /// Check if the 2D module is active.
    bool isActive() const noexcept { return active_; }

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
    /// Access CVODE solver statistics.
    long lastCvodeSteps() const {
        return solver_ ? solver_->last_num_steps() : 0;
    }
    double lastCvodeStepSize() const {
        return solver_ ? solver_->last_step_size() : 0.0;
    }
#else
    long lastCvodeSteps() const { return 0; }
    double lastCvodeStepSize() const { return 0.0; }
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
    /// Non-outfall coupling points only, for the live macro-step path. Stable
    /// storage that state_.node_coupling points at; built once in initialize()
    /// when COUPLING_INTERVAL > 1. Empty (and state_.node_coupling == nullptr)
    /// on the default held-flux path.
    std::vector<CouplingPoint> node_coupling_points_;

    bool   active_           = false;
    double sim_time_         = 0.0;
    /// Routing time accumulated since the last 2D advance (time-based
    /// macro-step). Lets the 2D solver integrate one large adaptive window over
    /// many routing steps instead of being hard-stopped every step.
    double pending_dt_       = 0.0;
    bool   force_next_window_ = false;

    /// Previous cumulative boundary flux (Σ edge_bc_cum_flux, m³), for the
    /// per-step delta in the global mass balance.
    double prev_boundary_cum_ = 0.0;

    // ── Decoupled-timestep coupling accumulators (2026-07 plan) ─────────────
    /// Per-coupling-point junction exchange volume (m³, + = 2D→1D drain,
    /// − = 1D→2D spill) accumulated by computeCouplingExchangeStep every 1D
    /// routing step since the last fired window. Indexed like coupling_points_.
    std::vector<double> window_exchange_accum_;
    /// Per-coupling-point outfall exchange volume (m³, + = 1D discharge onto
    /// the surface, − = withdrawal) accumulated per routing step. Indexed like
    /// coupling_points_ (non-outfall slots stay 0).
    std::vector<double> window_outfall_accum_;
    /// Per-cell withdrawal budget (m³) for the CURRENT window, seeded from
    /// max(0, cell volume) at the last window boundary. Junction drains and
    /// outfall withdrawals draw it down so their window-cumulative total can
    /// never overdraw the frozen 2D state.
    std::vector<double> window_avail_budget_;
    /// Sampled per-step exchange series for the CURRENT window (published on
    /// state_.coupling_series). advancePostRouting appends one row per routing
    /// step; fireAdvanceWindow finalizes the zero-mean deviation coefficients
    /// the CVODE RHS interpolates ("interpolate the temporally misaligned
    /// fluxes" — user refinement 2026-07-20).
    CouplingForcingSeries coupling_series_;
    /// Scratch row reused by the per-step sampling (net 2D-source rates).
    std::vector<double> series_row_;
    /// At least one outfall withdrawal was clamped by the budget this window.
    bool window_had_outfall_clamp_ = false;
    /// Seed window_avail_budget_ from the current (just-accepted) 2D state,
    /// zero both accumulators, and clear the series samples — called at
    /// initialize() and after every fired window (success, failure, or
    /// quiescent skip).
    void resetWindowAccumulators();
    /// Windows in which at least one outfall withdrawal hit the availability
    /// cap; reported once at finalize().
    long outfall_clamp_windows_ = 0;
    /// 2D advance windows the solver failed to integrate (surface held frozen,
    /// exchanges un-booked); reported once at finalize().
    long failed_advance_windows_ = 0;

    /// CFL hint cached at the last 2D advance (per-cell celerity minimum over
    /// wet coupling-stencil cells); 1e30 while those are dry. Refreshed by
    /// updateCflHint().
    double cfl_hint_ = 1.0e30;
    /// Cells participating in the explicit 1D↔2D exchange (coupling-point
    /// stencils) — the only cells the CFL hint scans. Built at initialize().
    std::vector<int> cfl_cells_;
    void updateCflHint();

    /// Dry-cell active-set mask (opt-in via [2D_OPTIONS] ACTIVE_SET). Owned
    /// here; state_.active_set points at it. Rebuilt once per fired window.
    ActiveSetData active_set_;

    /// Effective time-based 2D advance window (s), resolved at initialize()
    /// from COUPLING_WINDOW / AUTO. 0 = time gating off (fire every routing
    /// step, or per COUPLING_INTERVAL when > 1). May be halved at runtime by
    /// the stability guard; recovers toward window_target_.
    double effective_window_ = 0.0;
    /// Resolved window target the stability guard recovers toward.
    double window_target_ = 0.0;
    /// Consecutive clean (non-failed) fired windows since the last halving.
    int    clean_windows_ = 0;
    /// Windows skipped entirely (dry mesh, zero sources) — diagnostics.
    long   quiescent_windows_ = 0;
    /// Simulation time of the most recent routing step (for the finalize flush).
    double last_t_ = 0.0;

    /// Integrate one accumulated macro-step window: coupling exchange, sources,
    /// solver advance (with failure handling), and booking. Extracted
    /// from advancePostRouting so finalize() can flush a partial window.
    void fireAdvanceWindow(SimulationContext& ctx, double dt, double t);

    /// One-shot guard: resolve deferred boundary timeseries/curve NAMES to
    /// registry indices on the first advance (ctx.table_names is populated by
    /// then), not at parse time.
    bool boundary_names_resolved_ = false;
#ifdef OPENSWMM_HAS_2D
    /// Time integrator, chosen at runtime. Default is the serial CPU
    /// CvodeSurfaceSolver (constructed in initialize()); a future GPU plugin
    /// backend slots in here without touching SurfaceRouter2D. See
    /// docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md §2.1.
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
    /// latest storage) and the evaporation loss into state_.evap_loss_total.
    /// All terms in the 2D solver's SI internal units (m³).
    void accumulateMassBalance(SimulationContext& ctx, double dt);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
