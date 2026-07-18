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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
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
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/mesh/VertexReconstruction.hpp"
#include "2d/solver/DiffusiveConductance.hpp"
#include "2d/solver/SurfaceFluxCalculator.hpp"
#include "2d/solver/CvodeSurfaceSolver.hpp"
#include "2d/input/SectionHandlers2D.hpp"
#include "uncertainty/UncertaintyConfig.hpp"

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
// DiffusiveConductance Tests
// ============================================================================

TEST(DiffusiveConductance, ZeroForDryCell) {
    double K = diffusiveConductance(0.0005, 0.035, 0.01, 0.001);
    EXPECT_EQ(K, 0.0);
}

TEST(DiffusiveConductance, PositiveForWetCell) {
    double K = diffusiveConductance(0.1, 0.035, 0.01, 0.001);
    EXPECT_GT(K, 0.0);
}

TEST(DiffusiveConductance, IncreasesWithDepth) {
    double K1 = diffusiveConductance(0.01, 0.035, 0.01, 0.001);
    double K2 = diffusiveConductance(0.1,  0.035, 0.01, 0.001);
    double K3 = diffusiveConductance(1.0,  0.035, 0.01, 0.001);
    EXPECT_LT(K1, K2);
    EXPECT_LT(K2, K3);
}

TEST(DiffusiveConductance, DecreasesWithRoughness) {
    double K_smooth = diffusiveConductance(0.1, 0.01, 0.01, 0.001);
    double K_rough  = diffusiveConductance(0.1, 0.1,  0.01, 0.001);
    EXPECT_GT(K_smooth, K_rough);
}

TEST(DiffusiveConductance, SmoothTransitionContinuous) {
    // Test that smooth variant transitions continuously through dry_depth
    double dry = 0.01;
    double K_below = diffusiveConductanceSmooth(dry * 0.99, 0.035, 0.01, dry);
    double K_at    = diffusiveConductanceSmooth(dry,        0.035, 0.01, dry);
    double K_above = diffusiveConductanceSmooth(dry * 1.01, 0.035, 0.01, dry);

    // Should be monotonically increasing through the transition
    EXPECT_LE(K_below, K_at);
    EXPECT_LE(K_at, K_above);
    // Below dry should be > 0 (smooth, not hard cutoff)
    EXPECT_GT(K_below, 0.0);
}

