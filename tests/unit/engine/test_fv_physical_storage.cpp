// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      test_fv_physical_storage.cpp
 * @brief     Tests for fv physical storage.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <numeric>
#include "../../verification/fv_implicit_phase1/storage_cases.hpp"

namespace {
using namespace fvnetwork;
/**
 * @brief Sum accepted cell and node water represented by a pressure-storage problem.
 *
 * Includes every supplied node-volume entry without filtering outfalls or carry; no
 * shape/finite/sign validation or mutation occurs.
 *
 * @param[in] p Problem with non-null mesh, cell_dx/old_area covering every cell and consistent
 *     old_node_volume values.
 * @return Raw sum of every old node volume plus old_area*cell_dx in ft3.
 */
double inventory(const S::Problem& p) {
    double v = std::accumulate(p.old_node_volume.begin(), p.old_node_volume.end(), 0.0);
    for (int i = 0; i < p.mesh->n_cells(); ++i)
        v += p.mesh->cell_dx[i] * p.old_area[i];
    return v;
}
/**
 * @brief Install a solved endpoint as the next pressure-storage problem's old state.
 *
 * Copies five vectors progressively; allocation failure may leave a prefix replaced. Does not
 * advance an elapsed clock, copy face ledgers, validate acceptance or provide transactional
 * rollback.
 *
 * @param[in,out] p Mutable problem whose mesh/control/forcing identity remains unchanged.
 * @param[in] e Compatible accepted endpoint supplying area/discharge/depth/head/node-volume
 *     vectors.
 */
void advance(S::Problem& p, const S::Endpoint& e) {
    p.old_area = e.area;
    p.old_discharge = e.discharge;
    p.initial_depth = e.depth;
    p.initial_head = e.head;
    p.old_node_volume = e.node_volume;
}
/**
 * @brief Create distinctive endpoint markers for physical-storage refusal tests.
 *
 * Deliberately nonphysical state used only for preservation comparisons.
 * @return Owned depth{-1},discharge{-2},head{-3},area{-4},node_volume{-5}, one face with
 *     flux.mass=-6; unrelated fields default.
 */
S::Endpoint sentinel() {
    S::Endpoint e;
    e.depth = {-1};
    e.discharge = {-2};
    e.head = {-3};
    e.area = {-4};
    e.node_volume = {-5};
    e.faces.resize(1);
    e.faces[0].flux.mass = -6;
    return e;
}
/**
 * @brief Assert selected physical-storage endpoint markers remain unchanged.
 *
 * Uses exact GoogleTest comparisons with a fatal one-face count assertion before indexing. Does not
 * compare every flux/state field or mutate the endpoint.
 *
 * @param[in] e Endpoint expected to retain sentinel() arrays/face marker.
 */
void unchanged(const S::Endpoint& e) {
    EXPECT_EQ(e.depth, (std::vector<double>{-1}));
    EXPECT_EQ(e.discharge, (std::vector<double>{-2}));
    EXPECT_EQ(e.head, (std::vector<double>{-3}));
    EXPECT_EQ(e.area, (std::vector<double>{-4}));
    EXPECT_EQ(e.node_volume, (std::vector<double>{-5}));
    ASSERT_EQ(e.faces.size(), 1u);
    EXPECT_EQ(e.faces[0].flux.mass, -6);
}
/**
 * @brief Pack cell residual pairs then storage/junction residuals in linearization order.
 *
 * No dimension/index validation, unit normalization or state mutation occurs; allocation errors
 * propagate.
 *
 * @param[in] p Problem with valid mesh node-kind indices for every l.nodes entry.
 * @param[in] e Evaluation with matching volume/momentum cells and storage/junction node arrays.
 * @param[in] l Linearization whose nodes lists valid ordered active nodes.
 * @return Owned vector of interleaved volume/momentum residuals followed by storage residual for
 *     kNodeStorage or junction residual otherwise, retaining native row scaling.
 */
std::vector<double> rows(const S::Problem& p, const S::Evaluation& e, const S::Linearization& l) {
    std::vector<double> r;
    for (std::size_t c = 0; c < e.volume.size(); ++c) {
        r.push_back(e.volume[c]);
        r.push_back(e.momentum[c]);
    }
    for (int j : l.nodes)
        r.push_back(p.mesh->node_kind[j] == kNodeStorage ? e.storage[j] : e.junction[j]);
    return r;
}
}

