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
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_MESH_DATA_HPP
#define OPENSWMM_ENGINE_2D_MESH_DATA_HPP

#include <vector>
#include <string>
#include <cstdint>

namespace openswmm::twoD {

/// Maximum vertices (and therefore edge slots) per cell. The mesh is a mixed
/// triangle / convex-quadrilateral mesh (2D_TRI_QUAD_MESH_PLAN_2026-09-06):
/// every per-cell connectivity and edge SoA is padded to this fixed stride so
/// the flat `[cell * kMaxCellVerts + k]` addressing the solvers, the C API,
/// the HDF5 layout and the GUI all share stays a plain stride, never a CSR.
/// Slots k ≥ cell_nv[c] are padding (vertex −1, neighbour −2, zero geometry).
inline constexpr int kMaxCellVerts = 4;

/**
 * @brief SoA storage for 2D mixed triangle/quad mesh geometry and topology.
 *
 * All vertex arrays are indexed by vertex index [0, n_vertices).
 * All cell arrays are indexed by cell index [0, n_triangles()) — the
 * historical `tri_*` names are kept for the per-cell scalar arrays; a "tri"
 * is any cell (triangle or quad). Cells are ordered triangles first, then
 * quads (the `[2D_TRIANGLES]` rows, then the `[2D_QUADS]` rows).
 * Edge / connectivity arrays are flat 2D: [cell * kMaxCellVerts + k] for
 * k in [0, cell_nv[cell]).
 *
 * Local-edge convention (unified for both shapes): edge k has endpoints
 * `v[(k+1) % nv], v[(k+2) % nv]`. For a triangle this is the historical
 * "edge k is opposite vertex k" rule, so every existing `TRI EDGE` row keeps
 * its meaning; for a quad it is a fixed rotation of the natural numbering.
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
    // Cell static properties — indexed by cell index [0, n_triangles())
    // -----------------------------------------------------------------------

    /// Vertices per cell: 3 (triangle) or 4 (quadrilateral).
    std::vector<uint8_t> cell_nv;
    /// Connectivity, padded: [cell * kMaxCellVerts + k]; −1 in padding slots.
    /// Counter-clockwise order (MeshBuilder orients the outward normals from
    /// the centroid, so a clockwise triangle still works; quads are validated
    /// CCW + convex at parse time).
    std::vector<int> cell_v;
    /// Neighbour across local edge k, padded: [cell * kMaxCellVerts + k].
    /// −1 = boundary edge, −2 = padding slot (k ≥ cell_nv).
    std::vector<int> cell_nbr;

    // Precomputed geometry (per cell)
    std::vector<double> tri_area;       ///< Planimetric area (m²)
    std::vector<double> tri_cx;         ///< Centroid X (area centroid)
    std::vector<double> tri_cy;         ///< Centroid Y (area centroid)
    std::vector<double> tri_cz;         ///< Mean of the cell's vertex elevations

    // Edge geometry — flat 2D: [cell * kMaxCellVerts + k]
    std::vector<double> edge_length;    ///< Length of each edge
    std::vector<double> edge_nx;        ///< Outward normal X component
    std::vector<double> edge_ny;        ///< Outward normal Y component
    std::vector<double> edge_mx;        ///< Edge midpoint X
    std::vector<double> edge_my;        ///< Edge midpoint Y
    std::vector<double> edge_mz;        ///< Edge midpoint Z (interpolated)
    /// Centroid → edge-midpoint distance along the outward normal,
    /// (m⃗_e − c⃗)·n̂_e (m). For a triangle this equals the classic 2A/(3ξ)
    /// (the centroid sits one third of the altitude up); for a quad it is the
    /// exact normal distance. The ghost-cell arm every boundary-edge law uses.
    std::vector<double> edge_dist_c;

    /// Quad VFR precomputed data (B&S 2007 two-plane storage model, see
    /// mesh/QuadVfr.hpp): per CELL, six sorted sub-triangle elevations
    /// [cell*6 + k] and two sub-triangle areas [cell*2 + k]. Allocated only
    /// when the mesh holds at least one quad (empty otherwise); triangle rows
    /// are zero and never read. Rebuilt by MeshBuilder with the z-dependents.
    std::vector<double> quad_vfr_z;
    std::vector<double> quad_vfr_a;

    /// Per-edge conveyance factor in [0, 1], flat 2D [cell * kMaxCellVerts + k].
    /// Default 1.0 (unrestricted) for every edge.  Multiplies the
    /// diffusion-wave flux in SurfaceFluxCalculator::computeEdgeFluxes
    /// (the last factor before edge_flux is stored).  Symmetric: for
    /// an interior edge shared by triangles A and B, the slots
    /// [A*3 + e_A] and [B*3 + e_B] must carry the same value;
    /// SurfaceRouter2D::initialize enforces this when draining the
    /// [2D_EDGE_CONVEYANCE] pending rows.  See §11A of
    /// docs/2dModelStrategy.md.
    ///
    /// Cross-reference: this is the edge transmissivity ψ in the
    /// Integral-Porosity Shallow-Water literature (Sanders 2008;
    /// Bruwier et al. 2017).
    std::vector<double> edge_conveyance;

    // Surface properties
    std::vector<double> mannings_n;     ///< Manning's roughness coefficient
    std::vector<double> tri_init_depth; ///< Initial water depth (m, default 0 = dry)
    std::vector<double> tri_init_u;     ///< [2D_INITIAL_VELOCITY] u (m/s, default 0)
    std::vector<double> tri_init_v;     ///< [2D_INITIAL_VELOCITY] v (m/s, default 0)
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
    /// 1 when the [2D_VERTEX_NODE_MAP] row authored an explicit AREA token.
    /// Rows without one keep the 1.0 default value but are eligible for the
    /// COUPLING_AREA AUTO derivation at coupling-point resolve.
    std::vector<uint8_t> vert_coupling_area_set;
    std::vector<double> tri_coupling_cd;        ///< Discharge coefficient
    std::vector<double> tri_coupling_area;      ///< Effective exchange area

    // Deferred resolution names (populated during parsing, cleared after resolve)
    std::vector<std::string> vert_coupled_node_name;
    std::vector<std::string> tri_coupled_node_name;

    /// One authored node→cell coupling row (repeated-row form of
    /// [2D_TRIANGLE_NODE_MAP]). A triangle may carry several rows — one per
    /// coupled node — so these live in a flat vector, not per-triangle
    /// arrays. Source of truth for cell couplings: the parser, the GUI C API
    /// and (synthesised at resolve time) the legacy per-triangle arrays all
    /// feed this vector, and buildCouplingPoints() iterates it. The legacy
    /// arrays above are kept as a last-row-wins mirror for the existing
    /// getter API and the GeoPackage writer (single-coupling only).
    struct TriCouplingRow {
        int         tri  = -1;    ///< Triangle index
        int         node = -1;    ///< SWMM node index (-1 until resolved)
        std::string node_name;    ///< Deferred name (cleared after resolve)
        double      cd   = 0.65;  ///< Discharge coefficient
        double      area = 1.0;   ///< Effective exchange area
        bool        area_set = false; ///< True when the row authored an
                                      ///< explicit AREA token (COUPLING_AREA
                                      ///< AUTO derives the rest at resolve).
    };
    std::vector<TriCouplingRow> tri_couplings;

    // -----------------------------------------------------------------------
    // Capacity queries
    // -----------------------------------------------------------------------

    int n_vertices()  const noexcept { return static_cast<int>(vx.size()); }
    /// Number of CELLS (triangles + quads). The historical name is kept for
    /// the ~200 call sites; `n_cells()` is the same value.
    int n_triangles() const noexcept { return static_cast<int>(cell_nv.size()); }
    int n_cells()     const noexcept { return static_cast<int>(cell_nv.size()); }
    /// Number of quadrilateral cells (O(n) scan; not a hot path).
    int n_quads() const noexcept {
        int q = 0;
        for (uint8_t nv : cell_nv) q += (nv == 4);
        return q;
    }
    /// Edge-slot stride of the PUBLIC (API / HDF5) layout: 3 for an
    /// all-triangle mesh (byte-compatible with every existing consumer),
    /// kMaxCellVerts once a quad exists. Internal SoAs always use
    /// kMaxCellVerts.
    int edge_stride() const noexcept { return n_quads() > 0 ? kMaxCellVerts : 3; }
    /// Number of internal edge slots (n_cells * kMaxCellVerts).
    int n_edge_slots() const noexcept { return n_cells() * kMaxCellVerts; }

    // -----------------------------------------------------------------------
    // Cell / edge-slot accessors
    // -----------------------------------------------------------------------

    /// Flat edge/vertex slot of local index k in cell c.
    static constexpr int slot(int c, int k) noexcept { return c * kMaxCellVerts + k; }
    /// Local index k of a flat slot; cell of a flat slot.
    static constexpr int slot_local(int s) noexcept { return s % kMaxCellVerts; }
    static constexpr int slot_cell(int s)  noexcept { return s / kMaxCellVerts; }

    int cell_vertex_count(int c) const noexcept { return cell_nv[static_cast<std::size_t>(c)]; }
    int cell_vertex(int c, int k) const noexcept { return cell_v[static_cast<std::size_t>(slot(c, k))]; }
    int cell_neighbour(int c, int k) const noexcept { return cell_nbr[static_cast<std::size_t>(slot(c, k))]; }
    bool is_quad(int c) const noexcept { return cell_nv[static_cast<std::size_t>(c)] == 4; }

    /// Endpoint vertices of local edge k of cell c: v[(k+1)%nv], v[(k+2)%nv].
    void cell_edge_vertices(int c, int k, int& a, int& b) const noexcept {
        const int nv = cell_nv[static_cast<std::size_t>(c)];
        a = cell_v[static_cast<std::size_t>(slot(c, (k + 1) % nv))];
        b = cell_v[static_cast<std::size_t>(slot(c, (k + 2) % nv))];
    }

    void set_triangle(int c, int v0, int v1, int v2) noexcept {
        cell_nv[static_cast<std::size_t>(c)] = 3;
        cell_v[static_cast<std::size_t>(slot(c, 0))] = v0;
        cell_v[static_cast<std::size_t>(slot(c, 1))] = v1;
        cell_v[static_cast<std::size_t>(slot(c, 2))] = v2;
        cell_v[static_cast<std::size_t>(slot(c, 3))] = -1;
    }
    void set_quad(int c, int v0, int v1, int v2, int v3) noexcept {
        cell_nv[static_cast<std::size_t>(c)] = 4;
        cell_v[static_cast<std::size_t>(slot(c, 0))] = v0;
        cell_v[static_cast<std::size_t>(slot(c, 1))] = v1;
        cell_v[static_cast<std::size_t>(slot(c, 2))] = v2;
        cell_v[static_cast<std::size_t>(slot(c, 3))] = v3;
    }

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
        vert_coupling_area_set.resize(n, 0);
    }

    /// Resize the per-cell arrays to nt cells. New cells are TRIANGLES
    /// (cell_nv = 3, vertex slots 0 / padding −1) until set_quad() is called.
    void resize_triangles(int nt) {
        auto n = static_cast<std::size_t>(nt);
        const auto old_n = cell_nv.size();
        cell_nv.resize(n, 3);
        const auto n4 = n * static_cast<std::size_t>(kMaxCellVerts);
        cell_v.resize(n4, 0);
        cell_nbr.resize(n4, -1);
        // Padding slot of every NEW triangle: vertex −1, neighbour −2.
        for (std::size_t c = old_n; c < n; ++c) {
            cell_v[c * kMaxCellVerts + 3]   = -1;
            cell_nbr[c * kMaxCellVerts + 3] = -2;
        }
        tri_area.resize(n, 0.0);
        tri_cx.resize(n, 0.0);
        tri_cy.resize(n, 0.0);
        tri_cz.resize(n, 0.0);

        edge_length.resize(n4, 0.0);
        edge_nx.resize(n4, 0.0);
        edge_ny.resize(n4, 0.0);
        edge_mx.resize(n4, 0.0);
        edge_my.resize(n4, 0.0);
        edge_mz.resize(n4, 0.0);
        edge_dist_c.resize(n4, 0.0);
        edge_conveyance.resize(n4, 1.0);  // §11A — default unrestricted

        mannings_n.resize(n, 0.035);
        tri_init_depth.resize(n, 0.0);
        tri_init_u.resize(n, 0.0);
        tri_init_v.resize(n, 0.0);
        tri_tag.resize(n);
        tri_coupled_node.resize(n, -1);
        tri_coupled_node_name.resize(n);
        tri_coupling_cd.resize(n, 0.65);
        tri_coupling_area.resize(n, 1.0);
    }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_MESH_DATA_HPP
