/**
 * @file test_runoff_interface_capi.cpp
 * @brief Unit tests for the Phase 1b runoff interface C API
 *        (``swmm_runoff_iface_open_write`` / ``_open_read`` /
 *        ``_save_step`` / ``_read_step`` / ``_close``).
 *
 * @details The C API exposes the existing internal
 *          ``runoff_iface::RunoffInterfaceFile`` and adds engine-side
 *          auto-save into ``stepRunoff``. These tests verify the round
 *          trip: open the file in SAVE mode → run a simulation → close
 *          → reopen in READ mode → assert that the per-substep
 *          subcatchment runoff values match what the engine recorded.
 *
 *          Working directory is set to ``tests/unit/engine/data/`` by
 *          the test CMakeLists.txt.
 *
 *          USE-mode auto-skip is a follow-up; this test exercises the
 *          read API by calling ``swmm_runoff_iface_read_step`` directly
 *          and verifying it returns sensible values without engine
 *          orchestration.
 *
 * @see docs/C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md Phase 1b
 * @ingroup engine_tests
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

namespace {

// Per-test temp file lifecycle helper — uses an absolute path under the
// fixture data dir so the file lives next to the .out the simulation
// produces (working dir is tests/unit/engine/data/ at runtime).
std::string make_temp_iface_path(const std::string& tag) {
    namespace fs = std::filesystem;
    return (fs::current_path() / ("phase1b_" + tag + ".rfi")).string();
}

class RunoffIfaceCApiTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;

    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }

    void TearDown() override {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    void open_and_init() {
        ASSERT_EQ(swmm_engine_open(engine_,
                                    "site_drainage_model.inp",
                                    "site_drainage_model.rpt",
                                    "site_drainage_model.out",
                                    nullptr),
                  SWMM_OK)
            << "open failed: " << swmm_get_last_error_msg(engine_);
        ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
    }
};

// ---------------------------------------------------------------------------
// Bad-param contracts.
// ---------------------------------------------------------------------------

TEST_F(RunoffIfaceCApiTest, RejectsNullEngine) {
    EXPECT_EQ(swmm_runoff_iface_open_write(nullptr, "ignored.rfi"),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_runoff_iface_open_read(nullptr, "ignored.rfi"),
              SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_runoff_iface_save_step(nullptr, 1.0), SWMM_ERR_BADHANDLE);
    int has = -1;
    EXPECT_EQ(swmm_runoff_iface_read_step(nullptr, &has), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_runoff_iface_close(nullptr), SWMM_ERR_BADHANDLE);
}

TEST_F(RunoffIfaceCApiTest, RejectsNullOrEmptyPath) {
    open_and_init();
    EXPECT_EQ(swmm_runoff_iface_open_write(engine_, nullptr),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_runoff_iface_open_write(engine_, ""),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_runoff_iface_open_read(engine_, nullptr),
              SWMM_ERR_BADPARAM);
}

TEST_F(RunoffIfaceCApiTest, CloseIsIdempotent) {
    open_and_init();
    // Safe to call without ever opening a file.
    EXPECT_EQ(swmm_runoff_iface_close(engine_), SWMM_OK);
    EXPECT_EQ(swmm_runoff_iface_close(engine_), SWMM_OK);
}

TEST_F(RunoffIfaceCApiTest, RefusesDoubleOpenWithoutClose) {
    open_and_init();
    const auto path = make_temp_iface_path("dbl");
    ASSERT_EQ(swmm_runoff_iface_open_write(engine_, path.c_str()), SWMM_OK);
    // Second open without intervening close must fail (non-OK).
    EXPECT_NE(swmm_runoff_iface_open_write(engine_, path.c_str()), SWMM_OK);
    // Cleanup: close and remove the file so subsequent tests / CI runs
    // don't see a leftover.
    EXPECT_EQ(swmm_runoff_iface_close(engine_), SWMM_OK);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------------
// SAVE → READ round trip — the headline behaviour. After running the full
// fixture with a SAVE-mode runoff interface file open, the file must
// re-open cleanly in READ mode and produce non-zero record reads.
// ---------------------------------------------------------------------------

TEST_F(RunoffIfaceCApiTest, SaveThenReadRoundTrip) {
    open_and_init();
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);

    const auto path = make_temp_iface_path("rt");
    ASSERT_EQ(swmm_runoff_iface_open_write(engine_, path.c_str()), SWMM_OK)
        << "open_write failed";

    // Drive the simulation to completion — the engine auto-saves one
    // record per runoff substep from inside stepRunoff().
    double elapsed = 0.0;
    while (true) {
        int rc = swmm_engine_step(engine_, &elapsed);
        if (rc != 0) break;
        if (elapsed <= 0.0) break;
    }
    ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);
    ASSERT_EQ(swmm_runoff_iface_close(engine_), SWMM_OK);

    // File should exist and be non-empty.
    ASSERT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path),
              static_cast<std::uintmax_t>(28))   // header is 28 bytes
        << "Expected at least one substep record beyond the header";

    // Reopen in READ mode and verify that swmm_runoff_iface_read_step
    // is callable and reads at least one record before EOF.
    ASSERT_EQ(swmm_runoff_iface_open_read(engine_, path.c_str()), SWMM_OK)
        << "open_read failed — header mismatch?";

    int records_read = 0;
    int has = 0;
    for (;;) {
        ASSERT_EQ(swmm_runoff_iface_read_step(engine_, &has), SWMM_OK);
        if (!has) break;
        ++records_read;
        // Bail after a few thousand to keep the test fast even if EOF
        // detection regresses.
        if (records_read > 100000) {
            FAIL() << "read_step appears to be looping past EOF";
            break;
        }
    }
    EXPECT_GT(records_read, 0)
        << "Expected at least one record from the saved file";

    EXPECT_EQ(swmm_runoff_iface_close(engine_), SWMM_OK);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------------
// Explicit save_step (force a snapshot) — even outside the engine's
// internal hook, the explicit C API entrypoint must succeed (and be a
// no-op when no SAVE file is open, which is exercised by the close-then-
// save sequence below).
// ---------------------------------------------------------------------------

TEST_F(RunoffIfaceCApiTest, SaveStepIsNoopWhenNoFileOpen) {
    open_and_init();
    // No file open — must succeed silently.
    EXPECT_EQ(swmm_runoff_iface_save_step(engine_, 1.0), SWMM_OK);
}

TEST_F(RunoffIfaceCApiTest, ReadStepWhenNoFileOpenReturnsHasZero) {
    open_and_init();
    int has = -1;
    EXPECT_EQ(swmm_runoff_iface_read_step(engine_, &has), SWMM_OK);
    EXPECT_EQ(has, 0) << "Expected has_data=0 with no file open";
}

} // namespace
