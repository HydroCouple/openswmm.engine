// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      test_fv_linear_workspace.cpp
 * @brief     Tests for fv linear workspace.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include "../../verification/fv_implicit_phase1/network_cases.hpp"

namespace {
using namespace fvnetwork;
/**
 * @brief Construct a diagonally dominant CSR workspace test matrix.
 *
 * No validation/factorization; n=0 yields row{0}. Intended small synthetic dimensionless systems.
 *
 * @param[in] n Nonnegative dimension fitting CSR index/storage limits.
 * @param[in] dense True stores every column; false retains only the main and adjacent diagonals.
 * @return Owned sorted CSR with diagonal 4+0.01*r and off-diagonal 0.1+0.001*c.
 */
S::Matrix matrix(int n, bool dense = false) {
    S::Matrix a;
    a.row.push_back(0);
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c)
            if (dense || std::abs(r - c) <= 1) {
                a.column.push_back(c);
                a.value.push_back(r == c ? 4.0 + .01 * r : .1 + .001 * c);
            }
        a.row.push_back(a.value.size());
    }
    return a;
}
/**
 * @brief Manufacture a right-hand side from a known sinusoidal solution.
 *
 * Allocates zeroed output and records a nonfatal expectation for apply; failure may return
 * partial/default values. No separate success flag or input mutation.
 *
 * @param[in] a Valid nonempty CSR row-pointer structure representing a square matrix.
 * @return Owned A*x with x[i]=1+0.1*sin(0.7*i).
 */
std::vector<double> rhs(const S::Matrix& a) {
    std::vector<double> x(a.row.size() - 1), b(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = 1 + .1 * std::sin(.7 * i);
    EXPECT_TRUE(a.apply(x.data(), b.data()));
    return b;
}
/**
 * @brief Solve one manufactured scalar or two-variable-block linear system.
 *
 * Uses rhs(a) and does not mutate the matrix or retain argument references; allocation/solver
 * failure follows the selected solver contract.
 *
 * @param[in] a Valid square matrix; block mode requires even dimension.
 * @param[in] block True selects solveBlockLinear with half-dimension blocks; false solveLinear.
 * @param[in,out] work Optional reusable workspace borrowed for this synchronous solve; null
 *     requests a cold solve.
 * @param[in] limits GMRES iteration/tolerance controls passed unchanged.
 * @param[out] x Solution storage passed to the selected solver; consult returned status before use.
 * @return Selected solver result, including convergence and workspace diagnostics.
 */
S::LinearResult linear(const S::Matrix& a, bool block, S::LinearWorkspace* work,
                       const NativeGmres::Controls& limits, std::vector<double>& x) {
    return block ? S::solveBlockLinear(a, rhs(a), (a.row.size() - 1) / 2, limits, x, work)
                 : S::solveLinear(a, rhs(a), limits, x, work);
}
/**
 * @brief Compare the endpoint fields used by workspace reuse tests exactly.
 *
 * Records nonfatal equality expectations for area, depth, discharge and head vectors. Other
 * endpoint fields and physical conservation are not checked here.
 *
 * @param[in] a First endpoint, unchanged.
 * @param[in] b Second endpoint, unchanged.
 */
void same(const S::Endpoint& a, const S::Endpoint& b) {
    EXPECT_EQ(a.area, b.area);
    EXPECT_EQ(a.depth, b.depth);
    EXPECT_EQ(a.discharge, b.discharge);
    EXPECT_EQ(a.head, b.head);
}
}

