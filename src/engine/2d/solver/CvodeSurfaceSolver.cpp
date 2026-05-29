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
#include "../mesh/VertexReconstruction.hpp"
#include "../uncertainty/CorrelatedFieldGenerator.hpp"

#include <cvode/cvode.h>
#include <cvode/cvode_ls.h>
#include <nvector/nvector_serial.h>
#include <sunlinsol/sunlinsol_spgmr.h>
#include <sundials/sundials_context.h>

#include <cstring>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cassert>

namespace openswmm::twoD {

// ============================================================================
// Spectral preconditioner callbacks
// ============================================================================

static int spectral_precond_setup(sunrealtype /*t*/, N_Vector /*y*/,
                                   N_Vector /*fy*/, sunbooleantype /*jok*/,
                                   sunbooleantype* jcurPtr,
                                   sunrealtype /*gamma*/, void* /*user_data*/) {
    // Eigenbasis is mesh-geometry–based and never rebuilt at runtime.
    *jcurPtr = SUNTRUE;
    return 0;
}

static int spectral_precond_solve(sunrealtype /*t*/, N_Vector /*y*/,
                                   N_Vector /*fy*/, N_Vector r, N_Vector z,
                                   sunrealtype gamma, sunrealtype /*delta*/,
                                   int /*lr*/, void* user_data) {
    auto* ctx = static_cast<CvodeSolverContext*>(user_data);
    ctx->precond->apply(N_VGetArrayPointer(r),
                        N_VGetArrayPointer(z),
                        static_cast<double>(gamma));
    return 0;
}

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

    // H-formulation: CVODE integrates the water-surface elevation H, not the
    // depth. Dry cells naturally sit at H = z (no special case for the
    // wet/dry boundary; H is smooth across it). Depth is a derived quantity
    // and is clamped non-negative for the conductance code. dH/dt = dh/dt
    // because z is constant in time, so ydot is unchanged downstream.
    for (int i = 0; i < nt; ++i) {
        state.head[i]  = y_data[i];
        state.depth[i] = std::max(y_data[i] - mesh.tri_cz[i], 0.0);
    }

    // 2. Reconstruct head at vertices (pseudo-Laplacian)
    reconstructVertexHeads(mesh, state);

    // 3. Compute unlimited gradients (Green-Gauss)
    computeUnlimitedGradients(mesh, state);

    // 4. Apply slope limiter (Jawahar-Kamath)
    computeLimitedGradients(mesh, state, opts.limiter_epsilon);

    // 5. Compute edge fluxes
    computeEdgeFluxes(mesh, state, opts);

    // 6. Assemble RHS
    assembleRHS(mesh, state, ydot_data);

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
                                   int /*jok*/, int* jcurPtr, double /*gamma*/,
                                   void* user_data) {
    auto* ctx    = static_cast<CvodeSolverContext*>(user_data);
    auto& mesh   = *ctx->mesh;
    auto& state  = *ctx->state;
    auto* solver = ctx->solver;

    // Phase 1 always rebuilds (jok argument ignored). Phase 2's lazy reuse
    // will set this based on the AMG hierarchy's age policy.
    *jcurPtr = 1;  // SUNTRUE

    int nt = mesh.n_triangles();
    auto& D = solver->precond_diag_;
    D.assign(static_cast<std::size_t>(nt), 0.0);

    // Edge transmissivities, then sum per cell with area normalisation.
    // dh_floor regularises the divide at flat-water (|Δh| → 0). Any value
    // well below the smallest meaningful head difference works; at the
    // dry_depth ~ 1e-5 m target scale, 1e-9 m is six orders below.
    constexpr double dh_floor = 1.0e-9;
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
        D[i] = -T_sum * inv_area;
    }
    return 0;
}

