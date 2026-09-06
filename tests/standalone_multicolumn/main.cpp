// SPDX-License-Identifier: Apache-2.0
//
// Standalone (gtest-free) parser harness for MultiColumnSeriesFile.
// Mirrors tests/unit/engine/test_multicolumn_series_file.cpp — see
// README.md in this directory. Run with CWD = tests/unit/engine/data/.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "input/MultiColumnSeriesFile.hpp"
#include "core/DateTime.hpp"

using namespace openswmm::input;
namespace dt = openswmm::datetime;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_checks;                                                          \
        if (cond) { std::printf("PASS  %s\n", msg); }                        \
        else      { std::printf("FAIL  %s\n", msg); ++g_failures; }          \
    } while (0)

static bool near(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

int main() {
    std::vector<std::string> errors;

    // --- Wide CSV (P2) ---------------------------------------------------
    {
        ParsedSeriesFile f;
        SeriesFileStatus st{};
        const bool ok = parse_multicolumn_series_file(
            "rain_series/rain_wide.csv", f, errors, &st);
        CHECK(ok && st == SeriesFileStatus::OK, "wide CSV parses");
        CHECK(f.headers.size() == 2001u, "wide CSV: 2001 headers");
        CHECK(f.dates.size() == 6u, "wide CSV: 6 rows");
        CHECK(near(f.columns[1999][2], 1999.2, 1e-12),
              "wide CSV: column 1999 row 2 uncorrupted (past 4 KB)");
        CHECK(near(f.columns[2000][5], 2000.5, 1e-12),
              "wide CSV: last column last row uncorrupted");
        CHECK(f.find_column("G1999") == 1999,
              "wide CSV: lookup after >4 KB header lands correctly");
        CHECK(f.col_periods_precip[2000] == 6, "wide CSV: per-column stats");
        CHECK(f.unparsed_rows == 0, "wide CSV: no unparsed rows");
    }

    // --- TSF with AM/PM (P4) ---------------------------------------------
    {
        ParsedSeriesFile f;
        SeriesFileStatus st{};
        const bool ok = parse_multicolumn_series_file(
            "rain_series/rain_ampm.tsf", f, errors, &st);
        CHECK(ok && st == SeriesFileStatus::OK, "TSF parses");
        CHECK(f.headers.size() == 3u && f.headers[1] == "SENSOR1",
              "TSF: IDs: header names the columns");
        CHECK(f.dates.size() == 4u, "TSF: parameter/units rows skipped");
        const double day0 = dt::encodeDate(2020, 1, 1);
        CHECK(near(f.dates[0], day0), "TSF: 12:00:00 AM == midnight");
        CHECK(near(f.dates[2], day0 + dt::encodeTime(13, 30, 0)),
              "TSF: 01:30:00 PM == 13:30");
        CHECK(near(f.dates[3], day0 + dt::encodeTime(23, 45, 0)),
              "TSF: 11:45:00 PM == 23:45");
        CHECK(near(f.columns[2][2], 0.60, 1e-12), "TSF: SENSOR2 values");
    }

    // --- Datetime parsing ------------------------------------------------
    {
        const double day = dt::encodeDate(2020, 1, 2);
        double v = 0.0;
        CHECK(parse_series_datetime("01/02/2020 12:00:00 AM", v) && near(v, day),
              "datetime: 12 AM wraps to 0");
        CHECK(parse_series_datetime("01/02/2020 12:30:00 PM", v)
                  && near(v, day + dt::encodeTime(12, 30, 0)),
              "datetime: 12:30 PM stays 12:30");
        CHECK(parse_series_datetime("1/2/2020 5:00 PM", v)
                  && near(v, day + dt::encodeTime(17, 0, 0)),
              "datetime: 5 PM == 17:00 (no seconds)");
        CHECK(parse_series_datetime("2020-01-02 05:00", v)
                  && near(v, day + dt::encodeTime(5, 0, 0)),
              "datetime: ISO unchanged");
        CHECK(!parse_series_datetime("not a date", v), "datetime: junk rejected");
    }

    // --- Delimiter sniff --------------------------------------------------
    {
        ParsedSeriesFile f;
        const bool ok = parse_multicolumn_series_file(
            "rain_series/rain_multi.tsv", f, errors, nullptr);
        CHECK(ok && f.headers.size() == 3u && near(f.columns[2][1], 0.30, 1e-12),
              "TSV sniffed and parsed");
        CHECK(f.find_column("west_gage") == 2, "lookup is case-insensitive");
        CHECK(f.find_column("DateTime") == -1, "time column never matches");
        CHECK(f.first_data_column() == 1, "first data column default");

        // Quoted header name containing commas: raw counting sees 1 tab vs 2
        // commas and picks comma, mis-splitting the whole file. The sniff must
        // count only delimiters OUTSIDE quotes, like the splitter does.
        ParsedSeriesFile q;
        const bool qok = parse_multicolumn_series_file(
            "rain_series/rain_quoted_header.tsv", q, errors, nullptr);
        CHECK(qok && q.headers.size() == 2u
                  && q.headers[1] == "Rain, North, mm",
              "sniff: a quoted header name with commas stays ONE column");
        CHECK(q.dates.size() == 3u && near(q.columns[1][0], 0.10, 1e-12)
                  && near(q.columns[1][2], 0.50, 1e-12),
              "sniff: quoted-header file splits on the tab, values intact");

        CHECK(looks_like_multicolumn_series_file("rain_series/rain_multi.csv"),
              "sniff: comma CSV is multi-column");
        CHECK(looks_like_multicolumn_series_file("rain_series/rain_ampm.tsf"),
              "sniff: TSF is multi-column");
        CHECK(!looks_like_multicolumn_series_file("rain_series/rain_std.dat"),
              "sniff: legacy station .dat is NOT multi-column");
    }

    // --- Cache single-read ------------------------------------------------
    {
        MultiColumnFileCache cache;
        const ParsedSeriesFile* a =
            cache.get_or_parse("rain_series/rain_multi.csv", errors);
        const ParsedSeriesFile* b =
            cache.get_or_parse("rain_series/rain_multi.csv", errors);
        CHECK(a != nullptr && a == b && cache.parse_count() == 1,
              "cache: repeated get_or_parse parses once");
        cache.get_or_parse("rain_series/nope.csv", errors);
        cache.get_or_parse("rain_series/nope.csv", errors);
        CHECK(cache.parse_count() == 2, "cache: failures cached too");
    }

    // --- `path:column` token split (one rule for gages AND timeseries) ----
    {
        std::string p, c;
        CHECK(!split_series_file_token("rain_series/rain_multi.csv", p, c)
                  && p == "rain_series/rain_multi.csv" && c.empty(),
              "split: plain relative path has no column");
        CHECK(!split_series_file_token("C:\\dir\\f.csv", p, c)
                  && p == "C:\\dir\\f.csv" && c.empty(),
              "split: drive letter is not a column separator");
        CHECK(split_series_file_token("C:\\dir\\f.csv:COL", p, c)
                  && p == "C:\\dir\\f.csv" && c == "COL",
              "split: drive letter plus column");
        CHECK(!split_series_file_token("/a/b:c/f.csv", p, c)
                  && p == "/a/b:c/f.csv" && c.empty(),
              "split: colon inside a directory name is not a column");
        CHECK(split_series_file_token("f.csv:COL", p, c)
                  && p == "f.csv" && c == "COL",
              "split: relative path plus column");
        CHECK(split_series_file_token("f.csv:", p, c)
                  && p == "f.csv" && c.empty(),
              "split: trailing colon = separator with an empty column");
    }

    // --- Failure modes ----------------------------------------------------
    {
        ParsedSeriesFile f;
        SeriesFileStatus st{};
        std::vector<std::string> errs;
        CHECK(!parse_multicolumn_series_file("rain_series/nope.csv", f, errs, &st)
                  && st == SeriesFileStatus::OPEN_FAILED,
              "missing file: OPEN_FAILED");
        // The message content is part of the contract — a diagnostic that does
        // not name the file it could not open is not actionable.
        CHECK(errs.size() == 1
                  && errs[0].find("rain_series/nope.csv") != std::string::npos
                  && errs[0].find("cannot open") != std::string::npos,
              "missing file: error message names the path");
        const bool ok = parse_multicolumn_series_file(
            "rain_series/rain_garbage.csv", f, errs, &st);
        CHECK(ok && f.dates.empty() && f.unparsed_rows == 2,
              "garbage rows counted, zero data rows");
        CHECK(errs.size() == 1,
              "a parseable file with garbage rows reports no file-level error");
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
