// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file test_pump_ideal_curve.cpp
 * @brief `[PUMPS]` curve column "*" — the ideal-pump placeholder.
 *
 * @details Legacy `pump_readParams` (link.c:1437) accepts "*" in place of a
 *          pump curve name and leaves `pumpCurve = -1`, which `pump_validate`
 *          then types as IDEAL_PUMP (flow = inlet node inflow). The refactored
 *          parser looked "*" up in the curve table and the post-parse resolver
 *          raised a fatal `ERROR 209: undefined object *.`, so any model with
 *          an ideal pump failed to open.
 *
 *          That path is reachable from the GUI without the user ever typing an
 *          asterisk: `InpWriter` emits "*" for an unset curve index, so saving
 *          a model that contains an ideal pump produced an `.inp` the engine
 *          then refused to re-open.
 *
 *          The working directory is tests/unit/engine/data/ (see CMakeLists),
 *          so the fixture is referenced as pumps/ideal_pump.inp and every
 *          artifact is written under pumps/ where it can be reviewed.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_model.h>

#include "core/SWMMEngine.hpp"

namespace {

constexpr const char* kSrc = "pumps/ideal_pump.inp";
constexpr const char* kOut = "pumps/_ideal_pump_roundtrip.inp";

int pumpCurveOf(SWMM_Engine e, const char* name) {
    const int idx = swmm_link_index(e, name);
    EXPECT_GE(idx, 0) << "link not found: " << name;
    if (idx < 0) return -99;
    int curve = -99;
    EXPECT_EQ(swmm_link_get_pump_curve(e, idx, &curve), SWMM_OK);
    return curve;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

}  // namespace

// ===========================================================================
// The regression: a strict open of a model with a "*" pump curve succeeds and
// the pump carries no curve (ideal), while a real curve still resolves.
// ===========================================================================

TEST(IdealPumpCurve, AsteriskOpensAndMeansNoCurve) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, kSrc, "pumps/_parse.rpt", "pumps/_parse.out",
                               nullptr), SWMM_OK)
        << swmm_get_last_error_msg(e);

    EXPECT_EQ(pumpCurveOf(e, "P_IDEAL"), -1);   // "*" → no curve → IDEAL_PUMP
    EXPECT_GE(pumpCurveOf(e, "P_CURVE"), 0);    // named curve still resolves

    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

// An ideal pump is not just parseable — it routes. Legacy sets its flow to the
// inlet node's inflow, so the run must complete rather than abort or stall.
TEST(IdealPumpCurve, RunsToCompletion) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, kSrc, "pumps/_run.rpt", "pumps/_run.out",
                               nullptr), SWMM_OK)
        << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK) << swmm_get_last_error_msg(e);

    double elapsed = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK)
            << swmm_get_last_error_msg(e);
    } while (elapsed > 0.0);

    EXPECT_EQ(swmm_engine_end(e), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

// Save → re-open: the writer emits "*" for the unset curve and the parser
// accepts it back, so the ideal pump survives a GUI round-trip.
TEST(IdealPumpCurve, SurvivesWriteAndReopen) {
    SWMM_Engine e1 = swmm_engine_create();
    ASSERT_NE(e1, nullptr);
    ASSERT_EQ(swmm_engine_open(e1, kSrc, "pumps/_rt1.rpt", "pumps/_rt1.out",
                               nullptr), SWMM_OK)
        << swmm_get_last_error_msg(e1);
    ASSERT_EQ(swmm_model_write(e1, kOut), SWMM_OK) << swmm_get_last_error_msg(e1);
    swmm_engine_close(e1);
    swmm_engine_destroy(e1);

    const std::string written = slurp(kOut);
    EXPECT_NE(written.find("P_IDEAL"), std::string::npos) << written;

    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, kOut, "pumps/_rt2.rpt", "pumps/_rt2.out",
                               nullptr), SWMM_OK)
        << swmm_get_last_error_msg(e2);

    EXPECT_EQ(pumpCurveOf(e2, "P_IDEAL"), -1);
    EXPECT_GE(pumpCurveOf(e2, "P_CURVE"), 0);

    swmm_engine_close(e2);
    swmm_engine_destroy(e2);
}

// ===========================================================================
// TYPE3/TYPE5 speed parity — legacy pump_getInflow (link.c PUMP3/PUMP5):
//   - s stays 1.0 for a PUMP3 curve; ONLY PUMP5 reads the live speed setting
//     (head is affinity-scaled by 1/s² before the lookup);
//   - the setting multiplies the flow exactly ONCE (return qIn * setting).
// A fractional setting (variable-speed PUMP5, or a PUMP3 under a MODULATE
// control action) must therefore give Q = s·Curve(...) — not s²·Curve(...),
// and a PUMP3 must sample the curve at the raw head. On/off settings
// (0 or 1) cannot see either defect, so this gate pins s = 0.5 and checks
// the converged state against the closed form of a linear curve
// Q(h) = 100 − h (CFS/ft, so every unit factor is 1).
// ===========================================================================

