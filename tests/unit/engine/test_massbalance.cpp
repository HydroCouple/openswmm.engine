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

#include <cmath>
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

// ---------------------------------------------------------------------------
// A self-routed subcatchment must not feed its own runoff back to itself.
//
// Found 2026-08-22 while writing the falsifier fixture for the CASCADE gate
// above: `selfroute` booked 2.328 in against legacy 5.x's 0.417 -- 5.6x, with
// -265 % continuity -- while direct, 2-deep, 3-deep and all-direct cascades
// all agreed with legacy to the digit. The ledger guard added in 421e95c2 was
// correct and could not help: the divergence is one layer up, in assembleRunon,
// where the water genuinely recirculates.
//
// Legacy carries `!= subcatchIndex` in THREE places -- run-on distribution
// (subcatch.c:546-548), the ledger (763), and washoff (surfqual.c:363). 421e95c2
// matched only the second. That is lesson 142: porting one site is not porting
// the invariant.
//
// The gate compares SELF-ROUTED against DIRECT on two fixtures differing only
// in one outlet. A self-route is a no-op in legacy's model -- the subcatchment
// discharges to the system exactly as if the outlet named a node -- so the two
// must agree, and that equality is a far stronger statement than "smaller than
// the broken value".
TEST(RunoffLedgerCascadeTest, SelfRoutedSubcatchmentDoesNotRecirculate) {
    double rain_direct = -1.0, rain_self = -1.0;
    const double direct = runOnceAndGetRunoffTotal("selfroute_direct", "JN",
                                                   &rain_direct);
    // SA -> SA. Legacy treats the self-reference as no outlet subcatchment at
    // all, so this must behave exactly like the line above.
    const double self = runOnceAndGetRunoffTotal("selfroute_self", "SA",
                                                 &rain_self);

    ASSERT_GT(direct, 0.0) << "the direct fixture produced no runoff at all — "
                              "the deck, not the guard";
    EXPECT_NEAR(rain_self, rain_direct, rain_direct * 1e-9)
        << "the outlet change altered PRECIPITATION, so the fixtures are not "
           "comparable and nothing below counts";

    // The invariant. Before the fix `self` was several times `direct`, because
    // SA's runoff re-entered SA as run-on every step.
    EXPECT_NEAR(self, direct, direct * 1e-6)
        << "a self-routed subcatchment is not behaving like a directly "
           "connected one: direct=" << direct << " self=" << self
        << ". Ratio " << (direct > 0.0 ? self / direct : 0.0)
        << " — greater than 1 means its runoff is recirculating.";
}

// ---------------------------------------------------------------------------
// The quality half of the same guard.
//
// The two gates above are VOLUMETRIC and are blind to the washoff site: with
// the run-on guard in place and the washoff guard removed, both still pass
// while `qual_runoff_load` on a cascaded deck reads 1.522 lbs against the
// directly-connected deck's 1.369 -- MORE load reaching the system than if
// every subcatchment discharged straight to a node, which cannot happen.
// That measurement is why this gate exists; without it the changeset's third
// site is unasserted.
//
// Legacy is not usable as the control here. On this fixture EPA 5.x reports
// Surface Buildup 0.885 lbs and Surface Runoff 0.000 against our 2.500 and
// 1.369: the buildup/washoff functions themselves diverge, so a
// cross-engine number would be measuring that gap rather than this guard.
// The invariants below are internal differentials on one engine, which the
// falsifier sweep showed do discriminate.
namespace {

// The cascade deck plus one pollutant on one land use. POW buildup with a
// six-hour antecedent period gives a load large enough to be unambiguous,
// and EXP washoff makes it depend on runoff rate rather than volume alone.
std::string qualityDeck(const char* sa_outlet) {
    return cascadeDeck(sa_outlet) +
        "\n[POLLUTANTS]\n"
        "TSS     MG/L   0.0    0.0  0.0  0.0  NO  *  0.0  0.0  0.0\n"
        "\n[LANDUSES]\n"
        "RESID   0  0  0\n"
        "\n[COVERAGES]\n"
        "SA      RESID  100\n"
        "SB      RESID  100\n"
        "\n[BUILDUP]\n"
        "RESID   TSS  POW  100.0  1.0  1.0  AREA\n"
        "\n[WASHOFF]\n"
        "RESID   TSS  EXP  2.0  1.5  0  0\n";
}

// Returns the qual_runoff_load LEDGER term; `sa_total_out` receives SA's own
// cumulative washoff, which is the other side of the split at :2886.
double runQualityDeck(const std::string& base, const char* sa_outlet,
                      double* sa_total_out) {
    const std::string inp = forcedOutPath(base + ".inp");
    std::ofstream(inp) << qualityDeck(sa_outlet);

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

    const auto& ctx = as_cpp_engine(e).context();
    const double load = ctx.mass_balance.qual_runoff_load.empty()
                        ? -1.0 : ctx.mass_balance.qual_runoff_load[0];
    // SA is subcatchment 0 and TSS is pollutant 0, so total_load[0] is SA's.
    if (sa_total_out != nullptr)
        *sa_total_out = ctx.subcatches.total_load.empty()
                        ? -1.0 : ctx.subcatches.total_load[0];

    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return load;
}

}  // namespace

