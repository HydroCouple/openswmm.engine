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
#include "uncertainty/FiedlerDiagnostic1D.hpp"
#include "uncertainty/NetworkLaplacian1D.hpp"
#include "uncertainty/SpectralROM1D.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
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
// PR 4 — grounded Laplacian for outfall boundaries
// ============================================================================

// Build an n-node chain 0-1-..-(n-1) with the last node an outfall.
// With ground_outfalls=true the interior endpoint of the outfall-adjacent
// conduit gains a Dirichlet diagonal term, making the operator SPD.
static CsrGraph make_grounded_chain(int n, std::vector<int>& active_map,
                                    std::vector<int>& full_to_active,
                                    bool ground = true) {
    const int nc = n - 1;
    std::vector<int> n1(static_cast<std::size_t>(nc));
    std::vector<int> n2(static_cast<std::size_t>(nc));
    for (int k = 0; k < nc; ++k) {
        n1[static_cast<std::size_t>(k)] = k;
        n2[static_cast<std::size_t>(k)] = k + 1;
    }
    std::vector<int> is_outfall(static_cast<std::size_t>(n), 0);
    is_outfall[static_cast<std::size_t>(n - 1)] = 1;  // last node = outfall
    return NetworkLaplacian1D::buildUniform(n, nc, n1.data(), n2.data(),
                                            is_outfall.data(),
                                            active_map, full_to_active, ground);
}

TEST(GroundedLaplacian, PositiveDefiniteWithOutfall) {
    // 8-node chain, node 7 = outfall → 7 active nodes.  The grounded operator
    // is strictly positive definite: no null mode, smallest eigenvalue > 0.
    std::vector<int> active_map, full_to_active;
    CsrGraph L = make_grounded_chain(8, active_map, full_to_active);
    ASSERT_EQ(static_cast<int>(active_map.size()), 7);

    GraphEigenBasis basis;
    basis.null_tol = 1.0e-8;
    ASSERT_TRUE(basis.build(L, 5));
    EXPECT_EQ(basis.num_null, 0) << "grounded operator has no null space";
    EXPECT_GT(basis.eigenvalues[0], 1.0e-8)
        << "smallest eigenvalue must be strictly positive";
}

TEST(GroundedLaplacian, RitzResidualSmall) {
    // Each retained (λ_j, v_j) must satisfy ‖L·v_j − λ_j·v_j‖₂ ≤ 1e-8.
    std::vector<int> active_map, full_to_active;
    CsrGraph L = make_grounded_chain(8, active_map, full_to_active);

    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 5));
    const int n = basis.n_nodes;
    std::vector<double> y(static_cast<std::size_t>(n), 0.0);
    for (int j = 0; j < basis.num_kept; ++j) {
        const double* vj = &basis.P[static_cast<std::size_t>(j * n)];
        std::fill(y.begin(), y.end(), 0.0);
        csr_matvec(L, vj, y.data());
        const double lam = basis.eigenvalues[static_cast<std::size_t>(j)];
        double resid = 0.0;
        for (int i = 0; i < n; ++i) {
            const double d = y[static_cast<std::size_t>(i)]
                           - lam * vj[static_cast<std::size_t>(i)];
            resid += d * d;
        }
        EXPECT_LE(std::sqrt(resid), 1.0e-8) << "Ritz residual too large, mode " << j;
    }
}

TEST(GroundedLaplacian, UniformVectorProjectsNonzero) {
    // The constant vector is NOT orthogonal to the grounded eigenmodes:
    // most of its energy is representable by the retained modes.  This is the
    // behavioural point of grounding — uniform inputs now project non-trivially.
    std::vector<int> active_map, full_to_active;
    CsrGraph L = make_grounded_chain(8, active_map, full_to_active);

    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 5));
    const int n = basis.n_nodes;
    const double norm_sq = static_cast<double>(n);  // ‖1‖² = n

    double captured = 0.0;
    for (int j = 0; j < basis.num_kept; ++j) {
        const double* vj = &basis.P[static_cast<std::size_t>(j * n)];
        double dot = 0.0;
        for (int i = 0; i < n; ++i) dot += vj[static_cast<std::size_t>(i)];  // P[:,j]·1
        captured += dot * dot;
    }
    EXPECT_GE(captured, 0.5 * norm_sq)
        << "retained grounded modes must capture >= 50% of the constant vector";
}

TEST(GroundedLaplacian, UniformForcingCreatesSpread) {
    // A SpectralROM1D on a grounded basis, forced by a SPATIALLY UNIFORM runoff
    // field, develops ensemble spread — this fails on the ungrounded operator
    // (uniform forcing projects to exactly zero on every zero-mean Neumann mode).
    std::vector<int> active_map, full_to_active;
    CsrGraph L = make_grounded_chain(8, active_map, full_to_active);

    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 5));

    SpectralROM1D rom;
    rom.basis         = &basis;
    rom.full_to_active = full_to_active;
    rom.n_full_nodes  = 8;
    rom.n_ensemble    = 20;
    rom.mannings_pert = 0.20;
    rom.runoff_pert   = 0.20;
    rom.initialize();

    // Seed all members on a zero deterministic reference, then force uniformly.
    // Deviation form: spread comes from (runoff_mult − 1)·r_j with
    // r_j = P^T·(uniform runoff), nonzero only on the grounded operator.
    std::vector<double> h0(static_cast<std::size_t>(rom.n_nodes), 0.0);
    rom.seed(h0.data());

    std::vector<double> runoff(static_cast<std::size_t>(rom.n_nodes), 1.0e-4);  // uniform
    for (int step = 0; step < 10; ++step)
        rom.advance(10.0, 1.0e-3, h0.data(), runoff.data());
    rom.computeQuantiles(h0.data(), nullptr);

    // Interior active nodes must show spread (endpoints nearest the ground may be small).
    double max_spread = 0.0;
    for (int i = 0; i < rom.n_nodes; ++i)
        max_spread = std::max(max_spread,
                              rom.q95[static_cast<std::size_t>(i)]
                            - rom.q05[static_cast<std::size_t>(i)]);
    EXPECT_GT(max_spread, 1.0e-10)
        << "uniform forcing on a grounded basis must produce ensemble spread";
}

