/**
 * @file test_quality_routing.cpp
 * @brief Unit tests for batch water quality routing (QualitySolver).
 *
 * @details Tests each stage of the quality routing pipeline:
 *   - addWetWeatherLoads: subcatchment washoff → node mass/volume inflow
 *   - accumulateLinkLoads: link mass transport → downstream node
 *   - mixAtNodes: complete mixing (CSTR) at each node
 *   - applyDecay: first-order decay in nodes and links
 *   - updateLinkQuality: copy upstream node conc → link
 *   - execute: full pipeline integration test
 *
 * @see src/engine/quality/QualityRouting.cpp
 * @ingroup engine_quality
 */

#include <gtest/gtest.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <numeric>

#include "quality/QualityRouting.hpp"
#include "core/SimulationContext.hpp"

namespace {

using openswmm::SimulationContext;
using openswmm::quality::QualitySolver;
using openswmm::quality::ZERO_VOLUME;

// ============================================================================
// Helper: build a minimal SimulationContext for quality tests
// ============================================================================

/**
 * @brief Create a small network:
 *        Subcatch0 → Node0 →(Link0)→ Node1 →(Link1)→ Node2 (outfall)
 *        with `np` pollutants.
 */
SimulationContext makeContext(int np) {
    SimulationContext ctx;

    // 3 nodes, 2 links, 1 subcatchment
    ctx.node_names.add("N0"); // 0
    ctx.node_names.add("N1"); // 1
    ctx.node_names.add("N2"); // 2

    ctx.link_names.add("L0"); // 0
    ctx.link_names.add("L1"); // 1

    ctx.subcatch_names.add("S0"); // 0

    for (int p = 0; p < np; ++p) {
        ctx.pollutant_names.add("P" + std::to_string(p));
    }

    ctx.allocate_objects();

    // Link connectivity: L0: N0→N1, L1: N1→N2
    ctx.links.node1[0] = 0;
    ctx.links.node2[0] = 1;
    ctx.links.node1[1] = 1;
    ctx.links.node2[1] = 2;

    // Subcatchment outlet → Node0
    ctx.subcatches.outlet_node[0] = 0;

    return ctx;
}

// ============================================================================
// Test fixture
// ============================================================================

class QualityRoutingTest : public ::testing::Test {
protected:
    static constexpr int NP = 2; // number of pollutants
    SimulationContext ctx;
    QualitySolver solver;

    void SetUp() override {
        ctx = makeContext(NP);
        solver.init(ctx.n_nodes(), ctx.n_links(), NP);
    }
};

// ============================================================================
// addWetWeatherLoads
// ============================================================================

TEST_F(QualityRoutingTest, WetWeatherLoadsAccumulateMassAndVolume) {
    double dt = 10.0; // seconds

    // Subcatch0 has runoff flowing to Node0
    ctx.subcatches.runoff[0] = 2.0;       // cfs
    ctx.subcatches.old_runoff[0] = 4.0;   // cfs

    // Pollutant concentrations on subcatch
    // conc[subcatch * np + pollutant]
    ctx.subcatches.conc[0 * NP + 0] = 10.0; // P0 new
    ctx.subcatches.conc[0 * NP + 1] = 20.0; // P1 new
    ctx.subcatches.conc_old[0 * NP + 0] = 8.0;  // P0 old
    ctx.subcatches.conc_old[0 * NP + 1] = 16.0;  // P1 old

    // Reset assembly arrays (as execute() does)
    std::fill(ctx.nodes.qual_mass_in.begin(), ctx.nodes.qual_mass_in.end(), 0.0);
    std::fill(ctx.nodes.qual_vol_in.begin(), ctx.nodes.qual_vol_in.end(), 0.0);

    solver.addWetWeatherLoads(ctx, dt);

    // q = 0.5 * (4.0 + 2.0) = 3.0 cfs
    double q = 3.0;
    EXPECT_DOUBLE_EQ(ctx.nodes.qual_vol_in[0], q * dt);

    // mass_rate_p0 = 0.5 * (4.0*8.0 + 2.0*10.0) = 0.5*(32+20) = 26.0
    EXPECT_DOUBLE_EQ(ctx.nodes.qual_mass_in[0 * NP + 0], 26.0);

    // mass_rate_p1 = 0.5 * (4.0*16.0 + 2.0*20.0) = 0.5*(64+40) = 52.0
    EXPECT_DOUBLE_EQ(ctx.nodes.qual_mass_in[0 * NP + 1], 52.0);

    // Other nodes should be untouched
    EXPECT_DOUBLE_EQ(ctx.nodes.qual_vol_in[1], 0.0);
    EXPECT_DOUBLE_EQ(ctx.nodes.qual_vol_in[2], 0.0);
}

TEST_F(QualityRoutingTest, WetWeatherLoadsZeroRunoffNoContribution) {
    ctx.subcatches.runoff[0] = 0.0;
    ctx.subcatches.old_runoff[0] = 0.0;

    std::fill(ctx.nodes.qual_mass_in.begin(), ctx.nodes.qual_mass_in.end(), 0.0);
    std::fill(ctx.nodes.qual_vol_in.begin(), ctx.nodes.qual_vol_in.end(), 0.0);

    solver.addWetWeatherLoads(ctx, 10.0);

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        EXPECT_DOUBLE_EQ(ctx.nodes.qual_vol_in[static_cast<size_t>(i)], 0.0);
    }
}

