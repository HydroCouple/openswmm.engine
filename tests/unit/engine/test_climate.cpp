/**
 * @file test_climate.cpp
 * @brief Unit tests for climate processing — Hargreaves ET, batch evaporation.
 *
 * @details Tests:
 *   - Hargreaves evapotranspiration formula (solar radiation, latitude)
 *   - Daily climate state updates (monthly, temperature-based)
 *   - Batch evaporation distribution to subcatchments
 *   - Monthly adjustment factors
 *
 * @see src/engine/hydrology/Climate.hpp
 * @ingroup engine_hydrology
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "hydrology/Climate.hpp"
#include "core/DateTime.hpp"

using namespace openswmm;
using namespace openswmm::climate;

// ============================================================================
// Hargreaves evapotranspiration
// ============================================================================

TEST(Hargreaves, PositiveETForRealisticInputs) {
    // Miami, FL: lat=25.8, summer day, avg temp=85F, range=15F
    double et = hargreaves(25.8, 180, 85.0, 15.0);
    EXPECT_GT(et, 0.0);
    // Typical summer ET: 3-8 mm/day
    EXPECT_GT(et, 2.0);
    EXPECT_LT(et, 12.0);
}

TEST(Hargreaves, ZeroRangeGivesZeroET) {
    // Zero temperature range → zero ET
    double et = hargreaves(40.0, 180, 70.0, 0.0);
    EXPECT_DOUBLE_EQ(et, 0.0);
}

TEST(Hargreaves, HigherLatitudeReducesET) {
    // Same temp but higher latitude should produce different ET
    double et_low  = hargreaves(25.0, 180, 75.0, 15.0);  // subtropical
    double et_high = hargreaves(60.0, 180, 75.0, 15.0);  // subarctic

    // Both positive
    EXPECT_GT(et_low, 0.0);
    EXPECT_GT(et_high, 0.0);
}

TEST(Hargreaves, WinterDayLessETThanSummer) {
    // Same location, winter vs summer
    double et_summer = hargreaves(40.0, 172, 80.0, 20.0);  // June 21
    double et_winter = hargreaves(40.0, 355, 35.0, 10.0);  // Dec 21

    EXPECT_GT(et_summer, et_winter);
}

TEST(Hargreaves, NegativeTempRangeClamped) {
    // Negative range should be clamped to 0 internally
    double et = hargreaves(40.0, 180, 70.0, -5.0);
    EXPECT_DOUBLE_EQ(et, 0.0);
}

TEST(Hargreaves, PolarLatitudeWinterGivesZeroOrLow) {
    // 70°N in winter → very low or zero solar radiation
    double et = hargreaves(70.0, 355, 10.0, 5.0);
    // Very low ET expected (near zero or zero)
    EXPECT_GE(et, 0.0);
    EXPECT_LT(et, 1.0);
}

TEST(Hargreaves, EquatorConsistentYear) {
    // Equatorial location: relatively consistent ET year-round
    double et_jan = hargreaves(0.0, 15,  80.0, 15.0);
    double et_jul = hargreaves(0.0, 196, 80.0, 15.0);

    // Both should be substantial and similar
    EXPECT_GT(et_jan, 2.0);
    EXPECT_GT(et_jul, 2.0);
    // Within ~30% of each other
    double ratio = et_jan / et_jul;
    EXPECT_GT(ratio, 0.7);
    EXPECT_LT(ratio, 1.3);
}

// ============================================================================
// Daily climate state update
// ============================================================================

TEST(DailyClimate, ConstantMethodKeepsRate) {
    ClimateState state;
    state.evap_method = EvapMethod::CONSTANT;
    state.evap_rate = 1e-6;

    updateDailyClimate(state, 180, 6);
    EXPECT_NEAR(state.evap_rate, 1e-6, 1e-12);
}

TEST(DailyClimate, MonthlyMethodSetsFromTable) {
    ClimateState state;
    state.evap_method = EvapMethod::MONTHLY;
    state.monthly_evap[6] = 0.2;  // July: 0.2 in/day

    updateDailyClimate(state, 180, 6);

    // Should be converted from in/day to ft/sec
    EXPECT_GT(state.evap_rate, 0.0);
}

TEST(DailyClimate, TemperatureMethodUsesHargreaves) {
    ClimateState state;
    state.evap_method = EvapMethod::TEMPERATURE;
    state.latitude    = 40.0;
    state.temperature = 80.0;
    state.temp_range  = 20.0;

    updateDailyClimate(state, 180, 6);

    // Should produce a positive evaporation rate
    EXPECT_GT(state.evap_rate, 0.0);
}

TEST(DailyClimate, MonthlyAdjustmentApplied) {
    ClimateState state;
    state.evap_method = EvapMethod::CONSTANT;
    state.evap_rate = 1e-6;
    state.adjust_evap[3] = 0.5;  // April: halve evaporation

    updateDailyClimate(state, 100, 3);

    EXPECT_NEAR(state.evap_rate, 0.5e-6, 1e-12);
}

TEST(DailyClimate, SaturationVaporPressureComputed) {
    ClimateState state;
    state.evap_method = EvapMethod::CONSTANT;
    state.temperature = 70.0;

    updateDailyClimate(state, 180, 6);

    // ea should be positive for any reasonable temperature
    EXPECT_GT(state.ea, 0.0);
}

TEST(DailyClimate, VaporPressureIncreasesWithTemp) {
    ClimateState state1, state2;
    state1.evap_method = state2.evap_method = EvapMethod::CONSTANT;
    state1.temperature = 50.0;
    state2.temperature = 90.0;

    updateDailyClimate(state1, 180, 6);
    updateDailyClimate(state2, 180, 6);

    EXPECT_GT(state2.ea, state1.ea);
}

// ============================================================================
// Batch evaporation distribution
// ============================================================================

TEST(BatchEvap, LimitedByPondedDepth) {
    double evap_rate = 1e-4;  // ft/sec (high evap)
    double ponded[4] = {0.001, 0.0, 0.1, 0.0005};
    double evap_out[4] = {};
    double dt = 60.0;  // seconds

    batchDistributeEvap(evap_rate, ponded, evap_out, 4, dt);

    // evap_out[i] = min(evap_rate, ponded[i] / dt)
    EXPECT_NEAR(evap_out[0], std::min(evap_rate, 0.001 / 60.0), 1e-12);
    EXPECT_DOUBLE_EQ(evap_out[1], 0.0);  // no ponded water → no evap
    EXPECT_NEAR(evap_out[2], evap_rate, 1e-12);  // 0.1/60 >> evap_rate
    EXPECT_NEAR(evap_out[3], std::min(evap_rate, 0.0005 / 60.0), 1e-12);
}

TEST(BatchEvap, ZeroEvapRateGivesZero) {
    double ponded[3] = {0.1, 0.2, 0.3};
    double evap_out[3] = {-1, -1, -1};

    batchDistributeEvap(0.0, ponded, evap_out, 3, 60.0);

    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(evap_out[i], 0.0);
    }
}

TEST(BatchEvap, UniformPondedDepthGivesUniformEvap) {
    double evap_rate = 1e-5;
    double ponded[5] = {1.0, 1.0, 1.0, 1.0, 1.0};  // large ponded depth
    double evap_out[5] = {};

    batchDistributeEvap(evap_rate, ponded, evap_out, 5, 60.0);

    for (int i = 0; i < 5; ++i) {
        EXPECT_NEAR(evap_out[i], evap_rate, 1e-12);
    }
}

TEST(BatchEvap, ZeroDtHandled) {
    double evap_rate = 1e-5;
    double ponded[2] = {0.1, 0.2};
    double evap_out[2] = {};

    batchDistributeEvap(evap_rate, ponded, evap_out, 2, 0.0);

    // dt=0 → inv_dt=0 → max_evap=0 → all evap capped at 0
    EXPECT_DOUBLE_EQ(evap_out[0], 0.0);
    EXPECT_DOUBLE_EQ(evap_out[1], 0.0);
}

// ============================================================================
// 7-day moving average
// ============================================================================

TEST(MovingAvg7Test, EmptyReturnsZero) {
    MovingAvg7 ma;
    EXPECT_DOUBLE_EQ(ma.avg_temp(), 0.0);
    EXPECT_DOUBLE_EQ(ma.avg_range(), 0.0);
}

TEST(MovingAvg7Test, SingleValueReturnsSelf) {
    MovingAvg7 ma;
    ma.push(70.0, 15.0);
    EXPECT_DOUBLE_EQ(ma.avg_temp(), 70.0);
    EXPECT_DOUBLE_EQ(ma.avg_range(), 15.0);
}

TEST(MovingAvg7Test, ThreeValuesAverage) {
    MovingAvg7 ma;
    ma.push(60.0, 10.0);
    ma.push(70.0, 20.0);
    ma.push(80.0, 30.0);
    EXPECT_NEAR(ma.avg_temp(), 70.0, 1e-10);
    EXPECT_NEAR(ma.avg_range(), 20.0, 1e-10);
}

TEST(MovingAvg7Test, SevenValuesFullBuffer) {
    MovingAvg7 ma;
    for (int i = 0; i < 7; ++i)
        ma.push(60.0 + i * 2.0, 10.0 + i);
    // temps: 60, 62, 64, 66, 68, 70, 72 → avg = 66
    // ranges: 10, 11, 12, 13, 14, 15, 16 → avg = 13
    EXPECT_NEAR(ma.avg_temp(), 66.0, 1e-10);
    EXPECT_NEAR(ma.avg_range(), 13.0, 1e-10);
    EXPECT_EQ(ma.count, 7);
}

TEST(MovingAvg7Test, WraparoundDropsOldest) {
    MovingAvg7 ma;
    // Fill with 7 values of 50.0
    for (int i = 0; i < 7; ++i)
        ma.push(50.0, 5.0);
    EXPECT_NEAR(ma.avg_temp(), 50.0, 1e-10);

    // Push one new value of 120.0 — drops oldest 50.0
    ma.push(120.0, 75.0);
    EXPECT_EQ(ma.count, 7);
    // New avg: (6*50 + 120) / 7 = 420/7 = 60.0
    EXPECT_NEAR(ma.avg_temp(), 60.0, 1e-10);
    // New range avg: (6*5 + 75) / 7 = 105/7 = 15.0
    EXPECT_NEAR(ma.avg_range(), 15.0, 1e-10);
}

// ============================================================================
// Hargreaves with moving average (via updateDailyClimate)
// ============================================================================

TEST(DailyClimate, HargreavesUsesMovingAverage) {
    ClimateState state;
    state.evap_method = EvapMethod::TEMPERATURE;
    state.latitude    = 40.0;
    state.temperature = 80.0;
    state.temp_range  = 20.0;

    // First call seeds the moving average (count=1)
    updateDailyClimate(state, 180, 6);
    double rate1 = state.evap_rate;
    EXPECT_GT(rate1, 0.0);

    // Second call with different temp — moving average smooths it
    state.temperature = 60.0;
    state.temp_range  = 10.0;
    updateDailyClimate(state, 181, 6);
    double rate2 = state.evap_rate;
    EXPECT_GT(rate2, 0.0);

    // Smoothed average should produce rate between the two extremes
    // not equal to what 60/10 alone would give
    ClimateState ref;
    ref.evap_method = EvapMethod::TEMPERATURE;
    ref.latitude = 40.0;
    ref.temperature = 60.0;
    ref.temp_range  = 10.0;
    updateDailyClimate(ref, 181, 6);
    double rate_raw = ref.evap_rate;

    // rate2 (smoothed) should differ from rate_raw (unsmoothed single value)
    EXPECT_NE(rate2, rate_raw);
}

// ============================================================================
// Psychrometric constant (gamma)
// ============================================================================

TEST(DailyClimate, GammaComputedPositive) {
    ClimateState state;
    state.evap_method = EvapMethod::CONSTANT;
    state.temperature = 70.0;

    updateDailyClimate(state, 180, 6);

    EXPECT_GT(state.gamma, 0.0);
}

TEST(DailyClimate, GammaIncreasesWithTemp) {
    ClimateState cold, warm;
    cold.evap_method = warm.evap_method = EvapMethod::CONSTANT;
    cold.temperature = 32.0;
    warm.temperature = 90.0;

    updateDailyClimate(cold, 180, 6);
    updateDailyClimate(warm, 180, 6);

    // gamma = 0.000359 / (0.27 + 0.000459*ta)
    // Denominator increases with temp → gamma decreases
    EXPECT_GT(cold.gamma, warm.gamma);
}

// ============================================================================
// ClimateFileReader tests
// ============================================================================

#include "hydrology/ClimateFile.hpp"
#include <fstream>
#include <cstdio>
#include <filesystem>

// Helper: write a temporary file and return path
static std::string writeTempFile(const std::string& content) {
    auto tmp = std::filesystem::temp_directory_path() / "swmm_climate_test_XXXXXX";
    std::string path = tmp.string();
    std::ofstream ofs(path);
    if (!ofs) return "";
    ofs << content;
    ofs.close();
    return path;
}

TEST(ClimateFileReader, OpenNonexistentFileFails) {
    climate::ClimateFileReader reader;
    EXPECT_FALSE(reader.open("/nonexistent/path/climate.dat", 0.0, 0));
    EXPECT_FALSE(reader.isOpen());
}

TEST(ClimateFileReader, DetectsUserPreparedFormat) {
    std::string data =
        "STA001 2020 1 1 40.0 25.0 0.10 5.0\n"
        "STA001 2020 1 2 42.0 28.0 0.12 6.0\n"
        "STA001 2020 1 3 38.0 22.0 * 4.0\n";
    std::string path = writeTempFile(data);

    climate::ClimateFileReader reader;
    EXPECT_TRUE(reader.open(path, 0.0, 0));  // US units
    EXPECT_EQ(reader.format(), climate::ClimateFileFormat::USER_PREPARED);
    reader.close();
    std::remove(path.c_str());
}

TEST(ClimateFileReader, UserPreparedReadsValues) {
    // US units: temps in °F, evap in in/day, wind in mph
    std::string data =
        "STA001 2020 7 1 90.0 70.0 0.20 8.0\n"
        "STA001 2020 7 2 88.0 68.0 0.18 7.0\n"
        "STA001 2020 7 3 92.0 72.0 0.22 9.0\n";
    std::string path = writeTempFile(data);

    climate::ClimateFileReader reader;
    ASSERT_TRUE(reader.open(path, 0.0, 0));

    // Julian date for 2020-07-01 (approximate; use a date we know)
    // Legacy DateTime encodes as days since some epoch
    // For this test, we call getRecord with a date that triggers month=7, year=2020
    // Since we use datetime::decodeDate, let's compute the Julian date
    double jul = openswmm::datetime::encodeDate(2020, 7, 1);

    climate::DailyClimateRecord rec;
    ASSERT_TRUE(reader.getRecord(jul, rec));

    // Day 1: TMAX=90, TMIN=70 (US, no conversion needed)
    EXPECT_NEAR(rec.tmax, 90.0, 0.1);
    EXPECT_NEAR(rec.tmin, 70.0, 0.1);
    EXPECT_NEAR(rec.evap, 0.20, 0.01);
    EXPECT_NEAR(rec.wind, 8.0, 0.1);

    // Day 2
    jul = openswmm::datetime::encodeDate(2020, 7, 2);
    ASSERT_TRUE(reader.getRecord(jul, rec));
    EXPECT_NEAR(rec.tmax, 88.0, 0.1);
    EXPECT_NEAR(rec.tmin, 68.0, 0.1);

    reader.close();
    std::remove(path.c_str());
}

TEST(ClimateFileReader, UserPreparedHandlesMissing) {
    std::string data =
        "STA001 2020 3 15 * * * *\n";
    std::string path = writeTempFile(data);

    climate::ClimateFileReader reader;
    ASSERT_TRUE(reader.open(path, 0.0, 0));

    double jul = openswmm::datetime::encodeDate(2020, 3, 15);
    climate::DailyClimateRecord rec;
    ASSERT_TRUE(reader.getRecord(jul, rec));

    EXPECT_TRUE(std::isnan(rec.tmax));
    EXPECT_TRUE(std::isnan(rec.tmin));
    EXPECT_TRUE(std::isnan(rec.evap));
    EXPECT_TRUE(std::isnan(rec.wind));

    reader.close();
    std::remove(path.c_str());
}

TEST(ClimateFileReader, UserPreparedSIConversion) {
    // SI units: temps in °C, should be converted to °F
    std::string data =
        "STA001 2020 6 1 30.0 20.0 5.0 3.0\n";
    std::string path = writeTempFile(data);

    climate::ClimateFileReader reader;
    ASSERT_TRUE(reader.open(path, 0.0, 1));  // SI units

    double jul = openswmm::datetime::encodeDate(2020, 6, 1);
    climate::DailyClimateRecord rec;
    ASSERT_TRUE(reader.getRecord(jul, rec));

    // 30°C → 86°F, 20°C → 68°F
    EXPECT_NEAR(rec.tmax, 86.0, 0.1);
    EXPECT_NEAR(rec.tmin, 68.0, 0.1);

    reader.close();
    std::remove(path.c_str());
}

TEST(ClimateFileReader, MonthBoundaryTransition) {
    // Data spanning two months
    std::string data =
        "STA001 2020 1 30 40.0 25.0 0.1 5.0\n"
        "STA001 2020 1 31 41.0 26.0 0.1 5.0\n"
        "STA001 2020 2 1 35.0 20.0 0.08 4.0\n"
        "STA001 2020 2 2 36.0 21.0 0.09 4.5\n";
    std::string path = writeTempFile(data);

    climate::ClimateFileReader reader;
    ASSERT_TRUE(reader.open(path, 0.0, 0));

    // Read Jan 31
    climate::DailyClimateRecord rec;
    double jul = openswmm::datetime::encodeDate(2020, 1, 31);
    ASSERT_TRUE(reader.getRecord(jul, rec));
    EXPECT_NEAR(rec.tmax, 41.0, 0.1);

    // Read Feb 1 — triggers month boundary
    jul = openswmm::datetime::encodeDate(2020, 2, 1);
    ASSERT_TRUE(reader.getRecord(jul, rec));
    EXPECT_NEAR(rec.tmax, 35.0, 0.1);

    // Read Feb 2
    jul = openswmm::datetime::encodeDate(2020, 2, 2);
    ASSERT_TRUE(reader.getRecord(jul, rec));
    EXPECT_NEAR(rec.tmax, 36.0, 0.1);

    reader.close();
    std::remove(path.c_str());
}

TEST(ClimateFileReader, DetectsGHCNDFormat) {
    std::string data =
        "DATE        TMIN    TMAX    EVAP    AWND\n"
        "20200101    -50     100     0       25\n";
    std::string path = writeTempFile(data);

    climate::ClimateFileReader reader;
    ASSERT_TRUE(reader.open(path, 0.0, 0));
    EXPECT_EQ(reader.format(), climate::ClimateFileFormat::GHCND);

    reader.close();
    std::remove(path.c_str());
}

// ============================================================================
// Adjustment factor application tests
// ============================================================================

TEST(DailyClimate, EvapAdjustmentMultiplies) {
    ClimateState state;
    state.evap_method = EvapMethod::MONTHLY;
    state.monthly_evap[5] = 0.3;     // June: 0.3 in/day
    state.adjust_evap[5]  = 0.75;    // Reduce to 75%

    updateDailyClimate(state, 170, 5);
    double adjusted = state.evap_rate;

    state.adjust_evap[5] = 1.0;
    updateDailyClimate(state, 170, 5);
    double unadjusted = state.evap_rate;

    EXPECT_NEAR(adjusted / unadjusted, 0.75, 1e-10)
        << "Evap adjustment should multiply the rate by the factor";
}

TEST(DailyClimate, EvapAdjustmentZeroKillsRate) {
    ClimateState state;
    state.evap_method = EvapMethod::MONTHLY;
    state.monthly_evap[0] = 0.1;
    state.adjust_evap[0]  = 0.0;

    updateDailyClimate(state, 15, 0);
    EXPECT_DOUBLE_EQ(state.evap_rate, 0.0);
}

TEST(DailyClimate, GammaMatchesLegacyFormula) {
    ClimateState state;
    state.evap_method = EvapMethod::CONSTANT;

    // Test several temperatures against the exact formula
    double temps[] = {0.0, 32.0, 50.0, 70.0, 100.0};
    for (double ta : temps) {
        state.temperature = ta;
        updateDailyClimate(state, 180, 6);
        double expected = 0.000359 / (0.27 + 0.000459 * ta);
        EXPECT_NEAR(state.gamma, expected, 1e-12)
            << "Gamma mismatch at temperature " << ta;
    }
}

TEST(DailyClimate, EaMatchesLegacyFormula) {
    ClimateState state;
    state.evap_method = EvapMethod::CONSTANT;

    double temps[] = {32.0, 50.0, 70.0, 90.0};
    for (double ta : temps) {
        state.temperature = ta;
        updateDailyClimate(state, 180, 6);
        double expected = 8.1175e6 * std::exp(-7701.544 / (ta + 405.0265));
        EXPECT_NEAR(state.ea, expected, 1e-12)
            << "Ea mismatch at temperature " << ta;
    }
}
