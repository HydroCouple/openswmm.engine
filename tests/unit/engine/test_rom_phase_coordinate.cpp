/**
 * @file test_rom_phase_coordinate.cpp
 * @brief PR H11 — unit tests for the pure computeTravelTime()/phaseOffset()
 *        functions and the DetHistoryRing history buffer.
 *
 * @details Hand-built fixtures, independent of the real hydraulics: these
 *          pin the travel-time accumulation rules (orientation, stagnation,
 *          caps, branching, cycle termination) and the ring buffer's
 *          storage/interpolation/eviction contract. Engine-level plumbing
 *          (velocity -> T̄ through the real engine) lives in
 *          test_engine_1d_rom_lifecycle.cpp; the effect on ROM quantiles
 *          lives in the PhaseCoordinate suite of test_spectral_rom1d.cpp.
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "uncertainty/RomPhaseCoordinate.hpp"

using openswmm::uncertainty::computeTravelTime;
using openswmm::uncertainty::DetHistoryRing;
using openswmm::uncertainty::phaseOffset;
using openswmm::uncertainty::PhaseConfig;

// ============================================================================
// computeTravelTime
// ============================================================================

TEST(RomPhaseCoordinate, SingleEdgeMatchesClosedForm) {
    // T = L / ((5/3) * u); u=2.0, L=100 -> T = 100 / (5/3 * 2) = 30.0.
    const std::vector<int>    n1  = {0};
    const std::vector<int>    n2  = {1};
    const std::vector<double> len = {100.0};
    const std::vector<double> vel = {2.0};
    std::vector<double> tbar(2, -1.0);
    PhaseConfig cfg;
    computeTravelTime(1, n1.data(), n2.data(), len.data(), vel.data(), 2, cfg,
                       tbar.data());
    EXPECT_NEAR(tbar[0], 0.0, 1e-12);
    EXPECT_NEAR(tbar[1], 100.0 / (5.0 / 3.0 * 2.0), 1e-9);
}

TEST(RomPhaseCoordinate, ThreeEdgeChainAccumulatesAdditively) {
    // 0->1->2->3, uniform u and L on each edge -> T[3] = 3 * T_edge.
    const std::vector<int>    n1  = {0, 1, 2};
    const std::vector<int>    n2  = {1, 2, 3};
    const std::vector<double> len = {60.0, 60.0, 60.0};
    const std::vector<double> vel = {1.0, 1.0, 1.0};
    std::vector<double> tbar(4, -1.0);
    PhaseConfig cfg;
    computeTravelTime(3, n1.data(), n2.data(), len.data(), vel.data(), 4, cfg,
                       tbar.data());
    const double t_edge = 60.0 / (5.0 / 3.0 * 1.0);
    EXPECT_NEAR(tbar[0], 0.0, 1e-12);
    EXPECT_NEAR(tbar[1], t_edge, 1e-9);
    EXPECT_NEAR(tbar[2], 2.0 * t_edge, 1e-9);
    EXPECT_NEAR(tbar[3], 3.0 * t_edge, 1e-9);
}

TEST(RomPhaseCoordinate, ReverseFlowOrientsAccumulationTheOtherWay) {
    // Same topology as above but negative velocity everywhere -> flow is
    // n2->n1 for each edge, so travel time accumulates 3->2->1->0 instead.
    const std::vector<int>    n1  = {0, 1, 2};
    const std::vector<int>    n2  = {1, 2, 3};
    const std::vector<double> len = {60.0, 60.0, 60.0};
    const std::vector<double> vel = {-1.0, -1.0, -1.0};
    std::vector<double> tbar(4, -1.0);
    PhaseConfig cfg;
    computeTravelTime(3, n1.data(), n2.data(), len.data(), vel.data(), 4, cfg,
                       tbar.data());
    const double t_edge = 60.0 / (5.0 / 3.0 * 1.0);
    EXPECT_NEAR(tbar[3], 0.0, 1e-12);   // now the source
    EXPECT_NEAR(tbar[2], t_edge, 1e-9);
    EXPECT_NEAR(tbar[1], 2.0 * t_edge, 1e-9);
    EXPECT_NEAR(tbar[0], 3.0 * t_edge, 1e-9);
}

TEST(RomPhaseCoordinate, StagnantEdgeContributesExactlyZero) {
    // |u| below u_min must give t_e == 0, not a huge L/c value and not NaN
    // or inf -- the deliberate answer to the u->0 regime (see file header).
    const std::vector<int>    n1  = {0};
    const std::vector<int>    n2  = {1};
    const std::vector<double> len = {1000.0};
    const std::vector<double> vel = {1.0e-6};  // below default u_min = 1e-3
    std::vector<double> tbar(2, -1.0);
    PhaseConfig cfg;
    computeTravelTime(1, n1.data(), n2.data(), len.data(), vel.data(), 2, cfg,
                       tbar.data());
    EXPECT_EQ(tbar[1], 0.0);
    EXPECT_TRUE(std::isfinite(tbar[1]));
}

TEST(RomPhaseCoordinate, TauEdgeMaxCapsOneSlowEdge) {
    // A very slow edge would otherwise give a huge L/c; tau_edge_max caps it.
    const std::vector<int>    n1  = {0};
    const std::vector<int>    n2  = {1};
    const std::vector<double> len = {1.0e6};
    const std::vector<double> vel = {0.01};  // above u_min, but slow
    std::vector<double> tbar(2, -1.0);
    PhaseConfig cfg;
    cfg.tau_edge_max = 500.0;
    computeTravelTime(1, n1.data(), n2.data(), len.data(), vel.data(), 2, cfg,
                       tbar.data());
    EXPECT_NEAR(tbar[1], 500.0, 1e-9);
}

TEST(RomPhaseCoordinate, TauTotalMaxSaturatesALongChain) {
    // Many edges, each just under tau_edge_max, sum past tau_total_max.
    constexpr int kEdges = 20;
    std::vector<int>    n1(kEdges), n2(kEdges);
    std::vector<double> len(kEdges, 1000.0), vel(kEdges, 1.0);
    for (int e = 0; e < kEdges; ++e) { n1[e] = e; n2[e] = e + 1; }
    std::vector<double> tbar(kEdges + 1, -1.0);
    PhaseConfig cfg;
    cfg.tau_edge_max  = 1.0e6;   // effectively no per-edge cap
    cfg.tau_total_max = 2000.0;  // but the whole path saturates here
    computeTravelTime(kEdges, n1.data(), n2.data(), len.data(), vel.data(),
                       kEdges + 1, cfg, tbar.data());
    EXPECT_NEAR(tbar[kEdges], 2000.0, 1e-9);
}

TEST(RomPhaseCoordinate, BranchingUsesMaxOverIncomingPaths) {
    // 0->2 (short) and 1->2 (long), both flowing into node 2: T[2] must be
    // the MAX of the two incoming path times, not their sum and not
    // whichever edge happens to be listed first.
    const std::vector<int>    n1  = {0, 1};
    const std::vector<int>    n2  = {2, 2};
    const std::vector<double> len = {10.0, 1000.0};
    const std::vector<double> vel = {1.0, 1.0};
    std::vector<double> tbar(3, -1.0);
    PhaseConfig cfg;
    computeTravelTime(2, n1.data(), n2.data(), len.data(), vel.data(), 3, cfg,
                       tbar.data());
    const double t_short = 10.0 / (5.0 / 3.0);
    const double t_long  = 1000.0 / (5.0 / 3.0);
    EXPECT_NEAR(tbar[2], t_long, 1e-9);
    EXPECT_GT(tbar[2], t_short);

    // Edge order reversed must give the identical result.
    const std::vector<int>    n1b  = {1, 0};
    const std::vector<int>    n2b  = {2, 2};
    const std::vector<double> lenb = {1000.0, 10.0};
    const std::vector<double> velb = {1.0, 1.0};
    std::vector<double> tbar2(3, -1.0);
    computeTravelTime(2, n1b.data(), n2b.data(), lenb.data(), velb.data(), 3,
                       cfg, tbar2.data());
    EXPECT_NEAR(tbar2[2], t_long, 1e-9);
}

TEST(RomPhaseCoordinate, CyclicFlowGraphTerminatesWithFiniteValues) {
    // 0->1->2->0: a genuine flow cycle. The bounded sweep count must
    // terminate (not hang) and produce finite, capped values everywhere.
    const std::vector<int>    n1  = {0, 1, 2};
    const std::vector<int>    n2  = {1, 2, 0};
    const std::vector<double> len = {50.0, 50.0, 50.0};
    const std::vector<double> vel = {1.0, 1.0, 1.0};
    std::vector<double> tbar(3, -1.0);
    PhaseConfig cfg;
    cfg.max_sweeps = 8;
    cfg.tau_total_max = 10000.0;
    computeTravelTime(3, n1.data(), n2.data(), len.data(), vel.data(), 3, cfg,
                       tbar.data());
    for (double v : tbar) {
        EXPECT_TRUE(std::isfinite(v));
        EXPECT_GE(v, 0.0);
        EXPECT_LE(v, cfg.tau_total_max);
    }
}

TEST(RomPhaseCoordinate, EmptyGraphGivesAllZeros) {
    std::vector<double> tbar(4, -1.0);
    PhaseConfig cfg;
    computeTravelTime(0, nullptr, nullptr, nullptr, nullptr, 4, cfg,
                       tbar.data());
    for (double v : tbar) EXPECT_EQ(v, 0.0);
}

// ============================================================================
// phaseOffset
// ============================================================================

TEST(RomPhaseCoordinate, PhaseOffsetZeroAtNominalMultiplier) {
    // The invariant everything else rests on: mm=1 -> tau=0 for any T̄,
    // including large/nonzero ones.
    EXPECT_EQ(phaseOffset(1.0, 0.0), 0.0);
    EXPECT_EQ(phaseOffset(1.0, 12345.6), 0.0);
    EXPECT_EQ(phaseOffset(1.0, -999.0), 0.0);
}

TEST(RomPhaseCoordinate, PhaseOffsetSignAndMagnitude) {
    EXPECT_NEAR(phaseOffset(1.2, 100.0), 20.0, 1e-12);   // slower member -> late (+)
    EXPECT_NEAR(phaseOffset(0.8, 100.0), -20.0, 1e-12);  // faster member -> early (-)
}

// ============================================================================
// DetHistoryRing
// ============================================================================

TEST(DetHistoryRing, ExactRecallAtPlaneTimes) {
    DetHistoryRing ring;
    ring.configure(/*n_nodes=*/2, /*max_planes=*/8, /*min_spacing=*/0.0);
    const std::vector<double> p0 = {1.0, 2.0};
    const std::vector<double> p1 = {3.0, 4.0};
    ring.push(0.0, p0.data());
    ring.push(10.0, p1.data());
    EXPECT_NEAR(ring.sample(0.0, 0), 1.0, 1e-12);
    EXPECT_NEAR(ring.sample(0.0, 1), 2.0, 1e-12);
    EXPECT_NEAR(ring.sample(10.0, 0), 3.0, 1e-12);
    EXPECT_NEAR(ring.sample(10.0, 1), 4.0, 1e-12);
}

