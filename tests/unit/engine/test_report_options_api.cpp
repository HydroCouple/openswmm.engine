/**
 * @file test_report_options_api.cpp
 * @brief Unit tests for the [REPORT] section keys exposed via the public
 *        swmm_options_get / swmm_options_set API.
 *
 * @details Slice BV.1 (added 2026-05-22).  The engine has long stored the
 *          [REPORT] section flags in ctx.options.rpt_* and the InpWriter
 *          emits them — but the public C API exposed no way to read or
 *          write them.  This file exercises the new RPT_* key surface.
 *
 *          Bool keys (YES/NO):
 *            RPT_DISABLED, RPT_INPUT, RPT_CONTINUITY,
 *            RPT_FLOWSTATS, RPT_CONTROLS, RPT_AVERAGES
 *
 *          Selector keys (ALL / NONE / "name1,name2,..."):
 *            RPT_SUBCATCHMENTS, RPT_NODES, RPT_LINKS
 */

#include <gtest/gtest.h>

#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>

class ReportOptionsApiTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        // Minimal topology so swmm_model_write would succeed if a test
        // exercises it (not required for the key-level tests below).
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "O1", SWMM_NODE_OUTFALL),  SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "L1", SWMM_LINK_CONDUIT),  SWMM_OK);
        ASSERT_EQ(swmm_link_set_nodes(engine,
                      swmm_link_index(engine, "L1"),
                      swmm_node_index(engine, "J1"),
                      swmm_node_index(engine, "O1")), SWMM_OK);
    }
    void TearDown() override { if (engine) swmm_engine_destroy(engine); }

    std::string getopt(const char* key) {
        char buf[512] = {};
        EXPECT_EQ(swmm_options_get(engine, key, buf, sizeof(buf)), SWMM_OK);
        return std::string(buf);
    }
};

// ---------------------------------------------------------------------------
// Bool keys: defaults match SimulationOptions defaults; round-trip YES/NO.
// ---------------------------------------------------------------------------

TEST_F(ReportOptionsApiTest, BoolKeyDefaults) {
    // SimulationOptions.hpp: rpt_disabled=false, rpt_input=false,
    // rpt_continuity=true, rpt_flowstats=true, rpt_controls=false,
    // rpt_averages=false.
    EXPECT_EQ(getopt("RPT_DISABLED"),   "NO");
    EXPECT_EQ(getopt("RPT_INPUT"),      "NO");
    EXPECT_EQ(getopt("RPT_CONTINUITY"), "YES");
    EXPECT_EQ(getopt("RPT_FLOWSTATS"),  "YES");
    EXPECT_EQ(getopt("RPT_CONTROLS"),   "NO");
    EXPECT_EQ(getopt("RPT_AVERAGES"),   "NO");
}

TEST_F(ReportOptionsApiTest, BoolKeyRoundTrip) {
    // Flip each key and confirm get sees the new value.
    EXPECT_EQ(swmm_options_set(engine, "RPT_DISABLED",   "YES"), SWMM_OK);
    EXPECT_EQ(swmm_options_set(engine, "RPT_INPUT",      "YES"), SWMM_OK);
    EXPECT_EQ(swmm_options_set(engine, "RPT_CONTINUITY", "NO"),  SWMM_OK);
    EXPECT_EQ(swmm_options_set(engine, "RPT_FLOWSTATS",  "NO"),  SWMM_OK);
    EXPECT_EQ(swmm_options_set(engine, "RPT_CONTROLS",   "YES"), SWMM_OK);
    EXPECT_EQ(swmm_options_set(engine, "RPT_AVERAGES",   "YES"), SWMM_OK);

    EXPECT_EQ(getopt("RPT_DISABLED"),   "YES");
    EXPECT_EQ(getopt("RPT_INPUT"),      "YES");
    EXPECT_EQ(getopt("RPT_CONTINUITY"), "NO");
    EXPECT_EQ(getopt("RPT_FLOWSTATS"),  "NO");
    EXPECT_EQ(getopt("RPT_CONTROLS"),   "YES");
    EXPECT_EQ(getopt("RPT_AVERAGES"),   "YES");
}

