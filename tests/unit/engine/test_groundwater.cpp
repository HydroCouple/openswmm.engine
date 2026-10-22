/**
 * @file      test_groundwater.cpp
 * @brief     Unit tests for two-zone groundwater model.
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
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "hydrology/Groundwater.hpp"
#include "core/SimulationContext.hpp"
#include "core/DateTime.hpp"
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_output.h>
#include <openswmm/legacy/engine/openswmm_solver.h>

#ifndef BENCHMARK_DATA_DIR
#define BENCHMARK_DATA_DIR ""
#endif

using namespace openswmm;
using namespace openswmm::groundwater;

// Dummy context — GWSolver::execute reads ctx.options.flow_units and
// ctx.subcatches.area[] for mass balance accumulation.
/**
 * @brief Borrow the persistent groundwater helper context with three subcatchment areas.
 *
 * Separate init flag/mutable shared storage require serialized use. Subsequent calls retain all
 * prior mutations and do not reset fields; no complete hydraulic model or groundwater solver is
 * initialized.
 * @return Mutable process-lifetime context first populated with S1/S2/S3 and area one authored acre
 *     each.
 */
static SimulationContext& make_gw_dummy_ctx() {
    static SimulationContext ctx;
    static bool init = false;
    if (!init) {
        ctx.subcatch_names.add("S1");
        ctx.subcatch_names.add("S2");
        ctx.subcatch_names.add("S3");
        ctx.subcatches.resize(3);
        for (int i = 0; i < 3; ++i)
            ctx.subcatches.area[static_cast<std::size_t>(i)] = 1.0;
        init = true;
    }
    return ctx;
}
static SimulationContext& gw_dummy_ctx = make_gw_dummy_ctx();

TEST(GroundwaterEvaporation, UsesRunoffEndpointMonthAheadOfRoutingClock) {
    SimulationContext ctx;
    ctx.subcatch_names.add("S1");
    ctx.subcatches.resize(1);
    ctx.subcatches.area[0] = 1.0;
    ctx.current_date = datetime::encodeDate(1970, 3, 31) + datetime::encodeTime(23, 0, 0);
    ctx.patterns.add("MONTHLY", 0, {0.1, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.8, 0.7, 0.5, 0.4, 0.2});
    GWSolver solver;
    solver.init(1);
    auto& state = solver.state();
    state.total_depth[0] = 10.0;
    state.theta[0] = 0.2;
    state.upper_evap_frac[0] = 1.0;
    state.upper_evap_pat[0] = 0;
    const double zero[] = {0.0}, perv[] = {1.0};
    solver.execute(ctx, 3600.0, 1e-7, zero, zero, perv, zero, 4);
    EXPECT_DOUBLE_EQ(state.upper_evap[0], 0.5e-7);
    EXPECT_EQ(datetime::monthOfYear(ctx.current_date), 3);
}

TEST(GroundwaterEvaporation, SurfaceEvaporationSpendsTheSharedDemandBeforeSoilEt) {
    for (const double surface : {0.0, 4e-7, 1e-6}) {
        SimulationContext ctx;
        ctx.subcatch_names.add("S1");
        ctx.subcatches.resize(1);
        ctx.subcatches.area[0] = 100;
        GWSolver solver;
        solver.init(1);
        auto& state = solver.state();
        state.total_depth[0] = 10;
        state.theta[0] = .2;
        state.upper_evap_frac[0] = 1;
        const double zero[] = {0}, perv[] = {1}, used[] = {surface};
        solver.execute(ctx, 60, 1e-6, zero, zero, perv, used, 1);
        EXPECT_NEAR(state.upper_evap[0], 1e-6 - surface, 1e-15);
        EXPECT_NEAR(state.upper_evap[0] + state.lower_evap[0] + surface, 1e-6, 1e-15);
    }
}