TEST(GroundedLaplacian, NeumannFallbackPreserved) {
    // ground_outfalls=false restores the pure Neumann (topological) Laplacian.
    // The defining structural invariant is the row sum: a pure graph Laplacian
    // has EVERY row summing to zero, whereas grounding adds a Dirichlet diagonal
    // term to the outfall-adjacent interior node, making that one row sum equal
    // to the grounding weight.  (We test row sums rather than num_null because
    // GraphEigenBasis::lanczos starts from a zero-mean ramp deliberately
    // orthogonal to the constant null vector — so a connected chain's constant
    // null mode is never in the Krylov space and num_null stays 0 for both
    // operators; row sums are the operator-level discriminant.)
    auto row_sums = [](const CsrGraph& L) {
        std::vector<double> s(static_cast<std::size_t>(L.n), 0.0);
        for (int r = 0; r < L.n; ++r)
            for (int k = L.row_ptr[static_cast<std::size_t>(r)];
                 k < L.row_ptr[static_cast<std::size_t>(r + 1)]; ++k)
                s[static_cast<std::size_t>(r)] += L.values[static_cast<std::size_t>(k)];
        return s;
    };

    std::vector<int> am_n, fta_n, am_g, fta_g;
    CsrGraph L_neu = make_grounded_chain(8, am_n, fta_n, /*ground=*/false);
    CsrGraph L_gnd = make_grounded_chain(8, am_g, fta_g, /*ground=*/true);

    // Neumann: all row sums zero.
    for (double s : row_sums(L_neu))
        EXPECT_NEAR(s, 0.0, 1.0e-14) << "Neumann Laplacian rows must sum to zero";

    // Grounded: exactly one row (the outfall-adjacent interior node) sums to the
    // grounding weight (1.0 for the uniform chain); all other rows sum to zero.
    const auto sg = row_sums(L_gnd);
    int n_grounded_rows = 0;
    for (double s : sg) {
        if (std::abs(s) > 1.0e-12) {
            EXPECT_NEAR(s, 1.0, 1.0e-12) << "grounded row must sum to the conduit weight";
            ++n_grounded_rows;
        }
    }
    EXPECT_EQ(n_grounded_rows, 1)
        << "exactly one interior node is adjacent to the single outfall";
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

// PR 5: runoff is now an independent Fisher-Yates shuffle of the Manning
// strata (near-zero rank correlation), not a reversed column (rank
// correlation exactly -1).  Manning stays the ascending reference; runoff's
// contract is: LHS coverage exact (sorted columns match) and NOT descending.
TEST(SpectralROM1D, LHSManningAscendsRunoffShuffled) {
    ROM1DFixture f;
    const int M = f.rom.n_ensemble;

    for (int i = 1; i < M; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_GE(f.rom.mannings_mult[ui], f.rom.mannings_mult[ui - 1])
            << "Manning's not ascending at i=" << i;
    }

    bool descending = true;
    for (int i = 1; i < M; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (f.rom.runoff_mult[ui] > f.rom.runoff_mult[ui - 1]) { descending = false; break; }
    }
    EXPECT_FALSE(descending) << "runoff_mult should not be strictly descending";

    // Coverage: both columns stratify the same [1-p, 1+p] range with p=0.20
    // (see ROM1DFixture), so sorted runoff must equal sorted Manning exactly.
    auto sorted_mann = f.rom.mannings_mult;
    auto sorted_run  = f.rom.runoff_mult;
    std::sort(sorted_mann.begin(), sorted_mann.end());
    std::sort(sorted_run.begin(), sorted_run.end());
    for (int i = 0; i < M; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(sorted_run[ui], sorted_mann[ui], 1.0e-10)
            << "runoff strata coverage differs at i=" << i;
    }
}

// ============================================================================
// SpectralROM1D — seed + quantiles
// ============================================================================

TEST(SpectralROM1D, SeedProducesTightQuantiles) {
    // Deviation form: seed zeroes all deviations, so quantiles equal the
    // deterministic reference EXACTLY at t=0 (no basis-truncation loss).
    ROM1DFixture f;
    auto h = f.bump_head();
    f.rom.seed(h.data());
    f.rom.computeQuantiles(h.data(), nullptr);

    for (int i = 0; i < f.rom.n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_DOUBLE_EQ(f.rom.q05[ui], h[ui]) << "node " << i;
        EXPECT_DOUBLE_EQ(f.rom.q50[ui], h[ui]) << "node " << i;
        EXPECT_DOUBLE_EQ(f.rom.q95[ui], h[ui]) << "node " << i;
    }
}

TEST(SpectralROM1D, QuantilesClampedToInvert) {
    // With an invert floor supplied, no quantile may dip below it even when
    // deviations pull members downward.
    ROM1DFixture f;
    auto h = f.bump_head();
    f.rom.seed(h.data());

    std::vector<double> runoff(static_cast<std::size_t>(f.N));
    for (int i = 0; i < f.N; ++i)
        runoff[static_cast<std::size_t>(i)] = 1.0e-5 * (1.0 + 0.5 * std::sin(i * 0.4));
    for (int step = 0; step < 20; ++step)
        f.rom.advance(30.0, 0.1, h.data(), runoff.data());

    std::vector<double> invert(static_cast<std::size_t>(f.N), 0.0);
    f.rom.computeQuantiles(h.data(), invert.data());
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
        f.rom.advance(30.0, 0.1, h.data(), runoff.data());
    f.rom.computeQuantiles(h.data(), nullptr);

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
    // Deviation form: spread grows from the Manning-sensitivity term
    // (−λ·K1d·(1/mm−1)·b_j, b_j from the bump reference) plus (rm−1)·r_j.
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
        f.rom.advance(30.0, 0.1, h.data(), runoff.data());
    f.rom.computeQuantiles(h.data(), nullptr);

    double max_spread = 0.0;
    for (int i = 0; i < f.rom.n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        max_spread = std::max(max_spread, f.rom.q95[ui] - f.rom.q05[ui]);
    }
    EXPECT_GT(max_spread, 1.0e-6) << "spread should be nonzero with 30% perturbations";
}

TEST(SpectralROM1D, FrozenReferenceSaturatesAtAnalyticSteadyState) {
    // With b_j frozen (constant h_det) and no runoff, the deviation ODE has
    // the closed-form fixed point δa_ij = (mm_i − 1)·b_j  (DEVIATION_FORM.md
    // §4.3). After enough decay time every member must sit on it exactly.
    ROM1DFixture f;
    auto h = f.bump_head();
    f.rom.seed(h.data());

    for (int step = 0; step < 200; ++step)
        f.rom.advance(30.0, 1.0, h.data(), nullptr);  // large K1d → fast convergence

    // b_j = P[:,j]^T h
    const auto nn = static_cast<std::size_t>(f.rom.n_nodes);
    const auto nk = static_cast<std::size_t>(f.rom.n_kept);
    for (std::size_t j = 0; j < nk; ++j) {
        const double* Pj = &f.basis.P[j * nn];
        double bj = 0.0;
        for (std::size_t t = 0; t < nn; ++t) bj += Pj[t] * h[t];
        for (int m = 0; m < f.rom.n_ensemble; ++m) {
            const double mm       = f.rom.mannings_mult[static_cast<std::size_t>(m)];
            const double expected = (mm - 1.0) * bj;
            const double actual   =
                f.rom.a_ensemble[static_cast<std::size_t>(m) * nk + j];
            // Tolerance 1e-8: modes whose |b_j| falls below the activity
            // threshold stay frozen at 0 while the analytic value is ~1e-9.
            EXPECT_NEAR(actual, expected, 1.0e-8)
                << "member " << m << " mode " << j;
        }
    }
}

TEST(SpectralROM1D, DecayingReferenceDrivesDeviationsToZero) {
    // When the deterministic reference decays to zero, b_j → 0 drags every
    // deviation to zero with it: all members re-converge on the (zero)
    // deterministic trajectory and the quantile band collapses onto it.
    ROM1DFixture f;
    auto h = f.bump_head();
    f.rom.seed(h.data());

    std::vector<double> h_det = h;
    for (int step = 0; step < 400; ++step) {
        f.rom.advance(30.0, 1.0, h_det.data(), nullptr);
        for (double& v : h_det) v *= 0.9;  // geometric decay of the reference
    }
    f.rom.computeQuantiles(h_det.data(), nullptr);

    double max_q95 = *std::max_element(f.rom.q95.begin(), f.rom.q95.end());
    EXPECT_LT(max_q95, 1.0e-6)
        << "band must collapse onto the decayed (zero) reference";
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
        rom.advance(30.0, 0.01, h.data(), runoff.data());
    rom.computeQuantiles(h.data(), nullptr);

    // Rate spread (rate_i/mean − 1 ≠ 0) must produce head spread
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
        rom_scalar.advance(30.0, 0.1, h.data(), runoff.data());
        rom_ens.advance(30.0, 0.1, h.data(), runoff.data());
    }
    rom_scalar.computeQuantiles(h.data(), nullptr);
    rom_ens.computeQuantiles(h.data(), nullptr);

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

// ============================================================================
// FiedlerDiagnostic1D
// ============================================================================

// 6-node chain, outfalls at ends → 4 active nodes (full 1,2,3,4 → active 0,1,2,3).
// GraphEigenBasis::build requires n_nodes >= 4; a 5-node chain leaves only 3 active.
// full_to_active: {-1, 0, 1, 2, 3, -1}
static GraphEigenBasis make_chain_fiedler_basis(
        std::vector<int>& an1_out,
        std::vector<int>& an2_out,
        std::vector<double>& lens_out,
        std::vector<int>& full_to_active_out) {
    // Full graph: 6 nodes, conduits 0-1,...,4-5; outfalls at 0 and 5.
    int n1f[5] = {0,1,2,3,4};
    int n2f[5] = {1,2,3,4,5};
    int is_outfall[6] = {1,0,0,0,0,1};
    std::vector<int> active_map;
    // Neumann (topological) Laplacian: the Fiedler vector / algebraic connectivity
    // is defined for the ungrounded operator (λ₁ = 0, constant null mode).  Pass
    // ground_outfalls=false so these diagnostic tests exercise the operator they
    // were designed for, independent of the PR-4 grounded default.
    CsrGraph L = NetworkLaplacian1D::buildUniform(
        6, 5, n1f, n2f, is_outfall, active_map, full_to_active_out,
        /*ground_outfalls=*/false);
    // Active nodes: 0→full1, 1→full2, 2→full3, 3→full4.
    // Active-node conduit connectivity (internal conduits only):
    //   full 1-2 → active 0-1
    //   full 2-3 → active 1-2
    //   full 3-4 → active 2-3
    an1_out = {0, 1, 2};
    an2_out = {1, 2, 3};
    lens_out = {100.0, 100.0, 100.0};

    GraphEigenBasis basis;
    basis.build(L, 2);
    return basis;
}

TEST(FiedlerDiagnostic1D, ThrowsIfBasisNotReady) {
    FiedlerDiagnostic1D fd;
    // basis = nullptr
    EXPECT_THROW(fd.compute({0}, {1}, {}), std::runtime_error);
}

TEST(FiedlerDiagnostic1D, ThrowsOnLengthsMismatch) {
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    // lengths size 1 != n1 size 3 → invalid_argument
    EXPECT_THROW(fd.compute(an1, an2, {99.0}), std::invalid_argument);
}

TEST(FiedlerDiagnostic1D, IsReadyAfterCompute) {
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, lens);
    EXPECT_TRUE(fd.is_ready());
}

TEST(FiedlerDiagnostic1D, Lambda2MatchesBasisEigenvalue) {
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, lens);
    EXPECT_DOUBLE_EQ(fd.lambda2, basis.eigenvalues[0]);
}

TEST(FiedlerDiagnostic1D, Phi2MatchesBasisColumn0) {
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, lens);

    const int n = basis.n_nodes;
    for (int i = 0; i < n; ++i)
        EXPECT_DOUBLE_EQ(fd.phi2[static_cast<std::size_t>(i)],
                         basis.P[static_cast<std::size_t>(i)])
            << "phi2[" << i << "] != P[0*n+" << i << "]";
}

TEST(FiedlerDiagnostic1D, GradientNonNegative) {
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, lens);
    for (std::size_t i = 0; i < fd.grad.size(); ++i)
        EXPECT_GE(fd.grad[i], 0.0);
}

TEST(FiedlerDiagnostic1D, MaxGradientPositive) {
    // On a 3-active-node chain the Fiedler vector is non-constant → max grad > 0.
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, lens);
    double max_g = *std::max_element(fd.grad.begin(), fd.grad.end());
    EXPECT_GT(max_g, 0.0);
}

