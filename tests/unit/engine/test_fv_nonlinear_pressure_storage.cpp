// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

#include "fv_test_support.hpp"
#include "hydraulics/fv/NonlinearPressureStorage.hpp"

namespace {
using namespace fvtest;
using Solver = openswmm::fv::NonlinearPressureStorage;
using Status = Solver::Status;
namespace k = openswmm::fv::kernels;

struct Problem {
    FvGeometry geometry;
    std::vector<Solver::Row> rows;
    std::vector<Solver::Edge> edges;
    explicit Problem(double c = 300.0) {
        buildGeometry(circular(3.0), false, c, geometry);
    }
    void cell(double old, double length = 25.0, double bed = 0.0) {
        rows.push_back({&geometry, length, bed, old, 0.0, bed, bed + 100.0});
        rows.back().old_area = k::areaOfDepth(geometry, old - bed);
    }
    void node(double old) { rows.push_back({nullptr, 0.0, 0.0, old, 0.0, 0.0, 100.0}); }
    // Independently assemble the prescribed sources in extended precision.
    // Closure calls are shared, but the graph balance assembly is separate.
    void manufacture(const std::vector<double>& target, double dt) {
        std::vector<long double> source(rows.size(), 0.0L);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (row.geometry)
                source[i] = static_cast<long double>(row.length) *
                    (static_cast<long double>(k::areaOfDepth(*row.geometry, target[i] - row.bed)) -
                     row.old_area) / dt;
        }
        for (const auto& edge : edges) {
            const long double neighbor = edge.right < 0 ? edge.boundary_head : target[edge.right];
            const long double q = static_cast<long double>(edge.conductance) *
                                  (static_cast<long double>(target[edge.left]) - neighbor);
            source[edge.left] += q;
            if (edge.right >= 0) source[edge.right] -= q;
        }
        for (std::size_t i = 0; i < rows.size(); ++i) rows[i].source = static_cast<double>(source[i]);
    }
    long double residual(std::size_t i, const std::vector<double>& h, double dt) const {
        const auto& row = rows[i];
        long double r = -static_cast<long double>(dt) * row.source;
        if (row.geometry)
            r += static_cast<long double>(row.length) *
                (static_cast<long double>(k::areaOfDepth(*row.geometry, h[i] - row.bed)) - row.old_area);
        for (const auto& edge : edges) {
            const long double other = edge.right < 0 ? edge.boundary_head : h[edge.right];
            const long double v = static_cast<long double>(dt) * edge.conductance *
                                 (static_cast<long double>(h[edge.left]) - other);
            if (edge.left == static_cast<int>(i)) r += v;
            if (edge.right == static_cast<int>(i)) r -= v;
        }
        return r;
    }
    void check(const std::vector<double>& heads, double dt, const Solver::Controls& c) const {
        ASSERT_EQ(heads.size(), rows.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            const double old = row.geometry ? row.length * row.old_area : 0.0;
            const double budget = c.absolute_volume + c.relative_volume *
                std::max({1.0, old, dt * std::fabs(row.source)});
            EXPECT_LE(std::fabs(residual(i, heads, dt)), budget * 1.01) << i;
            EXPECT_GE(heads[i], row.lower_head);
            EXPECT_LE(heads[i], row.upper_head);
        }
    }
};

// Test-only dense Gaussian elimination, separate from the candidate's PCG.
std::vector<double> denseSolve(std::vector<std::vector<long double>> a,
                               std::vector<long double> b) {
    const int n = static_cast<int>(b.size());
    for (int j = 0; j < n; ++j) {
        int pivot = j;
        for (int i = j + 1; i < n; ++i)
            if (std::fabs(a[i][j]) > std::fabs(a[pivot][j])) pivot = i;
        std::swap(a[j], a[pivot]); std::swap(b[j], b[pivot]);
        EXPECT_GT(std::fabs(a[j][j]), 0.0L);
        for (int i = j + 1; i < n; ++i) {
            const long double factor = a[i][j] / a[j][j];
            for (int col = j + 1; col < n; ++col) a[i][col] -= factor * a[j][col];
            b[i] -= factor * b[j];
        }
    }
    std::vector<double> x(n);
    for (int i = n - 1; i >= 0; --i) {
        long double sum = b[i];
        for (int j = i + 1; j < n; ++j) sum -= a[i][j] * x[j];
        x[i] = static_cast<double>(sum / a[i][i]);
    }
    return x;
}

