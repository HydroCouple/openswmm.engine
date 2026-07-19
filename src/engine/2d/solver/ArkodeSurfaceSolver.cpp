/**
 * @file ArkodeSurfaceSolver.cpp
 * @brief Implementation of the ARKODE/ARKStep IMEX wrapper for 2D surface routing.
 *
 * @see ArkodeSurfaceSolver.hpp
 * @ingroup engine_2d
 */

#ifdef OPENSWMM_HAS_2D

#include "ArkodeSurfaceSolver.hpp"
#include "SurfaceFluxCalculator.hpp"
#include "../mesh/VertexReconstruction.hpp"
#include "../mesh/VfrClosure.hpp"
#include "../coupling/NodeCoupling.hpp"   // live node-coupling helpers (macro-step path)
#include "../../data/NodeData.hpp"        // 1D node state read by the live coupling
#if defined(OPENSWMM_HAVE_HYPRE)
#include "HypreAmgPreconditioner.hpp"
#endif

#include <arkode/arkode.h>
#include <arkode/arkode_arkstep.h>
#include <arkode/arkode_ls.h>
#include <nvector/nvector_serial.h>
#include <nvector/nvector_openmp.h>
#include <sunlinsol/sunlinsol_spgmr.h>
#include <sundials/sundials_context.h>

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace openswmm::twoD {

namespace {
// ---------------------------------------------------------------------------
// Volume ⇄ free-surface reconstruction, selected by CELL_CLOSURE. Mirrors the
// identical helpers in CvodeSurfaceSolver.cpp: FLAT is the legacy flat-cell
// closure η = tri_cz + V/A; VFR reconstructs η from the planar-bed VFR
// relation (VfrClosure.hpp), C¹-regularized by VFR_MIN_WET_FRAC. depth stays
// the mean depth V/A under both. The smooth conductance vanishes at the dry
// limit on its own. See plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
// ---------------------------------------------------------------------------
inline void reconstructFromVolume(const MeshData& m, const SolverOptions2D& o,
                                  int i, double V,
                                  double& head, double& depth) noexcept {
    const double A = m.tri_area[i];
    const double v = (V > 0.0) ? V : 0.0;
    depth = (A > 1.0e-30) ? v / A : 0.0;
    if (o.cell_closure == CellClosure2D::VFR) {
        double z1 = m.vz[m.tri_v0[i]];
        double z2 = m.vz[m.tri_v1[i]];
        double z3 = m.vz[m.tri_v2[i]];
        vfrSort3(z1, z2, z3);
        head = vfrEtaFromMeanDepth(z1, z2, z3, depth, o.vfr_min_wet_frac);
    } else {
        head = m.tri_cz[i] + depth;
    }
}
inline double volumeFromHead(const MeshData& m, const SolverOptions2D& o,
                             int i, double head) noexcept {
    if (o.cell_closure == CellClosure2D::VFR) {
        double z1 = m.vz[m.tri_v0[i]];
        double z2 = m.vz[m.tri_v1[i]];
        double z3 = m.vz[m.tri_v2[i]];
        vfrSort3(z1, z2, z3);
        return m.tri_area[i]
               * vfrMeanDepthFromEta(z1, z2, z3, head, o.vfr_min_wet_frac);
    }
    const double d = head - m.tri_cz[i];
    return (d > 0.0) ? m.tri_area[i] * d : 0.0;
}

// State-vector factory: the threaded OpenMP N_Vector when THREADS > 1 (its
// reductions/axpys run multithreaded, so ARKODE's vector ops — cloned from this
// one — thread too), else the serial vector. The math is unchanged; only the
// vector-op loop structure differs (reductions sum in a different order, within
// solver tolerance). num_threads ≤ 1 keeps the exact serial path.
inline N_Vector makeStateVec(int n, int nthreads, SUNContext ctx) {
    return (nthreads > 1) ? N_VNew_OpenMP(n, nthreads, ctx)
                          : N_VNew_Serial(n, ctx);
}
} // namespace

// ============================================================================
// RHS functions — the IMEX split (registered as ARKRhsFn callbacks)
// ============================================================================

// Explicit half F_E: source/sink forcing only. Self-contained — reconstructs
// depth locally from the stage volume and touches no shared state arrays, so it
// never races the implicit half's pipeline. Accumulator rows carry no explicit
// contribution (the live orifice ∫Q dt is integrated by F_I).
int ArkodeSurfaceSolver::fe_fn(double /*t*/, N_Vector y, N_Vector ydot,
                                void* user_data) {
    auto* ctx = static_cast<ArkodeSolverContext*>(user_data);
    auto& mesh  = *ctx->mesh;
    auto& state = *ctx->state;
    auto& opts  = *ctx->opts;

    const int nt = mesh.n_triangles();
    const double* y_data = N_VGetArrayPointer(y);
    double* ydot_data    = N_VGetArrayPointer(ydot);

    assembleExplicitRHS(mesh, state, opts, y_data, ydot_data);

    for (int k = 0; k < ctx->solver->nc_; ++k) ydot_data[nt + k] = 0.0;
    return 0;
}

// Implicit half F_I: the stiff diffusion flux divergence + the live 1D↔2D
// orifice coupling. Runs the reconstruct → edge-flux pipeline (identical to the
// CVODE rhs), then assembles the flux divergence and
// scatters the live orifice exchange. Leaves state.head / state.edge_flux at the
// stage state the preconditioner reads.
int ArkodeSurfaceSolver::fi_fn(double /*t*/, N_Vector y, N_Vector ydot,
                                void* user_data) {
    auto* ctx = static_cast<ArkodeSolverContext*>(user_data);
    auto& mesh  = *ctx->mesh;
    auto& state = *ctx->state;
    auto& opts  = *ctx->opts;

    const int nt = mesh.n_triangles();
    double* y_data    = N_VGetArrayPointer(y);
    double* ydot_data = N_VGetArrayPointer(ydot);

    // 1. Reconstruct head / mean depth from the integrated cell volume.
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
    for (int i = 0; i < nt; ++i) {
        reconstructFromVolume(mesh, opts, i, y_data[i], state.head[i], state.depth[i]);
    }

    // 2. Reconstruct vertex heads on the RHS path to preserve existing held
    // coupling behaviour. The diffusive-wave edge flux itself uses centroid
    // head differences; output gradients are refreshed once per accepted
    // window by SurfaceRouter2D instead of inside every RHS/Jv evaluation.
    reconstructVertexHeads(mesh, state, opts.num_threads);
    // 3. Edge fluxes.
    computeEdgeFluxes(mesh, state, opts);

    // 6. Flux divergence (the implicit operator).
    assembleImplicitRHS(mesh, state, opts, ydot_data);

    // 7. Live node coupling (macro-step path only). Evaluated against the live
    //    stage head so it self-limits; ∫Q dt accumulated in the augmented rows.
    if (state.node_coupling != nullptr && state.nodes_1d != nullptr) {
        const auto& cps   = *state.node_coupling;
        const auto& nodes = *state.nodes_1d;
        const int   ncp   = static_cast<int>(cps.size());
        for (int k = 0; k < ncp; ++k) {
            const double Q = computeNodeCouplingQ(cps[k], mesh, state, nodes, opts);
            scatterCouplingToYdot(mesh, state, cps[k], -Q, ydot_data);
            ydot_data[nt + k] = Q;
        }
    }
    return 0;
}

// ============================================================================
// Local-inertial RHS (MOMENTUM=inertial). State layout: [V(nt), q(ne), accum].
// ============================================================================

// Explicit half F_E for the local-inertial scheme. Always: cell source forcing.
// In the default explicit-gravity (LISFLOOD-FP) split it ALSO carries the
// transport — the continuity divergence (cells) and the surface-gradient gravity
// (edges) — leaving only the stiff friction implicit. In the implicit-gravity
// comparison path, q rows carry no explicit part.
int ArkodeSurfaceSolver::fe_inertial_fn(double /*t*/, N_Vector y, N_Vector ydot,
                                         void* user_data) {
    auto* ctx = static_cast<ArkodeSolverContext*>(user_data);
    auto& mesh   = *ctx->mesh;
    auto& state  = *ctx->state;
    auto& opts   = *ctx->opts;
    auto* solver = ctx->solver;
    const InertialEdges& E = solver->edges_;

    const int nt = mesh.n_triangles();
    const int ne = solver->ne_;
    double* y_data    = N_VGetArrayPointer(y);
    double* ydot_data = N_VGetArrayPointer(ydot);
    const double* q   = y_data + nt;

    assembleExplicitRHS(mesh, state, opts, y_data, ydot_data);   // cell sources

    if (solver->gravity_explicit_) {
        constexpr double G = 9.80665;
        const double hmin = opts.dry_depth;
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
        for (int i = 0; i < nt; ++i)
            reconstructFromVolume(mesh, opts, i, y_data[i], state.head[i], state.depth[i]);

        // Continuity divergence ADDED to the cell source rows.
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
        for (int i = 0; i < nt; ++i) {
            double s = 0.0;
            for (int k = E.cell_ptr[i]; k < E.cell_ptr[i + 1]; ++k) {
                const int e = E.cell_edge[k];
                s += E.cell_sign[k] * q[e] * E.xi[e];
            }
            ydot_data[i] -= s;
        }
        // Surface-gradient gravity on the edge rows.
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
        for (int e = 0; e < ne; ++e) {
            const int cl = E.cL[e], cr = E.cR[e];
            const double hf = std::max(state.head[cl], state.head[cr]) - E.zface[e];
            ydot_data[nt + e] = (hf > hmin)
                ? -G * hf * (state.head[cr] - state.head[cl]) * E.inv_dx[e]
                : 0.0;
        }
    } else {
        for (int e = 0; e < ne; ++e) ydot_data[nt + e] = 0.0;  // gravity is implicit
    }
    for (int k = 0; k < solver->nc_; ++k) ydot_data[nt + ne + k] = 0.0;
    return 0;
}

// Implicit half F_I for the local-inertial scheme. Default (explicit gravity):
// only the stiff per-edge friction −g·n²·q|q|/h_f^(7/3) — a diagonal operator,
// so the preconditioner is exact and there is no global solve. Implicit-gravity
// comparison path: the full coupled system (continuity + gravity + friction).
// h_f = max(η_L,η_R) − z_face; a dry interface relaxes residual q to 0.
int ArkodeSurfaceSolver::fi_inertial_fn(double /*t*/, N_Vector y, N_Vector ydot,
                                         void* user_data) {
    auto* ctx = static_cast<ArkodeSolverContext*>(user_data);
    auto& mesh   = *ctx->mesh;
    auto& state  = *ctx->state;
    auto& opts   = *ctx->opts;
    auto* solver = ctx->solver;
    const InertialEdges& E = solver->edges_;

    const int nt = mesh.n_triangles();
    const int ne = solver->ne_;
    double* y_data    = N_VGetArrayPointer(y);
    double* ydot_data = N_VGetArrayPointer(ydot);
    const double* q   = y_data + nt;

    constexpr double G = 9.80665;
    const double hmin = opts.dry_depth;
    const bool grav_imp = !solver->gravity_explicit_;

    // Reconstruct head / depth from the integrated cell volume.
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
    for (int i = 0; i < nt; ++i)
        reconstructFromVolume(mesh, opts, i, y_data[i], state.head[i], state.depth[i]);

    // Cell rows: continuity divergence only when gravity is implicit; otherwise
    // the cells carry no implicit term (transport is explicit).
    if (grav_imp) {
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
        for (int i = 0; i < nt; ++i) {
            double s = 0.0;
            for (int k = E.cell_ptr[i]; k < E.cell_ptr[i + 1]; ++k) {
                const int e = E.cell_edge[k];
                s += E.cell_sign[k] * q[e] * E.xi[e];
            }
            ydot_data[i] = -s;
        }
    } else {
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
        for (int i = 0; i < nt; ++i) ydot_data[i] = 0.0;
    }

    // Edge rows: friction always implicit; gravity implicit only in the
    // comparison path.
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
    for (int e = 0; e < ne; ++e) {
        const int cl = E.cL[e], cr = E.cR[e];
        const double hf = std::max(state.head[cl], state.head[cr]) - E.zface[e];
        double dq;
        if (hf > hmin) {
            const double n    = 0.5 * (mesh.mannings_n[cl] + mesh.mannings_n[cr]);
            const double hf73 = std::pow(hf, 7.0 / 3.0);
            dq = -G * n * n * q[e] * std::abs(q[e]) / hf73;          // friction
            if (grav_imp)
                dq += -G * hf * (state.head[cr] - state.head[cl]) * E.inv_dx[e];
        } else {
            dq = -q[e];   // dry interface: bleed residual q to 0 (non-stiff)
        }
        ydot_data[nt + e] = dq;
    }

    for (int k = 0; k < solver->nc_; ++k) ydot_data[nt + ne + k] = 0.0;
    return 0;
}

// Block-Jacobi preconditioner for the inertial [V,q] Newton matrix
//   M = [ I_V       γD     ;  γGrad   I_q+γR ].
// The q-block (I_q+γR) is DIAGONAL (friction is per-edge, no edge–edge coupling)
// so it inverts exactly: w_e = 1/(1+γ·R_e), R_e = 2·g·n²·|q_e|/h_f^(7/3). The
// Schur complement on V is the elliptic η-operator S = I − γ²·D·diag(w)·Grad;
// we keep only its DIAGONAL (a Jacobi η-preconditioner), which is accurate when
// the lateral coupling is weak (large cells) and needs no AMG / no global solve
// — so it scales to 1M cells. Off-diagonal gravity coupling is left to GMRES.
int ArkodeSurfaceSolver::psetup_inertial_fn(double /*t*/, N_Vector y, N_Vector /*fy*/,
                                             int jok, int* jcurPtr, double gamma,
                                             void* user_data) {
    auto* ctx    = static_cast<ArkodeSolverContext*>(user_data);
    auto& mesh   = *ctx->mesh;
    auto& opts   = *ctx->opts;
    auto* solver = ctx->solver;
    const InertialEdges& E = solver->edges_;

    const int nt = mesh.n_triangles();
    const int ne = solver->ne_;
    const double* y_data = N_VGetArrayPointer(y);
    const double* q      = y_data + nt;

    const bool recompute = (jok == SUNFALSE);
    *jcurPtr = recompute ? SUNTRUE : SUNFALSE;
    if (!recompute && !solver->prec_wq_.empty()) return 0;   // reuse (lagged)

    constexpr double G = 9.80665;
    const double hmin = opts.dry_depth;
    const bool grav_imp = !solver->gravity_explicit_;

    auto& wq = solver->prec_wq_;  wq.assign(static_cast<std::size_t>(ne), 0.0);
    auto& dV = solver->prec_dV_;  dV.assign(static_cast<std::size_t>(nt), 1.0);  // I_V

    auto headOf = [&](int i) {
        const double A = mesh.tri_area[i];
        const double v = (y_data[i] > 0.0) ? y_data[i] : 0.0;
        return mesh.tri_cz[i] + ((A > 1.0e-30) ? v / A : 0.0);
    };

    for (int e = 0; e < ne; ++e) {
        const int cl = E.cL[e], cr = E.cR[e];
        const double hf = std::max(headOf(cl), headOf(cr)) - E.zface[e];
        double R, ge;
        if (hf > hmin) {
            const double n    = 0.5 * (mesh.mannings_n[cl] + mesh.mannings_n[cr]);
            const double hf73 = std::pow(hf, 7.0 / 3.0);
            R  = 2.0 * G * n * n * std::abs(q[e]) / hf73;   // friction damping rate
            ge = G * hf * E.inv_dx[e];                      // gravity edge coeff
        } else {
            R  = 1.0;   // dry-edge relaxation rate (matches the RHS −q term)
            ge = 0.0;
        }
        wq[e] = 1.0 / (1.0 + gamma * R);
        // Schur η-diagonal contribution only when gravity is implicit. In the
        // default explicit-gravity split the V-block has no implicit term, so
        // dV stays I and the block-diagonal preconditioner is EXACT.
        if (grav_imp) {
            const double T = gamma * gamma * wq[e] * E.xi[e] * ge;
            dV[cl] += T / mesh.tri_area[cl];
            dV[cr] += T / mesh.tri_area[cr];
        }
    }
    return 0;
}

int ArkodeSurfaceSolver::psolve_inertial_fn(double /*t*/, N_Vector /*y*/, N_Vector /*fy*/,
                                             N_Vector r, N_Vector z,
                                             double /*gamma*/, double /*delta*/,
                                             int /*lr*/, void* user_data) {
    auto* ctx    = static_cast<ArkodeSolverContext*>(user_data);
    auto* solver = ctx->solver;
    const int nt = ctx->mesh->n_triangles();
    const int ne = solver->ne_;

    const double* rd = N_VGetArrayPointer(r);
    double*       zd = N_VGetArrayPointer(z);
    const double* dV = solver->prec_dV_.data();
    const double* wq = solver->prec_wq_.data();

#pragma omp parallel for schedule(static) num_threads(ctx->opts->num_threads)
    for (int i = 0; i < nt; ++i) zd[i] = rd[i] / dV[i];              // Schur η-diagonal
#pragma omp parallel for schedule(static) num_threads(ctx->opts->num_threads)
    for (int e = 0; e < ne; ++e) zd[nt + e] = wq[e] * rd[nt + e];   // friction diagonal
    for (int k = 0; k < solver->nc_; ++k) zd[nt + ne + k] = rd[nt + ne + k];  // accum identity
    return 0;
}

// ============================================================================
// Preconditioner callbacks — operate on the implicit (diffusion) Jacobian.
// Logic is identical to CvodeSurfaceSolver: M = I − γ·J over the diffusion
// stencil, lagged via jok; AMG one V-cycle or per-cell Jacobi diagonal. The
// accumulator rows have M_kk = 1 ⇒ identity pass-through.
// ============================================================================

int ArkodeSurfaceSolver::psetup_fn(double /*t*/, N_Vector /*y*/, N_Vector /*fy*/,
                                    int jok, int* jcurPtr, double gamma,
                                    void* user_data) {
    auto* ctx    = static_cast<ArkodeSolverContext*>(user_data);
    auto& mesh   = *ctx->mesh;
    auto& state  = *ctx->state;
    auto* solver = ctx->solver;

    const bool recompute = (jok == SUNFALSE);
    *jcurPtr = recompute ? SUNTRUE : SUNFALSE;

#if defined(OPENSWMM_HAVE_HYPRE)
    if (ctx->amg_active) {
        solver->amg_precond_->setup(mesh, state, gamma, recompute);
        return 0;
    }
#else
    (void)gamma;
#endif

    if (!recompute && !solver->precond_diag_.empty())
        return 0;

    int nt = mesh.n_triangles();
    auto& D = solver->precond_diag_;
    D.assign(static_cast<std::size_t>(nt), 0.0);

    constexpr double dh_floor = 1.0e-9;
#pragma omp parallel for schedule(static) num_threads(ctx->opts->num_threads)
    for (int i = 0; i < nt; ++i) {
        const int nbr[3] = {mesh.tri_nbr0[i], mesh.tri_nbr1[i], mesh.tri_nbr2[i]};
        double T_sum = 0.0;
        for (int e = 0; e < 3; ++e) {
            if (nbr[e] < 0) continue;
            double dh = std::abs(state.head[i] - state.head[nbr[e]]);
            double F  = std::abs(state.edge_flux[i * 3 + e]);
            T_sum    += F / std::max(dh, dh_floor);
        }
        double inv_area = (mesh.tri_area[i] > 1.0e-30)
                              ? 1.0 / mesh.tri_area[i] : 0.0;
        // Chain rule ∂F/∂V = (∂F/∂η)·(dη/dV). FLAT: dη/dV = 1/A. VFR:
        // dη/dV = 1/(A·max(w, ε)) — partially wet cells respond faster, so the
        // preconditioner diagonal must carry the factor or it underestimates M
        // at the wetting front. CVODE's stiff BDF tolerates the mismatch (extra
        // Krylov iterations); ARKStep's DIRK stage solve does NOT — without this
        // the implicit stage fails to converge, steps collapse to hmin and the
        // advance exhausts MAX_CVODE_STEPS. Mirrors CvodeSurfaceSolver::psetup_fn.
        double detadv = inv_area;
        if (ctx->opts->cell_closure == CellClosure2D::VFR) {
            double z1 = mesh.vz[mesh.tri_v0[i]];
            double z2 = mesh.vz[mesh.tri_v1[i]];
            double z3 = mesh.vz[mesh.tri_v2[i]];
            vfrSort3(z1, z2, z3);
            detadv *= vfrDEtaDMeanDepth(z1, z2, z3, state.head[i],
                                        ctx->opts->vfr_min_wet_frac);
        }
        D[i] = -T_sum * detadv;
    }
    return 0;
}

int ArkodeSurfaceSolver::psolve_fn(double /*t*/, N_Vector /*y*/, N_Vector /*fy*/,
                                    N_Vector r, N_Vector z,
                                    double gamma, double /*delta*/, int /*lr*/,
                                    void* user_data) {
    auto* ctx    = static_cast<ArkodeSolverContext*>(user_data);
    auto* solver = ctx->solver;

#if defined(OPENSWMM_HAVE_HYPRE)
    if (ctx->amg_active) {
        const int n = ctx->mesh->n_triangles();
        const double* rd = N_VGetArrayPointer(r);
        double*       zd = N_VGetArrayPointer(z);
        solver->amg_precond_->solve(rd, zd, n);
        for (int k = 0; k < solver->nc_; ++k) zd[n + k] = rd[n + k];
        return 0;
    }
#endif

    int nt = static_cast<int>(solver->precond_diag_.size());
    const double* r_data = N_VGetArrayPointer(r);
    double*       z_data = N_VGetArrayPointer(z);
    const double* D      = solver->precond_diag_.data();

    constexpr double m_floor = 1.0e-12;
#pragma omp parallel for schedule(static) num_threads(ctx->opts->num_threads)
    for (int i = 0; i < nt; ++i) {
        double m = 1.0 - gamma * D[i];
        if (std::abs(m) < m_floor) m = std::copysign(m_floor, m);
        z_data[i] = r_data[i] / m;
    }
    for (int k = 0; k < solver->nc_; ++k) z_data[nt + k] = r_data[nt + k];
    return 0;
}

// ============================================================================
// Lifecycle
// ============================================================================

ArkodeSurfaceSolver::ArkodeSurfaceSolver() = default;

ArkodeSurfaceSolver::~ArkodeSurfaceSolver() {
    finalize();
}

ArkodeSurfaceSolver::ArkodeSurfaceSolver(ArkodeSurfaceSolver&& o) noexcept
    : arkode_mem_(o.arkode_mem_), ls_(o.ls_), y_(o.y_), abstol_(o.abstol_),
      sun_ctx_(o.sun_ctx_), ctx_(o.ctx_),
      last_nsteps_(o.last_nsteps_), last_h_(o.last_h_),
      nc_(o.nc_),
      coupling_accum_start_(std::move(o.coupling_accum_start_)),
      last_coupling_exchange_(std::move(o.last_coupling_exchange_)),
      momentum_(o.momentum_), ne_(o.ne_), edges_(std::move(o.edges_)),
      prec_dV_(std::move(o.prec_dV_)), prec_wq_(std::move(o.prec_wq_)),
      precond_diag_(std::move(o.precond_diag_))
#if defined(OPENSWMM_HAVE_HYPRE)
      , amg_precond_(std::move(o.amg_precond_))
#endif
{
    o.arkode_mem_ = nullptr;
    o.ls_         = nullptr;
    o.y_          = nullptr;
    o.abstol_     = nullptr;
    o.sun_ctx_    = nullptr;
    ctx_.solver = this;
}

ArkodeSurfaceSolver& ArkodeSurfaceSolver::operator=(ArkodeSurfaceSolver&& o) noexcept {
    if (this != &o) {
        finalize();
        arkode_mem_   = o.arkode_mem_;   o.arkode_mem_ = nullptr;
        ls_           = o.ls_;           o.ls_         = nullptr;
        y_            = o.y_;            o.y_          = nullptr;
        abstol_       = o.abstol_;       o.abstol_     = nullptr;
        sun_ctx_      = o.sun_ctx_;      o.sun_ctx_    = nullptr;
        ctx_          = o.ctx_;
        ctx_.solver   = this;
        last_nsteps_  = o.last_nsteps_;
        last_h_       = o.last_h_;
        nc_           = o.nc_;
        coupling_accum_start_   = std::move(o.coupling_accum_start_);
        last_coupling_exchange_ = std::move(o.last_coupling_exchange_);
        momentum_     = o.momentum_;
        ne_           = o.ne_;
        edges_        = std::move(o.edges_);
        prec_dV_      = std::move(o.prec_dV_);
        prec_wq_      = std::move(o.prec_wq_);
        precond_diag_ = std::move(o.precond_diag_);
#if defined(OPENSWMM_HAVE_HYPRE)
        amg_precond_  = std::move(o.amg_precond_);
#endif
    }
    return *this;
}

void ArkodeSurfaceSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                      SolverOptions2D& opts) {
    if (arkode_mem_) finalize();

    int nt = mesh.n_triangles();
    if (nt <= 0) return;

    if (opts.linear_solver != LinearSolverType::GMRES) {
        throw std::runtime_error(
            "ArkodeSurfaceSolver: only LINEAR_SOLVER=GMRES is wired");
    }
    if (opts.preconditioner != PreconditionerType::NONE &&
        opts.preconditioner != PreconditionerType::JACOBI &&
        opts.preconditioner != PreconditionerType::AMG) {
        throw std::runtime_error(
            "ArkodeSurfaceSolver: only PRECONDITIONER=NONE, JACOBI, or AMG is "
            "wired; ILU is reserved");
    }
    PreconditionerType pc = opts.preconditioner;
#if !defined(OPENSWMM_HAVE_HYPRE)
    if (pc == PreconditionerType::AMG) {
        pc = PreconditionerType::JACOBI;
    }
#endif
    ctx_.amg_active = (pc == PreconditionerType::AMG);

    ctx_.mesh   = &mesh;
    ctx_.state  = &state;
    ctx_.opts   = &opts;
    ctx_.solver = this;

    // Momentum closure is authoritative in opts.momentum (SurfaceRouter2D folds
    // the OPENSWMM_2D_MOMENTUM env override into it before constructing the
    // solver, so the router and solver agree). INERTIAL adds the per-edge q DOFs.
    momentum_ = opts.momentum;
    ne_ = 0;
    if (momentum_ == MomentumType::INERTIAL) {
        // Default: explicit gravity transport / implicit friction (LISFLOOD-FP,
        // scales O(n)). OPENSWMM_2D_GRAVITY_IMPLICIT forces the implicit-gravity
        // (Schur-coupled) path for comparison.
        gravity_explicit_ = (std::getenv("OPENSWMM_2D_GRAVITY_IMPLICIT") == nullptr);
        edges_.build(mesh);
        ne_ = edges_.ne;
    }

    int err = SUNContext_Create(SUN_COMM_NULL, &sun_ctx_);
    if (err != 0)
        throw std::runtime_error("SUNContext_Create failed");

    // Augmented state: nt cell volumes + ne_ per-edge discharges (inertial only)
    // + nc_ live-coupling ∫Q dt accumulators. Accumulators sit AFTER the q block.
    nc_ = (state.node_coupling != nullptr)
              ? static_cast<int>(state.node_coupling->size()) : 0;
    const int ntot     = nt + ne_ + nc_;
    const int accum_off = nt + ne_;   // first accumulator row

    y_ = makeStateVec(ntot, opts.num_threads, sun_ctx_);
    if (!y_)
        throw std::runtime_error("N_VNew (state) failed");

    double* y_data = N_VGetArrayPointer(y_);
    for (int i = 0; i < nt; ++i) {
        y_data[i] = volumeFromHead(mesh, opts, i, state.head[i]);
        state.volume[i] = y_data[i];
    }
    for (int e = 0; e < ne_; ++e) y_data[nt + e] = 0.0;          // q starts at rest
    for (int k = 0; k < nc_; ++k) y_data[accum_off + k] = 0.0;   // ∫Q dt accumulators
    coupling_accum_start_.assign(static_cast<std::size_t>(nc_), 0.0);
    last_coupling_exchange_.assign(static_cast<std::size_t>(nc_), 0.0);

    // Create the ARKStep IMEX integrator: both fe and fi non-NULL ⇒ additive
    // Runge–Kutta ImEx (3rd-order ARK324L2SA pair by default). The RHS pair is
    // the DW split or the local-inertial split per the momentum closure.
    const bool inertial = (momentum_ == MomentumType::INERTIAL);
    arkode_mem_ = inertial
        ? ARKStepCreate(fe_inertial_fn, fi_inertial_fn, 0.0, y_, sun_ctx_)
        : ARKStepCreate(fe_fn, fi_fn, 0.0, y_, sun_ctx_);
    if (!arkode_mem_)
        throw std::runtime_error("ARKStepCreate failed");

    err = ARKodeSetOrder(arkode_mem_, 3);
    if (err != ARK_SUCCESS)
        throw std::runtime_error("ARKodeSetOrder failed");

    // Per-cell absolute volume tolerance = depth_atol · A_i; accumulators carry
    // a huge atol so their quadrature never constrains the step. Same scheme as
    // CVODE (see CvodeSurfaceSolver::initialize for the rationale).
    abstol_ = makeStateVec(ntot, opts.num_threads, sun_ctx_);
    if (!abstol_)
        throw std::runtime_error("N_VNew (atol) failed");
    {
        double* av = N_VGetArrayPointer(abstol_);
        for (int i = 0; i < nt; ++i) {
            const double A = mesh.tri_area[i];
            av[i] = opts.abs_tolerance * ((A > 1.0e-30) ? A : 1.0);
        }
        // Per-edge discharge q (m²/s): a small absolute tolerance; rtol scales it
        // with the discharge magnitude for fast flow.
        for (int e = 0; e < ne_; ++e) av[nt + e] = opts.abs_tolerance;
        for (int k = 0; k < nc_; ++k) av[accum_off + k] = 1.0e30;
    }
    err = ARKodeSVtolerances(arkode_mem_, opts.rel_tolerance, abstol_);
    if (err != ARK_SUCCESS)
        throw std::runtime_error("ARKodeSVtolerances failed");

    err = ARKodeSetUserData(arkode_mem_, &ctx_);
    if (err != ARK_SUCCESS)
        throw std::runtime_error("ARKodeSetUserData failed");

    ARKodeSetMinStep(arkode_mem_, opts.min_timestep);
    ARKodeSetMaxStep(arkode_mem_, opts.max_timestep);
    // Per-window internal-step guard. MAX_CVODE_STEPS is calibrated for CVODE's
    // adaptive-order BDF; ARKStep's fixed order-3 DIRK takes structurally more,
    // smaller steps for the same window — measured ~2× on the diffusive-wave
    // face-gate (VFR_FACE) path. Scale the guard for ARKODE so the CVODE-tuned
    // default (500) does not spuriously exhaust and freeze a window (which would
    // silently drop that window's forcing); MAX_CVODE_STEPS stays the user knob.
    constexpr long kArkodeStepGuardFactor = 4;
    ARKodeSetMaxNumSteps(arkode_mem_,
                         kArkodeStepGuardFactor * opts.max_cvode_steps);

    // SPGMR for the implicit-stage Newton–Krylov solve. PREC_LEFT for JACOBI/AMG
    // and for the inertial block-Jacobi preconditioner.
    const int prec_type = (inertial || pc != PreconditionerType::NONE) ? 1 : 0;
    ls_ = SUNLinSol_SPGMR(y_, prec_type, opts.max_krylov_dim, sun_ctx_);
    if (!ls_)
        throw std::runtime_error("SUNLinSol_SPGMR creation failed");

    err = ARKodeSetLinearSolver(arkode_mem_, ls_, /*A*/ nullptr);
    if (err != ARK_SUCCESS)
        throw std::runtime_error("ARKodeSetLinearSolver failed");

    if (inertial) {
        // Block-Jacobi preconditioner for the [V,q] system (no AMG, no global
        // solve). Schur η-diagonal + exact friction-damped q-diagonal.
        prec_dV_.assign(static_cast<std::size_t>(nt), 1.0);
        prec_wq_.assign(static_cast<std::size_t>(ne_), 0.0);
        err = ARKodeSetPreconditioner(arkode_mem_, psetup_inertial_fn, psolve_inertial_fn);
        if (err != ARK_SUCCESS)
            throw std::runtime_error("ARKodeSetPreconditioner (inertial) failed");
    } else if (pc == PreconditionerType::JACOBI) {
        precond_diag_.assign(static_cast<std::size_t>(nt), 0.0);
        err = ARKodeSetPreconditioner(arkode_mem_, psetup_fn, psolve_fn);
        if (err != ARK_SUCCESS)
            throw std::runtime_error("ARKodeSetPreconditioner failed");
    }
#if defined(OPENSWMM_HAVE_HYPRE)
    else if (pc == PreconditionerType::AMG) {
        amg_precond_ = std::make_unique<HypreAmgPreconditioner>();
        amg_precond_->initialize(mesh);
        err = ARKodeSetPreconditioner(arkode_mem_, psetup_fn, psolve_fn);
        if (err != ARK_SUCCESS)
            throw std::runtime_error("ARKodeSetPreconditioner (AMG) failed");
    }
#endif
}

