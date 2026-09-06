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
 * @file Serialize2D.hpp
 * @brief Header-only helpers shared by 2D model serialization consumers
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
 * @brief Collect `[2D_BOUNDARY_CONDITIONS]`-shaped rows for serialization.
 *
 * @param pending  Parse-time pending rows (may be null/empty).
 * @param boundary Drained per-edge BC storage (may be null/unsized).
 * @param drained  SolverOptions2D::pending_rows_drained — true once
 *                 initialize() moved the pending rows into @p boundary.
 * @param flow_to_si_applied  SolverOptions2D::bc_flow_to_si_applied — the
 *                 display-flow → m³/s factor initialize() scaled the constant
 *                 SPECIFIED_FLOW values by (1.0 when it has not run). The file
 *                 contract is display flow units per metre, so constant flows
 *                 read from @p boundary are divided by it. Constant STAGE heads
 *                 need no such treatment: they follow the mesh scaling, and the
 *                 `;; UNITS: SI (m)` header the writer emits covers them.
 * @return Authored rows; empty when every edge is a default WALL.
 *
 * @note Source selection: BEFORE the drain the pending rows are the only
 *       (and authoritative) source. AFTER the drain, BoundaryData is the
 *       live state — post-initialize API mutations edit it, so it wins —
 *       and the optional GROUP labels (not representable in BoundaryData)
 *       are re-attached from the retained pending rows by (tri, edge).
 *       NORMAL_FLOW's authored slope sentinel (0.0 = auto) is preserved:
 *       nothing writes the computed slope back into edge_bed_slope.
 */
inline std::vector<PendingBoundaryRow> collectBCRows(
    const std::vector<PendingBoundaryRow>* pending,
    const BoundaryData* boundary,
    bool drained,
    double flow_to_si_applied = 1.0) {

    const double flow_to_display =
        (flow_to_si_applied > 0.0) ? 1.0 / flow_to_si_applied : 1.0;

    if (!drained) {
        if (pending && !pending->empty()) return *pending;
        // Not drained and no pending rows: nothing has been authored yet
        // (boundary, if sized at all, holds only WALL defaults).
    }

    std::vector<PendingBoundaryRow> rows;
    if (!boundary) return rows;

    const int n = boundary->size();
    for (int idx = 0; idx < n; ++idx) {
        const auto type =
            static_cast<BoundaryType>(boundary->edge_bc_type[idx]);
        if (type == BoundaryType::WALL) continue;

        PendingBoundaryRow r;
        r.tri     = idx / 3;
        r.edge    = idx % 3;
        r.bc_type = static_cast<int>(type);

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
            break;
        }
        rows.push_back(std::move(r));
    }

    // Re-attach the authored GROUP labels (organizational metadata that
    // BoundaryData does not carry) from the retained pending rows.
    if (pending && !pending->empty() && !rows.empty()) {
        std::unordered_map<int, const std::string*> groups;
        groups.reserve(pending->size());
        for (const auto& p : *pending) {
            if (!p.group.empty()) groups[p.tri * 3 + p.edge] = &p.group;
        }
        if (!groups.empty()) {
            for (auto& r : rows) {
                auto it = groups.find(r.tri * 3 + r.edge);
                if (it != groups.end()) r.group = *it->second;
            }
        }
    }
    return rows;
}

/**
 * @brief Collect `[2D_EDGE_CONVEYANCE]`-shaped rows for serialization.
 *
 * @param pending Parse-time pending rows (may be null/empty).
 * @param mesh    Mesh whose edge_conveyance slots hold the drained values
 *                (may be null).
 * @param drained SolverOptions2D::pending_rows_drained — true once
 *                initialize() moved the pending rows into the mesh slots.
 * @return Authored rows; empty when every edge is at the default 1.0.
 *
 * @note Source selection mirrors collectBCRows: pending rows before the
 *       drain (the mesh slots are still all-1.0 defaults then); the mesh
 *       slots after it (they are the live state API mutations edit, and
 *       reconstruction is lossless — pair + value). The walk derives the
 *       vertex pair for local edge e of triangle t as v[(e+1)%3],
 *       v[(e+2)%3] (the drain convention in SurfaceRouter2D::initialize),
 *       and de-duplicates the interior-edge mirror so each undirected
 *       edge is emitted once with v_from < v_to.
 */
inline std::vector<PendingEdgeConveyanceRow> collectConveyanceRows(
    const std::vector<PendingEdgeConveyanceRow>* pending,
    const MeshData* mesh,
    bool drained) {

    if (!drained && pending && !pending->empty()) return *pending;

    std::vector<PendingEdgeConveyanceRow> rows;
    if (!mesh) return rows;

    const int nt = mesh->n_triangles();
    if (nt < 1 ||
        mesh->edge_conveyance.size() < static_cast<std::size_t>(nt) * 3)
        return rows;

    std::unordered_set<std::int64_t> seen;
    for (int t = 0; t < nt; ++t) {
        const int v[3] = { mesh->tri_v0[t], mesh->tri_v1[t], mesh->tri_v2[t] };
        for (int e = 0; e < 3; ++e) {
            const double k = mesh->edge_conveyance[t * 3 + e];
            if (k == 1.0) continue;
            const int va = std::min(v[(e + 1) % 3], v[(e + 2) % 3]);
            const int vb = std::max(v[(e + 1) % 3], v[(e + 2) % 3]);
            const std::int64_t key =
                (static_cast<std::int64_t>(va) << 32)
                | (static_cast<std::int64_t>(vb) & 0xFFFFFFFFLL);
            if (!seen.insert(key).second) continue;
            rows.push_back(PendingEdgeConveyanceRow{va, vb, k});
        }
    }
    return rows;
}

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SERIALIZE_2D_HPP
