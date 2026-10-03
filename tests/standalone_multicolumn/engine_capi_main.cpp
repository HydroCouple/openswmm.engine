// SPDX-License-Identifier: Apache-2.0
//
// Standalone (gtest-free) end-to-end harness through the built engine.
// Mirrors the engine-level assertions of the extended gtest files — see
// README.md in this directory. Run with CWD = tests/unit/engine/data/.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_gages.h"
#include "openswmm/engine/openswmm_tables.h"

// Engine-internal test hook (C++ symbol exported by the shared library).
#include "input/MultiColumnSeriesFile.hpp"

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

// Report/output artifacts. The CWD has to be tests/unit/engine/data/ so the
// fixtures' relative tokens resolve, but that directory also holds CHECKED-IN
// .rpt/.out files — a harness run must not be able to overwrite one, so every
// artifact is written to tests/standalone_multicolumn/output/ instead.
static std::string art(const char* name) {
    static const std::string dir = "../../../standalone_multicolumn/output/";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + name;
}

// Does any error accumulated on the engine carry this text? Asserting the
// numeric code matters: "the open failed" alone would also pass if the model
// broke for an unrelated reason, or if the code changed.
static bool hasError(SWMM_Engine e, const char* needle) {
    const std::string want = needle;
    const int n = swmm_get_error_count(e);
    for (int i = 0; i < n; ++i) {
        const char* m = swmm_get_error_at(e, i);
        if (m && std::string(m).find(want) != std::string::npos) return true;
    }
    const char* last = swmm_get_last_error_msg(e);
    return last && std::string(last).find(want) != std::string::npos;
}

static std::vector<double> gageSeries(SWMM_Engine e, const char* id,
                                      std::vector<double>* times = nullptr) {
    const int g = swmm_gage_index(e, id);
    if (g < 0) return {};
    int n = 0;
    if (swmm_gage_get_rainfall_series_count(e, g, &n) != SWMM_OK || n <= 0)
        return {};
    std::vector<double> t(static_cast<std::size_t>(n));
    std::vector<double> v(static_cast<std::size_t>(n));
    swmm_gage_get_rainfall_series(e, g, t.data(), v.data(), n);
    if (times) *times = t;
    return v;
}

static std::vector<double> tableYs(SWMM_Engine e, const char* id) {
    const int t = swmm_table_index(e, id);
    if (t < 0) return {};
    int n = 0;
    if (swmm_table_get_point_count(e, t, &n) != SWMM_OK) return {};
    std::vector<double> ys(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        double x = 0.0, y = 0.0;
        swmm_table_get_point(e, t, k, &x, &y);
        ys[static_cast<std::size_t>(k)] = y;
    }
    return ys;
}

