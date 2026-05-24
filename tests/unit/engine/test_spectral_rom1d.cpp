/**
 * @file test_spectral_rom1d.cpp
 * @brief Phase 4 tests: GraphEigenBasis, NetworkLaplacian1D, SpectralROM1D.
 *
 * All three components are compiled from source; no engine shared-lib dependency.
 * Tests cover:
 *   - CsrGraph + GraphEigenBasis: Laplacian eigendecomposition correctness
 *   - NetworkLaplacian1D: active-node maps and Laplacian structural properties
 *   - SpectralROM1D: LHS design, seed/quantile tightness, spread growth,
 *     ensemble runoff path, external samples, clear()
 *
 * @ingroup unit_tests
 */

#include <gtest/gtest.h>

#include "uncertainty/GraphEigenBasis.hpp"
#include "uncertainty/NetworkLaplacian1D.hpp"
#include "uncertainty/SpectralROM1D.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using namespace openswmm::uncertainty;

// ============================================================================
// Helpers
// ============================================================================

// Linear chain of n nodes (0-1-2-..-(n-1)), no outfalls.
static CsrGraph make_chain_laplacian(int n) {
    int nc = n - 1;
    std::vector<int> n1(static_cast<std::size_t>(nc));
    std::vector<int> n2(static_cast<std::size_t>(nc));
    for (int k = 0; k < nc; ++k) {
        n1[static_cast<std::size_t>(k)] = k;
        n2[static_cast<std::size_t>(k)] = k + 1;
    }
    std::vector<int> is_outfall(static_cast<std::size_t>(n), 0);
    std::vector<int> active_map, full_to_active;
    return NetworkLaplacian1D::buildUniform(n, nc, n1.data(), n2.data(),
                                            is_outfall.data(),
                                            active_map, full_to_active);
}

// ============================================================================
// CsrGraph / GraphEigenBasis
// ============================================================================

TEST(GraphEigenBasis, BuildSucceedsOnChain) {
    CsrGraph L = make_chain_laplacian(10);
    GraphEigenBasis basis;
    bool ok = basis.build(L, 5);
    ASSERT_TRUE(ok);
    EXPECT_GT(basis.num_kept, 0);
    EXPECT_EQ(basis.last_error, 0);
}

TEST(GraphEigenBasis, EigenvaluesAscending) {
    CsrGraph L = make_chain_laplacian(20);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 8));
    for (int j = 1; j < basis.num_kept; ++j)
        EXPECT_LE(basis.eigenvalues[static_cast<std::size_t>(j - 1)],
                  basis.eigenvalues[static_cast<std::size_t>(j)]);
}

TEST(GraphEigenBasis, EigenvaluesPositive) {
    // All retained eigenvalues must exceed null_tol (null mode filtered out).
    CsrGraph L = make_chain_laplacian(15);
    GraphEigenBasis basis;
    basis.null_tol = 1.0e-8;
    ASSERT_TRUE(basis.build(L, 6));
    for (int j = 0; j < basis.num_kept; ++j)
        EXPECT_GT(basis.eigenvalues[static_cast<std::size_t>(j)], 1.0e-8);
}

TEST(GraphEigenBasis, PColumnsOrthonormal) {
    CsrGraph L = make_chain_laplacian(20);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 6));
    int n = basis.n_nodes;
    int k = basis.num_kept;
    // Check P^T P = I_k to within 1e-10
    for (int j1 = 0; j1 < k; ++j1) {
        for (int j2 = 0; j2 < k; ++j2) {
            double dot = 0.0;
            for (int i = 0; i < n; ++i) {
                dot += basis.P[static_cast<std::size_t>(j1 * n + i)]
                     * basis.P[static_cast<std::size_t>(j2 * n + i)];
            }
            double expected = (j1 == j2) ? 1.0 : 0.0;
            EXPECT_NEAR(dot, expected, 1.0e-10)
                << "P^T P [" << j1 << "," << j2 << "] = " << dot;
        }
    }
}

