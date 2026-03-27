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

// Dummy context — snow execute/plowSnow don't actually read ctx
static SimulationContext dummy_ctx;

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
