// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      test_fv_block_ilu.cpp
 * @brief     Tests for fv block ilu.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include "../../verification/fv_implicit_phase1/network_cases.hpp"

namespace {
using namespace fvnetwork;
using Dense = std::vector<std::vector<double>>;
/**
 * @brief Convert a dense square fixture matrix to sorted CSR, retaining every diagonal.
 *
 * Does not validate row shapes/finite values or factor the matrix; n=0 yields row{0}. NaN compares
 * nonzero and is retained.
 *
 * @param[in] a n-by-n dense matrix with every row at least n entries; counts/indices must fit the
 *     CSR integer types.
 * @return Owned matrix with explicit diagonal zeros and all nonzero off-diagonal entries, ordered
 *     by increasing column.
 */
S::Matrix csr(const Dense& a) {
    S::Matrix m;
    m.row.push_back(0);
    for (std::size_t r = 0; r < a.size(); ++r) {
        for (std::size_t c = 0; c < a.size(); ++c)
            if (a[r][c] != 0 || r == c) {
                m.column.push_back(static_cast<int>(c));
                m.value.push_back(a[r][c]);
            }
        m.row.push_back(m.value.size());
    }
    return m;
}
/**
 * @brief Apply the fixture CSR matrix to a compatible dense vector.
 *
 * Uses nonfatal EXPECT_TRUE on apply success and does not return a separate validity flag. Caller
 * provides compatible dimensions/buffers; allocation errors propagate.
 *
 * @param[in] a Valid square CSR matrix with dimension x.size().
 * @param[in] x Readable input vector, unchanged.
 * @return Owned result initialized to zero before a.apply; partial/default values can remain after
 *     a recorded failure.
 */
std::vector<double> matvec(const S::Matrix& a, const std::vector<double>& x) {
    std::vector<double> b(x.size());
    EXPECT_TRUE(a.apply(x.data(), b.data()));
    return b;
}
/**
 * @brief Solve the dense fixture system with independent long-double partial pivoting.
 *
 * Copies inputs into a long-double augmented matrix, pivots by maximum absolute column entry and
 * back-substitutes. No finite/conditioning checks or tolerance-based singularity threshold; does
 * not call production solvers or mutate inputs.
 *
 * @param[in] original Readable square coefficient matrix at least b.size() in both dimensions.
 * @param[in] b Right-hand-side vector; size must fit int.
 * @return Owned double-converted solution, empty on an exactly zero selected pivot or a
 *     zero-dimensional system.
 */
std::vector<double> oracle(const Dense& original, const std::vector<double>& b) {
    const int n = static_cast<int>(b.size());
    std::vector<std::vector<long double>> a(n, std::vector<long double>(n + 1));
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c)
            a[r][c] = original[r][c];
        a[r][n] = b[r];
    }
    for (int c = 0; c < n; ++c) {
        int p = c;
        for (int r = c + 1; r < n; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c]))
                p = r;
        if (a[p][c] == 0)
            return {};
        std::swap(a[p], a[c]);
        for (int r = c + 1; r < n; ++r) {
            const auto f = a[r][c] / a[c][c];
            for (int j = c; j <= n; ++j)
                a[r][j] -= f * a[c][j];
        }
    }
    std::vector<long double> x(n);
    for (int r = n; r-- > 0;) {
        auto v = a[r][n];
        for (int c = r + 1; c < n; ++c)
            v -= a[r][c] * x[c];
        x[r] = v / a[r][r];
    }
    return {x.begin(), x.end()};
}
/**
 * @brief Assert the Euclidean norm of the computed linear residual is bounded.
 *
 * Calls matvec then accumulates hypot over b entries and records EXPECT_LE. Does not normalize by
 * rhs norm, validate dimensions or independently test solution conditioning.
 *
 * @param[in] a Valid CSR system matrix compatible with x and b.
 * @param[in] b Right-hand-side vector with x.size() entries.
 * @param[in] x Candidate solution, unchanged.
 * @param[in] tolerance Nonnegative absolute bound in the represented residual norm units.
 */
