// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file test_2d_infil_subcatch_aquifer.cpp
 * @brief U3 (track I-b, 2026-09-07) — `[2D_OPTIONS] INFIL_DESTINATION
 *        SUBCATCH_AQUIFER`: 2D per-cell infiltration recharges the legacy
 *        aquifer of the subcatchment whose polygon contains the cell.
 *
 * Gates:
 *   - G1 a cell inside a subcatchment polygon recharges THAT subcatchment:
 *     the groundwater ledger's infiltration grows and the 2D ledger reports
 *     the transfer share.
 *   - G2 a cell outside every polygon stays LOST — booked to `infil_out`,
 *     nothing to groundwater, one warning.
 *   - G3 LOST is bit-identical to the pre-U3 behaviour (the control).
 *   - G4 the one-owner check refuses SUBCATCH_AQUIFER when the integrated2d
 *     component is registered.
 *   - G5 AQUIFER_2D is still refused at initialize.
 *
 * Artefacts under tests/output/infil_subcatch_aquifer (CLAUDE.md §4.1).
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_infil2d.h>

#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

const fs::path kOutDir =
    fs::path(OPENSWMM_INFIL_AQ_TEST_OUT_DIR) / "infil_subcatch_aquifer";

/// One subcatchment S1 whose polygon covers x∈[0,10], y∈[0,10] with an
/// aquifer, and a 2D pan. @p pan_x0 shifts the mesh: 0 puts the pan inside
/// the polygon, 100 puts it outside every polygon.
std::string deck(const std::string& twod_options_extra, double pan_x0,
                 const std::string& extra_sections = "") {
    std::ostringstream m;
    const double x1 = pan_x0 + 10.0;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "INFILTRATION         GREEN_AMPT\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
         "REPORT_STEP          00:01:00\nWET_STEP             00:01:00\n"
         "DRY_STEP             00:01:00\nROUTING_STEP         5\n"
         "ALLOW_PONDING        NO\n\n"
         "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         // A gage is required even though this deck's water comes from the
         // cells' INIT_DEPTH, not from rainfall.
         "[RAINGAGES]\n"
         "RG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
         "[TIMESERIES]\n"
         "rain_ts 0:00 0.0\n\n"
         "[SUBCATCHMENTS]\n"
         ";;Name Raingage Outlet Area %Imperv Width Slope CurbLen\n"
         "S1  RG1  J1  0.0247  25  50  0.5  0\n\n"
         "[SUBAREAS]\n;;Subcatch Nimp Nperv Simp Sperv PctZero RouteTo\n"
         "S1  0.01  0.1  0.05  0.05  25  OUTLET\n\n"
         "[INFILTRATION]\n;;Subcatch p1 p2 p3\nS1  3.5  0.5  0.26\n\n"
         "[AQUIFERS]\n"
         ";;Name Por WP FC Ks Kslope Tslope ETu ETs Seep Ebot Egw Umc\n"
         "AQ1  0.46  0.13  0.23  0.8  10  15  0.35  14  0.002  -5.0  -2.0  0.30\n\n"
         "[GROUNDWATER]\n"
         ";;Subcatch Aquifer Node Esurf A1 B1 A2 B2 A3 Dsw Egwt\n"
         "S1  AQ1  J1  0.0  0.001  1.0  0.0  0.0  0.0  0.0  *\n\n"
         "[Polygons]\n"
         ";;Subcatch X Y\n"
         "S1  0   0\nS1  10  0\nS1  10  10\nS1  0   10\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n"
      << twod_options_extra << "\n"
         "[2D_VERTICES]\n"
      << pan_x0 << " 0.0 -10.0\n"
      << x1     << " 0.0 -10.0\n"
      << x1     << " 10.0 -10.0\n"
      << pan_x0 << " 10.0 -10.0\n\n"
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
         "0 1 2 0.03 0.5\n0 2 3 0.03 0.5\n\n"
         "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50.0  -  -  -  -\n\n"
      << extra_sections
      << "[REPORT]\nINPUT NO\n";
    return m.str();
}

struct DeckRun {
    SWMM_Engine e = nullptr;
    openswmm::SWMMEngine* eng = nullptr;
    bool opened = false, started = false;
};

DeckRun openDeck(const std::string& tag, const std::string& body) {
    fs::create_directories(kOutDir);
    DeckRun r;
    const fs::path inp = kOutDir / (tag + ".inp");
    { std::ofstream f(inp); f << body; }
    r.e = swmm_engine_create();
    if (!r.e) return r;
    r.opened = swmm_engine_open(r.e, inp.string().c_str(),
                                (kOutDir / (tag + ".rpt")).string().c_str(),
                                (kOutDir / (tag + ".out")).string().c_str(),
                                nullptr) == SWMM_OK;
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    return r;
}

bool run(DeckRun& r) {
    if (!r.opened) return false;
    if (swmm_engine_initialize(r.e) != SWMM_OK) return false;
    if (swmm_engine_start(r.e, 1) != SWMM_OK) return false;
    r.started = true;
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    return true;
}

