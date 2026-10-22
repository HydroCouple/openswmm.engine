// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      test_fv_conduit_condensation.cpp
 * @brief     Tests for fv conduit condensation.
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
 * @brief Solve the fixture system with independent dense long-double partial pivoting.
 *
 * Copies coefficients/rhs into an augmented matrix, pivots by maximum absolute column entry,
 * eliminates and back-substitutes. No finite/conditioning checks or small-pivot tolerance is
 * applied. Does not mutate inputs or invoke production solvers.
 *
 * @param[in] m Valid canonical square CSR matrix compatible with rhs; duplicate columns would
 *     overwrite during expansion.
 * @param[in] rhs Right-hand side; size must fit int and dense storage limits.
 * @return Owned solution converted to double, or empty on an exactly zero selected
 *     pivot/zero-dimensional system.
 */
std::vector<double> oracle(const S::Matrix& m, const std::vector<double>& rhs) {
    const int n = static_cast<int>(rhs.size());
    std::vector<std::vector<long double>> a(n, std::vector<long double>(n + 1));
    for (int r = 0; r < n; ++r) {
        for (auto t = m.row[r]; t < m.row[r + 1]; ++t)
            a[r][m.column[t]] = m.value[t];
        a[r][n] = rhs[r];
    }
    for (int c = 0; c < n; ++c) {
        int p = c;
        for (int r = c + 1; r < n; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c]))
                p = r;
        std::swap(a[p], a[c]);
        if (a[c][c] == 0)
            return {};
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
 * @brief Convert a dense square test matrix to sorted CSR with explicit diagonal zeros.
 *
 * Does not validate row shapes, finite coefficients or numerical conditioning; NaN compares nonzero
 * and is retained. Inputs are unchanged.
 *
 * @param[in] a n-by-n readable coefficient matrix; every row covers n entries and counts fit CSR
 *     integer storage.
 * @return Owned CSR with row{0} for an empty matrix and otherwise all nonzero/diagonal entries in
 *     increasing column order.
 */
S::Matrix matrix(const std::vector<std::vector<double>>& a) {
    S::Matrix m;
    m.row.push_back(0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (std::size_t j = 0; j < a.size(); ++j)
            if (a[i][j] != 0 || i == j) {
                m.column.push_back(static_cast<int>(j));
                m.value.push_back(a[i][j]);
            }
        m.row.push_back(m.value.size());
    }
    return m;
}
/**
 * @brief Apply a compatible CSR fixture matrix to a dense vector.
 *
 * Records EXPECT_TRUE on matrix apply, without a separate validity return or dimension recovery.
 * Allocation errors propagate.
 *
 * @param[in] a Valid square CSR matrix with dimension x.size().
 * @param[in] x Readable compatible vector, unchanged.
 * @return Owned product buffer initialized to zero before apply; partial/default values can remain
 *     after nonfatal failure.
 */
std::vector<double> matvec(const S::Matrix& a, const std::vector<double>& x) {
    std::vector<double> y(x.size());
    EXPECT_TRUE(a.apply(x.data(), y.data()));
    return y;
}
/**
 * @brief Construct the minimal length/range metadata for one condensation conduit.
 *
 * This is not a full hydraulic network and cannot be assumed solver-ready. Does not validate counts
 * or seed state; allocation errors propagate.
 *
 * @param[in] cells Intended positive cell count supported by vector allocation and conduit indices.
 * @return Owned mesh with cells one-foot lengths, conduit begin{0}/count{cells}; all other
 *     topology/geometry remains default.
 */
NetworkMeshData oneConduit(int cells) {
    NetworkMeshData m;
    m.cell_dx.assign(cells, 1.0);
    m.conduit_cell_begin = {0};
    m.conduit_cell_count = {cells};
    return m;
}
/**
 * @brief Assert the absolute Euclidean residual norm of a condensed-system solution.
 *
 * Computes matvec then hypot over rhs entries and uses nonfatal EXPECT_LE. Does not normalize,
 * verify vector extents or estimate numerical conditioning.
 *
 * @param[in] m Valid CSR matrix compatible with rhs/x.
 * @param[in] rhs RHS with x.size() entries.
 * @param[in] x Candidate solution, unchanged.
 * @param[in] tolerance Nonnegative absolute residual-norm bound.
 */
void check(const S::Matrix& m, const std::vector<double>& rhs, const std::vector<double>& x,
           double tolerance) {
    const auto y = matvec(m, x);
    double error = 0.0;
    for (std::size_t i = 0; i < rhs.size(); ++i)
        error = std::hypot(error, y[i] - rhs[i]);
    EXPECT_LE(error, tolerance);
}
}