void check(const S::Matrix& a, const std::vector<double>& b, const std::vector<double>& x,
           double tolerance) {
    const auto image = matvec(a, x);
    double norm = 0;
    for (std::size_t i = 0; i < b.size(); ++i)
        norm = std::hypot(norm, image[i] - b[i]);
    EXPECT_LE(norm, tolerance);
}
/**
 * @brief Create reduced-GMRES controls using block-ILU(0) preconditioning.
 *
 * @param[in] tree Enable tree-interface path when true.
 * @return Owned default controls with BlockIlu0, reduced_gmres_only=true and tree_interface=tree;
 *     unrelated defaults retained.
 */
S::CondensationControls blocked(bool tree = false) {
    S::CondensationControls c;
    c.preconditioner = S::Preconditioner::BlockIlu0;
    c.reduced_gmres_only = true;
    c.tree_interface = tree;
    return c;
}
}

TEST(FvBlockIlu, MixedNoncommutingBlockChainIsAnExactFixedPreconditioner) {
    const std::vector<int> begin = {0, 2, 4, 6, 7}, size = {2, 2, 2, 1, 1};
    Dense a(8, std::vector<double>(8));
    for (int i = 0; i < 5; ++i)
        for (int r = 0; r < size[i]; ++r)
            for (int c = 0; c < size[i]; ++c)
                a[begin[i] + r][begin[i] + c] = r == c ? 4 + r : (r == 0 ? .3 : -.2);
    for (int i = 0; i < 4; ++i)
        for (int r = 0; r < size[i]; ++r)
            for (int c = 0; c < size[i + 1]; ++c) {
                a[begin[i] + r][begin[i + 1] + c] = .1 + .03 * r - .02 * c;
                a[begin[i + 1] + c][begin[i] + r] = -.2 + .01 * c + .04 * r;
            }
    const auto m = csr(a);
    const auto original = m;
    const std::vector<double> known = {1, -2, 3, -4, 5, -6, 7, -8}, b = matvec(m, known),
                              ref = oracle(a, b);
    ASSERT_EQ(ref.size(), known.size());
    std::vector<double> x;
    NativeGmres::Controls controls;
    controls.max_iterations = 1;
    controls.relative_tolerance = 1.e-12;
    const auto result = S::solveBlockLinear(m, b, 3, controls, x);
    ASSERT_TRUE(result.converged());
    EXPECT_EQ(result.gmres.iterations, 1);
    EXPECT_EQ(result.block_ilu.blocks, 5);
    EXPECT_EQ(result.block_ilu.entries, 13u);
    EXPECT_EQ(result.shifted_pivots, 0);
    EXPECT_EQ(result.block_ilu.failed_block, -1);
    for (std::size_t i = 0; i < known.size(); ++i) {
        EXPECT_NEAR(x[i], known[i], 1.e-12);
        EXPECT_NEAR(x[i], ref[i], 1.e-12);
    }
    EXPECT_EQ(m.row, original.row);
    EXPECT_EQ(m.column, original.column);
    EXPECT_EQ(m.value, original.value);
    check(m, b, x, result.gmres.tolerance);
}

TEST(FvBlockIlu, LocalPivotingHandlesAZeroScalarDiagonalInsideAnInvertibleCellBlock) {
    const auto m = csr({{0, 2}, {3, 1}});
    const std::vector<double> known = {2, -3}, b = matvec(m, known);
    std::vector<double> x;
    const auto r = S::solveBlockLinear(m, b, 1, {}, x);
    ASSERT_TRUE(r.converged());
    EXPECT_EQ(r.shifted_pivots, 0);
    EXPECT_EQ(r.gmres.iterations, 1);
    EXPECT_NEAR(x[0], 2, 1.e-12);
    EXPECT_NEAR(x[1], -3, 1.e-12);
}

