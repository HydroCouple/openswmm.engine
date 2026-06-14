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
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/core/InpWriter.hpp"
#include "../../src/engine/input/handlers/TablesHandler.hpp"
#include "../../src/engine/data/TableData.hpp"

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
    int nidx = ctx.table_names.add("RAIN_A");
    int idx  = ctx.tables.add("RAIN_A", TableType::TIMESERIES);
    ASSERT_EQ(nidx, idx);
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
    int nidx = ctx.table_names.add("RAIN_E");
    int idx  = ctx.tables.add("RAIN_E", TableType::TIMESERIES);
    ASSERT_EQ(nidx, idx);
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
