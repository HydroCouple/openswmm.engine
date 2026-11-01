/**
 * @file      test_snow.cpp
 * @brief     Unit tests for snowmelt — degree-day, rain-on-snow, ATI, cold content, plowing.
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
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <numeric>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_heat.h>
#include <openswmm/engine/openswmm_snow.h>
#include <openswmm/engine/openswmm_snow2d.h>
#include <openswmm/engine/openswmm_hotstart.h>

#include "hydrology/Snow.hpp"
#include "hydrology/Gage.hpp"
#include "hydrology/SourceWaterDriver.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "core/SWMMEngine.hpp"
#include "core/SimulationContext.hpp"
#include "core/UnitConversion.hpp"
#include "core/DateTime.hpp"
#include "hydrology/surface/UebSnow.hpp"
#include "hydrology/surface/MeshSnow.hpp"
#include "hydrology/Radiation.hpp"
#include "hydrology/surface/UebLumped.hpp"
#include "hydrology/surface/UebHeld.hpp"
#include "hydrology/surface/SnowCheckpoint.hpp"
#include <future>

using namespace openswmm;
using namespace openswmm::snow;

// Dummy context — plowSnow reads ctx.subcatches.area for removed volume
/**
 * @brief Borrow a persistent context supplying snow-plow subcatchment areas.
 *
 * The separate init flag and mutable shared state require serialized use. Does not reset later
 * mutations, populate full subcatchment metadata or validate/initialize a model; reference lasts
 * for process lifetime.
 * @return Mutable static context whose area vector is first initialized to ten entries of 10
 *     authored acres.
 */
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
    EXPECT_EQ(soa.sfrac.size(), 20u); // 4 * 5
}

TEST(SnowSoA, DefaultATIIs32F) {
    SnowSoA soa;
    soa.resize(2);
    for (auto& a : soa.ati) {
        EXPECT_DOUBLE_EQ(a, 32.0); // freezing point
    }
}

// ============================================================================
// Degree-day melt
// ============================================================================

class SnowSolverTest : public ::testing::Test {
protected:
    SnowSolver solver;

    /**
     * @brief Initialize two snow catchments with uniform subarea snow state.
     *
     * Every subarea starts with .5 ft SWE, zero free water/cold content, ATI 30 F, base 32 F, bounded
     * melt coefficients in ft/(F*s), free-water capacity .1 and pervious/impervious area fractions
     * .7/.15. No melt step occurs. Fatal GoogleTest assertions return from this hook on failure; any
     * created handle remains owned by the fixture for teardown.
     */
    void SetUp() override {
        solver.init(2); // 2 subcatchments
        auto& soa = solver.state();

        // Set up one subarea per subcatch with snow
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < N_SUBAREAS; ++k) {
                auto idx = static_cast<std::size_t>(j * N_SUBAREAS + k);
                soa.wsnow[idx] = 0.5; // 0.5 ft SWE
                soa.fw[idx] = 0.0;
                soa.coldc[idx] = 0.0;
                soa.ati[idx] = 30.0;   // below freezing
                soa.tbase[idx] = 32.0; // freeze point
                soa.dhm[idx] = 1e-5;   // melt coeff (ft/degF/sec)
                soa.dhmin[idx] = 5e-6;
                soa.dhmax[idx] = 2e-5;
                soa.fwfrac[idx] = 0.1; // 10% free water capacity
                soa.fArea[idx] = (k == SNOW_PERV) ? 0.7 : 0.15;
            }
        }
    }
};

TEST_F(SnowSolverTest, NoMeltBelowBaseTemp) {
    auto& soa = solver.state();
    double dt = 3600.0; // 1 hour

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
    for (auto& cc : soa.coldc)
        cc = 0.0;

    // Warm temperature above base
    double temp = 50.0; // 18 deg above tbase=32
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

    EXPECT_GT(total_melt + total_fw_increase, 0.0) << "Expected some melt when temp > tbase";
}

TEST_F(SnowSolverTest, ColdContentAbsorbsMelt) {
    auto& soa = solver.state();

    // Set large cold content to absorb all melt
    for (auto& cc : soa.coldc)
        cc = 100.0;

    double dt = 60.0; // short step
    solver.execute(dummy_ctx, dt, 40.0, 0.0, 0.0);

    // All melt absorbed by cold content → imelt should be 0
    for (auto& m : soa.imelt) {
        EXPECT_DOUBLE_EQ(m, 0.0) << "Cold content should absorb all melt";
    }
}

TEST_F(SnowSolverTest, SWEDecreasesWithMelt) {
    auto& soa = solver.state();

    // Zero cold content
    for (auto& cc : soa.coldc)
        cc = 0.0;

    double initial_swe = 0.0;
    for (auto& w : soa.wsnow)
        initial_swe += w;

    double dt = 3600.0;
    // Very warm temperature for significant melt
    solver.execute(dummy_ctx, dt, 60.0, 0.0, 0.0);

    double final_swe = 0.0;
    for (auto& w : soa.wsnow)
        final_swe += w;

    EXPECT_LT(final_swe, initial_swe) << "SWE should decrease when melting occurs";
}

TEST_F(SnowSolverTest, NoMeltWhenNoSnow) {
    auto& soa = solver.state();
    for (auto& w : soa.wsnow)
        w = 0.0;

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
        EXPECT_NEAR(soa.dhm[i], mid, 0.02 * mid + 1e-12) << "At equinox, dhm ≈ (dhmin+dhmax)/2";
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

    soa.fArea[plow_idx] = 0.2;
    soa.fArea[imperv_idx] = 0.3;
    soa.fArea[perv_idx] = 0.5;

    soa.wsnow[plow_idx] = 2.0; // 2 ft SWE (above plow threshold)
    soa.wsnow[imperv_idx] = 0.1;
    soa.wsnow[perv_idx] = 0.1;

    soa.weplow[0] = 1.0; // plow when > 1 ft

    // Plow fractions: 20% removed, 30% to imperv, 40% to perv, 10% immediate melt
    soa.sfrac[0] = 0.2; // removed from system
    soa.sfrac[1] = 0.3; // to impervious
    soa.sfrac[2] = 0.4; // to pervious
    soa.sfrac[3] = 0.1; // immediate melt
    soa.sfrac[4] = 0.0; // to other subcatch

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
    soa.wsnow[plow_idx] = 0.5; // below threshold
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
        soa.dhm[idx] = 0.0; // no melt
        soa.fwfrac[idx] = 0.1;
    }

    // Use plowSnow to add snowfall (accumulation path)
    double snowfall = 1e-4; // ft/sec
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
        soa.wsnow[idx] = 1.0; // plenty of snow
        soa.fw[idx] = 0.0;
        soa.coldc[idx] = 0.0; // no cold content to absorb melt
        soa.ati[idx] = 40.0;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx] = 1e-5;   // melt coeff
        soa.fwfrac[idx] = 0.0; // zero free water capacity → all melt drains
        soa.fArea[idx] = 0.33;
    }

    double dt = 3600.0;
    // temp=50 → 18 deg above tbase; melt_rate = dhm * 18 = 1.8e-4 ft/sec
    solver.execute(dummy_ctx, dt, 50.0, 0.0, 0.0);

    // With zero fwfrac, all melt should drain (imelt > 0)
    // and SWE should decrease
    double total_swe = 0.0;
    for (auto& w : soa.wsnow)
        total_swe += w;
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
        soa.dhm[idx] = 1e-5;
        soa.fwfrac[idx] = 0.1;
        soa.fArea[idx] = 0.33;
    }

    double initial_swe = 0.0;
    for (auto& w : soa.wsnow)
        initial_swe += w;

    // Below freezing: no melt, cold content may increase
    solver.execute(dummy_ctx, 3600.0, 20.0, 0.0, 0.0);

    double final_swe = 0.0;
    for (auto& w : soa.wsnow)
        final_swe += w;
    EXPECT_NEAR(final_swe, initial_swe, 1e-10) << "SWE should not change below freezing (no melt)";
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
        soa.ati[idx] = 30.0;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx] = 0.0; // no melt
        soa.fwfrac[idx] = 0.1;
        soa.fArea[idx] = 0.33;
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
            EXPECT_LT(soa.ati[idx], 30.0) << "ATI should decrease toward cold temp";
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
        soa.wsnow[idx] = 1.0;
        soa.fw[idx] = 0.0;
        soa.coldc[idx] = 0.0;
        soa.ati[idx] = 40.0;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx] = 1e-5;
        soa.fwfrac[idx] = 0.0; // all melt drains
        soa.fArea[idx] = 0.33;
        soa.si[idx] = 1.0; // depth at 100% cover
    }
    // ADC defaults are all 1.0 — asc = 1.0

    solver.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);

    // Melt should have occurred (imelt > 0 for subareas with snow)
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        EXPECT_GT(soa.imelt[idx], 0.0) << "Subarea " << k << " should have melt with default ADC";
    }
}

TEST(SnowADC, ZeroCurveEliminatesMelt) {
    // ADC = all 0.0 → asc = 0.0 → no melt when pack is in depletion phase.
    // In the legacy new-snow ADC scheme (Gap #18), the ADC curve is consulted
    // only when awesi < awe (pack has melted below its pre-snowfall reference).
    // Setting awe = 1.0 puts the pack in the "depletion from full coverage" state.
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Set ADC curves to all zero
    for (int i = 0; i < 10; ++i) {
        soa.adc_imperv[i] = 0.0;
        soa.adc_perv[i] = 0.0;
    }

    // Only set snow on IMPERV and PERV subareas.
    // PLOWABLE (k=0) is never subject to areal depletion → asc=1.0 always (legacy rule).
    for (int k = SNOW_IMPERV; k <= SNOW_PERV; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx] = 0.5; // partial snow (awesi = 0.5/1.0 = 0.5)
        soa.fw[idx] = 0.0;
        soa.coldc[idx] = 0.0;
        soa.ati[idx] = 40.0;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx] = 1e-5;
        soa.fwfrac[idx] = 0.0;
        soa.fArea[idx] = 0.33;
        soa.si[idx] = 1.0; // depth at 100% cover
        // Simulate post-snowfall depletion: pack has melted below its reference.
        // awe=1.0 means awesi (0.5) < awe (1.0) → legacy uses regular ADC curve.
        soa.awe[idx] = 1.0;
    }

    double initial_swe = 0.0;
    for (int k = SNOW_IMPERV; k <= SNOW_PERV; ++k)
        initial_swe += soa.wsnow[static_cast<std::size_t>(k)];

    solver.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);

    double final_swe = 0.0;
    for (int k = SNOW_IMPERV; k <= SNOW_PERV; ++k)
        final_swe += soa.wsnow[static_cast<std::size_t>(k)];

    // With ADC = zeros and awesi < awe, asc = 0 → no melt → SWE unchanged
    EXPECT_NEAR(final_swe, initial_swe, 1e-10)
        << "SWE should not change when ADC is all zero and pack is in depletion phase";
}

TEST(SnowADC, PartialCoverReducesMelt) {
    // ADC with intermediate values → partial asc → reduced melt vs. full ADC.
    // The ADC curve applies when awesi < awe (pack melted below reference level).
    // Setting awe = 1.0 puts both solvers in the "depletion from full coverage" state.
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Linear ADC: adc[i] = i * 0.1 (0.0, 0.1, 0.2, ... 0.9)
    for (int i = 0; i < 10; ++i) {
        soa.adc_imperv[i] = i * 0.1;
        soa.adc_perv[i] = i * 0.1;
    }

    // Set up two solvers: one with linear ADC, one with default (all 1.0)
    SnowSolver solver_full;
    solver_full.init(1);
    auto& soa_full = solver_full.state();

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        // Partial cover solver: awesi=0.5 < awe=1.0 → ADC[5]=0.5 → asc~0.5
        soa.wsnow[idx] = 0.5;
        soa.fw[idx] = 0.0;
        soa.coldc[idx] = 0.0;
        soa.ati[idx] = 40.0;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx] = 1e-5;
        soa.fwfrac[idx] = 0.0;
        soa.fArea[idx] = 0.33;
        soa.si[idx] = 1.0;
        soa.awe[idx] = 1.0; // depletion state: ADC curve is consulted

        // Full cover solver: same state but ADC = all 1.0 (default) → asc=1.0
        soa_full.wsnow[idx] = 0.5;
        soa_full.fw[idx] = 0.0;
        soa_full.coldc[idx] = 0.0;
        soa_full.ati[idx] = 40.0;
        soa_full.tbase[idx] = 32.0;
        soa_full.dhm[idx] = 1e-5;
        soa_full.fwfrac[idx] = 0.0;
        soa_full.fArea[idx] = 0.33;
        soa_full.si[idx] = 1.0;
        soa_full.awe[idx] = 1.0; // depletion state
    }

    solver.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);
    solver_full.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);

    // Partial ADC (asc~0.5) should produce less melt than full ADC (asc=1.0)
    double total_melt_partial = 0.0, total_melt_full = 0.0;
    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        total_melt_partial += soa.imelt[idx];
        total_melt_full += soa_full.imelt[idx];
    }
    EXPECT_GT(total_melt_full, 0.0);
    EXPECT_LT(total_melt_partial, total_melt_full)
        << "Partial ADC (linear) should produce less melt than full ADC (all 1.0) in depletion phase";
}

TEST(SnowADC, FullSnowCoverGivesAscOne) {
    // When wsnow >= si (awesi >= 1.0), asc should be 1.0
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Use a distinctive ADC curve that would reduce asc if awesi < 1
    for (int i = 0; i < 10; ++i) {
        soa.adc_imperv[i] = 0.0;
        soa.adc_perv[i] = 0.0;
    }

    for (int k = 0; k < N_SUBAREAS; ++k) {
        auto idx = static_cast<std::size_t>(k);
        soa.wsnow[idx] = 2.0; // awesi = 2.0/1.0 = 2.0 → >= 1.0 → asc = 1.0
        soa.fw[idx] = 0.0;
        soa.coldc[idx] = 0.0;
        soa.ati[idx] = 40.0;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx] = 1e-5;
        soa.fwfrac[idx] = 0.0;
        soa.fArea[idx] = 0.33;
        soa.si[idx] = 1.0; // 100% cover at 1.0 ft
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
        // Legacy's constant (climate.c:1176), not 2*pi/365. It is a
        // calibration, not a mis-divided year: the period is exactly 364
        // days, so with the day-81 equinox offset the peak lands exactly on
        // day 172, the summer solstice. See the solstice gate in
        // test_transport_snow.cpp, which asserts what it is calibrated TO
        // rather than the formula itself.
        double expected = std::sin(0.0172615 * (day - 81.0));
        EXPECT_NEAR(soa.season, expected, 1e-10) << "Season factor mismatch on day " << day;
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
    soa.fArea[plow_idx] = 0.3;
    soa.fArea[static_cast<std::size_t>(SNOW_IMPERV)] = 0.3;
    soa.fArea[static_cast<std::size_t>(SNOW_PERV)] = 0.4;
    soa.wsnow[plow_idx] = 0.0;
    soa.weplow[0] = 0.5; // plow when depth > 0.5 ft
    soa.sfrac[0] = 0.25; // 25% removed from system
    soa.sfrac[1] = 0.0;
    soa.sfrac[2] = 0.0;
    soa.sfrac[3] = 0.0;
    soa.sfrac[4] = 0.0;

    // Set up dummy context with subcatch area
    SimulationContext ctx;
    ctx.subcatches.area.push_back(10.0); // 10 acres

    // Add snowfall to exceed plow threshold
    double snowfall = 2e-4; // ft/sec
    double dt = 3600.0;
    solver.plowSnow(ctx, dt, snowfall);

    // Snow accumulated = 2e-4 * 3600 = 0.72 ft, > 0.5 threshold
    // removed = sfrac[0] * exc * fArea[plow] * area_ft2
    //
    // F9 — this line read `10.0 * 43560.0`. It is a US-only literal and it
    // is not even the US number the engine uses: `subcatches.area` is in
    // PROJECT land-area units and the conversion is `1 / UCF(LANDAREA)`,
    // which is what legacy does too (`Subcatch.area` is parsed to ft² that
    // way, `output.c:250` converts back the same way). `Ucf[LANDAREA][US]`
    // is the rounded 2.2956e-5, so 1/UCF is 43561.596, not 43560 — the gate
    // was 3.7e-5 off from the engine AND from legacy, and on SI it was off
    // by the whole 2.471x hectare/acre ratio.
    double exc = snowfall * dt;
    double expected_removed = 0.25 * exc * 0.3 * (10.0 / ucf::UCF(ucf::LANDAREA, ctx.options));
    EXPECT_NEAR(soa.removed, expected_removed, 1e-3)
        << "Removed volume should match sfrac[0] * excess * plowArea * subcatchArea";
}

