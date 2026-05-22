/**
 * @file CvodeSurfaceSolver.hpp
 * @brief CVODE (SUNDIALS) wrapper for the 2D surface routing ODE system.
 *
 * @details Wraps the SUNDIALS CVODE solver for time integration of the
 *          semi-discrete finite volume surface flow equations. Uses BDF
 *          for time discretisation, CVODE's default Newton corrector
 *          with inexact tolerance, and SPGMR (Krylov GMRES) for the
 *          inner linear solves. A Jacobi preconditioner is wired by
 *          default (per-cell diagonal approximation rebuilt each
 *          Jacobian refresh); see SolverOptions2D for the menu of other
 *          linear solver / preconditioner tiers reserved for future use.
 *
 *          The ODE system is:
 *            dy/dt = f(t, y)
 *          where y[i] = H_i (water-surface elevation at triangle i) and
 *          f computes the RHS from the finite volume formulation.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §4.2
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_CVODE_SURFACE_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_CVODE_SURFACE_SOLVER_HPP

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"

#ifdef OPENSWMM_HAS_2D

#include <vector>

// Forward declarations for SUNDIALS types (avoid pulling in full headers)
struct SUNContext_;
typedef struct SUNContext_* SUNContext;
typedef struct _generic_N_Vector* N_Vector;
typedef struct _generic_SUNLinearSolver* SUNLinearSolver;

namespace openswmm::twoD {

class CvodeSurfaceSolver;

/**
 * @brief Context passed to the CVODE RHS / preconditioner callbacks.
 *
 * Contains all data needed to evaluate f(t, y) and to set up / apply the
 * Jacobi preconditioner. Passed as user_data to CVodeSetUserData().
 *
 * The `solver` back-pointer lets the preconditioner callbacks reach the
 * owning solver's per-instance scratch storage (e.g. the cached diagonal
 * used by Jacobi). It is set in CvodeSurfaceSolver::initialize() and
 * cleared in finalize().
 */
struct CvodeSolverContext {
    MeshData*           mesh   = nullptr;
    SurfaceStateData*   state  = nullptr;
    SolverOptions2D*    opts   = nullptr;
    CvodeSurfaceSolver* solver = nullptr;
};

/**
 * @brief CVODE wrapper for the 2D surface routing ODE system.
 */
class CvodeSurfaceSolver {
public:
    CvodeSurfaceSolver() = default;
    ~CvodeSurfaceSolver();

    // Non-copyable
    CvodeSurfaceSolver(const CvodeSurfaceSolver&) = delete;
    CvodeSurfaceSolver& operator=(const CvodeSurfaceSolver&) = delete;

    // Movable
    CvodeSurfaceSolver(CvodeSurfaceSolver&& o) noexcept;
    CvodeSurfaceSolver& operator=(CvodeSurfaceSolver&& o) noexcept;

    /**
     * @brief Initialize the CVODE solver.
     *
     * Creates SUNDIALS context, N_Vector, linear solver, and configures
     * CVODE with BDF method and the chosen iterative linear solver.
     *
     * @param mesh  Mesh data (must outlive the solver).
     * @param state Surface state (must outlive the solver).
     * @param opts  Solver options.
     */
    void initialize(MeshData& mesh, SurfaceStateData& state,
                    SolverOptions2D& opts);

    /**
     * @brief Advance the solution from t_current to t_target.
     *
     * CVODE internally sub-steps as needed to meet error tolerances.
     * The coupling flux and rainfall in `state` are held constant during
     * CVODE's internal steps (operator-splitting).
     *
     * @param t_current Current simulation time (s).
     * @param t_target  Target time to advance to (s).
     * @return Actual time reached (should equal t_target on success).
     */
    double advance(double t_current, double t_target);

    /**
     * @brief Reinitialize CVODE with current state vector.
     *
     * Call after externally modifying state.depth (e.g., hot start).
     *
     * @param t0 New initial time.
     */
    void reinitialize(double t0);

    /**
     * @brief Release all SUNDIALS resources.
     */
    void finalize();

    /// Get number of internal steps taken in last advance() call.
    long last_num_steps() const noexcept { return last_nsteps_; }

    /// Get last internal step size used by CVODE.
    double last_step_size() const noexcept { return last_h_; }

    /// Check if solver is initialized.
    bool is_initialized() const noexcept { return cvode_mem_ != nullptr; }

private:
    void*           cvode_mem_ = nullptr;  ///< CVODE memory block
    SUNLinearSolver ls_        = nullptr;  ///< SPGMR Krylov linear solver
    N_Vector        y_         = nullptr;  ///< State vector (water-surface elevation H)
    SUNContext      sun_ctx_   = nullptr;  ///< SUNDIALS context

    CvodeSolverContext ctx_;               ///< RHS callback context

    long   last_nsteps_ = 0;
    double last_h_      = 0.0;

    /// Cached diagonal of the Jacobi preconditioner, sized to n_triangles.
    /// Populated in psetup_fn from the current edge fluxes; consumed in
    /// psolve_fn. Phase 1 stores diag(J) as a heuristic per-cell value
    /// (sum of edge transmissivities, normalised by cell area, negated).
    std::vector<double> precond_diag_;

    // ------------------------------------------------------------------
    // SUNDIALS callbacks. All three have C linkage requirements imposed
    // by SUNDIALS' function-pointer typedefs; we expose them as static
    // member functions to keep them inside the class scope.
    // ------------------------------------------------------------------

    /**
     * @brief RHS function: f(t, y, ydot).
     *
     * Registered as CVRhsFn callback. Computes the full semi-discrete
     * finite volume RHS from the current state.
     */
    static int rhs_fn(double t, N_Vector y, N_Vector ydot, void* user_data);

    /**
     * @brief Preconditioner setup: cache diag(J) for Jacobi.
     *
     * Registered as CVLsPrecSetupFn. Called by CVODE when the linear
     * system's Jacobian needs refreshing. Reads the current edge fluxes
     * (held in state.edge_flux from the most recent rhs_fn call) and
     * builds a per-cell diagonal approximation:
     *   D[i] ≈ -(Σ_e |F_e| / max(|Δh_e|, ε)) / A_i
     * This is the negative sum of edge transmissivities per unit area —
     * the dominant self-derivative of the diffusive-wave RHS.
     */
    static int psetup_fn(double t, N_Vector y, N_Vector fy,
                          int jok, int* jcurPtr, double gamma,
                          void* user_data);

    /**
     * @brief Preconditioner apply: solve (I − γD) z = r element-wise.
     *
     * Registered as CVLsPrecSolveFn. Applied by GMRES at each Krylov
     * iteration. Uses the diagonal cached by psetup_fn.
     */
    static int psolve_fn(double t, N_Vector y, N_Vector fy,
                          N_Vector r, N_Vector z,
                          double gamma, double delta, int lr,
                          void* user_data);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D

#endif // OPENSWMM_ENGINE_2D_CVODE_SURFACE_SOLVER_HPP