TEST(FvLinearWorkspace, ScalarAndBlockReuseSymbolicsButRefactorEveryChangedMatrix) {
    for (bool block : {false, true}) {
        S::LinearWorkspace work;
        auto a = matrix(8, true);
        std::size_t retained = 0;
        for (int step = 0; step < 4; ++step) {
            for (std::size_t r = 0; r + 1 < a.row.size(); ++r)
                for (auto t = a.row[r]; t < a.row[r + 1]; ++t)
                    a.value[t] = a.column[t] == static_cast<int>(r)
                                     ? 4. + step + .1 * r
                                     : .05 * (step + 1) / (1 + r + a.column[t]);
            std::vector<double> fresh, cached;
            const auto reference = linear(a, block, nullptr, {}, fresh),
                       result = linear(a, block, &work, {}, cached);
            ASSERT_TRUE(reference.converged());
            ASSERT_TRUE(result.converged());
            EXPECT_EQ(cached, fresh);
            EXPECT_TRUE(result.workspace.enabled);
            EXPECT_EQ(result.workspace.symbolic_reused, step > 0);
            EXPECT_EQ(result.workspace.symbolic_rebuilt, step == 0);
            EXPECT_EQ(result.gmres.reused_workspace, step > 0);
            if (step) {
                EXPECT_EQ(result.gmres.workspace_growths, 0);
                EXPECT_EQ(work.bytes(), retained);
            } else
                EXPECT_GT(result.gmres.workspace_growths, 0);
            EXPECT_EQ(work.bytes(), result.preconditioner_bytes + result.gmres.workspace_bytes);
            EXPECT_EQ(work.bytes(), result.workspace.retained_bytes);
            retained = work.bytes();
        }
        std::vector<double> zero;
        const auto z = block ? S::solveBlockLinear(a, std::vector<double>(8), 4, {}, zero, &work)
                             : S::solveLinear(a, std::vector<double>(8), {}, zero, &work);
        ASSERT_TRUE(z.converged());
        EXPECT_EQ(zero, std::vector<double>(8, 0)); // never a stale warm start
        work.release();
        EXPECT_EQ(work.bytes(), 0u);
    }
}

TEST(FvLinearWorkspace, FullColumnAndRowIdentityInvalidateEvenWhenSizesAndNnzAreUnchanged) {
    for (bool block : {false, true}) {
        auto a = matrix(4, true), b = a;
        // Prune to two different eight-entry patterns with equal row lengths.
        a.row = {0};
        a.column.clear();
        a.value.clear();
        b = a;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c)
                if (c == r || c == (r + 1) % 4) {
                    a.column.push_back(c);
                    a.value.push_back(c == r ? 4 : .1);
                }
            for (int c = 0; c < 4; ++c)
                if (c == r || c == (r + 2) % 4) {
                    b.column.push_back(c);
                    b.value.push_back(c == r ? 4 : .2);
                }
            a.row.push_back(a.value.size());
            b.row.push_back(b.value.size());
        }
        ASSERT_EQ(a.row, b.row);
        ASSERT_EQ(a.value.size(), b.value.size());
        S::LinearWorkspace work;
        std::vector<double> x, fresh;
        ASSERT_TRUE(linear(a, block, &work, {}, x).converged());
        const auto changed = linear(b, block, &work, {}, x);
        ASSERT_TRUE(changed.converged());
        EXPECT_TRUE(changed.workspace.symbolic_rebuilt);
        EXPECT_TRUE(changed.workspace.cache_released);
        ASSERT_TRUE(linear(b, block, nullptr, {}, fresh).converged());
        EXPECT_EQ(x, fresh);
    }
    // Identical COLUMN bytes with different valid row boundaries: the scatter
    // map must be rebuilt even if the aggregate block pattern happens to match.
    S::Matrix a;
    a.row = {0, 2, 4, 6, 8};
    a.column = {0, 1, 2, 3, 0, 2, 1, 3};
    a.value = {4, 1, 2, 1, 1, 3, 1, 3};
    auto b = a;
    b.row = {0, 1, 4, 6, 8};
    S::LinearWorkspace work;
    std::vector<double> x, fresh;
    ASSERT_TRUE(linear(a, true, &work, {}, x).converged());
    const auto changed = linear(b, true, &work, {}, x);
    ASSERT_TRUE(changed.converged());
    EXPECT_TRUE(changed.workspace.symbolic_rebuilt);
    ASSERT_TRUE(linear(b, true, nullptr, {}, fresh).converged());
    EXPECT_EQ(x, fresh);
}