// ============================================================================
// accumulateLinkLoads (tested via execute pipeline since it's private)
// We test it indirectly through the full execute()
// ============================================================================

// ============================================================================
// mixAtNodes — tested indirectly through execute
// ============================================================================

// ============================================================================
// applyDecay — tested indirectly through execute
// ============================================================================

// ============================================================================
// Full pipeline: execute()
// ============================================================================

TEST_F(QualityRoutingTest, ExecuteCompleteMixingBasic) {
    double dt = 10.0;

    // Node0: has old volume and old concentration
    ctx.nodes.old_volume[0] = 100.0; // ft3
    ctx.nodes.volume[0] = 100.0;     // remains same (no evap)
    ctx.nodes.conc_old[0 * NP + 0] = 5.0;  // P0 old conc at Node0
    ctx.nodes.conc_old[0 * NP + 1] = 10.0; // P1 old conc at Node0

    // Link0 flows from Node0 → Node1 at 1.0 cfs with some concentration
    ctx.links.flow[0] = 1.0; // positive = N0→N1
    ctx.links.conc_old[0 * NP + 0] = 5.0;
    ctx.links.conc_old[0 * NP + 1] = 10.0;

    // Node1: receiving link0's contribution
    ctx.nodes.old_volume[1] = 50.0;
    ctx.nodes.volume[1] = 60.0; // = old_volume + v_in (no evap)
    ctx.nodes.conc_old[1 * NP + 0] = 2.0;
    ctx.nodes.conc_old[1 * NP + 1] = 4.0;

    // Link1 no flow
    ctx.links.flow[1] = 0.0;

    // Node2 (outfall): empty
    ctx.nodes.old_volume[2] = 0.0;
    ctx.nodes.volume[2] = 0.0;

    // No decay
    ctx.pollutants.k_decay[0] = 0.0;
    ctx.pollutants.k_decay[1] = 0.0;

    // No subcatch runoff
    ctx.subcatches.runoff[0] = 0.0;
    ctx.subcatches.old_runoff[0] = 0.0;

    solver.execute(ctx, dt);

    // Node0: no inflow (no link feeds it, no subcatch runoff)
    // → conc stays at old value
    EXPECT_DOUBLE_EQ(ctx.nodes.conc[0 * NP + 0], 5.0);
    EXPECT_DOUBLE_EQ(ctx.nodes.conc[0 * NP + 1], 10.0);

    // Node1: receives Link0's flow (q=1.0 cfs) with conc_old
    // vol_in = |1.0| * 10 = 10 ft3
    // mass_in_p0 = 1.0 * 5.0 * dt = 50.0
    // c_new_p0 = (c_old*v_old + mass_in) / (v_old + v_in)
    //          = (2.0*50.0 + 50.0) / (50.0 + 10.0) = 150/60 = 2.5
    double v_old1 = 50.0;
    double v_in1 = 1.0 * dt;  // 10
    double mass_in_p0 = 1.0 * 5.0 * dt; // 50
    double mass_in_p1 = 1.0 * 10.0 * dt; // 100
    double c_new_p0 = (2.0 * v_old1 + mass_in_p0) / (v_old1 + v_in1);
    double c_new_p1 = (4.0 * v_old1 + mass_in_p1) / (v_old1 + v_in1);

    EXPECT_NEAR(ctx.nodes.conc[1 * NP + 0], c_new_p0, 1e-10);
    EXPECT_NEAR(ctx.nodes.conc[1 * NP + 1], c_new_p1, 1e-10);
}

