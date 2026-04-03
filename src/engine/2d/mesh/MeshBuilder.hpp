/**
 * @file MeshBuilder.hpp
 * @brief Topology construction and geometry precomputation for the 2D mesh.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_MESH_BUILDER_HPP
#define OPENSWMM_ENGINE_2D_MESH_BUILDER_HPP

#include "../data/MeshData.hpp"

namespace openswmm::twoD {

/**
 * @brief Build mesh topology and precompute geometry from raw vertex/triangle data.
 *
 * Must be called after parsing is complete and before solver initialization.
 * Populates: tri_nbr*, tri_area, tri_c*, edge_length, edge_n*, edge_m*.
 *
 * @param mesh The mesh data with vx/vy/vz and tri_v0/v1/v2 already populated.
 */
void buildMeshTopology(MeshData& mesh);

/**
 * @brief Validate mesh data for consistency.
 *
 * Checks: vertex indices in bounds, positive areas, no degenerate triangles.
 * @param mesh The mesh to validate.
 * @return Empty string if valid, or a description of the first error found.
 */
std::string validateMesh(const MeshData& mesh);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_MESH_BUILDER_HPP
