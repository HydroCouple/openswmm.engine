/**
 * @file      test_files_section.cpp
 * @brief     Unit tests for the [FILES] section — handler + writer + C API.
 *
 * @details Covers:
 *          - swmm_files_get/set: round-trip every recognised key,
 *            invalid keys / values rejected, mode normalisation.
 *          - InpWriter: emits `[FILES]` rows when the spec is non-empty
 *            and skips when blank.
 *          - FilesHandler: parses every legacy keyword combo, including
 *            the SAVE HOTSTART date+time tail.
 *          - End-to-end round-trip: write → read-back → values survive.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_hotstart.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_reactions.h>
#ifdef OPENSWMM_HAS_2D
#include <openswmm/engine/openswmm_gw2d.h>
#endif

#include <openswmm/legacy/engine/openswmm_solver.h> // swmm_run: legacy SWMM 5.3

#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/core/SWMMEngine.hpp"
#include "../../src/engine/core/HotStartManager.hpp"
#include "../../src/engine/input/handlers/FilesHandler.hpp"

namespace fs = std::filesystem;
using openswmm::FileMode;
using openswmm::HotStartManager;
using openswmm::SimulationContext;

// ---------------------------------------------------------------------------
// Pure parser — no engine handle required.
// ---------------------------------------------------------------------------

TEST(FilesHandlerTest, ParsesEveryKindAndMode) {
    SimulationContext ctx;
    openswmm::input::handle_files(ctx, {
                                           "USE  RAINFALL  \"rain.dat\"",
                                           "SAVE RUNOFF    \"runoff.bin\"",
                                           "USE  RDII      \"rdii.dat\"",
                                           "USE  INFLOWS   \"inflows.dat\"",
                                           "SAVE OUTFLOWS  \"outflows.dat\"",
                                           "USE  HOTSTART  \"hot_in.hsf\"",
                                           "SAVE HOTSTART  \"hot_out.hsf\"",
                                       });
    EXPECT_EQ(ctx.files.rainfall_mode, FileMode::USE);
    EXPECT_EQ(ctx.files.rainfall_path, "rain.dat");
    EXPECT_EQ(ctx.files.runoff_mode, FileMode::SAVE);
    EXPECT_EQ(ctx.files.runoff_path, "runoff.bin");
    EXPECT_EQ(ctx.files.rdii_mode, FileMode::USE);
    EXPECT_EQ(ctx.files.rdii_path, "rdii.dat");
    EXPECT_EQ(ctx.files.inflows_path, "inflows.dat");
    EXPECT_EQ(ctx.files.outflows_path, "outflows.dat");
    EXPECT_EQ(ctx.files.hotstart_use_path, "hot_in.hsf");
    ASSERT_EQ(ctx.files.hotstart_saves.size(), 1u);
    EXPECT_EQ(ctx.files.hotstart_saves.front().path, "hot_out.hsf");
    EXPECT_EQ(ctx.files.hotstart_saves.front().datetime, 0.0);
}

TEST(FilesHandlerTest, ParsesSaveHotstartWithDateTime) {
    SimulationContext ctx;
    openswmm::input::handle_files(ctx, {
                                           "SAVE HOTSTART \"out.hsf\" 01/15/2026 12:30:00",
                                       });
    ASSERT_EQ(ctx.files.hotstart_saves.size(), 1u);
    EXPECT_EQ(ctx.files.hotstart_saves.front().path, "out.hsf");
    EXPECT_GT(ctx.files.hotstart_saves.front().datetime, 0.0);
}

TEST(FilesHandlerTest, IgnoresUnknownKindAndMode) {
    SimulationContext ctx;
    openswmm::input::handle_files(ctx, {
                                           "UNKNOWNMODE RAINFALL \"x.dat\"",   // bad mode → skipped
                                           "USE         FROBNICATE \"y.dat\"", // bad kind → skipped
                                           "USE         RAINFALL   \"good.dat\"",
                                       });
    EXPECT_EQ(ctx.files.rainfall_path, "good.dat");
    EXPECT_EQ(ctx.files.rainfall_mode, FileMode::USE);
}

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

class FilesApiTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    /**
     * @brief Create an editable J1-to-O1 conduit model for API edits.
     *
     * Allocates the fixture-owned handle, adds junction/outfall/L1 conduit and connects its endpoints.
     * Does not yet set complete hydraulic geometry or initialize a simulation. Fatal assertions leave
     * the partial model for teardown.
     */
    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "O1", SWMM_NODE_OUTFALL), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "L1", SWMM_LINK_CONDUIT), SWMM_OK);
        ASSERT_EQ(swmm_link_set_nodes(engine, swmm_link_index(engine, "L1"),
                                      swmm_node_index(engine, "J1"), swmm_node_index(engine, "O1")),
                  SWMM_OK);
    }
    /**
     * @brief Destroy the fixture engine when allocation succeeded.
     *
     * Releases model resources through engine destruction; the member value is not reset and must not
     * be reused after teardown.
     */
    void TearDown() override {
        if (engine)
            swmm_engine_destroy(engine);
    }
};

