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
#include <openswmm/engine/openswmm_inflows.h>
#include <openswmm/engine/openswmm_pollutants.h>
#include <openswmm/engine/openswmm_quality.h>
#include <openswmm/engine/openswmm_infrastructure.h>

#include <cstring>
#include <map>
#include <string>
#include <tuple>

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

// ============================================================================
// Phase 0.1 — referential-integrity extensions
// ============================================================================

namespace {

// Count report entries matching (obj_type, field, cascaded).
int count_entries(const SWMM_ImpactReport& r, int obj_type,
                  const char* field, int cascaded) {
    int n = 0;
    for (int i = 0; i < r.n_entries; ++i) {
        if (r.entries[i].obj_type == obj_type &&
            r.entries[i].cascaded == cascaded &&
            std::strcmp(r.entries[i].field, field) == 0)
            ++n;
    }
    return n;
}

// Multiset of (obj_type, field, cascaded) — for analyze-vs-delete parity.
std::map<std::tuple<int, std::string, int>, int>
entry_multiset(const SWMM_ImpactReport& r) {
    std::map<std::tuple<int, std::string, int>, int> m;
    for (int i = 0; i < r.n_entries; ++i)
        ++m[{r.entries[i].obj_type, r.entries[i].field, r.entries[i].cascaded}];
    return m;
}

} // namespace

// Deleting a node cascades its ext-inflow / DWF / RDII rows and renumbers
// the surviving rows' node_idx.
TEST_F(DeletionTest, DeleteNodeCascadesInflowRows) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "B", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "C", SWMM_NODE_OUTFALL),  SWMM_OK);

    ASSERT_EQ(swmm_ext_inflow_add(engine, 1, "FLOW", "", "FLOW", 1.0, 1.0, 0.5, ""), SWMM_OK);
    ASSERT_EQ(swmm_ext_inflow_add(engine, 2, "FLOW", "", "FLOW", 1.0, 1.0, 0.7, ""), SWMM_OK);
    ASSERT_EQ(swmm_dwf_add(engine, 1, "FLOW", 0.25, "", "", "", ""), SWMM_OK);
    ASSERT_EQ(swmm_rdii_add(engine, 1, "UH1", 12.0), SWMM_OK);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_node_delete(engine, 1, &report), SWMM_OK);  // delete B

    EXPECT_EQ(count_entries(report, SWMM_REF_EXT_INFLOW,  "node_idx", 1), 1);
    EXPECT_EQ(count_entries(report, SWMM_REF_DWF_INFLOW,  "node_idx", 1), 1);
    EXPECT_EQ(count_entries(report, SWMM_REF_RDII_ASSIGN, "node_idx", 1), 1);
    swmm_impact_report_free(&report);

    EXPECT_EQ(swmm_dwf_count(engine), 0);
    EXPECT_EQ(swmm_rdii_count(engine), 0);
    ASSERT_EQ(swmm_ext_inflow_count(engine), 1);

    // Surviving row targeted C (was idx 2) — must be renumbered to 1.
    int node_idx = -99;
    double baseline = 0.0;
    char cons[32], ts[64], type[16], pat[64];
    double mf, sf;
    ASSERT_EQ(swmm_ext_inflow_get(engine, 0, &node_idx,
                  cons, sizeof cons, ts, sizeof ts, type, sizeof type,
                  &mf, &sf, &baseline, pat, sizeof pat), SWMM_OK);
    EXPECT_EQ(node_idx, 1);
    EXPECT_DOUBLE_EQ(baseline, 0.7);
}

