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

// ============================================================================
// Modified Horton infiltration
// ============================================================================

TEST(ModHortonInfil, InitMatchesHorton) {
    // Modified Horton uses same init as standard Horton
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.1, 0.0, default_opts());
    EXPECT_NEAR(s.f0, 3.0 / 12.0 / 3600.0, 1e-12);
    EXPECT_NEAR(s.fmin, 0.5 / 12.0 / 3600.0, 1e-12);
}

TEST(ModHortonInfil, WetPeriodInfiltrates) {
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.1, 0.0, default_opts());

    double precip = 5.0 / 12.0 / 3600.0;
    double f = modHorton_getInfil(s, precip, 0.0, 300.0);

    EXPECT_GT(f, 0.0);
    EXPECT_LE(f, precip);
}

TEST(ModHortonInfil, DecayOverTime) {
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.1, 0.0, default_opts());

    double precip = 5.0 / 12.0 / 3600.0;
    double f1 = modHorton_getInfil(s, precip, 0.0, 300.0);
    double f2 = modHorton_getInfil(s, precip, 0.0, 300.0);
    double f3 = modHorton_getInfil(s, precip, 0.0, 300.0);

    EXPECT_GE(f1, f2);
    EXPECT_GE(f2, f3);
}

// ============================================================================
// Horton edge cases
// ============================================================================

TEST(HortonInfil, ZeroPrecipZeroDepthNoDryRecovery) {
    // No precip, no depth, no recovery needed if tp=0
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.1, 0.0, default_opts());
    s.tp = 0.0;

    double f = horton_getInfil(s, 0.0, 0.0, 300.0);
    EXPECT_EQ(f, 0.0);
}

TEST(HortonInfil, PondedDepthContributes) {
    // Ponded depth alone (no active precip) should contribute
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.1, 0.0, default_opts());

    double f = horton_getInfil(s, 0.0, 0.1, 300.0);  // depth=0.1ft
    EXPECT_GT(f, 0.0);
}

TEST(HortonInfil, MaxCumulativeInfiltration) {
    HortonState s;
    horton_init(s, 3.0, 0.5, 4.0, 0.1, 5.0, default_opts());  // Fmax=5in→ft

    double precip = 5.0 / 12.0 / 3600.0;
    double total = 0.0;
    // Run many steps until Fmax is reached
    for (int i = 0; i < 10000; ++i) {
        double f = horton_getInfil(s, precip, 0.0, 60.0);
        total += f * 60.0;
    }

    // Cumulative should not exceed Fmax (converted to ft)
    double Fmax_ft = 5.0 / 12.0;
    EXPECT_LE(total, Fmax_ft * 1.1);  // small tolerance for discretization
}

// ============================================================================
// Green-Ampt numerical checks
// ============================================================================

TEST(GreenAmptInfil, InitialRateHigherThanKs) {
    // Green-Ampt initial rate should be > Ks when soil is dry
    GreenAmptState s;
    grnampt_init(s, 4.0, 1.0, 0.3, default_opts());

    double high_precip = 10.0 / 12.0 / 3600.0;
    double f = grnampt_getInfil(s, high_precip, 0.0, 60.0);

    // Initially, f should be >= Ks because suction head pulls water in
    EXPECT_GE(f, s.Ks * 0.9);
}

TEST(GreenAmptInfil, LongTermApproachesKs) {
    // After much infiltration, rate should approach Ks
    GreenAmptState s;
    grnampt_init(s, 4.0, 1.0, 0.3, default_opts());

    double high_precip = 10.0 / 12.0 / 3600.0;
    double f_last = 0.0;
    for (int i = 0; i < 5000; ++i) {
        f_last = grnampt_getInfil(s, high_precip, 0.0, 60.0);
    }

    // Should be close to Ks
    EXPECT_NEAR(f_last, s.Ks, s.Ks * 0.3);
}

// ============================================================================
// SCS Curve Number numerical checks
// ============================================================================

TEST(CurveNumInfil, CN100GivesNearZeroRetention) {
    CurveNumState s;
    curvenum_init(s, 100.0, 0.01);
    // CN=100 → S very small (implementation may cap CN at 98-99)
    EXPECT_LT(s.Smax, 0.02);  // near zero but not necessarily exact
}

TEST(CurveNumInfil, SmaxFormulaVerification) {
    // S = (1000/CN - 10) / 12  (in feet)
    for (double cn : {50.0, 60.0, 70.0, 80.0, 90.0, 95.0}) {
        CurveNumState s;
        curvenum_init(s, cn, 0.01);
        double expected = (1000.0 / cn - 10.0) / 12.0;
        EXPECT_NEAR(s.Smax, expected, 1e-10) << "CN=" << cn;
    }
}

TEST(CurveNumInfil, TotalInfilApproachesRetention) {
    // For large cumulative rainfall, total infiltration should approach S
    CurveNumState s;
    curvenum_init(s, 75.0, 0.001);  // low regen

    double precip = 3.0 / 12.0 / 3600.0;
    double total_infil = 0.0;
    for (int i = 0; i < 5000; ++i) {
        double f = curvenum_getInfil(s, precip, 0.0, 60.0);
        total_infil += f * 60.0;
    }

    // Total infiltration should be bounded by Smax
    EXPECT_LE(total_infil, s.Smax * 1.5);  // approximate upper bound
    EXPECT_GT(total_infil, 0.0);
}

// ============================================================================
// ODE solver additional tests
// ============================================================================

TEST(OdeSolver, HarmonicOscillator) {
    // dy1/dt = y2, dy2/dt = -y1 → y1(t) = cos(t), y2(t) = -sin(t)
    double y[2] = {1.0, 0.0};  // y1(0)=1, y2(0)=0
    auto derivs = [](double /*x*/, const double* y, double* dydx) {
        dydx[0] = y[1];
        dydx[1] = -y[0];
    };

    int rc = openswmm::ode::integrate(y, 2, 0.0, 2.0 * 3.14159265, 1e-8, 0.01, derivs);
    EXPECT_EQ(rc, 0);
    // After one full period, should return to initial state
    EXPECT_NEAR(y[0], 1.0, 1e-5);
    EXPECT_NEAR(y[1], 0.0, 1e-5);
}

TEST(OdeSolver, QuadraticGrowth) {
    // dy/dt = 2*t, y(0) = 0 → y(t) = t²
    double y = 0.0;
    auto derivs = [](double x, const double* /*y*/, double* dydx) {
        dydx[0] = 2.0 * x;
    };

    int rc = openswmm::ode::integrate(&y, 1, 0.0, 3.0, 1e-8, 0.1, derivs);
    EXPECT_EQ(rc, 0);
    EXPECT_NEAR(y, 9.0, 1e-6);
}

// ============================================================================
// FindRoot additional tests
// ============================================================================

TEST(FindRoot, NewtonLinearExact) {
    // f(x) = 2x - 6 = 0 → x = 3
    double root = 1.0;
    auto func = [](double x, double* f, double* df) {
        *f = 2.0 * x - 6.0;
        *df = 2.0;
    };
    int iters = openswmm::findroot::newton(0.0, 10.0, &root, 1e-12, func);
    EXPECT_GT(iters, 0);
    EXPECT_NEAR(root, 3.0, 1e-12);
}

TEST(FindRoot, RidderTrigRoot) {
    // Find x such that sin(x) = 0 near x = pi
    auto func = [](double x) -> double { return std::sin(x); };
    double root = openswmm::findroot::ridder(2.5, 3.8, 1e-10, func);
    EXPECT_NEAR(root, 3.14159265358979, 1e-8);
}