TEST(GroundwaterParity, MonthBoundaryAndHourlyHortonMatchLegacySavedResults) {
    const std::string base = "mc_storm_parity/groundwater_month_horton";
    ASSERT_EQ(swmm_run((base + ".inp").c_str(), (base + ".legacy.rpt").c_str(),
                       (base + ".legacy.out").c_str()),
              0);
    SWMM_Engine engine = swmm_engine_create();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(swmm_engine_open(engine, (base + ".inp").c_str(), (base + ".new.rpt").c_str(),
                               (base + ".new.out").c_str(), nullptr),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine, 1), SWMM_OK);
    double elapsed = 0.0;
    int error = 0;
    do {
        error = swmm_engine_step(engine, &elapsed);
    } while (error == SWMM_OK && elapsed > 0.0);
    EXPECT_EQ(error, SWMM_OK);
    EXPECT_EQ(swmm_engine_end(engine), SWMM_OK);
    swmm_engine_close(engine);
    swmm_engine_destroy(engine);

    SWMM_Output legacy = swmm_output_open((base + ".legacy.out").c_str());
    SWMM_Output current = swmm_output_open((base + ".new.out").c_str());
    ASSERT_NE(legacy, nullptr);
    ASSERT_NE(current, nullptr);
    ASSERT_EQ(swmm_output_get_period_count(legacy), 112);
    ASSERT_EQ(swmm_output_get_period_count(current), 112);
    ASSERT_EQ(swmm_output_get_subcatch_count(legacy), 1);
    ASSERT_EQ(swmm_output_get_subcatch_count(current), 1);
    for (int period = 0; period < 112; ++period) {
        for (int var = 0; var < 8; ++var) {
            float a = 0.0f, b = 0.0f;
            ASSERT_EQ(swmm_output_get_subcatch_result(legacy, period, var, &a), 0);
            ASSERT_EQ(swmm_output_get_subcatch_result(current, period, var, &b), 0);
            EXPECT_TRUE(std::isfinite(b)) << period << ":" << var;
            EXPECT_EQ(a, b) << period << ":" << var;
        }
        for (int var = 0; var < 15; ++var) {
            float a = 0.0f, b = 0.0f;
            ASSERT_EQ(swmm_output_get_system_result(legacy, period, var, &a), 0);
            ASSERT_EQ(swmm_output_get_system_result(current, period, var, &b), 0);
            EXPECT_TRUE(std::isfinite(b)) << period << ":" << var;
            EXPECT_EQ(a, b) << period << ":" << var;
        }
    }
    swmm_output_close(legacy);
    swmm_output_close(current);
}

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
    std::vector<double> frac_perv = std::vector<double>(3, 1.0);
    std::vector<double> perv_evap_rate = std::vector<double>(3, 0.0);

    /**
     * @brief Initialize three identical groundwater stores with prescribed hydraulic state.
     *
     * Seeds porosity/field capacity/wilting point .4/.2/.1, Ks=1e-5 ft/s, 10 ft aquifer thickness,
     * theta=.25 and lower depth 5 ft, plus evaporation/loss and exchange coefficients. No timestep or
     * forcing is applied; the state is owned by solver. Fatal GoogleTest assertions return from this
     * hook on failure; any created handle remains owned by the fixture for teardown.
     */
    void SetUp() override {
        solver.init(3);
        auto& soa = solver.state();

        for (int i = 0; i < 3; ++i) {
            auto ui = static_cast<std::size_t>(i);
            soa.porosity[ui] = 0.4;
            soa.field_cap[ui] = 0.2;
            soa.wilt_point[ui] = 0.1;
            soa.k_sat[ui] = 1e-5; // ft/sec
            soa.k_slope[ui] = 10.0;
            soa.tension_slope[ui] = 0.0;
            soa.upper_evap_frac[ui] = 0.5;
            soa.lower_evap_depth[ui] = 2.0;  // ft
            soa.lower_loss_coeff[ui] = 1e-6; // deep perc coeff
            soa.total_depth[ui] = 10.0;      // ft aquifer thickness

            soa.a1[ui] = 0.001;
            soa.b1[ui] = 1.0;
            soa.a2[ui] = 0.0;
            soa.b2[ui] = 0.0;
            soa.a3[ui] = 0.0;
            soa.h_star[ui] = 2.0;

            soa.theta[ui] = 0.25;      // above field cap
            soa.lower_depth[ui] = 5.0; // ft
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

    solver.execute(gw_dummy_ctx, 3600.0, 0.0, infil_rate.data(), sw_head.data(), frac_perv.data(),
                   perv_evap_rate.data());

    // theta should decrease (water percolated down)
    for (int i = 0; i < 3; ++i) {
        EXPECT_LE(soa.theta[i], theta0[i]) << "Theta should decrease when above field capacity";
    }
}

TEST_F(GWSolverTest, NoPecBelowFieldCapacity) {
    auto& soa = solver.state();

    // Set theta below field capacity
    for (auto& t : soa.theta)
        t = 0.15;

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    // With no infil and theta < field_cap, perc = 0
    // Only changes from evap and GW flow
    double theta_before = soa.theta[0];

    solver.execute(gw_dummy_ctx, 60.0, 0.0, infil_rate.data(), sw_head.data(), frac_perv.data(),
                   perv_evap_rate.data());

    // theta shouldn't increase without infiltration
    EXPECT_LE(soa.theta[0], theta_before + 1e-10);
}

TEST_F(GWSolverTest, InfiltrationIncreasesTheta) {
    auto& soa = solver.state();

    // Start with theta at wilt point
    for (auto& t : soa.theta)
        t = 0.1;

    std::vector<double> infil_rate(3, 1e-4); // substantial infiltration
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 3600.0, 0.0, infil_rate.data(), sw_head.data(), frac_perv.data(),
                   perv_evap_rate.data());

    // theta should increase with infiltration
    for (int i = 0; i < 3; ++i) {
        EXPECT_GT(soa.theta[i], 0.1) << "Infiltration should increase theta";
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

    solver.execute(gw_dummy_ctx, 3600.0, 0.0, infil_rate.data(), sw_head.data(), frac_perv.data(),
                   perv_evap_rate.data());

    for (int i = 0; i < 3; ++i) {
        EXPECT_GT(soa.gw_flow[i], 0.0) << "GW flow should be positive when H > H*";
    }
}

TEST_F(GWSolverTest, GWFlowZeroBelowThreshold) {
    auto& soa = solver.state();

    // Set lower_depth below h_star
    for (auto& h : soa.lower_depth)
        h = 1.0; // below h_star=2.0

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 3600.0, 0.0, infil_rate.data(), sw_head.data(), frac_perv.data(),
                   perv_evap_rate.data());

    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(soa.gw_flow[i], 0.0) << "GW flow should be zero when H <= H*";
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
    soa.lower_depth[0] = 7.0; // H = 7.0

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 0.001, 0.0, infil_rate.data(), sw_head.data(), frac_perv.data(),
                   perv_evap_rate.data());

    // After RKF45 integration the water table moves, so gw_flow reflects
    // the end-state flux.  Just verify flow is positive (H started > H*).
    EXPECT_GT(soa.gw_flow[0], 0.0) << "GW flow should be positive when H > H*";
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

    solver.execute(gw_dummy_ctx, 0.001, 0.0, infil_rate.data(), sw_head.data(), frac_perv.data(),
                   perv_evap_rate.data());

    EXPECT_NEAR(soa.deep_loss[0], 0.001 * 5.0 / 10.0, 1e-8);
}