int CvodeSurfaceSolver::psolve_fn(double /*t*/, N_Vector /*y*/, N_Vector /*fy*/,
                                   N_Vector r, N_Vector z,
                                   double gamma, double /*delta*/, int /*lr*/,
                                   void* user_data) {
    auto* ctx    = static_cast<CvodeSolverContext*>(user_data);
    auto* solver = ctx->solver;

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
    for (int i = 0; i < nt; ++i) {
        double m = 1.0 - gamma * D[i];
        if (std::abs(m) < m_floor) m = std::copysign(m_floor, m);
        z_data[i] = r_data[i] / m;
    }
    return 0;
}

// ============================================================================
// Lifecycle
// ============================================================================

CvodeSurfaceSolver::~CvodeSurfaceSolver() {
    finalize();
}

CvodeSurfaceSolver::CvodeSurfaceSolver(CvodeSurfaceSolver&& o) noexcept
    : cvode_mem_(o.cvode_mem_), ls_(o.ls_), y_(o.y_),
      sun_ctx_(o.sun_ctx_), ctx_(o.ctx_),
      precond_(std::move(o.precond_)),
      rom_basis_(std::move(o.rom_basis_)),
      rom_(std::move(o.rom_)),
      rom_seeded_(o.rom_seeded_),
      last_nsteps_(o.last_nsteps_), last_h_(o.last_h_),
      wet_count_at_seed_(o.wet_count_at_seed_),
      last_reseed_t_(o.last_reseed_t_),
      precond_diag_(std::move(o.precond_diag_)) {
    o.cvode_mem_ = nullptr;
    o.ls_        = nullptr;
    o.y_         = nullptr;
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
        sun_ctx_      = o.sun_ctx_;      o.sun_ctx_   = nullptr;
        ctx_          = o.ctx_;
        ctx_.solver   = this;
        precond_      = std::move(o.precond_);
        rom_basis_    = std::move(o.rom_basis_);
        rom_          = std::move(o.rom_);
        rom_seeded_   = o.rom_seeded_;
        last_nsteps_  = o.last_nsteps_;
        last_h_       = o.last_h_;
        wet_count_at_seed_ = o.wet_count_at_seed_;
        last_reseed_t_     = o.last_reseed_t_;
        precond_diag_ = std::move(o.precond_diag_);
    }
    return *this;
}

void CvodeSurfaceSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                     SolverOptions2D& opts) {
    if (cvode_mem_) finalize();

    int nt = mesh.n_triangles();
    if (nt <= 0) return;

    // ------------------------------------------------------------------
    // Option validation: Phase 1 wires GMRES + (NONE | JACOBI). Other
    // enum values exist as forward-looking hooks but are not implemented
    // yet; fail loudly rather than silently substituting, so a typo in
    // the input file doesn't quietly give the user a different solver
    // than they asked for.
    // ------------------------------------------------------------------
    if (opts.linear_solver != LinearSolverType::GMRES) {
        throw std::runtime_error(
            "CvodeSurfaceSolver: only LINEAR_SOLVER=GMRES is wired in Phase 1; "
            "BICGSTAB and TFQMR are reserved for future use");
    }
    if (opts.preconditioner != PreconditionerType::NONE &&
        opts.preconditioner != PreconditionerType::JACOBI) {
        throw std::runtime_error(
            "CvodeSurfaceSolver: only PRECONDITIONER=NONE or JACOBI is wired in "
            "Phase 1; ILU and (future) AMG are reserved for the hypre/BoomerAMG "
            "integration");
    }

    ctx_.mesh   = &mesh;
    ctx_.state  = &state;
    ctx_.opts   = &opts;
    ctx_.solver = this;

    // Create SUNDIALS context
    int err = SUNContext_Create(SUN_COMM_NULL, &sun_ctx_);
    if (err != 0)
        throw std::runtime_error("SUNContext_Create failed");

    // Create N_Vector wrapping state.depth
    y_ = N_VNew_Serial(nt, sun_ctx_);
    if (!y_)
        throw std::runtime_error("N_VNew_Serial failed");

    // Copy initial water-surface elevations H into y (H-formulation).
    // SurfaceRouter2D::initialize sets state.head[i] = mesh.tri_cz[i] for the
    // dry initial condition; that already gives the right H = z start state.
    double* y_data = N_VGetArrayPointer(y_);
    std::memcpy(y_data, state.head.data(), nt * sizeof(double));

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

    // Set tolerances. With the H-formulation, y is the water-surface
    // elevation (≈ 100 m on typical models) but the physically interesting
    // signal is depth (often mm-scale). CVODE's WRMS weight is
    // w_i = rtol · |y_i| + atol; with y_i ≈ 100 and the user's rtol = 1e-4
    // this gives w_i ≈ 1e-2, four orders of magnitude looser than the
    // depth-formulation gave at the same nominal rtol. Compensate by
    // collapsing the user-provided REL_TOLERANCE into a fixed per-cell ATOL
    // computed against a depth scale (dry_depth), and disabling CVODE's
    // automatic |y|-scaling. The effective error scale is then
    //   atol_per_cell ≈ ABS_TOLERANCE + REL_TOLERANCE · dry_depth
    // which matches the depth-formulation's behaviour at depths ~ dry_depth.
    const double atol_per_cell = opts.abs_tolerance
                                 + opts.rel_tolerance * opts.dry_depth;
    err = CVodeSStolerances(cvode_mem_, /*rtol*/ 0.0, atol_per_cell);
    if (err != CV_SUCCESS)
        throw std::runtime_error("CVodeSStolerances failed");

    // Set user data (context pointer)
    err = CVodeSetUserData(cvode_mem_, &ctx_);
    if (err != CV_SUCCESS)
        throw std::runtime_error("CVodeSetUserData failed");

    // Set timestep bounds
    CVodeSetMinStep(cvode_mem_, opts.min_timestep);
    CVodeSetMaxStep(cvode_mem_, opts.max_timestep);
    CVodeSetMaxNumSteps(cvode_mem_, opts.max_cvode_steps);

    // Build spectral preconditioner if requested
    bool use_spectral = (opts.preconditioner == PreconditionerType::SPECTRAL);
    if (use_spectral) {
        precond_ = std::make_unique<SpectralPrecond2D>();
        precond_->num_modes = opts.spectral_num_modes;
        if (!precond_->build(mesh, opts.spectral_num_modes)) {
            precond_.reset();
            use_spectral = false;
        } else {
            ctx_.precond = precond_.get();
        }
    }

    // Create iterative linear solver
    const int prec_side = (use_spectral || opts.preconditioner == PreconditionerType::JACOBI)
                              ? SUN_PREC_LEFT : SUN_PREC_NONE;
    switch (opts.linear_solver) {
        case LinearSolverType::GMRES:
            ls_ = SUNLinSol_SPGMR(y_, prec_side, opts.max_krylov_dim, sun_ctx_);
            break;
        case LinearSolverType::BICGSTAB:
            ls_ = SUNLinSol_SPBCGS(y_, prec_side, opts.max_krylov_dim, sun_ctx_);
            break;
        case LinearSolverType::TFQMR:
            ls_ = SUNLinSol_SPTFQMR(y_, prec_side, opts.max_krylov_dim, sun_ctx_);
            break;
    }
    if (!ls_)
        throw std::runtime_error("SUNLinSol_SPGMR creation failed");

    err = CVodeSetLinearSolver(cvode_mem_, ls_, /*A*/ nullptr);
    if (err != CV_SUCCESS)
        throw std::runtime_error("CVodeSetLinearSolver failed");

    // Register preconditioner callbacks
    if (use_spectral) {
        err = CVodeSetPreconditioner(cvode_mem_,
                                     spectral_precond_setup,
                                     spectral_precond_solve);
        if (err != CV_SUCCESS)
            throw std::runtime_error("CVodeSetPreconditioner failed");
    } else if (opts.preconditioner == PreconditionerType::JACOBI) {
        precond_diag_.assign(static_cast<std::size_t>(nt), 0.0);
        err = CVodeSetPreconditioner(cvode_mem_, psetup_fn, psolve_fn);
        if (err != CV_SUCCESS)
            throw std::runtime_error("CVodeSetPreconditioner failed");
    }

    // Initialize ROM sidecar if requested
    if (opts.enable_rom) {
        const SpectralPrecond2D* rom_b = precond_.get();
        if (!rom_b) {
            rom_basis_ = std::make_unique<SpectralPrecond2D>();
            if (!rom_basis_->build(mesh, opts.rom_modes)) {
                rom_basis_.reset();
            } else {
                rom_b = rom_basis_.get();
            }
        }
        if (rom_b && rom_b->num_kept > 0) {
            rom_ = std::make_unique<SpectralROM>();
            rom_->basis               = rom_b;
            rom_->n_ensemble          = opts.rom_members;
            rom_->mannings_pert       = opts.rom_mannings_pert;
            rom_->rainfall_pert       = opts.rom_rainfall_pert;
            rom_->mode_drop_threshold = opts.rom_mode_drop_threshold;
            rom_->initialize();
            rom_seeded_ = false;
        }
    }
}


