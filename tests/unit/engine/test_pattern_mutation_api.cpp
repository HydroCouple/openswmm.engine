/**
 * @file test_pattern_mutation_api.cpp
 * @brief BR-PAT — Unit tests for the pattern mutation surface used by the
 *        GUI's PatternEditorDialog CRUD path.
 *
 * @details Covers the BR-PAT C API additions to ::openswmm_tables.h:
 *            - swmm_pattern_remove (cascades to ref sites, idempotent on
 *              stale index, preserves order of remaining patterns)
 *            - swmm_pattern_rename (rewrites every stored reference, rejects
 *              collisions, no-ops on same-name)
 *
 * @see include/openswmm/engine/openswmm_tables.h
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_inflows.h>
#include <openswmm/engine/openswmm_tables.h>

// ---------------------------------------------------------------------------
// Fixture — bare engine + two junctions for DWF / inflow attachments.
// ---------------------------------------------------------------------------

class PatternMutationTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;
    int j1_idx = -1;
    int j2_idx = -1;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "J2", SWMM_NODE_JUNCTION), SWMM_OK);
        j1_idx = swmm_node_index(engine, "J1");
        j2_idx = swmm_node_index(engine, "J2");
        ASSERT_GE(j1_idx, 0);
        ASSERT_GE(j2_idx, 0);
    }

    void TearDown() override { if (engine) swmm_engine_destroy(engine); }

    // Convenience: add a pattern with N factors all equal to value.
    void addPattern(const char* id, int type, int n, double value) {
        ASSERT_EQ(swmm_pattern_add(engine, id, type), SWMM_OK);
        const int idx = swmm_pattern_index(engine, id);
        ASSERT_GE(idx, 0);
        std::vector<double> facs(n, value);
        ASSERT_EQ(swmm_pattern_set_factors(engine, idx, facs.data(),
                                            static_cast<int>(facs.size())),
                  SWMM_OK);
    }

    // Read a DWF pat-N field (N in {1,2,3,4}) for the entry at idx.
    std::string getDwfPat(int entry, int which) {
        char p1[64] = {}, p2[64] = {}, p3[64] = {}, p4[64] = {}, c[64] = {};
        int n = -1; double avg = 0;
        EXPECT_EQ(swmm_dwf_get(engine, entry, &n,
                               c, sizeof(c), &avg,
                               p1, sizeof(p1), p2, sizeof(p2),
                               p3, sizeof(p3), p4, sizeof(p4)), SWMM_OK);
        switch (which) {
            case 1: return p1;
            case 2: return p2;
            case 3: return p3;
            case 4: return p4;
            default: return {};
        }
    }

    std::string getInflowPattern(int entry) {
        int n = -1; char c[64] = {}, ts[64] = {}, tp[64] = {}, pat[64] = {};
        double mf = 0, sf = 0, base = 0;
        EXPECT_EQ(swmm_ext_inflow_get(engine, entry, &n,
                                       c, sizeof(c), ts, sizeof(ts),
                                       tp, sizeof(tp),
                                       &mf, &sf, &base,
                                       pat, sizeof(pat)), SWMM_OK);
        return pat;
    }
};

// ---------------------------------------------------------------------------
// Case 1 — remove on a stale index is a SWMM_OK no-op (the GUI re-resolves
// indices lazily; a double-click on Delete must not crash or error).
// ---------------------------------------------------------------------------

TEST_F(PatternMutationTest, RemoveOutOfRangeIsNoop) {
    addPattern("P1", 0, 12, 1.0);
    EXPECT_EQ(swmm_pattern_count(engine), 1);

    EXPECT_EQ(swmm_pattern_remove(engine, -1), SWMM_OK);
    EXPECT_EQ(swmm_pattern_remove(engine, 99), SWMM_OK);
    EXPECT_EQ(swmm_pattern_count(engine), 1);
}

// ---------------------------------------------------------------------------
// Case 2 — remove shifts subsequent patterns down by one and preserves the
// remaining names / types / factors.
// ---------------------------------------------------------------------------

TEST_F(PatternMutationTest, RemovePreservesOrderOfRemaining) {
    addPattern("A", 0, 12, 1.0);
    addPattern("B", 1, 7,  2.0);
    addPattern("C", 2, 24, 3.0);
    ASSERT_EQ(swmm_pattern_count(engine), 3);

    ASSERT_EQ(swmm_pattern_remove(engine, 1 /* "B" */), SWMM_OK);
    EXPECT_EQ(swmm_pattern_count(engine), 2);

    EXPECT_STREQ(swmm_pattern_id(engine, 0), "A");
    EXPECT_STREQ(swmm_pattern_id(engine, 1), "C");

    int t = -1;
    EXPECT_EQ(swmm_pattern_get_type(engine, 0, &t), SWMM_OK); EXPECT_EQ(t, 0);
    EXPECT_EQ(swmm_pattern_get_type(engine, 1, &t), SWMM_OK); EXPECT_EQ(t, 2);

    int fc = 0;
    EXPECT_EQ(swmm_pattern_get_factor_count(engine, 0, &fc), SWMM_OK); EXPECT_EQ(fc, 12);
    EXPECT_EQ(swmm_pattern_get_factor_count(engine, 1, &fc), SWMM_OK); EXPECT_EQ(fc, 24);
}

