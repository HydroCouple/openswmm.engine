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
 * @file test_2d_quad_mesh.cpp
 * @brief Mixed triangle/quadrilateral mesh gates
 *        (plans/2D_TRI_QUAD_MESH_PLAN_2026-09-06.md, phases Q1–Q2).
 *
 * @details
 *  - Geometry: shoelace area, area centroid (not the vertex mean), outward
 *    normals, centroid→edge distances, the unified edge convention
 *    edge k = (v[(k+1)%nv], v[(k+2)%nv]), padded-slot contract.
 *  - Topology: quad–quad and quad–triangle adjacency across a mixed strip;
 *    InertialEdges enumerates every shared edge exactly once.
 *  - Quad VFR (Begnudelli & Sanders 2007): the closure equals a brute-force
 *    numerical integration of the two-plane bed (independent oracle), round
 *    trips η ↔ h̄ to 1e-10, is monotone and hits all three diagonal cases.
 *  - Marcher on quads / mixed meshes: lake at rest bit-exact (FLAT + VFR),
 *    dry-neighbour wall, positivity with FOUR exporting faces (the β/nv
 *    share), closed-basin conservation, Manning steady slope on a quad strip.
 *  - Parser / writer: [2D_QUADS] rows, the triangles-then-quads index rule,
 *    convexity and ordering errors, byte-exact INIT_DEPTH/TAG round trip.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "2d/data/BoundaryData.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/input/SectionHandlers2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/mesh/QuadVfr.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialEdges.hpp"
#include "2d/solver/InertialKernels.hpp"

using namespace openswmm::twoD;

namespace {

// nx × ny grid of dx-square QUAD cells, bed from z(x, y).
template <typename ZFn>
MeshData makeQuadGrid(int nx, int ny, double dx, ZFn z, double n = 0.03) {
    MeshData mesh;
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx;
            mesh.vy[v] = j * dx;
            mesh.vz[v] = z(i * dx, j * dx);
        }
    mesh.resize_triangles(nx * ny);
    int c = 0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int v00 = j * nvx + i,       v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.set_quad(c++, v00, v10, v11, v01);   // CCW
        }
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

// Mixed strip: nx quads in the middle (columns 1..nx), one column of two
// triangles at each end. Cells: triangles first (4), then quads.
template <typename ZFn>
MeshData makeMixedStrip(int nx, double dx, ZFn z, double n = 0.03) {
    MeshData mesh;
    const int ncol = nx + 2, nvx = ncol + 1;
    mesh.resize_vertices(nvx * 2);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx; mesh.vy[v] = j * dx; mesh.vz[v] = z(i * dx, j * dx);
        }
    mesh.resize_triangles(4 + nx);
    auto V = [&](int i, int j) { return j * nvx + i; };
    // Left cap column 0 → two triangles, right cap column ncol-1 → two triangles.
    mesh.set_triangle(0, V(0, 0), V(1, 0), V(1, 1));
    mesh.set_triangle(1, V(0, 0), V(1, 1), V(0, 1));
    mesh.set_triangle(2, V(ncol - 1, 0), V(ncol, 0), V(ncol, 1));
    mesh.set_triangle(3, V(ncol - 1, 0), V(ncol, 1), V(ncol - 1, 1));
    for (int i = 0; i < nx; ++i) {
        const int col = i + 1;
        mesh.set_quad(4 + i, V(col, 0), V(col + 1, 0), V(col + 1, 1), V(col, 1));
    }
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

SurfaceStateData makeState(const MeshData& mesh) {
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.head[i]   = mesh.tri_cz[i];
        state.depth[i]  = 0.0;
        state.volume[i] = 0.0;
    }
    return state;
}

void seedSurface(const MeshData& mesh, const SolverOptions2D& opts,
                 SurfaceStateData& state, double eta) {
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.volume[i] = inertial::cellVolumeFromEta(mesh, opts, i, eta);
        inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i],
                               state.depth[i]);
    }
}

double totalVolume(const SurfaceStateData& state, int nt) {
    double s = 0.0;
    for (int i = 0; i < nt; ++i) s += state.volume[i];
    return s;
}