TEST(FiedlerDiagnostic1D, RankSortedDescending) {
    // Use a larger chain for a meaningful gradient distribution.
    std::vector<int> an1, an2;
    for (int k = 0; k < 9; ++k) { an1.push_back(k); an2.push_back(k + 1); }
    CsrGraph L = make_chain_laplacian(10);
    GraphEigenBasis basis;
    ASSERT_TRUE(basis.build(L, 4));

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, {});  // unit lengths

    for (std::size_t k = 1; k < fd.rank.size(); ++k) {
        EXPECT_GE(fd.grad[static_cast<std::size_t>(fd.rank[k - 1])],
                  fd.grad[static_cast<std::size_t>(fd.rank[k])])
            << "rank not descending at position " << k;
    }
}

TEST(FiedlerDiagnostic1D, GradAtActiveNodeMatchesGrad) {
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, lens);

    for (int ai = 0; ai < basis.n_nodes; ++ai)
        EXPECT_DOUBLE_EQ(fd.gradAtActiveNode(ai),
                         fd.grad[static_cast<std::size_t>(ai)]);
}

TEST(FiedlerDiagnostic1D, GradAtFullNodeMapsCorrectly) {
    // 5-node chain, outfalls at 0 and 4:
    //   full_to_active = {-1, 0, 1, 2, -1}
    // Full node 2 → active node 1 → fd.grad[1]
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, lens);

    // Full index 2 maps to active index 1
    EXPECT_DOUBLE_EQ(fd.gradAtFullNode(2, f2a), fd.grad[1]);
    // Full index 1 maps to active index 0
    EXPECT_DOUBLE_EQ(fd.gradAtFullNode(1, f2a), fd.grad[0]);
}

TEST(FiedlerDiagnostic1D, GradAtFullNodeZeroForOutfall) {
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, lens);

    // Outfall nodes (full 0 and 5) have full_to_active = -1 → 0.0
    EXPECT_DOUBLE_EQ(fd.gradAtFullNode(0, f2a), 0.0);
    EXPECT_DOUBLE_EQ(fd.gradAtFullNode(5, f2a), 0.0);
}

TEST(FiedlerDiagnostic1D, GradAtFullNodeZeroForOutOfRange) {
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, lens);

    EXPECT_DOUBLE_EQ(fd.gradAtFullNode(-1, f2a), 0.0);
    EXPECT_DOUBLE_EQ(fd.gradAtFullNode(999, f2a), 0.0);
}

TEST(FiedlerDiagnostic1D, CouplingAnnotation) {
    // Demonstrate coupling-path annotation: given a coupling point with
    // cp.node_idx (full SWMM index), retrieve the Fiedler bottleneck score
    // from the 1D diagnostic via gradAtFullNode(cp.node_idx, full_to_active).
    //
    // Setup: 6-node chain, outfalls at 0 and 5.  Coupling point at full node 2
    //        (second active node — expected nonzero gradient on the Fiedler vector).
    std::vector<int> an1, an2;
    std::vector<double> lens;
    std::vector<int> f2a;  // full_to_active for use with cp.node_idx
    GraphEigenBasis basis = make_chain_fiedler_basis(an1, an2, lens, f2a);
    ASSERT_TRUE(basis.is_ready());

    FiedlerDiagnostic1D fd;
    fd.basis = &basis;
    fd.compute(an1, an2, lens);
    ASSERT_TRUE(fd.is_ready());

    // Simulate coupling point at full node 2 (active index 1):
    const int cp_node_idx = 2;
    const double bottleneck_score = fd.gradAtFullNode(cp_node_idx, f2a);

    // Score must be >= 0; on a 4-active-node chain with a non-constant
    // Fiedler vector, an interior node should have nonzero gradient.
    EXPECT_GE(bottleneck_score, 0.0);
    EXPECT_GT(bottleneck_score, 0.0);

    // Outfall coupling points return 0 (no gradient defined for excluded nodes).
    EXPECT_DOUBLE_EQ(fd.gradAtFullNode(0, f2a), 0.0);
    EXPECT_DOUBLE_EQ(fd.gradAtFullNode(5, f2a), 0.0);
}

// ============================================================================
// PR 3 — Weighted Laplacian (H_sym static basis)
// ============================================================================

// Helper: build a 5-node chain with heterogeneous weights.
// Weights: conduit 0 = w_low, conduit 1 = w_high, conduit 2 = w_low, conduit 3 = w_low.
// n1/n2 connectivity: 0-1-2-3-4, no outfalls.
static CsrGraph make_weighted_chain(int n, const double* weights) {
    int nc = n - 1;
    std::vector<int> n1v(static_cast<std::size_t>(nc));
    std::vector<int> n2v(static_cast<std::size_t>(nc));
    for (int k = 0; k < nc; ++k) {
        n1v[static_cast<std::size_t>(k)] = k;
        n2v[static_cast<std::size_t>(k)] = k + 1;
    }
    std::vector<int> is_outfall(static_cast<std::size_t>(n), 0);
    std::vector<int> active_map, full_to_active;
    return NetworkLaplacian1D::buildWeighted(n, nc, n1v.data(), n2v.data(),
                                             is_outfall.data(), weights,
                                             active_map, full_to_active);
}

// ============================================================================
// PR 1 — Warm-start Lanczos
// ============================================================================

TEST(GraphEigenBasisWarmStart, ColdStartFallbackWhenV0Null) {
    // v0_block == nullptr must produce the same result as before (regression guard).
    CsrGraph L = make_chain_laplacian(20);
    GraphEigenBasis b1, b2;
    ASSERT_TRUE(b1.build(L, 5, nullptr));
    ASSERT_TRUE(b2.build(L, 5));  // default v0_block=nullptr
    ASSERT_EQ(b1.num_kept, b2.num_kept);
    for (int j = 0; j < b1.num_kept; ++j)
        EXPECT_NEAR(b1.eigenvalues[static_cast<std::size_t>(j)],
                    b2.eigenvalues[static_cast<std::size_t>(j)], 1e-10)
            << "eigenvalue " << j;
}

TEST(GraphEigenBasisWarmStart, ColdAndWarmBothFindTheKSmallestEigenvalues) {
    // CONTRACT: the Krylov starting vector is an implementation detail. Cold
    // or warm, build(L, k) must return the k SMALLEST nontrivial eigenvalues
    // of L — so both paths must agree with the analytic spectrum AND with
    // each other. Asserted across several chain lengths so the result cannot
    // depend on one lucky (n, k) configuration.
    //
    // HISTORY — this test previously asserted only the warm path, on n = 20
    // alone, above a comment claiming the cold-start ramp "can only excite
    // odd-k eigenmodes (k=1,3,5,7,9) and misses even modes" while "the
    // warm-start sum-of-columns starting vector is not purely anti-symmetric
    // and correctly finds all k=1..5 modes". BOTH halves of that premise were
    // false:
    //   * the ramp's blind spot was a genuine DEFECT, not an acceptable
    //     quirk — it returned λ_1,λ_3,λ_5,λ_7,λ_9 while reporting them as the
    //     five smallest, and λ_2 < λ_3;
    //   * the warm start did NOT escape it. Summing columns that are every
    //     one of them antisymmetric yields another antisymmetric vector,
    //     landing back in the identical invariant subspace.
    // The test only ever passed because it was never wired into CMake and so
    // had never run. Fixed at the source (GraphEigenBasis::lanczos(): the
    // cold start is now a zero-mean deterministic pseudo-random vector, which
    // has generic components on every eigenvector), which also fixes the warm
    // path transitively.
    for (int n : {6, 10, 15, 20, 30}) {
        SCOPED_TRACE("chain length n=" + std::to_string(n));
        const int k = 5;
        CsrGraph L = make_chain_laplacian(n);

        GraphEigenBasis cold;
        ASSERT_TRUE(cold.build(L, k));
        ASSERT_EQ(cold.num_kept, k);

        GraphEigenBasis warm;
        ASSERT_TRUE(warm.build(L, k, cold.P.data()));
        ASSERT_EQ(warm.num_kept, k);

        // Analytic path-graph Laplacian spectrum: λ_j = 2(1 − cos(jπ/n)),
        // ascending in j — so the k smallest nontrivial are j = 1..k with NO
        // parity gaps.
        for (int j = 0; j < k; ++j) {
            const double expected = 2.0 * (1.0 - std::cos((j + 1) * M_PI / n));
            EXPECT_NEAR(cold.eigenvalues[static_cast<std::size_t>(j)],
                        expected, 1e-6)
                << "COLD eigenvalue " << j << " is not the " << (j + 1)
                << "-th smallest (parity blind spot regression?)";
            EXPECT_NEAR(warm.eigenvalues[static_cast<std::size_t>(j)],
                        expected, 1e-6)
                << "WARM eigenvalue " << j << " is not the " << (j + 1)
                << "-th smallest";
        }
    }
}

TEST(GraphEigenBasisWarmStart, ColdStartReachesSymmetricEigenvectors) {
    // Direct regression guard on the defect itself, independent of the
    // eigenvalue bookkeeping above: on a left-right symmetric chain, the
    // even-k eigenvectors are SYMMETRIC under i → n−1−i. An antisymmetric
    // start vector (the old linear ramp) can never represent them, at any
    // Krylov dimension. Assert the retained basis actually contains a
    // symmetric eigenvector — i.e. the start vector is not confined to the
    // antisymmetric invariant subspace.
    const int n = 20;
    CsrGraph L = make_chain_laplacian(n);
    GraphEigenBasis b;
    ASSERT_TRUE(b.build(L, 5));

    int n_symmetric = 0, n_antisymmetric = 0;
    for (int j = 0; j < b.num_kept; ++j) {
        const double* v = &b.P[static_cast<std::size_t>(j * n)];
        // Compare v against its own index-reversal.
        double d_sym = 0.0, d_anti = 0.0;
        for (int i = 0; i < n; ++i) {
            const double vi = v[static_cast<std::size_t>(i)];
            const double vr = v[static_cast<std::size_t>(n - 1 - i)];
            d_sym  += (vi - vr) * (vi - vr);
            d_anti += (vi + vr) * (vi + vr);
        }
        if (std::sqrt(d_sym)  < 1e-8) ++n_symmetric;
        if (std::sqrt(d_anti) < 1e-8) ++n_antisymmetric;
    }
    EXPECT_GT(n_symmetric, 0)
        << "no symmetric eigenvector in the retained basis — the Krylov start "
           "is trapped in the antisymmetric invariant subspace (the pre-fix "
           "linear-ramp defect)";
    EXPECT_GT(n_antisymmetric, 0)
        << "no antisymmetric eigenvector either — unexpected for a path graph";
}