TEST(DetHistoryRing, LinearInterpolationAtMidpoint) {
    DetHistoryRing ring;
    ring.configure(1, 8, 0.0);
    const std::vector<double> p0 = {0.0};
    const std::vector<double> p1 = {10.0};
    ring.push(0.0, p0.data());
    ring.push(10.0, p1.data());
    EXPECT_NEAR(ring.sample(2.5, 0), 2.5, 1e-9);
    EXPECT_NEAR(ring.sample(5.0, 0), 5.0, 1e-9);
    EXPECT_NEAR(ring.sample(7.5, 0), 7.5, 1e-9);
}

TEST(DetHistoryRing, ClampsBeforeOldest) {
    DetHistoryRing ring;
    ring.configure(1, 8, 0.0);
    const std::vector<double> p0 = {5.0};
    const std::vector<double> p1 = {9.0};
    ring.push(100.0, p0.data());
    ring.push(110.0, p1.data());
    EXPECT_NEAR(ring.sample(0.0, 0), 5.0, 1e-12);
    EXPECT_NEAR(ring.sample(50.0, 0), 5.0, 1e-12);
}

TEST(DetHistoryRing, ClampsAfterNewest) {
    DetHistoryRing ring;
    ring.configure(1, 8, 0.0);
    const std::vector<double> p0 = {5.0};
    const std::vector<double> p1 = {9.0};
    ring.push(100.0, p0.data());
    ring.push(110.0, p1.data());
    EXPECT_NEAR(ring.sample(200.0, 0), 9.0, 1e-12);
}

