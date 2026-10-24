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
 * @file      NodesHandler.cpp
 * @brief     Section handlers for [JUNCTIONS], [OUTFALLS], [DIVIDERS], [STORAGE], [COORDINATES].
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
/**
 * @brief Grow base-node arrays through a registered index without resetting prefixes.
 *
 * Delegates grow_to(idx+1); quality/load matrices require separate sizing. Allocation can interrupt
 * column extension.
 *
 * @param[in,out] ctx Context whose NodeData is extended; registry/subtype/spatial stores are not
 *     resized here.
 * @param[in] idx Nonnegative registered node index <INT_MAX so idx+1 is representable.
 */
static void ensure_node_capacity(SimulationContext& ctx, int idx) {
    ctx.nodes.grow_to(idx + 1);
}

// Ensure spatial arrays are large enough
/**
 * @brief Extend only node X/Y coordinate columns to an expected count.
 *
 * Preserves existing values; does not grow node_has_xy or node/base-name stores. Independent vector
 * allocation can fail.
 *
 * @param[in,out] ctx Context whose spatial.node_x/node_y receive zero-filled new entries.
 * @param[in] n_nodes Nonnegative node count; unchecked size_t conversion.
 */
static void ensure_spatial_capacity(SimulationContext& ctx, int n_nodes) {
    const auto un = static_cast<std::size_t>(n_nodes);
    if (ctx.spatial.node_x.size() < un)
        ctx.spatial.node_x.resize(un, 0.0);
    if (ctx.spatial.node_y.size() < un)
        ctx.spatial.node_y.resize(un, 0.0);
}

// ============================================================================
// handle_junctions()
// ============================================================================

/**
 * @brief Parse ordinary junction definitions and retained object comments.
 *
 * Short rows skip; duplicate IDs append ERR_DUP_NAME through add_unique and retain the earlier
 * object. New rows and partial field writes are not rolled back on later refusal/allocation.
 * Registers name/grows node before checking optional depth/surcharge/ponded-area values for
 * negativity; rejected values therefore leave a default registered row. Invert may be negative.
 * Stores JUNCTION type, elevations/depths and ponded area without a complete finite/numeric check;
 * post-parse supplies topology/solver initialization.
 *
 * @param[in,out] ctx Model context receiving authored definitions and errors; caller supplies a
 *     coherent BUILDING/input phase and serializes edits.
 * @param[in] lines Borrowed section rows, copied/tokenized synchronously; lengths/areas/flows
 *     remain project display units until post-parse conversion.
 */
void handle_junctions(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Pre-reserve from the section's row count (an upper bound: some rows
    // are comments or duplicates). Capacity only — see reserve_to().
    ctx.nodes.reserve_to(ctx.nodes.count() + static_cast<int>(lines.size()));
    ctx.node_names.reserve(static_cast<std::size_t>(ctx.node_names.size()) + lines.size());
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 2)
            continue;

        const std::string& name = tok[0];

        // Register name; a second definition (any case spelling) is ERR 207
        int idx = add_unique(ctx.node_names, name, ctx.errors);
        if (idx < 0)
            continue; // duplicate ID (ERR 207, legacy input.c parity)

        ensure_node_capacity(ctx, idx);

        // legacy junc_readParams (node.c): the INVERT ELEVATION may be
        // negative, but max depth, initial depth, surcharge depth and ponded
        // area may not — "check for non-negative values (except for invert
        // elev.)" — and a negative one is ERROR 211 naming the offending token,
        // tested in column order once the row is read.
        //
        // Accepting them is not a lenient no-op. A negative max depth drives a
        // pathological run: 1218-nodes carries maxDepth = -612 on six junctions
        // and v6 simulated 21 days straight past the 1,800 s harness cap, where
        // legacy refuses the deck in 24 ms. 960-nodes is the same rule at
        // -0.100000.
        bool negative_param = false;
        for (std::size_t i = 2; i < tok.size() && i <= 5; ++i) {
            if (to_double(tok[i]) < 0.0) {
                ctx.errors.push_back(format_error(ERR_NUMBER, tok[i]));
                negative_param = true;
                break;
            }
        }
        if (negative_param)
            continue;

        ctx.node_subtypes.set_node_type(ctx.nodes, idx, NodeType::JUNCTION);
        ctx.nodes.invert_elev[idx] = to_double(tok[1]); // Elev
        if (tok.size() > 2)
            ctx.nodes.full_depth[idx] = to_double(tok[2]); // MaxDepth
        if (tok.size() > 3)
            ctx.nodes.init_depth[idx] = to_double(tok[3]); // InitDepth
        if (tok.size() > 4)
            ctx.nodes.sur_depth[idx] = to_double(tok[4]); // SurDepth
        if (tok.size() > 5)
            ctx.nodes.ponded_area[idx] = to_double(tok[5]); // Aponded
        if (!pl.comment.empty())
            ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_virtual_junctions()
