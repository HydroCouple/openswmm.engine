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
 * @file test_timeseries_file_roundtrip.cpp
 * @brief Round-trip tests for file-backed [TIMESERIES] entries.
 *
 * @details Reproduces and pins the fix for the engine IO bug pair:
 *   (1) TablesHandler used to stash the FILE token in Table::id as
 *       "FILE:path", clobbering the table name.
 *   (2) InpWriter [TIMESERIES] only emitted inline x/y rows, so any
 *       file-backed series was silently rewritten as inline (or, when
 *       the file failed to load, dropped entirely).
 *
 * After the fix the FILE form is preserved verbatim across read → write,
 * including an optional `:column` suffix inside the quoted path.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/engine/core/DateTime.hpp"
#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/core/InpWriter.hpp"
#include "../../src/engine/input/handlers/TablesHandler.hpp"
#include "../../src/engine/input/PostParseResolver.hpp"
#include "../../src/engine/data/TableData.hpp"

#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_tables.h"

namespace fs = std::filesystem;
using openswmm::SimulationContext;
using openswmm::TableType;

namespace {

// Find the [TIMESERIES] block and return its body lines (everything between
// the header and the next bracketed section or EOF). One-shot helper for the
// assertions below.
std::string sliceTimeseriesBlock(const std::string& content) {
    auto begin = content.find("[TIMESERIES]");
    if (begin == std::string::npos) return {};
    auto body  = begin + std::string("[TIMESERIES]").size();
    auto end   = content.find('\n', body);
    if (end == std::string::npos) return {};
    ++end;
    auto next  = content.find("\n[", end);
    return content.substr(end, next == std::string::npos ? std::string::npos
                                                          : next - end);
}

} // namespace

// ---------------------------------------------------------------------------
// Bug #2 — parser stores the path on Table::file_path, leaves Table::id alone.
// ---------------------------------------------------------------------------

TEST(TimeseriesFileRoundTrip, ParserStoresPathOnDedicatedField) {
    SimulationContext ctx;
    openswmm::input::handle_timeseries(ctx, {
        "RAIN_A   FILE   \"rain_2024.csv\"",
    });
    ASSERT_EQ(ctx.tables.tables.size(), 1u);
    const auto& tbl = ctx.tables.tables.front();
    EXPECT_EQ(tbl.id,        "RAIN_A");           // name, not "FILE:..."
    EXPECT_EQ(tbl.file_path, "rain_2024.csv");
}

TEST(TimeseriesFileRoundTrip, ParserPreservesColumnSuffixVerbatim) {
    SimulationContext ctx;
    openswmm::input::handle_timeseries(ctx, {
        "RAIN_E   FILE   \"rainfall_2024.csv:East_Gage\"",
    });
    ASSERT_EQ(ctx.tables.tables.size(), 1u);
    EXPECT_EQ(ctx.tables.tables.front().id,        "RAIN_E");
    EXPECT_EQ(ctx.tables.tables.front().file_path, "rainfall_2024.csv:East_Gage");
}

// ---------------------------------------------------------------------------
// Bug #1 — writer emits a FILE row for file-backed series instead of inline.
// ---------------------------------------------------------------------------

TEST(TimeseriesFileRoundTrip, WriterEmitsFileRowForFileBackedSeries) {
    SimulationContext ctx;
    int idx = ctx.tables.add("RAIN_A", TableType::TIMESERIES);
    ctx.tables[idx].file_path = "rain_2024.csv";

    const auto path = (fs::temp_directory_path()
                       / "ts_file_roundtrip_single.inp").string();
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, path), 0);

    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    const std::string content = ss.str();
    f.close();
    fs::remove(path);

    const auto block = sliceTimeseriesBlock(content);
    ASSERT_FALSE(block.empty()) << "[TIMESERIES] block missing from output";
    EXPECT_NE(block.find("RAIN_A"),         std::string::npos);
    EXPECT_NE(block.find("FILE"),           std::string::npos);
    EXPECT_NE(block.find("\"rain_2024.csv\""), std::string::npos);
    // The inline-row tell would be a date column ("MM/DD/YYYY") on a RAIN_A
    // data row. Scan only data rows — the ";;Name Date/Time Value" header
    // comment legitimately contains a '/'.
    std::istringstream bs(block);
    std::string line;
    while (std::getline(bs, line)) {
        if (!line.empty() && line[0] == ';') continue;  // skip comment/header lines
        EXPECT_EQ(line.find('/'), std::string::npos)
            << "writer leaked inline date rows for a file-backed series: " << line;
    }
}