TEST(FvBlockIlu, SingularCellPivotsAreShiftedOnlyInTheFixedPreconditioner) {
    const auto m = csr({{0, 0, 1, 0}, {0, 0, 0, 1}, {1, 0, 0, 0}, {0, 1, 0, 0}});
    const auto original = m;
    const std::vector<double> known = {1, -2, 3, -4}, b = matvec(m, known);
    std::vector<double> x;
    const auto r = S::solveBlockLinear(m, b, 2, {}, x);
    ASSERT_TRUE(r.converged());
    EXPECT_GT(r.shifted_pivots, 0);
    EXPECT_GT(r.block_ilu.largest_shift, 0);
    EXPECT_EQ(r.block_ilu.minimum_pivot_ratio, 0);
    for (int i = 0; i < 4; ++i)
        EXPECT_NEAR(x[i], known[i], 1.e-9);
    EXPECT_EQ(m.value, original.value);
    check(m, b, x, r.gmres.tolerance);
}

TEST(FvBlockIlu, MissingDiagonalsAndCompatibleZeroRowsNeverBecomeEquationPins) {
    S::Matrix m;
    m.row = {0, 1, 2};
    m.column = {1, 0};
    m.value = {1, 1};
    const auto original = m;
    std::vector<double> x;
    auto r = S::solveBlockLinear(m, {3, 2}, 0, {}, x);
    ASSERT_TRUE(r.converged());
    EXPECT_NEAR(x[0], 2, 1.e-9);
    EXPECT_NEAR(x[1], 3, 1.e-9);
    EXPECT_EQ(m.row, original.row);
    EXPECT_EQ(m.column, original.column);
    EXPECT_EQ(m.value, original.value);
    m.row = {0, 0, 0};
    m.column.clear();
    m.value.clear();
    r = S::solveBlockLinear(m, {0, 0}, 0, {}, x);
    ASSERT_TRUE(r.converged());
    EXPECT_EQ(x, (std::vector<double>{0, 0}));
    EXPECT_TRUE(m.value.empty());
    x = {-9};
    r = S::solveBlockLinear(m, {1, 1}, 0, {}, x);
    EXPECT_FALSE(r.converged());
    EXPECT_EQ(x, (std::vector<double>{-9}));
}

TEST(FvBlockIlu, NoOffBlockFillIsAddedOnACycleAndEveryOriginalEquationIsChecked) {
    const Dense a = {{4, .2, 0, -.1}, {-.3, 5, .1, 0}, {0, .2, 6, -.2}, {.1, 0, -.1, 3}};
    const auto m = csr(a);
    const std::vector<double> known = {1, -2, 3, -4}, b = matvec(m, known), ref = oracle(a, b);
    for (std::size_t pairs : {0u, 1u, 2u}) {
        std::vector<double> x;
        const auto r = S::solveBlockLinear(m, b, pairs, {}, x);
        ASSERT_TRUE(r.converged());
        if (pairs == 0)
            EXPECT_EQ(r.block_ilu.entries, 12u);
        for (int i = 0; i < 4; ++i) {
            EXPECT_NEAR(x[i], known[i], 1.e-9);
            EXPECT_NEAR(x[i], ref[i], 1.e-9);
        }
        check(m, b, x, r.gmres.tolerance);
    }
}