namespace {

void writeSpeedDeck(const std::string& path, const char* curve_kind,
                    bool setting_via_rule = false) {
    std::ofstream f(path);
    f << "[TITLE]\npump speed parity (" << curve_kind << ")\n\n"
      << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 00:10:00\n"
      << "ROUTING_STEP 2\nREPORT_STEP 00:01:00\n\n"
      << "[STORAGE]\n"
      << ";;Name Elev Ymax Y0 Shape A B C\n"
      << "SU1 0.0 10 5 FUNCTIONAL 0 0 100000 0\n\n"
      << "[JUNCTIONS]\nJ2 20.0 15 0 0 0\n\n"
      << "[OUTFALLS]\nOUT 15.0 FREE NO\n\n"
      << "[CONDUITS]\nC1 J2 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
      << "[PUMPS]\nP1 SU1 J2 PC ON 0 0\n\n"
      << "[CURVES]\nPC " << curve_kind << " 0 100\nPC 100 0\n\n"
      << "[INFLOWS]\nSU1 FLOW ts5 FLOW 1.0 1.0\n\n"
      << "[TIMESERIES]\nts5 0:00 5\nts5 24:00 5\n";
    if (setting_via_rule) {
        f << "\n[CONTROLS]\n"
          << "RULE R1\n"
          << "IF SIMULATION TIME > 0.0001\n"
          << "THEN PUMP P1 SETTING = 0.5\n";
    }
}

} // namespace

namespace {

// Run one speed-parity deck and check the converged state against the
// legacy relation Q = s·Curve(h) (single setting multiply; TYPE5 samples
// the affinity-scaled head, TYPE3 the raw head). The 0.5 speed arrives
// either through the C API (re-asserted before every step — the per-step
// target reset mirrors legacy link_setTargetSetting, which would fold an
// external target back to the live setting) or through a [CONTROLS] rule
// (THEN PUMP P1 SETTING = 0.5), which legacy applies AFTER that reset, so
// it must both land and persist with no API help.
void runSpeedParityCheck(const char* kind, bool setting_via_rule) {
    const bool type5 = (std::string(kind) == "PUMP5");
    const std::string tag = std::string("pumps/_speed_") + kind +
                            (setting_via_rule ? "_rule" : "");
    writeSpeedDeck(tag + ".inp", kind, setting_via_rule);

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, (tag + ".inp").c_str(),
                               (tag + ".rpt").c_str(),
                               (tag + ".out").c_str(), nullptr), SWMM_OK)
        << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);

    const int pidx = swmm_link_index(e, "P1");
    ASSERT_GE(pidx, 0);

    // 60 simulated seconds — enough for the pump flow and junction heads
    // to settle onto the curve relation; the big wet well barely moves.
    double elapsed = 0.0;
    for (int i = 0; i < 30; ++i) {
        if (!setting_via_rule)
            ASSERT_EQ(swmm_link_set_control_setting(e, pidx, 0.5), SWMM_OK);
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK)
            << swmm_get_last_error_msg(e);
    }

    auto& ctx = static_cast<openswmm::SWMMEngine*>(e)->context();
    const auto uj = static_cast<std::size_t>(pidx);
    const auto un1 = static_cast<std::size_t>(ctx.links.node1[uj]);
    const auto un2 = static_cast<std::size_t>(ctx.links.node2[uj]);

    const double s = ctx.links.setting[uj];
    ASSERT_DOUBLE_EQ(s, 0.5)
        << kind << (setting_via_rule
            ? ": the control rule's SETTING never reached the pump"
            : ": setting did not persist");

    const double head =
        (ctx.nodes.depth[un2] + ctx.nodes.invert_elev[un2]) -
        (ctx.nodes.depth[un1] + ctx.nodes.invert_elev[un1]);
    const double h_curve = type5 ? std::max(head / (s * s), 0.0)
                                 : std::max(head, 0.0);
    ASSERT_GT(h_curve, 0.0) << kind;
    ASSERT_LT(h_curve, 100.0) << kind << ": operating point left the curve";

    // Legacy: Q = setting · Curve(h) with ONE setting multiply.
    const double expected = s * (100.0 - h_curve);
    const double q = ctx.links.flow[uj];
    EXPECT_NEAR(q, expected, 0.02 * expected + 0.1)
        << kind << ": head=" << head << " h_curve=" << h_curve
        << " — s²·Curve (double setting multiply) and/or a speed-scaled "
           "TYPE3 head would land far from the legacy relation";

    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

} // namespace

TEST(PumpSpeedParity, FractionalSettingSamplesAndScalesLikeLegacy) {
    for (const char* kind : {"PUMP3", "PUMP5"})
        runSpeedParityCheck(kind, /*setting_via_rule=*/false);
}

// End-to-end controls confirmation: the fractional speed comes from a
// [CONTROLS] rule instead of the API. Exercises parser → ControlEngine →
// target_setting → computePumpFlowK in the legacy order
// (updatePumpTargetSettings BEFORE controls_.evaluate, SWMMEngine.cpp),
// so the rule's value must survive the per-step target reset on its own.
TEST(PumpSpeedParity, ControlRuleSettingReachesPump) {
    for (const char* kind : {"PUMP3", "PUMP5"})
        runSpeedParityCheck(kind, /*setting_via_rule=*/true);
}