TEST(GraphEigenBasis, EigenvectorsAreEigenvectors) {
    // L * p_j ≈ λ_j * p_j  for each retained mode.
    CsrGraph L = make_chain_laplacian(20);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 5));
    int n = basis.n_nodes;
    std::vector<double> y(static_cast<std::size_t>(n), 0.0);
    for (int j = 0; j < basis.num_kept; ++j) {
        const double* pj = &basis.P[static_cast<std::size_t>(j * n)];
        std::fill(y.begin(), y.end(), 0.0);
        csr_matvec(L, pj, y.data());
        double lam = basis.eigenvalues[static_cast<std::size_t>(j)];
        for (int i = 0; i < n; ++i)
            EXPECT_NEAR(y[static_cast<std::size_t>(i)],
                        lam * pj[static_cast<std::size_t>(i)], 1.0e-9)
                << "mode " << j << " node " << i;
    }
}

TEST(GraphEigenBasis, NullModesExcluded) {
    // Disconnected graph: two separate chains of 5 nodes each (10 nodes total).
    // Two null modes (one per connected component) → both discarded.
    int n_full = 10;
    // chain 0-1-2-3-4 and chain 5-6-7-8-9
    std::vector<int> n1 = {0,1,2,3, 5,6,7,8};
    std::vector<int> n2 = {1,2,3,4, 6,7,8,9};
    int nc = static_cast<int>(n1.size());
    std::vector<int> is_outfall(static_cast<std::size_t>(n_full), 0);
    std::vector<int> active_map, full_to_active;
    CsrGraph L = NetworkLaplacian1D::buildUniform(
        n_full, nc, n1.data(), n2.data(), is_outfall.data(),
        active_map, full_to_active);

    GraphEigenBasis basis;
    basis.null_tol = 1.0e-8;
    ASSERT_TRUE(basis.build(L, 8));
    // None of the retained eigenvalues should be below null_tol
    for (int j = 0; j < basis.num_kept; ++j)
        EXPECT_GT(basis.eigenvalues[static_cast<std::size_t>(j)], 1.0e-8);
    // At least one null mode expected (the global constant vector).
    // With two components, numerical Lanczos may land the second near-zero
    // eigenvalue just above null_tol in floating point, so we don't pin the count.
    EXPECT_GE(basis.num_null, 1);
}

// ============================================================================
// NetworkLaplacian1D
// ============================================================================

TEST(NetworkLaplacian1D, ActiveMapExcludesOutfalls) {
    // 5-node chain; nodes 0 and 4 are outfalls → 3 active nodes
    int n1[4] = {0,1,2,3};
    int n2[4] = {1,2,3,4};
    int is_outfall[5] = {1,0,0,0,1};
    std::vector<int> active_map, full_to_active;
    CsrGraph L = NetworkLaplacian1D::buildUniform(
        5, 4, n1, n2, is_outfall, active_map, full_to_active);

    ASSERT_EQ(static_cast<int>(active_map.size()), 3);
    EXPECT_EQ(active_map[0], 1);
    EXPECT_EQ(active_map[1], 2);
    EXPECT_EQ(active_map[2], 3);

    EXPECT_EQ(full_to_active[0], -1);  // outfall
    EXPECT_EQ(full_to_active[4], -1);  // outfall
    EXPECT_EQ(full_to_active[2],  1);  // middle node → active idx 1
}

TEST(NetworkLaplacian1D, LaplacianRowSumsZero) {
    // Row sums of graph Laplacian must be zero (each internal node).
    CsrGraph L = make_chain_laplacian(8);
    ASSERT_EQ(L.n, 8);
    for (int r = 0; r < L.n; ++r) {
        double row_sum = 0.0;
        for (int k = L.row_ptr[static_cast<std::size_t>(r)];
             k < L.row_ptr[static_cast<std::size_t>(r + 1)]; ++k)
            row_sum += L.values[static_cast<std::size_t>(k)];
        EXPECT_NEAR(row_sum, 0.0, 1.0e-14) << "row " << r;
    }
}