TEST(FvLinearWorkspace, BackendPartitionAndDimensionChangesReplaceTheSingleCachedLayout) {
    S::LinearWorkspace work;
    auto a = matrix(6, true);
    std::vector<double> x;
    ASSERT_TRUE(S::solveBlockLinear(a, rhs(a), 3, {}, x, &work).converged());
    auto changed = S::solveBlockLinear(a, rhs(a), 1, {}, x, &work);
    ASSERT_TRUE(changed.converged());
    EXPECT_TRUE(changed.workspace.symbolic_rebuilt);
    EXPECT_TRUE(changed.workspace.cache_released);
    changed = S::solveLinear(a, rhs(a), {}, x, &work);
    ASSERT_TRUE(changed.converged());
    EXPECT_TRUE(changed.workspace.symbolic_rebuilt);
    a = matrix(2);
    changed = S::solveLinear(a, rhs(a), {}, x, &work);
    ASSERT_TRUE(changed.converged());
    EXPECT_TRUE(changed.workspace.symbolic_rebuilt);
}

TEST(FvLinearWorkspace, NumericZerosReuseThePatternButPrunedStructuralEntriesInvalidateIt) {
    auto a = matrix(4, true);
    S::LinearWorkspace work;
    std::vector<double> x, fresh;
    ASSERT_TRUE(linear(a, true, &work, {}, x).converged());
    for (std::size_t r = 0; r < 4; ++r)
        for (auto t = a.row[r]; t < a.row[r + 1]; ++t)
            if (a.column[t] != static_cast<int>(r))
                a.value[t] = 0;
    auto result = linear(a, true, &work, {}, x);
    ASSERT_TRUE(result.converged());
    EXPECT_TRUE(result.workspace.symbolic_reused);
    ASSERT_TRUE(linear(a, true, nullptr, {}, fresh).converged());
    EXPECT_EQ(x, fresh);
    S::Matrix sparse;
    sparse.row = {0, 1, 2, 3, 4};
    sparse.column = {0, 1, 2, 3};
    sparse.value = {4, 4.01, 4.02, 4.03};
    result = linear(sparse, true, &work, {}, x);
    ASSERT_TRUE(result.converged());
    EXPECT_TRUE(result.workspace.symbolic_rebuilt);
}

TEST(FvLinearWorkspace, TighterRestartAndMemoryLimitsReleaseOversizedRetainedBuffers) {
    for (bool block : {false, true}) {
        const auto a = matrix(24);
        S::LinearWorkspace small, work;
        std::vector<double> fresh, x;
        NativeGmres::Controls limits;
        limits.restart = 4;
        ASSERT_TRUE(linear(a, block, &small, limits, fresh).converged());
        const auto small_bytes = small.bytes();
        limits.restart = 20;
        ASSERT_TRUE(linear(a, block, &work, limits, x).converged());
        EXPECT_GT(work.bytes(), small_bytes);
        limits.restart = 4;
        limits.workspace_limit_bytes = small_bytes;
        const auto resized = linear(a, block, &work, limits, x);
        ASSERT_TRUE(resized.converged());
        EXPECT_TRUE(resized.workspace.cache_released);
        EXPECT_TRUE(resized.workspace.symbolic_rebuilt);
        EXPECT_LE(work.bytes(), small_bytes);
        EXPECT_EQ(x, fresh);
        limits.workspace_limit_bytes = 1;
        x = {-9};
        const auto failed = linear(a, block, &work, limits, x);
        EXPECT_EQ(failed.gmres.status, NativeGmres::Status::MemoryLimit);
        EXPECT_EQ(work.bytes(), 0u);
        EXPECT_EQ(x, (std::vector<double>{-9}));
        const auto recovered = linear(a, block, &work, {}, x);
        EXPECT_TRUE(recovered.converged());
        EXPECT_TRUE(recovered.workspace.symbolic_rebuilt);
    }
}

