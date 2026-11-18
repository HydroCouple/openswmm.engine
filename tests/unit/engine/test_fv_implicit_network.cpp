// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      test_fv_implicit_network.cpp
 * @brief     Tests for fv implicit network.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <numeric>
#include "../../verification/fv_implicit_phase1/network_cases.hpp"
#include "hydraulics/fv/ImplicitReachSolver.hpp"

namespace {
using namespace fvnetwork;
/**
 * @brief Solve a CSR network linearization through independent dense reference elimination.
 *
 * Copies CSR values into a dense augmented matrix, uses partial pivoting and back substitution.
 * Duplicate CSR columns overwrite rather than sum; assumes canonical fixture CSR. Does not mutate
 * inputs, check finite values/conditioning or call production linear solvers.
 *
 * @param[in] m Valid square CSR matrix compatible with rhs, with valid column indices.
 * @param[in] rhs Right-hand side; dimension must fit int and available dense storage.
 * @return Owned double-converted solution, empty when a selected long-double pivot magnitude is
 *     below 1e-25 or dimension is zero.
 */
std::vector<double> dense(const S::Matrix& m, const std::vector<double>& rhs) {
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
        if (std::fabs(a[c][c]) < 1.e-25)
            return {};
        for (int r = c + 1; r < n; ++r) {
            const auto factor = a[r][c] / a[c][c];
            for (int k = c; k <= n; ++k)
                a[r][k] -= factor * a[c][k];
        }
    }
    std::vector<long double> x(n);
    for (int r = n; r-- > 0;) {
        auto sum = a[r][n];
        for (int c = r + 1; c < n; ++c)
            sum -= a[r][c] * x[c];
        x[r] = sum / a[r][r];
    }
    return {x.begin(), x.end()};
}
/**
 * @brief Pack native residual rows in the network linearization ordering.
 *
 * No size/index validation or equation-unit normalization occurs; inputs are unchanged.
 *
 * @param[in] e Evaluation with momentum at least as long as volume and junction entries covering
 *     l.nodes.
 * @param[in] l Linearization whose nodes list supplies valid ordered junction indices.
 * @return Owned vector interleaving cell volume/momentum residuals, followed by e.junction[j] for
 *     listed nodes, retaining native row scaling.
 */
std::vector<double> rows(const S::Evaluation& e, const S::Linearization& l) {
    std::vector<double> r;
    for (std::size_t c = 0; c < e.volume.size(); ++c) {
        r.push_back(e.volume[c]);
        r.push_back(e.momentum[c]);
    }
    for (int j : l.nodes)
        r.push_back(e.junction[j]);
    return r;
}
/**
 * @brief Build a synthetic network endpoint for failed-operation preservation checks.
 *
 * Markers deliberately do not form a physical hydraulic state.
 * @return Owned depth{-1},area{-2},discharge{-3},head{-4} and one face with flux.mass=-5; other
 *     fields default.
 */
S::Endpoint sentinel() {
    S::Endpoint e;
    e.depth = {-1};
    e.area = {-2};
    e.discharge = {-3};
    e.head = {-4};
    e.faces.resize(1);
    e.faces[0].flux.mass = -5;
    return e;
}
/**
 * @brief Assert selected network endpoint markers survived a refused solve.
 *
 * Compares vectors and mass marker exactly, with fatal one-face size assertion before indexing.
 * Does not inspect every endpoint/flux field or mutate the object.
 *
 * @param[in] e Endpoint expected to retain sentinel() fields.
 */
void unchanged(const S::Endpoint& e) {
    EXPECT_EQ(e.depth, (std::vector<double>{-1}));
    EXPECT_EQ(e.area, (std::vector<double>{-2}));
    EXPECT_EQ(e.discharge, (std::vector<double>{-3}));
    EXPECT_EQ(e.head, (std::vector<double>{-4}));
    ASSERT_EQ(e.faces.size(), 1u);
    EXPECT_EQ(e.faces[0].flux.mass, -5);
}
/**
 * @brief Flip stored chain and incident-face directions over one cell-index interval.
 *
 * Negates chain_dir entries in[begin,end) and each face side whose incident cell lies in that
 * interval. Assumes fixture chain positions equal the selected cell-index range. Does not reverse
 * connectivity, change discharges/geometry or validate bounds.
 *
 * @param[in,out] m Consistent mesh with chain_dir index-compatible with the intended cell ordering
 *     and valid face-direction arrays.
 * @param[in] begin Valid nonnegative inclusive cell/range start.
 * @param[in] end Exclusive end at or after begin and within chain_dir storage.
 */
