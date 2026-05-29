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
 * @license  MIT License
 */

#include "NodesHandler.hpp"

#include "../Tokenizer.hpp"
#include "../SectionParser.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/NodeData.hpp"

#include "../InputParseUtils.hpp"

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
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 2) continue;

        const std::string& name = tok[0];

        // Register name (skip if already registered — handles duplicate lines)
        int idx = ctx.node_names.find(name);
        if (idx < 0) idx = ctx.node_names.add(name);

        ensure_node_capacity(ctx, idx);

        ctx.nodes.type[idx]       = NodeType::JUNCTION;
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
// handle_outfalls()
// ============================================================================

void handle_outfalls(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.size() < 3) continue;

        const std::string& name = tok[0];
        int idx = ctx.node_names.find(name);
        if (idx < 0) idx = ctx.node_names.add(name);

        ensure_node_capacity(ctx, idx);

        ctx.nodes.type[idx]       = NodeType::OUTFALL;
        ctx.nodes.invert_elev[idx] = to_double(tok[1]);  // Elev

        // Type: FREE, NORMAL, FIXED, TIDAL, TIMESERIES
        const std::string otype = Tokenizer::to_upper(tok[2]);
        if      (otype == "FREE")       ctx.nodes.outfall_type[idx] = OutfallType::FREE;
        else if (otype == "NORMAL")     ctx.nodes.outfall_type[idx] = OutfallType::NORMAL;
        else if (otype == "FIXED")      ctx.nodes.outfall_type[idx] = OutfallType::FIXED;
        else if (otype == "TIDAL")      ctx.nodes.outfall_type[idx] = OutfallType::TIDAL;
        else if (otype == "TIMESERIES") ctx.nodes.outfall_type[idx] = OutfallType::TIMESERIES;

        // Stage or curve reference
        if (tok.size() > 3) {
            // For FIXED: numeric stage; for TIDAL/TIMESERIES: curve/tseries name → resolve later
            if (ctx.nodes.outfall_type[idx] == OutfallType::FIXED) {
                ctx.nodes.outfall_param[idx] = to_double(tok[3]);
            }
            // For TIDAL / TIMESERIES, the name resolution is deferred to a post-parse pass
        }

        // Gated (YES/NO)
        if (tok.size() > 4) {
            ctx.nodes.outfall_has_flap_gate[idx] =
                Tokenizer::parse_boolean(tok[4]);
        }

        // Route-to subcatchment (optional last field)
        if (tok.size() > 5 && !tok[5].empty() && tok[5] != "*") {
            int sc = ctx.subcatch_names.find(tok[5]);
            ctx.nodes.outfall_route_to[idx] = sc;  // may be -1 if not yet parsed
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
        int idx = ctx.node_names.find(name);
        if (idx < 0) idx = ctx.node_names.add(name);

        ensure_node_capacity(ctx, idx);
        auto ui = static_cast<std::size_t>(idx);

        ctx.nodes.type[ui]        = NodeType::DIVIDER;
        ctx.nodes.invert_elev[ui] = to_double(tok[1]);

        // tok[2] = diversion link name (resolved in post-parse)
        // Store link name for deferred resolution
        const std::string& div_link_name = tok[2];
        ctx.nodes.divider_link_name[ui] = div_link_name;
        int dl = ctx.link_names.find(div_link_name);
        ctx.nodes.divider_link[ui] = dl; // may be -1 if not yet parsed

        // tok[3] = divider type
        const std::string dtype = Tokenizer::to_upper(tok[3]);
        if (dtype == "CUTOFF") {
            ctx.nodes.divider_type[ui] = DividerType::CUTOFF;
            if (tok.size() > 4) ctx.nodes.divider_cutoff[ui] = to_double(tok[4]);
        } else if (dtype == "OVERFLOW") {
            ctx.nodes.divider_type[ui] = DividerType::OVERFLOW_DIV;
        } else if (dtype == "TABULAR") {
            ctx.nodes.divider_type[ui] = DividerType::TABULAR;
            // tok[4] = curve name (deferred)
            if (tok.size() > 4) {
                ctx.nodes.divider_curve_name[ui] = tok[4];
                int ci = ctx.table_names.find(tok[4]);
                ctx.nodes.divider_curve[ui] = ci; // may be -1
            }
        } else if (dtype == "WEIR") {
            ctx.nodes.divider_type[ui] = DividerType::WEIR;
            if (tok.size() > 4) ctx.nodes.divider_cutoff[ui] = to_double(tok[4]);
            if (tok.size() > 5) ctx.nodes.divider_cd[ui]     = to_double(tok[5]);
            if (tok.size() > 6) ctx.nodes.divider_max_depth[ui] = to_double(tok[6]);
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
        int idx = ctx.node_names.find(name);
        if (idx < 0) idx = ctx.node_names.add(name);

        ensure_node_capacity(ctx, idx);

        ctx.nodes.type[idx]        = NodeType::STORAGE;
        ctx.nodes.invert_elev[idx] = to_double(tok[1]);  // Elev
        ctx.nodes.full_depth[idx]  = to_double(tok[2]);  // MaxDepth
        ctx.nodes.init_depth[idx]  = to_double(tok[3]);  // InitDepth

        if (tok.size() < 5) {
            if (!pl.comment.empty())
                ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
            continue;
        }
        const std::string shape = Tokenizer::to_upper(tok[4]);

        if (shape == "TABULAR") {
            // Next token is curve name — resolve to index in post-parse pass
            ctx.nodes.storage_curve[idx] = -1;
            if (tok.size() > 5)
                ctx.nodes.storage_curve_name[idx] = tok[5];
        } else if (shape == "FUNCTIONAL") {
            // A1, A2, A0
            if (tok.size() > 5) ctx.nodes.storage_a[idx] = to_double(tok[5]);
            if (tok.size() > 6) ctx.nodes.storage_b[idx] = to_double(tok[6]);
            if (tok.size() > 7) ctx.nodes.storage_c[idx] = to_double(tok[7]);
        }

        // Optional: SurDepth, Fevap, Seep
        const int param_offset = (shape == "TABULAR") ? 6 : 8;
        if (static_cast<int>(tok.size()) > param_offset)
            ctx.nodes.sur_depth[idx] = to_double(tok[param_offset]);
        if (static_cast<int>(tok.size()) > param_offset + 1)
            ctx.nodes.storage_evap_frac[idx] = to_double(tok[param_offset + 1]);
        if (static_cast<int>(tok.size()) > param_offset + 2)
            ctx.nodes.storage_seep_rate[idx] = to_double(tok[param_offset + 2]);
        if (!pl.comment.empty())
            ctx.nodes.comments[static_cast<std::size_t>(idx)] = pl.comment;
    }
}

// ============================================================================
// handle_coordinates()
// ============================================================================

void handle_coordinates(SimulationContext& ctx, const std::vector<std::string>& lines) {
    ensure_spatial_capacity(ctx, ctx.node_names.size());

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const int idx = ctx.node_names.find(tok[0]);
        if (idx < 0) continue;  // unknown node — silently skip

        ensure_spatial_capacity(ctx, idx + 1);

        ctx.spatial.node_x[idx] = to_double(tok[1]);
        ctx.spatial.node_y[idx] = to_double(tok[2]);
    }
}

} /* namespace openswmm::input */
