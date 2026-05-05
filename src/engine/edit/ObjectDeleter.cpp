/**
 * @file ObjectDeleter.cpp
 * @brief Implementation of object deletion and cascade analysis.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "ObjectDeleter.hpp"
#include "../data/LinkData.hpp"
#include "../../../include/openswmm/engine/openswmm_edit.h"

#include <algorithm>
#include <vector>

namespace openswmm::edit {

// ============================================================================
// Internal helpers
// ============================================================================

// Decrement all elements > deleted_idx, leaving -1 (sentinel) alone.
static void renumber_refs(std::vector<int>& refs, int deleted_idx) {
    for (auto& r : refs) {
        if (r > deleted_idx) --r;
    }
}

// Erase all spatial arrays for a node at idx (if they exist).
static void erase_node_spatial(SimulationContext& ctx, int idx) {
    const auto ui = static_cast<std::size_t>(idx);
    auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx)); };
    e(ctx.spatial.node_x);
    e(ctx.spatial.node_y);
    // Remove tag keyed by the node name (looked up before erase)
    const std::string& name = ctx.node_names.name_of(idx);
    ctx.node_tags.erase(name);
}

static void erase_link_spatial(SimulationContext& ctx, int idx) {
    const auto ui = static_cast<std::size_t>(idx);
    auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx)); };
    e(ctx.spatial.link_x);
    e(ctx.spatial.link_y);
    if (ui < ctx.spatial.link_vertices_x.size())
        ctx.spatial.link_vertices_x.erase(ctx.spatial.link_vertices_x.begin() + static_cast<std::ptrdiff_t>(idx));
    if (ui < ctx.spatial.link_vertices_y.size())
        ctx.spatial.link_vertices_y.erase(ctx.spatial.link_vertices_y.begin() + static_cast<std::ptrdiff_t>(idx));
    const std::string& name = ctx.link_names.name_of(idx);
    ctx.link_tags.erase(name);
}

static void erase_subcatch_spatial(SimulationContext& ctx, int idx) {
    const auto ui = static_cast<std::size_t>(idx);
    auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx)); };
    e(ctx.spatial.subcatch_x);
    e(ctx.spatial.subcatch_y);
    if (ui < ctx.spatial.subcatch_polygon_x.size())
        ctx.spatial.subcatch_polygon_x.erase(ctx.spatial.subcatch_polygon_x.begin() + static_cast<std::ptrdiff_t>(idx));
    if (ui < ctx.spatial.subcatch_polygon_y.size())
        ctx.spatial.subcatch_polygon_y.erase(ctx.spatial.subcatch_polygon_y.begin() + static_cast<std::ptrdiff_t>(idx));
    const std::string& name = ctx.subcatch_names.name_of(idx);
    ctx.subcatch_tags.erase(name);
}

static void erase_gage_spatial(SimulationContext& ctx, int idx) {
    const auto ui = static_cast<std::size_t>(idx);
    auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx)); };
    e(ctx.spatial.gage_x);
    e(ctx.spatial.gage_y);
}

// Erase per-subcatch pattern arrays at idx
static void erase_subcatch_patterns(SimulationContext& ctx, int idx) {
    auto e = [&](auto& v) {
        if (static_cast<std::size_t>(idx) < v.size())
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx));
    };
    e(ctx.subcatch_n_perv_pattern);
    e(ctx.subcatch_d_store_pattern);
    e(ctx.subcatch_infil_pattern);
    e(ctx.base_n_perv);
    e(ctx.base_ds_perv);
}

// Erase inlet_usage at idx (all parallel arrays in InletUsageStore)
static void erase_inlet_usage(SimulationContext& ctx, int idx) {
    auto& iu = ctx.inlet_usages;
    const auto ui = static_cast<std::size_t>(idx);
    auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx)); };
    e(iu.link_index); e(iu.design_index); e(iu.node_index); e(iu.num_inlets);
    e(iu.placement); e(iu.clog_factor); e(iu.flow_limit);
    e(iu.local_depress); e(iu.local_width); e(iu.street_index);
    e(iu.stat_capture_vol); e(iu.stat_bypass_vol);
    e(iu.stat_backflow_vol); e(iu.stat_peak_flow);
}

// ============================================================================
// ANALYZE — node
// ============================================================================

CascadeResult analyze_node_impact(const SimulationContext& ctx, int node_idx) {
    CascadeResult result;
    const int nl = ctx.n_links();
    for (int i = 0; i < nl; ++i) {
        if (ctx.links.node1[static_cast<std::size_t>(i)] == node_idx)
            result.add(SWMM_REF_LINK, i, "node1", true);
        else if (ctx.links.node2[static_cast<std::size_t>(i)] == node_idx)
            result.add(SWMM_REF_LINK, i, "node2", true);
    }
    const int nsc = ctx.n_subcatches();
    for (int i = 0; i < nsc; ++i) {
        if (ctx.subcatches.outlet_node[static_cast<std::size_t>(i)] == node_idx)
            result.add(SWMM_REF_SUBCATCH, i, "outlet_node", false);
    }
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        if (i == node_idx) continue;
        if (ctx.nodes.outfall_route_to[static_cast<std::size_t>(i)] == node_idx)
            result.add(SWMM_REF_NODE, i, "outfall_route_to", false);
    }
    const int niu = ctx.inlet_usages.count();
    for (int i = 0; i < niu; ++i) {
        if (ctx.inlet_usages.node_index[static_cast<std::size_t>(i)] == node_idx)
            result.add(SWMM_REF_INLET_USAGE, i, "node_index", false);
    }
    return result;
}

// ============================================================================
// ANALYZE — link
// ============================================================================

CascadeResult analyze_link_impact(const SimulationContext& ctx, int link_idx) {
    CascadeResult result;
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        if (ctx.nodes.divider_link[static_cast<std::size_t>(i)] == link_idx)
            result.add(SWMM_REF_NODE, i, "divider_link", false);
    }
    const int niu = ctx.inlet_usages.count();
    for (int i = 0; i < niu; ++i) {
        if (ctx.inlet_usages.link_index[static_cast<std::size_t>(i)] == link_idx)
            result.add(SWMM_REF_INLET_USAGE, i, "link_index", true);
    }
    return result;
}

// ============================================================================
// ANALYZE — subcatch
// ============================================================================

CascadeResult analyze_subcatch_impact(const SimulationContext& ctx, int sc_idx) {
    CascadeResult result;
    const int nsc = ctx.n_subcatches();
    for (int i = 0; i < nsc; ++i) {
        if (i == sc_idx) continue;
        if (ctx.subcatches.outlet_subcatch[static_cast<std::size_t>(i)] == sc_idx)
            result.add(SWMM_REF_SUBCATCH, i, "outlet_subcatch", false);
    }
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        if (ctx.nodes.outfall_route_to[static_cast<std::size_t>(i)] == sc_idx)
            result.add(SWMM_REF_NODE, i, "outfall_route_to", false);
    }
    return result;
}

// ============================================================================
// ANALYZE — gage
// ============================================================================

CascadeResult analyze_gage_impact(const SimulationContext& ctx, int gage_idx) {
    CascadeResult result;
    const int nsc = ctx.n_subcatches();
    for (int i = 0; i < nsc; ++i) {
        if (ctx.subcatches.gage[static_cast<std::size_t>(i)] == gage_idx)
            result.add(SWMM_REF_SUBCATCH, i, "gage", false);
    }
    return result;
}

// ============================================================================
// ANALYZE — table
// ============================================================================

CascadeResult analyze_table_impact(const SimulationContext& ctx, int table_idx) {
    CascadeResult result;

    // Rain gages referencing this table as ts_index
    const int ng = ctx.n_gages();
    for (int i = 0; i < ng; ++i) {
        if (ctx.gages.ts_index[static_cast<std::size_t>(i)] == table_idx)
            result.add(SWMM_REF_GAGE, i, "ts_index", false);
    }

    // Nodes
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.nodes.storage_curve[ui] == table_idx)
            result.add(SWMM_REF_NODE, i, "storage_curve", false);
        if (ctx.nodes.divider_curve[ui] == table_idx)
            result.add(SWMM_REF_NODE, i, "divider_curve", false);
        // outfall_param encodes table index as double when type is TIDAL or TIMESERIES
        if (ctx.nodes.type[ui] == NodeType::OUTFALL &&
            (ctx.nodes.outfall_type[ui] == OutfallType::TIDAL ||
             ctx.nodes.outfall_type[ui] == OutfallType::TIMESERIES) &&
            static_cast<int>(ctx.nodes.outfall_param[ui]) == table_idx)
            result.add(SWMM_REF_NODE, i, "outfall_param", false);
    }

    // Links
    const int nl = ctx.n_links();
    for (int i = 0; i < nl; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.links.pump_curve[ui] == table_idx)
            result.add(SWMM_REF_LINK, i, "pump_curve", false);
        if (ctx.links.xsect_curve[ui] == table_idx)
            result.add(SWMM_REF_LINK, i, "xsect_curve", false);
    }

    return result;
}

// ============================================================================
// ANALYZE — transect
// ============================================================================

CascadeResult analyze_transect_impact(const SimulationContext& ctx, int transect_idx) {
    CascadeResult result;
    const int nl = ctx.n_links();
    for (int i = 0; i < nl; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if ((ctx.links.xsect_shape[ui] == XsectShape::IRREGULAR ||
             ctx.links.xsect_shape[ui] == XsectShape::CUSTOM) &&
            ctx.links.xsect_curve[ui] == transect_idx)
            result.add(SWMM_REF_LINK, i, "xsect_curve[transect]", false);
    }
    return result;
}

// ============================================================================
// DELETE — link (forward-declared for use by delete_node)
// ============================================================================

CascadeResult delete_link(SimulationContext& ctx, int link_idx);

// ============================================================================
// DELETE — node
// ============================================================================

CascadeResult delete_node(SimulationContext& ctx, int node_idx) {
    CascadeResult result;

    // --- Step 1: cascade-delete all links that touch this node ---
    // Collect in descending order so erasure doesn't invalidate earlier indices.
    std::vector<int> links_to_delete;
    const int nl = ctx.n_links();
    for (int i = 0; i < nl; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.links.node1[ui] == node_idx || ctx.links.node2[ui] == node_idx)
            links_to_delete.push_back(i);
    }
    std::sort(links_to_delete.rbegin(), links_to_delete.rend());
    for (int li : links_to_delete) {
        // Adjust li if it was affected by earlier deletions in this loop
        CascadeResult lr = delete_link(ctx, li);
        result.add(SWMM_REF_LINK, li, "node1/node2", true);
        for (auto& e : lr.entries) result.entries.push_back(e);
    }

    // --- Step 2: nullify subcatch outlet_node references ---
    const int nsc = ctx.n_subcatches();
    for (int i = 0; i < nsc; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.subcatches.outlet_node[ui] == node_idx) {
            ctx.subcatches.outlet_node[ui] = -1;
            result.add(SWMM_REF_SUBCATCH, i, "outlet_node", false);
        }
    }

    // --- Step 3: nullify outfall_route_to on other nodes ---
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        if (i == node_idx) continue;
        if (ctx.nodes.outfall_route_to[static_cast<std::size_t>(i)] == node_idx) {
            ctx.nodes.outfall_route_to[static_cast<std::size_t>(i)] = -1;
            result.add(SWMM_REF_NODE, i, "outfall_route_to", false);
        }
    }

    // --- Step 4: nullify inlet_usage node_index references ---
    const int niu = ctx.inlet_usages.count();
    for (int i = 0; i < niu; ++i) {
        if (ctx.inlet_usages.node_index[static_cast<std::size_t>(i)] == node_idx) {
            ctx.inlet_usages.node_index[static_cast<std::size_t>(i)] = -1;
            result.add(SWMM_REF_INLET_USAGE, i, "node_index", false);
        }
    }

    // --- Step 5: erase SoA row ---
    erase_node_spatial(ctx, node_idx);
    ctx.node_names.remove_at(node_idx);
    ctx.nodes.erase_at(node_idx);

    // --- Step 6: renumber cross-references for nodes with index > node_idx ---
    renumber_refs(ctx.links.node1, node_idx);
    renumber_refs(ctx.links.node2, node_idx);
    renumber_refs(ctx.subcatches.outlet_node, node_idx);
    renumber_refs(ctx.nodes.outfall_route_to, node_idx);
    renumber_refs(ctx.inlet_usages.node_index, node_idx);
    // gw_node in subcatches also references nodes
    renumber_refs(ctx.subcatches.gw_node, node_idx);

    return result;
}

// ============================================================================
// DELETE — link
// ============================================================================

CascadeResult delete_link(SimulationContext& ctx, int link_idx) {
    CascadeResult result;

    // --- Step 1: nullify divider_link on nodes ---
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        if (ctx.nodes.divider_link[static_cast<std::size_t>(i)] == link_idx) {
            ctx.nodes.divider_link[static_cast<std::size_t>(i)] = -1;
            result.add(SWMM_REF_NODE, i, "divider_link", false);
        }
    }

    // --- Step 2: delete inlet_usages that reference this link ---
    // Collect in descending order to avoid index shift during erase
    std::vector<int> usages_to_delete;
    const int niu = ctx.inlet_usages.count();
    for (int i = 0; i < niu; ++i) {
        if (ctx.inlet_usages.link_index[static_cast<std::size_t>(i)] == link_idx)
            usages_to_delete.push_back(i);
    }
    std::sort(usages_to_delete.rbegin(), usages_to_delete.rend());
    for (int ui : usages_to_delete) {
        result.add(SWMM_REF_INLET_USAGE, ui, "link_index", true);
        erase_inlet_usage(ctx, ui);
    }

    // --- Step 3: erase SoA row ---
    erase_link_spatial(ctx, link_idx);
    ctx.link_names.remove_at(link_idx);
    ctx.links.erase_at(link_idx);

    // --- Step 4: renumber cross-references ---
    renumber_refs(ctx.nodes.divider_link, link_idx);
    renumber_refs(ctx.inlet_usages.link_index, link_idx);
    // outfall_link_idx is a cached value rebuilt at init; nullify for safety
    for (auto& v : ctx.nodes.outfall_link_idx) {
        if (v == link_idx) v = -1;
        else if (v > link_idx) --v;
    }

    return result;
}

// ============================================================================
// DELETE — subcatchment
// ============================================================================

CascadeResult delete_subcatch(SimulationContext& ctx, int sc_idx) {
    CascadeResult result;

    // --- Step 1: nullify outlet_subcatch references ---
    const int nsc = ctx.n_subcatches();
    for (int i = 0; i < nsc; ++i) {
        if (i == sc_idx) continue;
        if (ctx.subcatches.outlet_subcatch[static_cast<std::size_t>(i)] == sc_idx) {
            ctx.subcatches.outlet_subcatch[static_cast<std::size_t>(i)] = -1;
            result.add(SWMM_REF_SUBCATCH, i, "outlet_subcatch", false);
        }
    }

    // --- Step 2: nullify outfall_route_to on nodes ---
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        if (ctx.nodes.outfall_route_to[static_cast<std::size_t>(i)] == sc_idx) {
            ctx.nodes.outfall_route_to[static_cast<std::size_t>(i)] = -1;
            result.add(SWMM_REF_NODE, i, "outfall_route_to", false);
        }
    }

    // --- Step 3: erase SoA row ---
    erase_subcatch_spatial(ctx, sc_idx);
    erase_subcatch_patterns(ctx, sc_idx);
    ctx.subcatch_names.remove_at(sc_idx);
    ctx.subcatches.erase_at(sc_idx);

    // --- Step 4: renumber ---
    renumber_refs(ctx.subcatches.outlet_subcatch, sc_idx);
    renumber_refs(ctx.nodes.outfall_route_to, sc_idx);

    return result;
}

// ============================================================================
// DELETE — gage
// ============================================================================

CascadeResult delete_gage(SimulationContext& ctx, int gage_idx) {
    CascadeResult result;

    // --- Step 1: nullify subcatch gage references ---
    const int nsc = ctx.n_subcatches();
    for (int i = 0; i < nsc; ++i) {
        if (ctx.subcatches.gage[static_cast<std::size_t>(i)] == gage_idx) {
            ctx.subcatches.gage[static_cast<std::size_t>(i)] = -1;
            result.add(SWMM_REF_SUBCATCH, i, "gage", false);
        }
    }

    // --- Step 2: erase SoA row ---
    erase_gage_spatial(ctx, gage_idx);
    ctx.gage_names.remove_at(gage_idx);
    ctx.gages.erase_at(gage_idx);

    // --- Step 3: renumber ---
    renumber_refs(ctx.subcatches.gage, gage_idx);

    return result;
}

// ============================================================================
// DELETE — table/curve
// ============================================================================

CascadeResult delete_table(SimulationContext& ctx, int table_idx) {
    CascadeResult result;

    // --- Step 1: nullify all cross-references ---
    const int ng = ctx.n_gages();
    for (int i = 0; i < ng; ++i) {
        if (ctx.gages.ts_index[static_cast<std::size_t>(i)] == table_idx) {
            ctx.gages.ts_index[static_cast<std::size_t>(i)] = -1;
            result.add(SWMM_REF_GAGE, i, "ts_index", false);
        }
    }

    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.nodes.storage_curve[ui] == table_idx) {
            ctx.nodes.storage_curve[ui] = -1;
            result.add(SWMM_REF_NODE, i, "storage_curve", false);
        }
        if (ctx.nodes.divider_curve[ui] == table_idx) {
            ctx.nodes.divider_curve[ui] = -1;
            result.add(SWMM_REF_NODE, i, "divider_curve", false);
        }
        if (ctx.nodes.type[ui] == NodeType::OUTFALL &&
            (ctx.nodes.outfall_type[ui] == OutfallType::TIDAL ||
             ctx.nodes.outfall_type[ui] == OutfallType::TIMESERIES) &&
            static_cast<int>(ctx.nodes.outfall_param[ui]) == table_idx) {
            ctx.nodes.outfall_param[ui] = 0.0;
            result.add(SWMM_REF_NODE, i, "outfall_param", false);
        }
    }

    const int nl = ctx.n_links();
    for (int i = 0; i < nl; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.links.pump_curve[ui] == table_idx) {
            ctx.links.pump_curve[ui] = -1;
            result.add(SWMM_REF_LINK, i, "pump_curve", false);
        }
        if (ctx.links.xsect_curve[ui] == table_idx) {
            ctx.links.xsect_curve[ui] = -1;
            result.add(SWMM_REF_LINK, i, "xsect_curve", false);
        }
    }

    // --- Step 2: erase the table entry ---
    ctx.tables.tables.erase(ctx.tables.tables.begin() + static_cast<std::ptrdiff_t>(table_idx));
    ctx.table_names.remove_at(table_idx);

    // --- Step 3: renumber cross-references for indices > table_idx ---
    renumber_refs(ctx.gages.ts_index, table_idx);
    renumber_refs(ctx.nodes.storage_curve, table_idx);
    renumber_refs(ctx.nodes.divider_curve, table_idx);
    renumber_refs(ctx.links.pump_curve, table_idx);
    renumber_refs(ctx.links.xsect_curve, table_idx);

    // outfall_param stores table index as double for TIDAL/TIMESERIES — renumber
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.nodes.type[ui] == NodeType::OUTFALL &&
            (ctx.nodes.outfall_type[ui] == OutfallType::TIDAL ||
             ctx.nodes.outfall_type[ui] == OutfallType::TIMESERIES)) {
            int ref = static_cast<int>(ctx.nodes.outfall_param[ui]);
            if (ref > table_idx) ctx.nodes.outfall_param[ui] = static_cast<double>(ref - 1);
        }
    }

    return result;
}

// ============================================================================
// DELETE — transect
// ============================================================================

CascadeResult delete_transect(SimulationContext& ctx, int transect_idx) {
    CascadeResult result;

    // --- Step 1: nullify link xsect_curve references for IRREGULAR/CUSTOM shapes ---
    const int nl = ctx.n_links();
    for (int i = 0; i < nl; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if ((ctx.links.xsect_shape[ui] == XsectShape::IRREGULAR ||
             ctx.links.xsect_shape[ui] == XsectShape::CUSTOM) &&
            ctx.links.xsect_curve[ui] == transect_idx) {
            ctx.links.xsect_curve[ui] = -1;
            ctx.links.xsect_shape[ui] = XsectShape::CIRCULAR;
            result.add(SWMM_REF_LINK, i, "xsect_curve[transect]", false);
        }
    }

    // --- Step 2: erase from TransectStore ---
    const auto ui = static_cast<std::size_t>(transect_idx);
    auto erase_ts = [&](auto& v) {
        if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(transect_idx));
    };
    erase_ts(ctx.transects.names);
    erase_ts(ctx.transects.n_left);
    erase_ts(ctx.transects.n_right);
    erase_ts(ctx.transects.n_channel);
    erase_ts(ctx.transects.x_left_bank);
    erase_ts(ctx.transects.x_right_bank);
    erase_ts(ctx.transects.x_factor);
    erase_ts(ctx.transects.y_factor);
    erase_ts(ctx.transects.stations);
    erase_ts(ctx.transects.elevations);

    // Also erase from the built transect_tables (if populated)
    if (ui < ctx.transect_tables.size())
        ctx.transect_tables.erase(ctx.transect_tables.begin() + static_cast<std::ptrdiff_t>(transect_idx));

    // --- Step 3: renumber xsect_curve for IRREGULAR/CUSTOM links with ref > transect_idx ---
    for (int i = 0; i < nl; ++i) {
        const auto li = static_cast<std::size_t>(i);
        if ((ctx.links.xsect_shape[li] == XsectShape::IRREGULAR ||
             ctx.links.xsect_shape[li] == XsectShape::CUSTOM) &&
            ctx.links.xsect_curve[li] > transect_idx) {
            --ctx.links.xsect_curve[li];
        }
    }

    return result;
}

} // namespace openswmm::edit