// Brute-force oracle: mean depth of water at stage eta over the quad's
// two-plane bed model (B&S 2007), by midpoint quadrature on each sub-triangle.
double bruteForceMeanDepth(const double* x, const double* y, const double* z,
                           double eta, int& kase) {
    double zs[6], A1, A2;
    kase = quadVfrPrecompute(x, y, z, zs, A1, A2);
    // Recover the sub-triangle vertex sets from the case (same rule as the
    // header) to integrate the planar beds.
    int p[4] = {0, 1, 2, 3};
    for (int i = 1; i < 4; ++i) {
        const int key = p[i]; int j = i - 1;
        while (j >= 0 && z[p[j]] > z[key]) { p[j + 1] = p[j]; --j; }
        p[j + 1] = key;
    }
    const int n1 = p[0], n2 = p[1], n3 = p[2], n4 = p[3];
    int t1[3], t2[3];
    if (kase == 1)      { t1[0]=n1;t1[1]=n2;t1[2]=n4; t2[0]=n1;t2[1]=n3;t2[2]=n4; }
    else if (kase == 2) { t1[0]=n1;t1[1]=n2;t1[2]=n4; t2[0]=n2;t2[1]=n3;t2[2]=n4; }
    else                { t1[0]=n1;t1[1]=n3;t1[2]=n4; t2[0]=n2;t2[1]=n3;t2[2]=n4; }
    auto integrate = [&](const int* t) {
        const double ax = x[t[0]], ay = y[t[0]], az = z[t[0]];
        const double bx = x[t[1]], by = y[t[1]], bz = z[t[1]];
        const double cx = x[t[2]], cy = y[t[2]], cz = z[t[2]];
        const double area = 0.5 * std::abs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
        const int N = 400;
        double sum = 0.0; int cnt = 0;
        for (int i = 0; i < N; ++i)
            for (int j = 0; j < N - i; ++j) {
                // Barycentric midpoints of a regular sub-triangulation.
                const double u = (i + 1.0 / 3.0) / N, v = (j + 1.0 / 3.0) / N;
                const double zb = az + u * (bz - az) + v * (cz - az);
                sum += std::max(0.0, eta - zb); ++cnt;
                if (j < N - i - 1) {
                    const double u2 = (i + 2.0 / 3.0) / N, v2 = (j + 2.0 / 3.0) / N;
                    const double zb2 = az + u2 * (bz - az) + v2 * (cz - az);
                    sum += std::max(0.0, eta - zb2); ++cnt;
                }
            }
        return area * sum / cnt;   // volume over this sub-triangle
    };
    return (integrate(t1) + integrate(t2)) / (A1 + A2);
}

}  // namespace

