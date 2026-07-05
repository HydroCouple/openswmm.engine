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
#include <sunlinsol/sunlinsol_spbcgs.h>
#include <sunlinsol/sunlinsol_sptfqmr.h>
#include <sundials/sundials_context.h>

#include <cstring>
#include <algorithm>
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

    // 1. Copy y into state.depth, compute head = z + depth
    for (int i = 0; i < nt; ++i) {
        state.depth[i] = std::max(y_data[i], 0.0);
        state.head[i]  = mesh.tri_cz[i] + state.depth[i];
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
      last_reseed_t_(o.last_reseed_t_) {
    o.cvode_mem_ = nullptr;
    o.ls_ = nullptr;
    o.y_ = nullptr;
    o.sun_ctx_ = nullptr;
}

CvodeSurfaceSolver& CvodeSurfaceSolver::operator=(CvodeSurfaceSolver&& o) noexcept {
    if (this != &o) {
        finalize();
        cvode_mem_ = o.cvode_mem_;    o.cvode_mem_ = nullptr;
        ls_ = o.ls_;                  o.ls_ = nullptr;
        y_ = o.y_;                    o.y_ = nullptr;
        sun_ctx_ = o.sun_ctx_;        o.sun_ctx_ = nullptr;
        ctx_ = o.ctx_;
        precond_    = std::move(o.precond_);
        rom_basis_  = std::move(o.rom_basis_);
        rom_        = std::move(o.rom_);
        rom_seeded_ = o.rom_seeded_;
        last_nsteps_ = o.last_nsteps_;
        last_h_ = o.last_h_;
        wet_count_at_seed_ = o.wet_count_at_seed_;
        last_reseed_t_ = o.last_reseed_t_;
    }
    return *this;
}

void CvodeSurfaceSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                     SolverOptions2D& opts) {
    if (cvode_mem_) finalize();

    int nt = mesh.n_triangles();
    if (nt <= 0) return;

    ctx_.mesh  = &mesh;
    ctx_.state = &state;
    ctx_.opts  = &opts;

    // Create SUNDIALS context
    int err = SUNContext_Create(SUN_COMM_NULL, &sun_ctx_);
    if (err != 0)
        throw std::runtime_error("SUNContext_Create failed");

    // Create N_Vector wrapping state.depth
    y_ = N_VNew_Serial(nt, sun_ctx_);
    if (!y_)
        throw std::runtime_error("N_VNew_Serial failed");

    // Copy initial depths into y
    double* y_data = N_VGetArrayPointer(y_);
    std::memcpy(y_data, state.depth.data(), nt * sizeof(double));

    // Create CVODE memory (BDF for stiff systems)
    cvode_mem_ = CVodeCreate(CV_BDF, sun_ctx_);
    if (!cvode_mem_)
        throw std::runtime_error("CVodeCreate failed");

    // Initialize CVODE with the RHS function
    err = CVodeInit(cvode_mem_, rhs_fn, 0.0, y_);
    if (err != CV_SUCCESS)
        throw std::runtime_error("CVodeInit failed");

    // Set tolerances
    err = CVodeSStolerances(cvode_mem_, opts.rel_tolerance, opts.abs_tolerance);
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

    // Build spectral preconditioner (if requested) before creating the linear solver
    bool use_spectral = (opts.preconditioner == PreconditionerType::SPECTRAL);
    if (use_spectral) {
        precond_ = std::make_unique<SpectralPrecond2D>();
        precond_->num_modes = opts.spectral_num_modes;
        if (!precond_->build(mesh, opts.spectral_num_modes)) {
            // Build failed — fall back to no preconditioner silently
            precond_.reset();
            use_spectral = false;
        } else {
            ctx_.precond = precond_.get();
        }
    }

    // Create iterative linear solver
    int prec_side = use_spectral ? SUN_PREC_LEFT : SUN_PREC_NONE;
    switch (opts.linear_solver) {
        case LinearSolverType::GMRES:
            ls_ = SUNLinSol_SPGMR(y_, prec_side, opts.max_krylov_dim, sun_ctx_);
            break;
        case LinearSolverType::BICGSTAB:
            throw std::runtime_error(
                "BICGSTAB linear solver not wired in Phase 1; use GMRES");
        case LinearSolverType::TFQMR:
            throw std::runtime_error(
                "TFQMR linear solver not wired in Phase 1; use GMRES");
    }
    if (!ls_)
        throw std::runtime_error("SUNLinSol creation failed");

    // Attach linear solver to CVODE
    err = CVodeSetLinearSolver(cvode_mem_, ls_, nullptr);
    if (err != CV_SUCCESS)
        throw std::runtime_error("CVodeSetLinearSolver failed");

    // Register preconditioner callbacks
    if (use_spectral) {
        err = CVodeSetPreconditioner(cvode_mem_,
                                     spectral_precond_setup,
                                     spectral_precond_solve);
        if (err != CV_SUCCESS)
            throw std::runtime_error("CVodeSetPreconditioner failed");
    }

    // Initialize ROM sidecar if requested
    if (opts.enable_rom) {
        // Reuse the spectral precond eigenbasis if already built, else build one.
        const SpectralPrecond2D* rom_b = precond_.get();
        if (!rom_b) {
            rom_basis_ = std::make_unique<SpectralPrecond2D>();
            if (!rom_basis_->build(mesh, opts.rom_modes)) {
                rom_basis_.reset();  // basis build failed — ROM disabled
            } else {
                rom_b = rom_basis_.get();
            }
        }
        if (rom_b && rom_b->num_kept > 0) {
            rom_ = std::make_unique<SpectralROM>();
            rom_->basis         = rom_b;
            rom_->n_ensemble         = opts.rom_members;
            rom_->mannings_pert      = opts.rom_mannings_pert;
            rom_->rainfall_pert      = opts.rom_rainfall_pert;
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
        ctx_.state->depth[i] = std::max(y_data[i], 0.0);
        ctx_.state->head[i]  = ctx_.mesh->tri_cz[i] + ctx_.state->depth[i];
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
        // Deterministic depth reference for the deviation form (always the
        // current CVODE depth field; distinct from h_cell, which may be null).
        const double* h_det = ctx_.state->depth.data();
        rom_->advance(dt, effective_keff_, rain, h_cell, h_det);
        rom_->computeQuantiles(h_det, ctx_.opts->rom_parametric_tails);
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
    double* y_data = N_VGetArrayPointer(y_);
    std::memcpy(y_data, ctx_.state->depth.data(), nt * sizeof(double));

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
    if (ls_) {
        SUNLinSolFree(ls_);
        ls_ = nullptr;
    }
    if (cvode_mem_) {
        CVodeFree(&cvode_mem_);
        cvode_mem_ = nullptr;
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
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
