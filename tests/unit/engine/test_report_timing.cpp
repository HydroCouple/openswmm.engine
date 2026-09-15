// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file test_report_timing.cpp
 * @brief Legacy parity for the .rpt "Analysis begun on / Total elapsed time"
 *        section.
 *
 * @details Legacy takes its wall-clock stamp in `report_writeLogo()`, which
 *          `swmm_open()` calls BEFORE `project_readInput()`, so the reported
 *          elapsed time covers parse + validation + initialization + routing.
 *          The modern engine previously stamped the clock in the report
 *          plugin's `prepare()` — the very end of `start()` — which excluded
 *          the entire initialization window. On a large model that window can
 *          be tens of minutes, so the .rpt understated the run by that much
 *          relative to PCSWMM.
 *
 *          Covered here:
 *            - `ctx.wall_start` is stamped inside `open()`, bracketed by
 *              wall-clock reads taken either side of the call.
 *            - `write_timing()` reads `ctx.wall_start` rather than a
 *              plugin-local clock, so initialization time reaches the report.
 *            - Elapsed formatting matches legacy across the sub-second, the
 *              hh:mm:ss and the >= 24 h `d.hh:mm:ss` branches.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"

namespace {

constexpr const char* kInp = "warnerr_base.inp";
constexpr const char* kRpt = "_report_timing.rpt";
constexpr const char* kOut = "_report_timing.out";

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/** @brief Returns the text after "Total elapsed time: " up to end of line. */
std::string elapsed_field(const std::string& rpt) {
    const std::string key = "Total elapsed time: ";
    const auto at = rpt.find(key);
    if (at == std::string::npos) return {};
    const auto from = at + key.size();
    const auto to   = rpt.find('\n', from);
    return rpt.substr(from, (to == std::string::npos ? rpt.size() : to) - from);
}

class ReportTimingTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }

    void TearDown() override {
        if (engine_ != nullptr) {
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    /** @brief Drives a complete open → … → report → close cycle. */
    void run_to_report() {
        ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);
        double elapsed_days = 0.0;
        int guard = 0;
        do {
            ASSERT_EQ(swmm_engine_step(engine_, &elapsed_days), SWMM_OK);
        } while (elapsed_days > 0.0 && ++guard < 100000);
        ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);
        ASSERT_EQ(swmm_engine_report(engine_), SWMM_OK);
    }

    SWMM_Engine engine_ = nullptr;
};

// ---------------------------------------------------------------------------
// The clock starts at open(), not at start()
// ---------------------------------------------------------------------------

TEST_F(ReportTimingTest, WallStartIsStampedDuringOpen) {
    std::time_t before = 0;
    std::time(&before);

    ASSERT_EQ(swmm_engine_open(engine_, kInp, kRpt, kOut, nullptr), SWMM_OK);

    std::time_t after = 0;
    std::time(&after);

    const std::time_t wall_start = as_cpp_engine(engine_).context().wall_start;

    // Stamped, and stamped inside the open() call — which is what puts parse,
    // cross-reference resolution, validation and module init inside the
    // reported elapsed time, matching legacy report_writeLogo().
    EXPECT_NE(wall_start, 0);
    EXPECT_GE(wall_start, before);
    EXPECT_LE(wall_start, after);

    ASSERT_EQ(swmm_engine_close(engine_), SWMM_OK);
}

// ---------------------------------------------------------------------------
// write_timing() reads ctx.wall_start, so initialization time is reported
// ---------------------------------------------------------------------------

TEST_F(ReportTimingTest, ElapsedTimeMeasuredFromContextWallStart) {
    ASSERT_EQ(swmm_engine_open(engine_, kInp, kRpt, kOut, nullptr), SWMM_OK);

    // Stand in for a long initialization: back-date the open stamp by
    // 30 h 00 m 00 s. Legacy renders that as "1.06:00:00" — one whole day
    // rolled into the "d." prefix, remainder as hh:mm:ss (report.c
    // report_writeSysTime + datetime_timeToStr).
    as_cpp_engine(engine_).context().wall_start -= (30 * 3600);

    run_to_report();
    ASSERT_EQ(swmm_engine_close(engine_), SWMM_OK);

    const std::string rpt = read_file(kRpt);
    ASSERT_FALSE(rpt.empty());
    EXPECT_NE(rpt.find("Analysis begun on:"), std::string::npos);
    EXPECT_NE(rpt.find("Analysis ended on:"), std::string::npos);

    const std::string elapsed = elapsed_field(rpt);
    ASSERT_FALSE(elapsed.empty());

    // A sub-second model run would print "< 1 sec" if the plugin had kept its
    // own clock; reading ctx.wall_start is what surfaces the 30 h instead.
    EXPECT_EQ(elapsed.rfind("1.06:00:0", 0), 0u)
        << "expected legacy d.hh:mm:ss rollover, got: " << elapsed;
}

// ---------------------------------------------------------------------------
// Short runs still report "< 1 sec" (legacy's elapsedTime < 1.0 branch)
// ---------------------------------------------------------------------------

TEST_F(ReportTimingTest, SubSecondRunReportsLessThanOneSecond) {
    ASSERT_EQ(swmm_engine_open(engine_, kInp, kRpt, kOut, nullptr), SWMM_OK);
    run_to_report();
    ASSERT_EQ(swmm_engine_close(engine_), SWMM_OK);

    const std::string elapsed = elapsed_field(read_file(kRpt));
    ASSERT_FALSE(elapsed.empty());
    // The model is a single junction over one hour; wall time is well under a
    // second on any supported host. Guard against flake by also accepting the
    // 00:00:0N form rather than pinning "< 1 sec" alone.
    EXPECT_TRUE(elapsed.rfind("< 1 sec", 0) == 0u ||
                elapsed.rfind("00:00:0", 0) == 0u)
        << "unexpected elapsed field: " << elapsed;
}

// ---------------------------------------------------------------------------
// Hour boundary: below one day there is no "d." prefix
// ---------------------------------------------------------------------------

TEST_F(ReportTimingTest, ElapsedUnderOneDayHasNoDayPrefix) {
    ASSERT_EQ(swmm_engine_open(engine_, kInp, kRpt, kOut, nullptr), SWMM_OK);

    // 23 h 59 m 59 s — the last value legacy renders without a day prefix.
    as_cpp_engine(engine_).context().wall_start -= (23 * 3600 + 59 * 60 + 59);

    run_to_report();
    ASSERT_EQ(swmm_engine_close(engine_), SWMM_OK);

    const std::string elapsed = elapsed_field(read_file(kRpt));
    ASSERT_FALSE(elapsed.empty());
    EXPECT_EQ(elapsed.find('.'), std::string::npos)
        << "elapsed under 24 h must not carry a day prefix: " << elapsed;
    EXPECT_EQ(elapsed.rfind("23:59:5", 0), 0u) << elapsed;
}

}  // namespace
