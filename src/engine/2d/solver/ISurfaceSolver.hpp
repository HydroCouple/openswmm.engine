/**
 * @file ISurfaceSolver.hpp
 * @brief Backend-neutral interface for the 2D surface-routing time integrator.
 *
 * @details Phase 0 of the portable GPU CVODE strategy
 *          (docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md §6). Extracts the solver
 *          contract that SurfaceRouter2D depends on so the concrete solver
 *          can be chosen at runtime:
 *
 *            ISurfaceSolver
 *             ├── CvodeSurfaceSolver        (serial CPU; today's path, default)
 *             └── CvodeKokkosSurfaceSolver  (GPU plugin; lands Phase 2+)
 *
 *          This header is dependency-free (no SUNDIALS, no Kokkos): it only
 *          forward-declares the 2D data types it passes by reference, so it
 *          compiles regardless of which backend — if any — is available.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_I_SURFACE_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_I_SURFACE_SOLVER_HPP

#include <vector>

namespace openswmm::twoD {

// Forward declarations — passed by reference, no definitions needed here.
struct MeshData;
struct SurfaceStateData;
struct SolverOptions2D;

/**
 * @brief Abstract time integrator for the 2D surface-routing ODE system.
 *
 * The method set mirrors the lifecycle SurfaceRouter2D drives: one-time
 * setup, repeated advance, optional reinitialize after external state edits
 * (hot start), and teardown. The status accessors expose the most recent
 * integrator step count and step size for runtime reporting.
 *
 * Implementations are non-copyable (they own backend resources) and owned
 * by SurfaceRouter2D through a unique_ptr<ISurfaceSolver>; the virtual
 * destructor makes that deletion correct.
 */
class ISurfaceSolver {
public:
    virtual ~ISurfaceSolver() = default;

    /// One-time setup. @p mesh and @p state must outlive the solver.
    virtual void initialize(MeshData& mesh, SurfaceStateData& state,
                            SolverOptions2D& opts) = 0;

    /// Advance the solution from @p t_current to @p t_target (s).
    /// @return the time actually reached (== t_target on success).
    virtual double advance(double t_current, double t_target) = 0;

    /// Reinitialize the integrator at @p t0 after external state edits.
    virtual void reinitialize(double t0) = 0;

    /// Re-time the integrator at @p t0 keeping the SIGNED cell volumes from
    /// state.volume. Used by the failed-window freeze path: reinitialize()
    /// reseeds from the reconstructed head, which clamps at the dry anchor and
    /// silently zeroes negative-volume debt (creating water); this variant
    /// preserves it. Default falls back to reinitialize() for backends that
    /// have not implemented volume-exact resync.
    virtual void resyncFromVolumes(double t0) { reinitialize(t0); }

    /// Release all backend resources.
    virtual void finalize() = 0;

    /// Number of internal integrator steps in the last advance() call.
    virtual long last_num_steps() const noexcept = 0;

    /// Last internal step size used by the integrator.
    virtual double last_step_size() const noexcept = 0;

    /// Per-point ∫Q dt (m³) from the live node-coupling macro-step path. Default
    /// returns empty for backends that do not implement live coupling (so the
    /// caller falls back to the held-flux booking). See CvodeSurfaceSolver.
    virtual const std::vector<double>& last_coupling_exchange() const noexcept {
        static const std::vector<double> kEmpty;
        return kEmpty;
    }

    /// True once initialize() has completed and the solver is ready.
    virtual bool is_initialized() const noexcept = 0;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_I_SURFACE_SOLVER_HPP
