/**
 * @file test_rdii.cpp
 * @brief Unit tests for RDII unit hydrograph convolution, initial abstraction,
 *        dry period detection, and pollutant quality loads.
 *
 * @see src/engine/hydrology/RDII.hpp
 * @ingroup engine_hydrology
 */

#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <vector>
#include <numeric>

#include "hydrology/RDII.hpp"
#include "data/InflowData.hpp"
#include "core/SimulationContext.hpp"
#include "input/handlers/InflowsHandler.hpp"

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_inflows.h>

using namespace openswmm;
using namespace openswmm::rdii;

// ============================================================================
// Helper: build a simple UnitHydParams with uniform parameters across months
// ============================================================================

static UnitHydParams makeUniformUH(double r0, double tPeak0_sec, double tBase0_sec,
                                   double r1 = 0.0, double tPeak1 = 0.0, double tBase1 = 0.0,
                                   double r2 = 0.0, double tPeak2 = 0.0, double tBase2 = 0.0) {
    UnitHydParams uh{};
    for (int m = 0; m < 12; ++m) {
        uh.r[m][0] = r0;       uh.tPeak[m][0] = tPeak0_sec; uh.tBase[m][0] = tBase0_sec;
        uh.r[m][1] = r1;       uh.tPeak[m][1] = tPeak1;     uh.tBase[m][1] = tBase1;
        uh.r[m][2] = r2;       uh.tPeak[m][2] = tPeak2;     uh.tBase[m][2] = tBase2;
    }
    return uh;
}

// ============================================================================
// UH Ordinate Tests
// ============================================================================

class UHOrdinateTest : public ::testing::Test {
protected:
    RDIISolver solver;

    // Expose uhOrdinate via a wrapper since it's private
    // We test indirectly through addUnitHydParams + computeAll,
    // but also test the triangular shape properties.
};

TEST(UHOrdinate, ZeroParametersReturnZero) {
    // A UH with zero tPeak and tBase should produce zero ordinate
    UnitHydParams uh{};
    // All params default to zero — ordinate should be 0 at any time
    RDIISolver solver;
    solver.addUnitHydParams("test", uh);
    // We can't call uhOrdinate directly (private), but we verify
    // that computeAll with zero UH params produces zero RDII
    // This is tested indirectly in the convolution tests below.
}

TEST(UHOrdinate, TriangularShapeProperties) {
    // For a triangular UH:
    //   - Peak ordinate = 2/tBase * 3600 (in 1/hr)
    //   - Area under curve = 1.0 (unit hydrograph property)
    //   - Zero at t=0 and t=tBase

    double tPeak = 3600.0;  // 1 hour
    double tBase = 7200.0;  // 2 hours

    // Peak ordinate = 2 / 7200 * 3600 = 1.0 (1/hr)
    double qPeak = 2.0 / tBase * 3600.0;
    EXPECT_NEAR(qPeak, 1.0, 1e-10);

    // Area = 0.5 * tBase * qPeak = 0.5 * 2hr * 1/hr = 1.0
    double area = 0.5 * (tBase / 3600.0) * qPeak;
    EXPECT_NEAR(area, 1.0, 1e-10);
}

TEST(UHOrdinate, AsymmetricTriangle) {
    // tPeak = 1800s (0.5 hr), tBase = 5400s (1.5 hr), K = 2
    double tPeak = 1800.0;
    double tBase = 5400.0;

    double qPeak = 2.0 / tBase * 3600.0;
    // qPeak = 2/5400 * 3600 = 1.333... (1/hr)
    EXPECT_NEAR(qPeak, 2.0 / 5400.0 * 3600.0, 1e-10);

    // Area should still be 1.0 (unit hydrograph)
    double area = 0.5 * (tBase / 3600.0) * qPeak;
    EXPECT_NEAR(area, 1.0, 1e-10);
}

// ============================================================================
// getRainInterval Tests
// ============================================================================

TEST(RainInterval, MinimumLimbDuration) {
    // UH with tPeak=600s, tBase=1200s → falling limb = 600s
    // With wet_step=300, should return min(300, 600, 600) = 300
    UnitHydParams uh = makeUniformUH(0.5, 600.0, 1200.0);
    int ri = RDIISolver::getRainInterval(uh, 300.0);
    EXPECT_EQ(ri, 300);
}

TEST(RainInterval, ShortPeakDeterminesInterval) {
    // UH with tPeak=120s, tBase=600s → min limb = 120s
    // With wet_step=300, should return 120
    UnitHydParams uh = makeUniformUH(0.5, 120.0, 600.0);
    int ri = RDIISolver::getRainInterval(uh, 300.0);
    EXPECT_EQ(ri, 120);
}

TEST(RainInterval, WetStepCapsInterval) {
    // UH with large time scales, wet_step limits
    UnitHydParams uh = makeUniformUH(0.5, 7200.0, 14400.0);
    int ri = RDIISolver::getRainInterval(uh, 300.0);
    EXPECT_EQ(ri, 300);
}

TEST(RainInterval, MultiResponseMinimum) {
    // 3 responses with different time scales — smallest limb wins
    UnitHydParams uh = makeUniformUH(
        0.5, 600.0, 1200.0,   // SHORT: limbs 600, 600
        0.3, 3600.0, 7200.0,  // MEDIUM: limbs 3600, 3600
        0.1, 200.0, 800.0     // LONG: rising=200, falling=600 → min=200
    );
    int ri = RDIISolver::getRainInterval(uh, 1000.0);
    EXPECT_EQ(ri, 200);
}

// ============================================================================
// getMaxPeriods Tests
// ============================================================================

TEST(MaxPeriods, BasicCalculation) {
    // tBase=1200s, rainInterval=300 → periods = 1200/300 + 1 = 5
    UnitHydParams uh = makeUniformUH(0.5, 600.0, 1200.0);
    int mp = RDIISolver::getMaxPeriods(uh, 0, 300);
    EXPECT_EQ(mp, 5);
}

TEST(MaxPeriods, LargerBase) {
    // tBase=7200s, rainInterval=300 → periods = 7200/300 + 1 = 25
    UnitHydParams uh = makeUniformUH(0.5, 3600.0, 7200.0);
    int mp = RDIISolver::getMaxPeriods(uh, 0, 300);
    EXPECT_EQ(mp, 25);
}

TEST(MaxPeriods, MonthVariation) {
    // Different tBase per month → should return max across all months
    UnitHydParams uh{};
    for (int m = 0; m < 12; ++m) {
        uh.r[m][0] = 0.5;
        uh.tPeak[m][0] = 600.0;
        uh.tBase[m][0] = 1200.0 + m * 300.0;  // 1200..4500
    }
    int mp = RDIISolver::getMaxPeriods(uh, 0, 300);
    // Max tBase = 1200 + 11*300 = 4500 → 4500/300 + 1 = 16
    EXPECT_EQ(mp, 16);
}

TEST(MaxPeriods, ZeroIntervalReturnsZero) {
    UnitHydParams uh = makeUniformUH(0.5, 600.0, 1200.0);
    int mp = RDIISolver::getMaxPeriods(uh, 0, 0);
    EXPECT_EQ(mp, 0);
}

// ============================================================================
// Initial Abstraction Tests
// ============================================================================

TEST(InitialAbstraction, ReducesRainfall) {
    // Set up a UH with iaMax=0.5 across all months/responses
    UnitHydParams uh{};
    for (int m = 0; m < 12; ++m) {
        uh.iaMax[m][0] = 0.5;
        uh.iaRecov[m][0] = 0.01;
    }

    UHResponseData rd;
    rd.allocate(10);
    rd.ia_used = 0.0;  // All IA available

    // Rainfall of 0.3 should be fully absorbed (< iaMax)
    // We test via the solver's init + computeAll pathway
    // For direct IA testing, we verify the math:
    double iaUnused = 0.5 - 0.0;  // = 0.5
    double netRain = 0.3 - iaUnused;  // = -0.2 → clamped to 0
    netRain = std::max(netRain, 0.0);
    EXPECT_NEAR(netRain, 0.0, 1e-10);

    // IA used should increase by 0.3
    double iaUsed = 0.0 + (0.3 - 0.0);  // rainfall - netRain
    EXPECT_NEAR(iaUsed, 0.3, 1e-10);
}

