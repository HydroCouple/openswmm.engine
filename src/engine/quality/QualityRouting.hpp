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
 * @file QualityRouting.hpp
 * @brief Water quality routing — constituent transport, mixing, decay.
 *
 * @details Batch-oriented quality routing:
 *   1. Batch accumulate link mass flows to downstream nodes (vectorisable)
 *   2. Batch complete mixing at all nodes
 *   3. Batch first-order decay in all links/nodes
 *   4. Batch evaporation concentration factor
 *
 * @note Legacy reference: src/legacy/engine/qualrout.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_ROUTING_HPP
#define OPENSWMM_QUALITY_ROUTING_HPP

#include <vector>

namespace openswmm {

struct SimulationContext;

namespace quality {

// ============================================================================
// Constants
// ============================================================================

constexpr double ZERO_VOLUME = 0.0353147;  ///< 1 liter in ft3
constexpr double ZERO_DEPTH  = 0.003281;   ///< 1 mm in ft

/**
 * @brief One node's [TREATMENT] application, against the CALLER's inflow
 *        figures — the seam the LEGACY pass and the LARD MIX share.
 *
 * @details Evaluates the node's compiled expressions (removal- and
 *          concentration-typed, with co-treatment resolution and cycle
 *          detection), rewrites `nodes.conc` for every treated pollutant and
 *          books the removed mass into `qual_routing_reacted`. The inflow
 *          concentration and rate come from the CALLER because the two
 *          engines hold them in different places: LEGACY accumulates
 *          `qual_mass_in`/`qual_vol_in`, while LARD's node inflow lives in
 *          its solver-internal drain accumulators — reading the LEGACY
 *          arrays under LARD made an R-typed expression a literal no-op
 *          (`cin` read 0, and `cOut = (c_in > 0) ? … : c_node` kept the
 *          untreated value; P2.3's first draft shipped exactly that).
 *
 * @param q_in_cfs  Inflow rate feeding the node this interval, ft³/s.
 * @param cin       Per-pollutant inflow concentration, length `np`.
 */
void applyNodeTreatment(SimulationContext& ctx, int node, double dt,
                        double q_in_cfs, const double* cin);

// ============================================================================
// Quality solver
// ============================================================================

class QualitySolver {
public:
    void init(int n_nodes, int n_links, int n_pollutants);

    /**
     * @brief Execute one quality routing timestep.
     *
     * @details
     *   1. Accumulate link mass flows → node quality (batch over links)
     *   2. Complete mixing at nodes: c_new = (c_old*V + W*dt) / (V + Vin)
     *   3. First-order decay: c = c * (1 - k*dt)
     *   4. Update link quality from upstream node
     *
     * @param ctx  Simulation context.
     * @param dt   Timestep (seconds).
     */
    void execute(SimulationContext& ctx, double dt);

    /**
     * @brief Stage 1 of execute() only: reset the assembly arrays and run the
     *        five external-load adders (washoff, RDII, DWF, GW, iface) into
     *        nodes.qual_mass_in / qual_vol_in — no mixing, decay, or link
     *        update.
     *
     * @details Split out (behavior-preserving refactor) so the Eulerian ARD
     *          engine (QUALITY_SOLVER EULERIAN_ARD) can consume the same
     *          source loads while replacing the CSTR transport stages —
     *          master plan §4.3 source-attribution seam.
     */
    void assembleExternalLoads(SimulationContext& ctx, double dt);

    /**
     * @brief Add direct external inflow (`[INFLOWS]` CONCEN/MASS) pollutant
     *        loads, and the direct inflow's water, to the node assembly arrays.
     *
     * @details The mass rates were evaluated by the inflow solver into
     *          nodes.ext_qual_mass (CONCEN rows already multiplied by the
     *          node's flow). This also folds the direct inflow volume into
     *          qual_vol_in, which is the mixing denominator — legacy divides
     *          by Node[j].inflow, a total that includes lateral inflow.
     * @see Legacy: routing.c addExternalInflows() pollutant portion
     */
    void addExtInflowLoads(SimulationContext& ctx, double dt);
    /// S3: 2D→1D junction drain — its water into qual_vol_in, its species
    /// mass (queued by SurfaceRouter2D, drained by assembleLateralInflows)
    /// into qual_mass_in. No-op when no 2D coupling is active.
    void addCouplingLoads(SimulationContext& ctx, double dt);

    /**
     * @brief Add subcatchment washoff quality loads to node inflows.
     *
     * @details For each subcatchment with runoff, adds the washoff
     *          concentration × flow as mass inflow to the outlet node.
     *          Matches legacy addWetWeatherInflows() in routing.c.
     */
    void addWetWeatherLoads(SimulationContext& ctx, double dt);

    /// Update link quality using volume-balance mixing with upstream node
    /// (DW/KW) or upstream node concentration with exponential decay (STEADY).
    /// Public for testing.
    void updateLinkQuality(SimulationContext& ctx, double dt);

    /// Apply treatment expressions at nodes with treatment defined. Public
    /// since E5b: the ARD engine reuses this exact evaluator for treatment
    /// interop — it runs on the PUBLISHED nodes.conc after the ARD step and
    /// the engine absorbs the treated concentrations back into its node
    /// stores (ArdEngine::absorbTreatedNodeConc). Books its own
    /// qual_routing_reacted losses.
    void applyTreatment(SimulationContext& ctx, double dt);

private:
    int n_pollutants_ = 0;

    // Quality mass inflow arrays are stored on NodeData (nodes.qual_mass_in[],
    // nodes.qual_vol_in[]) so that external quality sources (user forcing, DWF
    // quality, etc.) can contribute at the same assembly point.

    /// Add RDII pollutant loads to node quality inflows.
    void addRdiiLoads(SimulationContext& ctx, double dt);

    /// Add default dry weather pollutant loads (c_dwf) to node inflows.
    void addDwfLoads(SimulationContext& ctx, double dt);

    /// Add groundwater inflow pollutant loads (c_gw) to node inflows.
    void addGwLoads(SimulationContext& ctx, double dt);

    /// Add routing interface file pollutant loads to node inflows.
    void addIfaceLoads(SimulationContext& ctx, double dt);

    /// Batch accumulate link mass flows to downstream nodes.
    void accumulateLinkLoads(SimulationContext& ctx, double dt);

    /// Batch complete mixing at all nodes.
    void mixAtNodes(SimulationContext& ctx, double dt);

    /// Batch first-order decay.
    void applyDecay(SimulationContext& ctx, double dt);

};

} // namespace quality
} // namespace openswmm

#endif // OPENSWMM_QUALITY_ROUTING_HPP
