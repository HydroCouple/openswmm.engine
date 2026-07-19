/**
 * @file CvodeSurfaceSolver.cpp
 * @brief Implementation of CVODE wrapper for 2D surface routing.
 *
 * @see CvodeSurfaceSolver.hpp
 * @ingroup engine_2d
 */

#ifdef OPENSWMM_HAS_2D

#include "CvodeSurfaceSolver.hpp"
#include "SurfaceFluxCalculator.hpp"
#include "../data/ActiveSetData.hpp"
#include "../mesh/VertexReconstruction.hpp"
#include "../mesh/VfrClosure.hpp"
#include "../coupling/NodeCoupling.hpp"   // live node-coupling helpers (macro-step path)
#include "../../data/NodeData.hpp"        // 1D node state read by the live coupling
#if defined(OPENSWMM_HAVE_HYPRE)
#include "HypreAmgPreconditioner.hpp"
#endif

#include <cvode/cvode.h>
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
#include <cassert>

#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#else
static inline int omp_get_max_threads() { return 1; }
#endif

namespace openswmm::twoD {

namespace {
// ---------------------------------------------------------------------------
// Volume ⇄ (free-surface η, mean depth h̄) reconstruction — the single place
// the integrated-state interpretation lives, selected by CELL_CLOSURE:
//
//   FLAT (legacy): depth = V/A, η = tri_cz + depth — exact only for a fully
//     wetted cell; on a partially wet cell it overstates η (up to 2/3 of the
//     cell relief), the driver of the uphill-creep artifact.
//   VFR: η from the exact planar-bed stage–storage relation through the
//     cell's vertex elevations (VfrClosure.hpp), C¹-regularized by the
//     VFR_MIN_WET_FRAC wet-area floor so dη/dV stays bounded for the
//     Newton/FD-Jacobian path. depth stays the MEAN depth V/A (unchanged
//     meaning for evap/coupling ramps).
//
// The smooth (V/A)^(5/3) conductance vanishes at the dry limit on its own,
// so no explicit wet/dry shutoff is needed under either closure.
// See plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
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
        // Regularized forward relation — the exact inverse of the closure
        // above, so head ↔ volume seeding round-trips (a head seeded at the
        // closure's dry anchor maps back to exactly V = 0).
        return m.tri_area[i]
               * vfrMeanDepthFromEta(z1, z2, z3, head, o.vfr_min_wet_frac);
    }
    const double d = head - m.tri_cz[i];
    return (d > 0.0) ? m.tri_area[i] * d : 0.0;
}

// State-vector factory: threaded OpenMP N_Vector when THREADS > 1 (CVODE clones
// it for all work vectors, so the BDF/Newton/Krylov vector ops thread too), else
// the serial vector. Math is unchanged; reductions sum in a different order
// (within solver tolerance). num_threads ≤ 1 keeps the exact serial path.
inline N_Vector makeStateVec(int n, int nthreads, SUNContext ctx) {
    return (nthreads > 1) ? N_VNew_OpenMP(n, nthreads, ctx)
                          : N_VNew_Serial(n, ctx);
}
} // namespace

// ============================================================================
// RHS function — registered as CVRhsFn callback
// ============================================================================

