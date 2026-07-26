/**
 * @file ObjectDeleter.cpp
 * @brief Implementation of object deletion and cascade analysis.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "ObjectDeleter.hpp"
#include "../data/LinkData.hpp"
#include "../../../include/openswmm/engine/openswmm_edit.h"

#include <algorithm>
#include <cctype>
#include <sstream>
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
// Tags are erased by NodeData::erase_at via the index-shift path;
// no name-keyed map cleanup needed.
static void erase_node_spatial(SimulationContext& ctx, int idx) {
    const auto ui = static_cast<std::size_t>(idx);
    auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx)); };
    e(ctx.spatial.node_x);
    e(ctx.spatial.node_y);
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

// Erase lid_usage at idx (all parallel arrays in LidUsageStore)
static void erase_lid_usage(SimulationContext& ctx, int idx) {
    auto& lu = ctx.lid_usage;
    const auto ui = static_cast<std::size_t>(idx);
    auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx)); };
    e(lu.subcatch_index); e(lu.lid_index); e(lu.number);
    e(lu.area); e(lu.width); e(lu.init_sat);
    e(lu.from_imperv); e(lu.to_perv); e(lu.rpt_file);
    e(lu.drain_to); e(lu.from_perv);
    e(lu.wb_inflow); e(lu.wb_evap); e(lu.wb_infil);
    e(lu.wb_surf_flow); e(lu.wb_drain_flow);
    e(lu.wb_init_vol); e(lu.wb_final_vol);
}

// True when the treatment store is sized consistently for stripe erasure.
static bool treatment_stripe_valid(const TreatmentData& T, int node_idx) {
    return T.n_pollutants > 0 && node_idx < T.n_nodes &&
           T.expressions.size() == static_cast<std::size_t>(T.n_nodes) *
                                   static_cast<std::size_t>(T.n_pollutants);
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
// Control-rule reference scan
// ============================================================================

static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::vector<int> find_control_rule_refs(const SimulationContext& ctx,
                                        const std::string& object_name) {
    static const char* kObjectKeywords[] = {
        "NODE", "LINK", "CONDUIT", "PUMP", "ORIFICE", "WEIR", "OUTLET"};

    std::vector<int> hits;
    if (object_name.empty()) return hits;

    for (int r = 0; r < ctx.control_rules.count(); ++r) {
        const std::string& text = ctx.control_rules.rule_text[static_cast<std::size_t>(r)];
        std::istringstream is(text);
        std::string tok, prev;
        bool found = false;
        while (!found && is >> tok) {
            if (!prev.empty()) {
                for (const char* kw : kObjectKeywords) {
                    if (iequals(prev, kw) && iequals(tok, object_name)) {
                        found = true;
                        break;
                    }
                }
            }
            prev = tok;
        }
        if (found) hits.push_back(r);
    }
    return hits;
}

// Append the rule hits for @p name to a cascade result as
// SWMM_REF_CONTROL_RULE entries (cascaded=0 — rules are never edited).
static void add_rule_refs(const SimulationContext& ctx, const std::string& name,
                          CascadeResult& result) {
    for (int r : find_control_rule_refs(ctx, name))
        result.add(SWMM_REF_CONTROL_RULE, r, "rule_text", false);
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
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.subcatches.outlet_node[ui] == node_idx)
            result.add(SWMM_REF_SUBCATCH, i, "outlet_node", false);
        if (ui < ctx.subcatches.gw_node.size() && ctx.subcatches.gw_node[ui] == node_idx)
            result.add(SWMM_REF_SUBCATCH, i, "gw_node", false);
    }
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        if (i == node_idx) continue;
        const int r = ctx.node_subtypes.outfall_row(i);
        if (r >= 0 && ctx.node_subtypes.outfalls.route_to[static_cast<std::size_t>(r)] == node_idx)
            result.add(SWMM_REF_NODE, i, "outfall_route_to", false);
    }
    const int niu = ctx.inlet_usages.count();
    for (int i = 0; i < niu; ++i) {
        if (ctx.inlet_usages.node_index[static_cast<std::size_t>(i)] == node_idx)
            result.add(SWMM_REF_INLET_USAGE, i, "node_index", false);
    }
    for (int i = ctx.ext_inflows.count() - 1; i >= 0; --i)
        if (ctx.ext_inflows.node_idx[static_cast<std::size_t>(i)] == node_idx)
            result.add(SWMM_REF_EXT_INFLOW, i, "node_idx", true);
    for (int i = ctx.dwf_inflows.count() - 1; i >= 0; --i)
        if (ctx.dwf_inflows.node_idx[static_cast<std::size_t>(i)] == node_idx)
            result.add(SWMM_REF_DWF_INFLOW, i, "node_idx", true);
    for (int i = ctx.rdii_assigns.count() - 1; i >= 0; --i)
        if (ctx.rdii_assigns.node_idx[static_cast<std::size_t>(i)] == node_idx)
            result.add(SWMM_REF_RDII_ASSIGN, i, "node_idx", true);
    if (treatment_stripe_valid(ctx.treatment, node_idx)) {
        const int np = ctx.treatment.n_pollutants;
        const auto first = static_cast<std::size_t>(node_idx) * static_cast<std::size_t>(np);
        for (int p = 0; p < np; ++p)
            if (!ctx.treatment.expressions[first + static_cast<std::size_t>(p)].empty())
                result.add(SWMM_REF_TREATMENT, p, "expression", true);
    }
    add_rule_refs(ctx, ctx.node_names.name_of(node_idx), result);
    return result;
}

// ============================================================================
// ANALYZE — link
// ============================================================================

CascadeResult analyze_link_impact(const SimulationContext& ctx, int link_idx) {
    CascadeResult result;
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        const int r = ctx.node_subtypes.divider_row(i);
        if (r >= 0 && ctx.node_subtypes.dividers.link[static_cast<std::size_t>(r)] == link_idx)
            result.add(SWMM_REF_NODE, i, "divider_link", false);
    }
    const int niu = ctx.inlet_usages.count();
    for (int i = 0; i < niu; ++i) {
        if (ctx.inlet_usages.link_index[static_cast<std::size_t>(i)] == link_idx)
            result.add(SWMM_REF_INLET_USAGE, i, "link_index", true);
    }
    add_rule_refs(ctx, ctx.link_names.name_of(link_idx), result);
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
        const int r = ctx.node_subtypes.outfall_row(i);
        if (r >= 0 && ctx.node_subtypes.outfalls.route_to[static_cast<std::size_t>(r)] == sc_idx)
            result.add(SWMM_REF_NODE, i, "outfall_route_to", false);
    }
    for (int i = ctx.lid_usage.count() - 1; i >= 0; --i)
        if (ctx.lid_usage.subcatch_index[static_cast<std::size_t>(i)] == sc_idx)
            result.add(SWMM_REF_LID_USAGE, i, "subcatch_index", true);
    const std::string& sc_name = ctx.subcatch_names.name_of(sc_idx);
    if (!sc_name.empty()) {
        for (int i = 0; i < ctx.lid_usage.count(); ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (ctx.lid_usage.subcatch_index[ui] != sc_idx &&
                ui < ctx.lid_usage.drain_to.size() &&
                ctx.lid_usage.drain_to[ui] == sc_name)
                result.add(SWMM_REF_LID_USAGE, i, "drain_to", false);
        }
        for (int i = 0; i < ctx.snowpacks.count(); ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (ui < ctx.snowpacks.removal_subcatch.size() &&
                ctx.snowpacks.removal_subcatch[ui] == sc_name)
                result.add(SWMM_REF_SNOWPACK, i, "removal_subcatch", false);
        }
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
    const std::string& gage_name = ctx.gage_names.name_of(gage_idx);
    if (!gage_name.empty()) {
        for (std::size_t i = 0; i < ctx.unit_hyds.gage_names.size(); ++i)
            if (ctx.unit_hyds.gage_names[i] == gage_name)
                result.add(SWMM_REF_HYDROGRAPH, static_cast<int>(i), "gage_name", false);
        for (std::size_t i = 0; i < ctx.unit_hyds.entries.size(); ++i)
            if (ctx.unit_hyds.entries[i].gage_name == gage_name)
                result.add(SWMM_REF_HYDROGRAPH, static_cast<int>(i), "uh_gage_name", false);
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

    // External-inflow rows referencing this timeseries by name
    {
        const std::string& t_name = ctx.table_names.name_of(table_idx);
        if (!t_name.empty()) {
            for (int i = 0; i < ctx.ext_inflows.count(); ++i)
                if (ctx.ext_inflows.ts_name[static_cast<std::size_t>(i)] == t_name)
                    result.add(SWMM_REF_EXT_INFLOW, i, "ts_name", false);
        }
    }

    // Nodes — dispatch by type so cascade entries stay in ascending-node order.
    const auto& subs = ctx.node_subtypes;
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        switch (ctx.nodes.type[static_cast<std::size_t>(i)]) {
            case NodeType::STORAGE: {
                const int r = subs.storage_row(i);
                if (r >= 0 && subs.storages.curve[static_cast<std::size_t>(r)] == table_idx)
                    result.add(SWMM_REF_NODE, i, "storage_curve", false);
                break;
            }
            case NodeType::DIVIDER: {
                const int r = subs.divider_row(i);
                if (r >= 0 && subs.dividers.curve[static_cast<std::size_t>(r)] == table_idx)
                    result.add(SWMM_REF_NODE, i, "divider_curve", false);
                break;
            }
            case NodeType::OUTFALL: {
                // outfall param encodes table index as double for TIDAL/TIMESERIES
                const int r = subs.outfall_row(i);
                if (r >= 0) {
                    const auto ur = static_cast<std::size_t>(r);
                    if ((subs.outfalls.bc_type[ur] == OutfallType::TIDAL ||
                         subs.outfalls.bc_type[ur] == OutfallType::TIMESERIES) &&
                        static_cast<int>(subs.outfalls.param[ur]) == table_idx)
                        result.add(SWMM_REF_NODE, i, "outfall_param", false);
                }
                break;
            }
            default: break;
        }
    }

    // Links — pump curve (PumpData) and outlet TABULAR rating curve (OutletData).
    {
        const auto& PD = ctx.link_subtypes.pumps;
        for (int r = 0; r < PD.count(); ++r)
            if (PD.curve[static_cast<std::size_t>(r)] == table_idx)
                result.add(SWMM_REF_LINK, PD.link_idx[static_cast<std::size_t>(r)], "pump_curve", false);
        const auto& OUT = ctx.link_subtypes.outlets;
        for (int r = 0; r < OUT.count(); ++r)
            if (OUT.curve[static_cast<std::size_t>(r)] == table_idx)
                result.add(SWMM_REF_LINK, OUT.link_idx[static_cast<std::size_t>(r)], "pump_curve", false);
    }
    const int nl = ctx.n_links();
    for (int i = 0; i < nl; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.links.xsect_curve[ui] == table_idx)
            result.add(SWMM_REF_LINK, i, "xsect_curve", false);
    }

    // Subcatchment adjustment patterns index table_names (see InpWriter tN)
    auto scan_adj = [&](const std::vector<int>& v, const char* field) {
        for (std::size_t i = 0; i < v.size(); ++i)
            if (v[i] == table_idx)
                result.add(SWMM_REF_SUBCATCH, static_cast<int>(i), field, false);
    };
    scan_adj(ctx.subcatch_n_perv_pattern,  "n_perv_pattern");
    scan_adj(ctx.subcatch_d_store_pattern, "d_store_pattern");
    scan_adj(ctx.subcatch_infil_pattern,   "infil_pattern");

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

    // --- Step 0: report (never edit) control rules referencing this node ---
    add_rule_refs(ctx, ctx.node_names.name_of(node_idx), result);

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
        // Record which end referenced the node (matches analyze_node_impact's
        // field literals) before delete_link mutates the arrays.
        const char* end_field =
            ctx.links.node1[static_cast<std::size_t>(li)] == node_idx ? "node1" : "node2";
        CascadeResult lr = delete_link(ctx, li);
        result.add(SWMM_REF_LINK, li, end_field, true);
        for (auto& e : lr.entries) result.entries.push_back(e);
    }

    // --- Step 2: nullify subcatch outlet_node / gw_node references ---
    const int nsc = ctx.n_subcatches();
    for (int i = 0; i < nsc; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.subcatches.outlet_node[ui] == node_idx) {
            ctx.subcatches.outlet_node[ui] = -1;
            result.add(SWMM_REF_SUBCATCH, i, "outlet_node", false);
        }
        if (ui < ctx.subcatches.gw_node.size() && ctx.subcatches.gw_node[ui] == node_idx) {
            ctx.subcatches.gw_node[ui] = -1;
            result.add(SWMM_REF_SUBCATCH, i, "gw_node", false);
        }
    }

    // --- Step 3: nullify outfall_route_to on other nodes ---
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        if (i == node_idx) continue;
        const int r = ctx.node_subtypes.outfall_row(i);
        if (r >= 0 && ctx.node_subtypes.outfalls.route_to[static_cast<std::size_t>(r)] == node_idx) {
            ctx.node_subtypes.outfalls.route_to[static_cast<std::size_t>(r)] = -1;
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

    // --- Step 4b: cascade-delete ext-inflow / DWF / RDII rows for this node ---
    // Descending so erasure doesn't shift the indices still to be visited,
    // and reported indices are the rows' original positions.
    auto erase_node_rows = [&](auto& store, int ref_type) {
        for (int i = store.count() - 1; i >= 0; --i) {
            if (store.node_idx[static_cast<std::size_t>(i)] == node_idx) {
                result.add(ref_type, i, "node_idx", true);
                store.erase(i);
            }
        }
    };
    erase_node_rows(ctx.ext_inflows,  SWMM_REF_EXT_INFLOW);
    erase_node_rows(ctx.dwf_inflows,  SWMM_REF_DWF_INFLOW);
    erase_node_rows(ctx.rdii_assigns, SWMM_REF_RDII_ASSIGN);

    // --- Step 4c: erase this node's treatment stripe ---
    // The expression array is positional [node * n_pollutants + poll]; leaving
    // the stripe in place would silently misalign every node after node_idx.
    if (treatment_stripe_valid(ctx.treatment, node_idx)) {
        auto& T = ctx.treatment;
        const int np = T.n_pollutants;
        const auto first = static_cast<std::ptrdiff_t>(node_idx) * np;
        for (int p = 0; p < np; ++p)
            if (!T.expressions[static_cast<std::size_t>(first + p)].empty())
                result.add(SWMM_REF_TREATMENT, p, "expression", true);
        T.expressions.erase(T.expressions.begin() + first,
                            T.expressions.begin() + first + np);
        if (T.compiled.size() == static_cast<std::size_t>(T.n_nodes) *
                                 static_cast<std::size_t>(np))
            T.compiled.erase(T.compiled.begin() + first,
                             T.compiled.begin() + first + np);
        if (T.has_treatment.size() == static_cast<std::size_t>(T.n_nodes))
            T.has_treatment.erase(T.has_treatment.begin() + node_idx);
        --T.n_nodes;
    }

    // --- Step 5: erase SoA row (base + subtype side-table) ---
    erase_node_spatial(ctx, node_idx);
    ctx.node_names.remove_at(node_idx);
    ctx.nodes.erase_at(node_idx);
    // Drop this node's subtype row and shift the side-table join keys for nodes
    // after node_idx (mirrors the base-array index shift just done above).
    ctx.node_subtypes.erase_node(node_idx, ctx.nodes.count());

    // --- Step 6: renumber cross-references for nodes with index > node_idx ---
    renumber_refs(ctx.links.node1, node_idx);
    renumber_refs(ctx.links.node2, node_idx);
    renumber_refs(ctx.subcatches.outlet_node, node_idx);
    renumber_refs(ctx.node_subtypes.outfalls.route_to, node_idx);
    renumber_refs(ctx.inlet_usages.node_index, node_idx);
    // gw_node in subcatches also references nodes
    renumber_refs(ctx.subcatches.gw_node, node_idx);
    // inflow-row stores also key on node index (rows for node_idx already erased)
    renumber_refs(ctx.ext_inflows.node_idx, node_idx);
    renumber_refs(ctx.dwf_inflows.node_idx, node_idx);
    renumber_refs(ctx.rdii_assigns.node_idx, node_idx);

    return result;
}

// ============================================================================
// DELETE — link
// ============================================================================

CascadeResult delete_link(SimulationContext& ctx, int link_idx) {
    CascadeResult result;

    // --- Step 0: report (never edit) control rules referencing this link ---
    add_rule_refs(ctx, ctx.link_names.name_of(link_idx), result);

    // --- Step 1: nullify divider_link on nodes ---
    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        const int r = ctx.node_subtypes.divider_row(i);
        if (r >= 0 && ctx.node_subtypes.dividers.link[static_cast<std::size_t>(r)] == link_idx) {
            ctx.node_subtypes.dividers.link[static_cast<std::size_t>(r)] = -1;
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
    // Drop the subtype side-table row and renumber its join keys (mirrors
    // delete_node's erase_node). Must run after LinkData::erase_at.
    ctx.link_subtypes.erase_link(link_idx, ctx.links.count());

    // --- Step 4: renumber cross-references ---
    renumber_refs(ctx.node_subtypes.dividers.link, link_idx);
    renumber_refs(ctx.inlet_usages.link_index, link_idx);
    // outfall link_idx is a cached value rebuilt at init; nullify for safety
    for (auto& v : ctx.node_subtypes.outfalls.link_idx) {
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
        const int r = ctx.node_subtypes.outfall_row(i);
        if (r >= 0 && ctx.node_subtypes.outfalls.route_to[static_cast<std::size_t>(r)] == sc_idx) {
            ctx.node_subtypes.outfalls.route_to[static_cast<std::size_t>(r)] = -1;
            result.add(SWMM_REF_NODE, i, "outfall_route_to", false);
        }
    }

    // --- Step 2b: cascade-delete LID usage rows on this subcatchment ---
    for (int i = ctx.lid_usage.count() - 1; i >= 0; --i) {
        if (ctx.lid_usage.subcatch_index[static_cast<std::size_t>(i)] == sc_idx) {
            result.add(SWMM_REF_LID_USAGE, i, "subcatch_index", true);
            erase_lid_usage(ctx, i);
        }
    }

    // --- Step 2c: clear name-based references (LID drain_to, snowpack removal) ---
    {
        const std::string sc_name = ctx.subcatch_names.name_of(sc_idx);
        if (!sc_name.empty()) {
            for (int i = 0; i < ctx.lid_usage.count(); ++i) {
                const auto ui = static_cast<std::size_t>(i);
                if (ui < ctx.lid_usage.drain_to.size() &&
                    ctx.lid_usage.drain_to[ui] == sc_name) {
                    ctx.lid_usage.drain_to[ui].clear();
                    result.add(SWMM_REF_LID_USAGE, i, "drain_to", false);
                }
            }
            for (int i = 0; i < ctx.snowpacks.count(); ++i) {
                const auto ui = static_cast<std::size_t>(i);
                if (ui < ctx.snowpacks.removal_subcatch.size() &&
                    ctx.snowpacks.removal_subcatch[ui] == sc_name) {
                    ctx.snowpacks.removal_subcatch[ui].clear();
                    result.add(SWMM_REF_SNOWPACK, i, "removal_subcatch", false);
                }
            }
        }
    }

    // --- Step 3: erase SoA row ---
    erase_subcatch_spatial(ctx, sc_idx);
    erase_subcatch_patterns(ctx, sc_idx);
    ctx.subcatch_names.remove_at(sc_idx);
    ctx.subcatches.erase_at(sc_idx);

    // --- Step 4: renumber ---
    renumber_refs(ctx.subcatches.outlet_subcatch, sc_idx);
    renumber_refs(ctx.node_subtypes.outfalls.route_to, sc_idx);
    renumber_refs(ctx.lid_usage.subcatch_index, sc_idx);

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

    // --- Step 1b: clear hydrograph gage assignments (name-based) ---
    {
        const std::string gage_name = ctx.gage_names.name_of(gage_idx);
        if (!gage_name.empty()) {
            for (std::size_t i = 0; i < ctx.unit_hyds.gage_names.size(); ++i) {
                if (ctx.unit_hyds.gage_names[i] == gage_name) {
                    ctx.unit_hyds.gage_names[i].clear();
                    result.add(SWMM_REF_HYDROGRAPH, static_cast<int>(i), "gage_name", false);
                }
            }
            for (std::size_t i = 0; i < ctx.unit_hyds.entries.size(); ++i) {
                if (ctx.unit_hyds.entries[i].gage_name == gage_name) {
                    ctx.unit_hyds.entries[i].gage_name.clear();
                    result.add(SWMM_REF_HYDROGRAPH, static_cast<int>(i), "uh_gage_name", false);
                }
            }
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
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.gages.ts_index[ui] == table_idx) {
            ctx.gages.ts_index[ui] = -1;
            // Clear the name-based mirror too, or the writer re-emits it.
            if (ui < ctx.gages.ts_name.size()) ctx.gages.ts_name[ui].clear();
            result.add(SWMM_REF_GAGE, i, "ts_index", false);
        }
    }

    // External-inflow rows reference timeseries by name only.
    {
        const std::string& t_name = ctx.table_names.name_of(table_idx);
        if (!t_name.empty()) {
            for (int i = 0; i < ctx.ext_inflows.count(); ++i) {
                const auto ui = static_cast<std::size_t>(i);
                if (ctx.ext_inflows.ts_name[ui] == t_name) {
                    ctx.ext_inflows.ts_name[ui].clear();
                    result.add(SWMM_REF_EXT_INFLOW, i, "ts_name", false);
                }
            }
        }
    }

    const int nn = ctx.n_nodes();
    for (int i = 0; i < nn; ++i) {
        switch (ctx.nodes.type[static_cast<std::size_t>(i)]) {
            case NodeType::STORAGE: {
                const int r = ctx.node_subtypes.storage_row(i);
                if (r >= 0 && ctx.node_subtypes.storages.curve[static_cast<std::size_t>(r)] == table_idx) {
                    ctx.node_subtypes.storages.curve[static_cast<std::size_t>(r)] = -1;
                    result.add(SWMM_REF_NODE, i, "storage_curve", false);
                }
                break;
            }
            case NodeType::DIVIDER: {
                const int r = ctx.node_subtypes.divider_row(i);
                if (r >= 0 && ctx.node_subtypes.dividers.curve[static_cast<std::size_t>(r)] == table_idx) {
                    ctx.node_subtypes.dividers.curve[static_cast<std::size_t>(r)] = -1;
                    result.add(SWMM_REF_NODE, i, "divider_curve", false);
                }
                break;
            }
            case NodeType::OUTFALL: {
                // outfall param encodes table index as double for TIDAL/TIMESERIES
                const int r = ctx.node_subtypes.outfall_row(i);
                if (r >= 0) {
                    const auto ur = static_cast<std::size_t>(r);
                    auto& O = ctx.node_subtypes.outfalls;
                    if ((O.bc_type[ur] == OutfallType::TIDAL ||
                         O.bc_type[ur] == OutfallType::TIMESERIES) &&
                        static_cast<int>(O.param[ur]) == table_idx) {
                        // -1, not 0: 0 is a valid table index and would silently
                        // re-point the outfall at whatever table is first.
                        O.param[ur] = -1.0;
                        O.param_name[ur].clear();
                        result.add(SWMM_REF_NODE, i, "outfall_param", false);
                    }
                }
                break;
            }
            default: break;
        }
    }

    {
        auto& PD = ctx.link_subtypes.pumps;
        for (int r = 0; r < PD.count(); ++r)
            if (PD.curve[static_cast<std::size_t>(r)] == table_idx) {
                PD.curve[static_cast<std::size_t>(r)] = -1;
                result.add(SWMM_REF_LINK, PD.link_idx[static_cast<std::size_t>(r)], "pump_curve", false);
            }
        auto& OUT = ctx.link_subtypes.outlets;
        for (int r = 0; r < OUT.count(); ++r)
            if (OUT.curve[static_cast<std::size_t>(r)] == table_idx) {
                OUT.curve[static_cast<std::size_t>(r)] = -1;
                result.add(SWMM_REF_LINK, OUT.link_idx[static_cast<std::size_t>(r)], "pump_curve", false);
            }
    }
    const int nl = ctx.n_links();
    for (int i = 0; i < nl; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.links.xsect_curve[ui] == table_idx) {
            ctx.links.xsect_curve[ui] = -1;
            result.add(SWMM_REF_LINK, i, "xsect_curve", false);
        }
    }

    // --- Step 2: erase the table entry ---
    ctx.tables.tables.erase(ctx.tables.tables.begin() + static_cast<std::ptrdiff_t>(table_idx));
    ctx.table_names.remove_at(table_idx);

    // Subcatchment adjustment patterns index table_names (see InpWriter tN)
    auto clear_adj = [&](std::vector<int>& v, const char* field) {
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i] == table_idx) {
                v[i] = -1;
                result.add(SWMM_REF_SUBCATCH, static_cast<int>(i), field, false);
            }
        }
    };
    clear_adj(ctx.subcatch_n_perv_pattern,  "n_perv_pattern");
    clear_adj(ctx.subcatch_d_store_pattern, "d_store_pattern");
    clear_adj(ctx.subcatch_infil_pattern,   "infil_pattern");

    // --- Step 3: renumber cross-references for indices > table_idx ---
    renumber_refs(ctx.gages.ts_index, table_idx);
    renumber_refs(ctx.node_subtypes.storages.curve, table_idx);
    renumber_refs(ctx.node_subtypes.dividers.curve, table_idx);
    renumber_refs(ctx.link_subtypes.pumps.curve, table_idx);
    renumber_refs(ctx.link_subtypes.outlets.curve, table_idx);
    renumber_refs(ctx.links.xsect_curve, table_idx);
    renumber_refs(ctx.subcatch_n_perv_pattern,  table_idx);
    renumber_refs(ctx.subcatch_d_store_pattern, table_idx);
    renumber_refs(ctx.subcatch_infil_pattern,   table_idx);

    // outfall param stores table index as double for TIDAL/TIMESERIES — renumber
    {
        auto& O = ctx.node_subtypes.outfalls;
        for (int r = 0; r < O.count(); ++r) {
            const auto ur = static_cast<std::size_t>(r);
            if (O.bc_type[ur] == OutfallType::TIDAL ||
                O.bc_type[ur] == OutfallType::TIMESERIES) {
                int ref = static_cast<int>(O.param[ur]);
                if (ref > table_idx) O.param[ur] = static_cast<double>(ref - 1);
            }
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

// ============================================================================
// Helpers for flat [object x pollutant] / [object x landuse] matrices
// ============================================================================

// Erase column `col` from a flat row-major [n_rows x n_cols] matrix.
// No-op unless the vector is sized exactly n_rows * n_cols.
template <class T>
static void erase_matrix_column(std::vector<T>& v, int n_rows, int n_cols, int col) {
    if (n_cols <= 1) { if (static_cast<int>(v.size()) == n_rows * n_cols) v.clear(); return; }
    if (v.size() != static_cast<std::size_t>(n_rows) * static_cast<std::size_t>(n_cols))
        return;
    std::size_t w = 0;
    for (std::size_t r = 0; r < static_cast<std::size_t>(n_rows); ++r)
        for (std::size_t c = 0; c < static_cast<std::size_t>(n_cols); ++c)
            if (c != static_cast<std::size_t>(col))
                v[w++] = std::move(v[r * static_cast<std::size_t>(n_cols) + c]);
    v.resize(w);
}

// Erase row `row` from a flat row-major [n_rows x n_cols] matrix.
template <class T>
static void erase_matrix_row(std::vector<T>& v, int n_rows, int n_cols, int row) {
    if (v.size() != static_cast<std::size_t>(n_rows) * static_cast<std::size_t>(n_cols))
        return;
    const auto first = v.begin() + static_cast<std::ptrdiff_t>(row) * n_cols;
    v.erase(first, first + n_cols);
}

// ============================================================================
// ANALYZE / DELETE — pollutant
// ============================================================================

// Shared scan so analyze and delete report identical impact sets.
static void scan_pollutant_refs(const SimulationContext& ctx, int pollut_idx,
                                CascadeResult& result) {
    const int np = ctx.n_pollutants();
    const std::string& pname = ctx.pollutant_names.name_of(pollut_idx);

    // Buildup / washoff functions per (landuse x pollutant)
    if (ctx.buildup.n_pollutants == np) {
        for (int lu = 0; lu < ctx.buildup.n_landuses; ++lu) {
            const auto k = static_cast<std::size_t>(lu) * np +
                           static_cast<std::size_t>(pollut_idx);
            if (k < ctx.buildup.func_type.size() && ctx.buildup.func_type[k] != 0)
                result.add(SWMM_REF_LANDUSE, lu, "buildup", true);
        }
    }
    if (ctx.washoff.n_pollutants == np) {
        for (int lu = 0; lu < ctx.washoff.n_landuses; ++lu) {
            const auto k = static_cast<std::size_t>(lu) * np +
                           static_cast<std::size_t>(pollut_idx);
            if (k < ctx.washoff.func_type.size() && ctx.washoff.func_type[k] != 0)
                result.add(SWMM_REF_LANDUSE, lu, "washoff", true);
        }
    }

    // Treatment expressions per (node x pollutant)
    if (ctx.treatment.n_pollutants == np) {
        for (int n = 0; n < ctx.treatment.n_nodes; ++n) {
            const auto k = static_cast<std::size_t>(n) * np +
                           static_cast<std::size_t>(pollut_idx);
            if (k < ctx.treatment.expressions.size() &&
                !ctx.treatment.expressions[k].empty())
                result.add(SWMM_REF_TREATMENT, n, "expression", true);
        }
    }

    // Co-pollutant references
    for (int i = 0; i < np; ++i) {
        if (i == pollut_idx) continue;
        if (static_cast<std::size_t>(i) < ctx.pollutants.co_pollut.size() &&
            ctx.pollutants.co_pollut[static_cast<std::size_t>(i)] == pollut_idx)
            result.add(SWMM_REF_POLLUTANT, i, "co_pollut", false);
    }

    // Inflow rows keyed by constituent name
    for (int i = ctx.ext_inflows.count() - 1; i >= 0; --i)
        if (ctx.ext_inflows.constituent[static_cast<std::size_t>(i)] == pname)
            result.add(SWMM_REF_EXT_INFLOW, i, "constituent", true);
    for (int i = ctx.dwf_inflows.count() - 1; i >= 0; --i)
        if (ctx.dwf_inflows.constituent[static_cast<std::size_t>(i)] == pname)
            result.add(SWMM_REF_DWF_INFLOW, i, "constituent", true);

    // LID pollutant-removal pairs (removals is lazily sized — may be shorter
    // than count())
    for (int i = 0; i < static_cast<int>(ctx.lid_controls.removals.size()); ++i) {
        const auto& pairs = ctx.lid_controls.removals[static_cast<std::size_t>(i)];
        for (const auto& pr : pairs) {
            if (pr.first == pollut_idx) {
                result.add(SWMM_REF_LID_CONTROL, i, "removals", false);
                break;
            }
        }
    }
}

CascadeResult analyze_pollutant_impact(const SimulationContext& ctx, int pollut_idx) {
    CascadeResult result;
    scan_pollutant_refs(ctx, pollut_idx, result);
    return result;
}

CascadeResult delete_pollutant(SimulationContext& ctx, int pollut_idx) {
    CascadeResult result;
    scan_pollutant_refs(ctx, pollut_idx, result);

    const int np = ctx.n_pollutants();
    const std::string pname = ctx.pollutant_names.name_of(pollut_idx);

    // --- Step 1: cascade-delete inflow rows keyed by constituent name ---
    for (int i = ctx.ext_inflows.count() - 1; i >= 0; --i)
        if (ctx.ext_inflows.constituent[static_cast<std::size_t>(i)] == pname)
            ctx.ext_inflows.erase(i);
    for (int i = ctx.dwf_inflows.count() - 1; i >= 0; --i)
        if (ctx.dwf_inflows.constituent[static_cast<std::size_t>(i)] == pname)
            ctx.dwf_inflows.erase(i);

    // --- Step 2: re-pack the per-pollutant dimension of quality matrices ---
    if (ctx.buildup.n_pollutants == np) {
        auto& B = ctx.buildup;
        erase_matrix_column(B.func_type,  B.n_landuses, np, pollut_idx);
        erase_matrix_column(B.coeff1,     B.n_landuses, np, pollut_idx);
        erase_matrix_column(B.coeff2,     B.n_landuses, np, pollut_idx);
        erase_matrix_column(B.coeff3,     B.n_landuses, np, pollut_idx);
        erase_matrix_column(B.normalizer, B.n_landuses, np, pollut_idx);
        B.n_pollutants = np - 1;
    }
    if (ctx.washoff.n_pollutants == np) {
        auto& W = ctx.washoff;
        erase_matrix_column(W.func_type,   W.n_landuses, np, pollut_idx);
        erase_matrix_column(W.coeff,       W.n_landuses, np, pollut_idx);
        erase_matrix_column(W.expon,       W.n_landuses, np, pollut_idx);
        erase_matrix_column(W.sweep_effic, W.n_landuses, np, pollut_idx);
        erase_matrix_column(W.bmp_effic,   W.n_landuses, np, pollut_idx);
        W.n_pollutants = np - 1;
    }
    if (ctx.treatment.n_pollutants == np) {
        auto& T = ctx.treatment;
        erase_matrix_column(T.expressions, T.n_nodes, np, pollut_idx);
        erase_matrix_column(T.compiled,    T.n_nodes, np, pollut_idx);
        if (static_cast<int>(T.cin.size()) == np)
            T.cin.erase(T.cin.begin() + pollut_idx);
        if (static_cast<int>(T.removal.size()) == np)
            T.removal.erase(T.removal.begin() + pollut_idx);
        T.n_pollutants = np - 1;
    }

    // Per-object concentration state ([LOADINGS] initial buildup lives in
    // subcatches.conc, so erase columns — never re-zero wholesale).
    if (ctx.subcatches.conc_n_pollutants == np) {
        auto& S = ctx.subcatches;
        const int n = S.count();
        erase_matrix_column(S.conc,         n, np, pollut_idx);
        erase_matrix_column(S.conc_old,     n, np, pollut_idx);
        erase_matrix_column(S.ponded_qual,  n, np, pollut_idx);
        erase_matrix_column(S.washoff_load, n, np, pollut_idx);
        S.conc_n_pollutants = np - 1;
    }
    if (ctx.nodes.conc_n_pollutants == np) {
        auto& N = ctx.nodes;
        const int n = N.count();
        erase_matrix_column(N.conc,                n, np, pollut_idx);
        erase_matrix_column(N.conc_old,            n, np, pollut_idx);
        erase_matrix_column(N.user_conc_mass_flux, n, np, pollut_idx);
        erase_matrix_column(N.qual_mass_in,        n, np, pollut_idx);
        erase_matrix_column(N.iface_qual_mass,     n, np, pollut_idx);
        erase_matrix_column(N.lid_drain_qual_load, n, np, pollut_idx);
        N.conc_n_pollutants = np - 1;
    }
    if (ctx.links.conc_n_pollutants == np) {
        auto& L = ctx.links;
        const int n = L.count();
        erase_matrix_column(L.conc,     n, np, pollut_idx);
        erase_matrix_column(L.conc_old, n, np, pollut_idx);
        L.conc_n_pollutants = np - 1;
    }

    // --- Step 3: nullify co-pollutant references, drop LID removal pairs ---
    for (std::size_t i = 0; i < ctx.pollutants.co_pollut.size(); ++i) {
        if (static_cast<int>(i) == pollut_idx) continue;
        if (ctx.pollutants.co_pollut[i] == pollut_idx) {
            ctx.pollutants.co_pollut[i] = -1;
            if (i < ctx.pollutants.co_frac.size()) ctx.pollutants.co_frac[i] = 0.0;
        }
    }
    for (int i = 0; i < static_cast<int>(ctx.lid_controls.removals.size()); ++i) {
        auto& pairs = ctx.lid_controls.removals[static_cast<std::size_t>(i)];
        pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
                        [&](const std::pair<int, double>& pr) {
                            return pr.first == pollut_idx;
                        }),
                    pairs.end());
        for (auto& pr : pairs)
            if (pr.first > pollut_idx) --pr.first;
    }

    // --- Step 4: erase the pollutant's own definition row ---
    {
        auto& P = ctx.pollutants;
        const auto ui = static_cast<std::size_t>(pollut_idx);
        auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(pollut_idx)); };
        e(P.units); e(P.mwt); e(P.k_decay);
        e(P.c_rain); e(P.c_gw); e(P.c_rdii); e(P.c_dwf);
        e(P.init_conc); e(P.co_pollut); e(P.co_frac);
        e(P.snow_only); e(P.comments);
    }
    ctx.pollutant_names.remove_at(pollut_idx);

    // --- Step 5: renumber co-pollutant references ---
    renumber_refs(ctx.pollutants.co_pollut, pollut_idx);

    return result;
}

// ============================================================================
// ANALYZE / DELETE — time pattern
// ============================================================================

// Every pattern-name reference site. Mirrors for_each_pattern_name_ref in
// openswmm_tables_impl.cpp; kept here so analyze/delete report per-site
// impact entries.
static void scan_pattern_refs(const SimulationContext& ctx, int pattern_idx,
                              CascadeResult& result) {
    const std::string& name = ctx.patterns.names[static_cast<std::size_t>(pattern_idx)];
    if (name.empty()) return;
    for (int i = 0; i < ctx.ext_inflows.count(); ++i)
        if (ctx.ext_inflows.pattern_name[static_cast<std::size_t>(i)] == name)
            result.add(SWMM_REF_EXT_INFLOW, i, "pattern_name", false);
    for (int i = 0; i < ctx.dwf_inflows.count(); ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ctx.dwf_inflows.pat1[ui] == name) result.add(SWMM_REF_DWF_INFLOW, i, "pat1", false);
        if (ctx.dwf_inflows.pat2[ui] == name) result.add(SWMM_REF_DWF_INFLOW, i, "pat2", false);
        if (ctx.dwf_inflows.pat3[ui] == name) result.add(SWMM_REF_DWF_INFLOW, i, "pat3", false);
        if (ctx.dwf_inflows.pat4[ui] == name) result.add(SWMM_REF_DWF_INFLOW, i, "pat4", false);
    }
    for (int i = 0; i < ctx.aquifers.count(); ++i)
        if (ctx.aquifers.upper_evap_pat[static_cast<std::size_t>(i)] == name)
            result.add(SWMM_REF_AQUIFER, i, "upper_evap_pat", false);
    if (ctx.options.evap_recovery_pat == name)
        result.add(SWMM_REF_PATTERN, -1, "options.evap_recovery_pat", false);
}

CascadeResult analyze_pattern_impact(const SimulationContext& ctx, int pattern_idx) {
    CascadeResult result;
    scan_pattern_refs(ctx, pattern_idx, result);
    return result;
}

CascadeResult delete_pattern(SimulationContext& ctx, int pattern_idx) {
    CascadeResult result;
    scan_pattern_refs(ctx, pattern_idx, result);

    const std::string name = ctx.patterns.names[static_cast<std::size_t>(pattern_idx)];

    // --- Step 1: clear name-based references ---
    if (!name.empty()) {
        for (auto& s : ctx.ext_inflows.pattern_name) if (s == name) s.clear();
        for (auto& s : ctx.dwf_inflows.pat1)         if (s == name) s.clear();
        for (auto& s : ctx.dwf_inflows.pat2)         if (s == name) s.clear();
        for (auto& s : ctx.dwf_inflows.pat3)         if (s == name) s.clear();
        for (auto& s : ctx.dwf_inflows.pat4)         if (s == name) s.clear();
        for (auto& s : ctx.aquifers.upper_evap_pat)  if (s == name) s.clear();
        if (ctx.options.evap_recovery_pat == name) ctx.options.evap_recovery_pat.clear();
    }

    // --- Step 2: erase the pattern row ---
    const auto u = static_cast<std::size_t>(pattern_idx);
    ctx.patterns.names.erase(ctx.patterns.names.begin() + static_cast<std::ptrdiff_t>(u));
    ctx.patterns.types.erase(ctx.patterns.types.begin() + static_cast<std::ptrdiff_t>(u));
    ctx.patterns.factors.erase(ctx.patterns.factors.begin() + static_cast<std::ptrdiff_t>(u));

    // Patterns are referenced by name everywhere — no index renumbering needed.
    return result;
}

// ============================================================================
// ANALYZE / DELETE — aquifer
// ============================================================================

CascadeResult analyze_aquifer_impact(const SimulationContext& ctx, int aquifer_idx) {
    CascadeResult result;
    for (std::size_t i = 0; i < ctx.subcatches.gw_aquifer.size(); ++i)
        if (ctx.subcatches.gw_aquifer[i] == aquifer_idx)
            result.add(SWMM_REF_SUBCATCH, static_cast<int>(i), "gw_aquifer", false);
    return result;
}

CascadeResult delete_aquifer(SimulationContext& ctx, int aquifer_idx) {
    CascadeResult result;

    // --- Step 1: nullify subcatchment references (drops their groundwater) ---
    for (std::size_t i = 0; i < ctx.subcatches.gw_aquifer.size(); ++i) {
        if (ctx.subcatches.gw_aquifer[i] == aquifer_idx) {
            ctx.subcatches.gw_aquifer[i] = -1;
            result.add(SWMM_REF_SUBCATCH, static_cast<int>(i), "gw_aquifer", false);
        }
    }

    // --- Step 2: erase the aquifer row ---
    {
        auto& A = ctx.aquifers;
        const auto ui = static_cast<std::size_t>(aquifer_idx);
        auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(aquifer_idx)); };
        e(A.names); e(A.porosity); e(A.wilting_point); e(A.field_capacity);
        e(A.conductivity); e(A.conduct_slope); e(A.tension_slope);
        e(A.upper_evap); e(A.lower_evap); e(A.lower_loss);
        e(A.bottom_elev); e(A.water_table_elev); e(A.upper_moist);
        e(A.upper_evap_pat);
    }
    ctx.aquifer_names.remove_at(aquifer_idx);

    // --- Step 3: renumber ---
    renumber_refs(ctx.subcatches.gw_aquifer, aquifer_idx);

    return result;
}

// ============================================================================
// ANALYZE / DELETE — snowpack
// ============================================================================

CascadeResult analyze_snowpack_impact(const SimulationContext& ctx, int snowpack_idx) {
    CascadeResult result;
    for (std::size_t i = 0; i < ctx.subcatches.snowpack.size(); ++i)
        if (ctx.subcatches.snowpack[i] == snowpack_idx)
            result.add(SWMM_REF_SUBCATCH, static_cast<int>(i), "snowpack", false);
    return result;
}

CascadeResult delete_snowpack(SimulationContext& ctx, int snowpack_idx) {
    CascadeResult result;

    // --- Step 1: nullify subcatchment references (index + raw-name mirror) ---
    for (std::size_t i = 0; i < ctx.subcatches.snowpack.size(); ++i) {
        if (ctx.subcatches.snowpack[i] == snowpack_idx) {
            ctx.subcatches.snowpack[i] = -1;
            if (i < ctx.subcatches.snowpack_name.size())
                ctx.subcatches.snowpack_name[i].clear();
            result.add(SWMM_REF_SUBCATCH, static_cast<int>(i), "snowpack", false);
        }
    }

    // --- Step 2: erase the snowpack row ---
    {
        auto& S = ctx.snowpacks;
        const auto ui = static_cast<std::size_t>(snowpack_idx);
        auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(snowpack_idx)); };
        e(S.names); e(S.plowable); e(S.impervious); e(S.pervious);
        e(S.removal); e(S.removal_subcatch);
    }
    ctx.snowpack_names.remove_at(snowpack_idx);

    // --- Step 3: renumber ---
    renumber_refs(ctx.subcatches.snowpack, snowpack_idx);

    return result;
}

// ============================================================================
// ANALYZE / DELETE — LID control
// ============================================================================

CascadeResult analyze_lid_impact(const SimulationContext& ctx, int lid_idx) {
    CascadeResult result;
    for (int i = ctx.lid_usage.count() - 1; i >= 0; --i)
        if (ctx.lid_usage.lid_index[static_cast<std::size_t>(i)] == lid_idx)
            result.add(SWMM_REF_LID_USAGE, i, "lid_index", true);
    return result;
}

CascadeResult delete_lid(SimulationContext& ctx, int lid_idx) {
    CascadeResult result;

    // --- Step 1: cascade-delete usage rows referencing this control ---
    for (int i = ctx.lid_usage.count() - 1; i >= 0; --i) {
        if (ctx.lid_usage.lid_index[static_cast<std::size_t>(i)] == lid_idx) {
            result.add(SWMM_REF_LID_USAGE, i, "lid_index", true);
            erase_lid_usage(ctx, i);
        }
    }

    // --- Step 2: erase the LID control row ---
    {
        auto& L = ctx.lid_controls;
        const auto ui = static_cast<std::size_t>(lid_idx);
        auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(lid_idx)); };
        e(L.names); e(L.lid_type); e(L.surface); e(L.soil); e(L.pavement);
        e(L.storage); e(L.drain); e(L.drainmat); e(L.removals);
    }
    ctx.lid_names.remove_at(lid_idx);

    // --- Step 3: renumber ---
    renumber_refs(ctx.lid_usage.lid_index, lid_idx);

    return result;
}

// ============================================================================
// ANALYZE / DELETE — street
// ============================================================================

CascadeResult analyze_street_impact(const SimulationContext& ctx, int street_idx) {
    CascadeResult result;
    for (int i = ctx.inlet_usages.count() - 1; i >= 0; --i)
        if (ctx.inlet_usages.street_index[static_cast<std::size_t>(i)] == street_idx)
            result.add(SWMM_REF_INLET_USAGE, i, "street_index", true);
    // Conduits with a STREET cross-section reference the street by name
    // (held in pump_curve_name — see PostParseResolver).
    const std::string& s_name = ctx.streets.names[static_cast<std::size_t>(street_idx)];
    if (!s_name.empty()) {
        for (int i = 0; i < ctx.n_links(); ++i) {
            const auto ui = static_cast<std::size_t>(i);
            if (ctx.links.xsect_shape[ui] == XsectShape::STREET_XSECT &&
                ui < ctx.links.pump_curve_name.size() &&
                ctx.links.pump_curve_name[ui] == s_name)
                result.add(SWMM_REF_LINK, i, "xsect_street", false);
        }
    }
    return result;
}

CascadeResult delete_street(SimulationContext& ctx, int street_idx) {
    CascadeResult result;

    // --- Step 1: cascade-delete inlet-usage rows on this street ---
    // (consistent with link delete, which also cascades usage rows)
    for (int i = ctx.inlet_usages.count() - 1; i >= 0; --i) {
        if (ctx.inlet_usages.street_index[static_cast<std::size_t>(i)] == street_idx) {
            result.add(SWMM_REF_INLET_USAGE, i, "street_index", true);
            erase_inlet_usage(ctx, i);
        }
    }

    // --- Step 1b: reset STREET cross-sections that reference this street by
    //              name (mirrors transect delete: fall back to CIRCULAR) ---
    {
        const std::string s_name = ctx.streets.names[static_cast<std::size_t>(street_idx)];
        if (!s_name.empty()) {
            for (int i = 0; i < ctx.n_links(); ++i) {
                const auto ui = static_cast<std::size_t>(i);
                if (ctx.links.xsect_shape[ui] == XsectShape::STREET_XSECT &&
                    ui < ctx.links.pump_curve_name.size() &&
                    ctx.links.pump_curve_name[ui] == s_name) {
                    ctx.links.pump_curve_name[ui].clear();
                    ctx.links.xsect_shape[ui] = XsectShape::CIRCULAR;
                    ctx.links.xsect_curve[ui] = -1;
                    result.add(SWMM_REF_LINK, i, "xsect_street", false);
                }
            }
        }
    }

    // --- Step 2: erase the street row ---
    {
        auto& S = ctx.streets;
        const auto ui = static_cast<std::size_t>(street_idx);
        auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(street_idx)); };
        e(S.names); e(S.t_crown); e(S.h_curb); e(S.sx); e(S.n_road);
        e(S.gutter_depres); e(S.gutter_width); e(S.sides);
        e(S.back_width); e(S.back_slope); e(S.back_n);
    }

    // --- Step 3: renumber ---
    renumber_refs(ctx.inlet_usages.street_index, street_idx);

    return result;
}

// ============================================================================
// ANALYZE / DELETE — inlet design
// ============================================================================

CascadeResult analyze_inlet_impact(const SimulationContext& ctx, int inlet_idx) {
    CascadeResult result;
    for (int i = ctx.inlet_usages.count() - 1; i >= 0; --i)
        if (ctx.inlet_usages.design_index[static_cast<std::size_t>(i)] == inlet_idx)
            result.add(SWMM_REF_INLET_USAGE, i, "design_index", true);
    return result;
}

CascadeResult delete_inlet(SimulationContext& ctx, int inlet_idx) {
    CascadeResult result;

    // --- Step 1: cascade-delete usage rows referencing this design ---
    for (int i = ctx.inlet_usages.count() - 1; i >= 0; --i) {
        if (ctx.inlet_usages.design_index[static_cast<std::size_t>(i)] == inlet_idx) {
            result.add(SWMM_REF_INLET_USAGE, i, "design_index", true);
            erase_inlet_usage(ctx, i);
        }
    }

    // --- Step 2: erase the inlet-design row ---
    {
        auto& I = ctx.inlets;
        const auto ui = static_cast<std::size_t>(inlet_idx);
        auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(inlet_idx)); };
        e(I.names); e(I.inlet_type); e(I.length); e(I.width);
        e(I.grate_type); e(I.open_area); e(I.splash_veloc);
    }

    // --- Step 3: renumber ---
    renumber_refs(ctx.inlet_usages.design_index, inlet_idx);

    return result;
}

// ============================================================================
// ANALYZE / DELETE — land use
// ============================================================================

static void scan_landuse_refs(const SimulationContext& ctx, int landuse_idx,
                              CascadeResult& result) {
    const int np = ctx.n_pollutants();
    if (ctx.buildup.n_landuses > landuse_idx && ctx.buildup.n_pollutants == np) {
        for (int p = 0; p < np; ++p) {
            const auto k = static_cast<std::size_t>(landuse_idx) * np +
                           static_cast<std::size_t>(p);
            if (k < ctx.buildup.func_type.size() && ctx.buildup.func_type[k] != 0)
                result.add(SWMM_REF_POLLUTANT, p, "buildup", true);
        }
    }
    if (ctx.washoff.n_landuses > landuse_idx && ctx.washoff.n_pollutants == np) {
        for (int p = 0; p < np; ++p) {
            const auto k = static_cast<std::size_t>(landuse_idx) * np +
                           static_cast<std::size_t>(p);
            if (k < ctx.washoff.func_type.size() && ctx.washoff.func_type[k] != 0)
                result.add(SWMM_REF_POLLUTANT, p, "washoff", true);
        }
    }
    const int nlu = ctx.subcatches.coverage_n_landuses;
    if (nlu > landuse_idx) {
        for (int sc = 0; sc < ctx.n_subcatches(); ++sc) {
            const auto k = static_cast<std::size_t>(sc) * nlu +
                           static_cast<std::size_t>(landuse_idx);
            if (k < ctx.subcatches.coverage.size() && ctx.subcatches.coverage[k] > 0.0)
                result.add(SWMM_REF_SUBCATCH, sc, "coverage", false);
        }
    }
}

CascadeResult analyze_landuse_impact(const SimulationContext& ctx, int landuse_idx) {
    CascadeResult result;
    scan_landuse_refs(ctx, landuse_idx, result);
    return result;
}

CascadeResult delete_landuse(SimulationContext& ctx, int landuse_idx) {
    CascadeResult result;
    scan_landuse_refs(ctx, landuse_idx, result);

    const int np = ctx.n_pollutants();

    // --- Step 1: re-pack the per-landuse dimension of quality matrices ---
    if (ctx.buildup.n_landuses > landuse_idx && ctx.buildup.n_pollutants == np) {
        auto& B = ctx.buildup;
        erase_matrix_row(B.func_type,  B.n_landuses, np, landuse_idx);
        erase_matrix_row(B.coeff1,     B.n_landuses, np, landuse_idx);
        erase_matrix_row(B.coeff2,     B.n_landuses, np, landuse_idx);
        erase_matrix_row(B.coeff3,     B.n_landuses, np, landuse_idx);
        erase_matrix_row(B.normalizer, B.n_landuses, np, landuse_idx);
        --B.n_landuses;
    }
    if (ctx.washoff.n_landuses > landuse_idx && ctx.washoff.n_pollutants == np) {
        auto& W = ctx.washoff;
        erase_matrix_row(W.func_type,   W.n_landuses, np, landuse_idx);
        erase_matrix_row(W.coeff,       W.n_landuses, np, landuse_idx);
        erase_matrix_row(W.expon,       W.n_landuses, np, landuse_idx);
        erase_matrix_row(W.sweep_effic, W.n_landuses, np, landuse_idx);
        erase_matrix_row(W.bmp_effic,   W.n_landuses, np, landuse_idx);
        --W.n_landuses;
    }

    // --- Step 2: erase this landuse's column from subcatch coverage/sweep ---
    {
        auto& S = ctx.subcatches;
        const int nlu = S.coverage_n_landuses;
        if (nlu > landuse_idx) {
            erase_matrix_column(S.coverage,         S.count(), nlu, landuse_idx);
            erase_matrix_column(S.sweep_last_swept, S.count(), nlu, landuse_idx);
            --S.coverage_n_landuses;
        }
    }

    // --- Step 3: erase the landuse row ---
    {
        auto& L = ctx.landuses;
        const auto ui = static_cast<std::size_t>(landuse_idx);
        auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(landuse_idx)); };
        e(L.sweep_interval); e(L.sweep_removal); e(L.last_swept); e(L.comments);
    }
    ctx.landuse_names.remove_at(landuse_idx);

    // Landuses are referenced positionally (matrix dimensions), not by stored
    // index — the re-packs above are the renumbering.
    return result;
}

// ============================================================================
// ANALYZE / DELETE — unit-hydrograph group (name-keyed)
// ============================================================================

CascadeResult analyze_hydrograph_impact(const SimulationContext& ctx,
                                        const std::string& uh_name) {
    CascadeResult result;
    for (int i = ctx.rdii_assigns.count() - 1; i >= 0; --i)
        if (ctx.rdii_assigns.uh_name[static_cast<std::size_t>(i)] == uh_name)
            result.add(SWMM_REF_RDII_ASSIGN, i, "uh_name", true);
    return result;
}

CascadeResult delete_hydrograph(SimulationContext& ctx, const std::string& uh_name) {
    CascadeResult result;

    // --- Step 1: cascade-delete RDII assignments using this group ---
    for (int i = ctx.rdii_assigns.count() - 1; i >= 0; --i) {
        if (ctx.rdii_assigns.uh_name[static_cast<std::size_t>(i)] == uh_name) {
            result.add(SWMM_REF_RDII_ASSIGN, i, "uh_name", true);
            ctx.rdii_assigns.erase(i);
        }
    }

    // --- Step 2: erase the group's own data (param lines, gage assignment,
    //             RDII-decay rows) ---
    auto& uh = ctx.unit_hyds;
    uh.entries.erase(
        std::remove_if(uh.entries.begin(), uh.entries.end(),
                       [&](const UnitHydEntry& e) { return e.name == uh_name; }),
        uh.entries.end());
    for (std::size_t i = uh.gage_assignments.size(); i-- > 0;) {
        if (uh.gage_assignments[i] == uh_name) {
            uh.gage_assignments.erase(uh.gage_assignments.begin() + static_cast<std::ptrdiff_t>(i));
            uh.gage_names.erase(uh.gage_names.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    auto& dd = ctx.rdii_decay;
    dd.entries.erase(
        std::remove_if(dd.entries.begin(), dd.entries.end(),
                       [&](const RDIIDecayEntry& e) { return e.uh_name == uh_name; }),
        dd.entries.end());

    return result;
}

} // namespace openswmm::edit
