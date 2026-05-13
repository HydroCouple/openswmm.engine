/**
 * @file MeshBuilder.cpp
 * @brief Implementation of mesh topology construction and geometry computation.
 *
 * @see MeshBuilder.hpp
 * @ingroup engine_2d
 */

#include "MeshBuilder.hpp"

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

// Edge record: first triangle and local edge that claimed this edge
struct EdgeRecord {
    int tri_idx;
    int local_edge;  // 0, 1, 2
};

// Get the two vertex indices for local edge e of triangle t
// Edge 0 = opposite v0 → between v1 and v2
// Edge 1 = opposite v1 → between v2 and v0
// Edge 2 = opposite v2 → between v0 and v1
inline std::pair<int, int> edgeVertices(const MeshData& mesh, int t, int e) {
    int v[3] = {mesh.tri_v0[t], mesh.tri_v1[t], mesh.tri_v2[t]};
    int a = v[(e + 1) % 3];
    int b = v[(e + 2) % 3];
    return {a, b};
}

inline EdgeKey makeEdgeKey(int va, int vb) {
    return {std::min(va, vb), std::max(va, vb)};
}

} // anonymous namespace


void buildMeshTopology(MeshData& mesh) {
    int nt = mesh.n_triangles();
    int nv = mesh.n_vertices();

    // --- 1. Build edge-neighbour adjacency ---

    // Map: sorted vertex pair → first triangle that claimed this edge
    std::unordered_map<EdgeKey, EdgeRecord, EdgeKeyHash> edge_map;
    edge_map.reserve(nt * 3);

    // Initialize neighbours to -1 (boundary)
    std::fill(mesh.tri_nbr0.begin(), mesh.tri_nbr0.end(), -1);
    std::fill(mesh.tri_nbr1.begin(), mesh.tri_nbr1.end(), -1);
    std::fill(mesh.tri_nbr2.begin(), mesh.tri_nbr2.end(), -1);

    auto set_nbr = [&](int tri, int local_edge, int nbr_tri) {
        switch (local_edge) {
            case 0: mesh.tri_nbr0[tri] = nbr_tri; break;
            case 1: mesh.tri_nbr1[tri] = nbr_tri; break;
            case 2: mesh.tri_nbr2[tri] = nbr_tri; break;
        }
    };

    for (int t = 0; t < nt; ++t) {
        for (int e = 0; e < 3; ++e) {
            auto [va, vb] = edgeVertices(mesh, t, e);
            auto key = makeEdgeKey(va, vb);

            auto it = edge_map.find(key);
            if (it == edge_map.end()) {
                // First triangle to claim this edge
                edge_map[key] = {t, e};
            } else {
                // Second triangle — complete the pair
                auto& first = it->second;
                set_nbr(t, e, first.tri_idx);
                set_nbr(first.tri_idx, first.local_edge, t);
            }
        }
    }

    // --- 2. Compute cell geometry ---

    for (int t = 0; t < nt; ++t) {
        int v0 = mesh.tri_v0[t];
        int v1 = mesh.tri_v1[t];
        int v2 = mesh.tri_v2[t];

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
    }

    // --- 3. Compute edge geometry ---

    for (int t = 0; t < nt; ++t) {
        double cx = mesh.tri_cx[t];
        double cy = mesh.tri_cy[t];

        for (int e = 0; e < 3; ++e) {
            auto [va, vb] = edgeVertices(mesh, t, e);
            int idx = t * 3 + e;

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
        }
    }
}


std::string validateMesh(const MeshData& mesh) {
    int nv = mesh.n_vertices();
    int nt = mesh.n_triangles();

    if (nv < 3) return "Mesh must have at least 3 vertices";
    if (nt < 1) return "Mesh must have at least 1 triangle";

    for (int t = 0; t < nt; ++t) {
        int v0 = mesh.tri_v0[t], v1 = mesh.tri_v1[t], v2 = mesh.tri_v2[t];

        if (v0 < 0 || v0 >= nv || v1 < 0 || v1 >= nv || v2 < 0 || v2 >= nv) {
            std::ostringstream oss;
            oss << "Triangle " << t << " has out-of-range vertex index";
            return oss.str();
        }

        if (v0 == v1 || v1 == v2 || v0 == v2) {
            std::ostringstream oss;
            oss << "Triangle " << t << " has duplicate vertex indices";
            return oss.str();
        }

        if (mesh.tri_area[t] <= 0.0) {
            std::ostringstream oss;
            oss << "Triangle " << t << " has zero or negative area";
            return oss.str();
        }

        if (mesh.mannings_n[t] <= 0.0) {
            std::ostringstream oss;
            oss << "Triangle " << t << " has non-positive Manning's n";
            return oss.str();
        }
    }

    return {};
}

} // namespace openswmm::twoD
