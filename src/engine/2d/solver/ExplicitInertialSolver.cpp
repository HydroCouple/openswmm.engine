/**
 * @file ExplicitInertialSolver.cpp
 * @brief Implementation of the explicit local-inertial FV marcher with
 *        power-of-two tiered local timestepping (LTS).
 *
 * @details Marching order (halving scheme): a macro cycle of 2^{K−1} base
 *          substeps of length dt0; tier k fires every 2^k substeps with
 *          Δt = 2^k·dt0. A face belongs to the FINER of its incident cells'
 *          tiers, so it always integrates at the rate the sharper side needs,
 *          reading the coarser cell's surface frozen at that cell's last
 *          firing. Every face firing books the identical ±ΔM = q·ξ·Δt_f into
 *          per-side accumulators; each cell applies its own side at its own
 *          firing — conservation across tier interfaces is exact by
 *          construction (same FP products, booked at different times).
 *
 *          Positivity: at face cadence each exporting face is capped at
 *          (β/3)·V of its exporting cell, so a cell's ≤ 3 outgoing faces can
 *          take at most β·V per own-step — V ≥ 0 without any cross-face
 *          coordination (exact at K = 1 where V is fresh every substep; the
 *          α-margin covers within-cycle depth drift for K > 1, with a
 *          zero-floor backstop at the cell update).
 *
 * @see ExplicitInertialSolver.hpp, InertialKernels.hpp
 * @ingroup engine_2d
 */

#include "ExplicitInertialSolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/BoundaryData.hpp"
#include "InertialKernels.hpp"
#include "SurfaceFluxCalculator.hpp"   // evapSink, computeBoundaryEdgeFlux
#include "../coupling/NodeCoupling.hpp"  // computeNodeCouplingQ (live exchange)
#include "../../data/NodeData.hpp"

namespace openswmm::twoD {

namespace {
/// Active-set / tier rebuild cadence in macro cycles. Between rebuilds the
/// lists are frozen and inactive cells accumulate their (rain / held-coupling)
/// sources lazily — this is where rain-on-grid thin films become near-free.
constexpr int kRebuildEveryCycles = 4;
}  // namespace

void ExplicitInertialSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                        SolverOptions2D& opts) {
    mesh_  = &mesh;
    state_ = &state;
    opts_  = &opts;

    const int nt = mesh.n_triangles();
    if (nt <= 0) return;

    edges_.build(mesh);
    const auto ne = static_cast<std::size_t>(edges_.ne);
    q_.assign(ne, 0.0);
    facc_L_.assign(ne, 0.0);
    facc_R_.assign(ne, 0.0);
    face_tier_.assign(ne, 0);
    if (opts.theta < 1.0) {
        qcx_.assign(static_cast<std::size_t>(nt), 0.0);
        qcy_.assign(static_cast<std::size_t>(nt), 0.0);
    }
    cell_active_.assign(static_cast<std::size_t>(nt), 0);
    tier_.assign(static_cast<std::size_t>(nt), 0);

    const int K = std::clamp(opts.lts_tiers, 1, 8);
    cells_by_tier_.assign(static_cast<std::size_t>(K), {});
    edges_by_tier_.assign(static_cast<std::size_t>(K), {});

    // Non-WALL boundary entries, evaluated at their owning cell's firing.
    bc_cell_.clear();
    bc_slot_.clear();
    if (state.boundary) {
        for (int i = 0; i < nt; ++i) {
            for (int e = 0; e < 3; ++e) {
                const int idx = i * 3 + e;
                const auto ty =
                    static_cast<BoundaryType>(state.boundary->edge_bc_type[idx]);
                const bool interior = (e == 0   ? mesh.tri_nbr0[i]
                                       : e == 1 ? mesh.tri_nbr1[i]
                                                : mesh.tri_nbr2[i]) >= 0;
                if (!interior && ty != BoundaryType::WALL) {
                    bc_cell_.push_back(i);
                    bc_slot_.push_back(idx);
                }
            }
        }
    }
    bc_accum_.assign(bc_cell_.size(), 0.0);

    // Live junction exchange (windowless coupling): one ∫Q dt accumulator per
    // point; spill budget tracked per 1D node. Their cells pin to tier 0 (the
    // exchange forcing changes at the fastest cadence), as do BC cells.
    exch_.assign(state.node_coupling ? state.node_coupling->size() : 0, 0.0);
    node_drawn_.assign(state.nodes_1d ? state.nodes_1d->volume.size() : 0, 0.0);
    pin_t0_.assign(static_cast<std::size_t>(nt), 0);
    for (int i : bc_cell_) pin_t0_[static_cast<std::size_t>(i)] = 1;
    if (state.node_coupling)
        for (const auto& cp : *state.node_coupling)
            if (cp.cell_idx >= 0)
                pin_t0_[static_cast<std::size_t>(cp.cell_idx)] = 1;

    reconstructAll();
    t_last_sync_ = 0.0;
    substeps_run_ = face_passes_ = last_steps_ = 0;
    last_dt_ = 0.0;
    telemetry_.clear();
    if (const char* p = std::getenv("OPENSWMM_2D_MARCHER_TELEMETRY"))
        telemetry_path_ = p;
    initialized_ = true;
}

