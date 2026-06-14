/**
 * @file test_output_node_stats.cpp
 * @brief Slice QA-01 — exercise swmm_output_get_node_stat_*.
 *
 * Uses the existing site_drainage_model.out fixture (already shipped in
 * tests/unit/engine/data/) so the test runs without re-simulating.
 * Tests focus on the API contract rather than absolute values:
 *  - Returns 0 (success) on a valid (handle, node_idx) pair.
 *  - Returns -1 on a NULL handle, NULL value pointer, or out-of-range
 *    node index.
 *  - max_depth >= 0 and finite for every node in the file.
 *  - max_overflow >= 0 and finite for every node.
 *  - vol_flooded >= 0 (the running sum is always non-negative since
 *    only positive overflow values contribute).
 *  - time_flooded is a multiple of report_step (count × dt math).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_output.h>

#include <cmath>
#include <string>

namespace {

constexpr const char* kFixturePath = "site_drainage_model.out";

// Open the fixture for each test so failures don't bleed across cases.
// CTest sets WORKING_DIRECTORY to tests/unit/engine/data, so the bare
// filename resolves correctly.
class OutputNodeStatsTest : public ::testing::Test {
protected:
    void SetUp() override {
        handle_ = swmm_output_open(kFixturePath);
        ASSERT_NE(handle_, nullptr)
            << "Failed to open fixture '" << kFixturePath
            << "' — check the test WORKING_DIRECTORY is data/.";
    }
    void TearDown() override {
        if (handle_) swmm_output_close(handle_);
    }
    SWMM_Output handle_ = nullptr;
};

} // anonymous

// ---------------------------------------------------------------------------
// Smoke: fixture has at least one node + one period.
// ---------------------------------------------------------------------------

TEST_F(OutputNodeStatsTest, FixtureHasNodesAndPeriods)
{
    EXPECT_GT(swmm_output_get_node_count(handle_), 0);
    EXPECT_GT(swmm_output_get_period_count(handle_), 0);
}

// ---------------------------------------------------------------------------
// Happy path — each getter returns 0 (success) and a finite value for
// every node in the file.
// ---------------------------------------------------------------------------

TEST_F(OutputNodeStatsTest, MaxDepthAllNodesNonNegativeFinite)
{
    const int n = swmm_output_get_node_count(handle_);
    for (int i = 0; i < n; ++i) {
        double v = -1.0;
        ASSERT_EQ(swmm_output_get_node_stat_max_depth(handle_, i, &v), 0)
            << "node_idx=" << i;
        EXPECT_TRUE(std::isfinite(v)) << "node_idx=" << i;
        EXPECT_GE(v, 0.0) << "node_idx=" << i;
    }
}

TEST_F(OutputNodeStatsTest, MaxOverflowAllNodesNonNegativeFinite)
{
    const int n = swmm_output_get_node_count(handle_);
    for (int i = 0; i < n; ++i) {
        double v = -1.0;
        ASSERT_EQ(swmm_output_get_node_stat_max_overflow(handle_, i, &v), 0)
            << "node_idx=" << i;
        EXPECT_TRUE(std::isfinite(v)) << "node_idx=" << i;
        EXPECT_GE(v, 0.0) << "node_idx=" << i;
    }
}

TEST_F(OutputNodeStatsTest, VolFloodedAllNodesNonNegativeFinite)
{
    const int n = swmm_output_get_node_count(handle_);
    for (int i = 0; i < n; ++i) {
        double v = -1.0;
        ASSERT_EQ(swmm_output_get_node_stat_vol_flooded(handle_, i, &v), 0)
            << "node_idx=" << i;
        EXPECT_TRUE(std::isfinite(v)) << "node_idx=" << i;
        EXPECT_GE(v, 0.0) << "node_idx=" << i;
    }
}

TEST_F(OutputNodeStatsTest, TimeFloodedAllNodesMultipleOfReportStep)
{
    const int n = swmm_output_get_node_count(handle_);
    const int reportStep = swmm_output_get_report_step(handle_);
    ASSERT_GT(reportStep, 0);

    for (int i = 0; i < n; ++i) {
        double v = -1.0;
        ASSERT_EQ(swmm_output_get_node_stat_time_flooded(handle_, i, &v), 0)
            << "node_idx=" << i;
        EXPECT_TRUE(std::isfinite(v)) << "node_idx=" << i;
        EXPECT_GE(v, 0.0) << "node_idx=" << i;

        // time_flooded must be N × report_step for some non-negative N.
        // The remainder-from-division check stays robust under float
        // round-trip because both sides are exact-integer doubles for
        // typical report-step values (60, 300, 3600 s, …).
        const double n_steps = v / static_cast<double>(reportStep);
        EXPECT_DOUBLE_EQ(n_steps, std::round(n_steps)) << "node_idx=" << i;
    }
}

// ---------------------------------------------------------------------------
// Cross-stat consistency — when vol_flooded > 0, time_flooded must also
// be > 0 (you can't accumulate volume in zero time). Same applies for
// max_overflow > 0.
// ---------------------------------------------------------------------------

TEST_F(OutputNodeStatsTest, VolFloodedImpliesTimeFlooded)
{
    const int n = swmm_output_get_node_count(handle_);
    for (int i = 0; i < n; ++i) {
        double vol = 0.0, t = 0.0, ov = 0.0;
        ASSERT_EQ(swmm_output_get_node_stat_vol_flooded(handle_, i, &vol), 0);
        ASSERT_EQ(swmm_output_get_node_stat_time_flooded(handle_, i, &t), 0);
        ASSERT_EQ(swmm_output_get_node_stat_max_overflow(handle_, i, &ov), 0);

        if (vol > 0.0) {
            EXPECT_GT(t,  0.0) << "node_idx=" << i;
            EXPECT_GT(ov, 0.0) << "node_idx=" << i;
        }
    }
}

// ---------------------------------------------------------------------------
// Error paths — invalid arguments must return -1 without touching `value`.
// ---------------------------------------------------------------------------

TEST_F(OutputNodeStatsTest, NullHandleReturnsMinusOne)
{
    double v = 42.0;
    EXPECT_EQ(swmm_output_get_node_stat_max_depth(nullptr,    0, &v), -1);
    EXPECT_EQ(swmm_output_get_node_stat_max_overflow(nullptr, 0, &v), -1);
    EXPECT_EQ(swmm_output_get_node_stat_vol_flooded(nullptr,  0, &v), -1);
    EXPECT_EQ(swmm_output_get_node_stat_time_flooded(nullptr, 0, &v), -1);
    EXPECT_DOUBLE_EQ(v, 42.0) << "value buffer must not be touched on error";
}

TEST_F(OutputNodeStatsTest, NullValueReturnsMinusOne)
{
    EXPECT_EQ(swmm_output_get_node_stat_max_depth(handle_,    0, nullptr), -1);
    EXPECT_EQ(swmm_output_get_node_stat_max_overflow(handle_, 0, nullptr), -1);
    EXPECT_EQ(swmm_output_get_node_stat_vol_flooded(handle_,  0, nullptr), -1);
    EXPECT_EQ(swmm_output_get_node_stat_time_flooded(handle_, 0, nullptr), -1);
}

TEST_F(OutputNodeStatsTest, OutOfRangeNodeIdxReturnsMinusOne)
{
    const int n = swmm_output_get_node_count(handle_);
    double v = 42.0;
    EXPECT_EQ(swmm_output_get_node_stat_max_depth(handle_,    -1, &v), -1);
    EXPECT_EQ(swmm_output_get_node_stat_max_depth(handle_,     n, &v), -1);
    EXPECT_EQ(swmm_output_get_node_stat_max_overflow(handle_, -1, &v), -1);
    EXPECT_EQ(swmm_output_get_node_stat_max_overflow(handle_,  n, &v), -1);
    EXPECT_EQ(swmm_output_get_node_stat_vol_flooded(handle_,  -1, &v), -1);
    EXPECT_EQ(swmm_output_get_node_stat_vol_flooded(handle_,   n, &v), -1);
    EXPECT_EQ(swmm_output_get_node_stat_time_flooded(handle_, -1, &v), -1);
    EXPECT_EQ(swmm_output_get_node_stat_time_flooded(handle_,  n, &v), -1);
    EXPECT_DOUBLE_EQ(v, 42.0) << "value buffer must not be touched on error";
}
