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
 * @file Outfall.cpp
 * @brief Outfall boundary depths — matching legacy link_setOutfallDepth / outfall_setOutletDepth.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Outfall.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/DateTime.hpp"
#include "../core/UnitConversion.hpp"
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

void buildOutfallLinkMap(SimulationContext& ctx) {
    auto& nodes = ctx.nodes;
    auto& outs  = ctx.node_subtypes.outfalls;   // relational side-table (authoritative)
    const int n_links = ctx.n_links();

    // Reset the cached connectivity on every outfall row.
    outs.link_idx.assign(static_cast<std::size_t>(outs.count()), -1);
    outs.link_offset.assign(static_cast<std::size_t>(outs.count()), 0.0);

    // Single pass over links; first matching conduit wins (matches the
    // first-break legacy behaviour of the inner scan).
    for (int k = 0; k < n_links; ++k) {
        auto uk = static_cast<std::size_t>(k);
        if (ctx.links.type[uk] != LinkType::CONDUIT) continue;

        int n1 = ctx.links.node1[uk];
        int n2 = ctx.links.node2[uk];

        if (n2 >= 0 && nodes.type[static_cast<std::size_t>(n2)] == NodeType::OUTFALL) {
            const int r = ctx.node_subtypes.outfall_row(n2);
            if (r >= 0 && outs.link_idx[static_cast<std::size_t>(r)] < 0) {
                outs.link_idx[static_cast<std::size_t>(r)]    = k;
                outs.link_offset[static_cast<std::size_t>(r)] = ctx.links.offset2[uk];
            }
        }
        if (n1 >= 0 && nodes.type[static_cast<std::size_t>(n1)] == NodeType::OUTFALL) {
            const int r = ctx.node_subtypes.outfall_row(n1);
            if (r >= 0 && outs.link_idx[static_cast<std::size_t>(r)] < 0) {
                outs.link_idx[static_cast<std::size_t>(r)]    = k;
                outs.link_offset[static_cast<std::size_t>(r)] = ctx.links.offset1[uk];
            }
        }
    }
}

namespace {
// Static (boundary-condition) outfall properties. Phase 3.2 of the relational
// refactor: read from the dense OutfallData side-table when available, else from
// the wide NodeData arrays. The side-table is built at hydraulics init *after*
// buildOutfallLinkMap, so link_idx/link_offset are resolved; and these fields are
// static during a run, so the side-table is an exact mirror of the wide arrays.
// The per-step-mutable head_2d is deliberately NOT included here — it is read
// separately from the side-table (outfalls.head_2d) at the use site.
struct OutfallStatic {
    OutfallType bc_type;
    double      param;
    uint8_t     has_flap;
    int         link_idx;
    double      link_offset;
};

inline OutfallStatic outfallStatic(const NodeData& nodes, const NodeSubtypes& subs,
                                   std::size_t uj) {
    (void)nodes;
    const int r = subs.outfall_row(static_cast<int>(uj));
    if (r >= 0) {
        const auto ur = static_cast<std::size_t>(r);
        return OutfallStatic{ subs.outfalls.bc_type[ur], subs.outfalls.param[ur],
                              subs.outfalls.has_flap_gate[ur], subs.outfalls.link_idx[ur],
                              subs.outfalls.link_offset[ur] };
    }
    // Non-outfall / no row: resize defaults (FREE, no flap, no cached link).
    return OutfallStatic{ OutfallType::FREE, 0.0, 0, -1, 0.0 };
}
}  // namespace