TEST(GraphEigenBasisWarmStart, WarmStartSignsAligned) {
    // After a warm-start rebuild, every eigenvector must have a NON-NEGATIVE
    // inner product with the corresponding column of v0_block.
    CsrGraph L = make_chain_laplacian(20);
    GraphEigenBasis cold;
    ASSERT_TRUE(cold.build(L, 5));

    GraphEigenBasis warm;
    ASSERT_TRUE(warm.build(L, 5, cold.P.data()));

    const int n = warm.n_nodes;
    for (int j = 0; j < warm.num_kept; ++j) {
        auto uj = static_cast<std::size_t>(j);
        const double* pj  = &warm.P[uj * static_cast<std::size_t>(n)];
        const double* ref = &cold.P[uj * static_cast<std::size_t>(n)];
        double dot = 0.0;
        for (int i = 0; i < n; ++i)
            dot += pj[static_cast<std::size_t>(i)]
                 * ref[static_cast<std::size_t>(i)];
        EXPECT_GE(dot, 0.0) << "mode " << j << " has negative alignment";
    }
}

TEST(GraphEigenBasisWarmStart, SignFlipInV0IsCompensated) {
    // Sign alignment aligns each P[:,j] WITH v0_block[:,j].
    // So if v0_block[:,0] = -cold.P[:,0], the warm result aligns with that
    // flipped reference (warm.P[:,0] · v0_block[:,0] >= 0), NOT with cold.P[:,0].
    // This test verifies the actual invariant: alignment with the provided v0.
    CsrGraph L = make_chain_laplacian(20);
    GraphEigenBasis cold;
    ASSERT_TRUE(cold.build(L, 5));

    // Flip sign of column 0 in a copy
    std::vector<double> v0_flipped(cold.P);
    const int n = cold.n_nodes;
    for (int i = 0; i < n; ++i)
        v0_flipped[static_cast<std::size_t>(i)] =
            -v0_flipped[static_cast<std::size_t>(i)];

    GraphEigenBasis warm;
    ASSERT_TRUE(warm.build(L, 5, v0_flipped.data()));

    // After sign alignment: warm.P[:,j] must have non-negative dot with
    // v0_block[:,j] for all j.
    for (int j = 0; j < warm.num_kept; ++j) {
        auto uj = static_cast<std::size_t>(j);
        const double* pw = &warm.P[uj * static_cast<std::size_t>(n)];
        const double* rv = &v0_flipped[uj * static_cast<std::size_t>(n)];
        double dot = 0.0;
        for (int i = 0; i < n; ++i)
            dot += pw[static_cast<std::size_t>(i)] * rv[static_cast<std::size_t>(i)];
        EXPECT_GE(dot, 0.0) << "mode " << j << " must align with its v0 column";
    }
}

// ============================================================================
// PR 3 — Weighted Laplacian (H_sym static basis)
// ============================================================================

TEST(WeightedLaplacian, DiffersFromUniform) {
    // A 5-node chain where conduit 1 has 10× higher conductance.
    // Uniform and weighted Laplacians should yield different eigenvalues.
    const int N = 5;
    const double w_low = 1.0, w_high = 10.0;
    double weights[4] = {w_low, w_high, w_low, w_low};

    CsrGraph L_unif = make_chain_laplacian(N);
    CsrGraph L_wt   = make_weighted_chain(N, weights);

    GraphEigenBasis b_unif, b_wt;
    ASSERT_TRUE(b_unif.build(L_unif, 3));
    ASSERT_TRUE(b_wt.build(L_wt, 3));

    // At least one eigenvalue must differ by more than numerical noise.
    bool any_different = false;
    for (int j = 0; j < b_unif.num_kept && j < b_wt.num_kept; ++j) {
        if (std::fabs(b_unif.eigenvalues[static_cast<std::size_t>(j)]
                    - b_wt.eigenvalues[static_cast<std::size_t>(j)]) > 1e-6)
            any_different = true;
    }
    EXPECT_TRUE(any_different)
        << "Weighted Laplacian with 10× conductance ratio must yield different eigenvalues";
}

TEST(WeightedLaplacian, FiedlerPlacesHighConductanceCorrectly) {
    // On a 5-node chain with one high-conductance conduit (index 1, between
    // nodes 1 and 2), the Fiedler vector should show that the network splits
    // most easily somewhere other than across conduit 1.
    // We verify: the Fiedler vector components at nodes 1 and 2 (flanking the
    // high-conductance conduit) are closer together than for the uniform case.
    const int N = 5;
    double weights[4] = {1.0, 10.0, 1.0, 1.0};

    CsrGraph L_unif = make_chain_laplacian(N);
    CsrGraph L_wt   = make_weighted_chain(N, weights);

    GraphEigenBasis b_unif, b_wt;
    ASSERT_TRUE(b_unif.build(L_unif, 1));
    ASSERT_TRUE(b_wt.build(L_wt, 1));

    // Fiedler vector = P[:,0] (first retained mode after null filtering)
    // Difference across conduit 1 (nodes 1-2)
    auto diff = [](const GraphEigenBasis& b, int n_a, int n_b) {
        return std::fabs(b.P[static_cast<std::size_t>(n_a)]
                       - b.P[static_cast<std::size_t>(n_b)]);
    };
    double delta_unif = diff(b_unif, 1, 2);
    double delta_wt   = diff(b_wt,   1, 2);

    // High conductance between 1-2 → Fiedler gap there is SMALLER than uniform.
    EXPECT_LT(delta_wt, delta_unif)
        << "High-conductance conduit should reduce Fiedler gap across it";
}

TEST(WeightedLaplacian, DryNetworkFallsBackToUniform) {
    // All weights at or below floor (1e-10) → buildWeighted clamps to 1e-10,
    // which produces a valid Laplacian proportional to the uniform one.
    // null_tol must be set below the floor eigenvalue (~7.6e-11 for a 5-node
    // chain at weight 1e-10) so the modes are not discarded as null modes.
    const int N = 5;
    double weights[4] = {0.0, 0.0, 0.0, 0.0};  // all zero — floor applied

    CsrGraph L_wt = make_weighted_chain(N, weights);

    GraphEigenBasis basis;
    basis.null_tol = 1.0e-12;  // below floor eigenvalue (~7.6e-11)
    bool ok = basis.build(L_wt, 3);

    // Must build successfully (fallback to floor weights, not NaN/crash).
    EXPECT_TRUE(ok);
    EXPECT_GT(basis.num_kept, 0);
    for (int j = 0; j < basis.num_kept; ++j)
        EXPECT_GT(basis.eigenvalues[static_cast<std::size_t>(j)], 0.0);
}

// ============================================================================
// PR 4 — time-varying basis update (updateBasis)
// ============================================================================

// Helper: build a minimal SpectralROM1D on a 6-node chain, seeded with a
// ramp head field.  Caller takes ownership of the returned basis.
static std::unique_ptr<GraphEigenBasis>
make_rom1d_chain(int n_nodes, SpectralROM1D& rom, double mann_pert = 0.10) {
    // Build uniform-weight Laplacian for a chain: node 0-1-2-..-(n-1)
    // with no outfalls (n_full_nodes == n_active == n_nodes)
    const int n_conduits = n_nodes - 1;
    std::vector<double> w(static_cast<std::size_t>(n_conduits), 1.0);
    std::vector<int> n1(static_cast<std::size_t>(n_conduits));
    std::vector<int> n2(static_cast<std::size_t>(n_conduits));
    for (int ci = 0; ci < n_conduits; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }
    std::vector<int> is_outfall(static_cast<std::size_t>(n_nodes), 0);
    std::vector<int> active_map, full_to_active;
    CsrGraph L = NetworkLaplacian1D::buildWeighted(
        n_nodes, n_conduits, n1.data(), n2.data(),
        is_outfall.data(), w.data(), active_map, full_to_active);

    auto basis_owned = std::make_unique<GraphEigenBasis>();
    EXPECT_TRUE(basis_owned->build(L, 4));

    rom.basis          = basis_owned.get();
    rom.full_to_active = std::move(full_to_active);
    rom.n_full_nodes   = n_nodes;
    rom.n_ensemble     = 10;
    rom.mannings_pert  = mann_pert;
    rom.runoff_pert    = 0.0;
    rom.initialize();

    // Seed with a linear ramp: h[i] = (i+1) * 0.1
    std::vector<double> h(static_cast<std::size_t>(n_nodes));
    for (int i = 0; i < n_nodes; ++i)
        h[static_cast<std::size_t>(i)] = (i + 1) * 0.1;
    rom.seed(h.data());
    return basis_owned;
}