TEST(FvLinearWorkspace, FailedAndInvalidLinearCallsClearTheCacheAndCannotPublishOldCorrections) {
    for (bool block : {false, true}) {
        const auto a = matrix(4, true);
        S::LinearWorkspace work;
        std::vector<double> x;
        ASSERT_TRUE(linear(a, block, &work, {}, x).converged());
        NativeGmres::Controls limit;
        limit.max_iterations = 0;
        x = {-9};
        EXPECT_FALSE(linear(a, block, &work, limit, x).converged());
        EXPECT_EQ(work.bytes(), 0u);
        EXPECT_EQ(x, (std::vector<double>{-9}));
        ASSERT_TRUE(linear(a, block, &work, {}, x).converged());
        auto bad = a;
        bad.row[1] = bad.value.size() + 1;
        x = {-9};
        const auto invalid =
            block ? S::solveBlockLinear(bad, std::vector<double>(4, 1), 2, {}, x, &work)
                  : S::solveLinear(bad, std::vector<double>(4, 1), {}, x, &work);
        EXPECT_FALSE(invalid.converged());
        EXPECT_EQ(work.bytes(), 0u);
        EXPECT_EQ(x, (std::vector<double>{-9}));
        ASSERT_TRUE(linear(a, block, &work, {}, x).converged());
    }
}

TEST(FvLinearWorkspace, StructuredAssemblyAccountsForAndEvictsAPreexistingLinearCache) {
    auto ch = make("loop", 16, 660);
    auto p = problem(ch, "loop", true);
    S::Linearization l;
    ASSERT_TRUE(S::linearize(p, trial(p), .25, 1.e-4, l));
    S::CondensationControls c;
    c.tree_interface = true;
    c.reduced_gmres_only = true;
    c.preconditioner = S::Preconditioner::BlockIlu0;
    S::LinearWorkspace fresh, work;
    std::vector<double> reference, x;
    auto r = S::solveCondensed(ch.mesh, l.matrix, rhs(l.matrix), {}, c, false, reference, &fresh);
    ASSERT_TRUE(r.converged());
    c.workspace_limit_bytes = r.condensation.peak_workspace_bytes;
    // Populate as much of this cap as possible without exceeding it. Entry
    // admission alone cannot evict it; subsequent Schur assembly must account
    // for these buffers before its own allocation.
    for (int n = 2; n < 100; ++n) {
        S::LinearWorkspace trial;
        auto a = matrix(n);
        std::vector<double> ignored;
        if (linear(a, true, &trial, {}, ignored).converged() &&
            trial.bytes() <= c.workspace_limit_bytes && trial.bytes() > work.bytes())
            work = std::move(trial);
    }
    ASSERT_GT(work.bytes(), 0u);
    ASSERT_LE(work.bytes(), c.workspace_limit_bytes);
    r = S::solveCondensed(ch.mesh, l.matrix, rhs(l.matrix), {}, c, false, x, &work);
    ASSERT_TRUE(r.converged());
    EXPECT_TRUE(r.workspace.cache_released);
    EXPECT_LE(r.condensation.peak_workspace_bytes, c.workspace_limit_bytes);
    EXPECT_LE(work.bytes(), c.workspace_limit_bytes);
    EXPECT_EQ(x, reference);
}

TEST(FvLinearWorkspace, ReducedFailureDropsItsCacheBeforeTheOriginalMatrixFallback) {
    auto ch = make("loop", 8, 660);
    auto p = problem(ch, "loop", true);
    S::Linearization l;
    ASSERT_TRUE(S::linearize(p, trial(p), .25, 1.e-4, l));
    S::LinearWorkspace work;
    std::vector<double> x;
    S::CondensationControls c;
    c.tree_interface = true;
    c.reduced_gmres_only = true;
    c.preconditioner = S::Preconditioner::BlockIlu0;
    ASSERT_TRUE(
        S::solveCondensed(ch.mesh, l.matrix, rhs(l.matrix), {}, c, false, x, &work).converged());
    c.workspace_limit_bytes = 1;
    const auto fallback =
        S::solveCondensed(ch.mesh, l.matrix, rhs(l.matrix), {}, c, true, x, &work);
    ASSERT_TRUE(fallback.converged());
    EXPECT_TRUE(fallback.fallback);
    EXPECT_TRUE(fallback.workspace.cache_released);
    EXPECT_TRUE(fallback.workspace.symbolic_rebuilt);
    EXPECT_EQ(fallback.block_ilu.blocks, l.residual.size() - ch.n);
    c.workspace_limit_bytes = 64 * 1024 * 1024;
    const auto reduced =
        S::solveCondensed(ch.mesh, l.matrix, rhs(l.matrix), {}, c, false, x, &work);
    ASSERT_TRUE(reduced.converged());
    EXPECT_TRUE(reduced.workspace.symbolic_rebuilt); // full -> reduced ordering
}

