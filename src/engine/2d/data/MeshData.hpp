/**
 * @file MeshData.hpp
 * @brief Structure-of-Arrays (SoA) storage for 2D triangular mesh geometry.
 *
 * @details Stores vertex coordinates, triangle connectivity, edge geometry,
 *          neighbour adjacency, vertex reconstruction stencils, and coupling
 *          maps to SWMM nodes. Follows the same data-oriented SoA pattern
 *          used throughout the engine (NodeData, LinkData, etc.).
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §2.1
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_MESH_DATA_HPP
#define OPENSWMM_ENGINE_2D_MESH_DATA_HPP

#include <vector>
#include <string>
#include <cstdint>

namespace openswmm::twoD {

/**
 * @brief SoA storage for 2D triangular mesh geometry and topology.
 *
 * All vertex arrays are indexed by vertex index [0, n_vertices).
 * All triangle arrays are indexed by triangle index [0, n_triangles).
 * Edge arrays are flat 2D: [tri * 3 + edge_local] for edge_local in {0,1,2}.
 */
struct MeshData {

    // -----------------------------------------------------------------------
    // Vertex arrays — indexed by vertex index [0, n_vertices)
    // -----------------------------------------------------------------------

    std::vector<double> vx;             ///< Vertex X coordinate
    std::vector<double> vy;             ///< Vertex Y coordinate
    std::vector<double> vz;             ///< Vertex Z (ground elevation)
    std::vector<std::string> vtag;      ///< Optional vertex tag

    // -----------------------------------------------------------------------
    // Triangle static properties — indexed by triangle index [0, n_triangles)
    // -----------------------------------------------------------------------

    // Connectivity (3 vertex indices per triangle)
    std::vector<int> tri_v0;            ///< Vertex 0 index
    std::vector<int> tri_v1;            ///< Vertex 1 index
    std::vector<int> tri_v2;            ///< Vertex 2 index

    // Neighbour connectivity (3 adjacent triangle indices, -1 = boundary)
    std::vector<int> tri_nbr0;          ///< Neighbour across edge opposite v0
    std::vector<int> tri_nbr1;          ///< Neighbour across edge opposite v1
    std::vector<int> tri_nbr2;          ///< Neighbour across edge opposite v2

    // Precomputed geometry
    std::vector<double> tri_area;       ///< Planimetric area (m²)
    std::vector<double> tri_cx;         ///< Centroid X
    std::vector<double> tri_cy;         ///< Centroid Y
    std::vector<double> tri_cz;         ///< Centroid Z (avg of vertex elevations)

    // Edge geometry — flat 2D: [tri * 3 + edge]
    std::vector<double> edge_length;    ///< Length of each edge
    std::vector<double> edge_nx;        ///< Outward normal X component
    std::vector<double> edge_ny;        ///< Outward normal Y component
    std::vector<double> edge_mx;        ///< Edge midpoint X
    std::vector<double> edge_my;        ///< Edge midpoint Y
    std::vector<double> edge_mz;        ///< Edge midpoint Z (interpolated)

    // Surface properties
    std::vector<double> mannings_n;     ///< Manning's roughness coefficient
    std::vector<std::string> tri_tag;   ///< Optional triangle tag

    // -----------------------------------------------------------------------
    // Vertex reconstruction stencil (pseudo-Laplacian weights)
    // -----------------------------------------------------------------------
    // Stored as CSR (compressed sparse row) for variable stencil sizes
    std::vector<int>    vert_stencil_ptr;   ///< [n_vertices + 1] row pointers
    std::vector<int>    vert_stencil_idx;   ///< Column indices (triangle indices)
    std::vector<double> vert_stencil_wt;    ///< Pseudo-Laplacian weights

    // -----------------------------------------------------------------------
    // Coupling maps
    // -----------------------------------------------------------------------

    std::vector<int> vert_coupled_node;     ///< SWMM node index (-1 = none)
    std::vector<int> tri_coupled_node;      ///< SWMM node index (-1 = none)

    // Coupling parameters (per vertex/triangle coupling point)
    std::vector<double> vert_coupling_cd;       ///< Discharge coefficient (default 0.65)
    std::vector<double> vert_coupling_area;     ///< Effective exchange area (m²)
    std::vector<double> tri_coupling_cd;        ///< Discharge coefficient
    std::vector<double> tri_coupling_area;      ///< Effective exchange area

    // Deferred resolution names (populated during parsing, cleared after resolve)
    std::vector<std::string> vert_coupled_node_name;
    std::vector<std::string> tri_coupled_node_name;

    // -----------------------------------------------------------------------
    // Capacity queries
    // -----------------------------------------------------------------------

    int n_vertices()  const noexcept { return static_cast<int>(vx.size()); }
    int n_triangles() const noexcept { return static_cast<int>(tri_v0.size()); }

    // -----------------------------------------------------------------------
    // Resize / allocation
    // -----------------------------------------------------------------------

    void resize_vertices(int nv) {
        auto n = static_cast<std::size_t>(nv);
        vx.resize(n, 0.0);
        vy.resize(n, 0.0);
        vz.resize(n, 0.0);
        vtag.resize(n);
        vert_coupled_node.resize(n, -1);
        vert_coupled_node_name.resize(n);
        vert_coupling_cd.resize(n, 0.65);
        vert_coupling_area.resize(n, 1.0);
    }

    void resize_triangles(int nt) {
        auto n = static_cast<std::size_t>(nt);
        tri_v0.resize(n, 0);
        tri_v1.resize(n, 0);
        tri_v2.resize(n, 0);
        tri_nbr0.resize(n, -1);
        tri_nbr1.resize(n, -1);
        tri_nbr2.resize(n, -1);
        tri_area.resize(n, 0.0);
        tri_cx.resize(n, 0.0);
        tri_cy.resize(n, 0.0);
        tri_cz.resize(n, 0.0);

        auto n3 = static_cast<std::size_t>(nt) * 3;
        edge_length.resize(n3, 0.0);
        edge_nx.resize(n3, 0.0);
        edge_ny.resize(n3, 0.0);
        edge_mx.resize(n3, 0.0);
        edge_my.resize(n3, 0.0);
        edge_mz.resize(n3, 0.0);

        mannings_n.resize(n, 0.035);
        tri_tag.resize(n);
        tri_coupled_node.resize(n, -1);
        tri_coupled_node_name.resize(n);
        tri_coupling_cd.resize(n, 0.65);
        tri_coupling_area.resize(n, 1.0);
    }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_MESH_DATA_HPP