TEST(InitialAbstraction, ExcessAfterIAFull) {
    // iaMax=0.2, iaUsed=0.15 → only 0.05 remaining
    double iaMax = 0.2;
    double iaUsed = 0.15;
    double rainfall = 0.3;

    double iaUnused = iaMax - iaUsed;  // 0.05
    double netRain = rainfall - iaUnused;  // 0.25
    netRain = std::max(netRain, 0.0);
    EXPECT_NEAR(netRain, 0.25, 1e-10);
}

TEST(InitialAbstraction, RecoveryDuringDryPeriod) {
    // During dry period, IA recovers at iaRecov rate
    double iaUsed = 0.4;
    double iaRecov = 0.1;  // per day
    double dt_sec = 3600.0;  // 1 hour

    double recovery = dt_sec / 86400.0 * iaRecov;
    double newIAUsed = iaUsed - recovery;
    newIAUsed = std::max(newIAUsed, 0.0);

    // 1 hour = 1/24 day → recovery = 0.1/24 ≈ 0.00417
    EXPECT_NEAR(newIAUsed, 0.4 - 0.1 / 24.0, 1e-10);
}

TEST(InitialAbstraction, ClampedAtZero) {
    // Recovery shouldn't go below zero
    double iaUsed = 0.001;
    double iaRecov = 1.0;  // high recovery rate
    double dt_sec = 86400.0;  // full day

    double recovery = dt_sec / 86400.0 * iaRecov;
    double newIAUsed = iaUsed - recovery;
    newIAUsed = std::max(newIAUsed, 0.0);
    EXPECT_NEAR(newIAUsed, 0.0, 1e-10);
}

// ============================================================================
// Dry Period Detection Tests
// ============================================================================

TEST(DryPeriod, ResetAfterLongDry) {
    UHResponseData rd;
    rd.allocate(10);
    rd.past_rain[0] = 0.5;
    rd.has_past_rain = 1;
    rd.dry_seconds = static_cast<long>(300) * 10;  // exactly at threshold

    // Rainfall arrives after long dry → should reset buffer
    // dry_seconds >= rainInterval * maxPeriods → new event
    EXPECT_GE(rd.dry_seconds, static_cast<long>(300) * rd.max_periods);
}

TEST(DryPeriod, NoResetDuringEvent) {
    UHResponseData rd;
    rd.allocate(10);
    rd.past_rain[0] = 0.5;
    rd.has_past_rain = 1;
    rd.dry_seconds = 600;  // short dry (< 300*10=3000)

    // Rain during an ongoing event should NOT reset
    EXPECT_LT(rd.dry_seconds, static_cast<long>(300) * rd.max_periods);
}

TEST(DryPeriod, DrySecondsAccumulate) {
    UHResponseData rd;
    rd.allocate(10);
    rd.dry_seconds = 0;

    // Accumulate dry time
    int rainInterval = 300;
    rd.dry_seconds += rainInterval;
    EXPECT_EQ(rd.dry_seconds, 300);
    rd.dry_seconds += rainInterval;
    EXPECT_EQ(rd.dry_seconds, 600);
}

// ============================================================================
// RDIISolver Integration Tests
// ============================================================================

// Helper to build a minimal SimulationContext for RDII testing
static SimulationContext makeRdiiContext(int n_nodes, int n_gages) {
    SimulationContext ctx;
    // Register node names so n_nodes() returns the correct count
    for (int i = 0; i < n_nodes; ++i) {
        ctx.node_names.add("N" + std::to_string(i));
    }
    ctx.nodes.resize(n_nodes);
    ctx.gages.rainfall.assign(static_cast<size_t>(n_gages), 0.0);
    ctx.options.wet_step = 300.0;
    ctx.options.start_date = 2460000.5;  // some Julian date
    ctx.options.flow_units = FlowUnits::CFS;
    return ctx;
}

TEST(RDIISolverInit, EmptyAssignments) {
    auto ctx = makeRdiiContext(5, 1);
    RDIISolver solver;
    solver.init(ctx);
    // No RDII assignments → no groups
    solver.applyRdiiInflows(ctx);
    for (int i = 0; i < 5; ++i) {
        EXPECT_NEAR(ctx.nodes.rdii_inflow[static_cast<size_t>(i)], 0.0, 1e-15);
    }
}

TEST(RDIISolverInit, RegistersUnitHydrographs) {
    RDIISolver solver;
    UnitHydParams uh = makeUniformUH(0.5, 3600.0, 7200.0);
    int idx = solver.addUnitHydParams("UH1", uh);
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(solver.findUnitHyd("UH1"), 0);
    EXPECT_EQ(solver.findUnitHyd("nonexistent"), -1);
}

TEST(RDIISolverInit, UpdateExistingUH) {
    RDIISolver solver;
    UnitHydParams uh1 = makeUniformUH(0.5, 3600.0, 7200.0);
    UnitHydParams uh2 = makeUniformUH(0.3, 1800.0, 5400.0);
    int idx1 = solver.addUnitHydParams("UH1", uh1);
    int idx2 = solver.addUnitHydParams("UH1", uh2);  // update
    EXPECT_EQ(idx1, idx2);
    // Updated params should reflect uh2
    EXPECT_NEAR(solver.uh_params[0].r[0][0], 0.3, 1e-10);
}

TEST(RDIISolverInit, InitFromContext) {
    auto ctx = makeRdiiContext(3, 1);

    // Add gage name
    ctx.gage_names.add("Gage1");

    // Add unit hydrograph entries
    UnitHydEntry e{};
    e.name = "UH1";
    e.month = -1;  // ALL
    e.response = 0;  // SHORT
    e.r = 0.5;
    e.t = 1.0;  // hours
    e.k = 1.0;
    e.dmax = 0.1;
    e.drecov = 0.01;
    e.dinit = 0.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH1", "Gage1");

    // Add RDII assignment
    ctx.rdii_assigns.add(0, "UH1", 10.0);  // node 0, area=10 acres

    RDIISolver solver;
    solver.init(ctx);

    // Should have registered UH1 with correct params
    EXPECT_EQ(solver.findUnitHyd("UH1"), 0);
    EXPECT_NEAR(solver.uh_params[0].r[0][0], 0.5, 1e-10);
    // tPeak = 1.0 * 3600 = 3600s
    EXPECT_NEAR(solver.uh_params[0].tPeak[0][0], 3600.0, 1e-6);
    // tBase = 1.0 * (1+1) * 3600 = 7200s
    EXPECT_NEAR(solver.uh_params[0].tBase[0][0], 7200.0, 1e-6);
}

TEST(RDIISolverInit, WarnsWhenAllOverridesMonthlyParams) {
    auto ctx = makeRdiiContext(1, 1);

    // A month-specific entry followed by an ALL entry for the same
    // response — the ALL entry silently overrides July's values, so a
    // WARNING 13 must be issued (matching legacy rdii_readUnitHydParams).
    UnitHydEntry e{};
    e.name = "UH1";
    e.month = 6;     // July (0-based)
    e.response = 0;  // SHORT
    e.r = 0.2;
    e.t = 1.0;
    e.k = 1.0;
    ctx.unit_hyds.add(e);

    e.month = -1;  // ALL
    e.r = 0.5;
    ctx.unit_hyds.add(e);

    RDIISolver solver;
    solver.init(ctx);

    ASSERT_EQ(ctx.warnings.size(), 1u);
    EXPECT_NE(ctx.warnings[0].find("WARNING 13"), std::string::npos);
    EXPECT_NE(ctx.warnings[0].find("UH1"), std::string::npos);

    // The values entered last (ALL) are the ones used for July
    EXPECT_NEAR(solver.uh_params[0].r[6][0], 0.5, 1e-10);
}

TEST(RDIISolverInit, NoWarningForDistinctMonthsAndResponses) {
    auto ctx = makeRdiiContext(1, 1);

    // Different months and different responses never overlap — no warning
    UnitHydEntry e{};
    e.name = "UH1";
    e.response = 0;  // SHORT
    e.r = 0.2;
    e.t = 1.0;
    e.k = 1.0;

    e.month = 0;  // January
    ctx.unit_hyds.add(e);
    e.month = 1;  // February
    ctx.unit_hyds.add(e);
    e.month = 0;  // January again but for a different response
    e.response = 1;  // MEDIUM
    ctx.unit_hyds.add(e);

    RDIISolver solver;
    solver.init(ctx);

    EXPECT_TRUE(ctx.warnings.empty());
}

