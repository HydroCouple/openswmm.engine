/**
 * @file test_spectral_correction.cpp
 * @brief Micro-tests for SpectralCoarse: Laplacian, eigenbasis, boundary masking,
 *        correction direction, and coarse operator assembly.
 *
 * These tests compile SpectralCoarse.cpp directly (not via the engine shared
 * library) so that all private internals are accessible without special exports.
 * No SWMM engine types are used here.
 *
 * @ingroup unit_tests
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include <algorithm>
#include <numeric>

#include "hydraulics/SpectralCoarse.hpp"

using namespace openswmm::dynwave;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static SpectralCoarse make_chain(int n_nodes, int n_outfall_start = -1,
                                  int n_outfall_end = -1,
                                  int num_modes = 5) {
    // Linear chain: nodes 0-1-2-..-(n_nodes-1)
    // conduits: k connects node k and node k+1
    int n_c = n_nodes - 1;
    std::vector<int> n1(static_cast<std::size_t>(n_c));
    std::vector<int> n2(static_cast<std::size_t>(n_c));
    for (int k = 0; k < n_c; ++k) {
        n1[static_cast<std::size_t>(k)] = k;
        n2[static_cast<std::size_t>(k)] = k + 1;
    }
    std::vector<int> outfall(static_cast<std::size_t>(n_nodes), 0);
    if (n_outfall_start >= 0 && n_outfall_start < n_nodes)
        outfall[static_cast<std::size_t>(n_outfall_start)] = 1;
    if (n_outfall_end >= 0 && n_outfall_end < n_nodes)
        outfall[static_cast<std::size_t>(n_outfall_end)] = 1;

    SpectralCoarse sc;
    sc.num_modes = num_modes;
    sc.init(n_nodes, n_c, outfall.data(), n1.data(), n2.data(), num_modes);
    return sc;
}

static SpectralCoarse make_ring(int n_nodes, int num_modes = 5) {
    // Ring: nodes 0-1-2-..(n-1)-0
    int n_c = n_nodes;
    std::vector<int> n1(static_cast<std::size_t>(n_c));
    std::vector<int> n2(static_cast<std::size_t>(n_c));
    for (int k = 0; k < n_nodes - 1; ++k) {
        n1[static_cast<std::size_t>(k)] = k;
        n2[static_cast<std::size_t>(k)] = k + 1;
    }
    n1[static_cast<std::size_t>(n_nodes - 1)] = n_nodes - 1;
    n2[static_cast<std::size_t>(n_nodes - 1)] = 0;

    std::vector<int> outfall(static_cast<std::size_t>(n_nodes), 0);

    SpectralCoarse sc;
    sc.num_modes = num_modes;
    sc.init(n_nodes, n_c, outfall.data(), n1.data(), n2.data(), num_modes);
    return sc;
}

// ============================================================================
// LaplacianConstruction
// ============================================================================

TEST(LaplacianConstruction, SingleLink) {
    // 2 nodes, neither outfall, 1 conduit
    int n1[1] = {0}, n2[1] = {1};
    int outfall[2] = {0, 0};
    SpectralCoarse sc;
    sc.num_modes = 1;
    bool ok = sc.init(2, 1, outfall, n1, n2, 1);

    EXPECT_TRUE(ok) << "init() failed: last_error=" << sc.last_error;
    EXPECT_EQ(sc.n_active, 2);
    EXPECT_EQ(sc.N, 2);
    EXPECT_GE(sc.num_kept, 1);
    EXPECT_EQ(sc.enabled, 1);
}

TEST(LaplacianConstruction, PathWithOutfall) {
    // 3 nodes (node 2 is outfall), 2 conduits: 0-1, 1-2
    int n1[2] = {0, 1}, n2[2] = {1, 2};
    int outfall[3] = {0, 0, 1};
    SpectralCoarse sc;
    sc.num_modes = 1;
    bool ok = sc.init(3, 2, outfall, n1, n2, 1);

    EXPECT_TRUE(ok);
    EXPECT_EQ(sc.n_active, 2);  // nodes 0 and 1 only
    EXPECT_EQ(sc.full_to_active[2], -1);
    EXPECT_GE(sc.full_to_active[0], 0);
    EXPECT_GE(sc.full_to_active[1], 0);
}

TEST(LaplacianConstruction, RingHasActiveNodes) {
    // 10-node ring, no outfalls
    auto sc = make_ring(10, 5);
    EXPECT_EQ(sc.n_active, 10);
    EXPECT_GE(sc.num_kept, 1);
    EXPECT_EQ(sc.enabled, 1);
}

TEST(LaplacianConstruction, DisconnectedTwoComponents) {
    // 4 nodes, 2 separate conduits: 0-1 and 2-3 (disconnected)
    int n1[2] = {0, 2}, n2[2] = {1, 3};
    int outfall[4] = {0, 0, 0, 0};
    SpectralCoarse sc;
    sc.num_modes = 4;
    sc.init(4, 2, outfall, n1, n2, 4);
    // Single-vector Lanczos can only find one null direction even for two
    // disconnected components: the linear ramp starting vector combines
    // both null eigenvectors into one zero-eigenvalue mode, causing lucky
    // breakdown after 2 steps with eigenvalues {0, 2}. So num_null == 1.
    if (sc.enabled) {
        EXPECT_GE(sc.num_null, 1) << "Disconnected graph should have >= 1 null modes";
    } else {
        EXPECT_GT(sc.last_error, 0) << "Disabled but no error code set";
    }
}

// ============================================================================
// EigenbasisValidation
// ============================================================================

TEST(EigenbasisValidation, NullModeDiscarded) {
    auto sc = make_ring(10, 5);
    ASSERT_EQ(sc.enabled, 1);
    for (int q = 0; q < sc.num_kept; ++q) {
        EXPECT_GT(sc.eigenvalues[static_cast<std::size_t>(q)], sc.null_tol)
            << "Eigenvalue " << q << " is below null_tol — null mode not discarded";
    }
}

TEST(EigenbasisValidation, EigenvaluesNonnegative) {
    auto sc = make_ring(12, 5);
    ASSERT_EQ(sc.enabled, 1);
    for (int q = 0; q < sc.num_kept; ++q) {
        EXPECT_GE(sc.eigenvalues[static_cast<std::size_t>(q)], 0.0)
            << "Eigenvalue " << q << " is negative";
    }
}

TEST(EigenbasisValidation, EigenvaluesSortedAscending) {
    auto sc = make_chain(20, -1, -1, 8);
    ASSERT_EQ(sc.enabled, 1);
    for (int q = 1; q < sc.num_kept; ++q) {
        EXPECT_LE(sc.eigenvalues[static_cast<std::size_t>(q - 1)],
                  sc.eigenvalues[static_cast<std::size_t>(q)] + 1e-10)
            << "Eigenvalues not sorted ascending at q=" << q;
    }
}

TEST(EigenbasisValidation, Orthonormality) {
    // P^T * P should be close to identity
    auto sc = make_ring(15, 6);
    ASSERT_EQ(sc.enabled, 1);
    ASSERT_GE(sc.num_kept, 2);

    int na = sc.n_active;
    int nk = sc.num_kept;
    for (int p = 0; p < nk; ++p) {
        for (int q = 0; q < nk; ++q) {
            const double* Pp = &sc.P[static_cast<std::size_t>(p * na)];
            const double* Pq = &sc.P[static_cast<std::size_t>(q * na)];
            double dot = 0.0;
            for (int i = 0; i < na; ++i)
                dot += Pp[static_cast<std::size_t>(i)] * Pq[static_cast<std::size_t>(i)];

            if (p == q) {
                EXPECT_NEAR(dot, 1.0, 1e-8)
                    << "P column " << p << " not unit-norm";
            } else {
                EXPECT_NEAR(dot, 0.0, 1e-8)
                    << "P columns " << p << " and " << q << " not orthogonal";
            }
        }
    }
}

// ============================================================================
// BoundaryMasking
// ============================================================================

TEST(BoundaryMasking, OutfallNodesExcluded) {
    // 5-node path; nodes 0 and 4 are outfalls
    // conduits: 0-1, 1-2, 2-3, 3-4
    int n1[4] = {0, 1, 2, 3}, n2[4] = {1, 2, 3, 4};
    int outfall[5] = {1, 0, 0, 0, 1};
    SpectralCoarse sc;
    sc.num_modes = 3;
    bool ok = sc.init(5, 4, outfall, n1, n2, 3);

    EXPECT_TRUE(ok);
    EXPECT_EQ(sc.n_active, 3);  // nodes 1, 2, 3 only
    EXPECT_EQ(sc.full_to_active[0], -1) << "Node 0 (outfall) should be excluded";
    EXPECT_EQ(sc.full_to_active[4], -1) << "Node 4 (outfall) should be excluded";
    EXPECT_GE(sc.full_to_active[1], 0);
    EXPECT_GE(sc.full_to_active[2], 0);
    EXPECT_GE(sc.full_to_active[3], 0);
}

TEST(BoundaryMasking, ActiveMapRoundtrip) {
    // full_to_active[active_map[i]] == i  for all active i
    auto sc = make_chain(10, 0, 9, 4);  // outfalls at 0 and 9
    ASSERT_EQ(sc.enabled, 1);
    for (int i = 0; i < sc.n_active; ++i) {
        int full = sc.active_map[static_cast<std::size_t>(i)];
        ASSERT_GE(full, 0);
        ASSERT_LT(full, sc.N);
        EXPECT_EQ(sc.full_to_active[static_cast<std::size_t>(full)], i)
            << "Round-trip failed at active index " << i;
    }
}

// ============================================================================
// CoarseOperator
// ============================================================================

TEST(CoarseOperator, AssemblySucceeds) {
    // 5-node chain, one outfall at end
    auto sc = make_chain(5, -1, 4, 2);
    ASSERT_EQ(sc.enabled, 1);

    // A_fine: diagonal = 1.0 for each active node (identity-like), no off-diag
    int n_active = sc.n_active;
    std::vector<double> diag(static_cast<std::size_t>(n_active), 1.0);

    // 4 conduits: 0-1, 1-2, 2-3, 3-4
    int cn1[4] = {0, 1, 2, 3}, cn2[4] = {1, 2, 3, 4};
    std::vector<double> off(4, 0.1);  // small conductance

    bool ok = sc.assembleCoarseOperator(diag.data(), 4, cn1, cn2, off.data());
    EXPECT_TRUE(ok) << "assembleCoarseOperator failed: last_error=" << sc.last_error;
}

TEST(CoarseOperator, IdentityDiagonalGivesPositiveDefiniteCoarse) {
    // With A_fine = I (no off-diagonal), A_coarse = P^T * I * P = P^T * P ≈ I
    // A_coarse should be SPD.
    auto sc = make_ring(10, 5);
    ASSERT_EQ(sc.enabled, 1);

    int na = sc.n_active;
    std::vector<double> diag(static_cast<std::size_t>(na), 1.0);
    // No conduit off-diagonals
    bool ok = sc.assembleCoarseOperator(diag.data(), 0, nullptr, nullptr, nullptr);
    EXPECT_TRUE(ok);

    // Verify A_coarse is close to identity (since P^T*P ≈ I for orthonormal P)
    int nk = sc.num_kept;
    for (int p = 0; p < nk; ++p) {
        double diag_val = sc.A_coarse[static_cast<std::size_t>(p + p * nk)];
        EXPECT_NEAR(diag_val, 1.0, 1e-6)
            << "A_coarse diagonal[" << p << "] should be ~1.0";
    }
}

// ============================================================================
// CorrectionDirection
// ============================================================================

TEST(CorrectionDirection, CorrectionReducesResidualNorm) {
    // 6-node chain, no outfalls. Assemble a simple A_fine, apply correction,
    // verify the residual norm decreases.
    auto sc = make_chain(6, -1, -1, 3);
    ASSERT_EQ(sc.enabled, 1);

    int na = sc.n_active;  // should be 6
    ASSERT_EQ(na, 6);

    // A_fine: uniform diagonal + tridiagonal off-diagonal
    std::vector<double> diag(static_cast<std::size_t>(na), 2.0);
    int cn1[5] = {0, 1, 2, 3, 4}, cn2[5] = {1, 2, 3, 4, 5};
    std::vector<double> off(5, 0.5);

    bool ok = sc.assembleCoarseOperator(diag.data(), 5, cn1, cn2, off.data());
    ASSERT_TRUE(ok);

    // Construct a depth residual: linear gradient (should be well-captured by coarse modes)
    std::vector<double> r(static_cast<std::size_t>(na));
    for (int i = 0; i < na; ++i)
        r[static_cast<std::size_t>(i)] = static_cast<double>(i) - 2.5;

    double norm_before = 0.0;
    for (int i = 0; i < na; ++i)
        norm_before = std::max(norm_before, std::fabs(r[static_cast<std::size_t>(i)]));

    std::vector<double> correction(static_cast<std::size_t>(na), 0.0);
    double omega = sc.applyCorrection(r.data(), correction.data(), 0.005);

    if (omega > 0.0) {
        // Residual after correction: r_new = r - omega*(A_fine * delta) ≈ r - correction
        // (first-order, same as the acceptance check)
        double norm_after = 0.0;
        for (int i = 0; i < na; ++i) {
            double r_new = r[static_cast<std::size_t>(i)]
                         - correction[static_cast<std::size_t>(i)];
            norm_after = std::max(norm_after, std::fabs(r_new));
        }
        EXPECT_LE(norm_after, norm_before * (1.0 + sc.accept_tol) + 1e-12)
            << "Correction was accepted but did not reduce residual norm";
    } else {
        // Correction rejected is also valid — just check diagnostics are consistent
        EXPECT_EQ(sc.corrections_rejected, 1);
        EXPECT_EQ(sc.corrections_accepted, 0);
    }
    EXPECT_EQ(sc.corrections_attempted, 1);
}

TEST(CorrectionDirection, DiagnosticsAccumulate) {
    auto sc = make_chain(8, -1, -1, 3);
    ASSERT_EQ(sc.enabled, 1);

    int na = sc.n_active;
    std::vector<double> diag(static_cast<std::size_t>(na), 1.5);
    int cn1[7] = {0,1,2,3,4,5,6}, cn2[7] = {1,2,3,4,5,6,7};
    std::vector<double> off(7, 0.3);
    ASSERT_TRUE(sc.assembleCoarseOperator(diag.data(), 7, cn1, cn2, off.data()));

    std::vector<double> r(static_cast<std::size_t>(na), 0.1);
    std::vector<double> corr(static_cast<std::size_t>(na));

    sc.applyCorrection(r.data(), corr.data(), 0.005);
    sc.applyCorrection(r.data(), corr.data(), 0.005);

    EXPECT_EQ(sc.corrections_attempted, 2);
    EXPECT_EQ(sc.corrections_accepted + sc.corrections_rejected, 2);

    sc.reset();
    EXPECT_EQ(sc.corrections_attempted, 0);
    EXPECT_EQ(sc.corrections_accepted, 0);
    EXPECT_EQ(sc.corrections_rejected, 0);
}

// ============================================================================
// EdgeCases
// ============================================================================

TEST(EdgeCases, AllOutfalls) {
    // All nodes are outfalls — should fail gracefully
    int n1[1] = {0}, n2[1] = {1};
    int outfall[2] = {1, 1};
    SpectralCoarse sc;
    sc.num_modes = 1;
    bool ok = sc.init(2, 1, outfall, n1, n2, 1);
    EXPECT_FALSE(ok);
    EXPECT_EQ(sc.enabled, 0);
    EXPECT_GT(sc.last_error, 0);
}

TEST(EdgeCases, SingleActiveNode) {
    // Only one non-outfall node — cannot form eigenbasis
    int n1[1] = {0}, n2[1] = {1};
    int outfall[2] = {0, 1};
    SpectralCoarse sc;
    sc.num_modes = 1;
    bool ok = sc.init(2, 1, outfall, n1, n2, 1);
    EXPECT_FALSE(ok);
    EXPECT_EQ(sc.enabled, 0);
}

TEST(EdgeCases, LargerChainStaysEnabled) {
    // 50-node chain should init and retain modes without issues
    auto sc = make_chain(50, 0, 49, 10);
    EXPECT_EQ(sc.enabled, 1) << "last_error=" << sc.last_error;
    EXPECT_GE(sc.num_kept, 1);
    EXPECT_EQ(sc.n_active, 48);  // 50 nodes minus 2 outfalls
}
