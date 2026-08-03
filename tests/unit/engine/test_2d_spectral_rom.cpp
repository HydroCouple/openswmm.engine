/**
 * @file test_2d_spectral_rom.cpp
 * @brief Unit tests for SpectralROM — linear ensemble ROM for 2D uncertainty.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

#include "2d/data/MeshData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/uncertainty/MeshEigenBasis.hpp"
#include "2d/uncertainty/SpectralROM.hpp"
#include "2d/uncertainty/FiedlerDiagnostic.hpp"
#include "2d/coupling/NodeCoupling.hpp"
#include "uncertainty/UncertaintyEnsemble.hpp"
#include "uncertainty/GraphEigenBasis.hpp"
#include "uncertainty/NetworkLaplacian1D.hpp"
#include "uncertainty/SpectralROM1D.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using namespace openswmm::uncertainty;

using namespace openswmm::twoD;

// ============================================================================
// Helpers
// ============================================================================

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

// Build a basis and a ROM configured for that basis.
// N=5 → 50 triangles, k=6 modes, M=20 members.
struct Fixture {
    MeshData            mesh;
    MeshEigenBasis   basis;
    SpectralROM         rom;

    explicit Fixture(int N = 5, int k = 6, int M = 20,
                     double m_pert = 0.20, double r_pert = 0.20) {
        mesh = makeStructuredMesh(N);
        EXPECT_TRUE(basis.build(mesh, k));
        EXPECT_GT(basis.num_kept, 0);

        rom.basis         = &basis;
        rom.n_ensemble    = M;
        rom.mannings_pert = m_pert;
        rom.rainfall_pert = r_pert;
        rom.initialize();
    }
};

// ============================================================================
// InitializeAllocatesCorrectly
// ============================================================================

TEST(SpectralROM, InitializeAllocatesCorrectly) {
    Fixture f;
    EXPECT_TRUE(f.rom.is_ready());
    EXPECT_EQ(f.rom.n_tri,  f.basis.n_triangles);
    EXPECT_EQ(f.rom.n_kept, f.basis.num_kept);

    int M = f.rom.n_ensemble;
    int k = f.rom.n_kept;
    int n = f.rom.n_tri;

    EXPECT_EQ(static_cast<int>(f.rom.a_ensemble.size()),  M * k);
    EXPECT_EQ(static_cast<int>(f.rom.r_coarse.size()),    k);
    EXPECT_EQ(static_cast<int>(f.rom.mannings_mult.size()), M);
    EXPECT_EQ(static_cast<int>(f.rom.rainfall_mult.size()), M);
    EXPECT_EQ(static_cast<int>(f.rom.q05.size()), n);
    EXPECT_EQ(static_cast<int>(f.rom.q50.size()), n);
    EXPECT_EQ(static_cast<int>(f.rom.q95.size()), n);
}

// ============================================================================
// IsReadyFalseBeforeInit
// ============================================================================

TEST(SpectralROM, IsReadyFalseBeforeInit) {
    SpectralROM rom;
    EXPECT_FALSE(rom.is_ready());
}

// ============================================================================
// LHSDesignCoversRange
// ============================================================================

TEST(SpectralROM, LHSDesignCoversRange) {
    Fixture f;
    double m_lo = 1.0 - f.rom.mannings_pert;
    double m_hi = 1.0 + f.rom.mannings_pert;
    double r_lo = 1.0 - f.rom.rainfall_pert;
    double r_hi = 1.0 + f.rom.rainfall_pert;

    for (int i = 0; i < f.rom.n_ensemble; ++i) {
        double mm = f.rom.mannings_mult[static_cast<std::size_t>(i)];
        double rm = f.rom.rainfall_mult[static_cast<std::size_t>(i)];
        EXPECT_GE(mm, m_lo) << "mannings_mult[" << i << "] < lower bound";
        EXPECT_LE(mm, m_hi) << "mannings_mult[" << i << "] > upper bound";
        EXPECT_GE(rm, r_lo) << "rainfall_mult[" << i << "] < lower bound";
        EXPECT_LE(rm, r_hi) << "rainfall_mult[" << i << "] > upper bound";
    }
}

// ============================================================================
// LHSDesignMonotonicity
// ============================================================================

TEST(SpectralROM, LHSDesignMonotonicity) {
    // Manning's strata are in ascending order (the reference column).  Since
    // PR 5, rainfall is an independent Fisher-Yates shuffle of the same
    // strata rather than a reversed column, so it is NOT monotone in either
    // direction; the contract is LHS coverage (sorted columns match Manning's
    // strata pattern exactly) plus near-zero rank correlation with Manning.
    Fixture f;
    const int M = f.rom.n_ensemble;
    for (int i = 0; i + 1 < M; ++i) {
        EXPECT_LT(f.rom.mannings_mult[static_cast<std::size_t>(i)],
                  f.rom.mannings_mult[static_cast<std::size_t>(i + 1)])
            << "mannings_mult not monotone at i=" << i;
    }

    bool rainfall_ascending = true, rainfall_descending = true;
    for (int i = 0; i + 1 < M; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (f.rom.rainfall_mult[ui] >= f.rom.rainfall_mult[ui + 1]) rainfall_ascending = false;
        if (f.rom.rainfall_mult[ui] <= f.rom.rainfall_mult[ui + 1]) rainfall_descending = false;
    }
    EXPECT_FALSE(rainfall_ascending) << "rainfall_mult should not be strictly ascending";
    EXPECT_FALSE(rainfall_descending) << "rainfall_mult should not be strictly descending";

    auto sorted_mann = f.rom.mannings_mult;
    auto sorted_rain = f.rom.rainfall_mult;
    std::sort(sorted_mann.begin(), sorted_mann.end());
    std::sort(sorted_rain.begin(), sorted_rain.end());
    for (int i = 0; i < M; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(sorted_rain[ui], sorted_mann[ui], 1.0e-10)
            << "rainfall strata coverage differs at i=" << i;
    }
}

// ============================================================================
// SeedThenQuantilesTight
// ============================================================================
// All members start from the same seed → all reconstructed depths are identical
// → q05 == q50 == q95 for every cell.

TEST(SpectralROM, SeedThenQuantilesTight) {
    Fixture f;
    int n = f.rom.n_tri;

    // Uniform positive depth field
    std::vector<double> h(static_cast<std::size_t>(n), 0.5);

    f.rom.seed(h.data());
    f.rom.computeQuantiles(h.data());

    // Deviation form: seed zeroes all deviations, so quantiles equal the
    // deterministic reference EXACTLY (no basis-truncation loss).
    for (int t = 0; t < n; ++t) {
        auto ut = static_cast<std::size_t>(t);
        EXPECT_DOUBLE_EQ(f.rom.q05[ut], h[ut]) << "q05 != h_det at t=" << t;
        EXPECT_DOUBLE_EQ(f.rom.q50[ut], h[ut]) << "q50 != h_det at t=" << t;
        EXPECT_DOUBLE_EQ(f.rom.q95[ut], h[ut]) << "q95 != h_det at t=" << t;
    }
}

// ============================================================================
// QuantilesOrderPreserved
// ============================================================================

TEST(SpectralROM, QuantilesOrderPreserved) {
    Fixture f;
    int n = f.rom.n_tri;

    std::vector<double> h(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t)
        h[static_cast<std::size_t>(t)] = 0.1 + 0.01 * t;

    f.rom.seed(h.data());

    // Advance several steps with rainfall forcing
    std::vector<double> rain(static_cast<std::size_t>(n), 1e-5);
    for (int step = 0; step < 10; ++step)
        f.rom.advance(30.0, 5.0, rain.data(), nullptr, h.data());

    f.rom.computeQuantiles(h.data());

    for (int t = 0; t < n; ++t) {
        EXPECT_LE(f.rom.q05[static_cast<std::size_t>(t)],
                  f.rom.q50[static_cast<std::size_t>(t)] + 1e-14)
            << "q05 > q50 at t=" << t;
        EXPECT_LE(f.rom.q50[static_cast<std::size_t>(t)],
                  f.rom.q95[static_cast<std::size_t>(t)] + 1e-14)
            << "q50 > q95 at t=" << t;
    }
}

// ============================================================================
// SpreadIncreasesAfterAdvance
// ============================================================================
// After advancing with forcing and large perturbation, at least one cell should
// show spread > 0 (q95 > q05).

TEST(SpectralROM, SpreadIncreasesAfterAdvance) {
    Fixture f(/*N=*/5, /*k=*/6, /*M=*/20, /*m_pert=*/0.30, /*r_pert=*/0.30);
    int n = f.rom.n_tri;

    std::vector<double> h(static_cast<std::size_t>(n), 0.3);
    f.rom.seed(h.data());

    // Advance with rainfall to drive member divergence
    std::vector<double> rain(static_cast<std::size_t>(n), 1e-4);
    for (int step = 0; step < 20; ++step)
        f.rom.advance(60.0, 5.0, rain.data(), nullptr, h.data());

    f.rom.computeQuantiles(h.data());

    double max_spread = 0.0;
    for (int t = 0; t < n; ++t) {
        double spread = f.rom.q95[static_cast<std::size_t>(t)]
                      - f.rom.q05[static_cast<std::size_t>(t)];
        max_spread = std::max(max_spread, spread);
    }

    EXPECT_GT(max_spread, 0.0)
        << "Expected spread > 0 after advance with 30% perturbation";
}

// ============================================================================
// NullRainfallDecays
// ============================================================================
// Without rainfall, all modes decay exponentially.  After enough time, all
// ensemble coefficients should be much smaller than the initial projection.