double CvodeSurfaceSolver::advance(double t_current, double t_target) {
    if (!cvode_mem_) return t_current;

    int nt = ctx_.mesh->n_triangles();

    // Set stop time to guarantee exact arrival
    CVodeSetStopTime(cvode_mem_, t_target);

    // Advance CVODE
    double t_reached = t_current;
    int flag = CVode(cvode_mem_, t_target, y_, &t_reached, CV_NORMAL);

    // Always record solver statistics (even on failure, for benchmarking)
    CVodeGetNumSteps(cvode_mem_, &last_nsteps_);
    CVodeGetLastStep(cvode_mem_, &last_h_);

    // Always copy solution: even on failure, y_ holds the last valid state
    // at t_reached so partial progress is captured and the solver stays consistent.
    double* y_data = N_VGetArrayPointer(y_);
    for (int i = 0; i < nt; ++i) {
        ctx_.state->head[i]  = y_data[i];
        ctx_.state->depth[i] = std::max(y_data[i] - ctx_.mesh->tri_cz[i], 0.0);
    }

    // Advance the ROM sidecar over [t_current, t_reached].
    // Handles both the success path and the partial-progress failure path.
    auto advanceROM = [&](double t_at, double dt) {
        if (!rom_ || dt <= 0.0) return;

        if (!rom_seeded_) {
            seedROM();
            last_reseed_t_ = t_at;
        } else {
            maybeReseedROM(t_at);
        }

        const double* rain = ctx_.state->rainfall.empty()
                                 ? nullptr
                                 : ctx_.state->rainfall.data();
        // Option A (Rayleigh-quotient K_eff) is active only when the basis
        // is geometric.  When depth-weighted (Option B), eigenvalues already
        // encode per-mode depth correction; passing h_cell would double-count.
        const double* h_cell = rom_->basis->depth_weighted
                                   ? nullptr
                                   : ctx_.state->depth.data();
        rom_->advance(dt, effective_keff_, rain, h_cell);
        rom_->computeQuantiles(ctx_.opts->rom_parametric_tails);
    };

    if (flag < 0) {
        advanceROM(t_reached, t_reached - t_current);
        return t_reached;
    }

    advanceROM(t_reached, t_reached - t_current);
    return t_reached;
}


// ============================================================================
// AUTO K_eff helpers
// ============================================================================

double CvodeSurfaceSolver::computeMeanWetDepth() const noexcept {
    const auto& depth = ctx_.state->depth;
    const double dry = ctx_.opts->dry_depth;
    double sum = 0.0;
    int n_wet = 0;
    for (auto h : depth) {
        if (h > dry) { sum += h; ++n_wet; }
    }
    return n_wet > 0 ? sum / static_cast<double>(n_wet) : 0.0;
}

double CvodeSurfaceSolver::computeMeanManningsN() const noexcept {
    const auto& n_arr = ctx_.mesh->mannings_n;
    if (n_arr.empty()) return 0.035;
    double sum = 0.0;
    for (auto n : n_arr) sum += n;
    return sum / static_cast<double>(n_arr.size());
}

