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

#include <cvode/cvode.h>
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
      last_nsteps_(o.last_nsteps_), last_h_(o.last_h_) {
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
        last_nsteps_ = o.last_nsteps_;
        last_h_ = o.last_h_;
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

    // Create iterative linear solver
    switch (opts.linear_solver) {
        case LinearSolverType::GMRES:
            ls_ = SUNLinSol_SPGMR(y_, SUN_PREC_NONE, opts.max_krylov_dim,
                                   sun_ctx_);
            break;
        case LinearSolverType::BICGSTAB:
            ls_ = SUNLinSol_SPBCGS(y_, SUN_PREC_NONE, opts.max_krylov_dim,
                                    sun_ctx_);
            break;
        case LinearSolverType::TFQMR:
            ls_ = SUNLinSol_SPTFQMR(y_, SUN_PREC_NONE, opts.max_krylov_dim,
                                      sun_ctx_);
            break;
    }
    if (!ls_)
        throw std::runtime_error("SUNLinSol creation failed");

    // Attach linear solver to CVODE
    err = CVodeSetLinearSolver(cvode_mem_, ls_, nullptr);
    if (err != CV_SUCCESS)
        throw std::runtime_error("CVodeSetLinearSolver failed");
}


double CvodeSurfaceSolver::advance(double t_current, double t_target) {
    if (!cvode_mem_) return t_current;

    int nt = ctx_.mesh->n_triangles();

    // Set stop time to guarantee exact arrival
    CVodeSetStopTime(cvode_mem_, t_target);

    // Advance CVODE
    double t_reached = t_current;
    int flag = CVode(cvode_mem_, t_target, y_, &t_reached, CV_NORMAL);

    if (flag < 0) {
        // CVODE failure — leave state unchanged
        return t_current;
    }

    // Copy solution back to state
    double* y_data = N_VGetArrayPointer(y_);
    for (int i = 0; i < nt; ++i) {
        ctx_.state->depth[i] = std::max(y_data[i], 0.0);
        ctx_.state->head[i]  = ctx_.mesh->tri_cz[i] + ctx_.state->depth[i];
    }

    // Record solver statistics
    CVodeGetNumSteps(cvode_mem_, &last_nsteps_);
    CVodeGetLastStep(cvode_mem_, &last_h_);

    return t_reached;
}


void CvodeSurfaceSolver::reinitialize(double t0) {
    if (!cvode_mem_) return;

    int nt = ctx_.mesh->n_triangles();
    double* y_data = N_VGetArrayPointer(y_);
    std::memcpy(y_data, ctx_.state->depth.data(), nt * sizeof(double));

    CVodeReInit(cvode_mem_, t0, y_);
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
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
