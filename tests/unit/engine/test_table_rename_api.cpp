/**
 * @file      test_table_rename_api.cpp
 * @brief     swmm_table_rename — renaming a timeseries or curve in place.
 *
 * The GUI adds a curve or timeseries to the engine as soon as it is created
 * (so pumps, gages, outfalls … can reference it while its editor is still
 * open) and must then be able to rename it without leaving a stale copy.
 * Covers: argument validation, same-name no-op, kind-scoped collisions,
 * lookup by the new name, and the name-based reference copies the writer and
 * deferred resolution read (gage, external inflow). Index references (pump
 * curve) are unaffected by a rename.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gages.h>
#include <openswmm/engine/openswmm_inflows.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_tables.h>

#include <string>

class TableRenameTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    /**
     * @brief Create an editable model with one J1 junction.
     *
     * Owns the stored engine handle until fixture teardown. Each allocation/add status is asserted; a
     * fatal assertion leaves any partial model for teardown.
     */
    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
    }
    /**
     * @brief Destroy the fixture engine when allocation succeeded.
     *
     * Releases model resources through engine destruction; the member value is not reset and must not
     * be reused after teardown.
     */
    void TearDown() override {
        if (engine)
            swmm_engine_destroy(engine);
    }

    /**
     * @brief Add a curve and locate its last matching non-timeseries table row.
     *
     * Records nonfatal add expectation, ignores per-row type-getter failures and continues scanning
     * rather than returning the first match. Requires valid table ID pointers; no points, validation or
     * simulation are supplied.
     *
     * @param[in] id Non-null curve ID expected to be addable in the editable fixture engine.
     * @param[in] type Curve type code, default nine for PUMP3.
     * @return Matching table index, or -1 when no matching nonzero-type table is found.
     */
    int curve(const char* id, int type = 9 /* PUMP3 */) {
        EXPECT_EQ(swmm_curve_add(engine, id, type), SWMM_OK);
        int idx = -1;
        const int n = swmm_table_count(engine);
        for (int i = 0; i < n; ++i) {
            int t = -1;
            swmm_table_get_type(engine, i, &t);
            if (t != 0 && std::string(swmm_table_id(engine, i)) == id)
                idx = i;
        }
        return idx;
    }
};