void ExplicitInertialSolver::reconstructAll() {
    const int nt = mesh_->n_triangles();
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
    }
}

void ExplicitInertialSolver::settleAccumulators() {
    const int nt = mesh_->n_triangles();
    const auto& ed = edges_;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        double pending = 0.0;
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            if (ed.cell_sign[p] > 0.0) {
                pending += facc_L_[e];
                facc_L_[e] = 0.0;
            } else {
                pending += facc_R_[e];
                facc_R_[e] = 0.0;
            }
        }
        if (pending == 0.0) continue;
        double v = state_->volume[i] + pending;
        state_->volume[i] = (v > 0.0) ? v : 0.0;
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
    }
}

void ExplicitInertialSolver::lazySourcesOnly(double t) {
    const int nt = mesh_->n_triangles();
    const double dt_lazy = t - t_last_sync_;
    if (dt_lazy <= 0.0) return;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        if (cell_active_[i]) continue;
        const double src =
            state_->rainfall[i] + state_->coupling_flux[i]
            - evapSink(state_->evap_rate[i], state_->depth[i],
                       opts_->dry_depth);
        if (src == 0.0) continue;
        double v = state_->volume[i] + dt_lazy * src * mesh_->tri_area[i];
        state_->volume[i] = (v > 0.0) ? v : 0.0;
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
    }
    t_last_sync_ = t;
}

