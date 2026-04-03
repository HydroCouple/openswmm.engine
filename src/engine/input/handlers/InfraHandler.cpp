/**
 * @file InfraHandler.cpp
 * @brief Section handlers for [STREETS], [INLETS], [INLET_USAGE],
 *        [ADJUSTMENTS], [EVENTS].
 *
 * @see InfraHandler.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "InfraHandler.hpp"
#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../InputParseUtils.hpp"
#include "../../core/DateTime.hpp"

#include <algorithm>
#include <charconv>

namespace openswmm::input {

// ============================================================================
// handle_streets()
// ============================================================================

void handle_streets(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Format: ID  Tcrown  Hcurb  Sx  nRoad  (Hdep  Wg  Sides  Tback  Sback  nBack)
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 5) continue;

        ctx.streets.names.push_back(std::string(tok[0]));
        ctx.streets.t_crown.push_back(to_double(tok[1]));
        ctx.streets.h_curb.push_back(to_double(tok[2]));
        ctx.streets.sx.push_back(to_double(tok[3]));
        ctx.streets.n_road.push_back(to_double(tok[4]));
        ctx.streets.gutter_depres.push_back(tok.size() > 5 ? to_double(tok[5]) : 0.0);
        ctx.streets.gutter_width.push_back(tok.size() > 6 ? to_double(tok[6]) : 0.0);
        ctx.streets.sides.push_back(tok.size() > 7 ? to_int(tok[7], 2) : 2);
        ctx.streets.back_width.push_back(tok.size() > 8 ? to_double(tok[8]) : 0.0);
        ctx.streets.back_slope.push_back(tok.size() > 9 ? to_double(tok[9]) : 0.0);
        ctx.streets.back_n.push_back(tok.size() > 10 ? to_double(tok[10]) : 0.0);
    }
}

// ============================================================================
// handle_inlets()
// ============================================================================

void handle_inlets(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Format varies by type:
    //   ID  GRATE       Length  Width  GrateType  (OpenArea)  (SplashVeloc)
    //   ID  CURB        Length  Height (ThroatType)
    //   ID  SLOTTED     Length  Width
    //   ID  DROP_GRATE  Length  Width  GrateType  (OpenArea)  (SplashVeloc)
    //   ID  DROP_CURB   Length  Height
    //   ID  CUSTOM      CurveID
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        std::string inlet_type = Tokenizer::to_upper(tok[1]);

        ctx.inlets.names.push_back(std::string(tok[0]));
        ctx.inlets.inlet_type.push_back(inlet_type);

        if (inlet_type == "CUSTOM") {
            // Custom inlet — tok[2] is curve ID, no numeric geometry
            ctx.inlets.length.push_back(0.0);
            ctx.inlets.width.push_back(0.0);
            ctx.inlets.grate_type.push_back(std::string(tok[2])); // curve ID stored here
            ctx.inlets.open_area.push_back(0.0);
            ctx.inlets.splash_veloc.push_back(0.0);
        } else {
            ctx.inlets.length.push_back(tok.size() > 2 ? to_double(tok[2]) : 0.0);
            ctx.inlets.width.push_back(tok.size() > 3 ? to_double(tok[3]) : 0.0);
            ctx.inlets.grate_type.push_back(tok.size() > 4 ? std::string(tok[4]) : "");
            ctx.inlets.open_area.push_back(tok.size() > 5 ? to_double(tok[5]) : 0.0);
            ctx.inlets.splash_veloc.push_back(tok.size() > 6 ? to_double(tok[6]) : 0.0);
        }
    }
}

// ============================================================================
// handle_inlet_usage()
// ============================================================================

void handle_inlet_usage(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Format: linkID  inletID  nodeID  (#Inlets  %Clog  Qmax  aLocal  wLocal  placement)
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const int link_idx = ctx.link_names.find(tok[0]);
        if (link_idx < 0) continue;

        // Find inlet design index by name
        int design_idx = -1;
        for (int i = 0; i < ctx.inlets.count(); ++i) {
            if (ctx.inlets.names[static_cast<std::size_t>(i)] == tok[1]) {
                design_idx = i;
                break;
            }
        }
        if (design_idx < 0) continue;

        const int node_idx = ctx.node_names.find(tok[2]);
        if (node_idx < 0) continue;

        int num_inlets = tok.size() > 3 ? to_int(tok[3], 1) : 1;
        double pct_clog = tok.size() > 4 ? to_double(tok[4]) : 0.0;
        double flow_limit = tok.size() > 5 ? to_double(tok[5]) : 0.0;
        double a_local = tok.size() > 6 ? to_double(tok[6]) : 0.0;
        double w_local = tok.size() > 7 ? to_double(tok[7]) : 0.0;

        int placement = 0; // AUTO
        if (tok.size() > 8) {
            std::string p = Tokenizer::to_upper(tok[8]);
            if (p == "ON_GRADE") placement = 1;
            else if (p == "ON_SAG") placement = 2;
        }

        ctx.inlet_usages.link_index.push_back(link_idx);
        ctx.inlet_usages.design_index.push_back(design_idx);
        ctx.inlet_usages.node_index.push_back(node_idx);
        ctx.inlet_usages.num_inlets.push_back(num_inlets);
        ctx.inlet_usages.placement.push_back(placement);
        ctx.inlet_usages.clog_factor.push_back(1.0 - pct_clog / 100.0);
        ctx.inlet_usages.flow_limit.push_back(flow_limit);
        ctx.inlet_usages.local_depress.push_back(a_local);
        ctx.inlet_usages.local_width.push_back(w_local);
        ctx.inlet_usages.street_index.push_back(-1);
    }
}

// ============================================================================
// handle_adjustments()
// ============================================================================

void handle_adjustments(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Ensure subcatchment pattern vectors are sized
    const auto ns = static_cast<std::size_t>(ctx.subcatch_names.size());
    if (ctx.subcatch_n_perv_pattern.size() < ns)
        ctx.subcatch_n_perv_pattern.resize(ns, -1);
    if (ctx.subcatch_d_store_pattern.size() < ns)
        ctx.subcatch_d_store_pattern.resize(ns, -1);
    if (ctx.subcatch_infil_pattern.size() < ns)
        ctx.subcatch_infil_pattern.resize(ns, -1);

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2) continue;

        std::string keyword = Tokenizer::to_upper(tok[0]);

        // Monthly arrays: TEMP, EVAP, RAIN, CONDUCT (need 13 tokens total)
        if (keyword == "TEMP" && tok.size() >= 13) {
            for (int i = 0; i < 12; ++i)
                ctx.adjust_temp[i] = to_double(tok[static_cast<std::size_t>(i + 1)]);
        }
        else if (keyword == "EVAP" && tok.size() >= 13) {
            for (int i = 0; i < 12; ++i)
                ctx.adjust_evap[i] = to_double(tok[static_cast<std::size_t>(i + 1)]);
        }
        else if (keyword == "RAIN" && tok.size() >= 13) {
            for (int i = 0; i < 12; ++i)
                ctx.adjust_rain[i] = to_double(tok[static_cast<std::size_t>(i + 1)]);
        }
        else if (keyword == "CONDUCT" && tok.size() >= 13) {
            for (int i = 0; i < 12; ++i) {
                double v = to_double(tok[static_cast<std::size_t>(i + 1)]);
                ctx.adjust_hydcon[i] = (v <= 0.0) ? 1.0 : v;
            }
        }
        // Subcatchment pattern assignments: N-PERV, DSTORE, INFIL
        else if (keyword == "N-PERV" && tok.size() >= 3) {
            const int si = ctx.subcatch_names.find(tok[1]);
            const int pi = ctx.table_names.find(tok[2]);
            if (si >= 0 && pi >= 0) {
                const auto usi = static_cast<std::size_t>(si);
                if (usi >= ctx.subcatch_n_perv_pattern.size())
                    ctx.subcatch_n_perv_pattern.resize(usi + 1, -1);
                ctx.subcatch_n_perv_pattern[usi] = pi;
            }
        }
        else if (keyword == "DSTORE" && tok.size() >= 3) {
            const int si = ctx.subcatch_names.find(tok[1]);
            const int pi = ctx.table_names.find(tok[2]);
            if (si >= 0 && pi >= 0) {
                const auto usi = static_cast<std::size_t>(si);
                if (usi >= ctx.subcatch_d_store_pattern.size())
                    ctx.subcatch_d_store_pattern.resize(usi + 1, -1);
                ctx.subcatch_d_store_pattern[usi] = pi;
            }
        }
        else if (keyword == "INFIL" && tok.size() >= 3) {
            const int si = ctx.subcatch_names.find(tok[1]);
            const int pi = ctx.table_names.find(tok[2]);
            if (si >= 0 && pi >= 0) {
                const auto usi = static_cast<std::size_t>(si);
                if (usi >= ctx.subcatch_infil_pattern.size())
                    ctx.subcatch_infil_pattern.resize(usi + 1, -1);
                ctx.subcatch_infil_pattern[usi] = pi;
            }
        }
    }
}


// ============================================================================
// handle_events()
// ============================================================================

void handle_events(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Format: StartDate  StartTime  EndDate  EndTime
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 4) continue;

        double start = parse_date(tok[0]) + parse_time_day_fraction(tok[1]);
        double end   = parse_date(tok[2]) + parse_time_day_fraction(tok[3]);

        if (start >= end) continue;

        ctx.events.push_back({start, end});
    }
}

} /* namespace openswmm::input */
