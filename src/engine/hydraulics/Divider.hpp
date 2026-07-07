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

#include "../data/NodeSubtypes.hpp"
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

/**
 * @brief Compute diversion flows for all divider nodes.
 *
 * @details For each divider:
 *   - Reads total inflow at the divider node
 *   - Computes diversion flow based on method
 *   - Sets diversion link flow and reduces primary link flow
 *
 *   Reads the relational DividerData side-table (ctx.node_subtypes.dividers),
 *   which replaces the former standalone DividerSoA. The data is identical
 *   (same dividers, same node-index order, same fields) so results are
 *   unchanged; the side-table is simply the single source of truth.
 *
 * @param ctx   Simulation context.
 * @param divs  Divider side-table (ctx.node_subtypes.dividers).
 */
void computeDividerFlows(SimulationContext& ctx, const DividerData& divs);

} // namespace divider
} // namespace openswmm

#endif // OPENSWMM_DIVIDER_HPP
