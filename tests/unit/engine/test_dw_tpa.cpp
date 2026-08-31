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
 * @file test_dw_tpa.cpp
 * @brief Gates for SURCHARGE_METHOD TPA in the dynamic wave solver
 *        (issue #156 Phase 5; TPA plan §B1–B4).
 *
 * @details DW's TPA is the pragmatic port: constant-width slot above the
 *          crown plus a per-conduit sub-atmospheric LATCH below it. Node
 *          depths in DW are floored at zero, so the representable vacuum is
 *          bounded by (invert − crown) at each node — the latch's testable
 *          value is sealed sub-CROWN behavior: the conduit keeps full-pipe
 *          geometry (no free-surface flip), the run stays smooth, venting
 *          changes the answer, repeated crown crossings do not chatter the
 *          mass balance, and above the crown TPA parallels SLOT.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_massbalance.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "dw_tpa_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

// Over-the-top drawdown under DYNWAVE (the FV fixture's sibling): two tanks,
// apex junction at invert 6 (crown 7), initially full at head 8; a mild
// withdrawal takes the shared head into the (invert, crown) band where DW can
// represent the sealed sub-crown state. apex_sealed toggles SUR_DEPTH.
std::string drawdownModel(const std::string& method, bool apex_sealed,
                          double withdraw_cfs = 0.3,
                          const std::string& extra = "") {
    std::ostringstream ss;
    ss << "[OPTIONS]\n"
          "FLOW_UNITS           CFS\n"
          "FLOW_ROUTING         DYNWAVE\n"
          "SURCHARGE_METHOD     " << method << "\n"
          "TPA_CELERITY         100\n"
          "START_DATE           01/01/2026\n"
          "START_TIME           00:00:00\n"
          "END_DATE             01/01/2026\n"
          "END_TIME             00:15:00\n"
          "REPORT_STEP          2\n"
          "ROUTING_STEP         0.5\n"
          "INERTIAL_DAMPING     PARTIAL\n"
       << extra
       << "\n[STORAGE]\n"
          ";;Name Elev MaxDepth InitDepth Shape      A1 A2 A0\n"
          "TA     0.0  20.0     8.0       FUNCTIONAL 0  0  100\n"
          "TB     0.0  20.0     8.0       FUNCTIONAL 0  0  100\n"
       << "\n[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth SurDepth\n"
          "JS     6.0  2.0      2.0       " << (apex_sealed ? "30" : "0") << "\n"
       << "\n[OUTFALLS]\n;;Name Elev Type Gated\nO_OUT  0.0  FREE NO\n"
          "\n[WEIRS]\n;;Name From To    Type       CrestHt Cd\n"
          "W_OVF  TB   O_OUT TRANSVERSE 19.5    3.33\n"
          "\n[CONDUITS]\n;;Name From To Length N     Z1 Z2\n"
          "C_UP   TA   JS 60.0   0.013 0  0\n"
          "C_DN   JS   TB 60.0   0.013 0  0\n"
          "\n[XSECTIONS]\n;;Link Shape    G1  G2 G3 G4 Barrels\n"
          "C_UP   CIRCULAR 1.0 0  0  0  1\n"
          "C_DN   CIRCULAR 1.0 0  0  0  1\n"
          "W_OVF  RECT_OPEN 3.0 1.0 0 0\n"
          "\n[INFLOWS]\n;;Node Constituent Tseries Type Mfactor Sfactor Baseline\n"
          "TB     FLOW        \"\"      FLOW 1.0     1.0     -"
       << withdraw_cfs << "\n";
    return ss.str();
}

struct DwRun {
    double final_ta = -1.0, final_tb = -1.0;
    double min_apex_head = 1e9;
    double max_abs_q = 0.0;      ///< max |Q| C_DN over the run
    double max_dq = 0.0;         ///< max step-to-step |ΔQ| C_DN (smoothness)
    double cont_err = 1e9;
    bool ok = false;
};

DwRun runDeck(const std::string& base, const std::string& deck) {
    DwRun r;
    const std::string inp = outPath(base + ".inp");
    { std::ofstream f(inp); f << deck; }
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, inp.c_str(), outPath(base + ".rpt").c_str(),
                         outPath(base + ".out").c_str(), nullptr) != 0) {
        ADD_FAILURE() << "open failed: " << swmm_get_last_error_msg(e);
        swmm_engine_destroy(e);
        return r;
    }
    EXPECT_EQ(swmm_engine_initialize(e), 0);
    EXPECT_EQ(swmm_engine_start(e, 1), 0);
    const int js = swmm_node_index(e, "JS");
    const int ta = swmm_node_index(e, "TA");
    const int tb = swmm_node_index(e, "TB");
    const int cd = swmm_link_index(e, "C_DN");
    EXPECT_GE(js, 0);
    double elapsed = 0.0, q_prev = 0.0;
    bool first = true;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            ADD_FAILURE() << "step failed: " << swmm_get_last_error_msg(e);
            break;
        }
        double h = 0.0, q = 0.0;
        swmm_node_get_head(e, js, &h);
        swmm_link_get_flow(e, cd, &q);
        r.min_apex_head = std::min(r.min_apex_head, h);
        r.max_abs_q = std::max(r.max_abs_q, std::fabs(q));
        if (!first) r.max_dq = std::max(r.max_dq, std::fabs(q - q_prev));
        q_prev = q;
        first = false;
    } while (elapsed > 0.0);
    swmm_node_get_depth(e, ta, &r.final_ta);
    swmm_node_get_depth(e, tb, &r.final_tb);
    swmm_get_routing_continuity_error(e, &r.cont_err);
    swmm_engine_end(e);
    swmm_engine_destroy(e);
    r.ok = true;
    return r;
}

} // namespace

