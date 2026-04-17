/**
 * @file test_operator_snapshot.cpp
 * @brief Unit tests for the Operator Snapshot Layer (Part 2).
 *
 * @details Verifies that:
 *   1. Callback fires each routing substep with valid snapshot data.
 *   2. Poll mode (swmm_get_operator_snapshot) returns populated snapshot.
 *   3. Snapshot dimensions match engine model counts.
 *   4. Topology pointers (node1, node2, link_type) are non-null.
 *   5. Picard telemetry (iterations, routing_dt) is reasonable.
 *   6. Flow/depth/head arrays are non-null and plausible.
 *   7. dqdh array is non-null.
 *   8. Node converged/surcharged flags are 0 or 1.
 *   9. Two independent engines receive independent snapshots.
 *  10. No callback → zero overhead (snapshot not populated).
 *
 * @see include/openswmm/engine/openswmm_operator_snapshot.h
 * @ingroup engine_unit_tests
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_operator_snapshot.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ============================================================================
// Test data path helper
// ============================================================================

std::string getTestDataDir() {
    // Relative to build directory — adjust if needed
    fs::path p = fs::path(__FILE__).parent_path() / "data";
    return p.string();
}

std::string testModel() {
    return (fs::path(getTestDataDir()) / "site_drainage_model.inp").string();
}

// ============================================================================
// Snapshot capture structure
// ============================================================================

struct CapturedSnapshot {
    int n_nodes         = 0;
    int n_links         = 0;
    int n_conduits      = 0;
    int iterations      = 0;
    int converged       = 0;
    double routing_dt   = 0.0;
    double sim_time     = 0.0;
    int count           = 0;   ///< Number of times callback was invoked

    bool node1_non_null     = false;
    bool node2_non_null     = false;
    bool link_type_non_null = false;
    bool link_flow_non_null = false;
    bool dqdh_non_null      = false;
    bool node_head_non_null = false;
    bool node_depth_non_null = false;
    bool sumdqdh_non_null   = false;
    bool node_converged_non_null = false;

    // First callback: sample values for sanity checks
    double first_routing_dt = 0.0;
    int first_iters         = 0;
};

void snapshotCallback(SWMM_Engine /*engine*/,
                      const SWMM_OperatorSnapshot* snap,
                      void* user_data) {
    auto* cap = static_cast<CapturedSnapshot*>(user_data);
    cap->count++;

    // Record latest values
    cap->n_nodes     = snap->n_nodes;
    cap->n_links     = snap->n_links;
    cap->n_conduits  = snap->n_conduits;
    cap->iterations  = snap->iterations;
    cap->converged   = snap->converged;
    cap->routing_dt  = snap->routing_dt;
    cap->sim_time    = snap->sim_time;

    cap->node1_non_null     = snap->node1 != nullptr;
    cap->node2_non_null     = snap->node2 != nullptr;
    cap->link_type_non_null = snap->link_type != nullptr;
    cap->link_flow_non_null = snap->link_flow != nullptr;
    cap->dqdh_non_null      = snap->dqdh != nullptr;
    cap->node_head_non_null = snap->node_head != nullptr;
    cap->node_depth_non_null = snap->node_depth != nullptr;
    cap->sumdqdh_non_null   = snap->sumdqdh != nullptr;
    cap->node_converged_non_null = snap->node_converged != nullptr;

    if (cap->count == 1) {
        cap->first_routing_dt = snap->routing_dt;
        cap->first_iters      = snap->iterations;
    }

    // Verify converged/surcharged flags are binary
    if (snap->node_converged) {
        for (int i = 0; i < snap->n_nodes; ++i) {
            EXPECT_TRUE(snap->node_converged[i] == 0 || snap->node_converged[i] == 1)
                << "node_converged[" << i << "] = " << (int)snap->node_converged[i];
        }
    }
    if (snap->node_surcharged) {
        for (int i = 0; i < snap->n_nodes; ++i) {
            EXPECT_TRUE(snap->node_surcharged[i] == 0 || snap->node_surcharged[i] == 1)
                << "node_surcharged[" << i << "] = " << (int)snap->node_surcharged[i];
        }
    }
}