// ---------------------------------------------------------------------------
// Geometry of a single skewed quad.
// ---------------------------------------------------------------------------
TEST(QuadMesh, SkewedQuadGeometry) {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 4.0, 5.0, 1.0};
    mesh.vy = {0.0, 0.0, 3.0, 2.0};
    mesh.vz = {1.0, 1.2, 1.5, 1.1};
    mesh.resize_triangles(1);
    mesh.set_quad(0, 0, 1, 2, 3);
    buildMeshTopology(mesh);
    EXPECT_TRUE(validateMesh(mesh).empty()) << validateMesh(mesh);

    EXPECT_EQ(mesh.cell_vertex_count(0), 4);
    EXPECT_EQ(mesh.n_quads(), 1);
    EXPECT_EQ(mesh.edge_stride(), 4);
    // Shoelace: (0,0),(4,0),(5,3),(1,2) → area = 0.5*|0*0-4*0 + 4*3-5*0 + 5*2-1*3 + 1*0-0*2| = 0.5*|12+7| = 9.5
    EXPECT_NEAR(mesh.tri_area[0], 9.5, 1e-12);
    // Area centroid of the polygon (shoelace centroid formula).
    double cx = 0, cy = 0;
    for (int k = 0; k < 4; ++k) {
        const int a = k, b = (k + 1) % 4;
        const double cr = mesh.vx[a] * mesh.vy[b] - mesh.vx[b] * mesh.vy[a];
        cx += (mesh.vx[a] + mesh.vx[b]) * cr;
        cy += (mesh.vy[a] + mesh.vy[b]) * cr;
    }
    cx /= 6.0 * 9.5; cy /= 6.0 * 9.5;
    EXPECT_NEAR(mesh.tri_cx[0], cx, 1e-12);
    EXPECT_NEAR(mesh.tri_cy[0], cy, 1e-12);
    // The vertex mean is NOT the centroid of this quad (guards D5).
    EXPECT_GT(std::fabs(mesh.tri_cx[0] - 2.5) + std::fabs(mesh.tri_cy[0] - 1.25), 1e-3);
    EXPECT_NEAR(mesh.tri_cz[0], 1.2, 1e-12);

    // Edge convention and outward normals: edge k = (v[(k+1)%4], v[(k+2)%4]).
    for (int k = 0; k < 4; ++k) {
        int a, b;
        mesh.cell_edge_vertices(0, k, a, b);
        EXPECT_EQ(a, (k + 1) % 4);
        EXPECT_EQ(b, (k + 2) % 4);
        const int s = MeshData::slot(0, k);
        const double len = std::hypot(mesh.vx[b] - mesh.vx[a], mesh.vy[b] - mesh.vy[a]);
        EXPECT_NEAR(mesh.edge_length[s], len, 1e-12);
        EXPECT_NEAR(mesh.edge_nx[s] * mesh.edge_nx[s] + mesh.edge_ny[s] * mesh.edge_ny[s], 1.0, 1e-12);
        // Outward: (midpoint − centroid)·n > 0 and equals edge_dist_c.
        const double dmx = mesh.edge_mx[s] - mesh.tri_cx[0];
        const double dmy = mesh.edge_my[s] - mesh.tri_cy[0];
        EXPECT_GT(mesh.edge_dist_c[s], 0.0);
        EXPECT_NEAR(mesh.edge_dist_c[s], dmx * mesh.edge_nx[s] + dmy * mesh.edge_ny[s], 1e-12);
        EXPECT_EQ(mesh.cell_neighbour(0, k), -1);
    }
}

// edge_dist_c on a triangle equals the classic 2A/(3L) (what every stage-BC
// ghost used before), so the quad path is the same quantity generalised.
TEST(QuadMesh, TriangleEdgeDistMatchesTwoAOverThreeL) {
    MeshData mesh;
    mesh.resize_vertices(3);
    mesh.vx = {0.0, 3.0, 1.0}; mesh.vy = {0.0, 0.5, 2.5}; mesh.vz = {0, 0, 0};
    mesh.resize_triangles(1);
    mesh.set_triangle(0, 0, 1, 2);
    buildMeshTopology(mesh);
    for (int k = 0; k < 3; ++k) {
        const int s = MeshData::slot(0, k);
        EXPECT_NEAR(mesh.edge_dist_c[s], 2.0 * mesh.tri_area[0] / (3.0 * mesh.edge_length[s]), 1e-12);
    }
    EXPECT_EQ(mesh.edge_dist_c[MeshData::slot(0, 3)], 0.0);   // padding
    EXPECT_EQ(mesh.cell_neighbour(0, 3), -2);
}

// Non-convex / self-intersecting quads are rejected with the cell named.
TEST(QuadMesh, NonConvexQuadRejected) {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 4.0, 1.0, 0.0};     // (1,1) is a re-entrant corner
    mesh.vy = {0.0, 0.0, 1.0, 4.0};
    mesh.vz = {0, 0, 0, 0};
    mesh.resize_triangles(1);
    mesh.set_quad(0, 0, 1, 2, 3);
    mesh.mannings_n[0] = 0.03;
    buildMeshTopology(mesh);
    const std::string err = validateMesh(mesh);
    EXPECT_NE(err.find("not a convex quadrilateral"), std::string::npos) << err;
    // Bow-tie ordering of a square is likewise rejected.
    mesh.vx = {0.0, 1.0, 0.0, 1.0}; mesh.vy = {0.0, 1.0, 1.0, 0.0};
    buildMeshTopology(mesh);
    EXPECT_FALSE(validateMesh(mesh).empty());
}

