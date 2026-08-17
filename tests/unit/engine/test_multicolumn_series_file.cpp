/**
 * @file test_multicolumn_series_file.cpp
 * @brief Parser-level tests for the parse-once multi-column series file
 *        cache (CSV/TSV/PCSWMM TSF).
 *
 * @details Covers the engine gaps closed by
 *          plans/MULTICOLUMN_SERIES_SINGLE_READ_2026-08-17.md:
 *          - P2: rows/headers wider than the old 4096-byte fgets buffer
 *            parse uncorrupted with correct column indices (rain_wide.csv,
 *            regenerable via rain_wide_gen.py next to it).
 *          - P4: PCSWMM .tsf (IDs: 3-row header, tab-delimited, 12-hour
 *            AM/PM datetimes) parses with correct 24-hour times.
 *          - Delimiter sniffing (comma vs tab) from the header row.
 *          - The cache's single-read guarantee (parse_count()).
 *          - Malformed-row accounting and open/format failure statuses.
 *
 *          Working directory is tests/unit/engine/data/ (see CMakeLists).
 *
 * @see src/engine/input/MultiColumnSeriesFile.{hpp,cpp}
 */

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "input/MultiColumnSeriesFile.hpp"
#include "core/DateTime.hpp"

using openswmm::input::MultiColumnFileCache;
using openswmm::input::ParsedSeriesFile;
using openswmm::input::SeriesFileStatus;
using openswmm::input::parse_multicolumn_series_file;
using openswmm::input::parse_series_datetime;
using openswmm::input::looks_like_multicolumn_series_file;
using openswmm::input::split_series_file_token;
namespace dt = openswmm::datetime;

namespace {

ParsedSeriesFile parseOrDie(const std::string& path) {
    ParsedSeriesFile f;
    std::vector<std::string> errors;
    SeriesFileStatus st = SeriesFileStatus::OPEN_FAILED;
    EXPECT_TRUE(parse_multicolumn_series_file(path, f, errors, &st))
        << path << (errors.empty() ? "" : (": " + errors.front()));
    EXPECT_EQ(st, SeriesFileStatus::OK);
    return f;
}

} // namespace

// ---------------------------------------------------------------------------
// P2 — wide files (rows and header far beyond the old 4096-byte limit)
// ---------------------------------------------------------------------------

TEST(MultiColumnSeriesFile, WideCsvParsesUncorrupted) {
    const ParsedSeriesFile f = parseOrDie("rain_series/rain_wide.csv");

    ASSERT_EQ(f.headers.size(), 2001u);      // time + 2000 data columns
    EXPECT_EQ(f.headers[1],    "G0001");
    EXPECT_EQ(f.headers[2000], "G2000");
    ASSERT_EQ(f.dates.size(), 6u);

    // value(c, r) = c + r/10 — spot-check columns far past the 4 KB mark,
    // where the old fixed buffer split the row and shifted the indices.
    EXPECT_NEAR(f.columns[1][0],    1.0,    1e-12);
    EXPECT_NEAR(f.columns[1000][3], 1000.3, 1e-12);
    EXPECT_NEAR(f.columns[1999][2], 1999.2, 1e-12);
    EXPECT_NEAR(f.columns[2000][5], 2000.5, 1e-12);

    // Column lookup by name still lands on the right data after a >4 KB header.
    const int col = f.find_column("G1999");
    ASSERT_EQ(col, 1999);
    EXPECT_NEAR(f.columns[static_cast<std::size_t>(col)][0], 1999.0, 1e-12);

    // Whole-file per-column stats: every row of every column is > 0.
    EXPECT_EQ(f.col_periods_precip[2000], 6);
    EXPECT_EQ(f.unparsed_rows, 0);
}

// ---------------------------------------------------------------------------
// P4 — PCSWMM TSF (IDs: header, tab-delimited, 12-hour AM/PM)
// ---------------------------------------------------------------------------

TEST(MultiColumnSeriesFile, TsfParsesAmPmDatetimes) {
    const ParsedSeriesFile f = parseOrDie("rain_series/rain_ampm.tsf");

    ASSERT_EQ(f.headers.size(), 3u);
    EXPECT_EQ(f.headers[1], "SENSOR1");
    EXPECT_EQ(f.headers[2], "SENSOR2");
    ASSERT_EQ(f.dates.size(), 4u) << "the parameter and units header rows "
                                     "must be skipped, not read as data";

    const double day0 = dt::encodeDate(2020, 1, 1);
    EXPECT_NEAR(f.dates[0], day0 + dt::encodeTime(0, 0, 0),   1e-9);  // 12:00 AM
    EXPECT_NEAR(f.dates[1], day0 + dt::encodeTime(0, 15, 0),  1e-9);  // 12:15 AM
    EXPECT_NEAR(f.dates[2], day0 + dt::encodeTime(13, 30, 0), 1e-9);  // 01:30 PM
    EXPECT_NEAR(f.dates[3], day0 + dt::encodeTime(23, 45, 0), 1e-9);  // 11:45 PM

    EXPECT_NEAR(f.columns[1][2], 1.20, 1e-12);
    EXPECT_NEAR(f.columns[2][2], 0.60, 1e-12);
}

