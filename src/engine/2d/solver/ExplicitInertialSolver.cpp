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
#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#endif
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
    // The Perot cell vectors serve the θ-blend AND the convective term, so
    // ADVECTION forces them on even at θ = 1.
    if (opts.theta < 1.0 || opts.advection) {
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
    bc_q_.assign(bc_cell_.size(), 0.0);

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

    // Seed face momentum from the optional [2D_INITIAL_VELOCITY] rows: per
    // interior face, q_e = mean of the two incident cells' (h·u, h·v)
    // projected onto the face normal (depth from the volume-primary IC the
    // router seeded before this call). t = 0 only — reinitialize() (hotstart
    // / external state edits) still zeroes face momentum. Without this a
    // depth-only IC cannot represent solutions with v(t=0) ≠ 0 (e.g. the
    // SWASHES Thacker planar oscillation).
    {
        bool any_uv = false;
        for (int i = 0; i < nt && !any_uv; ++i)
            any_uv = mesh.tri_init_u[i] != 0.0 || mesh.tri_init_v[i] != 0.0;
        if (any_uv) {
            for (int e = 0; e < edges_.ne; ++e) {
                const int a = edges_.cL[e], b = edges_.cR[e];
                const double qax = state.depth[a] * mesh.tri_init_u[a];
                const double qay = state.depth[a] * mesh.tri_init_v[a];
                const double qbx = state.depth[b] * mesh.tri_init_u[b];
                const double qby = state.depth[b] * mesh.tri_init_v[b];
                q_[e] = 0.5 * ((qax + qbx) * edges_.nx[e] +
                               (qay + qby) * edges_.ny[e]);
            }
            // Perot gather so the θ-blend and CFL speed see the seeded
            // momentum from the very first firing (same stencil as fireCells).
            if (!qcx_.empty()) {
                const auto& ed = edges_;
                for (int i = 0; i < nt; ++i) {
                    double sx = 0.0, sy = 0.0;
                    for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                        const int    e = ed.cell_edge[p];
                        const double fq = static_cast<double>(ed.cell_sign[p]) * q_[e] * ed.xi[e];
                        sx += fq * (ed.mx[e] - mesh.tri_cx[i]);
                        sy += fq * (ed.my[e] - mesh.tri_cy[i]);
                    }
                    const double inv_a = 1.0 / mesh.tri_area[i];
                    qcx_[i] = sx * inv_a;
                    qcy_[i] = sy * inv_a;
                }
            }
        }
    }
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
    if (!accumulators_pending_) return;
    accumulators_pending_ = false;
    const int nt = mesh_->n_triangles();
    const auto& ed = edges_;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        double pending = 0.0;
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            if (ed.cell_sign[p] > 0) {
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
                       opts_->dry_depth)
            - infilSink(state_->infil_rate[i], state_->depth[i],
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
                           opts_->dry_depth)
                - infilSink(state_->infil_rate[i], state_->depth[i],
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
    //    The hysteresis band scales with H_MOVE (capped at the historical
    //    ±1 mm): a fixed ±1 mm band made H_MOVE 1e-4 require 1.1 mm to
    //    activate — 10× the requested threshold — freezing wetting/drying
    //    fronts on shallow benchmarks (Thacker). Bit-identical at the
    //    default h_move = 0.003 (band = 1 mm either way).
    const double band  = std::min(0.001, 0.5 * opts_->h_move);
    const double h_on  = opts_->h_move + band;
    const double h_off = std::max(0.0, opts_->h_move - band);
    rebuild_seed_.assign(static_cast<std::size_t>(nt), 0);
    std::vector<uint8_t>& next = rebuild_seed_;
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
    const int K  = static_cast<int>(cells_by_tier_.size());
    const int na = static_cast<int>(active_cells_.size());
    dt0_ = 1.0e30;
    rebuild_dt_cell_.resize(static_cast<std::size_t>(na));
    std::vector<double>& dt_cell = rebuild_dt_cell_;
    // Parallel, exactly like its twin refreshDt0(): each iteration writes only
    // dt_cell[k], and the min is folded from per-thread partials. min over
    // doubles is exact and order-independent, so the partition cannot change
    // dt0_ — this was the longest serial stretch of the rebuild.
    const int nthr = std::max(1, opts_->num_threads);
    rebuild_dt_partial_.assign(static_cast<std::size_t>(nthr), 1.0e30);
#pragma omp parallel num_threads(nthr)
    {
#if defined(SWMM_USE_OPENMP)
        const int tid = omp_get_thread_num();
#else
        const int tid = 0;
#endif
        double local = 1.0e30;
#pragma omp for schedule(static) nowait
        for (int k = 0; k < na; ++k) {
            const int i = active_cells_[static_cast<std::size_t>(k)];
            const double h = state_->depth[i];
            double speed = 0.0;
            if (!qcx_.empty() && h > 1.0e-6)
                speed = inertial::qMagnitude(qcx_[i], qcy_[i]) / h;
            double dt = (h > opts_->dry_depth)
                            ? inertial::cellCflDt(opts_->cfl_number,
                                                  edges_.cell_lchar[i], h,
                                                  speed)
                            : 1.0e30;
            dt = std::min(dt, opts_->max_timestep);
            dt_cell[static_cast<std::size_t>(k)] = dt;
            if (dt < local) local = dt;
        }
        rebuild_dt_partial_[static_cast<std::size_t>(tid)] = local;
    }
    for (double v : rebuild_dt_partial_) dt0_ = std::min(dt0_, v);
    if (dt0_ >= 1.0e30) dt0_ = opts_->max_timestep;   // fully quiescent

    for (auto& v : cells_by_tier_) v.clear();
    for (auto& v : edges_by_tier_) v.clear();
    for (std::size_t k = 0; k < active_cells_.size(); ++k) {
        const int i = active_cells_[k];
        int tk = 0;
        if (K > 1) {
            const double ratio = dt_cell[k] / dt0_;
            // ilogb(x) IS floor(log2(x)) for finite positive x, in a few
            // cycles instead of a libm call (std::log2 measured 1.8 % of the
            // run). They can only disagree where log2's <=1 ulp error crosses
            // an integer — within an ulp of an exact power of two, where
            // ilogb is the exactly-correct one.
            tk = (ratio >= 2.0) ? std::min(K - 1, std::ilogb(ratio)) : 0;
            if (state_->coupling_flux[i] != 0.0 || pin_t0_[i]) tk = 0;
        }
        tier_[i] = static_cast<uint8_t>(tk);
        cells_by_tier_[static_cast<std::size_t>(tk)].push_back(i);
    }

    active_faces_.clear();
    for (int e = 0; e < edges_.ne; ++e) {
        const int a = edges_.cL[e], b = edges_.cR[e];
        if (cell_active_[a] && cell_active_[b]) {
            const auto ft = std::min(tier_[a], tier_[b]);
            face_tier_[e] = ft;
            edges_by_tier_[ft].push_back(e);
            active_faces_.push_back(e);
        } else {
            q_[e] = 0.0;   // walled faces carry no stale momentum
        }
    }

    for (std::size_t tk = 0;
         tk < cells_by_tier_.size() && tk < tier_occupancy_.size(); ++tk)
        tier_occupancy_[tk] += static_cast<long>(cells_by_tier_[tk].size());

    telemetry_.emplace_back(t, static_cast<int>(active_cells_.size()));
}