// F9's SI leg. The handoff predicted the defect was unobservable because
// "no deck in any corpus is SI and snowy" — true of DECKS, and this gate
// needs no deck. `plowSnow` takes the context directly, so the unit system
// is a field, and the whole 2.471x is one assignment away.
TEST(SnowRemoved, PlowingConvertsHectaresNotAcresUnderSI) {
    auto removed_for = [](FlowUnits units) {
        SnowSolver solver;
        solver.init(1);
        auto& soa = solver.state();
        auto plow_idx = static_cast<std::size_t>(SNOW_PLOWABLE);
        soa.fArea[plow_idx] = 0.3;
        soa.fArea[static_cast<std::size_t>(SNOW_IMPERV)] = 0.3;
        soa.fArea[static_cast<std::size_t>(SNOW_PERV)] = 0.4;
        soa.wsnow[plow_idx] = 0.0;
        soa.weplow[0] = 0.5;
        soa.sfrac[0] = 0.25;
        for (int i = 1; i < 5; ++i)
            soa.sfrac[static_cast<std::size_t>(i)] = 0.0;
        SimulationContext ctx;
        ctx.options.flow_units = units;
        ctx.subcatches.area.push_back(10.0); // 10 ACRES in US, 10 HECTARES in SI
        solver.plowSnow(ctx, 3600.0, 2e-4);
        return solver.state().removed;
    };

    const double us = removed_for(FlowUnits::CFS);
    const double si = removed_for(FlowUnits::CMS);
    ASSERT_GT(us, 0.0) << "nothing was ploughed, so neither leg means anything";

    // 10 hectares is 2.471 times 10 acres, so the SI figure must be that
    // much larger. Under the defect both legs multiplied by a hardcoded
    // 43560 and this ratio was exactly 1.
    const double expected = ucf::UCF(ucf::LANDAREA,
                                     [] {
                                         SimulationContext c;
                                         c.options.flow_units = FlowUnits::CFS;
                                         return c.options;
                                     }()) /
                            ucf::UCF(ucf::LANDAREA, [] {
                                SimulationContext c;
                                c.options.flow_units = FlowUnits::CMS;
                                return c.options;
                            }());
    EXPECT_NEAR(si / us, expected, 1e-9)
        << "SI removed " << si << " ft3 against US " << us
        << " — a ratio of 1 means the area conversion is a hardcoded acre "
           "factor and every SI snow deck under-reports its ploughed volume";
    EXPECT_NEAR(expected, 2.4710, 1.0e-3) << "sanity: ha/ac is 2.471";
}

TEST(SnowRemoved, NoPlowingMeansNoRemoval) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    auto plow_idx = static_cast<std::size_t>(SNOW_PLOWABLE);
    soa.fArea[plow_idx] = 0.3;
    soa.fArea[static_cast<std::size_t>(SNOW_PERV)] = 0.7;
    soa.wsnow[plow_idx] = 0.0;
    soa.weplow[0] = 2.0; // high threshold
    soa.sfrac[0] = 0.5;

    SimulationContext ctx;
    ctx.subcatches.area.push_back(10.0);

    // Small snowfall: 0.01 ft/sec * 1s = 0.01 ft < 2.0 threshold
    solver.plowSnow(ctx, 1.0, 0.01);
    EXPECT_DOUBLE_EQ(soa.removed, 0.0) << "No removal when snow < plow threshold";
}

TEST(SnowRemoved, MultipleTimestepsCumulate) {
    SnowSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    auto plow_idx = static_cast<std::size_t>(SNOW_PLOWABLE);
    soa.fArea[plow_idx] = 1.0;
    soa.fArea[static_cast<std::size_t>(SNOW_IMPERV)] = 0.0;
    soa.fArea[static_cast<std::size_t>(SNOW_PERV)] = 0.0;
    soa.weplow[0] = 0.1;
    soa.sfrac[0] = 1.0; // all removed

    SimulationContext ctx;
    ctx.subcatches.area.push_back(1.0); // 1 acre

    // First timestep
    solver.plowSnow(ctx, 1.0, 0.5);
    double first = soa.removed;
    EXPECT_GT(first, 0.0);

    // Second timestep — accumulates
    solver.plowSnow(ctx, 1.0, 0.5);
    EXPECT_GT(soa.removed, first) << "Removed should accumulate across timesteps";
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
        soa.wsnow[idx] = 1.0;
        soa.fw[idx] = 0.0;
        soa.coldc[idx] = 0.0;
        soa.ati[idx] = 40.0;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx] = 1e-5;
        soa.fwfrac[idx] = 0.0;
        soa.fArea[idx] = 0.33;
        soa.si[idx] = 1.0;
    }

    // Execute with rain-on-snow (rainfall above threshold) and valid gamma/ea
    double temp = 50.0;
    double wind = 10.0;
    double rainfall = 5e-5; // well above 0.02 in/hr threshold in ft/sec
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
        s0.fw[idx] = s1.fw[idx] = 0.0;
        s0.coldc[idx] = s1.coldc[idx] = 0.0;
        s0.ati[idx] = s1.ati[idx] = 40.0;
        s0.tbase[idx] = s1.tbase[idx] = 32.0;
        s0.dhm[idx] = s1.dhm[idx] = 1e-5;
        s0.fwfrac[idx] = s1.fwfrac[idx] = 0.0;
        s0.fArea[idx] = s1.fArea[idx] = 0.33;
        s0.si[idx] = s1.si[idx] = 1.0;
    }

    double rain = 5e-5;
    solver_zero.execute(dummy_ctx, 3600.0, 50.0, 10.0, rain, 0.0, 0.0);
    solver_pos.execute(dummy_ctx, 3600.0, 50.0, 10.0, rain, 0.001, 0.5);

    double melt_zero = 0.0, melt_pos = 0.0;
    for (int k = 0; k < N_SUBAREAS; ++k) {
        melt_zero += solver_zero.state().imelt[static_cast<std::size_t>(k)];
        melt_pos += solver_pos.state().imelt[static_cast<std::size_t>(k)];
    }
    // Positive gamma should contribute additional melt from turbulent exchange
    EXPECT_GE(melt_pos, melt_zero) << "Positive gamma should produce >= melt than zero gamma";
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
        soa.wsnow[idx] = 1.0;
        soa.fw[idx] = 0.0;
        soa.coldc[idx] = 0.0;
        soa.ati[idx] = 40.0;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx] = 1e-5;
        soa.fwfrac[idx] = 0.5; // high free water capacity
        soa.fArea[idx] = 0.33;
        soa.si[idx] = 1.0;
    }

    // Small melt that fits within free water capacity
    solver.execute(dummy_ctx, 60.0, 40.0, 0.0, 0.0); // short timestep, small excess

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
        soa.wsnow[idx] = 1.0;
        soa.fw[idx] = 0.0;
        soa.coldc[idx] = 0.0;
        soa.ati[idx] = 50.0;
        soa.tbase[idx] = 32.0;
        soa.dhm[idx] = 5e-4;    // high melt rate
        soa.fwfrac[idx] = 0.01; // very small free water capacity
        soa.fArea[idx] = 0.33;
        soa.si[idx] = 1.0;
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
        soa.wsnow[idx] = 1.0;
        soa.fw[idx] = 0.0;
        soa.coldc[idx] = 0.0;
        soa.ati[idx] = 20.0; // cold
        soa.tbase[idx] = 32.0;
        soa.dhm[idx] = 1e-5;
        soa.fwfrac[idx] = 0.1;
        soa.fArea[idx] = 0.33;
        soa.si[idx] = 1.0;
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
        sc.fw[idx] = sw.fw[idx] = 0.0;
        sc.coldc[idx] = 0.5; // has cold content
        sw.coldc[idx] = 0.0; // no cold content
        sc.ati[idx] = sw.ati[idx] = 40.0;
        sc.tbase[idx] = sw.tbase[idx] = 32.0;
        sc.dhm[idx] = sw.dhm[idx] = 1e-5;
        sc.fwfrac[idx] = sw.fwfrac[idx] = 0.0;
        sc.fArea[idx] = sw.fArea[idx] = 0.33;
        sc.si[idx] = sw.si[idx] = 1.0;
    }

    solver_cold.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);
    solver_warm.execute(dummy_ctx, 3600.0, 50.0, 0.0, 0.0);

    double melt_cold = 0.0, melt_warm = 0.0;
    for (int k = 0; k < N_SUBAREAS; ++k) {
        melt_cold += solver_cold.state().imelt[static_cast<std::size_t>(k)];
        melt_warm += solver_warm.state().imelt[static_cast<std::size_t>(k)];
    }
    EXPECT_LT(melt_cold, melt_warm) << "Cold content should reduce effective melt output";
}

// ============================================================================
// Engine integration — the seasonal melt coefficients
//
// Every gate above sets `dhm` by calling setMeltCoeffs() itself, which is
// exactly how the engine went without ever calling it: `dhm` stayed at its
// `assign(0.0)` value, `imelt = dhm * (temp - tbase)` was identically zero,
// and degree-day melt never fired on any deck. Legacy sets them once a day
// from setTemp (climate.c:1176-1180). This gate runs a real deck.
// ============================================================================

TEST(SnowEngineIntegration, ADegreeDayPackMeltsWhenSteppedThroughTheEngine) {
    {
        std::ofstream f("_snowdd.inp");
        f << "[TITLE]\ndegree-day melt through the engine\n\n[OPTIONS]\n"
             "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nINFILTRATION HORTON\n"
             "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
             "END_DATE 01/01/2026\nEND_TIME 01:00:00\n"
             "WET_STEP 00:01:00\nDRY_STEP 00:01:00\nROUTING_STEP 10\n"
             "REPORT_STEP 00:05:00\n\n"
             "[TEMPERATURE]\nTIMESERIES air_ts\n"
             "SNOWMELT 0.5 0.5 0.6 40.0 0.0 0.0\n\n"
             "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
             "[TIMESERIES]\n"
             "rain_ts 01/01/2026 00:00 0.0\nrain_ts 01/01/2026 01:05 0.0\n\n"
             "air_ts 01/01/2026 00:00 50.0\nair_ts 01/01/2026 01:05 50.0\n\n"
             // A RIPE pack: free water already at capacity, so melt leaves
             // the pack in the first step instead of filling the free-water
             // store for the first 98 minutes.
             "[SNOWPACKS]\n"
             "SP1 PLOWABLE   0.02 0.06 32.0 0.10 6 0.6 0.0\n"
             "SP1 IMPERVIOUS 0.02 0.06 32.0 0.10 6 0.6 0.0\n"
             "SP1 PERVIOUS   0.01 0.03 32.0 0.10 6 0.6 0.0\n"
             "SP1 REMOVAL    0.0 0.0 0.0 0.0 0.0 0.0\n\n"
             "[SUBCATCHMENTS]\nS1 RG1 J1 5 50 500 0.5 0 SP1\n\n"
             "[SUBAREAS]\nS1 0.01 0.1 0.02 0.02 25 OUTLET\n\n"
             "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
             "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
             "[OUTFALLS]\nOUT 9.0 FREE  NO\n\n"
             "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
             "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
             "[REPORT]\nINPUT NO\n";
    }
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_snowdd.inp", "_snowdd.rpt", "_snowdd.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK)
            break;
    } while (elapsed > 0.0 && ++guard < 40000);
    swmm_engine_end(e);

    auto& eng = *static_cast<SWMMEngine*>(e);
    const auto& ctx = eng.context();
    const auto& sn = eng.snowSolver().state();
    ASSERT_GE(ctx.n_subcatches(), 1);
    ASSERT_GE(ctx.subcatches.snowpack[0], 0) << "the deck built no snowpack";

    // The defect itself: nothing in the engine ever set these.
    for (int k = SNOW_PLOWABLE; k <= SNOW_PERV; ++k)
        EXPECT_GT(sn.dhm[static_cast<std::size_t>(k)], 0.0)
            << "subarea " << k
            << ": the seasonal melt coefficient is zero "
               "after a full run, so setMeltCoeffs() was never called and "
               "imelt = dhm * (temp - tbase) can only ever be zero";

    // And its consequence, on a deck 18 F above tbase for an hour.
    EXPECT_GT(ctx.subcatches.snow_net_imperv[0], 0.0) << "the pack published no net precipitation";
    EXPECT_LT(sn.wsnow[SNOW_IMPERV], 0.5)
        << "the impervious pack still holds its full 0.5 ft of water "
           "equivalent, so nothing melted";
    EXPECT_GT(ctx.subcatches.stat_runoff_vol[0], 0.0) << "no runoff from an hour of melt";
    swmm_engine_destroy(e);
}

namespace {
namespace ueb = openswmm::surface::ueb;
/**
 * @brief Construct one synthetic hourly UEB forcing sample.
 *
 * Shortwave and cosine use hour modulo 24, while temperature uses the full hour angle. Longwave is
 * 250 W/m2, precipitation 0.0001 m per sample before hour six; no date/model advancement or physical
 * validation.
 *
 * @param[in] hour Nonnegative intended hour index; first six hours receive precipitation.
 * @return Owned forcing with 3600 s duration, sinusoidal temperature C, integrated precipitation m, wind 2
 *     m/s, humidity 0.75 and short/longwave W/m2.
 */
ueb::Forcing uebWeather(int hour) {
    constexpr double pi = 3.14159265358979323846;
    ueb::Forcing f;
    f.seconds = 3600;
    f.temperature = -4 + 4 * std::sin(hour * pi / 12);
    f.precipitation = hour < 6 ? .0001 : 0;
    f.wind = 2;
    f.humidity = .75;
    f.shortwave = std::max(0., 500 * std::sin((hour % 24 - 6) * pi / 12));
    f.longwave = 250;
    f.cosine = std::max(0., std::sin((hour % 24 - 6) * pi / 12));
    return f;
}
}
TEST(UebSnow, MatchesIndependentDoublePrecisionFortranReference) {
    // Official snow.zip (1997-04-24), SHA256 6254b2015a88f23583b40ec1332a5fee266d5514ec0cb9be66f51526f6b3b8e1;
    // UEB22.zip SHA256 500c21d72696ad15d1ae289ed3aad3613afe210df3790397b28abb74c54f404b.
    // Constants below were produced by gfortran from the official equations,
    // using the historical ageing coefficient for this reproduction only.
    const double energy[]{-4859.966049526292, -1508.9129555877619};
    const double water[]{.19908470774519227, .19917508158011984};
    const double age[]{.31286398771368024, .29020828835645757};
    for (int mode = 0; mode < 2; ++mode) {
        ueb::Parameters p;
        p.stability = 0;
        p.age_dust = .3;
        p.temperature = mode ? ueb::TemperatureMethod::ModifiedForceRestore
                             : ueb::TemperatureMethod::Equilibrium;
        if (!mode) {
            p.roughness = .005;
            p.snow_density = 450;
            p.liquid_retention = .05;
            p.drainage_conductivity = 20;
            p.active_soil_depth = .4;
            p.new_snow_depth = .01;
        }
        ueb::Site site;
        site.pressure = 85000;
        ueb::State state;
        state.energy = -1000;
        state.water = .2;
        if (mode)
            state.history.push_back({86400, -3, -3});
        for (int h = 0; h < 72; ++h) {
            ueb::Fluxes flux;
            ASSERT_TRUE(ueb::advance(state, p, site, uebWeather(h), flux).empty());
        }
        EXPECT_NEAR(state.energy, energy[mode], 1e-8);
        EXPECT_NEAR(state.water, water[mode], 1e-12);
        EXPECT_NEAR(state.age, age[mode], 1e-12);
    }
}
TEST(UebSnow, SeasonalWaterAndEnergyClosureAcrossBothTemperatureMethods) {
    for (auto method :
         {ueb::TemperatureMethod::Equilibrium, ueb::TemperatureMethod::ModifiedForceRestore}) {
        ueb::Parameters p;
        p.temperature = method;
        ueb::Site site;
        ueb::State state;
        state.water = .2;
        state.energy = -1000;
        double expected_water = state.water, expected_energy = state.energy;
        for (int h = 0; h < 24 * 120; ++h) {
            auto f = uebWeather(h);
            f.temperature = -6. + h / 300. + 5 * std::sin(h * 3.141592653589793 / 12);
            f.precipitation = h % 120 < 12 ? .0005 : 0;
            f.shortwave *= .7;
            ueb::Fluxes flux;
            const auto error = ueb::advance(state, p, site, f, flux);
            ASSERT_TRUE(error.empty()) << error << " hour " << h;
            expected_water += flux.rain + flux.snow - flux.liquid - flux.sublimation;
            expected_energy += flux.net_energy;
            EXPECT_NEAR(expected_water, state.water, 1e-10);
            EXPECT_NEAR(expected_energy, state.energy, 1e-8);
            EXPECT_GE(state.water, 0);
            EXPECT_GE(flux.liquid, 0);
        }
        double duration = 0;
        for (const auto& sample : state.history)
            duration += sample.seconds;
        EXPECT_DOUBLE_EQ(duration, 86400);
    }
}
TEST(UebSnow, RefusesInvalidForcingAndParametersWithoutChangingStateOrResult) {
    ueb::Parameters p;
    ueb::State state;
    state.water = .3;
    state.energy = -1000;
    state.history = {{1234, -2, -1}};
    ueb::Fluxes flux;
    flux.liquid = 42;
    auto f = uebWeather(0);
    f.humidity = 1.5;
    EXPECT_FALSE(ueb::advance(state, p, {}, f, flux).empty());
    EXPECT_DOUBLE_EQ(state.water, .3);
    EXPECT_DOUBLE_EQ(state.energy, -1000);
    ASSERT_EQ(state.history.size(), 1u);
    EXPECT_DOUBLE_EQ(state.history[0].seconds, 1234);
    EXPECT_DOUBLE_EQ(flux.liquid, 42);
    f = uebWeather(0);
    p.roughness = p.measurement_height;
    EXPECT_FALSE(ueb::advance(state, p, {}, f, flux).empty());
    EXPECT_DOUBLE_EQ(flux.liquid, 42);
}
TEST(UebSnow, ThermalHistoryUsesActualDurationsAndHasNoCrossThreadState) {
    auto run = [](int member) {
        ueb::Parameters p;
        ueb::State s;
        s.water = .15;
        s.energy = -1000 - member * 100;
        s.history = {{86400, -2, -2}};
        for (int j = 0; j < 60; ++j) {
            auto f = uebWeather(j);
            f.seconds = j % 2 ? 1800 : 2700;
            f.temperature -= member;
            ueb::Fluxes flux;
            if (auto e = ueb::advance(s, p, {}, f, flux); !e.empty())
                throw std::runtime_error(e);
        }
        return s;
    };
    std::vector<std::future<ueb::State>> tasks;
    for (int j = 0; j < 8; ++j)
        tasks.push_back(std::async(std::launch::async, run, j));
    for (int j = 0; j < 8; ++j) {
        const auto concurrent = tasks[j].get(), serial = run(j);
        EXPECT_DOUBLE_EQ(concurrent.water, serial.water);
        EXPECT_DOUBLE_EQ(concurrent.energy, serial.energy);
        EXPECT_DOUBLE_EQ(concurrent.refreezing_depth, serial.refreezing_depth);
        double duration = 0;
        for (const auto& sample : concurrent.history)
            duration += sample.seconds;
        EXPECT_DOUBLE_EQ(duration, 86400);
    }
}