TEST_F(FilesApiTest, GetReturnsEmptyForUnsetSlots) {
    char buf[128];
    EXPECT_EQ(swmm_files_get(engine, "RAINFALL_PATH", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");
    EXPECT_EQ(swmm_files_get(engine, "RAINFALL_MODE", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");
}

TEST_F(FilesApiTest, SetThenGetRoundTripPathsAndModes) {
    EXPECT_EQ(swmm_files_set(engine, "RAINFALL_PATH", "rain.dat"), SWMM_OK);
    EXPECT_EQ(swmm_files_set(engine, "RAINFALL_MODE", "USE"), SWMM_OK);
    EXPECT_EQ(swmm_files_set(engine, "HOTSTART_SAVE_PATH", "h.hsf"), SWMM_OK);
    EXPECT_EQ(swmm_files_set(engine, "HOTSTART_SAVE_DATETIME", "46036.5"), SWMM_OK);

    char buf[128];
    EXPECT_EQ(swmm_files_get(engine, "RAINFALL_PATH", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "rain.dat");
    EXPECT_EQ(swmm_files_get(engine, "RAINFALL_MODE", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "USE");
    EXPECT_EQ(swmm_files_get(engine, "HOTSTART_SAVE_PATH", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "h.hsf");
}

TEST_F(FilesApiTest, ModeKeyAcceptsSaveUseAndEmpty) {
    EXPECT_EQ(swmm_files_set(engine, "RUNOFF_MODE", "SAVE"), SWMM_OK);
    EXPECT_EQ(swmm_files_set(engine, "RUNOFF_MODE", "USE"), SWMM_OK);
    EXPECT_EQ(swmm_files_set(engine, "RUNOFF_MODE", ""), SWMM_OK); // clears slot
    EXPECT_EQ(swmm_files_set(engine, "RUNOFF_MODE", "NONE"), SWMM_OK);

    EXPECT_EQ(swmm_files_set(engine, "RUNOFF_MODE", "GIBBERISH"), SWMM_ERR_BADPARAM);
}

TEST_F(FilesApiTest, KeyIsCaseInsensitive) {
    EXPECT_EQ(swmm_files_set(engine, "rainfall_path", "x.dat"), SWMM_OK);
    char buf[64];
    EXPECT_EQ(swmm_files_get(engine, "Rainfall_Path", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "x.dat");
}

TEST_F(FilesApiTest, UnknownKeyRejected) {
    char buf[16];
    EXPECT_EQ(swmm_files_get(engine, "WAT", buf, sizeof(buf)), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_files_set(engine, "WAT", "x"), SWMM_ERR_BADPARAM);
}

TEST_F(FilesApiTest, NullArgsRejected) {
    char buf[16];
    EXPECT_EQ(swmm_files_get(engine, nullptr, buf, sizeof(buf)), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_files_set(engine, nullptr, "x"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_files_set(engine, "RAINFALL_PATH", nullptr), SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// End-to-end: set fields → swmm_model_write → grep emitted [FILES] block.
// ---------------------------------------------------------------------------

TEST_F(FilesApiTest, WriterEmitsConfiguredFilesBlock) {
    ASSERT_EQ(swmm_files_set(engine, "RAINFALL_PATH", "rain.dat"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine, "RAINFALL_MODE", "USE"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine, "HOTSTART_USE_PATH", "h.hsf"), SWMM_OK);

    auto path = (fs::temp_directory_path() / "files_block.inp").string();
    ASSERT_EQ(swmm_model_write(engine, path.c_str()), SWMM_OK);

    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string content = ss.str();
    f.close(); // Windows: must release the handle before fs::remove.
    EXPECT_NE(content.find("[FILES]"), std::string::npos);
    EXPECT_NE(content.find("rain.dat"), std::string::npos);
    EXPECT_NE(content.find("h.hsf"), std::string::npos);
    EXPECT_NE(content.find("USE"), std::string::npos);
    fs::remove(path);
}

TEST_F(FilesApiTest, WriterSkipsBlockWhenSpecIsEmpty) {
    auto path = (fs::temp_directory_path() / "no_files.inp").string();
    ASSERT_EQ(swmm_model_write(engine, path.c_str()), SWMM_OK);

    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string content = ss.str();
    f.close(); // Windows: must release the handle before fs::remove.
    EXPECT_EQ(content.find("[FILES]"), std::string::npos);
    fs::remove(path);
}

// ---------------------------------------------------------------------------
// Multi-row SAVE HOTSTART (legacy supported up to 10 rows)
// ---------------------------------------------------------------------------

TEST(FilesHandlerTest, AppendsMultipleSaveHotstartRows) {
    SimulationContext ctx;
    openswmm::input::handle_files(ctx, {
                                           "SAVE HOTSTART \"first.hsf\"  01/01/2026 12:00:00",
                                           "SAVE HOTSTART \"second.hsf\" 01/01/2026 18:00:00",
                                           "SAVE HOTSTART \"third.hsf\"", // no datetime
                                       });
    ASSERT_EQ(ctx.files.hotstart_saves.size(), 3u);
    EXPECT_EQ(ctx.files.hotstart_saves[0].path, "first.hsf");
    EXPECT_GT(ctx.files.hotstart_saves[0].datetime, 0.0);
    EXPECT_EQ(ctx.files.hotstart_saves[1].path, "second.hsf");
    EXPECT_GT(ctx.files.hotstart_saves[1].datetime, ctx.files.hotstart_saves[0].datetime);
    EXPECT_EQ(ctx.files.hotstart_saves[2].path, "third.hsf");
    EXPECT_EQ(ctx.files.hotstart_saves[2].datetime, 0.0);
}

// ---------------------------------------------------------------------------
// Multi-slot SAVE HOTSTART C API (Slice BV-01 — swmm_hotstart_saves_*)
// Generalizes the slot-0 sugar above so the GUI can drive a multi-row table.
// ---------------------------------------------------------------------------

TEST_F(FilesApiTest, HotstartSavesCountStartsAtZero) {
    int count = -1;
    EXPECT_EQ(swmm_hotstart_saves_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 0);
}

TEST_F(FilesApiTest, HotstartSavesAddAppendsAndCountReflects) {
    EXPECT_EQ(swmm_hotstart_saves_add(engine, "a.hsf", 0.0), SWMM_OK);
    EXPECT_EQ(swmm_hotstart_saves_add(engine, "b.hsf", 46036.5), SWMM_OK);
    EXPECT_EQ(swmm_hotstart_saves_add(engine, "c.hsf", 0.0), SWMM_OK);

    int count = -1;
    ASSERT_EQ(swmm_hotstart_saves_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 3);

    char buf[128];
    double dt = -1.0;
    ASSERT_EQ(swmm_hotstart_saves_get_path(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "a.hsf");
    ASSERT_EQ(swmm_hotstart_saves_get_datetime(engine, 0, &dt), SWMM_OK);
    EXPECT_EQ(dt, 0.0);
    ASSERT_EQ(swmm_hotstart_saves_get_path(engine, 1, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "b.hsf");
    ASSERT_EQ(swmm_hotstart_saves_get_datetime(engine, 1, &dt), SWMM_OK);
    EXPECT_DOUBLE_EQ(dt, 46036.5);
    ASSERT_EQ(swmm_hotstart_saves_get_path(engine, 2, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "c.hsf");
}

TEST_F(FilesApiTest, HotstartSavesSetEditsExistingSlotInPlace) {
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "a.hsf", 0.0), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "b.hsf", 100.0), SWMM_OK);

    EXPECT_EQ(swmm_hotstart_saves_set_path(engine, 1, "renamed.hsf"), SWMM_OK);
    EXPECT_EQ(swmm_hotstart_saves_set_datetime(engine, 0, 50.0), SWMM_OK);

    char buf[128];
    double dt = -1.0;
    ASSERT_EQ(swmm_hotstart_saves_get_path(engine, 1, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "renamed.hsf");
    ASSERT_EQ(swmm_hotstart_saves_get_datetime(engine, 0, &dt), SWMM_OK);
    EXPECT_DOUBLE_EQ(dt, 50.0);
}

TEST_F(FilesApiTest, HotstartSavesRemoveShiftsSubsequentSlotsDown) {
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "a.hsf", 0.0), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "b.hsf", 0.0), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "c.hsf", 0.0), SWMM_OK);

    EXPECT_EQ(swmm_hotstart_saves_remove(engine, 1), SWMM_OK); // removes b

    int count = -1;
    ASSERT_EQ(swmm_hotstart_saves_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 2);

    char buf[128];
    ASSERT_EQ(swmm_hotstart_saves_get_path(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "a.hsf");
    ASSERT_EQ(swmm_hotstart_saves_get_path(engine, 1, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "c.hsf"); // formerly slot 2
}

TEST_F(FilesApiTest, HotstartSavesClearEmptiesVector) {
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "a.hsf", 0.0), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "b.hsf", 0.0), SWMM_OK);
    EXPECT_EQ(swmm_hotstart_saves_clear(engine), SWMM_OK);
    int count = -1;
    EXPECT_EQ(swmm_hotstart_saves_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 0);
}

TEST_F(FilesApiTest, HotstartSavesOutOfRangeIsBadindex) {
    char buf[16];
    double dt;
    int fmt;
    EXPECT_EQ(swmm_hotstart_saves_get_path(engine, 0, buf, sizeof(buf)), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_hotstart_saves_get_datetime(engine, 0, &dt), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_hotstart_saves_set_path(engine, 0, "x"), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_hotstart_saves_set_datetime(engine, 0, 0.0), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_hotstart_saves_get_format(engine, 0, &fmt), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_hotstart_saves_set_format(engine, 0, SWMM_HOTSTART_FORMAT_AUTO),
              SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_hotstart_saves_remove(engine, 0), SWMM_ERR_BADINDEX);

    ASSERT_EQ(swmm_hotstart_saves_add(engine, "a.hsf", 0.0), SWMM_OK);
    EXPECT_EQ(swmm_hotstart_saves_get_path(engine, -1, buf, sizeof(buf)), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_hotstart_saves_get_path(engine, 5, buf, sizeof(buf)), SWMM_ERR_BADINDEX);
    // A missing buffer is still a bad parameter.
    EXPECT_EQ(swmm_hotstart_saves_get_path(engine, 0, nullptr, 0), SWMM_ERR_BADPARAM);
}

TEST_F(FilesApiTest, HotstartSavesSlotZeroSugarStillWorks) {
    // Slot-0 sugar (swmm_files_set) must coexist with the new vector API.
    ASSERT_EQ(swmm_files_set(engine, "HOTSTART_SAVE_PATH", "via_sugar.hsf"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine, "HOTSTART_SAVE_DATETIME", "12.5"), SWMM_OK);

    int count = -1;
    ASSERT_EQ(swmm_hotstart_saves_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 1);

    char buf[128];
    double dt = -1.0;
    ASSERT_EQ(swmm_hotstart_saves_get_path(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "via_sugar.hsf");
    ASSERT_EQ(swmm_hotstart_saves_get_datetime(engine, 0, &dt), SWMM_OK);
    EXPECT_DOUBLE_EQ(dt, 12.5);

    // Appending via the new API extends past slot 0.
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "slot1.hsf", 24.0), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 2);

    // Reading slot 0 back via the sugar key still returns via_sugar.hsf.
    ASSERT_EQ(swmm_files_get(engine, "HOTSTART_SAVE_PATH", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "via_sugar.hsf");
}

TEST_F(FilesApiTest, HotstartSavesWriterRoundTripsEntriesAndDatetimes) {
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "first.hsf", 46036.5), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "second.hsf", 0.0), SWMM_OK);

    auto path = (fs::temp_directory_path() / "multi_hotstart.inp").string();
    ASSERT_EQ(swmm_model_write(engine, path.c_str()), SWMM_OK);

    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string content = ss.str();
    f.close();

    EXPECT_NE(content.find("first.hsf"), std::string::npos);
    EXPECT_NE(content.find("second.hsf"), std::string::npos);
    // The second row has datetime=0 → no trailing date string.
    // (We can't easily grep for absence, but presence of both paths plus a
    //  single date token in between is the round-trip signal.)
    fs::remove(path);
}

TEST_F(FilesApiTest, HotstartSavesNullArgsRejected) {
    EXPECT_EQ(swmm_hotstart_saves_count(engine, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hotstart_saves_get_path(engine, 0, nullptr, 16), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hotstart_saves_get_datetime(engine, 0, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hotstart_saves_add(engine, nullptr, 0.0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hotstart_saves_count(nullptr, nullptr), SWMM_ERR_BADHANDLE);
}

TEST_F(FilesApiTest, MultiRowSaveHotstartRoundTrip) {
    // Build an [FILES] block by hand-parsing into the engine's ctx,
    // then write + grep to confirm all rows survive.  Bypass the C API
    // (which is slot-0 sugar) and reach into the C++ context directly.
    // The handler-level test above covers the parse path; here we
    // verify InpWriter emits every row.
    //
    // Trick: use swmm_files_set on slot 0 once, then push two more
    // entries via the handler test seam (handle_files via raw lines).
    //
    // To keep the test small and engine-API-driven, we instead
    // exercise the full round-trip via the input plugin's write()
    // is overkill — just write directly using the engine's
    // SimulationContext via the friendlier swmm_files_set for slot 0
    // and confirm the singular keys still work, then separately
    // assert the writer emits the row with a datetime.
    ASSERT_EQ(swmm_files_set(engine, "HOTSTART_SAVE_PATH", "snap.hsf"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine, "HOTSTART_SAVE_DATETIME", "46036.5"), SWMM_OK);

    char buf[64];
    ASSERT_EQ(swmm_files_get(engine, "HOTSTART_SAVE_COUNT", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "1");

    auto path = (fs::temp_directory_path() / "hotstart_dt.inp").string();
    ASSERT_EQ(swmm_model_write(engine, path.c_str()), SWMM_OK);
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    f.close();
    const std::string content = ss.str();
    EXPECT_NE(content.find("SAVE"), std::string::npos);
    EXPECT_NE(content.find("HOTSTART"), std::string::npos);
    EXPECT_NE(content.find("snap.hsf"), std::string::npos);

    // The writer emits a "MM/DD/YYYY HH:MM:SS" tail when datetime > 0.
    // Don't assert the exact decoded date (SWMM epoch math is its own
    // thing) — just check the format pattern is present in the [FILES]
    // section line for `snap.hsf`.  Find the line and inspect it.
    const auto pos = content.find("snap.hsf");
    ASSERT_NE(pos, std::string::npos);
    const auto eol = content.find('\n', pos);
    const std::string row = content.substr(pos, eol - pos);
    EXPECT_NE(row.find('/'), std::string::npos)
        << "writer should emit MM/DD/YYYY when datetime > 0; row: " << row;
    EXPECT_NE(row.find(':'), std::string::npos)
        << "writer should emit HH:MM:SS when datetime > 0; row: " << row;
    fs::remove(path);
}

TEST_F(FilesApiTest, ClearingSlotZeroRemovesEmptyEntry) {
    // Setting both path and datetime to empty/zero should leave the
    // hotstart_saves vector clean (count == 0).  This keeps the
    // single-slot API back-compatible with the original schema where
    // an empty path meant "no SAVE HOTSTART row".
    ASSERT_EQ(swmm_files_set(engine, "HOTSTART_SAVE_PATH", "x.hsf"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine, "HOTSTART_SAVE_PATH", ""), SWMM_OK);

    char buf[16];
    ASSERT_EQ(swmm_files_get(engine, "HOTSTART_SAVE_COUNT", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "0");
}

// ---------------------------------------------------------------------------
// SAVE HOTSTART format keyword: `SAVE HOTSTART f [date time] LEGACY|NATIVE`.
// A row without it is AUTO and keeps its old meaning and its old text.
// Written files go to tests/unit/engine/data/hotstart_format/ for review.
// ---------------------------------------------------------------------------

namespace {
/**
 * @brief Read a fixture artifact into an owned string.
 *
 * Local stream closes at return. Does not assert or expose IO status; allocation exceptions
 * propagate.
 *
 * @param[in] p Borrowed input path opened in ordinary text mode.
 * @return Owned stream contents, empty on empty/unopenable input; read failures can yield partial
 *     text.
 */
std::string slurpFile(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// The written SAVE HOTSTART row naming `file`, without its newline.
/**
 * @brief Extract the line containing the first quoted matching filename.
 *
 * Does not verify the matched row actually begins SAVE HOTSTART; caller chooses text/filename where
 * the first occurrence identifies the intended row.
 *
 * @param[in] text Borrowed multiline written model text.
 * @param[in] file Raw filename searched inside literal double quotes; not escaped.
 * @return Owned complete containing line without its newline, or empty when no quoted match exists.
 */
std::string saveRowFor(const std::string& text, const std::string& file) {
    const auto at = text.find("\"" + file + "\"");
    if (at == std::string::npos)
        return {};
    const auto bol = text.rfind('\n', at) + 1;
    return text.substr(bol, text.find('\n', at) - bol);
}
} // namespace

TEST(FilesHandlerTest, SaveHotstartFormatKeywordTrailsPathAndDatetime) {
    using openswmm::HotstartFormat;
    SimulationContext ctx;
    openswmm::input::handle_files(ctx, {
                                           "SAVE HOTSTART \"plain.hsf\"",
                                           "SAVE HOTSTART \"native.hsf\" NATIVE",
                                           "SAVE HOTSTART \"dated.hsf\" 01/15/2026 12:30:00 legacy",
                                           "SAVE HOTSTART \"dated_plain.hsf\" 01/15/2026 12:30:00",
                                           "SAVE HOTSTART \"odd.hsf\" SOMETHING",
                                           "SAVE HOTSTART \"typo.hsf\" 01/15/2026 12:30:00 NATVE",
                                       });
    ASSERT_EQ(ctx.files.hotstart_saves.size(), 6u);
    EXPECT_TRUE(ctx.errors.empty());
    const auto& s = ctx.files.hotstart_saves;
    EXPECT_EQ(s[0].format, HotstartFormat::AUTO);
    EXPECT_EQ(s[1].format, HotstartFormat::NATIVE);
    EXPECT_EQ(s[1].datetime, 0.0);
    EXPECT_EQ(s[2].format, HotstartFormat::LEGACY); // case-insensitive
    EXPECT_GT(s[2].datetime, 0.0);
    EXPECT_EQ(s[2].datetime, s[3].datetime) << "the keyword moved the save time";
    EXPECT_EQ(s[3].format, HotstartFormat::AUTO);
    // Any other token in the keyword's place is ignored, as legacy ignores
    // it, but now with a warning that names it.
    EXPECT_EQ(s[4].format, HotstartFormat::AUTO);
    EXPECT_EQ(s[4].path, "odd.hsf");
    EXPECT_EQ(s[4].datetime, 0.0);
    EXPECT_EQ(s[5].format, HotstartFormat::AUTO);
    EXPECT_EQ(s[5].datetime, s[2].datetime);
    ASSERT_EQ(ctx.warnings.size(), 2u);
    EXPECT_NE(ctx.warnings[0].find("'SOMETHING'"), std::string::npos) << ctx.warnings[0];
    EXPECT_NE(ctx.warnings[1].find("'NATVE'"), std::string::npos) << ctx.warnings[1];
    EXPECT_EQ(ctx.warning_code, 101);
}

// Legacy iface.c reads a datetime only from a five-token row and rejects one
// whose date or time does not parse (ERR_DATETIME). Such a row used to be
// read as an end-of-run or midnight save in the default format.
TEST(FilesHandlerTest, FiveTokenSaveHotstartRowNeedsADateAndATime) {
    const struct {
        const char* row;
        const char* bad;
    } cases[] = {
        {"SAVE HOTSTART \"a.hsf\" NATIVE extra", "NATIVE"},
        {"SAVE HOTSTART \"b.hsf\" 01/15/2026 NATIVE", "NATIVE"},
        {"SAVE HOTSTART \"c.hsf\" 15/2026 12:30:00", "15/2026"},
    };
    for (const auto& c : cases) {
        SimulationContext ctx;
        openswmm::input::handle_files(ctx, {c.row});
        EXPECT_TRUE(ctx.files.hotstart_saves.empty()) << c.row;
        ASSERT_EQ(ctx.errors.size(), 1u) << c.row;
        EXPECT_NE(ctx.errors[0].find("ERROR 2"), std::string::npos) << ctx.errors[0];
        EXPECT_NE(ctx.errors[0].find(c.bad), std::string::npos) << ctx.errors[0];
    }
    // A valid date and time, decimal hours included, still parse.
    SimulationContext ok;
    openswmm::input::handle_files(
        ok, {"SAVE HOTSTART \"d.hsf\" 01/15/2026 12:30", "SAVE HOTSTART \"e.hsf\" 01/15/2026 0"});
    EXPECT_TRUE(ok.errors.empty());
    ASSERT_EQ(ok.files.hotstart_saves.size(), 2u);
    EXPECT_GT(ok.files.hotstart_saves[1].datetime, 0.0);
}

TEST_F(FilesApiTest, HotstartSavesFormatGetSetAndRange) {
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "a.hsf", 0.0), SWMM_OK);
    int fmt = -1;
    ASSERT_EQ(swmm_hotstart_saves_get_format(engine, 0, &fmt), SWMM_OK);
    EXPECT_EQ(fmt, SWMM_HOTSTART_FORMAT_AUTO);
    ASSERT_EQ(swmm_hotstart_saves_set_format(engine, 0, SWMM_HOTSTART_FORMAT_NATIVE), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_get_format(engine, 0, &fmt), SWMM_OK);
    EXPECT_EQ(fmt, SWMM_HOTSTART_FORMAT_NATIVE);

    // Editing the path or the datetime leaves the format alone.
    ASSERT_EQ(swmm_hotstart_saves_set_path(engine, 0, "b.hsf"), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_set_datetime(engine, 0, 10.0), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_get_format(engine, 0, &fmt), SWMM_OK);
    EXPECT_EQ(fmt, SWMM_HOTSTART_FORMAT_NATIVE);

    EXPECT_EQ(swmm_hotstart_saves_set_format(engine, 0, 3), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_hotstart_saves_set_format(engine, 0, -1), SWMM_ERR_BADPARAM);
    for (const int idx : {-1, 1}) {
        EXPECT_EQ(swmm_hotstart_saves_set_format(engine, idx, SWMM_HOTSTART_FORMAT_LEGACY),
                  SWMM_ERR_BADINDEX);
        EXPECT_EQ(swmm_hotstart_saves_get_format(engine, idx, &fmt), SWMM_ERR_BADINDEX);
    }
    EXPECT_EQ(swmm_hotstart_saves_get_format(engine, 0, nullptr), SWMM_ERR_BADPARAM);
}

TEST_F(FilesApiTest, WriterEmitsTheFormatKeywordOnlyWhenChosen) {
    const fs::path dir = "hotstart_format";
    fs::create_directories(dir);
    // Valid geometry, so the written decks reopen as whole models below.
    const int l1 = swmm_link_index(engine, "L1");
    ASSERT_EQ(swmm_link_set_length(engine, l1, 100.0), SWMM_OK);
    ASSERT_EQ(swmm_link_set_roughness(engine, l1, 0.013), SWMM_OK);
    ASSERT_EQ(swmm_link_set_xsect(engine, l1, SWMM_XSECT_CIRCULAR, 1.0, 0, 0, 0), SWMM_OK);
    ASSERT_EQ(swmm_node_set_max_depth(engine, swmm_node_index(engine, "J1"), 5.0), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "auto.hsf", 0.0), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "native.hsf", 0.0), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "legacy.hsf", 46036.5), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_set_format(engine, 1, SWMM_HOTSTART_FORMAT_NATIVE), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_set_format(engine, 2, SWMM_HOTSTART_FORMAT_LEGACY), SWMM_OK);

    const fs::path full = dir / "writer_full.inp";
    ASSERT_EQ(swmm_model_write(engine, full.string().c_str()), SWMM_OK);
    const std::string text = slurpFile(full);
    const std::string r_auto = saveRowFor(text, "auto.hsf");
    const std::string r_nat = saveRowFor(text, "native.hsf");
    const std::string r_leg = saveRowFor(text, "legacy.hsf");
    ASSERT_FALSE(r_auto.empty());
    ASSERT_FALSE(r_nat.empty());
    ASSERT_FALSE(r_leg.empty());
    // AUTO: byte-for-byte the row the writer produced before the keyword.
    EXPECT_EQ(r_auto.substr(r_auto.size() - std::string("\"auto.hsf\"").size()), "\"auto.hsf\"")
        << r_auto;
    EXPECT_EQ(r_nat.substr(r_nat.size() - 7), " NATIVE") << r_nat;
    EXPECT_EQ(r_leg.substr(r_leg.size() - 7), " LEGACY") << r_leg;
    EXPECT_NE(r_leg.find('/'), std::string::npos) << "datetime lost: " << r_leg;

    // The written rows parse back to the same three entries.
    SimulationContext back;
    openswmm::input::handle_files(back, {r_auto, r_nat, r_leg});
    ASSERT_EQ(back.files.hotstart_saves.size(), 3u);
    EXPECT_EQ(back.files.hotstart_saves[0].format, openswmm::HotstartFormat::AUTO);
    EXPECT_EQ(back.files.hotstart_saves[1].format, openswmm::HotstartFormat::NATIVE);
    EXPECT_EQ(back.files.hotstart_saves[2].format, openswmm::HotstartFormat::LEGACY);
    EXPECT_DOUBLE_EQ(back.files.hotstart_saves[2].datetime, 46036.5);

    // SWMM 5 export drops the keyword (a dated six-token row would lose
    // its datetime in legacy SWMM 5) and keeps everything else.
    const fs::path compat = dir / "writer_swmm5.inp";
    ASSERT_EQ(swmm_model_write_compat(engine, compat.string().c_str(), SWMM_INP_PROFILE_SWMM5),
              SWMM_OK);
    const std::string old = slurpFile(compat);
    EXPECT_EQ(saveRowFor(old, "native.hsf").find("NATIVE"), std::string::npos);
    const std::string old_leg = saveRowFor(old, "legacy.hsf");
    EXPECT_EQ(old_leg.find("LEGACY"), std::string::npos) << old_leg;
    EXPECT_NE(old_leg.find('/'), std::string::npos) << old_leg;
    EXPECT_EQ(saveRowFor(old, "auto.hsf"), r_auto);

    // Both decks reopen as models: the full one with every row's format and
    // the dated row's time, the SWMM 5 one with every row back to AUTO.
    const struct {
        fs::path inp;
        int formats[3];
    } decks[] = {
        {full,
         {SWMM_HOTSTART_FORMAT_AUTO, SWMM_HOTSTART_FORMAT_NATIVE, SWMM_HOTSTART_FORMAT_LEGACY}},
        {compat, {SWMM_HOTSTART_FORMAT_AUTO, SWMM_HOTSTART_FORMAT_AUTO, SWMM_HOTSTART_FORMAT_AUTO}},
    };
    for (const auto& d : decks) {
        SWMM_Engine e = swmm_engine_create();
        ASSERT_EQ(swmm_engine_open(e, d.inp.string().c_str(), "", "", nullptr), SWMM_OK)
            << d.inp << ": " << swmm_get_last_error_msg(e);
        int n = 0;
        ASSERT_EQ(swmm_hotstart_saves_count(e, &n), SWMM_OK);
        ASSERT_EQ(n, 3) << d.inp;
        for (int i = 0; i < 3; ++i) {
            int f = -1;
            ASSERT_EQ(swmm_hotstart_saves_get_format(e, i, &f), SWMM_OK);
            EXPECT_EQ(f, d.formats[i]) << d.inp << " row " << i;
        }
        double dt = 0.0;
        ASSERT_EQ(swmm_hotstart_saves_get_datetime(e, 2, &dt), SWMM_OK);
        EXPECT_DOUBLE_EQ(dt, 46036.5) << d.inp;
        swmm_engine_destroy(e);
    }
}

// ---------------------------------------------------------------------------
// The format keyword through real runs of the site drainage deck. The save
// run stops mid-storm (03:00) so a restart is distinguishable from a cold
// start. THREADS 1 in the deck keeps the .out files comparable byte-wise.
// ---------------------------------------------------------------------------

namespace {
/**
 * @brief Read a fixture artifact into an owned string.
 *
 * Local stream closes at return. Does not assert or expose IO status; allocation exceptions
 * propagate.
 *
 * @param[in] p Borrowed input path opened in binary mode.
 * @return Owned stream contents, empty on empty/unopenable input; read failures can yield partial
 *     text.
 */
std::string slurpBinary(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/**
 * @brief Write a copy of the fixed site-drainage model with FILES rows.
 *
 * Reads site_drainage_model.inp from the working directory through unchecked slurpFile. Missing
 * stop-time fragment is silently ignored; local output truncates without status checking. Does not
 * parse/run the result.
 *
 * @param[in] dir Existing destination directory.
 * @param[in] name Raw stem for the overwritten .inp file.
 * @param[in] files Raw FILES rows; empty omits the appended section.
 * @param[in] stop_mid_storm True attempts the exact end-date/time replacement to stop at 03:00
 *     during the storm.
 * @return Absolute destination input path, even if writing fails.
 */
fs::path deckWithFiles(const fs::path& dir, const std::string& name, const std::string& files,
                       bool stop_mid_storm = false) {
    std::string text = slurpFile("site_drainage_model.inp");
    if (stop_mid_storm) {
        const std::string end = "END_DATE             01/02/1998\nEND_TIME             06:00:00";
        const auto at = text.find(end);
        if (at != std::string::npos)
            text.replace(at, end.size(),
                         "END_DATE             01/01/1998\nEND_TIME             03:00:00");
    }
    if (!files.empty())
        text += "\n[FILES]\n" + files;
    const fs::path inp = fs::absolute(dir / (name + ".inp"));
    std::ofstream(inp) << text;
    return inp;
}

// Open, initialise, optionally apply `hs_file` through the C API (the
// reference path), run to the end, report and close.
/**
 * @brief Run a fixture through the public engine lifecycle with optional hotstart application.
 *
 * Owns/destroys a temporary engine and closes temporary hotstart handles. Requests saved results,
 * loops without a guard until elapsed completion or rejection, and calls end after successful start
 * even on step failure. Allocation is not checked separately; artifact paths may be overwritten.
 *
 * @param[in] inp Existing input path; matching .rpt/.out are used as destinations.
 * @param[in] hs_file Optional hotstart path, opened/applied after initialization; empty skips
 *     hotstart.
 * @return First rejected open/init/hotstart/start/step status, otherwise end status; report/close
 *     statuses are ignored.
 */
int runDeck(const fs::path& inp, const fs::path& hs_file = {}) {
    SWMM_Engine e = swmm_engine_create();
    fs::path rpt = inp, out = inp;
    rpt.replace_extension(".rpt");
    out.replace_extension(".out");
    int rc = swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(), out.string().c_str(),
                              nullptr);
    if (rc == SWMM_OK)
        rc = swmm_engine_initialize(e);
    if (rc == SWMM_OK && !hs_file.empty()) {
        SWMM_HotStart hs = nullptr;
        rc = swmm_hotstart_open(hs_file.string().c_str(), &hs);
        if (rc == SWMM_OK)
            rc = swmm_hotstart_apply(e, hs);
        swmm_hotstart_close(hs);
    }
    if (rc == SWMM_OK) {
        rc = swmm_engine_start(e, 1);
        if (rc == SWMM_OK) {
            double t = 0.0;
            int step = SWMM_OK;
            while ((step = swmm_engine_step(e, &t)) == SWMM_OK && t > 0.0) {
            }
            if (step != SWMM_OK)
                rc = step;
            const int end = swmm_engine_end(e);
            if (rc == SWMM_OK)
                rc = end;
            swmm_engine_report(e);
        }
    }
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return rc;
}

/**
 * @brief Wrap a filesystem path in double quotes for raw model authoring.
 *
 * Does not escape embedded quotes, newlines or other input-grammar delimiters; caller must supply a
 * safe path.
 *
 * @param[in] p Borrowed path whose native string form is copied.
 * @return Owned quoted string.
 */
std::string q(const fs::path& p) {
    return "\"" + p.string() + "\"";
}
} // namespace

TEST(HotstartFormatRunTest, SaveRowsWriteTheFormatTheyName) {
    const fs::path dir = fs::absolute("hotstart_format");
    fs::create_directories(dir);
    const fs::path a = dir / "save_auto.hsf";
    const fs::path l = dir / "save_legacy.hsf";
    const fs::path n = dir / "save_native.hsf";
    for (const auto& p : {a, l, n})
        fs::remove(p);

    ASSERT_EQ(runDeck(deckWithFiles(dir, "save_formats",
                                    "SAVE HOTSTART " + q(a) + "\n" + "SAVE HOTSTART " + q(l) +
                                        " LEGACY\n" + "SAVE HOTSTART " + q(n) + " NATIVE\n",
                                    true)),
              SWMM_OK);

    const std::string fa = slurpBinary(a), fl = slurpBinary(l), fn = slurpBinary(n);
    ASSERT_FALSE(fa.empty());
    ASSERT_FALSE(fn.empty());
    EXPECT_EQ(fa.rfind("SWMM5-HOTSTART", 0), 0u) << "AUTO without LIDs must stay legacy";
    EXPECT_EQ(fa, fl) << "AUTO and LEGACY must be the same legacy file";
    EXPECT_EQ(fn.rfind("OPENSWMM_HS_V1", 0), 0u);
}

TEST(HotstartFormatRunTest, UseHotstartReadsANativeFileExactlyAsTheCApiDoes) {
    const fs::path dir = fs::absolute("hotstart_format");
    fs::create_directories(dir);
    const fs::path l = dir / "restart_legacy.hsf";
    const fs::path n = dir / "restart_native.hsf";
    fs::remove(l);
    fs::remove(n);
    ASSERT_EQ(runDeck(deckWithFiles(
                  dir, "restart_save",
                  "SAVE HOTSTART " + q(l) + "\n" + "SAVE HOTSTART " + q(n) + " NATIVE\n", true)),
              SWMM_OK);

    // The [FILES] row must equal swmm_hotstart_open + _apply right after
    // initialize(): the reference this path is defined against.
    const fs::path via_files =
        deckWithFiles(dir, "restart_native_files", "USE HOTSTART " + q(n) + "\n");
    const fs::path via_capi = deckWithFiles(dir, "restart_native_capi", "");
    const fs::path cold = deckWithFiles(dir, "restart_cold", "");
    ASSERT_EQ(runDeck(via_files), SWMM_OK);
    ASSERT_EQ(runDeck(via_capi, n), SWMM_OK);
    ASSERT_EQ(runDeck(cold), SWMM_OK);
    auto out = [](fs::path p) { return slurpBinary(p.replace_extension(".out")); };
    ASSERT_FALSE(out(via_files).empty());
    EXPECT_EQ(out(via_files), out(via_capi))
        << "[FILES] USE HOTSTART of a native file differs from the C API restart";
    EXPECT_NE(out(via_files), out(cold)) << "the native restart was not applied";
    // The legacy file `l` is restarted in
    // LegacySaveOfAModelWithSubcatchmentsRestarts below.
    (void)l;
}

namespace {
// `base` (a deck in data/) plus a [FILES] section, written to `dir`.
/**
 * @brief Write caller deck text and an appended FILES section.
 *
 * Does not create parent directories, escape content or parse the file; path/allocation exceptions
 * can propagate.
 *
 * @param[in] base Borrowed complete base deck text.
 * @param[in] dir Existing destination directory.
 * @param[in] name Raw destination stem for an overwritten .inp file.
 * @param[in] files Raw FILES section body.
 * @return Absolute destination input path, even after an unchecked write failure.
 */
fs::path deckFrom(const std::string& base, const fs::path& dir, const std::string& name,
                  const std::string& files) {
    const fs::path inp = fs::absolute(dir / (name + ".inp"));
    std::ofstream(inp) << base << "\n[FILES]\n" << files;
    return inp;
}

// The run through the C++ engine, keeping its warnings.
/**
 * @brief Run the C++ fixture engine and copy its warnings before close.
 *
 * Local engine owns cleanup. Steps without a guard while elapsed is positive, ends after successful
 * start even on step rejection, and keeps warning storage independent of the engine lifetime.
 *
 * @param[in] inp Existing input path; matching report/result destinations may be overwritten.
 * @param[out] warnings Writable vector replaced by a copy of the model warnings before close,
 *     including partial lifecycle warnings.
 * @return First open/init/start/step error or, when those succeed, end status; report/close status
 *     is ignored.
 */
int runEngine(const fs::path& inp, std::vector<std::string>& warnings) {
    openswmm::SWMMEngine e;
    fs::path rpt = inp, out = inp;
    rpt.replace_extension(".rpt");
    out.replace_extension(".out");
    int rc = e.open(inp.string().c_str(), rpt.string().c_str(), out.string().c_str());
    if (rc == SWMM_OK)
        rc = e.initialize();
    if (rc == SWMM_OK)
        rc = e.start(1);
    if (rc == SWMM_OK) {
        double t = 0.0;
        int step = SWMM_OK;
        while ((step = e.step(&t)) == SWMM_OK && t > 0.0) {
        }
        if (step != SWMM_OK)
            rc = step;
        const int end = e.end();
        if (rc == SWMM_OK)
            rc = end;
        e.report();
    }
    warnings = e.context().warnings;
    e.close();
    return rc;
}

// The run through the in-repo legacy SWMM 5.3 engine.
/**
 * @brief Run the in-repository process-global legacy engine.
 *
 * Uses legacy global state, so callers must serialize legacy invocations. Lifecycle and cleanup
 * follow swmm_run; no new-engine handle is involved.
 *
 * @param[in] inp Existing input path; derived .rpt/.out files may be overwritten.
 * @return swmm_run status unchanged.
 */
int runSwmm53(const fs::path& inp) {
    fs::path rpt = inp, out = inp;
    rpt.replace_extension(".rpt");
    out.replace_extension(".out");
    return swmm_run(inp.string().c_str(), rpt.string().c_str(), out.string().c_str());
}

/**
 * @brief Find a warning message containing an expected text fragment.
 *
 * @param[in] w Borrowed warning messages.
 * @param[in] part Borrowed case-sensitive substring; empty matches an existing message.
 * @return True on the first matching warning, otherwise false.
 */
bool hasWarning(const std::vector<std::string>& w, const std::string& part) {
    for (const auto& m : w)
        if (m.find(part) != std::string::npos)
            return true;
    return false;
}

/**
 * @brief Replace the first required text fragment for a fixture variant.
 *
 * A missing fragment triggers a fatal GoogleTest assertion and leaves text unchanged. Only the
 * first occurrence is replaced; allocation exceptions propagate.
 *
 * @param[in,out] text Owned mutable deck text changed only after a match.
 * @param[in] from Borrowed exact fragment; empty matches at the beginning.
 * @param[in] to Borrowed replacement copied into text.
 */
void replaceOnce(std::string& text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    ASSERT_NE(at, std::string::npos) << from;
    text.replace(at, from.size(), to);
}
} // namespace

// What a legacy file has no place for, as a front end asks before a run to
// suggest NATIVE for a new row. [POLLUTANTS] alone is in both formats.
TEST(HotstartFormatRunTest, LegacyOmitsNamesTheStateOnlyNativeKeeps) {
    auto omits = [](SWMM_Engine e) {
        int f = -1;
        EXPECT_EQ(swmm_hotstart_legacy_omits(e, &f), SWMM_OK);
        return f;
    };
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, "site_drainage_model.inp", nullptr, nullptr, nullptr), SWMM_OK);
    EXPECT_EQ(omits(e), 0) << "a 1D model with pollutants needs nothing native";
    EXPECT_EQ(swmm_hotstart_legacy_omits(e, nullptr), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_options_set(e, "WATER_AGE", "YES"), SWMM_OK);
    EXPECT_EQ(omits(e), SWMM_HOTSTART_OMITS_WATER_AGE);
    ASSERT_EQ(swmm_reaction_species_add(e, "HOCL", 0, "MG", 0.0, 0.0), SWMM_OK);
    EXPECT_EQ(omits(e), SWMM_HOTSTART_OMITS_WATER_AGE | SWMM_HOTSTART_OMITS_REACTION_SPECIES);
#ifdef OPENSWMM_HAS_2D
    ASSERT_EQ(swmm_gw2d_row_add(e, SWMM_GW2D_SCOPE_GLOBAL, nullptr, -1, 36.0, 4.0, 0.45, 0.10, 2.0),
              SWMM_OK);
    EXPECT_EQ(omits(e) & SWMM_HOTSTART_OMITS_AQUIFER, SWMM_HOTSTART_OMITS_AQUIFER);
#endif
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    SWMM_Engine lid = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(lid, "lid_partition_free.inp", nullptr, nullptr, nullptr), SWMM_OK);
    EXPECT_EQ(omits(lid), SWMM_HOTSTART_OMITS_NODE_LIDS);
    swmm_engine_close(lid);
    swmm_engine_destroy(lid);
}

// Until 2026-10-18 the engine's legacy SAVE wrote a v4 header that counted
// the subcatchments but no subcatchment section, so USE HOTSTART refused the
// engine's own file on any model with subcatchments.
TEST(HotstartFormatRunTest, LegacySaveOfAModelWithSubcatchmentsRestarts) {
    const fs::path dir = fs::absolute("hotstart_format");
    fs::create_directories(dir);
    const fs::path l = dir / "legacy_restart.hsf";
    fs::remove(l);
    ASSERT_EQ(
        runDeck(deckWithFiles(dir, "legacy_restart_save", "SAVE HOTSTART " + q(l) + "\n", true)),
        SWMM_OK);

    const fs::path use = deckWithFiles(dir, "legacy_restart_use", "USE HOTSTART " + q(l) + "\n");
    const fs::path cold = deckWithFiles(dir, "legacy_restart_cold", "");
    ASSERT_EQ(runDeck(use), SWMM_OK) << "the engine refused its own legacy file";
    ASSERT_EQ(runDeck(cold), SWMM_OK);
    auto out = [](fs::path p) { return slurpBinary(p.replace_extension(".out")); };
    ASSERT_FALSE(out(use).empty());
    EXPECT_NE(out(use), out(cold)) << "the legacy restart was not applied";

    // The reference engine reads it as well.
    EXPECT_EQ(
        runSwmm53(deckWithFiles(dir, "legacy_restart_use_swmm53", "USE HOTSTART " + q(l) + "\n")),
        0);
}

// The engine's legacy file against the one legacy SWMM 5.3 writes for the
// same run, field by field: every part of the subcatchment record (ponded
// depths, runoff, each infiltration model's state, groundwater, snowpack,
// quality, buildup per area and per curb, last swept) and the routing state.
TEST(HotstartFormatRunTest, LegacySaveMatchesTheFileSwmm53Writes) {
    const fs::path dir = fs::absolute("hotstart_format");
    fs::create_directories(dir);
    const std::string base = slurpFile("hotstart_legacy_parity.inp");
    ASSERT_FALSE(base.empty());
    const std::pair<std::string, std::string> models[] = {
        {"HORTON", "3.0  0.5  4  7  0"},
        {"GREEN_AMPT", "3.5  0.5  0.25"},
        {"CURVE_NUMBER", "80  0.5  7"},
    };
    for (const auto& [model, row] : models) {
        SCOPED_TRACE(model);
        std::string text = base;
        replaceOnce(text, "INFILTRATION         HORTON", "INFILTRATION         " + model);
        for (const char* sub : {"S1", "S2", "S3"})
            replaceOnce(text, std::string(sub) + "          3.0  0.5  4  7  0",
                        std::string(sub) + "          " + row);
        const fs::path ours = dir / ("parity_" + model + "_engine.hsf");
        const fs::path ref = dir / ("parity_" + model + "_swmm53.hsf");
        fs::remove(ours);
        fs::remove(ref);
        ASSERT_EQ(runDeck(deckFrom(text, dir, "parity_" + model + "_engine",
                                   "SAVE HOTSTART " + q(ours) + "\n")),
                  SWMM_OK);
        ASSERT_EQ(runSwmm53(deckFrom(text, dir, "parity_" + model + "_swmm53",
                                     "SAVE HOTSTART " + q(ref) + "\n")),
                  0);

        const std::string a = slurpBinary(ours), b = slurpBinary(ref);
        ASSERT_FALSE(b.empty());
        ASSERT_EQ(a.size(), b.size());
        EXPECT_EQ(a.substr(0, 39), b.substr(0, 39)) << "stamp and object counts";
        // S1 and S3 carry a snowpack (35 doubles), S2 groundwater (24).
        std::size_t off = 39;
        for (const std::size_t n : {35u, 24u, 35u}) {
            for (std::size_t k = 0; k < n; ++k, off += sizeof(double)) {
                double x = 0.0, y = 0.0;
                std::memcpy(&x, a.data() + off, sizeof x);
                std::memcpy(&y, b.data() + off, sizeof y);
                EXPECT_NEAR(x, y, 1e-9 * std::max(1.0, std::fabs(y))) << "double at byte " << off;
            }
        }
        for (; off + sizeof(float) <= a.size(); off += sizeof(float)) {
            float x = 0.0f, y = 0.0f;
            std::memcpy(&x, a.data() + off, sizeof x);
            std::memcpy(&y, b.data() + off, sizeof y);
            EXPECT_NEAR(x, y, 1e-6 * std::max(1.0f, std::fabs(y))) << "float at byte " << off;
        }
    }
}

// Storage-node LID models: a legacy file carries no LID state, so a LEGACY
// save and a legacy USE both run with a warning — they used to fail. AUTO
// stays native, which carries the LID state, and warns about nothing.
TEST(HotstartFormatRunTest, StorageNodeLidModelTakesLegacyFilesWithAWarning) {
    const fs::path dir = fs::absolute("hotstart_format");
    fs::create_directories(dir);
    const std::string base = slurpFile("lid_partition_free.inp");
    ASSERT_FALSE(base.empty());
    const fs::path a = dir / "lid_auto.hsf";
    const fs::path l = dir / "lid_legacy.hsf";
    fs::remove(a);
    fs::remove(l);

    std::vector<std::string> w;
    ASSERT_EQ(runEngine(deckFrom(base, dir, "lid_save",
                                 "SAVE HOTSTART " + q(a) + "\nSAVE HOTSTART " + q(l) + " LEGACY\n"),
                        w),
              SWMM_OK);
    EXPECT_TRUE(HotStartManager::is_native_file(a.string())) << "AUTO with node LIDs is native";
    EXPECT_EQ(slurpBinary(l).rfind("SWMM5-HOTSTART4", 0), 0u);
    EXPECT_TRUE(
        hasWarning(w, "lid_legacy.hsf' is LEGACY; a legacy SWMM5 file carries no LID state"));
    EXPECT_FALSE(hasWarning(w, "lid_auto.hsf' is LEGACY"));

    ASSERT_EQ(runEngine(deckFrom(base, dir, "lid_use_legacy", "USE HOTSTART " + q(l) + "\n"), w),
              SWMM_OK);
    EXPECT_TRUE(hasWarning(w, "is a legacy SWMM5 file, which carries no LID state; "
                              "LID units start from their initial conditions."));

    ASSERT_EQ(runEngine(deckFrom(base, dir, "lid_use_native", "USE HOTSTART " + q(a) + "\n"), w),
              SWMM_OK);
    EXPECT_FALSE(hasWarning(w, "carries no LID state"));

    // With node LIDs too, the [FILES] row equals swmm_hotstart_open + _apply
    // right after initialize(): applied before init_modules() it lost what
    // the modules built afterwards.
    const std::string threaded = "THREADS 1\n";
    std::string single = base;
    replaceOnce(single, "[OPTIONS]\n", "[OPTIONS]\n" + threaded);
    const fs::path via_files =
        deckFrom(single, dir, "lid_native_files", "USE HOTSTART " + q(a) + "\n");
    const fs::path via_capi = deckFrom(single, dir, "lid_native_capi", "");
    const fs::path cold = deckFrom(single, dir, "lid_native_cold", "");
    ASSERT_EQ(runDeck(via_files), SWMM_OK);
    ASSERT_EQ(runDeck(via_capi, a), SWMM_OK);
    ASSERT_EQ(runDeck(cold), SWMM_OK);
    auto out = [](fs::path p) { return slurpBinary(p.replace_extension(".out")); };
    ASSERT_FALSE(out(via_files).empty());
    EXPECT_EQ(out(via_files), out(via_capi))
        << "[FILES] USE HOTSTART of a native file differs from the C API restart";
    EXPECT_NE(out(via_files), out(cold)) << "the native restart was not applied";
}

// A dated SAVE HOTSTART row is written from the state at the end of the first
// step that reaches its time. With a fixed routing step a step lands on 06:00,
// so the dated file of a 12 h run equals the end-of-run file of a run stopped
// at 06:00. A row the run never reaches is reported and not written; one
// dated before the start is written at the first step.
TEST(HotstartFormatRunTest, DatedSaveHoldsTheStateAtItsTime) {
    const fs::path dir = fs::absolute("hotstart_format");
    fs::create_directories(dir);
    std::string base = slurpFile("hotstart_legacy_parity.inp");
    ASSERT_FALSE(base.empty());
    replaceOnce(base, "THREADS              1", "THREADS              1\nVARIABLE_STEP        0");
    std::string stop6 = base;
    replaceOnce(stop6, "END_TIME             12:00:00", "END_TIME             06:00:00");

    const fs::path dated = dir / "dated_0600.hsf", dated_n = dir / "dated_0600_native.hsf";
    const fs::path early = dir / "dated_before_start.hsf", late = dir / "dated_after_end.hsf";
    const fs::path end6 = dir / "end_0600.hsf", end6_n = dir / "end_0600_native.hsf";
    for (const auto& p : {dated, dated_n, early, late, end6, end6_n})
        fs::remove(p);

    std::vector<std::string> w;
    ASSERT_EQ(
        runEngine(deckFrom(base, dir, "dated_full",
                           "SAVE HOTSTART " + q(dated) + " 01/01/2020 06:00:00\n" +
                               "SAVE HOTSTART " + q(dated_n) + " 01/01/2020 06:00:00 NATIVE\n" +
                               "SAVE HOTSTART " + q(early) + " 12/31/2019 00:00:00\n" +
                               "SAVE HOTSTART " + q(late) + " 01/02/2020 00:00:00\n"),
                  w),
        SWMM_OK);
    std::vector<std::string> w6;
    ASSERT_EQ(runEngine(deckFrom(stop6, dir, "dated_stop_0600",
                                 "SAVE HOTSTART " + q(end6) + "\n" + "SAVE HOTSTART " + q(end6_n) +
                                     " NATIVE\n"),
                        w6),
              SWMM_OK);

    const std::string fd = slurpBinary(dated);
    ASSERT_FALSE(fd.empty());
    EXPECT_EQ(fd, slurpBinary(end6)) << "the dated legacy file is not the 06:00 state";

    std::unique_ptr<openswmm::HotStartFile> n1(HotStartManager::open(dated_n.string()));
    std::unique_ptr<openswmm::HotStartFile> n2(HotStartManager::open(end6_n.string()));
    ASSERT_TRUE(n1 && n2) << HotStartManager::last_io_error();
    EXPECT_DOUBLE_EQ(n1->header.sim_time, n2->header.sim_time);
    ASSERT_EQ(n1->nodes.size(), n2->nodes.size());
    for (std::size_t i = 0; i < n1->nodes.size(); ++i) {
        EXPECT_EQ(n1->nodes[i].depth, n2->nodes[i].depth) << i;
        EXPECT_EQ(n1->nodes[i].volume, n2->nodes[i].volume) << i;
    }
    ASSERT_EQ(n1->links.size(), n2->links.size());
    for (std::size_t i = 0; i < n1->links.size(); ++i)
        EXPECT_EQ(n1->links[i].flow, n2->links[i].flow) << i;
    ASSERT_EQ(n1->subcatches.size(), n2->subcatches.size());
    for (std::size_t i = 0; i < n1->subcatches.size(); ++i)
        EXPECT_EQ(n1->subcatches[i].runoff, n2->subcatches[i].runoff) << i;
    EXPECT_EQ(n1->node_species, n2->node_species);
    EXPECT_EQ(n1->snow_state, n2->snow_state);

    EXPECT_FALSE(slurpBinary(early).empty()) << "a time before the start is written at once";
    EXPECT_NE(slurpBinary(early), fd);
    EXPECT_FALSE(fs::exists(late));
    EXPECT_TRUE(hasWarning(w, "dated_after_end.hsf' was not written; the run ended "
                              "before its save time 01/02/2020 00:00:00."));
    EXPECT_FALSE(hasWarning(w, "dated_0600.hsf' was not written"));
}