// ============================================================================

/**
 * @brief Parse virtual-junction authoring flags and optional drawing rim depth.
 *
 * Short rows skip; duplicate IDs append ERR_DUP_NAME through add_unique and retain the earlier
 * object. New rows and partial field writes are not rolled back on later refusal/allocation.
 * Accepts NAME INVERT [RIM_DEPTH]; >3 tokens appends ERR_VJ_EXTRA_TOKENS before registration.
 * Creates JUNCTION type with is_virtual=1. Rim depth is max(0,to_double), used for rendering;
 * hydraulic properties derive from attached conduits later. No topology/coupling validation here.
 *
 * @param[in,out] ctx Model context receiving authored definitions and errors; caller supplies a
 *     coherent BUILDING/input phase and serializes edits.
 * @param[in] lines Borrowed section rows, copied/tokenized synchronously; lengths/areas/flows
 *     remain project display units until post-parse conversion.
 */
void handle_virtual_junctions(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 2)
            continue;

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
        if (idx < 0)
            continue; // duplicate ID (ERR 207, legacy input.c parity)

        ensure_node_capacity(ctx, idx);

        ctx.node_subtypes.set_node_type(ctx.nodes, idx, NodeType::JUNCTION);
        ctx.nodes.is_virtual[static_cast<std::size_t>(idx)] = 1;
        ctx.nodes.invert_elev[idx] = to_double(tok[1]);
        // MaxDepth: rim/ground elevation for drawings only — never read by the
        // solver, which uses the derived pipe crown (NodeData::rim_depth).
        // Negatives and unparseable text collapse to 0 = unset.
        if (tok.size() > 2)
            ctx.nodes.rim_depth[static_cast<std::size_t>(idx)] = std::max(0.0, to_double(tok[2]));
        if (!pl.comment.empty())
            ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_outfalls()
// ============================================================================

/**
 * @brief Parse outfall boundary type, stage/reference and optional gate/route target.
 *
 * Short rows skip; duplicate IDs append ERR_DUP_NAME through add_unique and retain the earlier
 * object. New rows and partial field writes are not rolled back on later refusal/allocation.
 * Creates OUTFALL subtype; unknown boundary token keeps default FREE. FIXED stores project-length
 * stage; TIDAL/TIMESERIES retain the boundary table name with unresolved -1. FREE/NORMAL may
 * include a * stage placeholder before gate. Route-to subcatchment is looked up immediately and may
 * stay -1; no target creation or full boundary validation.
 *
 * @param[in,out] ctx Model context receiving authored definitions and errors; caller supplies a
 *     coherent BUILDING/input phase and serializes edits.
 * @param[in] lines Borrowed section rows, copied/tokenized synchronously; lengths/areas/flows
 *     remain project display units until post-parse conversion.
 */
