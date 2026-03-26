/**
 * @file test_infiltration.cpp
 * @brief Unit tests for Horton, Green-Ampt, and SCS Curve Number infiltration.
 *
 * @details Verifies each infiltration model produces correct rates for
 *          known input scenarios. Cross-referenced against legacy infil.c.
 *
 * @see src/engine/hydrology/Infiltration.hpp
 * @ingroup engine_hydrology
 */

#include <gtest/gtest.h>
#include <cmath>

#include "hydrology/Infiltration.hpp"
#include "core/SimulationOptions.hpp"

using namespace openswmm;
using namespace openswmm::infil;

// Default options for unit tests (US customary units)
static SimulationOptions default_opts() {
    SimulationOptions opts;
    return opts;
}

// ============================================================================
// Horton
// ============================================================================

TEST(HortonInfil, InitSetsParameters) {
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.1, 0.0, default_opts());  // in/hr units
    // f0 = 3.0 in/hr → 3.0/12/3600 ft/sec
    EXPECT_NEAR(s.f0, 3.0 / 12.0 / 3600.0, 1e-12);
    EXPECT_NEAR(s.fmin, 0.5 / 12.0 / 3600.0, 1e-12);
}

TEST(HortonInfil, DryPeriodRecovers) {
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.5, 0.0, default_opts());
    s.tp = 1000.0;  // pretend we've been infiltrating

    double f = horton_getInfil(s, 0.0, 0.0, 300.0);  // dry period
    EXPECT_EQ(f, 0.0);
    EXPECT_LT(s.tp, 1000.0);  // tp should have recovered (decreased)
}

TEST(HortonInfil, WetPeriodInfiltrates) {
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.1, 0.0, default_opts());

    double precip = 5.0 / 12.0 / 3600.0;  // 5 in/hr → ft/sec (exceeds f0)
    double f = horton_getInfil(s, precip, 0.0, 300.0);

    EXPECT_GT(f, 0.0);
    EXPECT_LE(f, precip);  // can't infiltrate more than available water
    EXPECT_LE(f, s.f0);    // can't exceed max capacity
}

TEST(HortonInfil, CapacityLimitedReducesRate) {
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.1, 0.0, default_opts());

    double low_precip = 0.1 / 12.0 / 3600.0;  // 0.1 in/hr (below fmin)
    double f = horton_getInfil(s, low_precip, 0.0, 300.0);

    EXPECT_NEAR(f, low_precip, 1e-12);  // limited by available water
}

TEST(HortonInfil, DecayOverTime) {
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.1, 0.0, default_opts());

    double precip = 5.0 / 12.0 / 3600.0;
    double f1 = horton_getInfil(s, precip, 0.0, 300.0);
    double f2 = horton_getInfil(s, precip, 0.0, 300.0);
    double f3 = horton_getInfil(s, precip, 0.0, 300.0);

    // Infiltration should decrease over time (Horton decay)
    EXPECT_GE(f1, f2);
    EXPECT_GE(f2, f3);
}

// ============================================================================
// Green-Ampt
// ============================================================================

TEST(GreenAmptInfil, InitSetsParameters) {
    GreenAmptState s;
    grnampt_init(s, 4.0, 1.0, 0.3, default_opts());  // S=4in, Ks=1in/hr, IMD=0.3
    EXPECT_NEAR(s.S, 4.0 / 12.0, 1e-10);     // in → ft
    EXPECT_NEAR(s.Ks, 1.0 / 12.0 / 3600.0, 1e-12);
    EXPECT_NEAR(s.IMDmax, 0.3, 1e-10);
}

TEST(GreenAmptInfil, UnsaturatedPassesThrough) {
    GreenAmptState s;
    grnampt_init(s, 4.0, 1.0, 0.3, default_opts());

    double low_precip = 0.1 / 12.0 / 3600.0;  // less than Ks
    double f = grnampt_getInfil(s, low_precip, 0.0, 300.0);

    EXPECT_NEAR(f, low_precip, 1e-12);  // all rainfall infiltrates
    EXPECT_FALSE(s.saturated);
}

TEST(GreenAmptInfil, SaturationOccurs) {
    GreenAmptState s;
    grnampt_init(s, 4.0, 1.0, 0.3, default_opts());

    double high_precip = 5.0 / 12.0 / 3600.0;  // exceeds Ks
    // Multiple steps to reach saturation
    for (int i = 0; i < 100; ++i) {
        grnampt_getInfil(s, high_precip, 0.0, 60.0);
    }
    // Should eventually saturate
    EXPECT_TRUE(s.saturated);
}

