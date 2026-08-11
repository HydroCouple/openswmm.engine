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
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_model.h>

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
