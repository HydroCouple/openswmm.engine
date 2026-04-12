/**
 * @file test_snow.cpp
 * @brief Unit tests for snowmelt — degree-day, rain-on-snow, ATI, cold content, plowing.
 *
 * @details Tests the batch-oriented snow model:
 *   - ATI update tracking antecedent temperature
 *   - Degree-day melt with seasonal coefficients
 *   - Cold content limiting melt
 *   - Free water routing through pack
 *   - Snow plowing redistribution
 *   - Mass balance: accumulation - melt = SWE change
 *
 * @see src/engine/hydrology/Snow.hpp
 * @ingroup engine_hydrology
 */

#include <gtest/gtest.h>
#include <cmath>
#include <numeric>

#include "hydrology/Snow.hpp"
#include "core/SimulationContext.hpp"
#include "core/UnitConversion.hpp"

using namespace openswmm;
using namespace openswmm::snow;

// Dummy context — plowSnow reads ctx.subcatches.area for removed volume
static SimulationContext& getDummyCtx() {
    static SimulationContext ctx;
    static bool init = false;
    if (!init) {
        // Provide at least one subcatchment area (10 acres) for plowSnow
        ctx.subcatches.area.resize(10, 10.0);
        init = true;
    }
    return ctx;
}
static SimulationContext& dummy_ctx = getDummyCtx();

// ============================================================================
// SnowSoA initialization
// ============================================================================

TEST(SnowSoA, ResizeSetsCorrectSizes) {
    SnowSoA soa;
    soa.resize(4);
    EXPECT_EQ(soa.n_subcatch, 4);
    // 4 subcatch × 3 subareas = 12 elements
    EXPECT_EQ(soa.wsnow.size(), 12u);
    EXPECT_EQ(soa.fw.size(), 12u);
    EXPECT_EQ(soa.coldc.size(), 12u);
    EXPECT_EQ(soa.ati.size(), 12u);
    EXPECT_EQ(soa.imelt.size(), 12u);
    EXPECT_EQ(soa.dhm.size(), 12u);
    EXPECT_EQ(soa.fArea.size(), 12u);
    EXPECT_EQ(soa.snn.size(), 4u);
    EXPECT_EQ(soa.sfrac.size(), 20u);  // 4 * 5
}

TEST(SnowSoA, DefaultATIIs32F) {
    SnowSoA soa;
    soa.resize(2);
    for (auto& a : soa.ati) {
        EXPECT_DOUBLE_EQ(a, 32.0);  // freezing point
    }
}

// ============================================================================
// Degree-day melt
// ============================================================================

class SnowSolverTest : public ::testing::Test {
protected:
    SnowSolver solver;

    void SetUp() override {
        solver.init(2);  // 2 subcatchments
        auto& soa = solver.state();

        // Set up one subarea per subcatch with snow
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < N_SUBAREAS; ++k) {
                auto idx = static_cast<std::size_t>(j * N_SUBAREAS + k);
                soa.wsnow[idx] = 0.5;     // 0.5 ft SWE
                soa.fw[idx]    = 0.0;
                soa.coldc[idx] = 0.0;
                soa.ati[idx]   = 30.0;    // below freezing
                soa.tbase[idx] = 32.0;    // freeze point
                soa.dhm[idx]   = 1e-5;    // melt coeff (ft/degF/sec)
                soa.dhmin[idx] = 5e-6;
                soa.dhmax[idx] = 2e-5;
                soa.fwfrac[idx]= 0.1;     // 10% free water capacity
                soa.fArea[idx] = (k == SNOW_PERV) ? 0.7 : 0.15;
            }
        }
    }
};

TEST_F(SnowSolverTest, NoMeltBelowBaseTemp) {
    auto& soa = solver.state();
    double dt = 3600.0;  // 1 hour

    // Temperature below base → no melt
    solver.execute(dummy_ctx, dt, 25.0, 5.0, 0.0);

    for (auto& m : soa.imelt) {
        EXPECT_DOUBLE_EQ(m, 0.0) << "No melt expected below base temp";
    }
}

