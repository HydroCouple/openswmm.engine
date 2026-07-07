/**
 * @file ArkodeSurfaceSolver.hpp
 * @brief SUNDIALS ARKODE/ARKStep IMEX integrator for the 2D surface routing ODE.
 *
 * @details Phase 1 of docs/IMEX_LOCAL_INERTIAL_IMPLEMENTATION_PLAN.md. An
 *          alternative ISurfaceSolver to CvodeSurfaceSolver that integrates the
 *          same semi-discrete finite-volume system
 *            dV/dt = F_E(V) + F_I(V)
 *          with an additive Runge–Kutta IMEX method instead of CVODE's fully
 *          implicit BDF. The right-hand side is split (see SurfaceFluxCalculator):
 *
 *            F_E (explicit, non-stiff): rainfall, held coupling, evaporation.
 *            F_I (implicit,  stiff)   : the diffusion flux divergence + the live
 *                                       1D↔2D orifice coupling.
 *
 *          Only F_I enters the implicit-stage Newton–Krylov solve, so the
 *          implicit Jacobian is the diffusion stencil — the BoomerAMG / Jacobi
 *          preconditioner (reused verbatim from the CVODE path; SUNDIALS 7's
 *          ARKLsPrecSetupFn/SolveFn share CVODE's callback signature) inverts
 *          M = I − γ·J over exactly that operator.
 *
 *          The state augmentation, tolerance scheme, and live-coupling ∫Q dt
 *          accumulators mirror CvodeSurfaceSolver so the router drives this
 *          backend through the unchanged ISurfaceSolver contract.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_ARKODE_SURFACE_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_ARKODE_SURFACE_SOLVER_HPP

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "ISurfaceSolver.hpp"
#include "InertialEdges.hpp"

#ifdef OPENSWMM_HAS_2D

#include <vector>
#include <memory>

// Forward declarations for SUNDIALS types (avoid pulling in full headers)
struct SUNContext_;
typedef struct SUNContext_* SUNContext;
typedef struct _generic_N_Vector* N_Vector;
typedef struct _generic_SUNLinearSolver* SUNLinearSolver;

namespace openswmm::twoD {

class ArkodeSurfaceSolver;
#if defined(OPENSWMM_HAVE_HYPRE)
class HypreAmgPreconditioner;  // complete type only in the .cpp (guarded)
#endif

/**
 * @brief Context passed to the ARKODE RHS / preconditioner callbacks.
 *
 * Mirrors CvodeSolverContext: everything the fe/fi and psetup/psolve callbacks
 * need to evaluate the split RHS and apply the preconditioner. Passed as
 * user_data via ARKodeSetUserData().
 */
struct ArkodeSolverContext {
    MeshData*            mesh   = nullptr;
    SurfaceStateData*    state  = nullptr;
    SolverOptions2D*     opts   = nullptr;
    ArkodeSurfaceSolver* solver = nullptr;
    bool                 amg_active = false;  ///< effective AMG (after hypre fallback)
};

/**
 * @brief ARKODE/ARKStep IMEX wrapper for the 2D surface routing ODE system.
 */
class ArkodeSurfaceSolver : public ISurfaceSolver {
public:
    ArkodeSurfaceSolver();
    ~ArkodeSurfaceSolver() override;

    ArkodeSurfaceSolver(const ArkodeSurfaceSolver&)            = delete;
    ArkodeSurfaceSolver& operator=(const ArkodeSurfaceSolver&) = delete;

    ArkodeSurfaceSolver(ArkodeSurfaceSolver&& o) noexcept;
    ArkodeSurfaceSolver& operator=(ArkodeSurfaceSolver&& o) noexcept;

    void initialize(MeshData& mesh, SurfaceStateData& state,
                    SolverOptions2D& opts) override;

    double advance(double t_current, double t_target) override;

    void reinitialize(double t0) override;

    void finalize() override;

    long last_num_steps() const noexcept override { return last_nsteps_; }

    double last_step_size() const noexcept override { return last_h_; }

    const std::vector<double>& last_coupling_exchange() const noexcept override {
        return last_coupling_exchange_;
    }

    bool is_initialized() const noexcept override { return arkode_mem_ != nullptr; }

private:
    void*           arkode_mem_ = nullptr;  ///< ARKStep memory block
    SUNLinearSolver ls_         = nullptr;  ///< SPGMR Krylov linear solver
    N_Vector        y_          = nullptr;  ///< State vector (cell water volume V)
    N_Vector        abstol_     = nullptr;  ///< Per-cell absolute tolerance (volume)
    SUNContext      sun_ctx_    = nullptr;  ///< SUNDIALS context

    ArkodeSolverContext ctx_;               ///< RHS / preconditioner callback context

    long   last_nsteps_ = 0;
    double last_h_      = 0.0;