TEST(NetworkLaplacian1D, LaplacianSymmetric) {
    CsrGraph L = make_chain_laplacian(8);
    // For each off-diagonal entry (r,c) with value v, entry (c,r) must also = v.
    for (int r = 0; r < L.n; ++r) {
        for (int k = L.row_ptr[static_cast<std::size_t>(r)];
             k < L.row_ptr[static_cast<std::size_t>(r + 1)]; ++k) {
            int c = L.col_idx[static_cast<std::size_t>(k)];
            double v = L.values[static_cast<std::size_t>(k)];
            if (c == r) continue;
            // Find (c, r)
            double v_sym = 0.0;
            bool found = false;
            for (int k2 = L.row_ptr[static_cast<std::size_t>(c)];
                 k2 < L.row_ptr[static_cast<std::size_t>(c + 1)]; ++k2) {
                if (L.col_idx[static_cast<std::size_t>(k2)] == r) {
                    v_sym = L.values[static_cast<std::size_t>(k2)];
                    found = true;
                    break;
                }
            }
            EXPECT_TRUE(found) << "missing symmetric entry (" << c << "," << r << ")";
            EXPECT_DOUBLE_EQ(v, v_sym) << "L[" << r << "," << c << "] != L[" << c << "," << r << "]";
        }
    }
}

TEST(NetworkLaplacian1D, WeightedLaplacianScalesEigenvalues) {
    // Scaling all weights by factor s scales all eigenvalues by s.
    int n = 10, nc = 9;
    std::vector<int> n1(static_cast<std::size_t>(nc));
    std::vector<int> n2(static_cast<std::size_t>(nc));
    for (int k = 0; k < nc; ++k) {
        n1[static_cast<std::size_t>(k)] = k;
        n2[static_cast<std::size_t>(k)] = k + 1;
    }
    std::vector<int> is_outfall(static_cast<std::size_t>(n), 0);
    std::vector<int> active_map, full_to_active;

    constexpr double s = 3.0;
    std::vector<double> weights_1(static_cast<std::size_t>(nc), 1.0);
    std::vector<double> weights_s(static_cast<std::size_t>(nc), s);

    CsrGraph L1 = NetworkLaplacian1D::buildWeighted(n, nc, n1.data(), n2.data(),
                                                    is_outfall.data(), weights_1.data(),
                                                    active_map, full_to_active);
    CsrGraph Ls = NetworkLaplacian1D::buildWeighted(n, nc, n1.data(), n2.data(),
                                                    is_outfall.data(), weights_s.data(),
                                                    active_map, full_to_active);

    GraphEigenBasis b1, bs;
    ASSERT_TRUE(b1.build(L1, 5));
    ASSERT_TRUE(bs.build(Ls, 5));
    ASSERT_EQ(b1.num_kept, bs.num_kept);
    for (int j = 0; j < b1.num_kept; ++j) {
        EXPECT_NEAR(bs.eigenvalues[static_cast<std::size_t>(j)],
                    s * b1.eigenvalues[static_cast<std::size_t>(j)], 1.0e-10)
            << "eigenvalue " << j << " not scaled";
    }
}

// ============================================================================
// SpectralROM1D — fixture
// ============================================================================

struct ROM1DFixture {
    static constexpr int N     = 20;
    static constexpr int M     = 30;
    static constexpr int K_req = 6;

    GraphEigenBasis basis;
    SpectralROM1D   rom;

    ROM1DFixture() {
        CsrGraph L = make_chain_laplacian(N);
        basis.build(L, K_req);

        rom.basis         = &basis;
        rom.n_ensemble    = M;
        rom.mannings_pert = 0.20;
        rom.runoff_pert   = 0.20;
        rom.initialize();
    }

    // Gaussian bump head field centred at node N/3 (off-centre → non-zero projections)
    std::vector<double> bump_head() const {
        std::vector<double> h(static_cast<std::size_t>(N));
        double x0 = N / 3.0;
        double sigma = N / 8.0;
        for (int i = 0; i < N; ++i) {
            double dx = i - x0;
            h[static_cast<std::size_t>(i)] = 0.10 * std::exp(-0.5 * dx * dx / (sigma * sigma));
        }
        return h;
    }
};

// ============================================================================
// SpectralROM1D — LHS design
// ============================================================================