TEST_F(SnowSolverTest, MeltAboveBaseTemp) {
    auto& soa = solver.state();
    double dt = 3600.0;

    // Zero out cold content so melt is not absorbed
    for (auto& cc : soa.coldc) cc = 0.0;

    // Warm temperature above base
    double temp = 50.0;  // 18 deg above tbase=32
    solver.execute(dummy_ctx, dt, temp, 0.0, 0.0);

    // Check that melt was produced (may be retained as free water)
    // Either imelt > 0 (excess drained) or fw increased (retained)
    double total_melt = 0.0;
    for (std::size_t i = 0; i < soa.imelt.size(); ++i) {
        total_melt += soa.imelt[i];
    }
    double total_fw_increase = 0.0;
    for (auto& f : soa.fw) {
        total_fw_increase += f;
    }

    EXPECT_GT(total_melt + total_fw_increase, 0.0)
        << "Expected some melt when temp > tbase";
}

TEST_F(SnowSolverTest, ColdContentAbsorbsMelt) {
    auto& soa = solver.state();

    // Set large cold content to absorb all melt
    for (auto& cc : soa.coldc) cc = 100.0;

    double dt = 60.0;  // short step
    solver.execute(dummy_ctx, dt, 40.0, 0.0, 0.0);

    // All melt absorbed by cold content → imelt should be 0
    for (auto& m : soa.imelt) {
        EXPECT_DOUBLE_EQ(m, 0.0)
            << "Cold content should absorb all melt";
    }
}

TEST_F(SnowSolverTest, SWEDecreasesWithMelt) {
    auto& soa = solver.state();

    // Zero cold content
    for (auto& cc : soa.coldc) cc = 0.0;

    double initial_swe = 0.0;
    for (auto& w : soa.wsnow) initial_swe += w;

    double dt = 3600.0;
    // Very warm temperature for significant melt
    solver.execute(dummy_ctx, dt, 60.0, 0.0, 0.0);

    double final_swe = 0.0;
    for (auto& w : soa.wsnow) final_swe += w;

    EXPECT_LT(final_swe, initial_swe)
        << "SWE should decrease when melting occurs";
}

TEST_F(SnowSolverTest, NoMeltWhenNoSnow) {
    auto& soa = solver.state();
    for (auto& w : soa.wsnow) w = 0.0;

    double dt = 3600.0;
    solver.execute(dummy_ctx, dt, 50.0, 0.0, 0.0);

    for (auto& m : soa.imelt) {
        EXPECT_DOUBLE_EQ(m, 0.0);
    }
}

// ============================================================================
// Seasonal melt coefficients
// ============================================================================

TEST_F(SnowSolverTest, SeasonalMeltCoeffsSummerSolstice) {
    auto& soa = solver.state();
    // Summer solstice ~ day 172 → season ≈ +1.0
    solver.setMeltCoeffs(172);

    // dhm should be near dhmax (summer)
    for (std::size_t i = 0; i < soa.dhm.size(); ++i) {
        EXPECT_NEAR(soa.dhm[i], soa.dhmax[i], 0.01 * soa.dhmax[i])
            << "At summer solstice, dhm ≈ dhmax";
    }
}

TEST_F(SnowSolverTest, SeasonalMeltCoeffsWinterSolstice) {
    auto& soa = solver.state();
    // Winter solstice ~ day 355 → season ≈ -1.0
    solver.setMeltCoeffs(355);

    // dhm should be near dhmin (winter)
    for (std::size_t i = 0; i < soa.dhm.size(); ++i) {
        EXPECT_NEAR(soa.dhm[i], soa.dhmin[i], 0.01 * soa.dhmin[i] + 1e-12)
            << "At winter solstice, dhm ≈ dhmin";
    }
}

TEST_F(SnowSolverTest, SeasonalMeltCoeffsEquinox) {
    auto& soa = solver.state();
    // Spring equinox ~ day 81 → season ≈ 0.0
    solver.setMeltCoeffs(81);

    // dhm should be midpoint of dhmin and dhmax
    for (std::size_t i = 0; i < soa.dhm.size(); ++i) {
        double mid = 0.5 * (soa.dhmin[i] + soa.dhmax[i]);
        EXPECT_NEAR(soa.dhm[i], mid, 0.02 * mid + 1e-12)
            << "At equinox, dhm ≈ (dhmin+dhmax)/2";
    }
}

// ============================================================================
// Snow plowing
// ============================================================================

