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
 * @file test_output_reader_live.cpp
 * @brief Live (footer-less) .out reading — swmm_output_open_live / _refresh.
 *
 * A run in progress is simulated by copying the finished
 * site_drainage_model.out fixture and truncating it at various points:
 * header only, k whole periods, k periods plus a partial record, and the
 * complete file. The live reader must count exactly the whole periods on
 * disk, track growth on refresh(), adopt the footer when it lands, and read
 * the same values the footer-based reader reads.
 *
 * Working copies are written next to the fixture under data/live_out/ (a
 * reviewable location, per the GUI/engine CLAUDE.md file-IO rule), not to a
 * temp dir.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_output.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* kFixturePath = "site_drainage_model.out";
constexpr long kFooterBytes = 6 * 4;

std::vector<char> readAll(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
}

void writePrefix(const fs::path& path, const std::vector<char>& bytes, std::size_t n) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(n));
}

void appendBytes(const fs::path& path, const std::vector<char>& bytes,
                 std::size_t from, std::size_t to) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out.write(bytes.data() + from, static_cast<std::streamsize>(to - from));
}

struct Layout {
    long output_start = 0;
    long bytes_per_period = 0;
    int  periods = 0;
    long file_size = 0;
};

// Derive the record layout from the finished file's footer + counts.
Layout layoutOf(const std::vector<char>& bytes, SWMM_Output finished) {
    Layout L;
    L.file_size = static_cast<long>(bytes.size());
    int32_t footer[6];
    std::memcpy(footer, bytes.data() + bytes.size() - kFooterBytes, kFooterBytes);
    L.output_start = footer[2];
    L.periods      = footer[3];
    L.bytes_per_period = (L.file_size - kFooterBytes - L.output_start) / L.periods;
    EXPECT_GT(L.bytes_per_period, 8);
    EXPECT_EQ(swmm_output_get_period_count(finished), L.periods);
    return L;
}

} // anonymous

TEST(OutputReaderLive, HeaderOnlyOpensWithZeroPeriods)
{
    const auto bytes = readAll(kFixturePath);
    ASSERT_FALSE(bytes.empty());
    SWMM_Output finished = swmm_output_open(kFixturePath);
    ASSERT_NE(finished, nullptr);
    const Layout L = layoutOf(bytes, finished);

    const fs::path p = fs::path("live_out") / "header_only.out";
    writePrefix(p, bytes, static_cast<std::size_t>(L.output_start));

    // The footer-based opener must reject it; the live opener must accept it.
    EXPECT_EQ(swmm_output_open(p.string().c_str()), nullptr);
    SWMM_Output live = swmm_output_open_live(p.string().c_str());
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(swmm_output_get_period_count(live), 0);
    EXPECT_EQ(swmm_output_is_live(live), 1);
    EXPECT_EQ(swmm_output_get_node_count(live), swmm_output_get_node_count(finished));
    EXPECT_EQ(swmm_output_get_link_count(live), swmm_output_get_link_count(finished));
    EXPECT_EQ(swmm_output_get_report_step(live), swmm_output_get_report_step(finished));
    swmm_output_close(live);
    swmm_output_close(finished);
}

TEST(OutputReaderLive, WholePeriodsCountedPartialRecordIgnored)
{
    const auto bytes = readAll(kFixturePath);
    SWMM_Output finished = swmm_output_open(kFixturePath);
    ASSERT_NE(finished, nullptr);
    const Layout L = layoutOf(bytes, finished);
    ASSERT_GE(L.periods, 3);

    const int k = L.periods / 2;
    const std::size_t whole = static_cast<std::size_t>(L.output_start + k * L.bytes_per_period);

    const fs::path pWhole = fs::path("live_out") / "k_periods.out";
    writePrefix(pWhole, bytes, whole);
    SWMM_Output live = swmm_output_open_live(pWhole.string().c_str());
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(swmm_output_get_period_count(live), k);

    // Partial next record: 8 bytes (the date) plus a few floats.
    const fs::path pPart = fs::path("live_out") / "k_periods_partial.out";
    writePrefix(pPart, bytes, whole + 8 + 12);
    SWMM_Output livePart = swmm_output_open_live(pPart.string().c_str());
    ASSERT_NE(livePart, nullptr);
    EXPECT_EQ(swmm_output_get_period_count(livePart), k);
    swmm_output_close(livePart);

    // Values read live equal the footer-based read, period by period.
    const int nNodes = swmm_output_get_node_count(finished);
    std::vector<float> a(static_cast<std::size_t>(nNodes)), b(a.size());
    for (int p = 0; p < k; ++p) {
        ASSERT_EQ(swmm_output_get_node_result(live, p, SWMM_OUT_NODE_DEPTH, a.data()), 0);
        ASSERT_EQ(swmm_output_get_node_result(finished, p, SWMM_OUT_NODE_DEPTH, b.data()), 0);
        EXPECT_EQ(a, b) << "period " << p;
        double ta = 0, tb = 0;
        ASSERT_EQ(swmm_output_get_period_time(live, p, &ta), 0);
        ASSERT_EQ(swmm_output_get_period_time(finished, p, &tb), 0);
        EXPECT_EQ(ta, tb);
    }
    // Beyond the live count is refused even though the finished file has more.
    EXPECT_NE(swmm_output_get_node_result(live, k, SWMM_OUT_NODE_DEPTH, a.data()), 0);

    swmm_output_close(live);
    swmm_output_close(finished);
}