TEST(DwTpa, OptionRoundTrip) {
    // Parse + API surface for the new enum value and celerity.
    const std::string inp = outPath("rt.inp");
    { std::ofstream f(inp); f << drawdownModel("TPA", true); }
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), outPath("rt.rpt").c_str(),
                               outPath("rt.out").c_str(), nullptr), 0)
        << swmm_get_last_error_msg(e);
    char buf[64];
    ASSERT_EQ(swmm_options_get(e, "SURCHARGE_METHOD", buf, sizeof buf), 0);
    EXPECT_STREQ(buf, "TPA");
    ASSERT_EQ(swmm_options_get(e, "TPA_CELERITY", buf, sizeof buf), 0);
    EXPECT_NEAR(std::stod(buf), 100.0, 1e-9);
    EXPECT_EQ(swmm_options_set(e, "SURCHARGE_METHOD", "BOGUS"),
              SWMM_ERR_BADPARAM);
    swmm_engine_destroy(e);
}

TEST(DwTpa, SealedSubCrownStaysSmooth) {
    // B-G2 flavor: the shared head settles inside (invert 6, crown 7). The
    // latched conduits must keep conveying smoothly through the sealed apex
    // (no free-surface flip blow-up, no NaN, tight continuity) and the apex
    // head must genuinely drop below the crown while the tanks stay coupled.
    const auto r = runDeck("dw_tpa_sealed", drawdownModel("TPA", true));
    ASSERT_TRUE(r.ok);
    EXPECT_LT(r.min_apex_head, 6.95) << "apex never went sub-crown";
    EXPECT_GE(r.min_apex_head, 5.99)
        << "DW node depths are floored — sub-invert should be impossible";
    EXPECT_LT(std::fabs(r.final_ta - r.final_tb), 0.75)
        << "tanks decoupled: TA " << r.final_ta << " TB " << r.final_tb;
    EXPECT_TRUE(std::isfinite(r.max_abs_q));
    EXPECT_LT(r.max_dq, 5.0) << "step-to-step flow jumps look like chatter";
    EXPECT_LT(std::fabs(r.cont_err), 2.0);
}

TEST(DwTpa, VentingChangesTheAnswer) {
    // The latch must DO something: the sealed and vented (SUR_DEPTH 0) runs
    // must differ measurably once the head is sub-crown — under venting the
    // conduit ends revert to ordinary free-surface branches at the apex.
    const auto sealed = runDeck("dw_tpa_sealed2", drawdownModel("TPA", true));
    const auto vented = runDeck("dw_tpa_vented", drawdownModel("TPA", false));
    ASSERT_TRUE(sealed.ok && vented.ok);
    const double d_final =
        std::fabs(sealed.final_ta - vented.final_ta) +
        std::fabs(sealed.min_apex_head - vented.min_apex_head);
    EXPECT_GT(d_final, 1e-3)
        << "sealed and vented runs are indistinguishable — the latch is inert";
    EXPECT_LT(std::fabs(vented.cont_err), 3.0);
}

TEST(DwTpa, CrownCrossingHysteresis) {
    // B-G4: a withdrawal/refill cycle drives the apex across the crown
    // repeatedly; the latch updates once per step and must not chatter the
    // mass balance. Oscillating forcing via a reversing baseline is not
    // expressible in one INFLOWS row, so cycle with a timeseries.
    std::string deck = drawdownModel("TPA", true, 0.0,
                                     "");
    deck += "\n[TIMESERIES]\n"
            ";;Name  Time  Value\n"
            "CYC     0:00  -1.0\n"
            "CYC     0:03  1.2\n"
            "CYC     0:06  -1.2\n"
            "CYC     0:09  1.2\n"
            "CYC     0:12  -1.2\n"
            "CYC     0:15  1.0\n";
    deck.replace(deck.find("TB     FLOW        \"\"      FLOW 1.0     1.0     -0\n"),
                 std::string("TB     FLOW        \"\"      FLOW 1.0     1.0     -0\n").size(),
                 "TB     FLOW        CYC     FLOW 1.0     1.0     0\n");
    const auto r = runDeck("dw_tpa_cyc", deck);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(std::isfinite(r.max_abs_q));
    EXPECT_LT(std::fabs(r.cont_err), 3.0)
        << "latch chatter is leaking mass: " << r.cont_err;
}

TEST(DwTpa, AboveCrownParallelsSlot) {
    // B-G1 flavor: with a positive INFLOW driving the whole system above the
    // crown, TPA is a constant-width slot — peak apex heads should track the
    // Sjoberg-slot run within a loose band (the widths differ by design).
    std::string tpa_deck  = drawdownModel("TPA", true, -0.8);   // inflow
    std::string slot_deck = drawdownModel("SLOT", true, -0.8);
    const auto t = runDeck("dw_tpa_above", tpa_deck);
    const auto s = runDeck("dw_slot_above", slot_deck);
    ASSERT_TRUE(t.ok && s.ok);
    EXPECT_LT(std::fabs(t.final_ta - s.final_ta), 0.5)
        << "TPA " << t.final_ta << " vs SLOT " << s.final_ta;
    EXPECT_LT(std::fabs(t.cont_err), 2.0);
}