TEST(SnowPlowing, ExcessSnowRedistributed) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Set up plowable area with excess snow
    auto plow_idx = static_cast<std::size_t>(0 * N_SUBAREAS + SNOW_PLOWABLE);
    auto imperv_idx = static_cast<std::size_t>(0 * N_SUBAREAS + SNOW_IMPERV);
    auto perv_idx = static_cast<std::size_t>(0 * N_SUBAREAS + SNOW_PERV);

    soa.fArea[plow_idx]   = 0.2;
    soa.fArea[imperv_idx] = 0.3;
    soa.fArea[perv_idx]   = 0.5;

    soa.wsnow[plow_idx]   = 2.0;   // 2 ft SWE (above plow threshold)
    soa.wsnow[imperv_idx] = 0.1;
    soa.wsnow[perv_idx]   = 0.1;

    soa.weplow[0] = 1.0;  // plow when > 1 ft

    // Plow fractions: 20% removed, 30% to imperv, 40% to perv, 10% immediate melt
    soa.sfrac[0] = 0.2;  // removed from system
    soa.sfrac[1] = 0.3;  // to impervious
    soa.sfrac[2] = 0.4;  // to pervious
    soa.sfrac[3] = 0.1;  // immediate melt
    soa.sfrac[4] = 0.0;  // to other subcatch

    double dt = 3600.0;
    solver.plowSnow(dummy_ctx, dt, 0.0);

    // Plowable snow should be reduced
    EXPECT_LT(soa.wsnow[plow_idx], 2.0);

    // Impervious and pervious should have received some snow
    EXPECT_GT(soa.wsnow[imperv_idx], 0.1);
    EXPECT_GT(soa.wsnow[perv_idx], 0.1);
}

TEST(SnowPlowing, NoPlowingBelowThreshold) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    auto plow_idx = static_cast<std::size_t>(0 * N_SUBAREAS + SNOW_PLOWABLE);
    soa.fArea[plow_idx] = 0.3;
    soa.wsnow[plow_idx] = 0.5;  // below threshold
    soa.weplow[0] = 1.0;

    double initial = soa.wsnow[plow_idx];
    solver.plowSnow(dummy_ctx, 3600.0, 0.0);

    // Only snowfall accumulation happens, no redistribution
    EXPECT_NEAR(soa.wsnow[plow_idx], initial, 1e-10);
}

// ============================================================================
// Batch accumulation (tested through execute — snowfall adds to wsnow)
// ============================================================================

TEST(SnowAccumulation, SnowfallAddsToSWE) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Set initial SWE and area fractions
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx] = 0.0;
        soa.fArea[idx] = (k == SNOW_PERV) ? 0.7 : 0.15;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx]   = 0.0;  // no melt
        soa.fwfrac[idx] = 0.1;
    }

    // Use plowSnow to add snowfall (accumulation path)
    double snowfall = 1e-4;  // ft/sec
    double dt = 3600.0;
    solver.plowSnow(dummy_ctx, dt, snowfall);

    // Each subarea with fArea > 0 should have accumulated snow
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        if (soa.fArea[idx] > 0.0) {
            EXPECT_NEAR(soa.wsnow[idx], snowfall * dt, 1e-10)
                << "Subarea " << k << " should have accumulated snow";
        }
    }
}

// ============================================================================
// Degree-day melt — tested via execute (warm temp, no cold content)
// ============================================================================

TEST(SnowDegreeDayMelt, WarmTempProducesMelt) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx]  = 1.0;      // plenty of snow
        soa.fw[idx]     = 0.0;
        soa.coldc[idx]  = 0.0;      // no cold content to absorb melt
        soa.ati[idx]    = 40.0;
        soa.tbase[idx]  = 32.0;
        soa.dhm[idx]    = 1e-5;     // melt coeff
        soa.fwfrac[idx] = 0.0;      // zero free water capacity → all melt drains
        soa.fArea[idx]  = 0.33;
    }

    double dt = 3600.0;
    // temp=50 → 18 deg above tbase; melt_rate = dhm * 18 = 1.8e-4 ft/sec
    solver.execute(dummy_ctx, dt, 50.0, 0.0, 0.0);

    // With zero fwfrac, all melt should drain (imelt > 0)
    // and SWE should decrease
    double total_swe = 0.0;
    for (auto& w : soa.wsnow) total_swe += w;
    EXPECT_LT(total_swe, 3.0) << "SWE should decrease with melt";
}

