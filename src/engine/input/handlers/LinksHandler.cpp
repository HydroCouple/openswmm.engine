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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "LinksHandler.hpp"

#include "../Tokenizer.hpp"
#include "../SectionParser.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/ErrorCodes.hpp"
#include "../../data/LinkData.hpp"
#include "../../data/InfraData.hpp"

#include "../InputParseUtils.hpp"

#include <charconv>
#include <string>
#include <unordered_map>

namespace openswmm::input {

static void ensure_link_capacity(SimulationContext& ctx, int idx) {
    ctx.links.grow_to(idx + 1);
}

// Resolve a link's end nodes and record the raw names for deferred
// re-resolution. Legacy parsing is order-independent, so a link section may
// precede [JUNCTIONS]/[OUTFALLS]/etc.; without the deferred pass the link is
// loaded silently orphaned (node1/node2 == -1) and the .inp writer then emits
// '*' in the FromNode/ToNode columns.
static void set_link_nodes(SimulationContext& ctx, int idx,
                           const std::string& n1, const std::string& n2) {
    ctx.links.node1[idx] = ctx.node_names.find(n1);
    ctx.links.node2[idx] = ctx.node_names.find(n2);
    ctx.pending_link_nodes.emplace_back(idx, std::make_pair(n1, n2));
}

// ============================================================================
// handle_conduits()
// ============================================================================

void handle_conduits(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Pre-reserve from the section's row count (an upper bound: some rows
    // are comments or duplicates). Capacity only — see reserve_to().
    ctx.links.reserve_to(ctx.links.count() + static_cast<int>(lines.size()));
    ctx.link_names.reserve(static_cast<std::size_t>(ctx.link_names.size()) + lines.size());
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 7) continue;  // Name Node1 Node2 Length Roughness In Out required

