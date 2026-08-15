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

namespace {

// Working directory is tests/unit/engine/data/ (see that CMakeLists).
constexpr const char* kInp = "rain_series/rain_us.inp";

class GageRainSeries : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;

    void openModel(const char* rpt, const char* out) {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, kInp, rpt, out, nullptr), SWMM_OK);
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
    // updateAllGages probes at (current_time + 1 s), so match that offset or
    // every interval boundary reads as an off-by-one.
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
        const double probe = start + elapsed + kSecond;

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
