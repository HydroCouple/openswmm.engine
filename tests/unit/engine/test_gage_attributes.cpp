/**
 * @file test_gage_attributes.cpp
 * @brief Unit tests for the rain-gage C API getters/setters added to close the
 *        Property-Browser parity gap (DA-ENG-04): recording interval, snow-catch
 *        factor, time-series id, station id, and rain-depth units.
 *
 * @details Covers the round-trip behaviour of:
 *   - swmm_gage_get_rain_interval        (reads GageData.interval_sec)
 *   - swmm_gage_get/set_snow_factor      (reads/writes GageData.snow_factor)
 *   - swmm_gage_get_timeseries           (resolves GageData.ts_index -> name)
 *   - swmm_gage_get_station_id /
 *     swmm_gage_set_station_id /
 *     swmm_gage_set_filename             (writes GageData.station_id, not col_name)
 *   - swmm_gage_get/set_rain_units       (reads/writes GageData.rain_units)
 *
 * @see src/engine/core/openswmm_gages_impl.cpp
 * @see include/openswmm/engine/openswmm_gages.h
 * @see Property-Browser consumer: openswmm.gui SWMMRainGagePropertyAdapter
 * @ingroup engine_api
 */

#include <gtest/gtest.h>
#include <cstring>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gages.h>

// Working directory is tests/unit/engine/data/ (set by CMakeLists.txt). The
// gage_noscalefactor.inp fixture declares one gage:
//   RG1  INTENSITY  0:15  1.0  TIMESERIES TS1
namespace {

class GageAttributesCApi : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;