void expectFailureUnchanged(Problem& problem, double dt, Solver::Controls c, Status expected) {
    Solver solver;
    const std::vector<double> before{123.0, -456.0, 789.0};
    auto output = before;
    const auto result = solver.solve(problem.rows, problem.edges, dt, c, output);
    EXPECT_EQ(result.status, expected);
    ASSERT_EQ(output.size(), before.size());
    EXPECT_EQ(std::memcmp(output.data(), before.data(), before.size() * sizeof(double)), 0);
}
} // namespace

TEST(FvNonlinearPressureStorage, CrownCrossingUsesActualAreaInBothDirections) {
    Solver solver;
    Solver::Controls c;
    int solves = 0, max_newton = 0, max_linear = 0, total_backtracks = 0;
    for (double celerity : {50.0, 100.0, 300.0, 1000.0, 4000.0}) {
        Problem p(celerity);
        for (bool filling : {false, true}) {
            for (double dt : {0.01, 1.0, 30.0, 1000.0}) {
                SCOPED_TRACE(::testing::Message() << celerity << ", fill=" << filling << ", dt=" << dt);
                p.rows.clear();
                p.cell(filling ? 2.98 : 3.5);
                const double target = filling ? 3.5 : 2.98;
                p.manufacture({target}, dt);
                std::vector<double> heads;
                const auto result = solver.solve(p.rows, p.edges, dt, c, heads);
                ASSERT_TRUE(result.converged()) << static_cast<int>(result.status);
                ++solves;
                max_newton = std::max(max_newton, result.newton_iterations);
                max_linear = std::max(max_linear, result.linear_iterations);
                total_backtracks += result.backtracks;
                ASSERT_EQ(heads.size(), 1u);
                p.check(heads, dt, c);
                const double old_volume = 25.0 * k::areaOfDepth(p.geometry, p.rows[0].old_head);
                const double volume_budget = c.absolute_volume + c.relative_volume * std::max(1.0, old_volume);
                EXPECT_NEAR(heads[0], target, 2.0 * volume_budget / (25.0 * p.geometry.t_slot));
                EXPECT_LE(result.scaled_residual, 1.0);
            }
        }
    }
    EXPECT_EQ(solves, 40);
    EXPECT_GT(total_backtracks, 0); // actually exercises safeguarded steps
    RecordProperty("manufactured_solves", solves);
    RecordProperty("maximum_newton_iterations", max_newton);
    RecordProperty("maximum_total_linear_iterations", max_linear);
    RecordProperty("total_backtracks", total_backtracks);
}