// Deleting a middle node must re-pack the positional treatment matrix so
// surviving nodes keep their own expressions (regression: silent misalignment).
TEST_F(DeletionTest, DeleteNodeRepacksTreatmentStripes) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "B", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "C", SWMM_NODE_OUTFALL),  SWMM_OK);
    ASSERT_EQ(swmm_pollutant_add(engine, "TSS",  0), SWMM_OK);
    ASSERT_EQ(swmm_pollutant_add(engine, "Lead", 1), SWMM_OK);

    ASSERT_EQ(swmm_treatment_set(engine, 0, 0, "R = 0.1"), SWMM_OK);
    ASSERT_EQ(swmm_treatment_set(engine, 1, 1, "R = 0.2"), SWMM_OK);
    ASSERT_EQ(swmm_treatment_set(engine, 2, 0, "R = 0.3"), SWMM_OK);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_node_delete(engine, 1, &report), SWMM_OK);  // delete B

    // B's stripe (one non-empty expression) reported as cascaded.
    EXPECT_EQ(count_entries(report, SWMM_REF_TREATMENT, "expression", 1), 1);
    swmm_impact_report_free(&report);

    // A keeps its expression; C's expression moves with it to idx 1.
    char buf[64];
    ASSERT_EQ(swmm_treatment_get(engine, 0, 0, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "R = 0.1");
    ASSERT_EQ(swmm_treatment_get(engine, 0, 1, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "");
    ASSERT_EQ(swmm_treatment_get(engine, 1, 0, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "R = 0.3");
    ASSERT_EQ(swmm_treatment_get(engine, 1, 1, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "");
}

// Deleting a node nullifies subcatchment gw_node references.
TEST_F(DeletionTest, DeleteNodeNullifiesSubcatchGwNode) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine, "B", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_add(engine, "S"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_set_gw_node(engine, 0, 1), SWMM_OK);  // gw → B

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_node_delete(engine, 1, &report), SWMM_OK);
    EXPECT_EQ(count_entries(report, SWMM_REF_SUBCATCH, "gw_node", 0), 1);
    swmm_impact_report_free(&report);

    int gw_node = -99;
    ASSERT_EQ(swmm_subcatch_get_gw_node(engine, 0, &gw_node), SWMM_OK);
    EXPECT_EQ(gw_node, -1);
}

// Deleting a subcatchment cascades its LID usage rows and renumbers survivors.
TEST_F(DeletionTest, DeleteSubcatchCascadesLidUsage) {
    ASSERT_EQ(swmm_subcatch_add(engine, "S1"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_add(engine, "S2"), SWMM_OK);
    ASSERT_EQ(swmm_lid_add(engine, "BC1", 0), SWMM_OK);
    ASSERT_EQ(swmm_lid_usage_add(engine, 0, 0, 1, 100.0, 5.0, 0.0, 25.0), SWMM_OK);
    ASSERT_EQ(swmm_lid_usage_add(engine, 1, 0, 2, 200.0, 8.0, 0.0, 50.0), SWMM_OK);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_subcatch_delete(engine, 0, &report), SWMM_OK);  // delete S1
    EXPECT_EQ(count_entries(report, SWMM_REF_LID_USAGE, "subcatch_index", 1), 1);
    swmm_impact_report_free(&report);

    ASSERT_EQ(swmm_lid_usage_count(engine), 1);
    int sc = -99, lid = -99, number = 0;
    ASSERT_EQ(swmm_lid_usage_get(engine, 0, &sc, &lid, &number,
                                 nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr), SWMM_OK);
    EXPECT_EQ(sc, 0);      // was 1 (S2), renumbered
    EXPECT_EQ(number, 2);  // S2's row survived intact
}

// Deleting a subcatchment clears snowpack removal_subcatch name references.
TEST_F(DeletionTest, DeleteSubcatchClearsSnowpackRemoval) {
    ASSERT_EQ(swmm_subcatch_add(engine, "S"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_add(engine, "SP"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_set_removal_subcatch(engine, 0, "S"), SWMM_OK);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_subcatch_delete(engine, 0, &report), SWMM_OK);
    EXPECT_EQ(count_entries(report, SWMM_REF_SNOWPACK, "removal_subcatch", 0), 1);
    swmm_impact_report_free(&report);

    char buf[32] = {0};
    ASSERT_EQ(swmm_snowpack_get_removal_subcatch(engine, 0, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "");
}

// Deleting a rain gage clears unit-hydrograph gage assignments (name-based).
TEST_F(DeletionTest, DeleteGageClearsHydrographAssignments) {
    ASSERT_EQ(swmm_gage_add(engine, "G1"), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_add_gage(engine, "UH1", "G1"), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_add(engine, "UH1", -1, 0,
                                  0.05, 1.0, 2.0, 0.0, 0.0, 0.0), SWMM_OK);

    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_gage_delete(engine, 0, &report), SWMM_OK);
    // The group-level gage assignment must be cleared. (Per-parameter-line
    // gage_name is only populated by the INP parser, not swmm_hydrograph_add,
    // so no "uh_gage_name" entry is expected in an API-built model.)
    EXPECT_EQ(count_entries(report, SWMM_REF_HYDROGRAPH, "gage_name", 0), 1);
    swmm_impact_report_free(&report);

    char uh[32] = {0}, gage[32] = {0};
    ASSERT_EQ(swmm_hydrograph_get_gage(engine, 0, uh, sizeof uh,
                                       gage, sizeof gage), SWMM_OK);
    EXPECT_STREQ(uh, "UH1");
    EXPECT_STREQ(gage, "");
}

// Deleting a timeseries clears name-based ext-inflow references.
TEST_F(DeletionTest, DeleteTableClearsExtInflowTsName) {
    ASSERT_EQ(swmm_node_add(engine, "A", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_timeseries_add(engine, "TS1"), SWMM_OK);
    ASSERT_EQ(swmm_ext_inflow_add(engine, 0, "FLOW", "TS1", "FLOW", 1.0, 1.0, 0.0, ""), SWMM_OK);

    int ti = swmm_table_index(engine, "TS1");
    ASSERT_GE(ti, 0);
    SWMM_ImpactReport report{};
    ASSERT_EQ(swmm_table_delete(engine, ti, &report), SWMM_OK);
    EXPECT_EQ(count_entries(report, SWMM_REF_EXT_INFLOW, "ts_name", 0), 1);
    swmm_impact_report_free(&report);

    int node_idx = -99;
    double mf, sf, baseline;
    char cons[32], ts[64], type[16], pat[64];
    ASSERT_EQ(swmm_ext_inflow_get(engine, 0, &node_idx,
                  cons, sizeof cons, ts, sizeof ts, type, sizeof type,
                  &mf, &sf, &baseline, pat, sizeof pat), SWMM_OK);
    EXPECT_STREQ(ts, "");
}

// analyze_impact must predict the same impact set the delete then reports
// (compared as a multiset of (obj_type, field, cascaded) — indices shift
// during the delete itself).
TEST_F(DeletionTest, AnalyzeMatchesDeleteForNode) {
    build_simple_network();
    ASSERT_EQ(swmm_subcatch_add(engine, "S"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_set_outlet(engine, 0, 1), SWMM_OK);   // outlet = B
    ASSERT_EQ(swmm_subcatch_set_gw_node(engine, 0, 1), SWMM_OK);  // gw → B
    ASSERT_EQ(swmm_ext_inflow_add(engine, 1, "FLOW", "", "FLOW", 1.0, 1.0, 0.1, ""), SWMM_OK);
    ASSERT_EQ(swmm_dwf_add(engine, 1, "FLOW", 0.2, "", "", "", ""), SWMM_OK);
    ASSERT_EQ(swmm_rdii_add(engine, 1, "UH1", 3.0), SWMM_OK);
    ASSERT_EQ(swmm_pollutant_add(engine, "TSS", 0), SWMM_OK);
    ASSERT_EQ(swmm_treatment_set(engine, 1, 0, "R = 0.5"), SWMM_OK);

    SWMM_ImpactReport predicted{};
    ASSERT_EQ(swmm_node_analyze_impact(engine, 1, &predicted), SWMM_OK);

    SWMM_ImpactReport actual{};
    ASSERT_EQ(swmm_node_delete(engine, 1, &actual), SWMM_OK);

    auto p = entry_multiset(predicted);
    auto a = entry_multiset(actual);
    // The delete report additionally contains the per-link nested cascade
    // entries and the link "node1/node2" markers; every predicted entry kind
    // must appear in the actual report with at least the predicted count.
    for (const auto& [key, n] : p) {
        EXPECT_GE(a[key], n) << "missing impact kind: type="
                             << std::get<0>(key) << " field=" << std::get<1>(key)
                             << " cascaded=" << std::get<2>(key);
    }
    swmm_impact_report_free(&predicted);
    swmm_impact_report_free(&actual);
}

// Deleting one subcatchment must not disturb another's LID usage, drain_to,
// or snowpack references (analyze/delete parity for subcatchments).
TEST_F(DeletionTest, AnalyzeMatchesDeleteForSubcatch) {
    ASSERT_EQ(swmm_subcatch_add(engine, "S1"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_add(engine, "S2"), SWMM_OK);
    ASSERT_EQ(swmm_lid_add(engine, "BC1", 0), SWMM_OK);
    ASSERT_EQ(swmm_lid_usage_add(engine, 0, 0, 1, 100.0, 5.0, 0.0, 25.0), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_add(engine, "SP"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_set_removal_subcatch(engine, 0, "S1"), SWMM_OK);

    SWMM_ImpactReport predicted{};
    ASSERT_EQ(swmm_subcatch_analyze_impact(engine, 0, &predicted), SWMM_OK);
    SWMM_ImpactReport actual{};
    ASSERT_EQ(swmm_subcatch_delete(engine, 0, &actual), SWMM_OK);

    EXPECT_EQ(entry_multiset(predicted), entry_multiset(actual));
    swmm_impact_report_free(&predicted);
    swmm_impact_report_free(&actual);
}