// ---------------------------------------------------------------------------
// Case 3 — remove cascades to ext-inflow references that match the removed
// pattern by name; references to other patterns are untouched.
// ---------------------------------------------------------------------------

TEST_F(PatternMutationTest, RemoveCascadesToExtInflows) {
    addPattern("PA", 0, 12, 1.0);
    addPattern("PB", 0, 12, 2.0);

    ASSERT_EQ(swmm_ext_inflow_add(engine, j1_idx, "FLOW", "",
                                   "FLOW", 1.0, 1.0, 0.5, "PA"), SWMM_OK);
    ASSERT_EQ(swmm_ext_inflow_add(engine, j2_idx, "FLOW", "",
                                   "FLOW", 1.0, 1.0, 0.5, "PB"), SWMM_OK);

    ASSERT_EQ(swmm_pattern_remove(engine, swmm_pattern_index(engine, "PA")), SWMM_OK);

    EXPECT_EQ(getInflowPattern(0), "");    // cleared
    EXPECT_EQ(getInflowPattern(1), "PB");  // untouched
}

// ---------------------------------------------------------------------------
// Case 4 — remove cascades to DWF pattern slots (pat1..pat4) independently.
// ---------------------------------------------------------------------------

TEST_F(PatternMutationTest, RemoveCascadesToDwfSlots) {
    addPattern("M", 0, 12, 1.0); // monthly
    addPattern("D", 1, 7,  1.0); // daily
    addPattern("H", 2, 24, 1.0); // hourly
    addPattern("W", 3, 24, 1.0); // weekend

    ASSERT_EQ(swmm_dwf_add(engine, j1_idx, "FLOW", 100.0,
                            "M", "D", "H", "W"), SWMM_OK);

    // Remove the daily pattern — only pat2 should clear.
    ASSERT_EQ(swmm_pattern_remove(engine, swmm_pattern_index(engine, "D")), SWMM_OK);

    EXPECT_EQ(getDwfPat(0, 1), "M");
    EXPECT_EQ(getDwfPat(0, 2), "");
    EXPECT_EQ(getDwfPat(0, 3), "H");
    EXPECT_EQ(getDwfPat(0, 4), "W");
}

// ---------------------------------------------------------------------------
// Case 5 — rename empty / nullptr is rejected with SWMM_ERR_BADPARAM.
// ---------------------------------------------------------------------------

TEST_F(PatternMutationTest, RenameRejectsEmptyOrNull) {
    addPattern("X", 0, 12, 1.0);

    EXPECT_EQ(swmm_pattern_rename(engine, 0, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_pattern_rename(engine, 0, ""),       SWMM_ERR_BADPARAM);
    EXPECT_STREQ(swmm_pattern_id(engine, 0), "X");
}

// ---------------------------------------------------------------------------
// Case 6 — rename to an in-use name is rejected with SWMM_ERR_BADPARAM and
// both patterns keep their existing identifiers.
// ---------------------------------------------------------------------------

TEST_F(PatternMutationTest, RenameCollisionRejected) {
    addPattern("X", 0, 12, 1.0);
    addPattern("Y", 0, 12, 2.0);

    EXPECT_EQ(swmm_pattern_rename(engine, 0, "Y"), SWMM_ERR_BADPARAM);
    EXPECT_STREQ(swmm_pattern_id(engine, 0), "X");
    EXPECT_STREQ(swmm_pattern_id(engine, 1), "Y");
}

// ---------------------------------------------------------------------------
// Case 7 — rename to the same name is a SWMM_OK no-op.
// ---------------------------------------------------------------------------

TEST_F(PatternMutationTest, RenameSameNameIsNoop) {
    addPattern("X", 0, 12, 1.0);
    EXPECT_EQ(swmm_pattern_rename(engine, 0, "X"), SWMM_OK);
    EXPECT_STREQ(swmm_pattern_id(engine, 0), "X");
}

// ---------------------------------------------------------------------------
// Case 8 — rename rewrites every stored reference: ext-inflow + every DWF slot
// holding the previous name. References to other patterns are untouched.
// ---------------------------------------------------------------------------

TEST_F(PatternMutationTest, RenameRipplesToAllReferenceSites) {
    addPattern("OLD", 0, 12, 1.0);
    addPattern("OTHER", 0, 12, 1.0);

    ASSERT_EQ(swmm_ext_inflow_add(engine, j1_idx, "FLOW", "",
                                   "FLOW", 1.0, 1.0, 0.0, "OLD"), SWMM_OK);
    ASSERT_EQ(swmm_dwf_add(engine, j1_idx, "FLOW", 100.0,
                            "OLD", "OTHER", "OLD", ""), SWMM_OK);

    ASSERT_EQ(swmm_pattern_rename(engine, swmm_pattern_index(engine, "OLD"),
                                  "NEW"), SWMM_OK);

    EXPECT_STREQ(swmm_pattern_id(engine, 0), "NEW");
    EXPECT_EQ(swmm_pattern_index(engine, "OLD"), -1);
    EXPECT_EQ(swmm_pattern_index(engine, "NEW"), 0);

    EXPECT_EQ(getInflowPattern(0), "NEW");
    EXPECT_EQ(getDwfPat(0, 1), "NEW");
    EXPECT_EQ(getDwfPat(0, 2), "OTHER");
    EXPECT_EQ(getDwfPat(0, 3), "NEW");
    EXPECT_EQ(getDwfPat(0, 4), "");
}