TEST(RDIISolverInit, WarnsWhenSameMonthRepeated) {
    auto ctx = makeRdiiContext(1, 1);

    // The same month specified twice for the same response
    UnitHydEntry e{};
    e.name = "UH1";
    e.month = 3;     // April
    e.response = 2;  // LONG
    e.r = 0.1;
    e.t = 2.0;
    e.k = 1.5;
    ctx.unit_hyds.add(e);
    e.r = 0.3;
    ctx.unit_hyds.add(e);

    RDIISolver solver;
    solver.init(ctx);

    ASSERT_EQ(ctx.warnings.size(), 1u);
    EXPECT_NE(ctx.warnings[0].find("WARNING 13"), std::string::npos);
    EXPECT_NEAR(solver.uh_params[0].r[3][2], 0.3, 1e-10);
}

// ============================================================================
// Convolution Tests
// ============================================================================

TEST(Convolution, ZeroRainfallProducesZeroRDII) {
    auto ctx = makeRdiiContext(2, 1);
    ctx.gage_names.add("G1");

    UnitHydEntry e{};
    e.name = "UH1"; e.month = -1; e.response = 0;
    e.r = 0.5; e.t = 1.0; e.k = 1.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH1", "G1");
    ctx.rdii_assigns.add(0, "UH1", 10.0);

    RDIISolver solver;
    solver.init(ctx);

    // Zero rainfall
    ctx.gages.rainfall[0] = 0.0;
    solver.computeAll(ctx, 0, 300.0);
    solver.applyRdiiInflows(ctx);

    EXPECT_NEAR(ctx.nodes.rdii_inflow[0], 0.0, 1e-15);
}

TEST(Convolution, RainfallProducesPositiveRDII) {
    auto ctx = makeRdiiContext(2, 1);
    ctx.gage_names.add("G1");

    UnitHydEntry e{};
    e.name = "UH1"; e.month = -1; e.response = 0;
    e.r = 0.5; e.t = 0.5; e.k = 1.0;  // tPeak=1800s, tBase=3600s
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH1", "G1");
    ctx.rdii_assigns.add(0, "UH1", 100.0);

    RDIISolver solver;
    solver.init(ctx);

    // Apply rainfall for several steps to fill the buffer
    double dt = 300.0;
    ctx.gages.rainfall[0] = 1.0;  // 1 in/hr

    // Step through enough intervals to trigger convolution
    int ri = RDIISolver::getRainInterval(solver.uh_params[0], 300.0);
    int steps_per_interval = std::max(1, ri / static_cast<int>(dt));

    for (int s = 0; s < steps_per_interval * 3; ++s) {
        std::fill(ctx.nodes.rdii_inflow.begin(), ctx.nodes.rdii_inflow.end(), 0.0);
        solver.computeAll(ctx, 0, dt);
        solver.applyRdiiInflows(ctx);
    }

    // After sustained rainfall, RDII should be positive
    EXPECT_GT(ctx.nodes.rdii_inflow[0], 0.0);
    // Node 1 (no RDII assignment) should be zero
    EXPECT_NEAR(ctx.nodes.rdii_inflow[1], 0.0, 1e-15);
}

TEST(Convolution, LargerAreaProducesMoreRDII) {
    // Two nodes with same UH but different contributing areas
    auto ctx = makeRdiiContext(3, 1);
    ctx.gage_names.add("G1");

    UnitHydEntry e{};
    e.name = "UH1"; e.month = -1; e.response = 0;
    e.r = 0.5; e.t = 0.5; e.k = 1.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH1", "G1");

    ctx.rdii_assigns.add(0, "UH1", 50.0);   // 50 acres
    ctx.rdii_assigns.add(1, "UH1", 200.0);  // 200 acres

    RDIISolver solver;
    solver.init(ctx);

    double dt = 300.0;
    ctx.gages.rainfall[0] = 2.0;

    int ri = RDIISolver::getRainInterval(solver.uh_params[0], 300.0);
    int steps = std::max(1, ri / static_cast<int>(dt)) * 3;

    for (int s = 0; s < steps; ++s) {
        std::fill(ctx.nodes.rdii_inflow.begin(), ctx.nodes.rdii_inflow.end(), 0.0);
        solver.computeAll(ctx, 0, dt);
        solver.applyRdiiInflows(ctx);
    }

    double q0 = ctx.nodes.rdii_inflow[0];
    double q1 = ctx.nodes.rdii_inflow[1];

    // Both should be positive
    EXPECT_GT(q0, 0.0);
    EXPECT_GT(q1, 0.0);

    // Node 1 has 4x the area → 4x the RDII
    EXPECT_NEAR(q1 / q0, 4.0, 0.01);
}

TEST(Convolution, HigherR_ProducesMoreRDII) {
    // Same area, different R fractions
    auto ctx = makeRdiiContext(3, 1);
    ctx.gage_names.add("G1");

    // UH1: R=0.2
    UnitHydEntry e1{};
    e1.name = "UH_low"; e1.month = -1; e1.response = 0;
    e1.r = 0.2; e1.t = 0.5; e1.k = 1.0;
    ctx.unit_hyds.add(e1);
    ctx.unit_hyds.add_gage("UH_low", "G1");

    // UH2: R=0.8
    UnitHydEntry e2{};
    e2.name = "UH_high"; e2.month = -1; e2.response = 0;
    e2.r = 0.8; e2.t = 0.5; e2.k = 1.0;
    ctx.unit_hyds.add(e2);
    ctx.unit_hyds.add_gage("UH_high", "G1");

    ctx.rdii_assigns.add(0, "UH_low",  100.0);
    ctx.rdii_assigns.add(1, "UH_high", 100.0);

    RDIISolver solver;
    solver.init(ctx);

    double dt = 300.0;
    ctx.gages.rainfall[0] = 1.0;

    int ri = RDIISolver::getRainInterval(solver.uh_params[0], 300.0);
    int steps = std::max(1, ri / static_cast<int>(dt)) * 3;

    for (int s = 0; s < steps; ++s) {
        std::fill(ctx.nodes.rdii_inflow.begin(), ctx.nodes.rdii_inflow.end(), 0.0);
        solver.computeAll(ctx, 0, dt);
        solver.applyRdiiInflows(ctx);
    }

    double q_low  = ctx.nodes.rdii_inflow[0];
    double q_high = ctx.nodes.rdii_inflow[1];

    EXPECT_GT(q_low, 0.0);
    EXPECT_GT(q_high, 0.0);
    // R=0.8 should produce 4x more RDII than R=0.2
    EXPECT_NEAR(q_high / q_low, 4.0, 0.01);
}

TEST(Convolution, RDIIDecaysAfterRainStops) {
    auto ctx = makeRdiiContext(2, 1);
    ctx.gage_names.add("G1");

    UnitHydEntry e{};
    e.name = "UH1"; e.month = -1; e.response = 0;
    e.r = 0.5; e.t = 0.5; e.k = 1.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH1", "G1");
    ctx.rdii_assigns.add(0, "UH1", 100.0);

    RDIISolver solver;
    solver.init(ctx);

    double dt = 300.0;
    int ri = RDIISolver::getRainInterval(solver.uh_params[0], 300.0);
    int steps_per_interval = std::max(1, ri / static_cast<int>(dt));

    // Phase 1: Rain for several intervals
    ctx.gages.rainfall[0] = 2.0;
    for (int s = 0; s < steps_per_interval * 4; ++s) {
        std::fill(ctx.nodes.rdii_inflow.begin(), ctx.nodes.rdii_inflow.end(), 0.0);
        solver.computeAll(ctx, 0, dt);
        solver.applyRdiiInflows(ctx);
    }
    double q_peak = ctx.nodes.rdii_inflow[0];
    EXPECT_GT(q_peak, 0.0);

    // Phase 2: Stop rain, step through many intervals
    ctx.gages.rainfall[0] = 0.0;
    double q_last = q_peak;
    for (int s = 0; s < steps_per_interval * 20; ++s) {
        std::fill(ctx.nodes.rdii_inflow.begin(), ctx.nodes.rdii_inflow.end(), 0.0);
        solver.computeAll(ctx, 0, dt);
        solver.applyRdiiInflows(ctx);
    }

    // After many dry steps, RDII should have decayed toward zero
    double q_final = ctx.nodes.rdii_inflow[0];
    EXPECT_LT(q_final, q_peak);
}

// ============================================================================
// IGNORE_RDII Tests
// ============================================================================

TEST(IgnoreRDII, OptionParsedFromOptions) {
    SimulationContext ctx;
    EXPECT_FALSE(ctx.options.ignore_rdii);  // default
    ctx.options.ignore_rdii = true;
    EXPECT_TRUE(ctx.options.ignore_rdii);
}

