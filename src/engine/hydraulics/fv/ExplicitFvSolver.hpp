/**
 * @file ExplicitFvSolver.hpp
 * @brief Serial/OpenMP CPU reference implementation of the explicit FV 1D solver.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_FV_EXPLICIT_FV_SOLVER_HPP
#define OPENSWMM_ENGINE_FV_EXPLICIT_FV_SOLVER_HPP

#include <array>
#include <cstdint>
#include <vector>

#include "FvKernels.hpp"
#include "FvOptions.hpp"
#include "INetworkSolver.hpp"
#include "NetworkMeshData.hpp"

namespace openswmm::fv {

class ExplicitFvSolver : public INetworkSolver {
public:
    ExplicitFvSolver() = default;
    ~ExplicitFvSolver() override = default;

    ExplicitFvSolver(const ExplicitFvSolver&) = delete;
    ExplicitFvSolver& operator=(const ExplicitFvSolver&) = delete;

    void   initialize(NetworkMeshData& mesh, NetworkStateData& state,
                      const FvOptions& opts) override;
    double advance(double t_current, double t_target,
                   const FvStepForcing& forcing) override;
    void   reinitialize(double t0) override;
    void   finalize() override;

    long   last_num_steps() const noexcept override { return last_nsteps_; }
    double last_step_size() const noexcept override { return last_h_; }
    double suggested_step() const noexcept override { return suggested_h_; }
    RunStats run_stats() const noexcept override;
    bool   is_initialized() const noexcept override { return mesh_ != nullptr; }

    /// Per-node net exchange VOLUME (ft³) accumulated over the last advance().
    /// Signed positive INTO the node. The Router glue turns this into the node
    /// inflow/outflow rates the reporting and mass-balance paths expect.
    const std::vector<double>& node_exchange() const noexcept { return node_exch_; }

    /// Per-node conduit-face exchange VOLUME split into arriving and departing
    /// magnitudes (ft³) over the last advance(). SWMM's node ledger is not a
    /// net figure — `inflow` and `outflow` are separate magnitudes, and the
    /// mass balance books an outfall's system discharge from `inflow` alone —
    /// so the split has to be accumulated here, per substep, rather than
    /// reconstructed from the net afterwards.
    const std::vector<double>& node_inflow_volume() const noexcept { return node_in_; }
    const std::vector<double>& node_outflow_volume() const noexcept { return node_out_; }

    /// Per-node flooding VOLUME (ft³) over the last advance().
    const std::vector<double>& node_flood_volume() const noexcept {
        return flood_vol_;
    }

    /// Time-integrated cell discharge (ft³) over the last advance(), used to
    /// publish a routing-step MEAN link flow rather than an end-of-step
    /// snapshot — the snapshot aliases badly when a routing step spans many
    /// substeps.
    const std::vector<double>& cell_flow_integral() const noexcept {
        return cell_q_int_;
    }

    /// Depth → volume through the node's own storage relation. Exposed for the
    /// Router glue's seeding path and for the conservation tests.
    double nodeVolumeFromDepth(int node, double depth) const;

    /// Number of LTS tiers the last macro cycle ran with. 1 means tiering
    /// found nothing to separate and the solver took the global-dt path.
    int lts_tiers() const noexcept { return lts_tiers_; }

    /// Cells per tier over the run — the §6.12 histogram that proves tiering
    /// is genuinely active on a case rather than degenerating to K = 1.
    const std::array<long, kMaxLtsTiers>& tier_occupancy() const noexcept {
        return tier_occupancy_;
    }

private:
    // -- substep pipeline ---------------------------------------------------
    void   refreshDepths();
    void   refreshNodeAreas();
    void   rebuildActiveLists();
    double censusDt() const;
    void   reconstructState();
    void   computeFaceFlux(int face);
    void   reconstructScalars(double dt);
    void   limitSpeciesFluxes(int species, double dt);
    kernels::FaceFlux adjustedFlux(int face) const;
    void   computeFluxes();
    void   limitPositivity(double dt);
    void   updateCells(double dt, const FvStepForcing& forcing);
    void   updateNodes(double dt, const FvStepForcing& forcing);
    void   dispersionSolve(double dt);

    double nodeDepthFromVolume(int node, double volume) const;

    // -- local time stepping (plan §3.3) ------------------------------------
    /// Per-control-volume stable step, the same quantities censusDt() reduces
    /// over but kept unreduced so each cell can be tiered on its own stiffness.
    double cellStableDt(int cell) const;
    double nodeStableDt(int node) const;

    /// Partition the active mesh into power-of-two tiers. Returns the tier
    /// count K and writes the finest requirement to @p dt0. K == 1 means the
    /// mesh is uniformly stiff and the caller should take the global path.
    int    assignTiers(double& dt0);

    /// Fire every entity due at one base substep. Faces compute their flux and
    /// BOOK `±F·Δt` into both incident control volumes' accumulators; cells and
    /// nodes drain what has accumulated since they last fired. What leaves a
    /// fine cell is therefore exactly what arrives in its coarse neighbour —
    /// the tier-interface conservation contract, exact by construction rather
    /// than by tolerance.
    ///
    /// @p dt0 is the BASE step; each entity scales it by its own `2^tier`. The
    /// due sets are nested (tiers 0..j fire together), so one pass with the
    /// per-entity Δt is also what keeps positivity limiting correct: a cell
    /// whose two faces fire at different cadences must be limited against both
    /// draws at once, not once per tier.
    void   fireFaces(const std::vector<int>& faces, double dt0);
    void   fireCells(const std::vector<int>& cells, double dt0,
                     const FvStepForcing& forcing);
    void   fireNodes(const std::vector<int>& nodes, double dt0,
                     const FvStepForcing& forcing);
    void   runMacroCycle(double dt0, int nsub, const FvStepForcing& forcing);

    /// Move every pending accumulator into the state as a pure transfer — no
    /// time advance, no friction, no source. The only safe way to empty the
    /// ledger, and required before re-tiering: a flux booked at one tier's Δt
    /// cannot be drained at another's.
    void   settleAccumulators();

    /// Snapshot / roll back the full prognostic state for step rejection.
    void   saveState();
    void   restoreState();

    /// Build one side of a face. @p cell < 0 selects the node ghost state,
    /// whose depth comes from the node head and whose velocity is extrapolated
    /// from the interior cell (transmissive momentum).
    /// @param measure_only  fill only @p i1_unreconstructed and @p z_side —
    ///                       used by the first pass, which needs the
    ///                       reconstructed bed before z* can be formed.
    void   faceSide(int face, int cell, int node, double zstar, int dir,
                    double u_interior, kernels::FaceState& out,
                    double& i1_unreconstructed, double& z_side,
                    bool measure_only) const;

    NetworkMeshData*  mesh_  = nullptr;
    NetworkStateData* state_ = nullptr;
    FvOptions         opts_{};

    // Face scratch. corr_l/corr_r are the Audusse well-balanced corrections
    // g·(I₁(h_K) − I₁(h*_K)) — per-cell, not per-face, which is why they are
    // stored separately from the shared flux.
    std::vector<double> f_mass_, f_mom_, f_sstar_, f_corr_l_, f_corr_r_;
    std::vector<double> f_scale_;

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
    std::vector<kernels::FaceFlux>  f_flux_;
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
    std::vector<double> cell_eta_;     ///< z_b + h
    std::vector<double> cell_u_;       ///< Q/A, dry-guarded
    std::vector<double> cell_q_int_;   ///< ∫Q dt over the routing step

    // Node scratch.
    std::vector<double> node_exch_;    ///< ∫(net inflow) dt over the routing step
    std::vector<double> node_in_, node_out_;
    std::vector<double> flood_vol_;

    // Step-rejection snapshot. A cell can cross the crown INSIDE a substep,
    // taking its celerity from the free-surface value to the slot value — a
    // factor of ~20 on a 3 ft pipe — so a step sized on the pre-step state can
    // violate CFL by more than an order of magnitude exactly when the model is
    // doing something interesting. Re-censusing the POST-step state and rolling
    // back when it disagrees is the principled fix; a heuristic fill-rate cap
    // is not, because the stiffness ratio depends on FV_SLOT_CELERITY.
    std::vector<double> save_cell_a_, save_cell_q_, save_cell_phi_;
    std::vector<double> save_node_vol_, save_node_head_;
    std::vector<double> save_exch_, save_in_, save_out_, save_flood_, save_qint_;

    /// Accept a substep when the post-step stable step is at least this
    /// fraction of the step actually taken.
    static constexpr double kStepAcceptRatio = 0.5;
    static constexpr int    kMaxStepRetries  = 8;

    // Work lists (plan §5.2.1). `halo_` is the compaction safety margin: a wet
    // front advances at most CFL cells per substep, so a halo of `rebuild_
    // interval_` cells keeps a stale list conservative between rebuilds.
    std::vector<int>  active_faces_;
    std::vector<char> cell_active_;
    std::vector<char> halo_prev_;   ///< previous halo level (double buffer)
    bool              lists_valid_  = false;
    int               since_rebuild_ = 0;
    static constexpr int kRebuildInterval = 8;

    // Local time stepping (plan §3.3). Tiers are assigned only at a
    // synchronisation point — never mid-cycle — because a cell re-tiered
    // between firings would either skip a flux it owes or drain one twice.
    std::vector<std::uint8_t>     cell_tier_, face_tier_, node_tier_;
    std::vector<std::vector<int>> cells_by_tier_, faces_by_tier_, nodes_by_tier_;

    /// Pending ∫F dt per control volume, in the cell's OWN axis for momentum.
    /// These are the tier-interface flux accumulators: a face books into them
    /// at ITS cadence and the owning volume drains them at its own.
    std::vector<double> acc_a_, acc_q_, acc_nvol_;

    /// Face flux already committed to an accumulator but not yet drained. The
    /// total invariant Σ(volume) + Σ(pending) is what the conservation gate
    /// checks mid-cycle; without it a snapshot taken between firings looks
    /// like a leak.
    int  lts_tiers_ = 1;
    std::array<long, kMaxLtsTiers> tier_occupancy_{};

    // Statistics.
    long   last_nsteps_  = 0;
    double last_h_       = 0.0;
    double suggested_h_  = 0.0;
    long   total_steps_  = 0;
    long   total_flux_   = 0;
    double min_h_        = 0.0;
    double sim_time_     = 0.0;
    double active_sum_   = 0.0;
    double active_min_   = -1.0;
    double active_max_   = -1.0;
    long   active_n_     = 0;
    double dt_cache_     = 0.0;
    int    census_count_ = 0;
};

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_EXPLICIT_FV_SOLVER_HPP