TEST(FvNonlinearPressureStorage, LoopedCrownSystemRetainsAllCellAndJunctionRows) {
    Solver solver;
    Solver::Controls controls;
    int solves = 0, max_newton = 0, max_linear = 0;
    for (int cells : {4, 8, 16}) {
        for (double celerity : {100.0, 300.0, 1000.0}) {
            SCOPED_TRACE(::testing::Message() << cells << " cells/branch, c=" << celerity);
            Problem p(celerity);
            p.node(3.05); p.node(3.05); // unknown fork and merge, zero storage
            std::vector<double> target{3.08, 3.02};
            for (int branch = 0; branch < 2; ++branch) {
                int previous = 0;
                for (int i = 0; i < cells; ++i) {
                    const int next = static_cast<int>(p.rows.size());
                    p.cell(i % 2 ? 3.1 : 2.98, 200.0 / cells);
                    target.push_back(3.08 - 0.06 * (i + 1.0) / (cells + 1.0));
                    p.edges.push_back({previous, next, 0.1 + 0.03 * i, 0.0});
                    previous = next;
                }
                p.edges.push_back({previous, 1, 0.3, 0.0});
            }
            p.edges.push_back({0, -1, 0.2, 3.2});
            p.edges.push_back({1, -1, 0.2, 3.0});
            p.manufacture(target, 30.0);
            std::vector<double> heads;
            const auto result = solver.solve(p.rows, p.edges, 30.0, controls, heads);
            ASSERT_TRUE(result.converged()) << static_cast<int>(result.status);
            ++solves;
            max_newton = std::max(max_newton, result.newton_iterations);
            max_linear = std::max(max_linear, result.linear_iterations);
            p.check(heads, 30.0, controls);
            ASSERT_EQ(heads.size(), static_cast<std::size_t>(2 * cells + 2));
            for (std::size_t i = 0; i < heads.size(); ++i) EXPECT_NEAR(heads[i], target[i], 1.e-7) << i;
            EXPECT_LE(result.linear_residual, result.linear_tolerance);
        }
    }
    RecordProperty("manufactured_solves", solves);
    RecordProperty("maximum_newton_iterations", max_newton);
    RecordProperty("maximum_total_linear_iterations", max_linear);
}

TEST(FvNonlinearPressureStorage, SlotOnlyCorrectionAgreesWithIndependentDenseOracle) {
    Problem p;
    for (int i = 0; i < 7; ++i) p.cell(5.5, 10.0 + i * 20.0);
    p.node(5.5);
    p.edges = {{0,1,.3,0}, {1,2,.4,0}, {2,3,.2,0}, {3,4,.7,0},
               {4,5,.5,0}, {5,6,.3,0}, {1,7,.4,0}, {7,5,.6,0},
               {0,-1,.4,6.0}, {6,-1,.5,5.0}};
    const double dt = 5.0;
    const int n = static_cast<int>(p.rows.size());
    std::vector<std::vector<long double>> a(n, std::vector<long double>(n, 0.0));
    std::vector<long double> b(n, 0.0);
    for (int i = 0; i < n; ++i)
        a[i][i] = p.rows[i].geometry ? p.rows[i].length * p.geometry.t_slot : 0.0;
    for (const auto& e : p.edges) {
        const long double weight = dt * static_cast<long double>(e.conductance);
        a[e.left][e.left] += weight;
        if (e.right >= 0) {
            a[e.right][e.right] += weight;
            a[e.left][e.right] -= weight; a[e.right][e.left] -= weight;
        } else b[e.left] += weight * (e.boundary_head - 5.5);
    }
    const auto correction = denseSolve(a, b);
    Solver solver;
    Solver::Controls c;
    std::vector<double> heads;
    const auto result = solver.solve(p.rows, p.edges, dt, c, heads);
    ASSERT_TRUE(result.converged()) << static_cast<int>(result.status);
    EXPECT_EQ(result.newton_iterations, 1);
    p.check(heads, dt, c);
    for (int i = 0; i < n; ++i) EXPECT_NEAR(heads[i], 5.5 + correction[i], 1.e-10);
}

TEST(FvNonlinearPressureStorage, ClosedNonlinearComponentConservesVolume) {
    Problem p(1000.0);
    p.cell(2.98, 25); p.cell(3.2, 100); p.node(3.1); p.cell(3.4, 50);
    p.edges = {{0,2,.1,0}, {1,2,.3,0}, {2,3,.2,0}, {0,3,.4,0}};
    const auto rows_before = p.rows;
    const auto edges_before = p.edges;
    Solver solver;
    Solver::Controls c;
    std::vector<double> heads;
    const auto result = solver.solve(p.rows, p.edges, 10.0, c, heads);
    ASSERT_TRUE(result.converged()) << static_cast<int>(result.status);
    p.check(heads, 10.0, c);
    long double volume_change = 0.0;
    for (std::size_t i = 0; i < p.rows.size(); ++i) {
        const auto& row = p.rows[i];
        if (row.geometry)
            volume_change += row.length * (static_cast<long double>(k::areaOfDepth(p.geometry, heads[i])) -
                                           row.old_area);
        EXPECT_EQ(row.old_head, rows_before[i].old_head);
        EXPECT_EQ(row.source, rows_before[i].source);
        EXPECT_EQ(row.length, rows_before[i].length);
        EXPECT_EQ(row.old_area, rows_before[i].old_area);
    }
    EXPECT_LE(std::fabs(volume_change), 1.e-8L);
    for (std::size_t e = 0; e < p.edges.size(); ++e)
        EXPECT_EQ(p.edges[e].conductance, edges_before[e].conductance);
}