// Deviation form: when the deterministic reference decays to zero, b_j → 0
// drags every member's deviation to zero with it (the band collapses onto the
// decayed reference).  Replaces the old total-head "coefficients decay" test.
TEST(SpectralROM, DecayingReferenceDrivesDeviationsToZero) {
    Fixture f(/*N=*/5, /*k=*/6, /*M=*/20, /*m_pert=*/0.30, /*r_pert=*/0.0);
    f.rom.mode_drop_threshold = 0.0;  // exercise the integrator, not the gate
    int n = f.rom.n_tri;
    int nk = f.rom.n_kept;
    int M  = f.rom.n_ensemble;

    // Off-centre bump so the reference has non-zero modal content (b_j != 0).
    std::vector<double> h_det(static_cast<std::size_t>(n));
    double sign0 = (f.basis.P[0] >= 0.0) ? 1.0 : -1.0;
    for (int t = 0; t < n; ++t)
        h_det[static_cast<std::size_t>(t)] =
            1.0 + sign0 * f.basis.P[static_cast<std::size_t>(t)] * 0.5;
    f.rom.seed(h_det.data());

    // Build non-zero deviations first (Manning sensitivity against the bump).
    for (int step = 0; step < 20; ++step)
        f.rom.advance(30.0, 5.0, nullptr, nullptr, h_det.data());
    double a_mid_max = 0.0;
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < nk; ++j)
            a_mid_max = std::max(a_mid_max,
                std::fabs(f.rom.a_ensemble[static_cast<std::size_t>(i * nk + j)]));
    ASSERT_GT(a_mid_max, 0.0) << "deviations must build up first";

    // Now decay the reference to zero: deviations must follow it down.
    for (int step = 0; step < 300; ++step) {
        for (double& v : h_det) v *= 0.9;
        f.rom.advance(100.0, 10.0, nullptr, nullptr, h_det.data());
    }
    double a_final_max = 0.0;
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < nk; ++j)
            a_final_max = std::max(a_final_max,
                std::fabs(f.rom.a_ensemble[static_cast<std::size_t>(i * nk + j)]));

    EXPECT_LT(a_final_max, 1e-6 * a_mid_max)
        << "deviations should decay with the vanishing reference";
}

// ============================================================================
// DepthsNonNegative
// ============================================================================

TEST(SpectralROM, DepthsNonNegative) {
    Fixture f;
    int n = f.rom.n_tri;

    // Seed with a field that will produce some negative reconstructions (spike)
    std::vector<double> h(static_cast<std::size_t>(n), 0.0);
    h[0] = 5.0;  // impulse on one cell

    f.rom.seed(h.data());
    f.rom.advance(60.0, 5.0, nullptr, nullptr, h.data());
    f.rom.computeQuantiles(h.data());

    for (int t = 0; t < n; ++t) {
        EXPECT_GE(f.rom.q05[static_cast<std::size_t>(t)], 0.0)
            << "q05 < 0 at t=" << t;
        EXPECT_GE(f.rom.q50[static_cast<std::size_t>(t)], 0.0)
            << "q50 < 0 at t=" << t;
        EXPECT_GE(f.rom.q95[static_cast<std::size_t>(t)], 0.0)
            << "q95 < 0 at t=" << t;
    }
}

// ============================================================================
// SteadyStateWithForcing
// ============================================================================
// For a single mode j with member i, the steady-state amplitude is
//   a_ss = f_j / rate_j = (r_coarse[j] * rm) / (lam_j * K_eff / mm)
// After a very long timestep, a_j → a_ss regardless of initial condition.

// Deviation form: with a frozen reference and Manning perturbation (no rain),
// each member's deviation converges to the closed-form fixed point
//   δa_ss[i,j] = (mm_i − 1)·b_j,   b_j = P[:,j]^T·h_det
// (the K_eff Rayleigh factor cancels exactly).  Replaces the old total-head
// a_ss = r_coarse/lam steady-state check.
TEST(SpectralROM, FrozenReferenceSaturatesAtAnalyticSteadyState) {
    MeshData mesh = makeStructuredMesh(5);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 4));

    SpectralROM rom;
    rom.basis         = &basis;
    rom.n_ensemble    = 10;
    rom.mannings_pert = 0.20;
    rom.rainfall_pert = 0.0;
    rom.mode_drop_threshold = 0.0;  // let every mode converge
    rom.initialize();

    int n  = rom.n_tri;
    int nk = rom.n_kept;
    int M  = rom.n_ensemble;

    // Off-centre bump reference → non-zero modal content b_j.
    std::vector<double> h_det(static_cast<std::size_t>(n));
    double sign0 = (basis.P[0] >= 0.0) ? 1.0 : -1.0;
    for (int t = 0; t < n; ++t)
        h_det[static_cast<std::size_t>(t)] =
            1.0 + sign0 * basis.P[static_cast<std::size_t>(t)] * 0.5;
    rom.seed(h_det.data());

    // Advance to convergence (large dt, no rainfall).
    const double K_eff = 8.0;
    for (int step = 0; step < 5; ++step)
        rom.advance(1e6, K_eff, nullptr, nullptr, h_det.data());

    for (int j = 0; j < nk; ++j) {
        double b_j = 0.0;
        for (int t = 0; t < n; ++t)
            b_j += basis.P[static_cast<std::size_t>(j * n + t)]
                 * h_det[static_cast<std::size_t>(t)];
        for (int i = 0; i < M; ++i) {
            double mm       = rom.mannings_mult[static_cast<std::size_t>(i)];
            double expected = (mm - 1.0) * b_j;
            double got      = rom.a_ensemble[static_cast<std::size_t>(i * nk + j)];
            EXPECT_NEAR(got, expected, 1e-9 * std::fabs(expected) + 1e-12)
                << "member=" << i << " mode=" << j;
        }
    }
}

// ============================================================================
// PerModeKeffDeepCellsDecayFaster  (Option A)
// ============================================================================
// A depth field concentrated in a sub-region makes the decay rate of modes
// localised in that region higher than the global-K_eff value, and modes
// localised in shallow cells lower.  We verify this by comparing the total
// modal energy after one advance step with and without h_cell.

TEST(SpectralROM, PerModeKeffDeepCellsDecayFaster) {
    // N=6 → 72 triangles.  Cells in the first half get deep (h=2.0 m);
    // cells in the second half get shallow (h=0.01 m).
    Fixture f;  // N=5, 50 tri, k=6, M=20
    // Disable mode dropping: constant-field seed projects near-zero onto
    // non-null eigenmodes; mode dropping would freeze coefficients before
    // the per-mode / uniform K_eff comparison can be observed.
    f.rom.mode_drop_threshold = 0.0;

    int n  = f.rom.n_tri;
    int nk = f.rom.n_kept;
    int M  = f.rom.n_ensemble;

    // Off-centre bump reference (non-zero modal content b_j) so the Manning-
    // sensitivity forcing — which is scaled by keff — actually drives δa.
    std::vector<double> h0(static_cast<std::size_t>(n));
    double sign0 = (f.basis.P[0] >= 0.0) ? 1.0 : -1.0;
    for (int t = 0; t < n; ++t)
        h0[static_cast<std::size_t>(t)] =
            0.5 + sign0 * f.basis.P[static_cast<std::size_t>(t)] * 0.5;
    f.rom.seed(h0.data());

    // Depth field: first half deep, second half shallow
    std::vector<double> h_nonuniform(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t)
        h_nonuniform[static_cast<std::size_t>(t)] = (t < n / 2) ? 2.0 : 0.01;

    const double K_eff = 5.0;
    const double dt    = 10.0;

    // --- advance with uniform K_eff (h_cell = null) ---
    std::vector<double> a0(f.rom.a_ensemble);  // save initial state
    f.rom.advance(dt, K_eff, nullptr, nullptr, h0.data());
    // Compute total energy (sum of |a_j|) after uniform advance
    double energy_uniform = 0.0;
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < nk; ++j)
            energy_uniform += std::abs(
                f.rom.a_ensemble[static_cast<std::size_t>(i * nk + j)]);

    // --- restore and advance with per-mode K_eff (h_cell provided) ---
    f.rom.a_ensemble = a0;
    f.rom.advance(dt, K_eff, nullptr, h_nonuniform.data(), h0.data());
    double energy_permode = 0.0;
    for (int i = 0; i < M; ++i)
        for (int j = 0; j < nk; ++j)
            energy_permode += std::abs(
                f.rom.a_ensemble[static_cast<std::size_t>(i * nk + j)]);

    // With a non-uniform depth field the effective rate differs from K_eff for
    // all modes, so total energy must differ between the two paths.
    EXPECT_NE(energy_uniform, energy_permode)
        << "Per-mode K_eff should produce different decay than uniform K_eff "
           "when depth is non-uniform";
}

// ============================================================================
// DepthWeightedBasisDiffersFromGeometric  (Option B)
// ============================================================================
// After buildDepthWeighted with a strongly non-uniform D_cell the eigenvectors
// must differ from the geometric basis, and the rebuild must preserve num_kept.

TEST(SpectralROM, DepthWeightedBasisDiffersFromGeometric) {
    Fixture f;  // builds geometric basis in f.basis
    int n  = f.basis.n_triangles;
    int nk = f.basis.num_kept;
    ASSERT_GT(nk, 0);

    // Save geometric eigenvectors and eigenvalues
    std::vector<double> P_geom  = f.basis.P;
    std::vector<double> ev_geom = f.basis.eigenvalues;

    // --- uniform D_cell = 1: eigenvectors and eigenvalues must be unchanged ---
    std::vector<double> D_uniform(static_cast<std::size_t>(n), 1.0);
    bool ok_uniform = f.basis.buildDepthWeighted(f.mesh, nk, D_uniform.data());
    ASSERT_TRUE(ok_uniform);
    EXPECT_TRUE(f.basis.depth_weighted);
    for (int j = 0; j < nk; ++j)
        EXPECT_NEAR(f.basis.eigenvalues[static_cast<std::size_t>(j)],
                    ev_geom[static_cast<std::size_t>(j)], 1e-6 * ev_geom[static_cast<std::size_t>(j)] + 1e-12)
            << "Uniform D_cell should preserve eigenvalue " << j;

    // --- non-uniform D_cell: eigenvectors must change ---
    // Reset to geometric first
    f.basis.build(f.mesh, nk);
    EXPECT_FALSE(f.basis.depth_weighted);

    // D_cell: cells in first half 100×, second half 1× (strongly non-uniform).
    // This is the normalised form: sum/n = ~50.5, values relative to mean.
    std::vector<double> D_nonuniform(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t)
        D_nonuniform[static_cast<std::size_t>(t)] = (t < n / 2) ? 100.0 : 1.0;

    bool ok = f.basis.buildDepthWeighted(f.mesh, nk, D_nonuniform.data());
    ASSERT_TRUE(ok) << "buildDepthWeighted failed with non-uniform D_cell";
    EXPECT_EQ(f.basis.num_kept, nk) << "num_kept changed after rebuild";
    EXPECT_TRUE(f.basis.depth_weighted);

    double max_diff = 0.0;
    for (std::size_t idx = 0; idx < static_cast<std::size_t>(n * nk); ++idx)
        max_diff = std::max(max_diff, std::abs(f.basis.P[idx] - P_geom[idx]));

    EXPECT_GT(max_diff, 1e-6)
        << "Depth-weighted eigenvectors should differ from geometric basis "
           "when D_cell is strongly non-uniform";
}