TEST(FvPhysicalStorage, RestPreservesEveryCellAndPhysicalVolume) {
    for (const std::string name :
         {"storage_tee", "storage_loop", "storage_reversed", "storage_closed"}) {
        auto ch = makeStorage(name);
        auto p = storageProblem(ch, name);
        S solver;
        S::Endpoint e;
        const auto r = solver.solve(p, 10, {}, e);
        ASSERT_TRUE(r.converged())
            << name << ' ' << static_cast<int>(r.status) << ' ' << r.scaled_residual;
        EXPECT_EQ(e.depth, p.initial_depth);
        EXPECT_EQ(e.area, p.old_area);
        EXPECT_EQ(e.node_volume, p.old_node_volume);
        EXPECT_EQ(e.head, p.initial_head);
        EXPECT_EQ(e.depth.size(), static_cast<std::size_t>(ch.n));
        EXPECT_EQ(r.max_storage_residual, 0.0);
    }
}

TEST(FvPhysicalStorage, InitializationSkipsStorageWhileBalancingOrdinaryHeads) {
    auto ch = makeStorage("storage_loop");
    auto p = storageProblem(ch, "storage_loop", true);
    auto x = trial(p);
    const auto old = x;
    ASSERT_TRUE(S::initializeHeads(p, x, 1.e-12));
    S::Evaluation e;
    ASSERT_TRUE(S::residual(p, x, .25, e));
    for (int j = 0; j < ch.mesh.n_nodes(); ++j) {
        if (ch.mesh.node_kind[j] == kNodeStorage) {
            EXPECT_EQ(x.head[j], old.head[j]);
            EXPECT_EQ(e.node_volume[j], p.old_node_volume[j]);
            EXPECT_NE(e.junction[j], 0.0);
            EXPECT_DOUBLE_EQ(e.storage[j], -.25 * e.junction[j]);
        } else if (ch.mesh.node_kind[j] == kNodeJunction)
            EXPECT_LE(std::fabs(e.junction[j]), 1.e-12);
    }
    EXPECT_EQ(x.depth, old.depth);
    EXPECT_EQ(x.discharge, old.discharge);
}

TEST(FvPhysicalStorage, VolumeDifferenceCrossesTableIntervalsWithoutFrozenArea) {
    auto ch = makeStorage("storage_tee");
    auto p = storageProblem(ch, "storage_tee");
    auto x = trial(p);
    p.initial_head[1] = 5.9;
    p.old_node_volume[1] = curveVolume(ch.mesh, 1, 5.9);
    x.head[1] = 6.2;
    S::Evaluation e;
    ASSERT_TRUE(S::residual(p, x, .25, e));
    const double actual = curveVolume(ch.mesh, 1, 6.2) - curveVolume(ch.mesh, 1, 5.9);
    EXPECT_DOUBLE_EQ(e.storage[1], actual - .25 * e.junction[1]);
    const double frozen = (3 + 2 * .3 * 5.9) * .3;
    EXPECT_GT(std::fabs(actual - frozen), .02);
    EXPECT_EQ(p.initial_head[1], 5.9);
    EXPECT_EQ(p.old_node_volume[1], curveVolume(ch.mesh, 1, 5.9));
}

