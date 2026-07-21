/**
 * @file test_2d_spectral_precond.cpp
 * @brief Unit tests for SpectralPrecond2D — geometric Laplacian eigenbasis
 *        and spectral projection preconditioner.
 *
 * @details Tests are structured in the same style as test_spectral_correction.cpp.
 *          SpectralPrecond2D and MeshBuilder are compiled directly into this
 *          target (no openswmm_engine shared-lib dependency needed).
 *
 *          Each test uses a structured N×N triangular mesh so results can be
 *          verified analytically or by algebraic identities.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

#include "2d/data/MeshData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/SpectralPrecond2D.hpp"

#include <cmath>
#include <numeric>
#include <vector>

using namespace openswmm::twoD;

// ============================================================================
// Mesh helpers
// ============================================================================

// 2-triangle unit-square mesh (same as test_2d_surface_routing.cpp)
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

// Structured N×N mesh on [0, domain_m]²
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
            int v00 = i * (N + 1) + j,    v01 = i * (N + 1) + j + 1;
            int v10 = (i+1) * (N + 1) + j, v11 = (i+1) * (N + 1) + j + 1;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v01; mesh.tri_v2[t] = v11; ++t;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v11; mesh.tri_v2[t] = v10; ++t;
        }

    buildMeshTopology(mesh);
    return mesh;
}

// Compute (geometric Laplacian) * vector using the same weights as SpectralPrecond2D.
// L[i,i] = sum w_ij;  L[i,j] = -w_ij;  w_ij = edge_length / centroid_dist
static std::vector<double> laplacian_matvec(const MeshData& mesh,
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

// ============================================================================
// BuildSucceeds
// ============================================================================

TEST(SpectralPrecond2D, BuildSucceeds) {
    auto mesh = makeStructuredMesh(4);  // 32 triangles
    SpectralPrecond2D p;
    bool ok = p.build(mesh, 6);
    EXPECT_TRUE(ok);
    EXPECT_EQ(p.last_error, 0);
    EXPECT_GT(p.num_kept, 0);
    EXPECT_LE(p.num_kept, 6);
    EXPECT_EQ(p.n_triangles, mesh.n_triangles());
    EXPECT_EQ(static_cast<int>(p.eigenvalues.size()), p.num_kept);
    EXPECT_EQ(static_cast<int>(p.P.size()), p.n_triangles * p.num_kept);
}

// ============================================================================
// BuildFailsSmallMesh
// ============================================================================

TEST(SpectralPrecond2D, BuildFailsSmallMesh) {
    // 2-triangle mesh has n_triangles = 2 < 4 → should fail
    auto mesh = makeUnitSquareMesh();
    SpectralPrecond2D p;
    bool ok = p.build(mesh, 4);
    EXPECT_FALSE(ok);
    EXPECT_EQ(p.last_error, 1);
}

// ============================================================================
// NullModeDiscarded
// ============================================================================

TEST(SpectralPrecond2D, NullModeDiscarded) {
    // Graph Laplacian of a connected mesh has exactly 1 null mode (constant vec).
    auto mesh = makeStructuredMesh(4);
    SpectralPrecond2D p;
    ASSERT_TRUE(p.build(mesh, 8));
    EXPECT_GE(p.num_null, 1);
    // All retained eigenvalues must be > null threshold
    for (int k = 0; k < p.num_kept; ++k)
        EXPECT_GT(p.eigenvalues[static_cast<std::size_t>(k)], 1e-8)
            << "eigenvalue " << k << " = " << p.eigenvalues[k];
}

// ============================================================================
// EigenvaluesPositive
// ============================================================================

TEST(SpectralPrecond2D, EigenvaluesPositive) {
    auto mesh = makeStructuredMesh(5);  // 50 triangles
    SpectralPrecond2D p;
    ASSERT_TRUE(p.build(mesh, 8));
    for (int k = 0; k < p.num_kept; ++k)
        EXPECT_GT(p.eigenvalues[static_cast<std::size_t>(k)], 0.0)
            << "eigenvalue " << k << " = " << p.eigenvalues[k];
}

// ============================================================================
// EigenvectorsOrthonormal  —  P^T * P ≈ I_k
// ============================================================================

TEST(SpectralPrecond2D, EigenvectorsOrthonormal) {
    auto mesh = makeStructuredMesh(4);  // 32 triangles
    SpectralPrecond2D p;
    ASSERT_TRUE(p.build(mesh, 6));

    int n = p.n_triangles;
    int k = p.num_kept;

    for (int a = 0; a < k; ++a) {
        const double* col_a = p.P.data() + a * n;
        for (int b = 0; b < k; ++b) {
            const double* col_b = p.P.data() + b * n;
            double dot = 0.0;
            for (int i = 0; i < n; ++i) dot += col_a[i] * col_b[i];
            if (a == b)
                EXPECT_NEAR(dot, 1.0, 1e-10)
                    << "‖v_" << a << "‖² = " << dot << " (expected 1.0)";
            else
                EXPECT_NEAR(dot, 0.0, 1e-10)
                    << "v_" << a << " · v_" << b << " = " << dot << " (expected 0.0)";
        }
    }
}

// ============================================================================
// EigenrelationSatisfied  —  ‖L*v_k − λ_k*v_k‖ / λ_k < 1e-6
// ============================================================================

TEST(SpectralPrecond2D, EigenrelationSatisfied) {
    auto mesh = makeStructuredMesh(4);
    SpectralPrecond2D p;
    ASSERT_TRUE(p.build(mesh, 6));

    int n = p.n_triangles;
    int k = p.num_kept;

    for (int q = 0; q < k; ++q) {
        double lam = p.eigenvalues[static_cast<std::size_t>(q)];
        const double* col = p.P.data() + q * n;

        std::vector<double> vq(col, col + n);
        std::vector<double> Lv = laplacian_matvec(mesh, vq);

        // Residual: L*v - λ*v
        double resid2 = 0.0, norm2 = 0.0;
        for (int i = 0; i < n; ++i) {
            double r = Lv[static_cast<std::size_t>(i)]
                     - lam * vq[static_cast<std::size_t>(i)];
            resid2 += r * r;
            norm2  += vq[static_cast<std::size_t>(i)] * vq[static_cast<std::size_t>(i)];
        }
        double rel_resid = std::sqrt(resid2 / norm2) / lam;
        EXPECT_LT(rel_resid, 1e-6)
            << "Mode " << q << ": λ=" << lam
            << ", rel_resid=" << rel_resid;
    }
}

// ============================================================================
// ApplyIsIdentityAtGammaZero
// ============================================================================

TEST(SpectralPrecond2D, ApplyIsIdentityAtGammaZero) {
    // scale = 1/(1+0*λ) - 1 = 0 → z = r + 0 = r
    auto mesh = makeStructuredMesh(4);
    SpectralPrecond2D p;
    ASSERT_TRUE(p.build(mesh, 6));

    int n = p.n_triangles;
    std::vector<double> r(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) r[static_cast<std::size_t>(i)] = 1.0 + 0.1 * i;

    std::vector<double> z(static_cast<std::size_t>(n), 0.0);
    p.apply(r.data(), z.data(), 0.0);

    for (int i = 0; i < n; ++i)
        EXPECT_DOUBLE_EQ(z[static_cast<std::size_t>(i)],
                         r[static_cast<std::size_t>(i)])
            << "at i=" << i;
}

// ============================================================================
// ApplyScalesEigenvector
// ============================================================================

TEST(SpectralPrecond2D, ApplyScalesEigenvector) {
    // apply(v_k, z, γ) should give z = v_k / (1 + γ*λ_k)
    // because: z = v_k + P * diag(s_j - 1) * P^T * v_k
    //            = v_k + (s_k - 1) * v_k  (since P^T*v_k = e_k)
    //            = s_k * v_k  = v_k / (1 + γ*λ_k)
    auto mesh = makeStructuredMesh(4);
    SpectralPrecond2D p;
    ASSERT_TRUE(p.build(mesh, 6));

    const double gamma = 1.0;
    int n = p.n_triangles;
    int k = p.num_kept;

    for (int q = 0; q < k; ++q) {
        double lam = p.eigenvalues[static_cast<std::size_t>(q)];
        double expected_scale = 1.0 / (1.0 + gamma * lam);

        const double* col = p.P.data() + q * n;
        std::vector<double> vq(col, col + n);
        std::vector<double> z(static_cast<std::size_t>(n), 0.0);
        p.apply(vq.data(), z.data(), gamma);

        for (int i = 0; i < n; ++i) {
            double expected = expected_scale * vq[static_cast<std::size_t>(i)];
            EXPECT_NEAR(z[static_cast<std::size_t>(i)], expected, 1e-12)
                << "Mode " << q << ", triangle " << i;
        }
    }
}

// ============================================================================
// ApplyOrthogonalComplementIsIdentity
// ============================================================================

TEST(SpectralPrecond2D, ApplyOrthogonalComplementIsIdentity) {
    // A vector orthogonal to all retained modes passes through unchanged.
    // We construct such a vector by subtracting its projection onto span(P).
    auto mesh = makeStructuredMesh(4);
    SpectralPrecond2D p;
    ASSERT_TRUE(p.build(mesh, 4));

    const double gamma = 2.0;
    int n = p.n_triangles;
    int k = p.num_kept;

    // Start with a random-ish vector
    std::vector<double> r(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) r[static_cast<std::size_t>(i)] = std::sin(i * 0.7);

    // Project out the span(P) component: r_perp = r - P * P^T * r
    std::vector<double> c(static_cast<std::size_t>(k), 0.0);
    for (int q = 0; q < k; ++q) {
        const double* col = p.P.data() + q * n;
        double dot = 0.0;
        for (int i = 0; i < n; ++i) dot += col[i] * r[static_cast<std::size_t>(i)];
        c[static_cast<std::size_t>(q)] = dot;
    }
    std::vector<double> r_perp = r;
    for (int q = 0; q < k; ++q) {
        const double* col = p.P.data() + q * n;
        for (int i = 0; i < n; ++i)
            r_perp[static_cast<std::size_t>(i)] -= c[static_cast<std::size_t>(q)] * col[i];
    }

    // apply(r_perp) should return r_perp unchanged (P^T * r_perp ≈ 0)
    std::vector<double> z(static_cast<std::size_t>(n), 0.0);
    p.apply(r_perp.data(), z.data(), gamma);

    for (int i = 0; i < n; ++i)
        EXPECT_NEAR(z[static_cast<std::size_t>(i)],
                    r_perp[static_cast<std::size_t>(i)], 1e-11)
            << "at i=" << i;
}