void flipAxis(NetworkMeshData& m, int begin, int end) {
    for (int i = begin; i < end; ++i)
        m.chain_dir[i] *= -1;
    for (int f = 0; f < m.n_faces(); ++f) {
        if (m.face_cl[f] >= begin && m.face_cl[f] < end)
            m.face_dir_l[f] *= -1;
        if (m.face_cr[f] >= begin && m.face_cr[f] < end)
            m.face_dir_r[f] *= -1;
    }
}
}

TEST(FvImplicitNetwork, LakeAtRestBalancesBedSectionBarrelsVirtualAndBranches) {
    for (const std::string name : {"bed", "geometry", "virtual", "tee", "loop", "reversed"})
        for (double c : {150.0, 660.0}) {
            SCOPED_TRACE(name + std::to_string(c));
            auto ch = make(name, 16, c);
            auto p = problem(ch, name);
            auto x = trial(p);
            S::Evaluation e;
            ASSERT_TRUE(S::residual(p, x, 10.0, e));
            for (double v : e.volume)
                EXPECT_NEAR(v, 0.0, 1.e-10);
            for (double v : e.momentum)
                EXPECT_NEAR(v, 0.0, 1.e-10);
            for (double v : e.junction)
                EXPECT_NEAR(v, 0.0, 1.e-12);
            S::Endpoint endpoint;
            S solver;
            const auto result = solver.solve(p, 10.0, {}, endpoint);
            ASSERT_TRUE(result.converged())
                << static_cast<int>(result.status) << ' ' << result.scaled_residual;
            EXPECT_EQ(endpoint.area, p.old_area);
            for (double q : endpoint.discharge)
                EXPECT_NEAR(q, 0.0, 2.e-12);
        }
}

TEST(FvImplicitNetwork, BalancedSpatialOperatorMatchesExistingExplicitFirstOrder) {
    for (const std::string name : {"bed", "geometry", "virtual", "loop"}) {
        SCOPED_TRACE(name);
        auto ch = make(name, 24, 150);
        auto p = problem(ch, name, true);
        auto x = trial(p);
        for (int i = 0; i < ch.n; ++i)
            x.discharge[i] = p.old_discharge[i] = .1 * std::cos(.2 * i);
        if (name == "loop") {
            // Freeze all node heads to compare the spatial operator alone,
            // separately from the explicit solver's algebraic relaxation.
            for (int j = 0; j < ch.mesh.n_nodes(); ++j)
                p.fixed_head[j] = x.head[j];
        }
        ch.state.cell_a = p.old_area;
        ch.state.cell_q = p.old_discharge;
        ch.state.node_head = p.initial_head;
        auto opts = defaultOptions();
        opts.lts = false;
        opts.pressurized_implicit = false;
        ExplicitFvSolver explicit_solver;
        explicit_solver.initialize(ch.mesh, ch.state, opts);
        constexpr double dt = 1.e-7;
        S::Evaluation e;
        ASSERT_TRUE(S::residual(p, x, dt, e));
        FvStepForcing forcing;
        if (!p.fixed_head.empty())
            forcing.node_fixed_head = p.fixed_head.data();
        ASSERT_EQ(explicit_solver.advance(0, dt, forcing), dt);
        for (int i = 0; i < ch.n; ++i) {
            EXPECT_NEAR(ch.state.cell_a[i], p.old_area[i] - e.volume[i] / ch.mesh.cell_dx[i],
                        2.e-13);
            EXPECT_NEAR(ch.state.cell_q[i], p.old_discharge[i] - e.momentum[i] / ch.mesh.cell_dx[i],
                        3.e-11);
        }
    }
}