TEST(UebConfiguration, PublicBatchEditsAreAtomicAndPreservePackOverrides) {
    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);
    auto& ctx = static_cast<openswmm::SWMMEngine*>(handle)->context();
    ctx.state = openswmm::EngineState::OPENED;
    ctx.snowpacks.names = {"test"};
    const SWMM_UebSnowpack pack{SWMM_SNOW_UEB, .4, 1.3, 2, .2, -300};
    ASSERT_EQ(swmm_snowpack_set_ueb(handle, 0, &pack), SWMM_OK);
    char error[512];
    ASSERT_EQ(swmm_snow_apply_configuration(
                  handle,
                  "SNOWMELT_MODEL UEB\nSNOW_STEP 00:30\nSITE 45 -120 -8 1500\nSHORTWAVE COMPUTED\n",
                  "TB 5\nTR 8\nZ0 .02\n", error, sizeof(error)),
              SWMM_OK)
        << error;
    EXPECT_DOUBLE_EQ(ctx.snowpacks.config.parameters.snow_temperature, 5);
    EXPECT_DOUBLE_EQ(ctx.snowpacks.config.parameters.rain_temperature, 8);
    EXPECT_DOUBLE_EQ(ctx.snowpacks.config.step, 1800);
    EXPECT_DOUBLE_EQ(ctx.snowpacks.config.packs.at(0).site.forest, .4);
    EXPECT_EQ(
        swmm_snow_apply_configuration(handle, "SNOW_STEP 01:00\n", "Z0 0\n", error, sizeof(error)),
        SWMM_ERR_BADPARAM);
    EXPECT_NE(error[0], '\0');
    EXPECT_DOUBLE_EQ(ctx.snowpacks.config.parameters.roughness, .02);
    EXPECT_DOUBLE_EQ(ctx.snowpacks.config.step, 1800);
    EXPECT_EQ(swmm_snow_apply_configuration(handle, "SHORTWAVE FILE\n", "", error, sizeof(error)),
              SWMM_ERR_BADPARAM);
    EXPECT_DOUBLE_EQ(ctx.snowpacks.config.step, 1800);
    ctx.state = openswmm::EngineState::INITIALIZED;
    EXPECT_EQ(swmm_snow_set_step(handle, 900), SWMM_ERR_LIFECYCLE);
    swmm_engine_destroy(handle);
}

TEST(UebLumped, FullCoverRetainsPointKernelStatesAndFluxes) {
    std::array<double, 10> curve;
    curve.fill(1);
    for (auto method :
         {ueb::TemperatureMethod::Equilibrium, ueb::TemperatureMethod::ModifiedForceRestore}) {
        ueb::Parameters p;
        p.temperature = method;
        ueb::LumpedState lumped;
        ASSERT_TRUE(ueb::initializeLumped(lumped, .2, -1000, .2, curve).empty());
        auto point = lumped.point;
        for (int h = 0; h < 72; ++h) {
            auto f = uebWeather(h);
            ueb::Fluxes flux;
            ueb::LumpedFluxes basin;
            ASSERT_TRUE(ueb::advance(point, p, {}, f, flux).empty());
            ASSERT_TRUE(ueb::advanceLumped(lumped, p, {}, f, curve, basin).empty());
            EXPECT_EQ(lumped.point.water, point.water);
            EXPECT_EQ(lumped.point.energy, point.energy);
            EXPECT_EQ(lumped.point.age, point.age);
            EXPECT_EQ(lumped.point.refreezing_depth, point.refreezing_depth);
            EXPECT_EQ(basin.liquid, flux.liquid);
            EXPECT_EQ(basin.sublimation, flux.sublimation);
            EXPECT_EQ(basin.throughfall, 0);
            EXPECT_NEAR(basin.geometry_energy, 0, 1e-10);
        }
    }
}
TEST(UebLumped, SnowfallAndRefreezingRainUseDifferentAccumulationAreas) {
    std::array<double, 10> curve{0, .1, .2, .3, .4, .5, .6, .7, .8, .9};
    for (bool snowfall : {false, true}) {
        ueb::LumpedState s;
        ueb::Parameters p;
        ASSERT_TRUE(ueb::initializeLumped(s, .1, -12000, .2, curve).empty());
        ASSERT_DOUBLE_EQ(s.cover, .5);
        const auto old = s;
        auto f = uebWeather(0);
        f.temperature = -5;
        f.snow_fraction = snowfall ? 1 : 0;
        f.precipitation = .005;
        f.shortwave = 0;
        f.longwave = 200;
        ueb::LumpedFluxes out;
        ASSERT_TRUE(ueb::advanceLumped(s, p, {}, f, curve, out).empty());
        ASSERT_GT(s.point.water * s.cover, old.water);
        EXPECT_DOUBLE_EQ(out.active_fraction, snowfall ? 1 : .5);
        EXPECT_NEAR(out.throughfall, snowfall ? 0 : .0025, 1e-12);
        EXPECT_NEAR(old.water + out.rain + out.snow - out.liquid - out.throughfall -
                        out.sublimation,
                    s.water, 1e-12);
        EXPECT_NEAR(s.cover, s.water / s.maximum, 1e-12);
    }
}
TEST(UebLumped, DepletionSeasonConservesWaterAndAccountsForEnergyRemapping) {
    std::array<double, 10> curve{0, .2, .35, .48, .59, .69, .78, .85, .91, .96};
    ueb::LumpedState s;
    ueb::Parameters p;
    ueb::Site site;
    site.drift = 1.3;
    ASSERT_TRUE(ueb::initializeLumped(s, .2, -1000, .3, curve).empty());
    double water = s.water, energy = s.cover * s.point.energy, geometry = 0;
    for (int h = 0; h < 24 * 120; ++h) {
        auto f = uebWeather(h);
        f.temperature = -6. + h / 300. + 5 * std::sin(h * 3.141592653589793 / 12);
        f.precipitation = h % 120 < 12 ? .0005 : 0;
        f.shortwave *= .7;
        ueb::LumpedFluxes out;
        const auto error = ueb::advanceLumped(s, p, site, f, curve, out);
        ASSERT_TRUE(error.empty()) << error << " hour " << h;
        water += out.rain + out.snow - out.liquid - out.throughfall - out.sublimation;
        energy += out.net_energy + out.geometry_energy;
        geometry += std::abs(out.geometry_energy);
        EXPECT_NEAR(water, s.water, 1e-10);
        EXPECT_NEAR(energy, (s.cover > 0 ? s.cover : 1) * s.point.energy, 1e-7);
    }
    EXPECT_GT(geometry, 0);
}
TEST(UebLumped, InvalidCurveAndInconsistentInventoryAreAtomic) {
    std::array<double, 10> curve;
    curve.fill(1);
    ueb::LumpedState s;
    ASSERT_TRUE(ueb::initializeLumped(s, .1, -1000, .1, curve).empty());
    ueb::LumpedFluxes result;
    result.liquid = 42;
    curve[4] = .5;
    EXPECT_FALSE(ueb::advanceLumped(s, {}, {}, uebWeather(0), curve, result).empty());
    EXPECT_DOUBLE_EQ(s.water, .1);
    EXPECT_DOUBLE_EQ(s.point.energy, -1000);
    EXPECT_DOUBLE_EQ(result.liquid, 42);
    curve.fill(1);
    s.cover = .5;
    EXPECT_FALSE(ueb::advanceLumped(s, {}, {}, uebWeather(0), curve, result).empty());
    EXPECT_DOUBLE_EQ(result.liquid, 42);
}

TEST(UebHeld, SubstepsPublishOnlyElapsedInventoryAndFinishAtThePointSolution) {
    std::array<double, 10> curve;
    curve.fill(1);
    ueb::HeldState held;
    ASSERT_TRUE(ueb::initializeLumped(held.accepted, .2, -1000, .2, curve).empty());
    const auto initial = held.accepted;
    auto f = uebWeather(1);
    f.precipitation = .005;
    ASSERT_TRUE(ueb::prepareHeld(held, {}, {}, f, curve).empty());
    EXPECT_DOUBLE_EQ(held.accepted.water, initial.water);
    const auto expected = held.end;
    double liquid = 0, sublimation = 0;
    for (int i = 0; i < 12; ++i) {
        ueb::LumpedFluxes flux;
        ASSERT_TRUE(ueb::consumeHeld(held, 300, curve, flux).empty());
        liquid += flux.liquid;
        sublimation += flux.sublimation;
        EXPECT_NEAR(initial.water + (i + 1) / 12. * (held.flux.rain + held.flux.snow) - liquid -
                        sublimation,
                    held.accepted.water, 1e-12);
        double time = 0;
        for (const auto& h : held.accepted.point.history)
            time += h.seconds;
        EXPECT_DOUBLE_EQ(time, (i + 1) * 300);
    }
    EXPECT_FALSE(held.pending());
    EXPECT_EQ(held.accepted.water, expected.water);
    EXPECT_EQ(held.accepted.point.energy, expected.point.energy);
    EXPECT_NEAR(liquid, held.flux.liquid, 1e-15);
}
TEST(UebHeld, EarlyForcingChangeKeepsAcceptedStateAndRejectsOverconsumption) {
    std::array<double, 10> curve;
    curve.fill(1);
    ueb::HeldState held;
    ASSERT_TRUE(ueb::initializeLumped(held.accepted, .2, -1000, .2, curve).empty());
    ASSERT_TRUE(ueb::prepareHeld(held, {}, {}, uebWeather(1), curve).empty());
    ueb::LumpedFluxes flux;
    ASSERT_TRUE(ueb::consumeHeld(held, 300, curve, flux).empty());
    const auto accepted = held.accepted;
    EXPECT_FALSE(ueb::consumeHeld(held, 3600, curve, flux).empty());
    EXPECT_EQ(held.elapsed, 300);
    EXPECT_EQ(held.accepted.water, accepted.water);
    ueb::closeHeld(held);
    auto f = uebWeather(2);
    f.precipitation = .003;
    f.snow_fraction = 1;
    ASSERT_TRUE(ueb::prepareHeld(held, {}, {}, f, curve).empty());
    EXPECT_EQ(held.begin.water, accepted.water);
    EXPECT_EQ(held.begin.point.energy, accepted.point.energy);
    ASSERT_EQ(held.begin.point.history.size(), accepted.point.history.size());
    EXPECT_EQ(held.begin.point.history.back().seconds, 300);
}

/**
 * @brief Write the two-hour native snowmelt integration fixture.
 *
 * Uses CFS/DYNWAVE/Horton, 45 F air, 400/300 W/m2 short/longwave, no rainfall and an initial
 * snowpack on a 5 acre catchment. Writes fixed authored US snow depths/site data. Stream status is
 * unchecked, tokens unescaped and no model execution occurs.
 *
 * @param[in] path Non-null destination filename; parent must exist and file is truncated.
 * @param[in] model Raw SNOWMELT_MODEL keyword, default UEB.
 */
static void writeNativeSnowDeck(const char* path, const std::string& model = "UEB") {
    {
        std::ofstream f(path);
        f << "[OPTIONS]\nIGNORE_GROUNDWATER YES\nIGNORE_QUALITY YES\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nINFILTRATION HORTON\n"
             "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 02:00:00\n"
             "WET_STEP 00:05:00\nDRY_STEP 00:05:00\nROUTING_STEP 30\nREPORT_STEP 00:05:00\n"
             "[TEMPERATURE]\nTIMESERIES air\nSNOWMELT_MODEL "
          << model
          << "\nSNOW_STEP 01:00\n"
             "SITE 45 -120 -8 1000\nSHORTWAVE TIMESERIES sw\nLONGWAVE TIMESERIES lw\n"
             "[RAINGAGES]\nRG INTENSITY 0:05 1 TIMESERIES rain\n"
             "[TIMESERIES]\nrain 01/01/2026 00:00 0\nrain 01/01/2026 03:00 0\n"
             "air 01/01/2026 00:00 45\nair 01/01/2026 03:00 45\n"
             "sw 01/01/2026 00:00 400\nsw 01/01/2026 03:00 400\n"
             "lw 01/01/2026 00:00 300\nlw 01/01/2026 03:00 300\n"
             "[SNOWPACKS]\nSP PLOWABLE .02 .06 32 .1 6 .6 0\n"
             "SP IMPERVIOUS .02 .06 32 .1 6 .6 0\nSP PERVIOUS .02 .06 32 .1 6 .6 0\n"
             "SP REMOVAL 0 0 0 0 0 0\n"
             "[SUBCATCHMENTS]\nS RG J 5 50 500 .5 0 SP\n"
             "[SUBAREAS]\nS .01 .1 .02 .02 25 OUTLET\n[INFILTRATION]\nS 3 .5 4 7 0\n"
             "[JUNCTIONS]\nJ 10 10 0 0 0\n[OUTFALLS]\nO 9 FREE NO\n"
             "[CONDUITS]\nC J O 400 .013 0 0 0\n[XSECTIONS]\nC CIRCULAR 3 0 0 0\n";
    }
}

TEST(SnowEngineIntegration, UebNativeRunMatchesHourlyKernelAndClosesSnowWaterBalance) {
    const char* path = "_snow_ueb_native.inp";
    writeNativeSnowDeck(path);
    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(
        swmm_engine_open(handle, path, "_snow_ueb_native.rpt", "_snow_ueb_native.out", nullptr),
        SWMM_OK);
    auto& engine = *static_cast<SWMMEngine*>(handle);
    auto& ctx = engine.context();
    ASSERT_EQ(swmm_engine_initialize(handle), SWMM_OK);
    ASSERT_TRUE(ctx.snowpacks.runtime.active);
    const double initial = ctx.snowpacks.runtime.columns[1].accepted.water;
    ASSERT_GT(initial, 0);
    ASSERT_EQ(swmm_engine_start(handle, 0), SWMM_OK);
    double elapsed = 0;
    int count = 0;
    do {
        ASSERT_EQ(swmm_engine_step(handle, &elapsed), SWMM_OK) << swmm_get_last_error_msg(handle);
        ASSERT_LT(++count, 1000);
        if (count == 1) {
            const auto bytes = surface::saveSnowCheckpoint(ctx, engine.snowSolver().state());
            EXPECT_FALSE(surface::restoreSnowCheckpoint(ctx, engine.snowSolver().state(),
                                                        bytes.substr(0, bytes.size() - 1))
                             .empty());
            EXPECT_EQ(surface::saveSnowCheckpoint(ctx, engine.snowSolver().state()), bytes);
            ASSERT_TRUE(
                surface::restoreSnowCheckpoint(ctx, engine.snowSolver().state(), bytes).empty());
            EXPECT_EQ(surface::saveSnowCheckpoint(ctx, engine.snowSolver().state()), bytes);
            ASSERT_EQ(swmm_hotstart_save(handle, "_snow_ueb_native.hsf"), SWMM_OK)
                << swmm_hotstart_last_io_error();
            SWMM_HotStart hs = nullptr;
            ASSERT_EQ(swmm_hotstart_open("_snow_ueb_native.hsf", &hs), SWMM_OK);
            SWMM_Engine second = swmm_engine_create();
            ASSERT_NE(second, nullptr);
            ASSERT_EQ(
                swmm_engine_open(second, path, "_snow_ueb_seed.rpt", "_snow_ueb_seed.out", nullptr),
                SWMM_OK);
            ASSERT_EQ(swmm_engine_initialize(second), SWMM_OK);
            ASSERT_EQ(swmm_hotstart_apply(second, hs), SWMM_OK) << swmm_hotstart_last_io_error();
            const auto& seeded = static_cast<SWMMEngine*>(second)->context().snowpacks.runtime;
            const auto& accepted = ctx.snowpacks.runtime.columns[1].accepted;
            EXPECT_EQ(seeded.columns[1].accepted.water, accepted.water);
            EXPECT_EQ(seeded.columns[1].accepted.point.energy, accepted.point.energy);
            EXPECT_EQ(seeded.columns[1].accepted.point.history.back().seconds,
                      accepted.point.history.back().seconds);
            EXPECT_FALSE(seeded.columns[1].pending());
            swmm_hotstart_close(hs);
            swmm_engine_destroy(second);
        }
    } while (elapsed > 0);
    ASSERT_EQ(swmm_engine_end(handle), SWMM_OK);
    const auto& state = ctx.snowpacks.runtime;
    const double a = ctx.subcatches.area[0] / ucf::UCF(ucf::LANDAREA, ctx.options) * .3048 * .3048;
    EXPECT_NEAR(initial * a + state.precipitation - state.liquid - state.throughfall -
                    state.sublimation,
                state.columns[1].accepted.water * a, 1e-9);
    EXPECT_GT(state.liquid, 0);
    EXPECT_GT(ctx.subcatches.stat_runoff_vol[0], 0);
    EXPECT_LT(state.columns[1].accepted.water, initial);
    EXPECT_FALSE(state.columns[1].pending());
    SWMM_SnowState reported{};
    ASSERT_EQ(swmm_subcatch_get_snow_process_state(handle, 0, 1, &reported), SWMM_OK);
    EXPECT_EQ(reported.model, SWMM_SNOW_UEB);
    EXPECT_EQ(reported.swe_m, state.columns[1].accepted.water);
    SWMM_SnowLedger ledger{};
    ASSERT_EQ(swmm_snow_get_ueb_ledger(handle, &ledger), SWMM_OK);
    EXPECT_NEAR(ledger.residual_m3, 0, 1e-9);
    EXPECT_EQ(ledger.liquid_m3, state.liquid);
    EXPECT_NEAR(ledger.energy_residual_kj, 0, 1e-6);
    ueb::State point;
    point.water = initial;
    point.energy = .6 * .0254 * 333500;
    ueb::Site site;
    site.pressure = 101325 * std::pow(1 - 2.25577e-5 * 1000, 5.25588);
    ueb::Forcing force;
    force.seconds = 3600;
    force.temperature = (45. - 32) / 1.8;
    force.humidity = .5;
    force.shortwave = 400;
    force.longwave = 300;
    force.snow_fraction = 0;
    for (int h = 0; h < 2; ++h) {
        ueb::Fluxes flux;
        ASSERT_TRUE(
            ueb::advance(point, ctx.snowpacks.config.parameters, site, force, flux).empty());
    }
    EXPECT_NEAR(state.columns[1].accepted.water, point.water, 1e-12);
    EXPECT_NEAR(state.columns[1].accepted.point.energy, point.energy, 1e-7);
    swmm_engine_destroy(handle);
}

