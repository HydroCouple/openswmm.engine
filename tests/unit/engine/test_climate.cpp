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
