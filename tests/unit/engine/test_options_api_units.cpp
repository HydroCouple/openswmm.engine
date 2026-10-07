/**
 * @file test_options_api_units.cpp
 * @brief Unit tests for the value units/grammar of numeric [OPTIONS] keys on
 *        the public swmm_options_get / swmm_options_set API.
 *
 * @details Added 2026-08-06 alongside the Simulation Options dialog
 *          persistence fixes.  Pins two contracts that regressed silently:
 *
 *          1. MINIMUM_STEP is a first-class key — historically only the
 *             [OPTIONS] parser and InpWriter knew it, so GUI edits were
 *             rejected with SWMM_ERR_BADPARAM and reverted on reload.
 *             Accepts decimal seconds or H:MM:SS (parse_time_seconds), the
 *             same grammar as ROUTING_STEP.
 *
 *          2. LAT_FLOW_TOL / SYS_FLOW_TOL speak PERCENT on both get and set,
 *             mirroring the [OPTIONS] surface.  The getter used to return the
 *             stored fraction while the setter divided by 100, so a
 *             get → set(returned) cycle shrank the value 100× per pass.  The
 *             idempotence test below guards that class of bug.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>

namespace fs = std::filesystem;

class OptionsApiUnitsTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;
    SWMM_Engine reopened = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        // Minimal valid topology so swmm_model_write / reopen succeed in the
        // disk round-trip test.
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "O1", SWMM_NODE_OUTFALL),  SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "L1", SWMM_LINK_CONDUIT),  SWMM_OK);
        const int l1 = swmm_link_index(engine, "L1");
        ASSERT_EQ(swmm_link_set_nodes(engine, l1,
                      swmm_node_index(engine, "J1"),
                      swmm_node_index(engine, "O1")), SWMM_OK);
        ASSERT_EQ(swmm_link_set_length(engine, l1, 100.0), SWMM_OK);
        ASSERT_EQ(swmm_link_set_roughness(engine, l1, 0.013), SWMM_OK);
        ASSERT_EQ(swmm_link_set_xsect(engine, l1, SWMM_XSECT_CIRCULAR,
                                      1.0, 0.0, 0.0, 0.0), SWMM_OK);
    }
    void TearDown() override {
        if (reopened) { swmm_engine_close(reopened); swmm_engine_destroy(reopened); }
        if (engine) swmm_engine_destroy(engine);
    }

    std::string getopt(SWMM_Engine e, const char* key) {
        char buf[512] = {};
        EXPECT_EQ(swmm_options_get(e, key, buf, sizeof(buf)), SWMM_OK)
            << "get failed for " << key;
        return std::string(buf);
    }
    double getd(SWMM_Engine e, const char* key) {
        return std::stod(getopt(e, key));
    }

    // Artifacts go under the test's working directory (build tree) so they
    // stay reviewable after a run, per project testing conventions.
    static fs::path artifactDir() {
        fs::path d = fs::current_path() / "test_options_api_units_artifacts";
        fs::create_directories(d);
        return d;
    }
};

// ---------------------------------------------------------------------------
// MINIMUM_STEP: default, decimal-seconds form, clock form.
// ---------------------------------------------------------------------------

TEST_F(OptionsApiUnitsTest, MinimumStepDefault) {
    // SimulationOptions.hpp: min_routing_step = 0.5.
    EXPECT_NEAR(getd(engine, "MINIMUM_STEP"), 0.5, 1e-12);
}

TEST_F(OptionsApiUnitsTest, MinimumStepRoundTrip) {
    ASSERT_EQ(swmm_options_set(engine, "MINIMUM_STEP", "1.0"), SWMM_OK);
    EXPECT_NEAR(getd(engine, "MINIMUM_STEP"), 1.0, 1e-12);

    // H:MM:SS clock form, same grammar as the [OPTIONS] parser.
    ASSERT_EQ(swmm_options_set(engine, "MINIMUM_STEP", "0:00:02"), SWMM_OK);
    EXPECT_NEAR(getd(engine, "MINIMUM_STEP"), 2.0, 1e-12);

    // get → set(returned) must be a no-op.
    const std::string echoed = getopt(engine, "MINIMUM_STEP");
    ASSERT_EQ(swmm_options_set(engine, "MINIMUM_STEP", echoed.c_str()), SWMM_OK);
    EXPECT_NEAR(getd(engine, "MINIMUM_STEP"), 2.0, 1e-12);
}

// ---------------------------------------------------------------------------
// LAT_FLOW_TOL / SYS_FLOW_TOL: percent on both sides, idempotent echo.
// ---------------------------------------------------------------------------

TEST_F(OptionsApiUnitsTest, FlowTolerancesArePercent) {
    ASSERT_EQ(swmm_options_set(engine, "LAT_FLOW_TOL", "5"), SWMM_OK);
    ASSERT_EQ(swmm_options_set(engine, "SYS_FLOW_TOL", "5"), SWMM_OK);
    EXPECT_NEAR(getd(engine, "LAT_FLOW_TOL"), 5.0, 1e-9);
    EXPECT_NEAR(getd(engine, "SYS_FLOW_TOL"), 5.0, 1e-9);
}

TEST_F(OptionsApiUnitsTest, FlowTolerancesIdempotentEcho) {
    // Regression: the getter used to return the stored fraction while the
    // setter expected percent, so each get→set cycle shrank the value 100×.
    ASSERT_EQ(swmm_options_set(engine, "LAT_FLOW_TOL", "5"), SWMM_OK);
    for (int i = 0; i < 2; ++i) {
        const std::string echoed = getopt(engine, "LAT_FLOW_TOL");
        ASSERT_EQ(swmm_options_set(engine, "LAT_FLOW_TOL", echoed.c_str()),
                  SWMM_OK);
    }
    EXPECT_NEAR(getd(engine, "LAT_FLOW_TOL"), 5.0, 1e-9);
}

// ---------------------------------------------------------------------------
// THREADS: plain integer on both sides; 0 is the "auto" sentinel the engine
// maps to omp_get_max_threads() at start (SWMMEngine.cpp) and must round-trip
// unchanged, matching the GUI spinbox's 0 = "auto" convention.
// ---------------------------------------------------------------------------

TEST_F(OptionsApiUnitsTest, ThreadsRoundTrip) {
    // SimulationOptions.hpp: num_threads = 1.
    EXPECT_EQ(getopt(engine, "THREADS"), "1");

    ASSERT_EQ(swmm_options_set(engine, "THREADS", "8"), SWMM_OK);
    EXPECT_EQ(getopt(engine, "THREADS"), "8");

    ASSERT_EQ(swmm_options_set(engine, "THREADS", "0"), SWMM_OK);
    EXPECT_EQ(getopt(engine, "THREADS"), "0");

    // get → set(returned) must be a no-op.
    const std::string echoed = getopt(engine, "THREADS");
    ASSERT_EQ(swmm_options_set(engine, "THREADS", echoed.c_str()), SWMM_OK);
    EXPECT_EQ(getopt(engine, "THREADS"), "0");
}

// ---------------------------------------------------------------------------
// Disk leg: set → swmm_model_write → file text → reopen → get.
// ---------------------------------------------------------------------------

TEST_F(OptionsApiUnitsTest, DiskRoundTrip) {
    ASSERT_EQ(swmm_options_set(engine, "MINIMUM_STEP", "2"),   SWMM_OK);
    ASSERT_EQ(swmm_options_set(engine, "LAT_FLOW_TOL", "3"),   SWMM_OK);
    ASSERT_EQ(swmm_options_set(engine, "SYS_FLOW_TOL", "3"),   SWMM_OK);
    ASSERT_EQ(swmm_options_set(engine, "THREADS",      "8"),   SWMM_OK);

    const fs::path inp = artifactDir() / "options_units_roundtrip.inp";
    const fs::path rpt = artifactDir() / "options_units_roundtrip.rpt";
    const fs::path out = artifactDir() / "options_units_roundtrip.out";
    ASSERT_EQ(swmm_model_write(engine, inp.string().c_str()), SWMM_OK)
        << swmm_get_last_error_msg(engine);
    ASSERT_TRUE(fs::exists(inp));

    // The .inp surface carries the same numbers.
    std::ifstream f(inp);
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("MINIMUM_STEP"), std::string::npos);
    EXPECT_NE(text.find("LAT_FLOW_TOL"), std::string::npos);

    reopened = swmm_engine_create();
    ASSERT_NE(reopened, nullptr);
    ASSERT_EQ(swmm_engine_open(reopened, inp.string().c_str(),
                               rpt.string().c_str(), out.string().c_str(),
                               nullptr), SWMM_OK)
        << "reopen failed: " << swmm_get_last_error_msg(reopened);

    EXPECT_NEAR(getd(reopened, "MINIMUM_STEP"), 2.0, 1e-9);
    EXPECT_NEAR(getd(reopened, "LAT_FLOW_TOL"), 3.0, 1e-9);
    EXPECT_NEAR(getd(reopened, "SYS_FLOW_TOL"), 3.0, 1e-9);
    EXPECT_EQ(getopt(reopened, "THREADS"), "8");
}

// ---------------------------------------------------------------------------
// UNSTEADY_FRICTION / UF_K3 (issue #156): an enum key and its coefficient.
// Nearest home for the pair — SURCHARGE_METHOD, the closest existing analogue,
// is not pinned on the C API anywhere. Both keys are inert in this build (no
// solver reads them), so the get/set surface is the only place a defect in the
// dispatch would show.
// ---------------------------------------------------------------------------

TEST_F(OptionsApiUnitsTest, UnsteadyFrictionDefaults) {
    // SimulationOptions.hpp: unsteady_friction = 0 (NONE), uf_k3 = 0.015.
    EXPECT_EQ(getopt(engine, "UNSTEADY_FRICTION"), "NONE");
    EXPECT_NEAR(getd(engine, "UF_K3"), 0.015, 1e-12);
}

TEST_F(OptionsApiUnitsTest, UnsteadyFrictionRoundTrip) {
    ASSERT_EQ(swmm_options_set(engine, "UNSTEADY_FRICTION", "VITKOVSKY"), SWMM_OK);
    ASSERT_EQ(swmm_options_set(engine, "UF_K3", "0.02"), SWMM_OK);
    EXPECT_EQ(getopt(engine, "UNSTEADY_FRICTION"), "VITKOVSKY");
    EXPECT_NEAR(getd(engine, "UF_K3"), 0.02, 1e-12);

    // The setter upper-cases, matching the [OPTIONS] parser's norm().
    ASSERT_EQ(swmm_options_set(engine, "UNSTEADY_FRICTION", "none"), SWMM_OK);
    EXPECT_EQ(getopt(engine, "UNSTEADY_FRICTION"), "NONE");

    // get -> set(returned) must be a no-op on both keys.
    ASSERT_EQ(swmm_options_set(engine, "UNSTEADY_FRICTION", "VITKOVSKY"), SWMM_OK);
    for (const char* key : {"UNSTEADY_FRICTION", "UF_K3"}) {
        const std::string echoed = getopt(engine, key);
        ASSERT_EQ(swmm_options_set(engine, key, echoed.c_str()), SWMM_OK) << key;
    }
    EXPECT_EQ(getopt(engine, "UNSTEADY_FRICTION"), "VITKOVSKY");
    EXPECT_NEAR(getd(engine, "UF_K3"), 0.02, 1e-12);
}

TEST_F(OptionsApiUnitsTest, UnsteadyFrictionUnknownTokenIsBadParam) {
    ASSERT_EQ(swmm_options_set(engine, "UNSTEADY_FRICTION", "VITKOVSKY"), SWMM_OK);
    // BADPARAM specifically, not just "not OK" — the [OPTIONS] parser silently
    // ignores an unknown token (accepted-and-inert posture), so the API is the
    // only surface that reports the typo back to a caller.
    EXPECT_EQ(swmm_options_set(engine, "UNSTEADY_FRICTION", "BOGUS"),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(getopt(engine, "UNSTEADY_FRICTION"), "VITKOVSKY");
}