void handle_outfalls(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // Pre-reserve from the section's row count (an upper bound: some rows
    // are comments or duplicates). Capacity only — see reserve_to().
    ctx.nodes.reserve_to(ctx.nodes.count() + static_cast<int>(lines.size()));
    ctx.node_names.reserve(static_cast<std::size_t>(ctx.node_names.size()) + lines.size());
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 3)
            continue;

        const std::string& name = tok[0];
        int idx = add_unique(ctx.node_names, name, ctx.errors);
        if (idx < 0)
            continue; // duplicate ID (ERR 207, legacy input.c parity)

        ensure_node_capacity(ctx, idx);

        const auto orow = static_cast<std::size_t>(
            ctx.node_subtypes.set_node_type(ctx.nodes, idx, NodeType::OUTFALL));
        auto& O = ctx.node_subtypes.outfalls;
        ctx.nodes.invert_elev[idx] = to_double(tok[1]); // Elev

        // Type: FREE, NORMAL, FIXED, TIDAL, TIMESERIES
        const std::string otype = Tokenizer::to_upper(tok[2]);
        if (otype == "FREE")
            O.bc_type[orow] = OutfallType::FREE;
        else if (otype == "NORMAL")
            O.bc_type[orow] = OutfallType::NORMAL;
        else if (otype == "FIXED")
            O.bc_type[orow] = OutfallType::FIXED;
        else if (otype == "TIDAL")
            O.bc_type[orow] = OutfallType::TIDAL;
        else if (otype == "TIMESERIES")
            O.bc_type[orow] = OutfallType::TIMESERIES;

        // Canonical column layout (legacy outfall_readParams):
        //   name elev FIXED      stage    (gated) (routeTo)
        //   name elev TIDAL      curveID  (gated) (routeTo)
        //   name elev TIMESERIES tseriesID (gated) (routeTo)
        //   name elev FREE|NORMAL         (gated) (routeTo)
        // FREE/NORMAL carry no stage-data field, so the gate/routeTo columns
        // shift left by one for those types.
        const bool has_stage_field =
            (O.bc_type[orow] == OutfallType::FIXED || O.bc_type[orow] == OutfallType::TIDAL ||
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
                O.param[orow] = -1.0;
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
            O.route_to[orow] = sc; // may be -1 if not yet parsed
        }
        if (!pl.comment.empty())
            ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_dividers()
// ============================================================================

/**
 * @brief Parse divider methods, deferred references and type-specific hydraulic fields.
 *
 * Short rows skip; duplicate IDs append ERR_DUP_NAME through add_unique and retain the earlier
 * object. New rows and partial field writes are not rolled back on later refusal/allocation.
 * Retains diversion-link name and optional TABULAR curve for post-parse resolution. CUTOFF/TABULAR
 * use one method parameter, WEIR uses qMin/dhMax/coefficient, OVERFLOW none; optional
 * depth/initial/surcharge/ponded fields follow that method-dependent tail. Unknown method keeps
 * defaults; numeric parsing is permissive, without full range/topology validation.
 *
 * @param[in,out] ctx Model context receiving authored definitions and errors; caller supplies a
 *     coherent BUILDING/input phase and serializes edits.
 * @param[in] lines Borrowed section rows, copied/tokenized synchronously; lengths/areas/flows
 *     remain project display units until post-parse conversion.
 */
