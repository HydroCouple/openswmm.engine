/**
 * @file CvodeKokkosSurfaceSolver.hpp
 * @brief Performance-portable CVODE surface solver (Phase 1: Kokkos/OpenMP).
 *
 * @details Implements ISurfaceSolver with a Kokkos-backed N_Vector and the
 *          RHS pipeline in KokkosSurfaceKernels.hpp. The CVODE configuration
 *          (BDF + Newton + SPGMR/GMRES + optional Jacobi preconditioner,
 *          tolerances, step bounds) is identical to the serial
 *          CvodeSurfaceSolver, so results match the CPU reference within
 *          solver tolerance — only the vector ops and RHS run through Kokkos.
 *
 *          Phase 1 uses the Kokkos OpenMP execution space: state is
 *          host-resident, so the host<->device deep_copies are no-ops and the
 *          RHS runs as multithreaded CPU loops. Phase 2 flips ExecSpace to
 *          CUDA (and keeps state device-resident) without touching this class
 *          beyond the data-residency copies already isolated in advance().
 *
 *          Lives entirely in the GPU plugin; never linked into the core
 *          engine, so the base library stays Kokkos-free.
 *
 * @ingroup engine_2d_gpu
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_CVODE_KOKKOS_SURFACE_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_GPU_CVODE_KOKKOS_SURFACE_SOLVER_HPP

#include "../solver/ISurfaceSolver.hpp"
#include "KokkosSurfaceKernels.hpp"

#include <nvector/nvector_kokkos.hpp>
#include <sundials/sundials_context.h>
#include <sundials/sundials_linearsolver.h>
#include <sundials/sundials_types.h>

#include <memory>

namespace openswmm::twoD {

struct MeshData;
struct SurfaceStateData;
struct SolverOptions2D;

namespace gpu {

/// SUNDIALS Kokkos vector specialised on the default execution space.
using VecType = ::sundials::kokkos::Vector<ExecSpace>;

#if defined(OPENSWMM_HAVE_HYPRE)
class KokkosAmgPreconditioner;  // complete type only in the .cpp (guarded)
#endif

/// Context handed to the CVODE callbacks via CVodeSetUserData().
struct KokkosSolverContext {
    MeshViews*  mesh        = nullptr;
    StateViews* state       = nullptr;
    double      dry_depth   = 1.0e-3;
    double      limiter_eps = 1.0e-6;
    double      flux_dh_eps = 1.0e-3;   ///< √|Δη| regularization (FLUX_DH_EPS)
#if defined(OPENSWMM_HAVE_HYPRE)
    bool                     use_amg = false;   ///< PRECONDITIONER=AMG selected
    KokkosAmgPreconditioner* amg     = nullptr; ///< BoomerAMG (when use_amg)
#endif
};

class CvodeKokkosSurfaceSolver final : public ISurfaceSolver {
public:
    // Out-of-line (see CvodeSurfaceSolver) so the unique_ptr AMG member's
    // destructor is instantiated in the .cpp where the type is complete.
    CvodeKokkosSurfaceSolver();
    ~CvodeKokkosSurfaceSolver() override;

    CvodeKokkosSurfaceSolver(const CvodeKokkosSurfaceSolver&)            = delete;
    CvodeKokkosSurfaceSolver& operator=(const CvodeKokkosSurfaceSolver&) = delete;

    void   initialize(MeshData& mesh, SurfaceStateData& state,
                      SolverOptions2D& opts) override;
    double advance(double t_current, double t_target) override;
    void   reinitialize(double t0) override;
    void   finalize() override;

    long   last_num_steps() const noexcept override { return last_nsteps_; }
    double last_step_size() const noexcept override { return last_h_; }
    bool   is_initialized() const noexcept override { return cvode_mem_ != nullptr; }

private:
    // SUNDIALS handles.
    SUNContext      sun_ctx_   = nullptr;
    void*           cvode_mem_ = nullptr;
    SUNLinearSolver ls_        = nullptr;
    std::unique_ptr<VecType> y_;        ///< Kokkos N_Vector for the state (volume V).
    std::unique_ptr<VecType> abstol_;   ///< per-cell absolute tolerance (A·abs_tol)

    // Device mirrors.
    MeshViews  mesh_v_;
    StateViews state_v_;

    // Host back-pointers (owned by SurfaceRouter2D; must outlive the solver).
    MeshData*         mesh_host_  = nullptr;
    SurfaceStateData* state_host_ = nullptr;
    SolverOptions2D*  opts_host_  = nullptr;

    KokkosSolverContext ctx_;

#if defined(OPENSWMM_HAVE_HYPRE)
    std::unique_ptr<KokkosAmgPreconditioner> amg_precond_;
#endif

    int  nt_ = 0;
    int  nv_ = 0;
    long last_nsteps_ = 0;
    double last_h_    = 0.0;

    // Host<->device transfer helpers (no-ops in Phase 1 OpenMP host space).
    void uploadMesh();
    void uploadSources();          ///< rainfall / coupling_flux / evap_rate
    void uploadBoundaryStatic();   ///< allocate + upload BC type/slope (once)
    void uploadBoundaryDynamic();  ///< upload resolved BC head/flow (per step)
    void seedVolumeFromHead();     ///< state.head -> y_ as volume V (+ state.volume)
    void downloadState();          ///< y_ (volume) -> state.volume / head / depth (host)

    // CVODE C-ABI callbacks.
    static int rhs_cb(sunrealtype t, N_Vector y, N_Vector ydot, void* user_data);
    static int psetup_cb(sunrealtype t, N_Vector y, N_Vector fy,
                         sunbooleantype jok, sunbooleantype* jcurPtr,
                         sunrealtype gamma, void* user_data);
    static int psolve_cb(sunrealtype t, N_Vector y, N_Vector fy,
                         N_Vector r, N_Vector z,
                         sunrealtype gamma, sunrealtype delta, int lr,
                         void* user_data);
};

} // namespace gpu
} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_GPU_CVODE_KOKKOS_SURFACE_SOLVER_HPP