TEST(TimeseriesFileRoundTrip, WriterPreservesColumnSuffix) {
    SimulationContext ctx;
    int idx = ctx.tables.add("RAIN_E", TableType::TIMESERIES);
    ctx.tables[idx].file_path = "rainfall_2024.csv:East_Gage";

    const auto path = (fs::temp_directory_path()
                       / "ts_file_roundtrip_column.inp").string();
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, path), 0);

    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    const std::string content = ss.str();
    f.close();
    fs::remove(path);

    EXPECT_NE(content.find("\"rainfall_2024.csv:East_Gage\""),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// End-to-end: parse → write → parse-again. The second parse must see the
// same file_path token byte-for-byte.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// P3 — engine-level: the `:column` suffix actually selects the column, and a
// comma CSV without a suffix loads the FIRST data column instead of a silent
// empty series. Unloadable/zero-row files now FAIL the open loudly.
// Working directory is tests/unit/engine/data/ (see CMakeLists).
// ---------------------------------------------------------------------------

namespace {

class EngineGuard {
public:
    EngineGuard() : e_(swmm_engine_create()) {}
    ~EngineGuard() {
        if (e_) { swmm_engine_close(e_); swmm_engine_destroy(e_); }
    }
    SWMM_Engine get() const { return e_; }
private:
    SWMM_Engine e_;
};

std::vector<double> tableYs(SWMM_Engine e, const char* id) {
    const int t = swmm_table_index(e, id);
    EXPECT_GE(t, 0) << id;
    if (t < 0) return {};
    int n = 0;
    EXPECT_EQ(swmm_table_get_point_count(e, t, &n), SWMM_OK);
    std::vector<double> ys(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        double x = 0.0, y = 0.0;
        EXPECT_EQ(swmm_table_get_point(e, t, k, &x, &y), SWMM_OK);
        ys[static_cast<std::size_t>(k)] = y;
    }
    return ys;
}

} // namespace

TEST(TimeseriesFileRoundTrip, EngineLoadsColumnSelectedSeries) {
    EngineGuard eng;
    ASSERT_NE(eng.get(), nullptr);
    ASSERT_EQ(swmm_engine_open(eng.get(), "rain_series/rain_ts_files.inp",
                               "_ts_files.rpt", "_ts_files.out", nullptr),
              SWMM_OK);

    // TS_COL = FILE "rain_multi.csv:WEST_GAGE" → the WEST column.
    const std::vector<double> west = tableYs(eng.get(), "TS_COL");
    ASSERT_EQ(west.size(), 4u)
        << "the :column form used to yield a silent empty series";
    EXPECT_NEAR(west[1], 0.30, 1e-12);
    EXPECT_NEAR(west[2], 0.90, 1e-12);

    // TS_NOCOL = FILE "rain_multi.csv" (no column) → first data column (EAST).
    const std::vector<double> east = tableYs(eng.get(), "TS_NOCOL");
    ASSERT_EQ(east.size(), 4u)
        << "a comma CSV without :col used to yield a silent empty series";
    EXPECT_NEAR(east[1], 0.60, 1e-12);
    EXPECT_NEAR(east[2], 1.20, 1e-12);
}

TEST(TimeseriesFileRoundTrip, MissingSeriesFileFailsOpenLoudly) {
    EngineGuard eng;
    ASSERT_NE(eng.get(), nullptr);
    EXPECT_NE(swmm_engine_open(eng.get(), "rain_series/rain_ts_missing.inp",
                               "_ts_missing.rpt", "_ts_missing.out", nullptr),
              SWMM_OK)
        << "a FILE series whose file cannot be opened must fail the open "
           "(legacy ERROR 361), not read 0.0 forever";
}

TEST(TimeseriesFileRoundTrip, ZeroRowSeriesFileFailsOpenLoudly) {
    EngineGuard eng;
    ASSERT_NE(eng.get(), nullptr);
    EXPECT_NE(swmm_engine_open(eng.get(), "rain_series/rain_ts_garbage.inp",
                               "_ts_garbage.rpt", "_ts_garbage.out", nullptr),
              SWMM_OK)
        << "a FILE series that parses to zero rows must fail the open";
}

// ---------------------------------------------------------------------------
// Path resolution: an already-anchored token must not be anchored a 2nd time
// ---------------------------------------------------------------------------

TEST(TimeseriesFileRoundTrip, ResolvedTokenIsNotAnchoredTwice) {
    // Working directory is tests/unit/engine/data/. The model claims to live
    // in rain_series/, so inp_dir is "rain_series" and the relative token
    // resolves to "rain_series/rain_multi.csv" — a path that is RELATIVE but
    // already anchored. The loader used to prepend inp_dir again, producing
    // "rain_series/rain_series/rain_multi.csv" and an empty series. On POSIX
    // a fully absolute token hid the bug (the old guard skipped a leading
    // '/'); on Windows "C:\…" hit it.
    SimulationContext ctx;
    ctx.inp_file_path = "rain_series/model.inp";
    openswmm::input::handle_timeseries(ctx, {
        "RAIN_REL   FILE   \"rain_multi.csv\"",
    });
    ASSERT_EQ(ctx.tables.tables.size(), 1u);

    openswmm::input::resolve_cross_references(ctx);

    const auto& rel = ctx.tables.tables.front();
    EXPECT_EQ(rel.file_path.absolute, "rain_series/rain_multi.csv")
        << "resolve_external_file_slots anchors the token exactly once";
    EXPECT_EQ(rel.file_path.str(), "rain_multi.csv")
        << "the verbatim token is preserved for round-trip";
    ASSERT_EQ(rel.x.size(), 4u) << "double-prepended path opened nothing";
    EXPECT_NEAR(rel.y[1], 0.60, 1e-12);  // first data column (EAST_GAGE)

    // The other route: a token that is already absolute resolves to itself and
    // still opens — the prepend must not apply to it either.
    SimulationContext ctx2;
    ctx2.inp_file_path = "rain_series/model.inp";
    const std::string abs_token =
        fs::absolute("rain_series/rain_multi.csv").generic_string();
    openswmm::input::handle_timeseries(ctx2, {
        "RAIN_ABS   FILE   \"" + abs_token + "\"",
    });
    openswmm::input::resolve_cross_references(ctx2);
    ASSERT_EQ(ctx2.tables.tables.size(), 1u);
    EXPECT_EQ(ctx2.tables.tables.front().file_path.absolute, abs_token);
    ASSERT_EQ(ctx2.tables.tables.front().x.size(), 4u);
}

TEST(TimeseriesFileRoundTrip, ParseWriteParsePreservesToken) {
    SimulationContext ctx1;
    openswmm::input::handle_timeseries(ctx1, {
        "RAIN_X   FILE   \"data/rain.dat\"",
    });

    const auto path = (fs::temp_directory_path()
                       / "ts_file_roundtrip_e2e.inp").string();
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx1, path), 0);

    // Read the written file back as raw text and pull the RAIN_X line out.
    std::ifstream f(path);
    std::string line, rain_x_line;
    while (std::getline(f, line)) {
        if (line.rfind("RAIN_X", 0) == 0) { rain_x_line = line; break; }
    }
    f.close();
    fs::remove(path);
    ASSERT_FALSE(rain_x_line.empty());

    // Round-trip the line through the parser again.
    SimulationContext ctx2;
    openswmm::input::handle_timeseries(ctx2, { rain_x_line });

    ASSERT_EQ(ctx2.tables.tables.size(), 1u);
    EXPECT_EQ(ctx2.tables.tables.front().id,        "RAIN_X");
    EXPECT_EQ(ctx2.tables.tables.front().file_path, "data/rain.dat");
}

