/**
 * @file ArkodeKokkosSurfaceSolver.cpp
 * @brief Implementation of the Kokkos + ARKStep local-inertial surface solver.
 *
 * @see ArkodeKokkosSurfaceSolver.hpp
 * @ingroup engine_2d_gpu
 */

#include "ArkodeKokkosSurfaceSolver.hpp"

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../solver/InertialEdges.hpp"

#include <arkode/arkode.h>
#include <arkode/arkode_arkstep.h>
#include <arkode/arkode_ls.h>
#include <sunlinsol/sunlinsol_spgmr.h>

#include <stdexcept>
#include <vector>
#include <cstdlib>

static_assert(sizeof(sunrealtype) == sizeof(double),
              "ArkodeKokkosSurfaceSolver requires a double-precision SUNDIALS build");

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
// Host<->device transfers
// ===========================================================================

void ArkodeKokkosSurfaceSolver::uploadMesh() {
    auto& m = *mesh_host_;
    mesh_v_.n_tri  = nt_;
    mesh_v_.n_vert = nv_;
    mesh_v_.tri_cz     = mirrorD(m.tri_cz,     "tri_cz");
    mesh_v_.tri_area   = mirrorD(m.tri_area,   "tri_area");
    mesh_v_.tri_cx     = mirrorD(m.tri_cx,     "tri_cx");
    mesh_v_.tri_cy     = mirrorD(m.tri_cy,     "tri_cy");
    mesh_v_.mannings_n = mirrorD(m.mannings_n, "mannings_n");
    mesh_v_.tri_nbr0   = mirrorI(m.tri_nbr0,   "tri_nbr0");
    mesh_v_.tri_nbr1   = mirrorI(m.tri_nbr1,   "tri_nbr1");
    mesh_v_.tri_nbr2   = mirrorI(m.tri_nbr2,   "tri_nbr2");
    mesh_v_.edge_length= mirrorD(m.edge_length,"edge_length");
}

void ArkodeKokkosSurfaceSolver::uploadEdges() {
    // Build the unique-edge structure host-side, then mirror to device.
    InertialEdges E;
    E.build(*mesh_host_);
    ne_ = E.ne;
    edges_v_.ne        = E.ne;
    edges_v_.cL        = mirrorI(E.cL,        "e_cL");
    edges_v_.cR        = mirrorI(E.cR,        "e_cR");
    edges_v_.xi        = mirrorD(E.xi,        "e_xi");
    edges_v_.inv_dx    = mirrorD(E.inv_dx,    "e_invdx");
    edges_v_.zface     = mirrorD(E.zface,     "e_zface");
    edges_v_.slotL     = mirrorI(E.slotL,     "e_slotL");
    edges_v_.slotR     = mirrorI(E.slotR,     "e_slotR");
    edges_v_.cell_ptr  = mirrorI(E.cell_ptr,  "e_cptr");
    edges_v_.cell_edge = mirrorI(E.cell_edge, "e_cedge");
    edges_v_.cell_sign = mirrorD(E.cell_sign, "e_csign");
}

void ArkodeKokkosSurfaceSolver::uploadSources() {
    copyToDevice(state_host_->rainfall,      state_v_.rainfall);
    copyToDevice(state_host_->coupling_flux, state_v_.coupling_flux);
    copyToDevice(state_host_->evap_rate,     state_v_.evap_rate);
}

void ArkodeKokkosSurfaceSolver::seedState() {
    // V = A·max(η − tri_cz, 0) from the host free surface; q starts at rest.
    std::vector<double> y(static_cast<std::size_t>(nt_ + ne_), 0.0);
    for (int i = 0; i < nt_; ++i) {
        const double d = state_host_->head[i] - mesh_host_->tri_cz[i];
        y[i] = (d > 0.0) ? mesh_host_->tri_area[i] * d : 0.0;
        state_host_->volume[i] = y[i];
    }
    Kokkos::deep_copy(y_->View(), HostConstD(y.data(), y.size()));
}