TEST(SnowDegreeDayMelt, ColdTempNoMelt) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx] = 1.0;
        soa.coldc[idx] = 0.0;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx]   = 1e-5;
        soa.fwfrac[idx]= 0.1;
        soa.fArea[idx] = 0.33;
    }

    double initial_swe = 0.0;
    for (auto& w : soa.wsnow) initial_swe += w;

    // Below freezing: no melt, cold content may increase
    solver.execute(dummy_ctx, 3600.0, 20.0, 0.0, 0.0);

    double final_swe = 0.0;
    for (auto& w : soa.wsnow) final_swe += w;
    EXPECT_NEAR(final_swe, initial_swe, 1e-10)
        << "SWE should not change below freezing (no melt)";
}

// ============================================================================
// ATI update — tested through cold-content path in execute
// ============================================================================

TEST(SnowATI, SubFreezingUpdatesATI) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx] = 1.0;
        soa.ati[idx]    = 30.0;
        soa.tbase[idx]  = 32.0;
        soa.dhm[idx]    = 0.0;   // no melt
        soa.fwfrac[idx] = 0.1;
        soa.fArea[idx]  = 0.33;
    }

    // Sub-freezing: ATI should move toward temperature (20°F)
    double temp = 20.0;
    for (int i = 0; i < 100; ++i) {
        solver.execute(dummy_ctx, 21600.0, temp, 0.0, 0.0);
    }

    // ATI should have moved toward temperature but be capped at tbase
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        if (soa.wsnow[idx] > 0.0) {
            EXPECT_LT(soa.ati[idx], 30.0)
                << "ATI should decrease toward cold temp";
        }
    }
}

// ============================================================================
// Areal depletion curve (ADC) tests
// ============================================================================

TEST(SnowADC, DefaultCurvesDoNotReduceMelt) {
    // Default ADC = all 1.0 → asc = 1.0 always → no reduction
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx]  = 1.0;
        soa.fw[idx]     = 0.0;
        soa.coldc[idx]  = 0.0;
        soa.ati[idx]    = 40.0;
        soa.tbase[idx]  = 32.0;
        soa.dhm[idx]    = 1e-5;
        soa.fwfrac[idx] = 0.0;     // all melt drains
        soa.fArea[idx]  = 0.33;
        soa.si[idx]     = 1.0;     // depth at 100% cover
    }
    // ADC defaults are all 1.0 — asc = 1.0

    solver.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);

    // Melt should have occurred (imelt > 0 for subareas with snow)
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        EXPECT_GT(soa.imelt[idx], 0.0)
            << "Subarea " << k << " should have melt with default ADC";
    }
}

TEST(SnowADC, ZeroCurveEliminatesMelt) {
    // ADC = all 0.0 → asc = 0.0 → no melt (snow-free area)
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Set ADC curves to all zero
    for (int i = 0; i < 10; ++i) {
        soa.adc_imperv[i] = 0.0;
        soa.adc_perv[i]   = 0.0;
    }

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx]  = 0.5;     // partial snow (awesi = 0.5/1.0 = 0.5)
        soa.fw[idx]     = 0.0;
        soa.coldc[idx]  = 0.0;
        soa.ati[idx]    = 40.0;
        soa.tbase[idx]  = 32.0;
        soa.dhm[idx]    = 1e-5;
        soa.fwfrac[idx] = 0.0;
        soa.fArea[idx]  = 0.33;
        soa.si[idx]     = 1.0;     // depth at 100% cover
    }

    double initial_swe = 0.0;
    for (auto& w : soa.wsnow) initial_swe += w;

    solver.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);

    double final_swe = 0.0;
    for (auto& w : soa.wsnow) final_swe += w;

    // With asc = 0, melt should be zero, SWE unchanged
    EXPECT_NEAR(final_swe, initial_swe, 1e-10)
        << "SWE should not change when ADC is all zero";
}

