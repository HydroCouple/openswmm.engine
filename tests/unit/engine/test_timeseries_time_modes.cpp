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
 * @file test_timeseries_time_modes.cpp
 * @brief Inline [TIMESERIES] time modes: time-only rows are elapsed times
 *        anchored at the simulation start; dated rows are absolute; both may
 *        mix in one series.
 *
 * @details Legacy SWMM seeds every series' date anchor with
 *          StartDate + StartTime before parsing (input.c:170), so rows
 *          authored without a date are relative to the simulation start until
 *          an explicit date re-anchors the series. The refactored engine
 *          tracks this explicitly: TablesHandler counts the leading date-less
 *          rows in Table::n_relative, resolve_cross_references() offsets
 *          exactly those rows by options.start_date (recording the applied
 *          offset in Table::rel_anchor), and InpWriter emits them back in
 *          elapsed H:MM[:SS] form while dated rows keep their dates.
 *
 *          These tests pin the defects fixed in this round:
 *          - the old `x[0] < 366` resolver heuristic shifted a MIXED series'
 *            dated rows by start_date too (pushing them ~107 years out);
 *          - the writer's `x0 - start < 366` heuristic rewrote a series the
 *            user authored WITH dates near the start date as elapsed times,
 *            and emitted "0:60" for 1:00 (truncate-then-round);
 *          - the parser shared one date anchor across interleaved series.
 *
 *          Per CLAUDE.md §4.1 artifacts are written next to the fixtures
 *          under tests/unit/engine/data/tseries_modes/ with the `_` prefix
 *          that .gitignore excludes.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/engine/core/SWMMEngine.hpp"
#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/core/InpWriter.hpp"
#include "../../src/engine/core/DateTime.hpp"

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_tables.h>

namespace {

using openswmm::SWMMEngine;
using openswmm::Table;
using openswmm::TableType;

constexpr const char* kDir = "tseries_modes/";

std::string data(const std::string& name) { return kDir + name; }

// 01/01/2007 06:00 as an OADate — the fixture's START_DATE + START_TIME.
const double kStart = openswmm::datetime::encodeDate(2007, 1, 1) + 0.25;
// 01/02/2007 00:00.
const double kJan2 = openswmm::datetime::encodeDate(2007, 1, 2);

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Rows of the [TIMESERIES] section whose first column is `id`, each
/// whitespace-split into columns (writer column widths are not a contract).
std::vector<std::vector<std::string>> seriesRows(const std::string& text,
                                                 const std::string& id) {
    std::vector<std::vector<std::string>> rows;
    std::istringstream in(text);
    std::string line;
    bool inside = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = line;
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front())))
            t.erase(t.begin());
        if (!t.empty() && t.front() == '[') {
            inside = (t.rfind("[TIMESERIES]", 0) == 0);
            continue;
        }
        if (!inside || t.empty() || t.front() == ';') continue;
        std::istringstream row(t);
        std::vector<std::string> c;
        std::string tok;
        while (row >> tok) c.push_back(tok);
        if (!c.empty() && c[0] == id) rows.push_back(std::move(c));
    }
    return rows;
}

const Table& table(SWMMEngine& eng, const std::string& id) {
    const int idx = eng.context().find_timeseries(id);
    EXPECT_GE(idx, 0) << "series " << id << " missing";
    return eng.context().tables[idx];
}

struct OpenedFixture {
    SWMMEngine eng;
    OpenedFixture() {
        EXPECT_EQ(eng.open(data("time_modes.inp").c_str(),
                           data("_time_modes.rpt").c_str(), nullptr),
                  SWMM_OK);
        for (const auto& e : eng.context().errors) ADD_FAILURE() << e;
    }
};

// ---------------------------------------------------------------------------
// Parsing + resolution
// ---------------------------------------------------------------------------

TEST(TimeseriesTimeModes, RelativeRowsAnchorAtSimulationStart) {
    OpenedFixture f;
    const Table& t = table(f.eng, "TS_REL");
    ASSERT_EQ(t.x.size(), 3u);
    EXPECT_EQ(t.n_relative, 3);
    EXPECT_DOUBLE_EQ(t.rel_anchor, kStart);
    EXPECT_NEAR(t.x[0], kStart, 1e-9);
    EXPECT_NEAR(t.x[1], kStart + 1.0 / 24.0, 1e-9);
    EXPECT_NEAR(t.x[2], kStart + 25.5 / 24.0, 1e-9);  // decimal hours > 24
}

