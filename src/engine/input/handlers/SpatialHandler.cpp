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
 * @file      SpatialHandler.cpp
 * @brief     Section handlers for [MAP], [VERTICES], [POLYGONS], [SYMBOLS].
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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

/**
 * @brief Update map extents and map-unit metadata from recognized rows.
 *
 * DIMENSIONS with four values overwrites extents; UNITS stores an uppercase token. Unknown/short
 * rows are ignored, prior fields retained. Uses permissive to_double conversion, no coordinate
 * ordering/range checks, no CRS assignment or geometry rescaling.
 *
 * @param[in,out] ctx Model context receiving map geometry/tags; name registries must already
 *     identify referenced objects.
 * @param[in] lines Borrowed section data rows, consumed synchronously; coordinates remain authored
 *     map units without transformation.
 */
void handle_map(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty())
            continue;

        // Normalize keyword to uppercase for comparison
        std::string keyword = tok[0];
        std::transform(keyword.begin(), keyword.end(), keyword.begin(), ::toupper);

        if (keyword == "DIMENSIONS" && tok.size() >= 5) {
            ctx.spatial.map_x1 = to_double(tok[1]);
            ctx.spatial.map_y1 = to_double(tok[2]);
            ctx.spatial.map_x2 = to_double(tok[3]);
            ctx.spatial.map_y2 = to_double(tok[4]);
        } else if (keyword == "UNITS" && tok.size() >= 2) {
            ctx.spatial.map_units = tok[1];
            std::transform(ctx.spatial.map_units.begin(), ctx.spatial.map_units.end(),
                           ctx.spatial.map_units.begin(), ::toupper);
        }
    }
}

// ============================================================================
// handle_vertices()
// ============================================================================

/**
 * @brief Rebuild each link's ordered intermediate-vertex lists.
 *
 * Grows outer arrays to the link registry then clears all current per-link vertices, including
 * links absent from lines. Appends recognized LINK X Y rows in input order; short/unknown-object
 * rows skip silently. Numeric parsing is permissive; X/Y pushes can fail independently, with no
 * rollback. Does not insert link endpoints or alter network topology.
 *
 * @param[in,out] ctx Model context receiving map geometry/tags; name registries must already
 *     identify referenced objects.
 * @param[in] lines Borrowed section data rows, consumed synchronously; coordinates remain authored
 *     map units without transformation.
 */