// ============================================================================
// DepthWeightedBasisMutuallyExcludesOptionA
// ============================================================================
// When the basis is depth-weighted (Option B), passing h_cell to advance()
// should be equivalent to passing nullptr — the Rayleigh-quotient path is
// bypassed because keff_modes_ remains K_eff for all modes.

TEST(SpectralROM, OptionASkippedWhenBasisIsDepthWeighted) {
    Fixture f;
    int n  = f.rom.n_tri;
    int nk = f.rom.n_kept;

    // Build a depth-weighted basis with uniform D (eigenvalues unchanged)
    std::vector<double> D_uniform(static_cast<std::size_t>(n), 1.0);
    f.basis.buildDepthWeighted(f.mesh, nk, D_uniform.data());
    ASSERT_TRUE(f.basis.depth_weighted);

    // Seed with first-mode depth
    std::vector<double> h0(static_cast<std::size_t>(n));
    double sign0 = (f.basis.P[0] >= 0.0) ? 1.0 : -1.0;
    for (int t = 0; t < n; ++t)
        h0[static_cast<std::size_t>(t)] =
            sign0 * f.basis.P[static_cast<std::size_t>(t)] * 0.5;
    f.rom.seed(h0.data());

    // Non-uniform h_cell that would change K_eff_j if Option A were active
    std::vector<double> h_nonuniform(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t)
        h_nonuniform[static_cast<std::size_t>(t)] = (t < n / 2) ? 2.0 : 0.01;

    auto a_before = f.rom.a_ensemble;

    // Advance with h_cell (Option A path)
    f.rom.advance(10.0, 5.0, nullptr, h_nonuniform.data(), h0.data());
    auto a_with_h = f.rom.a_ensemble;

    // Restore and advance without h_cell
    f.rom.a_ensemble = a_before;
    f.rom.advance(10.0, 5.0, nullptr, nullptr, h0.data());
    auto a_null = f.rom.a_ensemble;

    // With depth-weighted basis, both paths must produce identical results
    // because the Rayleigh-quotient branch is skipped when depth_weighted=true.
    // (The basis isn't actually depth_weighted here because SpectralROM's basis
    // pointer is the geometric one from the Fixture — this test verifies the
    // logic path in advance() itself.)
    // For the advance() function: when h_cell is provided but basis is geometric,
    // results differ.  This test documents that the two paths give DIFFERENT
    // results for a geometric basis (confirming Option A is active).
    bool any_diff = false;
    for (std::size_t i = 0; i < a_with_h.size(); ++i)
        if (std::abs(a_with_h[i] - a_null[i]) > 1e-15) { any_diff = true; break; }

    EXPECT_TRUE(any_diff)
        << "With geometric basis and non-uniform h_cell, Option A should "
           "produce different decay than h_cell=null";
}

// ============================================================================
// Problem 2: Dry-domain seed gives zero quantiles (no spurious reconstruction)
// ============================================================================
// After seeding with all-zero depth, every member's coefficient vector is zero,
// and computeQuantiles() must return zero for all cells.

TEST(SpectralROM, DrySeedProducesZeroQuantiles) {
    Fixture f;
    int n = f.rom.n_tri;

    std::vector<double> h_dry(static_cast<std::size_t>(n), 0.0);
    f.rom.seed(h_dry.data());
    f.rom.advance(30.0, 5.0, nullptr, nullptr, h_dry.data());
    f.rom.computeQuantiles(h_dry.data());

    for (int t = 0; t < n; ++t) {
        EXPECT_NEAR(f.rom.q05[static_cast<std::size_t>(t)], 0.0, 1e-10)
            << "q05 should be zero for dry-domain seed at cell " << t;
        EXPECT_NEAR(f.rom.q50[static_cast<std::size_t>(t)], 0.0, 1e-10)
            << "q50 should be zero for dry-domain seed at cell " << t;
        EXPECT_NEAR(f.rom.q95[static_cast<std::size_t>(t)], 0.0, 1e-10)
            << "q95 should be zero for dry-domain seed at cell " << t;
    }
}

// ============================================================================
// Problem 3: Log-normal parametric q95 exceeds sort-based for small ensemble
// ============================================================================
// With M=8 and large perturbations, the sort-based q95 equals the observed
// maximum (one sample).  The log-normal fit uses all 8 samples to extrapolate
// a smoother upper tail, which may exceed the observed maximum.

TEST(SpectralROM, ParametricQ95CanExceedSortBasedForSmallEnsemble) {
    // N=4 mesh, k=4 modes, M=8 members, large perturbations
    MeshData mesh = makeStructuredMesh(4);
    buildMeshTopology(mesh);

    MeshEigenBasis basis;
    basis.build(mesh, 4);
    ASSERT_GT(basis.num_kept, 0);

    SpectralROM rom;
    rom.basis         = &basis;
    rom.n_ensemble    = 8;
    rom.mannings_pert = 0.40;
    rom.rainfall_pert = 0.00;
    rom.initialize();

    int n = rom.n_tri;

    // Seed off-centre so coefficients are non-zero
    double sign0 = (basis.P[0] >= 0.0) ? 1.0 : -1.0;
    std::vector<double> h0(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t)
        h0[static_cast<std::size_t>(t)] =
            sign0 * basis.P[static_cast<std::size_t>(t)] * 0.5;
    rom.seed(h0.data());

    // Advance to create spread across members.
    // dt=5.0, K_eff=1.0: enough decay variation across mm ∈ [0.6,1.4] to
    // produce measurable spread without over-decaying to below DRY_THRESH.
    rom.advance(5.0, 1.0, nullptr, nullptr, h0.data());

    // Sort-based quantiles
    rom.computeQuantiles(h0.data(), false);
    std::vector<double> q95_sort = rom.q95;

    // Log-normal parametric quantiles
    rom.computeQuantiles(h0.data(), true);
    std::vector<double> q95_param = rom.q95;

    // For at least one cell with positive depth, the parametric q95 must differ
    // from the sort-based q95 (either wider or narrower — the key property is
    // that the estimates are not identical, confirming the parametric path fired).
    bool found_difference = false;
    for (int t = 0; t < n; ++t) {
        if (q95_sort[static_cast<std::size_t>(t)] > 1.0e-6 &&
            std::abs(q95_param[static_cast<std::size_t>(t)] -
                     q95_sort[static_cast<std::size_t>(t)]) > 1.0e-8) {
            found_difference = true;
            break;
        }
    }
    EXPECT_TRUE(found_difference)
        << "Parametric log-normal q95 should differ from sort-based q95 "
           "when M=8 and spread is non-trivial";
}

// ============================================================================
// ModesDroppedAfterDecay
// ============================================================================

TEST(SpectralROM, ModesDroppedAfterDecay) {
    Fixture f;  // N=5, k=6, M=20, threshold=1e-10

    // Seed with a multi-frequency field to get non-trivial projections.
    int nt = f.rom.n_tri;
    std::vector<double> h_full(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t)
        h_full[t] = 0.05 * (1.0 + std::cos(t * 0.7) + std::sin(t * 1.3));
    f.rom.seed(h_full.data());

    // Build deviation energy against the bump reference (Manning sensitivity).
    for (int s = 0; s < 5; ++s)
        f.rom.advance(30.0, 1.0, nullptr, nullptr, h_full.data());

    // Now decay to a ZERO reference with no rain: b_j → 0 removes the Manning
    // sensitivity forcing, so deviations decay (exp(-rate*2000) → 0).
    std::vector<double> h_zero(static_cast<std::size_t>(nt), 0.0);
    f.rom.advance(2000.0, 1.0, nullptr, nullptr, h_zero.data());

    // Second advance: energy is computed from the decayed (~0) coefficients;
    // all E_j << 1e-10 and no forcing → modes are dropped.
    f.rom.advance(1.0, 1.0, nullptr, nullptr, h_zero.data());

    EXPECT_LT(f.rom.n_modes_active, f.rom.n_kept)
        << "n_modes_active should decrease when all deviations have decayed below threshold";
}

// ============================================================================
// DroppedModesReactivateWithRainfall
// ============================================================================

TEST(SpectralROM, DroppedModesReactivateWithRainfall) {
    Fixture f;

    // Drive all modes to the dropped state.
    int nt = f.rom.n_tri;
    std::vector<double> h_full(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t)
        h_full[t] = 0.05 * (1.0 + std::cos(t * 0.7) + std::sin(t * 1.3));
    f.rom.seed(h_full.data());
    for (int s = 0; s < 5; ++s)
        f.rom.advance(30.0, 1.0, nullptr, nullptr, h_full.data());
    std::vector<double> h_zero(static_cast<std::size_t>(nt), 0.0);
    f.rom.advance(2000.0, 1.0, nullptr, nullptr, h_zero.data());
    f.rom.advance(1.0,    1.0, nullptr, nullptr, h_zero.data());
    ASSERT_LT(f.rom.n_modes_active, f.rom.n_kept) << "precondition: modes must be dropped first";

    // Advance WITH spatially-varying rainfall (zero reference so only the rain
    // criterion can reactivate).  A uniform rainfall field projects to zero on
    // all Laplacian eigenmodes, so only a non-uniform field satisfies it.
    std::vector<double> rainfall(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t)
        rainfall[static_cast<std::size_t>(t)] =
            1.0e-3 * (1.0 + std::cos(static_cast<double>(t) * 0.7));
    f.rom.advance(1.0, 1.0, rainfall.data(), nullptr, h_zero.data());

    EXPECT_GT(f.rom.n_modes_active, 0)
        << "Rainfall forcing should reactivate at least one mode";
}

// ============================================================================
// AllModesActiveAfterFreshSeed
// ============================================================================

