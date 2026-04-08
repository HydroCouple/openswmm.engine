/**
 * @file LinksHandler.cpp
 * @brief Section handlers for [CONDUITS], [PUMPS], [ORIFICES], [WEIRS], [OUTLETS], [XSECTIONS], [LOSSES], [TRANSECTS].
 *
 * ### [CONDUITS] format
 * ```
 * ;; Name   Node1   Node2   Length  Roughness  InOffset  OutOffset  InitFlow  MaxFlow
 * C1        J1      J2      100.0   0.013      0.0       0.0        0.0       0.0
 * ```
 *
 * ### [XSECTIONS] format
 * ```
 * ;; Link   Shape       Geom1  Geom2  Geom3  Geom4  Barrels
 * C1        CIRCULAR    1.0    0.0    0.0    0.0    1
 * ```
 *
 * @see Legacy reference: src/solver/input.c — readLink(), readXsect()
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "LinksHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/LinkData.hpp"
#include "../../data/InfraData.hpp"

#include "../InputParseUtils.hpp"

#include <charconv>
#include <string>
#include <unordered_map>

namespace openswmm::input {

static void ensure_link_capacity(SimulationContext& ctx, int idx) {
    const auto n = static_cast<std::size_t>(idx + 1);
    auto grow = [&](auto& vec, auto def) {
        if (vec.size() < n) vec.resize(n, def);
    };
    using LT = LinkType;
    using XS = XsectShape;
    using FC = FlowClass;
    grow(ctx.links.type,              LT::CONDUIT);
    grow(ctx.links.node1,             -1);
    grow(ctx.links.node2,             -1);
    grow(ctx.links.offset1,           0.0);
    grow(ctx.links.offset2,           0.0);
    grow(ctx.links.q0,                0.0);
    grow(ctx.links.q_limit,           0.0);
    grow(ctx.links.xsect_shape,       XS::CIRCULAR);
    grow(ctx.links.xsect_y_full,      0.0);
    grow(ctx.links.xsect_a_full,      0.0);
    grow(ctx.links.xsect_w_max,       0.0);
    grow(ctx.links.xsect_curve,       -1);
    grow(ctx.links.roughness,         0.013);
    grow(ctx.links.param1,            0.0);
    grow(ctx.links.length,            0.0);
    grow(ctx.links.mod_length,        0.0);
    grow(ctx.links.slope,             0.0);
    grow(ctx.links.barrels,           1);
    grow(ctx.links.beta,              0.0);
    grow(ctx.links.rough_factor,      0.0);
    grow(ctx.links.q_full,            0.0);
    grow(ctx.links.xsect_r_full,      0.0);
    grow(ctx.links.xsect_s_full,      0.0);
    grow(ctx.links.setting,           1.0);
    grow(ctx.links.target_setting,    1.0);
    grow(ctx.links.direction,         1);
    grow(ctx.links.loss_inlet,        0.0);
    grow(ctx.links.loss_outlet,       0.0);
    grow(ctx.links.loss_avg,          0.0);
    grow(ctx.links.has_flap_gate,     false);
    grow(ctx.links.seep_rate,         0.0);
    grow(ctx.links.evap_loss_rate,    0.0);
    grow(ctx.links.seep_loss_rate,    0.0);
    grow(ctx.links.culvert_code,      0);
    grow(ctx.links.normal_flow_limited, false);
    grow(ctx.links.inlet_control,     false);
    grow(ctx.links.stat_norm_ltd,     0L);
    grow(ctx.links.stat_inlet_ctrl,   0L);
    grow(ctx.links.dqdh,              0.0);
    grow(ctx.links.pump_curve,        -1);
    grow(ctx.links.pump_init_state,   false);
    grow(ctx.links.pump_startup,      0.0);
    grow(ctx.links.pump_shutoff,      0.0);
    if (ctx.links.pump_curve_name.size() < n) ctx.links.pump_curve_name.resize(n);
    grow(ctx.links.crest_height,      0.0);
    grow(ctx.links.cd,                0.0);
    grow(ctx.links.param2,            0.0);
    grow(ctx.links.flow,              0.0);
    grow(ctx.links.depth,             0.0);
    grow(ctx.links.volume,            0.0);
    grow(ctx.links.froude,            0.0);
    grow(ctx.links.flow_class,        FC::DRY);
    grow(ctx.links.is_closed,         false);
    grow(ctx.links.old_flow,          0.0);
    grow(ctx.links.old_depth,         0.0);
    grow(ctx.links.old_volume,        0.0);
    grow(ctx.links.stat_vol_flow,     0.0);
    grow(ctx.links.stat_max_flow,     0.0);
    grow(ctx.links.stat_max_veloc,    0.0);
    grow(ctx.links.stat_max_filling,  0.0);
    grow(ctx.links.stat_time_surcharged, 0.0);
    grow(ctx.links.stat_max_flow_date,    0.0);
    grow(ctx.links.stat_time_full_upstream,  0.0);
    grow(ctx.links.stat_time_full_dnstream,  0.0);
    grow(ctx.links.stat_time_full_both,      0.0);
    grow(ctx.links.stat_time_capacity_limited, 0.0);

    // Cross-section extended params (used by handle_xsections and PostParseResolver)
    grow(ctx.links.xsect_y_bot,       0.0);
    grow(ctx.links.xsect_a_bot,       0.0);
    grow(ctx.links.xsect_s_bot,       0.0);
    grow(ctx.links.xsect_r_bot,       0.0);
    grow(ctx.links.xsect_s_max,       0.0);
    grow(ctx.links.xsect_yw_max,      0.0);
    grow(ctx.links.xsect_batch_shape, 0);

    // Computed hydraulic properties (set by PostParseResolver)
    grow(ctx.links.rough_factor,      0.0);
    grow(ctx.links.q_max,             0.0);

    // Orifice opening rate (used by control rule transitions)
    grow(ctx.links.orate,             0.0);

    // Statistics: flow classification flat array
    // [link_idx * N_FLOW_CLASSES + class_idx]
    {
        const auto nfc = n * static_cast<std::size_t>(LinkData::N_FLOW_CLASSES);
        if (ctx.links.stat_flow_class.size() < nfc)
            ctx.links.stat_flow_class.resize(nfc, 0.0);
    }
}

// ============================================================================
// handle_conduits()
// ============================================================================

void handle_conduits(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 7) continue;  // Name Node1 Node2 Length Roughness In Out required

        const std::string& name = tok[0];
        int idx = ctx.link_names.find(name);
        if (idx < 0) idx = ctx.link_names.add(name);

        ensure_link_capacity(ctx, idx);

        ctx.links.type[idx] = LinkType::CONDUIT;

        // Resolve node indices — nodes must be parsed before conduits
        ctx.links.node1[idx]     = ctx.node_names.find(tok[1]);
        ctx.links.node2[idx]     = ctx.node_names.find(tok[2]);
        ctx.links.length[idx]    = to_double(tok[3]);
        ctx.links.roughness[idx] = to_double(tok[4]);
        ctx.links.offset1[idx]   = to_double(tok[5]);
        ctx.links.offset2[idx]   = to_double(tok[6]);
        if (tok.size() > 7) ctx.links.q0[idx]      = to_double(tok[7]);
        if (tok.size() > 8) ctx.links.q_limit[idx] = to_double(tok[8]);
    }
}

// ============================================================================
// handle_pumps()
// ============================================================================

void handle_pumps(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const std::string& name = tok[0];
        int idx = ctx.link_names.find(name);
        if (idx < 0) idx = ctx.link_names.add(name);

        ensure_link_capacity(ctx, idx);

        ctx.links.type[idx]  = LinkType::PUMP;
        ctx.links.node1[idx] = ctx.node_names.find(tok[1]);
        ctx.links.node2[idx] = ctx.node_names.find(tok[2]);
        // tok[3]: pump curve name — store for deferred resolution
        if (tok.size() > 3) {
            ctx.links.pump_curve_name[idx] = tok[3];
            ctx.links.pump_curve[idx] = ctx.table_names.find(tok[3]);
        }
        // tok[4]: init status (ON/OFF)
        if (tok.size() > 4) {
            ctx.links.pump_init_state[idx] =
                Tokenizer::to_upper(tok[4]) == "ON";
            double init_val = ctx.links.pump_init_state[idx] ? 1.0 : 0.0;
            ctx.links.setting[idx]        = init_val;
            ctx.links.target_setting[idx] = init_val;
        }
        // tok[5]: startup depth, tok[6]: shutoff depth
        if (tok.size() > 5)
            ctx.links.pump_startup[idx] = to_double(tok[5]);
        if (tok.size() > 6)
            ctx.links.pump_shutoff[idx] = to_double(tok[6]);
    }
}

// ============================================================================
// handle_orifices()
// ============================================================================

void handle_orifices(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const std::string& name = tok[0];
        int idx = ctx.link_names.find(name);
        if (idx < 0) idx = ctx.link_names.add(name);

        ensure_link_capacity(ctx, idx);

        ctx.links.type[idx]         = LinkType::ORIFICE;
        ctx.links.node1[idx]        = ctx.node_names.find(tok[1]);
        ctx.links.node2[idx]        = ctx.node_names.find(tok[2]);
        // tok[3]: SIDE or BOTTOM → store in param1 (0=BOTTOM, 1=SIDE)
        if (tok.size() > 3) {
            std::string otype = Tokenizer::to_upper(tok[3]);
            ctx.links.param1[idx] = (otype == "SIDE") ? 1.0 : 0.0;
        }
        // tok[4]: offset (height above invert)
        if (tok.size() > 4) ctx.links.offset1[idx]      = to_double(tok[4]);
        // tok[5]: discharge coefficient
        if (tok.size() > 5) ctx.links.cd[idx]            = to_double(tok[5]);
        // tok[6]: flap gate (YES/NO)
        if (tok.size() > 6) ctx.links.has_flap_gate[idx] = Tokenizer::to_upper(tok[6]) == "YES";
        // tok[7]: open/close time (seconds)
        if (tok.size() > 7) ctx.links.orate[idx]         = to_double(tok[7]);
    }
}

// ============================================================================
// handle_weirs()
// ============================================================================

void handle_weirs(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const std::string& name = tok[0];
        int idx = ctx.link_names.find(name);
        if (idx < 0) idx = ctx.link_names.add(name);

        ensure_link_capacity(ctx, idx);

        ctx.links.type[idx]  = LinkType::WEIR;
        ctx.links.node1[idx] = ctx.node_names.find(tok[1]);
        ctx.links.node2[idx] = ctx.node_names.find(tok[2]);
        // tok[3]: weir type (TRANSVERSE=0, SIDEFLOW=1, V-NOTCH=2, TRAPEZOIDAL=3)
        if (tok.size() > 3) {
            std::string wtype = Tokenizer::to_upper(tok[3]);
            if (wtype == "TRANSVERSE")  ctx.links.param1[idx] = 0.0;
            else if (wtype == "SIDEFLOW") ctx.links.param1[idx] = 1.0;
            else if (wtype == "V-NOTCH") ctx.links.param1[idx] = 2.0;
            else if (wtype == "TRAPEZOIDAL") ctx.links.param1[idx] = 3.0;
        }
        // tok[4]: crest height (above invert)
        if (tok.size() > 4) ctx.links.crest_height[idx] = to_double(tok[4]);
        // tok[5]: discharge coefficient
        if (tok.size() > 5) ctx.links.cd[idx]           = to_double(tok[5]);
        // tok[6]: flap gate (YES/NO)
        if (tok.size() > 6) ctx.links.has_flap_gate[idx] = Tokenizer::to_upper(tok[6]) == "YES";
        // tok[7]: end contractions
        if (tok.size() > 7) ctx.links.param2[idx]       = to_double(tok[7]);
    }
}

// ============================================================================
// handle_outlets()
// ============================================================================

void handle_outlets(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const std::string& name = tok[0];
        int idx = ctx.link_names.find(name);
        if (idx < 0) idx = ctx.link_names.add(name);

        ensure_link_capacity(ctx, idx);

        ctx.links.type[idx]         = LinkType::OUTLET;
        ctx.links.node1[idx]        = ctx.node_names.find(tok[1]);
        ctx.links.node2[idx]        = ctx.node_names.find(tok[2]);
        if (tok.size() > 3) ctx.links.crest_height[idx] = to_double(tok[3]);
        // tok[4]: TABULAR/FUNCTIONAL, tok[5]: curve name or C1, tok[6]: C2
        if (tok.size() > 5) ctx.links.cd[idx]    = to_double(tok[5]);
        if (tok.size() > 6) ctx.links.param2[idx] = to_double(tok[6]);
    }
}

// ============================================================================
// handle_xsections()
// ============================================================================

// Map of shape name → XsectShape enum
static const std::unordered_map<std::string, XsectShape> SHAPE_MAP = {
    {"CIRCULAR",        XsectShape::CIRCULAR},
    {"FILLED_CIRCULAR", XsectShape::FILLED_CIRCULAR},
    {"RECT_CLOSED",     XsectShape::RECT_CLOSED},
    {"RECT_OPEN",       XsectShape::RECT_OPEN},
    {"TRAPEZOIDAL",     XsectShape::TRAPEZOIDAL},
    {"TRIANGULAR",      XsectShape::TRIANGULAR},
    {"PARABOLIC",       XsectShape::PARABOLIC},
    {"POWER",           XsectShape::POWER},
    {"MODBASKETHANDLE", XsectShape::MODBASKETHANDLE},
    {"EGG",             XsectShape::EGGSHAPED},
    {"HORSESHOE",       XsectShape::HORSESHOE},
    {"GOTHIC",          XsectShape::GOTHIC},
    {"CATENARY",        XsectShape::CATENARY},
    {"SEMIELLIPTICAL",  XsectShape::SEMIELLIPTICAL},
    {"BASKETHANDLE",    XsectShape::BASKETHANDLE},
    {"SEMICIRCULAR",    XsectShape::SEMICIRCULAR},
    {"RECT_TRIANGULAR", XsectShape::RECT_TRIANG},
    {"RECT_TRIANG",     XsectShape::RECT_TRIANG},
    {"RECT_ROUND",      XsectShape::RECT_ROUND},
    {"HORIZ_ELLIPSE",   XsectShape::HORIZ_ELLIPSE},
    {"VERT_ELLIPSE",    XsectShape::VERT_ELLIPSE},
    {"ARCH",            XsectShape::ARCH},
    {"IRREGULAR",       XsectShape::IRREGULAR},
    {"CUSTOM",          XsectShape::CUSTOM},
    {"FORCE_MAIN",      XsectShape::FORCE_MAIN},
    {"STREET",          XsectShape::STREET_XSECT},
    {"DUMMY",           XsectShape::DUMMY},
};

void handle_xsections(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const int idx = ctx.link_names.find(tok[0]);
        if (idx < 0) continue;  // Unknown link — xsection appears before conduit?

        ensure_link_capacity(ctx, idx);

        const std::string shape_str = Tokenizer::to_upper(tok[1]);
        auto it = SHAPE_MAP.find(shape_str);
        if (it != SHAPE_MAP.end()) {
            ctx.links.xsect_shape[idx] = it->second;
        }

        // IRREGULAR shapes: tok[2] is transect name, not a dimension.
        // CUSTOM shapes: tok[2] = y_full, tok[3] = shape curve name.
        // Both need deferred resolution (TRANSECTS/CURVES may not be parsed yet).
        if (ctx.links.xsect_shape[idx] == XsectShape::IRREGULAR) {
            if (tok.size() > 2) {
                ctx.links.pump_curve_name[idx] = tok[2]; // Reuse field for transect name
                ctx.links.xsect_curve[idx] = -1;
            }
        } else if (ctx.links.xsect_shape[idx] == XsectShape::CUSTOM) {
            if (tok.size() > 2) ctx.links.xsect_y_full[idx] = to_double(tok[2]);
            if (tok.size() > 3) {
                ctx.links.pump_curve_name[idx] = tok[3]; // Shape curve name
                ctx.links.xsect_curve[idx] = -1;
            }
        } else {
            // Geom1 = full depth (diameter for circular, etc.)
            if (tok.size() > 2) ctx.links.xsect_y_full[idx] = to_double(tok[2]);
        }

        // Geom2 = width or second parameter (shape-dependent, skip for CUSTOM)
        if (ctx.links.xsect_shape[idx] != XsectShape::CUSTOM) {
            if (tok.size() > 3) ctx.links.xsect_w_max[idx]  = to_double(tok[3]);
        }

        // Geom3 = third shape parameter (triangle depth, side slope, etc.)
        if (tok.size() > 4) ctx.links.xsect_y_bot[idx]  = to_double(tok[4]);

        // Geom4 = fourth shape parameter (rBot, etc.)
        if (tok.size() > 5) ctx.links.xsect_r_bot[idx]  = to_double(tok[5]);

        // Barrels (number of identical conduits, default 1)
        if (tok.size() > 6) {
            int barrels = static_cast<int>(to_double(tok[6]));
            if (barrels > 0) ctx.links.barrels[idx] = barrels;
        }

        // Culvert code (optional, token 7)
        if (tok.size() > 7) {
            int cc = static_cast<int>(to_double(tok[7]));
            if (cc > 0) ctx.links.culvert_code[idx] = cc;
        }
    }
}

// ============================================================================
// handle_losses()
// ============================================================================

void handle_losses(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2) continue;  // At minimum: Link Kentry

        const int idx = ctx.link_names.find(tok[0]);
        if (idx < 0) continue;  // Unknown link

        ensure_link_capacity(ctx, idx);

        if (tok.size() > 1) ctx.links.loss_inlet[idx]    = to_double(tok[1]);
        if (tok.size() > 2) ctx.links.loss_outlet[idx]   = to_double(tok[2]);
        if (tok.size() > 3) ctx.links.loss_avg[idx]      = to_double(tok[3]);
        if (tok.size() > 4) ctx.links.has_flap_gate[idx] = Tokenizer::parse_boolean(tok[4]);
        if (tok.size() > 5) ctx.links.seep_rate[idx]     = to_double(tok[5]);
    }
}

// ============================================================================
// handle_transects()
// ============================================================================

void handle_transects(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Transect section uses 3-line blocks: NC, X1, GR
    // NC sets Manning's n values for the next transect.
    // X1 starts a new transect definition.
    // GR adds station-elevation pairs to the current transect.

    double nc_left    = 0.0;
    double nc_right   = 0.0;
    double nc_channel = 0.0;

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;

        const std::string keyword = Tokenizer::to_upper(tok[0]);

        if (keyword == "NC") {
            // NC  nLeft  nRight  nChannel
            if (tok.size() > 1) nc_left    = to_double(tok[1]);
            if (tok.size() > 2) nc_right   = to_double(tok[2]);
            if (tok.size() > 3) nc_channel = to_double(tok[3]);
        }
        else if (keyword == "X1") {
            // X1  name  nSta  xLeftBank  xRightBank  0  0  0  xFactor  yFactor
            if (tok.size() < 3) continue;

            const std::string& name = tok[1];

            ctx.transects.names.push_back(name);
            ctx.transects.n_left.push_back(nc_left);
            ctx.transects.n_right.push_back(nc_right);
            ctx.transects.n_channel.push_back(nc_channel);
            ctx.transects.x_left_bank.push_back(
                (tok.size() > 3) ? to_double(tok[3]) : 0.0);
            ctx.transects.x_right_bank.push_back(
                (tok.size() > 4) ? to_double(tok[4]) : 0.0);
            ctx.transects.x_factor.push_back(
                (tok.size() > 8) ? to_double(tok[8]) : 1.0);
            ctx.transects.y_factor.push_back(
                (tok.size() > 9) ? to_double(tok[9]) : 1.0);
            ctx.transects.stations.emplace_back();
            ctx.transects.elevations.emplace_back();
        }
        else if (keyword == "GR") {
            // GR  elev  station  elev  station ... (pairs)
            if (ctx.transects.names.empty()) continue;  // No active transect

            auto& sta  = ctx.transects.stations.back();
            auto& elev = ctx.transects.elevations.back();

            // Pairs start at tok[1]: elev station elev station ...
            for (std::size_t i = 1; i + 1 < tok.size(); i += 2) {
                elev.push_back(to_double(tok[i]));
                sta.push_back(to_double(tok[i + 1]));
            }
        }
    }
}

} /* namespace openswmm::input */