TEST(FvPhysicalStorage, StorageAndFlowJacobiansMatchIndependentDirectionalDifference) {
    auto ch = makeStorage("storage_loop", 4);
    auto p = storageProblem(ch, "storage_loop", true);
    auto x = trial(p);
    // Away from table knots so the exact piecewise-linear derivative is unique.
    for (int j = 0; j < ch.mesh.n_nodes(); ++j)
        if (ch.mesh.node_kind[j] == kNodeStorage)
            x.head[j] = 6.023;
    S::Linearization l;
    ASSERT_TRUE(S::linearize(p, x, .25, 1.e-4, l));
    std::vector<double> v(l.residual.size()), av(v.size());
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = .1 * std::sin(.37 * i + .2);
    ASSERT_TRUE(l.matrix.apply(v.data(), av.data()));
    // Keep slot area changes resolvable in double precision at c=660 ft/s.
    auto a = x, b = x;
    constexpr double eps = 1.e-3;
    for (int i = 0; i < ch.n; ++i) {
        a.depth[i] += eps * v[2 * i];
        b.depth[i] -= eps * v[2 * i];
        a.discharge[i] += eps * v[2 * i + 1] * l.column_scale[2 * i + 1];
        b.discharge[i] -= eps * v[2 * i + 1] * l.column_scale[2 * i + 1];
    }
    for (std::size_t i = 0; i < l.nodes.size(); ++i) {
        a.head[l.nodes[i]] += eps * v[2 * ch.n + i];
        b.head[l.nodes[i]] -= eps * v[2 * ch.n + i];
    }
    S::Evaluation ea, eb;
    ASSERT_TRUE(S::residual(p, a, .25, ea));
    ASSERT_TRUE(S::residual(p, b, .25, eb));
    const auto ra = rows(p, ea, l), rb = rows(p, eb, l);
    for (std::size_t i = 0; i < v.size(); ++i)
        EXPECT_NEAR(av[i], (ra[i] - rb[i]) / (2 * eps * l.row_scale[i]), 2.e-7) << i;
}

TEST(FvPhysicalStorage, SharedReceiptsCancelAcrossCellsStorageAndAlgebraicNodes) {
    auto ch = makeStorage("storage_closed");
    auto p = storageProblem(ch, "storage_closed", true);
    auto x = trial(p);
    x.head[1] += .2;
    S::Evaluation e;
    constexpr double dt = .25;
    ASSERT_TRUE(S::residual(p, x, dt, e));
    double expected = 0.0, residual = std::accumulate(e.volume.begin(), e.volume.end(), 0.0);
    for (int j = 0; j < ch.mesh.n_nodes(); ++j) {
        residual += e.storage[j];
        expected += e.node_volume[j] - p.old_node_volume[j] - dt * p.node_lateral[j];
    }
    expected -= dt * std::accumulate(p.cell_lateral.begin(), p.cell_lateral.end(), 0.0);
    EXPECT_NEAR(residual, expected, 2.e-14);
}

TEST(FvPhysicalStorage, AllNativePathsConserveCellAndStorageInventoryAcrossSteps) {
    for (const std::string name :
         {"storage_tee", "storage_loop", "storage_reversed", "storage_closed"}) {
        S::Endpoint comparator;
        for (int mode = 0; mode < 8; ++mode) {
            SCOPED_TRACE(name + " mode=" + std::to_string(mode));
            auto ch = makeStorage(name);
            auto p = storageProblem(ch, name, true);
            const double initial = inventory(p);
            S solver;
            S::Controls c;
            c.reuse_linear_workspace = mode >= 4;
            if (mode % 4 != 0)
                c.method = S::LinearMethod::CondensedOnly;
            if (mode % 4 >= 2)
                c.condensation.reduced_gmres_only = true;
            if (mode % 4 == 3)
                c.condensation.tree_interface = true;
            if (mode >= 4)
                c.preconditioner = S::Preconditioner::BlockIlu0;
            double external = 0.0, source = 0.0;
            S::Endpoint e;
            for (int step = 0; step < 4; ++step) {
                const auto r = solver.solve(p, .25, c, e);
                ASSERT_TRUE(r.converged())
                    << static_cast<int>(r.status) << ' ' << r.scaled_residual;
                EXPECT_LE(r.scaled_residual, 1.0);
                S::Evaluation value;
                ASSERT_TRUE(S::residual(p, e, .25, value));
                for (int j = 0; j < ch.mesh.n_nodes(); ++j)
                    if (ch.mesh.node_kind[j] == kNodeStorage) {
                        EXPECT_EQ(e.node_volume[j], curveVolume(ch.mesh, j, e.head[j]));
                        EXPECT_LE(std::fabs(value.storage[j]),
                                  c.absolute_volume +
                                      c.relative_volume * std::max(1., p.old_node_volume[j]));
                    }
                for (int f = 0; f < ch.mesh.n_faces(); ++f) {
                    const int j = ch.mesh.face_node[f];
                    if (j >= 0 && std::isfinite(p.fixed_head[j]))
                        external += .25 * (ch.mesh.face_cl[f] >= 0 ? 1 : -1) * e.faces[f].flux.mass;
                }
                source +=
                    .25 * (std::accumulate(p.node_lateral.begin(), p.node_lateral.end(), 0.0) +
                           std::accumulate(p.cell_lateral.begin(), p.cell_lateral.end(), 0.0));
                advance(p, e);
                EXPECT_NEAR(inventory(p) - initial + external - source, 0.0, 2.e-10);
            }
            EXPECT_GT(e.head[1], 6.0); // crosses a volume-table knot at 6 ft
            if (mode == 0)
                comparator = e;
            else {
                for (int i = 0; i < ch.n; ++i) {
                    EXPECT_NEAR(e.depth[i], comparator.depth[i], 1.e-8);
                    EXPECT_NEAR(e.discharge[i], comparator.discharge[i], 1.e-8);
                }
                for (int j = 0; j < ch.mesh.n_nodes(); ++j)
                    EXPECT_NEAR(e.head[j], comparator.head[j], 1.e-8);
            }
        }
    }
}

