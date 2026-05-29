/**
 * @file test_transect_mutation_api.cpp
 * @brief DA-ENG-09 + BQ-TR-02 — Unit tests for the transect mutation surface
 *        used by Slice BQ Phase 6.7.4 TransectEditor (GUI).
 *
 * @details Covers the new C API additions to ::openswmm_infrastructure.h:
 *            - swmm_transect_get_roughness            (DA-ENG-09)
 *            - swmm_transect_get/set_bank_stations    (DA-ENG-09)
 *            - swmm_transect_get/set_encroachment_stations  (BQ-TR-02)
 *            - swmm_transect_get/set_modifiers        (DA-ENG-09)
 *            - swmm_transect_get/set_comments         (DA-ENG-09)
 *            - swmm_transect_get_station_count        (DA-ENG-09)
 *            - swmm_transect_get_station              (DA-ENG-09)
 *            - swmm_transect_clear_stations           (DA-ENG-09)
 *            - swmm_transect_rename                   (DA-ENG-09)
 *            - swmm_transect_remove                   (DA-ENG-09)
 *
 * @see include/openswmm/engine/openswmm_infrastructure.h
 * @see docs/GUI_IMPLEMENTATION_PLAN.md (Slice BQ Phase 6.7.4 detail)
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_infrastructure.h>

// ---------------------------------------------------------------------------
// Fixture — bare engine with one transect "T1" already added.
// ---------------------------------------------------------------------------

class TransectMutationTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;
    int t1_idx = -1;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_transect_add(engine, "T1"), SWMM_OK);
        t1_idx = swmm_transect_index(engine, "T1");
        ASSERT_GE(t1_idx, 0);
    }

    void TearDown() override { if (engine) swmm_engine_destroy(engine); }
};

// ---------------------------------------------------------------------------
// Defaults — swmm_transect_add seeds every new field with documented defaults.
// ---------------------------------------------------------------------------

TEST_F(TransectMutationTest, AddSeedsDefaultsForAllNewFields) {
    // Roughness: 0.0 for all three.
    double nL = 99, nR = 99, nC = 99;
    EXPECT_EQ(swmm_transect_get_roughness(engine, t1_idx, &nL, &nR, &nC), SWMM_OK);
    EXPECT_DOUBLE_EQ(nL, 0.0);
    EXPECT_DOUBLE_EQ(nR, 0.0);
    EXPECT_DOUBLE_EQ(nC, 0.0);

    // Bank stations: 0.0 / 0.0.
    double bL = 99, bR = 99;
    EXPECT_EQ(swmm_transect_get_bank_stations(engine, t1_idx, &bL, &bR), SWMM_OK);
    EXPECT_DOUBLE_EQ(bL, 0.0);
    EXPECT_DOUBLE_EQ(bR, 0.0);

    // Encroachment stations: 0.0 / 0.0 (BQ-TR-02).
    double eL = 99, eR = 99;
    EXPECT_EQ(swmm_transect_get_encroachment_stations(engine, t1_idx, &eL, &eR), SWMM_OK);
    EXPECT_DOUBLE_EQ(eL, 0.0);
    EXPECT_DOUBLE_EQ(eR, 0.0);

    // Modifiers: x_factor=1.0, y_factor=1.0, length_factor=1.0.
    double xF = 0, yF = 0, lF = 0;
    EXPECT_EQ(swmm_transect_get_modifiers(engine, t1_idx, &xF, &yF, &lF), SWMM_OK);
    EXPECT_DOUBLE_EQ(xF, 1.0);
    EXPECT_DOUBLE_EQ(yF, 1.0);
    EXPECT_DOUBLE_EQ(lF, 1.0);

    // Comments: empty string.
    char buf[64] = "garbage";
    EXPECT_EQ(swmm_transect_get_comments(engine, t1_idx, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");

    // Station count: 0.
    EXPECT_EQ(swmm_transect_get_station_count(engine, t1_idx), 0);
}

// ---------------------------------------------------------------------------
// Roughness — set then read back.
// ---------------------------------------------------------------------------

TEST_F(TransectMutationTest, SetRoughnessRoundTripsThroughGetter) {
    ASSERT_EQ(swmm_transect_set_roughness(engine, t1_idx, 0.04, 0.05, 0.03), SWMM_OK);

    double nL = 0, nR = 0, nC = 0;
    EXPECT_EQ(swmm_transect_get_roughness(engine, t1_idx, &nL, &nR, &nC), SWMM_OK);
    EXPECT_DOUBLE_EQ(nL, 0.04);
    EXPECT_DOUBLE_EQ(nR, 0.05);
    EXPECT_DOUBLE_EQ(nC, 0.03);
}

TEST_F(TransectMutationTest, GetRoughnessAcceptsNullOutParams) {
    ASSERT_EQ(swmm_transect_set_roughness(engine, t1_idx, 0.04, 0.05, 0.03), SWMM_OK);

    // Each parameter independently NULLable.
    double v = 0;
    EXPECT_EQ(swmm_transect_get_roughness(engine, t1_idx, nullptr, nullptr, &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.03);
    EXPECT_EQ(swmm_transect_get_roughness(engine, t1_idx, &v, nullptr, nullptr), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 0.04);
    // All three NULL is also OK — caller is essentially just probing validity.
    EXPECT_EQ(swmm_transect_get_roughness(engine, t1_idx, nullptr, nullptr, nullptr), SWMM_OK);
}

// ---------------------------------------------------------------------------
// Bank stations — set then read back, independent of encroachment.
// ---------------------------------------------------------------------------

TEST_F(TransectMutationTest, SetBankStationsRoundTrips) {
    EXPECT_EQ(swmm_transect_set_bank_stations(engine, t1_idx, 10.0, 90.0), SWMM_OK);

    double bL = 0, bR = 0;
    EXPECT_EQ(swmm_transect_get_bank_stations(engine, t1_idx, &bL, &bR), SWMM_OK);
    EXPECT_DOUBLE_EQ(bL, 10.0);
    EXPECT_DOUBLE_EQ(bR, 90.0);

    // Setting bank stations does NOT touch encroachment stations.
    double eL = -1, eR = -1;
    EXPECT_EQ(swmm_transect_get_encroachment_stations(engine, t1_idx, &eL, &eR), SWMM_OK);
    EXPECT_DOUBLE_EQ(eL, 0.0);
    EXPECT_DOUBLE_EQ(eR, 0.0);
}

// ---------------------------------------------------------------------------
// Encroachment stations — BQ-TR-02; independent of bank stations.
// ---------------------------------------------------------------------------

TEST_F(TransectMutationTest, SetEncroachmentStationsRoundTripsIndependentOfBank) {
    ASSERT_EQ(swmm_transect_set_bank_stations(engine, t1_idx, 10.0, 90.0), SWMM_OK);
    EXPECT_EQ(swmm_transect_set_encroachment_stations(engine, t1_idx, 5.0, 95.0), SWMM_OK);

    double eL = 0, eR = 0;
    EXPECT_EQ(swmm_transect_get_encroachment_stations(engine, t1_idx, &eL, &eR), SWMM_OK);
    EXPECT_DOUBLE_EQ(eL, 5.0);
    EXPECT_DOUBLE_EQ(eR, 95.0);

    // Bank stations unchanged by the encroachment setter.
    double bL = 0, bR = 0;
    EXPECT_EQ(swmm_transect_get_bank_stations(engine, t1_idx, &bL, &bR), SWMM_OK);
    EXPECT_DOUBLE_EQ(bL, 10.0);
    EXPECT_DOUBLE_EQ(bR, 90.0);
}

// ---------------------------------------------------------------------------
// Modifiers — x_factor, y_factor, length_factor (meander).
// ---------------------------------------------------------------------------

TEST_F(TransectMutationTest, SetModifiersRoundTrips) {
    EXPECT_EQ(swmm_transect_set_modifiers(engine, t1_idx, 2.5, -1.0, 1.25), SWMM_OK);

    double xF = 0, yF = 0, lF = 0;
    EXPECT_EQ(swmm_transect_get_modifiers(engine, t1_idx, &xF, &yF, &lF), SWMM_OK);
    EXPECT_DOUBLE_EQ(xF, 2.5);
    EXPECT_DOUBLE_EQ(yF, -1.0);
    EXPECT_DOUBLE_EQ(lF, 1.25);
}

// ---------------------------------------------------------------------------
// Comments — round-trip + buffer truncation + NULL clear.
// ---------------------------------------------------------------------------

TEST_F(TransectMutationTest, SetCommentsRoundTrips) {
    const char* msg = "Downstream of culvert; field-surveyed 2024-06-12.";
    EXPECT_EQ(swmm_transect_set_comments(engine, t1_idx, msg), SWMM_OK);

    char buf[128] = {};
    EXPECT_EQ(swmm_transect_get_comments(engine, t1_idx, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, msg);
}

TEST_F(TransectMutationTest, GetCommentsTruncatesWithNulTerminator) {
    ASSERT_EQ(swmm_transect_set_comments(engine, t1_idx, "abcdefghij"), SWMM_OK);
    char buf[6] = {};
    EXPECT_EQ(swmm_transect_get_comments(engine, t1_idx, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "abcde");      // 5 chars + NUL
    EXPECT_EQ(buf[5], '\0');
}

TEST_F(TransectMutationTest, SetCommentsNullClearsContent) {
    ASSERT_EQ(swmm_transect_set_comments(engine, t1_idx, "something"), SWMM_OK);
    EXPECT_EQ(swmm_transect_set_comments(engine, t1_idx, nullptr), SWMM_OK);
    char buf[32] = "junk";
    EXPECT_EQ(swmm_transect_get_comments(engine, t1_idx, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");
}

TEST_F(TransectMutationTest, GetCommentsRejectsBadBuffer) {
    char buf[1];
    EXPECT_EQ(swmm_transect_get_comments(engine, t1_idx, nullptr, 64), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_transect_get_comments(engine, t1_idx, buf, 0),     SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// Stations — count, get, clear.
// ---------------------------------------------------------------------------

TEST_F(TransectMutationTest, StationsRoundTripWithCountAndGet) {
    ASSERT_EQ(swmm_transect_add_station(engine, t1_idx, 0.0,  10.0), SWMM_OK);
    ASSERT_EQ(swmm_transect_add_station(engine, t1_idx, 5.0,   2.0), SWMM_OK);
    ASSERT_EQ(swmm_transect_add_station(engine, t1_idx, 12.0,  3.0), SWMM_OK);
    ASSERT_EQ(swmm_transect_add_station(engine, t1_idx, 20.0, 11.0), SWMM_OK);

    EXPECT_EQ(swmm_transect_get_station_count(engine, t1_idx), 4);

    double s = 0, e = 0;
    EXPECT_EQ(swmm_transect_get_station(engine, t1_idx, 0, &s, &e), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 0.0);  EXPECT_DOUBLE_EQ(e, 10.0);
    EXPECT_EQ(swmm_transect_get_station(engine, t1_idx, 2, &s, &e), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 12.0); EXPECT_DOUBLE_EQ(e, 3.0);
}

TEST_F(TransectMutationTest, ClearStationsResetsToEmpty) {
    ASSERT_EQ(swmm_transect_add_station(engine, t1_idx, 0.0, 10.0), SWMM_OK);
    ASSERT_EQ(swmm_transect_add_station(engine, t1_idx, 5.0,  2.0), SWMM_OK);
    ASSERT_EQ(swmm_transect_get_station_count(engine, t1_idx), 2);

    EXPECT_EQ(swmm_transect_clear_stations(engine, t1_idx), SWMM_OK);
    EXPECT_EQ(swmm_transect_get_station_count(engine, t1_idx), 0);

    // After clear, can re-add (the snapshot-and-rewrite path used by the GUI).
    EXPECT_EQ(swmm_transect_add_station(engine, t1_idx, 1.0, 1.0), SWMM_OK);
    EXPECT_EQ(swmm_transect_get_station_count(engine, t1_idx), 1);
}

TEST_F(TransectMutationTest, GetStationOutOfRangeReturnsBadIndex) {
    ASSERT_EQ(swmm_transect_add_station(engine, t1_idx, 0.0, 10.0), SWMM_OK);
    double s = 0, e = 0;
    EXPECT_EQ(swmm_transect_get_station(engine, t1_idx, -1, &s, &e), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_transect_get_station(engine, t1_idx,  1, &s, &e), SWMM_ERR_BADINDEX);
}

// ---------------------------------------------------------------------------
// Rename — case-insensitive collision, same-name no-op, index lookup updated.
// ---------------------------------------------------------------------------

TEST_F(TransectMutationTest, RenameUpdatesIdAndIndexLookup) {
    EXPECT_EQ(swmm_transect_rename(engine, t1_idx, "MainChannel"), SWMM_OK);
    EXPECT_STREQ(swmm_transect_id(engine, t1_idx), "MainChannel");
    EXPECT_EQ(swmm_transect_index(engine, "MainChannel"), t1_idx);
    EXPECT_EQ(swmm_transect_index(engine, "T1"), -1);
}

TEST_F(TransectMutationTest, RenameSameNameIsNoop) {
    EXPECT_EQ(swmm_transect_rename(engine, t1_idx, "T1"), SWMM_OK);
    EXPECT_STREQ(swmm_transect_id(engine, t1_idx), "T1");
}

TEST_F(TransectMutationTest, RenameRejectsCaseInsensitiveCollision) {
    ASSERT_EQ(swmm_transect_add(engine, "T2"), SWMM_OK);
    // Try to rename T1 → "t2" (different case but collides).
    EXPECT_EQ(swmm_transect_rename(engine, t1_idx, "t2"), SWMM_ERR_BADPARAM);
    // Original name preserved.
    EXPECT_STREQ(swmm_transect_id(engine, t1_idx), "T1");
}

TEST_F(TransectMutationTest, RenameRejectsNullOrEmpty) {
    EXPECT_EQ(swmm_transect_rename(engine, t1_idx, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_transect_rename(engine, t1_idx, ""),       SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// Remove — out-of-range no-op, order preservation, full-field erasure.
// ---------------------------------------------------------------------------

TEST_F(TransectMutationTest, RemoveOutOfRangeIsNoop) {
    EXPECT_EQ(swmm_transect_count(engine), 1);
    EXPECT_EQ(swmm_transect_remove(engine, -1), SWMM_OK);
    EXPECT_EQ(swmm_transect_remove(engine, 99), SWMM_OK);
    EXPECT_EQ(swmm_transect_count(engine), 1);
}

TEST_F(TransectMutationTest, RemoveDropsTransectAndShiftsIndices) {
    ASSERT_EQ(swmm_transect_add(engine, "T2"), SWMM_OK);
    ASSERT_EQ(swmm_transect_add(engine, "T3"), SWMM_OK);
    EXPECT_EQ(swmm_transect_count(engine), 3);

    // Configure T2 so we can confirm it's the one that survives at idx 1
    // after dropping T1.
    const int t2 = swmm_transect_index(engine, "T2");
    ASSERT_EQ(swmm_transect_set_roughness(engine, t2, 0.04, 0.05, 0.03), SWMM_OK);
    ASSERT_EQ(swmm_transect_set_bank_stations(engine, t2, 7.5, 22.5), SWMM_OK);

    // Drop T1.
    EXPECT_EQ(swmm_transect_remove(engine, t1_idx), SWMM_OK);
    EXPECT_EQ(swmm_transect_count(engine), 2);

    // Order preserved: T2 now at idx 0, T3 at idx 1.
    EXPECT_STREQ(swmm_transect_id(engine, 0), "T2");
    EXPECT_STREQ(swmm_transect_id(engine, 1), "T3");

    // Verify the new idx-0 transect still carries T2's data (no shift mix-up).
    double nL = 0, nR = 0, nC = 0;
    EXPECT_EQ(swmm_transect_get_roughness(engine, 0, &nL, &nR, &nC), SWMM_OK);
    EXPECT_DOUBLE_EQ(nL, 0.04);
    EXPECT_DOUBLE_EQ(nR, 0.05);
    EXPECT_DOUBLE_EQ(nC, 0.03);

    double bL = 0, bR = 0;
    EXPECT_EQ(swmm_transect_get_bank_stations(engine, 0, &bL, &bR), SWMM_OK);
    EXPECT_DOUBLE_EQ(bL, 7.5);
    EXPECT_DOUBLE_EQ(bR, 22.5);
}

// ---------------------------------------------------------------------------
// Handle / index guards — uniform behaviour across the new surface.
// ---------------------------------------------------------------------------

TEST_F(TransectMutationTest, BadHandleReturnsBadHandle) {
    double v = 0;
    EXPECT_EQ(swmm_transect_get_roughness(nullptr, 0, &v, &v, &v),            SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_transect_set_bank_stations(nullptr, 0, 1, 2),              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_transect_get_bank_stations(nullptr, 0, &v, &v),            SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_transect_set_encroachment_stations(nullptr, 0, 1, 2),      SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_transect_get_encroachment_stations(nullptr, 0, &v, &v),    SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_transect_set_modifiers(nullptr, 0, 1, 1, 1),               SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_transect_get_modifiers(nullptr, 0, &v, &v, &v),            SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_transect_set_comments(nullptr, 0, "x"),                    SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_transect_clear_stations(nullptr, 0),                       SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_transect_rename(nullptr, 0, "x"),                          SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_transect_remove(nullptr, 0),                               SWMM_ERR_BADHANDLE);
}

TEST_F(TransectMutationTest, OutOfRangeIndexReturnsBadIndex) {
    double v = 0;
    EXPECT_EQ(swmm_transect_get_roughness(engine, 99, &v, &v, &v),         SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_transect_set_bank_stations(engine, 99, 1, 2),           SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_transect_set_encroachment_stations(engine, 99, 1, 2),   SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_transect_set_modifiers(engine, 99, 1, 1, 1),            SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_transect_set_comments(engine, 99, "x"),                 SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_transect_clear_stations(engine, 99),                    SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_transect_rename(engine, 99, "X"),                       SWMM_ERR_BADINDEX);
    // remove() is intentionally a no-op for out-of-range (mirrors patterns).
}