// Clockwise quads are accepted: normals are oriented outward from the
// centroid regardless of vertex orientation.
TEST(QuadMesh, ClockwiseQuadAccepted) {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 0.0, 2.0, 2.0}; mesh.vy = {0.0, 2.0, 2.0, 0.0}; mesh.vz = {0, 0, 0, 0};
    mesh.resize_triangles(1);
    mesh.set_quad(0, 0, 1, 2, 3);   // CW
    mesh.mannings_n[0] = 0.03;
    buildMeshTopology(mesh);
    EXPECT_TRUE(validateMesh(mesh).empty()) << validateMesh(mesh);
    EXPECT_NEAR(mesh.tri_area[0], 4.0, 1e-12);
    for (int k = 0; k < 4; ++k) EXPECT_GT(mesh.edge_dist_c[MeshData::slot(0, k)], 0.0);
}

// ---------------------------------------------------------------------------
// Topology across a mixed strip: every shared edge is found exactly once,
// quad–triangle adjacency works, InertialEdges' CSR covers all faces.
// ---------------------------------------------------------------------------
TEST(QuadMesh, MixedStripAdjacencyAndUniqueEdges) {
    auto mesh = makeMixedStrip(5, 1.0, [](double x, double) { return 0.01 * x; });
    ASSERT_TRUE(validateMesh(mesh).empty()) << validateMesh(mesh);
    EXPECT_EQ(mesh.n_triangles(), 9);
    EXPECT_EQ(mesh.n_quads(), 5);
    // Interior shared edges: the strip is a chain of 7 columns (6 column
    // interfaces) plus the diagonal inside each triangle cap (2).
    InertialEdges ed;
    ed.build(mesh);
    EXPECT_EQ(ed.ne, 6 + 2);
    // Each interior edge appears on both sides with consistent slots.
    for (int e = 0; e < ed.ne; ++e) {
        EXPECT_LT(ed.cL[e], ed.cR[e]);
        EXPECT_EQ(MeshData::slot_cell(ed.slotL[e]), ed.cL[e]);
        EXPECT_EQ(MeshData::slot_cell(ed.slotR[e]), ed.cR[e]);
        EXPECT_NEAR(mesh.edge_length[ed.slotL[e]], mesh.edge_length[ed.slotR[e]], 1e-12);
        // Opposite outward normals across the face.
        EXPECT_NEAR(mesh.edge_nx[ed.slotL[e]], -mesh.edge_nx[ed.slotR[e]], 1e-12);
        EXPECT_NEAR(mesh.edge_ny[ed.slotL[e]], -mesh.edge_ny[ed.slotR[e]], 1e-12);
    }
    // CSR incidence: quads carry 2 interior faces in the chain, the cap
    // triangles 2 (one diagonal + one column interface) or 1.
    for (int c = 4; c < 9; ++c)
        EXPECT_EQ(ed.cell_ptr[c + 1] - ed.cell_ptr[c], 2) << "quad " << c;
    // Vertex stencils reach every cell.
    int total = 0;
    for (int c = 0; c < mesh.n_triangles(); ++c) total += mesh.cell_vertex_count(c);
    EXPECT_EQ(total, 4 * 3 + 5 * 4);
}

