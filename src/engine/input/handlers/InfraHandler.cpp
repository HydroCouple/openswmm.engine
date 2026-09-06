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
 * @file InfraHandler.cpp
 * @brief Section handlers for [STREETS], [INLETS], [INLET_USAGE],
 *        [ADJUSTMENTS], [EVENTS].
 *
 * @see InfraHandler.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "InfraHandler.hpp"
#include "NodesHandler.hpp"
#include "../Tokenizer.hpp"
#include "../SectionParser.hpp"
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

        // One definition line per street; a second definition in any case
        // spelling is a duplicate ID (legacy input.c parity, ERR 207).
        bool dup = false;
        for (const auto& existing : ctx.streets.names) {
            if (ieq(existing, tok[0])) { dup = true; break; }
        }
        if (dup) {
            ctx.errors.push_back(format_error(ERR_DUP_NAME, tok[0]));
            continue;
        }

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

namespace {

/// Grate bar patterns, legacy inlet.c GrateTypeWords order.
const char* const kGrateTypeWords[] = {
    "P_BAR-50", "P_BAR-50x100", "P_BAR-30", "CURVED_VANE",
    "TILT_BAR-45", "TILT_BAR-30", "RETICULINE", "GENERIC"};

/// Curb-opening throat orientations, legacy inlet.c ThroatAngleWords order.
const char* const kThroatWords[] = {"HORIZONTAL", "INCLINED", "VERTICAL"};

/// -1 when `w` is not one of `words`.
int find_word(const char* const* words, int n, std::string_view w) {
    for (int i = 0; i < n; ++i)
        if (ieq(words[i], w)) return i;
    return -1;
}

/// Row index of the inlet design named `name`, or -1.
int find_inlet_design(const SimulationContext& ctx, std::string_view name) {
    for (int i = 0; i < ctx.inlets.count(); ++i)
        if (ieq(ctx.inlets.names[static_cast<std::size_t>(i)], name)) return i;
    return -1;
}

/// The optional tail shared by [INLET_USAGE] tokens 4-9 and
/// [INLET_JUNCTIONS] tokens 6-11 (plan §2.3).
struct UsageTail {
    int    num_inlets    = 1;
    int    placement     = 0;     ///< 0=AUTOMATIC 1=ON_GRADE 2=ON_SAG
    double clog_factor   = 1.0;
    double flow_limit    = 0.0;
    double local_depress = 0.0;
    double local_width   = 0.0;
};

/// Parse the tail beginning at token `first`. Pushes the legacy parse error
/// and returns false on the first bad token (inlet.c:405-431).
bool parse_inlet_usage_tail(SimulationContext& ctx,
                            const std::vector<std::string>& tok,
                            std::size_t first, UsageTail& out) {
    auto num = [&](std::size_t k, double& v, double lo, double hi) {
        v = to_double(tok[k], -1.0);
        if (v < lo || v > hi) {
            ctx.errors.push_back(format_error(ERR_NUMBER, tok[k]));
            return false;
        }
        return true;
    };

    if (tok.size() > first) {
        out.num_inlets = to_int(tok[first], 0);
        if (out.num_inlets < 1) {
            ctx.errors.push_back(format_error(ERR_NUMBER, tok[first]));
            return false;
        }
    }
    if (tok.size() > first + 1) {
        double pct = 0.0;
        if (!num(first + 1, pct, 0.0, 99.0)) return false;
        out.clog_factor = 1.0 - pct / 100.0;
    }
    if (tok.size() > first + 2 &&
        !num(first + 2, out.flow_limit, 0.0, 1.0e30)) return false;
    if (tok.size() > first + 3 &&
        !num(first + 3, out.local_depress, 0.0, 1.0e30)) return false;
    if (tok.size() > first + 4 &&
        !num(first + 4, out.local_width, 0.0, 1.0e30)) return false;
    if (tok.size() > first + 5) {
        static const char* const kPlacementWords[] = {"AUTOMATIC", "ON_GRADE", "ON_SAG"};
        out.placement = find_word(kPlacementWords, 3, tok[first + 5]);
        if (out.placement < 0) {
            ctx.errors.push_back(format_error(ERR_KEYWORD, tok[first + 5]));
            return false;
        }
    }
    return true;
}

void apply_usage_tail(InletUsageStore& u, int row, const UsageTail& t) {
    const auto ur = static_cast<std::size_t>(row);
    u.num_inlets[ur]    = t.num_inlets;
    u.placement[ur]     = t.placement;
    u.clog_factor[ur]   = t.clog_factor;
    u.flow_limit[ur]    = t.flow_limit;
    u.local_depress[ur] = t.local_depress;
    u.local_width[ur]   = t.local_width;
}

} // namespace