TEST(FvNonlinearPressureStorage, JunctionHasNoManufacturedStorage) {
    Problem p;
    p.node(5.0);
    p.edges = {{0,-1,2.0,6.0},{0,-1,1.0,3.0}};
    Solver solver;
    std::vector<double> heads;
    const auto result = solver.solve(p.rows, p.edges, 30.0, {}, heads);
    ASSERT_TRUE(result.converged());
    ASSERT_EQ(heads.size(), 1u);
    EXPECT_NEAR(heads[0], 5.0, 1.e-12);
    EXPECT_EQ(result.newton_iterations, 0);
}

TEST(FvNonlinearPressureStorage, DatumShiftDoesNotChangeCrownStorageSolution) {
    Solver solver;
    std::vector<double> reference;
    for (double datum : {0.0, 1024.0}) {
        Problem p(300.0);
        p.cell(datum + 2.98, 25.0, datum);
        p.cell(datum + 3.2, 50.0, datum);
        p.edges = {{0,1,.2,0},{1,-1,.5,datum+3.1}};
        p.manufacture({datum + 3.05, datum + 3.1}, 5.0);
        std::vector<double> heads;
        const auto result = solver.solve(p.rows, p.edges, 5.0, {}, heads);
        ASSERT_TRUE(result.converged()) << static_cast<int>(result.status);
        if (datum == 0.0) reference = heads;
        else for (std::size_t i = 0; i < heads.size(); ++i)
            EXPECT_NEAR(heads[i] - datum, reference[i], 1.e-9);
    }
}

TEST(FvNonlinearPressureStorage, RestStateIsPreservedAcrossDisconnectedAnchoredComponents) {
    Problem p;
    p.cell(5.5); p.cell(5.5); p.node(5.5); p.cell(6.0);
    p.edges = {{0,1,.2,0},{1,2,.1,0}};
    Solver solver;
    std::vector<double> heads;
    const auto result = solver.solve(p.rows, p.edges, 30.0, {}, heads);
    ASSERT_TRUE(result.converged());
    EXPECT_EQ(heads, (std::vector<double>{5.5,5.5,5.5,6.0}));
    EXPECT_EQ(result.linear_iterations, 0);
}

TEST(FvNonlinearPressureStorage, RejectsUnanchoredComponentsEvenAtZeroResidual) {
    Problem p;
    p.node(5.5); p.node(5.5); p.cell(5.5);
    p.edges = {{0,1,.2,0}};
    expectFailureUnchanged(p, 1.0, {}, Status::UnanchoredComponent);
}

TEST(FvNonlinearPressureStorage, RejectsDisconnectedUnanchoredRow) {
    Problem p;
    p.cell(5.5); p.node(5.5);
    p.edges = {{0,-1,.2,6.0}};
    expectFailureUnchanged(p, 1.0, {}, Status::UnanchoredComponent);
}

TEST(FvNonlinearPressureStorage, NonlinearExhaustionDoesNotPublish) {
    Problem p;
    p.cell(2.98); p.manufacture({3.5}, 1.0);
    Solver::Controls c; c.max_newton = 0;
    expectFailureUnchanged(p, 1.0, c, Status::NonlinearIterationLimit);
}

TEST(FvNonlinearPressureStorage, LinearExhaustionDoesNotPublish) {
    Problem p;
    p.cell(3.5); p.manufacture({4.0}, 1.0);
    Solver::Controls c; c.max_linear = 0;
    expectFailureUnchanged(p, 1.0, c, Status::LinearIterationLimit);
}