// ---------------------------------------------------------------------------
// Quad VFR closure vs a brute-force integration of the two-plane bed.
// ---------------------------------------------------------------------------
TEST(QuadVfr, MatchesBruteForceIntegrationAllCases) {
    std::mt19937 rng(20260906);
    std::uniform_real_distribution<double> uz(0.0, 1.0), ux(-0.2, 0.2);
    int seen[4] = {0, 0, 0, 0};
    for (int trial = 0; trial < 60; ++trial) {
        // Perturbed unit square (stays convex for these perturbations).
        double x[4] = {0.0 + ux(rng), 1.0 + ux(rng), 1.0 + ux(rng), 0.0 + ux(rng)};
        double y[4] = {0.0 + ux(rng), 0.0 + ux(rng), 1.0 + ux(rng), 1.0 + ux(rng)};
        double z[4] = {uz(rng), uz(rng), uz(rng), uz(rng)};
        double zs[6], A1, A2;
        const int kase = quadVfrPrecompute(x, y, z, zs, A1, A2);
        ASSERT_GE(kase, 1); ASSERT_LE(kase, 3);
        ++seen[kase];
        ASSERT_GT(A1 + A2, 0.0);
        const double zlo = *std::min_element(z, z + 4), zhi = *std::max_element(z, z + 4);
        for (int m = 0; m <= 10; ++m) {
            const double eta = zlo - 0.05 + (zhi - zlo + 0.4) * m / 10.0;
            int kk;
            const double oracle = bruteForceMeanDepth(x, y, z, eta, kk);
            const double got    = quadMeanDepthFromEta(zs, A1, A2, eta, 0.0);
            EXPECT_NEAR(got, oracle, 2.0e-4 * (1.0 + oracle))
                << "case " << kase << " eta " << eta;
        }
    }
    // All three B&S topologies were exercised.
    EXPECT_GT(seen[1], 0); EXPECT_GT(seen[2], 0); EXPECT_GT(seen[3], 0);
}

TEST(QuadVfr, RoundTripMonotoneAndRegularised) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> uz(0.0, 2.0);
    for (int trial = 0; trial < 40; ++trial) {
        double x[4] = {0, 3, 3, 0}, y[4] = {0, 0, 2, 2};
        double z[4] = {uz(rng), uz(rng), uz(rng), uz(rng)};
        double zs[6], A1, A2;
        quadVfrPrecompute(x, y, z, zs, A1, A2);
        for (double eps : {0.0, 0.01, 0.05}) {
            double prev = -1.0;
            for (int m = 0; m <= 40; ++m) {
                const double h = 0.001 + 0.1 * m;
                const double eta = quadEtaFromMeanDepth(zs, A1, A2, h, eps);
                const double back = quadMeanDepthFromEta(zs, A1, A2, eta, eps);
                EXPECT_NEAR(back, h, 1.0e-10 * (1.0 + h)) << "eps " << eps << " h " << h;
                EXPECT_GT(eta, prev);        // strictly monotone in h̄
                prev = eta;
            }
            // Dry limit maps back to zero depth; fully wet is the flat closure.
            const double eta0 = quadEtaFromMeanDepth(zs, A1, A2, 0.0, eps);
            EXPECT_NEAR(quadMeanDepthFromEta(zs, A1, A2, eta0, eps), 0.0, 1e-14);
            const double zw = (A1 * (zs[0] + zs[1] + zs[2]) / 3.0 +
                               A2 * (zs[3] + zs[4] + zs[5]) / 3.0) / (A1 + A2);
            EXPECT_NEAR(quadEtaFromMeanDepth(zs, A1, A2, 5.0, eps), zw + 5.0, 1e-12);
        }
    }
}

// A planar quad (four coplanar vertices) reduces to the flat closure exactly
// once wet, and the split's diagonal choice cannot matter.
TEST(QuadVfr, PlanarQuadIsExactPlane) {
    double x[4] = {0, 2, 2, 0}, y[4] = {0, 0, 1, 1};
    double z[4] = {0.0, 0.2, 0.5, 0.3};   // z = 0.1x + 0.3y — planar
    double zs[6], A1, A2;
    quadVfrPrecompute(x, y, z, zs, A1, A2);
    EXPECT_NEAR(A1 + A2, 2.0, 1e-12);
    const double zmean = 0.25;
    for (double eta : {0.6, 1.0, 3.0})
        EXPECT_NEAR(quadMeanDepthFromEta(zs, A1, A2, eta, 0.0), eta - zmean, 1e-12);
    // Partially wet: exact for a plane inclined along x only.
    double zz[4] = {0.0, 0.4, 0.4, 0.0};   // z = 0.2 x
    quadVfrPrecompute(x, y, zz, zs, A1, A2);
    const double eta = 0.2;                // wets x < 1: depth ∫₀¹(0.2−0.2x)dx / 2 = 0.05
    EXPECT_NEAR(quadMeanDepthFromEta(zs, A1, A2, eta, 0.0), 0.05, 1e-12);
}