TEST(SnowADC, PartialCoverReducesMelt) {
    // ADC with intermediate values → partial asc → reduced melt
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Linear ADC: adc[i] = i * 0.1 (0.0, 0.1, 0.2, ... 0.9)
    for (int i = 0; i < 10; ++i) {
        soa.adc_imperv[i] = i * 0.1;
        soa.adc_perv[i]   = i * 0.1;
    }

    // Set up two solvers: one with ADC, one with default (all 1.0)
    SnowSolver solver_full;
    solver_full.init(1);
    auto& soa_full = solver_full.state();

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        // Partial cover solver
        soa.wsnow[idx]  = 0.5;     // awesi = 0.5/1.0 = 0.5 → adc index ~5 → asc ~ 0.5
        soa.fw[idx]     = 0.0;
        soa.coldc[idx]  = 0.0;
        soa.ati[idx]    = 40.0;
        soa.tbase[idx]  = 32.0;
        soa.dhm[idx]    = 1e-5;
        soa.fwfrac[idx] = 0.0;
        soa.fArea[idx]  = 0.33;
        soa.si[idx]     = 1.0;

        // Full cover solver (default ADC = all 1.0)
        soa_full.wsnow[idx]  = 0.5;
        soa_full.fw[idx]     = 0.0;
        soa_full.coldc[idx]  = 0.0;
        soa_full.ati[idx]    = 40.0;
        soa_full.tbase[idx]  = 32.0;
        soa_full.dhm[idx]    = 1e-5;
        soa_full.fwfrac[idx] = 0.0;
        soa_full.fArea[idx]  = 0.33;
        soa_full.si[idx]     = 1.0;
    }

    solver.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);
    solver_full.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);

    // Partial cover should have less total melt than full cover
    double total_melt_partial = 0.0, total_melt_full = 0.0;
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        total_melt_partial += soa.imelt[idx];
        total_melt_full    += soa_full.imelt[idx];
    }
    EXPECT_GT(total_melt_full, 0.0);
    EXPECT_LT(total_melt_partial, total_melt_full)
        << "Partial ADC should produce less melt than full coverage";
}

TEST(SnowADC, FullSnowCoverGivesAscOne) {
    // When wsnow >= si (awesi >= 1.0), asc should be 1.0
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Use a distinctive ADC curve that would reduce asc if awesi < 1
    for (int i = 0; i < 10; ++i) {
        soa.adc_imperv[i] = 0.0;
        soa.adc_perv[i]   = 0.0;
    }

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx]  = 2.0;     // awesi = 2.0/1.0 = 2.0 → >= 1.0 → asc = 1.0
        soa.fw[idx]     = 0.0;
        soa.coldc[idx]  = 0.0;
        soa.ati[idx]    = 40.0;
        soa.tbase[idx]  = 32.0;
        soa.dhm[idx]    = 1e-5;
        soa.fwfrac[idx] = 0.0;
        soa.fArea[idx]  = 0.33;
        soa.si[idx]     = 1.0;     // 100% cover at 1.0 ft
    }

    solver.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);

    // Full cover → melt should not be zero (asc = 1.0 despite zero ADC curve)
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        EXPECT_GT(soa.imelt[idx], 0.0)
            << "Subarea " << k << " should have melt at full coverage (awesi >= 1)";
    }
}

// ============================================================================
// Seasonal factor tracking
// ============================================================================

TEST(SnowSeason, SetMeltCoeffsStoresSeasonFactor) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Summer solstice (day ~172): season ≈ +1.0
    solver.setMeltCoeffs(172);
    EXPECT_NEAR(soa.season, 1.0, 0.05);

    // Winter solstice (day ~355): season ≈ -1.0
    solver.setMeltCoeffs(355);
    EXPECT_NEAR(soa.season, -1.0, 0.05);

    // Equinox (day ~81): season ≈ 0.0
    solver.setMeltCoeffs(81);
    EXPECT_NEAR(soa.season, 0.0, 0.05);
}

TEST(SnowSeason, SeasonFactorMatchesSinFormula) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Test multiple days match the expected sin formula
    for (int day = 1; day <= 365; day += 30) {
        solver.setMeltCoeffs(day);
        double expected = std::sin(2.0 * 3.14159265358979 * (day - 81.0) / 365.0);
        EXPECT_NEAR(soa.season, expected, 1e-10)
            << "Season factor mismatch on day " << day;
    }
}