TEST(OutputReaderLive, RefreshTracksGrowthAndAdoptsFooter)
{
    const auto bytes = readAll(kFixturePath);
    SWMM_Output finished = swmm_output_open(kFixturePath);
    ASSERT_NE(finished, nullptr);
    const Layout L = layoutOf(bytes, finished);
    ASSERT_GE(L.periods, 3);

    const fs::path p = fs::path("live_out") / "growing.out";
    std::size_t written = static_cast<std::size_t>(L.output_start + L.bytes_per_period);
    writePrefix(p, bytes, written);

    SWMM_Output live = swmm_output_open_live(p.string().c_str());
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(swmm_output_get_period_count(live), 1);

    // Append two more periods; refresh must see them without re-opening.
    std::size_t next = written + 2 * static_cast<std::size_t>(L.bytes_per_period);
    appendBytes(p, bytes, written, next);
    written = next;
    int n = -1;
    ASSERT_EQ(swmm_output_refresh(live, &n), 0);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(swmm_output_get_period_count(live), 3);
    EXPECT_EQ(swmm_output_is_live(live), 1);

    // Append the rest including the footer: the reader adopts it and leaves
    // live mode; the count is now the writer's own.
    appendBytes(p, bytes, written, bytes.size());
    ASSERT_EQ(swmm_output_refresh(live, &n), 0);
    EXPECT_EQ(n, L.periods);
    EXPECT_EQ(swmm_output_is_live(live), 0);
    EXPECT_EQ(swmm_output_get_error_code(live), swmm_output_get_error_code(finished));

    // Idempotent afterwards.
    ASSERT_EQ(swmm_output_refresh(live, &n), 0);
    EXPECT_EQ(n, L.periods);

    // Last period reads identically.
    const int nLinks = swmm_output_get_link_count(finished);
    std::vector<float> a(static_cast<std::size_t>(nLinks)), b(a.size());
    ASSERT_EQ(swmm_output_get_link_result(live, L.periods - 1, SWMM_OUT_LINK_FLOW, a.data()), 0);
    ASSERT_EQ(swmm_output_get_link_result(finished, L.periods - 1, SWMM_OUT_LINK_FLOW, b.data()), 0);
    EXPECT_EQ(a, b);

    swmm_output_close(live);
    swmm_output_close(finished);
}

TEST(OutputReaderLive, FinishedFileOpensLiveAsFinal)
{
    // A complete file opened live is immediately non-live with the footer count.
    SWMM_Output finished = swmm_output_open(kFixturePath);
    ASSERT_NE(finished, nullptr);
    SWMM_Output live = swmm_output_open_live(kFixturePath);
    ASSERT_NE(live, nullptr);
    EXPECT_EQ(swmm_output_is_live(live), 0);
    EXPECT_EQ(swmm_output_get_period_count(live), swmm_output_get_period_count(finished));
    swmm_output_close(live);
    swmm_output_close(finished);
}

TEST(OutputReaderLive, RefreshOnNonLiveHandleIsNoOp)
{
    SWMM_Output finished = swmm_output_open(kFixturePath);
    ASSERT_NE(finished, nullptr);
    const int before = swmm_output_get_period_count(finished);
    int n = -1;
    EXPECT_EQ(swmm_output_refresh(finished, &n), 0);
    EXPECT_EQ(n, before);
    EXPECT_EQ(swmm_output_refresh(nullptr, &n), -1);
    swmm_output_close(finished);
}