TEST(UpdateBasis, SkippedWhenOperatorUnchanged) {
    // Call updateBasis twice with identical conduit_off → second call returns
    // early and does not rebuild (basis pointer stays the same).
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;  // disable time guard for unit test

    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }
    std::vector<double> w(static_cast<std::size_t>(NC), 0.01);  // some non-trivial weight

    // First call: establishes prev snapshot
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC);
    const GraphEigenBasis* ptr_after_first = rom.basis;

    // Second call with identical weights: should skip
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC);
    const GraphEigenBasis* ptr_after_second = rom.basis;

    EXPECT_EQ(ptr_after_first, ptr_after_second)
        << "updateBasis must return early when operator is unchanged";
}

TEST(UpdateBasis, FiresWhenOperatorChangesSignificantly) {
    // Call updateBasis with weights that differ by > 5% → rebuild fires and
    // eigenvalues change.
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;  // disable time guard for unit test

    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }

    // Initial call with uniform weights
    std::vector<double> w0(static_cast<std::size_t>(NC), 0.01);
    rom.updateBasis(w0.data(), n1.data(), n2.data(), NC);
    std::vector<double> eigs_before(rom.basis->eigenvalues);

    // Second call: central conduit weight doubles (50% change > 5% tol)
    std::vector<double> w1 = w0;
    w1[NC / 2] *= 2.0;
    rom.updateBasis(w1.data(), n1.data(), n2.data(), NC);
    std::vector<double> eigs_after(rom.basis->eigenvalues);

    bool any_changed = false;
    for (std::size_t j = 0; j < eigs_before.size() && j < eigs_after.size(); ++j)
        if (std::fabs(eigs_before[j] - eigs_after[j]) > 1e-6)
            any_changed = true;

    EXPECT_TRUE(any_changed)
        << "50% conduit weight change must trigger eigenbasis rebuild";
}

TEST(UpdateBasis, ReProjectionPreservesCoeffNorm) {
    // After a small weight perturbation (20%), the eigenvectors rotate slightly
    // so R = P_new^T*P_old ≈ I, and the Frobenius norm of the coefficient matrix
    // is approximately preserved (within 20% relative).
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    // Default mannings_pert (0.10): the Manning-sensitivity term is what
    // builds non-zero deviations against the ramp reference below.
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;  // disable time guard for unit test

    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }

    // Advance against the ramp reference to build non-zero deviations
    std::vector<double> h_det(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
        h_det[static_cast<std::size_t>(i)] = (i + 1) * 0.1;
    for (int step = 0; step < 10; ++step)
        rom.advance(10.0, 0.1, h_det.data(), nullptr);

    // Record Frobenius norm before
    double norm_before = 0.0;
    for (double v : rom.a_ensemble) norm_before += v * v;
    ASSERT_GT(norm_before, 0.0) << "deviations must be non-zero before rebuild";

    // Prime the skip criterion with current uniform weights
    std::vector<double> w0(static_cast<std::size_t>(NC), 1.0);
    rom.updateBasis(w0.data(), n1.data(), n2.data(), NC);

    // 20% increase on one conduit — above the 5% skip threshold, below the
    // level where eigenvectors rotate dramatically.
    std::vector<double> w1 = w0;
    w1[2] = 1.2;
    rom.updateBasis(w1.data(), n1.data(), n2.data(), NC);

    double norm_after = 0.0;
    for (double v : rom.a_ensemble) norm_after += v * v;

    // Re-projection with small rotation: norm should be within 20% of original.
    EXPECT_NEAR(norm_after, norm_before, norm_before * 0.20 + 1e-15)
        << "20% weight change should preserve coefficient norm within 20%";
}

TEST(UpdateBasis, QuantilesRemainValidAfterUpdate) {
    // updateBasis + advance + computeQuantiles must always give q05 <= q50 <= q95.
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;  // disable time guard for unit test

    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }

    // Ramp reference + zero invert floor for quantile reconstruction
    std::vector<double> h_det(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
        h_det[static_cast<std::size_t>(i)] = (i + 1) * 0.1;
    std::vector<double> invert(static_cast<std::size_t>(N), 0.0);

    // Simulate a series of weight updates (growing conductance over time)
    for (int step = 0; step < 5; ++step) {
        double scale = 0.005 * (1.0 + step * 0.5);
        std::vector<double> w(static_cast<std::size_t>(NC), scale);
        w[2] = scale * 3.0;  // one conduit always wetter
        rom.updateBasis(w.data(), n1.data(), n2.data(), NC);
        rom.advance(10.0, scale * 100.0, h_det.data(), nullptr);
        rom.computeQuantiles(h_det.data(), invert.data());

        for (int i = 0; i < rom.n_nodes; ++i) {
            auto ui = static_cast<std::size_t>(i);
            EXPECT_LE(rom.q05[ui], rom.q50[ui] + 1e-10)
                << "step " << step << " node " << i;
            EXPECT_LE(rom.q50[ui], rom.q95[ui] + 1e-10)
                << "step " << step << " node " << i;
            EXPECT_GE(rom.q05[ui], 0.0)
                << "step " << step << " node " << i;
        }
    }
}

TEST(UpdateBasis, CountersTrackSuccessfulRebuild) {
    // A single successful rebuild must increment basis_updates_attempted_
    // without incrementing basis_updates_failed_.
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;  // disable time guard for unit test

    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }
    std::vector<double> w0(static_cast<std::size_t>(NC), 0.01);

    rom.updateBasis(w0.data(), n1.data(), n2.data(), NC);

    EXPECT_EQ(rom.basis_updates_attempted_, 1);
    EXPECT_EQ(rom.basis_updates_failed_, 0);
}

TEST(UpdateBasis, FailedRebuildConsumesInterval) {
    // A rebuild that fails (topology-mismatch guard) must still consume the
    // retry interval so a persistently-failing rebuild backs off instead of
    // retrying full Lanczos on every routing step.
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 100.0;

    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }
    std::vector<double> w(static_cast<std::size_t>(NC), 0.05);  // above the weight floor

    // Force a topology mismatch: mark node 0 as an outfall in the ROM's
    // full_to_active map without changing n_nodes (fixed at initialize()).
    // buildWeighted() inside updateBasis() will then produce an active set
    // of size 5 != rom.n_nodes (6), tripping the guard at ~line 369.
    rom.full_to_active[0] = -1;

    rom.updateBasis(w.data(), n1.data(), n2.data(), NC, /*sim_time=*/200.0);

    EXPECT_EQ(rom.basis_updates_attempted_, 1);
    EXPECT_EQ(rom.basis_updates_failed_, 1);

    // Second call inside the interval (250 - 200 = 50 < 100) must be blocked
    // by the time-interval guard before incrementing attempted again.
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC, /*sim_time=*/250.0);

    EXPECT_EQ(rom.basis_updates_attempted_, 1)
        << "failed rebuild must consume the retry interval so retries back off";
}

// ============================================================================
// PR H1 — surcharge-onset cold-restart guard
// ============================================================================

TEST(ColdRestartGuard, SurchargeFlipForcesCold) {
    // Primary trigger: >5% of active nodes flip their surcharge flag between
    // rebuilds -> cold restart, even with IDENTICAL conduit_off (isolates the
    // trigger from any dqdh-based effect -- the surcharge classification
    // alone must be sufficient).
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;

    std::vector<int> n1(static_cast<std::size_t>(NC)), n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }
    std::vector<double> w(static_cast<std::size_t>(NC), 0.01);

    // First call establishes the surcharge baseline (all clear).
    std::vector<uint8_t> surch0(static_cast<std::size_t>(N), 0);
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC, /*sim_time=*/0.0, surch0.data());
    ASSERT_EQ(rom.basis_rebuilds_cold_forced_, 0)
        << "first-ever call has no previous surcharge state to compare against";

    // Second call: 2 of 6 active nodes now surcharged (33% > 5% threshold),
    // conduit_off UNCHANGED.
    std::vector<uint8_t> surch1 = surch0;
    surch1[1] = 1;
    surch1[4] = 1;
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC, /*sim_time=*/100.0, surch1.data());

    EXPECT_EQ(rom.basis_rebuilds_cold_forced_, 1)
        << "surcharge flip on 33% of active nodes must force a cold restart";
    EXPECT_EQ(rom.basis_updates_attempted_, 2)
        << "the surcharge trigger must bypass the skip criterion even though "
           "conduit_off did not change";
}

TEST(ColdRestartGuard, FailedColdAttemptKeepsSuccessfulBaseline) {
    // A failed forced-cold rebuild must not advance the warm/cold baseline.
    // If the last SUCCESSFUL basis is still pre-surcharge, a later retry with
    // the same surcharged state must still go cold.
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;

    std::vector<int> n1(static_cast<std::size_t>(NC)), n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }
    std::vector<double> w(static_cast<std::size_t>(NC), 0.01);

    std::vector<uint8_t> surch0(static_cast<std::size_t>(N), 0);
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC, /*sim_time=*/0.0, surch0.data());
    ASSERT_EQ(rom.basis_rebuilds_cold_forced_, 0);

    // Force the next attempt down the cold path, but also force it to fail by
    // making the active set size differ from rom.n_nodes.
    std::vector<uint8_t> surch1 = surch0;
    surch1[1] = 1;
    surch1[4] = 1;
    rom.full_to_active[0] = -1;
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC, /*sim_time=*/100.0, surch1.data());

    EXPECT_EQ(rom.basis_updates_attempted_, 2);
    EXPECT_EQ(rom.basis_updates_failed_, 1);
    EXPECT_EQ(rom.basis_rebuilds_cold_forced_, 0)
        << "failed attempts must not be counted as completed cold rebuilds";

    // Restore topology and retry with the SAME surcharged state. This must
    // still go cold, because the last successful baseline is still surch0.
    rom.full_to_active[0] = 0;
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC, /*sim_time=*/200.0, surch1.data());

    EXPECT_EQ(rom.basis_updates_attempted_, 3);
    EXPECT_EQ(rom.basis_updates_failed_, 1);
    EXPECT_EQ(rom.basis_rebuilds_cold_forced_, 1)
        << "retry must remain cold until a successful rebuild commits the new baseline";
}

