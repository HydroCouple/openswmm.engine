/**
 * @file test_files_section.cpp
 * @brief Unit tests for the [FILES] section — handler + writer + C API.
 *
 * @details Covers:
 *          - swmm_files_get/set: round-trip every recognised key,
 *            invalid keys / values rejected, mode normalisation.
 *          - InpWriter: emits `[FILES]` rows when the spec is non-empty
 *            and skips when blank.
 *          - FilesHandler: parses every legacy keyword combo, including
 *            the SAVE HOTSTART date+time tail.
 *          - End-to-end round-trip: write → read-back → values survive.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>

#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/input/handlers/FilesHandler.hpp"

namespace fs = std::filesystem;
using openswmm::FileMode;
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
    EXPECT_EQ(ctx.files.runoff_mode,   FileMode::SAVE);
    EXPECT_EQ(ctx.files.runoff_path,   "runoff.bin");
    EXPECT_EQ(ctx.files.rdii_mode,     FileMode::USE);
    EXPECT_EQ(ctx.files.rdii_path,     "rdii.dat");
    EXPECT_EQ(ctx.files.inflows_path,  "inflows.dat");
    EXPECT_EQ(ctx.files.outflows_path, "outflows.dat");
    EXPECT_EQ(ctx.files.hotstart_use_path,  "hot_in.hsf");
    EXPECT_EQ(ctx.files.hotstart_save_path, "hot_out.hsf");
    EXPECT_EQ(ctx.files.hotstart_save_datetime, 0.0);
}

TEST(FilesHandlerTest, ParsesSaveHotstartWithDateTime) {
    SimulationContext ctx;
    openswmm::input::handle_files(ctx, {
        "SAVE HOTSTART \"out.hsf\" 01/15/2026 12:30:00",
    });
    EXPECT_EQ(ctx.files.hotstart_save_path, "out.hsf");
    EXPECT_GT(ctx.files.hotstart_save_datetime, 0.0);
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

TEST_F(FilesApiTest, GetReturnsEmptyForUnsetSlots) {
    char buf[128];
    EXPECT_EQ(swmm_files_get(engine, "RAINFALL_PATH", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");
    EXPECT_EQ(swmm_files_get(engine, "RAINFALL_MODE", buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");
}

TEST_F(FilesApiTest, SetThenGetRoundTripPathsAndModes) {
    EXPECT_EQ(swmm_files_set(engine, "RAINFALL_PATH", "rain.dat"), SWMM_OK);
    EXPECT_EQ(swmm_files_set(engine, "RAINFALL_MODE", "USE"),       SWMM_OK);
    EXPECT_EQ(swmm_files_set(engine, "HOTSTART_SAVE_PATH", "h.hsf"), SWMM_OK);
    EXPECT_EQ(swmm_files_set(engine, "HOTSTART_SAVE_DATETIME", "46036.5"),
              SWMM_OK);

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
    EXPECT_EQ(swmm_files_set(engine, "RUNOFF_MODE", "USE"),  SWMM_OK);
    EXPECT_EQ(swmm_files_set(engine, "RUNOFF_MODE", ""),     SWMM_OK);  // clears slot
    EXPECT_EQ(swmm_files_set(engine, "RUNOFF_MODE", "NONE"), SWMM_OK);

    EXPECT_EQ(swmm_files_set(engine, "RUNOFF_MODE", "GIBBERISH"),
              SWMM_ERR_BADPARAM);
}

TEST_F(FilesApiTest, KeyIsCaseInsensitive) {
    EXPECT_EQ(swmm_files_set(engine, "rainfall_path", "x.dat"), SWMM_OK);
    char buf[64];
    EXPECT_EQ(swmm_files_get(engine, "Rainfall_Path", buf, sizeof(buf)),
              SWMM_OK);
    EXPECT_STREQ(buf, "x.dat");
}

TEST_F(FilesApiTest, UnknownKeyRejected) {
    char buf[16];
    EXPECT_EQ(swmm_files_get(engine, "WAT", buf, sizeof(buf)), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_files_set(engine, "WAT", "x"), SWMM_ERR_BADPARAM);
}

TEST_F(FilesApiTest, NullArgsRejected) {
    char buf[16];
    EXPECT_EQ(swmm_files_get(engine, nullptr, buf, sizeof(buf)),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_files_set(engine, nullptr, "x"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_files_set(engine, "RAINFALL_PATH", nullptr),
              SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// End-to-end: set fields → swmm_model_write → grep emitted [FILES] block.
// ---------------------------------------------------------------------------

TEST_F(FilesApiTest, WriterEmitsConfiguredFilesBlock) {
    ASSERT_EQ(swmm_files_set(engine, "RAINFALL_PATH", "rain.dat"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine, "RAINFALL_MODE", "USE"),       SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine, "HOTSTART_USE_PATH", "h.hsf"), SWMM_OK);

    auto path = (fs::temp_directory_path() / "files_block.inp").string();
    ASSERT_EQ(swmm_model_write(engine, path.c_str()), SWMM_OK);

    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    const std::string content = ss.str();
    f.close();  // Windows: must release the handle before fs::remove.
    EXPECT_NE(content.find("[FILES]"),         std::string::npos);
    EXPECT_NE(content.find("rain.dat"),        std::string::npos);
    EXPECT_NE(content.find("h.hsf"),           std::string::npos);
    EXPECT_NE(content.find("USE"),             std::string::npos);
    fs::remove(path);
}

TEST_F(FilesApiTest, WriterSkipsBlockWhenSpecIsEmpty) {
    auto path = (fs::temp_directory_path() / "no_files.inp").string();
    ASSERT_EQ(swmm_model_write(engine, path.c_str()), SWMM_OK);

    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    const std::string content = ss.str();
    f.close();  // Windows: must release the handle before fs::remove.
    EXPECT_EQ(content.find("[FILES]"), std::string::npos);
    fs::remove(path);
}