TEST(FvConduitCondensation, FullDenseAndCondensedCorrectionsAgreeOnEveryOriginalUnknown) {
    for (const std::string name : {"bed", "geometry", "virtual", "tee_source", "loop", "reversed"})
        for (int cells : {4, 8, 16}) {
            SCOPED_TRACE(name + std::to_string(cells));
            auto ch = make(name, cells, 660);
            auto p = problem(ch, name, true);
            auto x = trial(p);
            S::Linearization l;
            ASSERT_TRUE(S::linearize(p, x, .25, 1.e-4, l));
            std::vector<double> known(l.residual.size());
            for (std::size_t i = 0; i < known.size(); ++i)
                known[i] = std::sin(.7 * i + .2);
            const auto rhs = matvec(l.matrix, known), reference = oracle(l.matrix, rhs);
            ASSERT_EQ(reference.size(), known.size());
            NativeGmres::Controls linear;
            linear.absolute_tolerance = 1.e-13;
            linear.relative_tolerance = 1.e-12;
            std::vector<double> full;
            ASSERT_TRUE(S::solveLinear(l.matrix, rhs, linear, full).converged());
            for (bool krylov : {false, true}) {
                S::CondensationControls controls;
                controls.reduced_gmres_only = krylov;
                std::vector<double> answer;
                const auto r =
                    S::solveCondensed(ch.mesh, l.matrix, rhs, linear, controls, false, answer);
                ASSERT_TRUE(r.converged()) << static_cast<int>(r.condensation.status) << ' '
                                           << r.condensation.full_true_residual;
                EXPECT_FALSE(r.fallback);
                EXPECT_GT(r.condensation.eliminated, 0);
                EXPECT_EQ(r.condensation.eliminated + r.condensation.retained, known.size());
                EXPECT_EQ(r.condensation.used_reduced_gmres, krylov);
                if (krylov)
                    EXPECT_LE(r.gmres.tolerance, .501 * r.condensation.full_tolerance);
                EXPECT_LE(r.condensation.full_true_residual, r.condensation.full_tolerance);
                EXPECT_LE(r.condensation.peak_workspace_bytes, controls.workspace_limit_bytes);
                check(l.matrix, rhs, answer, r.condensation.full_tolerance);
                for (std::size_t i = 0; i < known.size(); ++i) {
                    EXPECT_NEAR(answer[i], known[i], 2.e-9);
                    EXPECT_NEAR(answer[i], reference[i], 2.e-9);
                    EXPECT_NEAR(answer[i], full[i], 2.e-9);
                }
            }
        }
}

TEST(FvConduitCondensation, InteriorCountDoesNotChangeProtectedInterfaceDimension) {
    for (int cells : {4, 16, 64, 256}) {
        auto ch = make("loop", cells, 660);
        auto p = problem(ch, "loop", true);
        S::Linearization l;
        ASSERT_TRUE(S::linearize(p, trial(p), .25, 1.e-4, l));
        std::vector<double> rhs(l.residual.size(), 1.0), answer;
        const auto r = S::solveCondensed(ch.mesh, l.matrix, rhs, {}, {}, false, answer);
        ASSERT_TRUE(r.converged()) << cells << ' ' << static_cast<int>(r.condensation.status);
        EXPECT_EQ(r.condensation.retained, 28);
        EXPECT_EQ(r.condensation.eliminated, 12 * (cells - 2));
        EXPECT_LT(r.condensation.schur_entries, 300u);
    }
}

TEST(FvConduitCondensation, SingularBlockInNonsingularInteriorUsesUnchangedFullFallback) {
    auto mesh = oneConduit(4);
    std::vector<std::vector<double>> a(8, std::vector<double>(8));
    for (int i : {0, 1, 6, 7})
        a[i][i] = 1;
    for (int i = 0; i < 2; ++i) {
        a[2 + i][4 + i] = 1;
        a[4 + i][2 + i] = 1;
    }
    const auto m = matrix(a);
    const std::vector<double> known = {1, 2, 3, 4, 5, 6, 7, 8}, rhs = matvec(m, known);
    std::vector<double> x = {-7};
    auto r = S::solveCondensed(mesh, m, rhs, {}, {}, false, x);
    EXPECT_FALSE(r.converged());
    EXPECT_EQ(r.condensation.status, S::CondensationStatus::BlockPivot);
    EXPECT_EQ(r.condensation.failed_cell, 1);
    EXPECT_EQ(x, (std::vector<double>{-7}));
    r = S::solveCondensed(mesh, m, rhs, {}, {}, true, x);
    ASSERT_TRUE(r.converged());
    EXPECT_TRUE(r.fallback);
    EXPECT_EQ(r.condensation.status, S::CondensationStatus::BlockPivot);
    for (int i = 0; i < 8; ++i)
        EXPECT_NEAR(x[i], known[i], 1.e-9);
}