TEST(RunoffLedgerCascadeTest, WashoffLoadIsBookedOnlyWhenItReachesTheSystem) {
    double sa_direct = -1.0, sa_cascade = -1.0, sa_self = -1.0;
    const double direct  = runQualityDeck("qual_direct",  "JN", &sa_direct);
    const double cascade = runQualityDeck("qual_cascade", "SB", &sa_cascade);
    const double self    = runQualityDeck("qual_self",    "SA", &sa_self);

    ASSERT_GT(direct, 0.0) << "the direct fixture washed nothing off at all — "
                              "the deck, not the guard";

    // Same statement as the volumetric gate, in the quality ledger: a
    // self-route is a no-op, so it must book exactly what a node outlet does.
    EXPECT_NEAR(self, direct, direct * 1e-6)
        << "a self-routed subcatchment's washoff load is not what a directly "
           "connected one books: direct=" << direct << " self=" << self;

    // Cascading SA must REMOVE SA's load from the ledger; the receiver books
    // it when it discharges. Without the guard this rose ABOVE `direct`,
    // which is the impossible signature.
    EXPECT_LT(cascade, direct)
        << "cascaded washoff load is still booked as reaching the system: "
           "direct=" << direct << " cascade=" << cascade
        << (cascade > direct
                ? " — and it EXCEEDS the directly-connected deck, so the same "
                  "mass is being counted twice"
                : "");

    // The other side of the split. `subcatches.total_load` is what the
    // subcatchment washed off, not what the system received, so cascading
    // must NOT zero it — legacy keeps its equivalent above its own guard
    // (surfqual.c:356). Making this conditional too drove SA's total to 0.
    EXPECT_GT(sa_cascade, 0.0)
        << "SA's own washoff total was zeroed by the ledger guard: the "
           "per-subcatchment total and the ledger term are different "
           "questions and only the ledger term is conditional";
    EXPECT_NEAR(sa_cascade, sa_direct, sa_direct * 1e-6)
        << "SA washed off a different amount depending on where it drains, "
           "which the per-subcatchment total should not see: direct="
        << sa_direct << " cascade=" << sa_cascade;
}