// ---------------------------------------------------------------------------
// Marcher gates on quad / mixed meshes.
// ---------------------------------------------------------------------------
TEST(QuadMarcher, LakeAtRestExactFlatAndVfrOnQuadsAndMixed) {
    for (const auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
        for (int kind = 0; kind < 2; ++kind) {
            auto bed = [](double x, double y) {
                return 0.05 * x + 0.11 * y + 0.3 * std::sin(0.7 * x);
            };
            MeshData mesh = (kind == 0) ? makeQuadGrid(6, 6, 5.0, bed)
                                        : makeMixedStrip(8, 5.0, bed);
            SolverOptions2D opts;
            opts.cell_closure = closure;
            auto state = makeState(mesh);
            seedSurface(mesh, opts, state, /*eta=*/8.0);
            const std::vector<double> v0 = state.volume;

            ExplicitInertialSolver solver;
            solver.initialize(mesh, state, opts);
            EXPECT_DOUBLE_EQ(solver.advance(0.0, 600.0), 600.0);
            for (int i = 0; i < mesh.n_triangles(); ++i)
                EXPECT_NEAR(state.volume[i], v0[i], 1.0e-9 * (v0[i] + 1.0))
                    << "cell " << i << " moved at rest (kind " << kind << ")";
            for (double f : state.edge_flux)
                EXPECT_LE(std::fabs(f), 1.0e-10) << "flux at rest";
            solver.finalize();
        }
    }
}

TEST(QuadMarcher, DryNeighbourWallNoCreepOnQuads) {
    auto mesh = makeQuadGrid(8, 4, 2.0, [](double x, double) {
        return (x < 8.0) ? 0.0 : 2.0;
    });
    SolverOptions2D opts;
    auto state = makeState(mesh);
    seedSurface(mesh, opts, state, 1.0);
    const std::vector<double> v0 = state.volume;
    const double sum0 = totalVolume(state, mesh.n_triangles());
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    solver.advance(0.0, 600.0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (mesh.tri_cz[i] > 1.5) EXPECT_EQ(state.volume[i], 0.0);
    EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1.0e-9 * sum0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        EXPECT_NEAR(state.volume[i], v0[i], 1.0e-9 * (v0[i] + 1.0));
    solver.finalize();
}

// Positivity with FOUR exporting faces: one flooded centre quad in a dry 3×3
// patch. Every face may take β/4 of the exporter, so the centre cell can
// never go negative and the basin total is conserved.
TEST(QuadMarcher, PositivityWithFourExportingFaces) {
    auto mesh = makeQuadGrid(3, 3, 1.0, [](double, double) { return 0.0; });
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    auto state = makeState(mesh);
    const int centre = 4;
    state.volume[centre] = 3.0 * mesh.tri_area[centre];
    inertial::cellEtaDepth(mesh, opts, centre, state.volume[centre],
                           state.head[centre], state.depth[centre]);
    const double sum0 = totalVolume(state, mesh.n_triangles());
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    for (int chunk = 0; chunk < 20; ++chunk) {
        solver.advance(chunk * 0.5, (chunk + 1) * 0.5);
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            ASSERT_GE(state.volume[i], 0.0) << "negative volume, cell " << i;
            ASSERT_FALSE(std::isnan(state.volume[i]));
        }
    }
    EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1.0e-10 * sum0);
    // Water actually spread to all four neighbours.
    for (int nb : {1, 3, 5, 7}) EXPECT_GT(state.volume[nb], 0.0);
    solver.finalize();
}