TEST(FvPhysicalStorage, OrdinaryNodesNeverAcquireArtificialStorage) {
    auto ch = makeStorage("storage_loop");
    auto p = storageProblem(ch, "storage_loop", true);
    auto x = trial(p);
    S::Evaluation a, b;
    ASSERT_TRUE(S::residual(p, x, .25, a));
    for (int j = 0; j < ch.mesh.n_nodes(); ++j)
        if (ch.mesh.node_kind[j] == kNodeJunction)
            ch.mesh.node_area[j] = 1.e20;
    ASSERT_TRUE(S::residual(p, x, .25, b));
    EXPECT_EQ(a.junction, b.junction);
    EXPECT_EQ(a.storage, b.storage);
    for (int j = 0; j < ch.mesh.n_nodes(); ++j)
        if (ch.mesh.node_kind[j] != kNodeStorage)
            EXPECT_EQ(b.node_volume[j], 0.0);
}

TEST(FvPhysicalStorage, InvalidHistoryCurvesAndUnsupportedFeaturesAreAtomic) {
    for (int error = 0; error < 16; ++error) {
        SCOPED_TRACE(error);
        auto ch = makeStorage("storage_tee");
        auto p = storageProblem(ch, "storage_tee");
        auto& m = ch.mesh;
        const int off = m.node_vol_off[1];
        switch (error) {
        case 0:
            p.old_node_volume.clear();
            break;
        case 1:
            p.old_node_volume.pop_back();
            break;
        case 2:
            p.old_node_volume[1] += .01;
            break;
        case 3:
            p.old_node_volume[0] = .01;
            break;
        case 4:
            m.node_vol_tbl[off + 1] = -1;
            break;
        case 5:
            m.node_vol_tbl[off + 2] = m.node_vol_tbl[off + 1];
            break;
        case 6:
            m.node_vol_tbl[off + 2] = nan();
            break;
        case 7:
            m.node_vol_tbl[off] = 1;
            break;
        case 8:
            m.node_vol_off[1] = static_cast<int>(m.node_vol_tbl.size());
            break;
        case 9:
            m.node_vol_dmax[1] = 0;
            break;
        case 10:
            m.node_full_depth[1] = 5;
            break;
        case 11:
            m.node_can_pond[1] = 1;
            break;
        case 12:
            p.fixed_head[1] = 6;
            break;
        case 13:
            m.node_vol_dmax.clear();
            break;
        case 14:
            m.node_invert[1] = nan();
            break;
        case 15:
            p.old_node_volume[1] = nan();
            break;
        }
        S solver;
        auto e = sentinel();
        const auto r = solver.solve(p, .25, {}, e);
        EXPECT_EQ(r.status, S::Status::InvalidInput);
        unchanged(e);
    }
}