void setAllOutfallDepths(SimulationContext& ctx, double current_time) {
    auto& nodes = ctx.nodes;
    int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    double ucf_len = ucf::Ucf[ucf::LENGTH][unit_sys];

    // Legacy: iterates over LINKS and calls link_setOutfallDepth(j) for each.
    // Each link finds if either end is an outfall and computes yNorm + yCrit.
    // Then calls node_setOutletDepth(n, yNorm, yCrit, z).
    // For FREE outfall: depth = z + MIN(yNorm, yCrit).

    // Per-outfall computation body (unchanged from the original loop). Reads
    // the wide NodeData arrays by node index `j` so results are bit-for-bit
    // identical regardless of how outfalls are enumerated below.
    auto process_outfall = [&](int j) {
        auto uj = static_cast<std::size_t>(j);

        double depth = 0.0;

        // Cached outfall → conduit mapping (populated once at init).
        // Falls back to a scan only if the cache is empty (e.g. in unit
        // tests that skip Router::init).
        const OutfallStatic ost = outfallStatic(nodes, ctx.node_subtypes, uj);
        int link_idx = ost.link_idx;
        double z = ost.link_offset;
        if (link_idx < 0) {
            for (int k = 0; k < ctx.n_links(); ++k) {
                auto uk = static_cast<std::size_t>(k);
                if (ctx.links.type[uk] != LinkType::CONDUIT) continue;
                if (ctx.links.node2[uk] == j) {
                    link_idx = k; z = ctx.links.offset2[uk]; break;
                } else if (ctx.links.node1[uk] == j) {
                    link_idx = k; z = ctx.links.offset1[uk]; break;
                }
            }
        }

        // Compute normal and critical depths for the connecting conduit
        double yNorm = 0.0, yCrit = 0.0;
        if (link_idx >= 0) {
            auto uk = static_cast<std::size_t>(link_idx);
            const auto& CD = ctx.link_subtypes.conduits;
            const int cr = ctx.link_subtypes.conduit_row(link_idx);
            int barrels = std::max((cr >= 0) ? CD.barrels[static_cast<std::size_t>(cr)] : 1, 1);
            double q = std::fabs(ctx.links.flow[uk]) / barrels;
            XSectParams xs = buildXSectParams(ctx, uk);
            const double beta_v = (cr >= 0) ? CD.beta[static_cast<std::size_t>(cr)] : 0.0;
            const double qmax_v = (cr >= 0) ? CD.q_max[static_cast<std::size_t>(cr)] : 0.0;
            yNorm = getYnorm(xs, beta_v, qmax_v, q);
            yCrit = xsect::getYcrit(xs, q);
        }

        switch (ost.bc_type) {
            case OutfallType::FREE:
                // Legacy: depth = z + MIN(yNorm, yCrit)
                depth = z + std::min(yNorm, yCrit);
                break;

            case OutfallType::NORMAL:
                // Legacy: depth = z + yNorm
                depth = z + yNorm;
                break;

            case OutfallType::FIXED: {
                // outfall_param is ALREADY in internal feet (converted once in
                // PostParseResolver, matching legacy node.c fixedStage). The
                // TIDAL/TIMESERIES branches below divide by ucf_len because
                // those read raw display-unit table values; FIXED must not —
                // a second division inflated the stage by 1/ucf_len in metric
                // models (e.g. user3: 223.7 m → 733.9 m), driving huge spurious
                // outfall backflow that flooded the upstream storage nodes.
                double stage = ost.param;
                // Legacy outfall_setOutletDepth (node.c:1429-1454):
                //   yCrit = MIN(yCrit, yNorm)
                //   if (yCrit+z+inv < stage)  yNew = stage - inv
                //   else if (z > 0) { if (stage < inv+z) yNew = MAX(0, stage-inv)
                //                     else               yNew = z + yCrit }
                //   else                      yNew = yCrit
                yCrit = std::min(yCrit, yNorm);
                if (yCrit + z + nodes.invert_elev[uj] < stage)
                    depth = stage - nodes.invert_elev[uj];
                else if (z > 0.0)
                    depth = (stage < nodes.invert_elev[uj] + z)
                                ? std::max(0.0, stage - nodes.invert_elev[uj])
                                : z + yCrit;
                else
                    // free-overfall (no offset, stage below critical-depth
                    // elev): node sits at the conduit's critical depth, NOT 0.
                    // Using max(0, stage-inv) here left a free-discharging
                    // FIXED outfall below its invert at depth 0, so the
                    // connecting conduit's downstream depth/area was too small
                    // and it under-conveyed (user3 EGG conduit CCOROUT1 ~6%),
                    // backing water up and surcharging the upstream node.
                    depth = yCrit;
                break;
            }

            case OutfallType::TIDAL: {
                int curve_idx = static_cast<int>(ost.param);
                double stage = nodes.invert_elev[uj];
                if (curve_idx >= 0 && curve_idx < static_cast<int>(ctx.tables.tables.size())) {
                    int h_tmp, m_tmp, s_tmp;
                    datetime::decodeTime(current_time, h_tmp, m_tmp, s_tmp);
                    double hour = static_cast<double>(h_tmp) + m_tmp / 60.0 + s_tmp / 3600.0;
                    stage = table_lookup_cursor(ctx.tables.tables[static_cast<std::size_t>(curve_idx)], hour) / ucf_len;
                }
                yCrit = std::min(yCrit, yNorm);
                if (yCrit + z + nodes.invert_elev[uj] < stage)
                    depth = stage - nodes.invert_elev[uj];
                else if (z > 0.0)
                    depth = (stage < nodes.invert_elev[uj] + z)
                                ? std::max(0.0, stage - nodes.invert_elev[uj])
                                : z + yCrit;
                else
                    depth = yCrit;  // free overfall sits at critical depth (legacy node.c:1453)
                break;
            }

            case OutfallType::TIMESERIES: {
                int ts_idx = static_cast<int>(ost.param);
                double stage = nodes.invert_elev[uj];
                if (ts_idx >= 0 && ts_idx < static_cast<int>(ctx.tables.tables.size())) {
                    stage = table_lookup_cursor(ctx.tables.tables[static_cast<std::size_t>(ts_idx)], current_time) / ucf_len;
                }
                yCrit = std::min(yCrit, yNorm);
                if (yCrit + z + nodes.invert_elev[uj] < stage)
                    depth = stage - nodes.invert_elev[uj];
                else if (z > 0.0)
                    depth = (stage < nodes.invert_elev[uj] + z)
                                ? std::max(0.0, stage - nodes.invert_elev[uj])
                                : z + yCrit;
                else
                    depth = yCrit;  // free overfall sits at critical depth (legacy node.c:1453)
                break;
            }
        }

        // Standard outfall stage in absolute elevation, used by the
        // overrides below to express the priority ordering as a sequence
        // of max() / replace operations.
        double z_inv      = nodes.invert_elev[uj];
        double h_standard = z_inv + depth;

        // ----------------------------------------------------------------
        // C4 + C5: 2D-coupling override (tailwater from the 2D surface).
        //
        // If a 2D module is attached and this outfall is in the coupling
        // map, SurfaceRouter2D::updateOutfallBoundaries has cached the
        // current 2D surface head at the coupling cell into
        // nodes.outfall_2d_head[uj]. Non-coupled outfalls keep the
        // sentinel (-1e30), so the predicate below is false for them and
        // the legacy path is preserved bit-for-bit.
        //
        // Spec R2: "if HGL on the 2D surface is above the outfall
        // invert, it becomes the downstream boundary condition." The
        // explicit h_2d > z_inv guard (C5) matches the spec text and
        // is robust against any future outfall type whose h_standard
        // might fall below z_inv.
        //
        // Wet/dry blend: updateOutfallBoundaries caches a Hermite ramp
        // factor in [0,1] (0 = dry → free discharge, 1 = wet → full
        // tailwater) keyed on the 2D solver's dry_depth. The override
        // blends the prescribed stage from h_standard (free) up to the
        // raw tailwater h_2d as the cell wets, so a near-dry outfall cell
        // reverts to free discharge instead of being pinned at bed level
        // (which deadlocked the pipe), and the wet/dry transition stays
        // C¹ to avoid step-to-step chatter.
        //
        // Flap-gate logic (matches the original updateOutfallBoundaries
        // intent): when the raw tailwater h_2d would *raise* the stage
        // above h_standard, a closed flap gate prevents the 2D side from
        // pushing water back into the pipe network, so we skip the
        // override and leave h_standard intact.
        //
        // See docs/1D_2D_COUPLING_GATE_REVIEW.md §6 (C4, C5).
        // Mutable 2D head from the outfall side-table; -1e30 sentinel (no override)
        // when there is no outfall row; ramp 0 = dry (override suppressed).
        const int orow = ctx.node_subtypes.outfall_row(static_cast<int>(uj));
        double h_2d = (orow >= 0)
            ? ctx.node_subtypes.outfalls.head_2d[static_cast<std::size_t>(orow)]
            : -1.0e30;
        double ramp_2d = (orow >= 0)
            ? ctx.node_subtypes.outfalls.ramp_2d[static_cast<std::size_t>(orow)]
            : 0.0;
        if (h_2d > z_inv && ramp_2d > 0.0) {
            bool flap_closed = (ost.has_flap != 0)
                               && (h_2d > h_standard);
            if (!flap_closed) {
                // Blend free (h_standard) → tailwater (h_2d) by wetness.
                // ramp=1 reproduces the legacy max(h_standard, h_2d).
                double h_blend = h_standard + ramp_2d * std::max(0.0, h_2d - h_standard);
                if (h_blend > h_standard) {
                    h_standard = h_blend;
                    depth      = h_standard - z_inv;
                }
            }
        }

        // ----------------------------------------------------------------
        // C6: prescribed-HGL overlay (highest priority, last word).
        //
        // The forcing API may set nodes.outfall_param-equivalent stage
        // via forcing.node_head_boundary_value with ForcingMode::OVERRIDE.
        // Apply it here as a direct replacement of the depth, so that
        // (a) the legacy outfall_type is never mutated — "unfix" is just
        //     setting the mode back to NONE — and
        // (b) prescribed stage beats both the legacy logic AND the 2D
        //     override unambiguously, which is the user-facing contract.
        //
        // The applyForcings block in SWMMEngine.cpp no longer writes to
        // nodes.outfall_type / nodes.outfall_param; it only stages the
        // value into forcing.node_head_boundary_value, and this overlay
        // is the sole consumer.
        //
        // See docs/1D_2D_COUPLING_GATE_REVIEW.md §6 (C6).
        if (ctx.forcing.node_head_boundary_mode[uj] == ForcingMode::OVERRIDE) {
            double prescribed_stage = ctx.forcing.node_head_boundary_value[uj];
            depth = std::max(prescribed_stage - z_inv, 0.0);
        }

        nodes.depth[uj] = depth;
        nodes.head[uj] = nodes.invert_elev[uj] + depth;
    };

    // Phase 2 (relational refactor): iterate the dense outfall side-table
    // instead of scanning every node, turning this per-routing-step pass from
    // O(n_nodes) into O(n_outfalls). The computation body is unchanged, so
    // results stay bit-for-bit identical. Falls back to a full node scan when
    // the side-tables are not built (e.g. unit tests that skip hydraulics init).
    // See docs/relational/RELATIONAL_NODE_REFACTOR_PLAN.md (Phase 2).
    const auto& outs = ctx.node_subtypes.outfalls;
    if (static_cast<int>(ctx.node_subtypes.subtype_row.size()) == ctx.n_nodes()) {
        for (int r = 0; r < outs.count(); ++r)
            process_outfall(outs.node_idx[static_cast<std::size_t>(r)]);
    } else {
        for (int j = 0; j < ctx.n_nodes(); ++j) {
            if (nodes.type[static_cast<std::size_t>(j)] == NodeType::OUTFALL)
                process_outfall(j);
        }
    }
}

} // namespace outfall
} // namespace openswmm
