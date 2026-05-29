/**
 * @file test_type_conversion.cpp
 * @brief Unit tests for in-place node and link type conversion.
 *
 * @details Covers: lifecycle guards, node type conversions (preserving common
 *          fields, clearing old type-specific fields, applying new defaults),
 *          link type conversions, topology warnings, and same-type no-op guard.
 *
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_edit.h>

// ============================================================================
// Fixture
// ============================================================================

class ConversionTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
    }

    void TearDown() override {
        swmm_engine_destroy(engine);
    }
};

// ============================================================================
// Node: Junction → Outfall
// ============================================================================

TEST_F(ConversionTest, JunctionToOutfall_PreservesInvert) {
    ASSERT_EQ(swmm_node_add(engine, "J", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_set_invert_elev(engine, 0, 5.0), SWMM_OK);
    ASSERT_EQ(swmm_node_set_max_depth(engine, 0, 3.0), SWMM_OK);

    SWMM_ConversionResult res{};
    ASSERT_EQ(swmm_node_convert(engine, 0, SWMM_NODE_OUTFALL, &res), SWMM_OK);
    EXPECT_EQ(res.new_type, SWMM_NODE_OUTFALL);
    swmm_conversion_result_free(&res);

    int t = -1;
    ASSERT_EQ(swmm_node_get_type(engine, 0, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_NODE_OUTFALL);

    double elev = 0.0, depth = 0.0;
    ASSERT_EQ(swmm_node_get_invert_elev(engine, 0, &elev), SWMM_OK);
    ASSERT_EQ(swmm_node_get_max_depth(engine, 0, &depth), SWMM_OK);
    EXPECT_DOUBLE_EQ(elev, 5.0);
    EXPECT_DOUBLE_EQ(depth, 3.0);
}

// ============================================================================
// Node: Storage → Junction clears storage fields
// ============================================================================

TEST_F(ConversionTest, StorageToJunction_ClearsStorageFields) {
    ASSERT_EQ(swmm_node_add(engine, "S", SWMM_NODE_STORAGE), SWMM_OK);
    ASSERT_EQ(swmm_node_set_storage_functional(engine, 0, 5.0, 0.5, 0.0), SWMM_OK);
    ASSERT_EQ(swmm_node_set_invert_elev(engine, 0, 10.0), SWMM_OK);

    SWMM_ConversionResult res{};
    ASSERT_EQ(swmm_node_convert(engine, 0, SWMM_NODE_JUNCTION, &res), SWMM_OK);
    EXPECT_EQ(res.new_type, SWMM_NODE_JUNCTION);
    EXPECT_GT(res.n_cleared, 0);
    swmm_conversion_result_free(&res);

    int t = -1;
    ASSERT_EQ(swmm_node_get_type(engine, 0, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_NODE_JUNCTION);

    // invert_elev must be preserved
    double elev = 0.0;
    ASSERT_EQ(swmm_node_get_invert_elev(engine, 0, &elev), SWMM_OK);
    EXPECT_DOUBLE_EQ(elev, 10.0);
}

// ============================================================================
// Node: Outfall → Storage sets storage defaults
// ============================================================================

TEST_F(ConversionTest, OutfallToStorage_SetsDefaults) {
    ASSERT_EQ(swmm_node_add(engine, "O", SWMM_NODE_OUTFALL), SWMM_OK);

    SWMM_ConversionResult res{};
    ASSERT_EQ(swmm_node_convert(engine, 0, SWMM_NODE_STORAGE, &res), SWMM_OK);
    // Should warn: model has no outfall
    EXPECT_GT(res.n_warnings, 0);
    swmm_conversion_result_free(&res);

    int t = -1;
    ASSERT_EQ(swmm_node_get_type(engine, 0, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_NODE_STORAGE);
}

// ============================================================================
// Node: Only outfall → other type warns
// ============================================================================

TEST_F(ConversionTest, OnlyOutfall_WarnsOnConvert) {
    ASSERT_EQ(swmm_node_add(engine, "O", SWMM_NODE_OUTFALL), SWMM_OK);

    SWMM_ConversionResult res{};
    ASSERT_EQ(swmm_node_convert(engine, 0, SWMM_NODE_JUNCTION, &res), SWMM_OK);
    EXPECT_GT(res.n_warnings, 0);  // must warn about no outfall
    swmm_conversion_result_free(&res);
}

// ============================================================================
// Node: Same type → SWMM_ERR_BADPARAM
// ============================================================================

TEST_F(ConversionTest, SameNodeTypeIsError) {
    ASSERT_EQ(swmm_node_add(engine, "J", SWMM_NODE_JUNCTION), SWMM_OK);
    EXPECT_EQ(swmm_node_convert(engine, 0, SWMM_NODE_JUNCTION, nullptr), SWMM_ERR_BADPARAM);
}

// ============================================================================
// Node: Invalid type → SWMM_ERR_BADPARAM
// ============================================================================

TEST_F(ConversionTest, InvalidNodeTypeIsError) {
    ASSERT_EQ(swmm_node_add(engine, "J", SWMM_NODE_JUNCTION), SWMM_OK);
    EXPECT_EQ(swmm_node_convert(engine, 0, 99, nullptr), SWMM_ERR_BADPARAM);
}

// ============================================================================
// Node: Lifecycle guard
// ============================================================================

TEST_F(ConversionTest, NodeConvertLifecycleGuard) {
    ASSERT_EQ(swmm_node_add(engine, "J", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "O", SWMM_NODE_OUTFALL), SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "C", SWMM_LINK_CONDUIT), SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine, 0, 0, 1), SWMM_OK);
    ASSERT_EQ(swmm_finalize_model(engine), SWMM_OK);

    EXPECT_EQ(swmm_node_convert(engine, 0, SWMM_NODE_STORAGE, nullptr), SWMM_ERR_LIFECYCLE);
}

// ============================================================================
// Link: Conduit → Pump clears conduit fields, sets pump defaults
// ============================================================================

TEST_F(ConversionTest, ConduitToPump_ClearsAndSetsDefaults) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "B", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "C", SWMM_LINK_CONDUIT), SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine, 0, 0, 1), SWMM_OK);
    ASSERT_EQ(swmm_link_set_length(engine, 0, 100.0), SWMM_OK);

    SWMM_ConversionResult res{};
    ASSERT_EQ(swmm_link_convert(engine, 0, SWMM_LINK_PUMP, &res), SWMM_OK);
    EXPECT_EQ(res.new_type, SWMM_LINK_PUMP);
    EXPECT_GT(res.n_cleared, 0);
    swmm_conversion_result_free(&res);

    int t = -1;
    ASSERT_EQ(swmm_link_get_type(engine, 0, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_LINK_PUMP);
}

// ============================================================================
// Link: Pump → Conduit sets conduit defaults
// ============================================================================

TEST_F(ConversionTest, PumpToConduit_SetsConduitDefaults) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "B", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "P", SWMM_LINK_PUMP), SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine, 0, 0, 1), SWMM_OK);

    SWMM_ConversionResult res{};
    ASSERT_EQ(swmm_link_convert(engine, 0, SWMM_LINK_CONDUIT, &res), SWMM_OK);
    EXPECT_EQ(res.new_type, SWMM_LINK_CONDUIT);
    swmm_conversion_result_free(&res);

    int t = -1;
    ASSERT_EQ(swmm_link_get_type(engine, 0, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_LINK_CONDUIT);
}

// ============================================================================
// Link: Conduit → Weir preserves node connectivity
// ============================================================================

TEST_F(ConversionTest, ConduitToWeir_PreservesNodes) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "B", SWMM_NODE_OUTFALL),  SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "C", SWMM_LINK_CONDUIT),  SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine, 0, 0, 1), SWMM_OK);

    SWMM_ConversionResult res{};
    ASSERT_EQ(swmm_link_convert(engine, 0, SWMM_LINK_WEIR, &res), SWMM_OK);
    swmm_conversion_result_free(&res);

    int from = -1, to = -1;
    ASSERT_EQ(swmm_link_get_from_node(engine, 0, &from), SWMM_OK);
    ASSERT_EQ(swmm_link_get_to_node(engine,   0, &to),   SWMM_OK);
    EXPECT_EQ(from, 0);
    EXPECT_EQ(to,   1);

    int t = -1;
    ASSERT_EQ(swmm_link_get_type(engine, 0, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_LINK_WEIR);
}

// ============================================================================
// Link: Same type → SWMM_ERR_BADPARAM
// ============================================================================

TEST_F(ConversionTest, SameLinkTypeIsError) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "B", SWMM_NODE_OUTFALL),  SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "C", SWMM_LINK_CONDUIT),  SWMM_OK);
    EXPECT_EQ(swmm_link_convert(engine, 0, SWMM_LINK_CONDUIT, nullptr), SWMM_ERR_BADPARAM);
}

// ============================================================================
// Link: Lifecycle guard
// ============================================================================

TEST_F(ConversionTest, LinkConvertLifecycleGuard) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "O", SWMM_NODE_OUTFALL),  SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "C", SWMM_LINK_CONDUIT),  SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine, 0, 0, 1), SWMM_OK);
    ASSERT_EQ(swmm_finalize_model(engine), SWMM_OK);

    EXPECT_EQ(swmm_link_convert(engine, 0, SWMM_LINK_PUMP, nullptr), SWMM_ERR_LIFECYCLE);
}

// ============================================================================
// Result structs: null result_out is valid (no crash)
// ============================================================================

TEST_F(ConversionTest, NullResultOutIsValid) {
    ASSERT_EQ(swmm_node_add(engine, "J", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "O", SWMM_NODE_OUTFALL),  SWMM_OK);
    EXPECT_EQ(swmm_node_convert(engine, 0, SWMM_NODE_STORAGE, nullptr), SWMM_OK);
}
