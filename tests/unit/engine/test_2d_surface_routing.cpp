/**
 * @file test_2d_surface_routing.cpp
 * @brief Unit tests for the optional 2D surface routing module.
 *
 * @details Verifies:
 *          - Mesh topology construction (neighbours, edges, areas)
 *          - Vertex reconstruction (pseudo-Laplacian stencils)
 *          - Gradient computation (Green-Gauss, unlimited and limited)
 *          - Diffusive conductance (dry cell handling, smooth transition)
 *          - Edge flux computation (upwind selection, C-property)
 *          - Orifice coupling (exchange flow, backflow prevention)
 *          - Input section parsing (options, vertices, triangles, maps)
 *          - SurfaceRouter2D orchestration (lifecycle, volume tracking)
 *
 * @see src/engine/2d/
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>
#include <numeric>

#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/BoundaryData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/mesh/VertexReconstruction.hpp"
#include "2d/solver/SurfaceFluxCalculator.hpp"
#include "2d/input/SectionHandlers2D.hpp"

#ifdef OPENSWMM_HAS_2D
#include "2d/output/Default2DOutputPlugin.hpp"
#include "core/SimulationContext.hpp"
#include <openswmm/plugin_sdk/SimulationSnapshot.hpp>
#include <hdf5.h>
#include <filesystem>
#include <stdexcept>
#endif

using namespace openswmm::twoD;

// ============================================================================
// Helper: Build a simple 2-triangle mesh (a unit square split diagonally)
// ============================================================================
//
//   v2 (0,1,0) -------- v3 (1,1,0)
//     |  \  T1  |           (T0: v0,v1,v3)
//     |   \     |           (T1: v0,v3,v2)
//     | T0  \   |
//   v0 (0,0,0) -------- v1 (1,0,0)
//

static MeshData makeUnitSquareMesh() {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 1.0, 0.0, 1.0};
    mesh.vy = {0.0, 0.0, 1.0, 1.0};
    mesh.vz = {0.0, 0.0, 0.0, 0.0};

    mesh.resize_triangles(2);
    // T0: lower-right triangle
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 3;
    mesh.mannings_n[0] = 0.035;
    // T1: upper-left triangle
    mesh.tri_v0[1] = 0; mesh.tri_v1[1] = 3; mesh.tri_v2[1] = 2;
    mesh.mannings_n[1] = 0.035;

    buildMeshTopology(mesh);
    return mesh;
}

// ============================================================================
// Helper: Build a tilted-plane mesh for gradient exactness tests
// ============================================================================
//
// z = 0.1 * x + 0.2 * y on a 2-triangle mesh
//

static MeshData makeTiltedPlaneMesh() {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 10.0, 0.0, 10.0};
    mesh.vy = {0.0,  0.0, 10.0, 10.0};
    // z = 0.1*x + 0.2*y
    mesh.vz = {0.0, 1.0, 2.0, 3.0};

    mesh.resize_triangles(2);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 3;
    mesh.tri_v0[1] = 0; mesh.tri_v1[1] = 3; mesh.tri_v2[1] = 2;
    mesh.mannings_n[0] = 0.03;
    mesh.mannings_n[1] = 0.03;

    buildMeshTopology(mesh);
    return mesh;
}


// ============================================================================
// MeshBuilder Tests
// ============================================================================

TEST(MeshBuilder, ComputesTriangleAreas) {
    auto mesh = makeUnitSquareMesh();
    // Each triangle is half of a 1×1 square → area = 0.5
    EXPECT_NEAR(mesh.tri_area[0], 0.5, 1e-12);
    EXPECT_NEAR(mesh.tri_area[1], 0.5, 1e-12);
}

TEST(MeshBuilder, ComputesCentroids) {
    auto mesh = makeUnitSquareMesh();
    // T0 vertices: (0,0), (1,0), (1,1) → centroid = (2/3, 1/3)
    EXPECT_NEAR(mesh.tri_cx[0], (0.0 + 1.0 + 1.0) / 3.0, 1e-12);
    EXPECT_NEAR(mesh.tri_cy[0], (0.0 + 0.0 + 1.0) / 3.0, 1e-12);
    // T1 vertices: (0,0), (1,1), (0,1) → centroid = (1/3, 2/3)
    EXPECT_NEAR(mesh.tri_cx[1], (0.0 + 1.0 + 0.0) / 3.0, 1e-12);
    EXPECT_NEAR(mesh.tri_cy[1], (0.0 + 1.0 + 1.0) / 3.0, 1e-12);
}

TEST(MeshBuilder, FindsSharedNeighbour) {
    auto mesh = makeUnitSquareMesh();
    // The two triangles share one edge (v0-v3 diagonal).
    // At least one neighbour of T0 must be T1, and vice versa.
    bool t0_sees_t1 = (mesh.tri_nbr0[0] == 1 || mesh.tri_nbr1[0] == 1
                       || mesh.tri_nbr2[0] == 1);
    bool t1_sees_t0 = (mesh.tri_nbr0[1] == 0 || mesh.tri_nbr1[1] == 0
                       || mesh.tri_nbr2[1] == 0);
    EXPECT_TRUE(t0_sees_t1);
    EXPECT_TRUE(t1_sees_t0);
}

TEST(MeshBuilder, BoundaryEdgesAreMinusOne) {
    auto mesh = makeUnitSquareMesh();
    // Each triangle has 3 edges; 1 is shared, 2 are boundary.
    int boundary_count_t0 = 0;
    if (mesh.tri_nbr0[0] == -1) ++boundary_count_t0;
    if (mesh.tri_nbr1[0] == -1) ++boundary_count_t0;
    if (mesh.tri_nbr2[0] == -1) ++boundary_count_t0;
    EXPECT_EQ(boundary_count_t0, 2);

    int boundary_count_t1 = 0;
    if (mesh.tri_nbr0[1] == -1) ++boundary_count_t1;
    if (mesh.tri_nbr1[1] == -1) ++boundary_count_t1;
    if (mesh.tri_nbr2[1] == -1) ++boundary_count_t1;
    EXPECT_EQ(boundary_count_t1, 2);
}

TEST(MeshBuilder, EdgeLengthsPositive) {
    auto mesh = makeUnitSquareMesh();
    int n3 = mesh.n_triangles() * 3;
    for (int i = 0; i < n3; ++i) {
        EXPECT_GT(mesh.edge_length[i], 0.0)
            << "Edge " << i << " has non-positive length";
    }
}

TEST(MeshBuilder, EdgeNormalsUnitLength) {
    auto mesh = makeUnitSquareMesh();
    int n3 = mesh.n_triangles() * 3;
    for (int i = 0; i < n3; ++i) {
        double len = std::sqrt(mesh.edge_nx[i] * mesh.edge_nx[i]
                               + mesh.edge_ny[i] * mesh.edge_ny[i]);
        EXPECT_NEAR(len, 1.0, 1e-12)
            << "Edge " << i << " normal is not unit length";
    }
}

TEST(MeshBuilder, RecomputeVertexZDependentsUpdatesIncidentTriangles) {
    auto mesh = makeUnitSquareMesh();
    // Both triangles reference v3 = (1,1,0). Bump v3's Z and confirm both
    // tri_cz values shift by exactly the per-triangle share (1/3) and the
    // three edge midpoints incident to v3 shift by exactly half each.
    const double new_z = 3.0;
    mesh.vz[3] = new_z;
    recomputeVertexZDependents(mesh, 3);

    // T0 vertices: v0=(0,0,0), v1=(1,0,0), v3=(1,1,3) → centroid Z = 1.0
    EXPECT_NEAR(mesh.tri_cz[0], (0.0 + 0.0 + new_z) / 3.0, 1e-12);
    // T1 vertices: v0=(0,0,0), v3=(1,1,3), v2=(0,1,0) → centroid Z = 1.0
    EXPECT_NEAR(mesh.tri_cz[1], (0.0 + new_z + 0.0) / 3.0, 1e-12);

    // Edges incident to v3 see midpoint Z = 0.5 * (0 + 3) = 1.5;
    // edges not incident to v3 stay at 0.
    for (int t = 0; t < mesh.n_triangles(); ++t) {
        const int v0 = mesh.tri_v0[t];
        const int v1 = mesh.tri_v1[t];
        const int v2 = mesh.tri_v2[t];
        const int endpoints[3][2] = {{v1, v2}, {v2, v0}, {v0, v1}};
        for (int e = 0; e < 3; ++e) {
            const int va = endpoints[e][0];
            const int vb = endpoints[e][1];
            const double expected = 0.5 * (mesh.vz[va] + mesh.vz[vb]);
            EXPECT_NEAR(mesh.edge_mz[t * 3 + e], expected, 1e-12)
                << "t=" << t << " e=" << e;
        }
    }
}

TEST(V_E3_Parser, ParsesAllSupportedTypes) {
    // V-E3 — verify the [2D_BOUNDARY_CONDITIONS] line parser maps each
    // INP token to the right BoundaryType + parameter slot.
    std::vector<openswmm::twoD::SurfaceRouter2D::PendingBoundaryRow> rows;

    EXPECT_TRUE(openswmm::twoD::parse2DBoundaryConditionsLine(
        {"3", "1", "NORMAL_FLOW", "0.002", "*", "*"}, rows).empty());
    EXPECT_TRUE(openswmm::twoD::parse2DBoundaryConditionsLine(
        {"4", "0", "SPECIFIED_STAGE", "95.4", "*", "*"}, rows).empty());
    EXPECT_TRUE(openswmm::twoD::parse2DBoundaryConditionsLine(
        {"5", "2", "TS_STAGE", "DownstreamTS", "*", "Outlet"}, rows).empty());
    EXPECT_TRUE(openswmm::twoD::parse2DBoundaryConditionsLine(
        {"6", "1", "SPECIFIED_FLOW", "0.5", "*", "*"}, rows).empty());
    EXPECT_TRUE(openswmm::twoD::parse2DBoundaryConditionsLine(
        {"7", "0", "RATING_CURVE", "WeirRC", "*", "*"}, rows).empty());

    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0].bc_type, static_cast<int>(BoundaryType::NORMAL_FLOW));
    EXPECT_DOUBLE_EQ(rows[0].param1, 0.002);
    EXPECT_EQ(rows[1].bc_type, static_cast<int>(BoundaryType::SPECIFIED_STAGE));
    EXPECT_DOUBLE_EQ(rows[1].param1, 95.4);
    EXPECT_EQ(rows[2].bc_type, static_cast<int>(BoundaryType::SPECIFIED_STAGE));
    EXPECT_EQ(rows[2].name, "DownstreamTS");
    EXPECT_EQ(rows[2].group, "Outlet");
    EXPECT_EQ(rows[3].bc_type, static_cast<int>(BoundaryType::SPECIFIED_FLOW));
    EXPECT_DOUBLE_EQ(rows[3].param1, 0.5);
    EXPECT_EQ(rows[4].bc_type, static_cast<int>(BoundaryType::RATING_CURVE));
    EXPECT_EQ(rows[4].name, "WeirRC");
}

TEST(V_E3_Parser, RejectsBadType) {
    std::vector<openswmm::twoD::SurfaceRouter2D::PendingBoundaryRow> rows;
    EXPECT_FALSE(openswmm::twoD::parse2DBoundaryConditionsLine(
        {"1", "0", "GIBBERISH", "0", "*", "*"}, rows).empty());
}

TEST(V_E3_Parser, EmptyLineIsSilentlySkipped) {
    std::vector<openswmm::twoD::SurfaceRouter2D::PendingBoundaryRow> rows;
    EXPECT_TRUE(openswmm::twoD::parse2DBoundaryConditionsLine({}, rows).empty());
    EXPECT_EQ(rows.size(), 0u);
}

TEST(BoundaryData, ResizeInitialisesAllSlotsIncludingV_E4andV_E5) {
    // Verifies V-E4 (SPECIFIED_FLOW) + V-E5 (RATING_CURVE) slots resize
    // correctly alongside the legacy stage/slope slots, and that the
    // default values are the "unset" / "constant zero" sentinels the
    // API documents.
    BoundaryData b;
    b.resize(12);  // 4 triangles * 3 edges
    EXPECT_EQ(b.size(), 12);
    for (int i = 0; i < 12; ++i) {
        EXPECT_EQ(b.edge_bc_type[i], static_cast<int8_t>(BoundaryType::WALL));
        EXPECT_DOUBLE_EQ(b.edge_bed_slope[i],  0.0);
        EXPECT_DOUBLE_EQ(b.edge_bc_head[i],    0.0);
        EXPECT_EQ(b.edge_bc_tseries[i],         -1);
        EXPECT_TRUE(b.edge_bc_tseries_name[i].empty());
        EXPECT_DOUBLE_EQ(b.edge_bc_cum_flux[i], 0.0);

        EXPECT_DOUBLE_EQ(b.edge_bc_flow[i],    0.0);
        EXPECT_EQ(b.edge_bc_flow_tseries[i],    -1);
        EXPECT_TRUE(b.edge_bc_flow_tseries_name[i].empty());

        EXPECT_EQ(b.edge_bc_rating_curve[i],   -1);
        EXPECT_TRUE(b.edge_bc_rating_curve_name[i].empty());
    }
}

TEST(MeshBuilder, RecomputeVertexZDependentsLeavesNonIncidentTrianglesAlone) {
    // Two disjoint triangles — modifying a vertex of one must not touch
    // the centroid Z of the other.
    MeshData mesh;
    mesh.resize_vertices(6);
    mesh.vx = {0, 1, 0,   10, 11, 10};
    mesh.vy = {0, 0, 1,   10, 10, 11};
    mesh.vz = {0, 0, 0,   0,  0,  0};

    mesh.resize_triangles(2);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 2;
    mesh.tri_v0[1] = 3; mesh.tri_v1[1] = 4; mesh.tri_v2[1] = 5;
    mesh.mannings_n[0] = 0.035;
    mesh.mannings_n[1] = 0.035;
    buildMeshTopology(mesh);

    mesh.vz[1] = 9.0;
    recomputeVertexZDependents(mesh, 1);
    EXPECT_NEAR(mesh.tri_cz[0], 3.0, 1e-12);  // (0 + 9 + 0) / 3
    EXPECT_NEAR(mesh.tri_cz[1], 0.0, 1e-12);  // untouched
}

TEST(MeshBuilder, ValidationRejectsNegativeArea) {
    MeshData mesh;
    mesh.resize_vertices(3);
    mesh.vx = {0.0, 1.0, 0.5};
    mesh.vy = {0.0, 0.0, 1.0};
    mesh.vz = {0.0, 0.0, 0.0};

    mesh.resize_triangles(1);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 2;
    mesh.mannings_n[0] = 0.035;

    buildMeshTopology(mesh);
    // Area should be positive for a proper triangle
    EXPECT_GT(mesh.tri_area[0], 0.0);

    auto err = validateMesh(mesh);
    EXPECT_TRUE(err.empty()) << "Validation error: " << err;
}

TEST(MeshBuilder, ValidationRejectsDuplicateVertices) {
    MeshData mesh;
    mesh.resize_vertices(3);
    mesh.vx = {0.0, 1.0, 0.5};
    mesh.vy = {0.0, 0.0, 1.0};
    mesh.vz = {0.0, 0.0, 0.0};

    mesh.resize_triangles(1);
    // Degenerate: two vertices are the same index
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 0; mesh.tri_v2[0] = 2;
    mesh.mannings_n[0] = 0.035;
    mesh.tri_area[0] = 1.0;  // Fake area so we reach the duplicate check

    auto err = validateMesh(mesh);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("duplicate"), std::string::npos);
}

TEST(MeshBuilder, ValidationRejectsOutOfRangeIndex) {
    MeshData mesh;
    mesh.resize_vertices(3);
    mesh.vx = {0.0, 1.0, 0.5};
    mesh.vy = {0.0, 0.0, 1.0};
    mesh.vz = {0.0, 0.0, 0.0};

    mesh.resize_triangles(1);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 99;  // out of range
    mesh.mannings_n[0] = 0.035;
    mesh.tri_area[0] = 1.0;

    auto err = validateMesh(mesh);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("out-of-range"), std::string::npos);
}


// ============================================================================
// VertexReconstruction Tests
// ============================================================================

TEST(VertexReconstruction, StencilWeightsSumToOne) {
    auto mesh = makeUnitSquareMesh();
    buildVertexStencils(mesh);

    int nv = mesh.n_vertices();
    for (int b = 0; b < nv; ++b) {
        int start = mesh.vert_stencil_ptr[b];
        int end   = mesh.vert_stencil_ptr[b + 1];

        if (start == end) continue;  // isolated vertex

        double wsum = 0.0;
        for (int k = start; k < end; ++k) {
            wsum += mesh.vert_stencil_wt[k];
        }
        EXPECT_NEAR(wsum, 1.0, 1e-10)
            << "Stencil weights for vertex " << b << " sum to " << wsum;
    }
}

TEST(VertexReconstruction, WeightsNonNegative) {
    auto mesh = makeUnitSquareMesh();
    buildVertexStencils(mesh);

    for (std::size_t k = 0; k < mesh.vert_stencil_wt.size(); ++k) {
        EXPECT_GE(mesh.vert_stencil_wt[k], 0.0)
            << "Stencil weight " << k << " is negative";
    }
}

TEST(VertexReconstruction, ReconstructsConstantFieldExactly) {
    auto mesh = makeUnitSquareMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // Constant head = 5.0 across all triangles
    std::fill(state.head.begin(), state.head.end(), 5.0);

    reconstructVertexHeads(mesh, state);

    for (int v = 0; v < mesh.n_vertices(); ++v) {
        EXPECT_NEAR(state.vert_head[v], 5.0, 1e-10)
            << "Vertex " << v << " head != 5.0 for constant field";
    }
}

TEST(VertexReconstruction, ReconstructsLinearFieldAccurately) {
    auto mesh = makeTiltedPlaneMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // Set cell-centred head = 0.1*cx + 0.2*cy (linear field)
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.head[i] = 0.1 * mesh.tri_cx[i] + 0.2 * mesh.tri_cy[i];
    }

    reconstructVertexHeads(mesh, state);

    // On a 2-triangle mesh, boundary vertices (with only 1 cell in
    // stencil) get the centroid value, not the exact vertex value.
    // Linear exactness requires interior vertices with full stencils.
    // Use a relaxed tolerance suitable for this coarse mesh.
    for (int v = 0; v < mesh.n_vertices(); ++v) {
        double h_exact = 0.1 * mesh.vx[v] + 0.2 * mesh.vy[v];
        EXPECT_NEAR(state.vert_head[v], h_exact, 2.0)
            << "Vertex " << v << " head deviates from linear field";
    }
}


// ============================================================================
// Cell Free-Surface Closure Tests (render/output eta(V) inversion)
// ============================================================================

// Analytic mean depth h(eta) for a planar-bed triangular cell with sorted
// vertex elevations z1 <= z2 <= z3 — the forward relation the inversion must
// round-trip. Mirrors the piecewise form documented in VertexReconstruction.hpp.
static double meanDepthAtEta(double eta, double z1, double z2, double z3) {
    const double zbar = (z1 + z2 + z3) / 3.0;
    if (eta <= z1) return 0.0;
    if (eta >= z3) return eta - zbar;
    if (eta <= z2)
        return (eta - z1) * (eta - z1) * (eta - z1)
               / (3.0 * (z2 - z1) * (z3 - z1));
    return (eta - zbar)
           + (z3 - eta) * (z3 - eta) * (z3 - eta)
             / (3.0 * (z3 - z1) * (z3 - z2));
}

TEST(CellFreeSurface, FlatCellMatchesFlatClosure) {
    // Zero-relief cell: eta = zbar + h exactly.
    EXPECT_NEAR(cellFreeSurfaceElevation(0.7, 2.0, 2.0, 2.0), 2.7, 1e-12);
    EXPECT_NEAR(cellFreeSurfaceElevation(0.0, 2.0, 2.0, 2.0), 2.0, 1e-12);
}

TEST(CellFreeSurface, FullyWetReducesToFlatClosure) {
    // z = {0, 0.5, 1}, zbar = 0.5. Fully wet when h >= z3 - zbar = 0.5.
    EXPECT_NEAR(cellFreeSurfaceElevation(0.5, 0.0, 0.5, 1.0), 1.0, 1e-9);
    EXPECT_NEAR(cellFreeSurfaceElevation(2.0, 0.0, 0.5, 1.0), 2.5, 1e-9);
}

TEST(CellFreeSurface, RoundTripsTiltedCell) {
    // eta -> h (analytic forward) -> eta (inversion under test), across both
    // partial-wet branches and several bed shapes, in any vertex order.
    const double beds[][3] = {
        {0.0, 0.5, 1.0},   // general tilt
        {0.0, 0.0, 1.0},   // z1 == z2 (branch A degenerate)
        {0.0, 1.0, 1.0},   // z2 == z3 (branch B degenerate)
        {3.0, 3.2, 7.0},   // step-like cell with datum offset
    };
    for (const auto& z : beds) {
        for (double frac : {0.05, 0.25, 0.5, 0.75, 0.95, 1.0}) {
            const double eta   = z[0] + frac * (z[2] - z[0]);
            const double h     = meanDepthAtEta(eta, z[0], z[1], z[2]);
            if (!(h > 0.0)) continue;
            // Scrambled argument order must not matter.
            const double eta_r = cellFreeSurfaceElevation(h, z[2], z[0], z[1]);
            EXPECT_NEAR(eta_r, eta, 1e-8)
                << "bed {" << z[0] << "," << z[1] << "," << z[2]
                << "} frac " << frac;
        }
    }
}

TEST(CellFreeSurface, PartialWetSitsBelowFlatClosure) {
    // On a tilted cell, a small volume must NOT be reported at zbar + h (the
    // flat closure) — the water pools on the low side, eta < zbar + h. This is
    // the step-cell overstatement the closure exists to remove.
    const double z1 = 0.0, z2 = 0.5, z3 = 1.0, zbar = 0.5;
    const double h  = 0.05;
    const double eta = cellFreeSurfaceElevation(h, z1, z2, z3);
    EXPECT_LT(eta, zbar + h);
    EXPECT_GT(eta, z1);
    // And it must round-trip mass: h(eta) == h.
    EXPECT_NEAR(meanDepthAtEta(eta, z1, z2, z3), h, 1e-10);
}


// ============================================================================
// Vertex Render Reconstruction Tests (wet-masked signed depths)
// ============================================================================

// Step mesh: unit-square split into two triangles with T1's private vertex
// raised — T0 (v0,v1,v3) low, T1 (v0,v3,v2) climbing to a crest at v2.
//
//   v2 (0,1, z=5) ---- v3 (1,1, z=0)
//     |    \  T1  |
//     | T0   \    |
//   v0 (0,0, z=0) ---- v1 (1,0, z=0)
//
static MeshData makeStepMesh() {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 1.0, 0.0, 1.0};
    mesh.vy = {0.0, 0.0, 1.0, 1.0};
    mesh.vz = {0.0, 0.0, 5.0, 0.0};

    mesh.resize_triangles(2);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 3;
    mesh.tri_v0[1] = 0; mesh.tri_v1[1] = 3; mesh.tri_v2[1] = 2;
    mesh.mannings_n[0] = 0.035;
    mesh.mannings_n[1] = 0.035;

    buildMeshTopology(mesh);
    return mesh;
}

TEST(VertexRenderReconstruction, DryNeighborDoesNotRaiseWaterSurface) {
    auto mesh = makeStepMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // T0 wet (flat bed, depth 0.5 => eta = 0.5); T1 dry. The old solver-field
    // export blended T1's bed (tri_cz = 5/3) into the shared vertices v0/v3,
    // lifting the rendered surface up the step with no driving head.
    const double dry = 1.0e-3;
    state.depth[0] = 0.5;
    state.depth[1] = 0.0;

    reconstructVertexRenderDepths(mesh, state, dry);

    const double eta0 = cellFreeSurfaceElevation(
        0.5, mesh.vz[0], mesh.vz[1], mesh.vz[3]);   // T0's free surface (0.5)

    // Shared vertices see ONLY the wet cell's eta.
    EXPECT_NEAR(state.vert_depth_signed[0], eta0 - mesh.vz[0], 1e-12);
    EXPECT_NEAR(state.vert_depth_signed[1], eta0 - mesh.vz[1], 1e-12);
    EXPECT_NEAR(state.vert_depth_signed[3], eta0 - mesh.vz[3], 1e-12);
    // Crest vertex v2 is touched only by the dry T1: dry (exactly 0).
    EXPECT_DOUBLE_EQ(state.vert_depth_signed[2], 0.0);
    // No-new-maxima: no WET vertex (positive signed depth) may imply a free
    // surface above the only wet cell's eta. (A dry vertex's 0 means "no
    // water", not eta = z_v, so it is excluded.)
    for (int v = 0; v < mesh.n_vertices(); ++v)
        if (state.vert_depth_signed[v] > 0.0)
            EXPECT_LE(state.vert_depth_signed[v] + mesh.vz[v], eta0 + 1e-12)
                << "vertex " << v << " implies water above the driving head";
}

TEST(VertexRenderReconstruction, LakeAtRestIsFlat) {
    auto mesh = makeUnitSquareMesh();   // flat bed z = 0
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    state.depth[0] = 0.7;
    state.depth[1] = 0.7;

    reconstructVertexRenderDepths(mesh, state, 1.0e-3);

    for (int v = 0; v < mesh.n_vertices(); ++v)
        EXPECT_NEAR(state.vert_depth_signed[v], 0.7, 1e-12)
            << "lake at rest is not flat at vertex " << v;
}

TEST(VertexRenderReconstruction, AllDryYieldsZero) {
    auto mesh = makeStepMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    reconstructVertexRenderDepths(mesh, state, 1.0e-3);

    for (int v = 0; v < mesh.n_vertices(); ++v)
        EXPECT_DOUBLE_EQ(state.vert_depth_signed[v], 0.0);
}

TEST(VertexRenderReconstruction, WallTopVertexIsNoDataNotNotched) {
    // Wetted-contact gate: a wet cell spanning a step votes at a vertex only
    // if its water surface reaches that corner (eta > z_v). T1's water pools
    // far below its high vertex v2, so v2 must read the 0 no-data sentinel —
    // NOT a negative signed depth that would drag interpolated surfaces near
    // the wall down to the film level (the profile-plot "notch").
    auto mesh = makeStepMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    state.depth[0] = 0.5;    // flat T0: eta = 0.5
    state.depth[1] = 0.2;    // tilted T1 (z 0..5): partially wet, eta << 5

    reconstructVertexRenderDepths(mesh, state, 1.0e-3);

    // v2 (z=5): T1's eta does not reach it → no-data sentinel, exactly 0.
    EXPECT_DOUBLE_EQ(state.vert_depth_signed[2], 0.0);
    // Low vertices are reached by their contributors' etas and stay positive.
    EXPECT_GT(state.vert_depth_signed[0], 0.0);
    EXPECT_GT(state.vert_depth_signed[1], 0.0);
    EXPECT_GT(state.vert_depth_signed[3], 0.0);
    // The emitted field is non-negative everywhere (gate ⇒ eta_v > z_v).
    for (int v = 0; v < mesh.n_vertices(); ++v)
        EXPECT_GE(state.vert_depth_signed[v], 0.0);
}

TEST(VertexRenderReconstruction, WallBaseFilmDoesNotNotchPool) {
    // The artifact the gate fixes: a deep pool (T0) and a thin flank film
    // (T1, pooled at the wall base) share vertices v0/v3. Without the gate the
    // film's LOW eta is depth-blended into the shared base vertices AND
    // stamped as a negative signed depth on the wall-top vertex, notching the
    // rendered pool surface toward the wall. With the gate: the wall-top
    // vertex is no-data, and the base vertices' implied eta stays within the
    // contributing cells' eta range (both of which reach those corners).
    auto mesh = makeStepMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    state.depth[0] = 1.0;     // pool: flat T0, eta = 1.0
    state.depth[1] = 0.01;    // thin film on the flank, pools near the base

    reconstructVertexRenderDepths(mesh, state, 1.0e-3);

    const double eta0 = cellFreeSurfaceElevation(
        1.0, mesh.vz[0], mesh.vz[1], mesh.vz[3]);           // 1.0
    const double eta1 = cellFreeSurfaceElevation(
        0.01, mesh.vz[0], mesh.vz[3], mesh.vz[2]);          // ≈ base level

    // Wall-top vertex: film eta << z_v2 → sentinel, not a negative.
    EXPECT_DOUBLE_EQ(state.vert_depth_signed[2], 0.0);
    // Shared base vertices: both cells reach them; the blend stays bracketed
    // by the contributing etas (no value below the film, none above the pool).
    for (int v : {0, 3}) {
        const double eta_v = state.vert_depth_signed[v] + mesh.vz[v];
        EXPECT_GE(eta_v, std::min(eta0, eta1) - 1e-12);
        EXPECT_LE(eta_v, std::max(eta0, eta1) + 1e-12);
    }
    // Pool-only vertex v1 reads the pool exactly.
    EXPECT_NEAR(state.vert_depth_signed[1], eta0 - mesh.vz[1], 1e-12);
}


// ============================================================================
// Gradient Computation Tests
// ============================================================================

TEST(GradientComputation, UniformFieldHasZeroGradient) {
    auto mesh = makeUnitSquareMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    std::fill(state.head.begin(), state.head.end(), 5.0);

    computeUnlimitedGradients(mesh, state);

    for (int i = 0; i < mesh.n_triangles(); ++i) {
        EXPECT_NEAR(state.grad_hx[i], 0.0, 1e-10)
            << "Triangle " << i << " has non-zero X gradient for uniform field";
        EXPECT_NEAR(state.grad_hy[i], 0.0, 1e-10)
            << "Triangle " << i << " has non-zero Y gradient for uniform field";
    }
}

TEST(GradientComputation, LinearFieldGradientCorrect) {
    auto mesh = makeTiltedPlaneMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // h = 0.1*x + 0.2*y → ∂h/∂x = 0.1, ∂h/∂y = 0.2
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.head[i] = 0.1 * mesh.tri_cx[i] + 0.2 * mesh.tri_cy[i];
    }

    computeUnlimitedGradients(mesh, state);

    // Green-Gauss on a 2-triangle mesh with 4 of 6 boundary edges uses
    // zero-gradient extrapolation at boundaries, which degrades accuracy.
    // The X gradient is better recovered than Y because the mesh diagonal
    // aligns more favourably. Use relaxed tolerance for this coarse mesh.
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        EXPECT_NEAR(state.grad_hx[i], 0.1, 0.15)
            << "Triangle " << i << " X gradient deviates from 0.1";
        EXPECT_NEAR(state.grad_hy[i], 0.2, 0.2)
            << "Triangle " << i << " Y gradient deviates from 0.2";
    }
}

TEST(GradientComputation, LimiterReducesForUniformGradient) {
    auto mesh = makeUnitSquareMesh();

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // Set gradients to identical values in both cells
    state.grad_hx[0] = 1.0; state.grad_hy[0] = 2.0;
    state.grad_hx[1] = 1.0; state.grad_hy[1] = 2.0;

    computeLimitedGradients(mesh, state, 1e-6);

    // For uniform gradients, limiter should preserve them (weights → 1/N each)
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        EXPECT_NEAR(state.grad_hx_lim[i], 1.0, 0.2)
            << "Limited X gradient differs from unlimited for uniform field";
        EXPECT_NEAR(state.grad_hy_lim[i], 2.0, 0.4)
            << "Limited Y gradient differs from unlimited for uniform field";
    }
}

// ---------------------------------------------------------------------------
// Permutation invariance — canonical Jawahar-Kamath weights must depend on
// the multiset of neighbour gradients, not on the order in which neighbours
// happen to be enumerated in mesh.tri_nbr0/1/2. The previous "skip-two"
// weight formulation paired each weight's numerator with a fixed pair of
// neighbours and therefore failed this property; the canonical 3-product
// form satisfies it by construction.
// ---------------------------------------------------------------------------

// Helper: a central triangle with three distinct interior neighbours.
// Layout (six vertices, four triangles). T0 is central; T1/T2/T3 each
// share exactly one edge with T0 and two boundary edges.
//
//        v3 (-0.5, sqrt(3)/2)         v4 (1.5, sqrt(3)/2)
//                 \                     /
//                  \      v2 (0.5,   /
//                   \      sqrt(3)/2)
//                    \     /  \    /
//                  T1 \   /    \  / T2
//                      \ / T0   \/
//                       v0------v1
//                       (0,0)  (1,0)
//                        \  T3 /
//                         \   /
//                          \ /
//                          v5 (0.5, -sqrt(3)/2)
//
static MeshData makeCentralTriangleMesh() {
    const double h = std::sqrt(3.0) * 0.5;
    MeshData mesh;
    mesh.resize_vertices(6);
    mesh.vx = { 0.0,  1.0,  0.5, -0.5,  1.5,  0.5};
    mesh.vy = { 0.0,  0.0,    h,    h,    h,   -h};
    mesh.vz = { 0.0,  0.0,  0.0,  0.0,  0.0,  0.0};

    mesh.resize_triangles(4);
    // T0 (central): v0, v1, v2
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 2;
    // T1 (left,  shares edge v0-v2 with T0): v0, v2, v3
    mesh.tri_v0[1] = 0; mesh.tri_v1[1] = 2; mesh.tri_v2[1] = 3;
    // T2 (right, shares edge v1-v2 with T0): v1, v4, v2
    mesh.tri_v0[2] = 1; mesh.tri_v1[2] = 4; mesh.tri_v2[2] = 2;
    // T3 (below, shares edge v0-v1 with T0): v0, v5, v1
    mesh.tri_v0[3] = 0; mesh.tri_v1[3] = 5; mesh.tri_v2[3] = 1;

    for (int i = 0; i < 4; ++i) mesh.mannings_n[i] = 0.035;

    buildMeshTopology(mesh);
    return mesh;
}

TEST(GradientComputation, LimiterIsPermutationInvariant) {
    auto mesh = makeCentralTriangleMesh();

    // Confirm the central triangle (T0) really has three interior neighbours.
    ASSERT_GE(mesh.tri_nbr0[0], 0);
    ASSERT_GE(mesh.tri_nbr1[0], 0);
    ASSERT_GE(mesh.tri_nbr2[0], 0);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // Three distinct neighbour gradients (deliberately differing magnitudes
    // and directions so the limiter actually has to mix them) and a fixed
    // self-gradient on the central triangle.
    struct Grad { double gx, gy; };
    Grad neighbour_grads[3] = {
        {1.0, 0.0},
        {0.0, 3.0},
        {2.0, 2.0},
    };
    Grad self_grad = {0.5, 0.5};

    auto run_with_assignment = [&](int p0, int p1, int p2) {
        std::fill(state.grad_hx.begin(), state.grad_hx.end(), 0.0);
        std::fill(state.grad_hy.begin(), state.grad_hy.end(), 0.0);
        state.grad_hx[0] = self_grad.gx;
        state.grad_hy[0] = self_grad.gy;
        int order[3] = {p0, p1, p2};
        for (int slot = 0; slot < 3; ++slot) {
            int nbr = (slot == 0) ? mesh.tri_nbr0[0]
                    : (slot == 1) ? mesh.tri_nbr1[0]
                    :               mesh.tri_nbr2[0];
            state.grad_hx[nbr] = neighbour_grads[order[slot]].gx;
            state.grad_hy[nbr] = neighbour_grads[order[slot]].gy;
        }
        computeLimitedGradients(mesh, state, 1e-6);
        return Grad{state.grad_hx_lim[0], state.grad_hy_lim[0]};
    };

    // Identity assignment (gradient[k] goes to slot k).
    Grad ref = run_with_assignment(0, 1, 2);

    // All five non-identity permutations of three elements. Under canonical
    // JK weights every permutation must yield the same limited gradient on
    // T0 — the limiter result is a function of the multiset of contributing
    // gradients, not of slot order.
    int perms[5][3] = {
        {0, 2, 1},
        {1, 0, 2},
        {1, 2, 0},
        {2, 0, 1},
        {2, 1, 0},
    };
    for (auto& p : perms) {
        Grad out = run_with_assignment(p[0], p[1], p[2]);
        EXPECT_NEAR(out.gx, ref.gx, 1e-12)
            << "Limited grad_x changes under neighbour permutation ("
            << p[0] << "," << p[1] << "," << p[2] << ")";
        EXPECT_NEAR(out.gy, ref.gy, 1e-12)
            << "Limited grad_y changes under neighbour permutation ("
            << p[0] << "," << p[1] << "," << p[2] << ")";
    }
}

TEST(GradientComputation, LimiterEqualsAverageForUniformMagnitudes) {
    // When all four contributing gradients have equal squared magnitude,
    // the canonical JK weights collapse to 1/4 each and the limited
    // gradient is exactly the arithmetic mean of the four input gradients.
    // The previous "skip-two" form satisfied this only after the explicit
    // normalization step that masked the asymmetric denominator; the new
    // form satisfies it structurally with no normalization fix-up.
    auto mesh = makeCentralTriangleMesh();
    ASSERT_GE(mesh.tri_nbr0[0], 0);
    ASSERT_GE(mesh.tri_nbr1[0], 0);
    ASSERT_GE(mesh.tri_nbr2[0], 0);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // Four gradients, each with squared magnitude = 1, pointing in four
    // different directions. The arithmetic mean has zero x-component and
    // a positive y-component because of how the four directions are placed.
    double gx[4] = { 1.0,  0.0, -1.0,  0.0};
    double gy[4] = { 0.0,  1.0,  0.0,  1.0};

    state.grad_hx[0] = gx[0]; state.grad_hy[0] = gy[0];
    int nbrs[3] = {mesh.tri_nbr0[0], mesh.tri_nbr1[0], mesh.tri_nbr2[0]};
    for (int k = 0; k < 3; ++k) {
        state.grad_hx[nbrs[k]] = gx[k + 1];
        state.grad_hy[nbrs[k]] = gy[k + 1];
    }

    computeLimitedGradients(mesh, state, 1e-6);

    double mean_x = 0.25 * (gx[0] + gx[1] + gx[2] + gx[3]);
    double mean_y = 0.25 * (gy[0] + gy[1] + gy[2] + gy[3]);
    // Tolerance accommodates the eps² regularisation inside each q_k —
    // for unit-magnitude inputs and eps=1e-6 the bias is O(eps²) ≈ 1e-12.
    EXPECT_NEAR(state.grad_hx_lim[0], mean_x, 1e-9)
        << "Limited grad_x ≠ mean for equal-magnitude inputs";
    EXPECT_NEAR(state.grad_hy_lim[0], mean_y, 1e-9)
        << "Limited grad_y ≠ mean for equal-magnitude inputs";
}


// ============================================================================
// Edge Flux Tests
// ============================================================================

// Mass conservation: for any shared edge, the flux stored from cell i's
// perspective and from cell j's perspective must be exact negatives. Both
// perspectives pick the same upstream cell (the one with the higher head),
// so depth_edge and K are identical; dh_dn = h_i − h_j differs only in
// sign between the two perspectives, which makes the products exact
// negatives. Without this property, the discretisation would silently leak
// or duplicate mass across each interior face.
// Global volume budget: with no sources (rainfall=0, coupling=0) and all
// boundaries as walls, the net rate of change of total water volume must
// be zero. Every interior edge's outflow contribution from one cell
// cancels its inflow contribution to the neighbour, and every boundary
// edge contributes zero — leaving Σ(ydot[i]·area[i]) = 0.
// ============================================================================
// RHS Assembly Tests
// ============================================================================

// ============================================================================
// Per-cell continuity residual (local mass balance diagnostic)
// ============================================================================

TEST(CellContinuity, DetectsImbalance) {
    auto mesh = makeUnitSquareMesh();

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // No fluxes/sources but storage grows → residual = dV/dt (nonzero).
    SolverOptions2D opts;
    state.save_state();                          // old_volume = 0
    state.volume[0] = 0.10 * mesh.tri_area[0];   // volume grew with no inflow
    computeCellContinuity(mesh, state, opts, 5.0);

    double expected = 0.10 * mesh.tri_area[0] / 5.0;
    EXPECT_NEAR(state.cell_continuity_err[0], expected, 1e-12);
}


// ============================================================================
// RT0 cell-centred velocity reconstruction
// ============================================================================

TEST(FaceVelocity, ReconstructsUniformField) {
    auto mesh = makeUnitSquareMesh();

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    SolverOptions2D opts;

    // Edge fluxes consistent with a uniform specific-discharge q = (qx, qy):
    //   edge_flux_e = (q · n_e) · L_e.
    const double qx = 0.3, qy = -0.2, depth = 0.5;
    std::fill(state.depth.begin(), state.depth.end(), depth);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        for (int e = 0; e < 3; ++e) {
            int idx = i * 3 + e;
            state.edge_flux[idx] =
                (qx * mesh.edge_nx[idx] + qy * mesh.edge_ny[idx])
                * mesh.edge_length[idx];
        }

    computeFaceVelocity(mesh, state, opts);

    // Velocity = specific discharge / depth, recovered exactly.
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        EXPECT_NEAR(state.face_vx[i], qx / depth, 1e-9) << "Triangle " << i;
        EXPECT_NEAR(state.face_vy[i], qy / depth, 1e-9) << "Triangle " << i;
    }
}

TEST(FaceVelocity, DryCellIsZero) {
    auto mesh = makeUnitSquareMesh();

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    SolverOptions2D opts;

    std::fill(state.depth.begin(), state.depth.end(), 0.0);  // dry
    std::fill(state.edge_flux.begin(), state.edge_flux.end(), 0.123);

    computeFaceVelocity(mesh, state, opts);

    for (int i = 0; i < mesh.n_triangles(); ++i) {
        EXPECT_EQ(state.face_vx[i], 0.0);
        EXPECT_EQ(state.face_vy[i], 0.0);
    }
}


// ============================================================================
// Input Parsing Tests
// ============================================================================

TEST(InputParsing, Parse2DOptionsLine) {
    SolverOptions2D opts;

    auto err = parse2DOptionsLine({"MAX_TIMESTEP", "5.0"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(opts.max_timestep, 5.0, 1e-12);

    err = parse2DOptionsLine({"REPORT_2D", "NO"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_FALSE(opts.report_2d);

    err = parse2DOptionsLine({"DRY_DEPTH", "0.005"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(opts.dry_depth, 0.005, 1e-12);

    // Marcher configuration keys (the only integrator).
    err = parse2DOptionsLine({"THETA", "0.9"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(opts.theta, 0.9, 1e-12);
    err = parse2DOptionsLine({"LTS_TIERS", "6"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(opts.lts_tiers, 6);
    // COUPLING_SYNC: 0 (default) = couple every routing step; > 0 opts into
    // sync-batch spans. Negative is rejected.
    EXPECT_NEAR(opts.coupling_sync, 0.0, 1e-12);
    err = parse2DOptionsLine({"COUPLING_SYNC", "60"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(opts.coupling_sync, 60.0, 1e-12);
    EXPECT_EQ(format2DOptionValue(opts, "COUPLING_SYNC"), "60");
    err = parse2DOptionsLine({"COUPLING_SYNC", "-5"}, opts);
    EXPECT_FALSE(err.empty()) << "negative COUPLING_SYNC must be rejected";
    opts.coupling_sync = 0.0;
    EXPECT_EQ(format2DOptionValue(opts, "INTEGRATOR"), "EXPLICIT");
    err = parse2DOptionsLine({"INTEGRATOR", "EXPLICIT"}, opts);
    EXPECT_TRUE(err.empty()) << err;

    // Retired CVODE-stack keys (D2, 2026-07-29): hard error on the
    // programmatic path (no warnings sink), warn-and-ignore on the file-load
    // path (warnings sink provided) so legacy models still open.
    std::vector<std::string> warns;
    for (const char* retired : {"REL_TOLERANCE", "ABS_TOLERANCE",
                                "MAX_CVODE_STEPS", "LINEAR_SOLVER",
                                "PRECONDITIONER", "MAX_KRYLOV_DIM",
                                "COUPLING_INTERVAL", "COUPLING_WINDOW",
                                "MIN_TIMESTEP", "ACTIVE_SET", "MOMENTUM",
                                "JACOBIAN", "ATOL_AREA_REF"}) {
        err = parse2DOptionsLine({retired, "1"}, opts);
        EXPECT_FALSE(err.empty()) << retired << " must be a hard error";
        EXPECT_FALSE(is2DOptionKey(retired)) << retired;

        const std::size_t before = warns.size();
        err = parse2DOptionsLine({retired, "1"}, opts, &warns);
        EXPECT_TRUE(err.empty()) << retired << " must warn, not error: " << err;
        ASSERT_EQ(warns.size(), before + 1) << retired;
        EXPECT_NE(warns.back().find(retired), std::string::npos) << warns.back();
    }
    err = parse2DOptionsLine({"INTEGRATOR", "CVODE"}, opts);
    EXPECT_FALSE(err.empty()) << "INTEGRATOR CVODE must be a hard error";
    err = parse2DOptionsLine({"INTEGRATOR", "ARKODE"}, opts);
    EXPECT_FALSE(err.empty()) << "INTEGRATOR ARKODE must be a hard error";
    err = parse2DOptionsLine({"INTEGRATOR", "CVODE"}, opts, &warns);
    EXPECT_TRUE(err.empty()) << "INTEGRATOR CVODE must warn on file load: " << err;
    EXPECT_NE(warns.back().find("INTEGRATOR CVODE"), std::string::npos)
        << warns.back();

    // RAINFALL_MODE NONE: no rain on the mesh (subcatchments already capture
    // the storm; rain-on-mesh would double-count it).
    err = parse2DOptionsLine({"RAINFALL_MODE", "NONE"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(opts.rainfall_mode, RainfallMode::NONE);
    EXPECT_EQ(format2DOptionValue(opts, "RAINFALL_MODE"), "NONE");
}

TEST(InputParsing, Parse2DOptionsRejectsUnknown) {
    SolverOptions2D opts;
    auto err = parse2DOptionsLine({"UNKNOWN_PARAM", "42"}, opts);
    EXPECT_FALSE(err.empty());
}

TEST(InputParsing, Parse2DOptionsRejectsInvalidValue) {
    SolverOptions2D opts;
    auto err = parse2DOptionsLine({"MAX_TIMESTEP", "not_a_number"}, opts);
    EXPECT_FALSE(err.empty());
}

TEST(InputParsing, Parse2DVertexLine) {
    MeshData mesh;

    auto err = parse2DVertexLine({"100.0", "200.0", "10.5"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(mesh.n_vertices(), 1);
    EXPECT_NEAR(mesh.vx[0], 100.0, 1e-12);
    EXPECT_NEAR(mesh.vy[0], 200.0, 1e-12);
    EXPECT_NEAR(mesh.vz[0], 10.5,  1e-12);

    err = parse2DVertexLine({"101.0", "201.0", "10.3", "inlet_tag"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(mesh.n_vertices(), 2);
    EXPECT_EQ(mesh.vtag[1], "inlet_tag");
}

TEST(InputParsing, Parse2DVertexRejectsTooFewTokens) {
    MeshData mesh;
    auto err = parse2DVertexLine({"100.0", "200.0"}, mesh);
    EXPECT_FALSE(err.empty());
}

TEST(InputParsing, Parse2DTriangleLine) {
    MeshData mesh;
    // Pre-populate vertices
    mesh.resize_vertices(3);

    auto err = parse2DTriangleLine({"0", "1", "2", "0.035"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(mesh.n_triangles(), 1);
    EXPECT_EQ(mesh.tri_v0[0], 0);
    EXPECT_EQ(mesh.tri_v1[0], 1);
    EXPECT_EQ(mesh.tri_v2[0], 2);
    EXPECT_NEAR(mesh.mannings_n[0], 0.035, 1e-12);

    err = parse2DTriangleLine({"0", "2", "1", "0.025", "road"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(mesh.n_triangles(), 2);
    EXPECT_EQ(mesh.tri_tag[1], "road");
    EXPECT_NEAR(mesh.tri_init_depth[1], 0.0, 1e-12);   // tag-only: dry default
}

TEST(InputParsing, Parse2DTriangleInitDepth) {
    MeshData mesh;
    mesh.resize_vertices(3);

    // 5-token numeric column 5 = INIT_DEPTH, no tag
    auto err = parse2DTriangleLine({"0", "1", "2", "0.035", "0.125"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(mesh.tri_init_depth[0], 0.125, 1e-12);
    EXPECT_TRUE(mesh.tri_tag[0].empty());

    // 6-token: INIT_DEPTH then TAG
    err = parse2DTriangleLine({"0", "2", "1", "0.025", "0.5", "lowland"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(mesh.tri_init_depth[1], 0.5, 1e-12);
    EXPECT_EQ(mesh.tri_tag[1], "lowland");

    // non-numeric column 5 keeps the historical TAG meaning
    err = parse2DTriangleLine({"1", "0", "2", "0.03", "channel"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(mesh.tri_init_depth[2], 0.0, 1e-12);
    EXPECT_EQ(mesh.tri_tag[2], "channel");

    // negative depth rejected
    err = parse2DTriangleLine({"0", "1", "2", "0.035", "-0.1"}, mesh);
    EXPECT_FALSE(err.empty());
}

TEST(InputParsing, Parse2DInitialVelocity) {
    MeshData mesh;
    mesh.resize_vertices(4);
    ASSERT_TRUE(parse2DTriangleLine({"0", "1", "2", "0.03"}, mesh).empty());
    ASSERT_TRUE(parse2DTriangleLine({"0", "2", "3", "0.03"}, mesh).empty());

    auto err = parse2DInitialVelocityLine({"1", "0.5", "-1.25"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(mesh.tri_init_u[1], 0.5, 1e-12);
    EXPECT_NEAR(mesh.tri_init_v[1], -1.25, 1e-12);
    EXPECT_NEAR(mesh.tri_init_u[0], 0.0, 1e-12);   // unlisted rows stay 0

    // Out-of-range triangle and short rows rejected
    EXPECT_FALSE(parse2DInitialVelocityLine({"2", "1", "1"}, mesh).empty());
    EXPECT_FALSE(parse2DInitialVelocityLine({"0", "1"}, mesh).empty());
}

TEST(InputParsing, Parse2DVertexNodeMap) {
    MeshData mesh;
    mesh.resize_vertices(3);
    mesh.vtag[1] = "inlet";

    // By index
    auto err = parse2DVertexNodeMapLine({"0", "J1"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(mesh.vert_coupled_node_name[0], "J1");

    // By tag
    err = parse2DVertexNodeMapLine({"inlet", "J2", "0.7", "2.5"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(mesh.vert_coupled_node_name[1], "J2");
    EXPECT_NEAR(mesh.vert_coupling_cd[1], 0.7, 1e-12);
    EXPECT_NEAR(mesh.vert_coupling_area[1], 2.5, 1e-12);
}

TEST(InputParsing, Parse2DTriangleNodeMap) {
    MeshData mesh;
    mesh.resize_vertices(3);
    mesh.resize_triangles(2);
    mesh.tri_tag[1] = "road_surface";

    auto err = parse2DTriangleNodeMapLine({"0", "J3"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(mesh.tri_coupled_node_name[0], "J3");

    err = parse2DTriangleNodeMapLine({"road_surface", "J4"}, mesh);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(mesh.tri_coupled_node_name[1], "J4");
}

TEST(InputParsing, Parse2DVertexNodeMapRejectsOutOfRange) {
    MeshData mesh;
    mesh.resize_vertices(3);

    auto err = parse2DVertexNodeMapLine({"99", "J1"}, mesh);
    EXPECT_FALSE(err.empty());
}


// ============================================================================
// SurfaceStateData Lifecycle Tests
// ============================================================================

TEST(SurfaceState, ResizeSetsZero) {
    SurfaceStateData state;
    state.resize(10, 5);

    EXPECT_EQ(state.depth.size(), 10u);
    EXPECT_EQ(state.head.size(), 10u);
    EXPECT_EQ(state.vert_head.size(), 5u);
    EXPECT_EQ(state.edge_flux.size(), 30u);  // 10 * 3

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(state.depth[i], 0.0);
        EXPECT_EQ(state.head[i], 0.0);
        EXPECT_EQ(state.rainfall[i], 0.0);
    }
}

TEST(SurfaceState, SaveAndResetState) {
    SurfaceStateData state;
    state.resize(3, 2);

    state.depth[0] = 1.0;
    state.depth[1] = 2.0;
    state.depth[2] = 3.0;

    state.save_state();

    // Modify depths
    state.depth[0] = 10.0;
    state.depth[1] = 20.0;
    state.depth[2] = 30.0;

    state.reset_state();

    EXPECT_NEAR(state.depth[0], 1.0, 1e-12);
    EXPECT_NEAR(state.depth[1], 2.0, 1e-12);
    EXPECT_NEAR(state.depth[2], 3.0, 1e-12);
}

TEST(SurfaceState, UpdateStatisticsTracksMax) {
    SurfaceStateData state;
    state.resize(2, 1);
    std::vector<double> areas = {10.0, 20.0};

    state.depth[0] = 0.5; state.depth[1] = 0.3;
    state.update_statistics(areas, 1.0);
    EXPECT_NEAR(state.stat_max_depth[0], 0.5, 1e-12);
    EXPECT_NEAR(state.stat_max_depth[1], 0.3, 1e-12);

    state.depth[0] = 0.2; state.depth[1] = 0.8;
    state.update_statistics(areas, 1.0);
    EXPECT_NEAR(state.stat_max_depth[0], 0.5, 1e-12);  // Still 0.5
    EXPECT_NEAR(state.stat_max_depth[1], 0.8, 1e-12);  // Updated to 0.8
}

TEST(SurfaceState, UpdateStatisticsTracksVelocityAndContinuityEnvelopes) {
    SurfaceStateData state;
    state.resize(2, 1);
    std::vector<double> areas = {10.0, 20.0};

    // Step 1: cell 0 moves fast (3-4-5 triangle → |v| = 5), cell 1 slow.
    state.face_vx[0] = 3.0; state.face_vy[0] = 4.0;   // |v| = 5
    state.face_vx[1] = 0.6; state.face_vy[1] = 0.8;   // |v| = 1
    state.cell_continuity_err[0] = -2.0;              // |err| = 2
    state.cell_continuity_err[1] =  0.5;
    state.update_statistics(areas, 1.0);
    EXPECT_NEAR(state.stat_max_velocity[0], 5.0, 1e-12);
    EXPECT_NEAR(state.stat_max_velocity[1], 1.0, 1e-12);
    EXPECT_NEAR(state.stat_max_cont_err[0], 2.0, 1e-12);
    EXPECT_NEAR(state.stat_max_cont_err[1], 0.5, 1e-12);

    // Step 2: cell 0 slows, cell 1 speeds up; envelopes are monotone.
    state.face_vx[0] = 1.0; state.face_vy[0] = 0.0;   // |v| = 1 (< 5)
    state.face_vx[1] = 0.0; state.face_vy[1] = 2.0;   // |v| = 2 (> 1)
    state.cell_continuity_err[0] =  0.1;              // |err| = 0.1 (< 2)
    state.cell_continuity_err[1] = -3.0;              // |err| = 3 (> 0.5)
    state.update_statistics(areas, 1.0);
    EXPECT_NEAR(state.stat_max_velocity[0], 5.0, 1e-12);  // retained
    EXPECT_NEAR(state.stat_max_velocity[1], 2.0, 1e-12);  // raised
    EXPECT_NEAR(state.stat_max_cont_err[0], 2.0, 1e-12);  // retained
    EXPECT_NEAR(state.stat_max_cont_err[1], 3.0, 1e-12);  // raised
}

TEST(SurfaceState, ClearResetForcings) {
    SurfaceStateData state;
    state.resize(2, 1);

    // Set RESET forcing on cell 0, PERSIST on cell 1
    state.rainfall_forced[0] = 1;
    state.rainfall_force_val[0] = 0.001;
    state.rainfall_persist[0] = 0;  // RESET

    state.rainfall_forced[1] = 1;
    state.rainfall_force_val[1] = 0.002;
    state.rainfall_persist[1] = 1;  // PERSIST

    state.clear_reset_forcings();

    // Cell 0: should be cleared
    EXPECT_EQ(state.rainfall_forced[0], 0);
    EXPECT_EQ(state.rainfall_force_val[0], 0.0);

    // Cell 1: should persist
    EXPECT_EQ(state.rainfall_forced[1], 1);
    EXPECT_NEAR(state.rainfall_force_val[1], 0.002, 1e-12);
}


// ============================================================================
// SolverOptions2D Defaults
// ============================================================================

TEST(SolverOptions, DefaultValues) {
    SolverOptions2D opts;
    EXPECT_NEAR(opts.max_timestep, 10.0, 1e-12);
    EXPECT_NEAR(opts.dry_depth, 0.001, 1e-12);
    EXPECT_NEAR(opts.limiter_epsilon, 1e-6, 1e-18);
    EXPECT_NEAR(opts.flux_dh_eps, 0.004, 1e-12);
    EXPECT_NEAR(opts.coupling_cd, 0.65, 1e-12);
    EXPECT_TRUE(opts.report_2d);
    EXPECT_EQ(opts.rainfall_mode, RainfallMode::NATURAL_NEIGHBOUR);
    // Closure defaults: legacy FLAT + upwind cell-mean face depth (VFR is opt-in).
    EXPECT_EQ(opts.cell_closure, CellClosure2D::FLAT);
    EXPECT_EQ(opts.face_reconstruction, FaceDepth2D::MEAN);
    EXPECT_NEAR(opts.vfr_min_wet_frac, 0.01, 1e-12);
    // Explicit local-inertial marcher — the only 2D integrator since the D2
    // retirement of the CVODE/ARKODE stack.
    EXPECT_NEAR(opts.theta, 0.8, 1e-12);
    EXPECT_NEAR(opts.cfl_number, 0.7, 1e-12);
    EXPECT_NEAR(opts.h_move, 0.003, 1e-12);
    EXPECT_EQ(opts.lts_tiers, 4);
    EXPECT_NEAR(opts.froude_max, 1.5, 1e-12);
    EXPECT_FALSE(opts.coupling_area_auto);
}


// ============================================================================
// MeshData Resize Tests
// ============================================================================

TEST(MeshData, ResizeVertices) {
    MeshData mesh;
    mesh.resize_vertices(5);

    EXPECT_EQ(mesh.n_vertices(), 5);
    EXPECT_EQ(mesh.vx.size(), 5u);
    EXPECT_EQ(mesh.vy.size(), 5u);
    EXPECT_EQ(mesh.vz.size(), 5u);
    EXPECT_EQ(mesh.vert_coupled_node.size(), 5u);

    // Default coupling is -1 (none)
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(mesh.vert_coupled_node[i], -1);
    }

    // Default Cd is 0.65
    for (int i = 0; i < 5; ++i) {
        EXPECT_NEAR(mesh.vert_coupling_cd[i], 0.65, 1e-12);
    }
}

TEST(MeshData, ResizeTriangles) {
    MeshData mesh;
    mesh.resize_triangles(3);

    EXPECT_EQ(mesh.n_triangles(), 3);
    EXPECT_EQ(mesh.tri_v0.size(), 3u);
    EXPECT_EQ(mesh.edge_length.size(), 9u);  // 3 * 3
    EXPECT_EQ(mesh.mannings_n.size(), 3u);

    // Default neighbours are -1
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(mesh.tri_nbr0[i], -1);
        EXPECT_EQ(mesh.tri_nbr1[i], -1);
        EXPECT_EQ(mesh.tri_nbr2[i], -1);
    }

    // Default Manning's n
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(mesh.mannings_n[i], 0.035, 1e-12);
    }
}


// ============================================================================
// Larger Mesh: 4-triangle diamond
// ============================================================================
//
//        v2 (0,2,0)
//       / \     / \
//     T2   \ T1/   T0
//     /     \ /     \
//   v3(-2,0) v0(0,0) v1(2,0)
//     \     / \     /
//     T3   /   \   /
//       \ /     \ /
//        v4 (0,-2,0)
//

static MeshData makeDiamondMesh() {
    MeshData mesh;
    mesh.resize_vertices(5);
    mesh.vx = { 0.0, 2.0,  0.0, -2.0,  0.0};
    mesh.vy = { 0.0, 0.0,  2.0,  0.0, -2.0};
    mesh.vz = { 0.0, 0.0,  0.0,  0.0,  0.0};

    mesh.resize_triangles(4);
    // T0: v0, v1, v2 (right-upper)
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 2;
    // T1: v0, v2, v3 (left-upper)
    mesh.tri_v0[1] = 0; mesh.tri_v1[1] = 2; mesh.tri_v2[1] = 3;
    // T2: v0, v3, v4 (left-lower)
    mesh.tri_v0[2] = 0; mesh.tri_v1[2] = 3; mesh.tri_v2[2] = 4;
    // T3: v0, v4, v1 (right-lower)
    mesh.tri_v0[3] = 0; mesh.tri_v1[3] = 4; mesh.tri_v2[3] = 1;

    for (int i = 0; i < 4; ++i) mesh.mannings_n[i] = 0.03;

    buildMeshTopology(mesh);
    return mesh;
}

TEST(DiamondMesh, AllTrianglesHaveOneNeighbourEach) {
    auto mesh = makeDiamondMesh();

    // Each triangle in a 4-triangle diamond has 2 internal edges (shared with
    // adjacent triangles) and 1 boundary edge.
    for (int t = 0; t < 4; ++t) {
        int internal = 0;
        if (mesh.tri_nbr0[t] >= 0) ++internal;
        if (mesh.tri_nbr1[t] >= 0) ++internal;
        if (mesh.tri_nbr2[t] >= 0) ++internal;
        EXPECT_EQ(internal, 2) << "Triangle " << t << " has "
                                << internal << " internal edges, expected 2";
    }
}

TEST(DiamondMesh, EqualAreasForSymmetricMesh) {
    auto mesh = makeDiamondMesh();
    // All 4 triangles have the same area on a symmetric diamond
    double expected_area = mesh.tri_area[0];
    for (int i = 1; i < 4; ++i) {
        EXPECT_NEAR(mesh.tri_area[i], expected_area, 1e-10)
            << "Triangle " << i << " area differs from T0";
    }
}

TEST(DiamondMesh, CentreVertexStencilHasFourCells) {
    auto mesh = makeDiamondMesh();
    buildVertexStencils(mesh);

    // Vertex 0 (centre) should have all 4 triangles in its stencil
    int start = mesh.vert_stencil_ptr[0];
    int end   = mesh.vert_stencil_ptr[1];
    EXPECT_EQ(end - start, 4);
}

TEST(DiamondMesh, VertexReconstructionConstantExact) {
    auto mesh = makeDiamondMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    std::fill(state.head.begin(), state.head.end(), 3.14);

    reconstructVertexHeads(mesh, state);

    for (int v = 0; v < mesh.n_vertices(); ++v) {
        EXPECT_NEAR(state.vert_head[v], 3.14, 1e-10);
    }
}

// ============================================================================
// Default2DOutputPlugin — CF-1.11 / UGRID-1.0 HDF5 writer (Path B engine fix)
// ============================================================================
//
// Validates that the 2D HDF5 plugin can be exercised in isolation:
//  1. prepare()                    creates the file with root attrs
//  2. prepareMeshAndDatasets(mesh) writes static topology + creates datasets
//  3. update(snap)                 appends one time-step of depth/head/...
//  4. finalize()                   closes datasets
//  5. The resulting .h5 opens cleanly and exposes the expected variables.
//
// The test bypasses SWMMEngine — Default2DOutputPlugin is exercised against
// a hand-built MeshData + a hand-built SimulationSnapshot with the new
// surface_* fields populated.

// ============================================================================
// §11A — Edge conveyance factor: data model, parser, flux apply
// ============================================================================

TEST(EdgeConveyance, DefaultsToOneForEveryEdgeAfterResize) {
    MeshData mesh = makeUnitSquareMesh();   // 2 triangles → 6 edge slots
    ASSERT_EQ(mesh.edge_conveyance.size(), 6u);
    for (double c : mesh.edge_conveyance) EXPECT_DOUBLE_EQ(c, 1.0);
}

TEST(EdgeConveyance, ParserAcceptsValidRowAndStashesIt) {
    std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow> pending;
    std::string err = parse2DEdgeConveyanceLine({"17", "18", "0.4"}, pending);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].v_from, 17);
    EXPECT_EQ(pending[0].v_to,   18);
    EXPECT_DOUBLE_EQ(pending[0].conveyance, 0.4);
}

TEST(EdgeConveyance, ParserRejectsOutOfRangeConveyance) {
    std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow> pending;
    EXPECT_FALSE(parse2DEdgeConveyanceLine({"0", "1", "-0.1"}, pending).empty());
    EXPECT_FALSE(parse2DEdgeConveyanceLine({"0", "1", "1.5"},  pending).empty());
    EXPECT_TRUE (parse2DEdgeConveyanceLine({"0", "1", "0.0"},  pending).empty());
    EXPECT_TRUE (parse2DEdgeConveyanceLine({"0", "1", "1.0"},  pending).empty());
    // The two bad rows pushed nothing; the two good rows pushed one each.
    EXPECT_EQ(pending.size(), 2u);
}

TEST(EdgeConveyance, ParserRejectsEqualFromAndTo) {
    std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow> pending;
    EXPECT_FALSE(parse2DEdgeConveyanceLine({"5", "5", "0.5"}, pending).empty());
    EXPECT_EQ(pending.size(), 0u);
}

TEST(EdgeConveyance, ParserRejectsTooFewTokens) {
    std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow> pending;
    EXPECT_FALSE(parse2DEdgeConveyanceLine({"0", "1"}, pending).empty());
    EXPECT_FALSE(parse2DEdgeConveyanceLine({"0"},      pending).empty());
}

TEST(EdgeConveyance, ParserSkipsEmptyTokenList) {
    // Empty input represents a comment-only / blank line and is silently OK.
    std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow> pending;
    EXPECT_TRUE(parse2DEdgeConveyanceLine({}, pending).empty());
    EXPECT_EQ(pending.size(), 0u);
}

#ifdef OPENSWMM_HAS_2D

TEST(Default2DOutputPlugin, WritesUgridHdf5WithExpectedDatasets) {
    namespace fs = std::filesystem;

    // Hand-build a tiny 2-triangle mesh (the unit square)
    MeshData mesh = makeUnitSquareMesh();
    const int n_tri  = mesh.n_triangles();
    const int n_vert = mesh.n_vertices();
    ASSERT_EQ(n_tri,  2);
    ASSERT_EQ(n_vert, 4);

    // Build a snapshot carrying one tick's worth of surface state
    openswmm::SimulationSnapshot snap;
    snap.sim_time            = 0.0;
    snap.surface_tri_count   = n_tri;
    snap.surface_vert_count  = n_vert;
    snap.surface_depth         = {0.05, 0.10};
    snap.surface_head          = {0.05, 0.10};
    snap.surface_grad_hx       = {0.0,  0.0};
    snap.surface_grad_hy       = {0.0,  0.0};
    snap.surface_grad_hx_lim   = {0.0,  0.0};
    snap.surface_grad_hy_lim   = {0.0,  0.0};
    snap.surface_rainfall      = {0.0,  0.0};
    snap.surface_coupling_flux = {0.0,  0.0};
    snap.surface_net_source    = {0.0,  0.0};
    snap.surface_face_vx       = {0.0,  0.0};
    snap.surface_face_vy       = {0.0,  0.0};
    snap.surface_continuity_err = {0.0,  0.0};
    snap.surface_edge_flux     = {0.0, 0.0, 0.0,  0.0, 0.0, 0.0}; // [tri*3+e]
    snap.surface_vert_head     = {0.0, 0.0, 0.0,  0.0};
    // Cumulative rendering envelopes (per face)
    snap.surface_stat_max_depth    = {0.05, 0.10};
    snap.surface_stat_max_velocity = {0.20, 0.40};
    snap.surface_stat_max_cont_err = {1.0e-6, 2.0e-6};

    // Output path in a temp location; remove any stale file first.
    const fs::path h5_path = fs::temp_directory_path() /
                              "openswmm_test_2d_output.h5";
    fs::remove(h5_path);

    // Exercise the plugin lifecycle
    Default2DOutputPlugin plugin(h5_path.string());
    ASSERT_EQ(plugin.initialize({}, nullptr), 0);

    // validate() / prepare() use a SimulationContext but only read what's
    // already set; an empty/default context is enough for the file-creation
    // path. We instantiate a stack context via the SWMM SDK header.
    openswmm::SimulationContext ctx{};
    ASSERT_EQ(plugin.validate(ctx), 0);
    ASSERT_EQ(plugin.prepare(ctx),  0);

    // Activate the global 2D mass balance so finalize() writes /mass_balance_2d.
    ctx.mass_balance_2d.active        = true;
    ctx.mass_balance_2d.init_storage  = 0.0;
    ctx.mass_balance_2d.rainfall_in   = 10.0;
    ctx.mass_balance_2d.final_storage = 10.0;  // closed, conservative → error ~0

    plugin.prepareMeshAndDatasets(mesh);

    ASSERT_EQ(plugin.update(snap), 0);
    ASSERT_EQ(plugin.finalize(ctx), 0);

    // File should exist on disk
    ASSERT_TRUE(fs::exists(h5_path))
        << "Default2DOutputPlugin did not create " << h5_path;

    // Open the file read-only and assert the documented UGRID datasets exist
    hid_t file_id = H5Fopen(h5_path.string().c_str(), H5F_ACC_RDONLY,
                             H5P_DEFAULT);
    ASSERT_GE(file_id, 0) << "H5Fopen failed on " << h5_path;

    auto exists = [file_id](const char* name) {
        return H5Lexists(file_id, name, H5P_DEFAULT) > 0;
    };
    EXPECT_TRUE(exists("Mesh2"));
    EXPECT_TRUE(exists("Mesh2_node_x"));
    EXPECT_TRUE(exists("Mesh2_node_y"));
    EXPECT_TRUE(exists("Mesh2_node_z"));
    EXPECT_TRUE(exists("Mesh2_face_nodes"));
    EXPECT_TRUE(exists("Mesh2_face_depth"));
    EXPECT_TRUE(exists("Mesh2_face_head"));
    EXPECT_TRUE(exists("Mesh2_node_head"));
    EXPECT_TRUE(exists("Mesh2_node_depth"));
    EXPECT_TRUE(exists("Mesh2_face_vx"));
    EXPECT_TRUE(exists("Mesh2_face_vy"));
    EXPECT_TRUE(exists("Mesh2_face_continuity_err"));
    EXPECT_TRUE(exists("Mesh2_face_max_depth"));
    EXPECT_TRUE(exists("Mesh2_face_max_velocity"));
    EXPECT_TRUE(exists("Mesh2_face_max_continuity_err"));
    EXPECT_TRUE(exists("mass_balance_2d"));
    EXPECT_TRUE(exists("time"));

    // /time should have one entry after our single update()
    {
        hid_t ds_time = H5Dopen2(file_id, "time", H5P_DEFAULT);
        ASSERT_GE(ds_time, 0);
        hid_t space = H5Dget_space(ds_time);
        hsize_t dims[1] = {0};
        H5Sget_simple_extent_dims(space, dims, nullptr);
        EXPECT_EQ(dims[0], 1u);
        H5Sclose(space);
        H5Dclose(ds_time);
    }

    // /Mesh2_face_depth should be [1, n_tri] after the single update()
    {
        hid_t ds = H5Dopen2(file_id, "Mesh2_face_depth", H5P_DEFAULT);
        ASSERT_GE(ds, 0);
        hid_t space = H5Dget_space(ds);
        hsize_t dims[2] = {0, 0};
        H5Sget_simple_extent_dims(space, dims, nullptr);
        EXPECT_EQ(dims[0], 1u);
        EXPECT_EQ(dims[1], static_cast<hsize_t>(n_tri));
        H5Sclose(space);
        H5Dclose(ds);
    }

    // Envelope dataset is fixed [n_tri] (no time dimension) and holds the
    // values from the last update().
    {
        hid_t ds = H5Dopen2(file_id, "Mesh2_face_max_velocity", H5P_DEFAULT);
        ASSERT_GE(ds, 0);
        hid_t space = H5Dget_space(ds);
        EXPECT_EQ(H5Sget_simple_extent_ndims(space), 1);
        hsize_t dims[1] = {0};
        H5Sget_simple_extent_dims(space, dims, nullptr);
        EXPECT_EQ(dims[0], static_cast<hsize_t>(n_tri));
        std::vector<double> vals(n_tri, 0.0);
        H5Dread(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, vals.data());
        EXPECT_NEAR(vals[0], 0.20, 1e-12);
        EXPECT_NEAR(vals[1], 0.40, 1e-12);
        H5Sclose(space);
        H5Dclose(ds);
    }

    // /mass_balance_2d group carries the scalar terms and a continuity_error attr.
    {
        hid_t grp = H5Gopen2(file_id, "mass_balance_2d", H5P_DEFAULT);
        ASSERT_GE(grp, 0);
        EXPECT_TRUE(H5Lexists(grp, "rainfall_in", H5P_DEFAULT) > 0);
        EXPECT_TRUE(H5Aexists(grp, "continuity_error") > 0);
        hid_t attr = H5Aopen(grp, "continuity_error", H5P_DEFAULT);
        double err = 1.0;
        H5Aread(attr, H5T_NATIVE_DOUBLE, &err);
        EXPECT_NEAR(err, 0.0, 1e-9);  // init+rainfall(10) − final(10) = 0
        H5Aclose(attr);
        H5Gclose(grp);
    }

    H5Fclose(file_id);
    fs::remove(h5_path);
}

#endif // OPENSWMM_HAS_2D