void ArkodeKokkosSurfaceSolver::downloadState() {
    // Copy the V block (first nt entries) back; reconstruct η, h̄.
    auto yv = y_->View();
    auto Vsub = Kokkos::subview(yv, Kokkos::make_pair(0, nt_));
    Kokkos::deep_copy(HostMutD(state_host_->volume.data(),
                               static_cast<std::size_t>(nt_)), Vsub);
    for (int i = 0; i < nt_; ++i) {
        const double A = mesh_host_->tri_area[i];
        const double v = (state_host_->volume[i] > 0.0) ? state_host_->volume[i] : 0.0;
        const double d = (A > 1.0e-30) ? v / A : 0.0;
        state_host_->depth[i] = d;
        state_host_->head[i]  = mesh_host_->tri_cz[i] + d;
    }
    // Project q → edge_flux on device, then copy to host (SurfaceRouter2D reads
    // state.edge_flux for continuity / velocity / mass balance and skips its DW
    // recompute in inertial mode).
    writebackEdgeFlux(nt_, edges_v_, yv, state_v_.edge_flux);
    Kokkos::fence();
    copyToHost(state_v_.edge_flux, state_host_->edge_flux);
}

// ===========================================================================
// ARKStep callbacks
// ===========================================================================

int ArkodeKokkosSurfaceSolver::fe_cb(sunrealtype, N_Vector y, N_Vector ydot,
                                     void* user_data) {
    auto* c = static_cast<ArkodeKokkosContext*>(user_data);
    evaluateInertialFe(*c->mesh, *c->state, *c->edges,
                       viewOf(y), viewOf(ydot), c->dry_depth);
    Kokkos::fence();
    return 0;
}

int ArkodeKokkosSurfaceSolver::fi_cb(sunrealtype, N_Vector y, N_Vector ydot,
                                     void* user_data) {
    auto* c = static_cast<ArkodeKokkosContext*>(user_data);
    evaluateInertialFi(*c->mesh, *c->state, *c->edges,
                       viewOf(y), viewOf(ydot), c->dry_depth);
    Kokkos::fence();
    return 0;
}

int ArkodeKokkosSurfaceSolver::psetup_cb(sunrealtype, N_Vector y, N_Vector,
                                         sunbooleantype, sunbooleantype* jcurPtr,
                                         sunrealtype gamma, void* user_data) {
    auto* c = static_cast<ArkodeKokkosContext*>(user_data);
    if (jcurPtr) *jcurPtr = SUNTRUE;   // friction diagonal recomputed each call
    precondInertialSetup(*c->mesh, *c->state, *c->edges,
                         viewOf(y), c->prec_wq, gamma, c->dry_depth);
    Kokkos::fence();
    return 0;
}

int ArkodeKokkosSurfaceSolver::psolve_cb(sunrealtype, N_Vector, N_Vector,
                                         N_Vector r, N_Vector z,
                                         sunrealtype, sunrealtype, int,
                                         void* user_data) {
    auto* c = static_cast<ArkodeKokkosContext*>(user_data);
    precondInertialSolve(c->nt, *c->edges, viewOf(r), viewOf(z), c->prec_wq);
    Kokkos::fence();
    return 0;
}

// ===========================================================================
// Lifecycle
// ===========================================================================

ArkodeKokkosSurfaceSolver::ArkodeKokkosSurfaceSolver() = default;

ArkodeKokkosSurfaceSolver::~ArkodeKokkosSurfaceSolver() { finalize(); }

