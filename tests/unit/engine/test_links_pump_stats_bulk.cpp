/**
 * @file test_links_pump_stats_bulk.cpp
 * @brief Unit tests for the bulk pump-statistics C API
 *        (`swmm_link_get_pump_stats_bulk`).
 *
 * @details The bulk getter is the single-call replacement for the three
 *          scalar accessors
 *            - swmm_link_get_stat_pump_cycles
 *            - swmm_link_get_stat_pump_on_time
 *            - swmm_link_get_stat_pump_volume
 *          and is intended to eliminate the @c 3N C-ABI crossings that the
 *          MCP server currently pays per "network-wide pump summary" call.
 *
 *          These tests verify the ABI contract — they do **not** attempt to
 *          validate the underlying statistics math (that is covered by
 *          @c test_gap_fixes.cpp / @c DiagPumpStats and by simulation-level
 *          tests with pump-bearing models). The site_drainage fixture used
 *          here has only conduit links, which exercises the sentinel path
 *          (cycles[i] == -1 for non-pumps); equivalence with the scalar
 *          getters on a pump-bearing model is covered by the matching
 *          Python test @c TestPumpStatsBulk in @c test_new_api.py.
 *
 *          Working directory is set to tests/unit/engine/data/ by
 *          @c tests/unit/engine/CMakeLists.txt.
 *
 * @see swmm_link_get_pump_stats_bulk
 * @see docs/C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md Phase 1
 * @ingroup engine_tests
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>

namespace {

// ---------------------------------------------------------------------------
// Fixture — opens and initialises the site_drainage_model.inp fixture so the
// per-link stat_pump_* vectors are sized. The site drainage model contains
// only conduit links; this exclusively exercises the non-pump sentinel path
// of the bulk getter (which is the worst case for sentinel emission).
// ---------------------------------------------------------------------------
class PumpStatsBulkTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int n_links_ = 0;

    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_,
                                    "site_drainage_model.inp",
                                    "site_drainage_model.rpt",
                                    "site_drainage_model.out",
                                    nullptr),
                  SWMM_OK)
            << "open failed: " << swmm_get_last_error_msg(engine_);
        ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
        n_links_ = swmm_link_count(engine_);
        ASSERT_GT(n_links_, 0);
    }

    void TearDown() override {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }
};

// ---------------------------------------------------------------------------
// Contract 1 — happy path: caller-provided full-length buffers are filled.
// For an all-conduit model every cycles entry must be the documented sentinel
// (-1) and the corresponding on_time / volume must be zero.
// ---------------------------------------------------------------------------
TEST_F(PumpStatsBulkTest, FullBufferAllConduitsYieldsSentinel) {
    std::vector<int>    cycles(n_links_, 0xDEADBEEF);
    std::vector<double> on_time(n_links_, -42.0);
    std::vector<double> volume(n_links_, -42.0);

    ASSERT_EQ(swmm_link_get_pump_stats_bulk(engine_,
                                             cycles.data(),
                                             on_time.data(),
                                             volume.data(),
                                             n_links_),
              SWMM_OK);

    for (int i = 0; i < n_links_; ++i) {
        EXPECT_EQ(cycles[i],  -1)  << "link " << i;
        EXPECT_EQ(on_time[i], 0.0) << "link " << i;
        EXPECT_EQ(volume[i],  0.0) << "link " << i;
    }
}

// ---------------------------------------------------------------------------
// Contract 2 — equivalence with scalar getters. For each link, the bulk
// outputs must match what the scalar accessor returns. (For non-pump links,
// the scalar accessor still returns the underlying vector entry — usually 0 —
// while the bulk getter substitutes the documented sentinel. We therefore
// only assert exact equality for pump links, and assert the sentinel for
// non-pump links.)
// ---------------------------------------------------------------------------
TEST_F(PumpStatsBulkTest, EquivalenceWithScalarGetters) {
    std::vector<int>    cycles(n_links_);
    std::vector<double> on_time(n_links_);
    std::vector<double> volume(n_links_);

    ASSERT_EQ(swmm_link_get_pump_stats_bulk(engine_,
                                             cycles.data(),
                                             on_time.data(),
                                             volume.data(),
                                             n_links_),
              SWMM_OK);

    for (int i = 0; i < n_links_; ++i) {
        int scalar_cycles = 0xC0FFEE;
        double scalar_ontime = -1.0;
        double scalar_volume = -1.0;
        ASSERT_EQ(swmm_link_get_stat_pump_cycles(engine_, i, &scalar_cycles), SWMM_OK);
        ASSERT_EQ(swmm_link_get_stat_pump_on_time(engine_, i, &scalar_ontime), SWMM_OK);
        ASSERT_EQ(swmm_link_get_stat_pump_volume(engine_, i, &scalar_volume), SWMM_OK);

        if (cycles[i] == -1) {
            // Non-pump: bulk substitutes the sentinel; scalar returns the
            // raw vector entry. Both representations should be consistent
            // with "no pump activity recorded".
            EXPECT_EQ(scalar_cycles, 0);
            EXPECT_EQ(on_time[i], 0.0);
            EXPECT_EQ(volume[i], 0.0);
        } else {
            // Pump: bulk and scalar must agree bit-for-bit.
            EXPECT_EQ(cycles[i], scalar_cycles);
            EXPECT_EQ(on_time[i], scalar_ontime);
            EXPECT_EQ(volume[i], scalar_volume);
        }
    }
}

// ---------------------------------------------------------------------------
// Contract 3 — partial buffer (count < n_links). Only the first @c count
// entries are written; the tail of the caller's buffer is left untouched.
// ---------------------------------------------------------------------------
TEST_F(PumpStatsBulkTest, PartialBufferWritesOnlyRequestedPrefix) {
    ASSERT_GE(n_links_, 2);
    const int half = n_links_ / 2;

    std::vector<int>    cycles(n_links_, 0xDEADBEEF);
    std::vector<double> on_time(n_links_, -42.0);
    std::vector<double> volume(n_links_, -42.0);

    ASSERT_EQ(swmm_link_get_pump_stats_bulk(engine_,
                                             cycles.data(),
                                             on_time.data(),
                                             volume.data(),
                                             half),
              SWMM_OK);

    for (int i = 0; i < half; ++i) {
        EXPECT_NE(cycles[i],  0xDEADBEEF) << "prefix entry " << i << " not written";
        EXPECT_NE(on_time[i], -42.0)      << "prefix entry " << i << " not written";
        EXPECT_NE(volume[i],  -42.0)      << "prefix entry " << i << " not written";
    }
    for (int i = half; i < n_links_; ++i) {
        EXPECT_EQ(cycles[i],  0xDEADBEEF) << "suffix entry " << i << " was clobbered";
        EXPECT_EQ(on_time[i], -42.0)      << "suffix entry " << i << " was clobbered";
        EXPECT_EQ(volume[i],  -42.0)      << "suffix entry " << i << " was clobbered";
    }
}

// ---------------------------------------------------------------------------
// Contract 4 — selective output. Any subset of the three pointers may be
// non-NULL; the others are skipped. The iteration cost is identical (this
// is by design and documented in the header).
// ---------------------------------------------------------------------------
TEST_F(PumpStatsBulkTest, SelectiveOutputCyclesOnly) {
    std::vector<int> cycles(n_links_, 0xDEADBEEF);
    ASSERT_EQ(swmm_link_get_pump_stats_bulk(engine_,
                                             cycles.data(),
                                             /*on_time=*/nullptr,
                                             /*volume=*/nullptr,
                                             n_links_),
              SWMM_OK);
    for (int i = 0; i < n_links_; ++i) {
        EXPECT_EQ(cycles[i], -1) << "link " << i;
    }
}

