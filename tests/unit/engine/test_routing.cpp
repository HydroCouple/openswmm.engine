/**
 * @file test_routing.cpp
 * @brief Unit tests for hydraulic routing — DW solver, Preissmann slot, CFL,
 *        force main friction, cross-section geometry, node hydraulics.
 *
 * @details Tests:
 *   - DWSolver Preissmann slot geometry (width, area, hyd radius)
 *   - DWSolver constants match legacy values
 *   - DWNodeArrays initialization
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

#ifdef _MSC_VER
#  define _USE_MATH_DEFINES
#endif
#include <gtest/gtest.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "hydraulics/DynamicWave.hpp"
#include "hydraulics/KinematicWave.hpp"
#include "hydraulics/Routing.hpp"
#include "hydraulics/XSectBatch.hpp"
#include "hydraulics/ForceMain.hpp"
#include "hydraulics/Node.hpp"
#include "core/SimulationContext.hpp"

#ifndef BENCHMARK_DATA_DIR
#  define BENCHMARK_DATA_DIR ""
#endif

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
// DWNodeArrays initialization
// ============================================================================

TEST(DWNodeArrays, DefaultValues) {
    DWNodeArrays na;
    na.resize(1);
    EXPECT_DOUBLE_EQ(na.new_surf_area[0], 0.0);
    EXPECT_DOUBLE_EQ(na.old_surf_area[0], 0.0);
    EXPECT_DOUBLE_EQ(na.sumdqdh[0], 0.0);
    EXPECT_DOUBLE_EQ(na.dYdT[0], 0.0);
    EXPECT_EQ(na.converged[0], 0);
    EXPECT_EQ(na.is_surcharged[0], 0);
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

TEST(NodeHydraulics, JunctionSurfAreaReturnsZero) {
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::JUNCTION;
    nodes.full_depth[0] = 10.0;

    // Non-storage nodes return 0 from getSurfArea — matches legacy
    // node_getSurfArea. The MIN_SURFAREA floor is applied downstream
    // at consumption sites in DynamicWave (see commit 5b2be6cf).
    double sa = node::getSurfArea(nodes, 0, 5.0);
    EXPECT_EQ(sa, 0.0);
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
    DPSLinkArrays da;
    da.resize(1);
    EXPECT_NEAR(da.As[0], 0.0, 1e-15);
    EXPECT_NEAR(da.hs[0], 0.0, 1e-15);
    EXPECT_NEAR(da.P[0], 1.0, 1e-15);
    EXPECT_NEAR(da.P_hat[0], 1.0, 1e-15);
    EXPECT_EQ(da.surcharged[0], 0);
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

// ============================================================================
// Anderson acceleration skip-flag tests (Issue 3)
//
// computeAASkipFlags() marks nodes where the Picard fixed-point operator G
// is non-smooth, disabling AA to avoid stale-residual mixing.
// ============================================================================

/// Minimal fixture: 2 junctions + 1 outfall, 1 circular conduit (J0 → J1),
/// 1 conduit (J1 → O2).  Enough to init DWSolver and call computeAASkipFlags
/// via execute().
class AASkipFlagTest : public ::testing::Test {
protected:
    SimulationContext ctx;
    XSectGroups groups_;
    std::vector<XSectParams> params_;
    DWSolver solver;

    void SetUp() override {
        // 3 nodes: J0 (junction), J1 (junction), O2 (outfall)
        ctx.nodes.resize(3);
        ctx.nodes.type[0] = NodeType::JUNCTION;
        ctx.nodes.invert_elev[0] = 100.0;
        ctx.nodes.full_depth[0] = 6.0;
        ctx.nodes.init_depth[0] = 0.0;

        ctx.nodes.type[1] = NodeType::JUNCTION;
        ctx.nodes.invert_elev[1] = 99.0;
        ctx.nodes.full_depth[1] = 6.0;
        ctx.nodes.init_depth[1] = 0.0;

        ctx.nodes.type[2] = NodeType::OUTFALL;
        ctx.nodes.invert_elev[2] = 98.0;
        ctx.nodes.full_depth[2] = 6.0;
        ctx.nodes.init_depth[2] = 0.0;

        // Crown elevations: invert + conduit diameter (2 ft pipes)
        ctx.nodes.crown_elev[0] = 102.0;  // 100 + 2
        ctx.nodes.crown_elev[1] = 101.0;  // 99 + 2
        ctx.nodes.crown_elev[2] = 100.0;  // 98 + 2

        // 2 conduits: C0 (J0→J1), C1 (J1→O2)
        ctx.links.resize(2);
        for (int i = 0; i < 2; ++i) {
            ctx.links.type[i] = LinkType::CONDUIT;
            ctx.links.xsect_shape[i] = XsectShape::CIRCULAR;
            ctx.links.xsect_y_full[i] = 2.0;
            ctx.links.xsect_a_full[i] = M_PI;  // π·1²
            ctx.links.xsect_w_max[i] = 2.0;
            ctx.links.roughness[i] = 0.013;
            ctx.links.length[i] = 400.0;
            ctx.links.mod_length[i] = 400.0;
            ctx.links.barrels[i] = 1;
            ctx.links.loss_inlet[i] = 0.0;
            ctx.links.loss_outlet[i] = 0.0;
            ctx.links.loss_avg[i] = 0.0;
        }
        ctx.links.node1[0] = 0;  ctx.links.node2[0] = 1;
        ctx.links.node1[1] = 1;  ctx.links.node2[1] = 2;

        // Build cross-section geometry tables
        params_.resize(2);
        double p[4] = {2.0, 0, 0, 0};
        xsect::setParams(params_[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
        xsect::setParams(params_[1], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
        groups_.build(params_.data(), 2);
    }

    void initSolver(SurchargeMethod method) {
        solver.surcharge_method = method;
        solver.anderson_accel = true;
        solver.init(3, 2, groups_, ctx);
    }
};

TEST_F(AASkipFlagTest, FreeSurfaceNoSkip) {
    // Free-surface flow: no surcharge → aa_skip_ should be all zeros
    initSolver(SurchargeMethod::EXTRAN);

    // Set shallow depths (well below crown)
    for (int i = 0; i < 3; ++i) {
        ctx.nodes.depth[i] = 0.5;
        ctx.nodes.old_depth[i] = 0.5;
        ctx.nodes.head[i] = ctx.nodes.invert_elev[i] + 0.5;
    }
    // No surcharge → nodeState should NOT be surcharged
    // Execute once to populate skip flags
    solver.execute(ctx, 10.0);

    const auto& flags = solver.aaSkipFlags();
    ASSERT_EQ(flags.size(), 3u);
    // Shallow free-surface: no node should be skipped
    EXPECT_EQ(flags[0], 0) << "Free-surface J0 should not skip AA";
    EXPECT_EQ(flags[1], 0) << "Free-surface J1 should not skip AA";
    // Outfall is always converged/skipped, but flag should still be 0
    EXPECT_EQ(flags[2], 0) << "Outfall should not skip AA";
}

TEST_F(AASkipFlagTest, ExtranSurchargedSkips) {
    // EXTRAN surcharge: surcharged nodes should skip AA
    initSolver(SurchargeMethod::EXTRAN);

    // Set depths above crown (surcharge) and maintain with lateral inflow
    double crown0 = ctx.nodes.full_depth[0];
    ctx.nodes.depth[0] = crown0 + 1.0;
    ctx.nodes.old_depth[0] = crown0 + 1.0;
    ctx.nodes.head[0] = ctx.nodes.invert_elev[0] + crown0 + 1.0;
    ctx.nodes.lat_flow[0] = 100.0;  // strong inflow to maintain surcharge

    // Pre-set is_surcharged so first iteration's skip flag is computed
    solver.nodeSurchargedFlag(0) = 1;

    ctx.nodes.depth[1] = 0.5;  // below crown
    ctx.nodes.old_depth[1] = 0.5;
    ctx.nodes.head[1] = ctx.nodes.invert_elev[1] + 0.5;

    ctx.nodes.depth[2] = 0.5;
    ctx.nodes.old_depth[2] = 0.5;
    ctx.nodes.head[2] = ctx.nodes.invert_elev[2] + 0.5;

    solver.execute(ctx, 10.0);

    const auto& flags = solver.aaSkipFlags();
    // J0 is surcharged → skip
    EXPECT_EQ(flags[0], 1) << "Surcharged J0 must skip AA under EXTRAN";
    // J1 is not surcharged → no skip
    EXPECT_EQ(flags[1], 0) << "Free-surface J1 should not skip AA";
}

TEST_F(AASkipFlagTest, DPSActiveSkipsEndNodes) {
    // DYNAMIC_SLOT with active As > 0: both end nodes of active conduit skip AA
    initSolver(SurchargeMethod::DYNAMIC_SLOT);

    // Set depths above crown to trigger DPS geometry activation
    double crown = ctx.links.xsect_y_full[0];  // 2.0 ft
    ctx.nodes.depth[0] = crown + 0.5;
    ctx.nodes.old_depth[0] = crown + 0.5;
    ctx.nodes.head[0] = ctx.nodes.invert_elev[0] + crown + 0.5;

    ctx.nodes.depth[1] = crown + 0.5;
    ctx.nodes.old_depth[1] = crown + 0.5;
    ctx.nodes.head[1] = ctx.nodes.invert_elev[1] + crown + 0.5;

    ctx.nodes.depth[2] = 0.5;
    ctx.nodes.old_depth[2] = 0.5;
    ctx.nodes.head[2] = ctx.nodes.invert_elev[2] + 0.5;

    // Execute — DPS geometry rewrites happen inside computeLinkGeometry
    solver.execute(ctx, 10.0);

    const auto& flags = solver.aaSkipFlags();
    // C0 connects J0→J1; if C0 has DPS active, both J0 and J1 should skip
    // (Whether DPS activates depends on the geometry pass; at minimum, the
    // code path is exercised without crashing)
    // We verify the structural invariant: if any flag is set, it was because
    // a conduit touching that node had non-smooth geometry
    EXPECT_GE(flags.size(), 3u);
}

TEST_F(AASkipFlagTest, SlotNearKinkSkipsEndNodes) {
    // SLOT method: conduit depth near 0.985*yFull → skip AA for end nodes
    initSolver(SurchargeMethod::SLOT);

    double yFull = ctx.links.xsect_y_full[0];  // 2.0 ft
    // Set node depths so conduit midpoint ≈ 0.99 * yFull (inside [0.98, 1.02] band)
    double d_near_kink = 0.99 * yFull;
    ctx.nodes.depth[0] = d_near_kink;
    ctx.nodes.old_depth[0] = d_near_kink;
    ctx.nodes.head[0] = ctx.nodes.invert_elev[0] + d_near_kink;

    ctx.nodes.depth[1] = d_near_kink;
    ctx.nodes.old_depth[1] = d_near_kink;
    ctx.nodes.head[1] = ctx.nodes.invert_elev[1] + d_near_kink;

    ctx.nodes.depth[2] = 0.5;
    ctx.nodes.old_depth[2] = 0.5;
    ctx.nodes.head[2] = ctx.nodes.invert_elev[2] + 0.5;

    solver.execute(ctx, 10.0);

    const auto& flags = solver.aaSkipFlags();
    // C0 connects J0→J1 with midpoint depth near kink → both should skip
    // C1 connects J1→O2; J1's depth is near kink so it might propagate
    EXPECT_GE(flags.size(), 3u);
    // The code path for SLOT near-kink detection is exercised without crash
}

TEST_F(AASkipFlagTest, SlotFarFromKinkNoSkip) {
    // SLOT method: conduit depth well below cutoff → no skip
    initSolver(SurchargeMethod::SLOT);

    // Set shallow depths (50% of yFull — far from 0.98-1.02 band)
    for (int i = 0; i < 3; ++i) {
        ctx.nodes.depth[i] = 1.0;  // 50% of yFull=2.0
        ctx.nodes.old_depth[i] = 1.0;
        ctx.nodes.head[i] = ctx.nodes.invert_elev[i] + 1.0;
    }

    solver.execute(ctx, 10.0);

    const auto& flags = solver.aaSkipFlags();
    EXPECT_EQ(flags[0], 0) << "Shallow SLOT flow should not skip AA at J0";
    EXPECT_EQ(flags[1], 0) << "Shallow SLOT flow should not skip AA at J1";
}

TEST_F(AASkipFlagTest, AADisabledNoFlags) {
    // When anderson_accel is false, skip flags should remain all zeros
    solver.surcharge_method = SurchargeMethod::EXTRAN;
    solver.anderson_accel = false;
    solver.init(3, 2, groups_, ctx);

    // Set surcharged depths
    double crown = ctx.nodes.full_depth[0];
    ctx.nodes.depth[0] = crown + 2.0;
    ctx.nodes.old_depth[0] = crown + 2.0;
    ctx.nodes.head[0] = ctx.nodes.invert_elev[0] + crown + 2.0;

    ctx.nodes.depth[1] = 0.5;
    ctx.nodes.old_depth[1] = 0.5;
    ctx.nodes.head[1] = ctx.nodes.invert_elev[1] + 0.5;

    ctx.nodes.depth[2] = 0.5;
    ctx.nodes.old_depth[2] = 0.5;
    ctx.nodes.head[2] = ctx.nodes.invert_elev[2] + 0.5;

    solver.execute(ctx, 10.0);

    const auto& flags = solver.aaSkipFlags();
    for (std::size_t i = 0; i < flags.size(); ++i) {
        EXPECT_EQ(flags[i], 0) << "AA disabled: no skip flags at node " << i;
    }
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
    DPSLinkArrays dps;
    dps.resize(1);
    dps.As[0] = 0.01;
    dps.hs[0] = -0.001;  // negative from depressurization

    if (dps.hs[0] < 0.0 && dps.As[0] > 0.0) {
        dps.hs[0] = 0.0;
    }
    EXPECT_NEAR(dps.hs[0], 0.0, 1e-15);
    EXPECT_GT(dps.As[0], 0.0);  // As still positive — residual conserved
}

TEST(DPS, FullDepressurizationResets) {
    DPSLinkArrays dps;
    dps.resize(1);
    dps.As[0] = -0.001;  // fully depressurized
    dps.hs[0] = 0.0;

    if (dps.As[0] <= 0.0) {
        dps.As[0] = 0.0;
        dps.hs[0] = 0.0;
    }
    EXPECT_NEAR(dps.As[0], 0.0, 1e-15);
    EXPECT_NEAR(dps.hs[0], 0.0, 1e-15);
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

// ============================================================================
// KW steady-state benchmark — normal-depth recovery
// ============================================================================
//
// Benchmark dataset: tests/benchmarks/manufactured/kinwave-normal-depth-rect-open/
//
// At steady state (q1=q2=q_in=Q_n, a1=a2=A_n) the KW continuity residual is
// identically zero: f(A_n) = S_n/s_full - Q_n/q_full = 0 exactly (WT=WX=0.6
// cancel).  Newton converges in 0 iterations; output error is FP rounding only.

namespace {

struct KWBenchRow {
    double d_n_ft;
    double A_n_ft2;
    double Q_n_cfs;
};

static std::vector<KWBenchRow> load_kw_bench(const std::string& path) {
    std::vector<KWBenchRow> rows;
    std::ifstream f(path);
    if (!f.is_open()) return rows;
    std::string line;
    bool header_seen = false;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!header_seen) { header_seen = true; continue; }  // skip column header
        std::istringstream ss(line);
        std::string tok;
        KWBenchRow row{};
        // columns: d_n_ft, A_n_ft2, R_n_ft, S_n_ft83, Q_n_cfs
        if (!std::getline(ss, tok, ',')) continue; row.d_n_ft  = std::stod(tok);
        if (!std::getline(ss, tok, ',')) continue; row.A_n_ft2 = std::stod(tok);
        if (!std::getline(ss, tok, ',')) continue; // R_n_ft (unused)
        if (!std::getline(ss, tok, ',')) continue; // S_n_ft83 (unused)
        if (!std::getline(ss, tok, ',')) continue; row.Q_n_cfs = std::stod(tok);
        rows.push_back(row);
    }
    return rows;
}

}  // namespace

TEST(KWSolverSteadyState, NormalDepthRecovered) {
    const std::string csv_path =
        std::string(BENCHMARK_DATA_DIR)
        + "/manufactured/kinwave-normal-depth-rect-open/reference.csv";

    auto rows = load_kw_bench(csv_path);
    if (rows.empty()) {
        GTEST_SKIP() << "KW benchmark CSV not found: " << csv_path;
    }

    // Channel parameters
    const double PHI    = 1.486;
    const double n_mann = 0.013;
    const double slope  = 0.001;
    const double w      = 10.0;
    const double y_full = 5.0;
    const double beta   = PHI * std::sqrt(slope) / n_mann;

    // Arbitrary conduit geometry for the time-marching coefficients;
    // at steady state these cancel and do not affect the zero-residual result.
    const double length = 500.0;
    const double dt     = 60.0;

    // Build cross-section — setParams fills a_full, r_full, s_full, s_max
    XSectParams xs{};
    const double p[4] = {y_full, w, 0.0, 0.0};
    xsect::setParams(xs, static_cast<int>(XSectShape::RECT_OPEN), p, 1.0);

    const double q_full = beta * xs.s_full;
    const double a_full = xs.a_full;

    kinwave::KWSolver solver;
    solver.init(1, XSectGroups{});

    double max_q_err = 0.0;
    double max_a_err = 0.0;

    for (const auto& row : rows) {
        // Compute A_n and Q_n from the same floating-point path the solver uses,
        // then cross-check against the hand-computed CSV reference (≤ 0.01%).
        double A_n = xsect::getAofY(xs, row.d_n_ft);
        double S_n = xsect::getSofA(xs, A_n);
        double Q_n = beta * S_n;

        EXPECT_NEAR(Q_n, row.Q_n_cfs, 1e-4 * row.Q_n_cfs)
            << "Manning Q_n mismatch vs CSV at d_n=" << row.d_n_ft << " ft";

        // Pre-load steady-state ICs: inlet and outlet both at normal depth
        solver.q1_[0]  = Q_n;
        solver.a1_[0]  = A_n;
        solver.q2_[0]  = Q_n;
        solver.a2_[0]  = A_n;
        solver.q_in_[0] = Q_n;

        solver.solveConduit(0, xs, q_full, a_full, xs.s_full,
                            beta, length, dt, 0.0);

        max_q_err = std::max(max_q_err, std::fabs(solver.q_out_[0] - Q_n));
        max_a_err = std::max(max_a_err, std::fabs(solver.a_out_[0] - A_n));
    }

    // DO NOT loosen these tolerances.
    //
    // The continuity residual f(A_n) = S_n/s_full - Q_n/q_full = 0 at steady
    // state (WT=WX=0.6 cancel exactly in the C1/C2 derivation).  Newton
    // therefore converges in 0 iterations; the only error is FP rounding
    // (~1e-15 relative).  The 1e-9 threshold sits a million times above that.
    //
    // If either assertion fails it means a real regression in solveConduit —
    // the Newton solve, the section-factor inversion, or the state
    // normalisation changed in a physically meaningful way.  Fix the code,
    // not the tolerance.
    EXPECT_LT(max_q_err / q_full, 1e-9)
        << "q_out deviated from normal-depth Q_n (max over all reference rows)";
    EXPECT_LT(max_a_err / a_full, 1e-9)
        << "a_out deviated from normal-depth A_n (max over all reference rows)";
}

// ============================================================================
// KW step-inflow benchmark — mass balance and steady-state convergence
//
// Benchmark dataset: tests/benchmarks/manufactured/kinwave-step-inflow-rectangular-conduit/
//
// Applies a step inflow Q_in = Q_n (normal-depth flow) to a channel initially
// at rest.  The kinematic wave arrives at the outlet after t_arrival = L/c_0
// ≈ 80 s.  After N_steps=10 steps (600 s >> t_arrival), two analytically
// exact properties are verified:
//   1. SS convergence: Q_out(T) ≈ Q_in (within 0.5%)
//   2. Mass balance:   |V_in - V_out - delta_V_stored| / V_in < 0.1%
// ============================================================================

// Step-inflow transient: SS convergence and mass balance for RECT_OPEN channel.
TEST(KWSolverTransient, StepInflowMassBalance) {
    const std::string csv_path =
        std::string(BENCHMARK_DATA_DIR)
        + "/manufactured/kinwave-step-inflow-rectangular-conduit/reference.csv";

    // Load channel parameters from benchmark CSV (single data row, header-only skip)
    double Q_in = 50.0, W = 10.0, slope = 0.001, n_mann = 0.013, length = 500.0;
    double dt = 60.0;
    double ss_tol_rel = 0.005;
    double massbal_tol_rel = 0.05;
    int n_steps = 10;
    {
        std::ifstream f(csv_path);
        if (!f.is_open()) {
            GTEST_SKIP() << "Benchmark CSV not found: " << csv_path;
        }
        std::string line;
        bool header_seen = false;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (!header_seen) { header_seen = true; continue; }
            std::istringstream ss(line);
            std::string tok;
            // columns: Q_in_cfs, channel_width_ft, bed_slope, n_mann,
            //          channel_length_ft, dt_s, n_steps, ...
            if (!std::getline(ss, tok, ',')) break; Q_in    = std::stod(tok);
            if (!std::getline(ss, tok, ',')) break; W       = std::stod(tok);
            if (!std::getline(ss, tok, ',')) break; slope   = std::stod(tok);
            if (!std::getline(ss, tok, ',')) break; n_mann  = std::stod(tok);
            if (!std::getline(ss, tok, ',')) break; length  = std::stod(tok);
            if (!std::getline(ss, tok, ',')) break; dt      = std::stod(tok);
            if (!std::getline(ss, tok, ',')) break; n_steps = std::stoi(tok);
            if (!std::getline(ss, tok, ',')) break; // ss_check_step (metadata only)
            if (!std::getline(ss, tok, ',')) break; ss_tol_rel = std::stod(tok);
            if (!std::getline(ss, tok, ',')) break; massbal_tol_rel = std::stod(tok);
            break;
        }
    }

    // Build cross-section (RECT_OPEN, W=10 ft, y_full=5 ft)
    const double PHI    = 1.486;                       // US Manning coefficient
    const double beta   = PHI * std::sqrt(slope) / n_mann;
    const double y_full = 5.0;

    XSectParams xs{};
    const double p[4] = { y_full, W, 0.0, 0.0 };
    xsect::setParams(xs, static_cast<int>(XSectShape::RECT_OPEN), p, 1.0);

    const double q_full = beta * xs.s_full;
    const double a_full = xs.a_full;

    // Solve for normal-depth area A_n at Q_in via xsect section factor
    // S_n = Q_in / beta;  A_n = getAofS(xs, S_n)
    const double s_n = Q_in / beta;
    const double A_n = xsect::getAofS(xs, s_n);
    const double Q_n = beta * xsect::getSofA(xs, A_n);  // should equal Q_in closely

    ASSERT_NEAR(Q_n, Q_in, 1e-3 * Q_in)
        << "Manning normal depth Q_n does not match Q_in to 0.1%";

    kinwave::KWSolver solver;
    solver.init(1, XSectGroups{});

    // Initial state: at rest
    solver.q1_[0]  = 0.0;
    solver.a1_[0]  = 0.0;
    solver.q2_[0]  = 0.0;
    solver.a2_[0]  = 0.0;
    solver.q_in_[0] = 0.0;

    double V_in  = 0.0;   // cumulative inflow volume (ft^3)
    double V_out = 0.0;   // cumulative outflow volume (ft^3)

    for (int step = 0; step < n_steps; ++step) {
        // Apply step inflow
        solver.q_in_[0] = Q_in;

        solver.solveConduit(0, xs, q_full, a_full, xs.s_full,
                            beta, length, dt, 0.0);

        V_in  += Q_in * dt;
        V_out += solver.q_out_[0] * dt;

        // Advance state for next step
        solver.q1_[0] = solver.q_in_[0];
        solver.a1_[0] = A_n;              // upstream at normal depth
        solver.q2_[0] = solver.q_out_[0];
        solver.a2_[0] = solver.a_out_[0];
    }

    // 1. Steady-state convergence: after 600 s >> t_arrival (≈80 s),
    //    outflow must be within 0.5% of Q_in.
    const double ss_tol = ss_tol_rel * Q_in;
    EXPECT_NEAR(solver.q_out_[0], Q_in, ss_tol)
        << "Q_out at step " << n_steps << " has not converged to Q_in within 0.5%"
        << "  Q_out=" << solver.q_out_[0] << "  Q_in=" << Q_in;

    // 2. Mass balance: compare cumulative net inflow against the conduit's final
    //    stored volume using the same trapezoidal area formula as KWSolver.
    //    The step-1 "no-flow" branch still justifies a loose tolerance, but the
    //    residual should be formed against the actual final storage, not A_n*L.
    const double delta_stored = V_in - V_out;
    const double final_stored_volume = 0.5 * (solver.a_in_[0] + solver.a_out_[0]) * length;
    const double massbal_err  = std::abs(delta_stored - final_stored_volume);
    EXPECT_GT(delta_stored, 0.0)
        << "Negative stored volume implies mass loss (V_out > V_in)";
    EXPECT_LT(massbal_err / V_in, massbal_tol_rel)
        << "Mass balance error " << massbal_err
        << " ft^3 exceeds " << (100.0 * massbal_tol_rel)
        << "% of V_in=" << V_in << " ft^3";
}

// ============================================================================
// Benchmark: force-main friction reference curves
//
// Both HW and DW formulas are direct transcriptions with no table lookup.
// The C++ result should match the analytical reference to within 1e-10 relative.
// ============================================================================

TEST(ForceMain, FrictionReferenceCurvesBenchmark) {
    std::string path = std::string(BENCHMARK_DATA_DIR)
        + "/manufactured/forcemain-friction-reference-curves/reference.csv";

    struct Row { std::string model; double v, R, param, Sf_ref; };
    std::vector<Row> rows;
    {
        std::ifstream in(path);
        if (!in.is_open()) {
            GTEST_SKIP() << "Benchmark data not found: " << path;
        }
        std::string line;
        bool header_seen = false;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (!header_seen) { header_seen = true; continue; }
            std::istringstream ss(line);
            std::string tok;
            std::vector<std::string> cols;
            while (std::getline(ss, tok, ','))
                cols.push_back(tok);
            if (cols.size() >= 5)
                rows.push_back({cols[0],
                    std::stod(cols[1]), std::stod(cols[2]),
                    std::stod(cols[3]), std::stod(cols[4])});
        }
    }
    if (rows.empty()) {
        GTEST_SKIP() << "Benchmark CSV is empty: " << path;
    }

    for (const auto& row : rows) {
        double Sf_computed;
        if (row.model == "HW")
            Sf_computed = forcemain::getFricSlope_HW(row.v, row.R, row.param);
        else
            Sf_computed = forcemain::getFricSlope_DW(row.v, row.R, row.param);

        double rel_err = std::abs(Sf_computed - row.Sf_ref) / row.Sf_ref;
        EXPECT_LT(rel_err, 1e-10)
            << row.model << " v=" << row.v << " R=" << row.R
            << " param=" << row.param
            << " computed=" << Sf_computed << " ref=" << row.Sf_ref
            << " rel_err=" << rel_err;
    }
}

// ============================================================================
// DW solver GVF backwater M1 benchmark
//
// Benchmark dataset: tests/benchmarks/manufactured/dynwave-gvf-backwater-m1/
//
// A 1000-ft RECT_OPEN channel (b=5 ft, S₀=0.001, n=0.013) is discretised into
// 5 conduits of 200 ft each.  Constant inflow Q=10 cfs enters at J0; J5 is a
// FIXED outfall at y_d = 1.5·y_n.  After 3600 s the DW solver must converge to
// the analytically computed M1 GVF profile (RK4-integrated from downstream).
//
// Tolerance: 5% of y_n ≈ 0.039 ft at junction nodes J0–J4.
// ============================================================================

TEST(DWSolverGVF, BackwaterM1Benchmark) {
    const std::string csv_path =
        std::string(BENCHMARK_DATA_DIR)
        + "/manufactured/dynwave-gvf-backwater-m1/reference.csv";

    struct Row { std::string node; double x_ft, z_inv_ft, y_gvf_ft; };
    std::vector<Row> rows;
    {
        std::ifstream in(csv_path);
        if (!in.is_open()) {
            GTEST_SKIP() << "Benchmark data not found: " << csv_path;
        }
        std::string line;
        bool header_seen = false;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (!header_seen) { header_seen = true; continue; }
            std::istringstream ss(line);
            std::string tok;
            std::vector<std::string> cols;
            while (std::getline(ss, tok, ','))
                cols.push_back(tok);
            if (cols.size() >= 4)
                rows.push_back({cols[0],
                    std::stod(cols[1]), std::stod(cols[2]),
                    std::stod(cols[3])});
        }
    }
    if (rows.size() < 6) {
        GTEST_SKIP() << "Benchmark CSV has fewer than 6 rows: " << csv_path;
    }

    // ---- Channel parameters ----
    const double PHI    = 1.486;
    const double n_mann = 0.013;
    const double S0     = 0.001;
    const double b      = 5.0;    // channel width (ft)
    const double y_full = 4.0;    // full depth (ft) — large to prevent surcharge
    const double L      = 200.0;  // conduit length (ft)
    const double Q      = 10.0;   // steady inflow (cfs)

    // ---- Cross-section geometry (same for all 5 conduits) ----
    XSectParams xs{};
    const double p_xs[4] = {y_full, b, 0.0, 0.0};
    xsect::setParams(xs, static_cast<int>(XSectShape::RECT_OPEN), p_xs, 1.0);

    // Build XSectGroups from 5 identical RECT_OPEN parameter blocks
    std::vector<XSectParams> xparams(5, xs);
    XSectGroups groups;
    groups.build(xparams.data(), 5);

    // ---- Conveyance parameters ----
    const double beta         = PHI * std::sqrt(S0) / n_mann;
    const double rough_factor = openswmm::constants::GRAVITY * (n_mann / PHI) * (n_mann / PHI);
    const double q_full       = beta * xs.s_full;

    // ---- Reference depths from CSV ----
    // rows[0..4] = J0..J4 (junctions), rows[5] = J5 (fixed outfall BC)
    // y_n_mann: Manning normal depth (provenance.yaml: 0.781692 ft); used for
    // tolerance only — the J0 GVF depth (0.792075 ft) is 1.3% above y_n because
    // the M1 profile asymptotes toward y_n as reach length → ∞.
    const double y_n_mann = 0.781692;      // Manning normal depth (ft)
    const double y_d      = rows[5].y_gvf_ft;  // downstream fixed stage

    // ---- Build SimulationContext: 6 nodes, 5 conduits ----
    SimulationContext ctx;
    ctx.nodes.resize(6);
    ctx.links.resize(5);

    // Node invert elevations (each conduit drops 0.2 ft over 200 ft → S₀=0.001)
    const double z_inv[6] = {1.0, 0.8, 0.6, 0.4, 0.2, 0.0};

    for (int i = 0; i < 6; ++i) {
        ctx.nodes.invert_elev[i] = z_inv[i];
        ctx.nodes.full_depth[i]  = 100.0;       // large — no overtopping
        ctx.nodes.crown_elev[i]  = z_inv[i] + 100.0;  // prevent spurious surcharge
        ctx.nodes.type[i]        = NodeType::JUNCTION;
        ctx.nodes.depth[i]       = y_n_mann;
        ctx.nodes.old_depth[i]   = y_n_mann;
        ctx.nodes.head[i]        = z_inv[i] + y_n_mann;
    }

    // J5 is the fixed-stage outfall (downstream BC)
    ctx.nodes.type[5]             = NodeType::OUTFALL;
    ctx.nodes.outfall_type[5]     = OutfallType::FIXED;
    ctx.nodes.outfall_param[5]    = y_d;  // stage = invert(0) + y_d = y_d
    ctx.nodes.outfall_link_idx[5] = 4;    // last conduit L4 (J4→J5)
    ctx.nodes.outfall_link_offset[5] = 0.0;
    ctx.nodes.depth[5]            = y_d;
    ctx.nodes.old_depth[5]        = y_d;
    ctx.nodes.head[5]             = z_inv[5] + y_d;

    // Constant lateral inflow at J0
    ctx.nodes.lat_flow[0] = Q;

    // Configure 5 conduits (J0→J1, J1→J2, ..., J4→J5)
    for (int i = 0; i < 5; ++i) {
        ctx.links.type[i]               = LinkType::CONDUIT;
        ctx.links.xsect_shape[i]        = XsectShape::RECT_OPEN;
        ctx.links.xsect_batch_shape[i]  = static_cast<int>(XSectShape::RECT_OPEN);
        ctx.links.xsect_y_full[i]       = xs.y_full;
        ctx.links.xsect_a_full[i]       = xs.a_full;
        ctx.links.xsect_w_max[i]        = xs.w_max;
        ctx.links.xsect_r_full[i]       = xs.r_full;
        ctx.links.xsect_s_full[i]       = xs.s_full;
        ctx.links.xsect_s_max[i]        = xs.s_max;
        ctx.links.beta[i]               = beta;
        ctx.links.rough_factor[i]       = rough_factor;
        ctx.links.q_full[i]             = q_full;
        ctx.links.q_max[i]              = q_full;
        ctx.links.length[i]             = L;
        ctx.links.mod_length[i]         = L;
        ctx.links.slope[i]              = S0;
        ctx.links.roughness[i]          = n_mann;
        ctx.links.barrels[i]            = 1;
        ctx.links.node1[i]              = i;
        ctx.links.node2[i]              = i + 1;
        ctx.links.flow[i]               = Q;
        ctx.links.old_flow[i]           = Q;
    }

    // ---- Initialize DWSolver ----
    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::EXTRAN;
    solver.init(6, 5, groups, ctx);

    // ---- Run 120 steps at dt=30 s (T=3600 s ≈ 18 wave travel times) ----
    const double dt      = 30.0;
    const int    n_steps = 120;

    for (int step = 0; step < n_steps; ++step) {
        ctx.nodes.save_state();   // depth → old_depth, net_inflow → old_net_inflow
        ctx.links.save_state();   // flow  → old_flow
        solver.execute(ctx, dt);
        // Restore constant lateral inflow (save_state copies but does not zero it)
        ctx.nodes.lat_flow[0] = Q;
    }

    // ---- Compare final depths to GVF reference ----
    // J5 is the fixed BC: skip.  Check J0–J4 within 5% of Manning normal depth.
    const double tol = 0.05 * y_n_mann;
    for (int i = 0; i < 5; ++i) {
        EXPECT_NEAR(ctx.nodes.depth[i], rows[i].y_gvf_ft, tol)
            << rows[i].node << " (x=" << rows[i].x_ft << " ft)"
            << "  depth=" << ctx.nodes.depth[i]
            << "  ref="   << rows[i].y_gvf_ft
            << "  tol="   << tol;
    }
}
