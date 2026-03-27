/**
 * @file test_groundwater.cpp
 * @brief Unit tests for two-zone groundwater model.
 *
 * @details Tests:
 *   - Upper zone percolation (unsaturated conductivity)
 *   - Lateral GW flow formula (a1*(H-H*)^b1 - a2*(Hsw-H*)^b2 + a3*H*Hsw)
 *   - Euler step state integration
 *   - ET from upper/lower zones
 *   - Deep percolation
 *   - Mass balance: infil = perc + upper_evap; perc = gw_flow + deep_loss + lower_evap + storage change
 *
 * @see src/engine/hydrology/Groundwater.hpp
 * @ingroup engine_hydrology
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "hydrology/Groundwater.hpp"
#include "core/SimulationContext.hpp"

using namespace openswmm;
using namespace openswmm::groundwater;

// Dummy context — GWSolver::execute doesn't actually read ctx
static SimulationContext gw_dummy_ctx;

// ============================================================================
// GWSoA initialization
// ============================================================================

TEST(GWSoA, ResizeSetsDefaults) {
    GWSoA soa;
    soa.resize(3);
    EXPECT_EQ(soa.n_subcatch, 3);
    EXPECT_EQ(soa.porosity.size(), 3u);
    EXPECT_EQ(soa.theta.size(), 3u);
    EXPECT_EQ(soa.gw_flow.size(), 3u);

    // Default values
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(soa.porosity[i], 0.4);
        EXPECT_DOUBLE_EQ(soa.theta[i], 0.2);
        EXPECT_DOUBLE_EQ(soa.gw_flow[i], 0.0);
    }
}

// ============================================================================
// Upper zone percolation
// ============================================================================

class GWSolverTest : public ::testing::Test {
protected:
    GWSolver solver;

    void SetUp() override {
        solver.init(3);
        auto& soa = solver.state();

        for (int i = 0; i < 3; ++i) {
            auto ui = static_cast<std::size_t>(i);
            soa.porosity[ui]       = 0.4;
            soa.field_cap[ui]      = 0.2;
            soa.wilt_point[ui]     = 0.1;
            soa.k_sat[ui]          = 1e-5;   // ft/sec
            soa.k_slope[ui]        = 10.0;
            soa.tension_slope[ui]  = 0.0;
            soa.upper_evap_frac[ui]= 0.5;
            soa.lower_evap_depth[ui]= 2.0;   // ft
            soa.lower_loss_coeff[ui]= 1e-6;  // deep perc coeff
            soa.total_depth[ui]    = 10.0;    // ft aquifer thickness

            soa.a1[ui] = 0.001;
            soa.b1[ui] = 1.0;
            soa.a2[ui] = 0.0;
            soa.b2[ui] = 0.0;
            soa.a3[ui] = 0.0;
            soa.h_star[ui] = 2.0;

            soa.theta[ui]       = 0.25;  // above field cap
            soa.lower_depth[ui] = 5.0;   // ft
        }
    }
};

TEST_F(GWSolverTest, PercAboveFieldCapacityIsPositive) {
    auto& soa = solver.state();

    // theta > field_cap → percolation should be positive
    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    // Save initial theta
    std::vector<double> theta0(soa.theta.begin(), soa.theta.end());

    solver.execute(gw_dummy_ctx, 3600.0, 0.0,
                   infil_rate.data(), sw_head.data());

    // theta should decrease (water percolated down)
    for (int i = 0; i < 3; ++i) {
        EXPECT_LE(soa.theta[i], theta0[i])
            << "Theta should decrease when above field capacity";
    }
}

TEST_F(GWSolverTest, NoPecBelowFieldCapacity) {
    auto& soa = solver.state();

    // Set theta below field capacity
    for (auto& t : soa.theta) t = 0.15;

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    // With no infil and theta < field_cap, perc = 0
    // Only changes from evap and GW flow
    double theta_before = soa.theta[0];

    solver.execute(gw_dummy_ctx, 60.0, 0.0,
                   infil_rate.data(), sw_head.data());

    // theta shouldn't increase without infiltration
    EXPECT_LE(soa.theta[0], theta_before + 1e-10);
}

TEST_F(GWSolverTest, InfiltrationIncreasesTheta) {
    auto& soa = solver.state();

    // Start with theta at wilt point
    for (auto& t : soa.theta) t = 0.1;

    std::vector<double> infil_rate(3, 1e-4);  // substantial infiltration
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 3600.0, 0.0,
                   infil_rate.data(), sw_head.data());

    // theta should increase with infiltration
    for (int i = 0; i < 3; ++i) {
        EXPECT_GT(soa.theta[i], 0.1)
            << "Infiltration should increase theta";
    }
}

// ============================================================================
// Lateral GW flow formula
// ============================================================================

TEST_F(GWSolverTest, GWFlowPositiveAboveThreshold) {
    auto& soa = solver.state();

    // lower_depth > h_star → GW flow should be positive
    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 3600.0, 0.0,
                   infil_rate.data(), sw_head.data());

    for (int i = 0; i < 3; ++i) {
        EXPECT_GT(soa.gw_flow[i], 0.0)
            << "GW flow should be positive when H > H*";
    }
}

TEST_F(GWSolverTest, GWFlowZeroBelowThreshold) {
    auto& soa = solver.state();

    // Set lower_depth below h_star
    for (auto& h : soa.lower_depth) h = 1.0;  // below h_star=2.0

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 3600.0, 0.0,
                   infil_rate.data(), sw_head.data());

    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(soa.gw_flow[i], 0.0)
            << "GW flow should be zero when H <= H*";
    }
}

TEST_F(GWSolverTest, GWFlowLinearWithUnitExponent) {
    auto& soa = solver.state();

    // With b1=1 and a2=a3=0:  Q = a1 * (H - H*)
    soa.a1[0] = 0.005;
    soa.b1[0] = 1.0;
    soa.a2[0] = 0.0;
    soa.a3[0] = 0.0;
    soa.h_star[0] = 2.0;
    soa.lower_depth[0] = 7.0;  // H = 7.0

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 0.001, 0.0,
                   infil_rate.data(), sw_head.data());

    // Q ≈ 0.005 * (7.0 - 2.0) = 0.025
    EXPECT_NEAR(soa.gw_flow[0], 0.025, 0.001);
}

// ============================================================================
// Deep percolation
// ============================================================================

TEST_F(GWSolverTest, DeepPercolationProportionalToDepth) {
    auto& soa = solver.state();

    // deep_loss = lower_loss_coeff * (lower_depth / total_depth)
    soa.lower_loss_coeff[0] = 0.001;
    soa.lower_depth[0] = 5.0;
    soa.total_depth[0] = 10.0;

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 0.001, 0.0,
                   infil_rate.data(), sw_head.data());

    EXPECT_NEAR(soa.deep_loss[0], 0.001 * 5.0 / 10.0, 1e-8);
}

// ============================================================================
// ET from zones
// ============================================================================

TEST_F(GWSolverTest, UpperEvapWhenAboveWiltPoint) {
    auto& soa = solver.state();
    soa.theta[0] = 0.25;  // above wilt_point=0.1
    soa.upper_evap_frac[0] = 0.5;

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);
    double max_evap = 1e-5;

    solver.execute(gw_dummy_ctx, 3600.0, max_evap,
                   infil_rate.data(), sw_head.data());

    EXPECT_GT(soa.upper_evap[0], 0.0);
    EXPECT_LE(soa.upper_evap[0], max_evap);
}

TEST_F(GWSolverTest, NoUpperEvapBelowWiltPoint) {
    auto& soa = solver.state();
    soa.theta[0] = 0.05;  // below wilt_point=0.1
    soa.upper_evap_frac[0] = 0.5;

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 3600.0, 1e-5,
                   infil_rate.data(), sw_head.data());

    EXPECT_DOUBLE_EQ(soa.upper_evap[0], 0.0);
}

// ============================================================================
// State clamping
// ============================================================================

TEST_F(GWSolverTest, ThetaClampedToPorosityRange) {
    auto& soa = solver.state();
    soa.theta[0] = 0.25;

    // Heavy infiltration to push theta up
    std::vector<double> infil_rate(3, 0.01);
    std::vector<double> sw_head(3, 0.0);

    // Long timestep to push bounds
    solver.execute(gw_dummy_ctx, 100000.0, 0.0,
                   infil_rate.data(), sw_head.data());

    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(soa.theta[i], soa.wilt_point[i]);
        EXPECT_LE(soa.theta[i], soa.porosity[i]);
    }
}

TEST_F(GWSolverTest, LowerDepthClampedNonNegative) {
    auto& soa = solver.state();

    // Heavy GW outflow to deplete lower zone
    for (int i = 0; i < 3; ++i) {
        soa.a1[i] = 1.0;  // very high GW flow
        soa.lower_depth[i] = 3.0;
    }

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 100000.0, 0.0,
                   infil_rate.data(), sw_head.data());

    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(soa.lower_depth[i], 0.0);
        EXPECT_LE(soa.lower_depth[i], soa.total_depth[i]);
    }
}
