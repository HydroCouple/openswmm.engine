/**
 * @file test_gage_scalefactor.cpp
 * @brief Unit tests for the rain gage scale factor feature in the new engine.
 *
 * @details Covers:
 *   - GageData SoA wiring (default value, grow_to preservation, erase_at,
 *     resize) for the new scale_factor field.
 *   - convertRainfall() multiplies by GageState::scale_factor.
 *   - updateAllGages() applies scale_factor on the primary timeseries path.
 *   - updateAllGages() applies the co-gage ratio
 *     (scale_factor[secondary] / scale_factor[primary]) when the secondary
 *     gage shares a timeseries with a primary.
 *   - getReportRainfall() applies scale_factor.
 *
 * @see src/engine/data/GageData.hpp
 * @see src/engine/hydrology/Gage.cpp
 * @see Legacy parity: src/legacy/engine/gage.c (convertRainfall + co-gage paths)
 * @ingroup engine_hydrology
 */

#include <gtest/gtest.h>
#include <cmath>

#include "hydrology/Gage.hpp"
#include "core/SimulationContext.hpp"
#include "data/GageData.hpp"
#include "data/TableData.hpp"

using namespace openswmm;

// ============================================================================
// GageData SoA — lifecycle wiring for scale_factor
// ============================================================================

TEST(GageDataScaleFactor, ResizeDefaultsToOne) {
    GageData g;
    g.resize(3);
    ASSERT_EQ(g.scale_factor.size(), 3u);
    for (double sf : g.scale_factor) EXPECT_DOUBLE_EQ(sf, 1.0);
}

TEST(GageDataScaleFactor, GrowToPreservesExistingAndDefaultsNew) {
    GageData g;
    g.resize(2);
    g.scale_factor[0] = 2.5;
    g.scale_factor[1] = 0.5;

    g.grow_to(5);
    ASSERT_EQ(g.count(), 5);
    EXPECT_DOUBLE_EQ(g.scale_factor[0], 2.5);
    EXPECT_DOUBLE_EQ(g.scale_factor[1], 0.5);
    EXPECT_DOUBLE_EQ(g.scale_factor[2], 1.0);
    EXPECT_DOUBLE_EQ(g.scale_factor[3], 1.0);
    EXPECT_DOUBLE_EQ(g.scale_factor[4], 1.0);
}

TEST(GageDataScaleFactor, EraseAtRemovesScaleFactor) {
    GageData g;
    g.resize(3);
    g.scale_factor = {1.0, 2.0, 3.0};

    g.erase_at(1);
    ASSERT_EQ(g.count(), 2);
    EXPECT_DOUBLE_EQ(g.scale_factor[0], 1.0);
    EXPECT_DOUBLE_EQ(g.scale_factor[1], 3.0);
}

// ============================================================================
// convertRainfall — scale_factor in the multiplicative chain
// ============================================================================

TEST(GageConvertRainfall, ScaleFactorMultipliesIntensity) {
    gage::GageState state;
    state.rain_type     = gage::RainType::INTENSITY;
    state.units_factor  = 1.0;
    state.adjust_factor = 1.0;
    state.scale_factor  = 2.0;

    double out = gage::convertRainfall(0.75, state);
    EXPECT_DOUBLE_EQ(out, 0.75 * 2.0);
}

TEST(GageConvertRainfall, ScaleFactorComposesWithUnitsAndAdjust) {
    gage::GageState state;
    state.rain_type     = gage::RainType::INTENSITY;
    state.units_factor  = 25.4;  // in -> mm
    state.adjust_factor = 1.1;   // monthly adjustment
    state.scale_factor  = 0.5;

    double out = gage::convertRainfall(2.0, state);
    EXPECT_DOUBLE_EQ(out, 2.0 * 25.4 * 0.5 * 1.1);
}

// ============================================================================
// updateAllGages — full primary path
// ============================================================================

namespace {

// Build a context with one gage tied to one INTENSITY timeseries containing
// a single non-zero entry.  Returns the OADate at which to query.
double setUpOneGageOneTimeseries(SimulationContext& ctx,
                                 double scale_factor,
                                 double ts_value = 1.0) {
    // Register names so n_gages() / table_names.size() report correctly.
    ctx.gage_names.add("RG1");
    ctx.table_names.add("TS1");
    ctx.gages.resize(1);

    // Timeseries: one entry at OADate = 0.0 (start of epoch), value = ts_value.
    int tidx = ctx.tables.add("TS1", TableType::TIMESERIES);
    auto& tbl = ctx.tables[tidx];
    tbl.x = {0.0, 1.0};       // Two points so the cursor has somewhere to land.
    tbl.y = {ts_value, 0.0};
    tbl.cursor.index = 0;

    // Gage wiring.
    ctx.gages.rain_type[0]      = 0;                       // INTENSITY (in/hr already)
    ctx.gages.interval_sec[0]   = 3600;
    ctx.gages.source[0]         = RainSource::TIMESERIES;
    ctx.gages.ts_index[0]       = tidx;
    ctx.gages.scale_factor[0]   = scale_factor;
    ctx.gages.snow_factor[0]    = 1.0;
    ctx.gages.co_gage_index[0]  = -1;
    ctx.gages.api_rainfall[0]   = -1.0;

    // Query at a time inside the first interval [0.0, 0.0 + 3600s).
    return 0.0;
}

} // namespace