TEST(FvImplicitNetwork, JunctionInitializationChangesOnlyHeadsAndCountsLateralOnce) {
    auto ch = make("tee_source", 8, 660);
    auto p = problem(ch, "tee_source", true);
    auto x = trial(p);
    x.head[1] = 20.0;
    const auto saved = x;
    ASSERT_TRUE(S::initializeHeads(p, x, 1.e-12));
    EXPECT_EQ(x.depth, saved.depth);
    EXPECT_EQ(x.discharge, saved.discharge);
    EXPECT_NE(x.head[1], saved.head[1]);
    S::Evaluation e;
    ASSERT_TRUE(S::residual(p, x, .2, e));
    EXPECT_NEAR(e.junction[1], 0.0, 1.e-12);
    double inflow = 0.0;
    for (int t = ch.mesh.node_face_ptr[1]; t < ch.mesh.node_face_ptr[2]; ++t)
        inflow += ch.mesh.node_face_sign[t] * e.faces[ch.mesh.node_face_idx[t]].flux.mass;
    EXPECT_NEAR(inflow, -.1, 1.e-12);
    auto impossible = p;
    impossible.node_lateral[1] = 1.e20;
    EXPECT_FALSE(S::initializeHeads(impossible, x, 1.e-12));
    S::Evaluation after;
    ASSERT_TRUE(S::residual(p, x, .2, after));
    EXPECT_NEAR(after.junction[1], 0.0, 1.e-12);
}

TEST(FvImplicitNetwork, InventoryIdentityIncludesCellAndNodeSourcesWithOneFaceReceipt) {
    auto ch = make("tee_source", 8, 150);
    auto p = problem(ch, "tee_source", true);
    auto x = trial(p);
    x.head[1] = 6.1;
    S::Evaluation e;
    constexpr double dt = .3;
    ASSERT_TRUE(S::residual(p, x, dt, e));
    double external_out = 0.0;
    for (int f = 0; f < ch.mesh.n_faces(); ++f) {
        const int j = ch.mesh.face_node[f];
        if (j >= 0 && std::isfinite(p.fixed_head[j]))
            external_out += (ch.mesh.face_cl[f] >= 0 ? 1 : -1) * e.faces[f].flux.mass;
    }
    const double summed =
        std::accumulate(e.volume.begin(), e.volume.end(), 0.0) - dt * e.junction[1];
    EXPECT_NEAR(summed, dt * (external_out - .12), 2.e-12);
}

TEST(FvImplicitNetwork, NodeAreaCannotCreateArtificialStorageOrHeadHistoryTerms) {
    auto ch = make("tee", 8, 150);
    auto p = problem(ch, "tee", true);
    auto x = trial(p);
    S::Evaluation a, b;
    ASSERT_TRUE(S::residual(p, x, .5, a));
    ch.mesh.node_area.assign(ch.mesh.n_nodes(), 1.e20);
    p.initial_head[1] = 70;
    ASSERT_TRUE(S::residual(p, x, .5, b));
    EXPECT_EQ(a.volume, b.volume);
    EXPECT_EQ(a.momentum, b.momentum);
    EXPECT_EQ(a.junction, b.junction);
}

TEST(FvImplicitNetwork, AxisAndFaceOrientationCovariance) {
    auto ch = make("tee", 4, 150);
    auto p = problem(ch, "tee", true);
    auto x = trial(p);
    for (int i = 0; i < ch.n; ++i)
        x.discharge[i] = p.old_discharge[i] = .2 + .03 * std::sin(i);
    S::Evaluation a, b;
    ASSERT_TRUE(S::residual(p, x, .5, a));
    flipAxis(ch.mesh, 4, 8);
    for (int i = 4; i < 8; ++i) {
        x.discharge[i] *= -1;
        p.old_discharge[i] *= -1;
    }
    ASSERT_TRUE(S::residual(p, x, .5, b));
    EXPECT_EQ(a.volume, b.volume);
    EXPECT_EQ(a.junction, b.junction);
    for (int i = 0; i < ch.n; ++i)
        EXPECT_NEAR(b.momentum[i], (i >= 4 && i < 8 ? -1 : 1) * a.momentum[i], 1.e-12);
    // Reverse the oriented L->R face frame itself, including node CSR sign.
    auto& m = ch.mesh;
    for (int f = 0; f < m.n_faces(); ++f) {
        const int dl = m.face_dir_l[f];
        std::swap(m.face_cl[f], m.face_cr[f]);
        m.face_dir_l[f] = -m.face_dir_r[f];
        m.face_dir_r[f] = -dl;
    }
    for (auto& s : m.cell_side0)
        s = 1 - s;
    for (auto& s : m.cell_side1)
        s = 1 - s;
    for (auto& s : m.node_face_sign)
        s = -s;
    S::Evaluation reversed;
    ASSERT_TRUE(S::residual(p, x, .5, reversed));
    EXPECT_EQ(b.volume, reversed.volume);
    EXPECT_EQ(b.momentum, reversed.momentum);
    EXPECT_EQ(b.junction, reversed.junction);
    for (int f = 0; f < m.n_faces(); ++f)
        EXPECT_EQ(b.faces[f].flux.mass, -reversed.faces[f].flux.mass);
}