void ExplicitInertialSolver::syncAndRebuild(double t) {
    settleAccumulators();
    const int nt = mesh_->n_triangles();
    const double dt_lazy = t - t_last_sync_;

    // 1. Lazy source integration on INACTIVE cells: rain + held coupling flux
    //    accumulate as pure storage (no face flux by construction).
    if (dt_lazy > 0.0) {
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
        for (int i = 0; i < nt; ++i) {
            if (cell_active_[i]) continue;
            const double src =
                state_->rainfall[i] + state_->coupling_flux[i]
                - evapSink(state_->evap_rate[i], state_->depth[i],
                           opts_->dry_depth);
            if (src == 0.0) continue;
            double v = state_->volume[i] + dt_lazy * src * mesh_->tri_area[i];
            state_->volume[i] = (v > 0.0) ? v : 0.0;
            inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                                   state_->head[i], state_->depth[i]);
        }
    }
    t_last_sync_ = t;

    // 2. Seed: hysteretic depth threshold (entering cells need h_on, active
    //    cells stay until h_off), plus concentrated sources (held coupling)
    //    and non-wall boundary cells. Rain alone does NOT activate — that is
    //    the point of the lazy tier.
    const double h_on  = opts_->h_move + 0.001;
    const double h_off = std::max(0.0, opts_->h_move - 0.001);
    std::vector<uint8_t> next(static_cast<std::size_t>(nt), 0);
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        const double thresh = cell_active_[i] ? h_off : h_on;
        if (state_->depth[i] >= thresh || state_->coupling_flux[i] != 0.0 ||
            pin_t0_[i])
            next[i] = 1;
    }

    // 3. One-ring halo so fronts can enter their neighbours within a rebuild
    //    period, then compact the work lists. A face flows only when BOTH
    //    incident cells are active — a one-sided face would export volume into
    //    a cell whose update never runs (measured as an 18 % basin loss); the
    //    halo guarantees the front always has an active receiving cell.
    cell_active_ = next;
    for (int e = 0; e < edges_.ne; ++e) {
        const int a = edges_.cL[e], b = edges_.cR[e];
        if (next[a] && !next[b]) cell_active_[b] = 1;
        else if (next[b] && !next[a]) cell_active_[a] = 1;
    }
    active_cells_.clear();
    for (int i = 0; i < nt; ++i)
        if (cell_active_[i]) active_cells_.push_back(i);

    // 4. Tier assignment from the local CFL step. Pinned to tier 0: cells with
    //    concentrated sources (coupling points) and boundary cells — their
    //    forcing changes fastest. dt0_ = the finest active requirement.
    const int K = static_cast<int>(cells_by_tier_.size());
    dt0_ = 1.0e30;
    std::vector<double> dt_cell(active_cells_.size());
    for (std::size_t k = 0; k < active_cells_.size(); ++k) {
        const int i = active_cells_[k];
        const double h = state_->depth[i];
        double speed = 0.0;
        if (!qcx_.empty() && h > 1.0e-6)
            speed = std::hypot(qcx_[i], qcy_[i]) / h;
        double dt = (h > opts_->dry_depth)
                        ? inertial::cellCflDt(opts_->cfl_number,
                                              edges_.cell_lchar[i], h, speed)
                        : 1.0e30;
        dt = std::min(dt, opts_->max_timestep);
        dt_cell[k] = dt;
        dt0_ = std::min(dt0_, dt);
    }
    if (dt0_ >= 1.0e30) dt0_ = opts_->max_timestep;   // fully quiescent

    for (auto& v : cells_by_tier_) v.clear();
    for (auto& v : edges_by_tier_) v.clear();
    for (std::size_t k = 0; k < active_cells_.size(); ++k) {
        const int i = active_cells_[k];
        int tk = 0;
        if (K > 1) {
            const double ratio = dt_cell[k] / dt0_;
            tk = (ratio >= 2.0)
                     ? std::min(K - 1, static_cast<int>(std::log2(ratio)))
                     : 0;
            if (state_->coupling_flux[i] != 0.0 || pin_t0_[i]) tk = 0;
        }
        tier_[i] = static_cast<uint8_t>(tk);
        cells_by_tier_[static_cast<std::size_t>(tk)].push_back(i);
    }

    for (int e = 0; e < edges_.ne; ++e) {
        const int a = edges_.cL[e], b = edges_.cR[e];
        if (cell_active_[a] && cell_active_[b]) {
            const auto ft = std::min(tier_[a], tier_[b]);
            face_tier_[e] = ft;
            edges_by_tier_[ft].push_back(e);
        } else {
            q_[e] = 0.0;   // walled faces carry no stale momentum
        }
    }

    telemetry_.emplace_back(t, static_cast<int>(active_cells_.size()));
}