TEST(MultiColumnSeriesFile, AmPmDatetimeParsing) {
    const double day = dt::encodeDate(2020, 1, 2);
    double v = 0.0;

    ASSERT_TRUE(parse_series_datetime("01/02/2020 12:00:00 AM", v));
    EXPECT_NEAR(v, day, 1e-9);
    ASSERT_TRUE(parse_series_datetime("01/02/2020 12:30:00 PM", v));
    EXPECT_NEAR(v, day + dt::encodeTime(12, 30, 0), 1e-9);
    ASSERT_TRUE(parse_series_datetime("1/2/2020 5:00 PM", v));
    EXPECT_NEAR(v, day + dt::encodeTime(17, 0, 0), 1e-9);

    // 24-hour forms unchanged.
    ASSERT_TRUE(parse_series_datetime("2020-01-02 05:00", v));
    EXPECT_NEAR(v, day + dt::encodeTime(5, 0, 0), 1e-9);
    ASSERT_TRUE(parse_series_datetime("1/2/2020 17:15:30", v));
    EXPECT_NEAR(v, day + dt::encodeTime(17, 15, 30), 1e-9);

    EXPECT_FALSE(parse_series_datetime("not a date", v));
}

// ---------------------------------------------------------------------------
// Delimiter sniffing
// ---------------------------------------------------------------------------

TEST(MultiColumnSeriesFile, SniffsTabDelimitedHeader) {
    const ParsedSeriesFile f = parseOrDie("rain_series/rain_multi.tsv");
    ASSERT_EQ(f.headers.size(), 3u);
    EXPECT_EQ(f.find_column("west_gage"), 2) << "lookup is case-insensitive";
    ASSERT_EQ(f.dates.size(), 4u);
    EXPECT_NEAR(f.columns[2][1], 0.30, 1e-12);
}

TEST(MultiColumnSeriesFile, SniffsCommaDelimitedHeader) {
    const ParsedSeriesFile f = parseOrDie("rain_series/rain_multi.csv");
    ASSERT_EQ(f.headers.size(), 3u);
    EXPECT_EQ(f.headers[1], "EAST_GAGE");
    ASSERT_EQ(f.dates.size(), 4u);
    EXPECT_NEAR(f.columns[1][2], 1.20, 1e-12);
}

TEST(MultiColumnSeriesFile, SniffIgnoresDelimitersInsideQuotedHeaderNames) {
    // rain_quoted_header.tsv header: DateTime<TAB>"Rain, North, mm"
    // Counting RAW delimiters gives 1 tab vs 2 commas → comma wins and the
    // whole file mis-splits. The split has always been quote-aware; the sniff
    // has to use the same notion of a delimiter.
    const ParsedSeriesFile f = parseOrDie("rain_series/rain_quoted_header.tsv");

    ASSERT_EQ(f.headers.size(), 2u);
    EXPECT_EQ(f.headers[1], "Rain, North, mm")
        << "the quoted name is one column, commas and all";
    EXPECT_EQ(f.find_column("rain, north, mm"), 1);

    ASSERT_EQ(f.dates.size(), 3u);
    EXPECT_NEAR(f.columns[1][0], 0.10, 1e-12);
    EXPECT_NEAR(f.columns[1][2], 0.50, 1e-12);
}

TEST(MultiColumnSeriesFile, ContentSniffRoutesFormats) {
    EXPECT_TRUE(looks_like_multicolumn_series_file("rain_series/rain_multi.csv"));
    EXPECT_TRUE(looks_like_multicolumn_series_file("rain_series/rain_multi.tsv"));
    EXPECT_TRUE(looks_like_multicolumn_series_file("rain_series/rain_ampm.tsf"));
    // A legacy station-format rain file must NOT be routed to the
    // multi-column parser.
    EXPECT_FALSE(looks_like_multicolumn_series_file("rain_series/rain_std.dat"));
    EXPECT_FALSE(looks_like_multicolumn_series_file("rain_series/nope.csv"));
}

// ---------------------------------------------------------------------------
// Column lookup semantics
// ---------------------------------------------------------------------------