void handle_dividers(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 4)
            continue;

        const std::string& name = tok[0];
        int idx = add_unique(ctx.node_names, name, ctx.errors);
        if (idx < 0)
            continue; // duplicate ID (ERR 207, legacy input.c parity)

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
            if (tok.size() > 4)
                D.cutoff[drow] = to_double(tok[4]);
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
            if (tok.size() > 4)
                D.cutoff[drow] = to_double(tok[4]);
            if (tok.size() > 5)
                D.max_depth[drow] = to_double(tok[5]);
            if (tok.size() > 6)
                D.cd[drow] = to_double(tok[6]);
        }

        // The optional tail is "maxDepth initDepth surDepth aPond", read from
        // the first token AFTER the type-specific parameters — legacy's `n`
        // (node.c divider_readParams, into x[7..10]).  OVERFLOW takes NO type
        // parameter, so its tail starts at token 4; CUTOFF (qCutoff) and
        // TABULAR (curve name) take one, WEIR (qMin dhMax cWeir) takes three.
        //
        // Two defects lived here: OVERFLOW used 5, so MaxDepth was read from
        // the InitDepth column; and initDepth/surDepth/aPond were never read
        // at all, so every divider silently lost them on load — not just on
        // save, since the live model carried the zeros into the simulation.
        int tail = 5; // CUTOFF, TABULAR
        if (dtype == "OVERFLOW")
            tail = 4;
        else if (dtype == "WEIR")
            tail = 7;
        const int ntoks = static_cast<int>(tok.size());
        if (ntoks > tail)
            ctx.nodes.full_depth[ui] = to_double(tok[tail]);
        if (ntoks > tail + 1)
            ctx.nodes.init_depth[ui] = to_double(tok[tail + 1]);
        if (ntoks > tail + 2)
            ctx.nodes.sur_depth[ui] = to_double(tok[tail + 2]);
        if (ntoks > tail + 3)
            ctx.nodes.ponded_area[ui] = to_double(tok[tail + 3]);
        if (!pl.comment.empty())
            ctx.nodes.comments[ui] = pl.comment;
    }
}

// ============================================================================
// handle_storage()
// ============================================================================

/**
 * @brief Parse storage geometry, raw shape parameters and optional exfiltration.
 *
 * Short rows skip; duplicate IDs append ERR_DUP_NAME through add_unique and retain the earlier
 * object. New rows and partial field writes are not rolled back on later refusal/allocation.
 * Registers STORAGE with elevation/max/initial depth, then TABULAR curve name, FUNCTIONAL
 * coefficients or geometric p1/p2/p3 with derived quadratic coefficients when valid. Unknown shape
 * defaults FUNCTIONAL; invalid geometric derivation leaves default coefficients. Tail supplies
 * surcharge, evaporation fraction, then bare Ksat or suction/Ksat/IMD. Keeps raw project-unit
 * dimensions/comments; no final geometry/table/finite validation or exfiltration state
 * initialization.
 *
 * @param[in,out] ctx Model context receiving authored definitions and errors; caller supplies a
 *     coherent BUILDING/input phase and serializes edits.
 * @param[in] lines Borrowed section rows, copied/tokenized synchronously; lengths/areas/flows
 *     remain project display units until post-parse conversion.
 */