double ArkodeSurfaceSolver::advance(double t_current, double t_target) {
    if (!arkode_mem_) return t_current;

    int nt = ctx_.mesh->n_triangles();
    const int accum_off = nt + ne_;   // accumulators sit after the q block

    if (nc_ > 0) {
        const double* ys = N_VGetArrayPointer(y_);
        for (int k = 0; k < nc_; ++k) coupling_accum_start_[k] = ys[accum_off + k];
    }

    ARKodeSetStopTime(arkode_mem_, t_target);

    double t_reached = t_current;
    int flag = ARKodeEvolve(arkode_mem_, t_target, y_, &t_reached, ARK_NORMAL);

    if (flag < 0) {
        return t_current;  // ARKODE failure — leave state unchanged
    }

    double* y_data = N_VGetArrayPointer(y_);
    // Each i writes only its own volume/head/depth ⇒ race-free under OpenMP.
#pragma omp parallel for schedule(static) num_threads(ctx_.opts->num_threads)
    for (int i = 0; i < nt; ++i) {
        const double V = y_data[i];
        ctx_.state->volume[i] = V;
        reconstructFromVolume(*ctx_.mesh, *ctx_.opts, i, V,
                              ctx_.state->head[i], ctx_.state->depth[i]);
    }

    // Inertial: project the per-edge discharge q back onto the redundant
    // edge_flux slots (inflow-positive to the storing cell) so the downstream
    // continuity / velocity / mass-balance diagnostics read the actual flux.
    // SurfaceRouter2D skips its DW edge-flux recompute in this mode. Each edge
    // writes its own two distinct slots (slotL/slotR) ⇒ race-free.
    if (momentum_ == MomentumType::INERTIAL) {
        const double* q = y_data + nt;
        std::fill(ctx_.state->edge_flux.begin(), ctx_.state->edge_flux.end(), 0.0);
#pragma omp parallel for schedule(static) num_threads(ctx_.opts->num_threads)
        for (int e = 0; e < ne_; ++e) {
            const double f = q[e] * edges_.xi[e];
            ctx_.state->edge_flux[edges_.slotL[e]] = -f;  // outflow from cL
            ctx_.state->edge_flux[edges_.slotR[e]] = +f;  // inflow to cR
        }
    }

    if (nc_ > 0) {
        const double* yd = N_VGetArrayPointer(y_);
        for (int k = 0; k < nc_; ++k)
            last_coupling_exchange_[k] = yd[accum_off + k] - coupling_accum_start_[k];
    }

    ARKodeGetNumSteps(arkode_mem_, &last_nsteps_);
    ARKodeGetLastStep(arkode_mem_, &last_h_);

    return t_reached;
}

