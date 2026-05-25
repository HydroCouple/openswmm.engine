/**
 * @file test_da4_engine_api.cpp
 * @brief DA.4.0 — Unit tests for the engine prerequisites of GUI Slice DA.4.
 *
 * @details Covers two C API additions wired for the GUI's outfall stage-data
 *          picker and the rich pattern editor:
 *            - swmm_node_get_outfall_tidal / _get_outfall_timeseries
 *              (read back the union-typed outfall_param slot with a type guard
 *               so callers can distinguish "unassigned" from a genuine index of 0)
 *            - swmm_pattern_get_type / _get_factor_count / _get_factor
 *              (round-trip the multiplier vector that swmm_pattern_set_factors
 *               writes; needed so the editor and DWF pickers can read the
 *               current state)
 *
 * @see docs/GUI_IMPLEMENTATION_PLAN.md slice DA.4 (openswmm.gui)
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_tables.h>

namespace {

class OutfallStageDataApiTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;
    int out_idx = -1;
    int curve_a_idx = -1;
    int ts_a_idx = -1;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_node_add(engine, "OUT1", SWMM_NODE_OUTFALL), SWMM_OK);
        out_idx = swmm_node_index(engine, "OUT1");
        ASSERT_GE(out_idx, 0);

        ASSERT_EQ(swmm_curve_add(engine, "TC1", 2 /*TIDAL*/), SWMM_OK);
        curve_a_idx = swmm_table_index(engine, "TC1");
        ASSERT_GE(curve_a_idx, 0);

        ASSERT_EQ(swmm_timeseries_add(engine, "TS1"), SWMM_OK);
        ts_a_idx = swmm_table_index(engine, "TS1");
        ASSERT_GE(ts_a_idx, 0);
    }

    void TearDown() override { if (engine) swmm_engine_destroy(engine); }
};

// Case 1 — set TIDAL with curve_a → get_outfall_tidal returns that idx.
TEST_F(OutfallStageDataApiTest, GetTidalRoundtripsAfterSet) {
    ASSERT_EQ(swmm_node_set_outfall_tidal(engine, out_idx, curve_a_idx), SWMM_OK);
    int got = -999;
    ASSERT_EQ(swmm_node_get_outfall_tidal(engine, out_idx, &got), SWMM_OK);
    EXPECT_EQ(got, curve_a_idx);
}

// Case 2 — set TIMESERIES → get_outfall_timeseries returns that idx.
TEST_F(OutfallStageDataApiTest, GetTimeseriesRoundtripsAfterSet) {
    ASSERT_EQ(swmm_node_set_outfall_timeseries(engine, out_idx, ts_a_idx), SWMM_OK);
    int got = -999;
    ASSERT_EQ(swmm_node_get_outfall_timeseries(engine, out_idx, &got), SWMM_OK);
    EXPECT_EQ(got, ts_a_idx);
}

// Case 3 — get_outfall_tidal on a FIXED-typed outfall returns BADPARAM
// (so the GUI can distinguish unassigned from genuine idx-zero).
TEST_F(OutfallStageDataApiTest, GetTidalRejectsFixedType) {
    ASSERT_EQ(swmm_node_set_outfall_stage(engine, out_idx, 12.5), SWMM_OK);
    int got = -999;
    EXPECT_EQ(swmm_node_get_outfall_tidal(engine, out_idx, &got), SWMM_ERR_BADPARAM);
    EXPECT_EQ(got, -999);  // untouched
}

// Case 4 — get_outfall_timeseries on a TIDAL-typed outfall returns BADPARAM.
TEST_F(OutfallStageDataApiTest, GetTimeseriesRejectsTidalType) {
    ASSERT_EQ(swmm_node_set_outfall_tidal(engine, out_idx, curve_a_idx), SWMM_OK);
    int got = -999;
    EXPECT_EQ(swmm_node_get_outfall_timeseries(engine, out_idx, &got), SWMM_ERR_BADPARAM);
}

// ============================================================================
// Pattern getters
// ============================================================================

class PatternGetterApiTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
    }

    void TearDown() override { if (engine) swmm_engine_destroy(engine); }
};

// Case 5 — pattern type round-trips across all four kinds.
TEST_F(PatternGetterApiTest, GetTypeRoundtripsAllKinds) {
    const int kinds[4][2] = {
        {0 /*MONTHLY*/, 12}, {1 /*DAILY*/, 7},
        {2 /*HOURLY*/,  24}, {3 /*WEEKEND*/, 24},
    };
    const char* names[4] = {"PM", "PD", "PH", "PW"};

    for (int k = 0; k < 4; ++k) {
        ASSERT_EQ(swmm_pattern_add(engine, names[k], kinds[k][0]), SWMM_OK);
        const int idx = swmm_pattern_index(engine, names[k]);
        ASSERT_GE(idx, 0);
        int got = -1;
        ASSERT_EQ(swmm_pattern_get_type(engine, idx, &got), SWMM_OK);
        EXPECT_EQ(got, kinds[k][0]);
    }
}

// Case 6 — factor_count + factor round-trip after set_factors.
TEST_F(PatternGetterApiTest, GetFactorCountAndFactorRoundtrip) {
    ASSERT_EQ(swmm_pattern_add(engine, "PH", 2 /*HOURLY*/), SWMM_OK);
    const int idx = swmm_pattern_index(engine, "PH");
    ASSERT_GE(idx, 0);

    double f24[24];
    for (int i = 0; i < 24; ++i) f24[i] = 1.0 + 0.1 * i;
    ASSERT_EQ(swmm_pattern_set_factors(engine, idx, f24, 24), SWMM_OK);

    int count = -1;
    ASSERT_EQ(swmm_pattern_get_factor_count(engine, idx, &count), SWMM_OK);
    EXPECT_EQ(count, 24);

    for (int i = 0; i < 24; ++i) {
        double v = -1.0;
        ASSERT_EQ(swmm_pattern_get_factor(engine, idx, i, &v), SWMM_OK);
        EXPECT_DOUBLE_EQ(v, f24[i]);
    }
}

// Case 7 — out-of-range factor index returns an error (the CHECK_INDEX macro
// returns SWMM_ERR_INDEX). Asserts the call fails — caller treats any non-OK
// as "unavailable" without depending on the specific code.
TEST_F(PatternGetterApiTest, GetFactorOutOfRangeIsRejected) {
    ASSERT_EQ(swmm_pattern_add(engine, "PM", 0 /*MONTHLY*/), SWMM_OK);
    const int idx = swmm_pattern_index(engine, "PM");
    ASSERT_GE(idx, 0);
    double f12[12] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    ASSERT_EQ(swmm_pattern_set_factors(engine, idx, f12, 12), SWMM_OK);

    double v = -1.0;
    EXPECT_NE(swmm_pattern_get_factor(engine, idx, 12, &v), SWMM_OK);
    EXPECT_NE(swmm_pattern_get_factor(engine, idx, -1, &v), SWMM_OK);
}

} // namespace
