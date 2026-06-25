/**
 * @file CvodeKokkosSurfaceSolver.cpp
 * @brief Implementation of the Phase 1 Kokkos/OpenMP CVODE surface solver.
 *
 * @see CvodeKokkosSurfaceSolver.hpp
 * @ingroup engine_2d_gpu
 */

#include "CvodeKokkosSurfaceSolver.hpp"
#if defined(OPENSWMM_HAVE_HYPRE)
#include "KokkosAmgPreconditioner.hpp"
#endif

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/BoundaryData.hpp"

#include <cvode/cvode.h>
#include <cvode/cvode_ls.h>
#include <sunlinsol/sunlinsol_spgmr.h>

#include <stdexcept>
#include <vector>
#include <cstdlib>

// The kernels treat the SUNDIALS vector payload as double. SUNDIALS must be
// built in double precision (the default). Fail fast otherwise.
static_assert(sizeof(sunrealtype) == sizeof(double),
              "CvodeKokkosSurfaceSolver requires a double-precision SUNDIALS build");

namespace openswmm::twoD {
namespace gpu {

namespace {

using HostConstD = Kokkos::View<const double*, Kokkos::HostSpace,
                                Kokkos::MemoryTraits<Kokkos::Unmanaged>>;
using HostConstI = Kokkos::View<const int*, Kokkos::HostSpace,
                                Kokkos::MemoryTraits<Kokkos::Unmanaged>>;
using HostMutD   = Kokkos::View<double*, Kokkos::HostSpace,
                                Kokkos::MemoryTraits<Kokkos::Unmanaged>>;

CDView mirrorD(const std::vector<double>& h, const char* name) {
    DView d(std::string(name), h.size());
    if (!h.empty()) Kokkos::deep_copy(d, HostConstD(h.data(), h.size()));
    return d;
}

CIView mirrorI(const std::vector<int>& h, const char* name) {
    IView d(std::string(name), h.size());
    if (!h.empty()) Kokkos::deep_copy(d, HostConstI(h.data(), h.size()));
    return d;
}

void copyToDevice(const std::vector<double>& h, DView d) {
    if (!h.empty()) Kokkos::deep_copy(d, HostConstD(h.data(), h.size()));
}

void copyToHost(DView d, std::vector<double>& h) {
    if (!h.empty()) Kokkos::deep_copy(HostMutD(h.data(), h.size()), d);
}

DView viewOf(N_Vector v) {
    return ::sundials::kokkos::GetVec<VecType>(v)->View();
}

} // anonymous namespace

// ===========================================================================
// Host<->device transfers (no-ops in Phase 1 OpenMP host space; real copies
// once ExecSpace is a device space in Phase 2).
// ===========================================================================

void CvodeKokkosSurfaceSolver::uploadMesh() {
    auto& m = *mesh_host_;
    mesh_v_.n_tri  = nt_;
    mesh_v_.n_vert = nv_;
    mesh_v_.tri_cz          = mirrorD(m.tri_cz,          "tri_cz");
    mesh_v_.tri_area        = mirrorD(m.tri_area,        "tri_area");
    mesh_v_.tri_cx          = mirrorD(m.tri_cx,          "tri_cx");
    mesh_v_.tri_cy          = mirrorD(m.tri_cy,          "tri_cy");
    mesh_v_.mannings_n      = mirrorD(m.mannings_n,      "mannings_n");
    mesh_v_.tri_nbr0        = mirrorI(m.tri_nbr0,        "tri_nbr0");
    mesh_v_.tri_nbr1        = mirrorI(m.tri_nbr1,        "tri_nbr1");
    mesh_v_.tri_nbr2        = mirrorI(m.tri_nbr2,        "tri_nbr2");
    mesh_v_.edge_length     = mirrorD(m.edge_length,     "edge_length");
    mesh_v_.edge_nx         = mirrorD(m.edge_nx,         "edge_nx");
    mesh_v_.edge_ny         = mirrorD(m.edge_ny,         "edge_ny");
    mesh_v_.edge_conveyance = mirrorD(m.edge_conveyance, "edge_conveyance");
    mesh_v_.vert_ptr        = mirrorI(m.vert_stencil_ptr, "vert_ptr");
    mesh_v_.vert_idx        = mirrorI(m.vert_stencil_idx, "vert_idx");
    mesh_v_.vert_wt         = mirrorD(m.vert_stencil_wt,  "vert_wt");
}

void CvodeKokkosSurfaceSolver::uploadSources() {
    copyToDevice(state_host_->rainfall,      state_v_.rainfall);
    copyToDevice(state_host_->coupling_flux, state_v_.coupling_flux);
    copyToDevice(state_host_->evap_rate,     state_v_.evap_rate);
}

void CvodeKokkosSurfaceSolver::uploadBoundaryStatic() {
    // Allocate + upload the static boundary fields (type, bed slope) once. When
    // no boundary conditions are attached the views stay extent-0 and the kernel
    // treats every boundary edge as a wall (legacy behaviour).
    const BoundaryData* b = state_host_->boundary;
    if (!b || b->size() == 0) return;
    const auto ne = static_cast<std::size_t>(b->size());
    state_v_.edge_bc_type   = IView("s_bctype",  ne);
    state_v_.edge_bed_slope = DView("s_bcslope", ne);
    state_v_.edge_bc_head   = DView("s_bchead",  ne);
    state_v_.edge_bc_flow   = DView("s_bcflow",  ne);
    // edge_bc_type is int8 on the host; widen to int for the device IView.
    std::vector<int> bctype_i(ne);
    for (std::size_t k = 0; k < ne; ++k)
        bctype_i[k] = static_cast<int>(b->edge_bc_type[k]);
    Kokkos::deep_copy(state_v_.edge_bc_type, HostConstI(bctype_i.data(), ne));
    copyToDevice(b->edge_bed_slope, state_v_.edge_bed_slope);
    uploadBoundaryDynamic();
}

void CvodeKokkosSurfaceSolver::uploadBoundaryDynamic() {
    // Re-upload the per-step-resolved boundary driving values (timeseries /
    // rating curve), already updated host-side by SurfaceRouter2D before advance.
    const BoundaryData* b = state_host_->boundary;
    if (!b || b->size() == 0) return;
    copyToDevice(b->edge_bc_head, state_v_.edge_bc_head);
    copyToDevice(b->edge_bc_flow, state_v_.edge_bc_flow);
}

void CvodeKokkosSurfaceSolver::seedVolumeFromHead() {
    // y is the cell volume V; seed it from the (possibly externally edited)
    // free surface: V = A·max(η − tri_cz, 0). Mirror into state.volume so
    // totalVolume() is correct before the first step (matches the serial
    // CvodeSurfaceSolver::initialize seed).
    std::vector<double> V(static_cast<std::size_t>(nt_));
    for (int i = 0; i < nt_; ++i) {
        const double d = state_host_->head[i] - mesh_host_->tri_cz[i];
        V[i] = (d > 0.0) ? mesh_host_->tri_area[i] * d : 0.0;
        state_host_->volume[i] = V[i];
    }
    copyToDevice(V, y_->View());
}

void CvodeKokkosSurfaceSolver::downloadState() {
    // y is the cell volume V. Store V and the reconstructed (η, h̄) so
    // totalVolume / continuity / output are consistent, exactly as the serial
    // solver does after CVode (CvodeSurfaceSolver::advance; reconstructFromVolume:
    // h̄ = V/A, η = tri_cz + h̄).
    copyToHost(y_->View(), state_host_->volume);
    for (int i = 0; i < nt_; ++i) {
        const double A = mesh_host_->tri_area[i];
        const double v = (state_host_->volume[i] > 0.0) ? state_host_->volume[i] : 0.0;
        const double d = (A > 1.0e-30) ? v / A : 0.0;
        state_host_->depth[i] = d;
        state_host_->head[i]  = mesh_host_->tri_cz[i] + d;
    }

    // Diagnostic fields the 2D HDF5 writer reads. The serial solver leaves
    // these in the shared host state as a side effect of its final rhs_fn;
    // the Kokkos path computes them on device, so copy them back to match —
    // node_head is the vertex-reconstructed head (vert_head), plus the
    // unlimited/limited gradient envelopes. edge_flux, face velocity and the
    // continuity residual are refreshed host-side by SurfaceRouter2D after
    // advance(), so they are intentionally NOT copied here.
    copyToHost(state_v_.vert_head,   state_host_->vert_head);
    copyToHost(state_v_.grad_hx,     state_host_->grad_hx);
    copyToHost(state_v_.grad_hy,     state_host_->grad_hy);
    copyToHost(state_v_.grad_hx_lim, state_host_->grad_hx_lim);
    copyToHost(state_v_.grad_hy_lim, state_host_->grad_hy_lim);
}

// ===========================================================================
// CVODE callbacks
// ===========================================================================

int CvodeKokkosSurfaceSolver::rhs_cb(sunrealtype /*t*/, N_Vector y,
                                     N_Vector ydot, void* user_data) {
    auto* c = static_cast<KokkosSolverContext*>(user_data);
    evaluateRhs(*c->mesh, *c->state, viewOf(y), viewOf(ydot),
                c->dry_depth, c->limiter_eps, c->flux_dh_eps);
    Kokkos::fence();
    return 0;
}

int CvodeKokkosSurfaceSolver::psetup_cb(sunrealtype /*t*/, N_Vector /*y*/,
                                        N_Vector /*fy*/, sunbooleantype /*jok*/,
                                        sunbooleantype* jcurPtr,
                                        sunrealtype gamma, void* user_data) {
    auto* c = static_cast<KokkosSolverContext*>(user_data);
    *jcurPtr = SUNTRUE;  // always rebuild here; CVODE's policy provides the lag.
#if defined(OPENSWMM_HAVE_HYPRE)
    if (c->use_amg) {
        c->amg->setup(*c->mesh, *c->state, gamma);
        return 0;
    }
#else
    (void)gamma;
#endif
    precondSetup(*c->mesh, *c->state);
    Kokkos::fence();
    return 0;
}

int CvodeKokkosSurfaceSolver::psolve_cb(sunrealtype /*t*/, N_Vector /*y*/,
                                        N_Vector /*fy*/, N_Vector r, N_Vector z,
                                        sunrealtype gamma, sunrealtype /*delta*/,
                                        int /*lr*/, void* user_data) {
    auto* c = static_cast<KokkosSolverContext*>(user_data);
#if defined(OPENSWMM_HAVE_HYPRE)
    if (c->use_amg) {
        c->amg->solve(viewOf(r), viewOf(z), gamma);
        return 0;
    }
#endif
    precondSolve(*c->state, viewOf(r), viewOf(z), gamma);
    Kokkos::fence();
    return 0;
}

// ===========================================================================
// Lifecycle
// ===========================================================================

CvodeKokkosSurfaceSolver::CvodeKokkosSurfaceSolver() = default;

CvodeKokkosSurfaceSolver::~CvodeKokkosSurfaceSolver() {
    finalize();
}

void CvodeKokkosSurfaceSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                          SolverOptions2D& opts) {
    if (cvode_mem_) finalize();

    nt_ = mesh.n_triangles();
    nv_ = mesh.n_vertices();
    if (nt_ <= 0) return;

    // Same option gate as the serial solver: GMRES + (NONE | JACOBI | AMG).
    if (opts.linear_solver != LinearSolverType::GMRES) {
        throw std::runtime_error(
            "CvodeKokkosSurfaceSolver: only LINEAR_SOLVER=GMRES is wired");
    }
    if (opts.preconditioner != PreconditionerType::NONE &&
        opts.preconditioner != PreconditionerType::JACOBI &&
        opts.preconditioner != PreconditionerType::AMG) {
        throw std::runtime_error(
            "CvodeKokkosSurfaceSolver: only PRECONDITIONER=NONE, JACOBI, or AMG "
            "is wired; ILU is reserved");
    }
    PreconditionerType pc = opts.preconditioner;  // effective preconditioner
#if !defined(OPENSWMM_HAVE_HYPRE)
    if (pc == PreconditionerType::AMG) {
        pc = PreconditionerType::JACOBI;
    }
#endif

    mesh_host_  = &mesh;
    state_host_ = &state;
    opts_host_  = &opts;

    if (SUNContext_Create(SUN_COMM_NULL, &sun_ctx_) != 0)
        throw std::runtime_error("SUNContext_Create failed");

    // Kokkos-backed N_Vector for the state (water-surface elevation H).
    y_ = std::make_unique<VecType>(static_cast<sunindextype>(nt_), sun_ctx_);

    // Mirror mesh to device once; allocate device state buffers.
    uploadMesh();
    state_v_.head         = DView("s_head",  nt_);
    state_v_.depth        = DView("s_depth", nt_);
    state_v_.grad_hx      = DView("s_gx",    nt_);
    state_v_.grad_hy      = DView("s_gy",    nt_);
    state_v_.grad_hx_lim  = DView("s_gxl",   nt_);
    state_v_.grad_hy_lim  = DView("s_gyl",   nt_);
    state_v_.vert_head    = DView("s_vhead", nv_ > 0 ? nv_ : 0);
    state_v_.edge_flux    = DView("s_eflux", static_cast<std::size_t>(nt_) * 3);
    state_v_.rainfall     = DView("s_rain",  nt_);
    state_v_.coupling_flux= DView("s_coup",  nt_);
    state_v_.evap_rate    = DView("s_evap",  nt_);
    state_v_.precond_diag = DView("s_pdiag", nt_);

    // Initial condition: y = cell volume V (SurfaceRouter2D sets head[i] =
    // tri_cz[i] ⇒ V = 0; a hot start with a raised surface seeds the volume).
    seedVolumeFromHead();
    uploadSources();
    uploadBoundaryStatic();

    // Resolve the √|Δη| flux regularization: [2D_OPTIONS] FLUX_DH_EPS, with the
    // env var OPENSWMM_2D_FLUX_DH_EPS overriding it when set (mirrors the serial
    // SurfaceFluxCalculator::fluxDhEps). Resolved on the host once; the device
    // kernel can't read getenv.
    double dh_eps = opts.flux_dh_eps;
    if (const char* s = std::getenv("OPENSWMM_2D_FLUX_DH_EPS")) {
        const double e = std::atof(s);
        if (e >= 0.0) dh_eps = e;
    }

    ctx_.mesh        = &mesh_v_;
    ctx_.state       = &state_v_;
    ctx_.dry_depth   = opts.dry_depth;
    ctx_.limiter_eps = opts.limiter_epsilon;
    ctx_.flux_dh_eps = dh_eps;

    cvode_mem_ = CVodeCreate(CV_BDF, sun_ctx_);
    if (!cvode_mem_) throw std::runtime_error("CVodeCreate failed");

    if (CVodeInit(cvode_mem_, rhs_cb, 0.0, y_->Convert()) != CV_SUCCESS)
        throw std::runtime_error("CVodeInit failed");

    // Per-cell tolerances (mirrors the serial volume-state path): the integrated
    // state is VOLUME (m³), so the error control is a per-cell absolute volume
    // tolerance atol_i = ABS_TOLERANCE · A_i (a physical depth floor) plus the
    // relative term rtol = REL_TOLERANCE, which scales the tolerance with the
    // cell's water content so deep/violent flow isn't forced to micron-accuracy.
    abstol_ = std::make_unique<VecType>(static_cast<sunindextype>(nt_), sun_ctx_);
    {
        std::vector<double> at(static_cast<std::size_t>(nt_));
        for (int i = 0; i < nt_; ++i) {
            const double A = mesh.tri_area[i];
            at[i] = opts.abs_tolerance * ((A > 1.0e-30) ? A : 1.0);
        }
        copyToDevice(at, abstol_->View());
    }
    if (CVodeSVtolerances(cvode_mem_, opts.rel_tolerance, abstol_->Convert())
            != CV_SUCCESS)
        throw std::runtime_error("CVodeSVtolerances failed");

    CVodeSetUserData(cvode_mem_, &ctx_);
    CVodeSetMinStep(cvode_mem_, opts.min_timestep);
    CVodeSetMaxStep(cvode_mem_, opts.max_timestep);
    CVodeSetMaxNumSteps(cvode_mem_, opts.max_cvode_steps);

    const int prec_type = (pc == PreconditionerType::NONE)
                              ? SUN_PREC_NONE : SUN_PREC_LEFT;
    ls_ = SUNLinSol_SPGMR(y_->Convert(), prec_type, opts.max_krylov_dim, sun_ctx_);
    if (!ls_) throw std::runtime_error("SUNLinSol_SPGMR creation failed");

    if (CVodeSetLinearSolver(cvode_mem_, ls_, nullptr) != CV_SUCCESS)
        throw std::runtime_error("CVodeSetLinearSolver failed");

    if (pc == PreconditionerType::JACOBI) {
        if (CVodeSetPreconditioner(cvode_mem_, psetup_cb, psolve_cb) != CV_SUCCESS)
            throw std::runtime_error("CVodeSetPreconditioner failed");
    }
#if defined(OPENSWMM_HAVE_HYPRE)
    else if (pc == PreconditionerType::AMG) {
        amg_precond_ = std::make_unique<KokkosAmgPreconditioner>();
        amg_precond_->initialize(mesh_v_);
        ctx_.use_amg = true;
        ctx_.amg     = amg_precond_.get();
        if (CVodeSetPreconditioner(cvode_mem_, psetup_cb, psolve_cb) != CV_SUCCESS)
            throw std::runtime_error("CVodeSetPreconditioner (AMG) failed");
    }
#endif
}

double CvodeKokkosSurfaceSolver::advance(double t_current, double t_target) {
    if (!cvode_mem_) return t_current;

    // Refresh the sources held constant across CVODE's internal sub-steps.
    uploadSources();
    uploadBoundaryDynamic();

    CVodeSetStopTime(cvode_mem_, t_target);
    double t_reached = t_current;
    const int flag = CVode(cvode_mem_, t_target, y_->Convert(),
                           &t_reached, CV_NORMAL);
    if (flag < 0) return t_current;  // failure — leave state unchanged

    downloadState();
    CVodeGetNumSteps(cvode_mem_, &last_nsteps_);
    CVodeGetLastStep(cvode_mem_, &last_h_);
    return t_reached;
}

void CvodeKokkosSurfaceSolver::reinitialize(double t0) {
    if (!cvode_mem_) return;
    seedVolumeFromHead();
    CVodeReInit(cvode_mem_, t0, y_->Convert());
}

void CvodeKokkosSurfaceSolver::finalize() {
    if (cvode_mem_) { CVodeFree(&cvode_mem_); cvode_mem_ = nullptr; }
    if (ls_)        { SUNLinSolFree(ls_);     ls_ = nullptr; }
#if defined(OPENSWMM_HAVE_HYPRE)
    ctx_.use_amg = false;
    ctx_.amg     = nullptr;
    amg_precond_.reset();  // release hypre IJ matrix/vectors + AMG hierarchy
#endif
    y_.reset();        // destroy the Kokkos vectors before their context
    abstol_.reset();
    if (sun_ctx_)   { SUNContext_Free(&sun_ctx_); sun_ctx_ = nullptr; }
}

} // namespace gpu
} // namespace openswmm::twoD
