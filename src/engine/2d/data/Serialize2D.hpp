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
 * @file      Serialize2D.hpp
 * @brief     Header-only helpers shared by 2D model serialization consumers
 *        (InpWriter `[2D_*]` emission, GeoPackage writer).
 *
 * @details Collect the authored row form of boundary conditions and edge
 *          conveyance from whichever source is still available:
 *          - the parse-time pending rows (retained after the initialize()
 *            drain) — byte-faithful, preserves group labels and the
 *            TS-vs-constant distinction; or
 *          - the drained BoundaryData / MeshData::edge_conveyance slots
 *            (API-built models, or legacy states with cleared pending
 *            rows) — loses the BC group label.
 *
 *          Define-free and dependency-free beyond the plain 2D data
 *          structs, so it compiles in builds without OPENSWMM_HAS_2D
 *          (e.g. inside the geopackage static lib).
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SERIALIZE_2D_HPP
#define OPENSWMM_ENGINE_2D_SERIALIZE_2D_HPP

#include "MeshData.hpp"
#include "BoundaryData.hpp"
#include "PendingRows2D.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace openswmm::twoD {

/**
 * @brief Copy authored boundary rows or reconstruct them from live edge storage.
 * @param[in] pending Optional retained authored rows. A nonempty vector wins before draining;
 * after draining it supplies GROUP labels, with the last nonempty label per slot winning.
 * @param[in] boundary Optional live boundary arrays with consistent size() extents. Used after
 * draining, or before draining when pending is null or empty. WALL slots are omitted.
 * @param[in] drained Whether initialization consumed the pending rows into live boundary arrays.
 * @param[in] flow_to_si_applied Positive factor previously applied to constant flow values;
 * reconstruction divides by this factor to recover display flow units per metre. A nonpositive
 * factor uses 1. Stage heads and bed slopes are copied in their current mesh units.
 * @return Independent owned rows, including time-series/curve names where present; an empty
 * vector when no usable source or no non-WALL slots exists. Invalid boundary enum values still
 * produce a row with default parameters; this helper does not validate authored data.
 * @note Inputs must remain stable for the call. Allocation failures propagate without mutating
 * them. Live NORMAL_FLOW slopes are copied unchanged, including a zero value; no slope is
 * computed here. The routine walks padded boundary slots rather than validating mesh topology.
 *
 * @see collectConveyanceRows()
 */
inline std::vector<PendingBoundaryRow> collectBCRows(const std::vector<PendingBoundaryRow>* pending,
                                                     const BoundaryData* boundary, bool drained,
                                                     double flow_to_si_applied = 1.0) {

    const double flow_to_display = (flow_to_si_applied > 0.0) ? 1.0 / flow_to_si_applied : 1.0;

    if (!drained) {
        if (pending && !pending->empty())
            return *pending;
        // Not drained and no pending rows: nothing has been authored yet
        // (boundary, if sized at all, holds only WALL defaults).
    }

    std::vector<PendingBoundaryRow> rows;
    if (!boundary)
        return rows;

    std::unordered_map<int, std::string> groups;
    if (pending)
        for (const auto& p : *pending) groups[MeshData::slot(p.tri,p.edge)] = p.group;
    const int n = boundary->size();
    for (int idx = 0; idx < n; ++idx) {
        const auto type = static_cast<BoundaryType>(boundary->edge_bc_type[idx]);
        if (type == BoundaryType::WALL && boundary->edge_wall_drag[idx] == 0 &&
            (groups.find(idx) == groups.end() || groups.at(idx).empty()))
            continue;

        PendingBoundaryRow r;
        r.tri = MeshData::slot_cell(idx);
        r.edge = MeshData::slot_local(idx);
        r.bc_type = static_cast<int>(type);
        if (auto it = groups.find(idx); it != groups.end()) r.group = it->second;

        switch (type) {
        case BoundaryType::NORMAL_FLOW:
            r.param1 = boundary->edge_bed_slope[idx];
            break;
        case BoundaryType::SPECIFIED_STAGE:
            if (!boundary->edge_bc_tseries_name[idx].empty())
                r.name = boundary->edge_bc_tseries_name[idx];
            else
                r.param1 = boundary->edge_bc_head[idx];
            break;
        case BoundaryType::SPECIFIED_FLOW:
            if (!boundary->edge_bc_flow_tseries_name[idx].empty())
                r.name = boundary->edge_bc_flow_tseries_name[idx];
            else
                r.param1 = boundary->edge_bc_flow[idx] * flow_to_display;
            break;
        case BoundaryType::RATING_CURVE:
            r.name = boundary->edge_bc_rating_curve_name[idx];
            break;
        case BoundaryType::WALL:
            r.param1 = boundary->edge_wall_drag[idx];
            break;
        }
        rows.push_back(std::move(r));
    }

    return rows;
}

/**
 * @brief Copy authored conveyance rows or reconstruct unique nondefault mesh edges.
 * @param[in] pending Optional authored rows; a nonempty vector wins before draining.
 * @param[in] mesh Optional stable mesh with valid connectivity and vertex indices. Its
 * edge_conveyance array must span n_edge_slots(); absent or undersized storage yields no rows.
 * @param[in] drained Whether initialization consumed pending rows into mesh edge slots.
 * @return Independent owned rows. Reconstruction walks each cell's actual vertex count, sorts
 * endpoint indices, omits factors equal to 1, and emits each undirected edge once. The first
 * encountered nondefault mirror wins; factors are copied without validation or averaging.
 * @note Before draining, null/empty pending rows fall back to the mesh. No source is modified;
 * allocation failures propagate. Mixed triangle/quadrilateral connectivity is supported through
 * MeshData::cell_edge_vertices(), rather than assuming a triangle-only local-edge convention.
 */
inline std::vector<PendingEdgeConveyanceRow>
collectConveyanceRows(const std::vector<PendingEdgeConveyanceRow>* pending, const MeshData* mesh,
                      bool drained) {

    if (!drained && pending && !pending->empty())
        return *pending;

    std::vector<PendingEdgeConveyanceRow> rows;
    if (!mesh)
        return rows;

    const int nt = mesh->n_triangles();
    if (nt < 1 || mesh->edge_conveyance.size() < static_cast<std::size_t>(mesh->n_edge_slots()))
        return rows;

    std::unordered_set<std::int64_t> seen;
    for (int t = 0; t < nt; ++t) {
        const int nvc = mesh->cell_vertex_count(t);
        for (int e = 0; e < nvc; ++e) {
            const double k = mesh->edge_conveyance[MeshData::slot(t, e)];
            if (k == 1.0)
                continue;
            int ea, eb;
            mesh->cell_edge_vertices(t, e, ea, eb);
            const int va = std::min(ea, eb);
            const int vb = std::max(ea, eb);
            const std::int64_t key = (static_cast<std::int64_t>(va) << 32) |
                                     (static_cast<std::int64_t>(vb) & 0xFFFFFFFFLL);
            if (!seen.insert(key).second)
                continue;
            rows.push_back(PendingEdgeConveyanceRow{va, vb, k});
        }
    }
    return rows;
}

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SERIALIZE_2D_HPP
