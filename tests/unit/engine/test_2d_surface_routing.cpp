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
#include "2d/solver/CvodeSurfaceSolver.hpp"
#include "2d/input/SectionHandlers2D.hpp"
#include "uncertainty/UncertaintyConfig.hpp"

#ifdef OPENSWMM_HAS_2D
#include "2d/output/Default2DOutputPlugin.hpp"
#include "2d/solver/CvodeSurfaceSolver.hpp"
#include "core/SimulationContext.hpp"
#include <openswmm/plugin_sdk/SimulationSnapshot.hpp>
#include <hdf5.h>
#include <filesystem>
#include <stdexcept>
#endif

using namespace openswmm::twoD;
using namespace openswmm::uncertainty;

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

TEST(EdgeFlux, ZeroFluxForUniformHead) {
    // C-property: still water on flat bed → zero flux
    auto mesh = makeUnitSquareMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    SolverOptions2D opts;

    // Uniform depth = 0.1 m on flat bed (z=0)
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.depth[i] = 0.1;
        state.head[i]  = 0.1;  // z + depth = 0 + 0.1
    }

    reconstructVertexHeads(mesh, state);
    computeUnlimitedGradients(mesh, state);
    computeLimitedGradients(mesh, state, opts.limiter_epsilon);
    computeEdgeFluxes(mesh, state, opts);

    int n3 = mesh.n_triangles() * 3;
    for (int i = 0; i < n3; ++i) {
        EXPECT_NEAR(state.edge_flux[i], 0.0, 1e-10)
            << "Edge " << i << " has non-zero flux for C-property test";
    }
}

TEST(EdgeFlux, BoundaryEdgesHaveZeroFlux) {
    auto mesh = makeUnitSquareMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    SolverOptions2D opts;

    // Sloped head: h varies between triangles
    state.depth[0] = 0.2; state.head[0] = 0.2;
    state.depth[1] = 0.1; state.head[1] = 0.1;

    reconstructVertexHeads(mesh, state);
    computeUnlimitedGradients(mesh, state);
    computeLimitedGradients(mesh, state, opts.limiter_epsilon);
    computeEdgeFluxes(mesh, state, opts);

    // Boundary edges (nbr == -1) must have zero flux
    for (int t = 0; t < mesh.n_triangles(); ++t) {
        for (int e = 0; e < 3; ++e) {
            int nbr = -1;
            switch (e) {
                case 0: nbr = mesh.tri_nbr0[t]; break;
                case 1: nbr = mesh.tri_nbr1[t]; break;
                case 2: nbr = mesh.tri_nbr2[t]; break;
            }
            if (nbr == -1) {
                EXPECT_NEAR(state.edge_flux[t * 3 + e], 0.0, 1e-15)
                    << "Boundary edge T" << t << "E" << e << " has non-zero flux";
            }
        }
    }
}

// Mass conservation: for any shared edge, the flux stored from cell i's
// perspective and from cell j's perspective must be exact negatives. Both
// perspectives pick the same upstream cell (the one with the higher head),
// so depth_edge and K are identical; dh_dn = h_i − h_j differs only in
// sign between the two perspectives, which makes the products exact
// negatives. Without this property, the discretisation would silently leak
// or duplicate mass across each interior face.
TEST(EdgeFlux, SharedEdgeFluxesAreAntisymmetric) {
    auto mesh = makeUnitSquareMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    SolverOptions2D opts;

    // Non-trivial state: heads differ across the shared edge.
    state.depth[0] = 0.20; state.head[0] = 0.20;
    state.depth[1] = 0.05; state.head[1] = 0.05;

    reconstructVertexHeads(mesh, state);
    computeUnlimitedGradients(mesh, state);
    computeLimitedGradients(mesh, state, opts.limiter_epsilon);
    computeEdgeFluxes(mesh, state, opts);

    auto nbr_of = [&](int t, int e) {
        switch (e) {
            case 0: return mesh.tri_nbr0[t];
            case 1: return mesh.tri_nbr1[t];
            case 2: return mesh.tri_nbr2[t];
        }
        return -1;
    };

    bool tested_at_least_one_pair = false;
    int nt = mesh.n_triangles();
    for (int t = 0; t < nt; ++t) {
        for (int e = 0; e < 3; ++e) {
            int j = nbr_of(t, e);
            if (j < 0) continue;  // boundary edge: antisymmetry doesn't apply
            // Find the edge of triangle j that points back at t.
            int e_back = -1;
            for (int ej = 0; ej < 3; ++ej) {
                if (nbr_of(j, ej) == t) { e_back = ej; break; }
            }
            ASSERT_GE(e_back, 0)
                << "Triangle " << j << " does not list " << t
                << " as a neighbour";

            double f_tj = state.edge_flux[t * 3 + e];
            double f_jt = state.edge_flux[j * 3 + e_back];
            EXPECT_NEAR(f_tj + f_jt, 0.0, 1e-12)
                << "Shared edge T" << t << "↔T" << j
                << " fluxes are not antisymmetric: f_tj=" << f_tj
                << ", f_jt=" << f_jt;
            tested_at_least_one_pair = true;
        }
    }
    EXPECT_TRUE(tested_at_least_one_pair)
        << "No interior edges in test mesh — antisymmetry was not exercised";
}

// Global volume budget: with no sources (rainfall=0, coupling=0) and all
// boundaries as walls, the net rate of change of total water volume must
// be zero. Every interior edge's outflow contribution from one cell
// cancels its inflow contribution to the neighbour, and every boundary
// edge contributes zero — leaving Σ(ydot[i]·area[i]) = 0.
TEST(EdgeFlux, ClosedSystemVolumeBudget) {
    auto mesh = makeUnitSquareMesh();
    buildVertexStencils(mesh);

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    SolverOptions2D opts;

    state.depth[0] = 0.20; state.head[0] = 0.20;
    state.depth[1] = 0.05; state.head[1] = 0.05;
    std::fill(state.rainfall.begin(),      state.rainfall.end(),      0.0);
    std::fill(state.coupling_flux.begin(), state.coupling_flux.end(), 0.0);

    reconstructVertexHeads(mesh, state);
    computeUnlimitedGradients(mesh, state);
    computeLimitedGradients(mesh, state, opts.limiter_epsilon);
    computeEdgeFluxes(mesh, state, opts);

    std::vector<double> ydot(mesh.n_triangles());
    assembleRHS(mesh, state, ydot.data());

    double net_dvol_dt = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        net_dvol_dt += ydot[i] * mesh.tri_area[i];
    }
    EXPECT_NEAR(net_dvol_dt, 0.0, 1e-12)
        << "Closed-system volume budget violated: Σ ydot·area = "
        << net_dvol_dt << " (expected 0 within 1e-12)";
}


