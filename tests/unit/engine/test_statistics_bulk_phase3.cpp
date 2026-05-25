/**
 * @file test_statistics_bulk_phase3.cpp
 * @brief Unit tests for the Phase 3 statistics-bulk C API additions —
 *        @ref swmm_stat_node_max_overflow_bulk,
 *        @ref swmm_stat_node_vol_flooded_bulk,
 *        @ref swmm_stat_node_time_flooded_bulk, and
 *        @ref swmm_stat_subcatch_max_runoff_bulk.
 *
 * @details These functions read cumulative statistics that are populated by
 *          the simulation, so the fixture runs the full site_drainage_model
 *          through to ENDED before the assertions execute. Numerical
 *          equivalence with the scalar accessors is the primary contract;
 *          bad-param paths are also exercised.
 *
 * @see docs/C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md Phase 3 — Statistics batch
 * @ingroup engine_tests
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_statistics.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_subcatchments.h>

namespace {

class StatsBulkPhase3Test : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int n_nodes_ = 0;
    int n_subs_ = 0;

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
        ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);

        // Drive the simulation to completion so the cumulative stat
        // vectors are populated. The fixture is small enough that this
        // is fast (< 1s on a laptop) — see test_site_drainage_model for
        // the full simulation contract test.
        double elapsed = 0.0;
        while (true) {
            int rc = swmm_engine_step(engine_, &elapsed);
            if (rc != 0) break;
            if (elapsed <= 0.0) break;
        }
        ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);

        n_nodes_ = swmm_node_count(engine_);
        n_subs_ = swmm_subcatch_count(engine_);
        ASSERT_GT(n_nodes_, 0);
        ASSERT_GT(n_subs_, 0);
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
// Equivalence — each bulk getter must match the scalar accessor for every
// element of the model.
// ---------------------------------------------------------------------------

TEST_F(StatsBulkPhase3Test, NodeMaxOverflowBulkMatchesScalar) {
    std::vector<double> bulk(n_nodes_, -42.0);
    ASSERT_EQ(swmm_stat_node_max_overflow_bulk(engine_, bulk.data(), n_nodes_),
              SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_stat_node_max_overflow(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "node " << i;
    }
}

TEST_F(StatsBulkPhase3Test, NodeVolFloodedBulkMatchesScalar) {
    std::vector<double> bulk(n_nodes_, -42.0);
    ASSERT_EQ(swmm_stat_node_vol_flooded_bulk(engine_, bulk.data(), n_nodes_),
              SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_stat_node_vol_flooded(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "node " << i;
    }
}

TEST_F(StatsBulkPhase3Test, NodeTimeFloodedBulkMatchesScalar) {
    std::vector<double> bulk(n_nodes_, -42.0);
    ASSERT_EQ(swmm_stat_node_time_flooded_bulk(engine_, bulk.data(), n_nodes_),
              SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_stat_node_time_flooded(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "node " << i;
    }
}

TEST_F(StatsBulkPhase3Test, SubcatchMaxRunoffBulkMatchesScalar) {
    std::vector<double> bulk(n_subs_, -42.0);
    ASSERT_EQ(swmm_stat_subcatch_max_runoff_bulk(engine_, bulk.data(), n_subs_),
              SWMM_OK);
    for (int i = 0; i < n_subs_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_stat_subcatch_max_runoff(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "subcatch " << i;
    }
}

// ---------------------------------------------------------------------------
// Non-negativity sanity — these are cumulative non-negative quantities. A
// regression that read the wrong SoA column would likely produce negatives.
// ---------------------------------------------------------------------------

TEST_F(StatsBulkPhase3Test, AllStatsAreNonNegative) {
    std::vector<double> mo(n_nodes_), vf(n_nodes_), tf(n_nodes_);
    std::vector<double> sr(n_subs_);
    ASSERT_EQ(swmm_stat_node_max_overflow_bulk(engine_, mo.data(), n_nodes_), SWMM_OK);
    ASSERT_EQ(swmm_stat_node_vol_flooded_bulk(engine_, vf.data(), n_nodes_), SWMM_OK);
    ASSERT_EQ(swmm_stat_node_time_flooded_bulk(engine_, tf.data(), n_nodes_), SWMM_OK);
    ASSERT_EQ(swmm_stat_subcatch_max_runoff_bulk(engine_, sr.data(), n_subs_), SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        EXPECT_GE(mo[i], 0.0) << "node " << i << " max_overflow";
        EXPECT_GE(vf[i], 0.0) << "node " << i << " vol_flooded";
        EXPECT_GE(tf[i], 0.0) << "node " << i << " time_flooded";
    }
    for (int i = 0; i < n_subs_; ++i) {
        EXPECT_GE(sr[i], 0.0) << "subcatch " << i << " max_runoff";
    }
}

// ---------------------------------------------------------------------------
// Bad-param contracts.
// ---------------------------------------------------------------------------

TEST_F(StatsBulkPhase3Test, RejectsNullBuffer) {
    EXPECT_EQ(swmm_stat_node_max_overflow_bulk(engine_, nullptr, n_nodes_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_stat_node_vol_flooded_bulk(engine_, nullptr, n_nodes_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_stat_node_time_flooded_bulk(engine_, nullptr, n_nodes_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_stat_subcatch_max_runoff_bulk(engine_, nullptr, n_subs_),
              SWMM_ERR_BADPARAM);
}

TEST_F(StatsBulkPhase3Test, RejectsNonPositiveCount) {
    std::vector<double> v(n_nodes_, 0.0);
    EXPECT_EQ(swmm_stat_node_max_overflow_bulk(engine_, v.data(), 0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_stat_node_max_overflow_bulk(engine_, v.data(), -3),
              SWMM_ERR_BADPARAM);
}

TEST_F(StatsBulkPhase3Test, RejectsNullEngine) {
    std::vector<double> v(std::max(n_nodes_, n_subs_), 0.0);
    EXPECT_EQ(swmm_stat_node_max_overflow_bulk(nullptr, v.data(), n_nodes_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_stat_node_vol_flooded_bulk(nullptr, v.data(), n_nodes_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_stat_node_time_flooded_bulk(nullptr, v.data(), n_nodes_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_stat_subcatch_max_runoff_bulk(nullptr, v.data(), n_subs_),
              SWMM_ERR_BADHANDLE);
}

// ---------------------------------------------------------------------------
// Count clipping.
// ---------------------------------------------------------------------------

TEST_F(StatsBulkPhase3Test, OversizedCountClippedAtN) {
    constexpr double kSentinel = -42.0;
    const int oversize = n_nodes_ + 5;
    std::vector<double> v(oversize, kSentinel);
    ASSERT_EQ(swmm_stat_node_max_overflow_bulk(engine_, v.data(), oversize),
              SWMM_OK);
    for (int i = n_nodes_; i < oversize; ++i) {
        EXPECT_EQ(v[i], kSentinel) << "tail index " << i;
    }
}

} // namespace