void ExplicitInertialSolver::fireFaces(const std::vector<int>& faces,
                                       double dt_f) {
    const auto& ed = edges_;
    const int   na = static_cast<int>(faces.size());
    const double theta = opts_->theta;
    const double beta_share = opts_->exchange_beta / 3.0;

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int e = faces[static_cast<std::size_t>(k)];
        const int a = ed.cL[e], b = ed.cR[e];
        const double hf = inertial::faceFlowDepth(state_->head[a],
                                                  state_->head[b], ed.zface[e]);
        if (hf <= opts_->dry_depth) {
            q_[e] = 0.0;
            continue;
        }
        double qhat = q_[e];
        if (!qcx_.empty()) {
            const double qn = 0.5 * ((qcx_[a] + qcx_[b]) * ed.nx[e] +
                                     (qcy_[a] + qcy_[b]) * ed.ny[e]);
            qhat = theta * q_[e] + (1.0 - theta) * qn;
        }
        double deta = state_->head[b] - state_->head[a];
        if (std::fabs(deta) < inertial::kEtaDeadband) deta = 0.0;
        const double slope = deta * ed.inv_dx_normal[e];
        double qn1 = inertial::inertialFaceUpdate(q_[e], qhat, hf, dt_f, slope,
                                                 ed.n2_face[e]);
        qn1 = inertial::froudeCap(qn1, hf, opts_->froude_max);

        // Positivity at face cadence: this face may take at most a β/3 share
        // of its exporting cell's volume over the exporter's WHOLE cell cycle.
        // The exporter republishes V only at its own firings, and a finer face
        // fires 2^(k_exp − t_face) times in between — divide the share by that
        // ratio or the repeated takes drain the cell into the backstop
        // (measured: a dam-break basin discarded to exactly zero).
        const int    exp_cell = (qn1 > 0.0) ? a : b;
        const int    refire   = 1 << (tier_[exp_cell] - face_tier_[e]);
        const double budget   = beta_share / refire *
                                std::max(state_->volume[exp_cell], 0.0);
        const double take = std::fabs(qn1) * ed.xi[e] * dt_f;
        if (take > budget)
            qn1 *= (take > 0.0) ? budget / take : 0.0;
        q_[e] = qn1;

        // Book the identical ±ΔM on both sides (single writer per face).
        const double dM = qn1 * ed.xi[e] * dt_f;   // positive = cL→cR
        facc_L_[e] -= dM;
        facc_R_[e] += dM;
    }
    face_passes_ += na;
}

void ExplicitInertialSolver::fireCells(const std::vector<int>& cells,
                                       double dt_c) {
    const auto& ed = edges_;
    const int   nc = static_cast<int>(cells.size());

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < nc; ++k) {
        const int i = cells[static_cast<std::size_t>(k)];
        // Gather + clear this cell's side of every incident face accumulator.
        double flux_m3 = 0.0;
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            if (ed.cell_sign[p] > 0.0) {
                flux_m3 += facc_L_[e];
                facc_L_[e] = 0.0;
            } else {
                flux_m3 += facc_R_[e];
                facc_R_[e] = 0.0;
            }
        }
        const double src =
            state_->rainfall[i] + state_->coupling_flux[i]
            - evapSink(state_->evap_rate[i], state_->depth[i], opts_->dry_depth);
        double v = state_->volume[i] + flux_m3 +
                   dt_c * src * mesh_->tri_area[i];
#ifndef NDEBUG
        if (v < -1.0e-12) {
            static thread_local bool warned = false;
            if (!warned && std::getenv("OPENSWMM_2D_MARCHER_CHECK")) {
                std::fprintf(stderr,
                             "[marcher-check] cell %d tier %d clamped v=%.6e "
                             "(V=%.6e flux=%.6e)\n",
                             i, static_cast<int>(tier_[i]), v,
                             state_->volume[i], flux_m3);
                warned = true;
            }
        }