int CvodeSurfaceSolver::rhs_fn(double /*t*/, N_Vector y, N_Vector ydot,
                                 void* user_data) {
    auto* ctx = static_cast<CvodeSolverContext*>(user_data);
    auto& mesh  = *ctx->mesh;
    auto& state = *ctx->state;
    auto& opts  = *ctx->opts;

    int nt = mesh.n_triangles();
    double* y_data    = N_VGetArrayPointer(y);
    double* ydot_data = N_VGetArrayPointer(ydot);

    // Active-set masking: frozen cells' y components never change (their ydot
    // is exactly 0 below), so their head/depth stay at the frozen-correct
    // values — the unpack, like every downstream stage, touches actives only.
    const ActiveSetData* as = state.active_set;
    const bool masked = (as != nullptr) && as->enabled;

    // Volume formulation: CVODE integrates the cell water volume V. The free
    // surface η and the mean wetted depth h̄ are reconstructed per cell (smooth,
    // monotone closure) and drive the downstream flux pipeline. Per-cell unpack:
    // each i writes only head[i]/depth[i] ⇒ schedule(static) is bit-identical to
    // serial for any thread count.
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
    for (int i = 0; i < nt; ++i) {
        if (masked && !as->cell_active[i]) continue;
        reconstructFromVolume(mesh, opts, i, y_data[i],
                              state.head[i], state.depth[i]);
    }

    // 2. Reconstruct vertex heads on the RHS path to preserve existing held
    // coupling behaviour. The diffusive-wave edge flux itself uses centroid
    // head differences; output gradients are refreshed once per accepted
    // window by SurfaceRouter2D instead of inside every RHS/Jv evaluation.
    reconstructVertexHeads(mesh, state, opts.num_threads);
    // 3. Compute edge fluxes
    computeEdgeFluxes(mesh, state, opts);

    // 6. Assemble RHS
    assembleRHS(mesh, state, opts, ydot_data);

    // 7. Live node coupling (macro-step path only; node_coupling == nullptr on
    //    the default held-flux path). Evaluate the orifice exchange against the
    //    CURRENT 2D head so it self-limits as the cell drains and CVODE
    //    integrates the stiff coupling implicitly (stable over a large window).
    //    The sink/source is scattered into the cell ydot; ∫Q dt is accumulated
    //    in the augmented state entries [nt, nt+nc) for conservative booking.
    if (state.node_coupling != nullptr && state.nodes_1d != nullptr) {
        const auto& cps   = *state.node_coupling;
        const auto& nodes = *state.nodes_1d;
        const int   ncp   = static_cast<int>(cps.size());
        for (int k = 0; k < ncp; ++k) {
            const double Q = computeNodeCouplingQ(cps[k], mesh, state, nodes, opts);
            scatterCouplingToYdot(mesh, state, cps[k], -Q, ydot_data);  // cells lose drain Q
            ydot_data[nt + k] = Q;                                      // dA_k/dt = Q
        }
    }

    return 0;  // Success
}

// ============================================================================
// Preconditioner callbacks (Jacobi)
// ============================================================================
//
// CVODE's BDF + Newton corrector solves M·Δy = r at each Newton iteration,
// where M = I − γ·J and J = ∂f/∂y. GMRES applies J·v products (computed by
// SUNDIALS via finite-difference quotients of rhs_fn) and uses the
// preconditioner P ≈ M to accelerate convergence.
//
// For Phase 1 we use a Jacobi (diagonal) preconditioner: P = I − γ·D, where
// D is a per-cell heuristic approximation to diag(J). The heuristic uses
// the most recently evaluated edge fluxes (held in state.edge_flux from the
// last rhs_fn call) and the corresponding head differences:
//
//     T_e ≈ |F_e| / max(|h_L − h_R|, ε)            (per-edge transmissivity)
//     D[i] ≈ −(Σ_e T_e) / A_i                       (negative outflux/area)
//
// This is the dominant term in the analytic ∂f_i/∂h_i for the collapsed
// Manning flux. Higher-order terms (the depth-dependence of h_up^(5/3) and
// the wet/dry Hermite-ramp derivative) are intentionally omitted — for a
// preconditioner the precision is unnecessary, and walking the same flux
// pipeline twice is wasteful. PSolve only applies the diagonal inverse
// element-wise, so any approximation error shows up as extra GMRES
// iterations, not as wrong answers.
//
// Phase 2 (BoomerAMG via hypre) will replace this with a full sparse
// Jacobian and a multigrid hierarchy. The PSetup / PSolve callback
// signatures stay the same — only the body changes — so the solver
// configuration logic in initialize() does not need to grow conditionals
// for "which preconditioner is wired today".

