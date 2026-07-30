/**
 * @file ExplicitInertialSolver.hpp
 * @brief Explicit local-inertial FV time-marcher for the 2D surface.
 *
 * @details Phase 1 of the 2026-07-29 2D reimplementation plan. A CFL-driven
 *          explicit marcher over the unique-face layout (InertialEdges):
 *          cell volume V is the conserved state (SurfaceStateData::volume,
 *          in place), one prognostic unit-width discharge q per interior
 *          face, semi-implicit friction, positivity limiter (V ≥ 0 always —
 *          no negative-volume debt machinery), and a flux-active cell set
 *          with lazy source-only integration for rain-on-grid thin films.
 *
 *          advance(t0, t1) ALWAYS reaches t1 (the step size is known a
 *          priori from the CFL bound), so none of the router's failed/frozen/
 *          partial-window machinery ever engages on this path. No linear
 *          solver, no Jacobian, no preconditioner.
 *
 *          Tiered local timestepping (LTS_TIERS > 1) lands in Phase 2; this
 *          phase steps the whole active set at the global CFL dt.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ISurfaceSolver.hpp"
#include "InertialEdges.hpp"

namespace openswmm::twoD {

class ExplicitInertialSolver final : public ISurfaceSolver {
public:
    void initialize(MeshData& mesh, SurfaceStateData& state,
                    SolverOptions2D& opts) override;
    double advance(double t_current, double t_target) override;
    void reinitialize(double t0) override;
    void resyncFromVolumes(double t0) override;
    void finalize() override;

    long   last_num_steps() const noexcept override { return last_steps_; }
    double last_step_size() const noexcept override { return last_dt_; }
    RunStats run_stats() const noexcept override;
    bool is_initialized() const noexcept override { return initialized_; }
    const std::vector<double>& last_coupling_exchange()
        const noexcept override {
        return exch_;
    }

private:
    // Recompute η/depth from state volumes for the whole mesh.
    void reconstructAll();
    // Flush every pending face accumulator into its cell (CSR gather over ALL
    // cells) and refresh the closure of touched cells. MUST run before any
    // tier/active-set reassignment: a cell deactivated or re-tiered with a
    // pending side-accumulator strands flux whose counterpart the other side
    // already applied — the backstop then realizes it as created volume.
    void settleAccumulators();
    // Apply lazily-accumulated sources (rain/held coupling) to INACTIVE cells
    // over [t_last_sync_, t], rebuild the active cell/edge lists, and assign
    // the LTS tiers (dt0_ = finest active CFL requirement).
    void syncAndRebuild(double t);
    // Fire one tier's faces over their Δt: inertial update + Froude cap +
    // face-cadence positivity share, booking ±ΔM into both side accumulators.
    void fireFaces(const std::vector<int>& faces, double dt_f);
    // Fire one tier's cells over their Δt: gather + clear own-side face
    // accumulators, apply sources, refresh closure + Perot vector; tier-0
    // firings also evaluate the availability-clamped boundary edges.
    void fireCells(const std::vector<int>& cells, double dt_c);
    // One halving-order macro cycle of nsub base substeps: tier k fires every
    // 2^k substeps.
    void runMacroCycle(double dt0, int nsub);

    MeshData*         mesh_  = nullptr;
    SurfaceStateData* state_ = nullptr;
    SolverOptions2D*  opts_  = nullptr;

    InertialEdges edges_;
    std::vector<double>  q_;            ///< per unique interior face (m²/s)
    std::vector<double>  qcx_, qcy_;    ///< Perot cell discharge vector (θ < 1)
    std::vector<uint8_t> cell_active_;
    std::vector<int>     active_cells_;
    std::vector<uint8_t> pin_t0_;       ///< cells pinned active + tier 0
                                        ///< (boundary + live coupling)

    // Tiered LTS. tier_[i] = k means cell i updates every 2^k base substeps
    // with Δt = 2^k·dt0; face tier = min of its incident cells so a face
    // always integrates at the finer cadence. Every face firing books the
    // identical ±ΔM into facc_L_/facc_R_ (single writer per face); each cell
    // applies + clears its own side at its own firing — conservation across
    // tier interfaces is exact by construction.
    std::vector<uint8_t> tier_;         ///< per-cell tier
    std::vector<uint8_t> face_tier_;    ///< per unique face
    std::vector<double>  facc_L_;       ///< pending ΔM for the cL side (m³)
    std::vector<double>  facc_R_;       ///< pending ΔM for the cR side (m³)
    std::vector<std::vector<int>> cells_by_tier_;
    std::vector<std::vector<int>> edges_by_tier_;
    double dt0_ = 0.0;                  ///< base (tier-0) step from the rebuild
    std::vector<int>     bc_cell_;      ///< cells with a non-WALL boundary edge
    std::vector<int>     bc_slot_;      ///< matching flat mesh edge slot
    std::vector<double>  bc_accum_;     ///< ∫F_applied dt per BC entry (m³),
                                        ///< inflow-positive, reset per advance

    // Live junction exchange (windowless coupling): state_->node_coupling
    // points, evaluated at tier-0 cadence against live 2D heads and the
    // routing-step 1D heads. exch_[k] = ∫Q_k dt (m³, + = 2D→1D), reset per
    // advance; node_drawn_ caps a step's total spill at the node's stored
    // volume so fill-and-spill thrash is structurally impossible.
    std::vector<double>  exch_;
    std::vector<double>  node_drawn_;   ///< spill drawn per node this advance (m³)

public:
    /// Experimental (OPENSWMM_2D_HEAD_RAMP): per-coupling-point 1D head trend
    /// slope (m/s, 2D frame) extrapolated across the batch — the exchange
    /// evaluates against h_1d + slope·τ instead of the held batch-start head.
    /// Empty = held heads (default).
    void setExchangeHeadSlopes(std::vector<double> slopes) {
        exch_head_slope_ = std::move(slopes);
    }
private:
    std::vector<double>  exch_head_slope_;
    double               exch_tau_ = 0.0;  ///< time into current advance (s)

    double t_last_sync_ = 0.0;          ///< lazy-source clock
    int    cycles_since_rebuild_ = 1000; ///< persists across advances (co-advance)
    /// Lazy-source landing without the O(nt) rebuild (advance boundaries
    /// between rebuild cadences).
    void lazySourcesOnly(double t);
    long   substeps_run_ = 0;           ///< cumulative substeps (whole run)
    long   face_passes_  = 0;           ///< cumulative face-kernel passes
    long   last_steps_   = 0;           ///< substeps in the last advance()
    double last_dt_      = 0.0;
    bool   initialized_  = false;

    // Active-fraction telemetry: (sim time, active cells) sampled at every
    // rebuild; dumped as CSV at finalize() when OPENSWMM_2D_MARCHER_TELEMETRY
    // names a file. The Phase-1 gate reads this to verify the thin-film budget
    // assumption on the Bellinge storm slice.
    std::vector<std::pair<double, int>> telemetry_;
    std::string telemetry_path_;
    /// Cumulative rebuild-sampled cell count per LTS tier (report histogram).
    std::array<long, 8> tier_occupancy_{};
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP
