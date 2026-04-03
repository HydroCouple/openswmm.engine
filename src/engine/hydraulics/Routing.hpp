/**
 * @file Routing.hpp
 * @brief Top-level routing dispatcher — KW / DW / Steady-state.
 *
 * @details Orchestrates the routing step:
 *   1. Save old hydraulic states
 *   2. Initialize node flows (lateral inflows)
 *   3. Dispatch to KinematicWave or DynamicWave solver
 *   4. Update node/link final states
 *   5. Update mass balance accumulators
 *
 * @note Legacy reference: src/legacy/engine/routing.c, flowrout.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ROUTING_HPP
#define OPENSWMM_ROUTING_HPP

#include "XSectBatch.hpp"
#include "KinematicWave.hpp"
#include "DynamicWave.hpp"
#include "Divider.hpp"

namespace openswmm {

struct SimulationContext;

// ============================================================================
// Routing model enum
// ============================================================================

enum class RouteModel : int {
    STEADY    = 0,   ///< Steady-state (pass-through)
    KINWAVE   = 1,   ///< Kinematic wave
    DYNWAVE   = 2    ///< Dynamic wave (St. Venant)
};

// ============================================================================
// Routing orchestrator
// ============================================================================

/**
 * @brief Top-level routing orchestrator.
 *
 * @details Owns the XSectGroups (shape-grouped batch xsect), KWSolver, and
 *          DWSolver. Initialised once after the model is built; then
 *          `step()` is called each routing timestep.
 */
class Router {
public:
    /**
     * @brief Initialise the router for the given model.
     *
     * @details Builds XSectGroups from the link cross-sections, initialises
     *          the KW and/or DW solver working arrays.
     *
     * @param ctx    Simulation context (must have links/nodes populated).
     * @param model  Routing model selection.
     */
    void init(SimulationContext& ctx, RouteModel model);

    /**
     * @brief Execute one routing timestep.
     *
     * @param ctx  Simulation context (modified in place).
     * @param dt   Routing timestep (seconds).
     * @returns Number of solver iterations used.
     */
    int step(SimulationContext& ctx, double dt,
             dynwave::DWSolver::NonConduitFlowFunc non_conduit_fn = nullptr);

    /**
     * @brief Compute adaptive timestep (DW only).
     *
     * @param ctx          Simulation context.
     * @param fixed_step   User-specified routing step.
     * @param courant      Courant factor target (0 = use fixed).
     * @returns Adaptive timestep (seconds).
     */
    double getAdaptiveStep(const SimulationContext& ctx,
                           double fixed_step, double courant) const;

    /// Access the shape-grouped xsect manager.
    const XSectGroups& xsectGroups() const { return groups_; }

    /// Set the DWSolver OpenMP thread count (delegates to DWSolver::setNumThreads).
    void setDWNumThreads(int n) { dw_solver_.setNumThreads(n); }

    /// Access the DW solver (for non-conduit node state scatter).
    dynwave::DWSolver& dwSolver() { return dw_solver_; }

private:
    RouteModel model_ = RouteModel::DYNWAVE;
    XSectGroups groups_;
    kinwave::KWSolver kw_solver_;
    dynwave::DWSolver dw_solver_;
    divider::DividerSoA dividers_;

    /// Save old hydraulic states before routing.
    void saveOldStates(SimulationContext& ctx);

    /// Initialise node inflows from lateral flows and losses.
    void initNodeFlows(SimulationContext& ctx, double dt);

    /// Update final link states (depth, volume) after routing.
    void updateLinkStates(SimulationContext& ctx);
};

} // namespace openswmm

#endif // OPENSWMM_ROUTING_HPP