TEST_F(QualityRoutingTest, ExecuteFirstOrderDecay) {
    double dt = 10.0;

    // Set up decay coefficient for P0 only
    ctx.pollutants.k_decay[0] = 0.01; // 1/day
    ctx.pollutants.k_decay[1] = 0.0;  // no decay

    // Initial concentrations at all nodes
    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<size_t>(i);
        ctx.nodes.old_volume[ui] = 100.0;
        ctx.nodes.volume[ui] = 100.0;
        ctx.nodes.conc_old[ui * NP + 0] = 50.0; // P0
        ctx.nodes.conc_old[ui * NP + 1] = 50.0; // P1
    }

    // Initial concentrations at all links
    for (int j = 0; j < ctx.n_links(); ++j) {
        auto uj = static_cast<size_t>(j);
        ctx.links.conc_old[uj * NP + 0] = 30.0;
        ctx.links.conc_old[uj * NP + 1] = 30.0;
    }

    // No flow, no subcatch runoff — mixing won't change concentrations
    ctx.links.flow[0] = 0.0;
    ctx.links.flow[1] = 0.0;
    ctx.subcatches.runoff[0] = 0.0;
    ctx.subcatches.old_runoff[0] = 0.0;

    solver.execute(ctx, dt);

    double decay_factor = 1.0 - 0.01 * dt;

    // P0 at nodes should be decayed; P1 should remain unchanged
    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<size_t>(i);
        EXPECT_NEAR(ctx.nodes.conc[ui * NP + 0], 50.0 * decay_factor, 1e-10)
            << "Node " << i << " P0 decay";
        EXPECT_NEAR(ctx.nodes.conc[ui * NP + 1], 50.0, 1e-10)
            << "Node " << i << " P1 no decay";
    }
}

TEST_F(QualityRoutingTest, ExecuteUpdateLinkQualFromUpstream) {
    double dt = 10.0;

    // Set up: Link0 has positive flow N0→N1, so upstream is N0
    ctx.links.flow[0] = 2.0;
    ctx.links.flow[1] = 0.0;

    // Node0 concentration (will get picked up after mixing)
    ctx.nodes.old_volume[0] = 100.0;
    ctx.nodes.volume[0] = 100.0;
    ctx.nodes.conc_old[0 * NP + 0] = 7.0;
    ctx.nodes.conc_old[0 * NP + 1] = 14.0;

    // Node1 / Node2 minimal
    ctx.nodes.old_volume[1] = 100.0;
    ctx.nodes.volume[1] = 100.0;
    ctx.nodes.old_volume[2] = 100.0;
    ctx.nodes.volume[2] = 100.0;

    // Old link concentrations (will be overwritten by upstream node)
    ctx.links.conc_old[0 * NP + 0] = 99.0;
    ctx.links.conc_old[0 * NP + 1] = 99.0;

    // No decay, no subcatch
    ctx.pollutants.k_decay[0] = 0.0;
    ctx.pollutants.k_decay[1] = 0.0;
    ctx.subcatches.runoff[0] = 0.0;
    ctx.subcatches.old_runoff[0] = 0.0;

    solver.execute(ctx, dt);

    // After execute, Link0 should copy upstream (Node0) concentration
    // Node0 has no inflow so conc[N0] = conc_old[N0] = 7.0 / 14.0
    EXPECT_NEAR(ctx.links.conc[0 * NP + 0], 7.0, 1e-10);
    EXPECT_NEAR(ctx.links.conc[0 * NP + 1], 14.0, 1e-10);
}