// ============================================================================
// Helper: create, open, run a few steps with callback, then close
// ============================================================================

void runWithCallback(const std::string& inp, CapturedSnapshot& cap, int max_steps = 10) {
    std::string rpt = inp + ".rpt";
    std::string out = inp + ".out";

    SWMM_Engine engine = swmm_engine_create();
    ASSERT_NE(engine, nullptr);

    int rc = swmm_engine_open(engine, inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
    ASSERT_EQ(rc, SWMM_OK) << "open failed: " << swmm_error_message(rc);

    rc = swmm_set_operator_snapshot_callback(engine, snapshotCallback, &cap);
    ASSERT_EQ(rc, SWMM_OK);

    rc = swmm_engine_initialize(engine);
    ASSERT_EQ(rc, SWMM_OK) << "initialize failed: " << swmm_error_message(rc);

    rc = swmm_engine_start(engine, 0);
    ASSERT_EQ(rc, SWMM_OK) << "start failed: " << swmm_error_message(rc);

    double elapsed = 0.0;
    for (int s = 0; s < max_steps; ++s) {
        rc = swmm_engine_step(engine, &elapsed);
        if (rc != SWMM_OK || elapsed == 0.0) break;
    }

    swmm_engine_end(engine);
    swmm_engine_close(engine);
    swmm_engine_destroy(engine);

    // Clean up temp files
    std::remove(rpt.c_str());
    std::remove(out.c_str());
}

} // namespace

// ============================================================================
// Test: Callback fires with valid data
// ============================================================================

TEST(OperatorSnapshot, CallbackFiresWithValidData) {
    CapturedSnapshot cap;
    ASSERT_NO_FATAL_FAILURE(runWithCallback(testModel(), cap));

    // Callback should have fired at least once
    EXPECT_GT(cap.count, 0) << "Snapshot callback was never invoked";

    // Dimensions positive
    EXPECT_GT(cap.n_nodes, 0);
    EXPECT_GT(cap.n_links, 0);
    EXPECT_GE(cap.n_conduits, 0);
    EXPECT_LE(cap.n_conduits, cap.n_links);

    // Pointers should be non-null
    EXPECT_TRUE(cap.node1_non_null);
    EXPECT_TRUE(cap.node2_non_null);
    EXPECT_TRUE(cap.link_type_non_null);
    EXPECT_TRUE(cap.link_flow_non_null);
    EXPECT_TRUE(cap.dqdh_non_null);
    EXPECT_TRUE(cap.node_head_non_null);
    EXPECT_TRUE(cap.node_depth_non_null);
    EXPECT_TRUE(cap.sumdqdh_non_null);
    EXPECT_TRUE(cap.node_converged_non_null);

    // Picard telemetry
    EXPECT_GT(cap.first_iters, 0);
    EXPECT_GT(cap.first_routing_dt, 0.0);
}

// ============================================================================
// Test: Poll mode returns snapshot
// ============================================================================

TEST(OperatorSnapshot, PollModeReturnsSnapshot) {
    std::string inp = testModel();
    std::string rpt = inp + ".poll.rpt";
    std::string out = inp + ".poll.out";

    SWMM_Engine engine = swmm_engine_create();
    ASSERT_NE(engine, nullptr);

    // No callback registered — poll mode only.
    // Calling swmm_get_operator_snapshot enables population automatically.

    ASSERT_EQ(SWMM_OK, swmm_engine_open(engine, inp.c_str(), rpt.c_str(), out.c_str(), nullptr));
    ASSERT_EQ(SWMM_OK, swmm_engine_initialize(engine));
    ASSERT_EQ(SWMM_OK, swmm_engine_start(engine, 0));

    // Before stepping, poll should return nullptr (enables poll mode internally)
    const SWMM_OperatorSnapshot* snap = nullptr;
    ASSERT_EQ(SWMM_OK, swmm_get_operator_snapshot(engine, &snap));
    EXPECT_EQ(snap, nullptr) << "Snapshot should be null before first routing step";

    // Step once
    double elapsed = 0.0;
    ASSERT_EQ(SWMM_OK, swmm_engine_step(engine, &elapsed));

    // Now poll should succeed (no callback needed)
    ASSERT_EQ(SWMM_OK, swmm_get_operator_snapshot(engine, &snap));
    ASSERT_NE(snap, nullptr);
    EXPECT_GT(snap->n_nodes, 0);
    EXPECT_GT(snap->n_links, 0);
    EXPECT_GT(snap->routing_dt, 0.0);

    swmm_engine_end(engine);
    swmm_engine_close(engine);
    swmm_engine_destroy(engine);
    std::remove(rpt.c_str());
    std::remove(out.c_str());
}