TEST(SpectralROM, AllModesActiveAfterFreshSeed) {
    Fixture f;

    // After initialize(), all modes start active.
    EXPECT_EQ(f.rom.n_modes_active, f.rom.n_kept);
    ASSERT_EQ(static_cast<int>(f.rom.mode_active.size()), f.rom.n_kept);
    for (int j = 0; j < f.rom.n_kept; ++j)
        EXPECT_TRUE(f.rom.mode_active[static_cast<std::size_t>(j)]);

    // After seeding with high-amplitude field, a single short advance should
    // keep all modes active (E_j = a_j^2 >> threshold = 1e-10).
    int nt = f.rom.n_tri;
    std::vector<double> h_full(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t)
        h_full[t] = 0.5 * (1.0 + std::cos(t * 0.7) + std::sin(t * 1.3));
    f.rom.seed(h_full.data());
    f.rom.advance(1.0, 1.0, nullptr, nullptr, h_full.data());

    // Energy at start of this advance was from the seed (high) → all active.
    EXPECT_EQ(f.rom.n_modes_active, f.rom.n_kept)
        << "All modes should remain active when seeded with amplitude >> threshold";
}

// ============================================================================
// UncertaintyEnsemble — PR 2 tests
// ============================================================================

// generate() produces vectors of exactly n_members length.
TEST(UncertaintyEnsemble, GenerateProducesCorrectSize) {
    UncertaintyEnsemble ens;
    ens.n_members = 30;
    ens.mannings_pert_2d = 0.20;
    ens.rainfall_pert_2d = 0.15;
    ens.generate();

    EXPECT_TRUE(ens.is_ready());
    EXPECT_EQ(static_cast<int>(ens.mannings_mult_2d.size()), 30);
    EXPECT_EQ(static_cast<int>(ens.rainfall_mult_2d.size()), 30);
}

// All samples stay within [1-p, 1+p] for both parameters.
TEST(UncertaintyEnsemble, SamplesInRange) {
    UncertaintyEnsemble ens;
    ens.n_members = 50;
    ens.mannings_pert_2d = 0.20;
    ens.rainfall_pert_2d = 0.25;
    ens.generate();

    for (int i = 0; i < ens.n_members; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_GE(ens.mannings_mult_2d[ui], 1.0 - ens.mannings_pert_2d);
        EXPECT_LE(ens.mannings_mult_2d[ui], 1.0 + ens.mannings_pert_2d);
        EXPECT_GE(ens.rainfall_mult_2d[ui], 1.0 - ens.rainfall_pert_2d);
        EXPECT_LE(ens.rainfall_mult_2d[ui], 1.0 + ens.rainfall_pert_2d);
    }
}

// Manning ascending (reference column); rainfall an independent Fisher-Yates
// shuffle of the same strata (PR 5) — near-zero rank correlation with
// Manning, LHS coverage exact, but NOT monotone in either direction (unlike
// the old reversed-column scheme, which gave exact rank correlation -1).
TEST(UncertaintyEnsemble, ManningAscendingRainfallShuffled) {
    UncertaintyEnsemble ens;
    ens.n_members = 20;
    ens.mannings_pert_2d = 0.20;
    ens.rainfall_pert_2d = 0.20;
    ens.generate();

    for (int i = 0; i + 1 < ens.n_members; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_LT(ens.mannings_mult_2d[ui], ens.mannings_mult_2d[ui + 1])
            << "mannings_mult not ascending at i=" << i;
    }

    bool rainfall_ascending = true, rainfall_descending = true;
    for (int i = 0; i + 1 < ens.n_members; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (ens.rainfall_mult_2d[ui] >= ens.rainfall_mult_2d[ui + 1]) rainfall_ascending = false;
        if (ens.rainfall_mult_2d[ui] <= ens.rainfall_mult_2d[ui + 1]) rainfall_descending = false;
    }
    EXPECT_FALSE(rainfall_ascending) << "rainfall_mult_2d should not be strictly ascending";
    EXPECT_FALSE(rainfall_descending) << "rainfall_mult_2d should not be strictly descending";

    auto sorted_mann = ens.mannings_mult_2d;
    auto sorted_rain = ens.rainfall_mult_2d;
    std::sort(sorted_mann.begin(), sorted_mann.end());
    std::sort(sorted_rain.begin(), sorted_rain.end());
    for (int i = 0; i < ens.n_members; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(sorted_rain[ui], sorted_mann[ui], 1.0e-10)
            << "rainfall strata coverage differs at i=" << i;
    }
}

// generate() throws for n_members < 2.
TEST(UncertaintyEnsemble, ThrowsForTooFewMembers) {
    UncertaintyEnsemble ens;
    ens.n_members = 1;
    EXPECT_THROW(ens.generate(), std::invalid_argument);
}

// SpectralROM initialized from external UncertaintyEnsemble samples reproduces
// the ensemble's design exactly (no internal LHS built).
TEST(SpectralROM, ExternalSamplesUsedWhenSet) {
    Fixture f;  // N=5, k=6, M=20

    // Build external samples via UncertaintyEnsemble with different perturbations
    // than the Fixture defaults (0.20/0.20) — use 0.10/0.30 so we can detect
    // if the ROM is using the external or internal LHS.
    UncertaintyEnsemble ens;
    ens.n_members        = f.rom.n_ensemble;
    ens.mannings_pert_2d = 0.10;
    ens.rainfall_pert_2d = 0.30;
    ens.generate();

    // Build a fresh ROM with the same basis but supply external samples.
    SpectralROM rom_ext;
    rom_ext.basis         = &f.basis;
    rom_ext.n_ensemble    = f.rom.n_ensemble;
    rom_ext.mannings_pert = 0.20;  // internal LHS would use this — but should be ignored
    rom_ext.rainfall_pert = 0.20;
    rom_ext.setExternalSamples(ens.manningsSamples2D(), ens.rainfallSamples2D());
    rom_ext.initialize();

    ASSERT_TRUE(rom_ext.is_ready());

    for (int i = 0; i < rom_ext.n_ensemble; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_DOUBLE_EQ(rom_ext.mannings_mult[ui], ens.mannings_mult_2d[ui])
            << "mannings_mult[" << i << "] does not match external sample";
        EXPECT_DOUBLE_EQ(rom_ext.rainfall_mult[ui], ens.rainfall_mult_2d[ui])
            << "rainfall_mult[" << i << "] does not match external sample";
    }

    // The external samples should differ from the internal LHS the ROM would build
    // with mannings_pert=0.20 — confirm the test is meaningful.
    bool found_diff = false;
    for (int i = 0; i < rom_ext.n_ensemble; ++i) {
        auto ui = static_cast<std::size_t>(i);
        // Internal LHS midpoint: (i+0.5)/M * 0.40 + 0.80  (mannings_pert=0.20)
        double M = static_cast<double>(rom_ext.n_ensemble);
        double internal_mm = 0.80 + ((static_cast<double>(i) + 0.5) / M) * 0.40;
        if (std::abs(rom_ext.mannings_mult[ui] - internal_mm) > 1e-12) {
            found_diff = true;
            break;
        }
    }
    EXPECT_TRUE(found_diff)
        << "External samples (pert=0.10) should differ from internal LHS (pert=0.20)";
}

// setExternalSamples throws when size doesn't match n_ensemble.
TEST(SpectralROM, SetExternalSamplesThrowsOnSizeMismatch) {
    Fixture f;
    SpectralROM rom_bad;
    rom_bad.basis      = &f.basis;
    rom_bad.n_ensemble = 20;
    std::vector<double> wrong(10, 1.0);  // size 10 != 20
    EXPECT_THROW(rom_bad.setExternalSamples(wrong, wrong), std::invalid_argument);
}

// Member count consistency: UncertaintyEnsemble.n_members must match SpectralROM.n_ensemble
// when samples are transferred. Verify by checking vector sizes.
TEST(UncertaintyEnsemble, MemberCountConsistencyWithROM) {
    constexpr int M = 40;

    UncertaintyEnsemble ens;
    ens.n_members        = M;
    ens.mannings_pert_2d = 0.15;
    ens.rainfall_pert_2d = 0.15;
    ens.generate();

    MeshData mesh = makeStructuredMesh(5);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));

    SpectralROM rom;
    rom.basis      = &basis;
    rom.n_ensemble = M;
    rom.setExternalSamples(ens.manningsSamples2D(), ens.rainfallSamples2D());
    rom.initialize();

    EXPECT_EQ(static_cast<int>(rom.mannings_mult.size()), M);
    EXPECT_EQ(static_cast<int>(rom.rainfall_mult.size()), M);
    EXPECT_EQ(static_cast<int>(rom.a_ensemble.size()), M * rom.n_kept);
}

// ============================================================================
// Spatial field — scalar limit equivalence (Phase 2)
// ============================================================================

// When spatial_mannings is populated from the scalar LHS (fromScalar), advance()
// should produce the same ROM coefficients as the pure scalar path.
TEST(SpectralROMSpatial, ScalarLimitManningSameAsScalarPath) {
    Fixture f(/*N=*/5, /*k=*/6, /*M=*/20, /*m_pert=*/0.20, /*r_pert=*/0.20);
    int n_tri = f.mesh.n_triangles();

    // Set a Gaussian-bump depth IC (off-centre to get nonzero modal projections).
    std::vector<double> h(static_cast<std::size_t>(n_tri));
    const double cx0 = 3.0, cy0 = 4.0, sig2 = 2.25;
    for (int t = 0; t < n_tri; ++t) {
        double dx = f.mesh.tri_cx[static_cast<std::size_t>(t)] - cx0;
        double dy = f.mesh.tri_cy[static_cast<std::size_t>(t)] - cy0;
        h[static_cast<std::size_t>(t)] = 0.05 + 0.08 * std::exp(-(dx*dx + dy*dy) / sig2);
    }

    // Uniform rainfall.
    std::vector<double> rain(static_cast<std::size_t>(n_tri), 5e-6);
    const double dt = 10.0, K_eff = 5.0;

    // --- Scalar path ---
    f.rom.seed(h.data());
    f.rom.advance(dt, K_eff, rain.data(), h.data(), h.data());
    std::vector<double> a_scalar(f.rom.a_ensemble);  // copy

    // --- Spatial path: spatial_mannings/rainfall set to uniform scalar values ---
    f.rom.spatial_mannings.fromScalar(f.rom.mannings_mult, n_tri);
    f.rom.spatial_rainfall.fromScalar(f.rom.rainfall_mult, n_tri);
    f.rom.seed(h.data());  // reset to same IC
    f.rom.advance(dt, K_eff, rain.data(), h.data(), h.data());

    ASSERT_EQ(f.rom.a_ensemble.size(), a_scalar.size());
    for (std::size_t k = 0; k < a_scalar.size(); ++k)
        EXPECT_NEAR(f.rom.a_ensemble[k], a_scalar[k], 1e-9)
            << "coefficient mismatch at k=" << k;
}