TEST(TimeseriesTimeModes, DatedRowsAreAbsoluteAndCarryTheDateForward) {
    OpenedFixture f;
    const Table& t = table(f.eng, "TS_DATED");
    ASSERT_EQ(t.x.size(), 3u);
    EXPECT_EQ(t.n_relative, 0);
    EXPECT_NEAR(t.x[0], kJan2, 1e-9);
    EXPECT_NEAR(t.x[1], kJan2 + 0.25, 1e-9);
    EXPECT_NEAR(t.x[2], kJan2 + 0.50, 1e-9);  // date-less continuation row
}

TEST(TimeseriesTimeModes, MixedSeriesShiftsOnlyTheRelativeHead) {
    OpenedFixture f;
    const Table& t = table(f.eng, "TS_MIXED");
    ASSERT_EQ(t.x.size(), 4u);
    EXPECT_EQ(t.n_relative, 2);
    EXPECT_NEAR(t.x[0], kStart, 1e-9);
    EXPECT_NEAR(t.x[1], kStart + 2.0 / 24.0, 1e-9);
    // The dated tail must NOT move (the old x[0] < 366 heuristic pushed it
    // ~107 years out).
    EXPECT_NEAR(t.x[2], kJan2 + 3.0 / 24.0, 1e-9);
    EXPECT_NEAR(t.x[3], kJan2 + 4.0 / 24.0, 1e-9);
}

TEST(TimeseriesTimeModes, InterleavedSeriesKeepIndependentAnchors) {
    OpenedFixture f;
    const Table& a = table(f.eng, "TS_ILA");
    const Table& b = table(f.eng, "TS_ILB");
    ASSERT_EQ(a.x.size(), 2u);
    ASSERT_EQ(b.x.size(), 2u);
    EXPECT_EQ(a.n_relative, 0);
    EXPECT_NEAR(a.x[0], kJan2, 1e-9);
    EXPECT_NEAR(a.x[1], kJan2 + 1.0 / 24.0, 1e-9);
    // TS_ILB never saw a date: it must anchor at the simulation start, not at
    // TS_ILA's date (the parser used to share one anchor across the section).
    EXPECT_EQ(b.n_relative, 2);
    EXPECT_NEAR(b.x[0], kStart, 1e-9);
    EXPECT_NEAR(b.x[1], kStart + 1.0 / 24.0, 1e-9);
}

// ---------------------------------------------------------------------------
// Writer emission
// ---------------------------------------------------------------------------

TEST(TimeseriesTimeModes, WriterPreservesAuthoredForms) {
    OpenedFixture f;
    std::vector<std::string> warnings;
    const std::string out = data("_time_modes_out.inp");
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(f.eng.context(), out, &warnings), 0);
    const std::string text = slurp(out);

    // Relative series: 2 columns after the name (time, value) — no date —
    // and "1:00", not the old truncate-then-round "0:60".
    auto rel = seriesRows(text, "TS_REL");
    ASSERT_EQ(rel.size(), 3u);
    EXPECT_EQ(rel[0].size(), 3u);
    EXPECT_EQ(rel[0][1], "0:00");
    EXPECT_EQ(rel[1][1], "1:00");
    EXPECT_EQ(rel[2][1], "25:30");  // 25.5 decimal hours round-trips as clock

    // Dated series keeps explicit dates on every row (3 columns after name).
    auto dated = seriesRows(text, "TS_DATED");
    ASSERT_EQ(dated.size(), 3u);
    for (const auto& r : dated) {
        ASSERT_EQ(r.size(), 4u) << "dated row lost its date column";
        EXPECT_NE(r[1].find('/'), std::string::npos);
    }
    EXPECT_EQ(dated[0][1], "01/02/2007");

    // Mixed series: elapsed head + dated tail.
    auto mixed = seriesRows(text, "TS_MIXED");
    ASSERT_EQ(mixed.size(), 4u);
    EXPECT_EQ(mixed[0].size(), 3u);
    EXPECT_EQ(mixed[1].size(), 3u);
    EXPECT_EQ(mixed[2].size(), 4u);
    EXPECT_EQ(mixed[3].size(), 4u);

    // Seconds resolution survives as H:MM:SS.
    auto sec = seriesRows(text, "TS_SEC");
    ASSERT_EQ(sec.size(), 2u);
    EXPECT_EQ(sec[0][1], "0:00:30");
    EXPECT_EQ(sec[1][1], "1:00");
}

