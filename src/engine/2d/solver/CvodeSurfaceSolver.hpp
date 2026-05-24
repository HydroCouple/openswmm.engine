/**
 * @file CvodeSurfaceSolver.hpp
 * @brief CVODE (SUNDIALS) wrapper for the 2D surface routing ODE system.
 *
 * @details Wraps the SUNDIALS CVODE solver for time integration of the
 *          semi-discrete finite volume surface flow equations. Uses BDF
 *          method with iterative linear solver (GMRES/BiCGStab/TFQMR).
 *
 *          The ODE system is:
 *            dy/dt = f(t, y)
 *          where y[i] = ψ_o[i] (depth at triangle i) and f computes the
 *          RHS from the finite volume formulation.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §4.2
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_CVODE_SURFACE_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_CVODE_SURFACE_SOLVER_HPP

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "SpectralPrecond2D.hpp"
#include "../uncertainty/SpectralROM.hpp"
#include "../coupling/NodeCoupling.hpp"
#include <memory>

namespace openswmm::uncertainty { struct SpectralROM1D; }

#ifdef OPENSWMM_HAS_2D

// Forward declarations for SUNDIALS types (avoid pulling in full headers)
struct SUNContext_;
typedef struct SUNContext_* SUNContext;
typedef struct _generic_N_Vector* N_Vector;
typedef struct _generic_SUNLinearSolver* SUNLinearSolver;

namespace openswmm::twoD {

/**
 * @brief Context passed to the CVODE RHS and preconditioner callbacks.
 *
 * Contains all data needed to evaluate f(t, y) and to apply the optional
 * spectral preconditioner. Passed as user_data to CVodeSetUserData().
 */
struct CvodeSolverContext {
    MeshData*          mesh   = nullptr;
    SurfaceStateData*  state  = nullptr;
    SolverOptions2D*   opts   = nullptr;
    SpectralPrecond2D* precond = nullptr;  ///< null if no spectral precond
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

    /// Get cumulative linear solver iterations since initialization.
    long get_num_lin_iters() const noexcept;

    /// Suppress CVODE "t + h = t" near-stagnation warnings (useful in benchmarks).
    void suppress_warnings() noexcept;

    /// Check if solver is initialized.
    bool is_initialized() const noexcept { return cvode_mem_ != nullptr; }

    /// Last effective K_eff used by the ROM (auto-computed or user override).
    /// Zero until the first seedROM() call.
    double effectiveKEff() const noexcept { return effective_keff_; }

    /**
     * @brief Apply coupling exchange flux to the ROM ensemble, if active.
     *
     * Delegates to SpectralROM::applyCouplingFlux(). No-op if the ROM sidecar
     * is not active or not yet seeded.  Must be called after
     * computeCouplingExchange() and before advance().
     *
     * @param cps        Coupling points (from buildCouplingPoints()).
     * @param node_heads Absolute SWMM node heads (length = n_nodes).
     * @param mesh       Mesh data (tri_area, tri_cz).
     * @param dt         Routing timestep (s).
     */
    void applyCouplingFluxToROM(const std::vector<CouplingPoint>& cps,
                                 const double* node_heads,
                                 const MeshData& mesh,
                                 double dt);

    /**
     * @brief Register a 1D network ROM for per-member coupling head reconstruction.
     *
     * When set, applyCouplingFluxToROM() passes this pointer to
     * SpectralROM::applyCouplingFlux() so each 2D ensemble member sees its
     * own reconstructed 1D head at coupling nodes (Phase 5A).
     *
     * The caller retains ownership; this solver stores only a non-owning pointer.
     * Pass nullptr to revert to the shared deterministic head path.
     *
     * @param rom1d  Pointer to a ready SpectralROM1D with full_to_active populated.
     */
    void setROM1D(const openswmm::uncertainty::SpectralROM1D* rom1d) noexcept {
        rom1d_ = rom1d;
    }

    /// Access the ROM sidecar (null if enable_rom was false).
    const SpectralROM* rom() const noexcept { return rom_.get(); }

private:
    void* cvode_mem_          = nullptr;  ///< CVODE memory block
    SUNLinearSolver ls_       = nullptr;  ///< Iterative linear solver
    N_Vector y_               = nullptr;  ///< State vector (wraps state.depth)
    SUNContext sun_ctx_        = nullptr;  ///< SUNDIALS context

    CvodeSolverContext ctx_;               ///< RHS / precond callback context
    std::unique_ptr<SpectralPrecond2D> precond_;  ///< null unless SPECTRAL precond

    // Spectral ROM sidecar (null unless opts.enable_rom)
    std::unique_ptr<SpectralPrecond2D> rom_basis_;  ///< standalone basis if no precond
    std::unique_ptr<SpectralROM>       rom_;
    bool rom_seeded_ = false;

    // Non-owning pointer to the 1D ROM; null unless setROM1D() was called.
    const openswmm::uncertainty::SpectralROM1D* rom1d_ = nullptr;

    /// Seed the ROM on first use (and on wet-domain refresh): optionally rebuild
    /// the eigenbasis with depth-weighted Laplacian (Option B), then project
    /// the current state.  Records wet_count_at_seed_ from the current depth.
    void seedROM();

    /// Trigger a reseed if the wet domain has changed by more than
    /// opts.rom_wet_reseed_fraction * n_tri since the last seed AND at least
    /// opts.rom_wet_reseed_min_interval simulation seconds have elapsed.
    void maybeReseedROM(double t_reached);

    long   last_nsteps_ = 0;
    double last_h_      = 0.0;

    int    wet_count_at_seed_ = 0;    ///< Wet-cell count at last seedROM() call.
    double last_reseed_t_     = -1.0; ///< Simulation time of last seedROM() call.
    double effective_keff_    = 0.0;  ///< K_eff used by ROM: auto-computed or user override.

    /// Mean depth over wet cells (h > opts.dry_depth). Returns 0 if all dry.
    double computeMeanWetDepth() const noexcept;
    /// Arithmetic mean of mesh.mannings_n.
    double computeMeanManningsN() const noexcept;
    /// Mean of |Δz/d| across internal edges. Returns 0 if no internal edges.
    double computeMeanBedSlope() const noexcept;
    /// Compute effective K_eff: user override if opts.rom_k_eff > 0,
    /// else h_mean^(5/3) / (2 * n_mean * sqrt(S_mean)) with flat-domain floor.
    double computeAutoKEff() const noexcept;

    /**
     * @brief RHS function: f(t, y, ydot).
     *
     * Registered as CVRhsFn callback. Computes the full semi-discrete
     * finite volume RHS from the current state.
     */
    static int rhs_fn(double t, N_Vector y, N_Vector ydot, void* user_data);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D

#endif // OPENSWMM_ENGINE_2D_CVODE_SURFACE_SOLVER_HPP