void handle_storage(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 4)
            continue;

        const std::string& name = tok[0];
        int idx = add_unique(ctx.node_names, name, ctx.errors);
        if (idx < 0)
            continue; // duplicate ID (ERR 207, legacy input.c parity)

        ensure_node_capacity(ctx, idx);

        const auto srow = static_cast<std::size_t>(
            ctx.node_subtypes.set_node_type(ctx.nodes, idx, NodeType::STORAGE));
        auto& S = ctx.node_subtypes.storages;
        ctx.nodes.invert_elev[idx] = to_double(tok[1]); // Elev
        ctx.nodes.full_depth[idx] = to_double(tok[2]);  // MaxDepth
        ctx.nodes.init_depth[idx] = to_double(tok[3]);  // InitDepth

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
            if (tok.size() > 5)
                S.a[srow] = to_double(tok[5]);
            if (tok.size() > 6)
                S.b[srow] = to_double(tok[6]);
            if (tok.size() > 7)
                S.c[srow] = to_double(tok[7]);
        } else {
            // CYLINDRICAL / CONICAL / PARABOLIC / PYRAMIDAL — three raw dimensions
            // (L, W, Z). Keep them verbatim for lossless round-trip AND derive the
            // quadratic area coefficients the solver evaluates (legacy discards the
            // raw values at this point; we don't).
            if (tok.size() > 5)
                S.p1[srow] = to_double(tok[5]);
            if (tok.size() > 6)
                S.p2[srow] = to_double(tok[6]);
            if (tok.size() > 7)
                S.p3[srow] = to_double(tok[7]);
            double a = 0.0, b = 0.0, c = 0.0;
            if (storage_shape_coeffs(sshape, S.p1[srow], S.p2[srow], S.p3[srow], a, b, c)) {
                S.a[srow] = a;
                S.b[srow] = b;
                S.c[srow] = c;
            }
            S.curve[srow] = -1;
        }

        // Optional: SurDepth, Fevap, then the exfiltration parameters —
        // TABULAR consumes one token for the curve name, every other shape
        // three numeric params (legacy storage_readParams n = 6 / 8).
        // legacy exfil_readStorageParams: ONE remaining token is a bare Ksat
        // (suction and IMD 0 → a constant Ks rate), otherwise three tokens
        // Psi Ksat IMD (Green-Ampt); Ksat 0 = no exfiltration. The third
        // token used to land in the dead `seep_rate` slot and the
        // Green-Ampt columns were never read, so every .inp storage
        // exfiltration (36 corpus decks) was silently ignored.
        const int param_offset = (sshape == StorageShape::TABULAR) ? 6 : 8;
        if (static_cast<int>(tok.size()) > param_offset)
            ctx.nodes.sur_depth[idx] = to_double(tok[param_offset]);
        if (static_cast<int>(tok.size()) > param_offset + 1)
            S.evap_frac[srow] = to_double(tok[param_offset + 1]);
        {
            const int n = param_offset + 2;
            const int ntoks = static_cast<int>(tok.size());
            if (ntoks == n + 1) {
                S.exfil_suction[srow] = 0.0;
                S.exfil_ksat[srow] = to_double(tok[n]);
                S.exfil_imd[srow] = 0.0;
            } else if (ntoks >= n + 3) {
                S.exfil_suction[srow] = to_double(tok[n]);
                S.exfil_ksat[srow] = to_double(tok[n + 1]);
                S.exfil_imd[srow] = to_double(tok[n + 2]);
            }
        }
        if (!pl.comment.empty())
            ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_coordinates()
// ============================================================================

/**
 * @brief Assign node map coordinates and mark explicit coordinate presence.
 *
 * Known node IDs overwrite X/Y (last duplicate wins) and set node_has_xy=1; short/unknown rows skip
 * silently. Grows coordinate/mask arrays as needed, preserving other nodes. Permissive numeric
 * parsing can write fallback values; no finite/range validation or node creation.
 *
 * @param[in,out] ctx Model context receiving authored definitions and errors; caller supplies a
 *     coherent BUILDING/input phase and serializes edits.
 * @param[in] lines Borrowed NODE X Y rows in authored map units, without CRS transformation.
 */
void handle_coordinates(SimulationContext& ctx, const std::vector<std::string>& lines) {
    ensure_spatial_capacity(ctx, ctx.node_names.size());

    // One row per node — hoist the token buffer so the loop allocates nothing
    // after the first row. Quoted node names fall back to the owned tokenizer,
    // which tokenize_views_into cannot handle; same result, more allocation.
    std::vector<std::string_view> tok;
    std::vector<std::string> quoted;

    for (const auto& line : lines) {
        if (line.find('"') == std::string::npos) {
            Tokenizer::tokenize_views_into(line, tok);
        } else {
            quoted = Tokenizer::tokenize(line);
            tok.assign(quoted.begin(), quoted.end());
        }
        if (tok.size() < 3)
            continue;

        const int idx = ctx.node_names.find(tok[0]);
        if (idx < 0)
            continue; // unknown node — silently skip

        ensure_spatial_capacity(ctx, idx + 1);

        ctx.spatial.node_x[idx] = to_double(tok[1]);
        ctx.spatial.node_y[idx] = to_double(tok[2]);
        if (ctx.spatial.node_has_xy.size() < ctx.spatial.node_x.size())
            ctx.spatial.node_has_xy.resize(ctx.spatial.node_x.size(), 0);
        ctx.spatial.node_has_xy[static_cast<std::size_t>(idx)] = 1; // G-X2
    }
}

} /* namespace openswmm::input */