TEST(SnowEngineIntegration, UebRemovalConservesUnequalAreasAndCrossModelTransfers) {
    for (bool degree_day_receiver : {false, true}) {
        SWMM_Engine handle = swmm_engine_create();
        ASSERT_NE(handle, nullptr);
        auto& engine = *static_cast<SWMMEngine*>(handle);
        auto& ctx = engine.context();
        auto& snow = engine.snowSolver().state();
        ctx.subcatch_names.add("A");
        ctx.subcatch_names.add("B");
        ctx.subcatches.resize(2);
        ctx.subcatches.area = {1, 2};
        ctx.subcatches.snowpack = {0, 1};
        ctx.subcatches.gage = {-1, -1};
        ctx.snowpacks.names = {"A", "B"};
        ctx.snowpacks.plowable.resize(2);
        ctx.snowpacks.impervious.resize(2);
        ctx.snowpacks.pervious.resize(2);
        ctx.snowpacks.config.model = surface::SnowKind::Ueb;
        if (degree_day_receiver)
            ctx.snowpacks.config.packs[1].model = surface::SnowKind::DegreeDay;
        ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
        ctx.options.end_date = ctx.options.start_date + 1;
        std::istringstream weather(
            "01/01/2026 00:00 -10 80 0 0 200\n01/01/2026 01:00 -10 80 0 0 200\n");
        ASSERT_TRUE(ctx.snowpacks.meteorology.read(weather).empty());
        snow.resize(2);
        snow.fArea = {.25, .25, .5, .25, .25, .5};
        snow.wsnow[0] = .1;
        snow.sfrac = {.1, .2, .3, .1, .3, 0, 0, 0, 0, 0};
        snow.to_subcatch[0] = 1;
        snow.track_age = true;
        snow.age[0] = 3600;
        ASSERT_TRUE(ueb::initializeSubcatchments(ctx, snow).empty());
        const double initial = ctx.snowpacks.runtime.initial_water;
        const double zero[]{0, 0};
        ASSERT_TRUE(ueb::advanceSubcatchments(ctx, snow, ctx.options.start_date, 0, 300, zero, zero)
                        .empty());
        const double base_area = 1 / ucf::UCF(ucf::LANDAREA, ctx.options) * .3048 * .3048;
        EXPECT_NEAR(snow.wsnow[0], 0, 1e-12);
        EXPECT_NEAR(snow.wsnow[1] * .3048 * .25 * base_area, .2 * initial, 1e-10);
        EXPECT_NEAR(snow.wsnow[2] * .3048 * .5 * base_area, .3 * initial, 1e-10);
        EXPECT_NEAR(snow.wsnow[5] * .3048 * .5 * base_area * 2, .3 * initial, 1e-10);
        SWMM_SnowLedger ledger{};
        ASSERT_EQ(swmm_snow_get_ueb_ledger(handle, &ledger), SWMM_OK);
        EXPECT_NEAR(ledger.removed_m3, .1 * initial, 1e-10);
        EXPECT_NEAR(ledger.liquid_m3, .1 * initial, 1e-10);
        EXPECT_NEAR(ledger.transferred_m3, degree_day_receiver ? -.3 * initial : 0, 1e-10);
        EXPECT_NEAR(ledger.residual_m3, 0, 1e-10);
        EXPECT_NEAR(ledger.energy_residual_kj, 0, 1e-6);
        EXPECT_NEAR(snow.age[5], 3900, 1e-10);
        swmm_engine_destroy(handle);
    }
}

