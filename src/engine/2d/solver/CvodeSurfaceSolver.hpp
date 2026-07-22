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
#include "ISurfaceSolver.hpp"

#ifdef OPENSWMM_HAS_2D

#include <vector>
#include <memory>

// Forward declarations for SUNDIALS types (avoid pulling in full headers)
struct SUNContext_;
typedef struct SUNContext_* SUNContext;
typedef struct _generic_N_Vector* N_Vector;
typedef struct _generic_SUNLinearSolver* SUNLinearSolver;

namespace openswmm::twoD {

class CvodeSurfaceSolver;
#if defined(OPENSWMM_HAVE_HYPRE)
class HypreAmgPreconditioner;  // complete type only in the .cpp (guarded)
#endif

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
    bool                amg_active = false;  ///< effective AMG (after hypre fallback)
};

/**
 * @brief CVODE wrapper for the 2D surface routing ODE system.
 */
class CvodeSurfaceSolver : public ISurfaceSolver {
public:
    // Defined out-of-line in the .cpp (where HypreAmgPreconditioner is a
    // complete type) so the unique_ptr member's destructor can be instantiated
    // there rather than at every construction site.
    CvodeSurfaceSolver();
    ~CvodeSurfaceSolver() override;

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
                    SolverOptions2D& opts) override;

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
    double advance(double t_current, double t_target) override;

    /**
     * @brief Reinitialize CVODE with current state vector.
     *
     * Call after externally modifying state.depth (e.g., hot start).
     *
     * @param t0 New initial time.
     */
    void reinitialize(double t0) override;

    /// Re-time at @p t0 keeping SIGNED cell volumes (failed-window freeze).
    void resyncFromVolumes(double t0) override;

    /**
     * @brief Release all SUNDIALS resources.
     */
    void finalize() override;

    /// Get number of internal steps taken in last advance() call.
    long last_num_steps() const noexcept override { return last_nsteps_; }

    /// Get last internal step size used by CVODE.
    double last_step_size() const noexcept override { return last_h_; }

    /// Per-point ∫Q dt (m³, +drain/−spill) accumulated over the last advance(),
    /// one entry per live node-coupling point (state.node_coupling order). Empty
    /// unless the live-coupling macro-step path is active. The caller books these
    /// to the 1D node lateral inflow + the 2D mass-balance ledger.
    const std::vector<double>& last_coupling_exchange() const noexcept override {
        return last_coupling_exchange_;
    }

    /// Cumulative CVODE + SPGMR statistics (read live from the solver memory).
    RunStats run_stats() const noexcept override;

    /// Check if solver is initialized.
    bool is_initialized() const noexcept override { return cvode_mem_ != nullptr; }

private:
    void*           cvode_mem_ = nullptr;  ///< CVODE memory block
    SUNLinearSolver ls_        = nullptr;  ///< SPGMR Krylov linear solver
    N_Vector        y_         = nullptr;  ///< State vector (cell water volume V)
    N_Vector        abstol_    = nullptr;  ///< Per-cell absolute tolerance vector (volume)
    SUNContext      sun_ctx_   = nullptr;  ///< SUNDIALS context

    CvodeSolverContext ctx_;               ///< RHS callback context

    long   last_nsteps_ = 0;
    double last_h_      = 0.0;

    // ── Live node-coupling (macro-step) state augmentation ────────────────────
    // When state.node_coupling is set (COUPLING_INTERVAL > 1), the state vector
    // is augmented to nt + nc_: the extra nc_ entries A_k = ∫Q_k dt accumulate
    // the live orifice exchange per coupling point so the 1D↔2D booking is
    // conservative (CVODE integrates A_k with the same BDF method as V). nc_ == 0
    // (default) ⇒ no augmentation, byte-identical to the legacy held-flux path.
    int    nc_ = 0;                              ///< number of live node-coupling points
    std::vector<double> coupling_accum_start_;   ///< A_k at the start of the current advance
    std::vector<double> last_coupling_exchange_; ///< per-point ∫Q dt over the last advance (m³)

    /// Cached diagonal of the Jacobi preconditioner, sized to n_triangles.
    /// Populated in psetup_fn from the current edge fluxes; consumed in
    /// psolve_fn. Phase 1 stores diag(J) as a heuristic per-cell value
    /// (sum of edge transmissivities, normalised by cell area, negated).
    std::vector<double> precond_diag_;

    /// Scratch for the VFR dη/dV chain-rule factors handed to the AMG
    /// assembly — a member so psetup does not allocate per call.
    std::vector<double> deta_dv_buf_;

#if defined(OPENSWMM_HAVE_HYPRE)
    /// hypre BoomerAMG preconditioner (PRECONDITIONER=AMG). Null unless AMG is
    /// selected. Owned via unique_ptr whose destructor sees the complete type
    /// in the .cpp (where ~CvodeSurfaceSolver and the move ops are defined).
    std::unique_ptr<HypreAmgPreconditioner> amg_precond_;
    /// True when AMG served the most recent psetup. The active-set bypass can
    /// hand mostly-dry systems to the (masked, near-exact) Jacobi diagonal
    /// instead; psolve_fn dispatches on this so it always pairs with the
    /// latest psetup, and a branch flip forces a fresh build of the taker.
    bool amg_used_last_setup_ = false;
#endif

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