TEST(GreenAmptInfil, DryPeriodRecoversMoisture) {
    GreenAmptState s;
    grnampt_init(s, 4.0, 1.0, 0.3, default_opts());
    s.saturated = true;
    s.F = 0.1;

    double f = grnampt_getInfil(s, 0.0, 0.0, 300.0);
    EXPECT_EQ(f, 0.0);
    EXPECT_FALSE(s.saturated);
}

// ============================================================================
// SCS Curve Number
// ============================================================================

TEST(CurveNumInfil, InitSetsSmax) {
    CurveNumState s;
    curvenum_init(s, 80.0, 0.01);
    // S = (1000/80 - 10)/12 = (12.5 - 10)/12 = 2.5/12 ft
    EXPECT_NEAR(s.Smax, 2.5 / 12.0, 1e-10);
}

TEST(CurveNumInfil, LowRainfallFullyInfiltrates) {
    CurveNumState s;
    curvenum_init(s, 80.0, 0.01);

    double low_precip = 0.01 / 12.0 / 3600.0;
    double f = curvenum_getInfil(s, low_precip, 0.0, 300.0);

    // Very small rainfall: P << S, so nearly all infiltrates
    EXPECT_GT(f, 0.0);
    EXPECT_LE(f, low_precip);
}

TEST(CurveNumInfil, HighCNLowInfiltration) {
    CurveNumState s;
    curvenum_init(s, 98.0, 0.01);  // CN=98 → very low S
    // S = (1000/98-10)/12 ≈ 0.017 ft

    double precip = 2.0 / 12.0 / 3600.0;  // 2 in/hr
    double f = curvenum_getInfil(s, precip, 0.0, 300.0);

    EXPECT_LT(f, precip);  // most runs off
}

TEST(CurveNumInfil, DryPeriodRecovery) {
    CurveNumState s;
    curvenum_init(s, 80.0, 0.01);
    s.S = 0.01;  // depleted retention
    s.T = 0.0;

    // Dry period long enough to trigger new event
    double f = curvenum_getInfil(s, 0.0, 0.0, 10000.0);
    EXPECT_EQ(f, 0.0);

    // S should have recovered
    EXPECT_GT(s.S, 0.01);
}

// ============================================================================
// ODE Solver basic test
// ============================================================================

#include "math/OdeSolver.hpp"

TEST(OdeSolver, ExponentialDecay) {
    // dy/dt = -y, y(0) = 1 → y(t) = exp(-t)
    double y = 1.0;
    auto derivs = [](double /*x*/, const double* y, double* dydx) {
        dydx[0] = -y[0];
    };

    int rc = openswmm::ode::integrate(&y, 1, 0.0, 1.0, 1e-6, 0.1, derivs);
    EXPECT_EQ(rc, 0);
    EXPECT_NEAR(y, std::exp(-1.0), 1e-5);
}

TEST(OdeSolver, LinearGrowth) {
    // dy/dt = 1, y(0) = 0 → y(t) = t
    double y = 0.0;
    auto derivs = [](double /*x*/, const double* /*y*/, double* dydx) {
        dydx[0] = 1.0;
    };

    int rc = openswmm::ode::integrate(&y, 1, 0.0, 5.0, 1e-6, 0.5, derivs);
    EXPECT_EQ(rc, 0);
    EXPECT_NEAR(y, 5.0, 1e-10);
}

// ============================================================================
// FindRoot basic test
// ============================================================================

#include "math/FindRoot.hpp"

TEST(FindRoot, NewtonSquareRoot) {
    // Find x such that x^2 - 2 = 0 → x = sqrt(2)
    double root = 1.5;
    auto func = [](double x, double* f, double* df) {
        *f = x * x - 2.0;
        *df = 2.0 * x;
    };
    int iters = openswmm::findroot::newton(0.1, 10.0, &root, 1e-10, func);
    EXPECT_GT(iters, 0);
    EXPECT_NEAR(root, std::sqrt(2.0), 1e-10);
}

TEST(FindRoot, RidderCubicRoot) {
    // Find x such that x^3 - 8 = 0 → x = 2
    auto func = [](double x) -> double { return x * x * x - 8.0; };
    double root = openswmm::findroot::ridder(0.0, 5.0, 1e-10, func);
    EXPECT_NEAR(root, 2.0, 1e-8);
}