void ArkodeSurfaceSolver::reinitialize(double t0) {
    if (!arkode_mem_) return;

    int nt = ctx_.mesh->n_triangles();
    double* y_data = N_VGetArrayPointer(y_);
    for (int i = 0; i < nt; ++i) {
        y_data[i] = volumeFromHead(*ctx_.mesh, *ctx_.opts, i, ctx_.state->head[i]);
        ctx_.state->volume[i] = y_data[i];
    }
    // q (edges) and accumulators keep their current values across a hot-start.

    if (momentum_ == MomentumType::INERTIAL)
        ARKStepReInit(arkode_mem_, fe_inertial_fn, fi_inertial_fn, t0, y_);
    else
        ARKStepReInit(arkode_mem_, fe_fn, fi_fn, t0, y_);
}

void ArkodeSurfaceSolver::finalize() {
    if (arkode_mem_) {
        ARKodeFree(&arkode_mem_);
        arkode_mem_ = nullptr;
    }
    if (ls_) {
        SUNLinSolFree(ls_);
        ls_ = nullptr;
    }
    if (y_) {
        N_VDestroy(y_);
        y_ = nullptr;
    }
    if (abstol_) {
        N_VDestroy(abstol_);
        abstol_ = nullptr;
    }
    if (sun_ctx_) {
        SUNContext_Free(&sun_ctx_);
        sun_ctx_ = nullptr;
    }
    precond_diag_.clear();
#if defined(OPENSWMM_HAVE_HYPRE)
    amg_precond_.reset();
#endif
    ctx_.solver = nullptr;
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