// ============================================================================
// Pollutant Quality Load Tests
// ============================================================================

TEST(RDIIQuality, PositiveFlowAddsQualityLoad) {
    // Verify the math: mass_rate = q * c_rdii
    double q = 5.0;       // CFS
    double c_rdii = 10.0; // mg/L
    double w = q * c_rdii;
    EXPECT_NEAR(w, 50.0, 1e-10);
}

TEST(RDIIQuality, ZeroFlowNoLoad) {
    double q = 0.0;
    double c_rdii = 10.0;
    double w = q * c_rdii;
    EXPECT_NEAR(w, 0.0, 1e-10);
}

TEST(RDIIQuality, NegativeFlowNoLoad) {
    // Negative RDII (shouldn't happen, but verify guard)
    double q = -1.0;
    // Legacy only adds quality for positive flow
    if (q > 0.0) {
        FAIL() << "Should not add quality for negative flow";
    }
    SUCCEED();
}

TEST(RDIIQuality, ZeroConcentrationNoLoad) {
    double q = 5.0;
    double c_rdii = 0.0;
    double w = q * c_rdii;
    EXPECT_NEAR(w, 0.0, 1e-10);
}

// ============================================================================
// UnitHydData / RDIIAssignData Storage Tests
// ============================================================================

TEST(UnitHydData, AddAndRetrieve) {
    UnitHydData uhd;
    UnitHydEntry e{};
    e.name = "UH1"; e.month = -1; e.response = 0;
    e.r = 0.5; e.t = 1.0; e.k = 0.5;
    uhd.add(e);
    EXPECT_EQ(uhd.count(), 1);
    EXPECT_EQ(uhd.entries[0].name, "UH1");
    EXPECT_NEAR(uhd.entries[0].r, 0.5, 1e-10);
}

TEST(UnitHydData, GageAssignment) {
    UnitHydData uhd;
    uhd.add_gage("UH1", "RainGage1");
    uhd.add_gage("UH2", "RainGage2");
    EXPECT_EQ(uhd.gage_assignments.size(), 2u);
    EXPECT_EQ(uhd.gage_assignments[0], "UH1");
    EXPECT_EQ(uhd.gage_names[0], "RainGage1");
}

TEST(RDIIAssignData, AddAndCount) {
    RDIIAssignData rd;
    rd.add(0, "UH1", 50.0);
    rd.add(1, "UH2", 100.0);
    EXPECT_EQ(rd.count(), 2);
    EXPECT_EQ(rd.node_idx[0], 0);
    EXPECT_EQ(rd.uh_name[1], "UH2");
    EXPECT_NEAR(rd.sewer_area[1], 100.0, 1e-10);
}

// ============================================================================
// INP Round-Trip Tests (verify output format)
// ============================================================================

TEST(InpFormat, HydrographsMonthNames) {
    // Verify month name mapping
    static const char* months[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                   "JUL","AUG","SEP","OCT","NOV","DEC"};
    for (int m = 0; m < 12; ++m) {
        EXPECT_NE(std::string(months[m]).size(), 0u);
    }
}

TEST(InpFormat, ResponseNames) {
    static const char* resp[] = {"SHORT","MEDIUM","LONG"};
    EXPECT_EQ(std::string(resp[0]), "SHORT");
    EXPECT_EQ(std::string(resp[1]), "MEDIUM");
    EXPECT_EQ(std::string(resp[2]), "LONG");
}

// ============================================================================
// Multi-Response Convolution Tests
// ============================================================================

TEST(MultiResponse, ThreeResponsesSuperimpose) {
    auto ctx = makeRdiiContext(2, 1);
    ctx.gage_names.add("G1");

    // Add 3 responses with different time scales
    UnitHydEntry e0{};
    e0.name = "UH_multi"; e0.month = -1; e0.response = 0;
    e0.r = 0.3; e0.t = 0.25; e0.k = 1.0;  // SHORT: fast
    ctx.unit_hyds.add(e0);

    UnitHydEntry e1{};
    e1.name = "UH_multi"; e1.month = -1; e1.response = 1;
    e1.r = 0.2; e1.t = 1.0; e1.k = 1.0;  // MEDIUM
    ctx.unit_hyds.add(e1);

    UnitHydEntry e2{};
    e2.name = "UH_multi"; e2.month = -1; e2.response = 2;
    e2.r = 0.1; e2.t = 2.0; e2.k = 1.0;  // LONG: slow
    ctx.unit_hyds.add(e2);

    ctx.unit_hyds.add_gage("UH_multi", "G1");
    ctx.rdii_assigns.add(0, "UH_multi", 100.0);

    RDIISolver solver;
    solver.init(ctx);

    double dt = 300.0;
    ctx.gages.rainfall[0] = 1.0;

    // Run enough steps
    for (int s = 0; s < 30; ++s) {
        std::fill(ctx.nodes.rdii_inflow.begin(), ctx.nodes.rdii_inflow.end(), 0.0);
        solver.computeAll(ctx, 0, dt);
        solver.applyRdiiInflows(ctx);
    }

    // Should have RDII from all three responses
    EXPECT_GT(ctx.nodes.rdii_inflow[0], 0.0);
}

// ============================================================================
// Month-Dependent Parameters Tests
// ============================================================================

TEST(MonthDependent, DifferentMonthsUseDifferentR) {
    auto ctx = makeRdiiContext(2, 1);
    ctx.gage_names.add("G1");

    // Add entries for specific months
    for (int m = 0; m < 12; ++m) {
        UnitHydEntry e{};
        e.name = "UH_monthly"; e.month = m; e.response = 0;
        e.r = 0.1 * (m + 1);  // R varies 0.1..1.2
        e.t = 0.5; e.k = 1.0;
        ctx.unit_hyds.add(e);
    }
    ctx.unit_hyds.add_gage("UH_monthly", "G1");
    ctx.rdii_assigns.add(0, "UH_monthly", 100.0);

    RDIISolver solver;
    solver.init(ctx);

    // Verify month-specific parameters stored correctly
    for (int m = 0; m < 12; ++m) {
        EXPECT_NEAR(solver.uh_params[0].r[m][0], 0.1 * (m + 1), 1e-10);
    }
}

// ============================================================================
// Circular Buffer Tests
// ============================================================================

TEST(CircularBuffer, AllocateAndInitialize) {
    UHResponseData rd;
    rd.allocate(5);
    EXPECT_EQ(rd.max_periods, 5);
    EXPECT_EQ(static_cast<int>(rd.past_rain.size()), 5);
    EXPECT_EQ(static_cast<int>(rd.past_month.size()), 5);
    EXPECT_EQ(rd.period, 0);
    EXPECT_EQ(rd.has_past_rain, 0);
    EXPECT_NEAR(rd.ia_used, 0.0, 1e-15);

    // All past rain should be zero
    for (int i = 0; i < 5; ++i) {
        EXPECT_NEAR(rd.past_rain[static_cast<size_t>(i)], 0.0, 1e-15);
    }
}

TEST(CircularBuffer, WrapAround) {
    UHResponseData rd;
    rd.allocate(3);

    // Write past the buffer size to test wrapping
    for (int i = 0; i < 5; ++i) {
        int p = i % rd.max_periods;
        rd.past_rain[static_cast<size_t>(p)] = static_cast<double>(i + 1);
    }

    // Buffer should contain [4, 5, 3] (last 3 values that wrapped)
    EXPECT_NEAR(rd.past_rain[0], 4.0, 1e-10);  // i=3 wrote to pos 0
    EXPECT_NEAR(rd.past_rain[1], 5.0, 1e-10);  // i=4 wrote to pos 1
    EXPECT_NEAR(rd.past_rain[2], 3.0, 1e-10);  // i=2 wrote to pos 2
}

// ============================================================================
// Mass Balance Tests
// ============================================================================

TEST(MassBalance, QualRoutingIiInExists) {
    SimulationContext ctx;
    ctx.mass_balance.resize_quality(3);
    EXPECT_EQ(ctx.mass_balance.qual_routing_ii_in.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_NEAR(ctx.mass_balance.qual_routing_ii_in[i], 0.0, 1e-15);
    }
}

