/**
 * @file Outfall.cpp
 * @brief Outfall boundary depths — matching legacy link_setOutfallDepth / outfall_setOutletDepth.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Outfall.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/DateTime.hpp"
#include "XSectBatch.hpp"
#include "Link.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace outfall {

/// Translate LinkData::XsectShape to batch XSectShape (different enum orderings).
static int translateShape(XsectShape link_shape) {
    switch (link_shape) {
        case XsectShape::CIRCULAR:        return static_cast<int>(XSectShape::CIRCULAR);
        case XsectShape::FILLED_CIRCULAR: return static_cast<int>(XSectShape::FILLED_CIRCULAR);
        case XsectShape::RECT_CLOSED:     return static_cast<int>(XSectShape::RECT_CLOSED);
        case XsectShape::RECT_OPEN:       return static_cast<int>(XSectShape::RECT_OPEN);
        case XsectShape::TRAPEZOIDAL:     return static_cast<int>(XSectShape::TRAPEZOIDAL);
        case XsectShape::TRIANGULAR:      return static_cast<int>(XSectShape::TRIANGULAR);
        case XsectShape::PARABOLIC:       return static_cast<int>(XSectShape::PARABOLIC);
        case XsectShape::POWER:           return static_cast<int>(XSectShape::POWERFUNC);
        case XsectShape::MODBASKETHANDLE: return static_cast<int>(XSectShape::MOD_BASKET);
        case XsectShape::EGGSHAPED:       return static_cast<int>(XSectShape::EGGSHAPED);
        case XsectShape::HORSESHOE:       return static_cast<int>(XSectShape::HORSESHOE);
        case XsectShape::GOTHIC:          return static_cast<int>(XSectShape::GOTHIC);
        case XsectShape::CATENARY:        return static_cast<int>(XSectShape::CATENARY);
        case XsectShape::SEMIELLIPTICAL:  return static_cast<int>(XSectShape::SEMIELLIPTICAL);
        case XsectShape::BASKETHANDLE:    return static_cast<int>(XSectShape::BASKETHANDLE);
        case XsectShape::SEMICIRCULAR:    return static_cast<int>(XSectShape::SEMICIRCULAR);
        case XsectShape::RECT_TRIANG:     return static_cast<int>(XSectShape::RECT_TRIANG);
        case XsectShape::RECT_ROUND:      return static_cast<int>(XSectShape::RECT_ROUND);
        case XsectShape::HORIZ_ELLIPSE:   return static_cast<int>(XSectShape::HORIZ_ELLIPSE);
        case XsectShape::VERT_ELLIPSE:    return static_cast<int>(XSectShape::VERT_ELLIPSE);
        case XsectShape::ARCH:            return static_cast<int>(XSectShape::ARCH);
        case XsectShape::IRREGULAR:       return static_cast<int>(XSectShape::IRREGULAR);
        case XsectShape::CUSTOM:          return static_cast<int>(XSectShape::CUSTOM);
        case XsectShape::FORCE_MAIN:      return static_cast<int>(XSectShape::FORCE_MAIN);
        case XsectShape::STREET_XSECT:    return static_cast<int>(XSectShape::STREET_XSECT);
        case XsectShape::DUMMY:           return static_cast<int>(XSectShape::DUMMY);
        default:                          return static_cast<int>(XSectShape::DUMMY);
    }
}

/// Build XSectParams from link SoA data for a conduit (with shape translation).
static XSectParams buildXSectParams(const SimulationContext& ctx, std::size_t uk) {
    XSectParams xs{};
    xs.type   = ctx.links.xsect_batch_shape[uk];
    xs.y_full = ctx.links.xsect_y_full[uk];
    xs.a_full = ctx.links.xsect_a_full[uk];
    xs.w_max  = ctx.links.xsect_w_max[uk];
    xs.r_full = ctx.links.xsect_r_full[uk];
    xs.s_full = ctx.links.xsect_s_full[uk];
    xs.s_max  = ctx.links.xsect_s_max[uk];
    xs.y_bot  = ctx.links.xsect_y_bot[uk];
    xs.a_bot  = ctx.links.xsect_a_bot[uk];
    xs.s_bot  = ctx.links.xsect_s_bot[uk];
    xs.r_bot  = ctx.links.xsect_r_bot[uk];
    return xs;
}

/// Compute normal depth for a given flow rate in a conduit.
/// Matches legacy link_getYnorm: s = q/beta, a = getAofS(s), y = getYofA(a).
static double getYnorm(const XSectParams& xs, double beta, double q_max,
                       double q) {
    if (beta <= 0.0 || q <= 0.0) return 0.0;
    if (q > q_max && q_max > 0.0) q = q_max;
    double s = q / beta;
    double a = xsect::getAofS(xs, s);
    double y = xsect::getYofA(xs, a);
    return y;
}

void setAllOutfallDepths(SimulationContext& ctx, double current_time) {
    auto& nodes = ctx.nodes;

    // Legacy: iterates over LINKS and calls link_setOutfallDepth(j) for each.
    // Each link finds if either end is an outfall and computes yNorm + yCrit.
    // Then calls node_setOutletDepth(n, yNorm, yCrit, z).
    // For FREE outfall: depth = z + MIN(yNorm, yCrit).

    for (int j = 0; j < ctx.n_nodes(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (nodes.type[uj] != NodeType::OUTFALL) continue;

        double depth = 0.0;

        // Find the conduit connecting to this outfall and compute depths
        int link_idx = -1;
        double z = 0.0;  // offset height at outfall end
        for (int k = 0; k < ctx.n_links(); ++k) {
            auto uk = static_cast<std::size_t>(k);
            if (ctx.links.type[uk] != LinkType::CONDUIT) continue;
            if (ctx.links.node2[uk] == j) {
                link_idx = k;
                z = ctx.links.offset2[uk];
                break;
            } else if (ctx.links.node1[uk] == j) {
                link_idx = k;
                z = ctx.links.offset1[uk];
                break;
            }
        }

        // Compute normal and critical depths for the connecting conduit
        double yNorm = 0.0, yCrit = 0.0;
        if (link_idx >= 0) {
            auto uk = static_cast<std::size_t>(link_idx);
            int barrels = std::max(ctx.links.barrels[uk], 1);
            double q = std::fabs(ctx.links.flow[uk]) / barrels;
            XSectParams xs = buildXSectParams(ctx, uk);
            yNorm = getYnorm(xs, ctx.links.beta[uk], ctx.links.q_max[uk], q);
            yCrit = xsect::getYcrit(xs, q);
        }

        switch (nodes.outfall_type[uj]) {
            case OutfallType::FREE:
                // Legacy: depth = z + MIN(yNorm, yCrit)
                depth = z + std::min(yNorm, yCrit);
                break;

            case OutfallType::NORMAL:
                // Legacy: depth = z + yNorm
                depth = z + yNorm;
                break;

            case OutfallType::FIXED: {
                double stage = nodes.outfall_param[uj];
                // Legacy: let yCrit = MIN(yCrit, yNorm)
                yCrit = std::min(yCrit, yNorm);
                // If critical depth elev < stage, use stage
                if (yCrit + z + nodes.invert_elev[uj] < stage)
                    depth = stage - nodes.invert_elev[uj];
                // Else if conduit above outfall invert, use critical depth
                else if (z > 0.0)
                    depth = z + yCrit;
                // Else use max of critical depth and stage
                else
                    depth = std::max(0.0, stage - nodes.invert_elev[uj]);
                break;
            }

            case OutfallType::TIDAL: {
                int curve_idx = static_cast<int>(nodes.outfall_param[uj]);
                double stage = nodes.invert_elev[uj];
                if (curve_idx >= 0 && curve_idx < static_cast<int>(ctx.tables.tables.size())) {
                    int h_tmp, m_tmp, s_tmp;
                    datetime::decodeTime(current_time, h_tmp, m_tmp, s_tmp);
                    double hour = static_cast<double>(h_tmp) + m_tmp / 60.0 + s_tmp / 3600.0;
                    stage = table_lookup_cursor(ctx.tables.tables[static_cast<std::size_t>(curve_idx)], hour);
                }
                yCrit = std::min(yCrit, yNorm);
                if (yCrit + z + nodes.invert_elev[uj] < stage)
                    depth = stage - nodes.invert_elev[uj];
                else if (z > 0.0)
                    depth = z + yCrit;
                else
                    depth = std::max(0.0, stage - nodes.invert_elev[uj]);
                break;
            }

            case OutfallType::TIMESERIES: {
                int ts_idx = static_cast<int>(nodes.outfall_param[uj]);
                double stage = nodes.invert_elev[uj];
                if (ts_idx >= 0 && ts_idx < static_cast<int>(ctx.tables.tables.size())) {
                    stage = table_lookup_cursor(ctx.tables.tables[static_cast<std::size_t>(ts_idx)], current_time);
                }
                yCrit = std::min(yCrit, yNorm);
                if (yCrit + z + nodes.invert_elev[uj] < stage)
                    depth = stage - nodes.invert_elev[uj];
                else if (z > 0.0)
                    depth = z + yCrit;
                else
                    depth = std::max(0.0, stage - nodes.invert_elev[uj]);
                break;
            }
        }

        nodes.depth[uj] = depth;
        nodes.head[uj] = nodes.invert_elev[uj] + depth;
    }
}

} // namespace outfall
} // namespace openswmm