TEST(FvBlockIlu, FactorAndKrylovBudgetsIterationLimitsAndOverflowFailAtomically) {
    const auto m = csr({{4, .1}, {.2, 3}});
    std::vector<double> x;
    auto r = S::solveBlockLinear(m, {1, 2}, 1, {}, x);
    ASSERT_TRUE(r.converged());
    EXPECT_LE(r.preconditioner_bytes + r.gmres.workspace_bytes,
              NativeGmres::Controls{}.workspace_limit_bytes);
    for (const auto bytes : {std::size_t{1}, r.preconditioner_bytes}) {
        NativeGmres::Controls controls;
        controls.workspace_limit_bytes = bytes;
        x = {-9};
        r = S::solveBlockLinear(m, {1, 2}, 1, controls, x);
        EXPECT_EQ(r.gmres.status, NativeGmres::Status::MemoryLimit);
        EXPECT_EQ(x, (std::vector<double>{-9}));
    }
    NativeGmres::Controls metadata_only;
    metadata_only.workspace_limit_bytes = 3 * sizeof(std::size_t);
    r = S::solveBlockLinear(m, {1, 2}, 1, metadata_only, x);
    EXPECT_EQ(r.gmres.status, NativeGmres::Status::MemoryLimit);
    EXPECT_EQ(r.preconditioner_bytes,
              3 * sizeof(std::size_t)); // retain failed setup's owned-buffer peak
    EXPECT_EQ(x, (std::vector<double>{-9}));
    NativeGmres::Controls controls;
    controls.max_iterations = 0;
    x = {-9};
    r = S::solveBlockLinear(m, {1, 2}, 1, controls, x);
    EXPECT_FALSE(r.converged());
    EXPECT_EQ(x, (std::vector<double>{-9}));
    const auto huge = csr({{1.e308, 1.e308}, {1.e308, -1.e308}});
    r = S::solveBlockLinear(huge, {1, 1}, 0, {}, x);
    EXPECT_EQ(r.gmres.status, NativeGmres::Status::PreconditionerFailure);
    EXPECT_EQ(r.block_ilu.failed_block, 1);
    EXPECT_EQ(x, (std::vector<double>{-9}));
}

TEST(FvBlockIlu, InvalidPartitionsCsrAndNonfiniteInputsDoNotPublish) {
    const auto m = csr({{1, 0}, {0, 1}});
    std::vector<double> x = {-9};
    EXPECT_EQ(S::solveBlockLinear(m, {1, 2}, 2, {}, x).gmres.status,
              NativeGmres::Status::InvalidInput);
    auto bad = m;
    bad.row = {0, 4, 4};
    EXPECT_FALSE(S::solveBlockLinear(bad, {1, 2}, 1, {}, x).converged());
    bad = m;
    bad.column[0] = -1;
    EXPECT_FALSE(S::solveBlockLinear(bad, {1, 2}, 1, {}, x).converged());
    bad = m;
    bad.value[0] = nan();
    EXPECT_FALSE(S::solveBlockLinear(bad, {1, 2}, 1, {}, x).converged());
    auto limits = NativeGmres::Controls{};
    limits.relative_tolerance = nan();
    EXPECT_FALSE(S::solveBlockLinear(m, {1, 2}, 1, limits, x).converged());
    EXPECT_FALSE(S::solveBlockLinear(m, {1, nan()}, 1, {}, x).converged());
    EXPECT_EQ(x, (std::vector<double>{-9}));
}

TEST(FvBlockIlu, EveryFullCondensedAndTreeCorrectionAgreesOnOriginalCellAndHeadVariables) {
    for (const std::string name :
         {"bed", "geometry", "virtual", "tee_source", "loop", "reversed"}) {
        SCOPED_TRACE(name);
        auto ch = make(name, 8, 660);
        auto p = problem(ch, name, true);
        S::Linearization l;
        ASSERT_TRUE(S::linearize(p, trial(p), .25, 1.e-4, l));
        std::vector<double> known(l.residual.size());
        for (std::size_t i = 0; i < known.size(); ++i)
            known[i] = std::sin(.2 + .7 * i);
        const auto b = matvec(l.matrix, known);
        std::vector<double> scalar, x;
        ASSERT_TRUE(S::solveLinear(l.matrix, b, {}, scalar).converged());
        auto r = S::solveBlockLinear(l.matrix, b, ch.n, {}, x);
        ASSERT_TRUE(r.converged());
        EXPECT_EQ(r.block_ilu.blocks, known.size() - ch.n);
        for (std::size_t i = 0; i < known.size(); ++i) {
            EXPECT_NEAR(x[i], known[i], 2.e-9);
            EXPECT_NEAR(x[i], scalar[i], 2.e-9);
        }
        for (bool tree : {false, true}) {
            r = S::solveCondensed(ch.mesh, l.matrix, b, {}, blocked(tree), false, x);
            ASSERT_TRUE(r.converged()) << static_cast<int>(r.condensation.status);
            EXPECT_FALSE(r.fallback);
            EXPECT_EQ(r.block_ilu.used, !tree || name == "loop");
            if (r.block_ilu.used)
                EXPECT_TRUE(r.condensation.reduced_block_ilu.used);
            for (std::size_t i = 0; i < known.size(); ++i)
                EXPECT_NEAR(x[i], known[i], 2.e-9);
            check(l.matrix, b, x, r.condensation.full_tolerance);
        }
    }
}

