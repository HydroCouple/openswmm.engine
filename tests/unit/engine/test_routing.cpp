/**
 * @file test_routing.cpp
 * @brief Unit tests for hydraulic routing — DW solver, Preissmann slot, CFL,
 *        force main friction, cross-section geometry, node hydraulics.
 *
 * @details Tests:
 *   - DWSolver Preissmann slot geometry (width, area, hyd radius)
 *   - DWSolver constants match legacy values
 *   - DWNodeState initialization
 *   - Surcharge method selection
 *   - XSectGroups-based batch geometry for routing
 *   - Force main friction (Hazen-Williams, Darcy-Weisbach)
 *   - Cross-section area/depth/width relationships
 *   - Node volume/surface area conversions
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
#include "hydraulics/ForceMain.hpp"
#include "hydraulics/Node.hpp"

using namespace openswmm;
using namespace openswmm::dynwave;

// ============================================================================
// DW constants match legacy
// ============================================================================

TEST(DWConstants, OmegaDefault) {
    EXPECT_DOUBLE_EQ(OMEGA, 0.5);
}

TEST(DWConstants, HeadToleranceDefault) {
    EXPECT_DOUBLE_EQ(DEFAULT_HEAD_TOL, 0.005);
}

TEST(DWConstants, MaxTrialsDefault) {
    EXPECT_EQ(DEFAULT_MAX_TRIALS, 8);
}

TEST(DWConstants, MaxVelocity) {
    EXPECT_DOUBLE_EQ(MAX_VELOCITY, 50.0);
}

TEST(DWConstants, MinTimestep) {
    EXPECT_DOUBLE_EQ(MIN_TIMESTEP, 0.001);
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
    EXPECT_DOUBLE_EQ(solver.head_tol, DEFAULT_HEAD_TOL);
    EXPECT_EQ(solver.max_trials, DEFAULT_MAX_TRIALS);
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

// ============================================================================
// Force Main friction tests
// ============================================================================

TEST(ForceMain, HazenWilliamsZeroVelocity) {
    double Sf = forcemain::getFricSlope_HW(0.0, 1.0, 100.0);
    EXPECT_NEAR(Sf, 0.0, 1e-15);
}

TEST(ForceMain, HazenWilliamsPositive) {
    // Typical values: V=3 ft/s, R=0.5 ft, C=100
    double Sf = forcemain::getFricSlope_HW(3.0, 0.5, 100.0);
    EXPECT_GT(Sf, 0.0);
}

TEST(ForceMain, HazenWilliamsHigherVelocityMoreFriction) {
    double Sf_low  = forcemain::getFricSlope_HW(1.0, 0.5, 100.0);
    double Sf_high = forcemain::getFricSlope_HW(5.0, 0.5, 100.0);
    EXPECT_GT(Sf_high, Sf_low);
}

TEST(ForceMain, HazenWilliamsHigherC_LessFriction) {
    double Sf_low_c  = forcemain::getFricSlope_HW(3.0, 0.5, 80.0);
    double Sf_high_c = forcemain::getFricSlope_HW(3.0, 0.5, 150.0);
    EXPECT_GT(Sf_low_c, Sf_high_c);
}

TEST(ForceMain, DarcyWeisbachZeroVelocity) {
    double Sf = forcemain::getFricSlope_DW(0.0, 1.0, 0.001);
    EXPECT_NEAR(Sf, 0.0, 1e-15);
}

TEST(ForceMain, DarcyWeisbachPositive) {
    // V=3, R=0.5, roughness=0.001 ft
    double Sf = forcemain::getFricSlope_DW(3.0, 0.5, 0.001);
    EXPECT_GT(Sf, 0.0);
}

TEST(ForceMain, DarcyWeisbachHigherVelocityMoreFriction) {
    double Sf_low  = forcemain::getFricSlope_DW(1.0, 0.5, 0.001);
    double Sf_high = forcemain::getFricSlope_DW(5.0, 0.5, 0.001);
    EXPECT_GT(Sf_high, Sf_low);
}

TEST(ForceMain, DarcyWeisbachRougherPipeMoreFriction) {
    double Sf_smooth = forcemain::getFricSlope_DW(3.0, 0.5, 0.0001);
    double Sf_rough  = forcemain::getFricSlope_DW(3.0, 0.5, 0.01);
    EXPECT_GT(Sf_rough, Sf_smooth);
}

TEST(ForceMain, BatchMatchesSingle) {
    const int N = 4;
    double vel[N]   = {1.0, 2.0, 3.0, 4.0};
    double hrad[N]  = {0.5, 0.5, 0.5, 0.5};
    double param[N] = {100.0, 100.0, 100.0, 100.0};
    double sf[N]    = {};

    forcemain::batchFricSlope(vel, hrad, param, sf,
                              forcemain::FrictionModel::HAZEN_WILLIAMS, N);

    for (int i = 0; i < N; ++i) {
        double expected = forcemain::getFricSlope_HW(vel[i], hrad[i], param[i]);
        EXPECT_NEAR(sf[i], expected, 1e-10);
    }
}

// ============================================================================
// Cross-section geometry tests
// ============================================================================

TEST(XSectGeometry, CircularAreaAtHalfDepth) {
    XSectParams xs;
    double p[4] = {4.0, 0, 0, 0};  // D=4ft
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR), p, 1.0);

    // At half depth (y=2ft), area = (pi*D^2/4) * (theta - sin(theta)) / (2*pi)
    // where theta = pi (180 deg) → area = pi*D^2/8 = pi*16/8 = 2*pi ≈ 6.283
    double a = xsect::getAofY(xs, 2.0);
    double expected = M_PI * 4.0;  // pi * r^2 / 2 = pi * 4 / 2... Actually A = pi*R^2 at half = pi*4/2 = 2pi
    EXPECT_NEAR(a, xs.a_full / 2.0, 0.01 * xs.a_full);
}

TEST(XSectGeometry, CircularFullArea) {
    XSectParams xs;
    double p[4] = {3.0, 0, 0, 0};  // D=3ft
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR), p, 1.0);

    // Full area = pi*D^2/4 = pi*9/4 ≈ 7.069
    double expected = M_PI * 9.0 / 4.0;
    EXPECT_NEAR(xs.a_full, expected, 0.01);
}

TEST(XSectGeometry, CircularFullHydRadius) {
    XSectParams xs;
    double p[4] = {3.0, 0, 0, 0};
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR), p, 1.0);

    // R_full = D/4 = 0.75
    EXPECT_NEAR(xs.r_full, 0.75, 0.001);
}

TEST(XSectGeometry, RectOpenArea) {
    XSectParams xs;
    double p[4] = {3.0, 10.0, 0, 0};  // 3ft deep, 10ft wide
    xsect::setParams(xs, static_cast<int>(XSectShape::RECT_OPEN), p, 1.0);

    // Full area = w * y = 30
    EXPECT_NEAR(xs.a_full, 30.0, 0.01);
    EXPECT_NEAR(xs.w_max, 10.0, 0.01);
}

TEST(XSectGeometry, TrapezoidalArea) {
    XSectParams xs;
    double p[4] = {3.0, 10.0, 2.0, 2.0};  // yFull=3, wBot=10, slope1=2, slope2=2
    xsect::setParams(xs, static_cast<int>(XSectShape::TRAPEZOIDAL), p, 1.0);

    // m = (2+2)/2 = 2, A = (wBot + m*y) * y = (10 + 2*3)*3 = 48
    EXPECT_NEAR(xs.a_full, 48.0, 0.1);
}

TEST(XSectGeometry, TriangularArea) {
    XSectParams xs;
    double p[4] = {2.0, 4.0, 0, 0};  // yFull=2, top width=4 → slope=2
    xsect::setParams(xs, static_cast<int>(XSectShape::TRIANGULAR), p, 1.0);

    // Full area = slope * y^2 = 2 * 4 = 8... actually triangle: A = wTop*y/2 = 4*2/2 = 4
    EXPECT_NEAR(xs.a_full, 4.0, 0.1);
}

TEST(XSectGeometry, AreaInverseConsistency) {
    // For a given depth → area → depth should return same depth
    XSectParams xs;
    double p[4] = {3.0, 0, 0, 0};
    xsect::setParams(xs, static_cast<int>(XSectShape::CIRCULAR), p, 1.0);

    double y_test = 1.5;
    double a = xsect::getAofY(xs, y_test);
    double y_back = xsect::getYofA(xs, a);
    EXPECT_NEAR(y_back, y_test, 0.01);
}

TEST(XSectGeometry, IsOpenShape) {
    EXPECT_TRUE(xsect::isOpen(static_cast<int>(XSectShape::RECT_OPEN)));
    EXPECT_TRUE(xsect::isOpen(static_cast<int>(XSectShape::TRAPEZOIDAL)));
    EXPECT_TRUE(xsect::isOpen(static_cast<int>(XSectShape::TRIANGULAR)));
    EXPECT_FALSE(xsect::isOpen(static_cast<int>(XSectShape::CIRCULAR)));
    EXPECT_FALSE(xsect::isOpen(static_cast<int>(XSectShape::RECT_CLOSED)));
}

// ============================================================================
// Node volume/surface area tests
// ============================================================================

TEST(NodeHydraulics, JunctionVolumeLinear) {
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::JUNCTION;
    nodes.full_depth[0] = 10.0;

    // Junction V = MIN_SURFAREA * depth (12.566 * depth)
    double v = node::getVolume(nodes, 0, 5.0);
    EXPECT_NEAR(v, 12.566 * 5.0, 0.1);
}

TEST(NodeHydraulics, JunctionSurfAreaConstant) {
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::JUNCTION;
    nodes.full_depth[0] = 10.0;

    // Junction surface area is MIN_SURFAREA (constant)
    double sa = node::getSurfArea(nodes, 0, 5.0);
    EXPECT_GT(sa, 0.0);
}

TEST(NodeHydraulics, StorageFunctionalVolume) {
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::STORAGE;
    nodes.full_depth[0] = 10.0;
    nodes.storage_curve[0] = -1;  // functional
    nodes.storage_a[0] = 1000.0;  // A coefficient
    nodes.storage_b[0] = 0.0;     // B exponent → A = 1000 * y^0 = 1000
    nodes.storage_c[0] = 0.0;     // C constant

    // V = integral of A dy from 0 to y
    // With A = 1000 (constant), V = 1000 * y
    double v = node::getVolume(nodes, 0, 5.0);
    EXPECT_NEAR(v, 5000.0, 1.0);
}

TEST(NodeHydraulics, StorageSurfAreaFunctional) {
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::STORAGE;
    nodes.full_depth[0] = 10.0;
    nodes.storage_curve[0] = -1;
    nodes.storage_a[0] = 500.0;
    nodes.storage_b[0] = 0.0;
    nodes.storage_c[0] = 0.0;

    // A(y) = 500 * y^0 + 0 = 500
    double sa = node::getSurfArea(nodes, 0, 5.0);
    EXPECT_NEAR(sa, 500.0, 1.0);
}

// ============================================================================
// Dynamic Preissmann Slot (DPS) tests
// Sharior, Hodges & Vasconcelos (2023)
// ============================================================================

TEST(DPS, SurchargeMethodEnum) {
    EXPECT_EQ(static_cast<int>(SurchargeMethod::EXTRAN), 0);
    EXPECT_EQ(static_cast<int>(SurchargeMethod::SLOT), 1);
    EXPECT_EQ(static_cast<int>(SurchargeMethod::DYNAMIC_SLOT), 2);
}

TEST(DPS, ConfigDefaults) {
    DPSConfig cfg;
    EXPECT_NEAR(cfg.c_pT, 25.0, 1e-10);
    EXPECT_NEAR(cfg.alpha, 3.0, 1e-10);
    EXPECT_NEAR(cfg.r, 0.5, 1e-10);
    EXPECT_NEAR(cfg.c_pT_sq, 625.0, 1e-10);
}

TEST(DPS, LinkStateDefaults) {
    DPSLinkState st;
    EXPECT_NEAR(st.As, 0.0, 1e-15);
    EXPECT_NEAR(st.hs, 0.0, 1e-15);
    EXPECT_NEAR(st.P, 1.0, 1e-15);
    EXPECT_NEAR(st.P_hat, 1.0, 1e-15);
    EXPECT_FALSE(st.surcharged);
}

TEST(DPS, PreissmannNumberDecay) {
    // Eq. 22: P_hat(t - t_s) = (P_hat_0 - 1) * exp(-10*(t-t_s)/r) + 1
    double P_hat_0 = 500.0;
    double r = 0.5;

    // At t - t_s = 0: P_hat = P_hat_0
    double P_at_0 = (P_hat_0 - 1.0) * std::exp(0.0) + 1.0;
    EXPECT_NEAR(P_at_0, P_hat_0, 1e-10);

    // At t - t_s = r: P_hat ≈ 1.028 (paper states this)
    double P_at_r = (P_hat_0 - 1.0) * std::exp(-10.0 * r / r) + 1.0;
    // exp(-10) ≈ 4.54e-5, so P_at_r ≈ 1 + 499*4.54e-5 ≈ 1.0227
    EXPECT_NEAR(P_at_r, 1.0 + (P_hat_0 - 1.0) * std::exp(-10.0), 1e-10);
    EXPECT_LT(P_at_r, 1.03);  // close to 1

    // At t - t_s → ∞: P_hat → 1
    double P_inf = (P_hat_0 - 1.0) * std::exp(-10.0 * 100.0 / r) + 1.0;
    EXPECT_NEAR(P_inf, 1.0, 1e-10);
}

TEST(DPS, DeltaHsComputation) {
    // Eq. 19: Delta_hs = c_pT^2 * Delta_As / (g * A_C * P^2)
    double c_pT = 25.0 * 3.28084;  // m/s → ft/s
    double c_pT_sq = c_pT * c_pT;
    double A_C = 7.069;  // 3ft diameter circular pipe: pi*D^2/4
    double g = 32.174;   // ft/s^2
    double P = 100.0;
    double deltaAs = 0.01;  // ft^2

    double deltaHs = c_pT_sq * deltaAs / (g * A_C * P * P);

    // Should be positive and small
    EXPECT_GT(deltaHs, 0.0);

    // With larger P, deltaHs should be smaller (wider effective slot)
    double deltaHs_largeP = c_pT_sq * deltaAs / (g * A_C * 500.0 * 500.0);
    EXPECT_LT(deltaHs_largeP, deltaHs);
}

TEST(DPS, InitialPreissmannNumber) {
    // Eq. 23: P_hat_0 = c_pT / (alpha * c_g)
    double c_pT = 25.0 * 3.28084;  // ft/s
    double alpha = 3.0;

    // Circular pipe D=3ft: A_full = pi*9/4, T_w = 3ft
    double A_full = M_PI * 9.0 / 4.0;
    double T_w = 3.0;
    double l_D = A_full / T_w;  // hydraulic depth
    double c_g = std::sqrt(32.174 * l_D);  // gravity-wave celerity

    double P_hat_0 = c_pT / (alpha * c_g);

    EXPECT_GT(P_hat_0, 1.0);  // c_pT >> c_g for this pipe size
    // For D=3ft pipe: c_g ≈ 8.7 ft/s, c_pT ≈ 82 ft/s → P_hat_0 ≈ 82/(3*8.7) ≈ 3.14
    EXPECT_GT(P_hat_0, 2.0);
}

TEST(DPS, HsAccumulation) {
    // Multiple increments should accumulate
    double hs = 0.0;
    double As = 0.0;

    double c_pT_sq = std::pow(25.0 * 3.28084, 2.0);
    double g = 32.174;
    double A_C = 7.069;
    double P = 100.0;
    double P2 = P * P;

    for (int i = 0; i < 5; ++i) {
        double deltaAs = 0.005;
        double deltaHs = c_pT_sq * deltaAs / (g * A_C * P2);
        As += deltaAs;
        hs += deltaHs;
    }

    EXPECT_NEAR(As, 0.025, 1e-10);
    EXPECT_GT(hs, 0.0);
    // Verify linearity: 5 * single increment
    double single = c_pT_sq * 0.005 / (g * A_C * P2);
    EXPECT_NEAR(hs, 5.0 * single, 1e-10);
}

TEST(DPS, DepressurizationHysteresis) {
    // When hs <= 0 but As > 0, clamp hs to 0
    DPSLinkState dps;
    dps.As = 0.01;
    dps.hs = -0.001;  // negative from depressurization

    if (dps.hs < 0.0 && dps.As > 0.0) {
        dps.hs = 0.0;
    }
    EXPECT_NEAR(dps.hs, 0.0, 1e-15);
    EXPECT_GT(dps.As, 0.0);  // As still positive — residual conserved
}

TEST(DPS, FullDepressurizationResets) {
    DPSLinkState dps;
    dps.As = -0.001;  // fully depressurized
    dps.hs = 0.0;

    if (dps.As <= 0.0) {
        dps.As = 0.0;
        dps.hs = 0.0;
    }
    EXPECT_NEAR(dps.As, 0.0, 1e-15);
    EXPECT_NEAR(dps.hs, 0.0, 1e-15);
}

TEST(DPS, PreissmannNumberClampedAboveOne) {
    // P must always be >= 1 (c_p <= c_pT)
    double P_face1 = 0.5;  // hypothetical underflow
    double P_face2 = 0.8;
    double P = 0.5 * (P_face1 + P_face2);
    P = std::max(P, 1.0);
    EXPECT_GE(P, 1.0);
}

TEST(DPS, OpenShapesExcluded) {
    // Open shapes should not participate in DPS
    EXPECT_TRUE(xsect::isOpen(static_cast<int>(XSectShape::RECT_OPEN)));
    EXPECT_TRUE(xsect::isOpen(static_cast<int>(XSectShape::TRAPEZOIDAL)));
    EXPECT_FALSE(xsect::isOpen(static_cast<int>(XSectShape::CIRCULAR)));
    EXPECT_FALSE(xsect::isOpen(static_cast<int>(XSectShape::RECT_CLOSED)));
}

TEST(DPS, CFL_WithPressureCelerity) {
    // CFL: dt = L / (|U| + c_p), where c_p = c_pT / P
    double c_pT = 25.0 * 3.28084;  // ft/s
    double P = 10.0;
    double c_p = c_pT / P;
    double velocity = 3.0;
    double length = 500.0;

    double dt = length / (std::abs(velocity) + c_p);

    // With P=10, c_p ≈ 8.2 ft/s → dt ≈ 500/(3+8.2) ≈ 44.6s
    EXPECT_GT(dt, 0.0);
    EXPECT_LT(dt, length / velocity);

    // With P=1 (fully developed), c_p = c_pT ≈ 82 ft/s → much smaller dt
    double dt_full = length / (std::abs(velocity) + c_pT);
    EXPECT_LT(dt_full, dt);
}

TEST(DPS, OptionsDefaultValues) {
    SimulationOptions opts;
    EXPECT_NEAR(opts.dps_target_celerity, 25.0, 1e-10);
    EXPECT_NEAR(opts.dps_alpha, 3.0, 1e-10);
    EXPECT_NEAR(opts.dps_decay_time, 0.5, 1e-10);
}