TEST(FvImplicitNetwork, SparseJacobianMatchesIndependentGlobalDirectionalDifferences) {
    for (const std::string name : {"geometry", "tee_source", "loop", "reversed"}) {
        auto ch = make(name, 4, 660);
        auto p = problem(ch, name, true);
        auto x = trial(p);
        for (int i = 0; i < ch.n; ++i)
            x.discharge[i] = .3 + .05 * std::cos(i);
        for (double step : {1.e-3, 1.e-4, 1.e-5}) {
            SCOPED_TRACE(name + std::to_string(step));
            S::Linearization l;
            ASSERT_TRUE(S::linearize(p, x, .7, step, l));
            const auto n = l.residual.size();
            std::vector<double> d(n), jv(n);
            for (std::size_t i = 0; i < n; ++i)
                d[i] = std::sin(.37 * i + .2);
            ASSERT_TRUE(l.matrix.apply(d.data(), jv.data()));
            auto plus = x, minus = x;
            constexpr double epsilon = 1.e-4;
            for (int c = 0; c < ch.n; ++c) {
                plus.depth[c] += epsilon * d[2 * c];
                minus.depth[c] -= epsilon * d[2 * c];
                plus.discharge[c] += epsilon * l.column_scale[2 * c + 1] * d[2 * c + 1];
                minus.discharge[c] -= epsilon * l.column_scale[2 * c + 1] * d[2 * c + 1];
            }
            for (std::size_t i = 0; i < l.nodes.size(); ++i) {
                plus.head[l.nodes[i]] += epsilon * d[2 * ch.n + i];
                minus.head[l.nodes[i]] -= epsilon * d[2 * ch.n + i];
            }
            S::Evaluation ep, em;
            ASSERT_TRUE(S::residual(p, plus, .7, ep));
            ASSERT_TRUE(S::residual(p, minus, .7, em));
            const auto rp = rows(ep, l), rm = rows(em, l);
            double error = 0, norm = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const double fd = (rp[i] - rm[i]) / (2 * epsilon * l.row_scale[i]);
                error = std::hypot(error, jv[i] - fd);
                norm = std::hypot(norm, fd);
            }
            EXPECT_LT(error / norm, 2.e-5);
        }
    }
}

TEST(FvImplicitNetwork, NativeIluGmresMatchesDenseCorrectionsOnBranchesAndLoops) {
    for (const std::string name : {"geometry", "tee_source", "loop", "reversed"}) {
        auto ch = make(name, 4, 660);
        auto p = problem(ch, name, true);
        auto x = trial(p);
        S::Linearization l;
        ASSERT_TRUE(S::linearize(p, x, 1.0, 1.e-4, l));
        auto rhs = l.residual;
        for (auto& v : rhs)
            v = -v;
        const auto reference = dense(l.matrix, rhs);
        ASSERT_EQ(reference.size(), rhs.size());
        for (int restart : {20, 40, 80}) {
            SCOPED_TRACE(name + std::to_string(restart));
            NativeGmres::Controls controls;
            controls.restart = restart;
            controls.max_iterations = 500;
            controls.absolute_tolerance = 1.e-14;
            controls.relative_tolerance = 1.e-11;
            std::vector<double> correction;
            const auto result = S::solveLinear(l.matrix, rhs, controls, correction);
            ASSERT_TRUE(result.gmres.converged())
                << static_cast<int>(result.gmres.status) << ' ' << result.gmres.true_residual;
            for (std::size_t i = 0; i < rhs.size(); ++i)
                EXPECT_NEAR(correction[i], reference[i], 2.e-9);
        }
    }
}