void ExplicitInertialSolver::refreshDt0() {
    // Between rebuilds the tier lists are frozen and depths keep evolving, so
    // a dt0_ computed up to kRebuildEveryCycles macro cycles ago can realize
    // an effective CFL well above the configured bound (measured: the
    // union-jack closed-lake seiche grows at CFL_NUMBER 0.7 with dt frozen
    // for 32 base substeps). Tightening mid-flight is unconditionally safe —
    // every tier still satisfies dt_cell ≥ 2^k·dt0 — while GROWING dt0 must
    // wait for syncAndRebuild, which reassigns the tiers.
    const int na = static_cast<int>(active_cells_.size());
    if (na == 0) return;
    double fresh = 1.0e30;
#pragma omp parallel num_threads(opts_->num_threads)
    {
        // Manual min-reduction: MSVC's default /openmp is OpenMP 2.0, which
        // lacks reduction(min:).
        double local = 1.0e30;
#pragma omp for schedule(static) nowait
        for (int k = 0; k < na; ++k) {
            const int i = active_cells_[static_cast<std::size_t>(k)];
            const double h = state_->depth[i];
            if (h <= opts_->dry_depth) continue;
            double speed = 0.0;
            if (!qcx_.empty() && h > 1.0e-6)
                speed = inertial::qMagnitude(qcx_[i], qcy_[i]) / h;
            const double dt = inertial::cellCflDt(opts_->cfl_number,
                                                  edges_.cell_lchar[i], h, speed);
            if (dt < local) local = dt;
        }
#pragma omp critical
        {
            if (local < fresh) fresh = local;
        }
    }
    if (fresh < dt0_) dt0_ = fresh;
}

