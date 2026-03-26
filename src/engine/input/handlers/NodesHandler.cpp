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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "NodesHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/NodeData.hpp"

#include <charconv>
#include <string>
#include <string_view>

namespace openswmm::input {

// ============================================================================
// Helpers
// ============================================================================

static double to_double(std::string_view sv, double def = 0.0) noexcept {
    double v = def;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    return v;
}

// Ensure NodeData arrays are large enough for index `idx`
static void ensure_node_capacity(SimulationContext& ctx, int idx) {
    const auto n = static_cast<std::size_t>(idx + 1);
    auto grow = [&](auto& vec, auto def) {
        if (vec.size() < n) vec.resize(n, def);
    };
    using NT  = NodeType;
    using OT  = OutfallType;
    grow(ctx.nodes.type,               NT::JUNCTION);
    grow(ctx.nodes.invert_elev,        0.0);
    grow(ctx.nodes.full_depth,         0.0);
    grow(ctx.nodes.init_depth,         0.0);
    grow(ctx.nodes.sur_depth,          0.0);
    grow(ctx.nodes.ponded_area,        0.0);
    grow(ctx.nodes.outfall_type,       OT::FREE);
    grow(ctx.nodes.outfall_param,      0.0);
    grow(ctx.nodes.outfall_has_flap_gate, false);
    grow(ctx.nodes.storage_curve,      -1);
    grow(ctx.nodes.storage_a,          0.0);
    grow(ctx.nodes.storage_b,          0.0);
    grow(ctx.nodes.storage_c,          0.0);
    grow(ctx.nodes.storage_seep_rate,  0.0);
    grow(ctx.nodes.exfil_suction,      0.0);
    grow(ctx.nodes.exfil_ksat,         0.0);
    grow(ctx.nodes.exfil_imd,          0.0);
    grow(ctx.nodes.depth,              0.0);
    grow(ctx.nodes.head,               0.0);
    grow(ctx.nodes.volume,             0.0);
    grow(ctx.nodes.lat_flow,           0.0);
    grow(ctx.nodes.inflow,             0.0);
    grow(ctx.nodes.outflow,            0.0);
    grow(ctx.nodes.overflow,           0.0);
    grow(ctx.nodes.losses,             0.0);
    grow(ctx.nodes.crown_elev,         0.0);
    grow(ctx.nodes.degree,             0);
    grow(ctx.nodes.old_net_inflow,     0.0);
    grow(ctx.nodes.full_volume,        0.0);
    grow(ctx.nodes.old_depth,          0.0);
    grow(ctx.nodes.old_volume,         0.0);
    grow(ctx.nodes.old_lat_flow,       0.0);
    grow(ctx.nodes.stat_vol_flooded,   0.0);
    grow(ctx.nodes.stat_time_flooded,  0.0);
    grow(ctx.nodes.stat_max_depth,     0.0);
    grow(ctx.nodes.stat_max_overflow,  0.0);
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
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
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
    }
}

// ============================================================================
// handle_outfalls()
// ============================================================================

void handle_outfalls(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
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
    }
}

// ============================================================================
// handle_dividers()
// ============================================================================

void handle_dividers(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2) continue;

        const std::string& name = tok[0];
        int idx = ctx.node_names.find(name);
        if (idx < 0) idx = ctx.node_names.add(name);

        ensure_node_capacity(ctx, idx);

        ctx.nodes.type[idx]        = NodeType::DIVIDER;
        ctx.nodes.invert_elev[idx] = to_double(tok[1]);
        if (tok.size() > 3) ctx.nodes.full_depth[idx] = to_double(tok[3]);
    }
}

// ============================================================================
// handle_storage()
// ============================================================================

void handle_storage(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 4) continue;

        const std::string& name = tok[0];
        int idx = ctx.node_names.find(name);
        if (idx < 0) idx = ctx.node_names.add(name);

        ensure_node_capacity(ctx, idx);

        ctx.nodes.type[idx]        = NodeType::STORAGE;
        ctx.nodes.invert_elev[idx] = to_double(tok[1]);  // Elev
        ctx.nodes.full_depth[idx]  = to_double(tok[2]);  // MaxDepth
        ctx.nodes.init_depth[idx]  = to_double(tok[3]);  // InitDepth

        if (tok.size() < 5) continue;
        const std::string shape = Tokenizer::to_upper(tok[4]);

        if (shape == "TABULAR") {
            // Next token is curve name — resolve to index in post-parse pass
            // For now store -1 (resolved later)
            ctx.nodes.storage_curve[idx] = -1;
            // TODO: store curve name for deferred resolution
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
        if (static_cast<int>(tok.size()) > param_offset + 2)
            ctx.nodes.storage_seep_rate[idx] = to_double(tok[param_offset + 2]);
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
