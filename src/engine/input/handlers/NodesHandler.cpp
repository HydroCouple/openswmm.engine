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
 * @file NodesHandler.cpp
 * @brief Section handlers for [JUNCTIONS], [OUTFALLS], [DIVIDERS], [STORAGE], [COORDINATES].
 *
 * ### [JUNCTIONS] format (legacy SWMM 5.x)
 * ```
 * ;; Name      Elev   MaxDepth  InitDepth  SurDepth  Aponded
 * J1           0.0    5.0       0.0        0.0       0.0
 * ```
 *
 * ### [VIRTUAL_JUNCTIONS] format (refactored engine only)
 * ```
 * ;; Name      Elev   [MaxDepth]
 * VJ1          9.0
 * VJ2          9.0    4.5
 * ```
 * MaxDepth is optional and RENDERING ONLY — it is the rim/ground depth a
 * viewer draws the surface at. The solver always uses the derived pipe crown.
 *
 * ### [OUTFALLS] format
 * ```
 * ;; Name      Elev   Type      Stage/Tseries  Gated  RouteTo
 * Out1         0.0    FREE
 * Out2         0.0    FIXED     1.5
 * Out3         0.0    TIMESERIES TSERIES1
 * ```
 *
 * ### [STORAGE] format
 * ```
 * ;; Name      Elev   MaxDepth  InitDepth  Shape   Curve/A1  A2  A0  SurDepth  Fevap  Seep
 * Pond1        0.0    10.0      0.0        TABULAR POND_CURVE
 * Pond2        0.0    5.0       0.0        FUNCTIONAL 100  0  50
 * ```
 *
 * @see Legacy reference: src/solver/input.c — readNode(), readOutfall(), readStorage()
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "NodesHandler.hpp"

#include "../Tokenizer.hpp"
#include "../SectionParser.hpp"
#include "../../core/ErrorCodes.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/NodeData.hpp"
#include "../../data/StorageGeometry.hpp"

#include "../InputParseUtils.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>

namespace openswmm::input {

// Ensure NodeData arrays are large enough for index `idx`
static void ensure_node_capacity(SimulationContext& ctx, int idx) {
    ctx.nodes.grow_to(idx + 1);
}

// Ensure spatial arrays are large enough
static void ensure_spatial_capacity(SimulationContext& ctx, int n_nodes) {
    const auto un = static_cast<std::size_t>(n_nodes);
    if (ctx.spatial.node_x.size() < un) ctx.spatial.node_x.resize(un, 0.0);
    if (ctx.spatial.node_y.size() < un) ctx.spatial.node_y.resize(un, 0.0);
}

// ============================================================================
// handle_junctions()
// ============================================================================

void handle_junctions(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Pre-reserve from the section's row count (an upper bound: some rows
    // are comments or duplicates). Capacity only — see reserve_to().
    ctx.nodes.reserve_to(ctx.nodes.count() + static_cast<int>(lines.size()));
    ctx.node_names.reserve(static_cast<std::size_t>(ctx.node_names.size()) + lines.size());
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 2) continue;

        const std::string& name = tok[0];