void handle_vertices(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const auto nl = static_cast<std::size_t>(ctx.link_names.size());
    if (ctx.spatial.link_vertices_x.size() < nl)
        ctx.spatial.link_vertices_x.resize(nl);
    if (ctx.spatial.link_vertices_y.size() < nl)
        ctx.spatial.link_vertices_y.resize(nl);

    // Geometry sections are the row-count leaders in a large model, so the
    // per-row token vector is hoisted and reused: after the first row this
    // allocates nothing. tokenize_views_into cannot handle quoted tokens, so
    // a line containing a quote falls back to the owned tokenizer — behaviour
    // is identical either way, only the allocation differs.
    std::vector<std::string_view> tok;
    std::vector<std::string> quoted;
    auto split = [&](const std::string& line) -> const std::vector<std::string_view>& {
        if (line.find('"') == std::string::npos) {
            Tokenizer::tokenize_views_into(line, tok);
        } else {
            quoted = Tokenizer::tokenize(line);
            tok.assign(quoted.begin(), quoted.end());
        }
        return tok;
    };

    // Rebuild from section contents to preserve deterministic per-link order.
    for (auto& xs : ctx.spatial.link_vertices_x)
        xs.clear();
    for (auto& ys : ctx.spatial.link_vertices_y)
        ys.clear();

    for (const auto& line : lines) {
        const auto& tok = split(line);
        if (tok.size() < 3)
            continue;

        const int idx = ctx.link_names.find(tok[0]);
        if (idx < 0)
            continue;

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

/**
 * @brief Rebuild each subcatchment's ordered polygon coordinate lists.
 *
 * Grows outer arrays then clears every current polygon before appending known SUBCATCH X Y rows.
 * Short/unknown rows skip silently; numeric conversion is permissive. Does not enforce ring
 * closure, orientation, point count or calculate area/centroid. Separate X/Y appends can fail
 * without rollback.
 *
 * @param[in,out] ctx Model context receiving map geometry/tags; name registries must already
 *     identify referenced objects.
 * @param[in] lines Borrowed section data rows, consumed synchronously; coordinates remain authored
 *     map units without transformation.
 */
void handle_polygons(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const auto ns = static_cast<std::size_t>(ctx.subcatch_names.size());
    if (ctx.spatial.subcatch_polygon_x.size() < ns)
        ctx.spatial.subcatch_polygon_x.resize(ns);
    if (ctx.spatial.subcatch_polygon_y.size() < ns)
        ctx.spatial.subcatch_polygon_y.resize(ns);

    // Geometry sections are the row-count leaders in a large model, so the
    // per-row token vector is hoisted and reused: after the first row this
    // allocates nothing. tokenize_views_into cannot handle quoted tokens, so
    // a line containing a quote falls back to the owned tokenizer — behaviour
    // is identical either way, only the allocation differs.
    std::vector<std::string_view> tok;
    std::vector<std::string> quoted;
    auto split = [&](const std::string& line) -> const std::vector<std::string_view>& {
        if (line.find('"') == std::string::npos) {
            Tokenizer::tokenize_views_into(line, tok);
        } else {
            quoted = Tokenizer::tokenize(line);
            tok.assign(quoted.begin(), quoted.end());
        }
        return tok;
    };

    // Rebuild from section contents to preserve deterministic per-subcatch order.
    for (auto& xs : ctx.spatial.subcatch_polygon_x)
        xs.clear();
    for (auto& ys : ctx.spatial.subcatch_polygon_y)
        ys.clear();

    for (const auto& line : lines) {
        const auto& tok = split(line);
        if (tok.size() < 3)
            continue;

        const int idx = ctx.subcatch_names.find(tok[0]);
        if (idx < 0)
            continue;

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

/**
 * @brief Update rain-gage symbol coordinates from known-name rows.
 *
 * Grows missing coordinate arrays with zero defaults; recognized rows overwrite the selected gage
 * (last duplicate wins). Other existing coordinates are preserved. Short/unknown rows skip
 * silently; no finite/range or CRS check.
 *
 * @param[in,out] ctx Model context receiving map geometry/tags; name registries must already
 *     identify referenced objects.
 * @param[in] lines Borrowed section data rows, consumed synchronously; coordinates remain authored
 *     map units without transformation.
 */
void handle_symbols(SimulationContext& ctx, const std::vector<std::string>& lines) {
    const auto ng = static_cast<std::size_t>(ctx.gage_names.size());
    if (ctx.spatial.gage_x.size() < ng)
        ctx.spatial.gage_x.resize(ng, 0.0);
    if (ctx.spatial.gage_y.size() < ng)
        ctx.spatial.gage_y.resize(ng, 0.0);

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3)
            continue;

        const int idx = ctx.gage_names.find(tok[0]);
        if (idx < 0)
            continue;

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

/**
 * @brief Assign per-object tags for known NODE, LINK and SUBCATCH rows.
 *
 * Category is uppercased, object ID resolved through its ASCII-insensitive registry. Grows tag
 * vectors as needed and overwrites each selected entry, preserving other tags. Unknown types/names
 * and short rows skip silently; no user-flag schema interaction or clearing of absent assignments.
 *
 * @param[in,out] ctx Model context receiving map geometry/tags; name registries must already
 *     identify referenced objects.
 * @param[in] lines Borrowed TYPE NAME TAG rows; tokenizer removes quotes, only token three supplies
 *     the tag.
 */
void handle_tags(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 3)
            continue;

        // Normalize object type to uppercase
        std::string obj_type = tok[0];
        std::transform(obj_type.begin(), obj_type.end(), obj_type.begin(), ::toupper);

        const std::string& name = tok[1];
        const std::string& tag = tok[2];

        // Resolve name → index against the SoA stores. Tags are stored
        // per-NodeData/LinkData/SubcatchData index, not name-keyed, so
        // they survive a subsequent swmm_*_rename.
        if (obj_type == "NODE") {
            const int idx = ctx.node_names.find(name);
            if (idx >= 0) {
                const auto u = static_cast<std::size_t>(idx);
                if (u >= ctx.nodes.tags.size())
                    ctx.nodes.tags.resize(u + 1);
                ctx.nodes.tags[u] = tag;
            }
        } else if (obj_type == "LINK") {
            const int idx = ctx.link_names.find(name);
            if (idx >= 0) {
                const auto u = static_cast<std::size_t>(idx);
                if (u >= ctx.links.tags.size())
                    ctx.links.tags.resize(u + 1);
                ctx.links.tags[u] = tag;
            }
        } else if (obj_type == "SUBCATCH") {
            const int idx = ctx.subcatch_names.find(name);
            if (idx >= 0) {
                const auto u = static_cast<std::size_t>(idx);
                if (u >= ctx.subcatches.tags.size())
                    ctx.subcatches.tags.resize(u + 1);
                ctx.subcatches.tags[u] = tag;
            }
        }
    }
}

} /* namespace openswmm::input */
