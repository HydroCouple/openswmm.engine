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
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ISurfaceSolver.hpp"
#include "InertialEdges.hpp"
#include "../data/SolverOptions2D.hpp"   // Momentum2D

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
    // Tighten-only dt0_ refresh from CURRENT depths/speeds between rebuilds
    // (dt0_ may only grow at syncAndRebuild, which reassigns the tiers).
    void refreshDt0();
    // Fire one tier's faces over their Δt: inertial update + Froude cap +
    // face-cadence positivity share, booking ±ΔM into both side accumulators.
    // global_step = every active cell fires at this same Δt (the window tail),
    // so a face's exporter republishes its volume every time this face does
    // and the positivity share is NOT divided down by a refire ratio.
    void fireFaces(const std::vector<int>& faces, double dt_f,
                   bool global_step = false);
    // Fire one tier's cells over their Δt: gather + clear own-side face
    // accumulators, apply sources, refresh closure + Perot vector; tier-0
    // firings also evaluate the availability-clamped boundary edges.
    // tier0 = this firing carries the tier-0 cadence work (boundary edges and
    // the live junction exchange), which fires once per finest substep.
    void fireCells(const std::vector<int>& cells, double dt_c, bool tier0);
    /// Closure-specialised body of fireCells (kSwe = FULL_SWE momentum update).
    template <bool kSwe>
    void fireCellsImpl(const std::vector<int>& cells, double dt_c, bool tier0);
    // One halving-order macro cycle of nsub base substeps: tier k fires every
    // 2^k substeps.
    void runMacroCycle(double dt0, int nsub);

    MeshData*         mesh_  = nullptr;
    SurfaceStateData* state_ = nullptr;
    SolverOptions2D*  opts_  = nullptr;

    InertialEdges edges_;
    /// Per unique interior face (m²/s). LOCAL_INERTIAL: the prognostic face
    /// discharge. FULL_SWE / DIFFUSIVE_WAVE: the last evaluated face mass
    /// flux per unit width (diagnostic — published as edge_flux).
    std::vector<double>  q_;
    /// LOCAL_INERTIAL: Perot cell discharge vector (θ < 1 / ADVECTION).
    /// FULL_SWE: the PROGNOSTIC cell-mean unit discharge (h·u, h·v), m²/s.
    std::vector<double>  qcx_, qcy_;

    // -----------------------------------------------------------------------
    // Momentum closure (MOMENTUM_EQUATION, 2D_FULL_SWE plan §2.1). One
    // marcher, three face laws; everything below the face/cell kernels
    // (tiers, active sets, positivity, coupling, transport) is shared.
    // -----------------------------------------------------------------------
    Momentum2D mode_ = Momentum2D::LOCAL_INERTIAL;
    /// Mesh carries at least one quad (2D_PERF_REGRESSION_DIAGNOSIS §2 F1):
    /// resolved ONCE in initialize(). MeshData::n_quads() is an O(n_cells)
    /// scan and was being re-run at the top of every face-tier fire.
    bool       has_quads_ = false;
    /// FULL_SWE: CSR of WALL boundary slots per cell (wall_ptr_[i] ..
    /// wall_ptr_[i+1]) so the mirror-flux booking touches only the perimeter
    /// cells that have one (F5); every other cell's row is empty.
    std::vector<int>     wall_ptr_, wall_slot_;
    /// FULL_SWE momentum accumulators per face side (m⁴/s = m²·m²/s): the
    /// x/y momentum the face booked for cL / cR, gathered and cleared by the
    /// cell exactly like facc_L_/facc_R_ (same writer, same cadence).
    std::vector<double>  macc_x_L_, macc_x_R_, macc_y_L_, macc_y_R_;
    /// Momentum the last boundaryFluxSwe() call booked into qcx_/qcy_, so its
    /// caller can rescale it when the availability clamp shrinks the mass.
    /// Scalar: the boundary loop is serial (perimeter-sized).
    double               swe_bc_dqx_ = 0.0, swe_bc_dqy_ = 0.0;
    /// RECONSTRUCTION_ORDER 2 (FULL_SWE): limited Green-Gauss gradients of
    /// (η, u, v) per cell, refreshed at every face pass, and the SSP-RK2
    /// stage buffers. Empty at order 1.
    bool                 second_order_ = false;
    std::vector<double>  gex_, gey_, gux_, guy_, gvx_, gvy_;
    std::vector<double>  rk_v0_, rk_qx0_, rk_qy0_;
    /// Stage-0 copies of the per-advance ledgers (F6): members so the RK2
    /// step allocates nothing.
    std::vector<double>  rk_inf0_, rk_bc0_, rk_ex0_, rk_drawn0_;
    void computeLimitedGradientsSwe();
    /// One SSP-RK2 (Heun) step of length dt over the active lists: two
    /// forward-Euler substeps averaged with the start state; the ledgers
    /// that accumulated over both stages are halved to the trapezoidal value.
    void runRk2Step(double dt);
    /// FRONT_REBUILD: frontier_[i] = 1 for an active cell with at least one
    /// inactive neighbour (the outer ring of the halo). When such a cell
    /// crosses the activation depth the front has used up the halo, and the
    /// next macro cycle rebuilds the active set instead of waiting for the
    /// cadence (front_breach_).
    bool                 front_rebuild_ = false;
    std::vector<uint8_t> frontier_;
    bool                 front_breach_  = false;
    double               h_on_front_    = 0.0;   ///< activation depth of the last rebuild
    /// DIFFUSIVE_WAVE: per-cell largest |surface slope| over its faces, from
    /// the last rebuild/refresh — the explicit diffusion step bound's slope.
    std::vector<double>  dw_slope_;
    /// Face kernels of the three closures (fireFaces dispatches on mode_).
    void fireFacesInertial(const std::vector<int>& faces, double dt_f, bool global_step);
    void fireFacesSwe(const std::vector<int>& faces, double dt_f, bool global_step);
    void fireFacesDiffusive(const std::vector<int>& faces, double dt_f, bool global_step);
    /// Species advection + dispersion booked on face e for the mass transfer
    /// dM (m³, positive cL→cR) at face depth hf — shared by every closure.
    void bookFaceSpecies(int e, int a, int b, double dM, double hf, double dt_f,
                         bool global_step) noexcept;
    /// FULL_SWE ghost-cell Riemann boundary flux for BC entry k on cell i:
    /// returns the inflow-positive volumetric flux (m³/s) and adds the
    /// momentum change (with the bed-slope correction) to the cell's q⃗.
    double boundaryFluxSwe(std::size_t k, int i, double dt_c);
    /// DIFFUSIVE_WAVE step bound helper: refresh dw_slope_ for the active cells.
    void refreshDiffusiveSlopes();
    std::vector<uint8_t> cell_active_;
    std::vector<int>     active_cells_;
    /// Every face with both sides active, ascending — the union of the face
    /// tier lists, in the order the tier-0 list would have carried them after
    /// a collapse. Rebuilt with the tier lists; fired as one list by the
    /// window tail.
    std::vector<int>     active_faces_;
    std::vector<uint8_t> pin_t0_;       ///< cells pinned active + tier 0
                                        ///< (boundary + live coupling)

    // Tiered LTS. tier_[i] = k means cell i updates every 2^k base substeps
    // with Δt = 2^k·dt0; face tier = min of its incident cells so a face
    // always integrates at the finer cadence. Every face firing books the
    // identical ±ΔM into facc_L_/facc_R_ (single writer per face); each cell
    // applies + clears its own side at its own firing — conservation across
    // tier interfaces is exact by construction.
    /// Rebuild scratch, held as members so a rebuild allocates nothing:
    /// the seed flags (n_triangles) and the per-active-cell CFL step.
    std::vector<uint8_t> rebuild_seed_;
    std::vector<double>  rebuild_dt_cell_;
    /// Per-thread minima for the rebuild's CFL reduction (min is exact and
    /// order-independent, so any partition gives the identical dt0).
    std::vector<double>  rebuild_dt_partial_;

    std::vector<uint8_t> tier_;         ///< per-cell tier
    std::vector<uint8_t> face_tier_;    ///< per unique face
    std::vector<double>  facc_L_;       ///< pending ΔM for the cL side (m³)
    std::vector<double>  facc_R_;       ///< pending ΔM for the cR side (m³)

    // S1 — species mass rides the SAME face accumulators, one pair per
    // species, [s * ne + e]. Booked in fireFaces immediately after the volume
    // ΔM, from the FINAL qn1 (after the Froude cap and the positivity share),
    // at the exporting cell's concentration read at that same substep;
    // gathered and cleared in fireCells alongside the volume side. That is
    // D-2DT2: the species flux inherits the volume flux's tier cadence rather
    // than reproducing it, so conservation across tier interfaces is the
    // marcher's own property and not a second one to prove. Empty unless
    // `state_->transport.active()`.
    std::vector<double>  sacc_L_;
    std::vector<double>  sacc_R_;
    /// Exporter concentration for a species at a cell, read against the
    /// cell's CURRENT published volume — the same volume the positivity share
    /// budgets against, so cumulative species takes are bounded by the same
    /// β share as the water and mass cannot go negative where volume cannot.
    double donorConc(int s, int cell) const noexcept;
    /// Remove `dv_m3` of water from cell `i` at the cell's concentration and
    /// book it to `ledger` — infiltration, boundary outflow, coupling drain.
    /// Evaporation deliberately does NOT go through here: it removes volume
    /// and no mass, so the concentration rises (§2.3 of the plan).
    void sinkMassAtCellConc(int i, double dv_m3, std::vector<double>& ledger,
                            double* per_point_ledger = nullptr) noexcept;
    /// S2: rainfall of `rain_m3` on cell `i` brings species at the
    /// `[POLLUTANTS]` rain concentration; booked to the gained ledger.
    void addRainMass(int i, double rain_m3) noexcept;
    /// S3: outfall discharge onto cell `i` over `area_dt = area·dt` brings
    /// species at `transport.coupling_src` (mass-rate density); gained ledger.
    void addCouplingSourceMass(int i, double area_dt) noexcept;
    /// S4: evaporation of `evap_m3` from cell `i` leaves at the cell's own
    /// temperature (temperature row only; solutes concentrate, S1).
    void sinkTemperatureWithEvap(int i, double evap_m3) noexcept;
    /// False when every booked ΔM is known to have been consumed already —
    /// true after a GLOBAL substep, where every active face fired and then
    /// every active cell gathered both of its sides (faces touching an
    /// inactive cell carry q = 0 and were zeroed when it deactivated). Under
    /// per-routing-step coupling every substep is global, so without this the
    /// marcher walked the whole per-cell CSR once per substep to find nothing.
    bool accumulators_pending_ = false;
    std::vector<std::vector<int>> cells_by_tier_;
    std::vector<std::vector<int>> edges_by_tier_;
    double dt0_ = 0.0;                  ///< base (tier-0) step from the rebuild
    std::vector<int>     bc_cell_;      ///< cells with a non-WALL boundary edge
    std::vector<int>     bc_slot_;      ///< matching flat mesh edge slot
    std::vector<double>  bc_accum_;     ///< ∫F_applied dt per BC entry (m³),
                                        ///< inflow-positive, reset per advance
    std::vector<double>  bc_q_;         ///< prognostic boundary-edge discharge
                                        ///< (m²/s, inflow-positive). For
                                        ///< SPECIFIED_STAGE it is integrated by
                                        ///< the SAME inertial momentum law as an
                                        ///< interior face (ghost at η_bc); for
                                        ///< the prescribed-flux types it records
                                        ///< the applied per-metre discharge so
                                        ///< the Perot reconstruction sees the
                                        ///< boundary momentum either way.

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
