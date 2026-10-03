/**
 * @file test_gage_rain_series.cpp
 * @brief swmm_gage_get_rainfall_series + USER_CSV rain-file loading.
 *
 * @details Two things are under test.
 *
 *          1. USER_CSV (`FILE "rain.csv:COLUMN"`) actually loads. The format is
 *             documented in the user manual (AppendixD "New in OpenSWMM v6",
 *             Chapter13 §13.9) but was parsed, round-tripped by InpWriter, and
 *             never read — such a gage contributed ZERO rainfall for the whole
 *             run, silently, with the continuity report closing cleanly.
 *
 *          2. The resolved-series API cannot drift from the runtime. The API
 *             exists so a host can read the rainfall a gage will actually apply
 *             without knowing how it is stored; if it and updateAllGages ever
 *             disagree, the value of the API is gone. RuntimeAgreement below is
 *             the guard: it steps the model and compares.
 *
 * @see src/engine/input/PostParseResolver.cpp (load_external_rain_files)
 * @see src/engine/hydrology/Gage.cpp (gageRainSeries / convertGageValue)
 */

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "openswmm/engine/openswmm_engine.h"
#include "openswmm/engine/openswmm_gages.h"
#include "openswmm/engine/openswmm_datetime.h"

// Engine-internal test hook for the single-read assertion (plan
// MULTICOLUMN_SERIES_SINGLE_READ_2026-08-17 §5). Not part of the C API.
#include "input/MultiColumnSeriesFile.hpp"

namespace {

// Working directory is tests/unit/engine/data/ (see that CMakeLists).
constexpr const char* kInp = "rain_series/rain_us.inp";

class GageRainSeries : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;

    void openModel(const char* rpt, const char* out) {
        openModelAt(kInp, rpt, out);
    }

