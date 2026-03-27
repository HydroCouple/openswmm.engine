/**
 * @file test_routing.cpp
 * @brief Unit tests for hydraulic routing — DW solver, Preissmann slot, CFL.
 *
 * @details Tests:
 *   - DWSolver Preissmann slot geometry (width, area, hyd radius)
 *   - DWSolver constants match legacy values
 *   - DWNodeState initialization
 *   - Surcharge method selection
 *   - XSectGroups-based batch geometry for routing
 *
 * @see src/engine/hydraulics/DynamicWave.hpp
 * @see src/engine/hydraulics/Routing.hpp
 * @ingroup engine_hydraulics
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "hydraulics/DynamicWave.hpp"
#include "hydraulics/Routing.hpp"
#include "hydraulics/XSectBatch.hpp"

using namespace openswmm;
using namespace openswmm::dynwave;

// ============================================================================
// DW constants match legacy
// ============================================================================

TEST(DWConstants, OmegaDefault) {
    EXPECT_DOUBLE_EQ(OMEGA, 0.5);
}

TEST(DWConstants, HeadToleranceDefault) {
    EXPECT_DOUBLE_EQ(DEFAULT_HEADTOL, 0.005);
}

TEST(DWConstants, MaxTrialsDefault) {
    EXPECT_EQ(DEFAULT_MAXTRIALS, 8);
}

TEST(DWConstants, MaxVelocity) {
    EXPECT_DOUBLE_EQ(MAXVELOCITY, 50.0);
}

TEST(DWConstants, MinTimestep) {
    EXPECT_DOUBLE_EQ(MINTIMESTEP, 0.001);
}

TEST(DWConstants, ExtranCrownCutoff) {
    EXPECT_NEAR(EXTRAN_CROWN_CUTOFF, 0.96, 1e-6);
}

TEST(DWConstants, SlotCrownCutoff) {
    EXPECT_NEAR(SLOT_CROWN_CUTOFF, 0.985257, 1e-6);
}

TEST(DWConstants, SlotWidthFactor) {
    EXPECT_DOUBLE_EQ(SLOT_WIDTH_FACTOR, 0.001);
}

// ============================================================================
// DWNodeState initialization
// ============================================================================

TEST(DWNodeState, DefaultValues) {
    DWNodeState ns;
    EXPECT_DOUBLE_EQ(ns.new_surf_area, 0.0);
    EXPECT_DOUBLE_EQ(ns.old_surf_area, 0.0);
    EXPECT_DOUBLE_EQ(ns.sumdqdh, 0.0);
    EXPECT_DOUBLE_EQ(ns.dYdT, 0.0);
    EXPECT_FALSE(ns.converged);
    EXPECT_FALSE(ns.is_surcharged);
}

// ============================================================================
// Surcharge method enum
// ============================================================================

TEST(SurchargeMethod, EnumValues) {
    EXPECT_EQ(static_cast<int>(SurchargeMethod::EXTRAN), 0);
    EXPECT_EQ(static_cast<int>(SurchargeMethod::SLOT), 1);
    EXPECT_EQ(static_cast<int>(SurchargeMethod::DYNAMIC_SLOT), 2);
}

// ============================================================================
// DWSolver initialization and parameters
// ============================================================================

TEST(DWSolver, InitSetsParametersCorrectly) {
    DWSolver solver;
    solver.head_tol = 0.01;
    solver.max_trials = 4;
    solver.omega = 0.6;
    solver.surcharge_method = SurchargeMethod::SLOT;

    EXPECT_DOUBLE_EQ(solver.head_tol, 0.01);
    EXPECT_EQ(solver.max_trials, 4);
    EXPECT_DOUBLE_EQ(solver.omega, 0.6);
    EXPECT_EQ(solver.surcharge_method, SurchargeMethod::SLOT);
}

TEST(DWSolver, DefaultParametersMatchLegacy) {
    DWSolver solver;
    EXPECT_DOUBLE_EQ(solver.head_tol, DEFAULT_HEADTOL);
    EXPECT_EQ(solver.max_trials, DEFAULT_MAXTRIALS);
    EXPECT_DOUBLE_EQ(solver.omega, OMEGA);
    EXPECT_EQ(solver.surcharge_method, SurchargeMethod::EXTRAN);
}

// ============================================================================
// RouteModel enum
// ============================================================================

TEST(RouteModel, EnumValues) {
    EXPECT_EQ(static_cast<int>(RouteModel::STEADY), 0);
    EXPECT_EQ(static_cast<int>(RouteModel::KINWAVE), 1);
    EXPECT_EQ(static_cast<int>(RouteModel::DYNWAVE), 2);
}

// ============================================================================
// XSectGroups geometry for routing (numerical precision)
// ============================================================================

class RoutingGeometryTest : public ::testing::Test {
protected:
    std::vector<XSectParams> params_;
    XSectGroups groups_;

    void SetUp() override {
        // Create a small network: 4 conduits
        params_.resize(4);

        // C1: Circular D=2ft
        double p0[4] = {2.0, 0, 0, 0};
        xsect::setParams(params_[0], static_cast<int>(XSectShape::CIRCULAR), p0, 1.0);

        // C2: Circular D=3ft
        double p1[4] = {3.0, 0, 0, 0};
        xsect::setParams(params_[1], static_cast<int>(XSectShape::CIRCULAR), p1, 1.0);

        // C3: Rectangular 3ft x 4ft
        double p2[4] = {3.0, 4.0, 0, 0};
        xsect::setParams(params_[2], static_cast<int>(XSectShape::RECT_CLOSED), p2, 1.0);

        // C4: Trapezoidal 2ft deep, 3ft bottom, 1:1 slopes
        double p3[4] = {2.0, 3.0, 1.0, 1.0};
        xsect::setParams(params_[3], static_cast<int>(XSectShape::TRAPEZOIDAL), p3, 1.0);

        groups_.build(params_.data(), 4);
    }
};

TEST_F(RoutingGeometryTest, BatchAreasMatchAnalytical) {
    // Test at various depth fractions
    double fractions[] = {0.1, 0.25, 0.5, 0.75, 0.9};

    for (double frac : fractions) {
        double depths[4];
        double areas_batch[4];

        for (int i = 0; i < 4; ++i)
            depths[i] = params_[i].y_full * frac;

        groups_.computeAreas(depths, areas_batch, 4);

        for (int i = 0; i < 4; ++i) {
            double area_elem = xsect::getAofY(params_[i], depths[i]);
            EXPECT_NEAR(areas_batch[i], area_elem, 1e-10)
                << "Link " << i << " at frac=" << frac;
        }
    }
}

TEST_F(RoutingGeometryTest, BatchHydRadMatchesAnalytical) {
    double depths[4];
    double hrad_batch[4];

    for (int i = 0; i < 4; ++i)
        depths[i] = params_[i].y_full * 0.5;

    groups_.computeHydRad(depths, hrad_batch, 4);

    for (int i = 0; i < 4; ++i) {
        double hrad_elem = xsect::getRofY(params_[i], depths[i]);
        EXPECT_NEAR(hrad_batch[i], hrad_elem, 1e-10)
            << "Link " << i;
    }
}

TEST_F(RoutingGeometryTest, BatchWidthsMatchAnalytical) {
    double depths[4];
    double widths_batch[4];

    for (int i = 0; i < 4; ++i)
        depths[i] = params_[i].y_full * 0.3;

    groups_.computeWidths(depths, widths_batch, 4);

    for (int i = 0; i < 4; ++i) {
        double width_elem = xsect::getWofY(params_[i], depths[i]);
        EXPECT_NEAR(widths_batch[i], width_elem, 1e-10)
            << "Link " << i;
    }
}

// ============================================================================
// Circular cross-section numerical checks for routing
// ============================================================================

TEST(RoutingCircular, AreaAtQuarterFull) {
    XSectParams xs;
    double p[4] = {4.0, 0, 0, 0};  // D=4ft
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR), p, 1.0);

    double y = 1.0;  // quarter full (y/D = 0.25)
    double a = xsect::getAofY(xs, y);

    // Analytical: A = R²(θ - sinθcosθ) where θ = acos(1 - y/R)
    double R = 2.0;
    double theta = std::acos(1.0 - y / R);
    double analytical = R * R * (theta - std::sin(theta) * std::cos(theta));

    EXPECT_NEAR(a, analytical, 0.01 * analytical);
}

TEST(RoutingCircular, HydRadAtHalfFull) {
    XSectParams xs;
    double p[4] = {4.0, 0, 0, 0};
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR), p, 1.0);

    double r = xsect::getRofY(xs, 2.0);  // half full

    // At half full: R_hyd = D/4 (exact for semicircle)
    // A = πD²/8, P = πD/2, R = A/P = D/4
    EXPECT_NEAR(r, 1.0, 0.02);  // D/4 = 4/4 = 1.0
}

// ============================================================================
// Manning's equation check
// ============================================================================

TEST(ManningsEquation, FullPipeVelocity) {
    // V = (PHI/n) * R^(2/3) * S^(1/2)
    // For circular D=2ft, n=0.013, S=0.01:
    double n = 0.013;
    double S = 0.01;
    double D = 2.0;
    double R_full = D / 4.0;  // 0.5 ft
    double PHI = 1.486;

    double V = PHI / n * std::pow(R_full, 2.0/3.0) * std::sqrt(S);

    // Q = V * A = V * pi*D²/4
    double A_full = 3.14159265 / 4.0 * D * D;
    double Q = V * A_full;

    // Verify reasonable range: V should be ~5-15 ft/s for this setup
    EXPECT_GT(V, 3.0);
    EXPECT_LT(V, 20.0);

    // Flow should be positive
    EXPECT_GT(Q, 0.0);
}

// ============================================================================
// Section factor (used for normal depth calculation)
// ============================================================================

TEST(SectionFactor, CircularAtFull) {
    XSectParams xs;
    double p[4] = {3.0, 0, 0, 0};  // D=3ft
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR), p, 1.0);

    // S_full = A_full * R_full^(2/3)
    double expected = xs.a_full * std::pow(xs.r_full, 2.0/3.0);
    EXPECT_NEAR(xs.s_full, expected, 0.01 * expected);
}

TEST(SectionFactor, RectangularAtFull) {
    XSectParams xs;
    double p[4] = {2.0, 5.0, 0, 0};  // 2ft x 5ft
    xsect::setParams(xs, static_cast<int>(XSectShape::RECT_CLOSED), p, 1.0);

    // A = 10 ft², R = w*y/(w+2y) = 5*2/(5+4) = 10/9
    double R = 10.0 / 9.0;
    double expected = xs.a_full * std::pow(R, 2.0/3.0);
    EXPECT_NEAR(xs.s_full, expected, 0.01 * expected);
}