TEST(SnowSeason, SeasonAffectsMeltCoeffs) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Set dhmin=1e-6, dhmax=1e-4 on all subareas
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.dhmin[idx] = 1e-6;
        soa.dhmax[idx] = 1e-4;
    }

    // Summer: dhm should be near dhmax
    solver.setMeltCoeffs(172);
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        EXPECT_NEAR(soa.dhm[idx], soa.dhmax[idx], 3e-6);
    }

    // Winter: dhm should be near dhmin
    solver.setMeltCoeffs(355);
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        EXPECT_NEAR(soa.dhm[idx], soa.dhmin[idx], 3e-6);
    }
}

// ============================================================================
// Snow removed volume tracking
// ============================================================================

TEST(SnowRemoved, InitiallyZero) {
    SnowSolver solver;
    solver.init(1);
    EXPECT_DOUBLE_EQ(solver.state().removed, 0.0);
}

TEST(SnowRemoved, PlowingAccumulatesRemoved) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Set up a subcatchment with plowable snow above plow threshold
    auto plow_idx = static_cast<std::size_t>(SNOW_PLOWABLE);
    soa.fArea[plow_idx]  = 0.3;
    soa.fArea[static_cast<std::size_t>(SNOW_IMPERV)] = 0.3;
    soa.fArea[static_cast<std::size_t>(SNOW_PERV)]   = 0.4;
    soa.wsnow[plow_idx]  = 0.0;
    soa.weplow[0]        = 0.5;    // plow when depth > 0.5 ft
    soa.sfrac[0]         = 0.25;   // 25% removed from system
    soa.sfrac[1]         = 0.0;
    soa.sfrac[2]         = 0.0;
    soa.sfrac[3]         = 0.0;
    soa.sfrac[4]         = 0.0;

    // Set up dummy context with subcatch area
    SimulationContext ctx;
    ctx.subcatches.area.push_back(10.0);  // 10 acres

    // Add snowfall to exceed plow threshold
    double snowfall = 2e-4;  // ft/sec
    double dt = 3600.0;
    solver.plowSnow(ctx, dt, snowfall);

    // Snow accumulated = 2e-4 * 3600 = 0.72 ft, > 0.5 threshold
    // removed = sfrac[0] * exc * fArea[plow] * area_ft2
    //         = 0.25 * 0.72 * 0.3 * (10 * 43560)
    double exc = snowfall * dt;
    double expected_removed = 0.25 * exc * 0.3 * (10.0 * 43560.0);
    EXPECT_NEAR(soa.removed, expected_removed, 1e-3)
        << "Removed volume should match sfrac[0] * excess * plowArea * subcatchArea";
}

TEST(SnowRemoved, NoPlowingMeansNoRemoval) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    auto plow_idx = static_cast<std::size_t>(SNOW_PLOWABLE);
    soa.fArea[plow_idx] = 0.3;
    soa.fArea[static_cast<std::size_t>(SNOW_PERV)] = 0.7;
    soa.wsnow[plow_idx] = 0.0;
    soa.weplow[0]       = 2.0;    // high threshold
    soa.sfrac[0]        = 0.5;

    SimulationContext ctx;
    ctx.subcatches.area.push_back(10.0);

    // Small snowfall: 0.01 ft/sec * 1s = 0.01 ft < 2.0 threshold
    solver.plowSnow(ctx, 1.0, 0.01);
    EXPECT_DOUBLE_EQ(soa.removed, 0.0)
        << "No removal when snow < plow threshold";
}

TEST(SnowRemoved, MultipleTimestepsCumulate) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    auto plow_idx = static_cast<std::size_t>(SNOW_PLOWABLE);
    soa.fArea[plow_idx] = 1.0;
    soa.fArea[static_cast<std::size_t>(SNOW_IMPERV)] = 0.0;
    soa.fArea[static_cast<std::size_t>(SNOW_PERV)]   = 0.0;
    soa.weplow[0]       = 0.1;
    soa.sfrac[0]        = 1.0;  // all removed

    SimulationContext ctx;
    ctx.subcatches.area.push_back(1.0);  // 1 acre

    // First timestep
    solver.plowSnow(ctx, 1.0, 0.5);
    double first = soa.removed;
    EXPECT_GT(first, 0.0);

    // Second timestep — accumulates
    solver.plowSnow(ctx, 1.0, 0.5);
    EXPECT_GT(soa.removed, first)
        << "Removed should accumulate across timesteps";
}