        const std::string& name = tok[0];
        int idx = add_unique(ctx.link_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_link_capacity(ctx, idx);

        const int cr = ctx.link_subtypes.set_link_type(ctx.links, idx, LinkType::CONDUIT);
        const auto ucr = static_cast<std::size_t>(cr);

        set_link_nodes(ctx, idx, tok[1], tok[2]);
        ctx.link_subtypes.conduits.length[ucr]    = to_double(tok[3]);
        ctx.link_subtypes.conduits.roughness[ucr] = to_double(tok[4]);
        ctx.links.offset1[idx]   = to_double(tok[5]);
        ctx.links.offset2[idx]   = to_double(tok[6]);
        if (tok.size() > 7) ctx.links.q0[idx]      = to_double(tok[7]);
        if (tok.size() > 8) ctx.links.q_limit[idx] = to_double(tok[8]);
        if (!pl.comment.empty())
            ctx.links.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_pumps()
// ============================================================================

void handle_pumps(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 3) continue;

        const std::string& name = tok[0];
        int idx = add_unique(ctx.link_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_link_capacity(ctx, idx);

        const int pr = ctx.link_subtypes.set_link_type(ctx.links, idx, LinkType::PUMP);
        const auto upr = static_cast<std::size_t>(pr);
        set_link_nodes(ctx, idx, tok[1], tok[2]);
        // tok[3]: pump curve name — store for deferred resolution.
        // "*" is the ideal-pump placeholder (legacy pump_readParams,
        // link.c:1437), not a curve name: leave curve at -1 so the pump is
        // typed IDEAL (curve_type 6) instead of raising ERR_NAME on a lookup
        // of "*". The .inp writer emits "*" for an unset curve, so rejecting
        // it here broke the save/re-open round-trip for ideal pumps.
        if (tok.size() > 3 && tok[3] != "*") {
            ctx.links.pump_curve_name[idx] = tok[3];
            ctx.link_subtypes.pumps.curve[upr] = ctx.find_curve(tok[3]);
        }
        // tok[4]: init status (ON/OFF)
        if (tok.size() > 4) {
            const bool on = Tokenizer::to_upper(tok[4]) == "ON";
            ctx.link_subtypes.pumps.init_state[upr] = on ? uint8_t{1} : uint8_t{0};
            double init_val = on ? 1.0 : 0.0;
            ctx.links.setting[idx]        = init_val;
            ctx.links.target_setting[idx] = init_val;
        }
        // tok[5]: startup depth, tok[6]: shutoff depth
        if (tok.size() > 5)
            ctx.link_subtypes.pumps.startup[upr] = to_double(tok[5]);
        if (tok.size() > 6)
            ctx.link_subtypes.pumps.shutoff[upr] = to_double(tok[6]);
        if (!pl.comment.empty())
            ctx.links.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_orifices()
// ============================================================================

void handle_orifices(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 3) continue;

        const std::string& name = tok[0];
        int idx = add_unique(ctx.link_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_link_capacity(ctx, idx);

        const int orr = ctx.link_subtypes.set_link_type(ctx.links, idx, LinkType::ORIFICE);
        const auto uorr = static_cast<std::size_t>(orr);
        set_link_nodes(ctx, idx, tok[1], tok[2]);
        // tok[3]: SIDE or BOTTOM → orifice_type (0=BOTTOM, 1=SIDE)
        if (tok.size() > 3)
            ctx.link_subtypes.orifices.orifice_type[uorr] =
                (Tokenizer::to_upper(tok[3]) == "SIDE") ? 1.0 : 0.0;
        // tok[4]: offset (height above invert)
        if (tok.size() > 4) ctx.links.offset1[idx]      = to_double(tok[4]);
        // tok[5]: discharge coefficient
        if (tok.size() > 5) ctx.link_subtypes.orifices.cd[uorr] = to_double(tok[5]);
        // tok[6]: flap gate (YES/NO)
        if (tok.size() > 6) ctx.links.has_flap_gate[idx] = Tokenizer::to_upper(tok[6]) == "YES";
        // tok[7]: open/close time (seconds)
        if (tok.size() > 7) ctx.link_subtypes.orifices.orate[uorr] = to_double(tok[7]);
        if (!pl.comment.empty())
            ctx.links.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_weirs()
// ============================================================================

void handle_weirs(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 3) continue;

        const std::string& name = tok[0];
        int idx = add_unique(ctx.link_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_link_capacity(ctx, idx);

        const int wr = ctx.link_subtypes.set_link_type(ctx.links, idx, LinkType::WEIR);
        const auto uwr = static_cast<std::size_t>(wr);
        set_link_nodes(ctx, idx, tok[1], tok[2]);
        // tok[3]: weir type (TRANSVERSE=0, SIDEFLOW=1, V-NOTCH=2, TRAPEZOIDAL=3)
        if (tok.size() > 3) {
            std::string wtype = Tokenizer::to_upper(tok[3]);
            double wt = 0.0;
            if (wtype == "SIDEFLOW") wt = 1.0;
            else if (wtype == "V-NOTCH") wt = 2.0;
            else if (wtype == "TRAPEZOIDAL") wt = 3.0;
            ctx.link_subtypes.weirs.weir_type[uwr] = wt;
        }
        // tok[4]: crest height (above invert)
        if (tok.size() > 4) ctx.link_subtypes.weirs.crest_height[uwr] = to_double(tok[4]);
        // tok[5]: discharge coefficient
        if (tok.size() > 5) ctx.link_subtypes.weirs.cd[uwr] = to_double(tok[5]);
        // tok[6]: flap gate (YES/NO)
        if (tok.size() > 6) ctx.links.has_flap_gate[idx] = Tokenizer::to_upper(tok[6]) == "YES";
        // tok[7]: end contractions
        if (tok.size() > 7) ctx.link_subtypes.weirs.end_contractions[uwr] = to_double(tok[7]);
        // tok[8]: end-section discharge coeff (legacy cDisch2, link.c weir_readParams x[5])
        if (tok.size() > 8 && tok[8] != "*")
            ctx.link_subtypes.weirs.cd2[uwr] = to_double(tok[8]);
        // tok[9]: can-surcharge flag (legacy x[6], default YES)
        if (tok.size() > 9 && tok[9] != "*")
            ctx.link_subtypes.weirs.can_surcharge[uwr] =
                (Tokenizer::to_upper(tok[9]) == "YES") ? uint8_t{1} : uint8_t{0};
        if (!pl.comment.empty())
            ctx.links.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_outlets()
// ============================================================================

void handle_outlets(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 3) continue;

        const std::string& name = tok[0];
        int idx = add_unique(ctx.link_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_link_capacity(ctx, idx);

        const int olr = ctx.link_subtypes.set_link_type(ctx.links, idx, LinkType::OUTLET);
        const auto uolr = static_cast<std::size_t>(olr);
        set_link_nodes(ctx, idx, tok[1], tok[2]);
        if (tok.size() > 3)
            ctx.link_subtypes.outlets.crest_height[uolr] = to_double(tok[3]);
        // tok[4]: type string (TABULAR/HEAD, TABULAR/DEPTH, FUNCTIONAL/HEAD, FUNCTIONAL/DEPTH)
        // tok[5]: curve name (TABULAR) or C1 coefficient (FUNCTIONAL)
        // tok[6]: C2 exponent (FUNCTIONAL only)
        // outlet_type: 0=FUNCTIONAL_HEAD, 1=FUNCTIONAL_DEPTH,
        //              2=TABULAR_HEAD,     3=TABULAR_DEPTH
        if (tok.size() > 4) {
            const auto& type_str = tok[4];
            bool is_tabular    = (type_str.find("TABULAR")    != std::string::npos);
            bool is_depth_based = (type_str.find("DEPTH")     != std::string::npos);
            ctx.link_subtypes.outlets.outlet_type[uolr] =
                is_tabular ? (is_depth_based ? 3.0 : 2.0) : (is_depth_based ? 1.0 : 0.0);
        }
        int outlet_type = static_cast<int>(ctx.link_subtypes.outlets.outlet_type[uolr]);
        bool is_tabular = (outlet_type >= 2);
        if (tok.size() > 5) {
            if (is_tabular) {
                // Store curve name in pump_curve_name for PostParseResolver
                auto uidx = static_cast<size_t>(idx);
                if (uidx < ctx.links.pump_curve_name.size())
                    ctx.links.pump_curve_name[uidx] = tok[5];
            } else {
                ctx.link_subtypes.outlets.coeff[uolr] = to_double(tok[5]);
            }
        }
        if (tok.size() > 6 && !is_tabular)
            ctx.link_subtypes.outlets.expon[uolr] = to_double(tok[6]);
        if (!pl.comment.empty())
            ctx.links.comments[static_cast<std::size_t>(idx)] = pl.comment;
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
    {"POLYGON",         XsectShape::POLYGON},
};

void handle_xsections(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const int idx = ctx.link_names.find(tok[0]);
        if (idx < 0) {
            // The link is declared in a section further down the file. Stash the
            // row for InputReader to re-dispatch once everything is parsed; a
            // silent drop here left the link with zero area and zero flow.
            ctx.deferred_section_rows.emplace_back("XSECTIONS", line);
            continue;
        }

        ensure_link_capacity(ctx, idx);

        const std::string shape_str = Tokenizer::to_upper(tok[1]);
        auto it = SHAPE_MAP.find(shape_str);
        if (it != SHAPE_MAP.end()) {
            ctx.links.xsect_shape[idx] = it->second;
        } else {
            // A bad shape keyword used to fall through silently, leaving the
            // link at its default-initialized CIRCULAR shape with Geom1-4
            // still parsed as if they were a circular pipe's diameter/etc —
            // e.g. a typo'd EGGSHAPED (correct keyword: EGG), or a nonsense
            // keyword, both silently became a 2 ft circular pipe with no
            // error and no warning. A bad [OPTIONS] keyword raises
            // ERR_KEYWORD; a bad shape keyword must too.
            ctx.errors.push_back(format_error(ERR_KEYWORD, shape_str));
            continue;
        }

        // IRREGULAR shapes: tok[2] is transect name, not a dimension.
        // STREET shapes:    tok[2] is street name, not a dimension.
        // CUSTOM shapes:    tok[2] = y_full, tok[3] = shape curve name.
        // POLYGON shapes:   tok[2] = scale, tok[3] = open-flag (both genuine
        //                   numbers, captured by the raw Geom1-4 block below);
        //                   curve name follows Barrels at tok[7], not tok[3]
        //                   like CUSTOM, since tok[3] here is real geometry.
        // All need deferred resolution (TRANSECTS/STREETS/CURVES may not be
        // parsed yet).
        if (ctx.links.xsect_shape[idx] == XsectShape::IRREGULAR ||
            ctx.links.xsect_shape[idx] == XsectShape::STREET_XSECT) {
            if (tok.size() > 2) {
                ctx.links.pump_curve_name[idx] = tok[2]; // Reuse field for transect/street name
                ctx.links.xsect_curve[idx] = -1;
            }
        } else if (ctx.links.xsect_shape[idx] == XsectShape::CUSTOM) {
            if (tok.size() > 2) ctx.links.xsect_y_full[idx] = to_double(tok[2]);
            if (tok.size() > 3) {
                ctx.links.pump_curve_name[idx] = tok[3]; // Shape curve name
                ctx.links.xsect_curve[idx] = -1;
            }
        } else if (ctx.links.xsect_shape[idx] == XsectShape::POLYGON) {
            // y_full/w_max come from PostParseResolver's compile() of the
            // boundary curve, not from these tokens — leave them at their
            // zero default rather than transiently holding scale/open-flag.
            if (tok.size() > 7) {
                ctx.links.pump_curve_name[idx] = tok[7]; // Boundary curve name
                ctx.links.xsect_cheb_idx[idx] = -1;
            } else {
                // A row short of tok[7] (e.g. Barrels omitted, which is
                // optional for every other shape) leaves pump_curve_name
                // empty; PostParseResolver's POLYGON block silently skips an
                // empty curve name, so without this the link would fall
                // through to a much later, harder-to-place "no usable
                // cross-section" error instead of pointing at the actual
                // malformed [XSECTIONS] row.
                ctx.errors.push_back(format_error(ERR_ITEMS, tok[0]));
                continue;
            }
        } else {
            // Geom1 = full depth (diameter for circular, etc.)
            if (tok.size() > 2) ctx.links.xsect_y_full[idx] = to_double(tok[2]);
        }

        // Geom2 = width or second parameter (shape-dependent, skip for CUSTOM/POLYGON)
        if (ctx.links.xsect_shape[idx] != XsectShape::CUSTOM &&
            ctx.links.xsect_shape[idx] != XsectShape::POLYGON) {
            if (tok.size() > 3) ctx.links.xsect_w_max[idx]  = to_double(tok[3]);
        }

        // Geom3 = third shape parameter (triangle depth, side slope, etc.)
        if (tok.size() > 4) ctx.links.xsect_y_bot[idx]  = to_double(tok[4]);

        // Geom4 = fourth shape parameter (rBot, etc.)
        if (tok.size() > 5) ctx.links.xsect_r_bot[idx]  = to_double(tok[5]);

        // Retain raw Geom1–Geom4 (display units) for lossless serialization —
        // the fields above are overwritten with derived geometry during init,
        // discarding e.g. a trapezoid's bottom width / side slopes.  Mirrors
        // swmm_link_set_xsect.  See LinkData::xsect_geom1.
        if (ctx.links.xsect_shape[idx] != XsectShape::IRREGULAR &&
            ctx.links.xsect_shape[idx] != XsectShape::STREET_XSECT) {
            if (tok.size() > 2) ctx.links.xsect_geom1[idx] = to_double(tok[2]);
            if (tok.size() > 3) ctx.links.xsect_geom2[idx] = to_double(tok[3]);
            if (tok.size() > 4) ctx.links.xsect_geom3[idx] = to_double(tok[4]);
            if (tok.size() > 5) ctx.links.xsect_geom4[idx] = to_double(tok[5]);
        }

        // Barrels (number of identical conduits, default 1). Cross-cutting
        // section: dual-write the conduit side-table row if it exists (the
        // [CONDUITS] handler created it); no-op for non-conduit links.
        const int cr = ctx.link_subtypes.conduit_row(idx);
        if (tok.size() > 6 && cr >= 0) {
            int barrels = static_cast<int>(to_double(tok[6]));
            if (barrels > 0)
                ctx.link_subtypes.conduits.barrels[static_cast<std::size_t>(cr)] = barrels;
        }

        // Culvert code (optional, token 7). Not for POLYGON — token 7 there
        // is the boundary curve name, not a number.
        if (tok.size() > 7 && cr >= 0 &&
            ctx.links.xsect_shape[idx] != XsectShape::POLYGON) {
            int cc = static_cast<int>(to_double(tok[7]));
            if (cc > 0)
                ctx.link_subtypes.conduits.culvert_code[static_cast<std::size_t>(cr)] = cc;
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
        if (idx < 0) {
            // Same deferral as [XSECTIONS] — the link may be declared later.
            ctx.deferred_section_rows.emplace_back("LOSSES", line);
            continue;
        }

        ensure_link_capacity(ctx, idx);

        const int cr = ctx.link_subtypes.conduit_row(idx);
        const auto ucr = static_cast<std::size_t>(cr);
        if (cr >= 0) {
            if (tok.size() > 1) ctx.link_subtypes.conduits.loss_inlet[ucr]  = to_double(tok[1]);
            if (tok.size() > 2) ctx.link_subtypes.conduits.loss_outlet[ucr] = to_double(tok[2]);
            if (tok.size() > 3) ctx.link_subtypes.conduits.loss_avg[ucr]    = to_double(tok[3]);
            if (tok.size() > 5) ctx.link_subtypes.conduits.seep_rate[ucr]   = to_double(tok[5]);
        }
        if (tok.size() > 4) ctx.links.has_flap_gate[idx] = Tokenizer::parse_boolean(tok[4]);
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
            //
            // A zero component means "unchanged from the preceding NC
            // record" (EPA SWMM 5.2.4; legacy transect.c::setManning), so
            // only positive values overwrite the active roughness. Zeroing
            // the stored values here wiped a previously declared channel
            // roughness and tripped ERR_TRANSECT_MANNING (227) during
            // validation for input EPA SWMM accepts.
            const double n_left    = (tok.size() > 1) ? to_double(tok[1]) : 0.0;
            const double n_right   = (tok.size() > 2) ? to_double(tok[2]) : 0.0;
            const double n_channel = (tok.size() > 3) ? to_double(tok[3]) : 0.0;
            // A negative component is invalid input, not an inheritance
            // request (legacy setManning returns ERR_NUMBER for it).
            if (n_left < 0.0) {
                ctx.errors.push_back(format_error(ERR_NUMBER, tok[1]));
                continue;
            }
            if (n_right < 0.0) {
                ctx.errors.push_back(format_error(ERR_NUMBER, tok[2]));
                continue;
            }
            if (n_channel < 0.0) {
                ctx.errors.push_back(format_error(ERR_NUMBER, tok[3]));
                continue;
            }
            if (n_left    > 0.0) nc_left    = n_left;
            if (n_right   > 0.0) nc_right   = n_right;
            if (n_channel > 0.0) nc_channel = n_channel;
            // overbank roughness defaults to the channel value, as in legacy
            if (nc_left  == 0.0) nc_left  = nc_channel;
            if (nc_right == 0.0) nc_right = nc_channel;
        }
        else if (keyword == "X1") {
            // Per EPA SWMM 5 (transect.c::setParams) the X1 layout is:
            //   X1  Name  Nsta  Xleft  Xright  0  0  Lfactor  Xfactor  Yfactor
            // Tokens:  1     2     3      4    5  6    7         8         9
            // (Only two placeholder zeros sit between Xright and Lfactor —
            // NOT three; the SWMM 5 user-manual line "0 0 0 Lfactor Wfactor
            // Eoff" misled an earlier read of this code.)
            //
            // SWMM also treats Lfactor==0 / Xfactor==0 as "use 1.0" — both
            // are multiplicative modifiers, and EPA SWMM-generated files
            // routinely emit zero placeholders here. Without the same
            // default-to-one fallback the chart collapses every station to
            // x*0 = 0 (the "vertical line" cross-section bug).
            if (tok.size() < 3) continue;

            const std::string& name = tok[1];

            // A second X1 with the same name (any case spelling) is a
            // duplicate ID in legacy (ERR 207). Record the error but keep
            // parsing so the GR/NC lines that follow stay attached to a
            // consistent transect slot; the open fails on ctx.errors anyway.
            for (const auto& existing : ctx.transects.names) {
                if (ieq(existing, name)) {
                    ctx.errors.push_back(format_error(ERR_DUP_NAME, name));
                    break;
                }
            }

            ctx.transects.names.push_back(name);
            // All parallel arrays in TransectStore must be kept in lock-step
            // with `names` — count() reports names.size() and downstream
            // accessors (swmm_transect_get_comments / _encroachment /
            // _modifiers) index every array by the same `ui`. Missing a
            // push_back here lets a subsequent get_*() read past end of an
            // empty vector and crash. INP doesn't supply comments /
            // encroachment, so default them at parse time.
            ctx.transects.comments.emplace_back();
            ctx.transects.n_left.push_back(nc_left);
            ctx.transects.n_right.push_back(nc_right);
            ctx.transects.n_channel.push_back(nc_channel);
            ctx.transects.x_left_bank.push_back(
                (tok.size() > 3) ? to_double(tok[3]) : 0.0);
            ctx.transects.x_right_bank.push_back(
                (tok.size() > 4) ? to_double(tok[4]) : 0.0);
            ctx.transects.x_left_encroachment.push_back(0.0);
            ctx.transects.x_right_encroachment.push_back(0.0);

            double lFactor = (tok.size() > 7) ? to_double(tok[7]) : 1.0;
            double xFactor = (tok.size() > 8) ? to_double(tok[8]) : 1.0;
            double yFactor = (tok.size() > 9) ? to_double(tok[9]) : 0.0;
            if (lFactor == 0.0) lFactor = 1.0;
            if (xFactor == 0.0) xFactor = 1.0;
            ctx.transects.length_factor.push_back(lFactor);
            ctx.transects.x_factor.push_back(xFactor);
            ctx.transects.y_factor.push_back(yFactor);

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