TEST(GageUpdateAllGages, ScaleFactorMultipliesPrimaryRainfall) {
    SimulationContext ctx_a, ctx_b;

    double t_a = setUpOneGageOneTimeseries(ctx_a, /*scale_factor=*/1.0, /*ts_value=*/2.5);
    double t_b = setUpOneGageOneTimeseries(ctx_b, /*scale_factor=*/2.0, /*ts_value=*/2.5);

    gage::updateAllGages(ctx_a, t_a);
    gage::updateAllGages(ctx_b, t_b);

    ASSERT_GT(ctx_a.gages.rainfall[0], 0.0)
        << "Baseline gage produced no rainfall — fixture is broken.";
    EXPECT_NEAR(ctx_b.gages.rainfall[0],
                2.0 * ctx_a.gages.rainfall[0],
                std::abs(2.0 * ctx_a.gages.rainfall[0]) * 1e-9);
}

TEST(GageUpdateAllGages, ApiOverrideBypassesScaleFactor) {
    SimulationContext ctx;
    double t = setUpOneGageOneTimeseries(ctx, /*scale_factor=*/2.0, /*ts_value=*/1.0);
    ctx.gages.api_rainfall[0] = 0.4;

    gage::updateAllGages(ctx, t);

    // API rainfall is the final value — scale_factor must not be re-applied.
    EXPECT_DOUBLE_EQ(ctx.gages.rainfall[0], 0.4);
}

// ============================================================================
// updateAllGages — co-gage ratio
// ============================================================================

TEST(GageUpdateAllGages, CoGageRainfallRescaledByRatio) {
    SimulationContext ctx;

    // Two gages, both pointing at TS1.  Gage 1 is a co-gage of gage 0.
    ctx.gage_names.add("RG1");
    ctx.gage_names.add("RG2");
    ctx.table_names.add("TS1");
    ctx.gages.resize(2);

    int tidx = ctx.tables.add("TS1", TableType::TIMESERIES);
    auto& tbl = ctx.tables[tidx];
    tbl.x = {0.0, 1.0};
    tbl.y = {1.0, 0.0};
    tbl.cursor.index = 0;

    for (int j = 0; j < 2; ++j) {
        ctx.gages.rain_type[j]    = 0;
        ctx.gages.interval_sec[j] = 3600;
        ctx.gages.source[j]       = RainSource::TIMESERIES;
        ctx.gages.ts_index[j]     = tidx;
        ctx.gages.snow_factor[j]  = 1.0;
        ctx.gages.api_rainfall[j] = -1.0;
    }

    // Primary scale_factor=2.0; secondary scale_factor=3.0; co_gage_index=0.
    ctx.gages.scale_factor[0]  = 2.0;
    ctx.gages.scale_factor[1]  = 3.0;
    ctx.gages.co_gage_index[0] = -1;
    ctx.gages.co_gage_index[1] = 0;

    gage::updateAllGages(ctx, /*current_time=*/0.0);

    ASSERT_GT(ctx.gages.rainfall[0], 0.0);
    // Secondary = primary * (3.0 / 2.0).
    EXPECT_NEAR(ctx.gages.rainfall[1],
                ctx.gages.rainfall[0] * 3.0 / 2.0,
                std::abs(ctx.gages.rainfall[0] * 3.0 / 2.0) * 1e-9);
}

TEST(GageUpdateAllGages, CoGageWithEqualScaleFactorsCopiesPrimary) {
    SimulationContext ctx;

    ctx.gage_names.add("RG1");
    ctx.gage_names.add("RG2");
    ctx.table_names.add("TS1");
    ctx.gages.resize(2);

    int tidx = ctx.tables.add("TS1", TableType::TIMESERIES);
    auto& tbl = ctx.tables[tidx];
    tbl.x = {0.0, 1.0};
    tbl.y = {0.8, 0.0};
    tbl.cursor.index = 0;

    for (int j = 0; j < 2; ++j) {
        ctx.gages.rain_type[j]    = 0;
        ctx.gages.interval_sec[j] = 3600;
        ctx.gages.source[j]       = RainSource::TIMESERIES;
        ctx.gages.ts_index[j]     = tidx;
        ctx.gages.snow_factor[j]  = 1.0;
        ctx.gages.api_rainfall[j] = -1.0;
        ctx.gages.scale_factor[j] = 1.5;
    }
    ctx.gages.co_gage_index[0] = -1;
    ctx.gages.co_gage_index[1] = 0;

    gage::updateAllGages(ctx, /*current_time=*/0.0);

    EXPECT_DOUBLE_EQ(ctx.gages.rainfall[1], ctx.gages.rainfall[0]);
}

// ============================================================================
// getReportRainfall — scale_factor parity
// ============================================================================

TEST(GageGetReportRainfall, ScaleFactorAppliedToReport) {
    SimulationContext ctx_a, ctx_b;
    double t_a = setUpOneGageOneTimeseries(ctx_a, /*scale_factor=*/1.0, /*ts_value=*/1.2);
    double t_b = setUpOneGageOneTimeseries(ctx_b, /*scale_factor=*/2.0, /*ts_value=*/1.2);

    // getReportRainfall reads the cursor — match what updateAllGages would set.
    ctx_a.tables[0].cursor.index = 0;
    ctx_b.tables[0].cursor.index = 0;

    double r_a = gage::getReportRainfall(ctx_a, 0, t_a);
    double r_b = gage::getReportRainfall(ctx_b, 0, t_b);

    ASSERT_GT(r_a, 0.0);
    EXPECT_NEAR(r_b, 2.0 * r_a, std::abs(2.0 * r_a) * 1e-9);
}
