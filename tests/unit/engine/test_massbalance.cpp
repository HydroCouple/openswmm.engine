/**
 * @file test_massbalance.cpp
 * @brief Deterministic continuity-fixture tests for the public mass-balance API.
 *
 * @details These tests exercise the C API accessors against a fresh engine
 *          context populated with known bookkeeping totals. They provide a
 *          closed-system verification surface for runoff, routing, and quality
 *          continuity without depending on a full simulation.
 *
 *          Also contains end-to-end forced-lateral-inflow continuity tests
 *          (issue #113): model inputs/outputs land under ./massbalance_out/
 *          (working dir is tests/unit/engine/data) for review — no temp
 *          files (project convention, CLAUDE.md §4.1).
 *
 * @see include/openswmm/engine/openswmm_massbalance.h
 * @see src/engine/core/openswmm_massbalance_impl.cpp
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_forcing.h>
#include <openswmm/engine/openswmm_massbalance.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>

#include "core/SWMMEngine.hpp"

namespace {

static openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

class MassBalanceApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }

    void TearDown() override {
        if (engine_ != nullptr) {
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    openswmm::SimulationContext::MassBalance& mb() {
        return as_cpp_engine(engine_).context().mass_balance;
    }

    SWMM_Engine engine_ = nullptr;
};

}  // namespace

TEST_F(MassBalanceApiTest, RunoffClosedSystemReportsZeroError) {
    auto& mass_balance = mb();
    mass_balance.runoff_rainfall = 100.0;
    mass_balance.runoff_init_store = 20.0;
    mass_balance.runoff_evap = 10.0;
    mass_balance.runoff_infil = 15.0;
    mass_balance.runoff_runoff = 70.0;
    mass_balance.runoff_final_store = 25.0;

    double error = -1.0;
    ASSERT_EQ(swmm_get_runoff_continuity_error(engine_, &error), SWMM_OK);
    EXPECT_NEAR(error, 0.0, 1e-12);
}

TEST_F(MassBalanceApiTest, RoutingClosedSystemReportsZeroError) {
    auto& mass_balance = mb();
    mass_balance.routing_dry_weather = 20.0;
    mass_balance.routing_wet_weather = 30.0;
    mass_balance.routing_gw_inflow = 10.0;
    mass_balance.routing_rdii = 5.0;
    mass_balance.routing_external = 15.0;
    mass_balance.routing_init_storage = 20.0;

    mass_balance.routing_flooding = 8.0;
    mass_balance.routing_outflow = 60.0;
    mass_balance.routing_evap_loss = 4.0;
    mass_balance.routing_seep_loss = 3.0;
    mass_balance.routing_final_storage = 25.0;

    double error = -1.0;
    ASSERT_EQ(swmm_get_routing_continuity_error(engine_, &error), SWMM_OK);
    EXPECT_NEAR(error, 0.0, 1e-12);
}

TEST_F(MassBalanceApiTest, QualityClosedSystemReportsZeroError) {
    auto& mass_balance = mb();
    mass_balance.resize_quality(1);
    mass_balance.qual_wet_deposition[0] = 10.0;
    mass_balance.qual_runoff_load[0] = 30.0;
    mass_balance.qual_routing_wet[0] = 40.0;
    mass_balance.qual_routing_ii_in[0] = 20.0;
    mass_balance.qual_routing_init[0] = 5.0;

    mass_balance.qual_routing_outflow[0] = 70.0;
    mass_balance.qual_routing_flood[0] = 20.0;
    mass_balance.qual_routing_reacted[0] = 10.0;
    mass_balance.qual_routing_final[0] = 5.0;

    double error = -1.0;
    ASSERT_EQ(swmm_get_quality_continuity_error(engine_, 0, &error), SWMM_OK);
    EXPECT_NEAR(error, 0.0, 1e-12);
}

TEST_F(MassBalanceApiTest, RunoffKnownImbalanceMatchesExpected) {
    auto& mass_balance = mb();
    mass_balance.runoff_rainfall = 100.0;
    mass_balance.runoff_init_store = 20.0;
    mass_balance.runoff_evap = 10.0;
    mass_balance.runoff_infil = 20.0;
    mass_balance.runoff_runoff = 70.0;
    mass_balance.runoff_final_store = 15.0;

    const double expected = 5.0 / 120.0;
    double error = -1.0;
    ASSERT_EQ(swmm_get_runoff_continuity_error(engine_, &error), SWMM_OK);
    EXPECT_NEAR(error, expected, 1e-12);
}

TEST_F(MassBalanceApiTest, RoutingKnownImbalanceMatchesExpected) {
    auto& mass_balance = mb();
    mass_balance.routing_dry_weather = 20.0;
    mass_balance.routing_wet_weather = 30.0;
    mass_balance.routing_gw_inflow = 10.0;
    mass_balance.routing_rdii = 5.0;
    mass_balance.routing_external = 15.0;
    mass_balance.routing_init_storage = 20.0;

    mass_balance.routing_flooding = 8.0;
    mass_balance.routing_outflow = 60.0;
    mass_balance.routing_evap_loss = 4.0;
    mass_balance.routing_seep_loss = 3.0;
    mass_balance.routing_final_storage = 20.0;

    const double expected = 5.0 / 100.0;
    double error = -1.0;
    ASSERT_EQ(swmm_get_routing_continuity_error(engine_, &error), SWMM_OK);
    EXPECT_NEAR(error, expected, 1e-12);
}

TEST_F(MassBalanceApiTest, QualityKnownImbalanceMatchesExpected) {
    auto& mass_balance = mb();
    mass_balance.resize_quality(1);
    mass_balance.qual_wet_deposition[0] = 10.0;
    mass_balance.qual_runoff_load[0] = 30.0;
    mass_balance.qual_routing_wet[0] = 40.0;
    mass_balance.qual_routing_ii_in[0] = 20.0;
    mass_balance.qual_routing_init[0] = 5.0;

    mass_balance.qual_routing_outflow[0] = 68.0;
    mass_balance.qual_routing_flood[0] = 20.0;
    mass_balance.qual_routing_reacted[0] = 10.0;
    mass_balance.qual_routing_final[0] = 5.0;

    const double expected = 2.0 / 100.0;
    double error = -1.0;
    ASSERT_EQ(swmm_get_quality_continuity_error(engine_, 0, &error), SWMM_OK);
    EXPECT_NEAR(error, expected, 1e-12);
}

TEST_F(MassBalanceApiTest, QualityZeroFluxStillReportsZero) {
    auto& mass_balance = mb();
    mass_balance.resize_quality(2);

    double error = -1.0;
    ASSERT_EQ(swmm_get_quality_continuity_error(engine_, 1, &error), SWMM_OK);
    EXPECT_NEAR(error, 0.0, 1e-12);
}
// ============================================================================
// End-to-end forced lateral inflow continuity — issue #113.
//
// Inflow injected via swmm_node_set_lateral_inflow (or a ForcingData lateral
// forcing) is routed and discharged, so it must be booked as external inflow
// (legacy: apiExtInflow → EXTERNAL_INFLOW) for routing continuity to close.
// ForcingData lateral forcings are per-step overlays on the persistent
// runtime-API value: RESET lasts exactly one step, PERSIST+ADD contributes a
// steady (non-compounding) rate, and neither mutates user_lat_flow.
// ============================================================================

namespace {

const char* kForcedOutDir = "massbalance_out";

std::string forcedOutPath(const std::string& name) {
    std::filesystem::create_directories(kForcedOutDir);
    return (std::filesystem::path(kForcedOutDir) / name).string();
}

// J_IN (invert 10) → 200 ft of 1-ft circular pipe → free outfall O_OUT
// (invert 8). No DWF, runoff, or [INFLOWS]: the ONLY inflow is the runtime
// forced lateral inflow, so SWMM_ROUTING_EXTERNAL isolates its booking.
const char* kForcedInp =
    "[OPTIONS]\n"
    "FLOW_UNITS           CFS\n"
    "FLOW_ROUTING         DYNWAVE\n"
    "START_DATE           01/01/2026\n"
    "START_TIME           00:00:00\n"
    "END_DATE             01/01/2026\n"
    "END_TIME             00:10:00\n"
    "REPORT_STEP          00:01:00\n"
    "ROUTING_STEP         1\n"
    "ALLOW_PONDING        NO\n"
    "\n"
    "[JUNCTIONS]\n"
    ";;Name  Elev  MaxDepth\n"
    "J_IN    10.0  5.0\n"
    "\n"
    "[OUTFALLS]\n"
    ";;Name  Elev  Type  Gated\n"
    "O_OUT   8.0   FREE  NO\n"
    "\n"
    "[CONDUITS]\n"
    ";;Name    From   To     Length  N      Z1  Z2\n"
    "C_MAIN    J_IN   O_OUT  200.0   0.013  0   0\n"
    "\n"
    "[XSECTIONS]\n"
    ";;Link    Shape     G1   G2  G3  G4  Barrels\n"
    "C_MAIN    CIRCULAR  1.0  0   0   0   1\n"
    "\n"
    "[COORDINATES]\n"
    ";;Node  X    Y\n"
    "J_IN    0.0    0.0\n"
    "O_OUT   200.0  0.0\n";

class ForcedInflowMassBalanceTest : public ::testing::Test {
protected:
    void TearDown() override {
        if (engine_ != nullptr) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    void openModel(const std::string& base) {
        const std::string inp = forcedOutPath(base + ".inp");
        std::ofstream(inp) << kForcedInp;
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, inp.c_str(),
                                   forcedOutPath(base + ".rpt").c_str(),
                                   forcedOutPath(base + ".out").c_str(),
                                   nullptr), 0)
            << swmm_get_last_error_msg(engine_);
        ASSERT_EQ(swmm_engine_initialize(engine_), 0)
            << swmm_get_last_error_msg(engine_);
        ASSERT_EQ(swmm_engine_start(engine_, 0), 0)
            << swmm_get_last_error_msg(engine_);
        node_ = swmm_node_index(engine_, "J_IN");
        ASSERT_GE(node_, 0);
    }

    void step() {
        ASSERT_EQ(swmm_engine_step(engine_, &elapsed_), 0)
            << swmm_get_last_error_msg(engine_);
    }

    double routingTotal(int component) {
        double v = -1.0;
        EXPECT_EQ(swmm_get_routing_total(engine_, component, &v), SWMM_OK);
        return v;
    }

    double lateralInflow() {
        double q = -1.0;
        EXPECT_EQ(swmm_node_get_lateral_inflow(engine_, node_, &q), SWMM_OK);
        return q;
    }

    SWMM_Engine engine_ = nullptr;
    int node_ = -1;
    double elapsed_ = 0.0;
};

}  // namespace

// Issue #113 regression: 1 cfs injected via the runtime API for the whole
// 600 s run is fully discharged out the outfall. The injected volume must
// appear under SWMM_ROUTING_EXTERNAL (with SWMM_ROUTING_FORCING_INFLOW as
// its subset diagnostic) and continuity must close. Before the fix the
// volume was only in total_out and the error grew unboundedly negative
// (≈ -4e3 on this fixture).
TEST_F(ForcedInflowMassBalanceTest, ApiForcedInflowBooksAsExternalAndCloses) {
    openModel("api_forced_inflow");
    ASSERT_EQ(swmm_node_set_lateral_inflow(engine_, node_, 1.0), SWMM_OK);

    int guard = 0;
    do {
        step();
    } while (elapsed_ > 0.0 && ++guard < 100000);
    ASSERT_EQ(swmm_engine_end(engine_), 0) << swmm_get_last_error_msg(engine_);

    const double forcing  = routingTotal(SWMM_ROUTING_FORCING_INFLOW);
    const double external = routingTotal(SWMM_ROUTING_EXTERNAL);

    EXPECT_NEAR(forcing, 600.0, 6.0);              // 1 cfs × 600 s, ft3
    EXPECT_NEAR(external, forcing, 1e-9 * forcing); // subset == whole here

    // Residual ≈ -0.044 is this fixture's inherent dynwave filling-transient
    // error — a DWF-driven run of the identical model reports exactly the
    // same value, so the forced-inflow booking adds no error of its own.
    double error = -1.0;
    ASSERT_EQ(swmm_get_routing_continuity_error(engine_, &error), SWMM_OK);
    EXPECT_NEAR(error, 0.0, 0.05);
}

// A RESET (transient) ADD forcing overlays exactly one routing step and
// never mutates the persistent runtime-API value.
TEST_F(ForcedInflowMassBalanceTest, TransientAddForcingLastsExactlyOneStep) {
    openModel("transient_add_forcing");
    ASSERT_EQ(swmm_node_set_lateral_inflow(engine_, node_, 1.0), SWMM_OK);
    step();                                   // baseline: API value only
    EXPECT_NEAR(lateralInflow(), 1.0, 1e-9);

    ASSERT_EQ(swmm_forcing_node_lat_inflow(engine_, node_, 2.0,
              SWMM_FORCING_ADD, SWMM_FORCING_RESET), SWMM_OK);
    step();                                   // overlay active this step
    EXPECT_NEAR(lateralInflow(), 3.0, 1e-9);

    const double forcing_after_overlay =
        routingTotal(SWMM_ROUTING_FORCING_INFLOW);

    step();                                   // transient expired
    EXPECT_NEAR(lateralInflow(), 1.0, 1e-9);  // API value survives

    // Cumulative forcing volume grows only by the API rate once the
    // transient is gone (the pre-fix += path kept the extra 2 cfs forever).
    const double dt_growth = routingTotal(SWMM_ROUTING_FORCING_INFLOW)
                           - forcing_after_overlay;
    EXPECT_NEAR(dt_growth, 1.0, 1e-6);        // 1 cfs × 1 s routing step

    ASSERT_EQ(swmm_engine_end(engine_), 0);
}

// A PERSIST ADD forcing contributes a steady rate every step — it must not
// compound (the pre-fix += path yielded 2, 4, 6, ... cfs on successive
// steps and quadratic cumulative volume).
TEST_F(ForcedInflowMassBalanceTest, PersistentAddForcingDoesNotCompound) {
    openModel("persistent_add_forcing");
    ASSERT_EQ(swmm_forcing_node_lat_inflow(engine_, node_, 2.0,
              SWMM_FORCING_ADD, SWMM_FORCING_PERSIST), SWMM_OK);

    for (int i = 0; i < 5; ++i) step();
    EXPECT_NEAR(lateralInflow(), 2.0, 1e-9);  // steady, not 10.0

    // Equal step windows must accrue equal forcing volume (the first routing
    // step can be shorter than ROUTING_STEP, so windows start after step 5).
    // The pre-fix += path compounded the rate: window 2 would accrue ≈ 1.6×
    // window 1 here.
    const double after5 = routingTotal(SWMM_ROUTING_FORCING_INFLOW);
    for (int i = 0; i < 5; ++i) step();
    const double after10 = routingTotal(SWMM_ROUTING_FORCING_INFLOW);
    for (int i = 0; i < 5; ++i) step();
    const double after15 = routingTotal(SWMM_ROUTING_FORCING_INFLOW);

    const double window1 = after10 - after5;
    const double window2 = after15 - after10;
    EXPECT_GT(window1, 0.0);
    EXPECT_NEAR(window2, window1, 1e-6 * window1);

    ASSERT_EQ(swmm_engine_end(engine_), 0);
}

// ---------------------------------------------------------------------------
// Cascaded run-on must not be booked as a system output.
//
// Found 2026-08-22 by tests/parity/transport/age_legacy.inp on its FIRST run,
// the first corpus deck ever to route a subcatchment onto another
// subcatchment: runoff continuity read -23.667 % against legacy 5.x's
// -0.271 % on the same hydrology, precipitation and infiltration agreeing to
// the digit and only Surface Runoff differing -- by exactly the donor's own
// 1.628 in. S1's water was counted once when S1 shed it and again when S2
// discharged it.
//
// Legacy guards it at subcatch.c:761-765 (`outNode == -1 && outSubcatch !=
// subcatchIndex` zeroes vOutflow before massbal_updateRunoffTotals).
//
// This gate is deliberately a CASCADE-vs-DIRECT comparison rather than an
// absolute number. An absolute expectation would pin whatever this build
// produces; the invariant is that moving a subcatchment's outlet from a node
// to a peer must REMOVE its contribution from the runoff ledger and change
// nothing else about the water.
namespace {

const char* kCascadeOptions =
    "[OPTIONS]\n"
    "FLOW_UNITS           CFS\n"
    "FLOW_ROUTING         KINWAVE\n"
    "INFILTRATION         HORTON\n"
    "START_DATE           01/01/2026\n"
    "START_TIME           00:00:00\n"
    "END_DATE             01/01/2026\n"
    "END_TIME             06:00:00\n"
    "WET_STEP             00:15:00\n"
    "DRY_STEP             00:15:00\n"
    "ROUTING_STEP         60\n"
    "REPORT_STEP          00:15:00\n"
    "ALLOW_PONDING        NO\n"
    "\n"
    "[EVAPORATION]\n"
    "CONSTANT             0.0\n"
    "DRY_ONLY             NO\n"
    "\n"
    "[RAINGAGES]\n"
    "RG1     INTENSITY  1:00  1.0  TIMESERIES rain_ts\n"
    "\n"
    "[TIMESERIES]\n"
    "rain_ts  01/01/2026 00:00  0.50\n"
    "rain_ts  01/01/2026 01:00  0.50\n"
    "rain_ts  01/01/2026 02:00  0.00\n"
    "rain_ts  01/01/2026 06:00  0.00\n"
    "\n";

const char* kCascadeTail =
    "\n[SUBAREAS]\n"
    "SA      0.01  0.10  0.02  0.05  25  OUTLET\n"
    "SB      0.01  0.10  0.05  0.10  25  OUTLET\n"
    "\n[INFILTRATION]\n"
    "SA      3.0  0.5  4  7  0\n"
    "SB      3.0  0.5  4  7  0\n"
    "\n[JUNCTIONS]\n"
    "JN      10.0  10.0  0  0  0\n"
    "\n[OUTFALLS]\n"
    "OF      9.0   FREE  NO\n"
    "\n[CONDUITS]\n"
    "CN      JN    OF    400.0  0.013  0  0\n"
    "\n[XSECTIONS]\n"
    "CN      CIRCULAR  3.0  0  0  0  1\n"
    "\n[COORDINATES]\n"
    "JN      0.0    0.0\n"
    "OF      400.0  0.0\n";

// `sa_outlet` is the only thing that differs between the two fixtures.
std::string cascadeDeck(const char* sa_outlet) {
    return std::string(kCascadeOptions) +
           "[SUBCATCHMENTS]\n"
           ";;Name  Gage  Outlet  Area  %Imperv  Width  Slope  CurbLen\n"
           "SA      RG1   " + sa_outlet + "      5.0   70.0     500.0  1.0    0\n"
           "SB      RG1   JN      5.0   30.0     300.0  0.5    0\n" +
           kCascadeTail;
}

double runOnceAndGetRunoffTotal(const std::string& base, const char* sa_outlet,
                                double* rainfall_out) {
    const std::string inp = forcedOutPath(base + ".inp");
    std::ofstream(inp) << cascadeDeck(sa_outlet);

    SWMM_Engine e = swmm_engine_create();
    EXPECT_NE(e, nullptr);
    EXPECT_EQ(swmm_engine_open(e, inp.c_str(),
                               forcedOutPath(base + ".rpt").c_str(),
                               forcedOutPath(base + ".out").c_str(), nullptr),
              0) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_start(e, 1), 0) << swmm_get_last_error_msg(e);
    double elapsed = 0.0;
    int guard = 0;
    do {
        EXPECT_EQ(swmm_engine_step(e, &elapsed), 0)
            << swmm_get_last_error_msg(e);
    } while (elapsed > 0.0 && ++guard < 100000);
    EXPECT_EQ(swmm_engine_end(e), 0) << swmm_get_last_error_msg(e);

    double runoff = -1.0, rain = -1.0;
    EXPECT_EQ(swmm_get_runoff_total(e, SWMM_RUNOFF_RUNOFF, &runoff), SWMM_OK);
    EXPECT_EQ(swmm_get_runoff_total(e, SWMM_RUNOFF_RAINFALL, &rain), SWMM_OK);
    if (rainfall_out != nullptr) *rainfall_out = rain;

    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return runoff;
}

}  // namespace

TEST(RunoffLedgerCascadeTest, RunOnIsNotBookedAsASystemOutput) {
    double rain_direct = -1.0, rain_cascade = -1.0;
    // SA -> JN: both subcatchments discharge to the drainage system.
    const double direct = runOnceAndGetRunoffTotal("cascade_direct", "JN",
                                                   &rain_direct);
    // SA -> SB: SA's water reaches the system only via SB.
    const double cascade = runOnceAndGetRunoffTotal("cascade_runon", "SB",
                                                    &rain_cascade);

    ASSERT_GT(direct, 0.0) << "the direct fixture produced no runoff at all — "
                              "the deck, not the ledger, is the problem";

    // Same rain on the same two subcatchments either way. If this moves, the
    // two fixtures differ by more than SA's outlet and nothing below counts.
    EXPECT_NEAR(rain_cascade, rain_direct, rain_direct * 1e-9)
        << "the outlet change altered PRECIPITATION, so the fixtures are not "
           "comparable";

    // The invariant. Cascading SA must remove SA's own shed volume from the
    // ledger — so the cascade total is strictly smaller. Before the fix the
    // two were EQUAL, because the ledger added every subcatchment
    // unconditionally.
    EXPECT_LT(cascade, direct)
        << "cascaded run-on is still being booked as a system output: "
           "direct=" << direct << " cascade=" << cascade;

    // And it must remove SA's contribution, not something else. SA is the
    // 70 %-impervious half, so the drop is a large fraction of the total —
    // a token difference would mean the guard fired on the wrong term.
    EXPECT_LT(cascade, direct * 0.75)
        << "the ledger dropped something, but far too little to be SA's "
           "runoff: direct=" << direct << " cascade=" << cascade;
}