TEST_F(TableRenameTest, RejectsEmptyNullAndBadIndex) {
    const int c = curve("PC1");
    EXPECT_EQ(swmm_table_rename(engine, c, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_table_rename(engine, c, ""), SWMM_ERR_BADPARAM);
    EXPECT_NE(swmm_table_rename(engine, 99, "X"), SWMM_OK);
    EXPECT_STREQ(swmm_table_id(engine, c), "PC1");
}

TEST_F(TableRenameTest, SameNameIsNoopAndCaseRespellingAllowed) {
    const int c = curve("PC1");
    EXPECT_EQ(swmm_table_rename(engine, c, "PC1"), SWMM_OK);
    EXPECT_EQ(swmm_table_rename(engine, c, "pc1"), SWMM_OK);
    EXPECT_STREQ(swmm_table_id(engine, c), "pc1");
}

TEST_F(TableRenameTest, CollisionIsKindScoped) {
    const int a = curve("A");
    curve("B");
    ASSERT_EQ(swmm_timeseries_add(engine, "TS"), SWMM_OK);
    // Another curve already uses the name (any case).
    EXPECT_EQ(swmm_table_rename(engine, a, "b"), SWMM_ERR_BADPARAM);
    EXPECT_STREQ(swmm_table_id(engine, a), "A");
    // A curve may share a name with a timeseries, as swmm_curve_add allows.
    EXPECT_EQ(swmm_table_rename(engine, a, "TS"), SWMM_OK);
}

TEST_F(TableRenameTest, LookupFollowsAndIndexReferencesSurvive) {
    const int c = curve("OLD");
    ASSERT_EQ(swmm_link_add(engine, "P1", SWMM_LINK_PUMP), SWMM_OK);
    const int p = swmm_link_index(engine, "P1");
    ASSERT_EQ(swmm_link_set_pump_curve(engine, p, c), SWMM_OK);

    ASSERT_EQ(swmm_table_rename(engine, c, "NEW"), SWMM_OK);
    EXPECT_EQ(swmm_table_index(engine, "OLD"), -1);
    EXPECT_EQ(swmm_table_index(engine, "NEW"), c);
    int ref = -2;
    ASSERT_EQ(swmm_link_get_pump_curve(engine, p, &ref), SWMM_OK);
    EXPECT_EQ(ref, c);
}

TEST_F(TableRenameTest, TimeseriesRenameRewritesNameReferences) {
    ASSERT_EQ(swmm_timeseries_add(engine, "RAIN"), SWMM_OK);
    const int ts = swmm_table_index(engine, "RAIN");
    ASSERT_GE(ts, 0);
    // A same-named curve must not be touched by a timeseries rename.
    curve("RAIN", 1 /* STORAGE */);

    ASSERT_EQ(swmm_gage_add(engine, "G1"), SWMM_OK);
    const int g = swmm_gage_index(engine, "G1");
    ASSERT_EQ(swmm_gage_set_timeseries(engine, g, "RAIN"), SWMM_OK);
    ASSERT_EQ(swmm_ext_inflow_add(engine, swmm_node_index(engine, "J1"), "FLOW", "RAIN", "FLOW",
                                  1.0, 1.0, 0.0, ""),
              SWMM_OK);

    ASSERT_EQ(swmm_table_rename(engine, ts, "STORM"), SWMM_OK);

    char buf[64] = {};
    ASSERT_EQ(swmm_gage_get_timeseries(engine, g, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "STORM");

    int n = -1;
    char c[64] = {}, tsn[64] = {}, tp[64] = {}, pat[64] = {};
    double mf = 0, sf = 0, base = 0;
    ASSERT_EQ(swmm_ext_inflow_get(engine, 0, &n, c, sizeof c, tsn, sizeof tsn, tp, sizeof tp, &mf,
                                  &sf, &base, pat, sizeof pat),
              SWMM_OK);
    EXPECT_STREQ(tsn, "STORM");

    // The curve that shared the old name keeps it.
    bool curveKept = false;
    for (int i = 0; i < swmm_table_count(engine); ++i) {
        int t = -1;
        swmm_table_get_type(engine, i, &t);
        if (t == 1 && std::string(swmm_table_id(engine, i)) == "RAIN")
            curveKept = true;
    }
    EXPECT_TRUE(curveKept);
}

// swmm_table_set_type — the GUI adds a curve at creation, so a later change
// of curve type in the editor must reach the engine without re-creating it.
TEST_F(TableRenameTest, SetTypeChangesCurveTypeAndKeepsReferences) {
    const int c = curve("PC", 7 /* PUMP1 */);
    ASSERT_EQ(swmm_table_add_point(engine, c, 1.0, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "P1", SWMM_LINK_PUMP), SWMM_OK);
    const int p = swmm_link_index(engine, "P1");
    ASSERT_EQ(swmm_link_set_pump_curve(engine, p, c), SWMM_OK);

    ASSERT_EQ(swmm_table_set_type(engine, c, 9 /* PUMP3 */), SWMM_OK);
    int t = -1;
    ASSERT_EQ(swmm_table_get_type(engine, c, &t), SWMM_OK);
    EXPECT_EQ(t, 9);
    int n = 0;
    ASSERT_EQ(swmm_table_get_point_count(engine, c, &n), SWMM_OK);
    EXPECT_EQ(n, 1);
    int ref = -2;
    ASSERT_EQ(swmm_link_get_pump_curve(engine, p, &ref), SWMM_OK);
    EXPECT_EQ(ref, c);
}

TEST_F(TableRenameTest, SetTypeRejectsTimeseriesBothWays) {
    const int c = curve("PC", 7);
    EXPECT_EQ(swmm_table_set_type(engine, c, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_table_set_type(engine, c, 12), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_timeseries_add(engine, "TS"), SWMM_OK);
    EXPECT_EQ(swmm_table_set_type(engine, swmm_table_index(engine, "TS"), 7), SWMM_ERR_BADPARAM);
}
