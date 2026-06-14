/**
 * @file test_hydrograph_mutation_api.cpp
 * @brief BS-02 — Unit tests for the hydrograph + RDII-decay mutation surface.
 *
 * @details Covers the BS-02 C API added to support the GUI's
 *          `HydrographGroupEditor` MVC layer:
 *            - swmm_hydrograph_set_rtk / _set_ia (upsert + partial-field merge)
 *            - swmm_hydrograph_remove_entry (key-based, idempotent)
 *            - swmm_hydrograph_remove_group (cascades to gage / decay / [RDII])
 *            - swmm_hydrograph_clear_group_months (preserves ALL row)
 *            - swmm_hydrograph_set_gage (set / replace / clear)
 *            - swmm_hydrograph_group_rename (walks all four data containers)
 *            - swmm_rdii_decay_set / _remove
 *
 * @see include/openswmm/engine/openswmm_inflows.h
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_inflows.h>

// ---------------------------------------------------------------------------
// Fixture: bare engine + two junctions so [RDII] node assignments are valid.
// ---------------------------------------------------------------------------

class HydrographMutationTest : public ::testing::Test {
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

    // Convenience: return entry index matching (uh, month, response), or -1.
    int findEntry(const char* uh, int month, int response) const {
        const int n = swmm_hydrograph_count(engine);
        for (int i = 0; i < n; ++i) {
            char buf[64]; int m = -2, r = -2;
            double r_, t_, k_, dmax_, drec_, dinit_;
            if (swmm_hydrograph_get(engine, i, buf, sizeof(buf), &m, &r,
                                    &r_, &t_, &k_, &dmax_, &drec_, &dinit_) != SWMM_OK)
                continue;
            if (std::strcmp(buf, uh) == 0 && m == month && r == response) return i;
        }
        return -1;
    }

    int findDecay(const char* uh, int response) const {
        const int n = swmm_rdii_decay_count(engine);
        for (int i = 0; i < n; ++i) {
            char buf[64]; int r = -2;
            double k_dep, k_0, k_T, T_ref, theta, T_freeze;
            if (swmm_rdii_decay_get(engine, i, buf, sizeof(buf), &r,
                                    &k_dep, &k_0, &k_T, &T_ref, &theta, &T_freeze) != SWMM_OK)
                continue;
            if (std::strcmp(buf, uh) == 0 && r == response) return i;
        }
        return -1;
    }
};

// ---------------------------------------------------------------------------
// Case 1 — set_rtk on a fresh key appends a new entry with IA = 0.
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, SetRtkAppendsNewEntryWithZeroIa) {
    EXPECT_EQ(swmm_hydrograph_count(engine), 0);

    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", -1, 0, 0.3, 1.5, 2.0), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 1);

    char buf[64]; int m = 0, r = 0;
    double r_, t_, k_, dmax_, drec_, dinit_;
    ASSERT_EQ(swmm_hydrograph_get(engine, 0, buf, sizeof(buf), &m, &r,
                                  &r_, &t_, &k_, &dmax_, &drec_, &dinit_), SWMM_OK);
    EXPECT_STREQ(buf, "G1");
    EXPECT_EQ(m, -1);
    EXPECT_EQ(r, 0);
    EXPECT_DOUBLE_EQ(r_, 0.3);
    EXPECT_DOUBLE_EQ(t_, 1.5);
    EXPECT_DOUBLE_EQ(k_, 2.0);
    EXPECT_DOUBLE_EQ(dmax_, 0.0);
    EXPECT_DOUBLE_EQ(drec_, 0.0);
    EXPECT_DOUBLE_EQ(dinit_, 0.0);
}

// ---------------------------------------------------------------------------
// Case 2 — set_rtk on an existing key updates in-place; row count unchanged.
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, SetRtkUpdatesInPlaceWhenKeyMatches) {
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", -1, 1, 0.2, 1.0, 2.5), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_count(engine), 1);

    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", -1, 1, 0.45, 2.5, 3.0), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 1);

    const int i = findEntry("G1", -1, 1);
    ASSERT_GE(i, 0);
    char buf[64]; int m, r;
    double r_, t_, k_, dmax_, drec_, dinit_;
    ASSERT_EQ(swmm_hydrograph_get(engine, i, buf, sizeof(buf), &m, &r,
                                  &r_, &t_, &k_, &dmax_, &drec_, &dinit_), SWMM_OK);
    EXPECT_DOUBLE_EQ(r_, 0.45);
    EXPECT_DOUBLE_EQ(t_, 2.5);
    EXPECT_DOUBLE_EQ(k_, 3.0);
}

// ---------------------------------------------------------------------------
// Case 3 — set_rtk then set_ia on same key preserves both halves (composes).
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, SetRtkThenSetIaPreservesBothFieldGroups) {
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", -1, 2, 0.1, 4.0, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_ia(engine,  "G1", -1, 2, 0.5, 0.1, 0.0), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 1);

    const int i = findEntry("G1", -1, 2);
    ASSERT_GE(i, 0);
    char buf[64]; int m, r;
    double r_, t_, k_, dmax_, drec_, dinit_;
    ASSERT_EQ(swmm_hydrograph_get(engine, i, buf, sizeof(buf), &m, &r,
                                  &r_, &t_, &k_, &dmax_, &drec_, &dinit_), SWMM_OK);
    EXPECT_DOUBLE_EQ(r_, 0.1);
    EXPECT_DOUBLE_EQ(t_, 4.0);
    EXPECT_DOUBLE_EQ(k_, 2.0);
    EXPECT_DOUBLE_EQ(dmax_, 0.5);
    EXPECT_DOUBLE_EQ(drec_, 0.1);
    EXPECT_DOUBLE_EQ(dinit_, 0.0);

    // Now overwrite just the IA half — RTK fields must survive.
    ASSERT_EQ(swmm_hydrograph_set_ia(engine, "G1", -1, 2, 1.0, 0.25, 0.1), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_get(engine, i, buf, sizeof(buf), &m, &r,
                                  &r_, &t_, &k_, &dmax_, &drec_, &dinit_), SWMM_OK);
    EXPECT_DOUBLE_EQ(r_, 0.1);
    EXPECT_DOUBLE_EQ(dmax_, 1.0);
    EXPECT_DOUBLE_EQ(drec_, 0.25);
    EXPECT_DOUBLE_EQ(dinit_, 0.1);
}

// ---------------------------------------------------------------------------
// Case 4 — remove_entry is key-based and idempotent.
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, RemoveEntryIsKeyBasedAndIdempotent) {
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", 0, 0, 0.1, 1.0, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", 1, 0, 0.2, 1.5, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", 0, 1, 0.3, 2.0, 2.0), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 3);

    EXPECT_EQ(swmm_hydrograph_remove_entry(engine, "G1", 1, 0), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 2);
    EXPECT_EQ(findEntry("G1", 1, 0), -1);
    EXPECT_GE(findEntry("G1", 0, 0), 0);
    EXPECT_GE(findEntry("G1", 0, 1), 0);

    // Idempotent — removing again returns SWMM_OK without changing the count.
    EXPECT_EQ(swmm_hydrograph_remove_entry(engine, "G1", 1, 0), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 2);
}

// ---------------------------------------------------------------------------
// Case 5 — remove_group cascades to gage assignment, decay, and [RDII].
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, RemoveGroupCascadesToAllReferences) {
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", -1, 0, 0.3, 1.5, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G2", -1, 0, 0.4, 2.0, 2.5), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_gage(engine, "G1", "RG1"), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_gage(engine, "G2", "RG2"), SWMM_OK);
    ASSERT_EQ(swmm_rdii_decay_set(engine, "G1", 0, 0.1, 0.05, 0.0, 10.0, 0.0, 0.0), SWMM_OK);
    ASSERT_EQ(swmm_rdii_decay_set(engine, "G2", 0, 0.2, 0.05, 0.0, 10.0, 0.0, 0.0), SWMM_OK);
    ASSERT_EQ(swmm_rdii_add(engine, j1_idx, "G1", 10.0), SWMM_OK);
    ASSERT_EQ(swmm_rdii_add(engine, j2_idx, "G1", 20.0), SWMM_OK);
    ASSERT_EQ(swmm_rdii_add(engine, j1_idx, "G2", 30.0), SWMM_OK);

    EXPECT_EQ(swmm_hydrograph_count(engine),       2);
    EXPECT_EQ(swmm_hydrograph_gage_count(engine),  2);
    EXPECT_EQ(swmm_rdii_decay_count(engine),       2);
    EXPECT_EQ(swmm_rdii_count(engine),             3);

    EXPECT_EQ(swmm_hydrograph_remove_group(engine, "G1"), SWMM_OK);

    EXPECT_EQ(swmm_hydrograph_count(engine),       1);
    EXPECT_EQ(swmm_hydrograph_gage_count(engine),  1);
    EXPECT_EQ(swmm_rdii_decay_count(engine),       1);
    EXPECT_EQ(swmm_rdii_count(engine),             1);

    // What's left should be G2 references only.
    EXPECT_GE(findEntry("G2", -1, 0), 0);
    EXPECT_GE(findDecay("G2", 0), 0);

    char uh[64], gage[64];
    ASSERT_EQ(swmm_hydrograph_get_gage(engine, 0, uh, sizeof(uh),
                                       gage, sizeof(gage)), SWMM_OK);
    EXPECT_STREQ(uh,   "G2");
    EXPECT_STREQ(gage, "RG2");
}

// ---------------------------------------------------------------------------
// Case 6 — clear_group_months preserves any month=-1 (ALL) row.
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, ClearGroupMonthsPreservesAllRow) {
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", -1, 0, 0.30, 1.0, 2.0), SWMM_OK);
    for (int m = 0; m < 12; ++m) {
        ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", m, 0, 0.10, 1.0, 2.0), SWMM_OK);
    }
    EXPECT_EQ(swmm_hydrograph_count(engine), 13);

    EXPECT_EQ(swmm_hydrograph_clear_group_months(engine, "G1"), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 1);
    EXPECT_GE(findEntry("G1", -1, 0), 0);
    for (int m = 0; m < 12; ++m) {
        EXPECT_EQ(findEntry("G1", m, 0), -1);
    }
}

// ---------------------------------------------------------------------------
// Case 7 — set_gage adds, replaces, and clears the gage assignment.
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, SetGageAddReplaceClear) {
    EXPECT_EQ(swmm_hydrograph_gage_count(engine), 0);

    ASSERT_EQ(swmm_hydrograph_set_gage(engine, "G1", "RG1"), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_gage_count(engine), 1);
    char uh[64], gage[64];
    ASSERT_EQ(swmm_hydrograph_get_gage(engine, 0, uh, sizeof(uh),
                                       gage, sizeof(gage)), SWMM_OK);
    EXPECT_STREQ(gage, "RG1");

    // Replace.
    ASSERT_EQ(swmm_hydrograph_set_gage(engine, "G1", "RG2"), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_gage_count(engine), 1);
    ASSERT_EQ(swmm_hydrograph_get_gage(engine, 0, uh, sizeof(uh),
                                       gage, sizeof(gage)), SWMM_OK);
    EXPECT_STREQ(gage, "RG2");

    // Clear via NULL.
    ASSERT_EQ(swmm_hydrograph_set_gage(engine, "G1", nullptr), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_gage_count(engine), 0);

    // Clear via empty string when there is no row — idempotent.
    EXPECT_EQ(swmm_hydrograph_set_gage(engine, "G1", ""), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_gage_count(engine), 0);
}

// ---------------------------------------------------------------------------
// Case 8 — group_rename walks entries, gage assignments, decay rows, and
//          [RDII] node assignments.
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, GroupRenameWalksAllFourContainers) {
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", -1, 0, 0.3, 1.0, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1",  3, 1, 0.2, 1.5, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_gage(engine, "G1", "RG1"), SWMM_OK);
    ASSERT_EQ(swmm_rdii_decay_set(engine, "G1", 0, 0.1, 0.05, 0.0, 10.0, 0.0, 0.0), SWMM_OK);
    ASSERT_EQ(swmm_rdii_add(engine, j1_idx, "G1", 12.5), SWMM_OK);

    // Locate G1 in the group index list.
    const int gn = swmm_hydrograph_group_count(engine);
    int g1_idx = -1;
    for (int i = 0; i < gn; ++i) {
        char buf[64];
        ASSERT_EQ(swmm_hydrograph_group_id(engine, i, buf, sizeof(buf)), SWMM_OK);
        if (std::strcmp(buf, "G1") == 0) { g1_idx = i; break; }
    }
    ASSERT_GE(g1_idx, 0);

    ASSERT_EQ(swmm_hydrograph_group_rename(engine, g1_idx, "RENAMED"), SWMM_OK);

    EXPECT_GE(findEntry("RENAMED", -1, 0), 0);
    EXPECT_GE(findEntry("RENAMED",  3, 1), 0);
    EXPECT_GE(findDecay("RENAMED", 0), 0);

    char uh[64], gage[64];
    ASSERT_EQ(swmm_hydrograph_get_gage(engine, 0, uh, sizeof(uh),
                                       gage, sizeof(gage)), SWMM_OK);
    EXPECT_STREQ(uh, "RENAMED");

    char uh2[64];
    int node_idx = -1; double area = 0.0;
    ASSERT_EQ(swmm_rdii_get(engine, 0, &node_idx, uh2, sizeof(uh2), &area), SWMM_OK);
    EXPECT_STREQ(uh2, "RENAMED");
}

// ---------------------------------------------------------------------------
// Case 9 — group_rename rejects empty / duplicate names.
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, GroupRenameRejectsBadNames) {
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", -1, 0, 0.3, 1.0, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G2", -1, 0, 0.3, 1.0, 2.0), SWMM_OK);

    EXPECT_EQ(swmm_hydrograph_group_rename(engine, 0, ""),       SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hydrograph_group_rename(engine, 0, nullptr),  SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hydrograph_group_rename(engine, 0, "G2"),     SWMM_ERR_BADPARAM);

    // Renaming to the same name is a no-op (returns SWMM_OK without changing
    // anything observable).
    EXPECT_EQ(swmm_hydrograph_group_rename(engine, 0, "G1"),     SWMM_OK);
}

// ---------------------------------------------------------------------------
// Case 10 — decay_set upsert + decay_remove (idempotent).
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, DecaySetUpsertAndRemoveIdempotent) {
    EXPECT_EQ(swmm_rdii_decay_count(engine), 0);

    ASSERT_EQ(swmm_rdii_decay_set(engine, "G1", 0,
                                  0.1, 0.05, 0.02, 10.0, 0.05, 0.0), SWMM_OK);
    EXPECT_EQ(swmm_rdii_decay_count(engine), 1);

    // Upsert in-place.
    ASSERT_EQ(swmm_rdii_decay_set(engine, "G1", 0,
                                  0.2, 0.1, 0.04, 12.0, 0.06, -1.0), SWMM_OK);
    EXPECT_EQ(swmm_rdii_decay_count(engine), 1);

    char buf[64]; int r = -1;
    double k_dep, k_0, k_T, T_ref, theta, T_freeze;
    ASSERT_EQ(swmm_rdii_decay_get(engine, 0, buf, sizeof(buf), &r,
                                  &k_dep, &k_0, &k_T, &T_ref, &theta, &T_freeze), SWMM_OK);
    EXPECT_STREQ(buf, "G1");
    EXPECT_EQ(r, 0);
    EXPECT_DOUBLE_EQ(k_dep, 0.2);
    EXPECT_DOUBLE_EQ(T_ref, 12.0);
    EXPECT_DOUBLE_EQ(T_freeze, -1.0);

    // Remove + idempotent re-remove.
    EXPECT_EQ(swmm_rdii_decay_remove(engine, "G1", 0), SWMM_OK);
    EXPECT_EQ(swmm_rdii_decay_count(engine), 0);
    EXPECT_EQ(swmm_rdii_decay_remove(engine, "G1", 0), SWMM_OK);
    EXPECT_EQ(swmm_rdii_decay_count(engine), 0);
}

// ---------------------------------------------------------------------------
// Case 11 — bad parameters are rejected with SWMM_ERR_BADPARAM.
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, BadParametersRejected) {
    EXPECT_EQ(swmm_hydrograph_set_rtk(engine, nullptr, -1, 0, 0.1, 1.0, 2.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hydrograph_set_rtk(engine, "",      -1, 0, 0.1, 1.0, 2.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hydrograph_set_rtk(engine, "G1",    -2, 0, 0.1, 1.0, 2.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hydrograph_set_rtk(engine, "G1",    12, 0, 0.1, 1.0, 2.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hydrograph_set_rtk(engine, "G1",     0, -1, 0.1, 1.0, 2.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hydrograph_set_rtk(engine, "G1",     0,  3, 0.1, 1.0, 2.0),
              SWMM_ERR_BADPARAM);

    EXPECT_EQ(swmm_rdii_decay_set(engine, "G1", -1,
                                  0.1, 0.0, 0.0, 10.0, 0.0, 0.0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_rdii_decay_set(engine, "G1",  3,
                                  0.1, 0.0, 0.0, 10.0, 0.0, 0.0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_rdii_decay_set(engine, "G1",  0,
                                  -0.1, 0.0, 0.0, 10.0, 0.0, 0.0), SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// Case 12 — remove_group leaves an empty engine and is idempotent.
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, RemoveGroupOnUnknownNameIsNoOp) {
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "G1", -1, 0, 0.3, 1.0, 2.0), SWMM_OK);

    EXPECT_EQ(swmm_hydrograph_remove_group(engine, "DOES_NOT_EXIST"), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 1);

    EXPECT_EQ(swmm_hydrograph_remove_group(engine, "G1"), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 0);
    EXPECT_EQ(swmm_hydrograph_group_count(engine), 0);
}

// ---------------------------------------------------------------------------
// Case 13 — multiple groups stay independent across removes.
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, MultipleGroupsStayIndependentAcrossRemoves) {
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "A", -1, 0, 0.1, 1.0, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "B", -1, 0, 0.2, 1.0, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_rtk(engine, "C", -1, 0, 0.3, 1.0, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_gage(engine, "A", "RG_A"), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_gage(engine, "B", "RG_B"), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_set_gage(engine, "C", "RG_C"), SWMM_OK);
    ASSERT_EQ(swmm_hydrograph_count(engine), 3);
    ASSERT_EQ(swmm_hydrograph_group_count(engine), 3);

    EXPECT_EQ(swmm_hydrograph_remove_group(engine, "B"), SWMM_OK);
    EXPECT_EQ(swmm_hydrograph_count(engine), 2);
    EXPECT_EQ(swmm_hydrograph_group_count(engine), 2);
    EXPECT_GE(findEntry("A", -1, 0), 0);
    EXPECT_GE(findEntry("C", -1, 0), 0);
    EXPECT_EQ(findEntry("B", -1, 0), -1);
}

// ---------------------------------------------------------------------------
// Case 14 — decay_set referencing an unknown UH still inserts (the engine
//           does not currently enforce referential integrity on decay rows —
//           document this so the GUI can rely on the behaviour).
// ---------------------------------------------------------------------------

TEST_F(HydrographMutationTest, DecaySetWithoutPriorGroupStillInserts) {
    EXPECT_EQ(swmm_hydrograph_count(engine), 0);
    EXPECT_EQ(swmm_rdii_decay_set(engine, "PHANTOM", 0,
                                  0.1, 0.05, 0.0, 10.0, 0.0, 0.0), SWMM_OK);
    EXPECT_EQ(swmm_rdii_decay_count(engine), 1);
    EXPECT_GE(findDecay("PHANTOM", 0), 0);

    // remove_group on a name with only a decay row still cleans it up.
    EXPECT_EQ(swmm_hydrograph_remove_group(engine, "PHANTOM"), SWMM_OK);
    EXPECT_EQ(swmm_rdii_decay_count(engine), 0);
}