double CvodeSurfaceSolver::computeMeanBedSlope() const noexcept {
    const auto& mesh = *ctx_.mesh;
    int nt = mesh.n_triangles();
    double sum = 0.0;
    int n_edges = 0;
    for (int i = 0; i < nt; ++i) {
        const int nbrs[3] = {mesh.tri_nbr0[static_cast<std::size_t>(i)],
                             mesh.tri_nbr1[static_cast<std::size_t>(i)],
                             mesh.tri_nbr2[static_cast<std::size_t>(i)]};
        for (int e = 0; e < 3; ++e) {
            int j = nbrs[e];
            if (j <= i) continue;   // boundary or already processed
            double dx = mesh.tri_cx[static_cast<std::size_t>(j)]
                      - mesh.tri_cx[static_cast<std::size_t>(i)];
            double dy = mesh.tri_cy[static_cast<std::size_t>(j)]
                      - mesh.tri_cy[static_cast<std::size_t>(i)];
            double dz = mesh.tri_cz[static_cast<std::size_t>(j)]
                      - mesh.tri_cz[static_cast<std::size_t>(i)];
            double d  = std::sqrt(dx*dx + dy*dy);
            if (d < 1.0e-14) continue;
            sum += std::abs(dz) / d;
            ++n_edges;
        }
    }
    return n_edges > 0 ? sum / static_cast<double>(n_edges) : 0.0;
}

double CvodeSurfaceSolver::computeAutoKEff() const noexcept {
    // Positive rom_k_eff is always an explicit user override.
    if (ctx_.opts->rom_k_eff > 0.0) return ctx_.opts->rom_k_eff;

    const double h_mean = computeMeanWetDepth();
    if (h_mean < 1.0e-9) return 0.0;   // all dry: ROM uses Euler forcing only

    const double n_mean = std::max(computeMeanManningsN(), 1.0e-4);
    constexpr double S_FLOOR = 1.0e-6; // flat-domain fallback (avoids ÷ sqrt(0))
    const double S_mean = std::max(computeMeanBedSlope(), S_FLOOR);

    return std::pow(h_mean, 5.0 / 3.0) / (2.0 * n_mean * std::sqrt(S_mean));
}

void CvodeSurfaceSolver::seedROM() {
    // Compute (or lock in) the effective K_eff for this ensemble epoch.
    effective_keff_ = computeAutoKEff();

    // Option B: on the very first seed, rebuild the standalone ROM eigenbasis
    // using depth-weighted edge conductances D_t = h[t]^(5/3).  This aligns
    // the mode shapes with the actual diffusivity distribution at t=0.
    // Only applies when the ROM owns a standalone basis (not sharing with the
    // spectral preconditioner), so the preconditioner is never affected.
    if (rom_basis_) {
        int nt = ctx_.mesh->n_triangles();

        // Normalised D_cell: (h[t] / h_mean)^(5/3)
        // Using h_mean as the reference keeps the eigenvalue scale identical to
        // the geometric Laplacian for a uniform depth field, so K_eff_global
        // continues to multiply them correctly.  Failure leaves the geometric
        // basis intact (buildDepthWeighted rolls back on Lanczos failure).
        double h_sum = 0.0;
        for (int t = 0; t < nt; ++t)
            h_sum += std::max(ctx_.state->depth[static_cast<std::size_t>(t)], 0.0);
        const double h_mean = h_sum / static_cast<double>(nt);

        std::vector<double> D_cell(static_cast<std::size_t>(nt), 1.0);
        if (h_mean > 1.0e-9) {
            const double inv_h53 = 1.0 / std::pow(h_mean, 5.0 / 3.0);
            for (int t = 0; t < nt; ++t) {
                double h_t = std::max(
                    ctx_.state->depth[static_cast<std::size_t>(t)], 0.0);
                D_cell[static_cast<std::size_t>(t)] =
                    std::pow(h_t, 5.0 / 3.0) * inv_h53;
            }
        }

        rom_basis_->buildDepthWeighted(*ctx_.mesh, rom_basis_->num_modes,
                                       D_cell.data());
    }

    rom_->seed(ctx_.state->depth.data());
    rom_seeded_ = true;

    // Generate spatially-correlated Manning and rainfall multiplier fields.
    // When corr_len == 0 (scalar mode), clear the spatial field so the fast
    // scalar path in SpectralROM::advance() and applyCouplingFlux() is used.
    const int n_tri   = ctx_.mesh->n_triangles();
    const double m_cl = ctx_.opts->rom_mannings_corr_len;
    const double r_cl = ctx_.opts->rom_rainfall_corr_len;

    if (m_cl > 0.0) {
        CorrelatedFieldGenerator::generate(
            ctx_.mesh->tri_cx.data(), ctx_.mesh->tri_cy.data(), n_tri,
            rom_->mannings_mult, ctx_.opts->rom_mannings_pert, m_cl,
            /*seed=*/UINT64_C(0xdeadbeef01), rom_->spatial_mannings);
    } else {
        rom_->spatial_mannings.clear();
    }

    if (r_cl > 0.0) {
        CorrelatedFieldGenerator::generate(
            ctx_.mesh->tri_cx.data(), ctx_.mesh->tri_cy.data(), n_tri,
            rom_->rainfall_mult, ctx_.opts->rom_rainfall_pert, r_cl,
            /*seed=*/UINT64_C(0xcafebabe02), rom_->spatial_rainfall);
    } else {
        rom_->spatial_rainfall.clear();
    }

    // Record current wet-cell count so maybeReseedROM can detect domain growth.
    constexpr double DRY_THRESH = 1.0e-4;
    wet_count_at_seed_ = 0;
    for (auto h : ctx_.state->depth)
        if (h > DRY_THRESH) ++wet_count_at_seed_;
}