#endif
        state_->volume[i] = (v > 0.0) ? v : 0.0;   // backstop; the face caps
                                                   // make deficits ~impossible
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
        // Refresh this cell's Perot discharge vector at its own cadence.
        if (!qcx_.empty()) {
            double sx = 0.0, sy = 0.0;
            for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                const int    e = ed.cell_edge[p];
                const double f = ed.cell_sign[p] * q_[e] * ed.xi[e];
                sx += f * (ed.mx[e] - mesh_->tri_cx[i]);
                sy += f * (ed.my[e] - mesh_->tri_cy[i]);
            }
            const double inv_a = 1.0 / mesh_->tri_area[i];
            qcx_[i] = sx * inv_a;
            qcy_[i] = sy * inv_a;
        }
    }

    // Boundary edges owned by cells of this firing (serial: perimeter-sized).
    for (std::size_t k = 0; k < bc_cell_.size(); ++k) {
        const int i = bc_cell_[k];
        if (tier_[i] != 0 || !cell_active_[i]) continue;
        // BC cells are pinned to tier 0, so they fire with every tier-0 list;
        // guard against double-firing when called for other tiers.
        if (&cells != &cells_by_tier_[0]) continue;
        double f = computeBoundaryEdgeFlux(*mesh_, *state_, *opts_,
                                           opts_->flux_dh_eps, i, bc_slot_[k]);
        if (f == 0.0) continue;
        if (f < 0.0)   // availability clamp on outflow
            f = std::max(f, -state_->volume[i] / dt_c);
        state_->volume[i] += dt_c * f;
        bc_accum_[k] += dt_c * f;
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
    }

    // Live junction exchange at tier-0 cadence (windowless coupling): the
    // orifice law against LIVE 2D heads and the routing step's 1D heads.
    // Drains cap at the exchange-β share of the source cell; spills cap at
    // the node's stored volume for the whole advance (node_drawn_ ledger) —
    // the same water cannot spill twice within a routing step.
    if (!exch_.empty() && &cells == &cells_by_tier_[0] &&
        state_->node_coupling && state_->nodes_1d) {
        const auto& pts = *state_->node_coupling;
        for (std::size_t k = 0; k < pts.size(); ++k) {
            const auto& cp = pts[k];
            const int   ci = cp.cell_idx;
            if (ci < 0 || !cell_active_[ci]) continue;
            double Q = computeNodeCouplingQ(cp, *mesh_, *state_,
                                            *state_->nodes_1d, *opts_,
                                            nullptr);
            if (Q == 0.0) continue;
            if (Q > 0.0) {   // 2D → 1D drain: availability share of the cell
                Q = std::min(Q, opts_->exchange_beta *
                                    std::max(state_->volume[ci], 0.0) / dt_c);
            } else {         // 1D → 2D spill: node stored-volume budget
                const auto ni = static_cast<std::size_t>(cp.node_idx);
                const double avail =
                    std::max(0.0, state_->nodes_1d->volume[ni] *
                                      opts_->vol_1d_to_2d) -
                    node_drawn_[ni];
                if (avail <= 0.0) continue;
                const double want = -Q * dt_c;
                const double take = std::min(want, avail);
                node_drawn_[ni] += take;
                Q = -take / dt_c;
            }
            state_->volume[ci] -= Q * dt_c;
            if (state_->volume[ci] < 0.0) state_->volume[ci] = 0.0;
            exch_[k] += Q * dt_c;
            inertial::cellEtaDepth(*mesh_, *opts_, ci, state_->volume[ci],
                                   state_->head[ci], state_->depth[ci]);
        }
    }
}

