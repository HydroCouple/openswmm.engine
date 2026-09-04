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
 * @file KinematicWave.hpp
 * @brief Kinematic wave routing solver — batch-oriented design.
 *
 * @details Port of legacy kinwave.c, restructured for data-oriented execution:
 *
 *          1. **Batch geometry** — uses XSectGroups to pre-compute section
 *             factors for all conduits before solving
 *          2. **Per-conduit Newton** — the continuity equation solve is
 *             inherently per-conduit (each converges independently), but
 *             conduits are grouped by shape so the section-factor evaluations
 *             within Newton can also be batched per group
 *          3. **Batch state update** — inlet/outlet areas and flows updated
 *             for all conduits in contiguous sweeps
 *
 *          Weighted finite-difference scheme:
 *            WX = 0.6 (space weighting)
 *            WT = 0.6 (time weighting)
 *
 * @note Legacy reference: src/legacy/engine/kinwave.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_KINEMATIC_WAVE_HPP
#define OPENSWMM_KINEMATIC_WAVE_HPP

#include "XSectBatch.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace hydstruct { class StructureSolver; }

namespace kinwave {

// ============================================================================
// Tree-layout routing helpers — shared by KINWAVE and STEADY
// ============================================================================
//
// Legacy keeps these in flowrout.c because Steady Flow and Kinematic Wave run
// the SAME outer loop (flowrout_execute) over a topologically sorted link
// array; only the per-conduit kernel differs. They live here so the KW loop
// and Router::executeSteadyFlow share one implementation.

/**
 * @brief Flow into the upstream end of a link under Steady/Kin. Wave routing.
 *
 * @details PARITY flowrout.c:517 getLinkInflow:
 * @code
 *     if ( Link[j].type == CONDUIT || Link[j].type == PUMP ||
 *          Node[n1].type == STORAGE )  q = link_getInflow(j);
 *     else                             q = 0.0;
 *     return node_getMaxOutflow(n1, q, dt);
 * @endcode
 * A non-conduit link therefore carries its own HEAD-DISCHARGE flow when it
 * drains a storage unit, and NOTHING otherwise — never the upstream node's
 * inflow. Passing the node inflow through gives every outlet of a node the
 * full inflow, so N outlets manufacture N× the water.
 *
 * @param ctx         Simulation context.
 * @param structures  Structure solver used to evaluate pump/orifice/weir/
 *                    outlet discharge. May be null (structures then carry 0).
 * @param j           Link index.
 * @param dt          Routing timestep (seconds).
 * @returns Link inflow (cfs), already limited by the upstream node's
 *          available volume.
 */
double getLinkInflow(SimulationContext& ctx,
                     hydstruct::StructureSolver* structures,
                     int j, double dt);

/**
 * @brief Update a storage node's depth & volume by successive approximation.
 *
 * @details PARITY flowrout.c:537 updateStorageState + :611 getStorageOutflow.
 * Called from inside the sorted-link loop, BEFORE routing the links that
 * drain node `i`, so those links see the converged end-of-step depth. The
 * outflow term is re-evaluated at each iterate because it depends on that
 * depth — an orifice on a pond is an implicit relation, not a known rate.
 *
 * `order`/`pos` locate the node's outlet links: the topological sort emits a
 * node's outgoing links contiguously, so the scan runs forward from `pos`
 * until node1 changes (matching legacy's `if (Link[m].node1 != i) break`).
 *
 * @param ctx         Simulation context.
 * @param structures  Structure solver (see getLinkInflow).
 * @param order       Topologically sorted link order.
 * @param pos         Current position in `order`.
 * @param i           Storage node index.
 * @param dt          Routing timestep (seconds).
 */
void updateStorageState(SimulationContext& ctx,
                        hydstruct::StructureSolver* structures,
                        const std::vector<int>& order,
                        int pos, int i, double dt);

/**
 * @brief End-of-step node/link state for tree-layout routing (KW/steady).
 *
 * @details PARITY flowrout.c:203-205 — after every link is routed, legacy
 * runs setNewNodeState for every node (non-storage volume integration,
 * overflow shedding, depth reset — a junction stores nothing under KW/SF)
 * and then setNewLinkState's updateNodeDepth raises, which lift each
 * conduit's end-node depths to the conduit end flow depths plus offsets,
 * with a flooded non-outfall node pinned at its full depth. A terminal
 * storage unit (no outlet link, so never visited by the sorted-link loop)
 * gets its updateStorageState here, as legacy does (flowrout.c:88-91).
 *
 * @param ctx              Simulation context.
 * @param structures       Structure solver (see getLinkInflow).
 * @param order            Topologically sorted link order.
 * @param storage_updated  Per-node flags set by the sorted-link loop.
 * @param link_y1          Per-link flow depth at the upstream end (ft).
 * @param link_y2          Per-link flow depth at the downstream end (ft).
 * @param dt               Routing timestep (seconds).
 */
void finishRouting(SimulationContext& ctx,
                   hydstruct::StructureSolver* structures,
                   const std::vector<int>& order,
                   const std::vector<char>& storage_updated,
                   const std::vector<double>& link_y1,
                   const std::vector<double>& link_y2,
                   double dt);

// ============================================================================
// Constants (matching legacy)
// ============================================================================

constexpr double WX    = 0.6;     ///< Distance weighting factor
constexpr double WT    = 0.6;     ///< Time weighting factor
constexpr double EPSIL = 0.001;   ///< Newton convergence tolerance

// ============================================================================
// KW solver — operates on entire conduit set
// ============================================================================

/**
 * @brief Kinematic wave solver state.
 *
 * @details Holds SoA working arrays for all conduits. Allocated once at init.
 *          The `execute()` method routes all conduits for one timestep.
 */
class KWSolver {
public:
    /// Initialise for n conduit-type links. Call once after model is built.
    /// Builds topological link order for upstream → downstream processing.
    void init(int n_conduits, const XSectGroups& groups);