TEST(QuadMarcher, ClosedBasinConservationMixed) {
    auto mesh = makeMixedStrip(20, 1.0, [](double, double) { return 0.0; });
    SolverOptions2D opts;
    auto state = makeState(mesh);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (mesh.tri_cx[i] < 5.0) {
            state.volume[i] = 1.0 * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
        }
    const double sum0 = totalVolume(state, mesh.n_triangles());
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    for (int chunk = 0; chunk < 40; ++chunk) {
        solver.advance(chunk * 2.0, (chunk + 1) * 2.0);
        for (int i = 0; i < mesh.n_triangles(); ++i)
            ASSERT_GE(state.volume[i], 0.0);
    }
    EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1.0e-10 * sum0);
    solver.finalize();
}

// Manning steady slope on a QUAD strip: the aligned cells remove the
// diagonal-face friction term of the union-jack mesh, so the gate tightens
// from the triangle test's 6 % to 4 % (the ~3 % that remains is the
// first-order upwind face-depth staggering — half a cell of the (L−x)^{3/5}
// profile — which is shape-independent).
TEST(QuadMarcher, ManningSteadySlopeRainOutflowQuadStrip) {
    const double S = 0.01, dx = 1.0, n_man = 0.03;
    const int nx = 40, ny = 4;
    auto mesh = makeQuadGrid(nx, ny, dx, [&](double x, double) { return S * x; }, n_man);
    SolverOptions2D opts;
    auto state = makeState(mesh);
    BoundaryData boundary;
    boundary.resize(mesh.n_edge_slots());
    int outlet_slots = 0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        for (int e = 0; e < mesh.cell_vertex_count(i); ++e) {
            const int idx = MeshData::slot(i, e);
            if (mesh.cell_neighbour(i, e) >= 0) continue;
            if (mesh.edge_mx[idx] < 1.0e-9) {
                boundary.edge_bc_type[idx] = static_cast<int8_t>(BoundaryType::NORMAL_FLOW);
                boundary.edge_bed_slope[idx] = S;
                ++outlet_slots;
            }
        }
    ASSERT_EQ(outlet_slots, ny);
    state.boundary = &boundary;
    const double rain = 2.0e-4;
    for (int i = 0; i < mesh.n_triangles(); ++i) state.rainfall[i] = rain;

    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    solver.advance(0.0, 6000.0);
    const double T = 1000.0;
    const double v_start = totalVolume(state, mesh.n_triangles());
    solver.advance(6000.0, 6000.0 + T);
    const double v_end = totalVolume(state, mesh.n_triangles());
    double q_out = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        for (int e = 0; e < mesh.cell_vertex_count(i); ++e) {
            const int idx = MeshData::slot(i, e);
            if (boundary.edge_bc_type[idx] == static_cast<int8_t>(BoundaryType::NORMAL_FLOW))
                q_out += -state.edge_flux[idx];
        }
    double q_rain = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i) q_rain += rain * mesh.tri_area[i];
    EXPECT_NEAR(q_rain * T, q_out * T + (v_end - v_start), 1.0e-8 * q_rain * T);
    EXPECT_NEAR(q_out, q_rain, 0.10 * q_rain);

    const double L = nx * dx, x_mid = 0.5 * L;
    const double q_mid = rain * (L - x_mid);
    const double h_n = std::pow(n_man * q_mid / std::sqrt(S), 3.0 / 5.0);
    double h_avg = 0.0; int h_cnt = 0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (std::fabs(mesh.tri_cx[i] - x_mid) < dx) { h_avg += state.depth[i]; ++h_cnt; }
    h_avg /= h_cnt;
    EXPECT_NEAR(h_avg, h_n, 0.04 * h_n);
    solver.finalize();
}

