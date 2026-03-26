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
#include <cmath>

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
    // Compute full-flow properties from cross-section geometry
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (ctx.links.type[uj] != LinkType::CONDUIT) continue;

        double y_full = ctx.links.xsect_y_full[uj];
        double w_max  = ctx.links.xsect_w_max[uj];
        XsectShape shape = ctx.links.xsect_shape[uj];

        // Compute full-flow area based on shape
        double a_full = 0.0;
        double r_full = 0.0;
        if (shape == XsectShape::CIRCULAR || shape == XsectShape::FILLED_CIRCULAR) {
            // CIRCULAR: A = pi*D^2/4, R = D/4
            a_full = 3.14159265358979 * y_full * y_full / 4.0;
            r_full = y_full / 4.0;
        } else if (shape == XsectShape::RECT_CLOSED || shape == XsectShape::RECT_OPEN) {
            // RECT: A = W*D
            a_full = w_max * y_full;
            double p = (shape == XsectShape::RECT_CLOSED)
                ? 2.0*y_full + w_max   // closed: wetted perimeter includes top
                : y_full + 2.0*w_max;  // open: no top
            r_full = (p > 0.0) ? a_full / p : 0.0;
        } else if (shape == XsectShape::TRAPEZOIDAL) {
            // TRAPEZOIDAL: w_max is bottom width, side slopes embedded
            a_full = w_max * y_full; // simplified approximation
            double p = 2.0 * y_full + w_max;
            r_full = (p > 0.0) ? a_full / p : 0.0;
        } else {
            // Default: treat as rectangular for other shapes
            a_full = w_max * y_full;
            double p = 2.0 * y_full + w_max;
            r_full = (p > 0.0) ? a_full / p : 0.0;
        }

        ctx.links.xsect_a_full[uj] = a_full;
        ctx.links.xsect_r_full[uj] = r_full;

        // Section factor: A*R^(2/3)
        if (r_full > 0.0)
            ctx.links.xsect_s_full[uj] = a_full * std::pow(r_full, 2.0/3.0);

        // Manning's full-flow: Q = (1.49/n) * A * R^(2/3) * S^(1/2)
        double n_val = ctx.links.roughness[uj];
        double slope = ctx.links.slope[uj];
        if (n_val > 0.0 && slope > 0.0) {
            ctx.links.q_full[uj] = (1.49 / n_val) * a_full *
                std::pow(r_full, 2.0/3.0) * std::sqrt(slope);
        }

        // Beta = (1.49/n) * S^(1/2) for conveyance
        if (n_val > 0.0 && slope > 0.0) {
            ctx.links.beta[uj] = (1.49 / n_val) * std::sqrt(slope);
        }

        // Conduit volume = A_full * Length
        ctx.links.volume[uj] = a_full * ctx.links.length[uj];
    }
}

} /* namespace openswmm::input */