int main() {
    // --- rain_shared.inp: single read + B2 + FILE-timeseries sharing -----
    {
        const long before = openswmm::input::multicolumn_parse_count_total();
        SWMM_Engine e = swmm_engine_create();
        const int rc = swmm_engine_open(e, "rain_series/rain_shared.inp",
                                        art("_sa_shared.rpt").c_str(),
                                        art("_sa_shared.out").c_str(),
                                        nullptr);
        const long after = openswmm::input::multicolumn_parse_count_total();
        CHECK(rc == SWMM_OK, "rain_shared.inp opens");
        CHECK(after - before == 1,
              "single-read: 4 gages + 2 FILE timeseries on one file => 1 parse");

        // rain_multi.csv: EAST 0/0.60/1.20/0, WEST 0/0.30/0.90/0, VOLUME over
        // a 900 s interval => in/hr = v/900*3600 = v*4.
        const std::vector<double> east = gageSeries(e, "CSV_EAST");
        const std::vector<double> west = gageSeries(e, "CSV_WEST");
        const std::vector<double> dflt = gageSeries(e, "CSV_DEFAULT");
        const std::vector<double> bare = gageSeries(e, "CSV_BARE");
        CHECK(east.size() == 4 && west.size() == 4 && dflt.size() == 4
                  && bare.size() == 4,
              "all four gages loaded 4 rows");
        CHECK(east.size() == 4 && near(east[1], 2.40) && near(east[2], 4.80)
                  && near(east[0], 0.0),
              "EAST column values are the file's (0.60,1.20 => 2.40,4.80 in/hr)");
        CHECK(west.size() == 4 && near(west[1], 1.20) && near(west[2], 3.60),
              "WEST column values are the file's (0.30,0.90 => 1.20,3.60 in/hr)");
        bool b2 = east.size() == 4 && dflt.size() == 4;
        for (std::size_t i = 0; b2 && i < 4; ++i)
            b2 = near(dflt[i], east[i], 1e-12);
        CHECK(b2 && !dflt.empty() && near(dflt[1], 2.40),
              "B2: empty column selector == first data column (EAST, 2.40)");
        CHECK(!east.empty() && !west.empty() && west[1] > 0.0
                  && near(east[1] / west[1], 2.0),
              "columns independent (EAST is 2x WEST)");

        // A bare path (no colon, no station/units) is what InpWriter emits for
        // a USER_CSV gage with an empty column; it must come back as USER_CSV
        // reading the first data column, not as a silently empty STAN_PRCP.
        int bare_fmt = -1;
        swmm_gage_get_file_format(e, swmm_gage_index(e, "CSV_BARE"), &bare_fmt);
        CHECK(bare_fmt == 6 && bare.size() == 4 && near(bare[1], 2.40),
              "bare multi-column path reloads as USER_CSV / first column");

        const std::vector<double> ts_east  = tableYs(e, "TS_EAST");
        const std::vector<double> ts_first = tableYs(e, "TS_FIRST");
        // Timeseries hold RAW file values (no rain-type conversion).
        CHECK(ts_east.size() == 4 && near(ts_east[1], 0.60, 1e-12)
                  && near(ts_east[2], 1.20, 1e-12),
              "FILE timeseries with :col loads EAST's raw values");
        CHECK(ts_first.size() == 4 && near(ts_first[1], 0.60, 1e-12)
                  && near(ts_first[2], 1.20, 1e-12),
              "FILE timeseries without :col loads the first data column");

        // Reload path: one more parse for the whole pass, not one per gage.
        const long r0 = openswmm::input::multicolumn_parse_count_total();
        CHECK(swmm_gage_reload_rain_files(e) == SWMM_OK, "reload succeeds");
        const long r1 = openswmm::input::multicolumn_parse_count_total();
        CHECK(r1 - r0 == 1, "reload re-reads the shared file exactly once");

        // --- B1 regression + new accessors on the open model ---
        const int csv = swmm_gage_index(e, "CSV_EAST");
        int fmt = -1;
        char col[64] = {};
        swmm_gage_get_file_format(e, csv, &fmt);
        swmm_gage_get_file_column(e, csv, col, sizeof(col));
        CHECK(fmt == 6 && std::string(col) == "EAST_GAGE",
              "accessors read parsed format/column");

        swmm_gage_set_station_id(e, csv, "STA_X");
        swmm_gage_get_file_format(e, csv, &fmt);
        swmm_gage_get_file_column(e, csv, col, sizeof(col));
        CHECK(fmt == 6 && std::string(col) == "EAST_GAGE",
              "B1: set_station_id preserves USER_CSV + column");

        swmm_gage_set_filename(e, csv, "other.csv", "");
        swmm_gage_get_file_format(e, csv, &fmt);
        swmm_gage_get_file_column(e, csv, col, sizeof(col));
        CHECK(fmt == 6 && std::string(col) == "EAST_GAGE",
              "B1: set_filename preserves USER_CSV + column");

        // A2: the way back OUT of USER_CSV (previously a one-way door).
        char sta[64] = {};
        CHECK(swmm_gage_set_file_format(e, csv, 5) == SWMM_OK, "set_file_format accepts STAN_PRCP");
        swmm_gage_get_file_format(e, csv, &fmt);
        swmm_gage_get_file_column(e, csv, col, sizeof(col));
        CHECK(fmt == 5 && std::string(col).empty(),
              "USER_CSV -> STAN_PRCP clears the column selector");
        swmm_gage_set_station_id(e, csv, "STA_A");
        CHECK(swmm_gage_set_file_format(e, csv, 6) == SWMM_OK, "set_file_format accepts USER_CSV");
        swmm_gage_get_file_format(e, csv, &fmt);
        swmm_gage_get_station_id(e, csv, sta, sizeof(sta));
        CHECK(fmt == 6 && std::string(sta).empty(),
              "STAN_PRCP -> USER_CSV clears the station id");
        CHECK(swmm_gage_set_file_format(e, csv, 7) != SWMM_OK &&
                  swmm_gage_set_file_format(e, csv, -2) != SWMM_OK,
              "set_file_format rejects codes outside RainFileFormat");

        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }

    // --- rain_us.inp: original USER_CSV + STAN_PRCP regressions ----------
    {
        SWMM_Engine e = swmm_engine_create();
        const int rc = swmm_engine_open(e, "rain_series/rain_us.inp",
                                        art("_sa_us.rpt").c_str(),
                                        art("_sa_us.out").c_str(), nullptr);
        CHECK(rc == SWMM_OK, "rain_us.inp opens (existing fixture)");

        const std::vector<double> east = gageSeries(e, "CSV_EAST");
        const std::vector<double> west = gageSeries(e, "CSV_WEST");
        const std::vector<double> stdv = gageSeries(e, "STD_GAGE");
        CHECK(east.size() == 4 && west.size() == 4,
              "rain_us: USER_CSV gages load 4 rows each");
        CHECK(!east.empty() && !west.empty() && west[1] > 0.0
                  && near(east[1] / west[1], 4.0),
              "rain_us: EAST/WEST ratio 4 (column + scale factor)");
        CHECK(!west.empty() && near(west[1], 1.2),
              "rain_us: project-unit conversion unchanged (0.30/900*3600)");
        // rain_std.dat holds STA_A (0/0.40/0.80/0) and STA_B (0/0.10/0.20/0).
        // STD_GAGE filters on STA_A, VOLUME over 900 s => v*4. Picking up
        // STA_B's rows instead (or as well) changes these numbers, which
        // `size()==4 && v>0` would not have caught. The 1e-6 tolerance is
        // required, not slack: the STAN_PRCP path deliberately keeps the
        // series float-quantized in inches for legacy interface-file parity,
        // so 0.40 reads back as 1.6000000238418579, not 1.6.
        CHECK(stdv.size() == 4 && near(stdv[1], 1.60, 1e-6)
                  && near(stdv[2], 3.20, 1e-6)
                  && near(stdv[0], 0.0) && near(stdv[3], 0.0),
              "rain_us: STAN_PRCP station filtering unchanged (STA_A only)");

        // Format preservation on the STAN gage.
        const int sg = swmm_gage_index(e, "STD_GAGE");
        int fmt = -1;
        swmm_gage_get_file_format(e, sg, &fmt);
        CHECK(fmt == 5, "STAN_PRCP gage reports format 5");
        swmm_gage_set_file_column(e, sg, "P1");
        swmm_gage_get_file_format(e, sg, &fmt);
        CHECK(fmt == 6, "set_file_column implies USER_CSV");

        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }

    // --- rain_tsf.inp: TSF gages end-to-end -------------------------------
    {
        SWMM_Engine e = swmm_engine_create();
        const int rc = swmm_engine_open(e, "rain_series/rain_tsf.inp",
                                        art("_sa_tsf.rpt").c_str(), art("_sa_tsf.out").c_str(), nullptr);
        CHECK(rc == SWMM_OK, "rain_tsf.inp opens");
        std::vector<double> t1;
        const std::vector<double> v1 = gageSeries(e, "TSF_G1", &t1);
        const std::vector<double> v2 = gageSeries(e, "TSF_G2");
        CHECK(v1.size() == 4 && v2.size() == 4, "TSF gages load 4 rows");
        bool ratio = v1.size() == 4 && v2.size() == 4;
        for (std::size_t i = 1; ratio && i < 4; ++i)
            ratio = v2[i] > 0.0 && near(v1[i] / v2[i], 2.0);
        CHECK(ratio, "TSF: SENSOR1 == 2x SENSOR2 (right columns)");
        CHECK(t1.size() == 4 && near((t1[3] - t1[0]) * 24.0, 23.75)
                  && near((t1[2] - t1[0]) * 24.0, 13.5),
              "TSF: AM/PM decoded to 24-hour times");
        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }

    // --- [TIMESERIES] column selection ------------------------------------
    {
        SWMM_Engine e = swmm_engine_create();
        const int rc = swmm_engine_open(e, "rain_series/rain_ts_files.inp",
                                        art("_sa_ts.rpt").c_str(), art("_sa_ts.out").c_str(), nullptr);
        CHECK(rc == SWMM_OK, "rain_ts_files.inp opens");
        const std::vector<double> west = tableYs(e, "TS_COL");
        const std::vector<double> east = tableYs(e, "TS_NOCOL");
        CHECK(west.size() == 4 && near(west[1], 0.30, 1e-12)
                  && near(west[2], 0.90, 1e-12),
              "P3: :column selects the named column");
        CHECK(east.size() == 4 && near(east[1], 0.60, 1e-12),
              "P3: comma CSV without :col loads first data column");
        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }

    // --- Loud failures -----------------------------------------------------
    {
        SWMM_Engine e = swmm_engine_create();
        const int rc_miss = swmm_engine_open(e, "rain_series/rain_ts_missing.inp",
                                             art("_sa_miss.rpt").c_str(),
                                             art("_sa_miss.out").c_str(), nullptr);
        CHECK(rc_miss != SWMM_OK && hasError(e, "ERROR 361"),
              "missing FILE timeseries fails the open with ERROR 361");
        swmm_engine_close(e);
        swmm_engine_destroy(e);

        e = swmm_engine_create();
        const int rc_junk = swmm_engine_open(e, "rain_series/rain_ts_garbage.inp",
                                             art("_sa_junk.rpt").c_str(),
                                             art("_sa_junk.out").c_str(), nullptr);
        CHECK(rc_junk != SWMM_OK && hasError(e, "ERROR 363"),
              "zero-row FILE timeseries fails the open with ERROR 363");
        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }

    // --- Runtime smoke: USER_CSV gage still rains --------------------------
    {
        SWMM_Engine e = swmm_engine_create();
        int rc = swmm_engine_open(e, "rain_series/rain_us.inp",
                                  art("_sa_run.rpt").c_str(), art("_sa_run.out").c_str(), nullptr);
        double peak = 0.0;
        if (rc == SWMM_OK && swmm_engine_initialize(e) == SWMM_OK &&
            swmm_engine_start(e, 1) == SWMM_OK) {
            const int east = swmm_gage_index(e, "CSV_EAST");
            double elapsed = 0.0;
            for (int i = 0; i < 5000; ++i) {
                if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
                double r = 0.0;
                if (swmm_gage_get_rainfall(e, east, &r) == SWMM_OK && r > peak)
                    peak = r;
                if (elapsed <= 0.0) break;
            }
            swmm_engine_end(e);
        }
        CHECK(peak > 0.0, "runtime: USER_CSV gage produces rainfall");
        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