    void openModelAt(const char* inp, const char* rpt, const char* out) {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, inp, rpt, out, nullptr), SWMM_OK);
    }

    void TearDown() override {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    int gageIndex(const char* id) { return swmm_gage_index(engine_, id); }

    std::vector<double> seriesValues(int idx) {
        int n = 0;
        EXPECT_EQ(swmm_gage_get_rainfall_series_count(engine_, idx, &n), SWMM_OK);
        std::vector<double> t(static_cast<std::size_t>(n));
        std::vector<double> v(static_cast<std::size_t>(n));
        if (n > 0)
            EXPECT_EQ(swmm_gage_get_rainfall_series(engine_, idx, t.data(), v.data(), n),
                      SWMM_OK);
        return v;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// USER_CSV loading
// ---------------------------------------------------------------------------

TEST_F(GageRainSeries, UserCsvGageLoadsItsColumn) {
    openModel("_rain_series_csv.rpt", "_rain_series_csv.out");

    const int east = gageIndex("CSV_EAST");
    ASSERT_GE(east, 0);

    int n = 0;
    ASSERT_EQ(swmm_gage_get_rainfall_series_count(engine_, east, &n), SWMM_OK);
    EXPECT_EQ(n, 4) << "the CSV has four data rows; a zero here means the "
                       "USER_CSV loader did not run at all";
}

TEST_F(GageRainSeries, UserCsvColumnsAreIndependent) {
    openModel("_rain_series_cols.rpt", "_rain_series_cols.out");

    const std::vector<double> east = seriesValues(gageIndex("CSV_EAST"));
    const std::vector<double> west = seriesValues(gageIndex("CSV_WEST"));
    ASSERT_EQ(east.size(), 4u);
    ASSERT_EQ(west.size(), 4u);

    // Both gages read the same file; picking the wrong column would make them
    // identical. EAST is 2x WEST in the data, and EAST also carries a 2.0 scale
    // factor, so the resolved ratio is 4.
    EXPECT_GT(east[1], 0.0);
    EXPECT_GT(west[1], 0.0);
    EXPECT_NEAR(east[1] / west[1], 4.0, 1e-9);
}

TEST_F(GageRainSeries, UserCsvValuesAreInProjectUnits) {
    openModel("_rain_series_units.rpt", "_rain_series_units.out");

    // WEST_GAGE row 2 is 0.30 in per 15-min interval, declared VOLUME, no scale
    // factor: 0.30 / 900 * 3600 = 1.2 in/hr. A stray 25.4x (the standard
    // rain-file units factor) would show up here immediately.
    const std::vector<double> west = seriesValues(gageIndex("CSV_WEST"));
    ASSERT_EQ(west.size(), 4u);
    EXPECT_NEAR(west[1], 1.2, 1e-9);
}

TEST_F(GageRainSeries, UserCsvGageProducesRainfallAtRuntime) {
    openModel("_rain_series_run.rpt", "_rain_series_run.out");

    // The whole point of the fix: before it, this gage read 0.0 forever.
    ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);
    double elapsed = 0.0;
    double peak = 0.0;
    const int east = gageIndex("CSV_EAST");
    // Run to completion: the first non-zero record is at 00:15, which a short
    // fixed step budget would never reach.
    for (int i = 0; i < 5000; ++i) {
        if (swmm_engine_step(engine_, &elapsed) != SWMM_OK) break;
        double r = 0.0;
        if (swmm_gage_get_rainfall(engine_, east, &r) == SWMM_OK)
            peak = std::max(peak, r);
        if (elapsed <= 0.0) break;
    }
    swmm_engine_end(engine_);
    EXPECT_GT(peak, 0.0) << "a USER_CSV gage must actually rain";
}

// ---------------------------------------------------------------------------
// Single-read guarantee: N gages + M FILE timeseries on ONE file ⇒ one parse
// ---------------------------------------------------------------------------

TEST_F(GageRainSeries, SharedFileIsParsedExactlyOnce) {
    const long before = openswmm::input::multicolumn_parse_count_total();
    // rain_shared.inp: 4 USER_CSV gages + 2 FILE timeseries, all on
    // rain_multi.csv.
    openModelAt("rain_series/rain_shared.inp",
                "_rain_shared.rpt", "_rain_shared.out");
    const long after = openswmm::input::multicolumn_parse_count_total();
    EXPECT_EQ(after - before, 1)
        << "six consumers of one file must trigger exactly one parse";

    // And every consumer actually got its data.
    for (const char* id : {"CSV_EAST", "CSV_WEST", "CSV_DEFAULT", "CSV_BARE"}) {
        const int g = gageIndex(id);
        ASSERT_GE(g, 0) << id;
        int n = 0;
        ASSERT_EQ(swmm_gage_get_rainfall_series_count(engine_, g, &n), SWMM_OK);
        EXPECT_EQ(n, 4) << id;
    }
}

// ---------------------------------------------------------------------------
// B2 — empty column selector defaults to the FIRST data column
// ---------------------------------------------------------------------------

TEST_F(GageRainSeries, EmptyColumnNameLoadsFirstDataColumn) {
    openModelAt("rain_series/rain_shared.inp",
                "_rain_shared_b2.rpt", "_rain_shared_b2.out");

    // CSV_DEFAULT is declared FILE "rain_multi.csv:" (empty column). The
    // first data column is EAST_GAGE, so its series must equal CSV_EAST's
    // (identical SCFs and no scale factors in this fixture).
    const std::vector<double> east = seriesValues(gageIndex("CSV_EAST"));
    const std::vector<double> dflt = seriesValues(gageIndex("CSV_DEFAULT"));
    ASSERT_EQ(east.size(), 4u);
    ASSERT_EQ(dflt.size(), 4u);
    for (std::size_t i = 0; i < east.size(); ++i)
        EXPECT_NEAR(dflt[i], east[i], 1e-12) << "row " << i;
    EXPECT_GT(dflt[1], 0.0);
}

// ---------------------------------------------------------------------------
// The writer's empty-column form (a BARE path) must reload as USER_CSV
// ---------------------------------------------------------------------------

TEST_F(GageRainSeries, BarePathUserCsvGageLoadsFirstColumn) {
    openModelAt("rain_series/rain_shared.inp",
                "_rain_shared_bare.rpt", "_rain_shared_bare.out");

    // CSV_BARE is declared FILE "rain_multi.csv" — no colon, no station/units
    // tokens, which is what InpWriter emits for a USER_CSV gage with an empty
    // column. The [RAINGAGES] grammar cannot distinguish that from STAN_PRCP,
    // so the loader recovers the format from the file's content. Getting this
    // wrong is silent: the STAN_PRCP reader fails every sscanf on a CSV and
    // hands back an empty series that reads 0.0 forever.
    const int bare = gageIndex("CSV_BARE");
    ASSERT_GE(bare, 0);

    int fmt = -1;
    ASSERT_EQ(swmm_gage_get_file_format(engine_, bare, &fmt), SWMM_OK);
    EXPECT_EQ(fmt, 6) << "a bare multi-column path must resolve to USER_CSV";

    const std::vector<double> east = seriesValues(gageIndex("CSV_EAST"));
    const std::vector<double> vals = seriesValues(bare);
    ASSERT_EQ(vals.size(), 4u) << "bare path yielded an empty series";
    ASSERT_EQ(east.size(), 4u);
    for (std::size_t i = 0; i < vals.size(); ++i)
        EXPECT_NEAR(vals[i], east[i], 1e-12) << "row " << i;
    EXPECT_GT(vals[1], 0.0);
}

// ---------------------------------------------------------------------------
// P4 — PCSWMM TSF gage end-to-end (AM/PM datetimes, IDs: header)
// ---------------------------------------------------------------------------

TEST_F(GageRainSeries, TsfGageLoadsItsColumnWith24HourTimes) {
    openModelAt("rain_series/rain_tsf.inp",
                "_rain_tsf.rpt", "_rain_tsf.out");

    const int g1 = gageIndex("TSF_G1");
    const int g2 = gageIndex("TSF_G2");
    ASSERT_GE(g1, 0);
    ASSERT_GE(g2, 0);

    int n = 0;
    ASSERT_EQ(swmm_gage_get_rainfall_series_count(engine_, g1, &n), SWMM_OK);
    ASSERT_EQ(n, 4) << "the TSF parameter/units header rows must be skipped";

    std::vector<double> t1(4), v1(4);
    ASSERT_EQ(swmm_gage_get_rainfall_series(engine_, g1, t1.data(), v1.data(), 4),
              SWMM_OK);
    const std::vector<double> v2 = seriesValues(g2);
    ASSERT_EQ(v2.size(), 4u);

    // SENSOR1 is exactly 2x SENSOR2 in the fixture — the wrong column (or a
    // shifted index) breaks the ratio immediately.
    for (int i = 1; i < 4; ++i) {
        ASSERT_GT(v2[static_cast<std::size_t>(i)], 0.0);
        EXPECT_NEAR(v1[static_cast<std::size_t>(i)] / v2[static_cast<std::size_t>(i)],
                    2.0, 1e-9) << "row " << i;
    }

    // AM/PM decoding: 12:00:00 AM = midnight, 11:45:00 PM = 23:45 — the
    // last record lies 23.75 h after the first.
    EXPECT_NEAR((t1[3] - t1[0]) * 24.0, 23.75, 1e-9);
    // 01:30:00 PM = 13:30.
    EXPECT_NEAR((t1[2] - t1[0]) * 24.0, 13.5, 1e-9);
}

// ---------------------------------------------------------------------------
// STAN_PRCP still works (station filtering, legacy units path)
// ---------------------------------------------------------------------------

TEST_F(GageRainSeries, StandardRainFileGageLoadsItsStation) {
    openModel("_rain_series_std.rpt", "_rain_series_std.out");

    const std::vector<double> std_v = seriesValues(gageIndex("STD_GAGE"));
    // Four STA_A rows; the four STA_B rows in the same file must be filtered out.
    ASSERT_EQ(std_v.size(), 4u);
    EXPECT_GT(std_v[1], 0.0);
}

// ---------------------------------------------------------------------------
// The anti-drift guard
// ---------------------------------------------------------------------------

TEST_F(GageRainSeries, RuntimeAgreement) {
    openModel("_rain_series_agree.rpt", "_rain_series_agree.out");

    struct GageProbe {
        const char* id;
        int idx;
        std::vector<double> times;
        std::vector<double> values;
        double interval;
    };
    std::vector<GageProbe> probes;
    for (const char* id : {"TS_GAGE", "STD_GAGE", "CSV_EAST", "CSV_WEST"}) {
        GageProbe p;
        p.id  = id;
        p.idx = gageIndex(id);
        ASSERT_GE(p.idx, 0) << id;

        int n = 0;
        ASSERT_EQ(swmm_gage_get_rainfall_series_count(engine_, p.idx, &n), SWMM_OK);
        ASSERT_GT(n, 0) << id << " has no resolved series";
        p.times.resize(static_cast<std::size_t>(n));
        p.values.resize(static_cast<std::size_t>(n));
        ASSERT_EQ(swmm_gage_get_rainfall_series(engine_, p.idx, p.times.data(),
                                                p.values.data(), n), SWMM_OK);
        ASSERT_EQ(swmm_gage_get_rain_interval(engine_, p.idx, &p.interval), SWMM_OK);
        probes.push_back(std::move(p));
    }

    // Reproduce the engine's own boxcar rule from the reported series, then
    // check it against what the running gage reports at the same instant.
    // The gage state is refreshed at the START of the last runoff substep
    // before the routing time (legacy runoff_execute updates gages at
    // OldRunoffTime), and runoff substeps snap to every gage's entry
    // boundaries — so at routing time T the gage still holds the interval
    // containing T − ε: probe one second BELOW the routing time. (The old
    // +1 s probe encoded the 0.5 s routing-grid offset a link-less model
    // used to pick up from the DW variable-step startup; with the legacy
    // fixed-step guard the grid is aligned and exact boundary instants
    // report the interval that ENDS there, as legacy does.)
    const double kSecond = 1.0 / 86400.0;
    double start = 0.0;
    ASSERT_EQ(swmm_datetime_encode_date(2020, 1, 1, &start), SWMM_OK);

    ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);

    double elapsed = 0.0;
    int checks = 0;
    int wet_checks = 0;
    for (int step = 0; step < 5000; ++step) {
        if (swmm_engine_step(engine_, &elapsed) != SWMM_OK) break;
        if (elapsed <= 0.0) break;
        const double probe = start + elapsed - kSecond;

        for (const GageProbe& p : probes) {
            double expected = 0.0;
            for (std::size_t k = 0; k < p.times.size(); ++k) {
                const double t0 = p.times[k];
                double t1 = t0 + p.interval * kSecond;
                if (k + 1 < p.times.size()) t1 = std::min(t1, p.times[k + 1]);
                if (probe >= t0 && probe < t1) { expected = p.values[k]; break; }
            }
            double actual = 0.0;
            ASSERT_EQ(swmm_gage_get_rainfall(engine_, p.idx, &actual), SWMM_OK);
            EXPECT_NEAR(actual, expected, 1e-6)
                << p.id << " disagrees with its reported series at elapsed="
                << elapsed << " — the API and the runtime have drifted";
            ++checks;
            if (actual > 0.0) ++wet_checks;
        }
    }
    swmm_engine_end(engine_);
    EXPECT_GT(checks, 0);
    // Without this the whole comparison passes vacuously if every gage happens
    // to read zero the entire run — which is exactly the bug being fixed.
    EXPECT_GT(wet_checks, 0)
        << "no gage reported any rainfall, so the agreement above proved nothing";
}