TEST_F(PumpStatsBulkTest, SelectiveOutputOnTimeOnly) {
    std::vector<double> on_time(n_links_, -42.0);
    ASSERT_EQ(swmm_link_get_pump_stats_bulk(engine_,
                                             /*cycles=*/nullptr,
                                             on_time.data(),
                                             /*volume=*/nullptr,
                                             n_links_),
              SWMM_OK);
    for (int i = 0; i < n_links_; ++i) {
        EXPECT_EQ(on_time[i], 0.0) << "link " << i;
    }
}

// ---------------------------------------------------------------------------
// Contract 5 — bad parameters. The function must reject obviously invalid
// inputs at entry without writing to caller memory.
// ---------------------------------------------------------------------------
TEST_F(PumpStatsBulkTest, RejectsNullEngine) {
    std::vector<int>    cycles(n_links_, 0xDEADBEEF);
    std::vector<double> on_time(n_links_, -42.0);
    std::vector<double> volume(n_links_, -42.0);
    EXPECT_EQ(swmm_link_get_pump_stats_bulk(nullptr,
                                             cycles.data(),
                                             on_time.data(),
                                             volume.data(),
                                             n_links_),
              SWMM_ERR_BADHANDLE);
    // Buffers must not have been written.
    EXPECT_EQ(cycles.front(),  0xDEADBEEF);
    EXPECT_EQ(on_time.front(), -42.0);
    EXPECT_EQ(volume.front(),  -42.0);
}

TEST_F(PumpStatsBulkTest, RejectsNonPositiveCount) {
    int cycles = 0xDEADBEEF;
    double on_time = -42.0;
    double volume = -42.0;
    EXPECT_EQ(swmm_link_get_pump_stats_bulk(engine_,
                                             &cycles, &on_time, &volume, 0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_link_get_pump_stats_bulk(engine_,
                                             &cycles, &on_time, &volume, -3),
              SWMM_ERR_BADPARAM);
    // Untouched.
    EXPECT_EQ(cycles,  0xDEADBEEF);
    EXPECT_EQ(on_time, -42.0);
    EXPECT_EQ(volume,  -42.0);
}

TEST_F(PumpStatsBulkTest, RejectsAllNullOutputs) {
    // Calling with all three outputs null is almost certainly a programming
    // error; the function returns BADPARAM rather than silently succeeding.
    EXPECT_EQ(swmm_link_get_pump_stats_bulk(engine_,
                                             nullptr, nullptr, nullptr,
                                             n_links_),
              SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// Contract 6 — count > n_links is clipped (consistent with other *_bulk
// getters in this API, e.g. swmm_link_get_flows_bulk). No write occurs past
// the n_links-th entry.
// ---------------------------------------------------------------------------
TEST_F(PumpStatsBulkTest, OversizedCountIsClippedAtNLinks) {
    const int oversized = n_links_ + 7;
    std::vector<int>    cycles(oversized, 0xDEADBEEF);
    std::vector<double> on_time(oversized, -42.0);
    std::vector<double> volume(oversized, -42.0);

    ASSERT_EQ(swmm_link_get_pump_stats_bulk(engine_,
                                             cycles.data(),
                                             on_time.data(),
                                             volume.data(),
                                             oversized),
              SWMM_OK);

    for (int i = n_links_; i < oversized; ++i) {
        EXPECT_EQ(cycles[i],  0xDEADBEEF) << "tail entry " << i << " was clobbered";
        EXPECT_EQ(on_time[i], -42.0)      << "tail entry " << i << " was clobbered";
        EXPECT_EQ(volume[i],  -42.0)      << "tail entry " << i << " was clobbered";
    }
}

} // namespace
