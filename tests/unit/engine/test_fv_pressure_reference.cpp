// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      test_fv_pressure_reference.cpp
 * @brief     Tests for fv pressure reference.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <limits>

#include "../../verification/fv_implicit_phase0/network_cases.hpp"

namespace {
using namespace fvtest;

/**
 * @brief Advance a shifted-datum pressure-network fixture for thirty seconds.
 *
 * Uses pressure-implicit options without LTS, holds heads at nodes zero/five through borrowed local
 * forcing arrays and checks exact target arrival/no divergence plus folded fork/merge flags each
 * call. Nonfatal expectations permit partial state on refusal; no success flag, files or retained
 * forcing pointers escape local solver lifetime.
 *
 * @param[in] name Network fixture name accepted by makeNetwork, yielding six nodes and the expected
 *     fork/merge.
 * @param[in] cells Positive per-link discretization count accepted by makeNetwork.
 * @param[in] celerity Pressure-wave speed ft/s.
 * @param[in] datum Elevation/head shift feet applied uniformly without changing depth.
 * @return Owned final channel mesh/state after six attempted 5 s windows.
 */
Channel runLoop(const std::string& name, int cells, double celerity, double datum) {
    Channel ch = fvbaseline::makeNetwork(name, cells, celerity);
    // Shift elevations without changing depths, lengths, friction or forcing.
    for (double& z : ch.mesh.cell_zb)
        z += datum;
    for (double& z : ch.mesh.face_zb)
        z += datum;
    for (double& z : ch.mesh.node_face_zb)
        z += datum;
    for (double& z : ch.mesh.node_invert)
        z += datum;
    for (double& h : ch.state.node_head)
        h += datum;
    std::vector<double> fixed(ch.mesh.n_nodes(), std::numeric_limits<double>::quiet_NaN());
    fixed[0] = ch.state.node_head[0];
    fixed[5] = ch.state.node_head[5];
    FvStepForcing forcing{};
    forcing.node_fixed_head = fixed.data();
    forcing.n_nodes = ch.mesh.n_nodes();
    FvOptions opts = defaultOptions();
    opts.pressurized_implicit = true;
    opts.lts = false;
    ExplicitFvSolver solver;
    solver.initialize(ch.mesh, ch.state, opts);
    for (int step = 0; step < 6; ++step) {
        const double target = (step + 1) * 5.0;
        EXPECT_EQ(solver.advance(step * 5.0, target, forcing), target);
        INetworkSolver::Divergence divergence;
        EXPECT_FALSE(solver.divergence(divergence));
        const auto& folded = solver.pressure_folded_nodes();
        EXPECT_EQ(folded.size(), 6u);
        if (folded.size() == 6) {
            EXPECT_TRUE(folded[1]); // degree-three fork
            EXPECT_TRUE(folded[4]); // degree-three merge
        }
    }
    return ch;
}
} // namespace

TEST(FvPressureReference, LoopedLakeAtRestDoesNotAcquireSpuriousMotion) {
    for (const int cells : {4, 16}) {
        for (const double celerity : {100.0, 300.0, 1000.0}) {
            for (const double datum : {0.0, 1024.0}) {
                SCOPED_TRACE(::testing::Message()
                             << cells << " cells, c=" << celerity << ", datum=" << datum);
                const auto ch = runLoop("loop_rest", cells, celerity, datum);
                for (int i = 0; i < ch.n; ++i) {
                    EXPECT_NEAR(ch.state.cell_q[i], 0.0, 1.e-9);
                    EXPECT_NEAR(kernels::depthOfArea(ch.mesh.geom[0], ch.state.cell_a[i]), 5.5,
                                1.e-9);
                }
                for (double head : ch.state.node_head)
                    EXPECT_NEAR(head, datum + 5.5, 1.e-9);
            }
        }
    }
}

TEST(FvPressureReference, LoopedThroughflowIsIndependentOfElevationDatum) {
    const auto base = runLoop("loop_pressure", 8, 300.0, 0.0);
    const auto shifted = runLoop("loop_pressure", 8, 300.0, 1024.0);
    ASSERT_EQ(base.n, shifted.n);
    for (int i = 0; i < base.n; ++i) {
        EXPECT_NEAR(base.state.cell_q[i], shifted.state.cell_q[i], 1.e-8);
        // On the slot line, tie area agreement to a depth error budget;
        // an absolute area threshold would depend on section/celerity.
        EXPECT_NEAR(base.state.cell_a[i], shifted.state.cell_a[i],
                    base.mesh.geom[0].t_slot * 1.e-8);
    }
    for (int n = 0; n < base.mesh.n_nodes(); ++n)
        EXPECT_NEAR(base.state.node_head[n], shifted.state.node_head[n] - 1024.0, 1.e-9);
}

TEST(FvPressureReference, ScalarAndPathSolvesUseTheSameHeadDatum) {
    for (int cells : {1, 8}) {
        SCOPED_TRACE(cells);
        std::vector<double> reference_q, reference_a;
        for (double datum : {0.0, 1024.0}) {
            Channel ch = makeNodedChannel(
                circular(3.0), cells, 400.0 / cells, [datum](double) { return datum; }, 0.013,
                300.0);
            seedLevel(ch, datum + 5.5);
            ch.state.node_head = {datum + 6.0, datum + 5.5};
            const double fixed[] = {datum + 6.0, datum + 5.5};
            FvStepForcing forcing{};
            forcing.node_fixed_head = fixed;
            forcing.n_nodes = 2;
            FvOptions opts = defaultOptions();
            opts.pressurized_implicit = true;
            opts.lts = false;
            ExplicitFvSolver solver;
            solver.initialize(ch.mesh, ch.state, opts);
            for (int step = 0; step < 6; ++step)
                EXPECT_EQ(solver.advance(step * 5.0, (step + 1) * 5.0, forcing), (step + 1) * 5.0);
            if (datum == 0.0) {
                reference_q = ch.state.cell_q;
                reference_a = ch.state.cell_a;
            } else {
                for (int i = 0; i < cells; ++i) {
                    EXPECT_NEAR(ch.state.cell_q[i], reference_q[i], 1.e-8);
                    EXPECT_NEAR(ch.state.cell_a[i], reference_a[i], ch.mesh.geom[0].t_slot * 1.e-8);
                }
            }
        }
    }
}