TEST(ColdRestartGuard, LargeEdgeDriftForcesCold) {
    // Secondary trigger: conduit_off jumps by 10x (r_e = 9.0, above the 1.0
    // ratio threshold) on 2 of 5 conduits (40% > 5% edge-fraction threshold)
    // -> cold restart, with no surcharge information at all (nullptr).
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;

    std::vector<int> n1(static_cast<std::size_t>(NC)), n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }
    std::vector<double> w0(static_cast<std::size_t>(NC), 0.01);
    rom.updateBasis(w0.data(), n1.data(), n2.data(), NC, /*sim_time=*/0.0);
    ASSERT_EQ(rom.basis_rebuilds_cold_forced_, 0);

    std::vector<double> w1 = w0;
    w1[0] *= 10.0;
    w1[1] *= 10.0;
    rom.updateBasis(w1.data(), n1.data(), n2.data(), NC, /*sim_time=*/100.0);

    EXPECT_EQ(rom.basis_rebuilds_cold_forced_, 1)
        << "a 10x jump on 40% of edges must force a cold restart via the "
           "secondary (dqdh-drift) trigger alone";
}

TEST(ColdRestartGuard, ModerateSingleEdgeChangeStaysWarm) {
    // The existing FiresWhenOperatorChangesSignificantly scenario (one
    // conduit doubles, 50% change on 1 of 5 edges) is well above the skip
    // tolerance (rebuild fires) but below BOTH cold thresholds (r_e=1.0 is
    // not > 1.0; 20% of edges is above the 5% edge-FRACTION threshold, but
    // that fraction only counts edges that individually exceed the ratio
    // threshold -- here none do) -- confirms ordinary operator drift takes
    // the warm path, matching the pre-H1 baseline behavior exactly.
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;

    std::vector<int> n1(static_cast<std::size_t>(NC)), n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }
    std::vector<double> w0(static_cast<std::size_t>(NC), 0.01);
    rom.updateBasis(w0.data(), n1.data(), n2.data(), NC, /*sim_time=*/0.0);

    std::vector<double> w1 = w0;
    w1[NC / 2] *= 2.0;  // 100% change on exactly 1 of 5 edges
    rom.updateBasis(w1.data(), n1.data(), n2.data(), NC, /*sim_time=*/100.0);

    EXPECT_EQ(rom.basis_updates_attempted_, 2) << "the rebuild must still fire";
    EXPECT_EQ(rom.basis_rebuilds_cold_forced_, 0)
        << "1 of 5 edges changing (20% > the 5% edge-fraction threshold) "
           "must still stay warm, because that edge's own r_e (1.0, a 2x "
           "change) does not exceed the per-edge ratio threshold -- the "
           "fraction only counts edges that individually cross it";
}

TEST(ColdRestartGuard, TenXOnTenPercentOfEdgesForcesCold) {
    // The checklist's literal secondary-trigger case: 10× on 10% of edges.
    // A 5-conduit chain cannot express 10% (one edge is already 20%), so use
    // a 21-node chain = 20 conduits, and jump exactly 2 of them.
    const int N = 21;
    const int NC = N - 1;               // 20 conduits → 2 edges == 10%
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;

    std::vector<int> n1(static_cast<std::size_t>(NC)), n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }
    std::vector<double> w0(static_cast<std::size_t>(NC), 0.01);
    rom.updateBasis(w0.data(), n1.data(), n2.data(), NC, /*sim_time=*/0.0);
    ASSERT_EQ(rom.basis_rebuilds_cold_forced_, 0);

    std::vector<double> w1 = w0;
    w1[3] *= 10.0;
    w1[11] *= 10.0;                     // exactly 2/20 = 10% of edges
    rom.updateBasis(w1.data(), n1.data(), n2.data(), NC, /*sim_time=*/100.0);

    EXPECT_EQ(rom.basis_rebuilds_cold_forced_, 1)
        << "10x on 10% of edges must cross the 5% edge-fraction threshold";
}

TEST(ColdRestartGuard, SlowDriftOnEveryEdgeStaysWarm) {
    // The counterpart the checklist asks for: a slow drift must NOT force cold.
    //
    // This is the case that distinguishes the two halves of the secondary
    // trigger. Here EVERY edge changes (100% of edges — far above the 5%
    // edge-FRACTION threshold) but each one only by 50%, so its per-edge
    // r_e = 0.5 never crosses the 1.0 ratio threshold. The fraction counts
    // only edges that individually exceed the ratio, so the count is 0 and
    // the rebuild must stay warm. A regression that compared "fraction of
    // edges CHANGED" instead of "fraction of edges that JUMPED" would force
    // cold here and silently discard the warm start on ordinary drift.
    //
    // The 50% drift is also deliberately above basis_update_tol (5%), so a
    // rebuild genuinely fires — this asserts "warm rebuild", not "skipped".
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;

    std::vector<int> n1(static_cast<std::size_t>(NC)), n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }
    std::vector<double> w0(static_cast<std::size_t>(NC), 0.01);
    rom.updateBasis(w0.data(), n1.data(), n2.data(), NC, /*sim_time=*/0.0);
    const int attempted_after_first = rom.basis_updates_attempted_;

    std::vector<double> w1 = w0;
    for (double& w : w1) w *= 1.5;      // every edge drifts 50%: r_e = 0.5
    rom.updateBasis(w1.data(), n1.data(), n2.data(), NC, /*sim_time=*/100.0);

    EXPECT_EQ(rom.basis_updates_attempted_, attempted_after_first + 1)
        << "a 50% drift is above basis_update_tol — a rebuild must actually fire";
    EXPECT_EQ(rom.basis_rebuilds_cold_forced_, 0)
        << "every edge changed, but none JUMPED past the per-edge ratio "
           "threshold — ordinary drift must keep the warm start";
}

TEST(ColdRestartGuard, EigenpairResidualSmallAfterWarmAndColdRebuild) {
    // Basis correctness invariant: regardless of warm or cold start, the
    // rebuilt eigenpairs must satisfy the Ritz residual bound.
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;

    std::vector<int> n1(static_cast<std::size_t>(NC)), n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }

    auto check_residual = [&](const std::vector<double>& weights) {
        // Mirror updateBasis()'s own normalization (mean weight 1.0) so the
        // reference Laplacian matches what was actually built internally.
        std::vector<double> w_norm = weights;
        double sum_w = 0.0;
        for (double w : w_norm) sum_w += std::max(w, 1.0e-6);
        const double scale = static_cast<double>(NC) / sum_w;
        for (double& w : w_norm) w = std::max(w, 1.0e-6) * scale;

        std::vector<int> is_outfall(static_cast<std::size_t>(N), 0);
        std::vector<int> active_map, full_to_active;
        CsrGraph L = NetworkLaplacian1D::buildWeighted(
            N, NC, n1.data(), n2.data(), is_outfall.data(), w_norm.data(),
            active_map, full_to_active);

        const int n = rom.basis->n_nodes;
        std::vector<double> y(static_cast<std::size_t>(n), 0.0);
        for (int j = 0; j < rom.basis->num_kept; ++j) {
            const double* vj = &rom.basis->P[static_cast<std::size_t>(j * n)];
            std::fill(y.begin(), y.end(), 0.0);
            csr_matvec(L, vj, y.data());
            const double lam = rom.basis->eigenvalues[static_cast<std::size_t>(j)];
            double resid = 0.0;
            for (int i = 0; i < n; ++i) {
                const double d = y[static_cast<std::size_t>(i)]
                               - lam * vj[static_cast<std::size_t>(i)];
                resid += d * d;
            }
            EXPECT_LE(std::sqrt(resid), 1.0e-8) << "mode " << j;
        }
    };

    // First rebuild: cold by construction (no previous state).
    std::vector<double> w0(static_cast<std::size_t>(NC), 0.01);
    rom.updateBasis(w0.data(), n1.data(), n2.data(), NC, /*sim_time=*/0.0);
    check_residual(w0);

    // Second rebuild: warm (small single-edge change).
    std::vector<double> w1 = w0;
    w1[NC / 2] *= 2.0;
    rom.updateBasis(w1.data(), n1.data(), n2.data(), NC, /*sim_time=*/100.0);
    ASSERT_EQ(rom.basis_rebuilds_cold_forced_, 0) << "sanity: this rebuild is warm";
    check_residual(w1);

    // Third rebuild: forced cold (large drift on many edges).
    std::vector<double> w2 = w1;
    w2[0] *= 10.0;
    w2[1] *= 10.0;
    rom.updateBasis(w2.data(), n1.data(), n2.data(), NC, /*sim_time=*/200.0);
    ASSERT_EQ(rom.basis_rebuilds_cold_forced_, 1) << "sanity: this rebuild is cold";
    check_residual(w2);
}

// ============================================================================
// PR 5 — weighted-Laplacian normalization (updateBasis)
// ============================================================================

