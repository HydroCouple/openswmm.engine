/**
 * @file ExplicitInertialSolver.cpp
 * @brief Implementation of the explicit local-inertial FV marcher.
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
#include "SurfaceFluxCalculator.hpp"   // evapSink, boundaryEdgeFlux

namespace openswmm::twoD {

namespace {
/// Rebuild cadence in substeps. Between rebuilds the active set is frozen and
/// inactive cells accumulate their (rain / held-coupling) sources lazily —
/// this is where rain-on-grid over thin films becomes near-free.
constexpr int kRebuildEvery = 16;
}  // namespace

void ExplicitInertialSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                        SolverOptions2D& opts) {
    mesh_  = &mesh;
    state_ = &state;
    opts_  = &opts;

    const int nt = mesh.n_triangles();
    if (nt <= 0) return;

    edges_.build(mesh);
    q_.assign(static_cast<std::size_t>(edges_.ne), 0.0);
    if (opts.theta < 1.0) {
        qcx_.assign(static_cast<std::size_t>(nt), 0.0);
        qcy_.assign(static_cast<std::size_t>(nt), 0.0);
    }
    cell_active_.assign(static_cast<std::size_t>(nt), 0);

    // Non-WALL boundary entries, evaluated per substep at their owning cell.
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

void ExplicitInertialSolver::syncAndRebuild(double t) {
    const int nt = mesh_->n_triangles();
    const double dt_lazy = t - t_last_sync_;

    // 1. Lazy source integration on INACTIVE cells: rain + held coupling flux
    //    accumulate as pure storage (no face flux by construction). Evaporation
    //    only draws from wet cells and thin films evaporate through the same
    //    smoothstep sink the DW path uses.
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

    // 2. Seed: hysteretic depth threshold (entering cells need h_on, cells
    //    already active stay until h_off), plus concentrated sources (held
    //    coupling) and non-wall boundary cells. Rain alone does NOT activate —
    //    that is the whole point of the lazy tier.
    const double h_on  = opts_->h_move + 0.001;
    const double h_off = std::max(0.0, opts_->h_move - 0.001);
    std::vector<uint8_t> next(static_cast<std::size_t>(nt), 0);
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int i = 0; i < nt; ++i) {
        const double thresh = cell_active_[i] ? h_off : h_on;
        if (state_->depth[i] >= thresh || state_->coupling_flux[i] != 0.0)
            next[i] = 1;
    }
    for (std::size_t k = 0; k < bc_cell_.size(); ++k)
        next[static_cast<std::size_t>(bc_cell_[k])] = 1;

    // 3. One-ring halo so fronts can enter their neighbours within a rebuild
    //    period, then compact the cell/edge work lists. Serial: O(nt) flag
    //    sweeps at kRebuildEvery cadence are noise next to the face passes.
    cell_active_ = next;
    for (int e = 0; e < edges_.ne; ++e) {
        const int a = edges_.cL[e], b = edges_.cR[e];
        if (next[a] && !next[b]) cell_active_[b] = 1;
        else if (next[b] && !next[a]) cell_active_[a] = 1;
    }
    active_cells_.clear();
    for (int i = 0; i < nt; ++i)
        if (cell_active_[i]) active_cells_.push_back(i);
    // A face flows only when BOTH incident cells are active — a one-sided
    // face would export volume into a cell whose update never runs (measured
    // as an 18 % basin loss). The 1-ring halo guarantees the front always has
    // an active receiving cell; the halo's own outer faces stay walls until
    // the next rebuild admits the next ring.
    active_edges_.clear();
    for (int e = 0; e < edges_.ne; ++e) {
        if (cell_active_[edges_.cL[e]] && cell_active_[edges_.cR[e]])
            active_edges_.push_back(e);
        else
            q_[e] = 0.0;   // walled faces carry no stale momentum
    }

    telemetry_.emplace_back(t, static_cast<int>(active_cells_.size()));
}

double ExplicitInertialSolver::stableDt() const {
    double dt = 1.0e30;
    for (int idx = 0; idx < static_cast<int>(active_cells_.size()); ++idx) {
        const int i = active_cells_[static_cast<std::size_t>(idx)];
        const double h = state_->depth[i];
        if (h <= opts_->dry_depth) continue;
        double speed = 0.0;
        if (!qcx_.empty() && h > 1.0e-6)
            speed = std::hypot(qcx_[i], qcy_[i]) / h;
        dt = std::min(dt, inertial::cellCflDt(opts_->cfl_number,
                                              edges_.cell_lchar[i], h, speed));
    }
    return dt;
}

void ExplicitInertialSolver::substep(double dt) {
    const auto& ed = edges_;
    const int   na = static_cast<int>(active_edges_.size());

    // Perot reconstruction of the cell discharge vector (θ-average input and
    // the CFL velocity term). Active cells only; skipped entirely at θ = 1.
    if (!qcx_.empty()) {
        const int nac = static_cast<int>(active_cells_.size());
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
        for (int k = 0; k < nac; ++k) {
            const int i = active_cells_[static_cast<std::size_t>(k)];
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

    // Face pass: one local-inertial update per active unique face.
    const double theta = opts_->theta;
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < na; ++k) {
        const int e = active_edges_[static_cast<std::size_t>(k)];
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
        double qn1 = inertial::inertialFaceUpdate(q_[e], qhat, hf, dt, slope,
                                                 ed.n2_face[e]);
        q_[e] = inertial::froudeCap(qn1, hf, opts_->froude_max);
    }

    // Positivity pass: each face has exactly one exporting cell (by sign), so
    // scaling that cell's outgoing faces by its λ is race-free and updates
    // both sides identically — conservation is exact under limiting.
    const int nac = static_cast<int>(active_cells_.size());
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < nac; ++k) {
        const int i = active_cells_[static_cast<std::size_t>(k)];
        double out = 0.0;
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
            const double f = ed.cell_sign[p] * q_[ed.cell_edge[p]] *
                             ed.xi[ed.cell_edge[p]];
            if (f > 0.0) out += f;
        }
        const double lam =
            inertial::positivityScale(state_->volume[i], out, dt,
                                      opts_->exchange_beta);
        if (lam < 1.0) {
            for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p) {
                const int e = ed.cell_edge[p];
                if (ed.cell_sign[p] * q_[e] > 0.0) q_[e] *= lam;
            }
        }
    }

    // Cell update: conservative CSR gather + sources; then refresh η/depth so
    // the next substep sees this one's surface.
#pragma omp parallel for schedule(static) num_threads(opts_->num_threads)
    for (int k = 0; k < nac; ++k) {
        const int i = active_cells_[static_cast<std::size_t>(k)];
        double flux = 0.0;   // m³/s, inflow-positive
        for (int p = ed.cell_ptr[i]; p < ed.cell_ptr[i + 1]; ++p)
            flux -= ed.cell_sign[p] * q_[ed.cell_edge[p]] *
                    ed.xi[ed.cell_edge[p]];
        const double src =
            state_->rainfall[i] + state_->coupling_flux[i]
            - evapSink(state_->evap_rate[i], state_->depth[i], opts_->dry_depth);
        double v = state_->volume[i] + dt * (flux + src * mesh_->tri_area[i]);
        state_->volume[i] = (v > 0.0) ? v : 0.0;
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
    }

    // Boundary edges (non-WALL), flux-form per owning cell. Serial: the list is
    // tiny (domain perimeter) and a cell may own several BC edges. Outflow is
    // clamped to the water actually present — a prescribed outflow cannot drain
    // a dry cell — and the APPLIED flux integral is accumulated so the router's
    // boundary ledger books exactly what happened (conservation by ledger).
    for (std::size_t k = 0; k < bc_cell_.size(); ++k) {
        const int i = bc_cell_[k];
        double f = computeBoundaryEdgeFlux(*mesh_, *state_, *opts_,
                                           opts_->flux_dh_eps, i, bc_slot_[k]);
        if (f == 0.0) continue;
        if (f < 0.0)   // availability clamp on outflow
            f = std::max(f, -state_->volume[i] / dt);
        state_->volume[i] += dt * f;
        bc_accum_[k] += dt * f;
        inertial::cellEtaDepth(*mesh_, *opts_, i, state_->volume[i],
                               state_->head[i], state_->depth[i]);
    }

    face_passes_ += na;
}

double ExplicitInertialSolver::advance(double t_current, double t_target) {
    if (!initialized_ || t_target <= t_current) return t_target;

    double t = t_current;
    t_last_sync_ = t_current;   // sources before this advance were booked by
                                // the caller's window/ledger accounting
    std::fill(bc_accum_.begin(), bc_accum_.end(), 0.0);
    long steps_this_advance = 0;
    int  since_rebuild = kRebuildEvery;   // force a rebuild on entry

    while (t < t_target) {
        if (since_rebuild >= kRebuildEvery) {
            syncAndRebuild(t);
            since_rebuild = 0;
        }
        double dt = stableDt();
        dt = std::min({dt, t_target - t, opts_->max_timestep});
        // A fully-dry / quiescent active set yields dt = min(t_target−t,
        // max_timestep): the marcher strides the window in O(1) substeps while
        // the lazy tier keeps accumulating rain.
        if (dt <= 0.0) break;

        if (!active_cells_.empty()) substep(dt);
        t += dt;
        ++steps_this_advance;
        ++since_rebuild;
        last_dt_ = dt;
    }

    syncAndRebuild(t_target);   // final lazy-source landing on [t, t_target]

    // Publish the flux picture the router's output/ledger contract reads
    // (MOMENTUM INERTIAL: no DW recompute). Interior unique faces project the
    // prognostic q into both incident flat slots (inflow-positive per cell);
    // boundary slots carry the WINDOW-MEAN applied flux, so the router's
    // −flux·dt_done booking recovers the exact ∫F_applied dt integral.
    std::fill(state_->edge_flux.begin(), state_->edge_flux.end(), 0.0);
    for (int e = 0; e < edges_.ne; ++e) {
        // Re-limit against the PUBLISHED surface: q was clamped with the face
        // depth its own update saw, but the subsequent cell pass moved the
        // heads — a draining front would otherwise publish a super-Froude
        // flux inconsistent with the published depths. Output-only: the next
        // substep re-derives its own clamp from fresh state.
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

    substeps_run_ += steps_this_advance;
    last_steps_ = steps_this_advance;
    return t_target;
}

void ExplicitInertialSolver::reinitialize(double /*t0*/) {
    if (!initialized_) return;
    // External state edit (hot start / breach redo): volumes are authoritative;
    // face momentum is stale information — drop it.
    std::fill(q_.begin(), q_.end(), 0.0);
    reconstructAll();
}

void ExplicitInertialSolver::resyncFromVolumes(double /*t0*/) {
    if (!initialized_) return;
    // Volumes already live in state_->volume; keep the face momentum (nothing
    // failed — this is a pure re-time on this path).
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
    cell_active_.clear(); active_cells_.clear(); active_edges_.clear();
    bc_cell_.clear(); bc_slot_.clear();
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