void ExplicitInertialSolver::fireFaces(const std::vector<int>& faces,
                                       double dt_f, bool global_step) {
    const auto& ed = edges_;
    const int   na = static_cast<int>(faces.size());
    const double theta = opts_->theta;
    const double beta_share = opts_->exchange_beta / 3.0;
    // VFR_FACE: block/convey at the shared edge's TRUE crest via the B&S
    // Eq. 14 wetted-edge depth; MEAN keeps the centroid zface bit-identical.
    const bool vfr_face =
        (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int e = faces[static_cast<std::size_t>(k)];
        const int a = ed.cL[e], b = ed.cR[e];
        const double hf = vfr_face
            ? inertial::faceFlowDepthVfr(state_->head[a], state_->head[b],
                                         ed.ze_lo[e], ed.ze_hi[e])
            : inertial::faceFlowDepth(state_->head[a],
                                      state_->head[b], ed.zface[e]);
        if (hf <= opts_->dry_depth) {
            q_[e] = 0.0;
            continue;
        }
        double qhat  = q_[e];
        double q_mag = std::fabs(q_[e]);
        if (!qcx_.empty()) {
            const double qfx = 0.5 * (qcx_[a] + qcx_[b]);
            const double qfy = 0.5 * (qcy_[a] + qcy_[b]);
            const double qn  = qfx * ed.nx[e] + qfy * ed.ny[e];
            qhat  = theta * q_[e] + (1.0 - theta) * qn;
            // Friction magnitude: the face flow VECTOR, floored at |q_n| so
            // a face whose reconstruction lags its own discharge (front
            // arrival, first firing after activation) never under-damps.
            q_mag = std::max(q_mag, inertial::qMagnitude(qfx, qfy));
        }
        double deta = state_->head[b] - state_->head[a];
        if (std::fabs(deta) < inertial::kEtaDeadband) deta = 0.0;
        const double slope = deta * ed.inv_dx_normal[e];
        // Convective momentum flux (ADVECTION, opt-in): both cells must be
        // wet — a wet/dry front keeps the pure local-inertial law, whose
        // robustness there is the reason this scheme exists.
        double adv = 0.0;
        if (opts_->advection && !qcx_.empty()) {
            const double hL = state_->depth[a], hR = state_->depth[b];
            if (hL > opts_->dry_depth && hR > opts_->dry_depth) {
                const double unL = (qcx_[a] * ed.nx[e] + qcy_[a] * ed.ny[e]) / hL;
                const double unR = (qcx_[b] * ed.nx[e] + qcy_[b] * ed.ny[e]) / hR;
                adv = inertial::inertialAdvection(q_[e], unL, hL, unR, hR,
                                                  ed.inv_dx_normal[e]);
            }
        }
        double qn1 = inertial::inertialFaceUpdate(q_[e], qhat, hf, dt_f, slope,
                                                 ed.n2_face[e], q_mag, adv);
        qn1 = inertial::froudeCap(qn1, hf, opts_->froude_max);

        // Positivity at face cadence: this face may take at most a β/3 share
        // of its exporting cell's volume over the exporter's WHOLE cell cycle.
        // The exporter republishes V only at its own firings, and a finer face
        // fires 2^(k_exp − t_face) times in between — divide the share by that
        // ratio or the repeated takes drain the cell into the backstop
        // (measured: a dam-break basin discarded to exactly zero).
        const int    exp_cell = (qn1 > 0.0) ? a : b;
        const int    refire   =
            global_step ? 1 : (1 << (tier_[exp_cell] - face_tier_[e]));
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
    accumulators_pending_ = true;
}

void ExplicitInertialSolver::fireCells(const std::vector<int>& cells,
                                       double dt_c, bool tier0) {
    const auto& ed = edges_;
    const int   nc = static_cast<int>(cells.size());

#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < nc; ++k) {
        const int i = cells[static_cast<std::size_t>(k)];
        // ONE walk of this cell's CSR row: gather + clear its side of every
        // incident face accumulator, and (when the Perot reconstruction is
        // live) accumulate its discharge vector from the same face ids. The
        // two loops used to load cell_edge/cell_sign twice for every incident
        // face; the arms (m_e - c_i) are precomputed per CSR entry so the
        // midpoint gather and the two subtractions are gone as well.
        const bool   perot   = !qcx_.empty();
        double flux_m3 = 0.0, sx = 0.0, sy = 0.0;
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const int e = ed.cell_edge[p];
            if (ed.cell_sign[p] > 0) {
                flux_m3 += facc_L_[e];
                facc_L_[e] = 0.0;
            } else {
                flux_m3 += facc_R_[e];
                facc_R_[e] = 0.0;
            }
            if (perot) {
                const double f =
                    static_cast<double>(ed.cell_sign[p]) * q_[e] * ed.xi[e];
                sx += f * ed.cell_arm_x[p];
                sy += f * ed.cell_arm_y[p];
            }
        }
        const double src =
            state_->rainfall[i] + state_->coupling_flux[i]
            - evapSink(state_->evap_rate[i], state_->depth[i], opts_->dry_depth)
            - infilSink(state_->infil_rate[i], state_->depth[i], opts_->dry_depth);
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
        if (perot) {
            const double inv_a = 1.0 / mesh_->tri_area[i];
            qcx_[i] = sx * inv_a;
            qcy_[i] = sy * inv_a;
        }
    }

    // Boundary edges owned by cells of this firing (serial: perimeter-sized).
    const bool vfr_face_bc =
        (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    for (std::size_t k = 0; k < bc_cell_.size(); ++k) {
        const int i = bc_cell_[k];
        if (tier_[i] != 0 || !cell_active_[i]) continue;
        // BC cells are pinned to tier 0, so they fire with every tier-0 list;
        // guard against double-firing when called for other tiers.
        if (!tier0) continue;
        const int    idx = bc_slot_[k];
        const auto   bt  = static_cast<BoundaryType>(
            state_->boundary->edge_bc_type[idx]);
        const double L = mesh_->edge_length[idx];
        double f;
        if (bt == BoundaryType::SPECIFIED_STAGE && L > 1.0e-12) {
            // Inertial stage boundary: the SAME momentum law as an interior
            // face, integrated against a ghost held at the prescribed stage
            // (η = η_bc, zero-gradient q → the θ-blend collapses to the
            // face's own bc_q_). The former collapsed-Manning flux was a
            // diffusive-wave law alien to the inertial interior: its
            // conductance saturated the equilibrium clamp into a Dirichlet
            // cell and every BC-driven steady case floated one head-jump
            // (~v²/2g scale) above the prescribed stage across the single
            // interior edge feeding the BC cell.
            const double eta_bc = state_->boundary->edge_bc_head[idx];
            double hf;
            if (vfr_face_bc) {
                // B&S Eq. 14 depth of the driving surface over the edge's
                // TRUE endpoint beds (same endpoint rule as the interior
                // VFR_FACE path: edge e is opposite vertex e).
                const int vv[3] = {mesh_->tri_v0[i], mesh_->tri_v1[i],
                                   mesh_->tri_v2[i]};
                const int    e_l = idx % 3;
                const double za  = mesh_->vz[vv[(e_l + 1) % 3]];
                const double zb  = mesh_->vz[vv[(e_l + 2) % 3]];
                hf = inertial::faceDepthFromEta(
                    std::max(state_->head[i], eta_bc),
                    std::min(za, zb), std::max(za, zb));
            } else {
                hf = inertial::faceFlowDepth(state_->head[i], eta_bc,
                                             mesh_->tri_cz[i]);
            }
            if (hf <= opts_->dry_depth) {
                bc_q_[k] = 0.0;
                continue;
            }
            double deta = state_->head[i] - eta_bc;
            if (std::fabs(deta) < inertial::kEtaDeadband) deta = 0.0;
            // The ghost sits across the boundary edge at the centroid→edge
            // distance 2A/(3L) (triangle centroid is 1/3 of the height up).
            const double slope = deta * (3.0 * L) / (2.0 * mesh_->tri_area[i]);
            const double n     = mesh_->mannings_n[i];
            double qn1 = inertial::inertialFaceUpdate(
                bc_q_[k], bc_q_[k], hf, dt_c, slope, n * n,
                std::fabs(bc_q_[k]));
            qn1 = inertial::froudeCap(qn1, hf, opts_->froude_max);
            f = qn1 * L;   // inflow-positive
        } else {
            f = computeBoundaryEdgeFlux(*mesh_, *state_, *opts_,
                                        opts_->flux_dh_eps, i, idx);
        }
        if (f == 0.0) {
            bc_q_[k] = 0.0;
            continue;
        }
        // Clamp the exchange in VOLUME space and re-derive the booked flux
        // from the applied change, so booking matches application exactly
        // (no −1 ulp volume dust from the flux-space clamp).
        const double v_old = state_->volume[i];
        double v_new = v_old + dt_c * f;
        if (bt == BoundaryType::SPECIFIED_STAGE) {
            // Equilibrium clamp, kept as the tiny-cell / overshoot backstop:
            // one substep moves the cell AT MOST to the prescribed stage. At
            // the inertial law's gravity-scale takes it almost never binds.
            const double v_eq = inertial::cellVolumeFromEta(
                *mesh_, *opts_, i, state_->boundary->edge_bc_head[idx]);
            if (f < 0.0) v_new = std::max(v_new, std::min(v_old, v_eq));
            else         v_new = std::min(v_new, std::max(v_old, v_eq));
        }
        if (v_new < 0.0) v_new = 0.0;   // availability clamp (exact floor)
        f = (v_new - v_old) / dt_c;
        // Momentum matches applied mass (mirrors the interior positivity-cap
        // rescale of qn1) — the prescribed-flux types record theirs here too.
        bc_q_[k] = (L > 1.0e-12) ? f / L : 0.0;
        if (f != 0.0) {
            state_->volume[i] = v_new;
            bc_accum_[k] += dt_c * f;
            inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                                   state_->head[i], state_->depth[i]);
        }
        // Perot completion: the parallel pass above rebuilt this tier-0
        // cell's discharge vector from INTERIOR edges only, so a cell fed
        // through its boundary carried a systematic (1−θ) drag on every
        // face (the SPECIFIED_FLOW entrance jump). Add the boundary edge's
        // own contribution in the interior gather's outward-flux convention.
        if (!qcx_.empty() && bc_q_[k] != 0.0) {
            const int vv[3] = {mesh_->tri_v0[i], mesh_->tri_v1[i],
                               mesh_->tri_v2[i]};
            const int    e_l = idx % 3;
            const int    va  = vv[(e_l + 1) % 3];
            const int    vb  = vv[(e_l + 2) % 3];
            const double mxb = 0.5 * (mesh_->vx[va] + mesh_->vx[vb]);
            const double myb = 0.5 * (mesh_->vy[va] + mesh_->vy[vb]);
            const double fo    = -f;   // outward volumetric flux (m³/s)
            const double inv_a = 1.0 / mesh_->tri_area[i];
            qcx_[i] += fo * (mxb - mesh_->tri_cx[i]) * inv_a;
            qcy_[i] += fo * (myb - mesh_->tri_cy[i]) * inv_a;
        }
    }

    // Live junction exchange at tier-0 cadence (windowless coupling): the
    // orifice law against LIVE 2D heads and the routing step's 1D heads.
    // Drains cap at the exchange-β share of the source cell; spills cap at
    // the node's stored volume for the whole advance (node_drawn_ ledger) —
    // the same water cannot spill twice within a routing step.
    if (!exch_.empty() && tier0 &&
        state_->node_coupling && state_->nodes_1d) {
        const auto& pts = *state_->node_coupling;
        for (std::size_t k = 0; k < pts.size(); ++k) {
            const auto& cp = pts[k];
            const int   ci = cp.cell_idx;
            if (ci < 0 || !cell_active_[ci]) continue;
            const double h_off = (k < exch_head_slope_.size())
                                     ? exch_head_slope_[k] * exch_tau_
                                     : 0.0;
            double Q = computeNodeCouplingQ(cp, *mesh_, *state_,
                                            *state_->nodes_1d, *opts_,
                                            nullptr, h_off);
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
    // Head-ramp clock: tier-0 fires once per finest substep, so dt_c here is
    // exactly the wall the batch has advanced since the last exchange pass.
    if (tier0) exch_tau_ += dt_c;
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
                fireCells(cells_by_tier_[k], (1 << k) * dt0, k == 0);
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
    exch_tau_ = 0.0;
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
        } else {
            refreshDt0();
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

        const double dt0 = std::min(dt0_, remaining);
        int nsub = nsub_full;
        if (nsub_full * dt0_ > remaining) {
            // Tail: not enough room for a full macro cycle — step the whole
            // active set once at the global dt so the window lands exactly.
            // Settle pending transfers first: a cell about to be stepped out
            // of cadence must not carry an in-flight accumulator whose cap
            // bookkeeping assumed the tiered schedule.
            //
            // This used to be expressed by collapsing every cell and face to
            // tier 0 and running a one-substep macro cycle, which cost an
            // O(n_cells + n_faces) re-tiering and (because the tier lists had
            // been destroyed) forced a full syncAndRebuild on the next entry.
            // Under per-routing-step coupling the tail fires in EVERY window,
            // so the marcher paid one full rebuild per substep — 4,644
            // rebuilds for 4,643 substeps on the 79k-cell benchmark. Firing
            // the union lists directly is the same arithmetic in the same
            // order (the collapsed tier-0 lists WERE these lists) and leaves
            // the tiering intact, so the rebuild keeps its own cadence.
            settleAccumulators();
            nsub = 1;
            fireFaces(active_faces_, dt0, /*global_step=*/true);
            fireCells(active_cells_, dt0, /*tier0=*/true);
            accumulators_pending_ = false;   // every side just gathered
            ++substeps_run_;
            ++last_steps_;
        } else {
            runMacroCycle(dt0, nsub);
        }
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
    const bool vfr_face =
        (opts_->face_reconstruction == FaceDepth2D::VFR_FACE);
    // Only the ACTIVE faces can carry flux: a face with an inactive side had
    // its q zeroed when the side deactivated, so the old full sweep spent
    // O(n_faces) recomputing a face depth and a Froude cap in order to publish
    // the zero the fill above already wrote. Identical output, and a quiescent
    // tail (active fraction reached 0.0 % in the storm run) now costs nothing.
    for (const int e : active_faces_) {
        const double hf = vfr_face
            ? inertial::faceFlowDepthVfr(state_->head[edges_.cL[e]],
                                         state_->head[edges_.cR[e]],
                                         edges_.ze_lo[e], edges_.ze_hi[e])
            : inertial::faceFlowDepth(state_->head[edges_.cL[e]],
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
    std::fill(bc_q_.begin(), bc_q_.end(), 0.0);
    std::fill(facc_L_.begin(), facc_L_.end(), 0.0);
    std::fill(facc_R_.begin(), facc_R_.end(), 0.0);
    accumulators_pending_ = false;
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
    bc_cell_.clear(); bc_slot_.clear(); bc_accum_.clear(); bc_q_.clear();
    telemetry_.clear();
    initialized_ = false;
}

ISurfaceSolver::RunStats ExplicitInertialSolver::run_stats() const noexcept {
    RunStats s;
    s.nsteps = substeps_run_;
    s.nrhs   = face_passes_;
    s.last_h = last_dt_;

    // Marcher telemetry for the report block: active-fraction spread over the
    // rebuild samples + cumulative tier-occupancy histogram. Must be read
    // BEFORE finalize() (which clears telemetry_) — SurfaceRouter2D does.
    if (!telemetry_.empty() && mesh_ && mesh_->n_triangles() > 0) {
        const double nt = static_cast<double>(mesh_->n_triangles());
        double mn = 1.0e30, mx = -1.0e30, sum = 0.0;
        for (const auto& [t, n] : telemetry_) {
            const double frac = n / nt;
            mn = std::min(mn, frac);
            mx = std::max(mx, frac);
            sum += frac;
        }
        s.active_frac_min  = mn;
        s.active_frac_max  = mx;
        s.active_frac_mean = sum / static_cast<double>(telemetry_.size());
    }
    s.n_tiers = static_cast<int>(
        std::min(cells_by_tier_.size(), tier_occupancy_.size()));
    for (int k = 0; k < s.n_tiers; ++k)
        s.tier_cells[k] = tier_occupancy_[static_cast<std::size_t>(k)];
    return s;
}

} // namespace openswmm::twoD