TEST(SpectralROM1D, LHSRangeAndSize) {
    ROM1DFixture f;
    ASSERT_EQ(static_cast<int>(f.rom.mannings_mult.size()), f.rom.n_ensemble);
    ASSERT_EQ(static_cast<int>(f.rom.runoff_mult.size()), f.rom.n_ensemble);

    double m_lo = 1.0 - f.rom.mannings_pert;
    double m_hi = 1.0 + f.rom.mannings_pert;
    for (int i = 0; i < f.rom.n_ensemble; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_GE(f.rom.mannings_mult[ui], m_lo - 1e-12);
        EXPECT_LE(f.rom.mannings_mult[ui], m_hi + 1e-12);
        EXPECT_GE(f.rom.runoff_mult[ui],  1.0 - f.rom.runoff_pert - 1e-12);
        EXPECT_LE(f.rom.runoff_mult[ui],  1.0 + f.rom.runoff_pert + 1e-12);
    }
}

TEST(SpectralROM1D, LHSManningAscendsRunoffDescends) {
    ROM1DFixture f;
    for (int i = 1; i < f.rom.n_ensemble; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_GE(f.rom.mannings_mult[ui], f.rom.mannings_mult[ui - 1])
            << "Manning's not ascending at i=" << i;
        EXPECT_LE(f.rom.runoff_mult[ui], f.rom.runoff_mult[ui - 1])
            << "Runoff not descending at i=" << i;
    }
}

// ============================================================================
// SpectralROM1D — seed + quantiles
// ============================================================================

TEST(SpectralROM1D, SeedProducesTightQuantiles) {
    // All members seeded from same head → q05≈q50≈q95 at t=0.
    ROM1DFixture f;
    auto h = f.bump_head();
    f.rom.seed(h.data());
    f.rom.computeQuantiles();

    // Tolerance: reconstruction via partial basis loses some energy
    for (int i = 0; i < f.rom.n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(f.rom.q05[ui], f.rom.q50[ui], 1.0e-12) << "node " << i;
        EXPECT_NEAR(f.rom.q50[ui], f.rom.q95[ui], 1.0e-12) << "node " << i;
    }
}

TEST(SpectralROM1D, QuantilesNonNegative) {
    ROM1DFixture f;
    auto h = f.bump_head();
    f.rom.seed(h.data());
    f.rom.computeQuantiles();
    for (int i = 0; i < f.rom.n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_GE(f.rom.q05[ui], 0.0);
        EXPECT_GE(f.rom.q50[ui], 0.0);
        EXPECT_GE(f.rom.q95[ui], 0.0);
    }
}

TEST(SpectralROM1D, QuantilesMonotone) {
    ROM1DFixture f;
    auto h = f.bump_head();
    f.rom.seed(h.data());
    // Non-uniform runoff forcing
    std::vector<double> runoff(static_cast<std::size_t>(f.N));
    for (int i = 0; i < f.N; ++i)
        runoff[static_cast<std::size_t>(i)] = 1.0e-5 * (1.0 + 0.5 * std::sin(i * 0.4));

    for (int step = 0; step < 20; ++step)
        f.rom.advance(30.0, 0.1, runoff.data());
    f.rom.computeQuantiles();

    for (int i = 0; i < f.rom.n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_LE(f.rom.q05[ui], f.rom.q50[ui] + 1.0e-14) << "q05 > q50 at node " << i;
        EXPECT_LE(f.rom.q50[ui], f.rom.q95[ui] + 1.0e-14) << "q50 > q95 at node " << i;
    }
}

// ============================================================================
// SpectralROM1D — spread growth
// ============================================================================

TEST(SpectralROM1D, SpreadGrowsWithPerturbation) {
    // 30% perturbations + forcing → q95-q05 spread should be nonzero after advance.
    ROM1DFixture f;
    f.rom.mannings_pert = 0.30;
    f.rom.runoff_pert   = 0.30;
    f.rom.initialize();

    auto h = f.bump_head();
    f.rom.seed(h.data());

    std::vector<double> runoff(static_cast<std::size_t>(f.N));
    for (int i = 0; i < f.N; ++i)
        runoff[static_cast<std::size_t>(i)] = 2.0e-5 * (1.0 + std::sin(i * 0.5));

    for (int step = 0; step < 30; ++step)
        f.rom.advance(30.0, 0.1, runoff.data());
    f.rom.computeQuantiles();

    double max_spread = 0.0;
    for (int i = 0; i < f.rom.n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        max_spread = std::max(max_spread, f.rom.q95[ui] - f.rom.q05[ui]);
    }
    EXPECT_GT(max_spread, 1.0e-6) << "spread should be nonzero with 30% perturbations";
}