TEST(FvImplicitNetwork, FlatReachCorrectionsMatchNativeBlockDirectReference) {
    auto ch = make("flat", 12, 660);
    auto p = problem(ch, "flat", true);
    auto x = trial(p);
    ImplicitReachSolver::Problem rp{&ch.mesh, p.old_area, p.old_discharge, nan(), nan(), 3.01, 100};
    std::vector<ImplicitReachSolver::Pair> rx;
    for (int i = 0; i < ch.n; ++i)
        rx.push_back({x.depth[i], x.discharge[i]});
    ImplicitReachSolver::Linearization rl;
    S::Linearization l;
    ASSERT_TRUE(S::linearize(p, x, .3, 1.e-4, l));
    ASSERT_TRUE(ImplicitReachSolver::linearize(rp, rx, .3, 1.e-4, rl));
    auto brhs = rl.residual;
    for (auto& v : brhs) {
        v[0] = -v[0];
        v[1] = -v[1];
    }
    std::vector<ImplicitReachSolver::Pair> direct;
    double relative;
    ASSERT_TRUE(ImplicitReachSolver::solveBlocks(rl, brhs, direct, relative));
    auto rhs = l.residual;
    for (auto& v : rhs)
        v = -v;
    std::vector<double> answer;
    NativeGmres::Controls controls;
    controls.absolute_tolerance = 1.e-14;
    ASSERT_TRUE(S::solveLinear(l.matrix, rhs, controls, answer).gmres.converged());
    for (int i = 0; i < ch.n; ++i)
        for (int k = 0; k < 2; ++k)
            EXPECT_NEAR(direct[i][k] * rl.column_scale[k],
                        answer[2 * i + k] * l.column_scale[2 * i + k], 2.e-10);
}

TEST(FvImplicitNetwork, NonlinearEndpointsConserveAndSolveOriginalBranchAndLoopRows) {
    for (const std::string name :
         {"bed", "geometry", "virtual", "tee_source", "loop", "reversed"}) {
        SCOPED_TRACE(name);
        auto ch = make(name, 8, 660);
        auto p = problem(ch, name, true);
        S solver;
        S::Endpoint endpoint;
        const auto result = solver.solve(p, .25, {}, endpoint);
        ASSERT_TRUE(result.converged())
            << static_cast<int>(result.status) << ' ' << result.scaled_residual << ' '
            << static_cast<int>(result.last_linear.gmres.status);
        S::Evaluation e;
        ASSERT_TRUE(S::residual(p, endpoint, .25, e));
        EXPECT_LE(result.scaled_residual, 1.0);
        double volume = 0, external = 0, source = 0;
        for (int i = 0; i < ch.n; ++i)
            volume += ch.mesh.cell_dx[i] * (endpoint.area[i] - p.old_area[i]);
        for (double v : p.node_lateral)
            source += v;
        for (double v : p.cell_lateral)
            source += v;
        for (int f = 0; f < ch.mesh.n_faces(); ++f) {
            const int j = ch.mesh.face_node[f];
            if (j >= 0 && std::isfinite(p.fixed_head[j]))
                external += (ch.mesh.face_cl[f] >= 0 ? 1 : -1) * endpoint.faces[f].flux.mass;
        }
        EXPECT_NEAR(volume + .25 * (external - source), 0.0, 2.e-9);
        for (double v : e.junction)
            EXPECT_NEAR(v, 0.0, 1.e-10);
        EXPECT_EQ(endpoint.faces.size(), ch.mesh.face_cl.size());
    }
}

TEST(FvImplicitNetwork, SmoothMomentumChangeVanishesWithTimestepOnNetwork) {
    auto ch = make("loop", 8, 150);
    auto p = problem(ch, "loop");
    for (int i = 0; i < ch.n; ++i)
        p.old_discharge[i] = .001 * std::cos(.3 * i);
    double previous = 1.e10;
    for (double dt : {1.e-3, 1.e-5, 1.e-7}) {
        S solver;
        S::Endpoint e;
        auto result = solver.solve(p, dt, {}, e);
        ASSERT_TRUE(result.converged()) << static_cast<int>(result.status);
        double change = 0;
        for (int i = 0; i < ch.n; ++i)
            change = std::hypot(change, e.discharge[i] - p.old_discharge[i]);
        EXPECT_LT(change, previous * .02);
        previous = change;
    }
}