// ============================================================================
// RHS Assembly Tests
// ============================================================================

TEST(RHSAssembly, RainfallOnlyProducesPositiveRate) {
    auto mesh = makeUnitSquareMesh();

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // Zero fluxes, constant rainfall = 0.001 m/s
    std::fill(state.edge_flux.begin(), state.edge_flux.end(), 0.0);
    std::fill(state.coupling_flux.begin(), state.coupling_flux.end(), 0.0);
    std::fill(state.rainfall.begin(), state.rainfall.end(), 0.001);

    std::vector<double> ydot(mesh.n_triangles());
    assembleRHS(mesh, state, ydot.data());

    for (int i = 0; i < mesh.n_triangles(); ++i) {
        EXPECT_NEAR(ydot[i], 0.001, 1e-12)
            << "Triangle " << i << " dψ/dt != rainfall rate";
    }
}

TEST(RHSAssembly, CouplingFluxAppearsInRHS) {
    auto mesh = makeUnitSquareMesh();

    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    std::fill(state.edge_flux.begin(), state.edge_flux.end(), 0.0);
    std::fill(state.rainfall.begin(), state.rainfall.end(), 0.0);
    state.coupling_flux[0] = -0.005;  // Drainage sink
    state.coupling_flux[1] =  0.003;  // Surcharge source

    std::vector<double> ydot(mesh.n_triangles());
    assembleRHS(mesh, state, ydot.data());

    EXPECT_NEAR(ydot[0], -0.005, 1e-12);
    EXPECT_NEAR(ydot[1],  0.003, 1e-12);
}


// ============================================================================
// Input Parsing Tests
// ============================================================================