TEST(FvBlockIlu, UnsafeExactInteriorFallsBackToTheOriginalBlockPreconditionedSystem) {
    NetworkMeshData mesh;
    mesh.cell_dx.assign(4, 1);
    mesh.conduit_cell_begin = {0};
    mesh.conduit_cell_count = {4};
    Dense a(8, std::vector<double>(8));
    for (int i : {0, 1, 6, 7})
        a[i][i] = 1;
    for (int i = 0; i < 2; ++i) {
        a[2 + i][4 + i] = 1;
        a[4 + i][2 + i] = 1;
    }
    const auto m = csr(a);
    const std::vector<double> known = {1, 2, 3, 4, 5, 6, 7, 8}, b = matvec(m, known);
    std::vector<double> x;
    const auto r = S::solveCondensed(mesh, m, b, {}, blocked(), true, x);
    ASSERT_TRUE(r.converged());
    EXPECT_TRUE(r.fallback);
    EXPECT_TRUE(r.block_ilu.used);
    EXPECT_EQ(r.condensation.status, S::CondensationStatus::BlockPivot);
    EXPECT_GT(r.shifted_pivots, 0);
    for (int i = 0; i < 8; ++i)
        EXPECT_NEAR(x[i], known[i], 1.e-9);
    check(m, b, x, r.gmres.tolerance);
}

TEST(FvBlockIlu, ALargeSingleCycleUsesMixedBlockGmresOnItsEntireRetainedCore) {
    constexpr int pairs = 128, n = 3 * pairs;
    NetworkMeshData mesh;
    mesh.cell_dx.assign(pairs, 1);
    for (int i = 0; i < pairs; ++i) {
        mesh.conduit_cell_begin.push_back(i);
        mesh.conduit_cell_count.push_back(1);
    }
    S::Matrix a;
    a.row.push_back(0);
    for (int r = 0; r < n; ++r) {
        std::vector<std::pair<int, double>> entries;
        if (r < 2 * pairs) {
            const int cell = r / 2;
            entries = {{r, 4}, {r ^ 1, .03}};
            if (r % 2 == 0) {
                entries.emplace_back(2 * pairs + cell, -.2);
                entries.emplace_back(2 * pairs + (cell + 1) % pairs, -.1);
            }
        } else {
            const int head = r - 2 * pairs;
            entries = {{r, 3}, {2 * head, -.25}, {2 * ((head + pairs - 1) % pairs), -.15}};
        }
        std::sort(entries.begin(), entries.end());
        for (auto [c, v] : entries) {
            a.column.push_back(c);
            a.value.push_back(v);
        }
        a.row.push_back(a.value.size());
    }
    std::vector<double> known(n);
    for (int i = 0; i < n; ++i)
        known[i] = 1 + .1 * std::sin(.2 * i);
    const auto b = matvec(a, known);
    std::vector<double> x, scalar;
    auto controls = blocked(true);
    controls.reduced_gmres_only = false; // size cap must select GMRES
    NativeGmres::Controls linear;
    linear.relative_tolerance = 1.e-12;
    const auto r = S::solveCondensed(mesh, a, b, linear, controls, false, x);
    ASSERT_TRUE(r.converged());
    EXPECT_FALSE(r.condensation.used_dense);
    EXPECT_TRUE(r.condensation.used_reduced_gmres);
    EXPECT_EQ(r.condensation.tree_eliminated, 0);
    EXPECT_EQ(r.condensation.core_scalars, n);
    EXPECT_TRUE(r.block_ilu.used);
    EXPECT_EQ(r.block_ilu.blocks, 2 * pairs);
    ASSERT_TRUE(S::solveLinear(a, b, linear, scalar).converged());
    for (int i = 0; i < n; ++i) {
        EXPECT_NEAR(x[i], known[i], 2.e-9);
        EXPECT_NEAR(x[i], scalar[i], 2.e-9);
    }
    check(a, b, x, r.condensation.full_tolerance);
}