TEST(DetHistoryRing, MinSpacingSuppressesOverDensePushes) {
    DetHistoryRing ring;
    ring.configure(1, 8, /*min_spacing=*/5.0);
    const std::vector<double> p0 = {1.0};
    const std::vector<double> p1 = {2.0};  // pushed too soon, must be dropped
    const std::vector<double> p2 = {3.0};  // pushed far enough, must be kept
    ring.push(0.0, p0.data());
    ring.push(1.0, p1.data());   // within 5.0 of t=0 -> suppressed
    ring.push(6.0, p2.data());   // >= 5.0 after t=0 -> stored
    // Only two planes should exist: t=0 (value 1.0) and t=6 (value 3.0).
    EXPECT_NEAR(ring.sample(0.0, 0), 1.0, 1e-12);
    EXPECT_NEAR(ring.sample(6.0, 0), 3.0, 1e-12);
    // Interpolating between them proves the suppressed p1 never landed.
    EXPECT_NEAR(ring.sample(3.0, 0), 2.0, 1e-9);  // midpoint of 1.0 and 3.0
}

TEST(DetHistoryRing, CapacityWrapEvictsOldestAndKeepsTimestampsAscending) {
    DetHistoryRing ring;
    ring.configure(1, /*max_planes=*/3, 0.0);
    for (int i = 0; i < 6; ++i) {
        const double v = static_cast<double>(i);
        ring.push(static_cast<double>(i) * 10.0, &v);
    }
    // Only the last 3 planes (t=30,40,50 -> values 3,4,5) should remain.
    EXPECT_NEAR(ring.sample(30.0, 0), 3.0, 1e-9);
    EXPECT_NEAR(ring.sample(40.0, 0), 4.0, 1e-9);
    EXPECT_NEAR(ring.sample(50.0, 0), 5.0, 1e-9);
    // Querying before the retained window clamps to the new oldest (t=30).
    EXPECT_NEAR(ring.sample(0.0, 0), 3.0, 1e-9);
    // Interpolation still works correctly after wrap.
    EXPECT_NEAR(ring.sample(35.0, 0), 3.5, 1e-9);
}

TEST(DetHistoryRing, EmptyRingReportsEmptyAndSampleIsSafe) {
    DetHistoryRing ring;
    ring.configure(1, 8, 0.0);
    EXPECT_TRUE(ring.empty());
    // Must not crash / must return a finite value even with no data.
    EXPECT_TRUE(std::isfinite(ring.sample(0.0, 0)));
}
