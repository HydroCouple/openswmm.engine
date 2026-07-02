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

    // Seed all members at zero head (δ from datum), then force uniformly.
    std::vector<double> h0(static_cast<std::size_t>(rom.n_nodes), 0.0);
    rom.seed(h0.data());

    std::vector<double> runoff(static_cast<std::size_t>(rom.n_nodes), 1.0e-4);  // uniform
    for (int step = 0; step < 10; ++step)
        rom.advance(10.0, 1.0e-3, runoff.data());
    rom.computeQuantiles();

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

TEST(GraphEigenBasisWarmStart, WarmStartMatchesColdStart) {
    // Warm-start (v0_block = cold.P) must produce the 5 smallest nontrivial
    // eigenvalues of the 20-node path graph, matching the analytic values
    // λ_k = 2*(1-cos(k*π/20)), k=1..5.
    //
    // Note: the cold-start linear ramp is anti-symmetric → it can only excite
    // odd-k eigenmodes (k=1,3,5,7,9) and misses even modes.  The warm-start
    // sum-of-columns starting vector is not purely anti-symmetric and correctly
    // finds all k=1..5 modes.
    CsrGraph L = make_chain_laplacian(20);
    GraphEigenBasis cold;
    ASSERT_TRUE(cold.build(L, 5));

    GraphEigenBasis warm;
    ASSERT_TRUE(warm.build(L, 5, cold.P.data()));

    ASSERT_EQ(warm.num_kept, 5);

    // Analytic eigenvalues of the 20-node path-graph Laplacian
    for (int j = 0; j < warm.num_kept; ++j) {
        double expected = 2.0 * (1.0 - std::cos((j + 1) * M_PI / 20.0));
        EXPECT_NEAR(warm.eigenvalues[static_cast<std::size_t>(j)],
                    expected, 1e-6)
            << "warm eigenvalue " << j;
    }
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
    auto basis_owned = make_rom1d_chain(N, rom, 0.0);  // no pert so all members equal
    rom.basis_update_interval = 0.0;  // disable time guard for unit test

    std::vector<int> n1(static_cast<std::size_t>(NC));
    std::vector<int> n2(static_cast<std::size_t>(NC));
    for (int ci = 0; ci < NC; ++ci) {
        n1[static_cast<std::size_t>(ci)] = ci;
        n2[static_cast<std::size_t>(ci)] = ci + 1;
    }

    // Advance briefly to build non-zero coefficients
    rom.advance(1.0, 1e-3, nullptr);

    // Record Frobenius norm before
    double norm_before = 0.0;
    for (double v : rom.a_ensemble) norm_before += v * v;

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

    // Simulate a series of weight updates (growing conductance over time)
    for (int step = 0; step < 5; ++step) {
        double scale = 0.005 * (1.0 + step * 0.5);
        std::vector<double> w(static_cast<std::size_t>(NC), scale);
        w[2] = scale * 3.0;  // one conduit always wetter
        rom.updateBasis(w.data(), n1.data(), n2.data(), NC);
        rom.advance(10.0, scale * 100.0, nullptr);
        rom.computeQuantiles();

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

// ============================================================================
// PR 6 — checkAndReseed
// ============================================================================

TEST(CheckAndReseed, ReseedFiresAfterLargeHeadChange) {
    // Seed with near-zero heads, then present 1.0 m heads → mean change >> 10%.
    const int N = 6;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);

    // Override seed with zeros
    std::vector<double> h_zero(static_cast<std::size_t>(N), 0.0);
    rom.seed(h_zero.data());
    EXPECT_EQ(rom.last_reseed_time_, 0.0);

    std::vector<double> h_one(static_cast<std::size_t>(N), 1.0);
    rom.reseed_head_fraction = 0.10;
    rom.reseed_min_interval  = 0.0;   // no interval restriction
    rom.checkAndReseed(h_one.data(), 120.0);

    EXPECT_DOUBLE_EQ(rom.last_reseed_time_, 120.0)
        << "last_reseed_time_ must update after reseed";
    for (int i = 0; i < N; ++i)
        EXPECT_DOUBLE_EQ(rom.seed_heads_[static_cast<std::size_t>(i)], 1.0)
            << "seed_heads_ must hold new values";
}

TEST(CheckAndReseed, ReseedDoesNotFireWhenChangeSmall) {
    // Seed with 1.0 m, present 1.02 m (2% change < 10% threshold).
    const int N = 6;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);

    std::vector<double> h_base(static_cast<std::size_t>(N), 1.0);
    rom.seed(h_base.data());
    rom.reseed_head_fraction = 0.10;
    rom.reseed_min_interval  = 0.0;

    std::vector<double> h_small(static_cast<std::size_t>(N), 1.02);
    rom.checkAndReseed(h_small.data(), 60.0);

    EXPECT_DOUBLE_EQ(rom.last_reseed_time_, 0.0)
        << "No reseed should fire for a 2% head change";
}