void handle_inlets(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Format varies by type (legacy inlet.c:321-327):
    //   ID  GRATE       Length  Width  GrateType  (OpenArea)  (SplashVeloc)
    //   ID  CURB        Length  Height (ThroatType)
    //   ID  SLOTTED     Length  Width
    //   ID  DROP_GRATE  Length  Width  GrateType  (OpenArea)  (SplashVeloc)
    //   ID  DROP_CURB   Length  Height
    //   ID  CUSTOM      CurveID
    // A GRATE line and a CURB line sharing one name are the legacy encoding of
    // a combination inlet: the second line MERGES into the first row and
    // retypes it COMBO (there is no COMBO keyword). Any other repeat of a name
    // is a duplicate ID, as for [STREETS].
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 3) continue;

        const std::string type = Tokenizer::to_upper(tok[1]);
        const bool is_grate   = (type == "GRATE" || type == "DROP_GRATE");
        const bool is_curb    = (type == "CURB"  || type == "DROP_CURB");
        const bool is_slotted = (type == "SLOTTED");
        const bool is_custom  = (type == "CUSTOM");
        if (!is_grate && !is_curb && !is_slotted && !is_custom) {
            ctx.errors.push_back(format_error(ERR_KEYWORD, tok[1]));
            continue;
        }

        // --- Duplicate name: merge a GRATE/CURB pair, reject anything else ---
        const int prev = find_inlet_design(ctx, tok[0]);
        if (prev >= 0) {
            const std::string& prev_type =
                ctx.inlets.inlet_type[static_cast<std::size_t>(prev)];
            const bool combo = (prev_type == "GRATE" && type == "CURB") ||
                               (prev_type == "CURB"  && type == "GRATE");
            if (!combo) {
                ctx.errors.push_back(format_error(ERR_DUP_NAME, tok[0]));
                continue;
            }
        }

        // --- Type-specific token count + range checks (legacy read*Params) ---
        double length = 0.0, width = 0.0, open_area = 0.0, splash = 0.0;
        int grate_type = -1, throat = 2;   // default VERTICAL

        if (is_grate || is_curb || is_slotted) {
            const std::size_t need = is_grate ? 5u : 4u;
            if (tok.size() < need) {
                ctx.errors.push_back(format_error(ERR_ITEMS, tok[0]));
                continue;
            }
            length = to_double(tok[2], -1.0);
            width  = to_double(tok[3], -1.0);   // CURB/DROP_CURB: opening height
            if (length <= 0.0) { ctx.errors.push_back(format_error(ERR_NUMBER, tok[2])); continue; }
            if (width  <= 0.0) { ctx.errors.push_back(format_error(ERR_NUMBER, tok[3])); continue; }
        }
        if (is_grate) {
            grate_type = find_word(kGrateTypeWords, 8, tok[4]);
            if (grate_type < 0) { ctx.errors.push_back(format_error(ERR_KEYWORD, tok[4])); continue; }
            if (grate_type == 7) {   // GENERIC: open area is required
                if (tok.size() < 6) { ctx.errors.push_back(format_error(ERR_ITEMS, tok[0])); continue; }
                open_area = to_double(tok[5], -1.0);
                if (open_area <= 0.0 || open_area > 1.0) {
                    ctx.errors.push_back(format_error(ERR_NUMBER, tok[5]));
                    continue;
                }
                if (tok.size() > 6) {
                    splash = to_double(tok[6], -1.0);
                    if (splash < 0.0) { ctx.errors.push_back(format_error(ERR_NUMBER, tok[6])); continue; }
                }
            }
        }
        if (type == "CURB" && tok.size() > 4) {   // DROP_CURB has no throat token
            throat = find_word(kThroatWords, 3, tok[4]);
            if (throat < 0) { ctx.errors.push_back(format_error(ERR_KEYWORD, tok[4])); continue; }
        }

        // --- Write (new row, or merge into the existing half of a COMBO) ---
        const int idx = (prev >= 0) ? prev : ctx.inlets.add_row(std::string(tok[0]), type);
        const auto ui = static_cast<std::size_t>(idx);
        if (prev >= 0) ctx.inlets.inlet_type[ui] = "COMBO";

        if (is_grate) {
            ctx.inlets.length[ui]       = length;
            ctx.inlets.width[ui]        = width;
            ctx.inlets.grate_type[ui]   = kGrateTypeWords[grate_type];
            ctx.inlets.open_area[ui]    = open_area;
            ctx.inlets.splash_veloc[ui] = splash;
        } else if (is_curb) {
            ctx.inlets.curb_length[ui] = length;
            ctx.inlets.curb_height[ui] = width;
            ctx.inlets.curb_throat[ui] = throat;
        } else if (is_slotted) {
            ctx.inlets.length[ui] = length;
            ctx.inlets.width[ui]  = width;
        } else {   // CUSTOM — the curve is resolved in PostParseResolver
            ctx.inlets.curve_id[ui] = std::string(tok[2]);
        }
        if (!pl.comment.empty() && ctx.inlets.comments[ui].empty())
            ctx.inlets.comments[ui] = pl.comment;
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
        if (link_idx < 0) { ctx.errors.push_back(format_error(ERR_NAME, tok[0])); continue; }

        const int design_idx = find_inlet_design(ctx, tok[1]);
        if (design_idx < 0) { ctx.errors.push_back(format_error(ERR_NAME, tok[1])); continue; }

        // The capture node may be declared after this section (legacy parsing
        // is order-independent), so an unresolved name is deferred to
        // PostParseResolver like the [INLET_JUNCTIONS] grammar's — ERR_NAME
        // there if it never appears.
        const int node_idx = ctx.node_names.find(tok[2]);

        UsageTail tail;
        if (!parse_inlet_usage_tail(ctx, tok, 3, tail)) continue;

        // street_index stays -1 here: it is a property of the host conduit's
        // cross-section and is resolved in PostParseResolver (plan §4.8).
        const int row = ctx.inlet_usages.add_row(link_idx, -1, design_idx, node_idx);
        apply_usage_tail(ctx.inlet_usages, row, tail);
        if (node_idx < 0)
            ctx.inlet_usages.pending_capture_name[static_cast<std::size_t>(row)] = tok[2];
    }
}

