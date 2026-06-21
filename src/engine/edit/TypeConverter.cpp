/**
 * @file TypeConverter.cpp
 * @brief In-place node and link type conversion.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "TypeConverter.hpp"
#include "../core/TypeHelpers.hpp"

namespace openswmm::edit {

// ============================================================================
// Node conversion
// ============================================================================

// Relational refactor (Phase 4): a node's subtype config now lives in the dense
// side-table (ctx.node_subtypes). Converting a node moves its row via
// set_node_type — which erases the old subtype row and inserts a fresh
// default row (matching the legacy clear-old-then-apply-defaults behavior) — so
// these helpers only record which logical fields the conversion cleared, for the
// cascade report. The field-name lists are preserved exactly from the legacy path.
static void record_cleared_outfall(std::vector<std::string>& cleared) {
    cleared.insert(cleared.end(), {
        "outfall_type", "outfall_param", "outfall_has_flap_gate",
        "outfall_route_to", "outfall_link_idx", "outfall_link_offset"
    });
}

static void record_cleared_storage(std::vector<std::string>& cleared) {
    cleared.insert(cleared.end(), {
        "storage_curve", "storage_a", "storage_b", "storage_c",
        "storage_seep_rate", "storage_evap_frac",
        "exfil_suction", "exfil_ksat", "exfil_imd"
    });
}

static void record_cleared_divider(std::vector<std::string>& cleared) {
    cleared.insert(cleared.end(), {
        "divider_type", "divider_cutoff", "divider_cd", "divider_max_depth",
        "divider_curve", "divider_link"
    });
}

ConversionResult convert_node(SimulationContext& ctx, int idx, NodeType new_type) {
    ConversionResult result;
    result.new_type = internal_to_c_node_type(new_type);
    const auto ui = static_cast<std::size_t>(idx);
    NodeData& nd = ctx.nodes;
    const NodeType old_type = nd.type[ui];

    // Record the old type-specific fields cleared by the conversion.
    switch (old_type) {
        case NodeType::OUTFALL:  record_cleared_outfall(result.cleared_fields); break;
        case NodeType::STORAGE:  record_cleared_storage(result.cleared_fields); break;
        case NodeType::DIVIDER:
            // Warn for any divider whose diversion link points to this node
            // (preserves the legacy scan; node_idx-ascending rows match the
            // legacy ascending node scan order).
            for (int r = 0; r < ctx.node_subtypes.dividers.count(); ++r) {
                const int dn = ctx.node_subtypes.dividers.node_idx[static_cast<std::size_t>(r)];
                if (dn == idx) continue;
                if (ctx.node_subtypes.dividers.link[static_cast<std::size_t>(r)] == idx) {
                    result.warnings.push_back(
                        "Node " + ctx.node_names.name_of(dn) +
                        ": divider_link reference cleared because source node changed type");
                }
            }
            record_cleared_divider(result.cleared_fields);
            break;
        default: break;
    }

    // New-type warnings (new defaults are applied by set_node_type below).
    if (new_type == NodeType::DIVIDER) {
        // Count connecting links to warn if degree != 3
        int degree = 0;
        for (int i = 0; i < ctx.n_links(); ++i) {
            const auto li = static_cast<std::size_t>(i);
            if (ctx.links.node1[li] == idx || ctx.links.node2[li] == idx)
                ++degree;
        }
        if (degree != 3)
            result.warnings.push_back(
                "Divider node has " + std::to_string(degree) +
                " connecting links; exactly 3 are required (1 inlet, 2 outlets)");
    }

    // Warn if we're converting the only OUTFALL away from OUTFALL
    if (old_type == NodeType::OUTFALL && new_type != NodeType::OUTFALL) {
        int outfall_count = 0;
        for (int i = 0; i < ctx.n_nodes(); ++i) {
            if (i != idx && ctx.nodes.type[static_cast<std::size_t>(i)] == NodeType::OUTFALL)
                ++outfall_count;
        }
        if (outfall_count == 0)
            result.warnings.push_back("Model will have no outfall boundary after this conversion");
    }

    // Move the subtype row and set nd.type (single source of truth). Erases the
    // old row (if any) and inserts a fresh default row for the new subtype.
    ctx.node_subtypes.set_node_type(nd, idx, new_type);
    return result;
}

// ============================================================================
// Link conversion
// ============================================================================

static void clear_conduit_fields(LinkData& ld, std::size_t ui,
                                  std::vector<std::string>& cleared) {
    ld.xsect_shape[ui]      = XsectShape::CIRCULAR;
    ld.xsect_y_full[ui]     = 0.0;
    ld.xsect_a_full[ui]     = 0.0;
    ld.xsect_w_max[ui]      = 0.0;
    ld.xsect_curve[ui]      = -1;
    ld.roughness[ui]        = 0.0;
    ld.length[ui]           = 0.0;
    ld.slope[ui]            = 0.0;
    ld.mod_length[ui]       = 0.0;
    ld.barrels[ui]          = 1;
    ld.beta[ui]             = 0.0;
    ld.rough_factor[ui]     = 0.0;
    ld.q_full[ui]           = 0.0;
    ld.xsect_r_full[ui]     = 0.0;
    ld.xsect_s_full[ui]     = 0.0;
    ld.xsect_s_max[ui]      = 0.0;
    ld.q_max[ui]            = 0.0;
    ld.xsect_y_bot[ui]      = 0.0;
    ld.xsect_a_bot[ui]      = 0.0;
    ld.xsect_s_bot[ui]      = 0.0;
    ld.xsect_r_bot[ui]      = 0.0;
    ld.xsect_yw_max[ui]     = 0.0;
    ld.xsect_batch_shape[ui]= 0;
    ld.loss_inlet[ui]       = 0.0;
    ld.loss_outlet[ui]      = 0.0;
    ld.loss_avg[ui]         = 0.0;
    ld.has_flap_gate[ui]    = 0;
    ld.seep_rate[ui]        = 0.0;
    ld.evap_loss_rate[ui]   = 0.0;
    ld.seep_loss_rate[ui]   = 0.0;
    ld.culvert_code[ui]     = 0;
    cleared.insert(cleared.end(), {
        "xsect_shape", "xsect_y_full", "roughness", "length", "slope",
        "barrels", "loss_inlet", "loss_outlet", "loss_avg", "has_flap_gate", "seep_rate"
    });
}

static void clear_pump_fields(LinkData& ld, std::size_t ui,
                               std::vector<std::string>& cleared) {
    ld.pump_curve[ui]      = -1;
    ld.pump_init_state[ui] = false;
    ld.pump_startup[ui]    = 0.0;
    ld.pump_shutoff[ui]    = 0.0;
    ld.pump_curve_type[ui] = -1;
    ld.pump_curve_name[ui] = {};
    cleared.insert(cleared.end(), {
        "pump_curve", "pump_init_state", "pump_startup", "pump_shutoff"
    });
}

static void clear_structure_fields(LinkData& ld, std::size_t ui,
                                    std::vector<std::string>& cleared) {
    ld.crest_height[ui] = 0.0;
    ld.cd[ui]           = 0.0;
    ld.param1[ui]       = 0.0;
    ld.param2[ui]       = 0.0;
    ld.orate[ui]        = 0.0;
    cleared.insert(cleared.end(), {"crest_height", "cd", "param1", "param2", "orate"});
}

ConversionResult convert_link(SimulationContext& ctx, int idx, LinkType new_type) {
    ConversionResult result;
    result.new_type = internal_to_c_link_type(new_type);
    const auto ui = static_cast<std::size_t>(idx);
    LinkData& ld = ctx.links;
    const LinkType old_type = ld.type[ui];

    // Clear old type-specific fields
    switch (old_type) {
        case LinkType::CONDUIT:
            if (ld.xsect_shape[ui] == XsectShape::IRREGULAR ||
                ld.xsect_shape[ui] == XsectShape::CUSTOM)
                result.warnings.push_back("Transect/shape-curve reference cleared");
            if (ld.barrels[ui] > 1)
                result.warnings.push_back(
                    "Multi-barrel property (barrels=" +
                    std::to_string(ld.barrels[ui]) + ") discarded");
            clear_conduit_fields(ld, ui, result.cleared_fields);
            break;
        case LinkType::PUMP:
            result.warnings.push_back(
                "Pump curve reference cleared; set conduit length after conversion");
            clear_pump_fields(ld, ui, result.cleared_fields);
            break;
        case LinkType::ORIFICE:
        case LinkType::WEIR:
        case LinkType::OUTLET:
            clear_structure_fields(ld, ui, result.cleared_fields);
            break;
    }

    // Move the subtype row and set ld.type[ui] (single source of truth): erases
    // the old subtype row and inserts a fresh default row for the new subtype.
    // Phase 6 Stage A.3: the wide arrays below are dual-written and remain
    // authoritative until Stage D, so the temporary build() mirror reproduces
    // identical values if the model is later initialized.
    ctx.link_subtypes.set_link_type(ld, idx, new_type);

    // Apply new type defaults (wide + freshly-created side-table row). Only
    // values that DIFFER from the side-table add_default seeds need an explicit
    // side-table write; the rest already match the seed.
    switch (new_type) {
        case LinkType::CONDUIT: {
            ld.roughness[ui]    = 0.013;
            ld.xsect_shape[ui]  = XsectShape::CIRCULAR;
            ld.xsect_y_full[ui] = 1.0;
            ld.barrels[ui]      = 1;
            const int cr = ctx.link_subtypes.conduit_row(idx);
            if (cr >= 0) ctx.link_subtypes.conduits.roughness[static_cast<std::size_t>(cr)] = 0.013;
            if (ld.length[ui] <= 0.0)
                result.warnings.push_back("Conduit length is 0; set a valid length before initializing");
            break;
        }
        case LinkType::PUMP:
            ld.pump_curve[ui]      = -1;
            ld.pump_init_state[ui] = false;
            ld.pump_startup[ui]    = 0.0;
            ld.pump_shutoff[ui]    = 0.0;
            ld.pump_curve_type[ui] = -1;
            // Side-table pump row already seeded with these defaults by add_default.
            result.warnings.push_back("Pump curve is unset (-1); assign a pump curve before initializing");
            break;
        case LinkType::ORIFICE: {
            ld.cd[ui]     = 0.65;
            ld.param1[ui] = 0.0;  // BOTTOM type
            const int orr = ctx.link_subtypes.orifice_row(idx);
            if (orr >= 0) ctx.link_subtypes.orifices.cd[static_cast<std::size_t>(orr)] = 0.65;
            break;
        }
        case LinkType::WEIR: {
            ld.cd[ui]     = 3.33;  // standard transverse weir (US units)
            ld.param1[ui] = 0.0;   // TRANSVERSE
            const int wr = ctx.link_subtypes.weir_row(idx);
            if (wr >= 0) ctx.link_subtypes.weirs.cd[static_cast<std::size_t>(wr)] = 3.33;
            break;
        }
        case LinkType::OUTLET: {
            ld.cd[ui] = 1.0;
            const int olr = ctx.link_subtypes.outlet_row(idx);
            if (olr >= 0) ctx.link_subtypes.outlets.coeff[static_cast<std::size_t>(olr)] = 1.0;
            break;
        }
    }

    return result;
}

} // namespace openswmm::edit