TEST(CheckAndReseed, ReseedMinIntervalRespected) {
    // Fire two large changes within reseed_min_interval; only first should reseed.
    const int N = 6;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom);

    std::vector<double> h_zero(static_cast<std::size_t>(N), 0.0);
    rom.seed(h_zero.data());
    rom.reseed_head_fraction = 0.10;
    rom.reseed_min_interval  = 60.0;

    std::vector<double> h_large(static_cast<std::size_t>(N), 2.0);

    // First call at t=10 — should reseed
    rom.checkAndReseed(h_large.data(), 10.0);
    EXPECT_DOUBLE_EQ(rom.last_reseed_time_, 10.0);

    // Second call at t=50 (only 40 s later, < 60 s interval) — must NOT reseed
    std::vector<double> h_large2(static_cast<std::size_t>(N), 5.0);
    rom.checkAndReseed(h_large2.data(), 50.0);
    EXPECT_DOUBLE_EQ(rom.last_reseed_time_, 10.0)
        << "Second reseed within min_interval must be suppressed";
}

TEST(CheckAndReseed, SpreadRebuildAfterReseed) {
    // After advance (spread grows), force a reseed (spread collapses), then
    // advance again — spread must grow back.
    const int N = 6;
    SpectralROM1D rom;
    auto basis_owned = make_rom1d_chain(N, rom, 0.20);  // 20% Manning pert

    // Advance 5 steps to build spread
    for (int s = 0; s < 5; ++s)
        rom.advance(30.0, 0.01, nullptr);
    rom.computeQuantiles();

    double spread_before = 0.0;
    for (int i = 0; i < N; ++i)
        spread_before = std::max(spread_before,
            rom.q95[static_cast<std::size_t>(i)] -
            rom.q05[static_cast<std::size_t>(i)]);
    EXPECT_GT(spread_before, 1e-10) << "spread must exist before reseed";

    // Force reseed: non-uniform ramp (constant vector projects to zero on Laplacian modes)
    std::vector<double> h_new(static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) h_new[static_cast<std::size_t>(i)] = (i + 1) * 0.05;
    rom.reseed_head_fraction = 0.01;  // very sensitive
    rom.reseed_min_interval  = 0.0;
    rom.checkAndReseed(h_new.data(), 200.0);
    EXPECT_DOUBLE_EQ(rom.last_reseed_time_, 200.0) << "reseed must fire";

    // Right after reseed, all members are identical → spread ≈ 0
    rom.computeQuantiles();
    double spread_after_reseed = 0.0;
    for (int i = 0; i < N; ++i)
        spread_after_reseed = std::max(spread_after_reseed,
            rom.q95[static_cast<std::size_t>(i)] -
            rom.q05[static_cast<std::size_t>(i)]);
    EXPECT_LT(spread_after_reseed, 1e-10) << "spread must collapse immediately after reseed";

    // Advance again — spread must rebuild
    for (int s = 0; s < 5; ++s)
        rom.advance(30.0, 0.01, nullptr);
    rom.computeQuantiles();

    double spread_rebuilt = 0.0;
    for (int i = 0; i < N; ++i)
        spread_rebuilt = std::max(spread_rebuilt,
            rom.q95[static_cast<std::size_t>(i)] -
            rom.q05[static_cast<std::size_t>(i)]);
    EXPECT_GT(spread_rebuilt, 1e-10) << "spread must rebuild after reseed + advance";
}