// ---------------------------------------------------------------------------
// Parser: [2D_QUADS] grammar and the triangles-then-quads index rule.
// ---------------------------------------------------------------------------
TEST(QuadParser, QuadRowsAppendAfterTriangles) {
    MeshData mesh;
    mesh.resize_vertices(6);
    mesh.vx = {0, 1, 2, 0, 1, 2}; mesh.vy = {0, 0, 0, 1, 1, 1}; mesh.vz = {0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(parse2DTriangleLine({"0", "1", "4", "0.03"}, mesh).empty());
    EXPECT_TRUE(parse2DTriangleLine({"0", "4", "3", "0.03", "0.5", "cap"}, mesh).empty());
    EXPECT_TRUE(parse2DQuadLine({"1", "2", "5", "4", "0.02", "street"}, mesh).empty());
    EXPECT_TRUE(parse2DQuadLine({"1", "2", "5", "4", "0.02", "0.1", "street2"}, mesh).empty());
    ASSERT_EQ(mesh.n_triangles(), 4);
    EXPECT_EQ(mesh.n_quads(), 2);
    EXPECT_EQ(mesh.cell_vertex_count(2), 4);
    EXPECT_EQ(mesh.cell_vertex(2, 3), 4);
    EXPECT_EQ(mesh.tri_tag[2], "street");
    EXPECT_DOUBLE_EQ(mesh.tri_init_depth[2], 0.0);
    EXPECT_DOUBLE_EQ(mesh.tri_init_depth[3], 0.1);
    EXPECT_EQ(mesh.tri_tag[3], "street2");
    EXPECT_DOUBLE_EQ(mesh.mannings_n[3], 0.02);
    // A triangle row after a quad row breaks the index contract.
    const std::string err = parse2DTriangleLine({"0", "1", "2", "0.03"}, mesh);
    EXPECT_NE(err.find("must precede"), std::string::npos) << err;
    // Too few tokens / bad vertex.
    EXPECT_FALSE(parse2DQuadLine({"1", "2", "5", "0.02"}, mesh).empty());
    EXPECT_FALSE(parse2DQuadLine({"1", "x", "5", "4", "0.02"}, mesh).empty());
}

TEST(QuadParser, BoundaryEdgeIndexAcceptsThreeForQuads) {
    std::vector<SurfaceRouter2D::PendingBoundaryRow> rows;
    EXPECT_TRUE(parse2DBoundaryConditionsLine({"7", "3", "WALL"}, rows).empty());
    EXPECT_FALSE(parse2DBoundaryConditionsLine({"7", "4", "WALL"}, rows).empty());
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].edge, 3);
}

// Options: MOMENTUM_EQUATION / RECONSTRUCTION_ORDER round trip and ADVECTION
// deprecation warning.
TEST(QuadParser, MomentumEquationOption) {
    SolverOptions2D opts;
    std::vector<std::string> warnings;
    EXPECT_TRUE(parse2DOptionsLine({"MOMENTUM_EQUATION", "FULL_SWE"}, opts, &warnings).empty());
    EXPECT_EQ(opts.momentum, Momentum2D::FULL_SWE);
    EXPECT_EQ(format2DOptionValue(opts, "MOMENTUM_EQUATION"), "FULL_SWE");
    EXPECT_TRUE(parse2DOptionsLine({"MOMENTUM_EQUATION", "diffusive_wave"}, opts, &warnings).empty());
    EXPECT_EQ(opts.momentum, Momentum2D::DIFFUSIVE_WAVE);
    EXPECT_TRUE(parse2DOptionsLine({"MOMENTUM_EQUATION", "LOCAL_INERTIAL"}, opts, &warnings).empty());
    EXPECT_EQ(opts.momentum, Momentum2D::LOCAL_INERTIAL);
    EXPECT_FALSE(parse2DOptionsLine({"MOMENTUM_EQUATION", "BOGUS"}, opts, &warnings).empty());
    EXPECT_TRUE(parse2DOptionsLine({"RECONSTRUCTION_ORDER", "2"}, opts, &warnings).empty());
    EXPECT_EQ(opts.reconstruction_order, 2);
    EXPECT_FALSE(parse2DOptionsLine({"RECONSTRUCTION_ORDER", "3"}, opts, &warnings).empty());
    EXPECT_TRUE(is2DOptionKey("MOMENTUM_EQUATION"));
    EXPECT_TRUE(is2DOptionKey("RECONSTRUCTION_ORDER"));
    EXPECT_TRUE(warnings.empty());
    EXPECT_TRUE(parse2DOptionsLine({"ADVECTION", "YES"}, opts, &warnings).empty());
    EXPECT_TRUE(opts.advection);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("deprecated"), std::string::npos);
}