void ExplicitInertialSolver::runMacroCycle(double dt0, int nsub) {
    const int K = static_cast<int>(cells_by_tier_.size());
    static const bool dbg_invariant = [] {
        const char* e = std::getenv("OPENSWMM_2D_MARCHER_CHECK");
        return e && e[0] == '1';
    }();
    auto invariant = [&]() -> double {
        double s = 0.0;
        for (int i = 0; i < mesh_->n_triangles(); ++i) s += state_->volume[i];
        for (int e = 0; e < edges_.ne; ++e) s += facc_L_[e] + facc_R_[e];
        return s;
    };

    for (int s = 0; s < nsub; ++s) {
        const double inv0 = dbg_invariant ? invariant() : 0.0;
        // Fire every tier due at this base substep: faces first (they read the
        // incident cells' published surfaces), then the due cells.
        for (int k = 0; k < K; ++k) {
            if (s % (1 << k)) continue;
            if (!edges_by_tier_[k].empty())
                fireFaces(edges_by_tier_[k], (1 << k) * dt0);
        }
        if (dbg_invariant) {
            const double inv1 = invariant();
            if (std::fabs(inv1 - inv0) > 1.0e-9 * (std::fabs(inv0) + 1.0))
                std::fprintf(stderr,
                             "[marcher-check] FACE phase moved invariant: "
                             "s=%d d=%.6e\n", s, inv1 - inv0);
        }
        for (int k = 0; k < K; ++k) {
            if (s % (1 << k)) continue;
            if (!cells_by_tier_[k].empty() || k == 0)
                fireCells(cells_by_tier_[k], (1 << k) * dt0);
        }
        if (dbg_invariant) {
            const double inv2 = invariant();
            if (std::fabs(inv2 - inv0) > 1.0e-9 * (std::fabs(inv0) + 1.0))
                std::fprintf(stderr,
                             "[marcher-check] CELL phase moved invariant: "
                             "s=%d d=%.6e (sources excluded? rain/bc active)\n",
                             s, inv2 - inv0);
        }
        ++substeps_run_;
        ++last_steps_;
    }
}

double ExplicitInertialSolver::advance(double t_current, double t_target) {
    if (!initialized_ || t_target <= t_current) return t_target;

    double t = t_current;
    // The lazy-source clock persists across advances (inactive cells may owe
    // sources for spans straddling advance boundaries); re-anchor only if the
    // caller's clock went backwards (hotstart / reinit).
    if (t_last_sync_ > t_current) t_last_sync_ = t_current;
    std::fill(bc_accum_.begin(), bc_accum_.end(), 0.0);
    std::fill(exch_.begin(), exch_.end(), 0.0);
    std::fill(node_drawn_.begin(), node_drawn_.end(), 0.0);
    last_steps_ = 0;
    // Rebuild cadence persists ACROSS advances: under windowless co-advance
    // the router calls advance() per ~1 s routing step, and a forced O(nt)
    // settle+rebuild per call was measured as the dominant cost at 228k
    // cells. The lazy-source clock still lands exactly (syncAndRebuild at
    // every entry whose cadence is due, plus the final landing below).
    int cycles_since_rebuild = cycles_since_rebuild_;

    while (t < t_target) {
        if (cycles_since_rebuild >= kRebuildEveryCycles) {
            syncAndRebuild(t);
            cycles_since_rebuild = 0;
        }
        const int K = static_cast<int>(cells_by_tier_.size());
        const int nsub_full = 1 << (K - 1);
        const double remaining = t_target - t;

        if (active_cells_.empty()) {
            // Quiescent: stride the window; the lazy tier keeps accumulating.
            t = t_target;
            last_dt_ = remaining;
            break;
        }

        double dt0 = std::min(dt0_, remaining);
        int    nsub = nsub_full;
        if (nsub_full * dt0_ > remaining) {
            // Tail: not enough room for a full macro cycle — degenerate to
            // global-dt stepping so the window lands exactly. Settle pending
            // transfers first: the re-tiering below invalidates the cap
            // bookkeeping of any in-flight accumulator.
            settleAccumulators();
            nsub = 1;
            for (auto& v : edges_by_tier_) v.clear();
            for (auto& v : cells_by_tier_) v.clear();
            for (int i : active_cells_) {
                tier_[i] = 0;
                cells_by_tier_[0].push_back(i);
            }
            for (int e = 0; e < edges_.ne; ++e)
                if (cell_active_[edges_.cL[e]] && cell_active_[edges_.cR[e]]) {
                    face_tier_[e] = 0;
                    edges_by_tier_[0].push_back(e);
                }
            cycles_since_rebuild = kRebuildEveryCycles;  // rebuild after tail
        }

        runMacroCycle(dt0, nsub);
        t += nsub * dt0;
        last_dt_ = dt0;
        ++cycles_since_rebuild;
    }

    cycles_since_rebuild_ = cycles_since_rebuild;
    // Final lazy-source landing: cheap when nothing is pending — the full
    // rebuild only runs on its own cadence.
    if (t_target > t_last_sync_) {
        if (cycles_since_rebuild_ >= kRebuildEveryCycles) {
            syncAndRebuild(t_target);
            cycles_since_rebuild_ = 0;
        } else {
            lazySourcesOnly(t_target);
        }
    }

    // Publish the flux picture the router's output/ledger contract reads
    // (MOMENTUM INERTIAL: no DW recompute). Interior faces re-limit q against
    // the PUBLISHED surface (the update's own clamp used the face depth it
    // saw; the subsequent cell pass moved the heads — a draining front would
    // otherwise publish a super-Froude flux inconsistent with the published
    // depths). Boundary slots carry the WINDOW-MEAN applied flux so the
    // router's −flux·dt_done booking recovers the exact ∫F_applied dt.
    std::fill(state_->edge_flux.begin(), state_->edge_flux.end(), 0.0);
    for (int e = 0; e < edges_.ne; ++e) {
        const double hf = inertial::faceFlowDepth(state_->head[edges_.cL[e]],
                                                  state_->head[edges_.cR[e]],
                                                  edges_.zface[e]);
        double qp = 0.0;
        if (hf > opts_->dry_depth)
            qp = inertial::froudeCap(q_[e], hf, opts_->froude_max);
        const double F = qp * edges_.xi[e];
        state_->edge_flux[edges_.slotL[e]] = -F;
        state_->edge_flux[edges_.slotR[e]] = +F;
    }
    const double span = t_target - t_current;
    if (span > 0.0)
        for (std::size_t k = 0; k < bc_cell_.size(); ++k)
            state_->edge_flux[bc_slot_[k]] = bc_accum_[k] / span;

    return t_target;
}

