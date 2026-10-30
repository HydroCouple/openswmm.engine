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
 * @file      MeshBuilder.hpp
 * @brief     Topology construction and geometry precomputation for the 2D mesh.
 *
 * @details After parsing [2D_VERTICES] and [2D_TRIANGLES], MeshBuilder
 *          computes:
 *          - Edge-neighbour adjacency via sorted vertex-pair hash map
 *          - Edge geometry (length, outward normal, midpoint)
 *          - Cell geometry (area, centroid)
 *          - Boundary edge identification
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §5.1
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_MESH_BUILDER_HPP
#define OPENSWMM_ENGINE_2D_MESH_BUILDER_HPP

#include "../data/MeshData.hpp"
#include <functional>

namespace openswmm::twoD {

/**
 * @brief Build mesh topology and precompute geometry from raw vertex/triangle data.
 *
 * Must be called after parsing is complete and before solver initialization. Populates:
 * tri_nbr*, tri_area, tri_c*, edge_length, edge_n*, edge_m*.
 *
 * @param[in,out] mesh The mesh data with vx/vy/vz and tri_v0/v1/v2 already populated.
 *
 * @pre Mixed-cell connectivity and all per-cell/per-edge arrays are already sized, vertex
 * references are in bounds, and XYZ values are finite in one consistent length unit. This
 * routine is not an input validator.
 * @post Replaces neighbor and geometry arrays, retaining triangle/quad cell numbering. Allocates
 * an edge hash map and lazily creates quad VFR data; allocation failures propagate.
 * @note Call before stencils/solver initialization, with exclusive mesh access. Geometry
 * lengths/areas retain the mesh's input units.
 */
void buildMeshTopology(MeshData& mesh);
/**
 * @brief Cancellable equivalent; progress receives integer percent and returns true to cancel.
 *
 * @param[in,out] mesh Pre-sized mixed-cell mesh with valid vertex references and finite
 * coordinates, in consistent length units.
 * @param[in] progress Optional synchronous percent callback [0,100]; true cancels. It must not
 * mutate the mesh or reenter construction.
 * @return True when all topology/geometry passes complete, false on cancellation.
 * @post Cancellation can leave partially rewritten neighbors/geometry; it is not transactional.
 * A caller needing rollback must build a private mesh copy. Allocation/callback exceptions
 * propagate. No callback/storage is retained.
 */
bool buildMeshTopologyCancellable(MeshData& mesh, const std::function<bool(int)>& progress);

/**
 * @brief Validate mesh data for consistency.
 *
 * Checks: vertex indices in bounds, positive areas, no degenerate triangles.
 * @param[in] mesh The mesh to validate.
 * @return Empty string if valid, or a description of the first error found.
 *
 * @pre Connectivity and derived geometry arrays are sized consistently; topology construction
 * has already populated cell areas. Invalid outer container sizes are not guarded.
 * @note Checks actual triangle/quad vertex bounds, duplicate vertices, positive area and
 * roughness, and quad convexity. It does not exhaustively reject nonfinite geometry or
 * nonmanifold edges. Read-only; formatting errors may allocate/throw.
 */
std::string validateMesh(const MeshData& mesh);
/**
 * @brief Cancellable validator. `cancelled` is set when progress returns true.
 *
 * @param[in] mesh Borrowed built mesh; container extents must be consistent before validation.
 * @param[in] progress Optional synchronous percent callback [0,100]; true cancels. Must not
 * mutate the mesh.
 * @param[out] cancelled Reset false on entry, then set true only for callback cancellation.
 * @return First validation error string, or empty for success OR cancellation; always inspect
 * cancelled before treating an empty string as valid.
 * @note Read-only validation, with the same checks/limitations as validateMesh(); allocation and
 * callback exceptions propagate. No references/callbacks retained.
 */
std::string validateMeshCancellable(const MeshData& mesh, const std::function<bool(int)>& progress,
                                    bool& cancelled);

/**
 * @brief Recompute Z-derived per-triangle / per-edge geometry for triangles incident to a vertex
 * whose Z just changed.
 *
 * Updates `tri_cz` (centroid Z = mean of vertex Zs) and `edge_mz` (per-edge midpoint Z) for
 * every triangle that references vertex `vidx`. XY-derived fields (`tri_area`, `tri_cx`,
 * `tri_cy`, `edge_length`, `edge_nx`, `edge_ny`, `edge_mx`, `edge_my`) are not affected.
 *
 * Used by `swmm_2d_set_vertex_z` and exposed here so tests can verify the recompute logic
 * without spinning up a full engine.
 *
 * @param[in,out] mesh The mesh; `mesh.vz[vidx]` is assumed to already hold the new Z.
 * @param[in] vidx Index of the vertex whose Z just changed.
 *
 * @pre Mesh topology/derived arrays are already built and sized; updated elevations use the
 * mesh's current length units.
 * @post Changes bed centroids, edge-midpoint beds and quad VFR elevations/areas. Does not
 * rewrite hydraulic head/depth, XY geometry, or reconstruction stencils.
 * @note Requires exclusive mesh access. Lazy quad side-data allocation may throw; no validation
 * or transactional rollback is provided.
 */
void recomputeVertexZDependents(MeshData& mesh, int vidx);

/**
 * @brief Whole-mesh form of `recomputeVertexZDependents`.
 *
 * Recomputes `tri_cz` and `edge_mz` for EVERY triangle in one pass, and produces
 * bitwise-identical values to calling the per-vertex form for every vertex in turn (same
 * operands, same operand order, same arithmetic).
 *
 * This exists because the per-vertex form scans all triangles per call: doing that once per
 * vertex is O(nVertices x nTriangles), which is minutes on a million-cell mesh.
 * `swmm_2d_set_vertex_z_bulk` writes every Z and then calls this once, making the same work
 * O(nVertices + nTriangles).
 *
 * @param[in,out] mesh The mesh; `mesh.vz` is assumed to already hold the new Zs.
 *
 * @pre Mesh topology/derived arrays are already built and sized; updated elevations use the
 * mesh's current length units.
 * @post Changes bed centroids, edge-midpoint beds and quad VFR elevations/areas. Does not
 * rewrite hydraulic head/depth, XY geometry, or reconstruction stencils.
 * @note Requires exclusive mesh access. Lazy quad side-data allocation may throw; no validation
 * or transactional rollback is provided.
 */
void recomputeAllZDependents(MeshData& mesh);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_MESH_BUILDER_HPP