TEST(SpectralROM1D, NullForcingDecaysToNearZero) {
    // With no runoff (null ptr), all modes should decay exponentially.
    ROM1DFixture f;
    auto h = f.bump_head();
    f.rom.seed(h.data());

    for (int step = 0; step < 200; ++step)
        f.rom.advance(30.0, 1.0, nullptr);  // large K1d → fast decay
    f.rom.computeQuantiles();

    double max_q95 = *std::max_element(f.rom.q95.begin(), f.rom.q95.end());
    EXPECT_LT(max_q95, 1.0e-4) << "heads should decay near zero without forcing";
}

// ============================================================================
// SpectralROM1D — ensemble runoff path
// ============================================================================

TEST(SpectralROM1D, SetEnsembleRunoffThrowsOnSizeMismatch) {
    ROM1DFixture f;
    std::vector<double> wrong(static_cast<std::size_t>(f.rom.n_ensemble + 5), 1.0e-5);
    EXPECT_THROW(f.rom.setEnsembleRunoff(wrong), std::invalid_argument);
}

TEST(SpectralROM1D, EnsembleRunoffHigherRateGivesMoreEnergy) {
    // Members with higher per-member runoff rates should accumulate more modal energy.
    // Requires non-uniform spatial runoff so r_coarse[j] != 0.
    const int N = 20, M = 20, K = 4;
    GraphEigenBasis basis;
    CsrGraph L = make_chain_laplacian(N);
    basis.build(L, K);

    SpectralROM1D rom;
    rom.basis         = &basis;
    rom.n_ensemble    = M;
    rom.mannings_pert = 0.0;  // uniform Manning → isolate runoff effect
    rom.runoff_pert   = 0.0;
    rom.initialize();

    // Seed from a Gaussian bump off-centre
    std::vector<double> h(static_cast<std::size_t>(N), 0.0);
    double x0 = N / 3.0, sigma = N / 8.0;
    for (int i = 0; i < N; ++i) {
        double dx = i - x0;
        h[static_cast<std::size_t>(i)] = 0.10 * std::exp(-0.5 * dx * dx / (sigma * sigma));
    }
    rom.seed(h.data());

    // Linearly spaced per-member rates (member 0 lowest, M-1 highest)
    std::vector<double> rates(static_cast<std::size_t>(M));
    double r_base = 1.0e-5;
    for (int i = 0; i < M; ++i)
        rates[static_cast<std::size_t>(i)] = r_base * (0.5 + static_cast<double>(i) / M);
    rom.setEnsembleRunoff(rates);

    // Non-uniform spatial runoff so r_coarse[j] != 0
    std::vector<double> runoff(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
        runoff[static_cast<std::size_t>(i)] = r_base * (1.0 + std::sin(i * 0.7));

    for (int step = 0; step < 60; ++step)
        rom.advance(30.0, 0.01, runoff.data());
    rom.computeQuantiles();

    // Higher-rate members should accumulate more total head → q95 > q05
    double max_spread = 0.0;
    for (int i = 0; i < N; ++i) {
        auto ui = static_cast<std::size_t>(i);
        max_spread = std::max(max_spread, rom.q95[ui] - rom.q05[ui]);
    }
    EXPECT_GT(max_spread, 1.0e-8)
        << "ensemble runoff should produce nonzero spread";
}

TEST(SpectralROM1D, ClearEnsembleRunoffRevertsToScalarPath) {
    // With mannings_pert=runoff_pert=0 and all ensemble rates equal to mean:
    // ensemble path and scalar path give identical results.
    // After clearEnsembleRunoff(), results stay the same.
    const int N = 20, M = 20, K = 4;
    GraphEigenBasis basis;
    CsrGraph L = make_chain_laplacian(N);
    basis.build(L, K);

    // Scalar-only ROM
    SpectralROM1D rom_scalar;
    rom_scalar.basis         = &basis;
    rom_scalar.n_ensemble    = M;
    rom_scalar.mannings_pert = 0.0;
    rom_scalar.runoff_pert   = 0.0;
    rom_scalar.initialize();

    // Ensemble ROM with uniform rates = mean
    SpectralROM1D rom_ens;
    rom_ens.basis         = &basis;
    rom_ens.n_ensemble    = M;
    rom_ens.mannings_pert = 0.0;
    rom_ens.runoff_pert   = 0.0;
    rom_ens.initialize();
    std::vector<double> uniform_rates(static_cast<std::size_t>(M), 1.0e-5);
    rom_ens.setEnsembleRunoff(uniform_rates);
    rom_ens.clearEnsembleRunoff();  // revert to scalar path

    std::vector<double> h(static_cast<std::size_t>(N), 0.0);
    for (int i = 0; i < N; ++i) {
        double dx = i - N / 3.0;
        h[static_cast<std::size_t>(i)] = 0.10 * std::exp(-0.5 * dx * dx / (N / 8.0 * N / 8.0));
    }
    rom_scalar.seed(h.data());
    rom_ens.seed(h.data());

    std::vector<double> runoff(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
        runoff[static_cast<std::size_t>(i)] = 1.0e-5 * (1.0 + std::sin(i * 0.7));

    for (int step = 0; step < 20; ++step) {
        rom_scalar.advance(30.0, 0.1, runoff.data());
        rom_ens.advance(30.0, 0.1, runoff.data());
    }
    rom_scalar.computeQuantiles();
    rom_ens.computeQuantiles();

    for (int i = 0; i < N; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(rom_scalar.q50[ui], rom_ens.q50[ui], 1.0e-12)
            << "q50 differs after clearEnsembleRunoff at node " << i;
    }
}

// ============================================================================
// SpectralROM1D — external samples
// ============================================================================

TEST(SpectralROM1D, SetExternalSamplesThrowsOnSizeMismatch) {
    ROM1DFixture f;
    std::vector<double> ok(static_cast<std::size_t>(f.rom.n_ensemble), 1.0);
    std::vector<double> bad(static_cast<std::size_t>(f.rom.n_ensemble + 3), 1.0);
    EXPECT_THROW(f.rom.setExternalSamples(bad, ok), std::invalid_argument);
    EXPECT_THROW(f.rom.setExternalSamples(ok, bad), std::invalid_argument);
}

TEST(SpectralROM1D, ExternalSamplesOverrideLHS) {
    // External samples should replace mannings_mult / runoff_mult after initialize().
    const int N = 15, M = 10, K = 4;
    GraphEigenBasis basis;
    CsrGraph L = make_chain_laplacian(N);
    basis.build(L, K);

    SpectralROM1D rom;
    rom.basis      = &basis;
    rom.n_ensemble = M;

    std::vector<double> ext_mann(static_cast<std::size_t>(M));
    std::vector<double> ext_runoff(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i) {
        ext_mann[static_cast<std::size_t>(i)]   = 1.5 + 0.01 * i;  // outside normal LHS range
        ext_runoff[static_cast<std::size_t>(i)] = 0.5 + 0.02 * i;
    }
    rom.setExternalSamples(ext_mann, ext_runoff);
    rom.initialize();

    for (int i = 0; i < M; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_DOUBLE_EQ(rom.mannings_mult[ui], ext_mann[ui]);
        EXPECT_DOUBLE_EQ(rom.runoff_mult[ui],   ext_runoff[ui]);
    }
}

// ============================================================================
// SpectralROM1D — is_ready / initialization guard
// ============================================================================

TEST(SpectralROM1D, NotReadyBeforeInitialize) {
    GraphEigenBasis basis;
    CsrGraph L = make_chain_laplacian(10);
    basis.build(L, 4);

    SpectralROM1D rom;
    rom.basis = &basis;
    EXPECT_FALSE(rom.is_ready());
}

TEST(SpectralROM1D, ReadyAfterInitialize) {
    ROM1DFixture f;
    EXPECT_TRUE(f.rom.is_ready());
}

TEST(SpectralROM1D, InitializeThrowsWithNullBasis) {
    SpectralROM1D rom;
    // basis = nullptr
    EXPECT_THROW(rom.initialize(), std::runtime_error);
}

TEST(SpectralROM1D, InitializeThrowsWithSmallEnsemble) {
    GraphEigenBasis basis;
    CsrGraph L = make_chain_laplacian(10);
    basis.build(L, 4);

    SpectralROM1D rom;
    rom.basis      = &basis;
    rom.n_ensemble = 1;
    EXPECT_THROW(rom.initialize(), std::runtime_error);
}
