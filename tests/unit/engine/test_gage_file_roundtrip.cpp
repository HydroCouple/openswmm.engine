/**
 * @file test_gage_file_roundtrip.cpp
 * @brief [RAINGAGES] write→read round-trip for FILE / TIMESERIES gages.
 *
 * @details Regression coverage for the InpWriter dropping the station-ID and
 *          rain-units tokens of FILE-source gages (and gage scale factors),
 *          which produced legacy "ERROR 203: too few items" on re-run and
 *          silently destroyed the fields on every save.  Legacy FILE grammar
 *          (gage.c gage_readParams / readGageFileFormat):
 *            Name Format Interval SCF FILE Fname Station Units [Start|*] [SF]
 *          Covers:
 *            - STAN_PRCP station + units (IN and MM) round-trip.
 *            - Optional scale factor round-trip ('*' start-date placeholder).
 *            - USER_CSV compact "path:col" single-token form round-trip.
 *            - Empty station writes '*' + warning; re-read normalizes to "".
 *            - TIMESERIES gage scale factor round-trip.
 *
 * @see src/engine/core/InpWriter.cpp ([RAINGAGES] section)
 * @see src/engine/input/handlers/CatchmentHandler.cpp (handle_raingages)
 * @see Legacy parity: src/legacy/engine/gage.c
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/core/InpWriter.hpp"
#include "../../src/engine/input/handlers/CatchmentHandler.hpp"

namespace fs = std::filesystem;
using openswmm::RainFileFormat;
using openswmm::SimulationContext;
using openswmm::input::handle_raingages;

namespace {

// Reviewable test-output directory (CLAUDE.md §4.1). Resolved at runtime
// relative to the test binary's CWD, then anchored to the source tree's
// tests/unit/engine/data/gage_roundtrip/ folder.
fs::path testDataDir() {
    fs::path here = fs::current_path();
    for (int i = 0; i < 8 && !fs::exists(here / "tests/unit/engine/data"); ++i) {
        if (here.has_parent_path()) here = here.parent_path();
    }
    fs::path dir = here / "tests/unit/engine/data/gage_roundtrip";
    fs::create_directories(dir);
    return dir;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

// Raw lines of the named section from written INP content (header excluded).
std::vector<std::string> sectionLines(const std::string& content,
                                      const std::string& name) {
    std::istringstream is(content);
    std::string line;
    std::vector<std::string> out;
    bool in_section = false;
    while (std::getline(is, line)) {
        if (!line.empty() && line[0] == '[') {
            in_section = (line.rfind("[" + name + "]", 0) == 0);
            continue;
        }
        if (in_section && !line.empty()) out.push_back(line);
    }
    return out;
}

// First data (non-";;") line of the written [RAINGAGES] block.
std::string firstGageLine(const std::string& content) {
    for (const auto& l : sectionLines(content, "RAINGAGES"))
        if (l.rfind(";;", 0) != 0) return l;
    return {};
}

// Parse one [RAINGAGES] data row into a fresh context.
void parseGages(SimulationContext& ctx, const std::string& row) {
    handle_raingages(ctx, std::vector<std::string>{row});
}

// Write ctx to a reviewable .inp and return its content.
std::string writeInp(const SimulationContext& ctx, const char* fname,
                     std::vector<std::string>* warnings = nullptr) {
    const fs::path dst = testDataDir() / fname;
    std::vector<std::string> local;
    EXPECT_EQ(openswmm::inp_writer::writeInpFile(
                  ctx, dst.string(), warnings ? warnings : &local), 0);
    return readFile(dst);
}

} // namespace

// ---------------------------------------------------------------------------
// STAN_PRCP — station + units are written and survive a round trip
// ---------------------------------------------------------------------------

TEST(GageFileRoundTrip, StationAndUnitsWrittenAndReparsed) {
    SimulationContext ctx;
    parseGages(ctx, "1 VOLUME 1:00 1.00 FILE \"Rainfall.dat\" KCVG IN");

    const std::string content = writeInp(ctx, "stan_prcp_in.inp");
    const std::string line = firstGageLine(content);
    ASSERT_FALSE(line.empty());
    EXPECT_NE(line.find("FILE \"Rainfall.dat\" KCVG IN"), std::string::npos)
        << "written line: " << line;

    SimulationContext ctx2;
    parseGages(ctx2, line);
    ASSERT_EQ(ctx2.n_gages(), 1);
    EXPECT_EQ(ctx2.gages.station_id[0], "KCVG");
    EXPECT_EQ(ctx2.gages.rain_units[0], 0); // IN
    EXPECT_EQ(ctx2.gages.file_format[0], RainFileFormat::STAN_PRCP);
    EXPECT_EQ(ctx2.gages.rain_type[0], 1); // VOLUME
}

TEST(GageFileRoundTrip, MmUnitsAndScaleFactorRoundTrip) {
    SimulationContext ctx;
    parseGages(ctx, "G2 INTENSITY 0:05 0.80 FILE \"rain.dat\" STA42 MM * 2.5");

    const std::string content = writeInp(ctx, "stan_prcp_mm_sf.inp");
    const std::string line = firstGageLine(content);
    ASSERT_FALSE(line.empty());
    EXPECT_NE(line.find("FILE \"rain.dat\" STA42 MM * 2.5"), std::string::npos)
        << "written line: " << line;

    SimulationContext ctx2;
    parseGages(ctx2, line);
    ASSERT_EQ(ctx2.n_gages(), 1);
    EXPECT_EQ(ctx2.gages.station_id[0], "STA42");
    EXPECT_EQ(ctx2.gages.rain_units[0], 1); // MM
    EXPECT_DOUBLE_EQ(ctx2.gages.scale_factor[0], 2.5);
}

// ---------------------------------------------------------------------------
// USER_CSV — compact "path:col" form stays a single re-parseable token
// ---------------------------------------------------------------------------

TEST(GageFileRoundTrip, UserCsvColumnFormRoundTrip) {
    SimulationContext ctx;
    parseGages(ctx, "G3 VOLUME 1:00 1.00 FILE \"gage.csv:P1\" 2");

    const std::string content = writeInp(ctx, "user_csv.inp");
    const std::string line = firstGageLine(content);
    ASSERT_FALSE(line.empty());
    EXPECT_NE(line.find("FILE \"gage.csv:P1\" 2"), std::string::npos)
        << "written line: " << line;

    SimulationContext ctx2;
    parseGages(ctx2, line);
    ASSERT_EQ(ctx2.n_gages(), 1);
    EXPECT_EQ(ctx2.gages.file_format[0], RainFileFormat::USER_CSV);
    EXPECT_EQ(ctx2.gages.col_name[0], "P1");
    EXPECT_EQ(ctx2.gages.file_path[0].str(), "gage.csv");
    EXPECT_DOUBLE_EQ(ctx2.gages.scale_factor[0], 2.0);
}

// ---------------------------------------------------------------------------
// Empty station — '*' placeholder keeps the line legacy-parseable
// ---------------------------------------------------------------------------

TEST(GageFileRoundTrip, EmptyStationWritesPlaceholderAndWarns) {
    SimulationContext ctx;
    // Damaged pre-fix form: no station / units tokens.
    parseGages(ctx, "G4 VOLUME 1:00 1.00 FILE \"rain.dat\"");
    ASSERT_EQ(ctx.n_gages(), 1);
    ASSERT_TRUE(ctx.gages.station_id[0].empty());

    std::vector<std::string> warnings;
    const std::string content = writeInp(ctx, "empty_station.inp", &warnings);
    const std::string line = firstGageLine(content);
    ASSERT_FALSE(line.empty());
    EXPECT_NE(line.find("FILE \"rain.dat\" * IN"), std::string::npos)
        << "written line: " << line;

    bool warned = false;
    for (const auto& w : warnings)
        if (w.find("station") != std::string::npos) warned = true;
    EXPECT_TRUE(warned) << "expected a missing-station warning";

    // '*' normalizes back to empty ("accept all rows"), not a literal filter.
    SimulationContext ctx2;
    parseGages(ctx2, line);
    ASSERT_EQ(ctx2.n_gages(), 1);
    EXPECT_TRUE(ctx2.gages.station_id[0].empty());
    EXPECT_EQ(ctx2.gages.file_format[0], RainFileFormat::STAN_PRCP);
}

// ---------------------------------------------------------------------------
// TIMESERIES — optional scale factor round-trips
// ---------------------------------------------------------------------------

TEST(GageFileRoundTrip, TimeseriesScaleFactorRoundTrip) {
    SimulationContext ctx;
    ctx.tables.add("TS1", openswmm::TableType::TIMESERIES);
    parseGages(ctx, "G5 INTENSITY 0:05 1.00 TIMESERIES TS1 3.5");
    ASSERT_EQ(ctx.gages.ts_index[0], 0);

    const std::string content = writeInp(ctx, "timeseries_sf.inp");
    const std::string line = firstGageLine(content);
    ASSERT_FALSE(line.empty());
    EXPECT_NE(line.find("TIMESERIES TS1 3.5"), std::string::npos)
        << "written line: " << line;

    SimulationContext ctx2;
    ctx2.tables.add("TS1", openswmm::TableType::TIMESERIES);
    parseGages(ctx2, line);
    ASSERT_EQ(ctx2.n_gages(), 1);
    EXPECT_DOUBLE_EQ(ctx2.gages.scale_factor[0], 3.5);
}
