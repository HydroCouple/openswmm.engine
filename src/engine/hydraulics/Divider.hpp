/**
 * @file Divider.hpp
 * @brief Flow divider node logic — cutoff, overflow, tabular, weir.
 *
 * @details Divider nodes split incoming flow between a primary and diversion
 *          link. The split depends on divider type:
 *            - CUTOFF:   diversion gets flow above cutoff value
 *            - OVERFLOW: diversion gets flow exceeding primary link capacity
 *            - TABULAR:  fraction from lookup table
 *            - WEIR:     weir equation determines diversion flow
 *
 *          Batch: group dividers by type, compute split per type group.
 *
 * @note Legacy reference: src/legacy/engine/node.c (divider_getOutflow)
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_DIVIDER_HPP
#define OPENSWMM_DIVIDER_HPP

#include "../data/NodeData.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace divider {

enum class DividerMethod : int {
    CUTOFF       = 0,
    OVERFLOW_DIV = 1,  ///< Renamed to avoid macOS math.h OVERFLOW macro
    TABULAR      = 2,
    WEIR         = 3
};

struct DividerSoA {
    int count = 0;
    std::vector<int>    node_idx;      ///< Divider node index
    std::vector<int>    div_link_idx;  ///< Diversion link index
    std::vector<int>    method;        ///< DividerMethod
    std::vector<double> cutoff_flow;   ///< Cutoff flow (for CUTOFF)
    std::vector<double> weir_cd;       ///< Discharge coeff (for WEIR)
    std::vector<double> weir_max_depth;///< Weir max depth
    std::vector<int>    table_idx;     ///< Curve index (for TABULAR)

    void resize(int n);
};

/**
 * @brief Compute diversion flows for all divider nodes.
 *
 * @details For each divider:
 *   - Reads total inflow at the divider node
 *   - Computes diversion flow based on method
 *   - Sets diversion link flow and reduces primary link flow
 *
 * @param ctx   Simulation context.
 * @param soa   Divider data.
 */
void computeDividerFlows(SimulationContext& ctx, const DividerSoA& soa);

} // namespace divider
} // namespace openswmm

#endif // OPENSWMM_DIVIDER_HPP