// ============================================================================
// handle_inlet_junctions()
// ============================================================================

void handle_inlet_junctions(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Format: Name Elev MaxDepth Inlet CaptureNode
    //         (#Inlets %Clog Qmax aLocal wLocal Placement)
    // Tokens 1-3 mirror [VIRTUAL_JUNCTIONS]; tokens 4-11 are the
    // [INLET_USAGE] tail, so both grammars share parse_inlet_usage_tail.
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 5) continue;

        const std::string& name = tok[0];
        if (tok.size() > 11) {
            ctx.errors.push_back(format_error(ERR_IJ_EXTRA_TOKENS, name));
            continue;
        }

        UsageTail tail;
        if (!parse_inlet_usage_tail(ctx, tok, 5, tail)) continue;

        const int idx = add_unique(ctx.node_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ctx.nodes.grow_to(idx + 1);
        ctx.node_subtypes.set_node_type(ctx.nodes, idx, NodeType::JUNCTION);
        const auto ui = static_cast<std::size_t>(idx);
        ctx.nodes.is_virtual[ui] = 1;
        ctx.nodes.is_inlet[ui]   = 1;
        ctx.nodes.invert_elev[ui] = to_double(tok[1]);
        // MaxDepth: the flood threshold above the street invert (plan D-E2);
        // like a virtual junction's it is carried in rim_depth, and a missing
        // or negative value collapses to 0 = derive from the street section.
        ctx.nodes.rim_depth[ui] = std::max(0.0, to_double(tok[2]));
        if (!pl.comment.empty()) ctx.nodes.comments[ui] = pl.comment;

        // The design and the capture node may be declared after this section,
        // so both names are resolved in PostParseResolver (623/625/627).
        const int row = ctx.inlet_usages.add_row(-1, idx, -1, -1);
        apply_usage_tail(ctx.inlet_usages, row, tail);
        const auto ur = static_cast<std::size_t>(row);
        ctx.inlet_usages.pending_design_name[ur]  = tok[3];
        ctx.inlet_usages.pending_capture_name[ur] = tok[4];
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
            const int pi = ctx.find_table_any(tok[2]);
            if (si >= 0 && pi >= 0) {
                const auto usi = static_cast<std::size_t>(si);
                if (usi >= ctx.subcatch_n_perv_pattern.size())
                    ctx.subcatch_n_perv_pattern.resize(usi + 1, -1);
                ctx.subcatch_n_perv_pattern[usi] = pi;
            }
        }
        else if (keyword == "DSTORE" && tok.size() >= 3) {
            const int si = ctx.subcatch_names.find(tok[1]);
            const int pi = ctx.find_table_any(tok[2]);
            if (si >= 0 && pi >= 0) {
                const auto usi = static_cast<std::size_t>(si);
                if (usi >= ctx.subcatch_d_store_pattern.size())
                    ctx.subcatch_d_store_pattern.resize(usi + 1, -1);
                ctx.subcatch_d_store_pattern[usi] = pi;
            }
        }
        else if (keyword == "INFIL" && tok.size() >= 3) {
            const int si = ctx.subcatch_names.find(tok[1]);
            const int pi = ctx.find_table_any(tok[2]);
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
