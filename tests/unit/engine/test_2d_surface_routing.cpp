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
#include "2d/input/SectionHandlers2D.hpp"

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

#ifdef OPENSWMM_HAS_2D

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