// ---------------------------------------------------------------------------
// Legacy external-file row grammar (table_parseFileLine). The loader used to
// accept ONLY "M/D/Y H:MM value" rows, so every elapsed-time or decimal-hour
// legacy file parsed zero rows and failed the open with ERROR 363 — which is
// how "timeseries specified by filepath" broke in the GUI for legacy models.
// Working directory is tests/unit/engine/data/ (see CMakeLists).
// ---------------------------------------------------------------------------

TEST(TimeseriesFileRoundTrip, ElapsedTimeFileAnchorsAtSimulationStart) {
    SimulationContext ctx;
    ctx.inp_file_path = "rain_series/model.inp";
    // Legacy input.c:176 seeds lastDate = StartDate + StartTime, so date-less
    // rows are elapsed from the start DATETIME — time fraction included.
    const double start = openswmm::datetime::encodeDate(2024, 1, 1)
                       + openswmm::datetime::encodeTime(6, 0, 0);
    ctx.options.start_date = start;
    openswmm::input::handle_timeseries(ctx, {
        "TS_EL   FILE   \"legacy_elapsed.dat\"",
    });
    openswmm::input::resolve_cross_references(ctx);

    const auto& tbl = ctx.tables.tables.front();
    ASSERT_EQ(tbl.x.size(), 4u) << "elapsed-time rows must parse";
    EXPECT_NEAR(tbl.x[0], start, 1e-12);
    EXPECT_NEAR(tbl.x[2], start + 2.0 / 24.0, 1e-12);
    EXPECT_NEAR(tbl.y[2], 5.0, 1e-12);
}