TEST(WeightNormalization, EigenvalueScaleInvariance) {
    // updateBasis() normalizes conduit weights to mean 1.0 before building the
    // Laplacian, so scaling every input weight by a constant factor (dt- or
    // units-dependent absolute conductance vs a normalized relative structure)
    // must produce an IDENTICAL eigenbasis, not merely a proportionally scaled
    // one — the normalization removes the scale dependence entirely.
    const int N = 6;
    const int NC = N - 1;
    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }

    SpectralROM1D rom1, rom2;
    auto basis1_owned = make_rom1d_chain(N, rom1);
    auto basis2_owned = make_rom1d_chain(N, rom2);
    rom1.basis_update_interval = 0.0;
    rom2.basis_update_interval = 0.0;

    std::vector<double> w1(static_cast<std::size_t>(NC));
    std::vector<double> w2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        double base = 0.01 * (1.0 + 0.3 * ci);  // non-uniform pattern
        w1[static_cast<std::size_t>(ci)] = base;
        w2[static_cast<std::size_t>(ci)] = base * 1000.0;
    }

    rom1.updateBasis(w1.data(), n1.data(), n2.data(), NC);
    rom2.updateBasis(w2.data(), n1.data(), n2.data(), NC);

    ASSERT_EQ(rom1.basis->num_kept, rom2.basis->num_kept);
    for (int j = 0; j < rom1.basis->num_kept; ++j) {
        const double e1 = rom1.basis->eigenvalues[static_cast<std::size_t>(j)];
        const double e2 = rom2.basis->eigenvalues[static_cast<std::size_t>(j)];
        EXPECT_NEAR(e2, e1, std::fabs(e1) * 1.0e-9 + 1.0e-12) << "mode " << j;
    }
}

TEST(WeightNormalization, UniformWeightsUnchanged) {
    // All-weight-1 input is already at mean 1.0, so normalization is the
    // identity transform: updateBasis() with uniform weights must give
    // eigenvalues matching GraphEigenBasis::build() on the pure topological
    // (buildUniform) Laplacian.
    const int N = 6;
    const int NC = N - 1;
    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }

    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);
    rom.basis_update_interval = 0.0;

    std::vector<double> w(static_cast<std::size_t>(NC), 1.0);
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC);

    std::vector<int> is_outfall(static_cast<std::size_t>(N), 0);
    std::vector<int> active_map, full_to_active;
    CsrGraph L_ref = NetworkLaplacian1D::buildUniform(
        N, NC, n1.data(), n2.data(), is_outfall.data(), active_map, full_to_active);
    GraphEigenBasis ref;
    ASSERT_TRUE(ref.build(L_ref, 4));

    ASSERT_EQ(rom.basis->num_kept, ref.num_kept);
    for (int j = 0; j < ref.num_kept; ++j) {
        EXPECT_NEAR(rom.basis->eigenvalues[static_cast<std::size_t>(j)],
                    ref.eigenvalues[static_cast<std::size_t>(j)], 1.0e-9)
            << "mode " << j;
    }
}

// ============================================================================
// PR 6 (reform) — DeviationForm invariants
// ============================================================================

// Helper: max (q95 − q05) across all nodes after computeQuantiles().
static double max_spread(const SpectralROM1D& rom) {
    double s = 0.0;
    for (int i = 0; i < rom.n_nodes; ++i)
        s = std::max(s, rom.q95[static_cast<std::size_t>(i)]
                      - rom.q05[static_cast<std::size_t>(i)]);
    return s;
}

TEST(DeviationForm, ZeroPerturbationIsExact) {
    // With mm = rm = 1 for all members, deviations are identically zero, so
    // q05 == q50 == q95 == h_det to machine precision at every node and step
    // — the strongest new invariant, impossible under the total-head form.
    ROM1DFixture f;
    f.rom.mannings_pert = 0.0;
    f.rom.runoff_pert   = 0.0;
    f.rom.initialize();

    auto h0 = f.bump_head();
    f.rom.seed(h0.data());

    std::vector<double> h_det(static_cast<std::size_t>(f.N));
    std::vector<double> runoff(static_cast<std::size_t>(f.N));
    for (int step = 0; step < 100; ++step) {
        // Arbitrary time-varying deterministic reference and forcing
        const double phase = 0.05 * step;
        for (int i = 0; i < f.N; ++i) {
            h_det[static_cast<std::size_t>(i)]  =
                h0[static_cast<std::size_t>(i)] * (1.0 + 0.5 * std::sin(phase + 0.3 * i));
            runoff[static_cast<std::size_t>(i)] =
                1.0e-5 * (1.0 + std::cos(phase + 0.7 * i));
        }
        f.rom.advance(15.0, 0.1, h_det.data(), runoff.data());
    }
    f.rom.computeQuantiles(h_det.data(), nullptr);

    for (int i = 0; i < f.rom.n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_NEAR(f.rom.q05[ui], h_det[ui], 1.0e-12) << "node " << i;
        EXPECT_NEAR(f.rom.q50[ui], h_det[ui], 1.0e-12) << "node " << i;
        EXPECT_NEAR(f.rom.q95[ui], h_det[ui], 1.0e-12) << "node " << i;
    }
}

TEST(DeviationForm, MedianTracksDeterministic) {
    // pert = 0.2, M = 50: after 50 steps the median stays bracketed well
    // inside the spread around the deterministic reference (no drift).
    const int N = 20, M = 50, K = 6;
    GraphEigenBasis basis;
    CsrGraph L = make_chain_laplacian(N);
    ASSERT_TRUE(basis.build(L, K));

    SpectralROM1D rom;
    rom.basis         = &basis;
    rom.n_ensemble    = M;
    rom.mannings_pert = 0.20;
    rom.runoff_pert   = 0.0;
    rom.initialize();

    std::vector<double> h_det(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) {
        double dx = i - N / 3.0;
        h_det[static_cast<std::size_t>(i)] =
            0.10 * std::exp(-0.5 * dx * dx / (N / 8.0 * N / 8.0));
    }
    rom.seed(h_det.data());
    for (int step = 0; step < 50; ++step)
        rom.advance(30.0, 0.1, h_det.data(), nullptr);
    rom.computeQuantiles(h_det.data(), nullptr);

    for (int i = 0; i < rom.n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        const double spread = rom.q95[ui] - rom.q05[ui];
        EXPECT_LE(std::abs(rom.q50[ui] - h_det[ui]), 0.25 * spread + 1.0e-15)
            << "median drifted outside 25% of spread at node " << i;
    }
}

TEST(DeviationForm, SpreadNeverCollapses) {
    // Regression against the old reseed-collapse behaviour: under sustained
    // non-uniform reference growth, total spread is non-decreasing after the
    // initial transient (5% dips tolerated for sort/quantile discreteness).
    ROM1DFixture f;   // mannings_pert = 0.20 (fixture default)
    auto h0 = f.bump_head();
    f.rom.seed(h0.data());

    std::vector<double> h_det(static_cast<std::size_t>(f.N));
    double prev_total = -1.0;
    for (int step = 0; step < 300; ++step) {
        const double grow = 1.0 + static_cast<double>(step) / 300.0;  // ramp up
        for (int i = 0; i < f.N; ++i)
            h_det[static_cast<std::size_t>(i)] = h0[static_cast<std::size_t>(i)] * grow;
        f.rom.advance(1.0, 0.01, h_det.data(), nullptr);

        if (step > 50) {
            f.rom.computeQuantiles(h_det.data(), nullptr);
            double total = 0.0;
            for (int i = 0; i < f.rom.n_nodes; ++i)
                total += f.rom.q95[static_cast<std::size_t>(i)]
                       - f.rom.q05[static_cast<std::size_t>(i)];
            if (prev_total >= 0.0) {
                EXPECT_GE(total, prev_total * 0.95)
                    << "spread collapsed at step " << step;
            }
            prev_total = total;
        }
    }
    EXPECT_GT(prev_total, 0.0) << "spread must be nonzero under sustained forcing";
}

TEST(DeviationForm, SpreadScalesWithManningPert) {
    // spread(pert=0.2) > spread(pert=0.1) > 0 under identical forcing.
    auto run_with_pert = [](double pert) {
        ROM1DFixture f;
        f.rom.mannings_pert = pert;
        f.rom.runoff_pert   = 0.0;
        f.rom.initialize();
        auto h = f.bump_head();
        f.rom.seed(h.data());
        for (int step = 0; step < 40; ++step)
            f.rom.advance(30.0, 0.1, h.data(), nullptr);
        f.rom.computeQuantiles(h.data(), nullptr);
        return max_spread(f.rom);
    };

    const double s10 = run_with_pert(0.10);
    const double s20 = run_with_pert(0.20);
    EXPECT_GT(s10, 0.0);
    EXPECT_GT(s20, s10) << "doubling the perturbation must widen the band";
}

TEST(DeviationForm, InvertClampRespected) {
    // Reference sits just above a 10 m invert; large perturbation pulls some
    // members below it. With the invert supplied, no quantile may dip under
    // 10 m — and without it, at least one must (proving the clamp engaged).
    ROM1DFixture f;
    f.rom.mannings_pert = 0.50;
    f.rom.runoff_pert   = 0.0;
    f.rom.initialize();

    // Strong spatial variation just above the invert (constant components
    // project to zero on the Neumann chain modes, so variation carries b_j).
    std::vector<double> h_det(static_cast<std::size_t>(f.N));
    for (int i = 0; i < f.N; ++i)
        h_det[static_cast<std::size_t>(i)] =
            10.0 + 0.05 * std::sin(0.6 * i) + 0.002 * i;

    f.rom.seed(h_det.data());
    for (int step = 0; step < 60; ++step)
        f.rom.advance(30.0, 0.5, h_det.data(), nullptr);

    std::vector<double> invert(static_cast<std::size_t>(f.N), 10.0);
    f.rom.computeQuantiles(h_det.data(), invert.data());
    double min_q05_clamped = 1.0e300;
    for (int i = 0; i < f.rom.n_nodes; ++i)
        min_q05_clamped = std::min(min_q05_clamped,
                                   f.rom.q05[static_cast<std::size_t>(i)]);
    EXPECT_GE(min_q05_clamped, 10.0) << "clamped quantiles must respect the invert";

    f.rom.computeQuantiles(h_det.data(), nullptr);
    double min_q05_free = 1.0e300;
    for (int i = 0; i < f.rom.n_nodes; ++i)
        min_q05_free = std::min(min_q05_free,
                                f.rom.q05[static_cast<std::size_t>(i)]);
    EXPECT_LT(min_q05_free, 10.0)
        << "test setup must actually push members below the invert";
}