TEST(FvImplicitNetwork, ShiftedPreconditionerCannotAcceptAnIncompatibleOriginalMatrix) {
    S::Matrix a{{0, 2, 4}, {0, 1, 0, 1}, {0, 1, 1, 0}};
    std::vector<double> x;
    auto r = S::solveLinear(a, {2, 1}, {}, x);
    ASSERT_TRUE(r.gmres.converged());
    EXPECT_GT(r.shifted_pivots, 0);
    EXPECT_NEAR(x[0], 1, 1.e-9);
    EXPECT_NEAR(x[1], 2, 1.e-9);
    a.value.assign(4, 0.0);
    x = {-7};
    r = S::solveLinear(a, {1, 1}, {}, x);
    EXPECT_FALSE(r.gmres.converged());
    EXPECT_EQ(x, (std::vector<double>{-7}));
    NativeGmres::Controls controls;
    controls.workspace_limit_bytes = 1;
    EXPECT_EQ(S::solveLinear(a, {1, 1}, controls, x).gmres.status,
              NativeGmres::Status::MemoryLimit);
    EXPECT_EQ(x, (std::vector<double>{-7}));
    a.row = {0, 4, 3};
    EXPECT_FALSE(S::solveLinear(a, {1, 1}, {}, x).gmres.converged());
}

TEST(FvImplicitNetwork, UnsupportedPhysicsAndFailedStepsNeverPublishEndpoint) {
    auto ch = make("tee_source", 4, 150);
    auto p = problem(ch, "tee_source", true);
    S solver;
    auto e = sentinel();
    auto limited = S::Controls{};
    limited.max_newton = 0;
    EXPECT_FALSE(solver.solve(p, 1, limited, e).converged());
    unchanged(e);
    limited = {};
    limited.linear.max_iterations = 0;
    EXPECT_FALSE(solver.solve(p, 1, limited, e).converged());
    unchanged(e);
    ch.mesh.node_kind[1] = kNodeStorage;
    EXPECT_EQ(solver.solve(p, 1, {}, e).status, S::Status::InvalidInput);
    unchanged(e);
    ch.mesh.node_kind[1] = kNodeJunction;
    ch.mesh.conduit_loss_inlet[0] = .1;
    EXPECT_EQ(solver.solve(p, 1, {}, e).status, S::Status::InvalidInput);
    unchanged(e);
    ch.mesh.conduit_loss_inlet[0] = 0;
    ch.mesh.node_face_sign[0] *= -1;
    EXPECT_EQ(solver.solve(p, 1, {}, e).status, S::Status::InvalidInput);
    unchanged(e);
    auto virtual_ch = make("virtual", 8, 150);
    auto vp = problem(virtual_ch, "virtual");
    vp.node_lateral = {1};
    EXPECT_EQ(solver.solve(vp, 1, {}, e).status, S::Status::InvalidInput);
    unchanged(e);
    vp.node_lateral.clear();
    vp.old_area[0] = kernels::areaOfDepth(virtual_ch.mesh.geom[0], 2.99);
    EXPECT_EQ(solver.solve(vp, 1, {}, e).status, S::Status::InvalidInput);
    unchanged(e);
}

TEST(FvImplicitNetwork, OrdinaryPressurizedDegreeTwoNodesRetainAlgebraicHeadRows) {
    auto ch = make("loop", 8, 660);
    auto p = problem(ch, "loop", true);
    auto x = trial(p);
    S::Evaluation original, changed;
    ASSERT_TRUE(S::residual(p, x, .25, original));
    x.head[2] += .1;
    ASSERT_TRUE(S::residual(p, x, .25, changed));
    EXPECT_NE(changed.junction[2], original.junction[2]);
    EXPECT_NE(changed.volume, original.volume);
    ASSERT_TRUE(S::initializeHeads(p, x, 1.e-12));
    S::Evaluation initialized;
    ASSERT_TRUE(S::residual(p, x, .25, initialized));
    EXPECT_NEAR(initialized.junction[2], 0.0, 1.e-12);
    EXPECT_NEAR(initialized.junction[3], 0.0, 1.e-12);
    S::Linearization l;
    ASSERT_TRUE(S::linearize(p, x, .25, 1.e-4, l));
    EXPECT_NE(std::find(l.nodes.begin(), l.nodes.end(), 2), l.nodes.end());
    EXPECT_NE(std::find(l.nodes.begin(), l.nodes.end(), 3), l.nodes.end());
    S solver;
    S::Endpoint e;
    ASSERT_TRUE(solver.solve(p, .25, {}, e).converged());
    ASSERT_TRUE(S::residual(p, e, .25, changed));
    for (int j : {1, 2, 3, 4})
        EXPECT_NEAR(changed.junction[j], 0.0, 1.e-10);
}