TEST(UebConfiguration, SharedCloudEditAndMeteorologyPreviewAreAtomic) {
    SWMM_Engine h = swmm_engine_create();
    ASSERT_NE(h, nullptr);
    auto& ctx = static_cast<SWMMEngine*>(h)->context();
    ctx.state = EngineState::OPENED;
    const SWMM_ClimateCloudConfig cloud{1, .35, .65, 2, .17};
    char error[1024], name[256];
    ASSERT_EQ(swmm_snow_apply_configuration_with_cloud(h, "SNOW_STEP 1800", "", &cloud, "", error,
                                                       sizeof(error)),
              SWMM_OK)
        << error;
    auto bad = cloud;
    bad.mode = 2;
    EXPECT_EQ(swmm_snow_apply_configuration_with_cloud(h, "SNOW_STEP 900", "", &bad, "absent",
                                                       error, sizeof(error)),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(ctx.snowpacks.config.step, 1800);
    SWMM_ClimateCloudConfig reported{};
    ASSERT_EQ(swmm_climate_get_cloud(h, &reported, name, sizeof(name)), SWMM_OK);
    EXPECT_EQ(reported.fraction, .35);
    EXPECT_EQ(reported.mode, 1);
    const char* path = "_snow_preview.met";
    {
        std::ofstream f(path);
        f << "01/01/2026 00:00 -2 80 1 0 250\n01/01/2026 01:00 3 60 4 200 310\n";
    }
    ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 2. / 24;
    SWMM_ClimateMetSummary summary{};
    ASSERT_EQ(swmm_climate_preview_meteorology(h, path, &summary, error, sizeof(error)), SWMM_OK)
        << error;
    EXPECT_EQ(summary.records, 2);
    EXPECT_EQ(summary.minimum[0], -2);
    EXPECT_EQ(summary.maximum[3], 200);
    EXPECT_TRUE(ctx.snowpacks.meteorology.empty());
    EXPECT_TRUE(ctx.snowpacks.config.met_file.empty());
    ctx.options.end_date += 1. / 24;
    EXPECT_EQ(swmm_climate_preview_meteorology(h, path, &summary, error, sizeof(error)),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(summary.records, 2);
    EXPECT_EQ(summary.maximum[3], 200);
    swmm_engine_destroy(h);
}
TEST(UebConfiguration, NamedSnowpackBatchCannotPartiallyPublish) {
    SWMM_Engine h = swmm_engine_create();
    ASSERT_NE(h, nullptr);
    SWMM_SnowpackEdit edits[2]{};
    edits[0].name = "One";
    edits[1].name = "Two";
    for (auto& e : edits)
        e.ueb = {SWMM_SNOW_UEB, .3, 1, 0, .1, -300};
    edits[0].surface[0][4] = 10;
    edits[1].removal[1] = 1.1;
    char error[512];
    auto& ctx = static_cast<SWMMEngine*>(h)->context();
    ctx.state = EngineState::OPENED;
    EXPECT_EQ(swmm_snowpack_apply(h, edits, 2, error, sizeof(error)), SWMM_ERR_BADPARAM);
    EXPECT_EQ(ctx.snowpacks.count(), 0);
    EXPECT_LT(ctx.snowpack_names.find("One"), 0);
    edits[1].removal[1] = .2;
    ASSERT_EQ(swmm_snowpack_apply(h, edits, 2, error, sizeof(error)), SWMM_OK) << error;
    EXPECT_EQ(ctx.snowpacks.count(), 2);
    EXPECT_EQ(ctx.snowpacks.plowable[0][4], 10);
    edits[0].surface[0][4] = 20;
    edits[1].ueb.forest_fraction = 2;
    EXPECT_EQ(swmm_snowpack_apply(h, edits, 2, error, sizeof(error)), SWMM_ERR_BADPARAM);
    EXPECT_EQ(ctx.snowpacks.plowable[0][4], 10);
    EXPECT_EQ(ctx.snowpacks.config.packs.at(1).site.forest, .3);
    swmm_engine_destroy(h);
}
TEST(SnowEngineIntegration, HeldForcingMergesIdenticalRainRecordsAndStopsAtGaps) {
    SimulationContext ctx;
    ctx.gages.resize(1);
    ctx.gage_names.add("R");
    ctx.gages.source[0] = RainSource::FILE_RAIN;
    ctx.gages.interval_sec[0] = 300;
    ctx.gages.rain_type[0] = 0;
    auto& t = ctx.gages.rain_series[0];
    const double start = datetime::encodeDate(2026, 1, 1);
    t.x = {start, start + 300. / 86400, start + 600. / 86400, start + 1200. / 86400};
    t.y = {1, 1, 2, 2};
    ctx.gages.st_cur[0] = 0;
    ctx.gages.st_next[0] = 1;
    EXPECT_NEAR((gage::gageNextRateChange(ctx, 0, start, start + 1. / 24) - start) * 86400, 600,
                1e-5);
    ctx.gages.st_cur[0] = 2;
    ctx.gages.st_next[0] = 3;
    EXPECT_NEAR((gage::gageNextRateChange(ctx, 0, start + 600. / 86400, start + 1. / 24) - start) *
                    86400,
                900, 1e-5);
    EXPECT_NEAR((gage::gageNextRateChange(ctx, 0, start + 900. / 86400, start + 1. / 24) - start) *
                    86400,
                1200, 1e-5);
    std::istringstream met("01/01/2026 00:00 -2 80 1 0 250\n01/01/2026 01:00 3 60 4 200 310\n");
    ASSERT_TRUE(ctx.snowpacks.meteorology.read(met).empty());
    ctx.snowpacks.runtime.active = true;
    ctx.options.start_date = start + 1800. / 86400;
    EXPECT_NEAR(ueb::nextSnowBoundary(ctx, 0, 3600), 1800, 1e-5);
    EXPECT_NEAR(ueb::nextSnowBoundary(ctx, 1800, 3600), 1800, 1e-5);
}

TEST(UebConfiguration, MeshAssignmentPrecedenceRoundTripAndAtomicEdit) {
    SWMM_Engine h = swmm_engine_create();
    ASSERT_NE(h, nullptr);
    auto& ctx = static_cast<SWMMEngine*>(h)->context();
    ctx.state = EngineState::OPENED;
    ctx.snowpacks.names = {"Alpine"};
    ctx.snowpack_names.add("Alpine");
    char error[1024];
    ASSERT_EQ(swmm_snow2d_apply_configuration(
                  h, "SNOW_STEP 00:30\nSNOWMELT AUTO",
                  "* SNOWPACK Alpine FC .2\nTAG forest SNOWPACK Alpine FC .7 F 1.4",
                  "CELL 2 SNOWPACK NONE\nCELL 3 SNOWPACK Alpine QG 2 SLOPE 30 ASPECT 180", error,
                  sizeof(error)),
              SWMM_OK)
        << error;
    auto row = surface::resolveMeshSnowRow(ctx.snowpacks.mesh_config, 0, "forest");
    EXPECT_EQ(row.pack, "Alpine");
    ASSERT_TRUE(row.forest);
    EXPECT_EQ(*row.forest, .7);
    row = surface::resolveMeshSnowRow(ctx.snowpacks.mesh_config, 1, "forest");
    EXPECT_EQ(row.pack, "NONE");
    row = surface::resolveMeshSnowRow(ctx.snowpacks.mesh_config, 2, "grass");
    EXPECT_EQ(*row.forest, .2);
    EXPECT_EQ(*row.ground_heat, 2);
    std::string sections[3];
    for (int section = 0; section < 3; ++section) {
        int needed = 0;
        ASSERT_EQ(swmm_snow2d_get_configuration(h, section, nullptr, 0, &needed), SWMM_OK);
        std::vector<char> data(needed);
        ASSERT_EQ(swmm_snow2d_get_configuration(h, section, data.data(), needed, &needed), SWMM_OK);
        sections[section] = data.data();
    }
    ASSERT_EQ(swmm_snow2d_apply_configuration(h, sections[0].c_str(), sections[1].c_str(),
                                              sections[2].c_str(), error, sizeof(error)),
              SWMM_OK)
        << error;
    EXPECT_EQ(swmm_snow2d_defaults_count(h), 2);
    EXPECT_EQ(swmm_snow2d_cells_count(h), 2);
    EXPECT_EQ(swmm_snow2d_apply_configuration(h, "SNOWMELT NO", "* SNOWPACK absent", "", error,
                                              sizeof(error)),
              SWMM_ERR_BADPARAM);
    int mode = -1;
    double step = 0;
    ASSERT_EQ(swmm_snow2d_get_options(h, &mode, &step), SWMM_OK);
    EXPECT_EQ(mode, 0);
    EXPECT_EQ(step, 1800);
    EXPECT_EQ(swmm_snow2d_apply_configuration(h, "", "", "CELL 3 SNOWPACK Alpine SLOPE 30", error,
                                              sizeof(error)),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_snow2d_cells_count(h), 2);
    swmm_engine_destroy(h);
}
TEST(MeshSnow, UniformPlaneMatchesPointKernelAndConservesWaterAtDifferentConsumerSteps) {
    SimulationContext ctx;
    ctx.options.ignore_snow_melt = false;
    ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 4. / 24;
    ctx.snowpack_names.add("SP");
    ctx.snowpacks.names = {"SP"};
    ctx.snowpacks.config.model = surface::SnowKind::Ueb;
    ctx.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
    ctx.snowpacks.pervious = {std::array<double, 7>{.02, .06, 32, .1, 6, .6, 0}};
    ctx.snowpacks.mesh_config.defaults["*"].pack = "SP";
    std::istringstream met("01/01/2026 00:00 5 50 0 400 300\n01/01/2026 01:00 5 50 0 400 300\n");
    ASSERT_TRUE(ctx.snowpacks.meteorology.read(met).empty());
    surface::MeshSnow coarse, fine;
    ASSERT_TRUE(coarse.initialize(ctx, {{"grass", 100, 0, 0}, {"grass", 230, 0, 0}}).empty());
    fine = coarse;
    ASSERT_TRUE(coarse.advance(ctx, 0, 3600, {0, 0}, {1, 1}).empty());
    for (int t = 0; t < 3600; t += 300)
        ASSERT_TRUE(fine.advance(ctx, t, t + 300, {0, 0}, {1, 1}).empty());
    const auto& a = coarse.columns()[0].accepted.point;
    const auto& b = fine.columns()[0].accepted.point;
    EXPECT_EQ(a.water, b.water);
    EXPECT_EQ(a.energy, b.energy);
    EXPECT_EQ(a.water, coarse.columns()[1].accepted.point.water);
    auto point = ueb::State{};
    point.water = 6. / 12 * .3048 + .6 / 12 * .3048;
    point.energy = .6 / 12 * .3048 * 333500;
    ueb::Forcing forcing;
    ueb::Site site;
    ASSERT_TRUE(
        climate::resolveUebForcing(ctx, ctx.options.start_date, 3600, 0, 0, forcing, site.pressure)
            .empty());
    ueb::Fluxes flux;
    ASSERT_TRUE(ueb::advance(point, ctx.snowpacks.config.parameters, site, forcing, flux).empty());
    EXPECT_EQ(a.water, point.water);
    EXPECT_EQ(a.energy, point.energy);
    for (const auto& book : fine.ledgers()) {
        EXPECT_NEAR(book.residual(), 0, 1e-10);
        EXPECT_NEAR(book.energyResidual(), 0, 1e-6);
        EXPECT_GT(book.liquid, 0);
    }
    const auto before = fine.outputs()[0].swe_m;
    EXPECT_FALSE(fine.advance(ctx, 3600, 3900, {0, -1}, {1, 1}).empty());
    EXPECT_EQ(fine.completedEnd(), 3600);
    EXPECT_EQ(fine.outputs()[0].swe_m, before);
}
TEST(MeshSnow, PackOverridesAndPartialCoverageUseTheirOwnKernels) {
    SimulationContext ctx;
    ctx.options.ignore_snow_melt = false;
    ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 2. / 24;
    ctx.snowpack_names.add("DD");
    ctx.snowpack_names.add("UEB");
    ctx.snowpacks.names = {"DD", "UEB"};
    ctx.snowpacks.pervious.assign(2, std::array<double, 7>{.02, .06, 32, .1, 6, .6, 0});
    ctx.snowpacks.config.model = surface::SnowKind::DegreeDay;
    ctx.snowpacks.config.packs[1].model = surface::SnowKind::Ueb;
    ctx.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
    ctx.snowpacks.mesh_config.defaults["*"].pack = "DD";
    ctx.snowpacks.mesh_config.cells[1].pack = "UEB";
    ctx.snowpacks.mesh_config.cells[2].pack = "NONE";
    ctx.climate_state.temperature = 45;
    ctx.climate_state.snow_divt = 32;
    std::istringstream met("01/01/2026 00:00 5 50 0 400 300\n01/01/2026 01:00 5 50 0 400 300\n");
    ASSERT_TRUE(ctx.snowpacks.meteorology.read(met).empty());
    surface::MeshSnow mixed, dd, ueb;
    ASSERT_TRUE(
        mixed.initialize(ctx, {{"", 100, 0, 0}, {"", 230, 0, 0}, {"", 90, 0, 0}, {"", 0, 0, 0}})
            .empty());
    ASSERT_TRUE(mixed.owns(0));
    ASSERT_TRUE(mixed.owns(1));
    EXPECT_FALSE(mixed.owns(2));
    EXPECT_FALSE(mixed.owns(3));
    EXPECT_EQ(mixed.kind(0), surface::SnowKind::DegreeDay);
    EXPECT_EQ(mixed.kind(1), surface::SnowKind::Ueb);
    const auto authored = ctx.snowpacks.mesh_config;
    ctx.snowpacks.mesh_config.cells.clear();
    ASSERT_TRUE(dd.initialize(ctx, {{"", 100, 0, 0}}).empty());
    ctx.snowpacks.mesh_config.defaults["*"].pack = "UEB";
    ASSERT_TRUE(ueb.initialize(ctx, {{"", 230, 0, 0}}).empty());
    ctx.snowpacks.mesh_config = authored;
    ASSERT_TRUE(mixed.advance(ctx, 0, 3600, {0, 0, 0, 0}, {1, 1, 1, 1}).empty());
    ASSERT_TRUE(dd.advance(ctx, 0, 3600, {0}, {1}).empty());
    ASSERT_TRUE(ueb.advance(ctx, 0, 3600, {0}, {1}).empty());
    EXPECT_EQ(mixed.outputs()[0].swe_m, dd.outputs()[0].swe_m);
    EXPECT_EQ(mixed.outputs()[1].swe_m, ueb.outputs()[0].swe_m);
    EXPECT_EQ(mixed.outputs()[1].energy_kJm2, ueb.outputs()[0].energy_kJm2);
    EXPECT_TRUE(std::isnan(mixed.outputs()[0].energy_kJm2));
    EXPECT_TRUE(std::isfinite(mixed.outputs()[1].energy_kJm2));
    for (int i : {0, 1})
        EXPECT_NEAR(mixed.ledgers()[i].residual(), 0, 1e-9);
    EXPECT_EQ(mixed.ledgers()[2].initial, 0);
    EXPECT_EQ(mixed.ledgers()[3].initial, 0);
    ctx.options.ignore_snow_melt = true;
    surface::MeshSnow disabled;
    EXPECT_TRUE(disabled.initialize(ctx, {{"", 100, 0, 0}, {"", 230, 0, 0}}).empty());
    EXPECT_FALSE(disabled.active());
    EXPECT_EQ(ctx.snowpacks.mesh_config.cells[1].pack, "UEB");
}

TEST(MeshSnow, DegreeDayUsesExistingPointKernelWithCompleteCover) {
    SimulationContext ctx;
    ctx.options.ignore_snow_melt = false;
    ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 2. / 24;
    ctx.snowpack_names.add("SP");
    ctx.snowpacks.names = {"SP"};
    ctx.snowpacks.pervious = {std::array<double, 7>{.02, .06, 32, .1, 6, .6, 0}};
    ctx.snowpacks.mesh_config.defaults["*"].pack = "SP";
    ctx.climate_state.temperature = 45;
    ctx.climate_state.snow_divt = 32;
    surface::MeshSnow driver;
    ASSERT_TRUE(driver.initialize(ctx, {{"", 50, 0, 0}}).empty());
    for (int t = 0; t < 3600; t += 300)
        ASSERT_TRUE(driver.advance(ctx, t, t + 300, {1e-6}, {1}).empty());
    EXPECT_GT(driver.ledgers()[0].liquid, 0);
    EXPECT_NEAR(driver.ledgers()[0].residual(), 0, 1e-10);
    EXPECT_TRUE(std::isnan(driver.outputs()[0].energy_kJm2));
}

TEST(MeshSnow, PartialStepCheckpointRestartsExactlyAndRejectsChangedWeather) {
    SimulationContext ctx;
    ctx.options.ignore_snow_melt = false;
    ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 2. / 24;
    ctx.snowpack_names.add("SP");
    ctx.snowpacks.names = {"SP"};
    ctx.snowpacks.config.model = surface::SnowKind::Ueb;
    ctx.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
    ctx.snowpacks.pervious = {std::array<double, 7>{.02, .06, 32, .1, 6, 0, 0}};
    ctx.snowpacks.mesh_config.defaults["*"].pack = "SP";
    std::istringstream met("01/01/2026 00:00 5 50 1 400 300\n01/01/2026 01:00 5 50 1 400 300\n");
    ASSERT_TRUE(ctx.snowpacks.meteorology.read(met).empty());
    surface::MeshSnow original, resumed;
    ASSERT_TRUE(original.initialize(ctx, {{"", 50, 0, 0}}).empty());
    resumed = original;
    ASSERT_TRUE(original.advance(ctx, 0, 317, {1e-7}, {1.2}).empty());
    const auto bytes = original.saveCheckpoint();
    ASSERT_FALSE(bytes.empty());
    ASSERT_TRUE(resumed.restoreCheckpoint(bytes).empty());
    EXPECT_EQ(resumed.saveCheckpoint(), bytes);
    EXPECT_FALSE(resumed.restoreCheckpoint(bytes.substr(0, bytes.size() - 1)).empty());
    EXPECT_EQ(resumed.saveCheckpoint(), bytes);
    for (int end : {719, 1800, 3600}) {
        const double start = original.completedEnd();
        ASSERT_TRUE(original.advance(ctx, start, end, {1e-7}, {1.2}).empty());
        ASSERT_TRUE(resumed.advance(ctx, start, end, {1e-7}, {1.2}).empty());
        EXPECT_EQ(original.saveCheckpoint(), resumed.saveCheckpoint());
    }
    ASSERT_TRUE(resumed.restoreCheckpoint(bytes, true).empty());
    EXPECT_EQ(resumed.completedEnd(), 0);
    EXPECT_FALSE(resumed.columns()[0].pending());
    EXPECT_EQ(resumed.ledgers()[0].residual(), 0);
    EXPECT_EQ(resumed.ledgers()[0].precipitation, 0);
    auto changed = ctx;
    changed.heat_config.cloud.configured = true;
    changed.heat_config.cloud.fraction = .5;
    ASSERT_TRUE(resumed.initialize(changed, {{"", 50, 0, 0}}).empty());
    EXPECT_FALSE(resumed.restoreCheckpoint(bytes).empty());
    ASSERT_TRUE(resumed.restoreCheckpoint("").empty());
    EXPECT_EQ(resumed.outputs()[0].swe_m, 0);
    EXPECT_EQ(resumed.outputs()[0].energy_kJm2, 0);
}

TEST(MeshSnow, ReciprocalGroundContactEnergyClosesAndRestoresWithItsOwnership) {
    SimulationContext ctx;
    ctx.options.ignore_snow_melt = false;
    ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 2. / 24;
    ctx.snowpack_names.add("SP");
    ctx.snowpacks.names = {"SP"};
    ctx.snowpacks.config.model = surface::SnowKind::Ueb;
    ctx.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
    ctx.snowpacks.pervious = {std::array<double, 7>{.02, .06, 32, .1, 6, .6, 0}};
    ctx.snowpacks.mesh_config.defaults["*"].pack = "SP";
    std::istringstream met("01/01/2026 00:00 5 50 0 400 300\n01/01/2026 01:00 5 50 0 400 300\n");
    ASSERT_TRUE(ctx.snowpacks.meteorology.read(met).empty());
    const std::vector<surface::MeshSnowGeometry> geometry{{"grass", 100, 0, 0}};
    surface::MeshSnow snow;
    ASSERT_TRUE(snow.initialize(ctx, geometry).empty());
    ASSERT_TRUE(snow.setGroundContactMask({1}).empty());
    ASSERT_TRUE(snow.advance(ctx, 0, 300, {0}, {1}).empty());
    ASSERT_TRUE(snow.applyGroundContactEnergy(0, 1234.).empty());
    const auto& ledger = snow.ledgers()[0];
    EXPECT_NEAR(ledger.ground_contact_energy, -1.234, 1e-12);
    EXPECT_NEAR(ledger.energyResidual(), 0, 1e-6);
    const auto checkpoint = snow.saveCheckpoint();
    ASSERT_FALSE(checkpoint.empty());
    surface::MeshSnow restored;
    ASSERT_TRUE(restored.initialize(ctx, geometry).empty());
    ASSERT_TRUE(restored.setGroundContactMask({1}).empty());
    ASSERT_TRUE(restored.restoreCheckpoint(checkpoint).empty());
    EXPECT_EQ(restored.saveCheckpoint(), checkpoint);
    surface::MeshSnow wrong_owner;
    ASSERT_TRUE(wrong_owner.initialize(ctx, geometry).empty());
    ASSERT_TRUE(wrong_owner.setGroundContactMask({0}).empty());
    EXPECT_NE(wrong_owner.restoreCheckpoint(checkpoint), "");
}

TEST(MeshSnow, SubNanosecondIntervalPreservesAcceptedSnowCoverage) {
    SimulationContext ctx;
    ctx.options.ignore_snow_melt = false;
    ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 2. / 24;
    ctx.snowpack_names.add("SP");
    ctx.snowpacks.names = {"SP"};
    ctx.snowpacks.config.model = surface::SnowKind::Ueb;
    ctx.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
    ctx.snowpacks.pervious = {std::array<double, 7>{.02, .06, 32, .1, 6, 0, 0}};
    ctx.snowpacks.mesh_config.defaults["*"].pack = "SP";
    std::istringstream met("01/01/2026 00:00 5 50 0 400 300\n01/01/2026 01:00 5 50 0 400 300\n");
    ASSERT_TRUE(ctx.snowpacks.meteorology.read(met).empty());
    surface::MeshSnow snow;
    ASSERT_TRUE(snow.initialize(ctx, {{"grass", 50, 0, 0}}).empty());
    ASSERT_TRUE(snow.setGroundContactMask({1}).empty());
    ASSERT_TRUE(snow.advance(ctx, 0, 300, {0}, {1}).empty());
    ASSERT_DOUBLE_EQ(snow.groundContactFraction(0), 1);
    const double contact = snow.ledgers()[0].ground_contact_energy;
    const double energy = snow.outputs()[0].energy_kJm2;
    const double end = std::nextafter(300., std::numeric_limits<double>::infinity());
    ASSERT_LT(end - 300., 1e-9);
    ASSERT_TRUE(snow.advance(ctx, 300, end, {0}, {1}).empty());
    EXPECT_EQ(snow.completedEnd(), end);
    EXPECT_DOUBLE_EQ(snow.lastSeconds(), 300);
    EXPECT_DOUBLE_EQ(snow.groundContactFraction(0), 1);
    EXPECT_DOUBLE_EQ(snow.ledgers()[0].ground_contact_energy, contact);
    EXPECT_DOUBLE_EQ(snow.outputs()[0].energy_kJm2, energy);
    ASSERT_TRUE(snow.advance(ctx, end, 600, {0}, {1}).empty());
    EXPECT_DOUBLE_EQ(snow.groundContactFraction(0), 1);
}

TEST(MeshSnow, SnowfallReestablishesGroundContactAfterMeltOut) {
    SimulationContext ctx;
    ctx.options.ignore_snow_melt = false;
    ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 2. / 24;
    ctx.snowpack_names.add("SP");
    ctx.snowpacks.names = {"SP"};
    ctx.snowpacks.config.model = surface::SnowKind::Ueb;
    ctx.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
    ctx.snowpacks.pervious = {std::array<double, 7>{.02, .06, 32, .1, .1, 0, 0}};
    ctx.snowpacks.mesh_config.defaults["*"].pack = "SP";
    const std::array<double, 10> adc{0, .2, .35, .48, .59, .69, .78, .85, .91, .96};
    for (int i = 0; i < 10; ++i)
        ctx.options.adc_perv[i] = adc[i];
    std::istringstream met(
        "01/01/2026 00:00 15 50 1 800 400\n01/01/2026 01:00 -5 50 1 0 250\n01/01/2026 02:00 -5 50 1 0 250\n");
    ASSERT_TRUE(ctx.snowpacks.meteorology.read(met).empty());
    surface::MeshSnow snow;
    ASSERT_TRUE(snow.initialize(ctx, {{"grass", 50, 0, 0}}).empty());
    EXPECT_EQ(snow.depletionCurve()[5], adc[5]);
    ASSERT_TRUE(snow.setGroundContactMask({1}).empty());
    ASSERT_TRUE(snow.advance(ctx, 0, 3600, {0}, {1}).empty());
    ASSERT_DOUBLE_EQ(snow.outputs()[0].cover_frac, 0);
    EXPECT_DOUBLE_EQ(snow.groundContactFraction(0), 0);
    const auto rewet_error = snow.advance(ctx, 3600, 7200, {1e-5}, {1});
    ASSERT_TRUE(rewet_error.empty()) << rewet_error;
    ASSERT_GT(snow.outputs()[0].swe_m, 0);
    EXPECT_DOUBLE_EQ(snow.outputs()[0].cover_frac, 1);
    EXPECT_DOUBLE_EQ(snow.groundContactFraction(0), 1);
    ASSERT_TRUE(snow.applyGroundContactEnergy(0, 1000).empty());
    EXPECT_NEAR(snow.ledgers()[0].energyResidual(), 0, 1e-8);
    EXPECT_NEAR(snow.ledgers()[0].ground_contact_energy, -1, 1e-12);
    EXPECT_NEAR(snow.ledgers()[0].residual(), 0, 1e-9);
}

TEST(MeshSnow, UebPartialCoverUsesProjectPerviousCurveAndBalancesContact) {
    SimulationContext ctx;
    ctx.options.ignore_snow_melt = false;
    ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 4. / 24;
    ctx.snowpack_names.add("SP");
    ctx.snowpacks.names = {"SP"};
    ctx.snowpacks.config.model = surface::SnowKind::Ueb;
    ctx.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
    ctx.snowpacks.pervious = {std::array<double, 7>{.02, .06, 32, .1, .1, 0, 0}};
    ctx.snowpacks.mesh_config.defaults["*"].pack = "SP";
    const std::array<double, 10> adc{0, .2, .35, .48, .59, .69, .78, .85, .91, .96};
    for (int i = 0; i < 10; ++i)
        ctx.options.adc_perv[i] = adc[i];
    std::istringstream met(
        "01/01/2026 00:00 15 50 1 800 400\n01/01/2026 01:00 15 50 1 800 400\n01/01/2026 02:00 15 50 1 800 400\n01/01/2026 03:00 15 50 1 800 400\n01/01/2026 04:00 15 50 1 800 400\n");
    ASSERT_TRUE(ctx.snowpacks.meteorology.read(met).empty());
    surface::MeshSnow snow;
    ASSERT_TRUE(snow.initialize(ctx, {{"grass", 50, 0, 0}}).empty());
    ASSERT_EQ(snow.depletionCurve()[5], adc[5]);
    ASSERT_TRUE(snow.setGroundContactMask({1}).empty());

    double time = 0;
    bool found_partial = false, melted_after_partial = false;
    for (; time < 4 * 3600 && !melted_after_partial; time += 30) {
        ASSERT_TRUE(snow.advance(ctx, time, time + 30, {0}, {1}).empty());
        const double cover = snow.outputs()[0].cover_frac;
        if (cover > 0 && cover < 1 && !found_partial) {
            found_partial = true;
            EXPECT_DOUBLE_EQ(snow.groundContactFraction(0), cover);
            const auto contact_error = snow.applyGroundContactEnergy(0, -1000);
            ASSERT_TRUE(contact_error.empty()) << contact_error;
            EXPECT_NEAR(snow.ledgers()[0].ground_contact_energy, 1, 1e-12);
            EXPECT_NEAR(snow.ledgers()[0].energyResidual(), 0, 1e-8);
        }
        if (found_partial && cover == 0)
            melted_after_partial = true;
    }
    ASSERT_TRUE(found_partial)
        << "Warm UEB forcing did not pass through fractional cover before melt-out.";
    ASSERT_TRUE(melted_after_partial) << "Warm UEB forcing did not melt the ADC-covered pack out.";
    EXPECT_DOUBLE_EQ(snow.groundContactFraction(0), 0);
    EXPECT_NEAR(snow.ledgers()[0].residual(), 0, 1e-9);
    EXPECT_NEAR(snow.ledgers()[0].energyResidual(), 0, 1e-6);
    EXPECT_NE(snow.ledgers()[0].geometry_energy, 0);
    auto changed = ctx;
    changed.options.adc_perv[5] = .7;
    surface::MeshSnow other_curve;
    ASSERT_TRUE(other_curve.initialize(changed, {{"grass", 50, 0, 0}}).empty());
    ASSERT_TRUE(other_curve.setGroundContactMask({1}).empty());
    EXPECT_FALSE(other_curve.restoreCheckpoint(snow.saveCheckpoint()).empty());
}

TEST(SharedRadiation, IntervalTemperatureIgnoresRunAheadCursorAndRuntimeOverridesWinOverMet) {
    SimulationContext ctx;
    const double start = datetime::encodeDate(2026, 1, 1);
    ctx.options.start_date = start;
    ctx.options.end_date = start + 1;
    ctx.tables.tables.resize(1);
    auto& table = ctx.tables.tables[0];
    table.id = "air";
    table.x = {start, start + 1. / 24};
    table.y = {32, 50};
    ctx.climate_state.temp_ts_index = 0;
    ctx.climate_state.temperature = 90;
    climate::RadiationState weather;
    ASSERT_TRUE(climate::resolveRadiation(ctx, start, 3600, weather).empty());
    EXPECT_NEAR(weather.temperature, 5, 1e-7);
    std::istringstream met("01/01/2026 00:00 -5 80 1 0 200\n");
    ASSERT_TRUE(ctx.snowpacks.meteorology.read(met).empty());
    ctx.forcing.climate_temperature_mode = ForcingMode::OVERRIDE;
    ctx.forcing.climate_temperature_value = 50;
    ctx.forcing.climate_wind_mode = ForcingMode::ADD;
    ctx.forcing.climate_wind_value = 1 / .44704;
    ASSERT_TRUE(climate::resolveRadiation(ctx, start, 3600, weather).empty());
    EXPECT_DOUBLE_EQ(weather.temperature, 10);
    EXPECT_DOUBLE_EQ(weather.wind, 2);
    EXPECT_DOUBLE_EQ(weather.longwave, 200);
}

#ifdef OPENSWMM_HAS_2D
TEST(SnowEngineIntegration, MeshSnowRoutesOnlyLiquidAndSeedsHotStarts) {
    for (const std::string model : {"DEGREE_DAY", "UEB"})
        for (bool warm : {false, true}) {
            const std::string stem = "_mesh_snow_" + model + (warm ? "_warm" : "_cold");
            SCOPED_TRACE(stem);
            {
                std::ofstream f(stem + ".inp");
                f << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\nEND_DATE 01/01/2026\n"
                     "START_TIME 00:00\nEND_TIME 01:00\nWET_STEP 00:05\nDRY_STEP 00:05\nROUTING_STEP 30\nREPORT_STEP 00:05\n"
                     "IGNORE_QUALITY YES\n[TEMPERATURE]\nTIMESERIES air\nSNOWMELT 1.11111111111111 .5 .6 0 45 0\nSNOWMELT_MODEL "
                  << model
                  << "\nSITE 45 -120 -8 1000\n"
                     "SHORTWAVE TIMESERIES sw\nLONGWAVE TIMESERIES lw\n"
                     "[RAINGAGES]\nRG INTENSITY 1:00 1.4 TIMESERIES rain\n"
                     "[TIMESERIES]\nair 01/01/2026 00:00 "
                  << (warm ? 8 : -5) << "\nair 01/01/2026 02:00 " << (warm ? 8 : -5)
                  << "\n"
                     "rain 01/01/2026 00:00 1\nrain 01/01/2026 02:00 1\n"
                     "sw 01/01/2026 00:00 "
                  << (warm ? 600 : 0) << "\nsw 01/01/2026 02:00 " << (warm ? 600 : 0)
                  << "\n"
                     "lw 01/01/2026 00:00 "
                  << (warm ? 400 : 200) << "\nlw 01/01/2026 02:00 " << (warm ? 400 : 200)
                  << "\n"
                     "[SNOWPACKS]\nSP PERVIOUS 1 2 0 .1 10 0 0\n"
                     "[2D_OPTIONS]\nMAX_TIMESTEP 30\nREPORT_2D NO\nINFILTRATION NO\nEVAPORATION NO\n"
                     "[2D_SNOW_DEFAULTS]\n* SNOWPACK SP\n[2D_VERTICES]\n0 0 0\n10 0 0\n0 10 0\n"
                     "[2D_TRIANGLES]\n0 1 2 .03\n";
            }
            SWMM_Engine handle = swmm_engine_create();
            ASSERT_NE(handle, nullptr);
            ASSERT_EQ(swmm_engine_open(handle, (stem + ".inp").c_str(), (stem + ".rpt").c_str(),
                                       (stem + ".out").c_str(), nullptr),
                      SWMM_OK)
                << swmm_get_last_error_msg(handle);
            ASSERT_EQ(swmm_engine_initialize(handle), SWMM_OK) << swmm_get_last_error_msg(handle);
            auto& engine = *static_cast<SWMMEngine*>(handle);
            auto& router = engine.surfaceRouter2D();
            ASSERT_TRUE(router.isActive());
            ASSERT_TRUE(router.state().snow.active());
            ASSERT_EQ(swmm_engine_start(handle, 0), SWMM_OK);
            double elapsed = 0;
            int count = 0;
            do {
                ASSERT_EQ(swmm_engine_step(handle, &elapsed), SWMM_OK)
                    << swmm_get_last_error_msg(handle);
                ASSERT_LT(++count, 2000);
                SWMM_SnowState state{};
                SWMM_MeshSnowLedger reported{};
                double geometry_energy = 0;
                ASSERT_EQ(swmm_snow2d_get_states(handle, 0, 1, &state), SWMM_OK);
                ASSERT_EQ(swmm_snow2d_get_ledgers(handle, 0, 1, &reported), SWMM_OK);
                ASSERT_EQ(swmm_snow2d_get_geometry_energy(handle, 0, 1, &geometry_energy), SWMM_OK);
                EXPECT_EQ(state.model, model == "UEB" ? SWMM_SNOW_UEB : SWMM_SNOW_DEGREE_DAY);
                EXPECT_EQ(state.swe_m, router.state().snow.outputs()[0].swe_m);
                EXPECT_EQ(reported.completed_s, router.state().snow.completedEnd());
                EXPECT_NEAR(reported.residual_m3, 0, 1e-10);
                if (model == "DEGREE_DAY") {
                    EXPECT_TRUE(std::isnan(state.energy_kjm2));
                    EXPECT_TRUE(std::isnan(geometry_energy));
                } else {
                    EXPECT_TRUE(std::isfinite(state.energy_kjm2));
                    EXPECT_DOUBLE_EQ(geometry_energy,
                                     router.state().snow.ledgers()[0].geometry_energy);
                }
                const auto previous = state.swe_m;
                EXPECT_EQ(swmm_snow2d_get_states(handle, 1, 1, &state), SWMM_ERR_BADPARAM);
                EXPECT_EQ(state.swe_m, previous);
                EXPECT_EQ(swmm_snow2d_get_geometry_energy(handle, 1, 1, &geometry_energy),
                          SWMM_ERR_BADPARAM);
                if (count == 1) {
                    ASSERT_EQ(swmm_hotstart_save(handle, (stem + ".hsf").c_str()), SWMM_OK)
                        << swmm_hotstart_last_io_error();
                    SWMM_HotStart hs = nullptr;
                    ASSERT_EQ(swmm_hotstart_open((stem + ".hsf").c_str(), &hs), SWMM_OK);
                    SWMM_Engine seed = swmm_engine_create();
                    ASSERT_EQ(swmm_engine_open(seed, (stem + ".inp").c_str(),
                                               (stem + "_seed.rpt").c_str(),
                                               (stem + "_seed.out").c_str(), nullptr),
                              SWMM_OK);
                    ASSERT_EQ(swmm_engine_initialize(seed), SWMM_OK);
                    ASSERT_EQ(swmm_hotstart_apply(seed, hs), SWMM_OK)
                        << swmm_hotstart_last_io_error();
                    const auto& restored =
                        static_cast<SWMMEngine*>(seed)->surfaceRouter2D().state().snow;
                    EXPECT_EQ(restored.outputs()[0].swe_m, router.state().snow.outputs()[0].swe_m);
                    EXPECT_EQ(restored.completedEnd(), 0);
                    EXPECT_EQ(restored.ledgers()[0].residual(), 0);
                    swmm_hotstart_close(hs);
                    swmm_engine_destroy(seed);
                }
            } while (elapsed > 0);
            ASSERT_EQ(swmm_engine_end(handle), SWMM_OK);
            const auto& ledger = router.state().snow.ledgers()[0];
            EXPECT_NEAR(ledger.raw_precipitation, .05, 1e-9);
            EXPECT_NEAR(ledger.precipitation, warm ? .05 : .07, 1e-9);
            EXPECT_NEAR(ledger.residual(), 0, 1e-9);
            EXPECT_NEAR(engine.context().mass_balance_2d.rainfall_in, ledger.liquid, 1e-10);
            EXPECT_NEAR(router.totalVolume(), ledger.liquid, 1e-10);
            if (warm)
                EXPECT_GT(ledger.liquid,
                          0); // rain can remain as retained free water while the pack melts
            else
                EXPECT_GT(ledger.stored, ledger.initial);
            if (model == "UEB")
                EXPECT_NEAR(ledger.energyResidual(), 0, 1e-6);
            swmm_engine_destroy(handle);
        }
}

TEST(SnowEngineIntegration, DegreeDayBoundaryAndReciprocalUebSoilContactOwnTheirEnergyOnce) {
    for (const std::string model : {"DEGREE_DAY", "UEB"})
        for (bool conduction : {true, false})
            for (bool surface_coupled : {true, false}) {
                if (model == "DEGREE_DAY" && !conduction)
                    continue;
                const std::string stem = "_mesh_snow_ground_boundary_" + model +
                                         (conduction ? "_contact" : "_external") +
                                         (surface_coupled ? "_surface" : "_aquifer");
                SCOPED_TRACE(stem);
                {
                    std::ofstream f(stem + ".inp");
                    f << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT YES\n"
                         "START_DATE 01/01/2026\nEND_DATE 01/01/2026\nSTART_TIME 00:00\nEND_TIME 00:01\n"
                         "WET_STEP 00:01\nDRY_STEP 00:01\nROUTING_STEP 30\nREPORT_STEP 00:01\n"
                         "[TEMPERATURE]\nTIMESERIES air\nSNOWMELT 1.11111111111111 .5 .6 0 45 0\n"
                         "SNOWMELT_MODEL "
                      << model
                      << "\nSITE 45 -120 -8 1000\n"
                         "[TIMESERIES]\nair 01/01/2026 00:00 -5\nair 01/01/2026 02:00 -5\n"
                         "[SNOWPACKS]\nSP PERVIOUS 1 2 0 .1 10 0 0\n"
                         "[2D_OPTIONS]\nMAX_TIMESTEP 30\nREPORT_2D NO\nINFILTRATION NO\nEVAPORATION NO\n"
                      << (surface_coupled
                              ? "HEAT_MODEL COUPLED\nBED_HEAT_MODEL GROUND_COLUMN\nHEAT_SURFACE_EXCHANGE NO\nHEAT_RADIATIVE_EXCHANGE NO\n"
                              : "")
                      << "[2D_SNOW_DEFAULTS]\n* SNOWPACK SP QG 20\n[2D_SNOW]\nCELL 2 SNOWPACK NONE\n"
                         "[2D_VERTICES]\n0 0 0\n10 0 0\n0 10 0\n10 10 0\n"
                         "[2D_TRIANGLES]\n0 1 2 .03\n1 3 2 .03\n"
                         "[2D_AQUIFER_OPTIONS]\nNODE_ENROLMENT ROWS\nGW_ET NONE\nM_LAYERS 4\nCLOSURE SIGMA\n"
                         "[2D_AQUIFER]\n* 36 1 .45 .1 2 HG0 .5\n"
                         "[GW_TRANSPORT_OPTIONS]\nTHERMAL_MODEL POROUS_EQUILIBRIUM\nCONDUCTION "
                      << (conduction ? "YES" : "NO")
                      << "\n"
                         "SURFACE_THERMAL_BC HEAT_FLUX 5\nDEEP_THERMAL_BC ADIABATIC\n"
                         "[GW_INITIAL_QUALITY]\n* SAT __TEMPERATURE__ -3\n* UNSAT __TEMPERATURE__ -3\n";
                }
                SWMM_Engine handle = swmm_engine_create();
                ASSERT_NE(handle, nullptr);
                ASSERT_EQ(swmm_engine_open(handle, (stem + ".inp").c_str(), (stem + ".rpt").c_str(),
                                           (stem + ".out").c_str(), nullptr),
                          SWMM_OK)
                    << swmm_get_last_error_msg(handle);
                ASSERT_EQ(swmm_engine_initialize(handle), SWMM_OK)
                    << swmm_get_last_error_msg(handle);
                auto& engine = *static_cast<SWMMEngine*>(handle);
                auto& router = engine.surfaceRouter2D();
                SWMM_GroundHeatStatus ground{};
                ground.struct_size = sizeof ground;
                ASSERT_EQ(swmm_heat_get_ground_status(handle, &ground), SWMM_OK);
                EXPECT_TRUE(ground.initialized);
                EXPECT_EQ(ground.conduction_active, conduction);
                EXPECT_EQ(ground.aquifer_cells, 2);
                SWMM_SnowState snow{};
                ASSERT_EQ(swmm_snow2d_get_states(handle, 0, 1, &snow), SWMM_OK);
                EXPECT_EQ(snow.model, model == "UEB" ? SWMM_SNOW_UEB : SWMM_SNOW_DEGREE_DAY);
                EXPECT_GT(snow.swe_m, 0);
                if (model == "DEGREE_DAY")
                    EXPECT_TRUE(std::isnan(snow.energy_kjm2));
                else
                    EXPECT_TRUE(std::isfinite(snow.energy_kjm2));
                ASSERT_EQ(swmm_engine_start(handle, 0), SWMM_OK) << swmm_get_last_error_msg(handle);
                double elapsed = 0;
                int count = 0;
                do {
                    ASSERT_EQ(swmm_engine_step(handle, &elapsed), SWMM_OK)
                        << swmm_get_last_error_msg(handle);
                    ASSERT_LT(++count, 100);
                } while (elapsed > 0);
                ASSERT_EQ(swmm_snow2d_get_states(handle, 0, 1, &snow), SWMM_OK);
                EXPECT_GT(snow.swe_m, 0);
                double surface_j[2]{};
                ASSERT_EQ(swmm_heat_get_ground_values(handle, SWMM_GROUND_HEAT_SURFACE_ENERGY, 0, 2,
                                                      surface_j),
                          SWMM_OK);
                if (model == "DEGREE_DAY") {
                    EXPECT_NEAR(surface_j[0], 5 * 50 * 60, 1e-6);
                    EXPECT_NEAR(surface_j[1], 5 * 50 * 60, 1e-6);
                } else if (conduction) {
                    EXPECT_NE(surface_j[0], 0);
                    EXPECT_NEAR(surface_j[1], 5 * 50 * 60, 1e-6)
                        << "contact-cell receipt=" << surface_j[0]
                        << " mask=" << router.state().snow.groundContactEnabled(0) << ','
                        << router.state().snow.groundContactEnabled(1)
                        << " cover=" << router.state().snow.groundContactFraction(0) << ','
                        << router.state().snow.groundContactFraction(1);
                    const auto& snow_book = router.state().snow.ledgers()[0];
                    EXPECT_NEAR(snow_book.prescribed_ground_energy, 0, 1e-12);
                    EXPECT_NEAR(surface_j[0] + snow_book.ground_contact_energy * 1000., 0, 1e-6);
                    EXPECT_NEAR(snow_book.energyResidual(), 0, 1e-6);
                } else {
                    EXPECT_NEAR(surface_j[0], 0, 1e-9);
                    EXPECT_NEAR(surface_j[1], 0, 1e-9);
                    EXPECT_NEAR(router.state().snow.ledgers()[0].prescribed_ground_energy,
                                20 * 50 * 60 / 1000., 1e-9);
                }
                double initial_j[2]{}, final_j[2]{};
                ASSERT_EQ(swmm_heat_get_ground_values(handle, SWMM_GROUND_HEAT_INITIAL_ENERGY, 0, 2,
                                                      initial_j),
                          SWMM_OK);
                ASSERT_EQ(
                    swmm_heat_get_ground_values(handle, SWMM_GROUND_HEAT_ENERGY, 0, 2, final_j),
                    SWMM_OK);
                EXPECT_NEAR(final_j[0] - initial_j[0], surface_j[0], 1e-5);
                ASSERT_EQ(swmm_engine_end(handle), SWMM_OK);
                swmm_engine_close(handle);
                swmm_engine_destroy(handle);
            }
}

TEST(SnowEngineIntegration, UebContactStopsAtMeltOutAndWetAreaOwnsTheSurface) {
    const std::string stem = "_mesh_snow_ueb_contact_melt_transition";
    {
        std::ofstream f(stem + ".inp");
        f << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT YES\n"
             "START_DATE 01/01/2026\nEND_DATE 01/01/2026\nSTART_TIME 00:00\nEND_TIME 12:00\n"
             "WET_STEP 00:01\nDRY_STEP 00:01\nROUTING_STEP 30\nREPORT_STEP 00:05\nIGNORE_QUALITY YES\n"
             "[TEMPERATURE]\nTIMESERIES air\nSNOWMELT 1.11111111111111 .5 .6 0 45 0\nSNOWMELT_MODEL UEB\nSITE 45 -120 -8 1000\n"
             "SHORTWAVE TIMESERIES sw\nLONGWAVE TIMESERIES lw\n"
             "[RAINGAGES]\nRG INTENSITY 1:00 1 TIMESERIES rain\n"
             "[TIMESERIES]\nair 01/01/2026 00:00 35\nair 01/01/2026 12:00 35\n"
             "rain 01/01/2026 00:00 0.1\nrain 01/01/2026 12:00 0.1\n"
             "sw 01/01/2026 00:00 900\nsw 01/01/2026 12:00 900\n"
             "lw 01/01/2026 00:00 400\nlw 01/01/2026 12:00 400\n"
             "[SNOWPACKS]\nSP PERVIOUS 1 2 0 .1 10 0 0\n"
             "[2D_OPTIONS]\nMAX_TIMESTEP 30\nREPORT_2D NO\nINFILTRATION YES\nEVAPORATION NO\n"
             "HEAT_MODEL COUPLED\nBED_HEAT_MODEL GROUND_COLUMN\nHEAT_SURFACE_EXCHANGE NO\nHEAT_RADIATIVE_EXCHANGE NO\n"
             "[2D_SNOW_DEFAULTS]\n* SNOWPACK SP QG 20\n"
             "[2D_VERTICES]\n0 0 0\n10 0 0\n0 10 0\n"
             "[2D_TRIANGLES]\n0 1 2 .03\n"
             "[2D_AQUIFER_OPTIONS]\nNODE_ENROLMENT ROWS\nGW_ET NONE\nM_LAYERS 4\nCLOSURE SIGMA\n"
             "[2D_AQUIFER]\n* 36 1 .45 .1 2 HG0 .5\n"
             "[GW_TRANSPORT_OPTIONS]\nTHERMAL_MODEL POROUS_EQUILIBRIUM\nCONDUCTION YES\n"
             "SURFACE_THERMAL_BC HEAT_FLUX 5\nDEEP_THERMAL_BC ADIABATIC\n"
             "[GW_INITIAL_QUALITY]\n* SAT __TEMPERATURE__ -3\n* UNSAT __TEMPERATURE__ -3\n";
    }
    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(swmm_engine_open(handle, (stem + ".inp").c_str(), (stem + ".rpt").c_str(),
                               (stem + ".out").c_str(), nullptr),
              SWMM_OK)
        << swmm_get_last_error_msg(handle);
    ASSERT_EQ(swmm_engine_initialize(handle), SWMM_OK) << swmm_get_last_error_msg(handle);
    auto& engine = *static_cast<SWMMEngine*>(handle);
    auto& router = engine.surfaceRouter2D();
    auto& snow = router.state().snow;
    ASSERT_EQ(snow.kind(0), surface::SnowKind::Ueb);
    ASSERT_GT(snow.outputs()[0].swe_m, 0);
    ASSERT_EQ(swmm_engine_start(handle, 0), SWMM_OK) << swmm_get_last_error_msg(handle);
    double elapsed = 0, previous_soil_j = 0, previous_contact_kj = 0, previous_precipitation = 0;
    double previous_cover = snow.outputs()[0].cover_frac;
    int count = 0;
    bool melted = false, contact_stopped = false, wet_snowless = false, external_resumed = false,
         rain_on_snow = false;
    do {
        ASSERT_EQ(swmm_engine_step(handle, &elapsed), SWMM_OK) << swmm_get_last_error_msg(handle);
        ASSERT_LT(++count, 10000);
        double soil_j = 0;
        ASSERT_EQ(
            swmm_heat_get_ground_values(handle, SWMM_GROUND_HEAT_SURFACE_ENERGY, 0, 1, &soil_j),
            SWMM_OK);
        const auto& book = snow.ledgers()[0];
        EXPECT_NEAR(book.energyResidual(), 0, 1e-5);
        const double cover = snow.outputs()[0].cover_frac;
        if (cover > 0 && book.precipitation > previous_precipitation)
            rain_on_snow = true;
        if (previous_cover > 0 && cover <= 0)
            melted = true;
        if (melted && previous_cover <= 0 && cover <= 0) {
            EXPECT_NEAR(book.ground_contact_energy, previous_contact_kj, 1e-9);
            contact_stopped = true;
            if (router.state().volume[0] > 1e-12) {
                wet_snowless = true;
                EXPECT_NEAR(soil_j - previous_soil_j, 0, 1e-6);
            } else if (soil_j - previous_soil_j > 0)
                external_resumed = true;
        }
        previous_cover = cover;
        previous_soil_j = soil_j;
        previous_contact_kj = book.ground_contact_energy;
        previous_precipitation = book.precipitation;
    } while (elapsed > 0);
    EXPECT_TRUE(melted) << "hot forcing did not deplete the assigned UEB pack";
    EXPECT_TRUE(contact_stopped) << "snow/soil contact continued after the snow cover disappeared";
    EXPECT_TRUE(rain_on_snow)
        << "the forcing did not exercise rain through an active UEB snow cover";
    EXPECT_TRUE(wet_snowless || external_resumed)
        << "neither ponded-water ownership nor a dry external boundary followed melt-out";
    ASSERT_EQ(swmm_engine_end(handle), SWMM_OK);
    swmm_engine_close(handle);
    swmm_engine_destroy(handle);
}
#endif

TEST(SnowEngineIntegration, SessionCheckpointResumesNativeSnowAndRoutingExactly) {
    for (const std::string model : {"DEGREE_DAY", "UEB"})
        for (int output : {0, 1}) {
            const auto stem = "_snow_session_" + model + std::to_string(output);
            writeNativeSnowDeck((stem + ".inp").c_str(), model);
            {
                std::ofstream f(stem + ".inp", std::ios::app);
                f << "[REPORT]\nSUBCATCHMENTS ALL\nNODES ALL\nLINKS ALL\n"
                     "[SUBCATCHMENTS]\nBare RG J 1 50 100 .5 0\n[SUBAREAS]\nBare .01 .1 .02 .02 25 OUTLET\n[INFILTRATION]\nBare 3 .5 4 7 0\n";
            }
            std::remove((stem + "a.out").c_str());
            std::remove((stem + "b.out").c_str());
            SWMMEngine a, b;
            ASSERT_EQ(a.open((stem + ".inp").c_str(), (stem + "a.rpt").c_str(),
                             (stem + "a.out").c_str(), nullptr),
                      0);
            ASSERT_EQ(b.open((stem + ".inp").c_str(), (stem + "b.rpt").c_str(),
                             (stem + "b.out").c_str(), nullptr),
                      0);
            ASSERT_EQ(a.initialize(), 0);
            ASSERT_EQ(b.initialize(), 0);
            if (output) {
                ASSERT_EQ(a.prepareOutputCheckpointing(), "");
                ASSERT_EQ(b.prepareOutputCheckpointing(), "");
            }
            ASSERT_EQ(a.start(output), 0);
            ASSERT_EQ(b.start(output), 0);
            ASSERT_EQ(a.enableCheckpointing(), "");
            ASSERT_EQ(b.enableCheckpointing(), "");
            double ta = 0, tb = 0;
            for (int k = 0; k < 11; ++k)
                ASSERT_EQ(a.step(&ta), 0);
            const auto before = surface::saveSnowCheckpoint(a.context(), a.snowSolver().state());
            ASSERT_EQ(a.saveCheckpoint(stem + ".chk"), "");
            ASSERT_EQ(b.loadCheckpoint(stem + ".chk"), "");
            EXPECT_EQ(surface::saveSnowCheckpoint(b.context(), b.snowSolver().state()), before);
            int count = 0;
            do {
                ASSERT_EQ(a.step(&ta), 0);
                ASSERT_EQ(b.step(&tb), 0);
                ASSERT_LT(++count, 1000);
                EXPECT_EQ(ta, tb);
                EXPECT_EQ(surface::saveSnowCheckpoint(a.context(), a.snowSolver().state()),
                          surface::saveSnowCheckpoint(b.context(), b.snowSolver().state()));
                EXPECT_EQ(a.context().nodes.depth, b.context().nodes.depth);
                EXPECT_EQ(a.context().subcatches.runoff, b.context().subcatches.runoff);
            } while (ta > 0);
            EXPECT_EQ(a.end(), 0);
            EXPECT_EQ(b.end(), 0);
            if (output) {
                std::ifstream fa(stem + "a.out", std::ios::binary),
                    fb(stem + "b.out", std::ios::binary);
                EXPECT_EQ(std::string(std::istreambuf_iterator<char>(fa), {}),
                          std::string(std::istreambuf_iterator<char>(fb), {}));
            }
        }
}

TEST(MeshSnow, ClimateOverrideClosesOldPredictionAndResetForcingDoesNotPredictFuture) {
    SimulationContext ctx;
    ctx.options.start_date = datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 2. / 24;
    ctx.snowpack_names.add("SP");
    ctx.snowpacks.names = {"SP"};
    ctx.snowpacks.config.model = surface::SnowKind::Ueb;
    ctx.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
    ctx.snowpacks.pervious = {std::array<double, 7>{.02, .06, 32, .1, 6, 0, 0}};
    ctx.snowpacks.mesh_config.defaults["*"].pack = "SP";
    std::istringstream met("01/01/2026 00:00 -5 70 1 0 200\n01/01/2026 01:00 -5 70 1 0 200\n");
    ASSERT_TRUE(ctx.snowpacks.meteorology.read(met).empty());
    surface::MeshSnow snow;
    ASSERT_TRUE(snow.initialize(ctx, {{"", 50, 0, 0}}).empty());
    ASSERT_TRUE(snow.advance(ctx, 0, 300, {0}, {1}).empty());
    ASSERT_TRUE(snow.columns()[0].pending());
    const auto accepted = snow.columns()[0].accepted;
    ctx.forcing.climate_temperature_mode = ForcingMode::OVERRIDE;
    ctx.forcing.climate_temperature_value = 70;
    ctx.forcing.climate_temperature_persist = ForcingPersist::RESET;
    auto expected = accepted;
    ueb::Forcing forcing;
    ueb::Site site;
    ueb::LumpedFluxes flux;
    const std::array<double, 10> cover{1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    ASSERT_TRUE(climate::resolveUebForcing(ctx, ctx.options.start_date + 300. / 86400, 300, 0, 0,
                                           forcing, site.pressure)
                    .empty());
    forcing.precipitation = 0;
    forcing.snow_catch = 1;
    ASSERT_TRUE(
        ueb::advanceLumped(expected, ctx.snowpacks.config.parameters, site, forcing, cover, flux)
            .empty());
    ASSERT_TRUE(snow.advance(ctx, 300, 600, {0}, {1}).empty());
    EXPECT_FALSE(snow.columns()[0].pending());
    EXPECT_EQ(snow.columns()[0].accepted.point.energy, expected.point.energy);
    EXPECT_EQ(snow.columns()[0].accepted.water, expected.water);
    ctx.forcing.climate_temperature_mode = ForcingMode::NONE;
    ASSERT_TRUE(snow.advance(ctx, 600, 900, {0}, {1}).empty());
    EXPECT_TRUE(snow.columns()[0].pending());
    EXPECT_NEAR(snow.ledgers()[0].residual(), 0, 1e-10);
}

TEST(SnowSourceOwnership, CompletedSnowAndLiquidStoresResumeExactlyAndDoNotBorrowFutureWater) {
    for (const std::string model : {"DEGREE_DAY", "UEB"}) {
        const auto stem = "_snow_source_" + model;
        writeNativeSnowDeck((stem + ".inp").c_str(), model);
        SWMMEngine engine;
        ASSERT_EQ(engine.open((stem + ".inp").c_str(), (stem + ".rpt").c_str(), nullptr), 0);
        ASSERT_EQ(engine.initialize(), 0);
        runoff::SourceWaterDriver source, resumed;
        ASSERT_EQ(source.initialize(engine.context(), {0}, {}), "");
        ASSERT_EQ(resumed.initialize(engine.context(), {0}, {}), "");
        ASSERT_TRUE(source.snow().active());
        const double initial = source.snow().ledgers()[0].initial;
        ASSERT_GT(initial, 0);
        ASSERT_EQ(source.stage(0, 317, 317), "");
        ASSERT_EQ(source.commit(), "");
        EXPECT_EQ(source.snow().ledgers()[0].end, 317);
        EXPECT_GT(source.snow().ledgers()[0].liquid, 0);
        EXPECT_GT(source.snow().ledgers()[0].stored, 0);
        EXPECT_NEAR(source.snow().ledgers()[0].residual(), 0, 1e-8);
        EXPECT_NEAR(source.balanceResidual(0), 0, 1e-8);
        detail::CompletedSourceArchive saved;
        source.checkpoint(saved, false);
        detail::CompletedSourceArchive restored(saved.bytes());
        resumed.checkpoint(restored, false);
        restored.finish();
        for (double end : {719., 1800., 3600., 7200.}) {
            ASSERT_EQ(source.stage(0, end, end), "");
            ASSERT_EQ(source.commit(), "");
            ASSERT_EQ(resumed.stage(0, end, end), "");
            ASSERT_EQ(resumed.commit(), "");
            detail::CompletedSourceArchive a, b;
            source.checkpoint(a, false);
            resumed.checkpoint(b, false);
            EXPECT_EQ(a.bytes(), b.bytes());
            EXPECT_NEAR(source.snow().ledgers()[0].residual(), 0, 1e-8);
            EXPECT_NEAR(source.balanceResidual(0), 0, 1e-8);
        }
        const auto& b = source.snow().ledgers()[0];
        EXPECT_NEAR(b.initial, b.stored + b.liquid + b.sublimation + b.removed, 1e-8);
        EXPECT_GT(b.liquid, 0);
        EXPECT_LT(b.stored, initial);
        EXPECT_GT(engine.snowSolver().state().wsnow[1],
                  0); // private owners retain their own accepted inventory
    }
}

#include "openswmm/engine/openswmm_output.h"
TEST(SnowEngineIntegration,
     NativeHistoryCarriesSiProcessFieldsAndAcceptedClockWithoutChangingLegacyBulk) {
    for (const std::string model : {"UEB", "DEGREE_DAY"}) {
        const auto stem = "_snow_history_" + model;
        writeNativeSnowDeck((stem + ".inp").c_str(), model);
        {
            std::ofstream f(stem + ".inp", std::ios::app);
            f << "[REPORT]\nSUBCATCHMENTS ALL\nNODES ALL\nLINKS ALL\n";
        }
        auto h = swmm_engine_create();
        ASSERT_NE(h, nullptr);
        ASSERT_EQ(swmm_engine_open(h, (stem + ".inp").c_str(), (stem + ".rpt").c_str(),
                                   (stem + ".out").c_str(), nullptr),
                  SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(h), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(h, 1), SWMM_OK);
        double elapsed = 0;
        do {
            ASSERT_EQ(swmm_engine_step(h, &elapsed), SWMM_OK) << swmm_get_last_error_msg(h);
        } while (elapsed > 0);
        ASSERT_EQ(swmm_engine_end(h), SWMM_OK);
        auto& e = *static_cast<SWMMEngine*>(h);
        const auto expected = surface::nativeSnowResults(e.context(), e.snowSolver().state(), 0);
        auto out = swmm_output_open((stem + ".out").c_str());
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(swmm_output_has_subcatch_variable(out, SWMM_OUT_SUBCATCH_SNOW_SWE),
                  model == "UEB");
        const int count = swmm_output_get_period_count(out);
        ASSERT_GT(count, 0);
        float legacy[9];
        std::fill(std::begin(legacy), std::end(legacy), 12345.f);
        int width = 0;
        ASSERT_EQ(swmm_output_get_subcatch_attribute(out, 0, count - 1, legacy, &width), 0);
        EXPECT_EQ(width, 8);
        EXPECT_EQ(legacy[8], 12345.f); // original caller's allocation remains safe
        for (int field = 0; field < 10; ++field) {
            float value = 12345;
            const int code = swmm_output_get_subcatch_series(
                out, 0, SWMM_OUT_SUBCATCH_SNOW_SWE + field, count - 1, count - 1, &value);
            if (model == "UEB") {
                ASSERT_EQ(code, 0);
                EXPECT_FLOAT_EQ(value, float(expected[field])) << field;
            } else {
                EXPECT_NE(code, 0);
                EXPECT_EQ(value, 12345);
            }
        }
        swmm_output_close(out);
        swmm_engine_destroy(h);
    }
}

#include "core/CompletedSourceArchive.hpp"
#include "core/SimulationContext.hpp"
#include "core/DateTime.hpp"
#include "hydrology/surface/MeshSnow.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <type_traits>

namespace snow_trial_test {
struct Fixture {
    openswmm::SimulationContext context;
    openswmm::surface::MeshSnow snow;
    std::vector<double> rain, catch_factor;
    int mode, count;

    Fixture(int model, int cells) : mode(model), count(cells) {
        using namespace openswmm;
        context.options.ignore_snow_melt = false;
        context.options.start_date = datetime::encodeDate(2026, 1, 1);
        context.options.end_date = context.options.start_date + 14;
        context.climate_state.temperature = 22;
        context.climate_state.temperature_src = 22;
        context.climate_state.snow_divt = 32;
        context.climate_state.humidity = 75;
        for (int month = 0; month < 12; ++month) {
            context.options.humidity[month] = 75;
            context.options.wind_speed[month] = 2;
        }
        context.heat_config.cloud.configured = true;
        context.heat_config.cloud.fraction = .5;
        context.snowpack_names.add("DD");
        context.snowpack_names.add("UEB");
        context.snowpacks.names = {"DD", "UEB"};
        context.snowpacks.pervious.assign(2, std::array<double, 7>{.02, .06, 32, .1, 6, .6, 0});
        context.snowpacks.config.model = surface::SnowKind::DegreeDay;
        context.snowpacks.config.packs[1].model = surface::SnowKind::Ueb;
        context.snowpacks.config.site = surface::ClimateSite{45, -120, -8, 1000};
        context.snowpacks.mesh_config.step = 1800;
        context.snowpacks.mesh_config.defaults["*"].pack = model == 1 ? "UEB" : "DD";
        std::vector<surface::MeshSnowGeometry> geometry;
        for (int i = 0; i < cells; ++i) {
            geometry.push_back({"snow_trial_vegetation_region_" + std::to_string(i % 5),
                                50. + 7 * (i % 13), double(i % 4) * 3, double(i % 6) * 45});
            if (model == 2)
                context.snowpacks.mesh_config.cells[i].pack = i % 3 == 1 ? "UEB" :
                                                               i % 3 == 2 ? "NONE" : "DD";
        }
        auto error = snow.initialize(context, geometry);
        if (!error.empty())
            throw std::runtime_error(error);
        std::vector<unsigned char> contact(cells, 0);
        for (int i = 0; i < cells; ++i)
            if (snow.owns(i) && snow.kind(i) == surface::SnowKind::Ueb && i % 2)
                contact[i] = 1;
        error = snow.setGroundContactMask(contact);
        if (!error.empty())
            throw std::runtime_error(error);
        rain.assign(cells, 1e-7);
        catch_factor.assign(cells, 1.2);
    }

    int lastOwned() const {
        for (int i = count - 1; i >= 0; --i)
            if (snow.owns(i))
                return i;
        return -1;
    }
    std::string step(double seconds = 30) {
        const double start = snow.completedEnd();
        return snow.advance(context, start, start + seconds, rain, catch_factor);
    }
    std::string configurationBytes() const {
        openswmm::detail::CompletedSourceArchive a;
        auto identity = snow.configurationIdentity();
        auto geometry = snow.geometry();
        auto curve = snow.depletionCurve();
        auto mask = snow.groundContactMask();
        bool active = snow.active(), has_ueb = snow.hasUeb();
        a(identity, curve, mask, active, has_ueb);
        a.sequence(geometry, [&](auto& g) { a(g.tag, g.area, g.slope, g.aspect); });
        for (int i = 0; i < count; ++i) {
            auto kind = snow.kind(i);
            bool owned = snow.owns(i);
            a(kind, owned);
        }
        return a.bytes();
    }
};

inline void write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), std::streamsize(bytes.size()));
    output.close();
    if (!output.good())
        throw std::runtime_error("Cannot write snow trial evidence.");
}
static_assert(std::is_nothrow_move_assignable_v<openswmm::surface::MeshSnow>);
static_assert(std::is_nothrow_move_assignable_v<openswmm::snow::DegreeDaySnow>);
}