TEST_F(QualityRoutingTest, ExecuteReverseFlowUsesCorrectUpstream) {
    double dt = 10.0;

    // Link0: negative flow means actual flow is N1→N0, so upstream = N1
    ctx.links.flow[0] = -2.0;
    ctx.links.flow[1] = 0.0;

    // Node0 and Node1
    ctx.nodes.old_volume[0] = 100.0;
    ctx.nodes.volume[0] = 100.0;
    ctx.nodes.old_volume[1] = 100.0;
    ctx.nodes.volume[1] = 100.0;
    ctx.nodes.old_volume[2] = 100.0;
    ctx.nodes.volume[2] = 100.0;

    ctx.nodes.conc_old[0 * NP + 0] = 1.0;  // Node0, P0
    ctx.nodes.conc_old[1 * NP + 0] = 9.0;  // Node1, P0

    ctx.links.conc_old[0 * NP + 0] = 9.0;  // Link0 old conc
    ctx.links.conc_old[0 * NP + 1] = 0.0;

    ctx.pollutants.k_decay[0] = 0.0;
    ctx.pollutants.k_decay[1] = 0.0;
    ctx.subcatches.runoff[0] = 0.0;
    ctx.subcatches.old_runoff[0] = 0.0;

    solver.execute(ctx, dt);

    // Upstream of Link0 when flow < 0 is node2 (N1)
    // After mixing: N1 has no inflow (Link0 reversed goes to N0 as downstream)
    // so N1 conc stays at 9.0
    // Link0 should copy from N1
    EXPECT_NEAR(ctx.links.conc[0 * NP + 0], 9.0, 1e-10);
}

TEST_F(QualityRoutingTest, ExecuteZeroPollutantsIsNoOp) {
    // Create a context with 0 pollutants
    SimulationContext ctx0 = makeContext(0);
    QualitySolver solver0;
    solver0.init(ctx0.n_nodes(), ctx0.n_links(), 0);

    // Should not crash or modify anything
    solver0.execute(ctx0, 10.0);
}

TEST_F(QualityRoutingTest, ExecuteEmptyNodeVolumeUsesCin) {
    double dt = 10.0;

    // Node1 has zero old volume — should use c_in directly
    ctx.nodes.old_volume[1] = 0.0;
    ctx.nodes.volume[1] = 10.0; // gains volume from link inflow

    // Link0: flow 1.0 cfs from N0→N1
    ctx.links.flow[0] = 1.0;
    ctx.links.conc_old[0 * NP + 0] = 20.0;
    ctx.links.conc_old[0 * NP + 1] = 40.0;
    ctx.links.flow[1] = 0.0;

    // Node0 stable
    ctx.nodes.old_volume[0] = 100.0;
    ctx.nodes.volume[0] = 100.0;
    ctx.nodes.conc_old[0 * NP + 0] = 5.0;
    ctx.nodes.conc_old[0 * NP + 1] = 10.0;

    ctx.nodes.old_volume[2] = 0.0;
    ctx.nodes.volume[2] = 0.0;

    ctx.pollutants.k_decay[0] = 0.0;
    ctx.pollutants.k_decay[1] = 0.0;
    ctx.subcatches.runoff[0] = 0.0;
    ctx.subcatches.old_runoff[0] = 0.0;

    // Node1 old conc doesn't matter when v_old = 0
    ctx.nodes.conc_old[1 * NP + 0] = 999.0;
    ctx.nodes.conc_old[1 * NP + 1] = 999.0;

    solver.execute(ctx, dt);

    // v_in = 1.0 * 10 = 10
    // mass_in_p0 = 1.0 * 20.0 * 10 = 200
    // c_in_p0 = mass_in / v_in = 200 / 10 = 20.0
    // Since old_volume = 0, c_new = c_in = 20.0
    // But capped at max(c_old, c_in): c_old=999 so cap won't reduce
    // Actually c_max = max(999, 20) = 999, c_new = min(20, 999) = 20
    EXPECT_NEAR(ctx.nodes.conc[1 * NP + 0], 20.0, 1e-10);
    EXPECT_NEAR(ctx.nodes.conc[1 * NP + 1], 40.0, 1e-10);
}

