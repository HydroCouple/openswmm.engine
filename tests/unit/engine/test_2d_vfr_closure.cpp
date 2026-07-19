/**
 * @file test_2d_vfr_closure.cpp
 * @brief Unit tests for the VFR (volume–free-surface) closure and the
 *        VFR_FACE edge reconstruction (Begnudelli & Sanders 2006/2007).
 *
 * @details Verifies:
 *          - Exact + regularized closure math (round-trip, monotonicity,
 *            C¹ joins, derivative bound, degenerate cells, dry anchor)
 *          - cellFreeSurfaceElevation delegation (render == closure, eps=0)
 *          - Flat-closure bias direction (η_flat ≥ η_VFR — the uphill driver)
 *          - Lake-at-rest C-property at shorelines: zero edge fluxes under
 *            CELL_CLOSURE=VFR where FLAT produces spurious head differences
 *          - VFR_FACE wetting gate: no flux across an edge whose bed sits
 *            above the upwind free surface; Eq. 14 depth when partially
 *            submerged
 *          - [2D_OPTIONS] CELL_CLOSURE / FACE_RECONSTRUCTION /
 *            VFR_MIN_WET_FRAC parse + format round-trip
 *
 * @see src/engine/2d/mesh/VfrClosure.hpp
 * @see plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md
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

#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/mesh/VertexReconstruction.hpp"
#include "2d/mesh/VfrClosure.hpp"
#include "2d/solver/SurfaceFluxCalculator.hpp"
#include "2d/input/SectionHandlers2D.hpp"

using namespace openswmm::twoD;

namespace {
constexpr double kEps = 0.01;   // default VFR_MIN_WET_FRAC

// z = 0.1*x + 0.2*y tilted plane, 2 triangles (same as the gradient tests).
// T0 vertex z: {0, 1, 3}; T1 vertex z: {0, 3, 2}.
MeshData makeTiltedPlaneMesh() {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 10.0, 0.0, 10.0};
    mesh.vy = {0.0, 0.0, 10.0, 10.0};
    mesh.vz = {0.0, 1.0, 2.0, 3.0};
    mesh.resize_triangles(2);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 3;
    mesh.tri_v0[1] = 0; mesh.tri_v1[1] = 3; mesh.tri_v2[1] = 2;
    mesh.mannings_n[0] = 0.03;
    mesh.mannings_n[1] = 0.03;
    buildMeshTopology(mesh);
    return mesh;
}

// Sorted vertex elevations of cell i.
void cellZ(const MeshData& m, int i, double& z1, double& z2, double& z3) {
    z1 = m.vz[m.tri_v0[i]];
    z2 = m.vz[m.tri_v1[i]];
    z3 = m.vz[m.tri_v2[i]];
    vfrSort3(z1, z2, z3);
}
} // namespace

// ============================================================================
// Closure math
// ============================================================================

TEST(VfrClosure, RoundTripExactAndRegularized) {
    const double z1 = 0.0, z2 = 0.4, z3 = 1.0;
    for (double eta = 0.001; eta <= 1.6; eta += 0.0013) {
        const double h_reg = vfrMeanDepthFromEta(z1, z2, z3, eta, kEps);
        if (h_reg > 0.0)
            EXPECT_NEAR(vfrEtaFromMeanDepth(z1, z2, z3, h_reg, kEps), eta, 1e-9);
        const double h_ex = vfrMeanDepthFromEtaExact(z1, z2, z3, eta);
        if (h_ex > 1e-15)
            EXPECT_NEAR(vfrEtaFromMeanDepth(z1, z2, z3, h_ex, 0.0), eta, 1e-9);
    }
}

TEST(VfrClosure, MonotoneWithBoundedSlope) {
    const double z1 = 0.0, z2 = 0.4, z3 = 1.0;
    double prev = -1.0e30;
    for (double h = 0.0; h <= 0.8; h += 1e-4) {
        const double e = vfrEtaFromMeanDepth(z1, z2, z3, h, kEps);
        EXPECT_GT(e, prev);
        prev = e;
        const double slope =
            (vfrEtaFromMeanDepth(z1, z2, z3, h + 1e-7, kEps) - e) / 1e-7;
        EXPECT_GT(slope, 0.0);
        EXPECT_LE(slope, 1.0 / kEps + 1.0);   // dη/dh̄ ≤ 1/ε (the CVODE bound)
    }
}

TEST(VfrClosure, C1AtBranchJoins) {
    const double z1 = 0.0, z2 = 0.4, z3 = 1.0;
    const double zbar = (z1 + z2 + z3) / 3.0;
    const double eta_s = vfrStageAtWetFraction(z1, z2, z3, kEps);
    const double joins[3] = {
        vfrMeanDepthFromEtaExact(z1, z2, z3, eta_s),   // ε-tail switch
        vfrMeanDepthFromEtaExact(z1, z2, z3, z2),      // lower→upper branch
        z3 - zbar                                       // upper→fully-wet
    };
    for (const double hj : joins) {
        const double d  = 1e-9;
        const double sl = (vfrEtaFromMeanDepth(z1, z2, z3, hj, kEps)
                           - vfrEtaFromMeanDepth(z1, z2, z3, hj - d, kEps)) / d;
        const double sr = (vfrEtaFromMeanDepth(z1, z2, z3, hj + d, kEps)
                           - vfrEtaFromMeanDepth(z1, z2, z3, hj, kEps)) / d;
        EXPECT_NEAR(sl, sr, 1e-3 * std::max(sl, sr));
    }
}

TEST(VfrClosure, AnalyticDerivativeMatchesFiniteDifference) {
    const double z1 = 0.0, z2 = 0.4, z3 = 1.0;
    for (double h = 1e-5; h <= 0.8; h += 3e-4) {
        const double e  = vfrEtaFromMeanDepth(z1, z2, z3, h, kEps);
        const double fd = (vfrEtaFromMeanDepth(z1, z2, z3, h + 1e-8, kEps)
                           - vfrEtaFromMeanDepth(z1, z2, z3, h - 1e-8, kEps))
                          / 2e-8;
        const double an = vfrDEtaDMeanDepth(z1, z2, z3, e, kEps);
        EXPECT_NEAR(fd, an, 0.02 * an + 1e-6);
    }
}

TEST(VfrClosure, FlatClosureOverestimatesEta) {
    // η_flat = z̄ + h̄ ≥ η_VFR with equality only fully wet — the sign of the
    // bias that drives thin films uphill under the legacy closure.
    const double z1 = 0.0, z2 = 0.4, z3 = 1.0;
    const double zbar = (z1 + z2 + z3) / 3.0;
    for (double h = 1e-4; h < 1.2; h += 1e-3) {
        const double e = vfrEtaFromMeanDepth(z1, z2, z3, h, 0.0);
        EXPECT_LE(e, zbar + h + 1e-12);
        if (h >= z3 - zbar) EXPECT_NEAR(e, zbar + h, 1e-12);
    }
}

TEST(VfrClosure, DryAnchorRoundTripsToZeroVolume) {
    const double z1 = 0.0, z2 = 0.4, z3 = 1.0;
    const double ed = vfrDryEta(z1, z2, z3, kEps);
    EXPECT_GT(ed, z1);
    EXPECT_LT(ed, (z1 + z2 + z3) / 3.0);
    // Seeding a dry cell's head at the anchor must map back to exactly V = 0
    // (solver initialize / reinitialize round-trip).
    EXPECT_EQ(vfrMeanDepthFromEta(z1, z2, z3, ed, kEps), 0.0);
}

TEST(VfrClosure, DegenerateCells) {
    // Flat cell → flat closure exactly.
    EXPECT_NEAR(vfrEtaFromMeanDepth(2.0, 2.0, 2.0, 0.3, kEps), 2.3, 1e-12);
    // Two equal low / two equal high vertices: finite, round-trips.
    for (double h : {0.01, 0.05, 0.2}) {
        const double ea = vfrEtaFromMeanDepth(0.0, 0.0, 1.0, h, kEps);
        EXPECT_NEAR(vfrMeanDepthFromEta(0.0, 0.0, 1.0, ea, kEps), h, 1e-9);
        const double eb = vfrEtaFromMeanDepth(0.0, 1.0, 1.0, h, kEps);
        EXPECT_NEAR(vfrMeanDepthFromEta(0.0, 1.0, 1.0, eb, kEps), h, 1e-9);
    }
    // 1-ulp z2≈z3 sliver must not divide by zero.
    EXPECT_TRUE(std::isfinite(
        vfrEtaFromMeanDepth(0.0, 1.0, 1.0 + 1e-13, 0.2, 0.0)));
}

TEST(VfrClosure, RenderClosureDelegates) {
    // cellFreeSurfaceElevation must be the eps = 0 closure (single source of
    // truth for render + solver). Spot-check both branches + dry + flat.
    EXPECT_NEAR(cellFreeSurfaceElevation(0.02, 1.0, 0.0, 3.0),
                vfrEtaFromMeanDepth(0.0, 1.0, 3.0, 0.02, 0.0), 1e-12);
    EXPECT_NEAR(cellFreeSurfaceElevation(0.9, 1.0, 0.0, 3.0),
                vfrEtaFromMeanDepth(0.0, 1.0, 3.0, 0.9, 0.0), 1e-12);
    EXPECT_EQ(cellFreeSurfaceElevation(0.0, 1.0, 0.0, 3.0), 0.0);   // dry → z1
    EXPECT_NEAR(cellFreeSurfaceElevation(0.5, 2.0, 2.0, 2.0), 2.5, 1e-12);
}

// ============================================================================
// Lake-at-rest C-property at a shoreline (the acceptance test for RC-S1)
// ============================================================================

TEST(VfrClosure, LakeAtRestHasZeroFluxesUnderVfr) {
    auto mesh = makeTiltedPlaneMesh();
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    SolverOptions2D opts;
    opts.cell_closure     = CellClosure2D::VFR;
    opts.vfr_min_wet_frac = kEps;

    // Physical still-water state: every cell holds the exact planar-bed
    // storage of the SAME free surface η0 (both cells partially wet).
    const double eta0 = 2.0;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        double z1, z2, z3;
        cellZ(mesh, i, z1, z2, z3);
        const double h = vfrMeanDepthFromEtaExact(z1, z2, z3, eta0);
        state.volume[i] = h * mesh.tri_area[i];
        state.depth[i]  = h;
        // Solver unpack under VFR: η from the regularized closure.
        state.head[i] = vfrEtaFromMeanDepth(z1, z2, z3, h, kEps);
        EXPECT_NEAR(state.head[i], eta0, 1e-9);   // closure recovers η0
    }
    computeEdgeFluxes(mesh, state, opts);
    // Heads agree to the closure's Newton tolerance (~1e-12 m); with the
    // regSqrt-linearized flux that bounds any residual flux ~1e-8 m³/s —
    // seven orders below the ~13 m³/s the FLAT closure produces on the same
    // state (companion test below).
    for (int idx = 0; idx < mesh.n_triangles() * 3; ++idx)
        EXPECT_NEAR(state.edge_flux[idx], 0.0, 1e-6)
            << "lake at rest must be a steady state under VFR (idx " << idx << ")";
}

TEST(VfrClosure, LakeAtRestFluxesNonzeroUnderFlat) {
    // Characterization of the legacy defect: the SAME physical state under the
    // FLAT closure reports unequal heads → spurious shoreline fluxes. This is
    // the mechanism that tilts the true surface into the hill.
    auto mesh = makeTiltedPlaneMesh();
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    SolverOptions2D opts;   // defaults: FLAT / MEAN

    const double eta0 = 2.0;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        double z1, z2, z3;
        cellZ(mesh, i, z1, z2, z3);
        const double h = vfrMeanDepthFromEtaExact(z1, z2, z3, eta0);
        state.volume[i] = h * mesh.tri_area[i];
        state.depth[i]  = h;
        state.head[i]   = mesh.tri_cz[i] + h;   // flat-closure unpack
    }
    EXPECT_GT(std::abs(state.head[0] - state.head[1]), 1e-3)
        << "flat closure should misreport partially-wet heads";
    computeEdgeFluxes(mesh, state, opts);
    double max_flux = 0.0;
    for (int idx = 0; idx < mesh.n_triangles() * 3; ++idx)
        max_flux = std::max(max_flux, std::abs(state.edge_flux[idx]));
    EXPECT_GT(max_flux, 1e-3)
        << "expected the legacy closure to produce spurious still-water flux";
}

// ============================================================================
// VFR_FACE wetting gate (B&S 2007 Eq. 14)
// ============================================================================

namespace {
// Two triangles whose SHARED edge is elevated: v1(z=1) — v2(z=2).
//   T0 = {v0(0), v1(1), v2(2)}   T1 = {v1(1), v3(2), v2(2)}
MeshData makeRaisedSharedEdgeMesh() {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 1.0, 0.0, 1.0};
    mesh.vy = {0.0, 0.0, 1.0, 1.0};
    mesh.vz = {0.0, 1.0, 2.0, 2.0};
    mesh.resize_triangles(2);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 2;
    mesh.tri_v0[1] = 1; mesh.tri_v1[1] = 3; mesh.tri_v2[1] = 2;
    mesh.mannings_n[0] = 0.03;
    mesh.mannings_n[1] = 0.03;
    buildMeshTopology(mesh);
    return mesh;
}

// The interior-edge flux magnitude from T0's slots.
double interiorFluxMagnitude(const MeshData& mesh, const SurfaceStateData& state) {
    const int nbr[3] = {mesh.tri_nbr0[0], mesh.tri_nbr1[0], mesh.tri_nbr2[0]};
    for (int e = 0; e < 3; ++e)
        if (nbr[e] == 1) return std::abs(state.edge_flux[0 * 3 + e]);
    ADD_FAILURE() << "no interior edge found";
    return 0.0;
}
} // namespace

TEST(VfrFaceGate, NoFluxWhenUpwindSurfaceBelowEdgeBed) {
    auto mesh = makeRaisedSharedEdgeMesh();
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // Upwind cell T0: water pooled below the shared edge (η = 0.9 < z_lo = 1).
    state.head[0] = 0.9;  state.depth[0] = 0.3;
    state.head[1] = 0.5;  state.depth[1] = 0.1;

    SolverOptions2D opts;
    opts.face_reconstruction = FaceDepth2D::MEAN;
    computeEdgeFluxes(mesh, state, opts);
    EXPECT_GT(interiorFluxMagnitude(mesh, state), 1e-6)
        << "MEAN (legacy) lets pooled water cross the raised edge";

    opts.face_reconstruction = FaceDepth2D::VFR_FACE;
    computeEdgeFluxes(mesh, state, opts);
    EXPECT_EQ(interiorFluxMagnitude(mesh, state), 0.0)
        << "VFR_FACE must gate flux when the upwind surface is below the edge";
}

TEST(VfrFaceGate, PartiallySubmergedEdgeUsesEq14Depth) {
    auto mesh = makeRaisedSharedEdgeMesh();
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());

    // Upwind surface midway up the shared edge: η = 1.5, edge z = [1, 2].
    // Eq. 14: d_face = (1.5 − 1)² / (2·(2 − 1)) = 0.125.
    state.head[0] = 1.5;  state.depth[0] = 0.5;
    state.head[1] = 0.5;  state.depth[1] = 0.1;

    SolverOptions2D opts;
    opts.face_reconstruction = FaceDepth2D::MEAN;
    computeEdgeFluxes(mesh, state, opts);
    const double f_mean = interiorFluxMagnitude(mesh, state);

    opts.face_reconstruction = FaceDepth2D::VFR_FACE;
    computeEdgeFluxes(mesh, state, opts);
    const double f_vfr = interiorFluxMagnitude(mesh, state);

    // Flux scales with the conveyance depth^(5/3): expect the exact ratio.
    const double expected = std::pow(0.125 / 0.5, 5.0 / 3.0);
    EXPECT_GT(f_vfr, 0.0);
    EXPECT_NEAR(f_vfr / f_mean, expected, 1e-9);
}

TEST(VfrFaceGate, InteriorFluxStaysAntisymmetric) {
    // Both incident cells must store the same magnitude for the shared edge
    // (redundant per-cell slots) — conservation under the gate.
    auto mesh = makeRaisedSharedEdgeMesh();
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    state.head[0] = 1.5;  state.depth[0] = 0.5;
    state.head[1] = 0.5;  state.depth[1] = 0.1;

    SolverOptions2D opts;
    opts.face_reconstruction = FaceDepth2D::VFR_FACE;
    computeEdgeFluxes(mesh, state, opts);

    double f0 = 0.0, f1 = 0.0;
    const int nbr0[3] = {mesh.tri_nbr0[0], mesh.tri_nbr1[0], mesh.tri_nbr2[0]};
    const int nbr1[3] = {mesh.tri_nbr0[1], mesh.tri_nbr1[1], mesh.tri_nbr2[1]};
    for (int e = 0; e < 3; ++e) {
        if (nbr0[e] == 1) f0 = state.edge_flux[0 * 3 + e];
        if (nbr1[e] == 0) f1 = state.edge_flux[1 * 3 + e];
    }
    EXPECT_NEAR(f0, -f1, 1e-15);
}

// ============================================================================
// [2D_OPTIONS] parse / format round-trip
// ============================================================================

TEST(VfrOptions, ParseAndFormatRoundTrip) {
    SolverOptions2D opts;
    EXPECT_EQ(opts.cell_closure, CellClosure2D::VFR);           // default (Phase 5)
    EXPECT_EQ(opts.face_reconstruction, FaceDepth2D::VFR_FACE); // default (Phase 5)

    // Legacy FLAT/MEAN still parse (kept selectable for A/B).
    EXPECT_TRUE(parse2DOptionsLine({"CELL_CLOSURE", "FLAT"}, opts).empty());
    EXPECT_EQ(opts.cell_closure, CellClosure2D::FLAT);
    EXPECT_TRUE(parse2DOptionsLine({"FACE_RECONSTRUCTION", "MEAN"}, opts).empty());
    EXPECT_EQ(opts.face_reconstruction, FaceDepth2D::MEAN);

    EXPECT_TRUE(parse2DOptionsLine({"CELL_CLOSURE", "VFR"}, opts).empty());
    EXPECT_EQ(opts.cell_closure, CellClosure2D::VFR);
    EXPECT_TRUE(parse2DOptionsLine({"FACE_RECONSTRUCTION", "VFR_FACE"}, opts).empty());
    EXPECT_EQ(opts.face_reconstruction, FaceDepth2D::VFR_FACE);
    EXPECT_TRUE(parse2DOptionsLine({"VFR_MIN_WET_FRAC", "0.02"}, opts).empty());
    EXPECT_NEAR(opts.vfr_min_wet_frac, 0.02, 1e-15);

    EXPECT_FALSE(parse2DOptionsLine({"CELL_CLOSURE", "BOGUS"}, opts).empty());
    EXPECT_FALSE(parse2DOptionsLine({"VFR_MIN_WET_FRAC", "0.0"}, opts).empty());
    EXPECT_FALSE(parse2DOptionsLine({"VFR_MIN_WET_FRAC", "0.9"}, opts).empty());

    EXPECT_TRUE(is2DOptionKey("CELL_CLOSURE"));
    EXPECT_TRUE(is2DOptionKey("FACE_RECONSTRUCTION"));
    EXPECT_TRUE(is2DOptionKey("VFR_MIN_WET_FRAC"));

    EXPECT_EQ(format2DOptionValue(opts, "CELL_CLOSURE"), "VFR");
    EXPECT_EQ(format2DOptionValue(opts, "FACE_RECONSTRUCTION"), "VFR_FACE");
    EXPECT_EQ(format2DOptionValue(opts, "VFR_MIN_WET_FRAC"), "0.02");
}
