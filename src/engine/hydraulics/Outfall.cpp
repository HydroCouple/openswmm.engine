/**
 * @file Outfall.cpp
 * @brief Outfall boundary depths — numerically identical to legacy.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Outfall.hpp"
#include "../core/SimulationContext.hpp"
#include "XSectBatch.hpp"
#include "Link.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace outfall {

void setAllOutfallDepths(SimulationContext& ctx, double current_time) {
    auto& nodes = ctx.nodes;

    for (int j = 0; j < ctx.n_nodes(); ++j) {
        auto uj = static_cast<size_t>(j);
        if (nodes.type[uj] != NodeType::OUTFALL) continue;

        double depth = 0.0;

        switch (nodes.outfall_type[uj]) {
            case OutfallType::FREE: {
                // Critical depth from connecting conduit
                // Find a link connected to this outfall
                for (int k = 0; k < ctx.n_links(); ++k) {
                    auto uk = static_cast<size_t>(k);
                    if (ctx.links.node2[uk] == j || ctx.links.node1[uk] == j) {
                        if (ctx.links.type[uk] == LinkType::CONDUIT) {
                            double q = std::fabs(ctx.links.flow[uk]);
                            XSectParams xs;
                            xs.type = static_cast<int>(ctx.links.xsect_shape[uk]);
                            xs.y_full = ctx.links.xsect_y_full[uk];
                            xs.a_full = ctx.links.xsect_a_full[uk];
                            xs.w_max = ctx.links.xsect_w_max[uk];
                            depth = xsect::getYcrit(xs, q);
                            break;
                        }
                    }
                }
                break;
            }

            case OutfallType::NORMAL: {
                // Normal depth from connecting conduit
                for (int k = 0; k < ctx.n_links(); ++k) {
                    auto uk = static_cast<size_t>(k);
                    if (ctx.links.node2[uk] == j || ctx.links.node1[uk] == j) {
                        if (ctx.links.type[uk] == LinkType::CONDUIT && ctx.links.beta[uk] > 0.0) {
                            double q = std::fabs(ctx.links.flow[uk]);
                            XSectParams xs;
                            xs.type = static_cast<int>(ctx.links.xsect_shape[uk]);
                            xs.y_full = ctx.links.xsect_y_full[uk];
                            xs.a_full = ctx.links.xsect_a_full[uk];
                            xs.w_max = ctx.links.xsect_w_max[uk];
                            xs.s_full = ctx.links.xsect_s_full[uk];
                            xs.s_max = xs.s_full;
                            depth = link::getDepthFromFlow(xs, ctx.links.beta[uk], q);
                            break;
                        }
                    }
                }
                break;
            }

            case OutfallType::FIXED: {
                // Fixed water surface elevation
                double wse = nodes.outfall_param[uj];
                depth = std::max(0.0, wse - nodes.invert_elev[uj]);
                break;
            }

            case OutfallType::TIDAL: {
                // Tidal curve: outfall_param stores curve index
                int curve_idx = static_cast<int>(nodes.outfall_param[uj]);
                if (curve_idx >= 0 && curve_idx < static_cast<int>(ctx.tables.tables.size())) {
                    // Tidal curves use hour of day as independent variable
                    double hour = std::fmod(current_time * 24.0, 24.0);
                    double wse = table_lookup_cursor(ctx.tables.tables[static_cast<size_t>(curve_idx)], hour);
                    depth = std::max(0.0, wse - nodes.invert_elev[uj]);
                }
                break;
            }

            case OutfallType::TIMESERIES: {
                // Time series: outfall_param stores timeseries index
                int ts_idx = static_cast<int>(nodes.outfall_param[uj]);
                if (ts_idx >= 0 && ts_idx < static_cast<int>(ctx.tables.tables.size())) {
                    double wse = table_lookup_cursor(ctx.tables.tables[static_cast<size_t>(ts_idx)], current_time);
                    depth = std::max(0.0, wse - nodes.invert_elev[uj]);
                }
                break;
            }
        }

        nodes.depth[uj] = depth;
        nodes.head[uj] = nodes.invert_elev[uj] + depth;
    }
}

} // namespace outfall
} // namespace openswmm
