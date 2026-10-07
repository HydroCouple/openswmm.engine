// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../core/SimulationContext.hpp"
#include <algorithm>
namespace openswmm {
// A surface-coupled outfall is a finite reservoir, not an unlimited stage
// boundary. Apply before node continuity consumes the hydraulic link flow.
inline double boundSurfaceOutfallFlow(const SimulationContext& ctx, int link, double q) {
    if (ctx.surface_outfall_link_limit.empty() || q == 0) return q;
    const int donor = q > 0 ? ctx.links.node1[link] : ctx.links.node2[link];
    if (donor < 0 || static_cast<std::size_t>(donor) >= ctx.surface_outfall_link_limit.size()) return q;
    const double cap = ctx.surface_outfall_link_limit[donor];
    return q > 0 ? std::min(q, cap) : std::max(q, -cap);
}
}
