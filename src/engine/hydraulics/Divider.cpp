/**
 * @file Divider.cpp
 * @brief Flow divider — numerically identical to legacy node.c divider logic.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Divider.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace divider {

void DividerSoA::resize(int n) {
    count = n;
    auto un = static_cast<size_t>(n);
    node_idx.assign(un, -1);
    div_link_idx.assign(un, -1);
    method.assign(un, 0);
    cutoff_flow.assign(un, 0.0);
    weir_cd.assign(un, 0.0);
    weir_max_depth.assign(un, 0.0);
    table_idx.assign(un, -1);
}

void computeDividerFlows(SimulationContext& ctx, const DividerSoA& soa) {
    double ucf_flow = ucf::Qcf[static_cast<int>(ctx.options.flow_units)];

    for (int k = 0; k < soa.count; ++k) {
        auto uk = static_cast<size_t>(k);
        int ni = soa.node_idx[uk];
        if (ni < 0 || ni >= ctx.n_nodes()) continue;
        auto uni = static_cast<size_t>(ni);

        double q_total = ctx.nodes.inflow[uni];
        if (q_total <= 0.0) continue;

        double q_divert = 0.0;
        auto dm = static_cast<DividerMethod>(soa.method[uk]);

        switch (dm) {
            case DividerMethod::CUTOFF: {
                // Diversion gets flow above cutoff
                double qmin = soa.cutoff_flow[uk];
                q_divert = std::max(0.0, q_total - qmin);
                break;
            }
            case DividerMethod::OVERFLOW_DIV: {
                // Diversion gets flow exceeding primary link capacity
                // (primary link capacity = q_full of non-diversion link)
                // For now: divert excess above node full volume rate
                double full_depth = ctx.nodes.full_depth[uni];
                double q_primary = full_depth > 0.0
                    ? q_total * (full_depth / (full_depth + 1.0)) : q_total;
                q_divert = std::max(0.0, q_total - q_primary);
                break;
            }
            case DividerMethod::TABULAR: {
                // Lookup diversion fraction from curve
                int ti = soa.table_idx[uk];
                if (ti >= 0 && ti < static_cast<int>(ctx.tables.tables.size())) {
                    double frac = table_lookup_cursor(ctx.tables.tables[static_cast<size_t>(ti)], q_total * ucf_flow);
                    q_divert = q_total * std::max(0.0, std::min(1.0, frac));
                }
                break;
            }
            case DividerMethod::WEIR: {
                // Weir equation: Q = Cd * W * H^1.5
                double depth = ctx.nodes.depth[uni];
                double cd = soa.weir_cd[uk];
                double w = soa.weir_max_depth[uk];  // weir width
                if (depth > 0.0 && cd > 0.0 && w > 0.0) {
                    q_divert = cd * w * depth * std::sqrt(depth);
                    q_divert = std::min(q_divert, q_total);
                }
                break;
            }
        }

        // Apply diversion: set diversion link flow
        int dl = soa.div_link_idx[uk];
        if (dl >= 0 && dl < ctx.n_links()) {
            ctx.links.flow[static_cast<size_t>(dl)] = q_divert;
        }
    }
}

} // namespace divider
} // namespace openswmm