    // ── Live node-coupling (macro-step) state augmentation ────────────────────
    // Identical scheme to CvodeSurfaceSolver: when state.node_coupling is set the
    // state vector grows to nt + nc_; the extra nc_ entries integrate ∫Q_k dt per
    // coupling point (the conservative 1D↔2D booking). nc_ == 0 ⇒ no augmentation.
    int    nc_ = 0;
    std::vector<double> coupling_accum_start_;
    std::vector<double> last_coupling_exchange_;

    // ── Local-inertial momentum (MOMENTUM=inertial) ───────────────────────────
    // Effective momentum closure, resolved in initialize() (env overrides opts).
    // When INERTIAL, the state grows to nt + ne_ + nc_: the extra ne_ entries are
    // the per-edge discharge q (LISFLOOD-FP), integrated implicitly with gravity
    // + friction. edges_ holds the unique-edge layout + per-cell incidence.
    MomentumType        momentum_ = MomentumType::DW;
    int                 ne_       = 0;
    InertialEdges       edges_;

    // Inertial IMEX split. When true (default) the gravity-wave transport
    // (continuity + surface-gradient) is EXPLICIT and only the stiff per-edge
    // friction is implicit — the LISFLOOD-FP semi-implicit scheme. The implicit
    // operator is then diagonal (exact preconditioner, no global solve), so it
    // scales O(n)/step to 1M cells. When false, gravity is implicit too (the
    // plan's Schur-coupled scheme) — kept for comparison; needs AMG to scale.
    // env OPENSWMM_2D_GRAVITY_IMPLICIT selects the false (implicit-gravity) path.
    bool                gravity_explicit_ = true;

    // Block-Jacobi preconditioner scratch for the inertial [V,q] system:
    //   prec_dV_[i] = diagonal of the Schur η-operator at cell i (1 + Σ gravity)
    //   prec_wq_[e] = 1/(1 + γ·R_e), the exact friction-damped q-block diagonal.
    // Rebuilt in psetup_inertial_fn; applied in psolve_inertial_fn. No global
    // solve / no AMG ⇒ scales to 1M cells.
    std::vector<double> prec_dV_;
    std::vector<double> prec_wq_;

    /// Cached diagonal of the Jacobi preconditioner, sized to n_triangles.
    std::vector<double> precond_diag_;

#if defined(OPENSWMM_HAVE_HYPRE)
    std::unique_ptr<HypreAmgPreconditioner> amg_precond_;
#endif

    // ------------------------------------------------------------------
    // SUNDIALS callbacks (ARKRhsFn / ARKLsPrecSetupFn / ARKLsPrecSolveFn).
    // The preconditioner signatures match CVODE's verbatim in SUNDIALS 7.
    // ------------------------------------------------------------------

    /// Explicit RHS F_E (DW): source/sink forcing (non-stiff).
    static int fe_fn(double t, N_Vector y, N_Vector ydot, void* user_data);

    /// Implicit RHS F_I (DW): diffusion flux divergence + live orifice coupling.
    static int fi_fn(double t, N_Vector y, N_Vector ydot, void* user_data);

    /// Explicit RHS F_E (local-inertial): cell sources only; q + accumulator
    /// rows carry no explicit contribution.
    static int fe_inertial_fn(double t, N_Vector y, N_Vector ydot, void* user_data);

    /// Implicit RHS F_I (local-inertial): continuity divergence (cells) +
    /// per-edge momentum ODE (gravity + friction).
    static int fi_inertial_fn(double t, N_Vector y, N_Vector ydot, void* user_data);

    /// Block-Jacobi preconditioner setup for the inertial [V,q] system: build
    /// the Schur η-diagonal (prec_dV_) and the friction-damped q-diagonal
    /// (prec_wq_) from the current state and γ.
    static int psetup_inertial_fn(double t, N_Vector y, N_Vector fy,
                                   int jok, int* jcurPtr, double gamma,
                                   void* user_data);

    /// Block-Jacobi preconditioner apply for the inertial [V,q] system.
    static int psolve_inertial_fn(double t, N_Vector y, N_Vector fy,
                                   N_Vector r, N_Vector z,
                                   double gamma, double delta, int lr,
                                   void* user_data);

    /// Preconditioner setup over the implicit (diffusion) Jacobian.
    static int psetup_fn(double t, N_Vector y, N_Vector fy,
                          int jok, int* jcurPtr, double gamma,
                          void* user_data);

    /// Preconditioner apply: solve (I − γD) z = r (Jacobi) or one AMG V-cycle.
    static int psolve_fn(double t, N_Vector y, N_Vector fy,
                          N_Vector r, N_Vector z,
                          double gamma, double delta, int lr,
                          void* user_data);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D

#endif // OPENSWMM_ENGINE_2D_ARKODE_SURFACE_SOLVER_HPP