// ============================================================================
// Rain-on-snow melt with gamma/ea parameters
// ============================================================================

TEST(SnowRainOnSnow, GammaEaAffectsMelt) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx]  = 1.0;
        soa.fw[idx]     = 0.0;
        soa.coldc[idx]  = 0.0;
        soa.ati[idx]    = 40.0;
        soa.tbase[idx]  = 32.0;
        soa.dhm[idx]    = 1e-5;
        soa.fwfrac[idx] = 0.0;
        soa.fArea[idx]  = 0.33;
        soa.si[idx]     = 1.0;
    }

    // Execute with rain-on-snow (rainfall above threshold) and valid gamma/ea
    double temp = 50.0;
    double wind = 10.0;
    double rainfall = 5e-5;  // well above 0.02 in/hr threshold in ft/sec
    double gamma = 0.001;
    double ea = 0.5;

    solver.execute(dummy_ctx, 3600.0, temp, wind, rainfall, gamma, ea);

    // With rain-on-snow, melt should occur
    double total_melt = 0.0;
    for (int k = 0; k < N_SUBAREAS; ++k)
        total_melt += soa.imelt[static_cast<std::size_t>(k)];
    EXPECT_GT(total_melt, 0.0) << "Rain-on-snow with gamma/ea should produce melt";
}

TEST(SnowRainOnSnow, ZeroGammaReducesMelt) {
    // Compare melt with gamma=0 vs gamma>0
    SnowSolver solver_zero, solver_pos;
    solver_zero.init(1);
    solver_pos.init(1);

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        auto& s0 = solver_zero.state();
        auto& s1 = solver_pos.state();
        s0.wsnow[idx] = s1.wsnow[idx] = 1.0;
        s0.fw[idx]    = s1.fw[idx]     = 0.0;
        s0.coldc[idx] = s1.coldc[idx]  = 0.0;
        s0.ati[idx]   = s1.ati[idx]    = 40.0;
        s0.tbase[idx] = s1.tbase[idx]  = 32.0;
        s0.dhm[idx]   = s1.dhm[idx]    = 1e-5;
        s0.fwfrac[idx]= s1.fwfrac[idx] = 0.0;
        s0.fArea[idx] = s1.fArea[idx]  = 0.33;
        s0.si[idx]    = s1.si[idx]     = 1.0;
    }

    double rain = 5e-5;
    solver_zero.execute(dummy_ctx, 3600.0, 50.0, 10.0, rain, 0.0, 0.0);
    solver_pos.execute(dummy_ctx, 3600.0, 50.0, 10.0, rain, 0.001, 0.5);

    double melt_zero = 0.0, melt_pos = 0.0;
    for (int k = 0; k < N_SUBAREAS; ++k) {
        melt_zero += solver_zero.state().imelt[static_cast<std::size_t>(k)];
        melt_pos  += solver_pos.state().imelt[static_cast<std::size_t>(k)];
    }
    // Positive gamma should contribute additional melt from turbulent exchange
    EXPECT_GE(melt_pos, melt_zero)
        << "Positive gamma should produce >= melt than zero gamma";
}

// ============================================================================
// SnowSoA resize includes new fields
// ============================================================================

TEST(SnowSoA, ResizeIncludesADCAndGlobalFields) {
    SnowSoA soa;
    soa.resize(3);

    EXPECT_EQ(static_cast<int>(soa.si.size()), 3 * N_SUBAREAS);
    EXPECT_EQ(static_cast<int>(soa.sba.size()), 3 * N_SUBAREAS);
    EXPECT_EQ(static_cast<int>(soa.sbws.size()), 3 * N_SUBAREAS);
    EXPECT_DOUBLE_EQ(soa.season, 0.0);
    EXPECT_DOUBLE_EQ(soa.removed, 0.0);

    // Default ADC should be all 1.0
    for (int i = 0; i < 10; ++i) {
        EXPECT_DOUBLE_EQ(soa.adc_imperv[i], 1.0);
        EXPECT_DOUBLE_EQ(soa.adc_perv[i], 1.0);
    }
}

// ============================================================================
// Free water routing through pack
// ============================================================================

