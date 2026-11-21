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
 * @file      KinematicWave.hpp
 * @brief     Kinematic wave routing solver — batch-oriented design.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_KINEMATIC_WAVE_HPP
#define OPENSWMM_KINEMATIC_WAVE_HPP

#include "XSectBatch.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace hydstruct {
class StructureSolver;
}

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
double getLinkInflow(SimulationContext& ctx, hydstruct::StructureSolver* structures, int j,
                     double dt);

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
void updateStorageState(SimulationContext& ctx, hydstruct::StructureSolver* structures,
                        const std::vector<int>& order, int pos, int i, double dt);

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
void finishRouting(SimulationContext& ctx, hydstruct::StructureSolver* structures,
                   const std::vector<int>& order, const std::vector<char>& storage_updated,
                   const std::vector<double>& link_y1, const std::vector<double>& link_y2,
                   double dt);

/**
 * @brief Limit positive conduit loss components by available tree-routing inflow.
 *
 * Shares q proportionally between evaporation and positive seepage only; negative seepage gain
 * remains unchanged and does not compete for donor water. Does not book mass-balance volumes,
 * validate rates or cap gaining aquifer inventory.
 *
 * @param[in,out] ctx Borrowed aligned conduit evaporation/seepage rates, updated in place only when
 *     positive combined loss exceeds q.
 * @param[in] ucr Valid conduit subtype row, not global link index.
 * @param[in] q Nonnegative available per-barrel inflow cfs.
 * @return Net capped evaporation+signed seepage cfs, which may be negative for an aquifer gain.
 */
double capConduitLoss(SimulationContext& ctx, std::size_t ucr, double q);

// ============================================================================
// Constants (matching legacy)
// ============================================================================

constexpr double WX = 0.6;      ///< Distance weighting factor
constexpr double WT = 0.6;      ///< Time weighting factor
constexpr double EPSIL = 0.001; ///< Newton convergence tolerance

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
    /**
     * @brief Allocate/reset kinematic-wave history and work vectors by global link slot.
     *
     * Zeros q1/a1/q2/a2 and endpoint depths; resizes other work vectors preserving prefixes. Does not
     * construct/set topological order, despite historical declaration comment. Allocation can leave
     * partial reset.
     *
     * @param[in] n_conduits Nonnegative number of global link slots required by execute (Router passes
     *     n_links despite historical name); negative unchecked.
     * @param[in] groups Borrowed shape-group manager, currently unused and not retained.
     */
    void init(int n_conduits, const XSectGroups& groups);

    /**
     * @brief Copy the topologically sorted global link order used for tree routing.
     *
     * Owned order copy; empty selects natural-link-order fallback at execute. Does not validate
     * topology/order or initialize numerical state. Allocation may throw.
     *
     * @param[in] sorted_links Borrowed valid global indices, with contiguous outgoing links per node
     *     and main divider outlet before diversion.
     */
    void setLinkOrder(const std::vector<int>& sorted_links) { sorted_links_ = sorted_links; }

    /**
     * @brief Route one tree-layout kinematic-wave step and update node/link native state.
     *
     * Uses configured topological order or natural fallback, updates storage before its outlet links,
     * routes ordinary conduits per barrel, and handles DUMMY/nonconduits as pass-through/head-discharge
     * paths. Publishes accepted KW inlet flow for quality, outlet cfs, ft depths, ft3 volumes and
     * full-state flags, then finishes node states. Scratch/fallback order can allocate. No generic
     * convergence-failure status or transactional rollback; caller serializes use.
     *
     * @param[in,out] ctx Borrowed resolved hydraulic model with old/current arrays and subtype mappings
     *     initialized; node inflow/outflow accumulators must be prepared.
     * @param[in] dt Positive finite routing seconds.
     * @param[in] structures Optional borrowed initialized nonconduit solver; NULL causes unsupported
     *     structure paths to carry0.
     * @return Integer-truncated average continuity iteration count, or1 when no conduits solved.
     */
    int execute(SimulationContext& ctx, double dt,
                hydstruct::StructureSolver* structures = nullptr);

    /// Topological link order (upstream → downstream)
    std::vector<int> sorted_links_;

    /// Per-node "already converged this step" flag for storage units
    /// (legacy Node[i].updated). Reset at the top of every execute().
    std::vector<char> storage_updated_;

    /**
     * @brief Solve one native per-barrel KW continuity update with safeguarded Newton.
     *
     * Updates inlet/outlet areas/flows and next-step history in place. Uses WX=WT=0.6 and normalized
     * area tolerance0.001; dry branch gives exact zero. Full/no-flow brackets and failed Newton retain
     * selected/last candidate without an error return. Caps accepted normalized inflow at1 after solve;
     * no output rollback, finite validation or barrel scaling.
     *
     * @param[in] idx Valid GLOBAL link slot in all solver history/input/output vectors; q_in already
     *     populated.
     * @param[in] xs Borrowed valid native scalar section with live profile tables; matching full/s_max
     *     properties.
     * @param[in] q_full Positive full per-barrel discharge cfs; nonpositive writes zero q_out/a_out and
     *     returns0.
     * @param[in] a_full Positive full per-barrel area ft2; nonpositive follows zero-output branch.
     * @param[in] s_full Historical full section-factor argument, currently unused; xs.s_full supplies
     *     geometry.
     * @param[in] beta Positive native conveyance coefficient matching section-factor units.
     * @param[in] length Positive routing length ft.
     * @param[in] dt Positive finite routing seconds.
     * @param[in] loss_rate Signed per-barrel net loss cfs already donor-capped; negative represents
     *     gain.
     * @return 0 for nonpositive full dimensions, otherwise positive function-evaluation/nominal count;
     *     refused/exhausted root cases are normalized to1.
     */
    int solveConduit(int idx, const XSectParams& xs, double q_full, double a_full, double s_full,
                     double beta, double length, double dt, double loss_rate);

    // Per-conduit SoA state (indexed by conduit-link index)
    // Public so that unit tests can set up / inspect working arrays directly.
    std::vector<double> q1_; ///< Previous inlet flow (cfs)
    std::vector<double> a1_; ///< Previous inlet area (ft2)
    std::vector<double> q2_; ///< Previous outlet flow (cfs)
    std::vector<double> a2_; ///< Previous outlet area (ft2)

    // Working buffers (reused each timestep)
    std::vector<double> q_in_;  ///< Inflow to each conduit (cfs)
    std::vector<double> a_in_;  ///< Inlet area from inflow (ft2)
    std::vector<double> q_out_; ///< Computed outflow (cfs)
    std::vector<double> a_out_; ///< Outlet area from Newton solve (ft2)
    std::vector<double> sf_in_; ///< Section factor at inlet
    std::vector<double> y1_;    ///< End-of-step upstream flow depth (ft)
    std::vector<double> y2_;    ///< End-of-step downstream flow depth (ft)

private:
    int n_conduits_ = 0;
};

} // namespace kinwave
} // namespace openswmm

#endif // OPENSWMM_KINEMATIC_WAVE_HPP
