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
 * @file MeshBuilder.cpp
 * @brief Implementation of mesh topology construction and geometry computation.
 *
 * @details Mixed triangle / quadrilateral cells (kMaxCellVerts = 4 padded
 *          layout, see MeshData.hpp). Triangle arithmetic is kept
 *          expression-for-expression identical to the triangle-only builder
 *          so an all-triangle mesh reproduces its geometry bit-for-bit; quads
 *          use the shoelace area and the area-weighted centroid of their two
 *          sub-triangles (the vertex mean is NOT the centroid of a skewed
 *          quad, and the Perot arms / ghost distances need the real one).
 *
 * @see MeshBuilder.hpp
 * @ingroup engine_2d
 */

#include "MeshBuilder.hpp"
#include "QuadVfr.hpp"

#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <utility>
#include <cstdint>

namespace openswmm::twoD {

namespace {

// Hash for (min, max) vertex pair — identifies a unique edge
struct EdgeKey {
    int v_lo, v_hi;
    bool operator==(const EdgeKey& o) const noexcept {
        return v_lo == o.v_lo && v_hi == o.v_hi;
    }
};

struct EdgeKeyHash {
    std::size_t operator()(const EdgeKey& k) const noexcept {
        // Cantor pairing
        auto a = static_cast<std::size_t>(k.v_lo);
        auto b = static_cast<std::size_t>(k.v_hi);
        return ((a + b) * (a + b + 1)) / 2 + b;
    }
};

// Edge record: first cell and local edge that claimed this edge
struct EdgeRecord {
    int cell_idx;
    int local_edge;  // 0 .. nv-1
};

inline EdgeKey makeEdgeKey(int va, int vb) {
    return {std::min(va, vb), std::max(va, vb)};
}

inline void computeQuadVfr(MeshData& mesh, int t);

} // anonymous namespace


void buildMeshTopology(MeshData& mesh) {
    const int nt = mesh.n_triangles();

    // --- 1. Build edge-neighbour adjacency ---

    // Map: sorted vertex pair → first cell that claimed this edge
    std::unordered_map<EdgeKey, EdgeRecord, EdgeKeyHash> edge_map;
    edge_map.reserve(static_cast<std::size_t>(nt) * kMaxCellVerts);

    // Initialize neighbours to -1 (boundary) on real slots, -2 on padding.
    for (int t = 0; t < nt; ++t) {
        const int nv = mesh.cell_vertex_count(t);
        for (int k = 0; k < kMaxCellVerts; ++k)
            mesh.cell_nbr[static_cast<std::size_t>(MeshData::slot(t, k))] =
                (k < nv) ? -1 : -2;
    }

    for (int t = 0; t < nt; ++t) {
        const int nv = mesh.cell_vertex_count(t);
        for (int e = 0; e < nv; ++e) {
            int va, vb;
            mesh.cell_edge_vertices(t, e, va, vb);
            auto key = makeEdgeKey(va, vb);

            auto it = edge_map.find(key);
            if (it == edge_map.end()) {
                // First cell to claim this edge
                edge_map[key] = {t, e};
            } else {
                // Second cell — complete the pair
                auto& first = it->second;
                mesh.cell_nbr[static_cast<std::size_t>(MeshData::slot(t, e))] =
                    first.cell_idx;
                mesh.cell_nbr[static_cast<std::size_t>(
                    MeshData::slot(first.cell_idx, first.local_edge))] = t;
            }
        }
    }

    // --- 2. Compute cell geometry ---

    for (int t = 0; t < nt; ++t) {
        const int nv = mesh.cell_vertex_count(t);
        if (nv == 3) {
            // Triangle: identical expressions to the triangle-only builder.
            int v0 = mesh.cell_vertex(t, 0);
            int v1 = mesh.cell_vertex(t, 1);
            int v2 = mesh.cell_vertex(t, 2);

            double x0 = mesh.vx[v0], y0 = mesh.vy[v0], z0 = mesh.vz[v0];
            double x1 = mesh.vx[v1], y1 = mesh.vy[v1], z1 = mesh.vz[v1];
            double x2 = mesh.vx[v2], y2 = mesh.vy[v2], z2 = mesh.vz[v2];

            // Centroid
            mesh.tri_cx[t] = (x0 + x1 + x2) / 3.0;
            mesh.tri_cy[t] = (y0 + y1 + y2) / 3.0;
            mesh.tri_cz[t] = (z0 + z1 + z2) / 3.0;

            // Area via cross product: 0.5 * ||(v1-v0) × (v2-v0)||
            double dx1 = x1 - x0, dy1 = y1 - y0;
            double dx2 = x2 - x0, dy2 = y2 - y0;
            mesh.tri_area[t] = 0.5 * std::abs(dx1 * dy2 - dx2 * dy1);
        } else {
            // Quadrilateral (v0 v1 v2 v3): shoelace area; centroid as the
            // area-weighted centroid of the two sub-triangles (v0,v1,v2) and
            // (v0,v2,v3) — a property of the polygon, independent of which
            // diagonal is used. Mean bed elevation over the four vertices.
            double x[4], y[4], z[4];
            for (int k = 0; k < 4; ++k) {
                const int v = mesh.cell_vertex(t, k);
                x[k] = mesh.vx[v]; y[k] = mesh.vy[v]; z[k] = mesh.vz[v];
            }
            const double a1s = 0.5 * ((x[1] - x[0]) * (y[2] - y[0]) -
                                      (x[2] - x[0]) * (y[1] - y[0]));
            const double a2s = 0.5 * ((x[2] - x[0]) * (y[3] - y[0]) -
                                      (x[3] - x[0]) * (y[2] - y[0]));
            const double a1 = std::abs(a1s), a2 = std::abs(a2s);
            const double area = a1 + a2;
            const double c1x = (x[0] + x[1] + x[2]) / 3.0;
            const double c1y = (y[0] + y[1] + y[2]) / 3.0;
            const double c2x = (x[0] + x[2] + x[3]) / 3.0;
            const double c2y = (y[0] + y[2] + y[3]) / 3.0;
            if (area > 0.0) {
                mesh.tri_cx[t] = (a1 * c1x + a2 * c2x) / area;
                mesh.tri_cy[t] = (a1 * c1y + a2 * c2y) / area;
            } else {
                mesh.tri_cx[t] = 0.25 * (x[0] + x[1] + x[2] + x[3]);
                mesh.tri_cy[t] = 0.25 * (y[0] + y[1] + y[2] + y[3]);
            }
            mesh.tri_cz[t] = 0.25 * (z[0] + z[1] + z[2] + z[3]);
            mesh.tri_area[t] = area;
        }
    }
    // Quad VFR side data (needs the connectivity + z only; sized on first quad).
    mesh.quad_vfr_z.clear();
    mesh.quad_vfr_a.clear();
    for (int t = 0; t < nt; ++t) computeQuadVfr(mesh, t);

    // --- 3. Compute edge geometry ---

    for (int t = 0; t < nt; ++t) {
        double cx = mesh.tri_cx[t];
        double cy = mesh.tri_cy[t];
        const int nv = mesh.cell_vertex_count(t);

        for (int e = 0; e < kMaxCellVerts; ++e) {
            int idx = MeshData::slot(t, e);
            if (e >= nv) {
                // Padding slot: zero geometry, so a stray read is inert.
                mesh.edge_mx[idx] = mesh.edge_my[idx] = mesh.edge_mz[idx] = 0.0;
                mesh.edge_length[idx] = 0.0;
                mesh.edge_nx[idx] = mesh.edge_ny[idx] = 0.0;
                mesh.edge_dist_c[idx] = 0.0;
                continue;
            }
            int va, vb;
            mesh.cell_edge_vertices(t, e, va, vb);

            double ax = mesh.vx[va], ay = mesh.vy[va], az = mesh.vz[va];
            double bx = mesh.vx[vb], by = mesh.vy[vb], bz = mesh.vz[vb];

            // Edge midpoint
            mesh.edge_mx[idx] = 0.5 * (ax + bx);
            mesh.edge_my[idx] = 0.5 * (ay + by);
            mesh.edge_mz[idx] = 0.5 * (az + bz);

            // Edge length (planimetric)
            double dx = bx - ax;
            double dy = by - ay;
            mesh.edge_length[idx] = std::sqrt(dx * dx + dy * dy);

            // Outward unit normal: perpendicular to edge, pointing away from centroid
            // Edge direction: (dx, dy). Normal candidates: (dy, -dx) or (-dy, dx)
            double nx = dy;
            double ny = -dx;

            // Ensure outward: dot product with (midpoint - centroid) should be positive
            double to_mid_x = mesh.edge_mx[idx] - cx;
            double to_mid_y = mesh.edge_my[idx] - cy;
            if (nx * to_mid_x + ny * to_mid_y < 0.0) {
                nx = -nx;
                ny = -ny;
            }

            // Normalize
            double len = std::sqrt(nx * nx + ny * ny);
            if (len > 1.0e-15) {
                mesh.edge_nx[idx] = nx / len;
                mesh.edge_ny[idx] = ny / len;
            } else {
                mesh.edge_nx[idx] = 0.0;
                mesh.edge_ny[idx] = 0.0;
            }
            // Centroid → edge normal distance (the ghost-cell arm).
            mesh.edge_dist_c[idx] =
                to_mid_x * mesh.edge_nx[idx] + to_mid_y * mesh.edge_ny[idx];
        }
    }
}


std::string validateMesh(const MeshData& mesh) {
    int nv_total = mesh.n_vertices();
    int nt = mesh.n_triangles();

    if (nv_total < 3) return "Mesh must have at least 3 vertices";
    if (nt < 1) return "Mesh must have at least 1 cell";

    for (int t = 0; t < nt; ++t) {
        const int nv = mesh.cell_vertex_count(t);
        const char* kind = (nv == 4) ? "Quad " : "Triangle ";
        if (nv != 3 && nv != 4) {
            std::ostringstream oss;
            oss << "Cell " << t << " has unsupported vertex count " << nv;
            return oss.str();
        }
        int v[4] = {-1, -1, -1, -1};
        for (int k = 0; k < nv; ++k) {
            v[k] = mesh.cell_vertex(t, k);
            if (v[k] < 0 || v[k] >= nv_total) {
                std::ostringstream oss;
                oss << kind << t << " has out-of-range vertex index";
                return oss.str();
            }
        }
        for (int a = 0; a < nv; ++a)
            for (int b = a + 1; b < nv; ++b)
                if (v[a] == v[b]) {
                    std::ostringstream oss;
                    oss << kind << t << " has duplicate vertex indices";
                    return oss.str();
                }

        if (mesh.tri_area[t] <= 0.0) {
            std::ostringstream oss;
            oss << kind << t << " has zero or negative area";
            return oss.str();
        }

        if (nv == 4) {
            // Convex and simple: every consecutive edge-pair cross product
            // has the same sign (either orientation is accepted — the edge
            // normals are oriented outward from the centroid regardless).
            int sign = 0;
            for (int k = 0; k < 4; ++k) {
                const int p = v[k], q = v[(k + 1) % 4], r = v[(k + 2) % 4];
                const double cr = (mesh.vx[q] - mesh.vx[p]) * (mesh.vy[r] - mesh.vy[q]) -
                                  (mesh.vy[q] - mesh.vy[p]) * (mesh.vx[r] - mesh.vx[q]);
                const int s = (cr > 0.0) ? 1 : (cr < 0.0) ? -1 : 0;
                if (s == 0 || (sign != 0 && s != sign)) {
                    std::ostringstream oss;
                    oss << "Quad " << t << " (vertices " << v[0] << " " << v[1]
                        << " " << v[2] << " " << v[3]
                        << ") is not a convex quadrilateral";
                    return oss.str();
                }
                sign = s;
            }
        }

        if (mesh.mannings_n[t] <= 0.0) {
            std::ostringstream oss;
            oss << kind << t << " has non-positive Manning's n";
            return oss.str();
        }
    }

    return {};
}

namespace {
// Quad VFR side data for cell t (no-op for triangles). Allocates the SoA on
// first use so all-triangle meshes carry nothing.
inline void computeQuadVfr(MeshData& mesh, int t) {
    if (mesh.cell_vertex_count(t) != 4) return;
    const auto nt = static_cast<std::size_t>(mesh.n_triangles());
    if (mesh.quad_vfr_z.size() != nt * kQuadVfrZ) mesh.quad_vfr_z.assign(nt * kQuadVfrZ, 0.0);
    if (mesh.quad_vfr_a.size() != nt * 2)         mesh.quad_vfr_a.assign(nt * 2, 0.0);
    double x[4], y[4], z[4];
    for (int k = 0; k < 4; ++k) {
        const int v = mesh.cell_vertex(t, k);
        x[k] = mesh.vx[v]; y[k] = mesh.vy[v]; z[k] = mesh.vz[v];
    }
    double A1 = 0.0, A2 = 0.0;
    quadVfrPrecompute(x, y, z, &mesh.quad_vfr_z[static_cast<std::size_t>(t) * kQuadVfrZ], A1, A2);
    mesh.quad_vfr_a[static_cast<std::size_t>(t) * 2 + 0] = A1;
    mesh.quad_vfr_a[static_cast<std::size_t>(t) * 2 + 1] = A2;
}

// Z-dependent geometry of one cell: mean bed and edge-midpoint beds. Triangle
// expressions kept identical to the triangle-only builder (bit-identity).
inline void recomputeCellZ(MeshData& mesh, int t) {
    const int nv = mesh.cell_vertex_count(t);
    if (nv == 3) {
        const int v0 = mesh.cell_vertex(t, 0);
        const int v1 = mesh.cell_vertex(t, 1);
        const int v2 = mesh.cell_vertex(t, 2);

        const double z0 = mesh.vz[v0];
        const double z1 = mesh.vz[v1];
        const double z2 = mesh.vz[v2];

        mesh.tri_cz[t] = (z0 + z1 + z2) / 3.0;

        // Edge e (0..2) is opposite vertex e; its endpoints are the other two
        // vertices of the triangle (same convention as cell_edge_vertices).
        mesh.edge_mz[MeshData::slot(t, 0)] = 0.5 * (z1 + z2);
        mesh.edge_mz[MeshData::slot(t, 1)] = 0.5 * (z2 + z0);
        mesh.edge_mz[MeshData::slot(t, 2)] = 0.5 * (z0 + z1);
        return;
    }
    double zsum = 0.0;
    for (int k = 0; k < nv; ++k) zsum += mesh.vz[mesh.cell_vertex(t, k)];
    mesh.tri_cz[t] = zsum / static_cast<double>(nv);
    for (int e = 0; e < nv; ++e) {
        int va, vb;
        mesh.cell_edge_vertices(t, e, va, vb);
        mesh.edge_mz[MeshData::slot(t, e)] = 0.5 * (mesh.vz[va] + mesh.vz[vb]);
    }
    computeQuadVfr(mesh, t);
}
} // namespace

void recomputeVertexZDependents(MeshData& mesh, int vidx) {
    const int nt = mesh.n_triangles();
    for (int t = 0; t < nt; ++t) {
        const int nv = mesh.cell_vertex_count(t);
        bool hit = false;
        for (int k = 0; k < nv && !hit; ++k) hit = (mesh.cell_vertex(t, k) == vidx);
        if (!hit) continue;
        recomputeCellZ(mesh, t);
    }
}

void recomputeAllZDependents(MeshData& mesh) {
    const int nt = mesh.n_triangles();
    for (int t = 0; t < nt; ++t) {
        // Identical body to recomputeVertexZDependents above, minus the
        // incidence filter — same operands in the same order, so the results
        // are bitwise equal to recomputing vertex by vertex.
        recomputeCellZ(mesh, t);
    }
}

} // namespace openswmm::twoD
