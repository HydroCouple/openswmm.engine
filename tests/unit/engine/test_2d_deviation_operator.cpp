/**
 * @file test_2d_deviation_operator.cpp
 * @brief Unit tests for DeviationOperator2D — the reduced k×k deviation
 *        operator and its matrix-exponential propagator — and for the
 *        SpectralROM reduced-operator advance path.
 *
 * @details Layers:
 *          1. expm correctness against closed forms (diagonal, nilpotent,
 *             rotation — the last with a norm large enough to force the
 *             scaling-and-squaring branch).
 *          2. propagate() against the scalar exact integrator it generalizes,
 *             including the singular-operator (Euler) limit and the semigroup
 *             property for a coupled non-diagonal system.
 *          3. Assembly structure on a uniform structured mesh: the isotropic
 *             physical operator must diagonalize on the eigenbasis with
 *             diagonal D·λ_j/A (the FV convention divides the graph
 *             conductances by cell area); anisotropy must break the isotropic
 *             eigenvalues while staying symmetric; advection must contribute
 *             the antisymmetric part that diffusion alone cannot carry.
 *          4. SpectralROM parity: advance() with an installed diagonal
 *             operator diag(K_eff·λ_j) must reproduce the legacy per-mode
 *             exponential path — same members, same Manning multipliers, same
 *             forcing — to near round-off. This pins the matrix branch to the
 *             extensively-validated diagonal branch at the exact point where
 *             the two formulations coincide.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

#include "2d/data/MeshData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/uncertainty/DeviationOperator2D.hpp"
#include "2d/uncertainty/MeshEigenBasis.hpp"
#include "2d/uncertainty/SpatialUncertaintyField.hpp"
#include "2d/uncertainty/SpectralROM.hpp"

#include <cmath>
#include <stdexcept>
#include <cstddef>
#include <vector>

using namespace openswmm::twoD;

namespace {

// Structured N×N triangular mesh on [0, domain_m]² (uniform triangle areas —
// several structure assertions below rely on that).
MeshData makeStructuredMesh(int N, double domain_m = 10.0) {
    MeshData mesh;
    const int nv = (N + 1) * (N + 1);
    const int nt = 2 * N * N;
    mesh.resize_vertices(nv);
    mesh.resize_triangles(nt);

    const double dx = domain_m / N;
    for (int i = 0; i <= N; ++i)
        for (int j = 0; j <= N; ++j) {
            const int vi = i * (N + 1) + j;
            mesh.vx[static_cast<std::size_t>(vi)] = j * dx;
            mesh.vy[static_cast<std::size_t>(vi)] = i * dx;
            mesh.vz[static_cast<std::size_t>(vi)] = 0.0;
        }
    int t = 0;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            const int v00 = i * (N + 1) + j,       v01 = i * (N + 1) + j + 1;
            const int v10 = (i + 1) * (N + 1) + j, v11 = (i + 1) * (N + 1) + j + 1;
            mesh.set_triangle(t, v00, v01, v11); ++t;
            mesh.set_triangle(t, v00, v11, v10); ++t;
        }
    buildMeshTopology(mesh);
    return mesh;
}

double maxAbsDiff(const std::vector<double>& a, const std::vector<double>& b) {
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

} // namespace

// ============================================================================
// 1. expm against closed forms
// ============================================================================

TEST(DeviationOperator2D, ExpmDiagonalIsElementwiseExp) {
    std::vector<double> A = {
        -1.0, 0.0, 0.0,
         0.0, -2.5, 0.0,
         0.0, 0.0, 0.75
    };
    DeviationOperator2D::expm(A, 3);
    EXPECT_NEAR(A[0], std::exp(-1.0),  1e-14);
    EXPECT_NEAR(A[4], std::exp(-2.5),  1e-13);
    EXPECT_NEAR(A[8], std::exp(0.75),  1e-14);
    EXPECT_NEAR(A[1], 0.0, 1e-15);
    EXPECT_NEAR(A[3], 0.0, 1e-15);
}

TEST(DeviationOperator2D, ExpmNilpotent) {
    // A = [[0,1],[0,0]] → exp(A) = [[1,1],[0,1]] exactly (series terminates).
    std::vector<double> A = {0.0, 1.0, 0.0, 0.0};
    DeviationOperator2D::expm(A, 2);
    EXPECT_NEAR(A[0], 1.0, 1e-15);
    EXPECT_NEAR(A[1], 1.0, 1e-14);
    EXPECT_NEAR(A[2], 0.0, 1e-15);
    EXPECT_NEAR(A[3], 1.0, 1e-15);
}

TEST(DeviationOperator2D, ExpmRotationExercisesScalingSquaring) {
    // A = θ·[[0,−1],[1,0]] → exp(A) = [[cosθ,−sinθ],[sinθ,cosθ]].
    // θ = 3.0 gives ‖A‖∞ = 3 > 1/2, forcing several squaring rounds.
    const double th = 3.0;
    std::vector<double> A = {0.0, -th, th, 0.0};
    DeviationOperator2D::expm(A, 2);
    EXPECT_NEAR(A[0],  std::cos(th), 1e-12);
    EXPECT_NEAR(A[1], -std::sin(th), 1e-12);
    EXPECT_NEAR(A[2],  std::sin(th), 1e-12);
    EXPECT_NEAR(A[3],  std::cos(th), 1e-12);
}

// ============================================================================
// 2. propagate against the scalar exact integrator
// ============================================================================

TEST(DeviationOperator2D, PropagateReducesToScalarExactIntegrator) {
    // k = 1: δa⁺ must equal (δa − g/r)e^{−r·s·dt} + g/(r·s)… careful — with the
    // operator scaled by s, the fixed point is g/(s·r). Verify against the
    // closed form of dδa/dt = −s·r·δa + g.
    const double r = 0.35, g = 0.8, dt = 4.0;
    for (const double s : {1.0, 0.7, 1.9}) {
        std::vector<double> M = {r};
        double a = 0.25;
        DeviationOperator2D::propagate(M, 1, s, dt, &a, &g);
        const double rate = s * r;
        const double exact = (0.25 - g / rate) * std::exp(-rate * dt) + g / rate;
        EXPECT_NEAR(a, exact, 1e-13) << "s = " << s;
    }
}

TEST(DeviationOperator2D, PropagateSingularOperatorIsEulerLimit) {
    // M = 0 → δa ← δa + g·dt (φ₁(0) = I). The augmented form must handle the
    // singular operator without any special-casing.
    std::vector<double> M = {0.0, 0.0, 0.0, 0.0};
    std::vector<double> a = {1.0, -2.0};
    const std::vector<double> g = {0.5, 0.25};
    DeviationOperator2D::propagate(M, 2, 1.0, 3.0, a.data(), g.data());
    EXPECT_NEAR(a[0], 1.0 + 0.5 * 3.0, 1e-13);
    EXPECT_NEAR(a[1], -2.0 + 0.25 * 3.0, 1e-13);
}

TEST(DeviationOperator2D, PropagateSemigroupOnCoupledSystem) {
    // Two half-steps must equal one full step for a genuinely coupled
    // (non-diagonal, non-normal) operator with constant forcing.
    const std::vector<double> M = {
        0.9, -0.3, 0.1,
        0.2,  0.7, 0.0,
       -0.1,  0.4, 1.1
    };
    const std::vector<double> g = {0.3, -0.2, 0.05};
    std::vector<double> a1 = {1.0, 0.5, -0.75};
    std::vector<double> a2 = a1;

    DeviationOperator2D::propagate(M, 3, 1.0, 5.0, a1.data(), g.data());

    DeviationOperator2D::propagate(M, 3, 1.0, 2.5, a2.data(), g.data());
    DeviationOperator2D::propagate(M, 3, 1.0, 2.5, a2.data(), g.data());

    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(a1[static_cast<std::size_t>(i)],
                    a2[static_cast<std::size_t>(i)], 1e-12);
}

// ============================================================================
// 3. Assembly structure
// ============================================================================

TEST(DeviationOperator2D, IsotropicAssemblyDiagonalizesOnEigenbasis) {
    // On a uniform mesh (constant area A), the isotropic physical operator is
    // L_geo/A scaled by D, so M = (D/A)·PᵀL_geoP = diag(D·λ_j/A) up to the
    // eigensolver's Ritz residual. Both the diagonal values and the smallness
    // of the off-diagonals are asserted.
    auto mesh = makeStructuredMesh(6);   // 72 triangles, uniform area
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 8));
    const int k = basis.num_kept;
    const double A = mesh.tri_area[0];
    for (int i = 1; i < mesh.n_triangles(); ++i)
        ASSERT_NEAR(mesh.tri_area[static_cast<std::size_t>(i)], A, 1e-12)
            << "fixture assumption violated: non-uniform areas";

    const double D = 2.75;  // arbitrary diffusivity scale
    DeviationOperator2D op;
    op.alpha_par = op.alpha_perp = 1.0;
    op.c_factor  = 0.0;
    ASSERT_TRUE(op.assemble(mesh, basis, D, nullptr, nullptr, nullptr));
    ASSERT_EQ(op.k, k);

    double diag_scale = 0.0;
    for (int j = 0; j < k; ++j)
        diag_scale = std::max(diag_scale,
            std::fabs(op.M[static_cast<std::size_t>(j * k + j)]));
    ASSERT_GT(diag_scale, 0.0);

    for (int p = 0; p < k; ++p) {
        for (int q = 0; q < k; ++q) {
            const double m = op.M[static_cast<std::size_t>(p * k + q)];
            if (p == q) {
                const double expect = D * basis.eigenvalues[
                    static_cast<std::size_t>(p)] / A;
                EXPECT_NEAR(m, expect, 1e-5 * std::fabs(expect) + 1e-12)
                    << "diagonal entry " << p;
            } else {
                EXPECT_LT(std::fabs(m), 1e-5 * diag_scale)
                    << "off-diagonal (" << p << "," << q << ")";
            }
        }
    }
}

TEST(DeviationOperator2D, AnisotropySymmetricAndDistinctFromIsotropic) {
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 8));
    const int k = basis.num_kept;

    // Uniform +x flow field defines the streamwise axis.
    const int nt = mesh.n_triangles();
    std::vector<double> u(static_cast<std::size_t>(nt), 0.5);
    std::vector<double> v(static_cast<std::size_t>(nt), 0.0);

    DeviationOperator2D iso;
    iso.alpha_par = iso.alpha_perp = 1.0;
    ASSERT_TRUE(iso.assemble(mesh, basis, 1.0, nullptr, u.data(), v.data()));

    DeviationOperator2D ani;
    ani.alpha_par  = 0.31;
    ani.alpha_perp = 1.0;
    ASSERT_TRUE(ani.assemble(mesh, basis, 1.0, nullptr, u.data(), v.data()));

    // No advection → both symmetric (uniform areas make the FV scaling
    // symmetric too).
    double asym_iso = 0.0, asym_ani = 0.0, diff = 0.0, scale = 0.0;
    for (int p = 0; p < k; ++p)
        for (int q = 0; q < k; ++q) {
            const auto pq = static_cast<std::size_t>(p * k + q);
            const auto qp = static_cast<std::size_t>(q * k + p);
            asym_iso = std::max(asym_iso, std::fabs(iso.M[pq] - iso.M[qp]));
            asym_ani = std::max(asym_ani, std::fabs(ani.M[pq] - ani.M[qp]));
            diff  = std::max(diff,  std::fabs(iso.M[pq] - ani.M[pq]));
            scale = std::max(scale, std::fabs(iso.M[pq]));
        }
    EXPECT_LT(asym_iso, 1e-12 * scale + 1e-15);
    EXPECT_LT(asym_ani, 1e-12 * scale + 1e-15);
    // The anisotropic operator must actually differ (streamwise conductances
    // reduced by α∥ = 0.31).
    EXPECT_GT(diff, 0.05 * scale);
}

TEST(DeviationOperator2D, AdvectionContributesAntisymmetricPart) {
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 8));
    const int k = basis.num_kept;

    const int nt = mesh.n_triangles();
    std::vector<double> u(static_cast<std::size_t>(nt), 0.5);
    std::vector<double> v(static_cast<std::size_t>(nt), 0.0);

    DeviationOperator2D op;
    op.alpha_par = op.alpha_perp = 1.0;
    op.c_factor  = 5.0 / 3.0;
    ASSERT_TRUE(op.assemble(mesh, basis, 1.0, nullptr, u.data(), v.data()));

    double asym = 0.0, scale = 0.0;
    for (int p = 0; p < k; ++p)
        for (int q = 0; q < k; ++q) {
            const auto pq = static_cast<std::size_t>(p * k + q);
            const auto qp = static_cast<std::size_t>(q * k + p);
            asym  = std::max(asym, std::fabs(op.M[pq] - op.M[qp]));
            scale = std::max(scale, std::fabs(op.M[pq]));
        }
    // The skew (transport) part is the whole point of the advection rung.
    EXPECT_GT(asym, 1e-3 * scale)
        << "advection produced no antisymmetric component";
}

// ============================================================================
// 3b. Energy-stable form (skew advection in the volume inner product)
// ============================================================================

namespace {
// Structured mesh with x stretched quadratically so cell areas vary ~3x, and a
// flow field converging on the centre -- the regime (ponds) where the plain
// projection of upwind advection is unstable.
MeshData makeStretchedMesh(int N, double domain_m = 10.0) {
    MeshData mesh = makeStructuredMesh(N, domain_m);
    for (std::size_t v = 0; v < mesh.vx.size(); ++v) {
        const double x = mesh.vx[v] / domain_m;             // 0..1
        mesh.vx[v] = domain_m * (0.25 * x + 0.75 * x * x);  // dx grows 4x across
    }
    buildMeshTopology(mesh);
    return mesh;
}

// G = Pᵀ·diag(A)·P
std::vector<double> volumeGram(const MeshEigenBasis& basis, const MeshData& mesh) {
    const int k = basis.num_kept, n = mesh.n_triangles();
    std::vector<double> G(static_cast<std::size_t>(k) * k, 0.0);
    for (int p = 0; p < k; ++p)
        for (int q = 0; q < k; ++q) {
            double s = 0.0;
            for (int t = 0; t < n; ++t)
                s += basis.P[static_cast<std::size_t>(p) * n + t] * mesh.tri_area[static_cast<std::size_t>(t)]
                   * basis.P[static_cast<std::size_t>(q) * n + t];
            G[static_cast<std::size_t>(p) * k + q] = s;
        }
    return G;
}

// Smallest eigenvalue of S = sym(G·M), the energy form of the reduced operator
// in the volume inner product (negative = a growing direction exists).
// Cyclic Jacobi on the k×k symmetric matrix; k is O(10-100).
double minEnergyEig(const std::vector<double>& G, const std::vector<double>& M, int k) {
    const auto K = static_cast<std::size_t>(k);
    std::vector<double> GM(K * K, 0.0), S(K * K, 0.0);
    for (std::size_t p = 0; p < K; ++p)
        for (std::size_t q = 0; q < K; ++q) {
            double s = 0.0;
            for (std::size_t r = 0; r < K; ++r) s += G[p * K + r] * M[r * K + q];
            GM[p * K + q] = s;
        }
    for (std::size_t p = 0; p < K; ++p)
        for (std::size_t q = 0; q < K; ++q) S[p * K + q] = 0.5 * (GM[p * K + q] + GM[q * K + p]);
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (std::size_t p = 0; p < K; ++p)
            for (std::size_t q = p + 1; q < K; ++q) off += S[p * K + q] * S[p * K + q];
        if (off < 1e-30) break;
        for (std::size_t p = 0; p < K; ++p)
            for (std::size_t q = p + 1; q < K; ++q) {
                const double apq = S[p * K + q];
                if (std::fabs(apq) < 1e-300) continue;
                const double theta = (S[q * K + q] - S[p * K + p]) / (2.0 * apq);
                const double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), sn = t * c;
                for (std::size_t r = 0; r < K; ++r) {   // rotate columns p, q
                    const double arp = S[r * K + p], arq = S[r * K + q];
                    S[r * K + p] = c * arp - sn * arq;
                    S[r * K + q] = sn * arp + c * arq;
                }
                for (std::size_t r = 0; r < K; ++r) {   // rotate rows p, q
                    const double apr = S[p * K + r], aqr = S[q * K + r];
                    S[p * K + r] = c * apr - sn * aqr;
                    S[q * K + r] = sn * apr + c * aqr;
                }
            }
    }
    double mn = 1e300;
    for (std::size_t p = 0; p < K; ++p) mn = std::min(mn, S[p * K + p]);
    return mn;
}
} // namespace

TEST(DeviationOperator2D, EnergyStableFormHasNoGrowingModeOnConvergingFlow) {
    auto mesh = makeStretchedMesh(10);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 24));
    const int k = basis.num_kept, nt = mesh.n_triangles();
    double cx = 0.0;
    for (int t = 0; t < nt; ++t) cx += mesh.tri_cx[static_cast<std::size_t>(t)];
    cx /= nt;
    // Flow converging on the line x = cx at 1 m/s from both sides: a sink line,
    // the sharpest convergence a cell-centred field can hold (Bellinge's ponds).
    std::vector<double> u(static_cast<std::size_t>(nt)), v(static_cast<std::size_t>(nt), 0.0);
    for (int t = 0; t < nt; ++t)
        u[static_cast<std::size_t>(t)] = (mesh.tri_cx[static_cast<std::size_t>(t)] < cx) ? 1.0 : -1.0;
    const auto G = volumeGram(basis, mesh);

    DeviationOperator2D plain;
    plain.alpha_par = 0.62; plain.alpha_perp = 2.0; plain.c_factor = 5.0 / 3.0;
    ASSERT_TRUE(plain.assemble(mesh, basis, 1.0e-3, nullptr, u.data(), v.data()));
    // Energy form of the plain operator in the plain inner product: xᵀ·sym(M)·x.
    std::vector<double> I(static_cast<std::size_t>(k) * k, 0.0);
    for (int i = 0; i < k; ++i) I[static_cast<std::size_t>(i) * k + i] = 1.0;
    const double plain_min = minEnergyEig(I, plain.M, k);
    EXPECT_LT(plain_min, -1e-6)
        << "fixture must make the plain upwind projection lose energy stability, or this test proves nothing"
        << " (min eig " << plain_min << ")";

    DeviationOperator2D stable;
    stable.alpha_par = 0.62; stable.alpha_perp = 2.0; stable.c_factor = 5.0 / 3.0;
    stable.skew_advection = true;
    stable.volume_inner_product = true;
    ASSERT_TRUE(stable.assemble(mesh, basis, 1.0e-3, nullptr, u.data(), v.data()));
    ASSERT_EQ(stable.GinvArea.size(), static_cast<std::size_t>(k) * k);
    const double stable_min = minEnergyEig(G, stable.M, k);
    EXPECT_GE(stable_min, -1e-10 * G[0])
        << "energy form of the stable operator must be non-negative for every direction (min eig "
        << stable_min << ")";

    // GinvArea really inverts the volume Gram.
    double worst = 0.0;
    for (int p = 0; p < k; ++p)
        for (int q = 0; q < k; ++q) {
            double s = 0.0;
            for (int r = 0; r < k; ++r) s += stable.GinvArea[static_cast<std::size_t>(p) * k + r] * G[static_cast<std::size_t>(r) * k + q];
            worst = std::max(worst, std::fabs(s - (p == q ? 1.0 : 0.0)));
        }
    EXPECT_LT(worst, 1e-9);
}

TEST(DeviationOperator2D, VolumeInnerProductReducesToPlainOnUniformAreas) {
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 8));
    const int nt = mesh.n_triangles();
    std::vector<double> u(static_cast<std::size_t>(nt), 0.4), v(static_cast<std::size_t>(nt), -0.2);
    DeviationOperator2D a, b;
    a.alpha_par = b.alpha_par = 0.62; a.alpha_perp = b.alpha_perp = 2.0; a.c_factor = b.c_factor = 0.0;
    ASSERT_TRUE(a.assemble(mesh, basis, 1.0, nullptr, u.data(), v.data()));
    b.volume_inner_product = true;
    ASSERT_TRUE(b.assemble(mesh, basis, 1.0, nullptr, u.data(), v.data()));
    // Uniform areas: G = A·I, so G⁻¹·Pᵀ·A·L·P = Pᵀ·L·P exactly (to round-off).
    double scale = 0.0;
    for (double m : a.M) scale = std::max(scale, std::fabs(m));
    EXPECT_LT(maxAbsDiff(a.M, b.M), 1e-10 * scale);
}

// ============================================================================
// 4. SpectralROM matrix path ≡ legacy diagonal path at the coincidence point
// ============================================================================

TEST(DeviationOperator2D, RomMatrixPathMatchesDiagonalPathForDiagonalOperator) {
    // Install M = diag(K_eff·λ_j) — exactly the operator the legacy diagonal
    // path integrates (uniform depth weights: h_cell = null on both sides).
    // Trajectories must then agree to near round-off across several advances
    // with Manning multipliers and rainfall forcing active. This isolates the
    // advance() branch itself: no assembly, no Ritz residual in the way.
    auto mesh = makeStructuredMesh(5);   // 50 triangles
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));
    const int nt = mesh.n_triangles();
    const int k  = basis.num_kept;

    const int    kM    = 8;
    const double K_eff = 0.9;
    const double dt    = 7.0;

    // Shared multipliers (like-for-like members on both paths).
    std::vector<double> mann(kM), ones(kM, 1.0);
    for (int i = 0; i < kM; ++i)
        mann[static_cast<std::size_t>(i)] = 0.8 + 0.4 * (i + 0.5) / kM;

    // Non-uniform deterministic depth (uniform fields project to ~0 on the
    // zero-mean modes) and non-uniform rainfall.
    std::vector<double> h_det(static_cast<std::size_t>(nt));
    std::vector<double> rain(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t) {
        h_det[static_cast<std::size_t>(t)] =
            0.10 + 0.05 * std::sin(0.37 * t + 0.2);
        rain[static_cast<std::size_t>(t)] =
            1.0e-5 * (1.0 + 0.5 * std::cos(0.23 * t));
    }

    auto makeRom = [&](SpectralROM& rom) {
        rom.basis         = &basis;
        rom.n_ensemble    = kM;
        rom.mannings_pert = 0.20;
        rom.rainfall_pert = 0.10;
        rom.setExternalSamples(mann, ones);
        rom.initialize();
        rom.seed(h_det.data());
    };

    SpectralROM diag_rom, mat_rom;
    makeRom(diag_rom);
    makeRom(mat_rom);

    std::vector<double> M(static_cast<std::size_t>(k) *
                          static_cast<std::size_t>(k), 0.0);
    for (int j = 0; j < k; ++j)
        M[static_cast<std::size_t>(j * k + j)] =
            K_eff * basis.eigenvalues[static_cast<std::size_t>(j)];
    mat_rom.setReducedOperator(M);

    for (int step = 0; step < 5; ++step) {
        diag_rom.advance(dt, K_eff, rain.data(), nullptr, h_det.data());
        mat_rom.advance(dt, K_eff, rain.data(), nullptr, h_det.data());
    }

    // The two integrators are mathematically identical for a diagonal M; the
    // only differences are round-off (Padé vs std::exp) — parts in 1e12.
    double max_a = 0.0;
    for (const double a : diag_rom.a_ensemble)
        max_a = std::max(max_a, std::fabs(a));
    ASSERT_GT(max_a, 1e-9) << "trajectory carried no deviation — vacuous test";

    EXPECT_LT(maxAbsDiff(diag_rom.a_ensemble, mat_rom.a_ensemble),
              1e-10 * std::max(max_a, 1.0) + 1e-13);

    // Quantiles must agree too (same reconstruction machinery downstream).
    diag_rom.computeQuantiles(h_det.data());
    mat_rom.computeQuantiles(h_det.data());
    EXPECT_LT(maxAbsDiff(diag_rom.q05, mat_rom.q05), 1e-10);
    EXPECT_LT(maxAbsDiff(diag_rom.q50, mat_rom.q50), 1e-10);
    EXPECT_LT(maxAbsDiff(diag_rom.q95, mat_rom.q95), 1e-10);
}

TEST(DeviationOperator2D, GroundedOperatorDampsDrainMode) {
    // A grounded basis (open-boundary cells tied to a zero-deviation ghost)
    // exposes a quasi-uniform drain mode. The operator must be assembled with
    // the SAME grounding: without it, the drain mode's Galerkin decay rate is
    // near zero — deviations parked on it would persist unphysically instead
    // of flushing through the boundary.
    auto mesh = makeStructuredMesh(6);
    const int n = mesh.n_triangles();

    std::vector<double> gw(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        for (int e = 0; e < mesh.cell_vertex_count(static_cast<int>(ui)); ++e) {
            const auto idx = static_cast<std::size_t>(openswmm::twoD::MeshData::slot(static_cast<int>(i), e));
            if (mesh.cell_neighbour(static_cast<int>(ui), e) >= 0) continue;
            if (mesh.edge_mx[idx] >= 1e-9) continue;
            const double dx = mesh.edge_mx[idx] - mesh.tri_cx[ui];
            const double dy = mesh.edge_my[idx] - mesh.tri_cy[ui];
            const double d  = 2.0 * std::sqrt(dx * dx + dy * dy);
            if (d > 1e-12) gw[ui] += mesh.edge_length[idx] / d;
        }
    }

    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6, gw.data()));
    const int k = basis.num_kept;

    DeviationOperator2D grounded, neumann;
    grounded.alpha_par = grounded.alpha_perp = 1.0;
    neumann.alpha_par  = neumann.alpha_perp  = 1.0;
    ASSERT_TRUE(grounded.assemble(mesh, basis, 1.0, nullptr, nullptr, nullptr,
                                  gw.data()));
    ASSERT_TRUE(neumann.assemble(mesh, basis, 1.0, nullptr, nullptr, nullptr));

    // Mode 0 of the grounded basis is the drain mode; its decay rate under
    // the grounded operator must exceed its rate under the ungrounded one.
    EXPECT_GT(grounded.M[0], neumann.M[0]);

    // And a deviation parked purely on the drain mode must decay under the
    // grounded operator.
    std::vector<double> a(static_cast<std::size_t>(k), 0.0);
    a[0] = 1.0;
    const std::vector<double> g(static_cast<std::size_t>(k), 0.0);
    DeviationOperator2D::propagate(grounded.M, k, 1.0, 5.0, a.data(), g.data());
    EXPECT_LT(std::fabs(a[0]), 1.0)
        << "drain-mode deviation did not decay under the grounded operator";
}

TEST(DeviationOperator2D, RomRejectsWrongOperatorSize) {
    auto mesh = makeStructuredMesh(5);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));

    SpectralROM rom;
    rom.basis      = &basis;
    rom.n_ensemble = 4;
    rom.initialize();

    std::vector<double> bad(static_cast<std::size_t>(
        (basis.num_kept + 1) * (basis.num_kept + 1)), 0.0);
    EXPECT_THROW(rom.setReducedOperator(bad), std::invalid_argument);
}

// ============================================================================
// PR H15 — per-cell conductance multiplier and per-member reduced operators
// ============================================================================

namespace {

// Flow field and grounding shared by the H15 assembly tests: a uniform
// x-directed velocity (so advection and anisotropy are both live) and
// grounding on the x=0 faces.
void h15Fixture(const MeshData& mesh, std::vector<double>& u, std::vector<double>& v,
                std::vector<double>& gw, std::vector<double>& h) {
    const int n = mesh.n_triangles();
    u.assign(static_cast<std::size_t>(n), 0.3);
    v.assign(static_cast<std::size_t>(n), 0.05);
    gw.assign(static_cast<std::size_t>(n), 0.0);
    h.assign(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        h[ui] = 0.10 + 0.04 * std::sin(0.3 * i);
        if (mesh.tri_cx[ui] < 1.0) gw[ui] = 0.25;
    }
}

} // namespace

TEST(DeviationOperator2D, CondMultNullIsBitIdenticalToAllOnes) {
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    std::vector<double> u, v, gw, h;
    h15Fixture(mesh, u, v, gw, h);
    ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
    DeviationOperator2D a, b;
    a.alpha_par = b.alpha_par = 0.62; a.alpha_perp = b.alpha_perp = 2.0;
    a.c_factor = b.c_factor = 5.0 / 3.0;
    std::vector<double> ones(static_cast<std::size_t>(mesh.n_triangles()), 1.0);
    ASSERT_TRUE(a.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data()));
    ASSERT_TRUE(b.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data(), ones.data()));
    ASSERT_EQ(a.M.size(), b.M.size());
    for (std::size_t i = 0; i < a.M.size(); ++i)
        EXPECT_EQ(a.M[i], b.M[i]) << "entry " << i;   // bit-identical, not NEAR
}

TEST(DeviationOperator2D, UniformCondMultScalesWholeOperator) {
    // 1/n multiplies diffusion, advection and grounding alike, so a uniform
    // factor c must give c·M (the shared-operator s = 1/mm convention).
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    std::vector<double> u, v, gw, h;
    h15Fixture(mesh, u, v, gw, h);
    ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
    DeviationOperator2D a, b;
    a.alpha_par = b.alpha_par = 0.62; a.alpha_perp = b.alpha_perp = 2.0;
    a.c_factor = b.c_factor = 5.0 / 3.0;
    const double c = 1.0 / 0.85;
    std::vector<double> cm(static_cast<std::size_t>(mesh.n_triangles()), c);
    ASSERT_TRUE(a.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data()));
    ASSERT_TRUE(b.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data(), cm.data()));
    double scale = 0.0;
    for (double m : a.M) scale = std::max(scale, std::fabs(m));
    ASSERT_GT(scale, 0.0);
    for (std::size_t i = 0; i < a.M.size(); ++i)
        EXPECT_NEAR(b.M[i], c * a.M[i], 1e-12 * scale) << "entry " << i;
}

TEST(DeviationOperator2D, NonUniformCondMultIsNotAScalarMultiple) {
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    std::vector<double> u, v, gw, h;
    h15Fixture(mesh, u, v, gw, h);
    ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
    DeviationOperator2D a, b;
    a.alpha_par = b.alpha_par = 0.62; a.alpha_perp = b.alpha_perp = 2.0;
    a.c_factor = b.c_factor = 5.0 / 3.0;
    const int n = mesh.n_triangles();
    std::vector<double> cm(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        cm[static_cast<std::size_t>(i)] = 1.0 / (1.0 + 0.2 * std::sin(0.7 * i));
    ASSERT_TRUE(a.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data()));
    ASSERT_TRUE(b.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data(), cm.data()));
    // Best scalar fit c* = <A,B>/<A,A>; the residual must be material.
    double ab = 0.0, aa = 0.0;
    for (std::size_t i = 0; i < a.M.size(); ++i) { ab += a.M[i] * b.M[i]; aa += a.M[i] * a.M[i]; }
    const double cstar = ab / aa;
    double res = 0.0, nrm = 0.0;
    for (std::size_t i = 0; i < a.M.size(); ++i) {
        res += (b.M[i] - cstar * a.M[i]) * (b.M[i] - cstar * a.M[i]);
        nrm += b.M[i] * b.M[i];
    }
    EXPECT_GT(std::sqrt(res / nrm), 1e-3)
        << "a non-uniform 1/n field left the operator a scalar multiple of nominal";
}

namespace {

// Builds a ROM on `basis` with M members; `mann` scalar multipliers (ones for
// the spatial path) and an optional spatial field.
void h15MakeRom(SpectralROM& rom, const MeshEigenBasis& basis, int M,
                const std::vector<double>& mann, const std::vector<double>& h_det,
                const SpatialUncertaintyField* field) {
    std::vector<double> ones(static_cast<std::size_t>(M), 1.0);
    rom.basis = &basis; rom.n_ensemble = M;
    rom.mannings_pert = 0.20; rom.rainfall_pert = 0.0;
    rom.setExternalSamples(mann, ones);
    rom.initialize();
    rom.seed(h_det.data());
    if (field) rom.spatial_mannings = *field;
}

// Assembles the nominal operator and one operator per member from `field`.
void h15Assemble(const MeshData& mesh, const MeshEigenBasis& basis,
                 const SpatialUncertaintyField& field, const std::vector<double>& h,
                 const std::vector<double>& u, const std::vector<double>& v,
                 const std::vector<double>& gw, std::vector<double>& M0,
                 std::vector<double>& M_all, int& k) {
    DeviationOperator2D op;
    op.alpha_par = 0.62; op.alpha_perp = 2.0; op.c_factor = 5.0 / 3.0;
    ASSERT_TRUE(op.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data()));
    M0 = op.M; k = op.k;
    const auto kk = static_cast<std::size_t>(k) * static_cast<std::size_t>(k);
    M_all.assign(static_cast<std::size_t>(field.n_members) * kk, 0.0);
    std::vector<double> cm(static_cast<std::size_t>(field.n_cells));
    for (int i = 0; i < field.n_members; ++i) {
        for (int t = 0; t < field.n_cells; ++t) cm[static_cast<std::size_t>(t)] = 1.0 / field.at(i, t);
        ASSERT_TRUE(op.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data(), cm.data()));
        std::copy(op.M.begin(), op.M.end(), M_all.begin() + static_cast<std::ptrdiff_t>(i) * static_cast<std::ptrdiff_t>(kk));
    }
}

} // namespace

TEST(DeviationOperator2D, PerMemberOperatorsWithUniformFieldMatchSharedOperatorPath) {
    // W_i(t) ≡ mm_i ⇒ M_i = M₀/mm_i and the per-member integrator must
    // reproduce the shared-operator path (s = 1/mm_i) to round-off.
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    std::vector<double> u, v, gw, h;
    h15Fixture(mesh, u, v, gw, h);
    ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
    const int nt = mesh.n_triangles(), M = 7;
    std::vector<double> mann(static_cast<std::size_t>(M)), ones(static_cast<std::size_t>(M), 1.0);
    for (int i = 0; i < M; ++i) mann[static_cast<std::size_t>(i)] = 0.8 + 0.4 * (i + 0.5) / M;
    SpatialUncertaintyField field;
    field.n_members = M; field.n_cells = nt;
    field.values.resize(static_cast<std::size_t>(M) * static_cast<std::size_t>(nt));
    for (int i = 0; i < M; ++i) for (int t = 0; t < nt; ++t) field.at(i, t) = mann[static_cast<std::size_t>(i)];

    std::vector<double> M0, M_all; int k = 0;
    h15Assemble(mesh, basis, field, h, u, v, gw, M0, M_all, k);

    SpectralROM shared, per;
    h15MakeRom(shared, basis, M, mann, h, nullptr);
    h15MakeRom(per, basis, M, ones, h, &field);
    shared.setReducedOperator(M0);
    per.setReducedOperator(M0);
    per.setReducedOperatorPerMember(M_all);
    ASSERT_TRUE(per.hasPerMemberOperators());

    for (int step = 0; step < 6; ++step) {
        shared.advance(30.0, 1.0, nullptr, nullptr, h.data());
        per.advance(30.0, 1.0, nullptr, nullptr, h.data());
    }
    double max_a = 0.0;
    for (double a : shared.a_ensemble) max_a = std::max(max_a, std::fabs(a));
    ASSERT_GT(max_a, 1e-9) << "vacuous: no deviation developed";
    EXPECT_LT(maxAbsDiff(shared.a_ensemble, per.a_ensemble), 1e-10 * max_a);
}

TEST(DeviationOperator2D, PerMemberOperatorsUnitFieldKeepsExactZeroDeviation) {
    // Every member's field ≡ 1 ⇒ M_i ≡ M₀ bit-for-bit ⇒ the Manning
    // sensitivity −(M_i − M₀)b is exactly 0 and δa stays exactly 0.
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    std::vector<double> u, v, gw, h;
    h15Fixture(mesh, u, v, gw, h);
    ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
    const int nt = mesh.n_triangles(), M = 5;
    std::vector<double> ones(static_cast<std::size_t>(M), 1.0);
    SpatialUncertaintyField field;
    field.n_members = M; field.n_cells = nt;
    field.values.assign(static_cast<std::size_t>(M) * static_cast<std::size_t>(nt), 1.0);
    std::vector<double> M0, M_all; int k = 0;
    h15Assemble(mesh, basis, field, h, u, v, gw, M0, M_all, k);
    SpectralROM rom;
    h15MakeRom(rom, basis, M, ones, h, &field);
    rom.setReducedOperator(M0);
    rom.setReducedOperatorPerMember(M_all);
    for (int step = 0; step < 4; ++step) rom.advance(30.0, 1.0, nullptr, nullptr, h.data());
    for (double a : rom.a_ensemble) EXPECT_EQ(a, 0.0);
}

TEST(DeviationOperator2D, PerMemberOperatorsLeaveTheDiagonalFallback) {
    // Same spatial field, with and without per-member operators: the second
    // run must take the reduced path (different trajectory from the diagonal
    // fallback), and clearing the operators must restore the fallback.
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    std::vector<double> u, v, gw, h;
    h15Fixture(mesh, u, v, gw, h);
    ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
    const int nt = mesh.n_triangles(), M = 6;
    std::vector<double> ones(static_cast<std::size_t>(M), 1.0);
    SpatialUncertaintyField field;
    field.n_members = M; field.n_cells = nt;
    field.values.resize(static_cast<std::size_t>(M) * static_cast<std::size_t>(nt));
    for (int i = 0; i < M; ++i)
        for (int t = 0; t < nt; ++t)
            field.at(i, t) = 1.0 + 0.2 * (2.0 * (i + 0.5) / M - 1.0) * (1.0 + 0.5 * std::sin(0.4 * t + i));
    std::vector<double> M0, M_all; int k = 0;
    h15Assemble(mesh, basis, field, h, u, v, gw, M0, M_all, k);

    SpectralROM diag, per, cleared;
    h15MakeRom(diag, basis, M, ones, h, &field);
    h15MakeRom(per, basis, M, ones, h, &field);
    h15MakeRom(cleared, basis, M, ones, h, &field);
    diag.setReducedOperator(M0);            // spatial field ⇒ diagonal fallback
    per.setReducedOperator(M0);
    per.setReducedOperatorPerMember(M_all);
    cleared.setReducedOperator(M0);
    cleared.setReducedOperatorPerMember(M_all);
    cleared.clearReducedOperator();
    EXPECT_FALSE(cleared.hasReducedOperator());
    EXPECT_FALSE(cleared.hasPerMemberOperators());

    for (int step = 0; step < 4; ++step) {
        diag.advance(30.0, 1.0, nullptr, nullptr, h.data());
        per.advance(30.0, 1.0, nullptr, nullptr, h.data());
        cleared.advance(30.0, 1.0, nullptr, nullptr, h.data());
    }
    double max_a = 0.0;
    for (double a : per.a_ensemble) max_a = std::max(max_a, std::fabs(a));
    ASSERT_GT(max_a, 1e-9);
    EXPECT_GT(maxAbsDiff(diag.a_ensemble, per.a_ensemble), 1e-6 * max_a)
        << "per-member operators did not change the integration path";
    // diag (shared op + spatial field) and cleared (no op) both take the
    // diagonal path and must coincide exactly.
    EXPECT_EQ(maxAbsDiff(diag.a_ensemble, cleared.a_ensemble), 0.0);
}

TEST(DeviationOperator2D, SetReducedOperatorPerMemberRejectsWrongSize) {
    auto mesh = makeStructuredMesh(5);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));
    const int M = 4;
    std::vector<double> ones(static_cast<std::size_t>(M), 1.0), h(static_cast<std::size_t>(mesh.n_triangles()), 0.1);
    SpectralROM rom;
    h15MakeRom(rom, basis, M, ones, h, nullptr);
    const auto k = static_cast<std::size_t>(basis.num_kept);
    EXPECT_THROW(rom.setReducedOperatorPerMember(std::vector<double>((M - 1) * k * k, 0.0)), std::invalid_argument);
    EXPECT_THROW(rom.setReducedOperatorPerMember(std::vector<double>(M * k * k + 1, 0.0)), std::invalid_argument);
    EXPECT_NO_THROW(rom.setReducedOperatorPerMember(std::vector<double>(M * k * k, 0.0)));
}

// ============================================================================
// H13 (2D half) — Manning elasticity dial
// ============================================================================

TEST(DeviationOperator2D, ManningElasticityScalesTheFixedPointExactlyAndOneIsBitIdentical) {
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    std::vector<double> u, v, gw, h;
    h15Fixture(mesh, u, v, gw, h);
    ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
    const int M = 6, k = basis.num_kept;
    std::vector<double> mann(static_cast<std::size_t>(M)), ones(static_cast<std::size_t>(M), 1.0);
    for (int i = 0; i < M; ++i) mann[static_cast<std::size_t>(i)] = 0.8 + 0.4 * (i + 0.5) / M;
    DeviationOperator2D op;
    op.alpha_par = 0.62; op.alpha_perp = 2.0; op.c_factor = 5.0 / 3.0;
    ASSERT_TRUE(op.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data()));

    SpectralROM e1, e1b, e06;
    h15MakeRom(e1,  basis, M, mann, h, nullptr);
    h15MakeRom(e1b, basis, M, mann, h, nullptr);
    h15MakeRom(e06, basis, M, mann, h, nullptr);
    e1.setReducedOperator(op.M);
    e1b.setReducedOperator(op.M);
    e06.setReducedOperator(op.M);
    e1b.manning_elasticity = 1.0;    // explicit 1.0 must equal the default bit-for-bit
    e06.manning_elasticity = 0.6;
    for (int step = 0; step < 400; ++step) {   // run to the fixed point
        e1.advance(60.0, 1.0, nullptr, nullptr, h.data());
        e1b.advance(60.0, 1.0, nullptr, nullptr, h.data());
        e06.advance(60.0, 1.0, nullptr, nullptr, h.data());
    }
    EXPECT_EQ(maxAbsDiff(e1.a_ensemble, e1b.a_ensemble), 0.0);
    // δa* = (mm−1)·e·b: the e = 0.6 trajectory's fixed point is 0.6× the e = 1 one.
    double max_a = 0.0;
    for (double a : e1.a_ensemble) max_a = std::max(max_a, std::fabs(a));
    ASSERT_GT(max_a, 1e-9);
    for (std::size_t i = 0; i < e1.a_ensemble.size(); ++i)
        EXPECT_NEAR(e06.a_ensemble[i], 0.6 * e1.a_ensemble[i], 1e-9 * max_a) << "coef " << i;
}

// ============================================================================
// Two-channel relative mode (2026-10-11): Manning in fractional coordinates
// with the depth-weighted Galerkin operator, additive forcings in absolute
// coordinates with the plain operator.
// ============================================================================

namespace {

struct TwoChannelOps { std::vector<double> plain, rel, ginv, wt; };

TwoChannelOps twoChannelOps(const MeshData& mesh, const MeshEigenBasis& basis,
                            const std::vector<double>& h, const std::vector<double>& u,
                            const std::vector<double>& v, const std::vector<double>& gw) {
    TwoChannelOps o;
    DeviationOperator2D op;
    op.alpha_par = 0.62; op.alpha_perp = 2.0; op.c_factor = 5.0 / 3.0;
    EXPECT_TRUE(op.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data()));
    o.plain = op.M;
    op.similarity_h = h.data();
    EXPECT_TRUE(op.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data()));
    o.rel = op.M; o.ginv = op.Ginv; o.wt = op.GramWeight;
    return o;
}

} // namespace

TEST(DeviationOperator2D, TwoChannelRainOnlyReproducesTheAbsoluteRom) {
    // No Manning spread: everything lives in the additive channel, which IS
    // the absolute ROM (plain operator, plain projection, h_det + P·δa).
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    std::vector<double> u, v, gw, h;
    h15Fixture(mesh, u, v, gw, h);
    ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
    const auto ops = twoChannelOps(mesh, basis, h, u, v, gw);
    const int nt = mesh.n_triangles(), M = 9;
    std::vector<double> ones(static_cast<std::size_t>(M), 1.0), rain_mult(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i) rain_mult[static_cast<std::size_t>(i)] = 0.8 + 0.4 * (i + 0.5) / M;
    std::vector<double> rain(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t) rain[static_cast<std::size_t>(t)] = 1e-5 * (1.0 + 0.5 * std::cos(0.4 * t));

    auto make = [&](SpectralROM& r) {
        r.basis = &basis; r.n_ensemble = M; r.mannings_pert = 0.0; r.rainfall_pert = 0.2;
        r.setExternalSamples(ones, rain_mult); r.initialize(); r.seed(h.data());
    };
    SpectralROM abs_rom, two;
    make(abs_rom); make(two);
    abs_rom.setReducedOperator(ops.plain);
    two.relative_deviation = true;
    two.setReducedOperator(ops.rel); two.setRelativeGram(ops.ginv, ops.wt); two.setAdditiveOperator(ops.plain);
    ASSERT_TRUE(two.twoChannel());
    for (int step = 0; step < 8; ++step) {
        abs_rom.advance(30.0, 1.0, rain.data(), nullptr, h.data());
        two.advance(30.0, 1.0, rain.data(), nullptr, h.data());
    }
    double max_a = 0.0;
    for (double a : abs_rom.a_ensemble) max_a = std::max(max_a, std::fabs(a));
    ASSERT_GT(max_a, 1e-9) << "vacuous: no rain deviation developed";
    EXPECT_LT(maxAbsDiff(abs_rom.a_ensemble, two.a_add), 1e-12 * max_a);
    for (double a : two.a_ensemble) EXPECT_EQ(a, 0.0) << "Manning channel must stay empty";
    abs_rom.computeQuantiles(h.data());
    two.computeQuantiles(h.data());
    EXPECT_LT(maxAbsDiff(abs_rom.q05, two.q05), 1e-12);
    EXPECT_LT(maxAbsDiff(abs_rom.q95, two.q95), 1e-12);
}

TEST(DeviationOperator2D, TwoChannelManningOnlyLeavesTheAdditiveChannelEmpty) {
    // No forcing spread: the additive channel stays exactly zero and the
    // band comes entirely from the fractional Manning channel.
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    std::vector<double> u, v, gw, h;
    h15Fixture(mesh, u, v, gw, h);
    ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
    const auto ops = twoChannelOps(mesh, basis, h, u, v, gw);
    const int M = 9;
    std::vector<double> mann(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i) mann[static_cast<std::size_t>(i)] = 0.8 + 0.4 * (i + 0.5) / M;
    SpectralROM two;
    h15MakeRom(two, basis, M, mann, h, nullptr);
    two.relative_deviation = true;
    two.setReducedOperator(ops.rel); two.setRelativeGram(ops.ginv, ops.wt); two.setAdditiveOperator(ops.plain);
    for (int step = 0; step < 8; ++step) two.advance(30.0, 1.0, nullptr, nullptr, h.data());
    for (double a : two.a_add) EXPECT_EQ(a, 0.0);
    double max_a = 0.0;
    for (double a : two.a_ensemble) max_a = std::max(max_a, std::fabs(a));
    EXPECT_GT(max_a, 1e-9) << "Manning channel did not develop";
    two.computeQuantiles(h.data());
    double spread = 0.0;
    for (std::size_t t = 0; t < two.q05.size(); ++t) spread = std::max(spread, two.q95[t] - two.q05[t]);
    EXPECT_GT(spread, 0.0);
}

TEST(DeviationOperator2D, GalerkinRelativeOperatorReducesToPlainForUniformDepth) {
    // H = c·I ⇒ G = c²·I, A = c²·PᵀLP ⇒ M = PᵀLP: the weighted Galerkin
    // projection is the plain one when depth is uniform.
    auto mesh = makeStructuredMesh(6);
    MeshEigenBasis basis;
    std::vector<double> u, v, gw, h;
    h15Fixture(mesh, u, v, gw, h);
    ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
    std::vector<double> hu(h.size(), 0.12);
    DeviationOperator2D op;
    op.alpha_par = 0.62; op.alpha_perp = 2.0; op.c_factor = 5.0 / 3.0;
    ASSERT_TRUE(op.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data()));
    const auto plain = op.M;
    op.similarity_h = hu.data();
    ASSERT_TRUE(op.assemble(mesh, basis, 1.7, h.data(), u.data(), v.data(), gw.data()));
    double scale = 0.0;
    for (double x : plain) scale = std::max(scale, std::fabs(x));
    EXPECT_LT(maxAbsDiff(plain, op.M), 1e-10 * scale);
}

TEST(DeviationOperator2D, TwoChannelManningTargetDoesNotDependOnCellArea) {
    // The relative Gram is volume-weighted (2026-10-23), so the Manning target
    // G⁻¹·(HP)ᵀ·(h ⊙ 1) must be projected in the same inner product. A plain
    // projection scaled the target by one over the cell area: the W3 plane's
    // 12.5 m² cells gave a band 12.5x narrow (0.109 against 1.360). The same
    // mesh at 10 m and 100 m across (cell areas 100x apart) must give the same
    // target fraction field.
    std::vector<std::vector<double>> frac(2);
    const double domains[2] = {10.0, 100.0};
    for (int d = 0; d < 2; ++d) {
        auto mesh = makeStructuredMesh(6, domains[d]);
        MeshEigenBasis basis;
        std::vector<double> u, v, gw, h;
        h15Fixture(mesh, u, v, gw, h);
        for (int i = 0; i < mesh.n_triangles(); ++i)
            gw[static_cast<std::size_t>(i)] = mesh.tri_cx[static_cast<std::size_t>(i)] < 0.1 * domains[d] ? 0.25 : 0.0;
        ASSERT_TRUE(basis.build(mesh, 8, gw.data()));
        const auto ops = twoChannelOps(mesh, basis, h, u, v, gw);
        const int M = 9;
        std::vector<double> mann(static_cast<std::size_t>(M));
        for (int i = 0; i < M; ++i) mann[static_cast<std::size_t>(i)] = 0.8 + 0.4 * (i + 0.5) / M;
        SpectralROM two;
        h15MakeRom(two, basis, M, mann, h, nullptr);
        two.relative_deviation = true;
        two.setReducedOperator(ops.rel); two.setRelativeGram(ops.ginv, ops.wt); two.setAdditiveOperator(ops.plain);
        two.advance(30.0, 1.0, nullptr, nullptr, h.data());
        const auto nt = static_cast<std::size_t>(mesh.n_triangles());
        frac[static_cast<std::size_t>(d)].assign(nt, 0.0);
        for (int j = 0; j < two.n_kept; ++j)
            for (std::size_t t = 0; t < nt; ++t)
                frac[static_cast<std::size_t>(d)][t] += basis.P[static_cast<std::size_t>(j) * nt + t] * two.b_coarse[static_cast<std::size_t>(j)];
    }
    double mean = 0.0;
    for (double f : frac[0]) mean += f;
    mean /= static_cast<double>(frac[0].size());
    EXPECT_NEAR(mean, 1.0, 0.5) << "the target is the constant fraction 1, projected";
    EXPECT_LT(maxAbsDiff(frac[0], frac[1]), 1e-8);
}
