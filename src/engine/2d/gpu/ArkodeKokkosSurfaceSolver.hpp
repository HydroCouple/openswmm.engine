/**
 * @file ArkodeKokkosSurfaceSolver.hpp
 * @brief Performance-portable local-inertial surface solver (Kokkos + ARKStep).
 *
 * @details The Kokkos counterpart of the serial ArkodeSurfaceSolver's
 *          explicit-gravity (LISFLOOD-FP) local-inertial scheme. State is the
 *          augmented [V(cells), q(edges)] vector in a Kokkos N_Vector, so BOTH
 *          the RHS/preconditioner kernels (KokkosInertialKernels.hpp) AND
 *          ARKStep's vector ops run device-resident — the serial-N_Vector
 *          bottleneck that limited the core OpenMP path to ~1.2× is removed.
 *
 *          Only the scalable split is implemented: gravity transport explicit,
 *          per-edge friction implicit (diagonal → exact preconditioner, no
 *          global solve). The implicit-gravity comparison path stays serial-only.
 *
 *          Lives entirely in the GPU plugin; never linked into the core engine.
 *
 * @ingroup engine_2d_gpu
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_ARKODE_KOKKOS_SURFACE_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_GPU_ARKODE_KOKKOS_SURFACE_SOLVER_HPP

#include "../solver/ISurfaceSolver.hpp"
#include "KokkosSurfaceKernels.hpp"
#include "KokkosInertialKernels.hpp"

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

using VecType = ::sundials::kokkos::Vector<ExecSpace>;

/// Context handed to the ARKStep callbacks via ARKodeSetUserData().
struct ArkodeKokkosContext {
    MeshViews*         mesh   = nullptr;
    StateViews*        state  = nullptr;
    InertialEdgeViews* edges  = nullptr;
    DView              prec_wq;          ///< [ne] friction-damped q-diagonal
    double             dry_depth = 1.0e-3;
    double             vfr_eps   = 1.0e-2; ///< VFR wet-fraction floor (VFR_MIN_WET_FRAC)
    int                nt = 0;
    int                ne = 0;
};

class ArkodeKokkosSurfaceSolver final : public ISurfaceSolver {
public:
    ArkodeKokkosSurfaceSolver();
    ~ArkodeKokkosSurfaceSolver() override;

    ArkodeKokkosSurfaceSolver(const ArkodeKokkosSurfaceSolver&)            = delete;
    ArkodeKokkosSurfaceSolver& operator=(const ArkodeKokkosSurfaceSolver&) = delete;

    void   initialize(MeshData& mesh, SurfaceStateData& state,
                      SolverOptions2D& opts) override;
    double advance(double t_current, double t_target) override;
    void   reinitialize(double t0) override;
    void   finalize() override;

    long   last_num_steps() const noexcept override { return last_nsteps_; }
    double last_step_size() const noexcept override { return last_h_; }
    bool   is_initialized() const noexcept override { return arkode_mem_ != nullptr; }

private:
    SUNContext      sun_ctx_    = nullptr;
    void*           arkode_mem_ = nullptr;
    SUNLinearSolver ls_         = nullptr;
    std::unique_ptr<VecType> y_;        ///< Kokkos N_Vector [V(nt), q(ne)]
    std::unique_ptr<VecType> abstol_;

    MeshViews         mesh_v_;
    StateViews        state_v_;
    InertialEdgeViews edges_v_;
    DView             prec_wq_;

    MeshData*         mesh_host_  = nullptr;
    SurfaceStateData* state_host_ = nullptr;
    SolverOptions2D*  opts_host_  = nullptr;

    ArkodeKokkosContext ctx_;

    int  nt_ = 0;
    int  ne_ = 0;
    int  nv_ = 0;
    long last_nsteps_ = 0;
    double last_h_    = 0.0;

    void uploadMesh();
    void uploadEdges();
    void uploadSources();
    void seedState();        ///< V from state.head, q = 0
    void downloadState();    ///< V → volume/head/depth, q → edge_flux (host)

    static int fe_cb(sunrealtype t, N_Vector y, N_Vector ydot, void* user_data);
    static int fi_cb(sunrealtype t, N_Vector y, N_Vector ydot, void* user_data);
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

#endif // OPENSWMM_ENGINE_2D_GPU_ARKODE_KOKKOS_SURFACE_SOLVER_HPP
