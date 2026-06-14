/**
 * @file test_nodes_bulk_phase3.cpp
 * @brief Unit tests for the Phase 3 node-bulk C API additions —
 *        @ref swmm_node_get_volumes_bulk, @ref swmm_node_get_outflows_bulk,
 *        @ref swmm_node_get_losses_bulk,
 *        @ref swmm_node_get_lateral_inflows_bulk, and
 *        @ref swmm_node_get_ids_bulk.
 *
 * @details Each test verifies the ABI contract — null handling, count
 *          clipping, equivalence with the scalar accessor — using the
 *          site_drainage_model.inp fixture. The numerical content of the
 *          SoA columns is exercised by the existing simulation-level tests
 *          (test_site_drainage_model, test_routing); these tests focus on
 *          the bulk-vs-scalar parity, stride-packed ID format, and
 *          boundary cases.
 *
 *          Working directory is set to tests/unit/engine/data/ by
 *          @c tests/unit/engine/CMakeLists.txt.
 *
 * @see docs/C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md Phase 3 — Nodes batch
 * @ingroup engine_tests
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_nodes.h>

namespace {

class NodesBulkPhase3Test : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    int n_nodes_ = 0;

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
        n_nodes_ = swmm_node_count(engine_);
        ASSERT_GT(n_nodes_, 0);
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
// node. This is the headline contract; if a bulk getter ever reads the
// wrong SoA column (the bug class that Phase 3 was created to surface,
// see plan Appendix A item 3) this test will flag it immediately.
// ---------------------------------------------------------------------------

TEST_F(NodesBulkPhase3Test, VolumesBulkMatchesScalarPerNode) {
    std::vector<double> bulk(n_nodes_, -1.0);
    ASSERT_EQ(swmm_node_get_volumes_bulk(engine_, bulk.data(), n_nodes_),
              SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_node_get_volume(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "node " << i;
    }
}

TEST_F(NodesBulkPhase3Test, OutflowsBulkMatchesScalarPerNode) {
    std::vector<double> bulk(n_nodes_, -1.0);
    ASSERT_EQ(swmm_node_get_outflows_bulk(engine_, bulk.data(), n_nodes_),
              SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_node_get_outflow(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "node " << i;
    }
}

TEST_F(NodesBulkPhase3Test, LossesBulkMatchesScalarPerNode) {
    std::vector<double> bulk(n_nodes_, -1.0);
    ASSERT_EQ(swmm_node_get_losses_bulk(engine_, bulk.data(), n_nodes_),
              SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_node_get_losses(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "node " << i;
    }
}

TEST_F(NodesBulkPhase3Test, LateralInflowsBulkMatchesScalarPerNode) {
    std::vector<double> bulk(n_nodes_, -1.0);
    ASSERT_EQ(swmm_node_get_lateral_inflows_bulk(engine_, bulk.data(),
                                                  n_nodes_),
              SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        double scalar = -1.0;
        ASSERT_EQ(swmm_node_get_lateral_inflow(engine_, i, &scalar), SWMM_OK);
        EXPECT_EQ(bulk[i], scalar) << "node " << i;
    }
}

// Backward-compat: lateral_inflows_bulk and the older inflows_bulk read
// the same SoA column (see plan Appendix A item 3).
TEST_F(NodesBulkPhase3Test, LateralInflowsBulkMatchesLegacyInflowsBulk) {
    std::vector<double> a(n_nodes_, -1.0), b(n_nodes_, -2.0);
    ASSERT_EQ(swmm_node_get_lateral_inflows_bulk(engine_, a.data(), n_nodes_),
              SWMM_OK);
    ASSERT_EQ(swmm_node_get_inflows_bulk(engine_, b.data(), n_nodes_),
              SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        EXPECT_EQ(a[i], b[i]) << "node " << i;
    }
}

// ---------------------------------------------------------------------------
// IDs bulk — stride-packed UTF-8 format.
// ---------------------------------------------------------------------------

TEST_F(NodesBulkPhase3Test, IdsBulkMatchesScalarPerNode) {
    constexpr int kStride = 64;
    std::vector<char> buf(static_cast<std::size_t>(n_nodes_) * kStride, '\xAA');
    ASSERT_EQ(swmm_node_get_ids_bulk(engine_, buf.data(), kStride, n_nodes_),
              SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        const char* bulk_id = buf.data() + i * kStride;
        const char* scalar_id = swmm_node_id(engine_, i);
        ASSERT_NE(scalar_id, nullptr) << "node " << i;
        EXPECT_STREQ(bulk_id, scalar_id) << "node " << i;
    }
}

TEST_F(NodesBulkPhase3Test, IdsBulkTruncatesAtStride) {
    // Find the longest existing ID and pick a stride that forces truncation.
    int longest = 0;
    for (int i = 0; i < n_nodes_; ++i) {
        int L = static_cast<int>(std::strlen(swmm_node_id(engine_, i)));
        if (L > longest) longest = L;
    }
    if (longest <= 1) {
        GTEST_SKIP() << "Fixture's longest node ID is too short to "
                        "meaningfully test truncation.";
    }
    const int stride = longest;  // exactly long enough → 1-char truncation
    std::vector<char> buf(static_cast<std::size_t>(n_nodes_) * stride, '\xAA');
    ASSERT_EQ(swmm_node_get_ids_bulk(engine_, buf.data(), stride, n_nodes_),
              SWMM_OK);
    for (int i = 0; i < n_nodes_; ++i) {
        const char* slot = buf.data() + i * stride;
        // Slot must be NUL-terminated and at most stride-1 bytes.
        const std::size_t len = std::strlen(slot);
        EXPECT_LE(len, static_cast<std::size_t>(stride - 1)) << "node " << i;
    }
}

// ---------------------------------------------------------------------------
// Bad-param contracts — every Phase 3 bulk getter rejects obvious misuse.
// ---------------------------------------------------------------------------

TEST_F(NodesBulkPhase3Test, RejectsNullBuffer) {
    EXPECT_EQ(swmm_node_get_volumes_bulk(engine_, nullptr, n_nodes_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_get_outflows_bulk(engine_, nullptr, n_nodes_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_get_losses_bulk(engine_, nullptr, n_nodes_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_get_lateral_inflows_bulk(engine_, nullptr, n_nodes_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_get_ids_bulk(engine_, nullptr, 64, n_nodes_),
              SWMM_ERR_BADPARAM);
}

TEST_F(NodesBulkPhase3Test, RejectsNonPositiveCount) {
    std::vector<double> v(n_nodes_, 0.0);
    EXPECT_EQ(swmm_node_get_volumes_bulk(engine_, v.data(), 0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_get_volumes_bulk(engine_, v.data(), -5),
              SWMM_ERR_BADPARAM);
}

TEST_F(NodesBulkPhase3Test, IdsBulkRejectsTinyStride) {
    std::vector<char> buf(n_nodes_, '\0');
    // stride < 2 is invalid (no room for at least one char + NUL).
    EXPECT_EQ(swmm_node_get_ids_bulk(engine_, buf.data(), 1, n_nodes_),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_get_ids_bulk(engine_, buf.data(), 0, n_nodes_),
              SWMM_ERR_BADPARAM);
}

TEST_F(NodesBulkPhase3Test, RejectsNullEngine) {
    std::vector<double> v(n_nodes_, 0.0);
    EXPECT_EQ(swmm_node_get_volumes_bulk(nullptr, v.data(), n_nodes_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_node_get_outflows_bulk(nullptr, v.data(), n_nodes_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_node_get_losses_bulk(nullptr, v.data(), n_nodes_),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_node_get_lateral_inflows_bulk(nullptr, v.data(), n_nodes_),
              SWMM_ERR_BADHANDLE);
    std::vector<char> buf(n_nodes_ * 64, '\0');
    EXPECT_EQ(swmm_node_get_ids_bulk(nullptr, buf.data(), 64, n_nodes_),
              SWMM_ERR_BADHANDLE);
}

// ---------------------------------------------------------------------------
// Count clipping — passing count > n_nodes only writes the first
// n_nodes entries (consistent with the existing bulk getters).
// ---------------------------------------------------------------------------

TEST_F(NodesBulkPhase3Test, OversizedCountClippedAtNNodes) {
    constexpr double kSentinel = -42.0;
    const int oversize = n_nodes_ + 5;
    std::vector<double> v(oversize, kSentinel);
    ASSERT_EQ(swmm_node_get_volumes_bulk(engine_, v.data(), oversize),
              SWMM_OK);
    // First n_nodes entries must have been overwritten; tail untouched.
    for (int i = n_nodes_; i < oversize; ++i) {
        EXPECT_EQ(v[i], kSentinel) << "tail index " << i;
    }
}

} // namespace