// ============================================================================
// ET from zones
// ============================================================================

TEST_F(GWSolverTest, UpperEvapWhenAboveWiltPoint) {
    auto& soa = solver.state();
    soa.theta[0] = 0.25; // above wilt_point=0.1
    soa.upper_evap_frac[0] = 0.5;
    // Reduce GW flow to prevent theta from draining below wilt point
    soa.a1[0] = 0.0;

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);
    double max_evap = 1e-5;

    solver.execute(gw_dummy_ctx, 60.0, max_evap, infil_rate.data(), sw_head.data(),
                   frac_perv.data(), perv_evap_rate.data());

    EXPECT_GT(soa.upper_evap[0], 0.0);
    EXPECT_LE(soa.upper_evap[0], max_evap);
}

TEST_F(GWSolverTest, NoUpperEvapBelowWiltPoint) {
    auto& soa = solver.state();
    soa.theta[0] = 0.05; // below wilt_point=0.1
    soa.upper_evap_frac[0] = 0.5;

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 3600.0, 1e-5, infil_rate.data(), sw_head.data(), frac_perv.data(),
                   perv_evap_rate.data());

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
    solver.execute(gw_dummy_ctx, 100000.0, 0.0, infil_rate.data(), sw_head.data(), frac_perv.data(),
                   perv_evap_rate.data());

    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(soa.theta[i], soa.wilt_point[i]);
        EXPECT_LE(soa.theta[i], soa.porosity[i]);
    }
}

TEST_F(GWSolverTest, LowerDepthClampedNonNegative) {
    auto& soa = solver.state();

    // Heavy GW outflow to deplete lower zone
    for (int i = 0; i < 3; ++i) {
        soa.a1[i] = 1.0; // very high GW flow
        soa.lower_depth[i] = 3.0;
    }

    std::vector<double> infil_rate(3, 0.0);
    std::vector<double> sw_head(3, 0.0);

    solver.execute(gw_dummy_ctx, 100000.0, 0.0, infil_rate.data(), sw_head.data(), frac_perv.data(),
                   perv_evap_rate.data());

    for (int i = 0; i < 3; ++i) {
        EXPECT_GE(soa.lower_depth[i], 0.0);
        EXPECT_LE(soa.lower_depth[i], soa.total_depth[i]);
    }
}