TEST(MeshSnow, RuntimeTrialPreservesConfigurationAndCompleteRefusalRecoveryHistory) {
    using namespace openswmm;
    detail::CompletedSourceArchive history;
    std::string magic = "Mesh snow runtime trial history v1";
    history(magic);
    for (int mode : {0, 1, 2})
        for (int count : {1, 8, 0, 2, 33, 1, 8, 1}) {
            snow_trial_test::Fixture f(mode, count);
            const auto immutable = f.configurationBytes();
            std::string anchor;
            for (int stage = 0; stage < 12; ++stage) {
                SCOPED_TRACE(std::to_string(mode) + ":" + std::to_string(count) + ":" +
                             std::to_string(stage));
                auto before = f.snow.saveCheckpoint();
                auto metadata_before = f.configurationBytes();
                auto rain = f.rain, catch_factor = f.catch_factor;
                auto start = f.snow.completedEnd();
                std::string error;
                const int last = f.lastOwned();
                if (stage == 1)
                    error = f.step(300);
                else if (stage == 2)
                    error = f.snow.advance(f.context, start + 1, start + 300, rain, catch_factor);
                else if (stage == 3) {
                    if (last >= 0)
                        rain[last] = -1;
                    error = f.snow.advance(f.context, start, start + 300, rain, catch_factor);
                } else if (stage == 4) {
                    error = f.step(300);
                    anchor = f.snow.saveCheckpoint();
                } else if (stage == 5) {
                    if (last >= 0)
                        catch_factor[last] = std::numeric_limits<double>::quiet_NaN();
                    error = f.snow.advance(f.context, start, start + 300, rain, catch_factor);
                } else if (stage == 6) {
                    f.context.forcing.climate_temperature_mode = ForcingMode::OVERRIDE;
                    f.context.forcing.climate_temperature_value = 35;
                    f.context.forcing.climate_temperature_persist = ForcingPersist::RESET;
                    f.rain.assign(count, 2e-7);
                    f.catch_factor.assign(count, 1.1);
                    error = f.step(300);
                } else if (stage == 7) {
                    f.context.forcing.climate_temperature_mode = ForcingMode::NONE;
                    error = f.step(300);
                } else if (stage == 8) {
                    error = f.snow.advance(f.context, start,
                                          std::nextafter(start, std::numeric_limits<double>::infinity()),
                                          rain, catch_factor);
                } else if (stage == 9)
                    error = f.snow.restoreCheckpoint(before.empty() ? "invalid" :
                                                     before.substr(0, before.size() - 1));
                else if (stage == 10)
                    error = f.snow.restoreCheckpoint(anchor);
                else if (stage == 11) {
                    error = f.snow.restoreCheckpoint(anchor, true);
                    if (error.empty())
                        error = f.step(300);
                }
                auto after = f.snow.saveCheckpoint();
                bool refusal = stage == 9 || (last >= 0 && (stage == 2 || stage == 3 || stage == 5));
                if (refusal) {
                    EXPECT_FALSE(error.empty());
                    EXPECT_EQ(after, before);
                } else {
                    EXPECT_TRUE(error.empty()) << error;
                    if (stage == 0)
                        EXPECT_EQ(after, before);
                    if (stage == 10)
                        EXPECT_EQ(after, anchor);
                }
                EXPECT_EQ(f.configurationBytes(), immutable);
                for (const auto& ledger : f.snow.ledgers())
                    EXPECT_NEAR(ledger.residual(), 0, 1e-9);
                auto metadata_after = f.configurationBytes();
                history(mode, count, stage, refusal, start, error, metadata_before, before,
                        metadata_after, after);
            }
        }
    if (const char* output = std::getenv("OPENSWMM_SNOW_TRIAL_TEST_OUT")) {
        std::filesystem::create_directories(output);
        snow_trial_test::write(std::filesystem::path(output) / "runtime-trial.bin",
                               history.bytes());
    }
}

