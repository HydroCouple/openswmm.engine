/**
 * @file SpatialHandler.cpp
 * @brief Section handlers for [MAP], [VERTICES], [POLYGONS], [SYMBOLS].
 *
 * ### [MAP] format
 * ```
 * [MAP]
 * DIMENSIONS  x1  y1  x2  y2
 * UNITS       NONE
 * ```
 *
 * ### [VERTICES] format
 * ```
 * ;; Link        X             Y
 * C1             2735.503      6483.508
 * C1             2174.216      6351.033
 * ```
 *
 * ### [POLYGONS] format
 * ```
 * ;; Subcatchment X             Y
 * S1              2174.216      8125.000
 * S1              2482.570      8125.000
 * ```
 *
 * ### [SYMBOLS] format
 * ```
 * ;; Gage         X             Y
 * RG1             1000.000      5000.000
 * ```
 *
 * @see SpatialHandler.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "SpatialHandler.hpp"
#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../InputParseUtils.hpp"

#include <algorithm>

namespace openswmm::input {

// ============================================================================
// handle_map()
// ============================================================================

void handle_map(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;

        // Normalize keyword to uppercase for comparison
        std::string keyword = tok[0];
        std::transform(keyword.begin(), keyword.end(), keyword.begin(), ::toupper);

        if (keyword == "DIMENSIONS" && tok.size() >= 5) {
            ctx.spatial.map_x1 = to_double(tok[1]);
            ctx.spatial.map_y1 = to_double(tok[2]);
            ctx.spatial.map_x2 = to_double(tok[3]);
            ctx.spatial.map_y2 = to_double(tok[4]);
        }
        else if (keyword == "UNITS" && tok.size() >= 2) {
            ctx.spatial.map_units = tok[1];
            std::transform(ctx.spatial.map_units.begin(),
                           ctx.spatial.map_units.end(),
                           ctx.spatial.map_units.begin(), ::toupper);
        }
    }
}

// ============================================================================
// handle_vertices()
// ============================================================================

void handle_vertices(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const auto nl = static_cast<std::size_t>(ctx.link_names.size());
    if (ctx.spatial.link_vertices_x.size() < nl) ctx.spatial.link_vertices_x.resize(nl);
    if (ctx.spatial.link_vertices_y.size() < nl) ctx.spatial.link_vertices_y.resize(nl);

    // Rebuild from section contents to preserve deterministic per-link order.
    for (auto& xs : ctx.spatial.link_vertices_x) xs.clear();
    for (auto& ys : ctx.spatial.link_vertices_y) ys.clear();

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const int idx = ctx.link_names.find(tok[0]);
        if (idx < 0) continue;

        const auto uidx = static_cast<std::size_t>(idx);
        if (ctx.spatial.link_vertices_x.size() <= uidx) {
            ctx.spatial.link_vertices_x.resize(uidx + 1);
            ctx.spatial.link_vertices_y.resize(uidx + 1);
        }

        ctx.spatial.link_vertices_x[uidx].push_back(to_double(tok[1]));
        ctx.spatial.link_vertices_y[uidx].push_back(to_double(tok[2]));
    }
}

// ============================================================================
// handle_polygons()
// ============================================================================

void handle_polygons(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const auto ns = static_cast<std::size_t>(ctx.subcatch_names.size());
    if (ctx.spatial.subcatch_polygon_x.size() < ns) ctx.spatial.subcatch_polygon_x.resize(ns);
    if (ctx.spatial.subcatch_polygon_y.size() < ns) ctx.spatial.subcatch_polygon_y.resize(ns);

    // Rebuild from section contents to preserve deterministic per-subcatch order.
    for (auto& xs : ctx.spatial.subcatch_polygon_x) xs.clear();
    for (auto& ys : ctx.spatial.subcatch_polygon_y) ys.clear();

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const int idx = ctx.subcatch_names.find(tok[0]);
        if (idx < 0) continue;

        const auto uidx = static_cast<std::size_t>(idx);
        if (ctx.spatial.subcatch_polygon_x.size() <= uidx) {
            ctx.spatial.subcatch_polygon_x.resize(uidx + 1);
            ctx.spatial.subcatch_polygon_y.resize(uidx + 1);
        }

        ctx.spatial.subcatch_polygon_x[uidx].push_back(to_double(tok[1]));
        ctx.spatial.subcatch_polygon_y[uidx].push_back(to_double(tok[2]));
    }
}

// ============================================================================
// handle_symbols()
// ============================================================================

void handle_symbols(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const auto ng = static_cast<std::size_t>(ctx.gage_names.size());
    if (ctx.spatial.gage_x.size() < ng) ctx.spatial.gage_x.resize(ng, 0.0);
    if (ctx.spatial.gage_y.size() < ng) ctx.spatial.gage_y.resize(ng, 0.0);

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        const int idx = ctx.gage_names.find(tok[0]);
        if (idx < 0) continue;

        const auto uidx = static_cast<std::size_t>(idx);
        if (ctx.spatial.gage_x.size() <= uidx) {
            ctx.spatial.gage_x.resize(uidx + 1, 0.0);
            ctx.spatial.gage_y.resize(uidx + 1, 0.0);
        }

        ctx.spatial.gage_x[uidx] = to_double(tok[1]);
        ctx.spatial.gage_y[uidx] = to_double(tok[2]);
    }
}

// ============================================================================
// handle_tags()
// ============================================================================

void handle_tags(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3) continue;

        // Normalize object type to uppercase
        std::string obj_type = tok[0];
        std::transform(obj_type.begin(), obj_type.end(), obj_type.begin(), ::toupper);

        const std::string& name = tok[1];
        const std::string& tag  = tok[2];

        if (obj_type == "NODE") {
            ctx.node_tags[name] = tag;
        }
        else if (obj_type == "LINK") {
            ctx.link_tags[name] = tag;
        }
        else if (obj_type == "SUBCATCH") {
            ctx.subcatch_tags[name] = tag;
        }
    }
}

} /* namespace openswmm::input */