TEST(TimeseriesTimeModes, WriterEmitsAuthoredElapsedEvenAfterStartDateEdit) {
    OpenedFixture f;
    // The GUI can edit START_DATE on an open model. The writer must subtract
    // the per-table rel_anchor (the offset actually baked into x), not the
    // live start_date — otherwise the authored elapsed times would shift.
    f.eng.context().options.start_date += 5.0;
    std::vector<std::string> warnings;
    const std::string out = data("_time_modes_startedit_out.inp");
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(f.eng.context(), out, &warnings), 0);
    auto rel = seriesRows(slurp(out), "TS_REL");
    ASSERT_EQ(rel.size(), 3u);
    EXPECT_EQ(rel[0][1], "0:00");
    EXPECT_EQ(rel[1][1], "1:00");
    EXPECT_EQ(rel[2][1], "25:30");
}

TEST(TimeseriesTimeModes, WriteReloadWriteIsAFixedPoint) {
    std::vector<std::string> warnings;
    const std::string out1 = data("_time_modes_rt1.inp");
    const std::string out2 = data("_time_modes_rt2.inp");
    {
        OpenedFixture f;
        ASSERT_EQ(openswmm::inp_writer::writeInpFile(f.eng.context(), out1, &warnings), 0);
    }
    {
        SWMMEngine eng2;
        ASSERT_EQ(eng2.open(out1.c_str(), data("_time_modes_rt.rpt").c_str(), nullptr),
                  SWMM_OK);
        // Reload re-anchors relative rows at the (unchanged) start date, so
        // the resolved tables must be identical to the first open.
        const Table& t = table(eng2, "TS_MIXED");
        EXPECT_EQ(t.n_relative, 2);
        EXPECT_NEAR(t.x[0], kStart, 1e-9);
        EXPECT_NEAR(t.x[2], kJan2 + 3.0 / 24.0, 1e-9);
        ASSERT_EQ(openswmm::inp_writer::writeInpFile(eng2.context(), out2, &warnings), 0);
    }
    // Byte-identical [TIMESERIES] sections on the second generation.
    auto sectionOf = [](const std::string& text) {
        const auto b = text.find("[TIMESERIES]");
        const auto e = text.find("\n[", b + 1);
        return text.substr(b, e == std::string::npos ? std::string::npos : e - b);
    };
    EXPECT_EQ(sectionOf(slurp(out1)), sectionOf(slurp(out2)));
}

// ---------------------------------------------------------------------------
// C API
// ---------------------------------------------------------------------------

TEST(TimeseriesTimeModes, RelativeInfoCApiRoundTripAndClearReset) {
    SWMM_Engine eng = swmm_engine_create();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(swmm_engine_open(eng, data("time_modes.inp").c_str(),
                               data("_time_modes_capi.rpt").c_str(), nullptr, nullptr),
              SWMM_OK);

    const int idx = swmm_table_index(eng, "TS_MIXED");
    ASSERT_GE(idx, 0);

    int n = -1;
    double anchor = 0.0;
    ASSERT_EQ(swmm_timeseries_get_relative_info(eng, idx, &n, &anchor), SWMM_OK);
    EXPECT_EQ(n, 2);
    EXPECT_DOUBLE_EQ(anchor, kStart);

    // The GUI save path: clear (resets both fields), re-add points, then
    // re-declare the relative info.
    ASSERT_EQ(swmm_table_clear(eng, idx), SWMM_OK);
    ASSERT_EQ(swmm_timeseries_get_relative_info(eng, idx, &n, &anchor), SWMM_OK);
    EXPECT_EQ(n, 0);
    EXPECT_DOUBLE_EQ(anchor, 0.0);

    ASSERT_EQ(swmm_table_add_point(eng, idx, kStart, 1.0), SWMM_OK);
    ASSERT_EQ(swmm_table_add_point(eng, idx, kStart + 0.5, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_timeseries_set_relative_info(eng, idx, 2, kStart), SWMM_OK);
    ASSERT_EQ(swmm_timeseries_get_relative_info(eng, idx, &n, &anchor), SWMM_OK);
    EXPECT_EQ(n, 2);
    EXPECT_DOUBLE_EQ(anchor, kStart);

    // Count is clamped to the point count; negative counts are rejected.
    EXPECT_EQ(swmm_timeseries_set_relative_info(eng, idx, 99, kStart), SWMM_OK);
    ASSERT_EQ(swmm_timeseries_get_relative_info(eng, idx, &n, &anchor), SWMM_OK);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(swmm_timeseries_set_relative_info(eng, idx, -1, kStart),
              SWMM_ERR_BADPARAM);

    swmm_engine_destroy(eng);
}

}  // namespace
