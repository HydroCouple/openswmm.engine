// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SurfaceExchange.hpp
 * @brief     Conservative surface exchange reservations and donor water limits.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "../core/SimulationContext.hpp"
#include <algorithm>
namespace openswmm {
// A surface-coupled outfall is a finite reservoir, not an unlimited stage
// boundary. Apply before node continuity consumes the hydraulic link flow.
/**
 * @brief Bound a link flow by its upwind surface-outfall donor allowance.
 *
 * Does not decrement allowance or validate finite values/cap signs; apply before node continuity
 * consumes flow.
 *
 * @param[in] ctx Borrowed aligned endpoint arrays and nonnegative per-node cfs allowances.
 * @param[in] link Valid global link slot when limits are nonempty and q!=0; otherwise endpoints are
 *     not read.
 * @param[in] q Signed native cfs, positive node1 to node2.
 * @return q clipped to donor cap while preserving direction; unchanged when no limits, zero flow or
 *     donor index is absent.
 */
inline double boundSurfaceOutfallFlow(const SimulationContext& ctx, int link, double q) {
    if (ctx.surface_outfall_link_limit.empty() || q == 0)
        return q;
    const int donor = q > 0 ? ctx.links.node1[link] : ctx.links.node2[link];
    if (donor < 0 || static_cast<std::size_t>(donor) >= ctx.surface_outfall_link_limit.size())
        return q;
    const double cap = ctx.surface_outfall_link_limit[donor];
    return q > 0 ? std::min(q, cap) : std::max(q, -cap);
}
}