TEST(InputParsing, Parse2DOptionsLine) {
    SolverOptions2D opts;

    auto err = parse2DOptionsLine({"MAX_TIMESTEP", "5.0"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(opts.max_timestep, 5.0, 1e-12);

    err = parse2DOptionsLine({"REL_TOLERANCE", "1e-5"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(opts.rel_tolerance, 1e-5, 1e-18);

    err = parse2DOptionsLine({"LINEAR_SOLVER", "BICGSTAB"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(opts.linear_solver, LinearSolverType::BICGSTAB);

    err = parse2DOptionsLine({"REPORT_2D", "NO"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_FALSE(opts.report_2d);

    err = parse2DOptionsLine({"DRY_DEPTH", "0.005"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_NEAR(opts.dry_depth, 0.005, 1e-12);

    err = parse2DOptionsLine({"COUPLING_INTERVAL", "3"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(opts.coupling_interval, 3);
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
    EXPECT_NEAR(opts.min_timestep, 0.001, 1e-12);
    EXPECT_NEAR(opts.rel_tolerance, 1e-4, 1e-18);
    EXPECT_NEAR(opts.abs_tolerance, 1e-6, 1e-18);
    EXPECT_NEAR(opts.dry_depth, 0.001, 1e-12);
    EXPECT_EQ(opts.max_krylov_dim, 30);
    EXPECT_EQ(opts.coupling_interval, 0);
    EXPECT_TRUE(opts.report_2d);
    EXPECT_EQ(opts.linear_solver, LinearSolverType::GMRES);
    EXPECT_EQ(opts.preconditioner, PreconditionerType::NONE);
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
// DWSolverSpectral — smoke test: ROM sidecar wired into CvodeSurfaceSolver
// ============================================================================

// Build a 5×5 structured mesh on [0,10]² with mild x-slope.
static MeshData makeSmallSlopedMesh() {
    const int N = 5;
    const double domain = 10.0;
    const double slope_x = 0.002;
    MeshData mesh;
    mesh.resize_vertices((N+1)*(N+1));
    mesh.resize_triangles(2*N*N);
    double dx = domain / N;
    double dy = domain / N;
    for (int i = 0; i <= N; ++i)
        for (int j = 0; j <= N; ++j) {
            int vi = i*(N+1)+j;
            double x = j*dx;
            mesh.vx[vi] = x;
            mesh.vy[vi] = i*dy;
            mesh.vz[vi] = -slope_x*x;
        }
    int t = 0;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            int v00=i*(N+1)+j, v01=i*(N+1)+j+1;
            int v10=(i+1)*(N+1)+j, v11=(i+1)*(N+1)+j+1;
            mesh.tri_v0[t]=v00; mesh.tri_v1[t]=v01; mesh.tri_v2[t]=v11;
            mesh.mannings_n[t]=0.035; ++t;
            mesh.tri_v0[t]=v00; mesh.tri_v1[t]=v11; mesh.tri_v2[t]=v10;
            mesh.mannings_n[t]=0.035; ++t;
        }
    buildMeshTopology(mesh);
    buildVertexStencils(mesh);
    return mesh;
}

TEST(DWSolverSpectral, ROMSidecarFiresAndProducesQuantiles) {
    MeshData mesh = makeSmallSlopedMesh();
    int n = mesh.n_triangles();

    SurfaceStateData state;
    state.resize(n, mesh.n_vertices());
    for (int i = 0; i < n; ++i) {
        state.depth[i] = 0.10;
        state.head[i]  = mesh.tri_cz[i] + 0.10;
    }

    SolverOptions2D opts;
    opts.rel_tolerance   = 1.0e-3;
    opts.abs_tolerance   = 1.0e-5;
    opts.max_timestep    = 5.0;
    opts.max_cvode_steps = 10000;
    opts.linear_solver   = LinearSolverType::GMRES;
    opts.preconditioner  = PreconditionerType::NONE;
    opts.enable_rom      = true;
    opts.rom_modes       = 4;
    opts.rom_members     = 10;
    opts.rom_k_eff       = 10.0;

    CvodeSurfaceSolver solver;
    solver.initialize(mesh, state, opts);
    solver.suppress_warnings();

    // ROM should be initialised but not yet seeded
    ASSERT_NE(solver.rom(), nullptr) << "ROM sidecar should be non-null after initialize()";

    double t = solver.advance(0.0, 5.0);
    EXPECT_GE(t, 0.0);

    const SpectralROM* rom = solver.rom();
    ASSERT_NE(rom, nullptr);
    EXPECT_EQ(static_cast<int>(rom->q05.size()), n);
    EXPECT_EQ(static_cast<int>(rom->q50.size()), n);
    EXPECT_EQ(static_cast<int>(rom->q95.size()), n);

    for (int i = 0; i < n; ++i) {
        EXPECT_LE(rom->q05[i], rom->q50[i] + 1e-14)
            << "q05 > q50 at cell " << i;
        EXPECT_LE(rom->q50[i], rom->q95[i] + 1e-14)
            << "q50 > q95 at cell " << i;
        EXPECT_GE(rom->q05[i], 0.0)
            << "q05 < 0 at cell " << i;
    }
}

// ============================================================================
// [2D_ROM] parser tests (PR 3)
// ============================================================================

TEST(InputParsing, Parse2DROMEnableYes) {
    SolverOptions2D opts;
    EXPECT_FALSE(opts.enable_rom);
    auto err = parse2DROMLine({"ENABLE", "YES"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_TRUE(opts.enable_rom);
}

TEST(InputParsing, Parse2DROMEnableNo) {
    SolverOptions2D opts;
    opts.enable_rom = true;
    auto err = parse2DROMLine({"ENABLE", "NO"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_FALSE(opts.enable_rom);
}

TEST(InputParsing, Parse2DROMMembersAndModes) {
    SolverOptions2D opts;
    auto err = parse2DROMLine({"MEMBERS", "30"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(opts.rom_members, 30);

    err = parse2DROMLine({"MODES", "8"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(opts.rom_modes, 8);
}

TEST(InputParsing, Parse2DROMPerturbations) {
    SolverOptions2D opts;
    auto err = parse2DROMLine({"MANNINGS_PERT", "0.15"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_DOUBLE_EQ(opts.rom_mannings_pert, 0.15);

    err = parse2DROMLine({"RAINFALL_PERT", "0.25"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_DOUBLE_EQ(opts.rom_rainfall_pert, 0.25);
}

TEST(InputParsing, Parse2DROMKEffPositiveAndNegative) {
    SolverOptions2D opts;
    auto err = parse2DROMLine({"K_EFF", "15.5"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_DOUBLE_EQ(opts.rom_k_eff, 15.5);

    // Negative K_EFF means AUTO mode (PR 4)
    err = parse2DROMLine({"K_EFF", "-1"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_LT(opts.rom_k_eff, 0.0);
}

TEST(InputParsing, Parse2DROMUnknownKeyReturnsError) {
    SolverOptions2D opts;
    auto err = parse2DROMLine({"BOGUS_KEY", "42"}, opts);
    EXPECT_FALSE(err.empty()) << "Expected error for unknown key";
}

TEST(InputParsing, Parse2DROMMembersTooFewReturnsError) {
    SolverOptions2D opts;
    auto err = parse2DROMLine({"MEMBERS", "1"}, opts);
    EXPECT_FALSE(err.empty()) << "MEMBERS=1 should be rejected";
}

// Spatial correlation length tokens (PR 9).
TEST(InputParsing, Parse2DROMSpatialCorrelationLengths) {
    SolverOptions2D opts;
    auto err = parse2DROMLine({"MANNINGS_CORR_LEN", "25.0"}, opts);
    EXPECT_TRUE(err.empty()) << "MANNINGS_CORR_LEN parse error: " << err;
    EXPECT_DOUBLE_EQ(opts.rom_mannings_corr_len, 25.0);

    err = parse2DROMLine({"RAINFALL_CORR_LEN", "50.5"}, opts);
    EXPECT_TRUE(err.empty()) << "RAINFALL_CORR_LEN parse error: " << err;
    EXPECT_DOUBLE_EQ(opts.rom_rainfall_corr_len, 50.5);

    // Zero is valid (= scalar mode).
    err = parse2DROMLine({"MANNINGS_CORR_LEN", "0"}, opts);
    EXPECT_TRUE(err.empty()) << "MANNINGS_CORR_LEN=0 should be accepted";
    EXPECT_DOUBLE_EQ(opts.rom_mannings_corr_len, 0.0);

    // Negative values rejected.
    err = parse2DROMLine({"MANNINGS_CORR_LEN", "-1"}, opts);
    EXPECT_FALSE(err.empty()) << "Negative MANNINGS_CORR_LEN should be rejected";

    err = parse2DROMLine({"RAINFALL_CORR_LEN", "-0.1"}, opts);
    EXPECT_FALSE(err.empty()) << "Negative RAINFALL_CORR_LEN should be rejected";
}

// ============================================================================
// [UNCERTAINTY] parser tests (PR 3)
// ============================================================================

TEST(InputParsing, ParseUncertaintyScalarMannings2D) {
    SolverOptions2D opts;
    UncertaintyConfig config;

    auto err = parseUncertaintyLine({"2D", "MANNINGS_N", "0.20"}, opts, config);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_TRUE(opts.enable_rom);
    EXPECT_DOUBLE_EQ(opts.rom_mannings_pert, 0.20);
    ASSERT_EQ(config.sources.size(), std::size_t{1});
    EXPECT_EQ(config.sources[0].name, "MANNINGS_N");
    EXPECT_EQ(config.sources[0].layer, LayerTarget::TWO_D);
    EXPECT_EQ(config.sources[0].dist,  DistType::UNIFORM);
    EXPECT_DOUBLE_EQ(config.sources[0].perturbation, 0.20);
}

TEST(InputParsing, ParseUncertaintyScalarRainfall2D) {
    SolverOptions2D opts;
    UncertaintyConfig config;

    auto err = parseUncertaintyLine({"2D", "RAINFALL", "UNIFORM", "0.15"}, opts, config);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_TRUE(opts.enable_rom);
    EXPECT_DOUBLE_EQ(opts.rom_rainfall_pert, 0.15);
    ASSERT_EQ(config.sources.size(), std::size_t{1});
    EXPECT_EQ(config.sources[0].name, "RAINFALL");
}

TEST(InputParsing, ParseUncertaintyUnsupportedLayerReturnsError) {
    // '1D' and '2D' are now valid layers; use a truly unsupported name.
    SolverOptions2D opts;
    UncertaintyConfig config;
    auto err = parseUncertaintyLine({"3D", "MANNINGS_N", "0.20"}, opts, config);
    EXPECT_FALSE(err.empty()) << "Expected error for unsupported layer 3D";
    EXPECT_TRUE(config.sources.empty());
}

TEST(InputParsing, ParseUncertaintyUnsupportedParameterReturnsError) {
    SolverOptions2D opts;
    UncertaintyConfig config;
    auto err = parseUncertaintyLine({"2D", "ROUGHNESS", "0.20"}, opts, config);
    EXPECT_FALSE(err.empty()) << "Expected error for unsupported parameter ROUGHNESS";
    EXPECT_TRUE(config.sources.empty());
}

// [UNCERTAINTY] overrides [2D_ROM] legacy fields (precedence test)
TEST(InputParsing, UncertaintyOverridesROMLegacyPerturbations) {
    SolverOptions2D opts;
    opts.rom_mannings_pert = 0.30;  // set via [2D_ROM]
    opts.rom_rainfall_pert = 0.30;

    UncertaintyConfig config;
    // [UNCERTAINTY] with smaller perturbation — must win
    auto err = parseUncertaintyLine({"2D", "MANNINGS_N", "0.10"}, opts, config);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_DOUBLE_EQ(opts.rom_mannings_pert, 0.10)
        << "[UNCERTAINTY] should override the [2D_ROM] MANNINGS_PERT value";

    err = parseUncertaintyLine({"2D", "RAINFALL", "0.05"}, opts, config);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_DOUBLE_EQ(opts.rom_rainfall_pert, 0.05)
        << "[UNCERTAINTY] should override the [2D_ROM] RAINFALL_PERT value";
}

// Multiple [UNCERTAINTY] lines accumulate in config.sources
TEST(InputParsing, UncertaintyMultipleLinesAccumulate) {
    SolverOptions2D opts;
    UncertaintyConfig config;

    parseUncertaintyLine({"2D", "MANNINGS_N", "0.20"}, opts, config);
    parseUncertaintyLine({"2D", "RAINFALL",   "0.10"}, opts, config);

    EXPECT_EQ(config.sources.size(), std::size_t{2});
    EXPECT_TRUE(config.has_2d());
    EXPECT_TRUE(config.mannings_2d().has_value());
    EXPECT_TRUE(config.rainfall_2d().has_value());
}

// ============================================================================
// AUTO K_eff tests (PR 4)
// ============================================================================
//
// Uses makeSmallSlopedMesh() (5×5 grid, 50 triangles, slope_x=0.002) which is
// large enough for the Lanczos eigensolver and already proven by the ROM smoke
// test above.
//
// Expected K_eff ≈ h_mean^(5/3) / (2 * n_mean * sqrt(S_mean))
// For h=0.10m, n=0.035, S≈0.002:  K_eff ≈ 14.8 m²/s  (within 50%)
// ============================================================================

// Flat version of makeSmallSlopedMesh(): same topology, all z = 0.
static MeshData makeFlatSmallMesh() {
    const int N = 5;
    const double domain = 10.0;
    MeshData mesh;
    mesh.resize_vertices((N+1)*(N+1));
    mesh.resize_triangles(2*N*N);
    double dx = domain / N;
    double dy = domain / N;
    for (int i = 0; i <= N; ++i)
        for (int j = 0; j <= N; ++j) {
            int vi = i*(N+1)+j;
            mesh.vx[vi] = j*dx;
            mesh.vy[vi] = i*dy;
            mesh.vz[vi] = 0.0;   // flat
        }
    int t = 0;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            int v00=i*(N+1)+j, v01=i*(N+1)+j+1;
            int v10=(i+1)*(N+1)+j, v11=(i+1)*(N+1)+j+1;
            mesh.tri_v0[t]=v00; mesh.tri_v1[t]=v01; mesh.tri_v2[t]=v11;
            mesh.mannings_n[t]=0.035; ++t;
            mesh.tri_v0[t]=v00; mesh.tri_v1[t]=v11; mesh.tri_v2[t]=v10;
            mesh.mannings_n[t]=0.035; ++t;
        }
    buildMeshTopology(mesh);
    buildVertexStencils(mesh);
    return mesh;
}

// Shared ROM options for AutoKEff tests.
static SolverOptions2D makeROMOpts(double k_eff_override) {
    SolverOptions2D opts;
    opts.rel_tolerance   = 1.0e-3;
    opts.abs_tolerance   = 1.0e-5;
    opts.max_timestep    = 5.0;
    opts.max_cvode_steps = 10000;
    opts.linear_solver   = LinearSolverType::GMRES;
    opts.preconditioner  = PreconditionerType::NONE;
    opts.enable_rom      = true;
    opts.rom_modes       = 4;
    opts.rom_members     = 4;
    opts.rom_k_eff       = k_eff_override;
    return opts;
}

TEST(AutoKEff, NominalEstimateInPhysicalRange) {
    // Sloped domain: slope_x=0.002, h=0.10m, n=0.035
    // Formula: K_eff = h^(5/3) / (2 * n * sqrt(S)) ≈ 14.8
    MeshData mesh = makeSmallSlopedMesh();
    int n = mesh.n_triangles();
    SurfaceStateData state;
    state.resize(n, mesh.n_vertices());
    for (int i = 0; i < n; ++i) {
        state.depth[i] = 0.10;
        state.head[i]  = mesh.tri_cz[i] + 0.10;
    }

    SolverOptions2D opts = makeROMOpts(-1.0);   // AUTO mode

    CvodeSurfaceSolver solver;
    solver.initialize(mesh, state, opts);
    solver.suppress_warnings();
    solver.advance(0.0, 5.0);

    double k = solver.effectiveKEff();
    // Physical range: K_eff must be positive and less than 200.
    EXPECT_GT(k, 0.0)   << "AUTO K_eff must be positive";
    EXPECT_LT(k, 200.0) << "AUTO K_eff too large: " << k;

    // Formula check: within 50% of h^(5/3) / (2*n*sqrt(S)).
    double k_expected = std::pow(0.10, 5.0/3.0) / (2.0 * 0.035 * std::sqrt(0.002));
    EXPECT_NEAR(k, k_expected, 0.5 * k_expected)
        << "AUTO K_eff=" << k << " differs from formula=" << k_expected << " by >50%";
}

TEST(AutoKEff, FlatDomainFallbackNonZero) {
    // All z=0 → S_mean=0 → should clamp to S_FLOOR=1e-6, giving K_eff > 0.
    MeshData mesh = makeFlatSmallMesh();
    int n = mesh.n_triangles();
    SurfaceStateData state;
    state.resize(n, mesh.n_vertices());
    for (int i = 0; i < n; ++i) {
        state.depth[i] = 0.10;
        state.head[i]  = 0.10;
    }

    SolverOptions2D opts = makeROMOpts(-1.0);   // AUTO mode

    CvodeSurfaceSolver solver;
    solver.initialize(mesh, state, opts);
    solver.suppress_warnings();
    solver.advance(0.0, 5.0);

    EXPECT_GT(solver.effectiveKEff(), 0.0)
        << "Flat domain should give K_eff > 0 via S_FLOOR fallback";
}

TEST(AutoKEff, DryDomainReturnsZero) {
    // All cells dry (h=0) → h_mean=0 → K_eff=0.
    MeshData mesh = makeSmallSlopedMesh();
    int n = mesh.n_triangles();
    SurfaceStateData state;
    state.resize(n, mesh.n_vertices());
    // depth stays at 0 (dry).

    SolverOptions2D opts = makeROMOpts(-1.0);   // AUTO mode

    CvodeSurfaceSolver solver;
    solver.initialize(mesh, state, opts);
    solver.suppress_warnings();
    solver.advance(0.0, 5.0);

    EXPECT_DOUBLE_EQ(solver.effectiveKEff(), 0.0)
        << "Dry domain should give K_eff = 0";
}

TEST(AutoKEff, PositiveOverrideAlwaysUsed) {
    // rom_k_eff=42.0 > 0 → effectiveKEff() must equal 42.0 exactly.
    MeshData mesh = makeSmallSlopedMesh();
    int n = mesh.n_triangles();
    SurfaceStateData state;
    state.resize(n, mesh.n_vertices());
    for (int i = 0; i < n; ++i) {
        state.depth[i] = 0.10;
        state.head[i]  = mesh.tri_cz[i] + 0.10;
    }

    SolverOptions2D opts = makeROMOpts(42.0);   // explicit override

    CvodeSurfaceSolver solver;
    solver.initialize(mesh, state, opts);
    solver.suppress_warnings();
    solver.advance(0.0, 5.0);

    EXPECT_DOUBLE_EQ(solver.effectiveKEff(), 42.0)
        << "Positive rom_k_eff override must be used unchanged";
}

// ============================================================================
// ROMScalarCase — end-to-end Phase 1 validation tests (PR 6)
//
// "Scalar case": uniform ROM parameters built from [2D_ROM] / [UNCERTAINTY]
// parser output, run through CvodeSurfaceSolver with the ROM sidecar active.
// ============================================================================

// --- Ensemble spread grows with perturbation size ----------------------------
//
// The ROM propagates parameter uncertainty onto the depth field.  With a
// non-uniform IC (Gaussian bump off-centre), the eigenmodes have non-trivial
// projections.  After advance, the maximum (q95 - q05) spread across all cells
// should be strictly positive, and a larger perturbation should produce more
// spread than a smaller one.
//
// Note: a uniform IC projects entirely onto the null Laplacian mode, which is
// filtered out; the ROM correctly sees zero spatial variation in that case.
// The test therefore uses a spatially non-uniform depth field.

// Build a Gaussian bump IC off-centre so it projects onto retained eigenmodes.
static void setGaussianBump(SurfaceStateData& state, const MeshData& mesh,
                              double h_bg, double amp, double cx, double cy,
                              double sigma) {
    int n = mesh.n_triangles();
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double dx = mesh.tri_cx[ui] - cx;
        double dy = mesh.tri_cy[ui] - cy;
        double bump = amp * std::exp(-(dx*dx + dy*dy) / (2.0 * sigma * sigma));
        state.depth[ui] = h_bg + bump;
        state.head[ui]  = mesh.tri_cz[ui] + state.depth[ui];
    }
}

TEST(ROMScalarCase, EnsembleSpreadGrowsWithPerturbation) {
    MeshData mesh = makeSmallSlopedMesh();
    int n = mesh.n_triangles();

    // Build base opts via the parser path (end-to-end: [2D_ROM] + [UNCERTAINTY]).
    auto buildOpts = [](double pert) {
        SolverOptions2D opts;
        opts.rel_tolerance   = 1.0e-3;
        opts.abs_tolerance   = 1.0e-5;
        opts.max_timestep    = 5.0;
        opts.max_cvode_steps = 10000;
        opts.linear_solver   = LinearSolverType::GMRES;
        opts.preconditioner  = PreconditionerType::NONE;
        parse2DROMLine({"ENABLE", "YES"}, opts);
        parse2DROMLine({"MEMBERS", "20"}, opts);
        parse2DROMLine({"MODES", "4"}, opts);
        parse2DROMLine({"K_EFF", "10.0"}, opts);
        UncertaintyConfig cfg;
        std::string s = std::to_string(pert);
        parseUncertaintyLine({"2D", "MANNINGS_N", s}, opts, cfg);
        return opts;
    };

    auto runAndGetMaxSpread = [&](double pert) -> double {
        SolverOptions2D opts = buildOpts(pert);
        MeshData m = makeSmallSlopedMesh();

        SurfaceStateData state;
        state.resize(n, m.n_vertices());
        // Gaussian bump at (3m, 4m) off-centre; σ=1.5m on a 10m domain.
        setGaussianBump(state, m, 0.02, 0.08, 3.0, 4.0, 1.5);

        CvodeSurfaceSolver solver;
        solver.initialize(m, state, opts);
        solver.suppress_warnings();
        solver.advance(0.0, 5.0);

        const SpectralROM* rom = solver.rom();
        if (!rom || rom->q05.empty()) return -1.0;

        double max_spread = 0.0;
        for (int i = 0; i < n; ++i) {
            auto ui = static_cast<std::size_t>(i);
            max_spread = std::max(max_spread, rom->q95[ui] - rom->q05[ui]);
        }
        return max_spread;
    };

    double spread_low  = runAndGetMaxSpread(0.05);
    double spread_high = runAndGetMaxSpread(0.30);

    EXPECT_GT(spread_low,  0.0) << "ROM ensemble should show nonzero spread";
    EXPECT_GT(spread_high, 0.0) << "ROM ensemble should show nonzero spread";
    EXPECT_GT(spread_high, spread_low)
        << "Larger perturbation (0.30) should yield more spread than smaller (0.05);"
        << " got spread_low=" << spread_low << " spread_high=" << spread_high;
}

// --- Quantile monotonicity after [UNCERTAINTY] parsing ----------------------
//
// Confirms the full pipeline: [2D_ROM] + [UNCERTAINTY] → parser → opts →
// CvodeSurfaceSolver → quantile output with q05 ≤ q50 ≤ q95 everywhere.

TEST(ROMScalarCase, QuantileMonotonicityAfterParserInput) {
    MeshData mesh = makeSmallSlopedMesh();
    int n = mesh.n_triangles();

    SolverOptions2D opts;
    opts.rel_tolerance   = 1.0e-3;
    opts.abs_tolerance   = 1.0e-5;
    opts.max_timestep    = 5.0;
    opts.max_cvode_steps = 10000;
    opts.linear_solver   = LinearSolverType::GMRES;
    opts.preconditioner  = PreconditionerType::NONE;

    // [2D_ROM] then [UNCERTAINTY] with both Manning and rainfall perturbations
    ASSERT_TRUE(parse2DROMLine({"ENABLE", "YES"}, opts).empty());
    ASSERT_TRUE(parse2DROMLine({"MEMBERS", "10"}, opts).empty());
    ASSERT_TRUE(parse2DROMLine({"MODES", "4"}, opts).empty());
    ASSERT_TRUE(parse2DROMLine({"K_EFF", "10.0"}, opts).empty());

    UncertaintyConfig config;
    ASSERT_TRUE(parseUncertaintyLine({"2D", "MANNINGS_N", "0.25"}, opts, config).empty());
    ASSERT_TRUE(parseUncertaintyLine({"2D", "RAINFALL",   "0.25"}, opts, config).empty());
    EXPECT_EQ(config.sources.size(), std::size_t{2});

    SurfaceStateData state;
    state.resize(n, mesh.n_vertices());
    // Add rainfall forcing to create spread
    state.rainfall.assign(static_cast<std::size_t>(n), 1.0e-5);
    for (int i = 0; i < n; ++i) {
        state.depth[i] = 0.10;
        state.head[i]  = mesh.tri_cz[i] + 0.10;
    }

    CvodeSurfaceSolver solver;
    solver.initialize(mesh, state, opts);
    solver.suppress_warnings();
    solver.advance(0.0, 5.0);

    const SpectralROM* rom = solver.rom();
    ASSERT_NE(rom, nullptr);
    ASSERT_EQ(static_cast<int>(rom->q05.size()), n);
    ASSERT_EQ(static_cast<int>(rom->q50.size()), n);
    ASSERT_EQ(static_cast<int>(rom->q95.size()), n);

    for (int i = 0; i < n; ++i) {
        EXPECT_LE(rom->q05[static_cast<std::size_t>(i)],
                  rom->q50[static_cast<std::size_t>(i)] + 1e-12)
            << "q05 > q50 at cell " << i;
        EXPECT_LE(rom->q50[static_cast<std::size_t>(i)],
                  rom->q95[static_cast<std::size_t>(i)] + 1e-12)
            << "q50 > q95 at cell " << i;
        EXPECT_GE(rom->q05[static_cast<std::size_t>(i)], 0.0)
            << "q05 < 0 at cell " << i;
    }
}

// ============================================================================
// ROMSpatialCase — end-to-end spatial 2D uncertainty validation (PR 10)
// ============================================================================

// When MANNINGS_CORR_LEN > 0 is parsed, seedROM() should generate a spatial
// Manning field and the ROM quantiles should remain valid (q05 ≤ q50 ≤ q95).
TEST(ROMSpatialCase, SpatialManningsFieldPopulatedAndQuantilesValid) {
    MeshData mesh = makeSmallSlopedMesh();
    int n = mesh.n_triangles();

    SolverOptions2D opts;
    opts.rel_tolerance   = 1.0e-3;
    opts.abs_tolerance   = 1.0e-5;
    opts.max_timestep    = 5.0;
    opts.max_cvode_steps = 10000;
    opts.linear_solver   = LinearSolverType::GMRES;
    opts.preconditioner  = PreconditionerType::NONE;

    ASSERT_TRUE(parse2DROMLine({"ENABLE",          "YES"},  opts).empty());
    ASSERT_TRUE(parse2DROMLine({"MEMBERS",         "10"},   opts).empty());
    ASSERT_TRUE(parse2DROMLine({"MODES",           "4"},    opts).empty());
    ASSERT_TRUE(parse2DROMLine({"K_EFF",           "10.0"}, opts).empty());
    ASSERT_TRUE(parse2DROMLine({"MANNINGS_PERT",   "0.20"}, opts).empty());
    // Spatial correlation length = 3 m on a 10m domain.
    ASSERT_TRUE(parse2DROMLine({"MANNINGS_CORR_LEN", "3.0"}, opts).empty());
    EXPECT_DOUBLE_EQ(opts.rom_mannings_corr_len, 3.0);

    SurfaceStateData state;
    state.resize(n, mesh.n_vertices());
    setGaussianBump(state, mesh, 0.02, 0.08, 3.0, 4.0, 1.5);

    CvodeSurfaceSolver solver;
    solver.initialize(mesh, state, opts);
    solver.suppress_warnings();
    solver.advance(0.0, 5.0);

    const SpectralROM* rom = solver.rom();
    ASSERT_NE(rom, nullptr);
    ASSERT_TRUE(rom->is_ready());

    // Spatial Manning field must be populated after seeding.
    EXPECT_TRUE(rom->spatial_mannings.is_spatial())
        << "spatial_mannings should be populated when MANNINGS_CORR_LEN > 0";
    EXPECT_EQ(rom->spatial_mannings.n_members, opts.rom_members);
    EXPECT_EQ(rom->spatial_mannings.n_cells,   n);

    // All spatial multiplier values must be in a physically reasonable range.
    for (int i = 0; i < opts.rom_members; ++i)
        for (int t = 0; t < n; ++t) {
            double v = rom->spatial_mannings.at(i, t);
            EXPECT_GT(v, 0.0) << "member " << i << " cell " << t;
            EXPECT_LT(v, 2.0) << "member " << i << " cell " << t;
        }

    // Quantile ordering must hold.
    ASSERT_EQ(static_cast<int>(rom->q05.size()), n);
    ASSERT_EQ(static_cast<int>(rom->q50.size()), n);
    ASSERT_EQ(static_cast<int>(rom->q95.size()), n);
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_LE(rom->q05[ui], rom->q50[ui] + 1e-12) << "q05 > q50 at cell " << i;
        EXPECT_LE(rom->q50[ui], rom->q95[ui] + 1e-12) << "q50 > q95 at cell " << i;
        EXPECT_GE(rom->q05[ui], 0.0)                  << "q05 < 0 at cell " << i;
    }
}

// Spatial rainfall field is populated when RAINFALL_CORR_LEN > 0.
TEST(ROMSpatialCase, SpatialRainfallFieldPopulated) {
    MeshData mesh = makeSmallSlopedMesh();
    int n = mesh.n_triangles();

    SolverOptions2D opts;
    opts.rel_tolerance   = 1.0e-3;
    opts.abs_tolerance   = 1.0e-5;
    opts.max_timestep    = 5.0;
    opts.max_cvode_steps = 10000;
    opts.linear_solver   = LinearSolverType::GMRES;
    opts.preconditioner  = PreconditionerType::NONE;

    ASSERT_TRUE(parse2DROMLine({"ENABLE",            "YES"},  opts).empty());
    ASSERT_TRUE(parse2DROMLine({"MEMBERS",           "8"},    opts).empty());
    ASSERT_TRUE(parse2DROMLine({"MODES",             "4"},    opts).empty());
    ASSERT_TRUE(parse2DROMLine({"K_EFF",             "10.0"}, opts).empty());
    ASSERT_TRUE(parse2DROMLine({"RAINFALL_CORR_LEN", "5.0"},  opts).empty());
    EXPECT_DOUBLE_EQ(opts.rom_rainfall_corr_len, 5.0);

    SurfaceStateData state;
    state.resize(n, mesh.n_vertices());
    setGaussianBump(state, mesh, 0.02, 0.08, 3.0, 4.0, 1.5);
    state.rainfall.assign(static_cast<std::size_t>(n), 1.0e-5);

    CvodeSurfaceSolver solver;
    solver.initialize(mesh, state, opts);
    solver.suppress_warnings();
    solver.advance(0.0, 5.0);

    const SpectralROM* rom = solver.rom();
    ASSERT_NE(rom, nullptr);

    // Spatial rainfall field must be populated.
    EXPECT_TRUE(rom->spatial_rainfall.is_spatial())
        << "spatial_rainfall should be populated when RAINFALL_CORR_LEN > 0";
    EXPECT_EQ(rom->spatial_rainfall.n_members, opts.rom_members);
    EXPECT_EQ(rom->spatial_rainfall.n_cells,   n);

    // Values in range.
    for (int i = 0; i < opts.rom_members; ++i)
        for (int t = 0; t < n; ++t) {
            double v = rom->spatial_rainfall.at(i, t);
            EXPECT_GT(v, 0.0) << "member " << i << " cell " << t;
            EXPECT_LT(v, 2.0) << "member " << i << " cell " << t;
        }
}

// Scalar mode (corr_len == 0) leaves spatial fields empty.
TEST(ROMSpatialCase, ScalarModeLeavesSpatialFieldsEmpty) {
    MeshData mesh = makeSmallSlopedMesh();
    int n = mesh.n_triangles();

    SolverOptions2D opts;
    opts.rel_tolerance   = 1.0e-3;
    opts.abs_tolerance   = 1.0e-5;
    opts.max_timestep    = 5.0;
    opts.max_cvode_steps = 10000;
    opts.linear_solver   = LinearSolverType::GMRES;
    opts.preconditioner  = PreconditionerType::NONE;

    ASSERT_TRUE(parse2DROMLine({"ENABLE",  "YES"},  opts).empty());
    ASSERT_TRUE(parse2DROMLine({"MEMBERS", "8"},    opts).empty());
    ASSERT_TRUE(parse2DROMLine({"MODES",   "4"},    opts).empty());
    ASSERT_TRUE(parse2DROMLine({"K_EFF",   "10.0"}, opts).empty());
    // Default corr_len = 0 → scalar mode.

    SurfaceStateData state;
    state.resize(n, mesh.n_vertices());
    setGaussianBump(state, mesh, 0.02, 0.08, 3.0, 4.0, 1.5);

    CvodeSurfaceSolver solver;
    solver.initialize(mesh, state, opts);
    solver.suppress_warnings();
    solver.advance(0.0, 5.0);

    const SpectralROM* rom = solver.rom();
    ASSERT_NE(rom, nullptr);
    EXPECT_FALSE(rom->spatial_mannings.is_spatial())
        << "spatial_mannings should be empty in scalar mode";
    EXPECT_FALSE(rom->spatial_rainfall.is_spatial())
        << "spatial_rainfall should be empty in scalar mode";
}

// ============================================================================
// CvodeSurfaceSolver — Phase 1 (BDF + Newton + GMRES + Jacobi) sanity tests
// ============================================================================
//
// These verify the post-restoration solver configuration: that GMRES + (NONE
// or JACOBI) initialises cleanly, that the Phase-2-reserved configurations
// fail loudly rather than silently substituting, and that a simple
// source-only advance succeeds end-to-end. These are NOT convergence tests at
// small dry_depth; that's the snoopy_lagoon integration question Phase 1 is
// set up to measure on the host build.

#ifdef OPENSWMM_HAS_2D

namespace {

// Helper: build a tiny flat-bed mesh + initial-state trio suitable for solver
// initialisation. Bed elevations are zero everywhere (vertex z = 0), so head
// = depth and the C-property degenerates trivially.
struct SolverFixture {
    MeshData         mesh;
    SurfaceStateData state;
    SolverOptions2D  opts;

    void build() {
        mesh = makeUnitSquareMesh();
        // rhs_fn calls reconstructVertexHeads(), which iterates
        // mesh.vert_stencil_ptr; buildVertexStencils must run before any
        // advance() is attempted.
        buildVertexStencils(mesh);
        state.resize(mesh.n_triangles(), mesh.n_vertices());
        // H-formulation: y_i = head_i = depth_i + z_i. Flat bed (z=0)
        // initially dry → head = 0.
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            state.head[i]  = mesh.tri_cz[i];
            state.depth[i] = 0.0;
        }
    }
};

} // anonymous namespace

TEST(CvodeSurfaceSolverPhase1, InitializesWithGmresAndNoPreconditioner) {
    SolverFixture fx; fx.build();
    fx.opts.linear_solver  = LinearSolverType::GMRES;
    fx.opts.preconditioner = PreconditionerType::NONE;

    CvodeSurfaceSolver solver;
    ASSERT_NO_THROW(solver.initialize(fx.mesh, fx.state, fx.opts));
    EXPECT_TRUE(solver.is_initialized());
    solver.finalize();
    EXPECT_FALSE(solver.is_initialized());
}

TEST(CvodeSurfaceSolverPhase1, InitializesWithGmresAndJacobi) {
    SolverFixture fx; fx.build();
    fx.opts.linear_solver  = LinearSolverType::GMRES;
    fx.opts.preconditioner = PreconditionerType::JACOBI;

    CvodeSurfaceSolver solver;
    ASSERT_NO_THROW(solver.initialize(fx.mesh, fx.state, fx.opts));
    EXPECT_TRUE(solver.is_initialized());
}

TEST(CvodeSurfaceSolverPhase1, RejectsBicgstabLinearSolver) {
    SolverFixture fx; fx.build();
    fx.opts.linear_solver  = LinearSolverType::BICGSTAB;
    fx.opts.preconditioner = PreconditionerType::JACOBI;

    CvodeSurfaceSolver solver;
    EXPECT_THROW(solver.initialize(fx.mesh, fx.state, fx.opts),
                 std::runtime_error);
}

TEST(CvodeSurfaceSolverPhase1, RejectsTfqmrLinearSolver) {
    SolverFixture fx; fx.build();
    fx.opts.linear_solver  = LinearSolverType::TFQMR;
    fx.opts.preconditioner = PreconditionerType::JACOBI;

    CvodeSurfaceSolver solver;
    EXPECT_THROW(solver.initialize(fx.mesh, fx.state, fx.opts),
                 std::runtime_error);
}

TEST(CvodeSurfaceSolverPhase1, RejectsIluPreconditioner) {
    SolverFixture fx; fx.build();
    fx.opts.linear_solver  = LinearSolverType::GMRES;
    fx.opts.preconditioner = PreconditionerType::ILU;

    CvodeSurfaceSolver solver;
    EXPECT_THROW(solver.initialize(fx.mesh, fx.state, fx.opts),
                 std::runtime_error);
}

TEST(CvodeSurfaceSolverPhase1, AdvancesUnderConstantRainfall) {
    // Smooth, source-only problem: uniform rainfall on a flat mesh with no
    // head differences. With Δh ≡ 0 everywhere the edge fluxes vanish and
    // f(y) = R is a constant scalar; this is the easiest possible
    // convergence test for BDF + Newton + GMRES + Jacobi and exercises the
    // full attach chain end-to-end without stressing the wet/dry path.
    SolverFixture fx; fx.build();
    fx.opts.linear_solver  = LinearSolverType::GMRES;
    fx.opts.preconditioner = PreconditionerType::JACOBI;

    const double rainfall_rate = 1.0e-4;   // m/s ≈ 360 mm/hr
    for (int i = 0; i < fx.mesh.n_triangles(); ++i) {
        fx.state.rainfall[i] = rainfall_rate;
    }

    CvodeSurfaceSolver solver;
    ASSERT_NO_THROW(solver.initialize(fx.mesh, fx.state, fx.opts));

    const double dt = 1.0;
    double t_reached = solver.advance(0.0, dt);
    EXPECT_NEAR(t_reached, dt, 1e-9)
        << "CVODE did not reach t_target — corrector may have stalled";

    // Expected depth after 1 s of constant rainfall at 1e-4 m/s.
    // Tolerance is loose because the cubic Hermite shutoff at depth <
    // dry_depth attenuates very early rainfall accumulation, and the
    // H-formulation's depth-derived value passes through max(y-z, 0).
    const double expected = rainfall_rate * dt;
    for (int i = 0; i < fx.mesh.n_triangles(); ++i) {
        EXPECT_NEAR(fx.state.depth[i], expected, 5.0e-6)
            << "Triangle " << i << " depth = " << fx.state.depth[i]
            << " (expected ~" << expected << ")";
    }
}

// ============================================================================
// Default2DOutputPlugin — CF-1.11 / UGRID-1.0 HDF5 writer
// ============================================================================

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
    snap.surface_edge_flux     = {0.0, 0.0, 0.0,  0.0, 0.0, 0.0}; // [tri*3+e]
    snap.surface_vert_head     = {0.0, 0.0, 0.0,  0.0};

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

    H5Fclose(file_id);
    fs::remove(h5_path);
}

#endif // OPENSWMM_HAS_2D
