// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      ISurfaceSolver.hpp
 * @brief     Backend-neutral interface for the 2D surface-routing time integrator.
 *
 * @details Extracts the solver contract that SurfaceRouter2D depends on so the
 *          concrete solver can be chosen at runtime:
 *
 *            ISurfaceSolver
 *             ├── ExplicitInertialSolver      (serial CPU; default)
 *             └── ExplicitKokkosSurfaceSolver (GPU/threaded plugin)
 *
 *          This header is dependency-free (no Kokkos): it only
 *          forward-declares the 2D data types it passes by reference, so it
 *          compiles regardless of which backend — if any — is available.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
 * The method set mirrors the lifecycle SurfaceRouter2D drives: one-time setup, repeated advance,
 * optional reinitialize after external state edits (hot start), and teardown. The status
 * accessors expose the most recent integrator step count and step size for runtime reporting.
 *
 * Implementations are non-copyable (they own backend resources) and owned by SurfaceRouter2D
 * through a unique_ptr<ISurfaceSolver>; the virtual destructor makes that deletion correct.
 */
class ISurfaceSolver {
public:
    /**
     * @brief Destroy a backend solver through its polymorphic interface.
     * @note Caller first stops concurrent operations and keeps all retained mesh/state/options
     * storage alive until resources are released. Plugin objects require plugin-owned
     * destruction on their controlling thread.
     */
    virtual ~ISurfaceSolver() = default;

    /**
     * @brief One-time setup. @p mesh and @p state must outlive the solver.
     *
     * @param[in,out] mesh Built mesh geometry/topology in SI units, retained by implementations;
     * must outlive the initialized solver.
     * @param[in,out] state Preallocated hydrodynamic/transport state, mutated during setup and
     * stepping; must outlive the solver.
     * @param[in,out] opts Validated solver configuration, retained by implementations; keep
     * alive and edit only through supported between-step paths.
     * @pre Topology, boundary/coupling wiring and derived storage are prepared by
     * SurfaceRouter2D. Serialize setup/use/finalize on this solver.
     * @note May allocate or throw on unsupported configuration/setup failure; the interface
     * provides no transactional rollback guarantee. Caller owns all referenced model storage.
     */
    virtual void initialize(MeshData& mesh, SurfaceStateData& state, SolverOptions2D& opts) = 0;

    /**
     * @brief Advance the solution from @p t_current to @p t_target (s).
     * @return the time actually reached (== t_target on success).
     *
     * @param[in] t_current Finite elapsed simulation start time, seconds, matching initialized
     * solver time.
     * @param[in] t_target Finite desired elapsed endpoint, seconds, at least t_current.
     * @post Mutates accepted hydraulics/transport, accumulators and step telemetry; partial
     * progress/failure handling is backend-specific.
     * @note A returned endpoint alone does not prove source completion; query
     * completedSourceTime(). Call synchronously between external edits; references passed at
     * initialize remain valid. Backend exceptions may propagate.
     */
    virtual double advance(double t_current, double t_target) = 0;

    /**
     * @brief Horizon through which ALL cell sources have actually landed, including lazy
     * inactive cells and LTS accumulators. Unsupported backends return -1; an outer routing
     * endpoint alone is not source-completion evidence.
     *
     * @return Completed source horizon in elapsed simulation seconds, or -1 for
     * unsupported/unavailable evidence. Default returns -1 without mutation/allocation; concrete
     * backends may cache it. Serialize reads with stepping.
     */
    virtual double completedSourceTime() const noexcept { return -1.0; }

    /**
     * @brief Reinitialize the integrator at @p t0 after external state edits.
     *
     * @param[in] t0 Finite elapsed time in seconds for reseeding after a hotstart/external state
     * edit.
     * @note Requires initialized retained state and serialized access. Backend implementations
     * may rebuild momentum/tier/source caches from edited heads and reset local telemetry;
     * errors may propagate. Use resyncFromVolumes() when signed-volume debt must be preserved.
     */
    virtual void reinitialize(double t0) = 0;