TEST(MultiColumnSeriesFile, ColumnLookupNeverMatchesTimeColumn) {
    const ParsedSeriesFile f = parseOrDie("rain_series/rain_multi.csv");
    EXPECT_EQ(f.find_column("DateTime"), -1)
        << "a match on the time column is as wrong as no match";
    EXPECT_EQ(f.find_column("NO_SUCH"), -1);
    EXPECT_EQ(f.first_data_column(), 1);
}

// ---------------------------------------------------------------------------
// `path:column` token split — ONE rule for gages and timeseries
// ---------------------------------------------------------------------------

TEST(MultiColumnSeriesFile, SplitsPathColumnTokenByOneSharedRule) {
    std::string path, col;

    // A plain relative path has no column.
    EXPECT_FALSE(split_series_file_token("rain_series/rain_multi.csv", path, col));
    EXPECT_EQ(path, "rain_series/rain_multi.csv");
    EXPECT_EQ(col, "");

    // Windows drive letter is not a column separator.
    EXPECT_FALSE(split_series_file_token("C:\\dir\\f.csv", path, col));
    EXPECT_EQ(path, "C:\\dir\\f.csv");
    EXPECT_EQ(col, "");

    // Drive letter AND a column.
    EXPECT_TRUE(split_series_file_token("C:\\dir\\f.csv:COL", path, col));
    EXPECT_EQ(path, "C:\\dir\\f.csv");
    EXPECT_EQ(col, "COL");

    // A colon inside a DIRECTORY name (legal on POSIX/macOS) is not a column:
    // the text after it still contains a path separator. This is the case the
    // gage reader and the timeseries loader used to disagree on, which keyed
    // the parse-once cache twice for one file.
    EXPECT_FALSE(split_series_file_token("/a/b:c/f.csv", path, col));
    EXPECT_EQ(path, "/a/b:c/f.csv");
    EXPECT_EQ(col, "");

    // Plain relative path with a column.
    EXPECT_TRUE(split_series_file_token("f.csv:COL", path, col));
    EXPECT_EQ(path, "f.csv");
    EXPECT_EQ(col, "COL");

    // Colon in a directory AND a real column.
    EXPECT_TRUE(split_series_file_token("/a/b:c/f.csv:COL", path, col));
    EXPECT_EQ(path, "/a/b:c/f.csv");
    EXPECT_EQ(col, "COL");

    // Trailing bare colon: a column separator with an empty column name (=
    // first data column), which is NOT the same as having no separator.
    EXPECT_TRUE(split_series_file_token("f.csv:", path, col));
    EXPECT_EQ(path, "f.csv");
    EXPECT_EQ(col, "");
}

// ---------------------------------------------------------------------------
// Single-read cache
// ---------------------------------------------------------------------------

TEST(MultiColumnSeriesFile, CacheParsesEachFileOnce) {
    MultiColumnFileCache cache;
    std::vector<std::string> errors;

    const ParsedSeriesFile* a = cache.get_or_parse("rain_series/rain_multi.csv", errors);
    const ParsedSeriesFile* b = cache.get_or_parse("rain_series/rain_multi.csv", errors);
    const ParsedSeriesFile* c = cache.get_or_parse("rain_series/rain_multi.csv", errors);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a, b);
    EXPECT_EQ(b, c);
    EXPECT_EQ(cache.parse_count(), 1);

    cache.get_or_parse("rain_series/rain_multi.tsv", errors);
    EXPECT_EQ(cache.parse_count(), 2);

    // Failures are cached too — a broken file is attempted once.
    cache.get_or_parse("rain_series/nope.csv", errors);
    cache.get_or_parse("rain_series/nope.csv", errors);
    EXPECT_EQ(cache.parse_count(), 3);
}

// ---------------------------------------------------------------------------
// Failure modes
// ---------------------------------------------------------------------------

TEST(MultiColumnSeriesFile, MissingFileReportsOpenFailed) {
    ParsedSeriesFile f;
    std::vector<std::string> errors;
    SeriesFileStatus st = SeriesFileStatus::OK;
    EXPECT_FALSE(parse_multicolumn_series_file("rain_series/nope.csv", f, errors, &st));
    EXPECT_EQ(st, SeriesFileStatus::OPEN_FAILED);
    EXPECT_FALSE(errors.empty());
}

TEST(MultiColumnSeriesFile, MalformedRowsAreCountedNotFatal) {
    // Header parses; both data rows have unparseable datetimes → zero rows,
    // still a "successful" parse (consumers decide whether empty is fatal).
    const ParsedSeriesFile f = parseOrDie("rain_series/rain_garbage.csv");
    EXPECT_EQ(f.dates.size(), 0u);
    EXPECT_EQ(f.unparsed_rows, 2);
}
