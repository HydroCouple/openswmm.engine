/**
 * @file test_events_section.cpp
 * @brief Unit tests for the [EVENTS] section — C API + InpWriter.
 *
 * @details Slice CW (added 2026-05-21).  Covers:
 *          - swmm_events_count is zero on a fresh engine.
 *          - swmm_events_add → swmm_events_get round-trip.
 *          - swmm_events_set rejects start >= end with SWMM_ERR_BADPARAM.
 *          - swmm_events_remove shifts indices down by one.
 *          - swmm_events_clear empties the list.
 *          - InpWriter emits [EVENTS] when non-empty, skips when empty.
 *
 * Engine state is kept minimal (one junction, one outfall, one conduit)
 * so swmm_model_write has a valid topology to serialise.  Mirrors the
 * style of test_files_section.cpp.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>

namespace fs = std::filesystem;

class EventsApiTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "O1", SWMM_NODE_OUTFALL),  SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "L1", SWMM_LINK_CONDUIT),  SWMM_OK);
        ASSERT_EQ(swmm_link_set_nodes(engine,
                      swmm_link_index(engine, "L1"),
                      swmm_node_index(engine, "J1"),
                      swmm_node_index(engine, "O1")), SWMM_OK);
    }
    void TearDown() override { if (engine) swmm_engine_destroy(engine); }
};

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

TEST_F(EventsApiTest, CountIsZeroOnFreshEngine) {
    int n = -1;
    EXPECT_EQ(swmm_events_count(engine, &n), SWMM_OK);
    EXPECT_EQ(n, 0);
}

TEST_F(EventsApiTest, AddThenGetRoundTrips) {
    // OADate 46036.0 ≈ 2026-01-01 00:00; +1.5 → 2026-01-02 12:00.
    int idx = -1;
    EXPECT_EQ(swmm_events_add(engine, 46036.0, 46037.5, &idx), SWMM_OK);
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(swmm_events_add(engine, 46100.25, 46101.75, &idx), SWMM_OK);
    EXPECT_EQ(idx, 1);

    int n = 0;
    EXPECT_EQ(swmm_events_count(engine, &n), SWMM_OK);
    EXPECT_EQ(n, 2);

    double s = 0.0, e = 0.0;
    EXPECT_EQ(swmm_events_get(engine, 0, &s, &e), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 46036.0);
    EXPECT_DOUBLE_EQ(e, 46037.5);
    EXPECT_EQ(swmm_events_get(engine, 1, &s, &e), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 46100.25);
    EXPECT_DOUBLE_EQ(e, 46101.75);

    // legacy alias still works
    int legacy = -1;
    EXPECT_EQ(swmm_get_event_count(engine, &legacy), SWMM_OK);
    EXPECT_EQ(legacy, 2);
}

TEST_F(EventsApiTest, AddRejectsStartGreaterOrEqualEnd) {
    int idx = -1;
    EXPECT_EQ(swmm_events_add(engine, 5.0, 5.0, &idx), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_events_add(engine, 5.0, 4.0, &idx), SWMM_ERR_BADPARAM);
    int n = -1;
    EXPECT_EQ(swmm_events_count(engine, &n), SWMM_OK);
    EXPECT_EQ(n, 0);
}

TEST_F(EventsApiTest, SetRejectsStartGreaterOrEqualEnd) {
    ASSERT_EQ(swmm_events_add(engine, 10.0, 11.0, nullptr), SWMM_OK);
    EXPECT_EQ(swmm_events_set(engine, 0, 12.0, 12.0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_events_set(engine, 0, 12.0, 11.0), SWMM_ERR_BADPARAM);

    // Underlying row is unchanged after a rejected set.
    double s = 0.0, e = 0.0;
    EXPECT_EQ(swmm_events_get(engine, 0, &s, &e), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 10.0);
    EXPECT_DOUBLE_EQ(e, 11.0);
}

TEST_F(EventsApiTest, GetSetRejectsBadIndex) {
    double s = 0.0, e = 0.0;
    EXPECT_EQ(swmm_events_get(engine, 0, &s, &e), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_events_get(engine, -1, &s, &e), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_events_set(engine, 0, 1.0, 2.0), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_events_remove(engine, 0), SWMM_ERR_BADINDEX);
}

TEST_F(EventsApiTest, RemoveShiftsIndices) {
    ASSERT_EQ(swmm_events_add(engine, 1.0, 2.0, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_events_add(engine, 3.0, 4.0, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_events_add(engine, 5.0, 6.0, nullptr), SWMM_OK);

    // Remove the middle row.
    EXPECT_EQ(swmm_events_remove(engine, 1), SWMM_OK);
    int n = 0;
    EXPECT_EQ(swmm_events_count(engine, &n), SWMM_OK);
    EXPECT_EQ(n, 2);

    double s = 0.0, e = 0.0;
    EXPECT_EQ(swmm_events_get(engine, 0, &s, &e), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 1.0); EXPECT_DOUBLE_EQ(e, 2.0);
    EXPECT_EQ(swmm_events_get(engine, 1, &s, &e), SWMM_OK);
    EXPECT_DOUBLE_EQ(s, 5.0); EXPECT_DOUBLE_EQ(e, 6.0);
}

TEST_F(EventsApiTest, ClearEmptiesTheList) {
    ASSERT_EQ(swmm_events_add(engine, 1.0, 2.0, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_events_add(engine, 3.0, 4.0, nullptr), SWMM_OK);
    EXPECT_EQ(swmm_events_clear(engine), SWMM_OK);

    int n = -1;
    EXPECT_EQ(swmm_events_count(engine, &n), SWMM_OK);
    EXPECT_EQ(n, 0);
    // Clear on an already-empty list is a no-op.
    EXPECT_EQ(swmm_events_clear(engine), SWMM_OK);
}

// ---------------------------------------------------------------------------
// End-to-end: writer emits [EVENTS] block round-trip.
// ---------------------------------------------------------------------------

TEST_F(EventsApiTest, WriterEmitsEventsBlockWhenNonEmpty) {
    // Two clearly-disjoint OADate windows.  Exact date strings depend on
    // the engine's encodeDate/decodeDate; we verify format-shape rather
    // than hardcoded date strings to stay calendar-arithmetic-agnostic.
    // 46036.5 has a 12:00 fractional component; 46100.25 → 06:00.
    ASSERT_EQ(swmm_events_add(engine, 46036.0,  46036.5,  nullptr), SWMM_OK);
    ASSERT_EQ(swmm_events_add(engine, 46100.25, 46100.75, nullptr), SWMM_OK);

    auto path = (fs::temp_directory_path() / "events_block.inp").string();
    ASSERT_EQ(swmm_model_write(engine, path.c_str()), SWMM_OK);

    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    const std::string content = ss.str();
    f.close();   // Windows: release the handle before fs::remove.

    EXPECT_NE(content.find("[EVENTS]"), std::string::npos);

    // Slice out the [EVENTS] block so the assertions below don't pick up
    // dates from other sections (start/end dates may appear in OPTIONS).
    const auto evPos = content.find("[EVENTS]");
    ASSERT_NE(evPos, std::string::npos);
    const auto nextSec = content.find("\n[", evPos + 1);
    const std::string evBlock = content.substr(
        evPos, nextSec == std::string::npos ? std::string::npos
                                            : nextSec - evPos);

    // Two date+time pairs ⇒ at least four MM/DD/YYYY tokens in the block
    // (Start Date + End Date for each row).
    const std::regex dateRe(R"(\d{2}/\d{2}/\d{4})");
    auto dateBegin = std::sregex_iterator(evBlock.begin(), evBlock.end(), dateRe);
    auto dateEnd   = std::sregex_iterator();
    EXPECT_GE(std::distance(dateBegin, dateEnd), 4);

    // HH:MM resolution (legacy SWMM 5 parity).  The writer must truncate
    // the engine's HH:MM:SS to HH:MM — confirm by counting occurrences:
    //   ":00" appears as the minute suffix on row 1 (start = 00:00) and
    //   row 2 end (.75 → 18:00); but NO ":00:00" SS-suffix should appear.
    EXPECT_EQ(evBlock.find(":00:00"), std::string::npos)
        << "InpWriter must truncate HH:MM:SS to HH:MM for legacy parity";
    // And the expected fractional times do appear:
    EXPECT_NE(evBlock.find("12:00"), std::string::npos);   // row 1 end
    EXPECT_NE(evBlock.find("06:00"), std::string::npos);   // row 2 start
    EXPECT_NE(evBlock.find("18:00"), std::string::npos);   // row 2 end

    fs::remove(path);
}

TEST_F(EventsApiTest, WriterSkipsEventsBlockWhenEmpty) {
    auto path = (fs::temp_directory_path() / "no_events.inp").string();
    ASSERT_EQ(swmm_model_write(engine, path.c_str()), SWMM_OK);

    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    const std::string content = ss.str();
    f.close();

    EXPECT_EQ(content.find("[EVENTS]"), std::string::npos);
    fs::remove(path);
}