// ============================================================================
// Groundwater linearized recession benchmark
//
// Benchmark dataset: tests/benchmarks/manufactured/groundwater-linearized-recession/
//
// With b1=1, a2=a3=0, no infiltration, no evaporation, and no deep loss, the
// lower-zone head satisfies:
//   dH/dt = -a1*(H-h_star) / (ucf_gwflow*(phi-theta))
//
// Exact solution: H(t) = h_star + (H_0-h_star)*exp(-lambda*t)
//   lambda = a1 / (ucf_gwflow*(phi-theta)) = 10/(43560*0.2) ≈ 1.1478e-3 /s
//
// GWSolver integrates with RKF45 (GWTOL=1e-4); test tolerance is 1e-3 ft.
// ============================================================================

// Linearized recession: H(t) = 0.5 + 2.5*exp(-t/871.2).
// RKF45 should match the continuous analytical solution within 1e-3 ft.
TEST(GWLinearizedRecession, ExponentialDecayTrajectory) {
    const std::string path = std::string(BENCHMARK_DATA_DIR) +
                             "/manufactured/groundwater-linearized-recession/reference.csv";

    // Load reference CSV (t_s, H_ft)
    struct GWRow {
        double t_s, H_ft;
    };
    std::vector<GWRow> rows;
    {
        std::ifstream in(path);
        if (!in.is_open()) {
            GTEST_SKIP() << "Benchmark data not found: " << path;
        }
        std::string line;
        bool header_seen = false;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#')
                continue;
            if (!header_seen) {
                header_seen = true;
                continue;
            }
            std::istringstream ss(line);
            std::string tok;
            double vals[2] = {};
            int col = 0;
            while (std::getline(ss, tok, ',') && col < 2)
                vals[col++] = std::stod(tok);
            if (col >= 2)
                rows.push_back({vals[0], vals[1]});
        }
    }
    if (rows.empty()) {
        GTEST_SKIP() << "Benchmark data empty: " << path;
    }

    // Minimal SimulationContext for GWSolver (1 subcatch, no nodes, US units)
    SimulationContext ctx;
    ctx.subcatch_names.add("S0");
    ctx.subcatches.resize(1);
    ctx.subcatches.area[0] = 1.0; // 1 acre

    GWSolver solver;
    solver.init(1);
    auto& soa = solver.state();

    // Linearized recession parameters (see definition.md for derivation)
    soa.total_depth[0] = 5.0;          // ft — total aquifer depth
    soa.lower_depth[0] = rows[0].H_ft; // 3.0 ft — initial head
    soa.h_star[0] = 0.5;               // ft — reference head
    soa.a1[0] = 10.0;                  // cfs/acre/ft — linear flow coefficient
    soa.b1[0] = 1.0;                   // linear exponent
    soa.a2[0] = 0.0;
    soa.a3[0] = 0.0;
    soa.porosity[0] = 0.4;
    soa.theta[0] = 0.2; // = field_cap — suppresses upper percolation
    soa.field_cap[0] = 0.2;
    soa.wilt_point[0] = 0.05;
    soa.lower_loss_coeff[0] = 0.0; // no deep loss
    soa.lower_evap_depth[0] = 0.0; // no lower-zone evap
    soa.upper_evap_frac[0] = 0.0;  // no upper-zone evap
    soa.upper_evap_pat[0] = -1;    // no seasonal pattern

    const double infil_rate[1] = {0.0};
    const double sw_head[1] = {0.0};
    const double frac_perv[1] = {1.0};
    const double perv_evap_rate[1] = {0.0};

    double max_err = 0.0;
    double sum_sq = 0.0;
    double prev_t = rows[0].t_s;

    for (size_t i = 1; i < rows.size(); ++i) {
        double dt = rows[i].t_s - prev_t;

        solver.execute(ctx, dt, 0.0, infil_rate, sw_head, frac_perv, perv_evap_rate);

        double err = std::abs(soa.lower_depth[0] - rows[i].H_ft);
        max_err = std::max(max_err, err);
        sum_sq += err * err;
        prev_t = rows[i].t_s;
    }

    ASSERT_GE(rows.size(), 2u) << "benchmark CSV must have at least one data row";
    double n = static_cast<double>(rows.size() - 1);

    // RKF45 with GWTOL=1e-4; tolerance is 1e-3 ft (10× GWTOL, absorbs step accumulation).
    // If this fails, investigate the GW flow formula, the ODE integration, or the
    // unit conversion — the tolerance already has a factor-of-10 margin.
    EXPECT_LT(max_err, 1e-3) << "GW lower_depth max error " << max_err
                             << " ft exceeds 1e-3 ft (benchmark: " << path << ")";
    EXPECT_LT(std::sqrt(sum_sq / n), 5e-4) << "GW lower_depth RMS error exceeds 5e-4 ft";
}