TEST(TimeseriesFileRoundTrip, DecimalHourFileParses) {
    SimulationContext ctx;
    ctx.inp_file_path = "rain_series/model.inp";
    const double start = openswmm::datetime::encodeDate(2024, 3, 1);
    ctx.options.start_date = start;
    openswmm::input::handle_timeseries(ctx, {
        "TS_DEC   FILE   \"legacy_dechours.dat\"",
    });
    openswmm::input::resolve_cross_references(ctx);

    const auto& tbl = ctx.tables.tables.front();
    ASSERT_EQ(tbl.x.size(), 4u) << "decimal-hour rows must parse";
    EXPECT_NEAR(tbl.x[1], start + 1.5 / 24.0, 1e-12);
    EXPECT_NEAR(tbl.x[3], start + 25.25 / 24.0, 1e-12)
        << "hours past 24 keep accumulating (no day wrap for elapsed rows)";
    EXPECT_NEAR(tbl.y[3], 3.0, 1e-12);
}

TEST(TimeseriesFileRoundTrip, DatedFileFullLegacyGrammar) {
    SimulationContext ctx;
    ctx.inp_file_path = "rain_series/model.inp";
    // start_date deliberately far from the file's dates: dated rows must NOT
    // be shifted by the relative-row anchoring.
    ctx.options.start_date = openswmm::datetime::encodeDate(2030, 6, 15);
    openswmm::input::handle_timeseries(ctx, {
        "TS_MIX   FILE   \"legacy_dated_mixed.dat\"",
    });
    openswmm::input::resolve_cross_references(ctx);

    const double jan1 = openswmm::datetime::encodeDate(2024, 1, 1);
    const auto& tbl = ctx.tables.tables.front();
    ASSERT_EQ(tbl.x.size(), 4u)
        << "dash dates, month names, carryover and decimal-hour rows must parse";
    EXPECT_NEAR(tbl.x[0], jan1, 1e-12);                     // 01-01-2024 0:00
    EXPECT_NEAR(tbl.x[1], jan1 + 6.0 / 24.0, 1e-12);        // Jan-01-2024 6:00
    EXPECT_NEAR(tbl.x[2], jan1 + 12.0 / 24.0, 1e-12);       // 12:00 (carryover)
    EXPECT_NEAR(tbl.x[3], jan1 + 1.0 + 6.5 / 24.0, 1e-12);  // 01/02/2024 6.5
    EXPECT_NEAR(tbl.y[3], 4.0, 1e-12);
}