int CvodeSurfaceSolver::psetup_fn(double /*t*/, N_Vector /*y*/, N_Vector /*fy*/,
                                   int jok, int* jcurPtr, double gamma,
                                   void* user_data) {
    auto* ctx    = static_cast<CvodeSolverContext*>(user_data);
    auto& mesh   = *ctx->mesh;
    auto& state  = *ctx->state;
    auto* solver = ctx->solver;

    // Lagged preconditioner: honor CVODE's jok flag. jok == SUNTRUE means the
    // saved Jacobian data is still current (only γ drifted) — reuse the
    // preconditioner and report it was NOT recomputed. jok == SUNFALSE means
    // CVODE wants a fresh Jacobian — rebuild. GMRES preconditions with the true
    // matrix-free operator, so reusing a slightly stale preconditioner only
    // changes the Krylov iteration count, never the converged solution. This
    // removes the dominant BoomerAMG hierarchy rebuild from the (majority)
    // γ-only psetup calls.
    const bool recompute = (jok == SUNFALSE);
    *jcurPtr = recompute ? SUNTRUE : SUNFALSE;

    const ActiveSetData* as = state.active_set;
    const bool masked = (as != nullptr) && as->enabled;

#if defined(OPENSWMM_HAVE_HYPRE)
    // AMG: assemble M = I − γ·J and rebuild the BoomerAMG hierarchy (only when
    // recompute; setup() reuses the prior hierarchy otherwise).
    //
    // Active-set bypass: on a mostly-dry masked system the Jacobian is
    // near-identity (frozen rows ARE identity), so one BoomerAMG hierarchy
    // build — O(n) with large constants over the FULL mesh graph — buys
    // nothing over the (masked, near-exact) Jacobi diagonal. Switch to Jacobi
    // while the active fraction is below the threshold; the branch flip in
    // either direction forces a fresh build of whichever preconditioner takes
    // over (its cache is stale or absent).
    if (ctx->amg_active) {
        static const double bypass_frac = []{
            const char* e = std::getenv("OPENSWMM_2D_AMG_BYPASS_FRAC");
            return (e && e[0]) ? std::atof(e) : 0.05;
        }();
        const int  ntot   = mesh.n_triangles();
        const bool bypass = masked && ntot > 0
                            && as->n_active() < bypass_frac * ntot;
        if (!bypass) {
            const bool force = recompute || !solver->amg_used_last_setup_;
            // VFR: hand the AMG assembly the per-cell dη/dV chain-rule factor so
            // M = I − γJ is scaled correctly at wetting fronts. Without it the
            // BoomerAMG hierarchy is mis-scaled under VFR and costs MORE Krylov
            // iterations than no preconditioner. Only when the matrix is actually
            // (re)assembled (force); FLAT passes nullptr ⇒ bit-identical.
            const double* deta = nullptr;
            std::vector<double> deta_dv;
            if (force && ctx->opts->cell_closure == CellClosure2D::VFR) {
                const int nt = mesh.n_triangles();
                deta_dv.resize(static_cast<std::size_t>(nt));
                for (int i = 0; i < nt; ++i) {
                    const double inv_area = (mesh.tri_area[i] > 1.0e-30)
                                                ? 1.0 / mesh.tri_area[i] : 0.0;
                    double z1 = mesh.vz[mesh.tri_v0[i]];
                    double z2 = mesh.vz[mesh.tri_v1[i]];
                    double z3 = mesh.vz[mesh.tri_v2[i]];
                    vfrSort3(z1, z2, z3);
                    deta_dv[i] = inv_area
                        * vfrDEtaDMeanDepth(z1, z2, z3, state.head[i],
                                            ctx->opts->vfr_min_wet_frac);
                }
                deta = deta_dv.data();
            }
            solver->amg_precond_->setup(mesh, state, gamma, force, deta);
            solver->amg_used_last_setup_ = true;
            return 0;
        }
        // Fall through to Jacobi. If AMG served the last setup, the cached
        // diagonal (if any) is stale — force a rebuild below.
        if (solver->amg_used_last_setup_) solver->precond_diag_.clear();
        solver->amg_used_last_setup_ = false;
    }
#else
    (void)gamma;
#endif

    // JACOBI: recompute the diagonal only when CVODE asks for a fresh Jacobian;
    // otherwise reuse the cached diagonal (psolve_fn re-applies the current γ).
    if (!recompute && !solver->precond_diag_.empty())
        return 0;

    int nt = mesh.n_triangles();
    auto& D = solver->precond_diag_;
    D.assign(static_cast<std::size_t>(nt), 0.0);  // sized BEFORE the parallel loop

    // Edge transmissivities, then sum per cell with area normalisation.
    // dh_floor regularises the divide at flat-water (|Δh| → 0). Any value
    // well below the smallest meaningful head difference works; at the
    // dry_depth ~ 1e-5 m target scale, 1e-9 m is six orders below.
    constexpr double dh_floor = 1.0e-9;
    // Each cell writes only its own D[i] (the assign() above already sized the
    // buffer), reading neighbour heads/fluxes read-only ⇒ schedule(static) is
    // bit-identical to serial.
#pragma omp parallel for schedule(static) num_threads(ctx->opts->num_threads)
    for (int i = 0; i < nt; ++i) {
        // Frozen cells keep D[i] = 0 (the assign above): psolve then computes
        // z_i = r_i / (1 − γ·0) = r_i — the exact identity row of M, matching
        // the frozen cell's exactly-zero Jacobian row.
        if (masked && !as->cell_active[i]) continue;
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
        // dη/dV = 1/(A·max(w, ε)) — partially wet cells respond faster, and
        // the diagonal must say so or the preconditioner underestimates M at
        // the wetting front (costing Krylov iterations, never correctness).
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

int CvodeSurfaceSolver::psolve_fn(double /*t*/, N_Vector /*y*/, N_Vector /*fy*/,
                                   N_Vector r, N_Vector z,
                                   double gamma, double /*delta*/, int /*lr*/,
                                   void* user_data) {
    auto* ctx    = static_cast<CvodeSolverContext*>(user_data);
    auto* solver = ctx->solver;

#if defined(OPENSWMM_HAVE_HYPRE)
    // AMG: apply one BoomerAMG V-cycle z ≈ M⁻¹ r over all cells. Dispatch on
    // whichever preconditioner served the most recent psetup (the active-set
    // bypass can switch to Jacobi on mostly-dry systems); CVODE guarantees
    // psolve pairs with the latest psetup.
    if (ctx->amg_active && solver->amg_used_last_setup_) {
        const int n = ctx->mesh->n_triangles();
        const double* rd = N_VGetArrayPointer(r);
        double*       zd = N_VGetArrayPointer(z);
        solver->amg_precond_->solve(rd, zd, n);
        // ∫Q dt accumulator rows have M_kk = 1 (nothing depends on them) ⇒ the
        // exact preconditioner is the identity z = r.
        for (int k = 0; k < solver->nc_; ++k) zd[n + k] = rd[n + k];
        return 0;
    }
#endif

    int nt = static_cast<int>(solver->precond_diag_.size());
    const double* r_data = N_VGetArrayPointer(r);
    double*       z_data = N_VGetArrayPointer(z);
    const double* D      = solver->precond_diag_.data();

    // m_i = 1 − γ·D[i]. The diagonal D[i] is non-positive by construction
    // (negative-sum-of-transmissivities), so m_i ≥ 1 for non-negative γ —
    // the divide cannot blow up under normal operation. The clamp below
    // guards against pathological cases (zero-area cells, all-dry meshes)
    // where D[i] could be exactly zero or undefined.
    constexpr double m_floor = 1.0e-12;
    // Element-wise diagonal solve: each i writes only z_data[i] ⇒ bit-exact.
#pragma omp parallel for schedule(static) num_threads(ctx->opts->num_threads)
    for (int i = 0; i < nt; ++i) {
        double m = 1.0 - gamma * D[i];
        if (std::abs(m) < m_floor) m = std::copysign(m_floor, m);
        z_data[i] = r_data[i] / m;
    }
    // ∫Q dt accumulator rows: identity (M_kk = 1).
    for (int k = 0; k < solver->nc_; ++k) z_data[nt + k] = r_data[nt + k];
    return 0;
}

// ============================================================================
// Lifecycle
// ============================================================================

CvodeSurfaceSolver::CvodeSurfaceSolver() = default;

CvodeSurfaceSolver::~CvodeSurfaceSolver() {
    finalize();
}

CvodeSurfaceSolver::CvodeSurfaceSolver(CvodeSurfaceSolver&& o) noexcept
    : cvode_mem_(o.cvode_mem_), ls_(o.ls_), y_(o.y_), abstol_(o.abstol_),
      sun_ctx_(o.sun_ctx_), ctx_(o.ctx_),
      last_nsteps_(o.last_nsteps_), last_h_(o.last_h_),
      precond_diag_(std::move(o.precond_diag_))
#if defined(OPENSWMM_HAVE_HYPRE)
      , amg_precond_(std::move(o.amg_precond_))
#endif
{
    o.cvode_mem_ = nullptr;
    o.ls_        = nullptr;
    o.y_         = nullptr;
    o.abstol_    = nullptr;
    o.sun_ctx_   = nullptr;
    // Re-bind the context's back-pointer to *this so callbacks routed via
    // o.ctx_ before the move still find the right solver.
    ctx_.solver = this;
}

CvodeSurfaceSolver& CvodeSurfaceSolver::operator=(CvodeSurfaceSolver&& o) noexcept {
    if (this != &o) {
        finalize();
        cvode_mem_    = o.cvode_mem_;    o.cvode_mem_ = nullptr;
        ls_           = o.ls_;           o.ls_        = nullptr;
        y_            = o.y_;            o.y_         = nullptr;
        abstol_       = o.abstol_;       o.abstol_    = nullptr;
        sun_ctx_      = o.sun_ctx_;      o.sun_ctx_   = nullptr;
        ctx_          = o.ctx_;
        ctx_.solver   = this;
        last_nsteps_  = o.last_nsteps_;
        last_h_       = o.last_h_;
        precond_diag_ = std::move(o.precond_diag_);
#if defined(OPENSWMM_HAVE_HYPRE)
        amg_precond_  = std::move(o.amg_precond_);
#endif
    }
    return *this;
}

void CvodeSurfaceSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                     SolverOptions2D& opts) {
    if (cvode_mem_) finalize();

    int nt = mesh.n_triangles();
    if (nt <= 0) return;

    // ------------------------------------------------------------------
    // Option validation: GMRES + (NONE | JACOBI | AMG). ILU is reserved and
    // throws. AMG is the default but needs a hypre build; without one it
    // degrades to JACOBI (the next-best always-available preconditioner) with
    // a one-line notice, so the portable base build still runs.
    // ------------------------------------------------------------------
    if (opts.linear_solver != LinearSolverType::GMRES) {
        throw std::runtime_error(
            "CvodeSurfaceSolver: only LINEAR_SOLVER=GMRES is wired; "
            "BICGSTAB and TFQMR are reserved for future use");
    }
    if (opts.preconditioner != PreconditionerType::NONE &&
        opts.preconditioner != PreconditionerType::JACOBI &&
        opts.preconditioner != PreconditionerType::AMG) {
        throw std::runtime_error(
            "CvodeSurfaceSolver: only PRECONDITIONER=NONE, JACOBI, or AMG is "
            "wired; ILU is reserved");
    }
    PreconditionerType pc = opts.preconditioner;  // effective preconditioner
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

    // Create SUNDIALS context
    int err = SUNContext_Create(SUN_COMM_NULL, &sun_ctx_);
    if (err != 0)
        throw std::runtime_error("SUNContext_Create failed");

    // Augmented state size: nt cell volumes + nc_ live node-coupling ∫Q dt
    // accumulators. nc_ == 0 (default / held-flux path) ⇒ ntot == nt, byte-for-
    // byte identical to the legacy solver. The accumulators are pure quadrature
    // (nothing depends on them), carried so the 1D↔2D booking is conservative.
    nc_ = (state.node_coupling != nullptr)
              ? static_cast<int>(state.node_coupling->size()) : 0;
    const int ntot = nt + nc_;

    // Create N_Vector wrapping state.depth (threaded when THREADS > 1).
    y_ = makeStateVec(ntot, opts.num_threads, sun_ctx_);
    if (!y_)
        throw std::runtime_error("N_VNew (state) failed");

    // Seed y with the initial cell volume V from the initial free surface.
    // SurfaceRouter2D::initialize sets state.head[i] = mesh.tri_cz[i] (dry) ⇒
    // V = 0; a hot start with a raised surface seeds the matching volume. Also
    // mirror into state.volume so totalVolume() is correct before the first step.
    double* y_data = N_VGetArrayPointer(y_);
    for (int i = 0; i < nt; ++i) {
        y_data[i] = volumeFromHead(mesh, opts, i, state.head[i]);
        state.volume[i] = y_data[i];
    }
    for (int k = 0; k < nc_; ++k) y_data[nt + k] = 0.0;  // ∫Q dt accumulators start at 0
    coupling_accum_start_.assign(static_cast<std::size_t>(nc_), 0.0);
    last_coupling_exchange_.assign(static_cast<std::size_t>(nc_), 0.0);

    // Create CVODE memory (BDF for stiff systems). The default nonlinear
    // corrector is Newton with inexact-tolerance control — exactly the
    // configuration validated in Kumar et al. (2009). We do not call
    // CVodeSetNonlinearSolver, which means we accept that default.
    cvode_mem_ = CVodeCreate(CV_BDF, sun_ctx_);
    if (!cvode_mem_)
        throw std::runtime_error("CVodeCreate failed");

    // Initialize CVODE with the RHS function
    err = CVodeInit(cvode_mem_, rhs_fn, 0.0, y_);
    if (err != CV_SUCCESS)
        throw std::runtime_error("CVodeInit failed");

    // Tolerances. The integrated state is now VOLUME (m³), a physical quantity,
    // so the error control is a per-cell absolute volume tolerance equal to a
    // depth tolerance × cell area — i.e. "control each cell's mean depth error
    // to depth_atol". This replaces the H-formulation's dry_depth-rescaled
    // scalar atol hack (y was a ~bed-elevation magnitude). rtol stays 0 so the
    // control is purely the physical per-cell atol.
    // WRMS error weight per cell: w_i = rtol·|V_i| + atol_i. With VOLUME as the
    // state both terms are physical: atol_i = ABS_TOLERANCE · A_i is a small
    // absolute depth floor (controls shallow cells), and the relative term
    // rtol = REL_TOLERANCE scales the tolerance with the cell's water content so
    // deep/violent flow (e.g. the cloudburst transient) is not forced to
    // micron-accuracy — that scaling is what lets the step size recover in deep
    // water. (The old H-formulation had to zero rtol and fold REL_TOLERANCE into
    // a dry_depth-scaled atol because y was a ~100 m elevation; with volume that
    // hack is gone.)
    abstol_ = makeStateVec(ntot, opts.num_threads, sun_ctx_);
    if (!abstol_)
        throw std::runtime_error("N_VNew (atol) failed");
    {
        double* av = N_VGetArrayPointer(abstol_);
        for (int i = 0; i < nt; ++i) {
            const double A = mesh.tri_area[i];
            av[i] = opts.abs_tolerance * ((A > 1.0e-30) ? A : 1.0);
        }
        // Accumulators carry a huge atol so their error never constrains the
        // step (they are diagnostic quadrature, not part of the cell dynamics);
        // they are still integrated to the step accuracy V drives.
        for (int k = 0; k < nc_; ++k) av[nt + k] = 1.0e30;
    }
    // Kept alive for the solver's lifetime (freed in finalize): CVODE may
    // reference the tolerance vector, so it must outlive the integration.
    err = CVodeSVtolerances(cvode_mem_, /*rtol*/ opts.rel_tolerance, abstol_);
    if (err != CV_SUCCESS)
        throw std::runtime_error("CVodeSVtolerances failed");

    // Set user data (context pointer)
    err = CVodeSetUserData(cvode_mem_, &ctx_);
    if (err != CV_SUCCESS)
        throw std::runtime_error("CVodeSetUserData failed");

    // Set timestep bounds
    CVodeSetMinStep(cvode_mem_, opts.min_timestep);
    CVodeSetMaxStep(cvode_mem_, opts.max_timestep);
    CVodeSetMaxNumSteps(cvode_mem_, opts.max_cvode_steps);

    // ------------------------------------------------------------------
    // Linear solver: SPGMR (Scaled Preconditioned GMRES).
    // ------------------------------------------------------------------
    //
    // Preconditioning side is PREC_LEFT for JACOBI (standard for diagonal
    // preconditioners on diffusive operators) or PREC_NONE.
    //
    // max_krylov_dim caps the Krylov subspace before a restart. The default
    // (30) is reasonable up to ~100k cells; larger meshes may benefit from
    // a larger subspace at the cost of memory. Phase 2's BoomerAMG path
    // will typically converge well below this limit, so we'd reduce it.
    //
    // Note on Jacobian: we DO NOT call CVodeSetJacFn or set a JacTimes
    // function. SUNDIALS computes J·v products via difference quotients of
    // rhs_fn.
    const int prec_type = (pc == PreconditionerType::NONE)
                              ? 0   // PREC_NONE
                              : 1;  // PREC_LEFT
    ls_ = SUNLinSol_SPGMR(y_, prec_type, opts.max_krylov_dim, sun_ctx_);
    if (!ls_)
        throw std::runtime_error("SUNLinSol_SPGMR creation failed");

    err = CVodeSetLinearSolver(cvode_mem_, ls_, /*A*/ nullptr);
    if (err != CV_SUCCESS)
        throw std::runtime_error("CVodeSetLinearSolver failed");

    if (pc == PreconditionerType::JACOBI) {
        // Pre-allocate the diagonal store; psetup_fn reuses this buffer.
        precond_diag_.assign(static_cast<std::size_t>(nt), 0.0);
        err = CVodeSetPreconditioner(cvode_mem_, psetup_fn, psolve_fn);
        if (err != CV_SUCCESS)
            throw std::runtime_error("CVodeSetPreconditioner failed");
    }
#if defined(OPENSWMM_HAVE_HYPRE)
    else if (pc == PreconditionerType::AMG) {
        // Build the hypre BoomerAMG preconditioner over the static sparsity;
        // psetup_fn refreshes M = I − γJ and rebuilds the hierarchy, psolve_fn
        // applies one V-cycle. Same CVODE callback surface as Jacobi.
        amg_precond_ = std::make_unique<HypreAmgPreconditioner>();
        amg_precond_->initialize(mesh);
        err = CVodeSetPreconditioner(cvode_mem_, psetup_fn, psolve_fn);
        if (err != CV_SUCCESS)
            throw std::runtime_error("CVodeSetPreconditioner (AMG) failed");
    }
#endif
}


double CvodeSurfaceSolver::advance(double t_current, double t_target) {
    if (!cvode_mem_) return t_current;

    int nt = ctx_.mesh->n_triangles();

    // Snapshot the ∫Q dt accumulators so the per-window node-coupling exchange
    // is y[nt+k](end) − y[nt+k](start). Not reset between windows (a reset would
    // need CVodeReInit, discarding the step-size/order history we want to keep).
    if (nc_ > 0) {
        const double* ys = N_VGetArrayPointer(y_);
        for (int k = 0; k < nc_; ++k) coupling_accum_start_[k] = ys[nt + k];
    }

    // Set stop time to guarantee exact arrival
    CVodeSetStopTime(cvode_mem_, t_target);

    // Advance CVODE
    double t_reached = t_current;
    int flag = CVode(cvode_mem_, t_target, y_, &t_reached, CV_NORMAL);

    if (flag < 0) {
        // CVODE failure — leave state unchanged
        return t_current;
    }

    // Copy solution back to state: y is the cell volume V. Store V and the
    // reconstructed (η, h̄) so totalVolume / continuity / output are consistent.
    double* y_data = N_VGetArrayPointer(y_);
    // Each i writes only its own volume/head/depth ⇒ race-free under OpenMP.
#pragma omp parallel for schedule(static) num_threads(ctx_.opts->num_threads)
    for (int i = 0; i < nt; ++i) {
        const double V = y_data[i];
        ctx_.state->volume[i] = V;
        reconstructFromVolume(*ctx_.mesh, *ctx_.opts, i, V,
                              ctx_.state->head[i], ctx_.state->depth[i]);
    }

    // Per-window node-coupling exchange ∫Q dt (m³, +drain/−spill) — the caller
    // books these to the 1D node lateral inflow + the 2D mass-balance ledger.
    if (nc_ > 0) {
        const double* yd = N_VGetArrayPointer(y_);
        for (int k = 0; k < nc_; ++k)
            last_coupling_exchange_[k] = yd[nt + k] - coupling_accum_start_[k];
    }

    // Record solver statistics
    CVodeGetNumSteps(cvode_mem_, &last_nsteps_);
    CVodeGetLastStep(cvode_mem_, &last_h_);

    return t_reached;
}


void CvodeSurfaceSolver::reinitialize(double t0) {
    if (!cvode_mem_) return;

    int nt = ctx_.mesh->n_triangles();
    // y is the cell volume V; reseed it from the (possibly externally edited)
    // free surface and mirror into state.volume.
    double* y_data = N_VGetArrayPointer(y_);
    for (int i = 0; i < nt; ++i) {
        y_data[i] = volumeFromHead(*ctx_.mesh, *ctx_.opts, i,
                                   ctx_.state->head[i]);
        ctx_.state->volume[i] = y_data[i];
    }

    CVodeReInit(cvode_mem_, t0, y_);

    // Invalidate the lagged preconditioner caches: the state was re-seeded, so
    // the cached Jacobi diagonal / AMG hierarchy no longer match it. CVODE is
    // expected to pass jok = SUNFALSE on the first psetup after a ReInit, but
    // the explicit reset makes the cache correct regardless of that policy.
    precond_diag_.clear();
#if defined(OPENSWMM_HAVE_HYPRE)
    if (amg_precond_) amg_precond_->invalidate();
#endif
}


void CvodeSurfaceSolver::finalize() {
    // CVode must be freed before its linear solver (CVODE holds an internal
    // pointer to ls_ via CVodeSetLinearSolver). Likewise free the N_Vector
    // and context last, since N_Vector destruction touches the context.
    if (cvode_mem_) {
        CVodeFree(&cvode_mem_);
        cvode_mem_ = nullptr;
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
    amg_precond_.reset();  // releases the hypre IJ matrix/vectors + AMG hierarchy
#endif
    ctx_.solver = nullptr;
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
