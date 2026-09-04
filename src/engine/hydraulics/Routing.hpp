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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ROUTING_HPP
#define OPENSWMM_ROUTING_HPP

#include "XSectBatch.hpp"
#include "KinematicWave.hpp"
#include "DynamicWave.hpp"
#include "Divider.hpp"
#include "fv/FvOptions.hpp"
#include "fv/INetworkSolver.hpp"
#include "fv/NetworkMeshData.hpp"

#include <memory>
#include <string>
#include <functional>
#include <vector>

namespace openswmm {

struct SimulationContext;

// ============================================================================
// Routing model enum
// ============================================================================

enum class RouteModel : int {
    STEADY    = 0,   ///< Steady-state (pass-through)
    KINWAVE   = 1,   ///< Kinematic wave
    DYNWAVE   = 2,   ///< Dynamic wave (St. Venant, implicit Picard)
    FV        = 3    ///< Explicit conservative finite volume (Godunov)
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
     * @param ctx        Simulation context (modified in place).
     * @param dt         Routing timestep (seconds).
     * @param evap_rate  Current evaporation rate (ft/sec), from climate module.
     * @returns Number of solver iterations used.
     */
    int step(SimulationContext& ctx, double dt,
             double evap_rate = 0.0,
             dynwave::DWSolver::NonConduitFlowFunc non_conduit_fn = nullptr);

    /**
     * @brief Compute adaptive timestep (DW only).
     *
     * @param ctx          Simulation context.
     * @param fixed_step   User-specified routing step.
     * @param courant      Courant factor target (0 = use fixed).
     * @returns Adaptive timestep (seconds).
     */
    double getAdaptiveStep(SimulationContext& ctx,
                           double fixed_step, double courant);

    /// Access the shape-grouped xsect manager.
    const XSectGroups& xsectGroups() const { return groups_; }

    /// Gap #44: true if a routing cycle was detected during the last init().
    /// For KW/STEADY routing only (DW does not toposort).
    bool hasCycle() const { return cycle_detected_; }

    /// Set the DWSolver OpenMP thread count (delegates to DWSolver::setNumThreads).
    void setDWNumThreads(int n, std::vector<std::string>* warnings = nullptr) {
        dw_solver_.setNumThreads(n, warnings);
    }

    /// Access the DW solver (for non-conduit node state scatter).
    dynwave::DWSolver& dwSolver() { return dw_solver_; }

    /// Supply the structure solver used to evaluate pump/orifice/weir/outlet
    /// discharge. DYNWAVE/FV get theirs through the non_conduit_fn callback,
    /// but KINWAVE/STEADY evaluate structures inline (legacy getLinkInflow),
    /// so they need the solver itself. Must be set before the first step().
    void setStructureSolver(hydstruct::StructureSolver* s) { structures_ = s; }

    /// Whether the most recent step() converged. DYNWAVE returns the solver's
    /// real final Picard flag; other models always converge (legacy only tracks
    /// dynwave non-convergence). FV has no convergence loop at all — that is a
    /// property of the scheme, not an omission. Feeds the "% of Steps Not
    /// Converging" stat.
    bool lastStepConverged() const {
        return (model_ == RouteModel::DYNWAVE) ? dw_solver_.lastConverged() : true;
    }

    /// Access the FV solver (null unless FLOW_ROUTING FV). Used by the report
    /// plugin for the "FV Solver Statistics" block and by the tests.
    const fv::INetworkSolver* fvSolver() const { return fv_solver_.get(); }

    /// The FV mesh built at init (empty unless FLOW_ROUTING FV).
    const fv::NetworkMeshData& fvMesh() const { return fv_mesh_; }

    /// Backend label chosen by the FV factory ("cpu (...)", "omp (...)", ...).
    const std::string& fvBackend() const { return fv_backend_; }

    /// Diagnostics accumulated while building the FV mesh; the caller surfaces
    /// them through ctx.warnings / ctx.errors.
    const std::vector<std::string>& fvWarnings() const { return fv_warnings_; }
    const std::vector<std::string>& fvErrors()   const { return fv_errors_; }

private:
    RouteModel model_ = RouteModel::DYNWAVE;
    XSectGroups groups_;

    /// Non-owning; set by SWMMEngine. Null in standalone Router unit tests,
    /// where structures then carry no flow.
    hydstruct::StructureSolver* structures_ = nullptr;

    /// Per-node "already converged this step" flag for storage units under
    /// STEADY (the KW solver keeps its own).
    std::vector<char> steady_storage_updated_;
    /// Per-link end-of-step flow depth under STEADY routing (uniform along
    /// the conduit); consumed by kinwave::finishRouting's node-depth raises.
    std::vector<double> steady_y_;

    // --- Explicit finite-volume solver (FLOW_ROUTING FV) -------------------
    // The mesh and state are owned here, not by the solver, exactly as
    // SurfaceRouter2D owns MeshData/SurfaceStateData: the solver is swappable
    // (CPU or Kokkos plugin) and must not own what survives a backend change.
    fv::NetworkMeshData                  fv_mesh_;
    fv::NetworkStateData                 fv_state_;
    fv::FvOptions                        fv_opts_;      ///< internal units
    std::unique_ptr<fv::INetworkSolver>  fv_solver_;
    std::string                          fv_backend_;
    std::vector<std::string>             fv_warnings_, fv_errors_;

    // Per-step forcing buffers, allocated once at init.
    std::vector<double> fv_lateral_, fv_fixed_head_, fv_struct_flow_, fv_cond_loss_;

    /// Time integral of each structure's discharge over the routing step
    /// (ft³), and the elapsed time the last segment started at. Under
    /// per-substep coupling the discharge MOVES within the step, so the
    /// reported flow has to be the mean that was actually applied, not
    /// whichever substep happened to be last.
    std::vector<double> fv_struct_int_;
    double              fv_struct_t_prev_ = 0.0;

    void initFv(SimulationContext& ctx);
    int  stepFv(SimulationContext& ctx, double dt,
                dynwave::DWSolver::NonConduitFlowFunc non_conduit_fn);
    void publishFv(SimulationContext& ctx, double dt);
    kinwave::KWSolver kw_solver_;
    dynwave::DWSolver dw_solver_;
    // Divider data moved to ctx.node_subtypes.dividers (relational side-table).
    std::vector<int> steady_sorted_links_;  ///< Topological link order for STEADY routing
    bool cycle_detected_ = false;           ///< Gap #44: set true when toposort detects a loop


    /// Initialise node inflows from lateral flows and losses (including storage evap).
    void initNodeFlows(SimulationContext& ctx, double dt, double evap_rate);

    /// Compute conduit evaporation and seepage loss rates (per barrel).
    void computeConduitLosses(SimulationContext& ctx, double dt, double evap_rate);

    /// Re-evaluate the head-dependent FV forcing (outfall stages, structure
    /// discharges) against the solver's current state. Called from the
    /// solver's substep loop under FV_STRUCTURE_COUPLING SUBSTEP.
    void refreshFvBoundaryFlows(SimulationContext& ctx, double t_elapsed,
                                const std::function<void()>& eval_structures);

    /// Bank each structure's discharge over the span it was in force for.
    void accumulateStructureFlows(const SimulationContext& ctx, double t_now);

    /// Update final link states (depth, volume) after routing.
    void updateLinkStates(SimulationContext& ctx);

    /// Execute steady-state flow routing (Gap #33).
    /// Matches legacy flowrout.c::steadyflow_execute() loop.
    int executeSteadyFlow(SimulationContext& ctx, double dt);
};

} // namespace openswmm

#endif // OPENSWMM_ROUTING_HPP
