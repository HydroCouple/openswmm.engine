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
 * @license  Apache-2.0
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

    /// Per-conduit flag: did HEC-5 inlet control cap this culvert's inflow at
    /// any point in the last advance()? Reported as the "Inlet Ctrl" column.
    const std::vector<uint8_t>& inlet_control() const noexcept {
        return inlet_control_;
    }

    /// Per-node flooding VOLUME (ft³) over the last advance().
    const std::vector<double>& node_flood_volume() const noexcept {
        return flood_vol_;
    }

    /// Pass-through classification (clean degree-2 junctions whose faces
    /// present the neighbouring cells to each other directly; refreshed with
    /// the structure flows). The publish path reads it to reconstruct those
    /// nodes' REPORTED heads face-consistently — the solver-internal head
    /// stays the stable wet-mean, which also serves as the ghost boundary
    /// state under LTS tier holds.
    const std::vector<std::uint8_t>& node_passthrough() const noexcept {
        return node_pass_;
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

    /// Linearize each node-coupling face's mass flux in the node head and apply
    /// the correction, so the node's response to its own inflow is damped
    /// rather than free-running (plan §7B.1).
    ///
    /// Conservation is preserved by CONSTRUCTION rather than by care: the
    /// correction is written into `f_mass_`, which is the single array both the
    /// cell update and the node update read. Damping the node head directly
    /// instead — the obvious approach — would imply a volume change different
    /// from the flux booked into the incident cells, and would trade exact mass
    /// conservation for stability, which is the one trade this solver cannot
    /// make.
    void   relaxNodeFluxes(double dt, const FvStepForcing& forcing);

    /// One node's worth of the above. Split out because the tiered path applies
    /// it per node at that node's own Δt, while the global path applies it to
    /// every node at the shared one.
    void   relaxOneNode(int node, double dt, const FvStepForcing& forcing);

    /// Algebraic junction: solve the junction's head from the instantaneous
    /// flux balance Σ sign·F(h) + q_lat + q_struct + carry/dt = 0 over its
    /// incident faces (monotone in h), re-solving the faces' Riemann problems
    /// at each trial head, and leave f_mass_ AND f_mom_ as computed at the
    /// root — mass and momentum stay jointly consistent, unlike a Δf_mass
    /// correction. Writes node_head permanently (the solve owns it); the
    /// bucket-path volume integration is skipped for these nodes and the small
    /// residual is carried in node_carry_ (see updateNodes) so conservation is
    /// exact without reintroducing a storage timescale. A PASS-THROUGH node
    /// (node_pass_) skips the solve entirely: its faces present the two
    /// neighbouring cells to each other directly, so there is no balance left
    /// to satisfy and the head is set to the mean adjacent stage, purely as a
    /// diagnostic.
    void   solveAlgebraicNode(int node, double dt, const FvStepForcing& forcing);

    /// Dispose of an algebraic junction's window residual: flooding at the
    /// sealed ceiling, real ponded storage at a pondable rim (demotes the node
    /// to the bucket path), or the carry ledger. Head is never recomputed
    /// here — the solve owns it; volume is derived.
    void   settleAlgebraicNode(int node, double carry);

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
    /// cell_qlat_ is m³/s per cell; node_lat_div_ flags the nodes whose
    /// forcing.node_lateral must read as zero on every node path.
    std::vector<double> cell_qlat_;
    std::vector<std::uint8_t> node_lat_div_;
    double nodeLateral(const FvStepForcing& forcing,
                       std::size_t un) const noexcept {
        if (!node_lat_div_.empty() && node_lat_div_[un]) return 0.0;
        return forcing.node_lateral ? forcing.node_lateral[un] : 0.0;
    }
    /// Cached V(full_depth) per node, for the ponding demote test.
    std::vector<double> node_vfull_;

    /// Is node n running the algebraic treatment RIGHT NOW? A ponding
    /// junction above its rim holds real water on its ponded area and reverts
    /// to the bucket/ODE path until it drains back below v_full; sealed
    /// junctions never demote.
    bool algebraicActive(int n) const noexcept {
        const auto un = static_cast<std::size_t>(n);
        if (!node_alg_[un]) return false;
        if (mesh_->node_can_pond[un] &&
            state_->node_volume[un] > node_vfull_[un]) return false;
        return true;
    }
    void   updateCells(double dt, const FvStepForcing& forcing);
    void   updateNodes(double dt, const FvStepForcing& forcing);
    void   dispersionSolve(double dt);

    /// Scatter forcing.structure_flow into a per-node net source, once per
    /// forcing refresh. One array read by the node relaxation, the global node
    /// ledger and the tiered node update alike, so all three integrate the
    /// same continuity equation — the relaxation previously omitted structure
    /// flows and equilibrated the node against a residual the ledger did not
    /// integrate.
    void   refreshStructFlows(const FvStepForcing& forcing);
    std::vector<double> node_qstruct_;

    double nodeDepthFromVolume(int node, double volume) const;

    /// dV/dH at an arbitrary depth — what the semi-implicit node correction is
    /// damped against once Picard sweeping has moved the head off the one
    /// `node_surf_area` was refreshed at.
    double nodeStorageSlope(int node, double depth) const;

    /// Is this face's flux the one being computed and booked RIGHT NOW?
    ///
    /// Under LTS a node's incident faces can sit in different tiers, so only
    /// some of them are firing on any given base step. A face that is not
    /// firing holds the flux it will book at its NEXT firing, over its own
    /// 2^k*dt0 window — recomputing it there would silently re-time a flux
    /// that belongs to a different window, which is the one property the
    /// macro cycle's face-open/volume-close offset exists to guarantee.
    ///
    /// Picard's flux re-solve is therefore restricted to live faces. The
    /// residual still sums over ALL incident faces (a held flux is the best
    /// estimate of what that face is carrying), and the linear correction is
    /// still written to all of them, exactly as the single-sweep scheme did —
    /// only the re-solve is gated. On the global path every face is live.
    /// Can the LTS macro cycle run under the current options and state?
    /// One predicate shared by advance() and censusDt(): tiering is off under
    /// RK2 (the two stages must share one Δt) and under transport (the FCT
    /// sweep is synchronous). censusDt() skips the node stability term only
    /// when tiering can actually supply the node its own fine tier — gating
    /// that skip on the FV_LTS option alone silently dropped the node bound
    /// from the global path whenever the option was on but tiering was not
    /// running (RK2, species), reproducing FV_NODE_DT NONE unasked.
    bool ltsEligible() const noexcept {
        return opts_.lts && opts_.time_integration != TimeIntegration::RK2 &&
               state_ && state_->n_species == 0;
    }

    bool faceIsLive(int f) const noexcept {
        return all_faces_live_ ||
               (!live_stamp_.empty() &&
                live_stamp_[static_cast<std::size_t>(f)] == live_gen_);
    }

    /// Faces whose flux is being computed on this base step. Stamped rather
    /// than cleared so marking a due set is O(due), not O(faces).
    std::vector<std::uint32_t> live_stamp_;
    std::uint32_t              live_gen_ = 0;
    /// The global (non-LTS) path recomputes every face each step, so there is
    /// no window to violate and the re-solve is unrestricted there. A run can
    /// use BOTH paths — a rejected macro cycle falls through to the global one
    /// — so this is set per step rather than once.
    bool all_faces_live_ = true;

    /// Semi-implicit friction for one cell: Manning, or — for a FORCE_MAIN
    /// running full — the Hazen-Williams / Darcy-Weisbach law the section
    /// actually obeys, through the engine's own forcemain functions.
    double frictionFor(const FvGeometry& g, double q, double u, double h,
                       double dt) const;

    /// Apply the node's capacity rule to a freshly-integrated volume: ponding,
    /// surcharge depth, and the flooding that removes water once neither can
    /// take it. Mirrors DW's getFloodedDepth so a model floods and ponds the
    /// same way under both solvers; shared by the global and tiered node
    /// updates, which differ only in the Δt each node integrates over.
    ///
    /// @param v_prev  the node's volume before this update — the ponded
    ///                flooding RATE is the water crossing the rim, which needs
    ///                where the surface started.
    void applyNodeCapacity(int node, double v_prev, double& vol, double& depth);

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

    /// One forward-Euler substep of the whole operator: reconstruct, flux,
    /// positivity-limit, transport, then the cell and node updates.
    void   takeSubstep(double dt, const FvStepForcing& forcing);

    /// Snapshot / average for SSP-RK2. `rkSave` records Uⁿ and the ledger
    /// totals; `rkAverage` forms ½(Uⁿ + U⁽²⁾) and halves the ledger deltas the
    /// two stages accumulated, which is what makes the reported flow integral
    /// the average of the two stage fluxes rather than their sum.
    void   rkSave();
    void   rkAverage(const FvStepForcing& forcing);

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

    /// The forcing for the advance() currently in flight. Held so the tiered
    /// path's face pass can apply the semi-implicit node coupling without
    /// threading it through fireFaces, which is otherwise forcing-free.
    const FvStepForcing* forcing_ = nullptr;
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
    std::vector<double> cell_q_int_;   ///< ∫(mean face mass flux) dt over the
                                       ///< routing step — the DISCHARGE the
                                       ///< report publishes; equals ∫Q dt in
                                       ///< open channels but not in the slot

    // Node scratch.
    std::vector<double> node_exch_;    ///< ∫(net inflow) dt over the routing step
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
    std::vector<double> save_cell_a_, save_cell_q_, save_cell_phi_;
    std::vector<double> save_node_vol_, save_node_head_;
    std::vector<double> save_exch_, save_in_, save_out_, save_flood_, save_qint_,
                        save_carry_;

    // SSP-RK2 stage-1 snapshot. Separate from the rejection snapshot above:
    // a rejected RK2 step has to roll back to Uⁿ, and Uⁿ must survive the
    // stage-1 save that the rejection machinery performs.
    std::vector<double> rk_cell_a_, rk_cell_q_, rk_cell_phi_;
    std::vector<double> rk_node_vol_, rk_node_head_;
    std::vector<double> rk_exch_, rk_in_, rk_out_, rk_flood_, rk_qint_,
                        rk_carry_;

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
    bool   lts_valid_    = false;
    int    lts_countdown_ = 0;
    double lts_dt0_      = 0.0;
    static constexpr int kRetierEveryCycles = 8;

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
