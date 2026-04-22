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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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

private:
    int n_pollutants_ = 0;

    // Quality mass inflow arrays are stored on NodeData (nodes.qual_mass_in[],
    // nodes.qual_vol_in[]) so that external quality sources (user forcing, DWF
    // quality, etc.) can contribute at the same assembly point.

    /// Add RDII pollutant loads to node quality inflows.
    void addRdiiLoads(SimulationContext& ctx, double dt);

    /// Batch accumulate link mass flows to downstream nodes.
    void accumulateLinkLoads(SimulationContext& ctx, double dt);

    /// Batch complete mixing at all nodes.
    void mixAtNodes(SimulationContext& ctx, double dt);

    /// Apply treatment expressions at nodes with treatment defined.
    void applyTreatment(SimulationContext& ctx, double dt);

    /// Batch first-order decay.
    void applyDecay(SimulationContext& ctx, double dt);

};

} // namespace quality
} // namespace openswmm

#endif // OPENSWMM_QUALITY_ROUTING_HPP