TEST_F(QualityRoutingTest, ConcentrationNeverNegative) {
    double dt = 10.0;

    // Set decay rate high enough that factor goes negative
    // k_decay = 0.2, dt = 10 → factor = 1 - 2.0 = -1.0
    ctx.pollutants.k_decay[0] = 0.2;
    ctx.pollutants.k_decay[1] = 0.0;

    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<size_t>(i);
        ctx.nodes.old_volume[ui] = 100.0;
        ctx.nodes.volume[ui] = 100.0;
        ctx.nodes.conc_old[ui * NP + 0] = 50.0;
        ctx.nodes.conc_old[ui * NP + 1] = 50.0;
    }

    ctx.links.flow[0] = 0.0;
    ctx.links.flow[1] = 0.0;
    ctx.subcatches.runoff[0] = 0.0;
    ctx.subcatches.old_runoff[0] = 0.0;

    solver.execute(ctx, dt);

    // With factor = -1, raw result would be negative. Decay clamps to >= 0.
    for (int i = 0; i < ctx.n_nodes(); ++i) {
        auto ui = static_cast<size_t>(i);
        EXPECT_GE(ctx.nodes.conc[ui * NP + 0], 0.0)
            << "Node " << i << " P0 concentration must be non-negative";
    }
}

// ============================================================================
// Multi-pollutant stress test
// ============================================================================

TEST_F(QualityRoutingTest, MultiPollutantStressTest) {
    constexpr int NPOLL = 8;
    SimulationContext ctx_stress = makeContext(NPOLL);
    QualitySolver solver_stress;
    solver_stress.init(ctx_stress.n_nodes(), ctx_stress.n_links(), NPOLL);

    double dt = 5.0;

    // Set up flow and concentrations
    ctx_stress.links.flow[0] = 3.0;
    ctx_stress.links.flow[1] = 2.0;
    ctx_stress.subcatches.runoff[0] = 1.0;
    ctx_stress.subcatches.old_runoff[0] = 1.0;

    for (int i = 0; i < ctx_stress.n_nodes(); ++i) {
        auto ui = static_cast<size_t>(i);
        ctx_stress.nodes.old_volume[ui] = 50.0;
        ctx_stress.nodes.volume[ui] = 50.0;
    }

    for (int p = 0; p < NPOLL; ++p) {
        auto up = static_cast<size_t>(p);
        ctx_stress.pollutants.k_decay[up] = 0.001 * (p + 1);

        // Node concentrations
        for (int i = 0; i < ctx_stress.n_nodes(); ++i) {
            auto ui = static_cast<size_t>(i);
            ctx_stress.nodes.conc_old[ui * NPOLL + up] = 10.0 * (p + 1);
        }
        // Link concentrations
        for (int j = 0; j < ctx_stress.n_links(); ++j) {
            auto uj = static_cast<size_t>(j);
            ctx_stress.links.conc_old[uj * NPOLL + up] = 10.0 * (p + 1);
        }
        // Subcatch concentrations
        ctx_stress.subcatches.conc[0 * NPOLL + up] = 5.0 * (p + 1);
        ctx_stress.subcatches.conc_old[0 * NPOLL + up] = 5.0 * (p + 1);
    }

    // Should not crash; all concentrations should be finite and non-negative
    solver_stress.execute(ctx_stress, dt);

    for (int i = 0; i < ctx_stress.n_nodes(); ++i) {
        for (int p = 0; p < NPOLL; ++p) {
            auto idx = static_cast<size_t>(i) * NPOLL + static_cast<size_t>(p);
            EXPECT_TRUE(std::isfinite(ctx_stress.nodes.conc[idx]))
                << "Node " << i << " P" << p << " is not finite";
            EXPECT_GE(ctx_stress.nodes.conc[idx], 0.0)
                << "Node " << i << " P" << p << " is negative";
        }
    }

    for (int j = 0; j < ctx_stress.n_links(); ++j) {
        for (int p = 0; p < NPOLL; ++p) {
            auto idx = static_cast<size_t>(j) * NPOLL + static_cast<size_t>(p);
            EXPECT_TRUE(std::isfinite(ctx_stress.links.conc[idx]))
                << "Link " << j << " P" << p << " is not finite";
            EXPECT_GE(ctx_stress.links.conc[idx], 0.0)
                << "Link " << j << " P" << p << " is negative";
        }
    }
}

// ============================================================================
// Constants
// ============================================================================

TEST(QualityConstantsTest, ZeroVolumeIsOneLiterInFt3) {
    // 1 liter = 0.0353147 ft3
    EXPECT_NEAR(ZERO_VOLUME, 0.0353147, 1e-6);
}