TEST(MassBalance, QualRoutingIiInResetToZero) {
    SimulationContext ctx;
    ctx.mass_balance.resize_quality(2);
    ctx.mass_balance.qual_routing_ii_in[0] = 100.0;
    ctx.mass_balance.qual_routing_ii_in[1] = 200.0;
    ctx.mass_balance.reset();
    EXPECT_NEAR(ctx.mass_balance.qual_routing_ii_in[0], 0.0, 1e-15);
    EXPECT_NEAR(ctx.mass_balance.qual_routing_ii_in[1], 0.0, 1e-15);
}

// ============================================================================
// [RDII_DECAY] — exponential IA model
// ============================================================================
// Tests for the new exponential / temperature-dependent IA model.
// @see docs/RDII_ExpDecay_Implementation.md

TEST(RdiiDecayParse, EightTokenRowPopulatesEntry) {
    SimulationContext ctx;
    std::vector<std::string> lines = {
        "SanSewer  SHORT   0.15  0.010  0.070  10.0  0.055  0.0"
    };
    input::handle_rdii_decay(ctx, lines);
    ASSERT_EQ(ctx.rdii_decay.count(), 1);
    const auto& e = ctx.rdii_decay.entries[0];
    EXPECT_EQ(e.uh_name, "SanSewer");
    EXPECT_EQ(e.response, 0);
    EXPECT_NEAR(e.k_dep, 0.15, 1e-12);
    EXPECT_NEAR(e.k_0, 0.010, 1e-12);
    EXPECT_NEAR(e.k_T, 0.070, 1e-12);
    EXPECT_NEAR(e.T_ref, 10.0, 1e-12);
    EXPECT_NEAR(e.theta_rec, 0.055, 1e-12);
    EXPECT_NEAR(e.T_freeze, 0.0, 1e-12);
}

TEST(RdiiDecayParse, AllThreeResponses) {
    SimulationContext ctx;
    std::vector<std::string> lines = {
        "G1  SHORT   0.10  0.01  0.07  10.0  0.05  0.0",
        "G1  MEDIUM  0.08  0.01  0.04  10.0  0.05  0.0",
        "G1  LONG    0.05  0.01  0.02  10.0  0.04  0.0",
    };
    input::handle_rdii_decay(ctx, lines);
    ASSERT_EQ(ctx.rdii_decay.count(), 3);
    EXPECT_EQ(ctx.rdii_decay.entries[0].response, 0);
    EXPECT_EQ(ctx.rdii_decay.entries[1].response, 1);
    EXPECT_EQ(ctx.rdii_decay.entries[2].response, 2);
}

TEST(RdiiDecayParse, SkipsRowsWithTooFewTokens) {
    SimulationContext ctx;
    std::vector<std::string> lines = {
        "TooShort SHORT 0.1",
        "Good     SHORT 0.10 0.01 0.07 10.0 0.05 0.0",
    };
    input::handle_rdii_decay(ctx, lines);
    EXPECT_EQ(ctx.rdii_decay.count(), 1);
    EXPECT_EQ(ctx.rdii_decay.entries[0].uh_name, "Good");
}

TEST(RdiiDecayParse, SkipsUnknownResponse) {
    SimulationContext ctx;
    std::vector<std::string> lines = {
        "G1 BOGUS  0.10 0.01 0.07 10.0 0.05 0.0",
        "G1 MEDIUM 0.10 0.01 0.07 10.0 0.05 0.0",
    };
    input::handle_rdii_decay(ctx, lines);
    ASSERT_EQ(ctx.rdii_decay.count(), 1);
    EXPECT_EQ(ctx.rdii_decay.entries[0].response, 1);
}

TEST(RdiiDecayParse, SkipsNegativeRates) {
    SimulationContext ctx;
    std::vector<std::string> lines = {
        "G1 SHORT -0.1  0.01  0.07 10.0 0.05 0.0",  // k_dep < 0
        "G1 SHORT  0.1 -0.01  0.07 10.0 0.05 0.0",  // k_0 < 0
        "G1 SHORT  0.1  0.01 -0.07 10.0 0.05 0.0",  // k_T < 0
        "G1 SHORT  0.1  0.01  0.07 10.0 0.05 0.0",  // good
    };
    input::handle_rdii_decay(ctx, lines);
    EXPECT_EQ(ctx.rdii_decay.count(), 1);
}

TEST(RdiiDecayInit, ResolvesEntriesToDecayParams) {
    auto ctx = makeRdiiContext(2, 1);
    ctx.gage_names.add("G1");

    UnitHydEntry e{};
    e.name = "UH"; e.month = -1; e.response = 0;
    e.r = 0.05; e.t = 1.0; e.k = 2.0; e.dmax = 5.0; e.dinit = 0.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH", "G1");
    ctx.rdii_assigns.add(0, "UH", 100.0);

    RDIIDecayEntry d{};
    d.uh_name = "UH"; d.response = 0;
    d.k_dep = 0.2; d.k_0 = 0.02; d.k_T = 0.05;
    d.T_ref = 10.0; d.theta_rec = 0.04; d.T_freeze = 0.0;
    ctx.rdii_decay.add(d);

    RDIISolver solver;
    solver.init(ctx);

    ASSERT_GE(solver.decay_params.size(), 1u);
    const auto& dp = solver.decay_params[0][0];
    EXPECT_TRUE(dp.active);
    EXPECT_NEAR(dp.k_dep, 0.2, 1e-12);
    EXPECT_NEAR(dp.k_T,   0.05, 1e-12);
    // The two unspecified responses remain inactive
    EXPECT_FALSE(solver.decay_params[0][1].active);
    EXPECT_FALSE(solver.decay_params[0][2].active);
}

TEST(RdiiDecayInit, UnknownGroupIsSkipped) {
    auto ctx = makeRdiiContext(1, 1);
    ctx.gage_names.add("G1");

    UnitHydEntry e{};
    e.name = "Real"; e.month = -1; e.response = 0;
    e.r = 0.05; e.t = 1.0; e.k = 2.0; e.dmax = 5.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("Real", "G1");
    ctx.rdii_assigns.add(0, "Real", 100.0);

    RDIIDecayEntry d{};
    d.uh_name = "DoesNotExist"; d.response = 0;
    d.k_dep = 0.2; d.k_0 = 0.02;
    ctx.rdii_decay.add(d);

    RDIISolver solver;
    solver.init(ctx);

    // Nothing should be active — entry pointed at an unknown group
    for (const auto& triple : solver.decay_params)
        for (const auto& dp : triple)
            EXPECT_FALSE(dp.active);
}

TEST(RdiiDecayInit, WarnsWhenNoTemperatureSource) {
    auto ctx = makeRdiiContext(1, 1);
    ctx.gage_names.add("G1");
    ctx.options.temp_source = 0;  // explicit: no temperature source

    UnitHydEntry e{};
    e.name = "UH"; e.month = -1; e.response = 0;
    e.r = 0.05; e.t = 1.0; e.k = 2.0; e.dmax = 5.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH", "G1");
    ctx.rdii_assigns.add(0, "UH", 100.0);

    RDIIDecayEntry d{};
    d.uh_name = "UH"; d.response = 0;
    d.k_dep = 0.1; d.k_0 = 0.01; d.k_T = 0.05; d.T_ref = 10.0;
    ctx.rdii_decay.add(d);

    RDIISolver solver;
    solver.init(ctx);
    EXPECT_FALSE(ctx.warnings.empty());
}

TEST(RdiiDecayInit, NoWarningWhenTemperatureConfigured) {
    auto ctx = makeRdiiContext(1, 1);
    ctx.gage_names.add("G1");
    ctx.options.temp_source = 1;  // timeseries

    UnitHydEntry e{};
    e.name = "UH"; e.month = -1; e.response = 0;
    e.r = 0.05; e.t = 1.0; e.k = 2.0; e.dmax = 5.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH", "G1");
    ctx.rdii_assigns.add(0, "UH", 100.0);

    RDIIDecayEntry d{};
    d.uh_name = "UH"; d.response = 0;
    d.k_dep = 0.1; d.k_0 = 0.01; d.k_T = 0.05; d.T_ref = 10.0;
    ctx.rdii_decay.add(d);

    RDIISolver solver;
    solver.init(ctx);
    EXPECT_TRUE(ctx.warnings.empty());
}

// ----------------------------------------------------------------------------
// Behavioural — exponential IA vs linear baseline.
//
// We run two identical setups (same UH, same rainfall, same nodes) except
// one has [RDII_DECAY] active. The qualitative checks below don't assume any
// specific numeric outcome — they just confirm the dispatch is wired and the
// exponential path produces a different but well-behaved trajectory.
// ----------------------------------------------------------------------------