TEST(DeviationForm, UpdateBasisPreservesDeviation) {
    // A basis rebuild re-projects deviations (R = P_new^T P_old); the spread
    // must be continuous across the rebuild (|Δ| ≤ 10%).
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom, 0.20);
    rom.basis_update_interval = 0.0;

    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }

    std::vector<double> h_det(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
        h_det[static_cast<std::size_t>(i)] = (i + 1) * 0.1;

    for (int step = 0; step < 10; ++step)
        rom.advance(10.0, 0.1, h_det.data(), nullptr);

    rom.computeQuantiles(h_det.data(), nullptr);
    const double spread_before = max_spread(rom);
    ASSERT_GT(spread_before, 0.0);

    // Prime the skip criterion, then force a rebuild with a 20% weight bump.
    std::vector<double> w0(static_cast<std::size_t>(NC), 1.0);
    rom.updateBasis(w0.data(), n1.data(), n2.data(), NC);
    std::vector<double> w1 = w0;
    w1[2] = 1.2;
    rom.updateBasis(w1.data(), n1.data(), n2.data(), NC);

    rom.computeQuantiles(h_det.data(), nullptr);
    const double spread_after = max_spread(rom);
    EXPECT_NEAR(spread_after, spread_before, spread_before * 0.10)
        << "spread must be continuous across a basis rebuild";
}

TEST(DeviationForm, ForcedColdRebuildPreservesDeviation) {
    // PR H1's forced-cold path calls GraphEigenBasis::build() with
    // v0_block = nullptr, which ALSO skips the sign-alignment pass (that pass
    // is gated on v0_block being non-null). The ensemble must survive anyway:
    // the R = P_newᵀ·P_old re-projection carries whatever sign each rebuilt
    // eigenvector came back with, so δa is mapped correctly even when modes
    // flip. If it did not, a forced-cold restart would silently scramble the
    // ensemble — a far worse bug than the stale-warm-start one H1 fixes.
    //
    // Isolated deliberately: the cold restart is triggered by a SURCHARGE FLIP
    // with conduit_off held IDENTICAL, so the operator itself does not change.
    // The rebuilt basis therefore spans the same space, and any spread change
    // is purely a re-projection/sign artifact rather than real physics.
    const int N = 6;
    const int NC = N - 1;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom, 0.20);
    rom.basis_update_interval = 0.0;

    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }

    std::vector<double> h_det(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i)
        h_det[static_cast<std::size_t>(i)] = (i + 1) * 0.1;
    for (int step = 0; step < 10; ++step)
        rom.advance(10.0, 0.1, h_det.data(), nullptr);

    rom.computeQuantiles(h_det.data(), nullptr);
    const double spread_before = max_spread(rom);
    ASSERT_GT(spread_before, 0.0);

    // Establish the surcharge baseline (all clear) with a successful rebuild.
    std::vector<double> w(static_cast<std::size_t>(NC), 1.0);
    std::vector<uint8_t> surch_clear(static_cast<std::size_t>(N), 0);
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC, /*sim_time=*/0.0,
                    surch_clear.data());

    // Same weights; only the surcharge flags flip → forced COLD rebuild on an
    // unchanged operator.
    std::vector<uint8_t> surch_on = surch_clear;
    surch_on[1] = 1;
    surch_on[4] = 1;
    rom.updateBasis(w.data(), n1.data(), n2.data(), NC, /*sim_time=*/100.0,
                    surch_on.data());
    ASSERT_EQ(rom.basis_rebuilds_cold_forced_, 1)
        << "sanity: this rebuild must actually have taken the cold path";

    rom.computeQuantiles(h_det.data(), nullptr);
    const double spread_after = max_spread(rom);
    EXPECT_NEAR(spread_after, spread_before, spread_before * 0.10)
        << "a forced-COLD rebuild must preserve the ensemble deviations — "
           "sign alignment is skipped on this path, so the R re-projection is "
           "solely responsible for keeping δa consistent";
}

// ============================================================================
// PR 9b — RegisteredParams (generic parameter consumption in the 1D ROM)
// ============================================================================

TEST(RegisteredParams, ForcingVectorProducesSpreadAndClearRestores) {
    // Zero built-in perturbation → the ONLY uncertainty source is the
    // registered FORCING_VECTOR param. Spread must appear with it and the
    // ROM must return to exact zero-deviation behavior after clearing.
    ROM1DFixture f;
    f.rom.mannings_pert = 0.0;
    f.rom.runoff_pert   = 0.0;
    f.rom.initialize();

    auto h_det = f.bump_head();
    f.rom.seed(h_det.data());

    // Non-uniform per-node field (uniform projects to ~0 on Neumann modes).
    std::vector<double> v(static_cast<std::size_t>(f.N));
    for (int i = 0; i < f.N; ++i)
        v[static_cast<std::size_t>(i)] = 1.0e-4 * (1.0 + std::sin(i * 0.7));

    // Lognormal-style multiplier column around 1 (any spread-carrying column works).
    std::vector<double> theta(static_cast<std::size_t>(f.rom.n_ensemble));
    for (int i = 0; i < f.rom.n_ensemble; ++i)
        theta[static_cast<std::size_t>(i)] =
            1.0 + 0.3 * (2.0 * (static_cast<double>(i) + 0.5)
                             / static_cast<double>(f.rom.n_ensemble) - 1.0);
    f.rom.addRegisteredParam(ParamEntry::FORCING_VECTOR, theta, v.data());

    for (int step = 0; step < 30; ++step)
        f.rom.advance(30.0, 0.1, h_det.data(), nullptr);
    f.rom.computeQuantiles(h_det.data(), nullptr);
    EXPECT_GT(max_spread(f.rom), 1.0e-9)
        << "a registered FORCING_VECTOR param must produce spread";

    // Clear + reseed: with no perturbation sources left, quantiles equal the
    // deterministic reference EXACTLY after any number of steps.
    f.rom.clearRegisteredParams();
    f.rom.seed(h_det.data());
    for (int step = 0; step < 30; ++step)
        f.rom.advance(30.0, 0.1, h_det.data(), nullptr);
    f.rom.computeQuantiles(h_det.data(), nullptr);
    for (int i = 0; i < f.rom.n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_DOUBLE_EQ(f.rom.q05[ui], h_det[ui]) << "node " << i;
        EXPECT_DOUBLE_EQ(f.rom.q95[ui], h_det[ui]) << "node " << i;
    }
}

TEST(RegisteredParams, RateMultEquivalentToBuiltInManning) {
    // A registered RATE_MULT column must reproduce, bit-exactly, the dynamics
    // of the same multipliers supplied through the built-in Manning path.
    const int N = 20, M = 20, K = 4;
    GraphEigenBasis basis;
    CsrGraph L = make_chain_laplacian(N);
    ASSERT_TRUE(basis.build(L, K));

    std::vector<double> h_det(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) {
        double dx = i - N / 3.0;
        h_det[static_cast<std::size_t>(i)] =
            0.10 * std::exp(-0.5 * dx * dx / (N / 8.0 * N / 8.0));
    }

    // ROM A: built-in Manning perturbation.
    SpectralROM1D romA;
    romA.basis = &basis; romA.n_ensemble = M;
    romA.mannings_pert = 0.20; romA.runoff_pert = 0.0;
    romA.initialize();
    romA.seed(h_det.data());

    // ROM B: zero built-in perturbation + RATE_MULT column equal to A's design.
    SpectralROM1D romB;
    romB.basis = &basis; romB.n_ensemble = M;
    romB.mannings_pert = 0.0; romB.runoff_pert = 0.0;
    romB.initialize();
    romB.addRegisteredParam(ParamEntry::RATE_MULT, romA.mannings_mult);
    romB.seed(h_det.data());

    for (int step = 0; step < 20; ++step) {
        romA.advance(30.0, 0.1, h_det.data(), nullptr);
        romB.advance(30.0, 0.1, h_det.data(), nullptr);
    }
    ASSERT_EQ(romA.a_ensemble.size(), romB.a_ensemble.size());
    for (std::size_t k = 0; k < romA.a_ensemble.size(); ++k)
        EXPECT_DOUBLE_EQ(romA.a_ensemble[k], romB.a_ensemble[k]) << "k=" << k;
}

TEST(RegisteredParams, InvalidRegistrationThrows) {
    ROM1DFixture f;
    std::vector<double> wrong(static_cast<std::size_t>(f.rom.n_ensemble + 3), 1.0);
    std::vector<double> ok(static_cast<std::size_t>(f.rom.n_ensemble), 1.0);
    EXPECT_THROW(f.rom.addRegisteredParam(ParamEntry::RATE_MULT, wrong),
                 std::invalid_argument);
    EXPECT_THROW(f.rom.addRegisteredParam(ParamEntry::FORCING_VECTOR, ok, nullptr),
                 std::invalid_argument);
}
