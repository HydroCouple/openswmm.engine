/**
 * @file PostParseResolver.cpp
 * @brief Post-parse cross-reference resolution.
 * @see PostParseResolver.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "PostParseResolver.hpp"
#include "../core/SimulationContext.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace openswmm::input {

void resolve_cross_references(SimulationContext& ctx) {
    // -------------------------------------------------------------------------
    // Subcatchment → gage linkage
    // -------------------------------------------------------------------------
    // Re-resolve any gage references that were -1 during SUBCATCHMENTS parsing
    // (can happen when RAINGAGES appears after SUBCATCHMENTS in the file).
    // We stored the gage name in the subcatch section; we need a separate
    // name lookup. But since we didn't store names, this pass just validates
    // that all gage indices are valid. The initial resolution handles
    // forward-references for in-order files (legacy SWMM requires order too).

    // -------------------------------------------------------------------------
    // Final counts → allocate SoA arrays to exact size
    // -------------------------------------------------------------------------
    // Up to this point the arrays have been grown incrementally with resize().
    // Trim/verify sizes are consistent.
    const int n_nodes    = ctx.node_names.size();
    const int n_links    = ctx.link_names.size();
    const int n_subcatch = ctx.subcatch_names.size();
    const int n_gages    = ctx.gage_names.size();
    const int n_polluts  = ctx.pollutant_names.size();

    // Ensure all SoA arrays match final counts (handles forward-declared objects)
    if (ctx.nodes.count() != n_nodes)         ctx.nodes.resize(n_nodes);
    if (ctx.links.count() != n_links)         ctx.links.resize(n_links);
    if (ctx.subcatches.count() != n_subcatch) ctx.subcatches.resize(n_subcatch);
    if (ctx.gages.count() != n_gages)         ctx.gages.resize(n_gages);

    // Allocate quality matrices now that all counts are final
    if (n_polluts > 0) {
        ctx.pollutants.resize_pollutants(n_polluts);
        ctx.pollutants.resize_quality(n_nodes, n_links, n_subcatch);
        ctx.nodes.resize_loads(n_polluts);
        ctx.links.resize_loads(n_polluts);
    }

    // Spatial coordinate arrays
    const auto un = static_cast<std::size_t>(n_nodes);
    if (ctx.spatial.node_x.size() < un) ctx.spatial.node_x.resize(un, 0.0);
    if (ctx.spatial.node_y.size() < un) ctx.spatial.node_y.resize(un, 0.0);

    const auto ul = static_cast<std::size_t>(n_links);
    if (ctx.spatial.link_x.size() < ul) ctx.spatial.link_x.resize(ul, 0.0);
    if (ctx.spatial.link_y.size() < ul) ctx.spatial.link_y.resize(ul, 0.0);

    const auto us = static_cast<std::size_t>(n_subcatch);
    if (ctx.spatial.subcatch_x.size() < us) ctx.spatial.subcatch_x.resize(us, 0.0);
    if (ctx.spatial.subcatch_y.size() < us) ctx.spatial.subcatch_y.resize(us, 0.0);

    // -------------------------------------------------------------------------
    // Timeseries date offset resolution
    // -------------------------------------------------------------------------
    // Timeseries without explicit dates have x-values starting near 0 (fractional
    // days from midnight). These are relative to the simulation start date.
    // Offset them by start_date so absolute Julian lookups work.
    for (std::size_t t = 0; t < ctx.tables.tables.size(); ++t) {
        auto& tbl = ctx.tables.tables[t];
        if (tbl.type != TableType::TIMESERIES) continue;
        if (tbl.x.empty()) continue;

        // If first x-value is small (< 366, i.e. less than one year in days),
        // it's a relative timeseries and needs the start_date offset.
        // Absolute dates (with MM/DD/YYYY) would produce values > 30000.
        if (tbl.x[0] < 366.0) {
            double offset = ctx.options.start_date;
            for (auto& xv : tbl.x) {
                xv += offset;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Gage timeseries re-resolution
    // -------------------------------------------------------------------------
    // If RAINGAGES section appeared before TIMESERIES, ts_index will be -1.
    // Re-resolve using the stored ts_name.
    for (int g = 0; g < n_gages; ++g) {
        auto ug = static_cast<std::size_t>(g);
        if (ctx.gages.ts_index[ug] < 0 &&
            ctx.gages.source[ug] == RainSource::TIMESERIES &&
            !ctx.gages.ts_name[ug].empty()) {
            ctx.gages.ts_index[ug] = ctx.table_names.find(ctx.gages.ts_name[ug]);
        }
    }

    // -------------------------------------------------------------------------
    // Subcatchment outlet re-resolution
    // -------------------------------------------------------------------------
    // If SUBCATCHMENTS parsed before JUNCTIONS/OUTFALLS, outlet_node will be -1.
    // Re-resolve using the stored outlet_name string.
    for (int s = 0; s < n_subcatch; ++s) {
        auto us = static_cast<std::size_t>(s);
        if (us >= ctx.subcatches.outlet_name.size()) continue;
        const auto& name = ctx.subcatches.outlet_name[us];
        if (name.empty()) continue;

        // Already resolved during parsing — validate it
        if (ctx.subcatches.outlet_node[us] >= 0 &&
            ctx.subcatches.outlet_node[us] < n_nodes) {
            continue;
        }
        if (ctx.subcatches.outlet_subcatch[us] >= 0 &&
            ctx.subcatches.outlet_subcatch[us] < n_subcatch) {
            continue;
        }

        // Try node first, then subcatchment
        int node_idx = ctx.node_names.find(name);
        if (node_idx >= 0) {
            ctx.subcatches.outlet_node[us] = node_idx;
            ctx.subcatches.outlet_subcatch[us] = -1;
        } else {
            int sub_idx = ctx.subcatch_names.find(name);
            if (sub_idx >= 0) {
                ctx.subcatches.outlet_subcatch[us] = sub_idx;
                ctx.subcatches.outlet_node[us] = -1;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Subcatchment gage re-resolution
    // -------------------------------------------------------------------------
    // If SUBCATCHMENTS parsed before RAINGAGES, gage indices may be -1.
    // The handler stored the name in the gage field via gage_names.find().
    // No name stored — just re-validate indices are in range.
    for (int s = 0; s < n_subcatch; ++s) {
        auto us = static_cast<std::size_t>(s);
        int gi = ctx.subcatches.gage[us];
        if (gi < 0 || gi >= n_gages) {
            ctx.subcatches.gage[us] = -1;
        }
    }

    // -------------------------------------------------------------------------
    // Storage curve name resolution
    // -------------------------------------------------------------------------
    for (int i = 0; i < n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (ctx.nodes.type[ui] == NodeType::STORAGE &&
            ctx.nodes.storage_curve[ui] < 0 &&
            !ctx.nodes.storage_curve_name[ui].empty()) {
            ctx.nodes.storage_curve[ui] = ctx.table_names.find(ctx.nodes.storage_curve_name[ui]);
        }
    }

    // -------------------------------------------------------------------------
    // Pump curve name resolution
    // -------------------------------------------------------------------------
    // pump_curve is stored as -1 during parsing if the pump section
    // appears before the curves section. Re-resolve here.
    // (Full implementation requires storing unresolved names in a side-table;
    //  for now, pumps parsed in order will have been resolved inline.)

    // -------------------------------------------------------------------------
    // Node init_depth → head initialisation
    // -------------------------------------------------------------------------
    for (int i = 0; i < n_nodes; ++i) {
        ctx.nodes.head[i]  = ctx.nodes.invert_elev[i] + ctx.nodes.init_depth[i];
        ctx.nodes.depth[i] = ctx.nodes.init_depth[i];
    }

    // -------------------------------------------------------------------------
    // External inflow timeseries resolution
    // -------------------------------------------------------------------------
    // Resolve timeseries name → table index for all external inflows
    for (int i = 0; i < ctx.ext_inflows.count(); ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (!ctx.ext_inflows.ts_name[ui].empty()) {
            int ts_idx = ctx.table_names.find(ctx.ext_inflows.ts_name[ui]);
            // Store resolved index - the inflow solver uses ts_name for lookup,
            // but we can cache the index for performance
            (void)ts_idx; // ts_name is used directly by InflowSolver
        }
        // Re-resolve node index if it was -1 during parsing
        if (ctx.ext_inflows.node_idx[ui] < 0) {
            // Can't resolve without stored node name — skip
        }
    }

    // -------------------------------------------------------------------------
    // Quality data matrix sizing
    // -------------------------------------------------------------------------
    // Resize buildup/washoff matrices now that landuse and pollutant counts are final
    const int n_landuses = ctx.landuse_names.size();
    if (n_landuses > 0 && n_polluts > 0) {
        if (ctx.buildup.n_landuses == 0) {
            ctx.buildup.resize(n_landuses, n_polluts);
        }
        if (ctx.washoff.n_landuses == 0) {
            ctx.washoff.resize(n_landuses, n_polluts);
        }
    }
    if (n_polluts > 0 && n_nodes > 0) {
        if (ctx.treatment.n_nodes == 0) {
            ctx.treatment.resize(n_nodes, n_polluts);
        }
    }

    // -------------------------------------------------------------------------
    // Subcatchment coverage matrix sizing
    // -------------------------------------------------------------------------
    if (n_landuses > 0 && n_subcatch > 0) {
        auto total = static_cast<std::size_t>(n_subcatch * n_landuses);
        if (ctx.subcatches.coverage.size() < total) {
            ctx.subcatches.coverage.resize(total, 0.0);
        }
        ctx.subcatches.coverage_n_landuses = n_landuses;
    }

    // -------------------------------------------------------------------------
    // Divider link and curve re-resolution
    // -------------------------------------------------------------------------
    for (int i = 0; i < n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (ctx.nodes.type[ui] != NodeType::DIVIDER) continue;

        // Re-resolve diversion link name → index
        if (ctx.nodes.divider_link[ui] < 0 &&
            !ctx.nodes.divider_link_name[ui].empty()) {
            ctx.nodes.divider_link[ui] = ctx.link_names.find(ctx.nodes.divider_link_name[ui]);
        }

        // Re-resolve diversion curve name → index (TABULAR dividers)
        if (ctx.nodes.divider_curve[ui] < 0 &&
            !ctx.nodes.divider_curve_name[ui].empty()) {
            ctx.nodes.divider_curve[ui] = ctx.table_names.find(ctx.nodes.divider_curve_name[ui]);
        }
    }

    // -------------------------------------------------------------------------
    // Node degree (connectivity) computation
    // -------------------------------------------------------------------------
    // Count the number of links connected to each node for downstream routing
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        int n1 = ctx.links.node1[uj];
        int n2 = ctx.links.node2[uj];
        if (n1 >= 0 && n1 < n_nodes)
            ctx.nodes.degree[static_cast<std::size_t>(n1)]++;
        if (n2 >= 0 && n2 < n_nodes)
            ctx.nodes.degree[static_cast<std::size_t>(n2)]++;
    }

    // -------------------------------------------------------------------------
    // Evaporation timeseries resolution
    // -------------------------------------------------------------------------
    if (ctx.options.evap_type == 2 && !ctx.options.evap_ts_name.empty()) {
        int ts_idx = ctx.table_names.find(ctx.options.evap_ts_name);
        (void)ts_idx; // stored by name, resolved at runtime
    }

    // -------------------------------------------------------------------------
    // Link cross-section derived properties
    // -------------------------------------------------------------------------
    // Constants matching legacy consts.h
    constexpr double PI      = 3.14159265358979;
    constexpr double GRAVITY = 32.2;
    constexpr double PHI     = 1.486;
    constexpr double MIN_DELTA_Z = 0.001;

    // Compute full-flow properties from cross-section geometry
    // (matches legacy xsect_setParams in xsect.c)
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

        double y_full = ctx.links.xsect_y_full[uj];
        double w_max  = ctx.links.xsect_w_max[uj];
        XsectShape shape = ctx.links.xsect_shape[uj];

        double a_full = 0.0, r_full = 0.0, s_full = 0.0, s_max = 0.0;
        double yw_max = 0.0;

        switch (shape) {
        case XsectShape::CIRCULAR:
            a_full = PI / 4.0 * y_full * y_full;
            r_full = 0.25 * y_full;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = 1.08 * s_full;
            w_max  = y_full;
            yw_max = 0.5 * y_full;
            break;

        case XsectShape::FORCE_MAIN:
            a_full = PI / 4.0 * y_full * y_full;
            r_full = 0.25 * y_full;
            s_full = a_full * std::pow(r_full, 0.63);  // H-W exponent
            s_max  = 1.06949 * s_full;
            w_max  = y_full;
            yw_max = 0.5 * y_full;
            // Store C-factor or roughness in s_bot
            ctx.links.xsect_s_bot[uj] = ctx.links.xsect_r_bot[uj];
            break;

        case XsectShape::FILLED_CIRCULAR:
            // Full values for unfilled pipe first
            a_full = PI / 4.0 * y_full * y_full;
            r_full = 0.25 * y_full;
            // yBot, aBot, etc. should be set from parsing
            // Adjust full values (simplified — full implementation needs
            // circ_getAofY for filled bottom area)
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = 1.08 * s_full;
            yw_max = 0.5 * y_full;
            break;

        case XsectShape::EGGSHAPED:
            a_full = 0.5105 * y_full * y_full;
            r_full = 0.1931 * y_full;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = 1.065 * s_full;
            w_max  = 2.0/3.0 * y_full;
            yw_max = 0.64 * y_full;
            break;

        case XsectShape::HORSESHOE:
            a_full = 0.8293 * y_full * y_full;
            r_full = 0.2538 * y_full;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = 1.077 * s_full;
            w_max  = 1.0 * y_full;
            yw_max = 0.5 * y_full;
            break;

        case XsectShape::GOTHIC:
            a_full = 0.6554 * y_full * y_full;
            r_full = 0.2269 * y_full;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = 1.065 * s_full;
            w_max  = 0.84 * y_full;
            yw_max = 0.45 * y_full;
            break;

        case XsectShape::CATENARY:
            a_full = 0.70277 * y_full * y_full;
            r_full = 0.23172 * y_full;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = 1.05 * s_full;
            w_max  = 0.9 * y_full;
            yw_max = 0.25 * y_full;
            break;

        case XsectShape::SEMIELLIPTICAL:
            a_full = 0.785 * y_full * y_full;
            r_full = 0.242 * y_full;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = 1.045 * s_full;
            w_max  = 1.0 * y_full;
            yw_max = 0.15 * y_full;
            break;

        case XsectShape::BASKETHANDLE:
            a_full = 0.7862 * y_full * y_full;
            r_full = 0.2464 * y_full;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = 1.06078 * s_full;
            w_max  = 0.944 * y_full;
            yw_max = 0.2 * y_full;
            break;

        case XsectShape::SEMICIRCULAR:
            a_full = 1.2697 * y_full * y_full;
            r_full = 0.2946 * y_full;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = 1.06637 * s_full;
            w_max  = 1.64 * y_full;
            yw_max = 0.15 * y_full;
            break;

        case XsectShape::RECT_CLOSED: {
            a_full = y_full * w_max;
            // Legacy: perimeter = 2*(yFull + wMax) — all 4 sides
            double p = 2.0 * (y_full + w_max);
            r_full = (p > 0.0) ? a_full / p : 0.0;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            // sMax at RECT_ALFMAX (0.97) of aFull
            constexpr double RECT_ALFMAX = 0.97;
            double a_max = RECT_ALFMAX * a_full;
            // Near-full R includes extra wetted perimeter from closing top
            double y_at_max = RECT_ALFMAX * y_full;
            double p_max = 2.0 * (y_at_max + w_max);
            double r_max = (p_max > 0.0) ? a_max / p_max : r_full;
            s_max = a_max * std::pow(r_max, 2.0/3.0);
            yw_max = y_full;
            break;
        }

        case XsectShape::RECT_OPEN: {
            double s_bot = ctx.links.xsect_s_bot[uj]; // # sides to ignore
            a_full = y_full * w_max;
            // Legacy: perimeter = (2 - sBot)*yFull + wMax
            double p = (2.0 - s_bot) * y_full + w_max;
            r_full = (p > 0.0) ? a_full / p : 0.0;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = s_full;  // open channel: sMax = sFull
            yw_max = y_full;
            break;
        }

        case XsectShape::TRAPEZOIDAL: {
            // At parse time: w_max = bottom width, xsect_y_bot = Geom3 (side slope1),
            //                xsect_r_bot = Geom4 (side slope2).
            // Legacy uses average of the two side slopes.
            double bot_w = w_max;
            double z1 = ctx.links.xsect_y_bot[uj];  // left side slope (H:V)
            double z2 = ctx.links.xsect_r_bot[uj];  // right side slope (H:V)
            double z = (z1 + z2) / 2.0;             // average side slope
            a_full = (bot_w + z * y_full) * y_full;
            double top_w = bot_w + 2.0 * z * y_full;
            double p = bot_w + 2.0 * y_full * std::sqrt(1.0 + z * z);
            r_full = (p > 0.0) ? a_full / p : 0.0;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = s_full;
            w_max  = top_w;
            yw_max = y_full;

            // Store batch-ready values for XSectGroups kernels:
            //   y_bot = bottom width, s_bot = average side slope
            ctx.links.xsect_y_bot[uj] = bot_w;
            ctx.links.xsect_s_bot[uj] = z;
            break;
        }

        case XsectShape::TRIANGULAR: {
            double z = ctx.links.xsect_y_bot[uj]; // side slope
            a_full = z * y_full * y_full;
            double top_w = 2.0 * z * y_full;
            double p = 2.0 * y_full * std::sqrt(1.0 + z * z);
            r_full = (p > 0.0) ? a_full / p : 0.0;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = s_full;
            w_max  = top_w;
            yw_max = y_full;
            break;
        }

        case XsectShape::PARABOLIC: {
            // w_max is top width at full depth
            a_full = 2.0/3.0 * w_max * y_full;
            double p = w_max + 8.0/3.0 * y_full * y_full / w_max; // approx
            r_full = (p > 0.0) ? a_full / p : 0.0;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = s_full;
            yw_max = y_full;
            break;
        }

        case XsectShape::MODBASKETHANDLE: {
            // Modified basket handle — bottom is semicircle of radius rBot
            double r_bot = ctx.links.xsect_r_bot[uj];
            a_full = PI/2.0 * r_bot * r_bot + w_max * (y_full - r_bot);
            double p = PI * r_bot + 2.0 * (y_full - r_bot) + w_max;
            r_full = (p > 0.0) ? a_full / p : 0.0;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = 1.06078 * s_full;
            yw_max = 0.2 * y_full;
            break;
        }

        case XsectShape::RECT_TRIANG: {
            double y_bot = ctx.links.xsect_y_bot[uj]; // triangle height
            double s_bot_slope = w_max / y_bot / 2.0;
            double r_bot_val = std::sqrt(1.0 + s_bot_slope * s_bot_slope);
            ctx.links.xsect_a_bot[uj] = y_bot * w_max / 2.0;
            ctx.links.xsect_s_bot[uj] = s_bot_slope;
            ctx.links.xsect_r_bot[uj] = r_bot_val;

            a_full = w_max * (y_full - y_bot / 2.0);
            double p = 2.0 * y_bot * r_bot_val + 2.0 * (y_full - y_bot) + w_max;
            r_full = (p > 0.0) ? a_full / p : 0.0;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            constexpr double RT_ALFMAX = 0.98;
            double a_max = RT_ALFMAX * a_full;
            s_max = a_max * std::pow(r_full, 2.0/3.0); // approx
            yw_max = y_full;
            break;
        }

        default:
            // For IRREGULAR, CUSTOM, POWER, RECT_ROUND — placeholder
            // These need shape table lookups (TODO: implement fully)
            a_full = w_max * y_full;
            double p_def = 2.0 * y_full + w_max;
            r_full = (p_def > 0.0) ? a_full / p_def : 0.0;
            s_full = a_full * std::pow(r_full, 2.0/3.0);
            s_max  = s_full;
            yw_max = y_full;
            break;
        }

        ctx.links.xsect_a_full[uj] = a_full;
        ctx.links.xsect_r_full[uj] = r_full;
        ctx.links.xsect_s_full[uj] = s_full;
        ctx.links.xsect_s_max[uj]  = s_max;
        ctx.links.xsect_w_max[uj]  = w_max;
        ctx.links.xsect_yw_max[uj] = yw_max;
    }

    // -------------------------------------------------------------------------
    // Conduit slope computation (matches legacy conduit_getSlope in link.c)
    // -------------------------------------------------------------------------
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

        int n1 = ctx.links.node1[uj];
        int n2 = ctx.links.node2[uj];
        if (n1 < 0 || n2 < 0) continue;

        double length = ctx.links.length[uj];
        if (length <= 0.0) continue;

        // Convert elevation offsets if ELEV_OFFSET mode
        if (ctx.options.link_offsets == 1) { // ELEV_OFFSET
            ctx.links.offset1[uj] =
                std::max(0.0, ctx.links.offset1[uj] - ctx.nodes.invert_elev[n1]);
            ctx.links.offset2[uj] =
                std::max(0.0, ctx.links.offset2[uj] - ctx.nodes.invert_elev[n2]);
        }

        double elev1 = ctx.links.offset1[uj] + ctx.nodes.invert_elev[n1];
        double elev2 = ctx.links.offset2[uj] + ctx.nodes.invert_elev[n2];
        double delta = std::fabs(elev1 - elev2);

        // Enforce minimum elevation change
        if (delta < MIN_DELTA_Z) {
            delta = MIN_DELTA_Z;
        }

        double slope;
        if (delta >= length) {
            slope = delta / length;
        } else {
            // slope = elev drop / horizontal distance
            slope = delta / std::sqrt(length * length - delta * delta);
        }

        // Apply minimum slope
        if (ctx.options.min_slope > 0.0 && slope < ctx.options.min_slope) {
            slope = ctx.options.min_slope;
        }

        // Negative slope for adverse gradient
        if (elev1 < elev2) slope = -slope;

        ctx.links.slope[uj] = slope;

        // Reverse conduit direction for adverse slope under DW routing
        if (ctx.options.routing_model == RoutingModel::DYNWAVE && slope < 0.0 &&
            ctx.links.xsect_a_full[uj] > 0.0) {
            // Swap nodes
            std::swap(ctx.links.node1[uj], ctx.links.node2[uj]);
            std::swap(ctx.links.offset1[uj], ctx.links.offset2[uj]);
            std::swap(ctx.links.loss_inlet[uj], ctx.links.loss_outlet[uj]);
            ctx.links.slope[uj] = -slope;
            ctx.links.direction[uj] *= -1;
            ctx.links.q0[uj] = -ctx.links.q0[uj];
        }
    }

    // -------------------------------------------------------------------------
    // Conduit flow properties (matches legacy conduit_validate in link.c)
    // -------------------------------------------------------------------------
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

        double n_val    = ctx.links.roughness[uj];
        double slope    = std::fabs(ctx.links.slope[uj]);
        double a_full   = ctx.links.xsect_a_full[uj];
        double s_full   = ctx.links.xsect_s_full[uj];
        double s_max    = ctx.links.xsect_s_max[uj];

        if (n_val <= 0.0 || a_full <= 0.0) continue;

        // Roughness factor for DW friction slope: GRAVITY * (n/PHI)^2
        ctx.links.rough_factor[uj] = GRAVITY * (n_val / PHI) * (n_val / PHI);

        // Conveyance factor: beta = PHI * sqrt(|slope|) / n
        double beta = PHI * std::sqrt(slope) / n_val;
        ctx.links.beta[uj] = beta;

        // Full-flow rate: q_full = sFull * beta
        ctx.links.q_full[uj] = s_full * beta;

        // Max flow at max section factor
        ctx.links.q_max[uj] = s_max * beta;

        // Conduit volume = A_full * modLength (or length if no lengthening)
        double mod_len = ctx.links.mod_length[uj];
        if (mod_len <= 0.0) mod_len = ctx.links.length[uj];
        ctx.links.mod_length[uj] = mod_len;
        ctx.links.volume[uj] = a_full * mod_len;
    }

    // -------------------------------------------------------------------------
    // Node fullDepth adjustment from connected link crowns
    // (matches legacy link_validate → node fullDepth adjustment)
    // -------------------------------------------------------------------------
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

        double y_full = ctx.links.xsect_y_full[uj];
        int n1 = ctx.links.node1[uj];
        int n2 = ctx.links.node2[uj];

        // Upstream node: crown = offset1 + y_full
        if (n1 >= 0 && n1 < n_nodes) {
            double crown = ctx.links.offset1[uj] + y_full;
            if (crown > ctx.nodes.full_depth[n1]) {
                ctx.nodes.full_depth[n1] = crown;
            }
        }
        // Downstream node: crown = offset2 + y_full
        if (n2 >= 0 && n2 < n_nodes) {
            double crown = ctx.links.offset2[uj] + y_full;
            if (crown > ctx.nodes.full_depth[n2]) {
                ctx.nodes.full_depth[n2] = crown;
            }
        }
    }
}

} /* namespace openswmm::input */