#include "SnowHeldFixture.hpp"
#include <future>
namespace {
void saveHeldRegression(const char* name,const std::string& bytes) {
    if (const char* directory=std::getenv("OPENSWMM_SNOW_HELD_TEST_OUT")) {
        std::filesystem::create_directories(directory);
        snow_held_test::write(std::filesystem::path(directory)/name,bytes);
    }
}
}
TEST(MeshSnow, PreparedUebConsumptionMatchesAllScalarGuardsAndTinyTail) {
    openswmm::detail::CompletedSourceArchive a;
    for(int kind:{0,1,2,3,4,5,8,9}) {
        snow_held_test::Case serial(512,1,0,kind),parallel(512,4,0,kind);
        auto expected=serial.history(),actual=parallel.history();
        ASSERT_EQ(expected,actual)<<kind;
        a(kind,actual);
    }
    saveHeldRegression("held-guards.bin",a.bytes());
}
TEST(MeshSnow, PreparedUebRefusalRetainsOwnerAndChoosesFirstCell) {
    openswmm::detail::CompletedSourceArchive a;
    for(int kind:{6,7}) {
        snow_held_test::Case serial(513,1,0,kind),parallel(513,4,0,kind);
        auto expected=serial.history(),actual=parallel.history();
        ASSERT_EQ(expected,actual)<<kind;
        a(kind,actual);
    }
    saveHeldRegression("held-refusals.bin",a.bytes());
}
TEST(MeshSnow, PreparedUebOwnersAdvanceIndependentlyOnHostThreads) {
    auto run=[](int threads) {
        snow_held_test::Case f(512,threads,0,0);
        return f.history();
    };
    auto expected=run(1);
    auto first=std::async(std::launch::async,run,4);
    auto second=std::async(std::launch::async,run,4);
    auto a=first.get(),b=second.get();
    ASSERT_EQ(expected,a);ASSERT_EQ(expected,b);
    saveHeldRegression("held-independent.bin",a);
}