// ---------------------------------------------------------------------------
// THE SEAM: what the conveyance receives must equal what the surface shed.
//
// Finding 4, 2026-08-22. The node injection fed each outlet node
// `q_runoff + q_runon`, but run-on is already inside `runoff[]` --
// assembleRunon sums every contributor into runon_inflow[] and Runoff.cpp:333
// consumes that array wholesale as an inflow rate. Legacy's
// subcatch_getWtdOutflow returns runoff alone. Measured: cascade 0.511
// acre-feet against legacy's 0.218, three-deep 0.536 against 0.318, with the
// excess equal to the donor's own runoff.
//
// It survived because NEITHER continuity check could see it. The runoff
// balance closed. The routing balance closed. Each was self-consistent on its
// own side of a seam across which 2.3x more water arrived than departed
// (lesson 147).
//
// So this gate is deliberately not another one-sided balance. It asserts
// CORRESPONDENCE: on a deck where every subcatchment drains to one junction
// and nothing is lost between surface and node, the routing wet-weather
// inflow must equal the runoff ledger's surface runoff. That equality is the
// thing no existing check in this file makes.
namespace {

struct SeamTotals {
    double runoff_out = -1.0;   // SWMM_RUNOFF_RUNOFF -- left the surface
    double wet_in     = -1.0;   // SWMM_ROUTING_WET_WEATHER -- reached the pipes
};

SeamTotals runAndReadSeam(const std::string& base, const char* sa_outlet) {
    const std::string inp = forcedOutPath(base + ".inp");
    std::ofstream(inp) << cascadeDeck(sa_outlet);

    SeamTotals t;
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

    EXPECT_EQ(swmm_get_runoff_total(e, SWMM_RUNOFF_RUNOFF, &t.runoff_out),
              SWMM_OK);
    EXPECT_EQ(swmm_get_routing_total(e, SWMM_ROUTING_WET_WEATHER, &t.wet_in),
              SWMM_OK);

    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return t;
}

}  // namespace

TEST(RunoffLedgerCascadeTest, ConveyanceReceivesWhatTheSurfaceShed) {
    // Both fixtures matter. `direct` is the control: with no cascade there is
    // no run-on to double-count, so it must have been correct before the fix
    // AND after it. If the control moves, the fix broke the ordinary case.
    const SeamTotals direct  = runAndReadSeam("seam_direct", "JN");
    const SeamTotals cascade = runAndReadSeam("seam_cascade", "SB");

    ASSERT_GT(direct.runoff_out, 0.0)
        << "the control fixture shed no runoff — the deck, not the seam";
    ASSERT_GT(cascade.runoff_out, 0.0)
        << "the cascade fixture shed no runoff — the deck, not the seam";

    // Every subcatchment on these decks drains (directly or via SB) to JN,
    // and there is nothing between the surface and the node to lose water in.
    //
    // The two totals cannot be compared to machine precision, and the bar
    // here is set from measurement rather than taste. The runoff ledger
    // integrates runoff[] on the RUNOFF clock; the routing total integrates
    // the interpolated q on the ROUTING clock. That is a quadrature
    // difference, and it shrinks with the wet step -- measured on this deck,
    // relative gap 5.1e-5 at WET_STEP 15 min, 2.0e-5 at 5 min, 6.6e-6 at
    // 1 min, 2.2e-6 at 20 s, converging to zero. A leak would not converge.
    // So 1e-3: twenty times above the floor this fixture can reach, and
    // three orders below the defect, which was 1.35.
    constexpr double kSeam = 1e-3;
    EXPECT_NEAR(direct.wet_in, direct.runoff_out, direct.runoff_out * kSeam)
        << "CONTROL: the node received " << direct.wet_in
        << " while the surface shed " << direct.runoff_out
        << ". With no cascade these cannot legitimately differ.";

    EXPECT_NEAR(cascade.wet_in, cascade.runoff_out, cascade.runoff_out * kSeam)
        << "the node received " << cascade.wet_in
        << " while the surface shed " << cascade.runoff_out
        << " (ratio " << (cascade.runoff_out > 0.0
                          ? cascade.wet_in / cascade.runoff_out : 0.0)
        << "). Run-on is already inside runoff[]; adding it again at the node "
           "books the same water twice. Both continuity checks still close — "
           "that is the point of comparing ACROSS the seam.";

    // The statement that does not rot with the tolerance. The control
    // measures what quadrature agreement this engine and this timestep can
    // actually reach; cascading must not degrade it. Measured: 1.5x with the
    // fix, 26000x without it.
    const double err_direct  = std::fabs(direct.wet_in / direct.runoff_out - 1.0);
    const double err_cascade = std::fabs(cascade.wet_in / cascade.runoff_out - 1.0);
    EXPECT_LE(err_cascade, std::max(10.0 * err_direct, 1e-9))
        << "cascading degraded the surface-to-node correspondence by "
        << (err_direct > 0.0 ? err_cascade / err_direct : 0.0)
        << "x: the control closes to " << err_direct << " and the cascade to "
        << err_cascade << " on the same engine and the same clock.";
}