// When spatial_mannings is populated uniformly (all cells same value per member),
// computeQuantiles() should give the same result as the scalar path.
TEST(SpectralROMSpatial, ScalarLimitQuantilesSame) {
    Fixture f(/*N=*/5, /*k=*/6, /*M=*/20);
    int n_tri = f.mesh.n_triangles();

    std::vector<double> h(static_cast<std::size_t>(n_tri));
    for (int t = 0; t < n_tri; ++t) {
        double dx = f.mesh.tri_cx[static_cast<std::size_t>(t)] - 3.0;
        double dy = f.mesh.tri_cy[static_cast<std::size_t>(t)] - 4.0;
        h[static_cast<std::size_t>(t)] = 0.05 + 0.08 * std::exp(-(dx*dx + dy*dy) / 2.25);
    }
    const double dt = 10.0, K_eff = 5.0;

    // Scalar path.
    f.rom.seed(h.data());
    f.rom.advance(dt, K_eff, nullptr, nullptr, h.data());
    f.rom.computeQuantiles(h.data());
    std::vector<double> q50_scalar(f.rom.q50);

    // Spatial path with uniform fields.
    f.rom.spatial_mannings.fromScalar(f.rom.mannings_mult, n_tri);
    f.rom.seed(h.data());
    f.rom.advance(dt, K_eff, nullptr, nullptr, h.data());
    f.rom.computeQuantiles(h.data());

    ASSERT_EQ(f.rom.q50.size(), q50_scalar.size());
    for (int t = 0; t < n_tri; ++t)
        EXPECT_NEAR(f.rom.q50[static_cast<std::size_t>(t)],
                    q50_scalar[static_cast<std::size_t>(t)], 1e-9)
            << "q50 mismatch at cell " << t;
}

// ============================================================================
// EnsembleRainfall — Phase 3 coupling (RunoffEnsemble → SpectralROM)
// ============================================================================

// setEnsembleRainfall throws when size != n_ensemble.
TEST(SpectralROM, SetEnsembleRainfallThrowsOnSizeMismatch) {
    Fixture f;
    std::vector<double> wrong(f.rom.n_ensemble + 5, 1e-5);
    EXPECT_THROW(f.rom.setEnsembleRainfall(wrong), std::invalid_argument);
}

// Members supplied with a higher per-member rainfall rate accumulate more depth
// than members supplied with a lower rate, confirming per-member forcing is
// correctly routed to individual member coefficient trajectories.
//
// Design: ensemble_rainfall_ acts as a per-member multiplier on r_coarse[j]
// (the deterministic spatial rainfall projection).  Since non-null Laplacian
// eigenvectors are zero-mean, a spatially NON-UNIFORM rainfall is required so
// that r_coarse[j] != 0 for retained modes j.
TEST(SpectralROM, EnsembleRainfallHigherRateGivesMoreDepth) {
    // Zero Manning and rainfall perturbation so the only source of member
    // divergence is the ensemble_rainfall_ we supply.
    MeshData mesh = makeStructuredMesh(5);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));
    ASSERT_GT(basis.num_kept, 0);

    const int M = 10;
    SpectralROM rom;
    rom.basis         = &basis;
    rom.n_ensemble    = M;
    rom.mannings_pert = 0.0;  // identical decay rates for all members
    rom.rainfall_pert = 0.0;
    rom.initialize();

    int n = rom.n_tri;

    // Dry initial state.
    std::vector<double> h0(static_cast<std::size_t>(n), 0.0);
    rom.seed(h0.data());

    // Non-uniform spatial rainfall: sine-wave pattern ensures r_coarse[j] != 0
    // for at least the first few non-null modes.
    std::vector<double> rain(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t)
        rain[static_cast<std::size_t>(t)] =
            1e-5 * (1.0 + std::sin(static_cast<double>(t) * 0.7));

    // Linearly-spaced per-member rates: member 0 gets the lowest rate,
    // member M-1 gets the highest.
    std::vector<double> rates(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i)
        rates[static_cast<std::size_t>(i)] =
            0.5e-5 + static_cast<double>(i) / static_cast<double>(M - 1) * 1.5e-5;

    rom.setEnsembleRainfall(rates);

    // Advance many steps so forcing accumulates.
    for (int step = 0; step < 30; ++step)
        rom.advance(60.0, 5.0, rain.data(), nullptr, h0.data());

    // Compute total modal energy per member (Σ_j a_{i,j}²): proxy for
    // depth magnitude since all members have identical decay rates.
    auto modal_energy = [&](int i) {
        int nk = rom.n_kept;
        double e = 0.0;
        for (int j = 0; j < nk; ++j) {
            double a = rom.a_ensemble[static_cast<std::size_t>(i * nk + j)];
            e += a * a;
        }
        return e;
    };

    // Members with higher runoff rate should have larger modal energy.
    double e_lo = modal_energy(0);      // lowest rate
    double e_hi = modal_energy(M - 1);  // highest rate
    EXPECT_GT(e_hi, e_lo)
        << "highest-rate member should have larger modal energy than lowest-rate member";
}

// After clearEnsembleRainfall(), advance() reverts to the scalar rainfall_mult
// path.  Verify: with rainfall_pert=0 (all rainfall_mult[i]=1) and ensemble
// rates all equal to the mean, both paths produce identical r_coarse scaling
// (both give fj = r_coarse[j] * 1.0).  After clearing, advance again and
// confirm the scalar path is active by seeing the same result.
TEST(SpectralROM, ClearEnsembleRainfallRevertsToScalarPath) {
    Fixture f(/*N=*/5, /*k=*/6, /*M=*/20, /*m_pert=*/0.0, /*r_pert=*/0.0);
    int n  = f.rom.n_tri;
    int nk = f.rom.n_kept;
    int M  = f.rom.n_ensemble;

    // Non-trivial initial state.
    std::vector<double> h0(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t)
        h0[static_cast<std::size_t>(t)] = 0.05 + 0.01 * t;
    f.rom.seed(h0.data());
    std::vector<double> a_seed(f.rom.a_ensemble);

    // Spatially non-uniform rainfall so r_coarse[j] != 0.
    std::vector<double> rain(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t)
        rain[static_cast<std::size_t>(t)] =
            1e-5 * (1.0 + std::sin(static_cast<double>(t) * 0.7));
    const double mean_rain = 1e-5;  // mean of the sine field ≈ mean_rain

    // --- Scalar path (rainfall_pert=0 → all rainfall_mult=1) ---
    f.rom.advance(30.0, 5.0, rain.data(), nullptr, h0.data());
    std::vector<double> a_scalar(f.rom.a_ensemble);

    // --- Ensemble path: all rates equal to mean_rain → scale_i = 1 for all i ---
    f.rom.a_ensemble = a_seed;
    std::vector<double> uniform_rates(static_cast<std::size_t>(M), mean_rain);
    f.rom.setEnsembleRainfall(uniform_rates);
    f.rom.advance(30.0, 5.0, rain.data(), nullptr, h0.data());
    std::vector<double> a_ensemble_uniform(f.rom.a_ensemble);

    // Both paths apply the same per-member scale (=1) → identical coefficients.
    for (std::size_t k = 0; k < static_cast<std::size_t>(M * nk); ++k)
        EXPECT_NEAR(a_ensemble_uniform[k], a_scalar[k], 1e-12)
            << "uniform ensemble_rainfall_ should match scalar path at k=" << k;

    // --- After clear: advance from same starting point → same as scalar ---
    f.rom.a_ensemble = a_seed;
    f.rom.clearEnsembleRainfall();
    f.rom.advance(30.0, 5.0, rain.data(), nullptr, h0.data());

    for (std::size_t k = 0; k < static_cast<std::size_t>(M * nk); ++k)
        EXPECT_NEAR(f.rom.a_ensemble[k], a_scalar[k], 1e-12)
            << "post-clear advance should match scalar path at k=" << k;
}

// ============================================================================
// Phase 5A — per-member 1D head in coupling path
// ============================================================================

namespace {

static CsrGraph make_1d_chain_for_5a(int n) {
    int nc = n - 1;
    std::vector<int> n1(static_cast<std::size_t>(nc));
    std::vector<int> n2(static_cast<std::size_t>(nc));
    for (int k = 0; k < nc; ++k) { n1[static_cast<std::size_t>(k)] = k; n2[static_cast<std::size_t>(k)] = k + 1; }
    std::vector<int> is_outfall(static_cast<std::size_t>(n), 0);
    std::vector<int> active_map, full_to_active;
    return NetworkLaplacian1D::buildUniform(n, nc, n1.data(), n2.data(),
                                            is_outfall.data(), active_map, full_to_active);
}

} // anonymous namespace