TEST(FvNonlinearPressureStorage, InadmissibleRootDoesNotPublishClampedHeads) {
    Problem p;
    p.cell(3.5);
    p.rows[0].upper_head = 3.5;
    p.manufacture({4.0}, 1.0);
    expectFailureUnchanged(p, 1.0, {}, Status::LineSearchFailed);
}

TEST(FvNonlinearPressureStorage, InvalidCoefficientsAndControlsDoNotPublish) {
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
        Problem p; p.cell(5.5);
        p.edges = {{0,-1,invalid,6.0}};
        expectFailureUnchanged(p, 1.0, {}, Status::InvalidInput);
    }
    Problem p; p.cell(5.5);
    p.edges = {{0,9,.2,0}};
    expectFailureUnchanged(p, 1.0, {}, Status::InvalidInput);
    p.edges.clear();
    expectFailureUnchanged(p, 0.0, {}, Status::InvalidInput);
    Solver::Controls c; c.relative_volume = -1.0;
    expectFailureUnchanged(p, 1.0, c, Status::InvalidInput);
}

TEST(FvNonlinearPressureStorage, IntegratedSourceOverflowIsRejected) {
    Problem p; p.cell(5.5);
    p.rows[0].source = std::numeric_limits<double>::max();
    // Valid finite inputs but their integrated source overflows.
    expectFailureUnchanged(p, 2.0, {}, Status::InvalidInput);
}

TEST(FvNonlinearPressureStorage, NonfiniteResidualDoesNotPublish) {
    Problem p; p.cell(5.5);
    p.edges = {{0,-1,10.0,std::numeric_limits<double>::max()}};
    // Each input and dt*C is finite; the actual flux product overflows.
    expectFailureUnchanged(p, 1.0, {}, Status::NonFinite);
}

TEST(FvNonlinearPressureStorage, LineSearchExhaustionDoesNotPublish) {
    Problem p; p.cell(3.5); p.manufacture({2.98}, 1.0);
    Solver::Controls c; c.max_backtracks = 0;
    expectFailureUnchanged(p, 1.0, c, Status::LineSearchFailed);
}

TEST(FvNonlinearPressureStorage, ReversingInternalEdgeOrientationPreservesTheSolution) {
    Problem p; p.cell(2.98); p.cell(3.5, 100.0); p.node(3.1);
    p.edges = {{0,1,.2,0},{1,2,.3,0},{2,0,.4,0},{2,-1,.5,3.1}};
    p.manufacture({3.05,3.1,3.08}, 5.0);
    Solver solver;
    std::vector<double> heads, reversed;
    ASSERT_TRUE(solver.solve(p.rows, p.edges, 5.0, {}, heads).converged());
    for (auto& edge : p.edges) if (edge.right >= 0) std::swap(edge.left, edge.right);
    ASSERT_TRUE(solver.solve(p.rows, p.edges, 5.0, {}, reversed).converged());
    ASSERT_EQ(heads.size(), reversed.size());
    for (std::size_t i = 0; i < heads.size(); ++i) EXPECT_NEAR(heads[i], reversed[i], 1.e-10);
}

TEST(FvNonlinearPressureStorage, ConservedOldAreaIsNotReconstructedFromInitialHead) {
    Problem p;
    p.cell(5.5);
    // Deliberately separate the conserved state from the initial head guess.
    // A future adapter must pass cell_a rather than round-trip its inverse.
    p.rows[0].old_area = k::areaOfDepth(p.geometry, 5.75);
    Solver solver;
    std::vector<double> heads;
    const auto result = solver.solve(p.rows, p.edges, 1.0, {}, heads);
    ASSERT_TRUE(result.converged());
    ASSERT_EQ(heads.size(), 1u);
    EXPECT_NEAR(heads[0], 5.75, 1.e-8);
    p.check(heads, 1.0, {});
}
