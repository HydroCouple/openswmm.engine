/**
 * @file test_climate_api.cpp
 * @brief Unit tests for the climatology configuration C API (openswmm_climate.h):
 *        temperature, evaporation, wind, snowmelt globals, areal-depletion
 *        curves, and monthly adjustments.
 *
 * @details Covers, against the climate_config.inp fixture:
 *   - parse integrity   : every getter returns the value parsed from the .inp
 *   - round-trip         : every setter is read back by its getter
 *   - validation         : enums, ranges, ADC fractions, and array counts reject
 *   - lifecycle          : setters fail once the engine is INITIALIZED
 *   - dtlong             : the previously-discarded longitude correction now
 *                          parses, exposes, and round-trips (minutes)
 *
 * @see src/engine/core/openswmm_climate_impl.cpp
 * @see include/openswmm/engine/openswmm_climate.h
 * @ingroup engine_api
 */

#include <gtest/gtest.h>
#include <array>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_climate.h>

// Working directory is tests/unit/engine/data/ (set by CMakeLists.txt).
namespace {

class ClimateConfigCApi : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;

    void openModel(const char* rpt, const char* out) {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, "climate_config.inp", rpt, out, nullptr),
                  SWMM_OK);
    }

    void TearDown() override {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }
};

constexpr double kEvapMonthly[12] =
    {0.10,0.11,0.12,0.13,0.14,0.15,0.16,0.17,0.18,0.19,0.20,0.21};
constexpr double kWindMonthly[12] =
    {1,2,3,4,5,6,7,8,9,10,11,12};
constexpr double kAdcImperv[10] =
    {1.0,0.9,0.8,0.7,0.6,0.5,0.4,0.3,0.2,0.1};
constexpr double kAdcPerv[10] =
    {1.0,0.95,0.9,0.85,0.8,0.75,0.7,0.65,0.6,0.55};
constexpr double kAdjTemp[12] =
    {-2,-1,0,1,2,3,4,5,4,3,2,1};
constexpr double kAdjEvap[12] =
    {0.80,0.85,0.90,0.95,1.00,1.05,1.10,1.15,1.10,1.05,1.00,0.95};
constexpr double kAdjRain[12] =
    {1.0,1.1,1.2,1.3,1.4,1.5,1.4,1.3,1.2,1.1,1.0,0.9};

} // namespace

// ---------------------------------------------------------------------------
// Parse integrity — getters return what the .inp declared
// ---------------------------------------------------------------------------

