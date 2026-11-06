// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_groundwater_init.cpp
 * @brief Groundwater initial state in an SI model with a non-zero aquifer
 *        bottom and per-subcatchment [GROUNDWATER] overrides (Egwt, Ebot,
 *        Egw, Umc).
 *
 * @details Regression guard for a class of defects that only shows when the
 *          aquifer bottom is not 0 and the project is SI: the saturated-zone
 *          depth must be (Egw - Ebot) / UCF(LENGTH), Egwt is an elevation and
 *          not a height above the bottom, the optional Ebot/Egw/Umc columns
 *          override the aquifer row, and `*` for Egwt means the node invert.
 *          All legacy example decks use Ebot = 0 in US units, so the existing
 *          groundwater tests cannot see these paths.
 *
 * Expected values follow legacy gwater.c (gwater_readGroundwaterParams,
 * gwater_validate, gwater_initState, gwater_getGroundwater).
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

constexpr double kFt = 0.3048;   // m per ft; legacy reads SI lengths as x / UCF(LENGTH) with UCF(LENGTH) = 0.3048

const char* kInp =
    "[OPTIONS]\n"
    "FLOW_UNITS           LPS\n"
    "FLOW_ROUTING         DYNWAVE\n"
    "INFILTRATION         HORTON\n"
    "START_DATE           01/01/2026\n"
    "START_TIME           00:00:00\n"
    "END_DATE             01/01/2026\n"
    "END_TIME             03:00:00\n"
    "REPORT_STEP          00:05:00\n"
    "WET_STEP             00:01:00\n"
    "DRY_STEP             00:05:00\n"
    "ROUTING_STEP         0:00:10\n"
    "\n[RAINGAGES]\n"
    "RG1  INTENSITY 0:05 1.0 TIMESERIES TS1\n"
    "\n[SUBCATCHMENTS]\n"
    "S1  RG1  J1  1.0  20  100  1.0  0\n"
    "S2  RG1  J1  1.0  20  100  1.0  0\n"
    "\n[SUBAREAS]\n"
    "S1  0.015  0.15  1.5  5.0  25  OUTLET\n"
    "S2  0.015  0.15  1.5  5.0  25  OUTLET\n"
    "\n[INFILTRATION]\n"
    "S1  75  6  4  7  0\n"
    "S2  75  6  4  7  0\n"
    "\n[AQUIFERS]\n"
    ";;Name Por WP FC Ksat Kslope Tslope ETu ETs Seep Ebot Egw Umc\n"
    "AQ1  0.40  0.08  0.20  10.4  10  0.015  0.35  1.0  0.025  115.07  122.07  0.20\n"
    "\n[GROUNDWATER]\n"
    ";;Sub Aq Node Esurf A1 B1 A2 B2 A3 Dsw Egwt Ebot Egw Umc\n"
    "S1  AQ1  J1  124.94  0.00015  1  0  0  0  0  123.31  *  121.0  0.25\n"
    "S2  AQ1  J1  124.94  0.00015  1  0  0  0  0  *  *  *  *\n"
    "\n[TIMESERIES]\n"
    "TS1  01/01/2026 00:00 40.0\n"
    "TS1  01/01/2026 01:00 0.0\n"
    "\n[JUNCTIONS]\n"
    "J1  120.0  3.0  0  0  0\n"
    "\n[OUTFALLS]\n"
    "O1  119.0  FREE\n"
    "\n[CONDUITS]\n"
    "C1  J1  O1  50.0  0.013  0  0\n"
    "\n[XSECTIONS]\n"
    "C1  CIRCULAR  0.5  0  0  0  1\n";

class GroundwaterInitSI : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    void TearDown() override { if (engine_) swmm_engine_destroy(engine_); }
    openswmm::SWMMEngine& eng() { return *static_cast<openswmm::SWMMEngine*>(engine_); }

    void open_and_start() {
        const fs::path dir = "groundwater_init";
        fs::create_directories(dir);
        const auto inp = dir / "gw_init_si.inp";
        { std::ofstream f(inp, std::ios::trunc); f << kInp; }
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, inp.string().c_str(),
                                   (dir / "gw_init_si.rpt").string().c_str(),
                                   (dir / "gw_init_si.out").string().c_str(),
                                   nullptr), SWMM_OK)
            << swmm_get_last_error_msg(engine_);
        ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK)
            << swmm_get_last_error_msg(engine_);
        ASSERT_EQ(swmm_engine_start(engine_, 0), SWMM_OK)
            << swmm_get_last_error_msg(engine_);
    }
};

TEST_F(GroundwaterInitSI, InitialStateUsesBottomAndOverrides) {
    open_and_start();
    const auto& gw = eng().gwSolver().state();
    const double tol = 1e-6;

    // S1: Egwt 123.31, Ebot from aquifer (115.07), Egw override 121.0, Umc 0.25
    EXPECT_NEAR(gw.bottom_elev[0], 115.07 / kFt, tol);
    EXPECT_NEAR(gw.total_depth[0], (124.94 - 115.07) / kFt, tol);
    EXPECT_NEAR(gw.lower_depth[0], (121.0 - 115.07) / kFt, tol);
    EXPECT_NEAR(gw.theta[0], 0.25, tol);
    EXPECT_NEAR(gw.h_star[0], (123.31 - 115.07) / kFt, tol);

    // S2: everything from the aquifer; Egwt omitted -> node invert (120 m)
    EXPECT_NEAR(gw.lower_depth[1], (122.07 - 115.07) / kFt, tol);
    EXPECT_NEAR(gw.theta[1], 0.20, tol);
    EXPECT_NEAR(gw.h_star[1], (120.0 - 115.07) / kFt, tol);

    // Water table below ground -> the upper zone has room, so infiltration
    // must not be capped at zero. The per-step cap may be unset ("no cap",
    // DBL_MAX) until the first groundwater step or already positive; either
    // is correct, zero is the defect.
    EXPECT_GT(gw.total_depth[0] - gw.lower_depth[0], 0.0);
    EXPECT_GT(gw.total_depth[1] - gw.lower_depth[1], 0.0);
    for (std::size_t i : {std::size_t{0}, std::size_t{1}}) {
        const double cap = eng().context().subcatches.gw_max_infil_vol[i];
        EXPECT_TRUE(cap > 0.0) << "zero infiltration cap at start for subcatchment " << i;
    }
}

TEST_F(GroundwaterInitSI, RunKeepsContinuityAndInfiltrates) {
    open_and_start();
    double elapsed = 0.0;
    for (;;) {
        ASSERT_EQ(swmm_engine_step(engine_, &elapsed), SWMM_OK)
            << swmm_get_last_error_msg(engine_);
        if (elapsed <= 0.0) break;
    }
    ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);

    const auto& mb = eng().context().mass_balance;
    // Initial storage (ft3 over 2 ha): S1 0.25*3.94 m + 0.40*5.93 m,
    // S2 0.20*2.87 m + 0.40*7.0 m — independent of the length unit.
    const double area_m2 = 1.0e4;
    const double s1 = 0.25 * (124.94 - 121.0) + 0.40 * (121.0 - 115.07);
    const double s2 = 0.20 * (124.94 - 122.07) + 0.40 * (122.07 - 115.07);
    const double init_ft3 = (s1 + s2) * area_m2 / (kFt * kFt * kFt);
    EXPECT_NEAR(mb.gw_init_storage / init_ft3, 1.0, 1e-3);
    EXPECT_GT(mb.gw_infil, 0.0) << "pervious infiltration must reach the aquifer";
}

} // namespace