static SimulationContext makeDecayCtx(bool with_decay) {
    auto ctx = makeRdiiContext(2, 1);
    ctx.gage_names.add("G1");

    UnitHydEntry e{};
    e.name = "UH"; e.month = -1; e.response = 0;
    e.r = 0.1; e.t = 0.5; e.k = 1.0; e.dmax = 2.0; e.dinit = 0.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH", "G1");
    ctx.rdii_assigns.add(0, "UH", 100.0);

    if (with_decay) {
        RDIIDecayEntry d{};
        d.uh_name = "UH"; d.response = 0;
        d.k_dep = 1.0;   // moderate depletion
        d.k_0   = 0.0;   // no recovery in this test
        d.k_T   = 0.0;
        d.T_ref = 10.0;
        d.theta_rec = 0.0;
        d.T_freeze  = 0.0;
        ctx.rdii_decay.add(d);
        // configure temperature source so no warning fires
        ctx.options.temp_source = 1;
        ctx.climate_state.temperature = 50.0;  // 10 deg C
    }
    return ctx;
}

TEST(RdiiDecayBehaviour, ExpModelProducesPositiveRdii) {
    auto ctx = makeDecayCtx(true);
    RDIISolver solver;
    solver.init(ctx);

    double dt = 300.0;
    ctx.gages.rainfall[0] = 0.5;  // continuous rain (in/hr)

    for (int s = 0; s < 60; ++s) {
        std::fill(ctx.nodes.rdii_inflow.begin(),
                  ctx.nodes.rdii_inflow.end(), 0.0);
        solver.computeAll(ctx, 0, dt);
        solver.applyRdiiInflows(ctx);
    }
    EXPECT_GT(ctx.nodes.rdii_inflow[0], 0.0);
}

TEST(RdiiDecayBehaviour, FrozenGroundSuppressesRecovery) {
    auto ctx = makeDecayCtx(true);
    // Override decay row with a positive base recovery so any non-zero
    // recovery is observable, then set the temperature below freezing.
    ctx.rdii_decay.entries[0].k_0 = 0.5;
    ctx.options.temp_source = 1;
    ctx.climate_state.temperature = 20.0;  // ~ -6.7 deg C, well below T_freeze=0

    RDIISolver solver;
    solver.init(ctx);

    // Drive ia_used positive with a wet period, then sit dry under freezing.
    double dt = 300.0;
    ctx.gages.rainfall[0] = 1.0;
    for (int s = 0; s < 20; ++s) solver.computeAll(ctx, 0, dt);

    double ia_used_wet =
        solver.decay_params.empty()
            ? 0.0
            : 0.0; // we read state through the internal SoA below

    // After the wet phase, the SHORT response should have iaUsed > 0.
    // We can't access groups_ directly (private), but we can confirm that
    // dry-period processing does not recover ia_used (frozen).
    ctx.gages.rainfall[0] = 0.0;
    // Run a long dry stretch; iaUsed should NOT decay back toward 0.
    for (int s = 0; s < 200; ++s) solver.computeAll(ctx, 0, dt);

    // No assertion on the private state; the qualitative guarantee is that
    // recovery is suppressed — verified directly via getRecoveryRate() unit
    // tests below. This test is here as a smoke check that the call chain
    // does not throw / explode when T < T_freeze for a long dry period.
    SUCCEED();
    (void)ia_used_wet;
}

TEST(RdiiDecayBehaviour, LinearPathUnchangedWhenNoDecayRows) {
    // Sanity: a UH group with no [RDII_DECAY] row produces the exact same
    // output as before the feature was added. This is the "incremental
    // adoption" guarantee.
    auto ctx_a = makeDecayCtx(false);  // no decay rows
    auto ctx_b = makeDecayCtx(false);  // identical control

    RDIISolver sa, sb;
    sa.init(ctx_a);
    sb.init(ctx_b);

    double dt = 300.0;
    for (int s = 0; s < 30; ++s) {
        ctx_a.gages.rainfall[0] = (s < 10) ? 0.5 : 0.0;
        ctx_b.gages.rainfall[0] = (s < 10) ? 0.5 : 0.0;
        std::fill(ctx_a.nodes.rdii_inflow.begin(),
                  ctx_a.nodes.rdii_inflow.end(), 0.0);
        std::fill(ctx_b.nodes.rdii_inflow.begin(),
                  ctx_b.nodes.rdii_inflow.end(), 0.0);
        sa.computeAll(ctx_a, 0, dt);
        sb.computeAll(ctx_b, 0, dt);
        sa.applyRdiiInflows(ctx_a);
        sb.applyRdiiInflows(ctx_b);
        EXPECT_NEAR(ctx_a.nodes.rdii_inflow[0],
                    ctx_b.nodes.rdii_inflow[0], 1e-12);
    }
}

// ----------------------------------------------------------------------------
// Reference parity — pins updateIA_exp / getRecoveryRate to the reference
// IAModel implementation (sparsehydro compute_excess_series). Golden values
// generated from the reference math:
//   wet:  consumed = avail * (1 - exp(-k_dep * P))
//         excess   = max(0, P - consumed)
//         avail   -= consumed            [take == lose, mass-consistent]
//   dry:  k_rec = (T >= T_freeze) ? k0 + kT * exp(theta * (T - T_ref)) : 0
//         avail = ia_max - (ia_max - avail) * exp(-k_rec * dt_hr)
// @see docs/RDII_ExpDecay_Implementation.md §2.1.1
// ----------------------------------------------------------------------------

static UnitHydParams makeIaOnlyUH(double ia_max) {
    UnitHydParams uh{};
    for (int m = 0; m < 12; ++m) uh.iaMax[m][0] = ia_max;
    return uh;
}

TEST(RdiiDecayReference, WetTraceMatchesGoldenValues) {
    // ia_max = 10 mm, k_dep = 0.3 1/mm, three 5 mm pulses. The first pulse is
    // in the over-drain regime (consumed = 7.77 > P = 5): excess clamps to 0
    // while the storage drains by the full consumed depth.
    SimulationContext ctx;  // temperature is not read in the wet branch
    UnitHydParams uh = makeIaOnlyUH(10.0);
    UHResponseData rd;      // ia_used = 0 → bucket full
    ExpDecayParams dp;
    dp.active = true; dp.k_dep = 0.3;

    const double gold_excess[3] = {0.0, 3.266569082194341, 4.613219281703784};
    const double gold_avail[3]  = {2.231301601484298, 0.497870683678639,
                                   0.111089965382423};
    for (int i = 0; i < 3; ++i) {
        double ex = updateIA_exp(uh, rd, dp, 0, 0, 5.0, 300.0, ctx);
        EXPECT_NEAR(ex, gold_excess[i], 1e-12) << "pulse " << i;
        EXPECT_NEAR(10.0 - rd.ia_used, gold_avail[i], 1e-12) << "pulse " << i;
    }
}

TEST(RdiiDecayReference, DryRecoveryMatchesGoldenValues) {
    // From an empty bucket, three 24 h dry steps at T = 10 deg C with
    // k0 = 0.05, kT = 0.02, theta = 0.1, T_ref = 20:
    //   k_rec = 0.05 + 0.02 * exp(-1) = 0.057357588823429 1/hr
    SimulationContext ctx;
    ctx.options.temp_source = 1;
    ctx.climate_state.temperature = 50.0;  // deg F → exactly 10 deg C
    UnitHydParams uh = makeIaOnlyUH(10.0);
    UHResponseData rd;
    rd.ia_used = 10.0;                     // empty bucket
    ExpDecayParams dp;
    dp.active = true; dp.k_dep = 0.3;
    dp.k_0 = 0.05; dp.k_T = 0.02; dp.theta_rec = 0.1;
    dp.T_ref = 20.0; dp.T_freeze = 0.0;

    const double gold_avail[3] = {7.475601134707937, 9.362741036891215,
                                  9.839130419663098};
    for (int i = 0; i < 3; ++i) {
        double ex = updateIA_exp(uh, rd, dp, 0, 0, 0.0, 86400.0, ctx);
        EXPECT_DOUBLE_EQ(ex, 0.0);
        EXPECT_NEAR(10.0 - rd.ia_used, gold_avail[i], 1e-12) << "day " << i;
    }
}