TEST(FvConduitCondensation, ProductionDensePivotsAcrossZeroLeafAndRejectsIncompatibility) {
    NetworkMeshData mesh;
    const auto m = matrix({{0, 2}, {3, 1}});
    const std::vector<double> known = {2, -3}, rhs = matvec(m, known);
    std::vector<double> x;
    auto r = S::solveCondensed(mesh, m, rhs, {}, {}, false, x);
    ASSERT_TRUE(r.converged());
    EXPECT_TRUE(r.condensation.used_dense);
    EXPECT_NEAR(x[0], 2, 1.e-14);
    EXPECT_NEAR(x[1], -3, 1.e-14);
    const auto singular = matrix({{0, 0}, {0, 0}});
    x = {-9};
    r = S::solveCondensed(mesh, singular, {1, 1}, {}, {}, true, x);
    EXPECT_FALSE(r.converged());
    EXPECT_TRUE(r.fallback);
    EXPECT_GE(r.condensation.failed_interface_pivot, 0);
    EXPECT_EQ(x, (std::vector<double>{-9}));
}

TEST(FvConduitCondensation, ResourceAndFillLimitsFailAtomicallyOrUseFullFallback) {
    auto ch = make("tee", 8, 660);
    auto p = problem(ch, "tee", true);
    S::Linearization l;
    ASSERT_TRUE(S::linearize(p, trial(p), .25, 1.e-4, l));
    auto rhs = l.residual;
    for (bool memory : {false, true}) {
        S::CondensationControls controls;
        if (memory)
            controls.workspace_limit_bytes = 1;
        else
            controls.max_schur_entries = 1;
        std::vector<double> x = {-9};
        auto r = S::solveCondensed(ch.mesh, l.matrix, rhs, {}, controls, false, x);
        EXPECT_FALSE(r.converged());
        EXPECT_EQ(x, (std::vector<double>{-9}));
        EXPECT_EQ(r.condensation.status,
                  memory ? S::CondensationStatus::MemoryLimit : S::CondensationStatus::FillLimit);
        r = S::solveCondensed(ch.mesh, l.matrix, rhs, {}, controls, true, x);
        ASSERT_TRUE(r.converged());
        EXPECT_TRUE(r.fallback);
    }
    S::CondensationControls controls;
    controls.reduced_gmres_only = true;
    NativeGmres::Controls linear;
    linear.max_iterations = 0;
    std::vector<double> x = {-9};
    auto r = S::solveCondensed(ch.mesh, l.matrix, rhs, linear, controls, false, x);
    EXPECT_FALSE(r.converged());
    EXPECT_EQ(r.condensation.status, S::CondensationStatus::InterfaceFailure);
    EXPECT_EQ(x, (std::vector<double>{-9}));
    S solver;
    S::Controls nonlinear;
    nonlinear.method = S::LinearMethod::CondensedOnly;
    nonlinear.condensation.workspace_limit_bytes = 1;
    S::Endpoint endpoint;
    endpoint.area = {-9};
    endpoint.discharge = {-8};
    endpoint.depth = {-7};
    endpoint.head = {-6};
    const auto failed = solver.solve(p, .25, nonlinear, endpoint);
    EXPECT_EQ(failed.status, S::Status::LinearFailure);
    EXPECT_EQ(endpoint.area, (std::vector<double>{-9}));
    EXPECT_EQ(endpoint.discharge, (std::vector<double>{-8}));
    EXPECT_EQ(endpoint.depth, (std::vector<double>{-7}));
    EXPECT_EQ(endpoint.head, (std::vector<double>{-6}));
    ASSERT_EQ(failed.linear_history.size(), 1u);
    EXPECT_EQ(failed.linear_history[0].condensation.status, S::CondensationStatus::MemoryLimit);
}