TEST(SnowSourceOwnership, SnapshotsStayIndependentAcrossAdvanceAndCheckpointReads) {
    for (const std::string model : {"DEGREE_DAY", "UEB"}) {
        SCOPED_TRACE(model);
        const auto stem = "_source_snow_snapshots_" + model;
        writeNativeSnowDeck((stem + ".inp").c_str(), model);
        SWMMEngine engine;
        ASSERT_EQ(engine.open((stem + ".inp").c_str(), (stem + ".rpt").c_str(), nullptr), 0);
        ASSERT_EQ(engine.initialize(), 0);
        runoff::SourceWaterDriver original;
        ASSERT_EQ(original.initialize(engine.context(), {0}, {}), "");
        ASSERT_TRUE(original.snow().active());
        using Snapshot = std::pair<std::string, std::string>;
        std::vector<std::array<std::string, 3>> history;
        const auto capture = [&](const char* tag, runoff::SourceWaterDriver& source) -> Snapshot {
            detail::CompletedSourceArchive identity, runtime;
            source.checkpoint(identity, true);
            source.checkpoint(runtime, false);
            history.push_back({tag, identity.bytes(), runtime.bytes()});
            return {identity.bytes(), runtime.bytes()};
        };
        const auto before = capture("original", original);
        auto first = original.waterTrial();
        auto second = original.waterTrial();
        EXPECT_EQ(capture("first_initial", first), before);
        EXPECT_EQ(capture("second_initial", second), before);
        const surface::SourceSnow* borrowed_trial = nullptr;
        runoff::SourceWaterDriver::ForcingTransform observe = [&](auto&) {
            if (!borrowed_trial)
                borrowed_trial = &first.snow(true);
        };
        ASSERT_EQ(first.stage(0, 317, 317, nullptr, nullptr, &observe), "");
        ASSERT_NE(borrowed_trial, nullptr);
        ASSERT_EQ(borrowed_trial->ledgers().size(), 1u);
        EXPECT_EQ(borrowed_trial->ledgers()[0].end, 317);
        EXPECT_EQ(borrowed_trial->ledgers()[0].liquid, first.snow(true).ledgers()[0].liquid);
        EXPECT_EQ(borrowed_trial->ledgers()[0].stored, first.snow(true).ledgers()[0].stored);
        EXPECT_EQ(borrowed_trial->state().wsnow, first.snow(true).state().wsnow);
        EXPECT_EQ(borrowed_trial->state().fw, first.snow(true).state().fw);
        EXPECT_GT(borrowed_trial->ledgers()[0].liquid, 0);
        EXPECT_EQ(capture("original_during_trial", original), before);
        EXPECT_EQ(capture("second_during_trial", second), before);
        first.cancel();
        EXPECT_EQ(capture("first_cancelled", first), before);
        ASSERT_EQ(first.stage(0, 317, 317), "");
        ASSERT_EQ(first.commit(), "");
        const auto after = capture("first_committed", first);
        EXPECT_NE(after.second, before.second);
        EXPECT_EQ(capture("original_after_commit", original), before);
        EXPECT_EQ(capture("second_before_read", second), before);
        detail::CompletedSourceArchive model_read(after.first);
        second.checkpoint(model_read, true);
        model_read.finish();
        EXPECT_EQ(capture("second_model_restored", second), before);
        detail::CompletedSourceArchive restored(after.second);
        second.checkpoint(restored, false);
        restored.finish();
        EXPECT_EQ(capture("second_restored", second), after);
        EXPECT_EQ(capture("original_after_read", original), before);
        auto refused = original.waterTrial();
        ASSERT_GT(after.second.size(), 1u);
        detail::CompletedSourceArchive truncated(after.second.substr(0, after.second.size() - 1));
        EXPECT_THROW(refused.checkpoint(truncated, false), std::runtime_error);
        EXPECT_EQ(capture("original_after_refusal", original), before);
        EXPECT_EQ(capture("first_after_refusal", first), after);
        EXPECT_EQ(capture("second_after_refusal", second), after);
        detail::CompletedSourceArchive retry(after.second);
        refused.checkpoint(retry, false);
        retry.finish();
        EXPECT_EQ(capture("refused_retry", refused), after);
        if (const char* output = std::getenv("OPENSWMM_SOURCE_SNOW_HISTORY")) {
            detail::CompletedSourceArchive archive;
            std::string magic = "C1 source snow fork history v1", saved_model = model;
            archive(magic, saved_model, history);
            std::filesystem::create_directories(output);
            std::ofstream out(std::filesystem::path(output) / (model + ".bin"), std::ios::binary);
            out.write(archive.bytes().data(), archive.bytes().size());
            ASSERT_TRUE(out.good());
        }
    }
}

#include <bit>
#include <cstdint>
TEST(SnowDegreeDayMelt, ThinNativePacksPreserveMasksThresholdAndOptionalAge) {
    for (int count : {1,33})
        for (bool track_age : {false,true}) {
            DegreeDaySnow solver;
            solver.init(count);
            auto& s=solver.state();
            s.track_age=track_age;
            const double threshold=.001/12.;
            for (int j=0;j<count;++j) {
                const auto first=std::size_t(3*j);
                s.wsnow[first]=-0.;s.fw[first]=-0.;s.imelt[first]=-0.;
                s.wsnow[first+1]=threshold;
                s.fw[first+2]=.00004;
                for(int k=0;k<3;++k) {
                    s.coldc[first+k]=.125;
                    s.asc[first+k]=.75;
                    s.age[first+k]=17.+k;
                    s.out_age[first+k]=99.;
                }
            }
            if(count>1) {
                s.enabled[1]=0;
                s.external[2]=true;
                s.external.resize(3);
                s.wsnow[9]=std::nextafter(threshold,std::numeric_limits<double>::infinity());
            }
            if(!track_age) {s.age.clear();s.out_age.clear();}
            const auto before=s;
            SimulationContext context;
            std::vector<double> zero(count,0.);
            solver.execute(context,8.,45.,0.,zero.data(),zero.data());
            for(int j=0;j<count;++j) {
                const bool masked=j==1||j==2;
                for(int k=0;k<3;++k) {
                    const auto i=std::size_t(3*j+k);
                    if(masked) {
                        EXPECT_EQ(std::bit_cast<uint64_t>(s.wsnow[i]),std::bit_cast<uint64_t>(before.wsnow[i]));
                        EXPECT_EQ(std::bit_cast<uint64_t>(s.fw[i]),std::bit_cast<uint64_t>(before.fw[i]));
                        EXPECT_DOUBLE_EQ(s.coldc[i],before.coldc[i]);
                        EXPECT_DOUBLE_EQ(s.asc[i],before.asc[i]);
                        if(track_age) {EXPECT_DOUBLE_EQ(s.age[i],before.age[i]);EXPECT_DOUBLE_EQ(s.out_age[i],before.out_age[i]);}
                        continue;
                    }
                    if(j==3&&k==0) {
                        EXPECT_EQ(std::bit_cast<uint64_t>(s.wsnow[i]),std::bit_cast<uint64_t>(before.wsnow[i]));
                        continue;
                    }
                    EXPECT_DOUBLE_EQ(s.wsnow[i],0.);
                    EXPECT_DOUBLE_EQ(s.fw[i],0.);
                    EXPECT_DOUBLE_EQ(s.coldc[i],0.);
                    EXPECT_DOUBLE_EQ(s.asc[i],0.);
                    EXPECT_NEAR(8.*s.imelt[i],before.wsnow[i]+before.fw[i],1.e-20);
                    if(k==0) EXPECT_EQ(std::bit_cast<uint64_t>(s.imelt[i]),uint64_t{0});
                    if(track_age) {
                        EXPECT_DOUBLE_EQ(s.age[i],0.);
                        EXPECT_DOUBLE_EQ(s.out_age[i],k==0?0.:before.age[i]);
                    }
                }
            }
            if(!track_age) {EXPECT_TRUE(s.age.empty());EXPECT_TRUE(s.out_age.empty());}
        }
}