    /// Set topological link order (must be called after init, before first execute).
    void setLinkOrder(const std::vector<int>& sorted_links) {
        sorted_links_ = sorted_links;
    }

    /**
     * @brief Route all conduits for one KW timestep.
     *
     * @details
     *   1. Batch-compute inlet section factors from inflows using XSectGroups
     *   2. For each conduit, solve the Newton continuity equation
     *   3. Batch-compute outflows from outlet section factors
     *   4. Update state arrays
     *
     * @param ctx  Simulation context (links/nodes modified in place).
     * @param dt   Timestep (seconds).
     * @returns Average number of Newton iterations across all conduits.
     */
    int execute(SimulationContext& ctx, double dt,
                hydstruct::StructureSolver* structures = nullptr);

    /// Topological link order (upstream → downstream)
    std::vector<int> sorted_links_;

    /// Per-node "already converged this step" flag for storage units
    /// (legacy Node[i].updated). Reset at the top of every execute().
    std::vector<char> storage_updated_;

    /// Per-conduit Newton solve. Returns iteration count.
    int solveConduit(int idx, const XSectParams& xs,
                     double q_full, double a_full, double s_full,
                     double beta, double length, double dt,
                     double loss_rate);

    // Per-conduit SoA state (indexed by conduit-link index)
    // Public so that unit tests can set up / inspect working arrays directly.
    std::vector<double> q1_;    ///< Previous inlet flow (cfs)
    std::vector<double> a1_;    ///< Previous inlet area (ft2)
    std::vector<double> q2_;    ///< Previous outlet flow (cfs)
    std::vector<double> a2_;    ///< Previous outlet area (ft2)

    // Working buffers (reused each timestep)
    std::vector<double> q_in_;      ///< Inflow to each conduit (cfs)
    std::vector<double> a_in_;      ///< Inlet area from inflow (ft2)
    std::vector<double> q_out_;     ///< Computed outflow (cfs)
    std::vector<double> a_out_;     ///< Outlet area from Newton solve (ft2)
    std::vector<double> sf_in_;     ///< Section factor at inlet
    std::vector<double> y1_;        ///< End-of-step upstream flow depth (ft)
    std::vector<double> y2_;        ///< End-of-step downstream flow depth (ft)

private:
    int n_conduits_ = 0;
};

} // namespace kinwave
} // namespace openswmm

#endif // OPENSWMM_KINEMATIC_WAVE_HPP