TEST(FvConduitCondensation, WiderStencilIsRejectedWithoutChangingTheOriginalEquation) {
    auto mesh = oneConduit(6);
    std::vector<std::vector<double>> a(12, std::vector<double>(12));
    for (int i = 0; i < 12; ++i)
        a[i][i] = 2;
    a[2][8] = .1; // nonadjacent INTERIOR coupling, outside eligible block stencil
    const auto m = matrix(a);
    const std::vector<double> known(12, 1), rhs = matvec(m, known);
    std::vector<double> x = {-9};
    auto r = S::solveCondensed(mesh, m, rhs, {}, {}, false, x);
    EXPECT_EQ(r.condensation.status, S::CondensationStatus::UnsupportedStencil);
    EXPECT_FALSE(r.converged());
    EXPECT_EQ(x, (std::vector<double>{-9}));
    r = S::solveCondensed(mesh, m, rhs, {}, {}, true, x);
    ASSERT_TRUE(r.converged());
    EXPECT_TRUE(r.fallback);
    check(m, rhs, x, 1.e-10);
}

TEST(FvConduitCondensation, DisconnectedComponentsAndIsolatedZeroHeadNeverReceivePins) {
    NetworkMeshData mesh;
    mesh.cell_dx.assign(8, 1);
    mesh.conduit_cell_begin = {0, 4};
    mesh.conduit_cell_count = {4, 4};
    std::vector<std::vector<double>> a(17, std::vector<double>(17));
    for (int i = 0; i < 16; ++i)
        a[i][i] = 2;
    a[16][16] = 0;
    const auto m = matrix(a);
    std::vector<double> rhs(17, 1), x = {-9};
    auto r = S::solveCondensed(mesh, m, rhs, {}, {}, false, x);
    EXPECT_FALSE(r.converged());
    EXPECT_EQ(x, (std::vector<double>{-9}));
    rhs[16] = 0;
    r = S::solveCondensed(mesh, m, rhs, {}, {}, false, x);
    ASSERT_TRUE(r.converged());
    EXPECT_EQ(x[16], 0);
    check(m, rhs, x, r.condensation.full_tolerance);
}

TEST(FvConduitCondensation, InvalidRangesCsrAndNonfiniteInputsCannotPublish) {
    auto mesh = oneConduit(4);
    const auto m = matrix({{1, 0}, {0, 1}});
    std::vector<double> x = {-9};
    EXPECT_FALSE(S::solveCondensed(mesh, m, {1, 1}, {}, {}, true, x).converged());
    EXPECT_EQ(x, (std::vector<double>{-9}));
    NetworkMeshData empty;
    auto bad = m;
    bad.row = {0, 4, 4};
    EXPECT_FALSE(S::solveCondensed(empty, bad, {1, 1}, {}, {}, true, x).converged());
    auto linear = NativeGmres::Controls{};
    linear.relative_tolerance = nan();
    EXPECT_FALSE(S::solveCondensed(empty, m, {1, 1}, linear, {}, true, x).converged());
    EXPECT_FALSE(S::solveCondensed(empty, m, {1, nan()}, {}, {}, true, x).converged());
    EXPECT_EQ(x, (std::vector<double>{-9}));
}

TEST(FvConduitCondensation, NonlinearEndpointsMatchFullGraphWithOriginalPhysicalBudgets) {
    for (const std::string name : {"geometry", "virtual", "tee_source", "loop", "reversed"}) {
        SCOPED_TRACE(name);
        auto ch = make(name, 16, 660);
        auto p = problem(ch, name, true);
        S solver;
        S::Endpoint full;
        ASSERT_TRUE(solver.solve(p, .25, {}, full).converged());
        for (bool krylov : {false, true}) {
            S::Controls controls;
            controls.method = S::LinearMethod::CondensedOnly;
            controls.condensation.reduced_gmres_only = krylov;
            S::Endpoint endpoint;
            const auto result = solver.solve(p, .25, controls, endpoint);
            ASSERT_TRUE(result.converged()) << static_cast<int>(result.status);
            EXPECT_LE(result.scaled_residual, 1.0);
            ASSERT_FALSE(result.linear_history.empty());
            for (const auto& linear : result.linear_history) {
                EXPECT_TRUE(linear.converged());
                EXPECT_FALSE(linear.fallback);
                EXPECT_EQ(linear.condensation.status, S::CondensationStatus::Converged);
            }
            for (int i = 0; i < ch.n; ++i) {
                EXPECT_NEAR(endpoint.depth[i], full.depth[i], 1.e-8);
                EXPECT_NEAR(endpoint.area[i], full.area[i], 1.e-10);
                EXPECT_NEAR(endpoint.discharge[i], full.discharge[i], 1.e-8);
            }
        }
    }
}