void ArkodeKokkosSurfaceSolver::initialize(MeshData& mesh, SurfaceStateData& state,
                                           SolverOptions2D& opts) {
    if (arkode_mem_) finalize();

    nt_ = mesh.n_triangles();
    nv_ = mesh.n_vertices();
    if (nt_ <= 0) return;

    mesh_host_  = &mesh;
    state_host_ = &state;
    opts_host_  = &opts;

    if (SUNContext_Create(SUN_COMM_NULL, &sun_ctx_) != 0)
        throw std::runtime_error("SUNContext_Create failed");

    uploadMesh();
    uploadEdges();

    const int ntot = nt_ + ne_;
    y_ = std::make_unique<VecType>(static_cast<sunindextype>(ntot), sun_ctx_);

    // Device state buffers.
    state_v_.head      = DView("s_head",  nt_);
    state_v_.depth     = DView("s_depth", nt_);
    state_v_.edge_flux = DView("s_eflux", static_cast<std::size_t>(nt_) * 3);
    state_v_.rainfall     = DView("s_rain", nt_);
    state_v_.coupling_flux= DView("s_coup", nt_);
    state_v_.evap_rate    = DView("s_evap", nt_);
    prec_wq_ = DView("prec_wq", ne_ > 0 ? ne_ : 1);

    seedState();
    uploadSources();

    ctx_.mesh      = &mesh_v_;
    ctx_.state     = &state_v_;
    ctx_.edges     = &edges_v_;
    ctx_.prec_wq   = prec_wq_;
    ctx_.dry_depth = opts.dry_depth;
    ctx_.nt        = nt_;
    ctx_.ne        = ne_;

    arkode_mem_ = ARKStepCreate(fe_cb, fi_cb, 0.0, y_->Convert(), sun_ctx_);
    if (!arkode_mem_) throw std::runtime_error("ARKStepCreate failed");

    if (ARKodeSetOrder(arkode_mem_, 3) != ARK_SUCCESS)
        throw std::runtime_error("ARKodeSetOrder failed");

    // Per-DOF tolerances: cells = ABS_TOLERANCE·A_i (depth floor); q = ABS_TOLERANCE.
    abstol_ = std::make_unique<VecType>(static_cast<sunindextype>(ntot), sun_ctx_);
    {
        std::vector<double> at(static_cast<std::size_t>(ntot));
        for (int i = 0; i < nt_; ++i) {
            const double A = mesh.tri_area[i];
            at[i] = opts.abs_tolerance * ((A > 1.0e-30) ? A : 1.0);
        }
        for (int e = 0; e < ne_; ++e) at[nt_ + e] = opts.abs_tolerance;
        Kokkos::deep_copy(abstol_->View(), HostConstD(at.data(), at.size()));
    }
    if (ARKodeSVtolerances(arkode_mem_, opts.rel_tolerance, abstol_->Convert())
            != ARK_SUCCESS)
        throw std::runtime_error("ARKodeSVtolerances failed");

    ARKodeSetUserData(arkode_mem_, &ctx_);
    ARKodeSetMinStep(arkode_mem_, opts.min_timestep);
    ARKodeSetMaxStep(arkode_mem_, opts.max_timestep);
    ARKodeSetMaxNumSteps(arkode_mem_, opts.max_cvode_steps);

    ls_ = SUNLinSol_SPGMR(y_->Convert(), SUN_PREC_LEFT, opts.max_krylov_dim, sun_ctx_);
    if (!ls_) throw std::runtime_error("SUNLinSol_SPGMR creation failed");
    if (ARKodeSetLinearSolver(arkode_mem_, ls_, nullptr) != ARK_SUCCESS)
        throw std::runtime_error("ARKodeSetLinearSolver failed");
    if (ARKodeSetPreconditioner(arkode_mem_, psetup_cb, psolve_cb) != ARK_SUCCESS)
        throw std::runtime_error("ARKodeSetPreconditioner failed");
}

double ArkodeKokkosSurfaceSolver::advance(double t_current, double t_target) {
    if (!arkode_mem_) return t_current;

    uploadSources();

    ARKodeSetStopTime(arkode_mem_, t_target);
    double t_reached = t_current;
    const int flag = ARKodeEvolve(arkode_mem_, t_target, y_->Convert(),
                                  &t_reached, ARK_NORMAL);
    if (flag < 0) return t_current;

    downloadState();
    ARKodeGetNumSteps(arkode_mem_, &last_nsteps_);
    ARKodeGetLastStep(arkode_mem_, &last_h_);
    return t_reached;
}

void ArkodeKokkosSurfaceSolver::reinitialize(double t0) {
    if (!arkode_mem_) return;
    seedState();  // V from head; q kept
    ARKStepReInit(arkode_mem_, fe_cb, fi_cb, t0, y_->Convert());
}

void ArkodeKokkosSurfaceSolver::finalize() {
    if (arkode_mem_) { ARKodeFree(&arkode_mem_); arkode_mem_ = nullptr; }
    if (ls_)         { SUNLinSolFree(ls_);        ls_ = nullptr; }
    y_.reset();
    abstol_.reset();
    if (sun_ctx_)    { SUNContext_Free(&sun_ctx_); sun_ctx_ = nullptr; }
}

} // namespace gpu
} // namespace openswmm::twoD