TEST_F(ReportOptionsApiTest, BoolKeyAcceptsAlternateTokens) {
    // TRUE/FALSE/1/0 must work in addition to YES/NO.
    EXPECT_EQ(swmm_options_set(engine, "RPT_INPUT", "TRUE"),  SWMM_OK);
    EXPECT_EQ(getopt("RPT_INPUT"), "YES");
    EXPECT_EQ(swmm_options_set(engine, "RPT_INPUT", "FALSE"), SWMM_OK);
    EXPECT_EQ(getopt("RPT_INPUT"), "NO");
    EXPECT_EQ(swmm_options_set(engine, "RPT_INPUT", "1"),     SWMM_OK);
    EXPECT_EQ(getopt("RPT_INPUT"), "YES");
    EXPECT_EQ(swmm_options_set(engine, "RPT_INPUT", "0"),     SWMM_OK);
    EXPECT_EQ(getopt("RPT_INPUT"), "NO");
}

TEST_F(ReportOptionsApiTest, BoolKeyRejectsGarbage) {
    EXPECT_EQ(swmm_options_set(engine, "RPT_INPUT", "MAYBE"),
              SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// Selector keys: ALL/NONE/SOME round-trip.
// ---------------------------------------------------------------------------

TEST_F(ReportOptionsApiTest, SelectorDefaultsAreAll) {
    // Default for subcatch/nodes/links is 1 = ALL.
    EXPECT_EQ(getopt("RPT_SUBCATCHMENTS"), "ALL");
    EXPECT_EQ(getopt("RPT_NODES"),         "ALL");
    EXPECT_EQ(getopt("RPT_LINKS"),         "ALL");
}

TEST_F(ReportOptionsApiTest, SelectorAllNoneRoundTrip) {
    EXPECT_EQ(swmm_options_set(engine, "RPT_SUBCATCHMENTS", "NONE"), SWMM_OK);
    EXPECT_EQ(getopt("RPT_SUBCATCHMENTS"), "NONE");
    EXPECT_EQ(swmm_options_set(engine, "RPT_SUBCATCHMENTS", "ALL"),  SWMM_OK);
    EXPECT_EQ(getopt("RPT_SUBCATCHMENTS"), "ALL");

    EXPECT_EQ(swmm_options_set(engine, "RPT_NODES", "NONE"), SWMM_OK);
    EXPECT_EQ(getopt("RPT_NODES"), "NONE");
    EXPECT_EQ(swmm_options_set(engine, "RPT_LINKS", "NONE"), SWMM_OK);
    EXPECT_EQ(getopt("RPT_LINKS"), "NONE");
}

TEST_F(ReportOptionsApiTest, SelectorSomeListRoundTripCommaSeparated) {
    EXPECT_EQ(swmm_options_set(engine, "RPT_NODES", "J1,J2,J3"), SWMM_OK);
    EXPECT_EQ(getopt("RPT_NODES"), "J1,J2,J3");
}

TEST_F(ReportOptionsApiTest, SelectorSomeListAcceptsWhitespaceDelimiters) {
    // SWMM .inp [REPORT] lines use space-separated names — the setter
    // must accept them too so a GUI text box like "J1 J2 J3" works.
    EXPECT_EQ(swmm_options_set(engine, "RPT_LINKS", "L1 L2  L3"), SWMM_OK);
    EXPECT_EQ(getopt("RPT_LINKS"), "L1,L2,L3");
}

TEST_F(ReportOptionsApiTest, SelectorSomeListEmptyTokensSkipped) {
    EXPECT_EQ(swmm_options_set(engine, "RPT_SUBCATCHMENTS",
                                ",,,Sub1,,Sub2,,"),
              SWMM_OK);
    EXPECT_EQ(getopt("RPT_SUBCATCHMENTS"), "Sub1,Sub2");
}

TEST_F(ReportOptionsApiTest, SelectorEmptyStringCollapsesToNone) {
    EXPECT_EQ(swmm_options_set(engine, "RPT_NODES", "Foo,Bar"), SWMM_OK);
    EXPECT_EQ(swmm_options_set(engine, "RPT_NODES", ""), SWMM_OK);
    EXPECT_EQ(getopt("RPT_NODES"), "NONE");
}

TEST_F(ReportOptionsApiTest, UnknownRptKeyRejected) {
    char buf[16];
    EXPECT_EQ(swmm_options_get(engine, "RPT_BOGUS", buf, sizeof(buf)),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_options_set(engine, "RPT_BOGUS", "YES"),
              SWMM_ERR_BADPARAM);
}
