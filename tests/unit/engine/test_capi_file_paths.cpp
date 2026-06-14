/**
 * @file test_capi_file_paths.cpp
 * @brief Slice IO-9 — typed external-file slot C-API.
 *
 * @details Pins the contract of `swmm_file_path_get` /
 *          `swmm_file_path_set` documented in
 *          include/openswmm/engine/openswmm_model.h. Covers every role
 *          enum value across both scalar and vector slots:
 *
 *            - Scalar get/set round-trip + .absolute cleared on set.
 *            - Vector slot dispatch by owner key (gage id, series id,
 *              hot-start save index).
 *            - Missing owner returns SWMM_ERR_BADPARAM.
 *            - Buffer truncation at buflen-1.
 *            - Unknown role returns SWMM_ERR_BADPARAM.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_gages.h>
#include <openswmm/engine/openswmm_hotstart.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>

#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/core/SWMMEngine.hpp"

namespace {

class CapiFilePathsTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;
    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        // Plant a minimal model so vector slots have owners to address.
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "O1", SWMM_NODE_OUTFALL), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "L1", SWMM_LINK_CONDUIT), SWMM_OK);
        ASSERT_EQ(swmm_link_set_nodes(
                    engine,
                    swmm_link_index(engine, "L1"),
                    swmm_node_index(engine, "J1"),
                    swmm_node_index(engine, "O1")), SWMM_OK);
        ASSERT_EQ(swmm_gage_add(engine, "G1"), SWMM_OK);
    }
    void TearDown() override { if (engine) swmm_engine_destroy(engine); }
};

} // namespace

// ---------------------------------------------------------------------------
// Scalar slot — set/get round-trip
// ---------------------------------------------------------------------------

TEST_F(CapiFilePathsTest, ScalarRainfallRoundTrip) {
    char abs_buf[128] = {};
    char orig_buf[128] = {};

    // Initially empty.
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_RAINFALL, nullptr,
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)), SWMM_OK);
    EXPECT_STREQ(abs_buf, "");
    EXPECT_STREQ(orig_buf, "");

    // Set populates .original.
    EXPECT_EQ(swmm_file_path_set(engine, SWMM_FILE_RAINFALL, nullptr,
                                   "rain.dat"), SWMM_OK);
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_RAINFALL, nullptr,
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)), SWMM_OK);
    EXPECT_STREQ(orig_buf, "rain.dat");
    // .absolute is empty until PostParseResolver runs.
    EXPECT_STREQ(abs_buf, "");
}

TEST_F(CapiFilePathsTest, SetClearsCachedAbsolute) {
    // Poke into the engine context directly to plant a stale .absolute,
    // then confirm that set() clears it.
    auto* h = engine;
    static_cast<openswmm::SWMMEngine*>(h)->context()
        .files.rainfall_path.absolute = "/tmp/stale.dat";

    EXPECT_EQ(swmm_file_path_set(engine, SWMM_FILE_RAINFALL, nullptr,
                                   "fresh.dat"), SWMM_OK);

    char abs_buf[64] = {}, orig_buf[64] = {};
    swmm_file_path_get(engine, SWMM_FILE_RAINFALL, nullptr,
                        abs_buf, sizeof(abs_buf), orig_buf, sizeof(orig_buf));
    EXPECT_STREQ(orig_buf, "fresh.dat");
    EXPECT_STREQ(abs_buf, "")
        << "set() must clear the cached absolute resolution";
}

// ---------------------------------------------------------------------------
// Every scalar role accepts get/set
// ---------------------------------------------------------------------------

TEST_F(CapiFilePathsTest, EveryScalarRoleSetGetRoundTrip) {
    const SWMM_FilePathRole roles[] = {
        SWMM_FILE_RAINFALL, SWMM_FILE_RUNOFF, SWMM_FILE_RDII,
        SWMM_FILE_INFLOWS,  SWMM_FILE_OUTFLOWS, SWMM_FILE_HOTSTART_USE,
        SWMM_FILE_CLIMATE_TEMP
    };
    int probe = 0;
    for (auto role : roles) {
        std::string val = "value_" + std::to_string(probe++);
        EXPECT_EQ(swmm_file_path_set(engine, role, nullptr, val.c_str()),
                   SWMM_OK) << "set failed for role=" << static_cast<int>(role);
        char abs_buf[64] = {}, orig_buf[64] = {};
        EXPECT_EQ(swmm_file_path_get(engine, role, nullptr,
                                       abs_buf, sizeof(abs_buf),
                                       orig_buf, sizeof(orig_buf)), SWMM_OK);
        EXPECT_STREQ(orig_buf, val.c_str());
    }
}

// ---------------------------------------------------------------------------
// Vector slot — RAINGAGE_DATA by gage id
// ---------------------------------------------------------------------------

TEST_F(CapiFilePathsTest, RaingageDataVectorSlotByGageId) {
    // First need a non-empty gage file_path vector. Plant via direct access
    // since openswmm_gages public API doesn't expose file_path[].
    auto* h = engine;
    auto& gages = static_cast<openswmm::SWMMEngine*>(h)->context().gages;
    gages.file_path.resize(1);   // matches the single gage "G1" planted

    EXPECT_EQ(swmm_file_path_set(engine, SWMM_FILE_RAINGAGE_DATA, "G1",
                                   "g1.dat"), SWMM_OK);

    char abs_buf[64] = {}, orig_buf[64] = {};
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_RAINGAGE_DATA, "G1",
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)), SWMM_OK);
    EXPECT_STREQ(orig_buf, "g1.dat");

    // Unknown owner → BADPARAM.
    EXPECT_EQ(swmm_file_path_set(engine, SWMM_FILE_RAINGAGE_DATA, "G_ghost",
                                   "x.dat"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_RAINGAGE_DATA, "G_ghost",
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)),
              SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// Vector slot — TIMESERIES_DATA by series id
// ---------------------------------------------------------------------------

TEST_F(CapiFilePathsTest, TimeseriesDataVectorSlotBySeriesId) {
    auto* h = engine;
    auto& ctx = static_cast<openswmm::SWMMEngine*>(h)->context();
    int t = ctx.table_names.add("RAIN_X");
    ctx.tables.add("RAIN_X", openswmm::TableType::TIMESERIES);
    ASSERT_GE(t, 0);

    EXPECT_EQ(swmm_file_path_set(engine, SWMM_FILE_TIMESERIES_DATA, "RAIN_X",
                                   "rain.csv:East"), SWMM_OK);
    char abs_buf[64] = {}, orig_buf[64] = {};
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_TIMESERIES_DATA, "RAIN_X",
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)), SWMM_OK);
    EXPECT_STREQ(orig_buf, "rain.csv:East");

    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_TIMESERIES_DATA, "GHOST",
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)),
              SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// Vector slot — HOTSTART_SAVE by decimal index
// ---------------------------------------------------------------------------

TEST_F(CapiFilePathsTest, HotstartSaveVectorSlotByIndex) {
    // Plant a hot-start save row at index 0 via the existing C-API.
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "save0.hsf", 0.0), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_saves_add(engine, "save1.hsf", 0.0), SWMM_OK);

    char abs_buf[64] = {}, orig_buf[64] = {};
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_HOTSTART_SAVE, "0",
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)), SWMM_OK);
    EXPECT_STREQ(orig_buf, "save0.hsf");

    EXPECT_EQ(swmm_file_path_set(engine, SWMM_FILE_HOTSTART_SAVE, "1",
                                   "save1_renamed.hsf"), SWMM_OK);
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_HOTSTART_SAVE, "1",
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)), SWMM_OK);
    EXPECT_STREQ(orig_buf, "save1_renamed.hsf");

    // Out-of-range index → BADPARAM.
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_HOTSTART_SAVE, "99",
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)),
              SWMM_ERR_BADPARAM);
    // Non-decimal owner → BADPARAM.
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_HOTSTART_SAVE, "abc",
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)),
              SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST_F(CapiFilePathsTest, UnknownRoleReturnsBadParam) {
    char abs_buf[16] = {}, orig_buf[16] = {};
    auto bogus = static_cast<SWMM_FilePathRole>(9999);
    EXPECT_EQ(swmm_file_path_get(engine, bogus, nullptr,
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_file_path_set(engine, bogus, nullptr, "x"),
              SWMM_ERR_BADPARAM);
}

TEST_F(CapiFilePathsTest, NullBufRejected) {
    char abs_buf[16] = {}, orig_buf[16] = {};
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_RAINFALL, nullptr,
                                   nullptr, 16, orig_buf, 16),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_RAINFALL, nullptr,
                                   abs_buf, 16, nullptr, 16),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_RAINFALL, nullptr,
                                   abs_buf, 0, orig_buf, 16),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_file_path_set(engine, SWMM_FILE_RAINFALL, nullptr, nullptr),
              SWMM_ERR_BADPARAM);
}

TEST_F(CapiFilePathsTest, BufferTruncatedAtBuflenMinusOne) {
    EXPECT_EQ(swmm_file_path_set(engine, SWMM_FILE_RAINFALL, nullptr,
                                   "verylong_rainfall_path.dat"), SWMM_OK);
    // The stored token lives in `.original` (set() clears `.absolute` until
    // PostParseResolver runs), so exercise truncation on the original buffer.
    char abs_buf[64] = {};
    char small[6] = {};   // room for 5 chars + NUL
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_RAINFALL, nullptr,
                                   abs_buf, sizeof(abs_buf),
                                   small, sizeof(small)), SWMM_OK);
    EXPECT_EQ(std::strlen(small), 5u);
    EXPECT_EQ(small[5], '\0');
}

TEST_F(CapiFilePathsTest, EmptyStringClearsSlot) {
    EXPECT_EQ(swmm_file_path_set(engine, SWMM_FILE_RUNOFF, nullptr,
                                   "x.dat"), SWMM_OK);
    EXPECT_EQ(swmm_file_path_set(engine, SWMM_FILE_RUNOFF, nullptr, ""),
              SWMM_OK);
    char abs_buf[16] = {}, orig_buf[16] = {};
    EXPECT_EQ(swmm_file_path_get(engine, SWMM_FILE_RUNOFF, nullptr,
                                   abs_buf, sizeof(abs_buf),
                                   orig_buf, sizeof(orig_buf)), SWMM_OK);
    EXPECT_STREQ(orig_buf, "");
}