TEST(FvPhysicalStorage, TrialBoundsAndFailedSolveDoNotPublishVolumeOrCache) {
    auto ch = makeStorage("storage_tee");
    auto p = storageProblem(ch, "storage_tee", true);
    auto x = trial(p);
    S::Evaluation e;
    e.storage = {-7};
    x.head[1] = 8.01;
    EXPECT_FALSE(S::residual(p, x, .25, e));
    EXPECT_EQ(e.storage, (std::vector<double>{-7}));
    S solver;
    S::Endpoint accepted;
    S::Controls c;
    c.reuse_linear_workspace = true;
    ASSERT_TRUE(solver.solve(p, .25, c, accepted).converged());
    EXPECT_GT(solver.linearWorkspaceBytes(), 0u);
    c.max_newton = 0;
    auto out = sentinel();
    EXPECT_EQ(solver.solve(p, .25, c, out).status, S::Status::IterationLimit);
    unchanged(out);
    EXPECT_EQ(solver.linearWorkspaceBytes(), 0u);
    c.max_newton = 30;
    c.linear.workspace_limit_bytes = 1;
    EXPECT_EQ(solver.solve(p, .25, c, out).status, S::Status::LinearFailure);
    unchanged(out);
    EXPECT_EQ(solver.linearWorkspaceBytes(), 0u);
}

TEST(FvPhysicalStorage, AbsoluteHeadsUseNodeInvertAndClosedCurveTop) {
    auto ch = makeStorage("storage_tee");
    auto p = storageProblem(ch, "storage_tee");
    ch.mesh.node_invert[1] = -2.0;
    p.old_node_volume[1] = curveVolume(ch.mesh, 1, p.initial_head[1]);
    S::Evaluation e;
    ASSERT_TRUE(S::residual(p, trial(p), .25, e));
    EXPECT_EQ(e.node_volume[1],
              ch.mesh.node_vol_tbl[ch.mesh.node_vol_off[1] + kNodeVolSamples - 1]);
    auto x = trial(p);
    x.head[1] = std::nextafter(6.0, 7.0);
    EXPECT_FALSE(S::residual(p, x, .25, e));
}

TEST(FvPhysicalStorage, MeshCurveMayClampAboveRimWithoutAddingSurchargeStorage) {
    auto ch = makeStorage("storage_tee");
    auto& m = ch.mesh;
    m.node_full_depth[1] = 7.5;
    const int off = m.node_vol_off[1], rim = 120;
    for (int i = rim + 1; i < kNodeVolSamples; ++i)
        m.node_vol_tbl[off + i] = m.node_vol_tbl[off + rim];
    auto p = storageProblem(ch, "storage_tee", true);
    S solver;
    S::Endpoint e;
    ASSERT_TRUE(solver.solve(p, .25, {}, e).converged());
    auto x = trial(p);
    x.head[1] = 7.5;
    S::Linearization l;
    ASSERT_TRUE(S::linearize(p, x, .25, 1.e-4, l));
    S::Evaluation a, b;
    ASSERT_TRUE(S::residual(p, x, .25, a));
    x.head[1] -= 1.e-5;
    ASSERT_TRUE(S::residual(p, x, .25, b));
    const auto row = l.residual.size() - 1;
    double diagonal = 0.;
    for (auto t = l.matrix.row[row]; t < l.matrix.row[row + 1]; ++t)
        if (l.matrix.column[t] == static_cast<int>(row))
            diagonal = l.matrix.value[t];
    EXPECT_NEAR(diagonal, (a.storage[1] - b.storage[1]) / (1.e-5 * l.row_scale[row]), 1.e-8);
    x.head[1] = std::nextafter(7.5, 8.0);
    EXPECT_FALSE(S::residual(p, x, .25, a));
    m.node_vol_tbl[off + rim + 1] -= 1;
    EXPECT_FALSE(S::residual(p, trial(p), .25, a));
}
