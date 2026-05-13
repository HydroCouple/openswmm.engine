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

static void clear_outfall_fields(NodeData& nd, std::size_t ui,
                                  std::vector<std::string>& cleared) {
    nd.outfall_type[ui]          = OutfallType::FREE;
    nd.outfall_param[ui]         = 0.0;
    nd.outfall_has_flap_gate[ui] = 0;
    nd.outfall_route_to[ui]      = -1;
    nd.outfall_link_idx[ui]      = -1;
    nd.outfall_link_offset[ui]   = 0.0;
    cleared.insert(cleared.end(), {
        "outfall_type", "outfall_param", "outfall_has_flap_gate",
        "outfall_route_to", "outfall_link_idx", "outfall_link_offset"
    });
}

static void clear_storage_fields(NodeData& nd, std::size_t ui,
                                  std::vector<std::string>& cleared) {
    nd.storage_curve[ui]       = -1;
    nd.storage_curve_name[ui]  = {};
    nd.storage_a[ui]           = 0.0;
    nd.storage_b[ui]           = 0.0;
    nd.storage_c[ui]           = 0.0;
    nd.storage_seep_rate[ui]   = 0.0;
    nd.storage_evap_frac[ui]   = 0.0;
    nd.storage_evap_loss[ui]   = 0.0;
    nd.storage_exfil_loss[ui]  = 0.0;
    nd.exfil_suction[ui]       = 0.0;
    nd.exfil_ksat[ui]          = 0.0;
    nd.exfil_imd[ui]           = 0.0;
    cleared.insert(cleared.end(), {
        "storage_curve", "storage_a", "storage_b", "storage_c",
        "storage_seep_rate", "storage_evap_frac",
        "exfil_suction", "exfil_ksat", "exfil_imd"
    });
}

static void clear_divider_fields(NodeData& nd, std::size_t ui,
                                  std::vector<std::string>& cleared) {
    nd.divider_type[ui]       = DividerType::CUTOFF;
    nd.divider_cutoff[ui]     = 0.0;
    nd.divider_cd[ui]         = 0.0;
    nd.divider_max_depth[ui]  = 0.0;
    nd.divider_curve[ui]      = -1;
    nd.divider_link[ui]       = -1;
    nd.divider_link_name[ui]  = {};
    nd.divider_curve_name[ui] = {};
    cleared.insert(cleared.end(), {
        "divider_type", "divider_cutoff", "divider_cd", "divider_max_depth",
        "divider_curve", "divider_link"
    });
}

static void apply_outfall_defaults(NodeData& nd, std::size_t ui) {
    nd.outfall_type[ui]          = OutfallType::FREE;
    nd.outfall_param[ui]         = 0.0;
    nd.outfall_has_flap_gate[ui] = 0;
    nd.outfall_route_to[ui]      = -1;
    nd.outfall_link_idx[ui]      = -1;
}

static void apply_storage_defaults(NodeData& nd, std::size_t ui) {
    nd.storage_curve[ui]     = -1;
    nd.storage_a[ui]         = 0.0;
    nd.storage_b[ui]         = 0.0;
    nd.storage_c[ui]         = 0.0;
    nd.storage_seep_rate[ui] = 0.0;
    nd.storage_evap_frac[ui] = 0.0;
}

static void apply_divider_defaults(NodeData& nd, std::size_t ui) {
    nd.divider_type[ui]   = DividerType::CUTOFF;
    nd.divider_cutoff[ui] = 0.0;
    nd.divider_link[ui]   = -1;
    nd.divider_curve[ui]  = -1;
}

ConversionResult convert_node(SimulationContext& ctx, int idx, NodeType new_type) {
    ConversionResult result;
    result.new_type = internal_to_c_node_type(new_type);
    const auto ui = static_cast<std::size_t>(idx);
    NodeData& nd = ctx.nodes;
    const NodeType old_type = nd.type[ui];

    // Clear old type-specific fields
    switch (old_type) {
        case NodeType::OUTFALL:  clear_outfall_fields(nd, ui, result.cleared_fields); break;
        case NodeType::STORAGE:  clear_storage_fields(nd, ui, result.cleared_fields); break;
        case NodeType::DIVIDER:
            // Nullify any link whose divider_link points to this node
            for (int i = 0; i < ctx.n_nodes(); ++i) {
                if (i == idx) continue;
                if (ctx.nodes.divider_link[static_cast<std::size_t>(i)] == idx) {
                    result.warnings.push_back(
                        "Node " + ctx.node_names.name_of(i) +
                        ": divider_link reference cleared because source node changed type");
                }
            }
            clear_divider_fields(nd, ui, result.cleared_fields);
            break;
        default: break;
    }

    // Apply new type defaults
    switch (new_type) {
        case NodeType::OUTFALL:  apply_outfall_defaults(nd, ui);  break;
        case NodeType::STORAGE:  apply_storage_defaults(nd, ui);  break;
        case NodeType::DIVIDER: {
            apply_divider_defaults(nd, ui);
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
            break;
        }
        default: break;
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

    nd.type[ui] = new_type;
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

    // Apply new type defaults
    switch (new_type) {
        case LinkType::CONDUIT:
            ld.roughness[ui]    = 0.013;
            ld.xsect_shape[ui]  = XsectShape::CIRCULAR;
            ld.xsect_y_full[ui] = 1.0;
            ld.barrels[ui]      = 1;
            if (ld.length[ui] <= 0.0)
                result.warnings.push_back("Conduit length is 0; set a valid length before initializing");
            break;
        case LinkType::PUMP:
            ld.pump_curve[ui]      = -1;
            ld.pump_init_state[ui] = false;
            ld.pump_startup[ui]    = 0.0;
            ld.pump_shutoff[ui]    = 0.0;
            ld.pump_curve_type[ui] = -1;
            result.warnings.push_back("Pump curve is unset (-1); assign a pump curve before initializing");
            break;
        case LinkType::ORIFICE:
            ld.cd[ui]     = 0.65;
            ld.param1[ui] = 0.0;  // BOTTOM type
            break;
        case LinkType::WEIR:
            ld.cd[ui]     = 3.33;  // standard transverse weir (US units)
            ld.param1[ui] = 0.0;   // TRANSVERSE
            break;
        case LinkType::OUTLET:
            ld.cd[ui] = 1.0;
            break;
    }

    ld.type[ui] = new_type;
    return result;
}

} // namespace openswmm::edit
