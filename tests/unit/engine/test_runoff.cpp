/**
 * @file test_runoff.cpp
 * @brief Unit tests for subcatchment runoff — ponded depth ODE, alpha, runoff rate.
 *
 * @details Tests the nonlinear reservoir model:
 *   - RunoffSoA::computeAlpha() — Manning's alpha coefficient
 *   - RunoffSolver::updatePondedDepth() — RK45 ODE integration of dd/dt
 *   - RunoffSolver::getRunoffRate() — Manning's outflow from ponded depth
 *   - Mass balance: inflow volume ≈ outflow + storage change + losses
 *
 * @see src/engine/hydrology/Runoff.hpp
 * @ingroup engine_hydrology
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "hydrology/Runoff.hpp"
#include "core/SimulationOptions.hpp"

using namespace openswmm;
using namespace openswmm::runoff;

// ============================================================================
// RunoffSoA::computeAlpha
// ============================================================================

TEST(RunoffAlpha, BasicComputation) {
    // Alpha = PHI * width * sqrt(slope) / (N * area)
    // PHI = 1.486 (US customary Manning constant)
    RunoffSoA soa;
    soa.resize(1);
    soa.area[0]       = 43560.0;   // 1 acre = 43560 ft²
    soa.width[0]      = 200.0;     // ft
    soa.slope[0]      = 0.01;      // 1% slope
    soa.imperv_pct[0] = 0.5;       // 50% impervious
    soa.n_imperv[0]   = 0.01;      // Manning's n for impervious
    soa.n_perv[0]     = 0.10;      // Manning's n for pervious

    soa.computeAlpha();

    double sq_slope = std::sqrt(0.01);  // 0.1
    double area_imperv = 43560.0 * 0.5;
    double area_perv   = 43560.0 * 0.5;

    double expected_alpha_i = PHI * 200.0 * sq_slope / (0.01 * area_imperv);
    double expected_alpha_p = PHI * 200.0 * sq_slope / (0.10 * area_perv);

    EXPECT_NEAR(soa.alpha_imperv[0], expected_alpha_i, 1e-10);
    EXPECT_NEAR(soa.alpha_perv[0],   expected_alpha_p, 1e-10);
    // Impervious alpha should be 10x pervious alpha (N ratio)
    EXPECT_NEAR(soa.alpha_imperv[0] / soa.alpha_perv[0], 10.0, 1e-10);
}

TEST(RunoffAlpha, ZeroAreaGivesZeroAlpha) {
    RunoffSoA soa;
    soa.resize(1);
    soa.area[0]  = 0.0;
    soa.width[0] = 200.0;
    soa.slope[0] = 0.01;
    soa.imperv_pct[0] = 0.5;
    soa.n_imperv[0]   = 0.01;
    soa.n_perv[0]     = 0.10;

    soa.computeAlpha();

    EXPECT_DOUBLE_EQ(soa.alpha_imperv[0], 0.0);
    EXPECT_DOUBLE_EQ(soa.alpha_perv[0],   0.0);
}

TEST(RunoffAlpha, ZeroManningsNGivesZeroAlpha) {
    RunoffSoA soa;
    soa.resize(1);
    soa.area[0]  = 43560.0;
    soa.width[0] = 200.0;
    soa.slope[0] = 0.01;
    soa.imperv_pct[0] = 0.5;
    soa.n_imperv[0]   = 0.0;
    soa.n_perv[0]     = 0.0;

    soa.computeAlpha();

    EXPECT_DOUBLE_EQ(soa.alpha_imperv[0], 0.0);
    EXPECT_DOUBLE_EQ(soa.alpha_perv[0],   0.0);
}

TEST(RunoffAlpha, FullyImperviousNoPerviousAlpha) {
    RunoffSoA soa;
    soa.resize(1);
    soa.area[0]  = 43560.0;
    soa.width[0] = 200.0;
    soa.slope[0] = 0.01;
    soa.imperv_pct[0] = 1.0;  // fully impervious
    soa.n_imperv[0]   = 0.01;
    soa.n_perv[0]     = 0.10;

    soa.computeAlpha();

    EXPECT_GT(soa.alpha_imperv[0], 0.0);
    EXPECT_DOUBLE_EQ(soa.alpha_perv[0], 0.0);  // no pervious area
}

TEST(RunoffAlpha, MultipleSubcatchments) {
    RunoffSoA soa;
    soa.resize(3);
    for (int i = 0; i < 3; ++i) {
        auto ui = static_cast<std::size_t>(i);
        soa.area[ui]       = 43560.0 * (1 + i);  // 1, 2, 3 acres
        soa.width[ui]      = 200.0;
        soa.slope[ui]      = 0.01;
        soa.imperv_pct[ui] = 0.5;
        soa.n_imperv[ui]   = 0.01;
        soa.n_perv[ui]     = 0.10;
    }

    soa.computeAlpha();

    // Alpha inversely proportional to area (all else equal)
    EXPECT_NEAR(soa.alpha_imperv[0] / soa.alpha_imperv[1], 2.0, 1e-10);
    EXPECT_NEAR(soa.alpha_imperv[0] / soa.alpha_imperv[2], 3.0, 1e-10);
}

// ============================================================================
// Ponded depth update via ODE solver (replicating internal logic)
// ============================================================================

#include "math/OdeSolver.hpp"

// Replicate the ponded depth update logic from RunoffSolver (private static)
// to test numerically without accessing private members directly.
static void testUpdatePondedDepth(double& depth, double inflow,
                                   double alpha, double dStore, double dt) {
    double tx = dt;
    if (depth + inflow * tx <= dStore) {
        depth += inflow * tx;
    } else {
        double dx = dStore - depth;
        if (dx > 0.0 && inflow > 0.0) {
            tx -= dx / inflow;
            depth = dStore;
        }
        if (alpha > 0.0 && tx > 0.0) {
            double captured_inflow = inflow;
            double captured_alpha  = alpha;
            double captured_dStore = dStore;
            openswmm::ode::integrate(&depth, 1, 0.0, tx, ODETOL, tx,
                [captured_inflow, captured_alpha, captured_dStore]
                (double, const double* d, double* dddt) {
                    double rx = *d - captured_dStore;
                    double outflow = (rx > 0.0)
                        ? captured_alpha * std::pow(rx, MEXP) : 0.0;
                    *dddt = captured_inflow - outflow;
                });
        } else {
            if (tx < 0.0) tx = 0.0;
            depth += inflow * tx;
        }
    }
    if (depth < 0.0) depth = 0.0;
}

// Replicate the runoff rate calculation
static double testGetRunoffRate(double depth, double dStore, double alpha) {
    double excess = depth - dStore;
    if (excess > 0.0 && alpha > 0.0)
        return alpha * std::pow(excess, MEXP);
    return 0.0;
}

TEST(PondedDepth, FillsDepressionStorageFirst) {
    double depth = 0.0;
    double inflow = 1e-5;
    double dStore = 0.01;
    double alpha = 1.0;
    double dt = 60.0;

    testUpdatePondedDepth(depth, inflow, alpha, dStore, dt);

    EXPECT_NEAR(depth, inflow * dt, 1e-10);
    EXPECT_LT(depth, dStore);
}

TEST(PondedDepth, OverflowsAfterDepressionStorage) {
    double depth = 0.0;
    double inflow = 1e-3;
    double dStore = 0.001;
    double alpha = 10.0;
    double dt = 300.0;

    testUpdatePondedDepth(depth, inflow, alpha, dStore, dt);

    EXPECT_GT(depth, dStore);
}

TEST(PondedDepth, NoInflowNoChange) {
    double depth = 0.005;
    double inflow = 0.0;
    double dStore = 0.01;
    double alpha = 1.0;
    double dt = 300.0;

    testUpdatePondedDepth(depth, inflow, alpha, dStore, dt);

    EXPECT_NEAR(depth, 0.005, 1e-10);
}

TEST(PondedDepth, SteadyStateBalance) {
    double inflow = 1e-4;
    double dStore = 0.001;
    double alpha = 5.0;
    double depth = dStore;

    for (int i = 0; i < 1000; ++i) {
        testUpdatePondedDepth(depth, inflow, alpha, dStore, 10.0);
    }

    double excess = std::pow(inflow / alpha, 3.0 / 5.0);
    double expected_ss = dStore + excess;

    EXPECT_NEAR(depth, expected_ss, 0.01 * expected_ss);
}

TEST(PondedDepth, NeverGoesNegative) {
    double depth = 0.0;
    double inflow = 0.0;
    double dStore = 0.0;
    double alpha = 100.0;
    double dt = 1000.0;

    testUpdatePondedDepth(depth, inflow, alpha, dStore, dt);

    EXPECT_GE(depth, 0.0);
}

// ============================================================================
// Runoff rate from ponded depth
// ============================================================================

TEST(RunoffRate, ZeroExcessGivesZeroRunoff) {
    EXPECT_DOUBLE_EQ(testGetRunoffRate(0.005, 0.01, 1.0), 0.0);
    EXPECT_DOUBLE_EQ(testGetRunoffRate(0.01, 0.01, 1.0), 0.0);
}

TEST(RunoffRate, PositiveExcessGivesPositiveRunoff) {
    double depth = 0.02;
    double dStore = 0.01;
    double alpha = 5.0;

    double rate = testGetRunoffRate(depth, dStore, alpha);
    double expected = alpha * std::pow(0.01, MEXP);
    EXPECT_NEAR(rate, expected, 1e-12);
    EXPECT_GT(rate, 0.0);
}

TEST(RunoffRate, ManningsFormula) {
    double alpha = 2.0;
    double dStore = 0.005;

    for (double excess = 0.001; excess < 0.1; excess += 0.01) {
        double depth = dStore + excess;
        double rate = testGetRunoffRate(depth, dStore, alpha);
        double expected = alpha * std::pow(excess, MEXP);
        EXPECT_NEAR(rate, expected, 1e-12)
            << "Mismatch at excess = " << excess;
    }
}

TEST(RunoffRate, ZeroAlphaGivesZeroRunoff) {
    double rate = testGetRunoffRate(0.05, 0.01, 0.0);
    EXPECT_DOUBLE_EQ(rate, 0.0);
}

// ============================================================================
// RunoffSoA resize and initialization
// ============================================================================

TEST(RunoffSoA, ResizeSetsCorrectSize) {
    RunoffSoA soa;
    soa.resize(5);
    EXPECT_EQ(soa.n_subcatch, 5);
    EXPECT_EQ(soa.area.size(), 5u);
    EXPECT_EQ(soa.runoff.size(), 5u);
    EXPECT_EQ(soa.depth_imperv0.size(), 5u);
}

TEST(RunoffSoA, ResizeInitializesToZero) {
    RunoffSoA soa;
    soa.resize(3);
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(soa.depth_imperv0[i], 0.0);
        EXPECT_DOUBLE_EQ(soa.depth_perv[i], 0.0);
        EXPECT_DOUBLE_EQ(soa.runoff[i], 0.0);
    }
}

// ============================================================================
// Mass balance check — inflow ≈ outflow + storage change
// ============================================================================

TEST(RunoffMassBalance, PondedDepthConservation) {
    // Start with some depth, apply constant inflow, verify mass balance
    double depth_init = 0.005;
    double depth = depth_init;
    double inflow = 5e-5;   // ft/sec
    double dStore = 0.002;
    double alpha = 3.0;
    double dt = 60.0;

    double total_inflow = 0.0;
    double total_runoff = 0.0;

    for (int step = 0; step < 100; ++step) {
        total_inflow += inflow * dt;

        testUpdatePondedDepth(depth, inflow, alpha, dStore, dt);

        double rate = testGetRunoffRate(depth, dStore, alpha);
        total_runoff += rate * dt;
    }

    double storage_change = depth - depth_init;
    double balance_error = total_inflow - total_runoff - storage_change;

    // Mass balance should be approximately conserved (ODE solver tolerance)
    EXPECT_NEAR(balance_error, 0.0, 0.01 * total_inflow)
        << "Mass balance error: " << (balance_error / total_inflow * 100.0) << "%";
}