void CvodeSurfaceSolver::maybeReseedROM(double t_reached) {
    if (!rom_ || !rom_seeded_) return;
    if (t_reached - last_reseed_t_ < ctx_.opts->rom_wet_reseed_min_interval) return;

    constexpr double DRY_THRESH = 1.0e-4;
    int wet_now = 0;
    for (auto h : ctx_.state->depth)
        if (h > DRY_THRESH) ++wet_now;

    int n_tri = ctx_.mesh->n_triangles();
    int threshold = static_cast<int>(ctx_.opts->rom_wet_reseed_fraction
                                     * static_cast<double>(n_tri));
    if (std::abs(wet_now - wet_count_at_seed_) > threshold) {
        seedROM();
        last_reseed_t_ = t_reached;
    }
}


void CvodeSurfaceSolver::reinitialize(double t0) {
    if (!cvode_mem_) return;

    int nt = ctx_.mesh->n_triangles();
    // H-formulation: y is water-surface elevation.
    double* y_data = N_VGetArrayPointer(y_);
    std::memcpy(y_data, ctx_.state->head.data(), nt * sizeof(double));

    CVodeReInit(cvode_mem_, t0, y_);
}


long CvodeSurfaceSolver::get_num_lin_iters() const noexcept {
    if (!cvode_mem_) return 0;
    long n = 0;
    CVodeGetNumLinIters(cvode_mem_, &n);
    return n;
}


void CvodeSurfaceSolver::suppress_warnings() noexcept {
    if (!cvode_mem_) return;
    CVodeSetMaxHnilWarns(cvode_mem_, 0);
}


void CvodeSurfaceSolver::applyCouplingFluxToROM(
    const std::vector<CouplingPoint>& cps,
    const double* node_heads,
    const MeshData& mesh,
    double dt)
{
    if (rom_ && rom_->is_ready())
        rom_->applyCouplingFlux(cps, node_heads, mesh, dt, rom1d_);
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
    if (sun_ctx_) {
        SUNContext_Free(&sun_ctx_);
        sun_ctx_ = nullptr;
    }
    precond_.reset();
    ctx_.precond = nullptr;
    rom_.reset();
    rom_basis_.reset();
    rom_seeded_ = false;
    wet_count_at_seed_ = 0;
    last_reseed_t_ = -1.0;
    effective_keff_ = 0.0;
    precond_diag_.clear();
    ctx_.solver = nullptr;
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