TEST(QualityConstantsTest, ZeroDepthIsOneMillimeterInFt) {
    EXPECT_NEAR(openswmm::quality::ZERO_DEPTH, 0.003281, 1e-5);
}

// ============================================================================
// Gap #38: Steady Flow quality routing (findSFLinkQual)
// ============================================================================

// For STEADY routing the link gets upstream node concentration scaled by
// fEvap and exact exponential decay over dt — no volume-balance mixing.

TEST(SteadyFlowQuality, NoDecayLinkGetsUpstreamConc) {
    SimulationContext ctx = makeContext(1);
    ctx.options.routing_model = openswmm::RoutingModel::STEADY;
    QualitySolver solver;
    solver.init(ctx.n_nodes(), ctx.n_links(), 1);

    // Upstream node 0 has concentration 10.0 after node mixing
    ctx.nodes.conc[0] = 10.0;

    // Link 0 (N0→N1): flowing, no decay, no evap
    ctx.links.flow[0]       = 1.0;
    ctx.links.volume[0]     = 100.0;
    ctx.links.old_volume[0] = 100.0;
    ctx.links.conc_old[0]   = 5.0;   // old link conc shouldn't matter for STEADY

    // k_decay = 0 → no decay
    ctx.pollutants.k_decay.assign(1, 0.0);

    solver.updateLinkQuality(ctx, 300.0);

    // link[0] conc should equal upstream (node 0) conc = 10.0
    EXPECT_NEAR(ctx.links.conc[0], 10.0, 1e-10);
}

TEST(SteadyFlowQuality, WithDecayUsesExponentialNotLinear) {
    SimulationContext ctx = makeContext(1);
    ctx.options.routing_model = openswmm::RoutingModel::STEADY;
    QualitySolver solver;
    solver.init(ctx.n_nodes(), ctx.n_links(), 1);

    const double dt    = 300.0;   // routing step
    const double k     = 0.001;   // /sec decay constant
    const double c_up  = 8.0;     // upstream conc

    ctx.nodes.conc[0]       = c_up;
    ctx.links.flow[0]       = 1.0;
    ctx.links.volume[0]     = 100.0;
    ctx.links.old_volume[0] = 100.0;
    ctx.links.conc_old[0]   = 999.0;  // irrelevant for STEADY
    ctx.pollutants.k_decay.assign(1, k);

    solver.updateLinkQuality(ctx, dt);

    double expected = c_up * std::exp(-k * dt);
    EXPECT_NEAR(ctx.links.conc[0], expected, 1e-9);

    // Verify it differs from the linear approximation (1 - k*dt):
    double linear_approx = c_up * (1.0 - k * dt);
    EXPECT_NE(ctx.links.conc[0], linear_approx)
        << "STEADY should use exp(-k*dt), not linear (1-k*dt)";
}

TEST(SteadyFlowQuality, OldLinkConcIgnored) {
    // Regardless of old link concentration, STEADY routing uses upstream node.
    SimulationContext ctx = makeContext(1);
    ctx.options.routing_model = openswmm::RoutingModel::STEADY;
    QualitySolver solver;
    solver.init(ctx.n_nodes(), ctx.n_links(), 1);

    ctx.nodes.conc[0]       = 5.0;
    ctx.links.flow[0]       = 2.0;
    ctx.links.volume[0]     = 50.0;
    ctx.links.old_volume[0] = 50.0;
    ctx.links.conc_old[0]   = 100.0;  // very different old conc
    ctx.pollutants.k_decay.assign(1, 0.0);

    solver.updateLinkQuality(ctx, 60.0);

    // Result must equal upstream conc, NOT some mixture with old conc
    EXPECT_NEAR(ctx.links.conc[0], 5.0, 1e-10);
}

// ============================================================================
// CSTR first-order decay trajectory benchmark
//
// Benchmark dataset: tests/benchmarks/manufactured/quality-cstr-first-order-decay/
//
// No inflow, no outflow, constant volume.  k_decay = 1e-3 /s, dt = 60 s.
// execute() applies factor = 1 - k*dt = 0.94 per step.  Reference:
//   C[N] = 100 * 0.94^N   (discrete recurrence — NOT exp(-k*t))
//
// After each execute() the test copies conc → conc_old to advance timestep
// state, matching what the simulation loop does between routing steps.
// ============================================================================