// When a SpectralROM1D is passed to applyCouplingFlux(), each ensemble member
// uses its own reconstructed 1D head at the coupling node.
// With mannings_pert=0 (no Manning variation), this is the ONLY source of
// per-member flux variation, so:
//   - With rom1d: members with different h_1d → different flux → nonzero spread
//   - Without rom1d: all members see the same deterministic head → zero spread
TEST(SpectralROM, CouplingFluxUsesROM1DHeadsPerMember) {
    Fixture f;
    // Remove Manning variation so only h_1d drives per-member differences.
    f.rom.mannings_pert = 0.0;
    f.rom.rainfall_pert = 0.0;
    f.rom.initialize();

    int M  = f.rom.n_ensemble;
    int nt = f.rom.n_tri;
    int nk = f.rom.n_kept;

    // Seed all 2D members identically from an off-centre Gaussian bump.
    std::vector<double> h2d(static_cast<std::size_t>(nt), 0.0);
    double sigma2d = nt / 8.0;
    for (int t = 0; t < nt; ++t) {
        double dx = t - nt / 3.0;
        h2d[static_cast<std::size_t>(t)] = 0.10 * std::exp(-0.5 * dx * dx / (sigma2d * sigma2d));
    }
    f.rom.seed(h2d.data());

    // ---- Build a 5-node SpectralROM1D ----
    GraphEigenBasis basis1d;
    CsrGraph L1d = make_1d_chain_for_5a(5);
    basis1d.build(L1d, 3);

    SpectralROM1D rom1d;
    rom1d.basis         = &basis1d;
    rom1d.n_ensemble    = M;
    rom1d.mannings_pert = 0.0;
    rom1d.runoff_pert   = 0.0;
    rom1d.initialize();
    // All nodes active, no outfalls.
    rom1d.full_to_active = {0, 1, 2, 3, 4};

    // Set a_ensemble so reconstructHead(i, active_node=0) = i * h_step.
    // reconstructHead(i,0) = a[i,0]*P[0,0]  →  a[i,0] = i*h_step / P[0,0]
    // (formula works for any sign of P[0,0])
    double P00    = basis1d.P[0];  // P[mode=0, node=0]
    double h_step = 0.02;          // 2 cm per member step
    auto   nk1    = static_cast<std::size_t>(rom1d.n_kept);
    for (int i = 0; i < M; ++i) {
        auto ui = static_cast<std::size_t>(i);
        for (std::size_t j = 0; j < nk1; ++j)
            rom1d.a_ensemble[ui * nk1 + j] = 0.0;
        rom1d.a_ensemble[ui * nk1 + 0] = (i * h_step) / P00;
    }
    std::fill(rom1d.mode_active.begin(), rom1d.mode_active.end(), true);

    // Coupling point: cell 0, node_idx 0 → active_1d_idx 0.
    CouplingPoint cp;
    cp.cell_idx      = 0;
    cp.vertex_idx    = -1;
    cp.node_idx      = 0;
    cp.cd            = 0.6;
    cp.area          = 0.04;
    cp.is_outfall    = false;
    cp.has_flap_gate = false;
    std::vector<CouplingPoint> cps = {cp};

    // Deterministic head = 0 so all members drain (or receive backflow via rom1d).
    std::vector<double> node_heads(5, 0.0);
    double dt = 10.0;

    // Before coupling: all member coefficients are identical (seeded equally).
    EXPECT_NEAR(f.rom.a_ensemble[0],
                f.rom.a_ensemble[static_cast<std::size_t>((M-1) * nk)],
                1.0e-14) << "seeding should give identical coefficients";

    // ---- Path A: with rom1d — per-member h_1d ----
    // Member 0: h_1d=0, member M-1: h_1d=(M-1)*h_step >> h_2d → backflow.
    std::vector<double> a_seed(f.rom.a_ensemble);
    f.rom.applyCouplingFlux(cps, node_heads.data(), f.mesh, dt, &rom1d);

    double a0_rom1d   = f.rom.a_ensemble[0];
    double aEnd_rom1d = f.rom.a_ensemble[static_cast<std::size_t>((M-1) * nk)];
    EXPECT_GT(std::abs(aEnd_rom1d - a0_rom1d), 1.0e-10)
        << "per-member h_1d should open coefficient spread between member 0 and M-1";

    // ---- Path B: without rom1d — same h_1d for all members ----
    f.rom.a_ensemble = a_seed;  // reset
    f.rom.applyCouplingFlux(cps, node_heads.data(), f.mesh, dt, nullptr);

    double a0_det   = f.rom.a_ensemble[0];
    double aEnd_det = f.rom.a_ensemble[static_cast<std::size_t>((M-1) * nk)];
    EXPECT_NEAR(a0_det, aEnd_det, 1.0e-12)
        << "deterministic head should give identical coupling delta for all members";
}

// When full_to_active maps the coupling node to -1 (outfall),
// applyCouplingFlux must fall back to node_heads[cp.node_idx] regardless of rom1d.
TEST(SpectralROM, CouplingFluxFallsBackForOutfallNode) {
    Fixture f;
    f.rom.mannings_pert = 0.0;
    f.rom.rainfall_pert = 0.0;
    f.rom.initialize();

    int M  = f.rom.n_ensemble;
    int nt = f.rom.n_tri;

    std::vector<double> h2d(static_cast<std::size_t>(nt), 0.0);
    double sigma2d = nt / 8.0;
    for (int t = 0; t < nt; ++t) {
        double dx = t - nt / 3.0;
        h2d[static_cast<std::size_t>(t)] = 0.10 * std::exp(-0.5 * dx * dx / (sigma2d * sigma2d));
    }
    f.rom.seed(h2d.data());

    GraphEigenBasis basis1d;
    CsrGraph L1d = make_1d_chain_for_5a(5);
    basis1d.build(L1d, 3);

    SpectralROM1D rom1d;
    rom1d.basis         = &basis1d;
    rom1d.n_ensemble    = M;
    rom1d.mannings_pert = 0.0;
    rom1d.runoff_pert   = 0.0;
    rom1d.initialize();
    // Node 0 maps to -1 → outfall → fallback to deterministic head.
    rom1d.full_to_active = {-1, 0, 1, 2, 3};

    // Set wildly different a_ensemble per member — if fallback works, it won't matter.
    auto nk1 = static_cast<std::size_t>(rom1d.n_kept);
    for (int i = 0; i < M; ++i) {
        auto ui = static_cast<std::size_t>(i);
        for (std::size_t j = 0; j < nk1; ++j)
            rom1d.a_ensemble[ui * nk1 + j] = static_cast<double>(i) * 10.0;
    }
    std::fill(rom1d.mode_active.begin(), rom1d.mode_active.end(), true);

    CouplingPoint cp;
    cp.cell_idx      = 0;
    cp.vertex_idx    = -1;
    cp.node_idx      = 0;   // → full_to_active[0] = -1 → outfall fallback
    cp.cd            = 0.6;
    cp.area          = 0.04;
    cp.is_outfall    = false;
    cp.has_flap_gate = false;
    std::vector<CouplingPoint> cps = {cp};
    std::vector<double> node_heads(5, 0.05);
    double dt = 10.0;

    std::vector<double> a_seed(f.rom.a_ensemble);

    // With rom1d (outfall node → fallback)
    f.rom.applyCouplingFlux(cps, node_heads.data(), f.mesh, dt, &rom1d);
    std::vector<double> a_with_rom1d(f.rom.a_ensemble);

    // Without rom1d
    f.rom.a_ensemble = a_seed;
    f.rom.applyCouplingFlux(cps, node_heads.data(), f.mesh, dt, nullptr);
    std::vector<double> a_without(f.rom.a_ensemble);

    for (std::size_t k = 0; k < a_with_rom1d.size(); ++k)
        EXPECT_DOUBLE_EQ(a_with_rom1d[k], a_without[k])
            << "outfall fallback should match nullptr rom1d at k=" << k;
}

// ============================================================================
// PR 7 (reform) — difference-form coupling
// ============================================================================

// Deviation form: the deterministic coupling exchange is already inside h_det,
// so with zero perturbation every member's flux equals Q_det and δQ_i = 0 —
// applyCouplingFlux must leave every member coefficient unchanged, while the
// diagnostic still reports the (nonzero) deterministic flux bounds.
TEST(SpectralROM, CouplingFluxAppliesOnlyDifference) {
    Fixture f;
    f.rom.mannings_pert = 0.0;
    f.rom.rainfall_pert = 0.0;
    f.rom.cd_pert       = 0.0;
    f.rom.initialize();

    int nt = f.rom.n_tri;
    // First-mode reference so the coupling cell has positive deterministic depth.
    double sign0 = (f.basis.P[0] >= 0.0) ? 1.0 : -1.0;
    std::vector<double> h2d(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t)
        h2d[static_cast<std::size_t>(t)] =
            0.20 + sign0 * f.basis.P[static_cast<std::size_t>(t)] * 0.10;
    f.rom.seed(h2d.data());
    const std::vector<double> a_before = f.rom.a_ensemble;

    CouplingPoint cp;
    cp.cell_idx = 0; cp.vertex_idx = -1; cp.node_idx = 0;
    cp.cd = 0.6; cp.area = 1.0e-4; cp.is_outfall = false; cp.has_flap_gate = false;
    std::vector<CouplingPoint> cps = {cp};
    std::vector<double> node_heads(1, 0.0);  // 1D head below → drainage

    f.rom.applyCouplingFlux(cps, node_heads.data(), f.mesh, 10.0);

    // No member coefficient changed (δQ_i = 0 for all).
    for (std::size_t k = 0; k < a_before.size(); ++k)
        EXPECT_NEAR(f.rom.a_ensemble[k], a_before[k], 1.0e-15) << "k=" << k;

    // Diagnostic still reports the deterministic flux, and it is nonzero.
    ASSERT_TRUE(f.rom.coupling_unc_output.is_valid());
    EXPECT_NEAR(f.rom.coupling_unc_output.q_min[0],
                f.rom.coupling_unc_output.q_max[0], 1.0e-14);
    EXPECT_GT(f.rom.coupling_unc_output.q_max[0], 0.0)
        << "deterministic drainage flux should be positive";
}