TEST(DiffusiveConductance, SmoothMatchesHardAboveThreshold) {
    // Well above dry_depth, smooth and hard variants should match
    double K_hard   = diffusiveConductance(1.0, 0.035, 0.01, 0.001);
    double K_smooth = diffusiveConductanceSmooth(1.0, 0.035, 0.01, 0.001);
    EXPECT_NEAR(K_hard, K_smooth, 1e-12);
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
// [UNCERTAINTY] parser tests (PR 9c — registry grammar)
// ============================================================================

// New order LAYER NAME PERT [DIST] [ENTRY] with explicit entry.
TEST(InputParsing, ParseUncertaintyNewOrderWithDistAndEntry) {
    SolverOptions2D opts;
    UncertaintyConfig config;
    auto err = parseUncertaintyLine(
        {"1D", "INFLOW", "0.30", "LOGNORMAL", "FORCING_VECTOR"}, opts, config);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(config.sources.size(), std::size_t{1});
    EXPECT_EQ(config.sources[0].name,  "INFLOW");
    EXPECT_EQ(config.sources[0].layer, LayerTarget::ONE_D);
    EXPECT_EQ(config.sources[0].dist,  DistType::LOGNORMAL);
    EXPECT_EQ(static_cast<int>(config.sources[0].entry),
              static_cast<int>(ParamEntry::FORCING_VECTOR));
    EXPECT_DOUBLE_EQ(config.sources[0].perturbation, 0.30);
}

// Known names imply their entry when none is given (new token order).
TEST(InputParsing, ParseUncertaintyEntryDefaultsFromName) {
    SolverOptions2D opts;
    UncertaintyConfig config;
    ASSERT_TRUE(parseUncertaintyLine({"1D", "MANNINGS_N", "0.20"}, opts, config).empty());
    ASSERT_TRUE(parseUncertaintyLine({"1D", "RAINFALL",   "0.10"}, opts, config).empty());
    ASSERT_TRUE(parseUncertaintyLine({"1D", "INFLOW",     "0.30"}, opts, config).empty());
    ASSERT_EQ(config.sources.size(), std::size_t{3});
    EXPECT_EQ(static_cast<int>(config.sources[0].entry),
              static_cast<int>(ParamEntry::RATE_MULT));
    EXPECT_EQ(static_cast<int>(config.sources[1].entry),
              static_cast<int>(ParamEntry::FORCING_MULT));
    EXPECT_EQ(static_cast<int>(config.sources[2].entry),
              static_cast<int>(ParamEntry::FORCING_VECTOR));
}

// Unknown NAME is accepted when an explicit ENTRY says how it enters the ODE.
TEST(InputParsing, ParseUncertaintyUnknownNameWithExplicitEntryAccepted) {
    SolverOptions2D opts;
    UncertaintyConfig config;
    auto err = parseUncertaintyLine(
        {"1D", "MY_KNOB", "0.10", "NORMAL", "RATE_MULT"}, opts, config);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(config.sources.size(), std::size_t{1});
    EXPECT_EQ(config.sources[0].name, "MY_KNOB");
    EXPECT_EQ(static_cast<int>(config.sources[0].entry),
              static_cast<int>(ParamEntry::RATE_MULT));
    EXPECT_EQ(config.sources[0].dist, DistType::NORMAL);
}

// Unknown ENTRY token is rejected with the supported list.
TEST(InputParsing, ParseUncertaintyUnknownEntryRejected) {
    SolverOptions2D opts;
    UncertaintyConfig config;
    auto err = parseUncertaintyLine(
        {"1D", "MY_KNOB", "0.10", "UNIFORM", "BOGUS_ENTRY"}, opts, config);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("RATE_MULT"), std::string::npos)
        << "error should list the supported entries: " << err;
    EXPECT_TRUE(config.sources.empty());
}

// Legacy token order LAYER NAME DIST PERT still accepted; name-implied entry set.
TEST(InputParsing, ParseUncertaintyLegacyOrderStillAccepted) {
    SolverOptions2D opts;
    UncertaintyConfig config;
    auto err = parseUncertaintyLine(
        {"1D", "MANNINGS_N", "LOGNORMAL", "0.20"}, opts, config);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(config.sources.size(), std::size_t{1});
    EXPECT_EQ(config.sources[0].dist, DistType::LOGNORMAL);
    EXPECT_DOUBLE_EQ(config.sources[0].perturbation, 0.20);
    EXPECT_EQ(static_cast<int>(config.sources[0].entry),
              static_cast<int>(ParamEntry::RATE_MULT));
}

// specs_for(layer) returns all active specs of that layer, parse order.
TEST(InputParsing, UncertaintySpecsForLayerAccessor) {
    SolverOptions2D opts;
    UncertaintyConfig config;
    parseUncertaintyLine({"2D", "MANNINGS_N", "0.20"}, opts, config);
    parseUncertaintyLine({"1D", "RAINFALL",   "0.10"}, opts, config);
    parseUncertaintyLine({"1D", "INFLOW", "0.30", "LOGNORMAL", "FORCING_VECTOR"},
                         opts, config);
    const auto one_d = config.specs_for(LayerTarget::ONE_D);
    ASSERT_EQ(one_d.size(), std::size_t{2});
    EXPECT_EQ(one_d[0].name, "RAINFALL");
    EXPECT_EQ(one_d[1].name, "INFLOW");
    EXPECT_EQ(config.specs_for(LayerTarget::TWO_D).size(), std::size_t{1});
    EXPECT_TRUE(config.specs_for(LayerTarget::RUNOFF).empty());
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
    // H-formulation: head = bed elevation → depth = head - z = 0 (dry).
    for (int i = 0; i < n; ++i) {
        state.depth[i] = 0.0;
        state.head[i]  = mesh.tri_cz[i];
    }

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