TEST(FvLinearWorkspace, InstanceReuseAcrossNewtonAndTimeStepsMatchesColdEndpointsExactly) {
    for (const std::string name : {"bed", "tee_source", "loop"})
        for (bool block : {false, true})
            for (bool reduced : {false, true}) {
                SCOPED_TRACE(name);
                auto ch = make(name, 8, 660);
                auto p = problem(ch, name, true);
                S solver, cold;
                S::Controls c;
                c.preconditioner =
                    block ? S::Preconditioner::BlockIlu0 : S::Preconditioner::ScalarIlu0;
                if (reduced) {
                    c.method = S::LinearMethod::CondensedOnly;
                    c.condensation.tree_interface = true;
                    c.condensation.reduced_gmres_only = true;
                }
                bool hit = false;
                for (int step = 0; step < 3; ++step) {
                    S::Endpoint expected, endpoint;
                    c.reuse_linear_workspace = false;
                    ASSERT_TRUE(cold.solve(p, .25, c, expected).converged());
                    c.reuse_linear_workspace = true;
                    const auto result = solver.solve(p, .25, c, endpoint);
                    ASSERT_TRUE(result.converged());
                    same(endpoint, expected);
                    for (const auto& r : result.linear_history)
                        hit |= r.workspace.symbolic_reused;
                    p.old_area = endpoint.area;
                    p.old_discharge = endpoint.discharge;
                    p.initial_depth = endpoint.depth;
                    p.initial_head = endpoint.head;
                }
                EXPECT_EQ(hit, !reduced || name == "loop");
                c.reuse_linear_workspace = false;
                S::Endpoint endpoint;
                ASSERT_TRUE(solver.solve(p, .25, c, endpoint).converged());
                EXPECT_EQ(solver.linearWorkspaceBytes(), 0u);
            }
}

TEST(FvLinearWorkspace, NonlinearFailureAfterASuccessfulCorrectionReleasesInstanceCache) {
    auto ch = make("loop", 8, 660);
    auto p = problem(ch, "loop", true);
    S solver;
    S::Controls c;
    c.preconditioner = S::Preconditioner::BlockIlu0;
    c.reuse_linear_workspace = true;
    c.max_newton = 1;
    S::Endpoint endpoint;
    endpoint.area = {-9};
    const auto failed = solver.solve(p, .25, c, endpoint);
    ASSERT_FALSE(failed.converged());
    ASSERT_FALSE(failed.linear_history.empty());
    EXPECT_TRUE(failed.last_linear.converged());
    EXPECT_EQ(solver.linearWorkspaceBytes(), 0u);
    EXPECT_EQ(endpoint.area, (std::vector<double>{-9}));
    c.max_newton = 30;
    ASSERT_TRUE(solver.solve(p, .25, c, endpoint).converged());
    ASSERT_GT(solver.linearWorkspaceBytes(), 0u);
    const auto rest = problem(ch, "loop", false);
    c.method = S::LinearMethod::CondensedOnly;
    c.condensation.workspace_limit_bytes = 1;
    const auto idle = solver.solve(rest, .25, c, endpoint);
    ASSERT_TRUE(idle.converged());
    EXPECT_TRUE(idle.linear_history.empty());
    EXPECT_EQ(solver.linearWorkspaceBytes(),
              0u); // enforce tighter cap even with NO linear correction
}