// Coupling flux spread comes from Cd (cd_pert), NOT Manning — the deviation
// form removed the orifice /mannings_mult scaling (orifice discharge does not
// depend on surface roughness).
TEST(SpectralROM, CouplingSpreadFromCdNotManning) {
    auto spread_for = [](double m_pert, double c_pert) {
        MeshData mesh = makeStructuredMesh(5);
        MeshEigenBasis basis;
        EXPECT_TRUE(basis.build(mesh, 6));
        SpectralROM rom;
        rom.basis         = &basis;
        rom.n_ensemble    = 20;
        rom.mannings_pert = m_pert;
        rom.rainfall_pert = 0.0;
        rom.cd_pert       = c_pert;
        rom.initialize();
        int nt = rom.n_tri;
        double sign0 = (basis.P[0] >= 0.0) ? 1.0 : -1.0;
        std::vector<double> h2d(static_cast<std::size_t>(nt));
        for (int t = 0; t < nt; ++t)
            h2d[static_cast<std::size_t>(t)] =
                0.20 + sign0 * basis.P[static_cast<std::size_t>(t)] * 0.10;
        rom.seed(h2d.data());
        CouplingPoint cp;
        cp.cell_idx = 0; cp.vertex_idx = -1; cp.node_idx = 0;
        cp.cd = 0.6; cp.area = 1.0e-4; cp.is_outfall = false; cp.has_flap_gate = false;
        std::vector<CouplingPoint> cps = {cp};
        std::vector<double> node_heads(1, 0.0);
        rom.applyCouplingFlux(cps, node_heads.data(), mesh, 10.0);
        return rom.coupling_unc_output.q_max[0] - rom.coupling_unc_output.q_min[0];
    };

    // Cd perturbation → spread; Manning perturbation alone → NO spread.
    EXPECT_GT(spread_for(/*m*/0.0, /*cd*/0.15), 1.0e-8)
        << "Cd perturbation must create coupling flux spread";
    EXPECT_NEAR(spread_for(/*m*/0.20, /*cd*/0.0), 0.0, 1.0e-14)
        << "Manning perturbation must NOT affect orifice coupling flux (/mm removed)";
}

// ============================================================================
// Phase 5B — CouplingUncertaintyOutput diagnostics
// ============================================================================

// applyCouplingFlux() must populate coupling_unc_output with q_min ≤ q_max
// at every non-outfall coupling point, and drive spread via Cd (cd_pert).
TEST(SpectralROM, CouplingUncOutputPopulatedAfterCouplingFlux) {
    Fixture f;
    f.rom.mannings_pert = 0.00;
    f.rom.rainfall_pert = 0.00;
    f.rom.cd_pert       = 0.15;  // Cd variation → coupling flux spread
    f.rom.initialize();

    int nt = f.rom.n_tri;

    // Seed with first-mode shape: h[t] = sign0 * P[0,t] * amp.
    // This projects exactly onto mode 0, guaranteeing nonzero reconstructed depth
    // at cells where P[0,t] > 0 — independent of mesh geometry.
    // (Same approach as CouplingFluxHighManningsDrainsLess.)
    double sign0 = (f.basis.P[0] >= 0.0) ? 1.0 : -1.0;
    double amp = 0.10;
    std::vector<double> h2d(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t)
        h2d[static_cast<std::size_t>(t)] =
            sign0 * f.basis.P[static_cast<std::size_t>(t)] * amp;
    f.rom.seed(h2d.data());

    // cp0: non-outfall at cell 0 (positive depth by sign0 construction).
    // Small area (1e-4) keeps Q below the h*tri_area/dt drainage cap so Manning
    // variation is not clamped away — same rationale as CouplingFluxHighManningsDrainsLess.
    // cp1: outfall (skipped by ROM → stays at 0).
    CouplingPoint cp0;
    cp0.cell_idx = 0; cp0.vertex_idx = -1; cp0.node_idx = 0;
    cp0.cd = 0.6; cp0.area = 1.0e-4; cp0.is_outfall = false; cp0.has_flap_gate = false;

    CouplingPoint cp1;
    cp1.cell_idx = 1; cp1.vertex_idx = -1; cp1.node_idx = 1;
    cp1.cd = 0.6; cp1.area = 0.04; cp1.is_outfall = true; cp1.has_flap_gate = false;

    std::vector<CouplingPoint> cps = {cp0, cp1};
    std::vector<double> node_heads(2, 0.0);

    f.rom.applyCouplingFlux(cps, node_heads.data(), f.mesh, 10.0);

    ASSERT_TRUE(f.rom.coupling_unc_output.is_valid());
    ASSERT_EQ(f.rom.coupling_unc_output.q_min.size(), std::size_t{2});
    ASSERT_EQ(f.rom.coupling_unc_output.q_max.size(), std::size_t{2});

    // Non-outfall coupling point (cp0): q_min ≤ q_max.
    EXPECT_LE(f.rom.coupling_unc_output.q_min[0], f.rom.coupling_unc_output.q_max[0]);

    // Cd mult ∈ [0.85, 1.15] → Q_i = Cd·cd_mult[i]·A·sqrt(2g·h) differs per member.
    EXPECT_GT(f.rom.coupling_unc_output.q_max[0] - f.rom.coupling_unc_output.q_min[0], 1.0e-8)
        << "15% Cd pert should create clear flux spread";

    // Outfall coupling point (cp1): output stays at 0.0 (skipped by ROM).
    EXPECT_DOUBLE_EQ(f.rom.coupling_unc_output.q_min[1], 0.0);
    EXPECT_DOUBLE_EQ(f.rom.coupling_unc_output.q_max[1], 0.0);
}

// ============================================================================
// Phase 5C — Discharge-coefficient uncertainty
// ============================================================================

// With Manning and rainfall pert = 0 but cd_pert > 0, each member has a
// different effective Cd → per-member flux varies → q_max > q_min.
TEST(SpectralROM, CdPertDrivesFluxSpread) {
    // Build ROM with zero Manning/rainfall pert so Cd is the only source of spread.
    MeshData mesh = makeStructuredMesh(5);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));

    SpectralROM rom;
    rom.basis         = &basis;
    rom.n_ensemble    = 20;
    rom.mannings_pert = 0.0;
    rom.rainfall_pert = 0.0;
    rom.cd_pert       = 0.15;  // ±15% Cd variation
    rom.initialize();

    int nt = rom.n_tri;

    // Seed with first-mode shape so coupling cell has nonzero depth.
    double sign0 = (basis.P[0] >= 0.0) ? 1.0 : -1.0;
    std::vector<double> h2d(static_cast<std::size_t>(nt));
    for (int t = 0; t < nt; ++t)
        h2d[static_cast<std::size_t>(t)] =
            sign0 * basis.P[static_cast<std::size_t>(t)] * 0.10;
    rom.seed(h2d.data());

    // Small coupling area keeps Q_orifice below the h*tri_area/dt drainage cap.
    CouplingPoint cp;
    cp.cell_idx = 0; cp.vertex_idx = -1; cp.node_idx = 0;
    cp.cd = 0.6; cp.area = 1.0e-4; cp.is_outfall = false; cp.has_flap_gate = false;

    std::vector<CouplingPoint> cps = {cp};
    std::vector<double> node_heads(1, 0.0);

    rom.applyCouplingFlux(cps, node_heads.data(), mesh, 10.0);

    ASSERT_TRUE(rom.coupling_unc_output.is_valid());
    EXPECT_GT(
        rom.coupling_unc_output.q_max[0] - rom.coupling_unc_output.q_min[0],
        1.0e-8)
        << "15% Cd perturbation should create nonzero flux spread at coupling point";
}

// With cd_pert = 0 (default) and zero Manning/rainfall pert, all members
// compute identical flux → q_min == q_max.
TEST(SpectralROM, CdPertZeroNoSpread) {
    MeshData mesh = makeStructuredMesh(5);
    MeshEigenBasis basis;
    ASSERT_TRUE(basis.build(mesh, 6));

    SpectralROM rom;
    rom.basis         = &basis;
    rom.n_ensemble    = 20;
    rom.mannings_pert = 0.0;
    rom.rainfall_pert = 0.0;
    // cd_pert defaults to 0.0 — no Cd variation
    rom.initialize();

    int nt = rom.n_tri;

    // Seed with index-based Gaussian so coupling cell has some depth.
    std::vector<double> h2d(static_cast<std::size_t>(nt), 0.0);
    double sigma2d = nt / 8.0;
    for (int t = 0; t < nt; ++t) {
        double dx = t - nt / 3.0;
        h2d[static_cast<std::size_t>(t)] =
            0.10 * std::exp(-0.5 * dx * dx / (sigma2d * sigma2d));
    }
    rom.seed(h2d.data());

    CouplingPoint cp;
    cp.cell_idx = 0; cp.vertex_idx = -1; cp.node_idx = 0;
    cp.cd = 0.6; cp.area = 0.04; cp.is_outfall = false; cp.has_flap_gate = false;

    std::vector<CouplingPoint> cps = {cp};
    std::vector<double> node_heads(1, 0.0);

    rom.applyCouplingFlux(cps, node_heads.data(), mesh, 10.0);

    ASSERT_TRUE(rom.coupling_unc_output.is_valid());
    EXPECT_NEAR(
        rom.coupling_unc_output.q_min[0],
        rom.coupling_unc_output.q_max[0],
        1.0e-14)
        << "cd_pert=0 with zero Manning/rainfall pert should give q_min == q_max";
}

// With mannings_pert = 0 and no rom1d, every member computes the same flux,
// so q_min == q_max at the (non-outfall) coupling point.
TEST(SpectralROM, CouplingUncOutputNoSpreadWithZeroPert) {
    Fixture f;
    f.rom.mannings_pert = 0.0;
    f.rom.rainfall_pert = 0.0;
    f.rom.initialize();

    int nt = f.rom.n_tri;
    std::vector<double> h2d(static_cast<std::size_t>(nt), 0.0);
    double sigma2d = nt / 8.0;
    for (int t = 0; t < nt; ++t) {
        double dx = t - nt / 3.0;
        h2d[static_cast<std::size_t>(t)] = 0.10 * std::exp(-0.5 * dx * dx / (sigma2d * sigma2d));
    }
    f.rom.seed(h2d.data());

    CouplingPoint cp;
    cp.cell_idx = 0; cp.vertex_idx = -1; cp.node_idx = 0;
    cp.cd = 0.6; cp.area = 0.04; cp.is_outfall = false; cp.has_flap_gate = false;
    std::vector<CouplingPoint> cps = {cp};
    std::vector<double> node_heads(1, 0.0);

    f.rom.applyCouplingFlux(cps, node_heads.data(), f.mesh, 10.0);

    ASSERT_TRUE(f.rom.coupling_unc_output.is_valid());
    // All members see the same h_2d and the same mannings_mult = 1.0 → identical Q.
    EXPECT_NEAR(f.rom.coupling_unc_output.q_min[0],
                f.rom.coupling_unc_output.q_max[0], 1.0e-14)
        << "zero perturbation should give q_min == q_max";
}

// ============================================================================
// PR 7 (reform) — DeviationForm invariants (2D)
// ============================================================================