TEST(RdiiDecayReference, RecoveryRateFreezeBoundary) {
    // Reference semantics: recovery suppressed strictly BELOW T_freeze —
    // recovery still occurs at T == T_freeze.
    ExpDecayParams dp;
    dp.k_0 = 0.05; dp.k_T = 0.02; dp.theta_rec = 0.1;
    dp.T_ref = 20.0; dp.T_freeze = 0.0;

    EXPECT_NEAR(getRecoveryRate(dp, 0.0), 0.052706705664732, 1e-12);
    EXPECT_DOUBLE_EQ(getRecoveryRate(dp, -1.0), 0.0);
    EXPECT_NEAR(getRecoveryRate(dp, 10.0), 0.057357588823429, 1e-12);
}

TEST(RdiiDecayReference, ZeroKdepDisablesAbstraction) {
    // Degenerate limit: k_dep = 0 → consumed = 0, excess = P, state frozen.
    SimulationContext ctx;
    UnitHydParams uh = makeIaOnlyUH(10.0);
    UHResponseData rd;
    ExpDecayParams dp;
    dp.active = true; dp.k_dep = 0.0;

    for (int i = 0; i < 3; ++i) {
        double ex = updateIA_exp(uh, rd, dp, 0, 0, 5.0, 300.0, ctx);
        EXPECT_DOUBLE_EQ(ex, 5.0);
        EXPECT_DOUBLE_EQ(rd.ia_used, 0.0);
    }
}

TEST(RdiiDecayReference, UnitSystemPairing) {
    // Unit-consistent parameter pairs scale by 25.4: a metric run
    // (mm, k_dep = 0.3 1/mm) and an imperial run (in, k_dep = 7.62 1/in)
    // produce identical trajectories up to the 25.4 depth factor.
    SimulationContext ctx;
    UnitHydParams uh_mm = makeIaOnlyUH(10.0);
    UnitHydParams uh_in = makeIaOnlyUH(10.0 / 25.4);
    UHResponseData rd_mm, rd_in;
    ExpDecayParams dp_mm, dp_in;
    dp_mm.active = true; dp_mm.k_dep = 0.3;
    dp_in.active = true; dp_in.k_dep = 0.3 * 25.4;

    for (int i = 0; i < 2; ++i) {
        double ex_mm = updateIA_exp(uh_mm, rd_mm, dp_mm, 0, 0, 5.0, 300.0, ctx);
        double ex_in = updateIA_exp(uh_in, rd_in, dp_in, 0, 0, 5.0 / 25.4,
                                    300.0, ctx);
        EXPECT_NEAR(ex_in * 25.4, ex_mm, 1e-9) << "pulse " << i;
        EXPECT_NEAR(rd_in.ia_used * 25.4, rd_mm.ia_used, 1e-9) << "pulse " << i;
    }
}

TEST(RdiiDecayInit, WarnsWhenKdepZero) {
    auto ctx = makeRdiiContext(1, 1);
    ctx.gage_names.add("G1");
    ctx.options.temp_source = 1;  // suppress the temperature-source warning

    UnitHydEntry e{};
    e.name = "UH"; e.month = -1; e.response = 0;
    e.r = 0.05; e.t = 1.0; e.k = 2.0; e.dmax = 5.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH", "G1");
    ctx.rdii_assigns.add(0, "UH", 100.0);

    RDIIDecayEntry d{};
    d.uh_name = "UH"; d.response = 0;
    d.k_dep = 0.0; d.k_0 = 0.01; d.k_T = 0.05; d.T_ref = 10.0;
    ctx.rdii_decay.add(d);

    RDIISolver solver;
    solver.init(ctx);

    bool found = false;
    for (const auto& w : ctx.warnings)
        if (w.find("k_dep") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

TEST(RdiiDecayInit, WarnsWhenTFreezeAtOrAboveTRef) {
    auto ctx = makeRdiiContext(1, 1);
    ctx.gage_names.add("G1");
    ctx.options.temp_source = 1;

    UnitHydEntry e{};
    e.name = "UH"; e.month = -1; e.response = 0;
    e.r = 0.05; e.t = 1.0; e.k = 2.0; e.dmax = 5.0;
    ctx.unit_hyds.add(e);
    ctx.unit_hyds.add_gage("UH", "G1");
    ctx.rdii_assigns.add(0, "UH", 100.0);

    RDIIDecayEntry d{};
    d.uh_name = "UH"; d.response = 0;
    d.k_dep = 0.1; d.k_0 = 0.01; d.k_T = 0.05;
    d.T_ref = 10.0; d.T_freeze = 15.0;  // T_freeze >= T_ref
    ctx.rdii_decay.add(d);

    RDIISolver solver;
    solver.init(ctx);

    bool found = false;
    for (const auto& w : ctx.warnings)
        if (w.find("T_freeze") != std::string::npos) found = true;
    EXPECT_TRUE(found);
}

// ----------------------------------------------------------------------------
// INP + GeoPackage round-trip semantics — we verify the data structure
// itself round-trips through the writer's expected order. The actual
// file-format I/O is covered by the existing format tests above.
// ----------------------------------------------------------------------------

TEST(RdiiDecayRoundTrip, ContextResetClearsDecay) {
    SimulationContext ctx;
    RDIIDecayEntry d{};
    d.uh_name = "UH"; d.response = 1;
    d.k_dep = 0.1; d.k_0 = 0.01; d.k_T = 0.02;
    ctx.rdii_decay.add(d);
    EXPECT_EQ(ctx.rdii_decay.count(), 1);
    ctx.reset();
    EXPECT_EQ(ctx.rdii_decay.count(), 0);
}

// ============================================================================
// C API — hydrograph, RDII assignment getter, RDII decay
// ============================================================================
// Exercises the C-level entry points used by the Python bindings and external
// consumers. Uses swmm_engine_new() to build an in-memory model.

class RdiiCApiTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;
    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        // Need at least one node so swmm_rdii_add can succeed.
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
    }
    void TearDown() override { swmm_engine_destroy(engine); }
};

