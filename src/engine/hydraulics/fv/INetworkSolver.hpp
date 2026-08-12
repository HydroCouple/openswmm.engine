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
 * @file INetworkSolver.hpp
 * @brief Backend-neutral interface for the explicit FV 1D network integrator.
 *
 * @details Modeled directly on 2d/solver/ISurfaceSolver.hpp so the two solver
 *          families are reviewable side by side:
 *
 *            INetworkSolver
 *             ├── ExplicitFvSolver              (serial/OpenMP CPU; default)
 *             └── ExplicitKokkosNetworkSolver   (GPU/threaded plugin)
 *
 *          Dependency-free (no Kokkos, no SimulationContext): it forward-
 *          declares the data types it passes by reference, so it compiles
 *          regardless of which backend — if any — is available.
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §4
 * @ingroup engine_fv
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_I_NETWORK_SOLVER_HPP
#define OPENSWMM_ENGINE_FV_I_NETWORK_SOLVER_HPP

namespace openswmm::fv {

// Forward declarations — passed by reference, no definitions needed here.
struct NetworkMeshData;
struct NetworkStateData;
struct FvOptions;

/// Per-routing-step boundary forcing pushed into the solver before advance()
/// and results pulled back after. Kept as a flat struct so the plugin ABI can
/// pass it across the C boundary unchanged.
struct FvStepForcing {
    /// Lateral inflow at each regular node (cfs), as assembled by
    /// SWMMEngine::assembleLateralInflows. Indexed by node.
    const double* node_lateral = nullptr;

    /// Prescribed head at boundary-controlled nodes (ft, absolute elevation);
    /// NaN entries mean "not prescribed" and the node is advanced by its own
    /// continuity equation. Indexed by node. Outfalls arrive here.
    const double* node_fixed_head = nullptr;

    /// Non-conduit (pump/orifice/weir/outlet) link flows in cfs, signed
    /// positive from node1 to node2. Indexed by link; conduits are ignored.
    const double* structure_flow = nullptr;

    /// Distributed conduit loss rate per unit length (ft²/s, positive = loss)
    /// from evaporation and seepage. Indexed by conduit row.
    const double* conduit_loss = nullptr;

    /// Optional hook the solver calls at the start of each substep so
    /// head-dependent boundary flows can be re-evaluated against the state it
    /// has actually reached, rather than held at the value they had when the
    /// routing step began (FV_STRUCTURE_COUPLING SUBSTEP).
    ///
    /// The callee is expected to refresh `structure_flow` and
    /// `node_fixed_head` in place. A raw function pointer, not a std::function:
    /// FvStepForcing has to stay trivially copyable so a device backend can
    /// take it by value.
    ///
    /// It does NOT cross the plugin ABI — a device backend leaves it null and
    /// clamps to ROUTING_STEP, since the structure equations live in the
    /// engine, not the kernels.
    void (*refresh)(void* user, double t_elapsed) = nullptr;
    void*  refresh_user = nullptr;

    int n_nodes = 0;
    int n_links = 0;
};

/**
 * @brief Abstract time integrator for the explicit FV 1D network.
 *
 * The lifecycle mirrors what Router drives: one-time setup, repeated advance
 * over a routing step (internally substepped at the CFL limit), optional
 * reinitialize after external state edits (hot start / runtime forcing), and
 * teardown.
 *
 * Implementations are non-copyable (they own backend resources) and owned by
 * Router through a unique_ptr<INetworkSolver>; the virtual destructor makes
 * that deletion correct.
 */
class INetworkSolver {
public:
    virtual ~INetworkSolver() = default;

    /// One-time setup. @p mesh, @p state and @p opts must outlive the solver.
    virtual void initialize(NetworkMeshData& mesh, NetworkStateData& state,
                            const FvOptions& opts) = 0;

    /// Advance from @p t_current to @p t_target (s), substepping at the CFL
    /// limit. @return the time actually reached (== t_target on success).
    virtual double advance(double t_current, double t_target,
                           const FvStepForcing& forcing) = 0;

    /// Reinitialize at @p t0 after external state edits (hot start, API writes).
    virtual void reinitialize(double t0) = 0;

    /// Release all backend resources.
    virtual void finalize() = 0;

    /// Substep count used by the last advance() call.
    virtual long last_num_steps() const noexcept = 0;

    /// Last internal substep size (s).
    virtual double last_step_size() const noexcept = 0;

    /// CFL-limited step the solver would take next, from its most recent
    /// census. Router::getAdaptiveStep publishes this as the routing-step
    /// suggestion.
    virtual double suggested_step() const noexcept = 0;

    /// Cumulative statistics over the whole run — what the "FV Solver
    /// Statistics" report block reads.
    struct RunStats {
        long   nsteps      = 0;    ///< explicit substeps
        long   nflux       = 0;    ///< face flux evaluations
        double last_h      = 0.0;  ///< last substep (s)
        double avg_h       = 0.0;  ///< sim-time / nsteps (s)
        double min_h       = 0.0;  ///< smallest substep taken (s)

        // Work-list compaction telemetry. Negative fractions mean "not
        // populated" (compaction disabled or never sampled).
        double active_frac_min  = -1.0;
        double active_frac_mean = -1.0;
        double active_frac_max  = -1.0;

        long   tier_cells[8] = {0}; ///< cumulative rebuild-sampled cells per LTS tier
        int    n_tiers       = 0;   ///< populated tier count (≤ 8)
    };

    /// Read cumulative statistics. Default: zeros (backend has no counters).
    virtual RunStats run_stats() const noexcept { return {}; }

    /// True once initialize() has completed and the solver is ready.
    virtual bool is_initialized() const noexcept = 0;
};

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_I_NETWORK_SOLVER_HPP