    /**
     * @brief Re-time the integrator at @p t0 keeping the SIGNED cell volumes from state.volume.
     * Used by the failed-window freeze path: reinitialize() reseeds from the reconstructed head,
     * which clamps at the dry anchor and silently zeroes negative-volume debt (creating water);
     * this variant preserves it. Default falls back to reinitialize() for backends that have not
     * implemented volume-exact resync.
     *
     * @param[in] t0 Finite elapsed simulation time, seconds.
     * @note Requires initialized retained model arrays and exclusive solver access. The default
     * implementation calls reinitialize(t0), so only overriding backends guarantee exact
     * signed-volume preservation. Backend errors may propagate; no ownership changes.
     */
    virtual void resyncFromVolumes(double t0) { reinitialize(t0); }

    /**
     * @brief Release all backend resources.
     *
     * @post Releases backend-owned buffers and invalidates initialization state; caller retains
     * mesh/state/options ownership. Concrete lifecycle/idempotence is backend-specific.
     * @note Serialize with all calls and respect a plugin's controlling-thread contract. Process
     * Kokkos runtime lifetime is separate from per-solver finalization.
     */
    virtual void finalize() = 0;

    /**
     * @brief Number of internal integrator steps in the last advance() call.
     *
     * @return Number of internal substeps from the last advance, as a value copy.
     * @note Read-only nonthrowing status query. Serialize reads with mutations unless the
     * concrete adapter explicitly provides safe snapshots.
     */
    virtual long last_num_steps() const noexcept = 0;

    /**
     * @brief Last internal step size used by the integrator.
     *
     * @return Last internal accepted substep size in seconds; does not advance time.
     * @note Read-only nonthrowing status query. Serialize reads with mutations unless the
     * concrete adapter explicitly provides safe snapshots.
     */
    virtual double last_step_size() const noexcept = 0;

    /**
     * @brief Per-point ∫Q dt (m³) from the live node-coupling macro-step path. Default returns
     * empty for backends that do not implement live coupling (so the caller falls back to the
     * held-flux booking). See ExplicitInertialSolver.
     *
     * @return Borrowed per-coupling-point signed integrated volumes in m3, positive 2D-to-1D.
     * Default returns an immutable empty vector. Reference is owned by the solver (or static
     * default), must not be freed, and values may change on the next
     * advance/reinitialize/finalize.
     * @note Read only between solver mutations; an empty vector asks the caller to use held-flux
     * accounting instead of live coupling receipts.
     */
    virtual const std::vector<double>& last_coupling_exchange() const noexcept {
        static const std::vector<double> kEmpty;
        return kEmpty;
    }

    /// Cumulative marcher statistics over the whole run — the throughput
    /// numbers the "2D Solver Statistics" report block reads.
    struct RunStats {
        long nsteps = 0;     ///< internal (marcher) substeps
        long nrhs = 0;       ///< face-kernel evaluations
        double last_h = 0.0; ///< last accepted internal step (s)
        double avg_h = 0.0;  ///< sim-time / nsteps (s), filled by the caller

        // Marcher telemetry. Guarded: n_tiers == 0 and negative fractions
        // mean "not populated".
        double active_frac_min = -1.0;  ///< min active-cell fraction (rebuild samples)
        double active_frac_mean = -1.0; ///< mean active-cell fraction
        double active_frac_max = -1.0;  ///< max active-cell fraction
        long tier_cells[8] = {0};       ///< cumulative rebuild-sampled cells per LTS tier
        int n_tiers = 0;                ///< populated tier count (≤ 8)
    };

    /**
     * @brief Read cumulative statistics. Default: zeros (backend has no counters).
     *
     * @return Value-owned cumulative RunStats; the default implementation returns
     * zero/unpopulated counters.
     * @note Read-only nonthrowing status query. Serialize reads with mutations unless the
     * concrete adapter explicitly provides safe snapshots.
     */
    virtual RunStats run_stats() const noexcept { return {}; }

    /**
     * @brief True once initialize() has completed and the solver is ready.
     *
     * @return True only when backend initialization completed and resources are ready.
     * @note Read-only nonthrowing status query. Serialize reads with mutations unless the
     * concrete adapter explicitly provides safe snapshots.
     */
    virtual bool is_initialized() const noexcept = 0;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_I_SURFACE_SOLVER_HPP