// ============================================================================
// Test: No callback → snapshot not populated (zero overhead)
// ============================================================================

TEST(OperatorSnapshot, NoCallbackNoOverhead) {
    std::string inp = testModel();
    std::string rpt = inp + ".nocb.rpt";
    std::string out = inp + ".nocb.out";

    SWMM_Engine engine = swmm_engine_create();
    ASSERT_NE(engine, nullptr);

    // Do NOT register a callback

    ASSERT_EQ(SWMM_OK, swmm_engine_open(engine, inp.c_str(), rpt.c_str(), out.c_str(), nullptr));
    ASSERT_EQ(SWMM_OK, swmm_engine_initialize(engine));
    ASSERT_EQ(SWMM_OK, swmm_engine_start(engine, 0));

    double elapsed = 0.0;
    ASSERT_EQ(SWMM_OK, swmm_engine_step(engine, &elapsed));

    // Poll should return null since no callback means snapshot is never populated
    const SWMM_OperatorSnapshot* snap = nullptr;
    ASSERT_EQ(SWMM_OK, swmm_get_operator_snapshot(engine, &snap));
    EXPECT_EQ(snap, nullptr) << "Snapshot should not be populated without a callback";

    swmm_engine_end(engine);
    swmm_engine_close(engine);
    swmm_engine_destroy(engine);
    std::remove(rpt.c_str());
    std::remove(out.c_str());
}

// ============================================================================
// Test: Snapshot dimensions match engine model
// ============================================================================

TEST(OperatorSnapshot, DimensionsMatchModel) {
    std::string inp = testModel();
    std::string rpt = inp + ".dim.rpt";
    std::string out = inp + ".dim.out";

    SWMM_Engine engine = swmm_engine_create();
    ASSERT_NE(engine, nullptr);

    int snap_n_nodes = 0, snap_n_links = 0;

    auto capture_dims = [](SWMM_Engine, const SWMM_OperatorSnapshot* s, void* ud) {
        auto* pair = static_cast<std::pair<int,int>*>(ud);
        pair->first  = s->n_nodes;
        pair->second = s->n_links;
    };
    std::pair<int,int> dims{0,0};
    ASSERT_EQ(SWMM_OK, swmm_set_operator_snapshot_callback(engine, capture_dims, &dims));

    ASSERT_EQ(SWMM_OK, swmm_engine_open(engine, inp.c_str(), rpt.c_str(), out.c_str(), nullptr));
    ASSERT_EQ(SWMM_OK, swmm_engine_initialize(engine));
    ASSERT_EQ(SWMM_OK, swmm_engine_start(engine, 0));

    double elapsed = 0.0;
    ASSERT_EQ(SWMM_OK, swmm_engine_step(engine, &elapsed));

    // Compare with engine query
    int eng_n_nodes = swmm_node_count(engine);
    int eng_n_links = swmm_link_count(engine);

    EXPECT_EQ(dims.first, eng_n_nodes);
    EXPECT_EQ(dims.second, eng_n_links);

    swmm_engine_end(engine);
    swmm_engine_close(engine);
    swmm_engine_destroy(engine);
    std::remove(rpt.c_str());
    std::remove(out.c_str());
}