TEST(FvConduitCondensation, LargeSingleCycleRetainsItsWholeCoreAndUsesSparseInterfaceGmres) {
    constexpr int edges = 128, cells = 4 * edges, n = 2 * cells + edges;
    NetworkMeshData mesh;
    mesh.cell_dx.assign(cells, 1.0);
    for (int e = 0; e < edges; ++e) {
        mesh.conduit_cell_begin.push_back(4 * e);
        mesh.conduit_cell_count.push_back(4);
    }
    S::Matrix a;
    a.row.push_back(0);
    for (int r = 0; r < n; ++r) {
        std::vector<std::pair<int, double>> entries;
        if (r < 2 * cells) {
            const int c = r / 2, e = c / 4;
            entries.emplace_back(r, 4.0);
            entries.emplace_back(r ^ 1, .03);
            if (c % 4 > 0)
                entries.emplace_back(r - 2, -.5);
            if (c % 4 < 3)
                entries.emplace_back(r + 2, -.4);
            if (r % 2 == 0 && c % 4 == 0)
                entries.emplace_back(2 * cells + e, -.2);
            if (r % 2 == 0 && c % 4 == 3)
                entries.emplace_back(2 * cells + (e + 1) % edges, -.2);
        } else {
            const int e = r - 2 * cells;
            entries = {{r, 2}, {8 * e, -.25}, {8 * ((e + edges - 1) % edges) + 6, -.3}};
        }
        std::sort(entries.begin(), entries.end());
        for (auto [col, value] : entries) {
            a.column.push_back(col);
            a.value.push_back(value);
        }
        a.row.push_back(a.value.size());
    }
    std::vector<double> known(n);
    for (int i = 0; i < n; ++i)
        known[i] = 1 + .1 * std::sin(.2 * i);
    const auto rhs = matvec(a, known);
    std::vector<double> x, full;
    const auto r = S::solveCondensed(mesh, a, rhs, {}, {}, false, x);
    ASSERT_TRUE(r.converged());
    EXPECT_FALSE(r.condensation.used_dense);
    EXPECT_TRUE(r.condensation.used_reduced_gmres);
    EXPECT_EQ(r.condensation.retained, 5 * edges);
    EXPECT_EQ(r.condensation.eliminated, 4 * edges);
    ASSERT_TRUE(S::solveLinear(a, rhs, {}, full).converged());
    for (int i = 0; i < n; ++i) {
        EXPECT_NEAR(x[i], known[i], 2.e-9);
        EXPECT_NEAR(x[i], full[i], 2.e-9);
    }
    check(a, rhs, x, r.condensation.full_tolerance);
}

TEST(FvConduitCondensation, AReducedSuccessCannotBypassOriginalReconstructionResidual) {
    auto mesh = oneConduit(3);
    std::vector<std::vector<double>> a(6, std::vector<double>(6));
    for (int i : {0, 1, 4, 5})
        a[i][i] = 1;
    a[2][2] = .1;
    a[2][3] = .2;
    a[3][2] = .3;
    a[3][3] = .4;
    auto linear = NativeGmres::Controls{};
    linear.absolute_tolerance = 1.e-30;
    linear.relative_tolerance = 1.e-30;
    const std::vector<double> rhs = {1, 0, .7, .8, 0, 0};
    const auto m = matrix(a);
    for (bool krylov : {false, true}) {
        S::CondensationControls controls;
        controls.reduced_gmres_only = krylov;
        std::vector<double> x = {-9};
        const auto r = S::solveCondensed(mesh, m, rhs, linear, controls, false, x);
        EXPECT_FALSE(r.converged());
        EXPECT_EQ(r.condensation.status, S::CondensationStatus::ReconstructionFailure);
        EXPECT_EQ(r.condensation.reduced_true_residual, 0.0);
        EXPECT_GT(r.condensation.full_true_residual, r.condensation.full_tolerance);
        EXPECT_EQ(x, (std::vector<double>{-9}));
        if (krylov) {
            EXPECT_TRUE(r.gmres.converged());
            EXPECT_EQ(r.condensation.reduced.iterations, 1);
            const auto fallback = S::solveCondensed(mesh, m, rhs, linear, controls, true, x);
            EXPECT_TRUE(fallback.fallback);
            EXPECT_TRUE(fallback.condensation.reduced.converged());
            EXPECT_EQ(fallback.condensation.reduced.iterations, 1);
            EXPECT_GT(fallback.gmres.iterations, 0);
            if (fallback.converged())
                check(m, rhs, x, fallback.gmres.tolerance);
            else
                EXPECT_EQ(x, (std::vector<double>{-9}));
        }
    }
}