void ExplicitInertialSolver::reinitialize(double /*t0*/) {
    if (!initialized_) return;
    // External state edit (hot start / breach redo): volumes are authoritative;
    // face momentum and pending transfers are stale — drop them.
    std::fill(q_.begin(), q_.end(), 0.0);
    std::fill(facc_L_.begin(), facc_L_.end(), 0.0);
    std::fill(facc_R_.begin(), facc_R_.end(), 0.0);
    reconstructAll();
}

void ExplicitInertialSolver::resyncFromVolumes(double /*t0*/) {
    if (!initialized_) return;
    // Volumes already live in state_->volume; keep the face momentum (nothing
    // failed — this is a pure re-time on this path). Pending transfers were
    // booked into volumes at the last cell firing; accumulators stay.
    reconstructAll();
}

void ExplicitInertialSolver::finalize() {
    if (!telemetry_path_.empty() && !telemetry_.empty()) {
        if (std::FILE* f = std::fopen(telemetry_path_.c_str(), "w")) {
            std::fprintf(f, "t_s,active_cells,active_frac\n");
            const double nt = std::max(1, mesh_ ? mesh_->n_triangles() : 1);
            for (const auto& [t, n] : telemetry_)
                std::fprintf(f, "%.3f,%d,%.6f\n", t, n, n / nt);
            std::fclose(f);
        }
    }
    q_.clear(); qcx_.clear(); qcy_.clear();
    facc_L_.clear(); facc_R_.clear();
    cell_active_.clear(); active_cells_.clear();
    tier_.clear(); face_tier_.clear();
    cells_by_tier_.clear(); edges_by_tier_.clear();
    bc_cell_.clear(); bc_slot_.clear(); bc_accum_.clear();
    telemetry_.clear();
    initialized_ = false;
}

ISurfaceSolver::RunStats ExplicitInertialSolver::run_stats() const noexcept {
    RunStats s;
    s.nsteps = substeps_run_;
    s.nrhs   = face_passes_;
    s.last_h = last_dt_;
    return s;
}

} // namespace openswmm::twoD