TEST(FvBlockIlu, ReducedWorkSurvivesFailedReconstructionAndOriginalMatrixFallback) {
    NetworkMeshData mesh;
    mesh.cell_dx.assign(3, 1);
    mesh.conduit_cell_begin = {0};
    mesh.conduit_cell_count = {3};
    Dense a(6, std::vector<double>(6));
    for (int i : {0, 1, 4, 5})
        a[i][i] = 1;
    a[2][2] = .1;
    a[2][3] = .2;
    a[3][2] = .3;
    a[3][3] = .4;
    NativeGmres::Controls linear;
    linear.absolute_tolerance = 1.e-30;
    linear.relative_tolerance = 1.e-30;
    std::vector<double> x = {-9};
    const auto r =
        S::solveCondensed(mesh, csr(a), {1, 0, .7, .8, 0, 0}, linear, blocked(), true, x);
    EXPECT_FALSE(r.converged());
    EXPECT_TRUE(r.fallback);
    EXPECT_TRUE(r.block_ilu.used);
    EXPECT_EQ(r.condensation.status, S::CondensationStatus::ReconstructionFailure);
    EXPECT_TRUE(r.condensation.reduced_block_ilu.used);
    EXPECT_TRUE(r.condensation.reduced.converged());
    EXPECT_GT(r.condensation.reduced.iterations, 0);
    EXPECT_EQ(x, (std::vector<double>{-9}));
}

TEST(FvBlockIlu, NonlinearEndpointsRespectOriginalBudgetsAndFailuresPreserveTheEndpoint) {
    for (const std::string name :
         {"bed", "geometry", "virtual", "tee_source", "loop", "reversed"}) {
        auto ch = make(name, 8, 660);
        auto p = problem(ch, name, true);
        S solver;
        S::Endpoint reference;
        ASSERT_TRUE(solver.solve(p, .25, {}, reference).converged());
        for (const auto method : {S::LinearMethod::FullGmres, S::LinearMethod::CondensedOnly})
            for (bool tree : {false, true}) {
                S::Controls c;
                c.method = method;
                c.preconditioner = S::Preconditioner::BlockIlu0;
                c.condensation.reduced_gmres_only = true;
                c.condensation.tree_interface = tree;
                S::Endpoint endpoint;
                const auto result = solver.solve(p, .25, c, endpoint);
                ASSERT_TRUE(result.converged());
                EXPECT_LE(result.scaled_residual, 1.0);
                for (int i = 0; i < ch.n; ++i) {
                    EXPECT_NEAR(endpoint.depth[i], reference.depth[i], 1.e-8);
                    EXPECT_NEAR(endpoint.area[i], reference.area[i], 1.e-10);
                    EXPECT_NEAR(endpoint.discharge[i], reference.discharge[i], 1.e-8);
                }
            }
    }
    auto ch = make("loop", 8, 660);
    auto p = problem(ch, "loop", true);
    S solver;
    S::Controls c;
    c.method = S::LinearMethod::CondensedOnly;
    c.preconditioner = S::Preconditioner::BlockIlu0;
    c.condensation.tree_interface = true;
    c.condensation.reduced_gmres_only = true;
    c.linear.max_iterations = 0;
    S::Endpoint endpoint;
    endpoint.area = {-9};
    endpoint.depth = {-8};
    endpoint.discharge = {-7};
    endpoint.head = {-6};
    const auto result = solver.solve(p, .25, c, endpoint);
    EXPECT_EQ(result.status, S::Status::LinearFailure);
    EXPECT_TRUE(result.last_linear.block_ilu.used);
    EXPECT_EQ(endpoint.area, (std::vector<double>{-9}));
    EXPECT_EQ(endpoint.depth, (std::vector<double>{-8}));
    EXPECT_EQ(endpoint.discharge, (std::vector<double>{-7}));
    EXPECT_EQ(endpoint.head, (std::vector<double>{-6}));
}