        // Register name; a second definition (any case spelling) is ERR 207
        int idx = add_unique(ctx.node_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_node_capacity(ctx, idx);

        ctx.node_subtypes.set_node_type(ctx.nodes, idx, NodeType::JUNCTION);
        ctx.nodes.invert_elev[idx] = to_double(tok[1]);                          // Elev
        if (tok.size() > 2) ctx.nodes.full_depth[idx]  = to_double(tok[2]);     // MaxDepth
        if (tok.size() > 3) ctx.nodes.init_depth[idx]  = to_double(tok[3]);     // InitDepth
        if (tok.size() > 4) ctx.nodes.sur_depth[idx]   = to_double(tok[4]);     // SurDepth
        if (tok.size() > 5) ctx.nodes.ponded_area[idx] = to_double(tok[5]);     // Aponded
        if (!pl.comment.empty())
            ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_virtual_junctions()
// ============================================================================

void handle_virtual_junctions(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 2) continue;

        const std::string& name = tok[0];

        // Name, invert elevation and an OPTIONAL rendering-only max depth.
        // Everything the solver uses is derived from the two attached
        // conduits, so a fourth token is still an error (keeps the format
        // extensible without ambiguity).
        if (tok.size() > 3) {
            ctx.errors.push_back(format_error(ERR_VJ_EXTRA_TOKENS, name));
            continue;
        }

        int idx = add_unique(ctx.node_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_node_capacity(ctx, idx);

        ctx.node_subtypes.set_node_type(ctx.nodes, idx, NodeType::JUNCTION);
        ctx.nodes.is_virtual[static_cast<std::size_t>(idx)] = 1;
        ctx.nodes.invert_elev[idx] = to_double(tok[1]);
        // MaxDepth: rim/ground elevation for drawings only — never read by the
        // solver, which uses the derived pipe crown (NodeData::rim_depth).
        // Negatives and unparseable text collapse to 0 = unset.
        if (tok.size() > 2)
            ctx.nodes.rim_depth[static_cast<std::size_t>(idx)] =
                std::max(0.0, to_double(tok[2]));
        if (!pl.comment.empty())
            ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_outfalls()
// ============================================================================

void handle_outfalls(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Pre-reserve from the section's row count (an upper bound: some rows
    // are comments or duplicates). Capacity only — see reserve_to().
    ctx.nodes.reserve_to(ctx.nodes.count() + static_cast<int>(lines.size()));
    ctx.node_names.reserve(static_cast<std::size_t>(ctx.node_names.size()) + lines.size());
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 3) continue;

        const std::string& name = tok[0];
        int idx = add_unique(ctx.node_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_node_capacity(ctx, idx);

        const auto orow = static_cast<std::size_t>(
            ctx.node_subtypes.set_node_type(ctx.nodes, idx, NodeType::OUTFALL));
        auto& O = ctx.node_subtypes.outfalls;
        ctx.nodes.invert_elev[idx] = to_double(tok[1]);  // Elev

        // Type: FREE, NORMAL, FIXED, TIDAL, TIMESERIES
        const std::string otype = Tokenizer::to_upper(tok[2]);
        if      (otype == "FREE")       O.bc_type[orow] = OutfallType::FREE;
        else if (otype == "NORMAL")     O.bc_type[orow] = OutfallType::NORMAL;
        else if (otype == "FIXED")      O.bc_type[orow] = OutfallType::FIXED;
        else if (otype == "TIDAL")      O.bc_type[orow] = OutfallType::TIDAL;
        else if (otype == "TIMESERIES") O.bc_type[orow] = OutfallType::TIMESERIES;

        // Canonical column layout (legacy outfall_readParams):
        //   name elev FIXED      stage    (gated) (routeTo)
        //   name elev TIDAL      curveID  (gated) (routeTo)
        //   name elev TIMESERIES tseriesID (gated) (routeTo)
        //   name elev FREE|NORMAL         (gated) (routeTo)
        // FREE/NORMAL carry no stage-data field, so the gate/routeTo columns
        // shift left by one for those types.
        const bool has_stage_field = (O.bc_type[orow] == OutfallType::FIXED  ||
                                      O.bc_type[orow] == OutfallType::TIDAL  ||
                                      O.bc_type[orow] == OutfallType::TIMESERIES);
        std::size_t next = 3;

        // FREE/NORMAL have no stage data, but the EPA GUI still emits the column
        // as a "*" placeholder (e.g. "OUT1 98.0 FREE * NO"). Skip it so the gate
        // and route-to columns stay aligned for both layouts.
        if (!has_stage_field && tok.size() > next && tok[next] == "*")
            ++next;

        if (has_stage_field && tok.size() > next) {
            if (O.bc_type[orow] == OutfallType::FIXED) {
                O.param[orow] = to_double(tok[next]);
            } else {
                // TIDAL / TIMESERIES: keep the name; PostParseResolver turns it
                // into a table index once [CURVES]/[TIMESERIES] have been read.
                // -1 (not 0) marks "unresolved" — 0 is a valid table index.
                O.param_name[orow] = tok[next];
                O.param[orow]      = -1.0;
            }
            ++next;
        }

        // Gated (YES/NO)
        if (tok.size() > next) {
            O.has_flap_gate[orow] = Tokenizer::parse_boolean(tok[next]);
            ++next;
        }

        // Route-to subcatchment (optional last field)
        if (tok.size() > next && !tok[next].empty() && tok[next] != "*") {
            int sc = ctx.subcatch_names.find(tok[next]);
            O.route_to[orow] = sc;  // may be -1 if not yet parsed
        }
        if (!pl.comment.empty())
            ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_dividers()
// ============================================================================

void handle_dividers(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 4) continue;

        const std::string& name = tok[0];
        int idx = add_unique(ctx.node_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_node_capacity(ctx, idx);
        auto ui = static_cast<std::size_t>(idx);

        const auto drow = static_cast<std::size_t>(
            ctx.node_subtypes.set_node_type(ctx.nodes, idx, NodeType::DIVIDER));
        auto& D = ctx.node_subtypes.dividers;
        ctx.nodes.invert_elev[ui] = to_double(tok[1]);

        // tok[2] = diversion link name (resolved in post-parse)
        // Store link name for deferred resolution
        const std::string& div_link_name = tok[2];
        D.link_name[drow] = div_link_name;
        int dl = ctx.link_names.find(div_link_name);
        D.link[drow] = dl; // may be -1 if not yet parsed

        // tok[3] = divider type
        const std::string dtype = Tokenizer::to_upper(tok[3]);
        if (dtype == "CUTOFF") {
            D.method[drow] = DividerType::CUTOFF;
            if (tok.size() > 4) D.cutoff[drow] = to_double(tok[4]);
        } else if (dtype == "OVERFLOW") {
            D.method[drow] = DividerType::OVERFLOW_DIV;
        } else if (dtype == "TABULAR") {
            D.method[drow] = DividerType::TABULAR;
            // tok[4] = curve name (deferred)
            if (tok.size() > 4) {
                D.curve_name[drow] = tok[4];
                int ci = ctx.find_curve(tok[4]);
                D.curve[drow] = ci; // may be -1
            }
        } else if (dtype == "WEIR") {
            D.method[drow] = DividerType::WEIR;
            // Legacy column order (node.c:1112): qMin dhMax cWeir — dhMax is
            // token 5 and the discharge coefficient token 6 (was swapped).
            if (tok.size() > 4) D.cutoff[drow]    = to_double(tok[4]);
            if (tok.size() > 5) D.max_depth[drow] = to_double(tok[5]);
            if (tok.size() > 6) D.cd[drow]        = to_double(tok[6]);
        }

        // MaxDepth after type-specific fields
        int md_offset = 5;
        if (dtype == "CUTOFF" || dtype == "OVERFLOW") md_offset = 5;
        else if (dtype == "TABULAR") md_offset = 5;
        else if (dtype == "WEIR") md_offset = 7;
        if (static_cast<int>(tok.size()) > md_offset)
            ctx.nodes.full_depth[ui] = to_double(tok[md_offset]);
        if (!pl.comment.empty())
            ctx.nodes.comments[ui] = pl.comment;
    }
}

// ============================================================================
// handle_storage()
// ============================================================================

void handle_storage(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 4) continue;

        const std::string& name = tok[0];
        int idx = add_unique(ctx.node_names, name, ctx.errors);
        if (idx < 0) continue;  // duplicate ID (ERR 207, legacy input.c parity)

        ensure_node_capacity(ctx, idx);

        const auto srow = static_cast<std::size_t>(
            ctx.node_subtypes.set_node_type(ctx.nodes, idx, NodeType::STORAGE));
        auto& S = ctx.node_subtypes.storages;
        ctx.nodes.invert_elev[idx] = to_double(tok[1]);  // Elev
        ctx.nodes.full_depth[idx]  = to_double(tok[2]);  // MaxDepth
        ctx.nodes.init_depth[idx]  = to_double(tok[3]);  // InitDepth

        if (tok.size() < 5) {
            if (!pl.comment.empty())
                ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
            continue;
        }
        const std::string shape = Tokenizer::to_upper(tok[4]);

        // Unknown keyword ⇒ leave the row at its FUNCTIONAL default, matching how
        // every other handler here treats an unrecognised keyword (e.g. LinksHandler's
        // SHAPE_MAP miss): these handlers have no error channel to report into.
        StorageShape sshape = StorageShape::FUNCTIONAL;
        storage_shape_from_keyword(shape, sshape);
        S.shape[srow] = sshape;

        if (sshape == StorageShape::TABULAR) {
            // Next token is curve name — resolve to index in post-parse pass
            S.curve[srow] = -1;
            if (tok.size() > 5)
                S.curve_name[srow] = tok[5];
        } else if (sshape == StorageShape::FUNCTIONAL) {
            // A1, A2, A0
            if (tok.size() > 5) S.a[srow] = to_double(tok[5]);
            if (tok.size() > 6) S.b[srow] = to_double(tok[6]);
            if (tok.size() > 7) S.c[srow] = to_double(tok[7]);
        } else {
            // CYLINDRICAL / CONICAL / PARABOLIC / PYRAMIDAL — three raw dimensions
            // (L, W, Z). Keep them verbatim for lossless round-trip AND derive the
            // quadratic area coefficients the solver evaluates (legacy discards the
            // raw values at this point; we don't).
            if (tok.size() > 5) S.p1[srow] = to_double(tok[5]);
            if (tok.size() > 6) S.p2[srow] = to_double(tok[6]);
            if (tok.size() > 7) S.p3[srow] = to_double(tok[7]);
            double a = 0.0, b = 0.0, c = 0.0;
            if (storage_shape_coeffs(sshape, S.p1[srow], S.p2[srow], S.p3[srow], a, b, c)) {
                S.a[srow] = a;
                S.b[srow] = b;
                S.c[srow] = c;
            }
            S.curve[srow] = -1;
        }

        // Optional: SurDepth, Fevap, Seep — TABULAR consumes one token for the curve
        // name, every other shape consumes three numeric params.
        const int param_offset = (sshape == StorageShape::TABULAR) ? 6 : 8;
        if (static_cast<int>(tok.size()) > param_offset)
            ctx.nodes.sur_depth[idx] = to_double(tok[param_offset]);
        if (static_cast<int>(tok.size()) > param_offset + 1)
            S.evap_frac[srow] = to_double(tok[param_offset + 1]);
        if (static_cast<int>(tok.size()) > param_offset + 2)
            S.seep_rate[srow] = to_double(tok[param_offset + 2]);
        if (!pl.comment.empty())
            ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_coordinates()
// ============================================================================

void handle_coordinates(SimulationContext& ctx, const std::vector<std::string>& lines) {
    ensure_spatial_capacity(ctx, ctx.node_names.size());

    // One row per node — hoist the token buffer so the loop allocates nothing
    // after the first row. Quoted node names fall back to the owned tokenizer,
    // which tokenize_views_into cannot handle; same result, more allocation.
    std::vector<std::string_view> tok;
    std::vector<std::string>      quoted;

    for (const auto& line : lines) {
        if (line.find('"') == std::string::npos) {
            Tokenizer::tokenize_views_into(line, tok);
        } else {
            quoted = Tokenizer::tokenize(line);
            tok.assign(quoted.begin(), quoted.end());
        }
        if (tok.size() < 3) continue;

        const int idx = ctx.node_names.find(tok[0]);
        if (idx < 0) continue;  // unknown node — silently skip

        ensure_spatial_capacity(ctx, idx + 1);

        ctx.spatial.node_x[idx] = to_double(tok[1]);
        ctx.spatial.node_y[idx] = to_double(tok[2]);
    }
}

} /* namespace openswmm::input */