    void openModel(const char* rpt, const char* out) {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, "gage_noscalefactor.inp", rpt, out, nullptr),
                  SWMM_OK);
        ASSERT_EQ(swmm_gage_count(engine_), 1);
    }

    void TearDown() override {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Recording interval
// ---------------------------------------------------------------------------

TEST_F(GageAttributesCApi, GetRainIntervalReturnsParsedValue) {
    openModel("_gage_interval_get.rpt", "_gage_interval_get.out");
    double secs = 0.0;
    EXPECT_EQ(swmm_gage_get_rain_interval(engine_, 0, &secs), SWMM_OK);
    EXPECT_DOUBLE_EQ(secs, 900.0);  // 0:15
}

TEST_F(GageAttributesCApi, SetRainIntervalRoundTrips) {
    openModel("_gage_interval_set.rpt", "_gage_interval_set.out");
    EXPECT_EQ(swmm_gage_set_rain_interval(engine_, 0, 300.0), SWMM_OK);
    double secs = 0.0;
    EXPECT_EQ(swmm_gage_get_rain_interval(engine_, 0, &secs), SWMM_OK);
    EXPECT_DOUBLE_EQ(secs, 300.0);
}

// ---------------------------------------------------------------------------
// Snow-catch factor (SCF) — distinct from the rainfall scale factor
// ---------------------------------------------------------------------------

TEST_F(GageAttributesCApi, GetSnowFactorDefaultsToOne) {
    openModel("_gage_scf_get.rpt", "_gage_scf_get.out");
    double scf = 0.0;
    EXPECT_EQ(swmm_gage_get_snow_factor(engine_, 0, &scf), SWMM_OK);
    EXPECT_DOUBLE_EQ(scf, 1.0);
}

TEST_F(GageAttributesCApi, SetSnowFactorRoundTrips) {
    openModel("_gage_scf_set.rpt", "_gage_scf_set.out");
    EXPECT_EQ(swmm_gage_set_snow_factor(engine_, 0, 1.4), SWMM_OK);
    double scf = 0.0;
    EXPECT_EQ(swmm_gage_get_snow_factor(engine_, 0, &scf), SWMM_OK);
    EXPECT_DOUBLE_EQ(scf, 1.4);
}

TEST_F(GageAttributesCApi, SetSnowFactorRejectsNonPositive) {
    openModel("_gage_scf_bad.rpt", "_gage_scf_bad.out");
    EXPECT_NE(swmm_gage_set_snow_factor(engine_, 0,  0.0), SWMM_OK);
    EXPECT_NE(swmm_gage_set_snow_factor(engine_, 0, -2.0), SWMM_OK);
    double scf = 0.0;
    EXPECT_EQ(swmm_gage_get_snow_factor(engine_, 0, &scf), SWMM_OK);
    EXPECT_DOUBLE_EQ(scf, 1.0) << "Rejected set must not mutate the stored value.";
}

// ---------------------------------------------------------------------------
// Time-series id
// ---------------------------------------------------------------------------

TEST_F(GageAttributesCApi, GetTimeseriesReturnsAssignedName) {
    openModel("_gage_ts_get.rpt", "_gage_ts_get.out");
    char buf[64] = {};
    EXPECT_EQ(swmm_gage_get_timeseries(engine_, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "TS1");
}

TEST_F(GageAttributesCApi, SetTimeseriesRoundTrips) {
    openModel("_gage_ts_set.rpt", "_gage_ts_set.out");
    // TS1 is the only series in the fixture; re-assign it and read it back.
    EXPECT_EQ(swmm_gage_set_timeseries(engine_, 0, "TS1"), SWMM_OK);
    char buf[64] = {};
    EXPECT_EQ(swmm_gage_get_timeseries(engine_, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "TS1");
    int src = -1;
    EXPECT_EQ(swmm_gage_get_data_source(engine_, 0, &src), SWMM_OK);
    EXPECT_EQ(src, SWMM_GAGE_TIMESERIES);
}

// ---------------------------------------------------------------------------
// File source — station id (standard SWMM rain file grammar) + units
// ---------------------------------------------------------------------------

TEST_F(GageAttributesCApi, SetFilenameStoresStationIdAndSwitchesToFile) {
    openModel("_gage_file_set.rpt", "_gage_file_set.out");
    EXPECT_EQ(swmm_gage_set_filename(engine_, 0, "rain.dat", "STA_07"), SWMM_OK);

    int src = -1;
    EXPECT_EQ(swmm_gage_get_data_source(engine_, 0, &src), SWMM_OK);
    EXPECT_EQ(src, SWMM_GAGE_FILE);

    char buf[64] = {};
    EXPECT_EQ(swmm_gage_get_station_id(engine_, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "STA_07");
}

TEST_F(GageAttributesCApi, SetStationIdRoundTrips) {
    openModel("_gage_station_set.rpt", "_gage_station_set.out");
    EXPECT_EQ(swmm_gage_set_station_id(engine_, 0, "GHCND:US1"), SWMM_OK);
    char buf[64] = {};
    EXPECT_EQ(swmm_gage_get_station_id(engine_, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "GHCND:US1");
}

TEST_F(GageAttributesCApi, GetRainUnitsDefaultsToInches) {
    openModel("_gage_units_get.rpt", "_gage_units_get.out");
    int units = -1;
    EXPECT_EQ(swmm_gage_get_rain_units(engine_, 0, &units), SWMM_OK);
    EXPECT_EQ(units, 0);  // IN
}

TEST_F(GageAttributesCApi, SetRainUnitsRoundTrips) {
    openModel("_gage_units_set.rpt", "_gage_units_set.out");
    EXPECT_EQ(swmm_gage_set_rain_units(engine_, 0, 1), SWMM_OK);  // MM
    int units = -1;
    EXPECT_EQ(swmm_gage_get_rain_units(engine_, 0, &units), SWMM_OK);
    EXPECT_EQ(units, 1);
}

TEST_F(GageAttributesCApi, SetRainUnitsRejectsOutOfRange) {
    openModel("_gage_units_bad.rpt", "_gage_units_bad.out");
    EXPECT_NE(swmm_gage_set_rain_units(engine_, 0,  2), SWMM_OK);
    EXPECT_NE(swmm_gage_set_rain_units(engine_, 0, -1), SWMM_OK);
}

// ---------------------------------------------------------------------------
// Bad-index guards (shared across the new getters)
// ---------------------------------------------------------------------------

TEST_F(GageAttributesCApi, GettersRejectBadIndex) {
    openModel("_gage_attr_badidx.rpt", "_gage_attr_badidx.out");
    double d = 0.0; int i = 0; char buf[8] = {};
    EXPECT_NE(swmm_gage_get_rain_interval(engine_, 9, &d),        SWMM_OK);
    EXPECT_NE(swmm_gage_get_snow_factor(engine_, 9, &d),          SWMM_OK);
    EXPECT_NE(swmm_gage_get_rain_units(engine_, 9, &i),           SWMM_OK);
    EXPECT_NE(swmm_gage_get_timeseries(engine_, 9, buf, sizeof(buf)), SWMM_OK);
    EXPECT_NE(swmm_gage_get_station_id(engine_, 9, buf, sizeof(buf)), SWMM_OK);
}