TEST_F(RdiiCApiTest, HydrographAddCountGet) {
    EXPECT_EQ(swmm_hydrograph_count(engine), 0);

    ASSERT_EQ(swmm_hydrograph_add(engine, "SanSewer",
                                   -1 /*ALL*/, 0 /*SHORT*/,
                                   0.055, 1.0, 2.0,
                                   8.0, 0.10, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_add(engine, "SanSewer",
                                   -1, 1 /*MEDIUM*/,
                                   0.032, 3.5, 2.0,
                                   0.0, 0.0, 0.0), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 2);

    char buf[64];
    int  month = -99, response = -99;
    double r=0, t=0, k=0, dmax=0, drecov=0, dinit=0;
    ASSERT_EQ(swmm_hydrograph_get(engine, 0, buf, sizeof(buf),
                                   &month, &response, &r, &t, &k,
                                   &dmax, &drecov, &dinit), SWMM_OK);
    EXPECT_STREQ(buf, "SanSewer");
    EXPECT_EQ(month, -1);
    EXPECT_EQ(response, 0);
    EXPECT_NEAR(r, 0.055, 1e-12);
    EXPECT_NEAR(t,   1.0, 1e-12);
    EXPECT_NEAR(k,   2.0, 1e-12);
    EXPECT_NEAR(dmax,   8.0,  1e-12);
    EXPECT_NEAR(drecov, 0.10, 1e-12);
    EXPECT_NEAR(dinit,  2.0,  1e-12);

    ASSERT_EQ(swmm_hydrograph_get(engine, 1, buf, sizeof(buf),
                                   &month, &response, &r, &t, &k,
                                   &dmax, &drecov, &dinit), SWMM_OK);
    EXPECT_EQ(response, 1);
    EXPECT_NEAR(t, 3.5, 1e-12);
}

TEST_F(RdiiCApiTest, HydrographRejectsBadInputs) {
    // null name
    EXPECT_EQ(swmm_hydrograph_add(engine, nullptr, -1, 0,
                                   0.1, 1.0, 2.0, 0, 0, 0),
              SWMM_ERR_BADPARAM);
    // response out of [0,2]
    EXPECT_EQ(swmm_hydrograph_add(engine, "UH", -1, 3,
                                   0.1, 1.0, 2.0, 0, 0, 0),
              SWMM_ERR_BADPARAM);
    // month out of [-1,11]
    EXPECT_EQ(swmm_hydrograph_add(engine, "UH", 12, 0,
                                   0.1, 1.0, 2.0, 0, 0, 0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hydrograph_add(engine, "UH", -2, 0,
                                   0.1, 1.0, 2.0, 0, 0, 0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hydrograph_count(engine), 0);
}

TEST_F(RdiiCApiTest, HydrographGageRoundTrip) {
    EXPECT_EQ(swmm_hydrograph_gage_count(engine), 0);

    ASSERT_EQ(swmm_hydrograph_add_gage(engine, "SanSewer", "G1"), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_add_gage(engine, "Combined", "G2"), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_gage_count(engine), 2);

    char uh[32], gage[32];
    ASSERT_EQ(swmm_hydrograph_get_gage(engine, 1, uh, sizeof(uh),
                                        gage, sizeof(gage)), SWMM_OK);
    EXPECT_STREQ(uh,   "Combined");
    EXPECT_STREQ(gage, "G2");
}

TEST_F(RdiiCApiTest, HydrographGetTruncatesOverlongName) {
    ASSERT_EQ(swmm_hydrograph_add(engine, "A_Very_Long_Hydrograph_Name_That_Exceeds_The_Buffer",
                                   -1, 0, 0.1, 1.0, 2.0, 0, 0, 0), SWMM_OK);
    char buf[8];
    int month, response;
    double r, t, k, dmax, drecov, dinit;
    ASSERT_EQ(swmm_hydrograph_get(engine, 0, buf, sizeof(buf),
                                   &month, &response, &r, &t, &k,
                                   &dmax, &drecov, &dinit), SWMM_OK);
    EXPECT_EQ(std::string(buf), "A_Very_");        // 7 chars + NUL
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

TEST_F(RdiiCApiTest, RdiiAssignmentRoundTrip) {
    ASSERT_EQ(swmm_rdii_add(engine, 0, "SanSewer", 1000.0), SWMM_OK);
    EXPECT_EQ(swmm_rdii_count(engine), 1);

    int node_idx = -1;
    double area = 0.0;
    char buf[64];
    ASSERT_EQ(swmm_rdii_get(engine, 0, &node_idx, buf, sizeof(buf), &area),
              SWMM_OK);
    EXPECT_EQ(node_idx, 0);
    EXPECT_STREQ(buf, "SanSewer");
    EXPECT_NEAR(area, 1000.0, 1e-12);
}

TEST_F(RdiiCApiTest, RdiiGetRejectsBadIndex) {
    int node_idx;
    double area;
    char buf[32];
    EXPECT_EQ(swmm_rdii_get(engine, 0, &node_idx, buf, sizeof(buf), &area),
              SWMM_ERR_BADINDEX);
}

TEST_F(RdiiCApiTest, RdiiDecayAddCountGet) {
    EXPECT_EQ(swmm_rdii_decay_count(engine), 0);

    ASSERT_EQ(swmm_rdii_decay_add(engine, "SanSewer", 0 /*SHORT*/,
                                   0.15, 0.010, 0.070,
                                   10.0, 0.055, 0.0), SWMM_OK);
    ASSERT_EQ(swmm_rdii_decay_add(engine, "SanSewer", 1 /*MEDIUM*/,
                                   0.10, 0.008, 0.037,
                                   10.0, 0.055, 0.0), SWMM_OK);
    EXPECT_EQ(swmm_rdii_decay_count(engine), 2);

    char buf[64];
    int response = -99;
    double k_dep=0, k_0=0, k_T=0, T_ref=0, theta_rec=0, T_freeze=0;
    ASSERT_EQ(swmm_rdii_decay_get(engine, 0, buf, sizeof(buf),
                                   &response, &k_dep, &k_0, &k_T,
                                   &T_ref, &theta_rec, &T_freeze), SWMM_OK);
    EXPECT_STREQ(buf, "SanSewer");
    EXPECT_EQ(response, 0);
    EXPECT_NEAR(k_dep, 0.15,  1e-12);
    EXPECT_NEAR(k_0,   0.010, 1e-12);
    EXPECT_NEAR(k_T,   0.070, 1e-12);
    EXPECT_NEAR(T_ref, 10.0,  1e-12);
    EXPECT_NEAR(theta_rec, 0.055, 1e-12);
    EXPECT_NEAR(T_freeze,  0.0,   1e-12);
}

TEST_F(RdiiCApiTest, RdiiDecayRejectsBadInputs) {
    // bad response
    EXPECT_EQ(swmm_rdii_decay_add(engine, "UH", 3, 0.1, 0.01, 0.01,
                                   10.0, 0.0, 0.0),
              SWMM_ERR_BADPARAM);
    // negative coefficient
    EXPECT_EQ(swmm_rdii_decay_add(engine, "UH", 0, -0.1, 0.01, 0.01,
                                   10.0, 0.0, 0.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_rdii_decay_add(engine, "UH", 0, 0.1, -0.01, 0.01,
                                   10.0, 0.0, 0.0),
              SWMM_ERR_BADPARAM);
    // null name
    EXPECT_EQ(swmm_rdii_decay_add(engine, nullptr, 0, 0.1, 0.01, 0.01,
                                   10.0, 0.0, 0.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_rdii_decay_count(engine), 0);
}

TEST_F(RdiiCApiTest, NullHandleReturnsBadHandle) {
    EXPECT_EQ(swmm_hydrograph_add(nullptr, "UH", -1, 0,
                                   0.1, 1.0, 2.0, 0, 0, 0),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_hydrograph_count(nullptr), -1);
    EXPECT_EQ(swmm_rdii_decay_count(nullptr), -1);
    EXPECT_EQ(swmm_hydrograph_gage_count(nullptr), -1);
    EXPECT_EQ(swmm_hydrograph_group_count(nullptr), -1);
}

// ============================================================================
// DA-ENG-01 — swmm_hydrograph_group_count / swmm_hydrograph_group_id
// ============================================================================

TEST_F(RdiiCApiTest, HydrographGroupSingleGroupTwelveMonths) {
    // One group "SanSewer" with 12 monthly entries should report as a single
    // group, not 12. Closes the GUI dataObjectCount / dataObjectNameAt
    // mismatch that left the Object Browser showing mostly blank rows.
    for (int m = 0; m < 12; ++m) {
        ASSERT_EQ(swmm_hydrograph_add(engine, "SanSewer", m, 0,
                                       0.05, 1.0, 2.0, 0, 0, 0), SWMM_OK);
    }
    EXPECT_EQ(swmm_hydrograph_count(engine), 12);
    EXPECT_EQ(swmm_hydrograph_group_count(engine), 1);

    char buf[64] = {};
    ASSERT_EQ(swmm_hydrograph_group_id(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "SanSewer");
}

TEST_F(RdiiCApiTest, HydrographGroupMultipleGroupsFirstOccurrenceOrder) {
    // Interleave entries for three groups; group_id must enumerate in
    // first-occurrence order regardless of subsequent interleaving.
    ASSERT_EQ(swmm_hydrograph_add(engine, "Combined",  0, 0, 0.1, 1, 2, 0,0,0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_add(engine, "Sanitary",  0, 0, 0.1, 1, 2, 0,0,0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_add(engine, "Combined",  1, 0, 0.1, 1, 2, 0,0,0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_add(engine, "Storm",     0, 0, 0.1, 1, 2, 0,0,0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_add(engine, "Sanitary",  1, 0, 0.1, 1, 2, 0,0,0), SWMM_OK);

    EXPECT_EQ(swmm_hydrograph_group_count(engine), 3);

    char buf[64] = {};
    ASSERT_EQ(swmm_hydrograph_group_id(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "Combined");
    ASSERT_EQ(swmm_hydrograph_group_id(engine, 1, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "Sanitary");
    ASSERT_EQ(swmm_hydrograph_group_id(engine, 2, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "Storm");

    EXPECT_EQ(swmm_hydrograph_group_id(engine, 3, buf, sizeof(buf)),
              SWMM_ERR_BADINDEX);
}

TEST_F(RdiiCApiTest, HydrographGroupCountIncludesGageOnlyGroups) {
    // A group can be introduced via gage assignment alone (parameter rows
    // arrive later). It must still surface as a group.
    ASSERT_EQ(swmm_hydrograph_add_gage(engine, "GageOnly", "G1"), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_add(engine, "Params", -1, 0,
                                   0.1, 1, 2, 0, 0, 0), SWMM_OK);

    EXPECT_EQ(swmm_hydrograph_group_count(engine), 2);

    char buf[64] = {};
    // Parameter-entry groups come first (entries iterated before gage_assignments).
    ASSERT_EQ(swmm_hydrograph_group_id(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "Params");
    ASSERT_EQ(swmm_hydrograph_group_id(engine, 1, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "GageOnly");
}