#ifndef BENCHMARK_DATA_DIR
#  define BENCHMARK_DATA_DIR ""
#endif

// First-order decay trajectory: C[N] = C0 * (1-k*dt)^N.
// Verifies applyDecay applies the linear factor correctly across multiple steps.
TEST(QualityCSTR, FirstOrderDecayTrajectory) {
    const std::string path = std::string(BENCHMARK_DATA_DIR)
        + "/manufactured/quality-cstr-first-order-decay/reference.csv";

    // Load reference CSV (t_s, C_mgl)
    struct DecayRow { double t_s, C_mgl; };
    std::vector<DecayRow> rows;
    {
        std::ifstream in(path);
        if (!in.is_open()) {
            GTEST_SKIP() << "Benchmark data not found: " << path;
        }
        std::string line;
        bool header_seen = false;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (!header_seen) { header_seen = true; continue; }
            std::istringstream ss(line);
            std::string tok;
            double vals[2] = {};
            int col = 0;
            while (std::getline(ss, tok, ',') && col < 2)
                vals[col++] = std::stod(tok);
            if (col >= 2)
                rows.push_back({vals[0], vals[1]});
        }
    }
    if (rows.empty()) {
        GTEST_SKIP() << "Benchmark data empty: " << path;
    }

    // 1 pollutant, 3 nodes, 2 links (from makeContext(1))
    SimulationContext ctx = makeContext(1);
    QualitySolver solver;
    solver.init(ctx.n_nodes(), ctx.n_links(), 1);

    const double K_DECAY = 1.0e-3;     // /s — first-order decay rate
    const double C_0     = rows[0].C_mgl;  // 100.0 mg/L
    const double V_NODE  = 1000.0;         // constant volume — must be > ZERO_VOLUME

    ctx.pollutants.k_decay.assign(1, K_DECAY);

    // Zero all flows (no mixing, no inflow — pure decay)
    for (size_t j = 0; j < static_cast<size_t>(ctx.n_links()); ++j) {
        ctx.links.flow[j] = 0.0;
    }
    ctx.subcatches.runoff[0]     = 0.0;
    ctx.subcatches.old_runoff[0] = 0.0;

    // Initialise node 0 with C_0; other nodes at 0 (not tested)
    for (size_t i = 0; i < static_cast<size_t>(ctx.n_nodes()); ++i) {
        ctx.nodes.old_volume[i] = V_NODE;
        ctx.nodes.volume[i]     = V_NODE;
        ctx.nodes.conc_old[i]   = (i == 0) ? C_0 : 0.0;
        ctx.nodes.conc[i]       = (i == 0) ? C_0 : 0.0;
    }
    for (size_t j = 0; j < static_cast<size_t>(ctx.n_links()); ++j) {
        ctx.links.old_volume[j] = V_NODE;
        ctx.links.volume[j]     = V_NODE;
        ctx.links.conc_old[j]   = 0.0;
        ctx.links.conc[j]       = 0.0;
    }

    double max_err  = 0.0;
    double sum_sq   = 0.0;
    double prev_t   = rows[0].t_s;

    for (size_t i = 1; i < rows.size(); ++i) {
        double dt = rows[i].t_s - prev_t;

        // Advance "old" state before each execute() — mirrors simulation loop
        ctx.nodes.conc_old[0] = ctx.nodes.conc[0];
        ctx.links.conc_old[0] = ctx.links.conc[0];

        solver.execute(ctx, dt);

        double err = std::abs(ctx.nodes.conc[0] - rows[i].C_mgl);
        max_err  = std::max(max_err, err);
        sum_sq  += err * err;
        prev_t   = rows[i].t_s;
    }

    double n = static_cast<double>(rows.size() - 1);

    EXPECT_LT(max_err, 1e-9)
        << "CSTR decay max error " << max_err
        << " mg/L exceeds 1e-9 (benchmark: " << path << ")";
    EXPECT_LT(std::sqrt(sum_sq / n), 1e-10)
        << "CSTR decay RMS error exceeds 1e-10 mg/L";
}

} /* anonymous namespace */
