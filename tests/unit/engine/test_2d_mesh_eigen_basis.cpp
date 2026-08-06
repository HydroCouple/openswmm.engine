/**
 * @file test_2d_mesh_eigen_basis.cpp
 * @brief Unit tests for MeshEigenBasis — the 2D geometric graph-Laplacian
 *        eigenbasis consumed by the surface uncertainty ROM.
 *
 * @details Three layers of assertion:
 *
 *          1. **Reference eigenpairs.** The `REFERENCE_*` arrays pin Λ (and,
 *             via a probe projection, P) on three fixtures: the 4-triangle
 *             engine-integration mesh, a 32-triangle structured mesh, and a
 *             5000-triangle structured mesh. The basis is a pure function of
 *             mesh geometry, so these are exact, reproducible constants — any
 *             change to the Laplacian assembly, the Lanczos start vector, the
 *             null-mode filter, or the retained-mode count moves them. Tolerance
 *             is 1e-10.
 *
 *             Eigenvectors are pinned as |Pᵀv| against a fixed probe vector
 *             rather than by storing 5000×10 components: this is invariant to
 *             the eigensolver's sign convention (which is not contractual)
 *             while still failing on any real rotation of the retained
 *             subspace.
 *
 *          2. **Build contract.** Success/failure codes, buffer shapes, the
 *             4-cell floor, ascending and strictly-positive eigenvalues.
 *
 *          3. **Operator-level invariants.** Orthonormality of P, and the
 *             eigenrelation ‖L·v − λ·v‖/λ checked against a Laplacian assembled
 *             independently inside this file — so a matching bug in the
 *             production assembly cannot hide behind a matching bug in the test.
 *
 *          Plus the depth-weighted rebuild: its own reference eigenvalues, the
 *          uniform-D scaling identity (eigenvectors unchanged, eigenvalues
 *          scaled by exactly D), and the guarantee that a rejected rebuild
 *          leaves the existing basis untouched.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

#include "2d/data/MeshData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/uncertainty/MeshEigenBasis.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

using namespace openswmm::twoD;

// ============================================================================
// Mesh fixtures
// ============================================================================

// 5 vertices, 4 triangles on a 3m × 3m sloped domain (z: 0.20 → 0.05 m) — the
// same mesh the engine 2D ROM integration test runs on, and the smallest mesh
// the basis accepts.
static MeshData makeSmallSlopedMesh() {
    MeshData mesh;
    mesh.resize_vertices(5);
    mesh.vx = {0.0, 3.0, 3.0, 0.0, 1.5};
    mesh.vy = {0.0, 0.0, 3.0, 3.0, 1.5};
    mesh.vz = {0.20, 0.15, 0.10, 0.15, 0.05};
    mesh.resize_triangles(4);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 4;
    mesh.tri_v0[1] = 1; mesh.tri_v1[1] = 2; mesh.tri_v2[1] = 4;
    mesh.tri_v0[2] = 2; mesh.tri_v1[2] = 3; mesh.tri_v2[2] = 4;
    mesh.tri_v0[3] = 3; mesh.tri_v1[3] = 0; mesh.tri_v2[3] = 4;
    buildMeshTopology(mesh);
    return mesh;
}

// 2-triangle unit-square mesh — below the 4-cell floor.
static MeshData makeUnitSquareMesh() {
    MeshData mesh;
    mesh.resize_vertices(4);
    mesh.vx = {0.0, 1.0, 0.0, 1.0};
    mesh.vy = {0.0, 0.0, 1.0, 1.0};
    mesh.vz = {0.0, 0.0, 0.0, 0.0};
    mesh.resize_triangles(2);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 3;
    mesh.tri_v0[1] = 0; mesh.tri_v1[1] = 3; mesh.tri_v2[1] = 2;
    buildMeshTopology(mesh);
    return mesh;
}

// Structured N×N triangular mesh on [0, domain_m]².
static MeshData makeStructuredMesh(int N, double domain_m = 10.0) {
    MeshData mesh;
    int nv = (N + 1) * (N + 1);
    int nt = 2 * N * N;
    mesh.resize_vertices(nv);
    mesh.resize_triangles(nt);

    double dx = domain_m / N;
    double dy = domain_m / N;

    for (int i = 0; i <= N; ++i)
        for (int j = 0; j <= N; ++j) {
            int vi = i * (N + 1) + j;
            mesh.vx[static_cast<std::size_t>(vi)] = j * dx;
            mesh.vy[static_cast<std::size_t>(vi)] = i * dy;
            mesh.vz[static_cast<std::size_t>(vi)] = 0.0;
        }

    int t = 0;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            int v00 = i * (N + 1) + j,     v01 = i * (N + 1) + j + 1;
            int v10 = (i+1) * (N + 1) + j, v11 = (i+1) * (N + 1) + j + 1;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v01; mesh.tri_v2[t] = v11; ++t;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v11; mesh.tri_v2[t] = v10; ++t;
        }

    buildMeshTopology(mesh);
    return mesh;
}

// Non-uniform per-cell diffusivity for the depth-weighted path.
static std::vector<double> makeDiffusivity(int n) {
    std::vector<double> D(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        D[static_cast<std::size_t>(i)] = 0.05 + 0.9 * ((i * 37) % 11) / 11.0;
    return D;
}

// ============================================================================
// Reference eigenpairs
// ============================================================================

static constexpr double REFERENCE_TOL = 1.0e-10;

// Deterministic probe vector, unrelated to the eigenbasis.
static std::vector<double> probeVector(int n) {
    std::vector<double> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] =
            std::sin(0.7 * i + 0.3) + 0.5 * std::cos(0.11 * i);
    return v;
}

// |P[:,q]ᵀ v| for every retained mode.
static std::vector<double> projectProbe(const MeshEigenBasis& b) {
    const int n = b.n_triangles;
    const auto v = probeVector(n);
    std::vector<double> out(static_cast<std::size_t>(b.num_kept), 0.0);
    for (int q = 0; q < b.num_kept; ++q) {
        double dot = 0.0;
        for (int i = 0; i < n; ++i)
            dot += b.P[static_cast<std::size_t>(q * n + i)]
                 * v[static_cast<std::size_t>(i)];
        out[static_cast<std::size_t>(q)] = std::fabs(dot);
    }
    return out;
}

// REFERENCE PROVENANCE — re-baselined 2026-08-05 with the Lanczos cold-start
// fix (GraphEigenBasis::lanczos(): the bare antisymmetric linear ramp could not
// reach symmetric eigenvectors on symmetric graphs; it is now ramp + symmetric
// quadratic + a tiny deterministic symmetry-breaking term). Values below that
// moved did so because they were START-VECTOR DEPENDENT, for two distinct
// reasons called out per-block. Eigen-relation, orthonormality and ascending
// -order tests are independent of these pins and pass unchanged.

// 4-triangle sloped mesh, k = 2.
//
// This mesh is 4 congruent triangles radiating from a centre vertex, so its
// cell adjacency is a 4-cycle whose Laplacian spectrum is {0, λ, λ, 2λ} —
// λ = 3 is DOUBLY DEGENERATE. Single-vector Lanczos returns exactly one
// representative of that 2-D eigenspace, and WHICH one is decided entirely by
// the start vector. The eigenvalues are therefore stable (and unchanged), but
// REFERENCE_SMALL_PROBE[0] legitimately moved: it pins an arbitrary basis
// choice inside a degenerate eigenspace, not a physical property. Kept as a
// change-detector only — a shift here means "the start vector changed", NOT
// "the basis is wrong". (`EigenrelationSatisfied` independently confirms the
// new vector still satisfies L·v = λ·v.)
static const std::vector<double> REFERENCE_SMALL_LAMBDA = {
    2.9999999999999996,
    5.9999999999999991
};
static const std::vector<double> REFERENCE_SMALL_PROBE = {
    0.036637844995388691,
    0.10589982216123994
};

// 4×4 structured mesh (32 triangles), k = 6.
static const std::vector<double> REFERENCE_S4_LAMBDA = {
    0.2276885050395718,
    0.37482065640125262,
    0.53099721838595748,
    1.1104183520635011,
    1.1396541732171597,
    1.1692553019794891
};
static const std::vector<double> REFERENCE_S4_PROBE = {
    0.80279865302121822,
    1.9680815046918414,
    2.7432351255828364,
    1.5975292632756188,
    0.40532789760016702,
    0.81883730437422986
};

// 50×50 structured mesh (5000 triangles) on a 100 m domain, k = 10.
//
// ⚠ THESE ARE NOT THE TRUE EIGENVALUES OF THIS MESH. At the production Krylov
// budget (m = min(n, max(k+1, 3k+15)) = 45 steps for k = 10) Lanczos is far
// from converged on 5000 cells: rebuilding the same mesh at k = 60 (m = 195)
// gives λ₀ = 1.476e-3, λ₂ = 4.75e-3 against the 1.922e-3 / 3.07e-2 pinned
// here — λ₂ is ~6.5× too high. Ritz values bound the true spectrum from
// ABOVE, so the k = 60 numbers are strictly closer to truth. The pre-fix
// pins (λ₀ = 1.886e-3, λ₂ = 2.33e-2) were equally unconverged — this is a
// PRE-EXISTING Krylov-budget limitation, not something the cold-start change
// introduced, and it is why these particular values are start-vector
// sensitive at all: an unconverged Ritz value still depends on where the
// Krylov space was launched from.
//
// Escalated separately (see .memory/current.md, "Krylov budget"): the fixed
// m ≈ 3k+15 heuristic is inadequate at this mesh size, and the ROM's modal
// decay rates λ_j·K_eff inherit the error. Left as a change-detector pin
// here; do NOT read these as physical eigenvalues.
static const std::vector<double> REFERENCE_S50_LAMBDA = {
    0.001922006722617993,
    0.0083345859604932199,
    0.030727045136007776,
    0.070465876085929913,
    0.12841217922570428,
    0.20483539628320788,
    0.29958006500388207,
    0.41211132447996168,
    0.54258906612696312,
    0.6897487367797317
};
static const std::vector<double> REFERENCE_S50_PROBE = {
    0.050103714238723271,
    0.07558983624123633,
    0.093646868859211807,
    0.022552605782717051,
    0.080603267035309928,
    0.074077391015603983,
    0.27801683229882385,
    0.50084909924047805,
    0.11171973482140102,
    0.024973319571514153
};

// buildDepthWeighted() on the 4×4 structured mesh with makeDiffusivity().
static const std::vector<double> REFERENCE_S4_DW_LAMBDA = {
    0.06169339529653272,
    0.11552641607327344,
    0.1733409480638759,
    0.29386736192152602,
    0.3388440697833508,
    0.35476728504404526
};

static void expectMatchesReference(const MeshEigenBasis& b,
                                   const std::vector<double>& ref_lambda,
                                   const std::vector<double>& ref_probe) {
    ASSERT_EQ(b.num_kept, static_cast<int>(ref_lambda.size()));
    for (int q = 0; q < b.num_kept; ++q)
        EXPECT_NEAR(b.eigenvalues[static_cast<std::size_t>(q)],
                    ref_lambda[static_cast<std::size_t>(q)], REFERENCE_TOL)
            << "eigenvalue " << q << " drifted from the reference basis";

    if (ref_probe.empty()) return;
    const auto p = projectProbe(b);
    ASSERT_EQ(p.size(), ref_probe.size());
    for (std::size_t q = 0; q < p.size(); ++q)
        EXPECT_NEAR(p[q], ref_probe[q], REFERENCE_TOL)
            << "eigenvector " << q << " drifted from the reference basis";
}

TEST(MeshEigenBasis, MatchesReferenceBasisSmallSlopedMesh) {
    auto mesh = makeSmallSlopedMesh();
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 2));
    EXPECT_EQ(b.n_triangles, 4);
    expectMatchesReference(b, REFERENCE_SMALL_LAMBDA, REFERENCE_SMALL_PROBE);
}

TEST(MeshEigenBasis, MatchesReferenceBasisStructured4x4) {
    auto mesh = makeStructuredMesh(4);  // 32 triangles
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 6));
    EXPECT_EQ(b.n_triangles, 32);
    expectMatchesReference(b, REFERENCE_S4_LAMBDA, REFERENCE_S4_PROBE);
}

TEST(MeshEigenBasis, MatchesReferenceBasisStructured50x50) {
    auto mesh = makeStructuredMesh(50, 100.0);  // 5000 triangles
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 10));
    EXPECT_EQ(b.n_triangles, 5000);
    expectMatchesReference(b, REFERENCE_S50_LAMBDA, REFERENCE_S50_PROBE);
}

TEST(MeshEigenBasis, MatchesReferenceDepthWeightedBasis) {
    auto mesh = makeStructuredMesh(4);
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 6));
    const auto D = makeDiffusivity(mesh.n_triangles());
    ASSERT_TRUE(b.buildDepthWeighted(mesh, 6, D.data()));
    EXPECT_TRUE(b.depth_weighted);
    expectMatchesReference(b, REFERENCE_S4_DW_LAMBDA, {});
}

// ============================================================================
// Build contract
// ============================================================================

TEST(MeshEigenBasis, BuildSucceeds) {
    auto mesh = makeStructuredMesh(4);  // 32 triangles
    MeshEigenBasis b;
    EXPECT_TRUE(b.build(mesh, 6));
    EXPECT_EQ(b.last_error, 0);
    EXPECT_TRUE(b.is_ready());
    EXPECT_GT(b.num_kept, 0);
    EXPECT_LE(b.num_kept, 6);
    EXPECT_EQ(b.n_triangles, mesh.n_triangles());
    EXPECT_EQ(static_cast<int>(b.eigenvalues.size()), b.num_kept);
    EXPECT_EQ(static_cast<int>(b.P.size()), b.n_triangles * b.num_kept);
    EXPECT_FALSE(b.depth_weighted);
}

TEST(MeshEigenBasis, BuildFailsSmallMesh) {
    // 2-triangle mesh has n_triangles = 2 < 4 → should fail
    auto mesh = makeUnitSquareMesh();
    MeshEigenBasis b;
    EXPECT_FALSE(b.build(mesh, 4));
    EXPECT_EQ(b.last_error, 1);
    EXPECT_FALSE(b.is_ready());
}

TEST(MeshEigenBasis, NullModeDiscarded) {
    // The graph Laplacian of a connected mesh has exactly one null mode (the
    // constant vector). It only enters the Krylov space on some meshes — the
    // ramp start vector is orthogonal to it by construction — so this asserts
    // the filter where it does fire, plus the invariant that holds everywhere:
    // no retained eigenvalue is below the null tolerance.
    auto mesh = makeStructuredMesh(4);
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 8));
    EXPECT_GE(b.num_null, 1);
    for (int k = 0; k < b.num_kept; ++k)
        EXPECT_GT(b.eigenvalues[static_cast<std::size_t>(k)], b.null_tol)
            << "eigenvalue " << k << " = " << b.eigenvalues[k];
}

TEST(MeshEigenBasis, EigenvaluesPositive) {
    auto mesh = makeStructuredMesh(5);  // 50 triangles
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 8));
    for (int k = 0; k < b.num_kept; ++k)
        EXPECT_GT(b.eigenvalues[static_cast<std::size_t>(k)], 0.0)
            << "eigenvalue " << k << " = " << b.eigenvalues[k];
}

TEST(MeshEigenBasis, EigenvaluesAscending) {
    auto mesh = makeStructuredMesh(5);
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 8));
    for (int k = 1; k < b.num_kept; ++k)
        EXPECT_GE(b.eigenvalues[static_cast<std::size_t>(k)],
                  b.eigenvalues[static_cast<std::size_t>(k - 1)]);
}

// ============================================================================
// Operator-level invariants
// ============================================================================

// (geometric Laplacian) × vector, assembled independently of MeshEigenBasis:
// L[i,i] = Σ w_ij;  L[i,j] = −w_ij;  w_ij = edge_length / centroid_dist
static std::vector<double> laplacianMatvec(const MeshData& mesh,
                                           const std::vector<double>& v) {
    int n = mesh.n_triangles();
    std::vector<double> y(static_cast<std::size_t>(n), 0.0);

    for (int i = 0; i < n; ++i) {
        const int nbrs[3] = {
            mesh.tri_nbr0[static_cast<std::size_t>(i)],
            mesh.tri_nbr1[static_cast<std::size_t>(i)],
            mesh.tri_nbr2[static_cast<std::size_t>(i)]
        };
        for (int e = 0; e < 3; ++e) {
            int j = nbrs[e];
            if (j < 0) continue;
            double len = mesh.edge_length[static_cast<std::size_t>(i * 3 + e)];
            double dx  = mesh.tri_cx[static_cast<std::size_t>(j)]
                       - mesh.tri_cx[static_cast<std::size_t>(i)];
            double dy  = mesh.tri_cy[static_cast<std::size_t>(j)]
                       - mesh.tri_cy[static_cast<std::size_t>(i)];
            double d   = std::sqrt(dx*dx + dy*dy);
            if (d < 1e-14) continue;
            double w = len / d;
            y[static_cast<std::size_t>(i)] += w * (v[static_cast<std::size_t>(i)]
                                                 - v[static_cast<std::size_t>(j)]);
        }
    }
    return y;
}

TEST(MeshEigenBasis, EigenvectorsOrthonormal) {
    auto mesh = makeStructuredMesh(4);  // 32 triangles
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 6));

    int n = b.n_triangles;
    int k = b.num_kept;

    for (int a = 0; a < k; ++a) {
        const double* col_a = b.P.data() + a * n;
        for (int c = 0; c < k; ++c) {
            const double* col_c = b.P.data() + c * n;
            double dot = 0.0;
            for (int i = 0; i < n; ++i) dot += col_a[i] * col_c[i];
            if (a == c)
                EXPECT_NEAR(dot, 1.0, 1e-10)
                    << "‖v_" << a << "‖² = " << dot << " (expected 1.0)";
            else
                EXPECT_NEAR(dot, 0.0, 1e-10)
                    << "v_" << a << " · v_" << c << " = " << dot << " (expected 0.0)";
        }
    }
}

TEST(MeshEigenBasis, EigenrelationSatisfied) {
    // ‖L·v_k − λ_k·v_k‖ / λ_k < 1e-6 against an independently assembled L.
    auto mesh = makeStructuredMesh(4);
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 6));

    int n = b.n_triangles;

    for (int q = 0; q < b.num_kept; ++q) {
        double lam = b.eigenvalues[static_cast<std::size_t>(q)];
        const double* col = b.P.data() + q * n;

        std::vector<double> vq(col, col + n);
        std::vector<double> Lv = laplacianMatvec(mesh, vq);

        double resid2 = 0.0, norm2 = 0.0;
        for (int i = 0; i < n; ++i) {
            double r = Lv[static_cast<std::size_t>(i)]
                     - lam * vq[static_cast<std::size_t>(i)];
            resid2 += r * r;
            norm2  += vq[static_cast<std::size_t>(i)] * vq[static_cast<std::size_t>(i)];
        }
        double rel_resid = std::sqrt(resid2 / norm2) / lam;
        EXPECT_LT(rel_resid, 1e-6)
            << "Mode " << q << ": λ=" << lam << ", rel_resid=" << rel_resid;
    }
}

// ============================================================================
// Depth-weighted rebuild
// ============================================================================

TEST(MeshEigenBasis, DepthWeightedUniformDMatchesGeometricUpToScale) {
    // A uniform D_cell scales every edge conductance identically, so the
    // eigenvectors are unchanged and the eigenvalues scale by exactly D.
    auto mesh = makeStructuredMesh(4);
    MeshEigenBasis geo;
    ASSERT_TRUE(geo.build(mesh, 6));
    const auto geo_lambda = geo.eigenvalues;
    const auto geo_probe  = projectProbe(geo);

    const double D_const = 0.25;
    std::vector<double> D(static_cast<std::size_t>(mesh.n_triangles()), D_const);

    MeshEigenBasis dw;
    ASSERT_TRUE(dw.build(mesh, 6));
    ASSERT_TRUE(dw.buildDepthWeighted(mesh, 6, D.data()));
    EXPECT_TRUE(dw.depth_weighted);
    ASSERT_EQ(dw.num_kept, geo.num_kept);

    for (int q = 0; q < dw.num_kept; ++q)
        EXPECT_NEAR(dw.eigenvalues[static_cast<std::size_t>(q)],
                    D_const * geo_lambda[static_cast<std::size_t>(q)], 1e-12);

    const auto dw_probe = projectProbe(dw);
    for (std::size_t q = 0; q < dw_probe.size(); ++q)
        EXPECT_NEAR(dw_probe[q], geo_probe[q], 1e-10);
}

TEST(MeshEigenBasis, DepthWeightedRejectsNullDAndPreservesBasis) {
    auto mesh = makeStructuredMesh(4);
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 6));
    const auto lambda_before = b.eigenvalues;
    const auto P_before      = b.P;

    EXPECT_FALSE(b.buildDepthWeighted(mesh, 6, nullptr));
    EXPECT_FALSE(b.depth_weighted);
    EXPECT_EQ(b.eigenvalues, lambda_before);
    EXPECT_EQ(b.P, P_before);
}

TEST(MeshEigenBasis, GroundedBasisCapturesUniformShift) {
    // Pure-Neumann Laplacian: the constant vector is the null mode, every
    // retained eigenvector is zero-mean, and a domain-wide uniform field
    // projects to ~nothing — so an ensemble whose dominant response is a
    // uniform profile shift carries almost no representable spread. Grounding
    // the open-boundary cells (an edge to a zero-deviation ghost) removes the
    // null mode: the lowest retained mode becomes a quasi-uniform "drain"
    // mode with a large mean, and uniform shifts become representable. This
    // is the mesh analogue of the 1D network Laplacian's outfall grounding.
    auto mesh = makeStructuredMesh(6);   // 72 triangles
    const int n = mesh.n_triangles();

    // Ground the cells along the x = 0 boundary at their face conductance.
    std::vector<double> gw(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const int nbrs[3] = {mesh.tri_nbr0[ui], mesh.tri_nbr1[ui],
                             mesh.tri_nbr2[ui]};
        for (int e = 0; e < 3; ++e) {
            const auto idx = static_cast<std::size_t>(i * 3 + e);
            if (nbrs[e] >= 0) continue;
            if (mesh.edge_mx[idx] >= 1e-9) continue;
            const double dx = mesh.edge_mx[idx] - mesh.tri_cx[ui];
            const double dy = mesh.edge_my[idx] - mesh.tri_cy[ui];
            const double d  = 2.0 * std::sqrt(dx * dx + dy * dy);
            if (d > 1e-12) gw[ui] += mesh.edge_length[idx] / d;
        }
    }
    ASSERT_GT(*std::max_element(gw.begin(), gw.end()), 0.0);

    MeshEigenBasis neumann, grounded;
    ASSERT_TRUE(neumann.build(mesh, 6));
    ASSERT_TRUE(grounded.build(mesh, 6, gw.data()));

    // Mean component of mode j: |Σ_i P[j,i]| / √n  (1.0 for the exact
    // constant vector, 0 for any zero-mean vector).
    auto meanComponent = [n](const MeshEigenBasis& b, int j) {
        double s = 0.0;
        for (int i = 0; i < n; ++i)
            s += b.P[static_cast<std::size_t>(j * n + i)];
        return std::fabs(s) / std::sqrt(static_cast<double>(n));
    };

    // Neumann modes are all orthogonal to the constant, up to the Lanczos
    // convergence residual (~1e-6 relative — same order as the eigenrelation
    // bound asserted above). The contrast with the grounded drain mode's
    // > 0.5 mean is four orders of magnitude, so the discrimination is sharp.
    for (int j = 0; j < neumann.num_kept; ++j)
        EXPECT_LT(meanComponent(neumann, j), 1e-4)
            << "Neumann mode " << j << " is not zero-mean";

    // The grounded basis's lowest mode carries the bulk of a uniform shift,
    // and its eigenvalue is genuinely positive (drains, does not persist).
    EXPECT_GT(meanComponent(grounded, 0), 0.5)
        << "grounded drain mode does not capture uniform shifts";
    EXPECT_GT(grounded.eigenvalues[0], 0.0);
    // Grounding tightens the spectrum's bottom end relative to Neumann's
    // first nontrivial eigenvalue (the drain mode slots in below it).
    EXPECT_LT(grounded.eigenvalues[0], neumann.eigenvalues[0]);
}

TEST(MeshEigenBasis, DepthWeightedRejectsMeshSizeMismatch) {
    auto mesh  = makeStructuredMesh(4);
    auto other = makeStructuredMesh(5);
    MeshEigenBasis b;
    ASSERT_TRUE(b.build(mesh, 6));
    const auto D = makeDiffusivity(other.n_triangles());

    EXPECT_FALSE(b.buildDepthWeighted(other, 6, D.data()));
    EXPECT_FALSE(b.depth_weighted);
}
