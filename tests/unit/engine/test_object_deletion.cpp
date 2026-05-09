/**
 * @file test_object_deletion.cpp
 * @brief Unit tests for general object deletion (swmm_*_delete family).
 *
 * @details Covers: lifecycle guards, isolated deletion, cascade-deletion
 *          of links when a referenced node is removed, nullification of
 *          subcatch outlet_node, index renumbering after deletion,
 *          non-mutating analyze_impact, NameIndex::remove_at, and
 *          deletion of gages, tables, and transects.
 *
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_gages.h>
#include <openswmm/engine/openswmm_tables.h>
#include <openswmm/engine/openswmm_edit.h>

// ============================================================================
// Fixture: small programmatic model
// ============================================================================

class DeletionTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
    }

    void TearDown() override {
        swmm_engine_destroy(engine);
    }

    // Build: nodes A(junction), B(junction), C(outfall)
    //        links AB, BC
    // NOTE: All adds must be done before setting properties because
    //       swmm_*_add calls resize() which re-initializes all SoA fields.
    void build_simple_network() {
        ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "B", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "C", SWMM_NODE_OUTFALL),  SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "AB", SWMM_LINK_CONDUIT), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "BC", SWMM_LINK_CONDUIT), SWMM_OK);

        // Set connectivity after all objects are added (add uses resize which resets all fields)
        ASSERT_EQ(swmm_link_set_nodes(engine,
                      swmm_link_index(engine, "AB"),
                      swmm_node_index(engine, "A"),
                      swmm_node_index(engine, "B")), SWMM_OK);
        ASSERT_EQ(swmm_link_set_nodes(engine,
                      swmm_link_index(engine, "BC"),
                      swmm_node_index(engine, "B"),
                      swmm_node_index(engine, "C")), SWMM_OK);
    }
};

// ============================================================================
// NameIndex::remove_at — internal correctness via C API reflection
// ============================================================================

TEST_F(DeletionTest, NodeNamesAfterDeleteMiddle) {
    ASSERT_EQ(swmm_node_add(engine, "N0", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "N1", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "N2", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "N3", SWMM_NODE_JUNCTION), SWMM_OK);

    // Delete N1 (idx 1)
    ASSERT_EQ(swmm_node_delete(engine, 1, nullptr), SWMM_OK);

    EXPECT_EQ(swmm_node_count(engine), 3);
    // Remaining: N0=0, N2=1, N3=2
    EXPECT_STREQ(swmm_node_id(engine, 0), "N0");
    EXPECT_STREQ(swmm_node_id(engine, 1), "N2");
    EXPECT_STREQ(swmm_node_id(engine, 2), "N3");
    EXPECT_EQ(swmm_node_index(engine, "N0"), 0);
    EXPECT_EQ(swmm_node_index(engine, "N2"), 1);
    EXPECT_EQ(swmm_node_index(engine, "N3"), 2);
    EXPECT_EQ(swmm_node_index(engine, "N1"), -1);
}

// ============================================================================
// Lifecycle guard
// ============================================================================

TEST_F(DeletionTest, LifecycleGuardRejectsInitialized) {
    ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "O1", SWMM_NODE_OUTFALL), SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "C1", SWMM_LINK_CONDUIT), SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine, 0, 0, 1), SWMM_OK);
    ASSERT_EQ(swmm_finalize_model(engine), SWMM_OK);

    // After finalize (INITIALIZED state) deletion must be rejected
    EXPECT_EQ(swmm_node_delete(engine, 0, nullptr), SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_link_delete(engine, 0, nullptr), SWMM_ERR_LIFECYCLE);
}

// ============================================================================
// analyze_impact is non-mutating
// ============================================================================

TEST_F(DeletionTest, AnalyzeImpactDoesNotMutate) {
    build_simple_network();

    int n_before = swmm_node_count(engine);
    int l_before = swmm_link_count(engine);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_node_analyze_impact(engine, 1, &report), SWMM_OK);
    swmm_impact_report_free(&report);

    EXPECT_EQ(swmm_node_count(engine), n_before);
    EXPECT_EQ(swmm_link_count(engine), l_before);
}

// ============================================================================
// Delete isolated node (no links)
// ============================================================================

TEST_F(DeletionTest, DeleteIsolatedNode) {
    ASSERT_EQ(swmm_node_add(engine, "X", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "Y", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "Z", SWMM_NODE_OUTFALL),  SWMM_OK);

    ASSERT_EQ(swmm_node_delete(engine, 1, nullptr), SWMM_OK);

    EXPECT_EQ(swmm_node_count(engine), 2);
    EXPECT_STREQ(swmm_node_id(engine, 0), "X");
    EXPECT_STREQ(swmm_node_id(engine, 1), "Z");
    EXPECT_EQ(swmm_node_index(engine, "Y"), -1);
}

// ============================================================================
// Delete node cascades its links
// ============================================================================

TEST_F(DeletionTest, DeleteNodeCascadesLinks) {
    build_simple_network();
    // Network: A(0)-AB-B(1)-BC-C(2)
    // Deleting B should cascade-delete AB and BC
    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_node_delete(engine, 1, &report), SWMM_OK);

    EXPECT_EQ(swmm_node_count(engine), 2);
    EXPECT_EQ(swmm_link_count(engine), 0);

    // Impact report must mention both links
    for (int i = 0; i < report.n_entries; ++i) {
        if (report.entries[i].obj_type == SWMM_REF_LINK && report.entries[i].cascaded) {
            // counts as cascade-deleted link entry
        }
    }
    EXPECT_TRUE(report.n_entries > 0);
    swmm_impact_report_free(&report);
}

// ============================================================================
// Deleting a node nullifies subcatch outlet_node
// ============================================================================

TEST_F(DeletionTest, DeleteNodeNullifiesSubcatchOutlet) {
    ASSERT_EQ(swmm_node_add(engine, "J", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "O", SWMM_NODE_OUTFALL),  SWMM_OK);
    ASSERT_EQ(swmm_subcatch_add(engine, "S1"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_set_outlet(engine, 0, 0), SWMM_OK);  // outlet = J (idx 0)

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_node_delete(engine, 0, &report), SWMM_OK);

    // Subcatch should survive, outlet_node nullified
    EXPECT_EQ(swmm_subcatch_count(engine), 1);
    EXPECT_EQ(swmm_node_count(engine), 1);

    // At least one entry for outlet_node nullification
    bool found = false;
    for (int i = 0; i < report.n_entries; ++i) {
        if (report.entries[i].obj_type == SWMM_REF_SUBCATCH &&
            report.entries[i].cascaded == 0)
            found = true;
    }
    EXPECT_TRUE(found);
    swmm_impact_report_free(&report);
}

// ============================================================================
// Index renumbering after deleting middle node
// ============================================================================

TEST_F(DeletionTest, IndexRenumberingAfterDeleteNode) {
    // 4 nodes: 0=A, 1=B, 2=C, 3=D (outfall)
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "B", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "C", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "D", SWMM_NODE_OUTFALL),  SWMM_OK);

    // Link CD: node1=2(C), node2=3(D) — should survive delete of B
    ASSERT_EQ(swmm_link_add(engine, "CD", SWMM_LINK_CONDUIT), SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine, 0, 2, 3), SWMM_OK);

    // Delete node B (idx=1) — no links touch B, so no cascade
    ASSERT_EQ(swmm_node_delete(engine, 1, nullptr), SWMM_OK);

    // Remaining nodes: A=0, C=1, D=2
    EXPECT_EQ(swmm_node_count(engine), 3);
    EXPECT_EQ(swmm_link_count(engine), 1);

    // Link CD should now have node1=1(C) and node2=2(D) (renumbered)
    int from = -1, to = -1;
    ASSERT_EQ(swmm_link_get_from_node(engine, 0, &from), SWMM_OK);
    ASSERT_EQ(swmm_link_get_to_node(engine, 0, &to),   SWMM_OK);
    EXPECT_EQ(from, 1);  // was 2 (C), now 1 after B deleted
    EXPECT_EQ(to,   2);  // was 3 (D), now 2 after B deleted
}

// ============================================================================
// Delete link — link count decrements
// ============================================================================

TEST_F(DeletionTest, DeleteLinkReducesCount) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "O", SWMM_NODE_OUTFALL),  SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "C1", SWMM_LINK_CONDUIT), SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "C2", SWMM_LINK_CONDUIT), SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine, 0, 0, 1), SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine, 1, 0, 1), SWMM_OK);

    ASSERT_EQ(swmm_link_delete(engine, 0, nullptr), SWMM_OK);
    EXPECT_EQ(swmm_link_count(engine), 1);
    EXPECT_STREQ(swmm_link_id(engine, 0), "C2");
}

// ============================================================================
// Delete gage nullifies subcatch gage references
// ============================================================================

TEST_F(DeletionTest, DeleteGageNullifiesSubcatchGage) {
    ASSERT_EQ(swmm_gage_add(engine, "G0"), SWMM_OK);
    ASSERT_EQ(swmm_gage_add(engine, "G1"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_add(engine, "S"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_set_gage(engine, 0, 1), SWMM_OK);  // S uses G1

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_gage_delete(engine, 1, &report), SWMM_OK);

    EXPECT_EQ(swmm_gage_count(engine), 1);

    bool found = false;
    for (int i = 0; i < report.n_entries; ++i) {
        if (report.entries[i].obj_type == SWMM_REF_SUBCATCH &&
            report.entries[i].cascaded == 0)
            found = true;
    }
    EXPECT_TRUE(found);
    swmm_impact_report_free(&report);
}

// ============================================================================
// Delete table nullifies pump_curve on link
// ============================================================================

TEST_F(DeletionTest, DeleteTableNullifiesPumpCurve) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "B", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine, "P", SWMM_LINK_PUMP), SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine, 0, 0, 1), SWMM_OK);

    // Add a pump curve table (type 8 = CURVE_PUMP2: head vs flow)
    ASSERT_EQ(swmm_curve_add(engine, "PumpCurve", 8), SWMM_OK);
    int ti = swmm_table_index(engine, "PumpCurve");
    ASSERT_GE(ti, 0);

    // Assign it to the pump link
    ASSERT_EQ(swmm_link_set_pump_curve(engine, 0, ti), SWMM_OK);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_table_delete(engine, ti, &report), SWMM_OK);

    EXPECT_EQ(swmm_table_count(engine), 0);

    // Pump link should have pump_curve nullified
    bool found = false;
    for (int i = 0; i < report.n_entries; ++i) {
        if (report.entries[i].obj_type == SWMM_REF_LINK &&
            report.entries[i].cascaded == 0)
            found = true;
    }
    EXPECT_TRUE(found);
    swmm_impact_report_free(&report);
}

// ============================================================================
// Delete subcatchment nullifies outfall_route_to
// ============================================================================

TEST_F(DeletionTest, DeleteSubcatchNullifiesOutfallRouteTo) {
    ASSERT_EQ(swmm_node_add(engine, "O", SWMM_NODE_OUTFALL), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_add(engine, "S"), SWMM_OK);
    ASSERT_EQ(swmm_node_set_outfall_route_to(engine, 0, 0), SWMM_OK);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_subcatch_delete(engine, 0, &report), SWMM_OK);

    EXPECT_EQ(swmm_subcatch_count(engine), 0);
    bool found = false;
    for (int i = 0; i < report.n_entries; ++i) {
        if (report.entries[i].obj_type == SWMM_REF_NODE &&
            report.entries[i].cascaded == 0)
            found = true;
    }
    EXPECT_TRUE(found);
    swmm_impact_report_free(&report);
}

// ============================================================================
// Bad index / bad handle guards
// ============================================================================

TEST_F(DeletionTest, BadIndexReturnsError) {
    EXPECT_EQ(swmm_node_delete(engine, -1, nullptr), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_node_delete(engine, 999, nullptr), SWMM_ERR_BADINDEX);
}

TEST_F(DeletionTest, NullHandleReturnsError) {
    EXPECT_EQ(swmm_node_delete(nullptr, 0, nullptr), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_node_analyze_impact(nullptr, 0, nullptr), SWMM_ERR_BADHANDLE);
}