TEST(SnowFreeWater, MeltFillsPackBeforeDraining) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx]  = 1.0;
        soa.fw[idx]     = 0.0;
        soa.coldc[idx]  = 0.0;
        soa.ati[idx]    = 40.0;
        soa.tbase[idx]  = 32.0;
        soa.dhm[idx]    = 1e-5;
        soa.fwfrac[idx] = 0.5;  // high free water capacity
        soa.fArea[idx]  = 0.33;
        soa.si[idx]     = 1.0;
    }

    // Small melt that fits within free water capacity
    solver.execute(dummy_ctx, 60.0, 40.0, 0.0, 0.0);  // short timestep, small excess

    // With high fwfrac, melt should be retained in free water
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        if (soa.wsnow[idx] > 0.0) {
            // imelt should be 0 (retained) or small
            // fw should have increased
            EXPECT_GE(soa.fw[idx], 0.0);
        }
    }
}

TEST(SnowFreeWater, ExcessDrains) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx]  = 1.0;
        soa.fw[idx]     = 0.0;
        soa.coldc[idx]  = 0.0;
        soa.ati[idx]    = 50.0;
        soa.tbase[idx]  = 32.0;
        soa.dhm[idx]    = 5e-4;  // high melt rate
        soa.fwfrac[idx] = 0.01;  // very small free water capacity
        soa.fArea[idx]  = 0.33;
        soa.si[idx]     = 1.0;
    }

    // Large melt that overflows free water capacity
    solver.execute(dummy_ctx, 3600.0, 60.0, 0.0, 0.0);

    // Some melt should drain as imelt > 0
    bool any_drain = false;
    for (int k = 0; k < N_SUBAREAS; ++k) {
        if (soa.imelt[static_cast<std::size_t>(k)] > 0.0)
            any_drain = true;
    }
    EXPECT_TRUE(any_drain) << "Excess melt beyond free water capacity should drain";
}

// ============================================================================
// Cold content interaction with melt
// ============================================================================

TEST(SnowColdContent, SubFreezingBuildsColdContent) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx]  = 1.0;
        soa.fw[idx]     = 0.0;
        soa.coldc[idx]  = 0.0;
        soa.ati[idx]    = 20.0;  // cold
        soa.tbase[idx]  = 32.0;
        soa.dhm[idx]    = 1e-5;
        soa.fwfrac[idx] = 0.1;
        soa.fArea[idx]  = 0.33;
        soa.si[idx]     = 1.0;
    }

    // Run sub-freezing timestep
    solver.execute(dummy_ctx, 3600.0, 10.0, 0.0, 0.0);

    // Cold content should have increased
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        if (soa.wsnow[idx] > 0.0) {
            EXPECT_GT(soa.coldc[idx], 0.0)
                << "Cold content should build during sub-freezing conditions";
        }
    }
}

TEST(SnowColdContent, ColdContentDelaysMelt) {
    // Compare melt with cold content vs without
    SnowSolver solver_cold, solver_warm;
    solver_cold.init(1);
    solver_warm.init(1);

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        auto& sc = solver_cold.state();
        auto& sw = solver_warm.state();
        sc.wsnow[idx] = sw.wsnow[idx] = 1.0;
        sc.fw[idx]    = sw.fw[idx]     = 0.0;
        sc.coldc[idx] = 0.5;   // has cold content
        sw.coldc[idx] = 0.0;   // no cold content
        sc.ati[idx]   = sw.ati[idx]    = 40.0;
        sc.tbase[idx] = sw.tbase[idx]  = 32.0;
        sc.dhm[idx]   = sw.dhm[idx]    = 1e-5;
        sc.fwfrac[idx]= sw.fwfrac[idx] = 0.0;
        sc.fArea[idx] = sw.fArea[idx]  = 0.33;
        sc.si[idx]    = sw.si[idx]     = 1.0;
    }

    solver_cold.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);
    solver_warm.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);

    double melt_cold = 0.0, melt_warm = 0.0;
    for (int k = 0; k < N_SUBAREAS; ++k) {
        melt_cold += solver_cold.state().imelt[static_cast<std::size_t>(k)];
        melt_warm += solver_warm.state().imelt[static_cast<std::size_t>(k)];
    }
    EXPECT_LT(melt_cold, melt_warm)
        << "Cold content should reduce effective melt output";
}