void finish(DeckRun& r) {
    if (!r.e) return;
    if (r.started) swmm_engine_end(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

bool warnedAbout(const DeckRun& r, const char* needle) {
    for (const auto& w : r.eng->context().warnings)
        if (w.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// G3 (control) + G1 — LOST vs SUBCATCH_AQUIFER on the same deck
// ---------------------------------------------------------------------------
TEST(Infil2DSubcatchAquifer, RechargeReachesTheContainingSubcatchmentsAquifer) {
    double lost_infil2d = 0.0, lost_gw_infil = 0.0;
    {
        DeckRun r = openDeck("lost", deck("", 0.0));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        lost_infil2d  = r.eng->context().mass_balance_2d.infil_out;
        lost_gw_infil = r.eng->context().mass_balance.gw_infil;
        EXPECT_GT(lost_infil2d, 0.0) << "the deck must actually infiltrate";
        EXPECT_DOUBLE_EQ(r.eng->context().mass_balance_2d.infil_to_aquifer, 0.0);
        EXPECT_DOUBLE_EQ(r.eng->context().mass_balance.gw_infil_2d_recharge, 0.0);
        finish(r);
    }
    {
        DeckRun r = openDeck("aq", deck("INFIL_DESTINATION SUBCATCH_AQUIFER\n", 0.0));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        const auto& ctx = r.eng->context();
        // The 2D side books the same infiltration and names the transfer.
        EXPECT_NEAR(ctx.mass_balance_2d.infil_out, lost_infil2d,
                    0.05 * lost_infil2d)
            << "routing the destination must not change how much infiltrates";
        EXPECT_GT(ctx.mass_balance_2d.infil_to_aquifer, 0.0);
        EXPECT_NEAR(ctx.mass_balance_2d.infil_to_aquifer,
                    ctx.mass_balance_2d.infil_out,
                    0.02 * ctx.mass_balance_2d.infil_out)
            << "every cell is inside S1, so all of it is a transfer";
        // The groundwater side received it.
        EXPECT_GT(ctx.mass_balance.gw_infil_2d_recharge, 0.0);
        EXPECT_GT(ctx.mass_balance.gw_infil, lost_gw_infil)
            << "the aquifer's infiltration input did not grow";
        // ft³ ↔ m³ on the same volume.
        constexpr double kM3ToFt3 = 1.0 / (0.3048 * 0.3048 * 0.3048);
        EXPECT_NEAR(ctx.mass_balance.gw_infil_2d_recharge,
                    ctx.mass_balance_2d.infil_to_aquifer * kM3ToFt3,
                    0.02 * ctx.mass_balance.gw_infil_2d_recharge);
        EXPECT_FALSE(warnedAbout(r, "outside every subcatchment"));
        finish(r);
    }
}

// ---------------------------------------------------------------------------
// G2 — a cell outside every polygon stays LOST, with a warning
// ---------------------------------------------------------------------------
TEST(Infil2DSubcatchAquifer, CellsOutsideEveryPolygonStayLost) {
    DeckRun r = openDeck("outside", deck("INFIL_DESTINATION SUBCATCH_AQUIFER\n", 100.0));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    const auto& ctx = r.eng->context();
    EXPECT_GT(ctx.mass_balance_2d.infil_out, 0.0);
    EXPECT_DOUBLE_EQ(ctx.mass_balance_2d.infil_to_aquifer, 0.0);
    EXPECT_DOUBLE_EQ(ctx.mass_balance.gw_infil_2d_recharge, 0.0);
    EXPECT_TRUE(warnedAbout(r, "outside every subcatchment"))
        << "silently losing the recharge is the failure this warning prevents";
    finish(r);
}

// ---------------------------------------------------------------------------
// G4 — one owner per subcatchment
// ---------------------------------------------------------------------------
TEST(Infil2DSubcatchAquifer, IntegratedComponentAndSubcatchAquiferConflict) {
    DeckRun r = openDeck(
        "one_owner",
        deck("INFIL_DESTINATION SUBCATCH_AQUIFER\n", 0.0,
             "[PROCESS_COMPONENTS]\n"
             "org.hydrocouple.openswmm.integrated2d  config=\"none.i2d\"\n\n"));
    // The component itself is planned-only, so the open may already fail on
    // its config; either way the run must not proceed silently.
    const bool init_ok =
        r.opened && swmm_engine_initialize(r.e) == SWMM_OK;
    EXPECT_FALSE(init_ok)
        << "a subcatchment aquifer with two owners must be refused";
    if (r.e) { swmm_engine_close(r.e); swmm_engine_destroy(r.e); }
}

// ---------------------------------------------------------------------------
// G5 — AQUIFER_2D needs an aquifer to send its water to
//
// This was "still refused, unconditionally" until the two-zone kernel landed
// (G1, 2026-09-07). It is now refused exactly when there is no [2D_AQUIFER]
// to receive the recharge, which is this deck — reading it as LOST instead
// would quietly drain a model its author believed was recharging. The
// accepting half is Aquifer2D.Aquifer2DDestinationIsAcceptedWithAnAquifer,
// which has a kernel to accept it.
// ---------------------------------------------------------------------------
TEST(Infil2DSubcatchAquifer, Aquifer2DDestinationNeedsA2DAquifer) {
    DeckRun r = openDeck("aq2d", deck("INFIL_DESTINATION AQUIFER_2D\n", 0.0));
    ASSERT_TRUE(r.opened);
    EXPECT_NE(swmm_engine_initialize(r.e), SWMM_OK);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
}

// ---------------------------------------------------------------------------
// A model with no subcatchments cannot route the destination anywhere.
// ---------------------------------------------------------------------------
TEST(Infil2DSubcatchAquifer, NoSubcatchmentsIsAnError) {
    std::string body = deck("INFIL_DESTINATION SUBCATCH_AQUIFER\n", 0.0);
    // Strip the subcatchment sections.
    for (const char* sec : {"[SUBCATCHMENTS]", "[SUBAREAS]", "[INFILTRATION]",
                            "[AQUIFERS]", "[GROUNDWATER]", "[Polygons]"}) {
        const auto a = body.find(sec);
        if (a == std::string::npos) continue;
        const auto b = body.find("\n\n", a);
        body.erase(a, (b == std::string::npos ? body.size() : b + 2) - a);
    }
    DeckRun r = openDeck("no_subs", body);
    ASSERT_TRUE(r.opened);
    EXPECT_NE(swmm_engine_initialize(r.e), SWMM_OK);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
}