TEST_F(ClimateConfigCApi, ScalarsMatchParsedInput) {
    openModel("_climate_scalars.rpt", "_climate_scalars.out");

    int isrc = -1;
    EXPECT_EQ(swmm_climate_get_temp_source(engine_, &isrc), SWMM_OK);
    EXPECT_EQ(isrc, SWMM_TEMP_TIMESERIES);

    char buf[64] = {};
    EXPECT_EQ(swmm_climate_get_temp_timeseries(engine_, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "TempTS");

    double d = 0.0;
    EXPECT_EQ(swmm_climate_get_elevation(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 100.0);
    EXPECT_EQ(swmm_climate_get_latitude(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 40.5);
    EXPECT_EQ(swmm_climate_get_longitude_correction(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 300.0);  // minutes, as written in the .inp

    int tunits = -99;
    EXPECT_EQ(swmm_climate_get_temp_units(engine_, &tunits), SWMM_OK);
    EXPECT_EQ(tunits, -1);  // fixture uses TIMESERIES temp source: units unspecified

    int etype = -1;
    EXPECT_EQ(swmm_climate_get_evap_type(engine_, &etype), SWMM_OK);
    EXPECT_EQ(etype, SWMM_EVAP_MONTHLY);
    EXPECT_EQ(swmm_climate_get_evap_recovery(engine_, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "RecovPat");

    int wtype = -1;
    EXPECT_EQ(swmm_climate_get_wind_type(engine_, &wtype), SWMM_OK);
    EXPECT_EQ(wtype, SWMM_WIND_MONTHLY);

    EXPECT_EQ(swmm_climate_get_snow_temp(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 32.5);
    EXPECT_EQ(swmm_climate_get_ati_weight(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 0.45);
    EXPECT_EQ(swmm_climate_get_neg_melt_ratio(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 0.55);
}

TEST_F(ClimateConfigCApi, ArraysMatchParsedInput) {
    openModel("_climate_arrays.rpt", "_climate_arrays.out");

    std::array<double, 12> m12{};
    std::array<double, 10> a10{};

    EXPECT_EQ(swmm_climate_get_evap_monthly(engine_, m12.data(), 12), SWMM_OK);
    for (int i = 0; i < 12; ++i) EXPECT_DOUBLE_EQ(m12[i], kEvapMonthly[i]) << "evap[" << i << "]";

    EXPECT_EQ(swmm_climate_get_wind_monthly(engine_, m12.data(), 12), SWMM_OK);
    for (int i = 0; i < 12; ++i) EXPECT_DOUBLE_EQ(m12[i], kWindMonthly[i]) << "wind[" << i << "]";

    EXPECT_EQ(swmm_climate_get_adc_impervious(engine_, a10.data(), 10), SWMM_OK);
    for (int i = 0; i < 10; ++i) EXPECT_DOUBLE_EQ(a10[i], kAdcImperv[i]) << "adcI[" << i << "]";

    EXPECT_EQ(swmm_climate_get_adc_pervious(engine_, a10.data(), 10), SWMM_OK);
    for (int i = 0; i < 10; ++i) EXPECT_DOUBLE_EQ(a10[i], kAdcPerv[i]) << "adcP[" << i << "]";

    EXPECT_EQ(swmm_climate_get_adjust_temperature(engine_, m12.data(), 12), SWMM_OK);
    for (int i = 0; i < 12; ++i) EXPECT_DOUBLE_EQ(m12[i], kAdjTemp[i]) << "adjT[" << i << "]";

    EXPECT_EQ(swmm_climate_get_adjust_evaporation(engine_, m12.data(), 12), SWMM_OK);
    for (int i = 0; i < 12; ++i) EXPECT_DOUBLE_EQ(m12[i], kAdjEvap[i]) << "adjE[" << i << "]";

    EXPECT_EQ(swmm_climate_get_adjust_rainfall(engine_, m12.data(), 12), SWMM_OK);
    for (int i = 0; i < 12; ++i) EXPECT_DOUBLE_EQ(m12[i], kAdjRain[i]) << "adjR[" << i << "]";

    // pan coefficients default to 1.0 (no FILE pan line in the fixture)
    EXPECT_EQ(swmm_climate_get_pan_coeff(engine_, m12.data(), 12), SWMM_OK);
    for (int i = 0; i < 12; ++i) EXPECT_DOUBLE_EQ(m12[i], 1.0) << "pan[" << i << "]";
}

// ---------------------------------------------------------------------------
// Round-trip — setters are observed by getters
// ---------------------------------------------------------------------------

TEST_F(ClimateConfigCApi, ScalarSettersRoundTrip) {
    openModel("_climate_scalar_set.rpt", "_climate_scalar_set.out");
    double d = 0.0;

    EXPECT_EQ(swmm_climate_set_elevation(engine_, 250.0), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_elevation(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 250.0);

    EXPECT_EQ(swmm_climate_set_latitude(engine_, -33.25), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_latitude(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, -33.25);

    EXPECT_EQ(swmm_climate_set_longitude_correction(engine_, 90.0), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_longitude_correction(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 90.0);

    EXPECT_EQ(swmm_climate_set_snow_temp(engine_, 30.0), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_snow_temp(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 30.0);

    EXPECT_EQ(swmm_climate_set_ati_weight(engine_, 0.2), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_ati_weight(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 0.2);

    EXPECT_EQ(swmm_climate_set_neg_melt_ratio(engine_, 0.7), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_neg_melt_ratio(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 0.7);

    EXPECT_EQ(swmm_climate_set_evap_type(engine_, SWMM_EVAP_TEMPERATURE), SWMM_OK);
    int t = -1;
    EXPECT_EQ(swmm_climate_get_evap_type(engine_, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_EVAP_TEMPERATURE);

    EXPECT_EQ(swmm_climate_set_wind_type(engine_, SWMM_WIND_FILE), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_wind_type(engine_, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_WIND_FILE);

    EXPECT_EQ(swmm_climate_set_temp_units(engine_, 0), SWMM_OK);  // C10
    EXPECT_EQ(swmm_climate_get_temp_units(engine_, &t), SWMM_OK);
    EXPECT_EQ(t, 0);
    EXPECT_EQ(swmm_climate_set_temp_units(engine_, 2), SWMM_OK);  // F
    EXPECT_EQ(swmm_climate_get_temp_units(engine_, &t), SWMM_OK);
    EXPECT_EQ(t, 2);
}

TEST_F(ClimateConfigCApi, ArraySettersRoundTrip) {
    openModel("_climate_array_set.rpt", "_climate_array_set.out");
    std::array<double, 12> in12{}, out12{};
    std::array<double, 10> in10{}, out10{};
    for (int i = 0; i < 12; ++i) in12[i] = 0.5 + i;
    for (int i = 0; i < 10; ++i) in10[i] = 1.0 - i * 0.1;

    EXPECT_EQ(swmm_climate_set_evap_monthly(engine_, in12.data(), 12), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_evap_monthly(engine_, out12.data(), 12), SWMM_OK);
    EXPECT_EQ(in12, out12);

    EXPECT_EQ(swmm_climate_set_pan_coeff(engine_, in12.data(), 12), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_pan_coeff(engine_, out12.data(), 12), SWMM_OK);
    EXPECT_EQ(in12, out12);

    EXPECT_EQ(swmm_climate_set_wind_monthly(engine_, in12.data(), 12), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_wind_monthly(engine_, out12.data(), 12), SWMM_OK);
    EXPECT_EQ(in12, out12);

    EXPECT_EQ(swmm_climate_set_adc_impervious(engine_, in10.data(), 10), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_adc_impervious(engine_, out10.data(), 10), SWMM_OK);
    EXPECT_EQ(in10, out10);

    EXPECT_EQ(swmm_climate_set_adc_pervious(engine_, in10.data(), 10), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_adc_pervious(engine_, out10.data(), 10), SWMM_OK);
    EXPECT_EQ(in10, out10);

    EXPECT_EQ(swmm_climate_set_adjust_temperature(engine_, in12.data(), 12), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_adjust_temperature(engine_, out12.data(), 12), SWMM_OK);
    EXPECT_EQ(in12, out12);

    EXPECT_EQ(swmm_climate_set_adjust_evaporation(engine_, in12.data(), 12), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_adjust_evaporation(engine_, out12.data(), 12), SWMM_OK);
    EXPECT_EQ(in12, out12);

    EXPECT_EQ(swmm_climate_set_adjust_rainfall(engine_, in12.data(), 12), SWMM_OK);
    EXPECT_EQ(swmm_climate_get_adjust_rainfall(engine_, out12.data(), 12), SWMM_OK);
    EXPECT_EQ(in12, out12);
}

TEST_F(ClimateConfigCApi, TimeseriesSettersSwitchSource) {
    openModel("_climate_ts_set.rpt", "_climate_ts_set.out");
    char buf[64] = {};

    EXPECT_EQ(swmm_climate_set_temp_timeseries(engine_, "TempTS"), SWMM_OK);
    int src = -1;
    EXPECT_EQ(swmm_climate_get_temp_source(engine_, &src), SWMM_OK);
    EXPECT_EQ(src, SWMM_TEMP_TIMESERIES);
    EXPECT_EQ(swmm_climate_get_temp_timeseries(engine_, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "TempTS");

    EXPECT_EQ(swmm_climate_set_evap_timeseries(engine_, "TS1"), SWMM_OK);
    int t = -1;
    EXPECT_EQ(swmm_climate_get_evap_type(engine_, &t), SWMM_OK);
    EXPECT_EQ(t, SWMM_EVAP_TIMESERIES);
    EXPECT_EQ(swmm_climate_get_evap_timeseries(engine_, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "TS1");
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

TEST_F(ClimateConfigCApi, EnumSettersRejectOutOfRange) {
    openModel("_climate_enum_bad.rpt", "_climate_enum_bad.out");
    EXPECT_NE(swmm_climate_set_temp_source(engine_, 3),  SWMM_OK);
    EXPECT_NE(swmm_climate_set_temp_source(engine_, -1), SWMM_OK);
    EXPECT_NE(swmm_climate_set_evap_type(engine_, 5),    SWMM_OK);
    EXPECT_NE(swmm_climate_set_evap_type(engine_, -1),   SWMM_OK);
    EXPECT_NE(swmm_climate_set_wind_type(engine_, 2),    SWMM_OK);
    EXPECT_NE(swmm_climate_set_wind_type(engine_, -1),   SWMM_OK);
    EXPECT_NE(swmm_climate_set_temp_units(engine_, 3),   SWMM_OK);
    EXPECT_NE(swmm_climate_set_temp_units(engine_, -2),  SWMM_OK);
    EXPECT_EQ(swmm_climate_set_temp_units(engine_, -1),  SWMM_OK);  // -1 is valid (unspecified)
}

TEST_F(ClimateConfigCApi, RangeSettersRejectOutOfRange) {
    openModel("_climate_range_bad.rpt", "_climate_range_bad.out");
    EXPECT_NE(swmm_climate_set_latitude(engine_, 91.0),  SWMM_OK);
    EXPECT_NE(swmm_climate_set_latitude(engine_, -91.0), SWMM_OK);
    EXPECT_NE(swmm_climate_set_ati_weight(engine_, 1.1), SWMM_OK);
    EXPECT_NE(swmm_climate_set_ati_weight(engine_, -0.1),SWMM_OK);
    EXPECT_NE(swmm_climate_set_neg_melt_ratio(engine_, 1.5),  SWMM_OK);
    EXPECT_NE(swmm_climate_set_neg_melt_ratio(engine_, -0.5), SWMM_OK);

    // A rejected set must not mutate the stored value.
    double d = 0.0;
    EXPECT_EQ(swmm_climate_get_ati_weight(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 0.45);
}

TEST_F(ClimateConfigCApi, AdcSettersRejectFractionsAndCount) {
    openModel("_climate_adc_bad.rpt", "_climate_adc_bad.out");
    std::array<double, 10> bad{};
    for (int i = 0; i < 10; ++i) bad[i] = kAdcImperv[i];
    bad[3] = 1.5;  // out of [0,1]
    EXPECT_NE(swmm_climate_set_adc_impervious(engine_, bad.data(), 10), SWMM_OK);
    bad[3] = -0.2;
    EXPECT_NE(swmm_climate_set_adc_pervious(engine_, bad.data(), 10), SWMM_OK);

    std::array<double, 10> ok{};
    ok.fill(0.5);
    EXPECT_NE(swmm_climate_set_adc_impervious(engine_, ok.data(), 9), SWMM_OK);   // wrong count
    EXPECT_NE(swmm_climate_set_adc_impervious(engine_, nullptr, 10), SWMM_OK);    // null
}

TEST_F(ClimateConfigCApi, MonthlySettersRejectWrongCount) {
    openModel("_climate_count_bad.rpt", "_climate_count_bad.out");
    std::array<double, 12> v{};
    v.fill(1.0);
    EXPECT_NE(swmm_climate_set_evap_monthly(engine_, v.data(), 11), SWMM_OK);
    EXPECT_NE(swmm_climate_set_wind_monthly(engine_, v.data(), 13), SWMM_OK);
    EXPECT_NE(swmm_climate_set_adjust_rainfall(engine_, v.data(), 0), SWMM_OK);
    EXPECT_NE(swmm_climate_get_evap_monthly(engine_, v.data(), 6), SWMM_OK);
}

TEST_F(ClimateConfigCApi, ConductivityClampsNonPositiveToOne) {
    openModel("_climate_conduct.rpt", "_climate_conduct.out");
    std::array<double, 12> in{};
    in.fill(2.0);
    in[2] = 0.0;    // clamp -> 1.0
    in[5] = -3.0;   // clamp -> 1.0
    EXPECT_EQ(swmm_climate_set_adjust_conductivity(engine_, in.data(), 12), SWMM_OK);

    std::array<double, 12> out{};
    EXPECT_EQ(swmm_climate_get_adjust_conductivity(engine_, out.data(), 12), SWMM_OK);
    EXPECT_DOUBLE_EQ(out[0], 2.0);
    EXPECT_DOUBLE_EQ(out[2], 1.0);
    EXPECT_DOUBLE_EQ(out[5], 1.0);
}

// ---------------------------------------------------------------------------
// Lifecycle — config setters are rejected once the engine is initialized
// ---------------------------------------------------------------------------

TEST_F(ClimateConfigCApi, SettersRejectedAfterInitialize) {
    openModel("_climate_lifecycle.rpt", "_climate_lifecycle.out");
    ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);

    EXPECT_EQ(swmm_climate_set_elevation(engine_, 5.0), SWMM_ERR_LIFECYCLE);
    std::array<double, 12> v{};
    v.fill(1.0);
    EXPECT_EQ(swmm_climate_set_evap_monthly(engine_, v.data(), 12), SWMM_ERR_LIFECYCLE);

    // Getters still work after initialize.
    double d = 0.0;
    EXPECT_EQ(swmm_climate_get_elevation(engine_, &d), SWMM_OK);
    EXPECT_DOUBLE_EQ(d, 100.0);
}

// ---------------------------------------------------------------------------
// Bad handle
// ---------------------------------------------------------------------------

TEST(ClimateConfigCApiNoFixture, NullHandleRejected) {
    int i = 0; double d = 0.0;
    EXPECT_EQ(swmm_climate_get_temp_source(nullptr, &i), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_climate_set_elevation(nullptr, 1.0), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_climate_get_latitude(nullptr, &d), SWMM_ERR_BADHANDLE);
}