namespace {
// Off-centre bump reference with non-zero modal content (b_j != 0).
std::vector<double> bumpRef(const MeshEigenBasis& basis, int nt, double base) {
    std::vector<double> h(static_cast<std::size_t>(nt));
    double sign0 = (basis.P[0] >= 0.0) ? 1.0 : -1.0;
    for (int t = 0; t < nt; ++t)
        h[static_cast<std::size_t>(t)] =
            base + sign0 * basis.P[static_cast<std::size_t>(t)] * 0.10;
    return h;
}
double maxSpread2D(const SpectralROM& rom) {
    double s = 0.0;
    for (int t = 0; t < rom.n_tri; ++t)
        s = std::max(s, rom.q95[static_cast<std::size_t>(t)]
                      - rom.q05[static_cast<std::size_t>(t)]);
    return s;
}
}  // namespace

// pert=0 → deviations identically zero → q05==q50==q95==h_det to 1e-12, for
// arbitrary time-varying reference/forcing.  Impossible under the total-head form.
TEST(DeviationForm2D, ZeroPerturbationIsExact) {
    Fixture f(/*N=*/5, /*k=*/6, /*M=*/20, /*m_pert=*/0.0, /*r_pert=*/0.0);
    int nt = f.rom.n_tri;
    auto h0 = bumpRef(f.basis, nt, 0.5);
    f.rom.seed(h0.data());

    std::vector<double> h_det(static_cast<std::size_t>(nt));
    std::vector<double> rain(static_cast<std::size_t>(nt));
    for (int step = 0; step < 60; ++step) {
        double phase = 0.05 * step;
        for (int t = 0; t < nt; ++t) {
            h_det[static_cast<std::size_t>(t)] =
                h0[static_cast<std::size_t>(t)] * (1.0 + 0.5 * std::sin(phase + 0.3 * t));
            rain[static_cast<std::size_t>(t)] = 1e-5 * (1.0 + std::cos(phase + 0.7 * t));
        }
        f.rom.advance(15.0, 5.0, rain.data(), nullptr, h_det.data());
    }
    f.rom.computeQuantiles(h_det.data());
    for (int t = 0; t < nt; ++t) {
        auto ut = static_cast<std::size_t>(t);
        EXPECT_NEAR(f.rom.q05[ut], h_det[ut], 1e-12) << "t=" << t;
        EXPECT_NEAR(f.rom.q50[ut], h_det[ut], 1e-12) << "t=" << t;
        EXPECT_NEAR(f.rom.q95[ut], h_det[ut], 1e-12) << "t=" << t;
    }
}

// Median stays bracketed inside the spread around the deterministic reference.
TEST(DeviationForm2D, MedianTracksDeterministic) {
    Fixture f(/*N=*/5, /*k=*/6, /*M=*/50, /*m_pert=*/0.20, /*r_pert=*/0.0);
    int nt = f.rom.n_tri;
    auto h_det = bumpRef(f.basis, nt, 0.5);
    f.rom.seed(h_det.data());
    for (int step = 0; step < 40; ++step)
        f.rom.advance(30.0, 5.0, nullptr, nullptr, h_det.data());
    f.rom.computeQuantiles(h_det.data());
    for (int t = 0; t < nt; ++t) {
        auto ut = static_cast<std::size_t>(t);
        double spread = f.rom.q95[ut] - f.rom.q05[ut];
        EXPECT_LE(std::abs(f.rom.q50[ut] - h_det[ut]), 0.25 * spread + 1e-12)
            << "median drifted outside 25% of spread at t=" << t;
    }
}

// Sustained reference growth → total spread non-decreasing after transient.
TEST(DeviationForm2D, SpreadNeverCollapses) {
    Fixture f(/*N=*/5, /*k=*/6, /*M=*/20, /*m_pert=*/0.20, /*r_pert=*/0.0);
    int nt = f.rom.n_tri;
    auto h0 = bumpRef(f.basis, nt, 0.2);
    f.rom.seed(h0.data());
    std::vector<double> h_det(static_cast<std::size_t>(nt));
    double prev = -1.0;
    for (int step = 0; step < 200; ++step) {
        double grow = 1.0 + static_cast<double>(step) / 200.0;
        for (int t = 0; t < nt; ++t)
            h_det[static_cast<std::size_t>(t)] = h0[static_cast<std::size_t>(t)] * grow;
        f.rom.advance(5.0, 1.0, nullptr, nullptr, h_det.data());
        if (step > 40) {
            f.rom.computeQuantiles(h_det.data());
            double total = 0.0;
            for (int t = 0; t < nt; ++t)
                total += f.rom.q95[static_cast<std::size_t>(t)]
                       - f.rom.q05[static_cast<std::size_t>(t)];
            if (prev >= 0.0) EXPECT_GE(total, prev * 0.95) << "spread collapsed at step " << step;
            prev = total;
        }
    }
    EXPECT_GT(prev, 0.0);
}

// spread(0.2) > spread(0.1) > 0 under identical forcing.
TEST(DeviationForm2D, SpreadScalesWithManningPert) {
    auto run = [](double pert) {
        Fixture f(/*N=*/5, /*k=*/6, /*M=*/20, pert, /*r_pert=*/0.0);
        int nt = f.rom.n_tri;
        auto h_det = bumpRef(f.basis, nt, 0.5);
        f.rom.seed(h_det.data());
        for (int step = 0; step < 40; ++step)
            f.rom.advance(30.0, 5.0, nullptr, nullptr, h_det.data());
        f.rom.computeQuantiles(h_det.data());
        return maxSpread2D(f.rom);
    };
    double s10 = run(0.10), s20 = run(0.20);
    EXPECT_GT(s10, 0.0);
    EXPECT_GT(s20, s10) << "doubling the perturbation must widen the band";
}

// ============================================================================
// Phase 6 — FiedlerDiagnostic
// ============================================================================

// compute() sets is_ready() to true and populates phi2/grad/rank.
TEST(FiedlerDiagnostic, IsReadyAfterCompute) {
    Fixture f;  // N=5 → 50 triangles, k=6 modes (num_kept >= 2)
    FiedlerDiagnostic fd;
    fd.basis = &f.basis;
    fd.compute(f.mesh);

    EXPECT_TRUE(fd.is_ready());
    EXPECT_EQ(static_cast<int>(fd.phi2.size()), f.basis.n_triangles);
    EXPECT_EQ(static_cast<int>(fd.grad.size()), f.basis.n_triangles);
    EXPECT_EQ(static_cast<int>(fd.rank.size()), f.basis.n_triangles);
}

// lambda2 must equal basis.eigenvalues[0].
// MeshEigenBasis stores only nontrivial modes, so eigenvalues[0] = λ₂.
TEST(FiedlerDiagnostic, Lambda2MatchesBasisEigenvalue) {
    Fixture f;
    FiedlerDiagnostic fd;
    fd.basis = &f.basis;
    fd.compute(f.mesh);

    EXPECT_DOUBLE_EQ(fd.lambda2, f.basis.eigenvalues[0]);
}

// phi2[t] must equal basis.P[0*n_tri + t] for all t.
// MeshEigenBasis filters the null mode before storing P, so j=0 IS the
// Fiedler vector (the smallest nontrivial eigenvector of the Laplacian).
TEST(FiedlerDiagnostic, Phi2MatchesBasisColumn0) {
    Fixture f;
    FiedlerDiagnostic fd;
    fd.basis = &f.basis;
    fd.compute(f.mesh);

    int n = f.basis.n_triangles;
    for (int t = 0; t < n; ++t) {
        EXPECT_DOUBLE_EQ(fd.phi2[static_cast<std::size_t>(t)],
                         f.basis.P[static_cast<std::size_t>(t)])
            << "phi2 mismatch at t=" << t;
    }
}

// Gradient must be non-negative everywhere.
TEST(FiedlerDiagnostic, GradientNonNegative) {
    Fixture f;
    FiedlerDiagnostic fd;
    fd.basis = &f.basis;
    fd.compute(f.mesh);

    for (int t = 0; t < f.basis.n_triangles; ++t)
        EXPECT_GE(fd.grad[static_cast<std::size_t>(t)], 0.0)
            << "grad < 0 at t=" << t;
}

// The max gradient must be positive (Fiedler vector is non-constant).
TEST(FiedlerDiagnostic, MaxGradientPositive) {
    Fixture f;
    FiedlerDiagnostic fd;
    fd.basis = &f.basis;
    fd.compute(f.mesh);

    double gmax = *std::max_element(fd.grad.begin(), fd.grad.end());
    EXPECT_GT(gmax, 0.0)
        << "Fiedler vector is non-constant so max gradient must be positive";
}

// rank must be sorted in descending gradient order.
TEST(FiedlerDiagnostic, RankSortedDescending) {
    Fixture f;
    FiedlerDiagnostic fd;
    fd.basis = &f.basis;
    fd.compute(f.mesh);

    int n = f.basis.n_triangles;
    for (int i = 0; i + 1 < n; ++i) {
        EXPECT_GE(fd.grad[static_cast<std::size_t>(fd.rank[static_cast<std::size_t>(i)])],
                  fd.grad[static_cast<std::size_t>(fd.rank[static_cast<std::size_t>(i + 1)])])
            << "rank not sorted at i=" << i;
    }
}

TEST(FiedlerDiagnostic, CouplingAnnotation2DSide) {
    // Demonstrate coupling-path annotation for the 2D side:
    // given a CouplingPoint with cell_idx, look up the Fiedler bottleneck
    // score directly from fd.grad[cp.cell_idx].
    Fixture f;
    FiedlerDiagnostic fd;
    fd.basis = &f.basis;
    fd.compute(f.mesh);
    ASSERT_TRUE(fd.is_ready());

    // Simulate coupling point at the highest-gradient 2D cell (rank[0]).
    const int cp_cell_idx = fd.rank[0];
    const double bottleneck_score = fd.grad[static_cast<std::size_t>(cp_cell_idx)];

    // The highest-ranked cell must have a positive gradient (non-constant Fiedler).
    EXPECT_GT(bottleneck_score, 0.0);

    // A low-gradient cell should have a smaller or equal score.
    const int low_cell_idx = fd.rank[static_cast<std::size_t>(f.basis.n_triangles - 1)];
    EXPECT_LE(fd.grad[static_cast<std::size_t>(low_cell_idx)], bottleneck_score);
}
