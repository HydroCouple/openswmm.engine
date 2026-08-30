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
 * @file test_fv_tpa_closure.cpp
 * @brief Gates for the FV two-component pressure approach (issue #156 P4).
 *
 * @details Kernel algebra of the TPA branch (round-trip through ΔA < 0,
 *          continuity with the slot line at the crown), option inertness
 *          (A-G5 analog), free-surface bit-identity, and the physics the
 *          closure exists for: a sealed siphon sustains sub-atmospheric
 *          full-pipe flow under TPA where the slot closure vents the crown
 *          and breaks the siphon (A-G4 analog), while an OPEN junction at
 *          the hump lets TPA vent and the siphon break again (the venting
 *          rule, A2). Plans: TPA_TWO_COMPONENT_PRESSURE_PLAN.md §3,
 *          MIXED_FLOW_CLOSURES_TPA_UF_PLAN_2026-08-29.md §2.2.
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

#include "../../src/engine/hydraulics/fv/FvKernels.hpp"
#include "../../src/engine/hydraulics/fv/NetworkMeshData.hpp"

namespace fs = std::filesystem;
namespace k = openswmm::fv::kernels;
using openswmm::fv::FvGeometry;

// ===========================================================================
// Kernel algebra
// ===========================================================================

namespace {

FvGeometry tpaGeom() {
    FvGeometry g{};
    g.y_full   = 1.0;
    g.a_full   = 0.7853981633974483;   // 1-ft circle
    g.y_crown  = 0.985257;
    g.t_slot   = 0.02;
    g.a_crown  = g.a_full + 0.5 * g.t_slot * (g.y_full - g.y_crown);
    g.i1_crown = 0.3927;               // representative; algebra-only tests
    g.r_full   = 0.25;
    return g;
}

} // namespace

TEST(TpaKernel, RoundTripBothSignsOfDeltaA) {
    const FvGeometry g = tpaGeom();
    for (const double hs : {-3.0, -0.5, -1e-6, 0.0, 1e-6, 0.5, 12.0}) {
        const double h = g.y_full + hs;
        const double a = k::tpaAreaOfDepth(g, h);
        EXPECT_NEAR(k::tpaDepthOfArea(g, a), h, 1e-12) << "hs=" << hs;
    }
}

TEST(TpaKernel, ContinuousWithSlotLineAtCrown) {
    // At A = a_crown both branches give h = y_full and I1 = i1_crown; the
    // pair (free table ↔ TPA line) switches without a state jump.
    const FvGeometry g = tpaGeom();
    EXPECT_NEAR(k::tpaDepthOfArea(g, g.a_crown), g.y_full, 1e-14);
    EXPECT_NEAR(k::tpaAreaOfDepth(g, g.y_full), g.a_crown, 1e-14);
    EXPECT_NEAR(k::tpaI1OfDepth(g, g.y_full), g.i1_crown, 1e-14);
}

TEST(TpaKernel, I1DropsQuadraticSlotTerm) {
    // Paper Eq. 12b: I = A_pipe·(hc + hs) — linear in hs. The slot's
    // ½·t_slot·d² numerical-storage pressure must be absent.
    const FvGeometry g = tpaGeom();
    const double d = 7.0;
    const double i1_tpa = k::tpaI1OfDepth(g, g.y_full + d);
    EXPECT_NEAR(i1_tpa - g.i1_crown, g.a_crown * d, 1e-12);
    // and symmetric for hs < 0 (the new half):
    EXPECT_NEAR(g.i1_crown - k::tpaI1OfDepth(g, g.y_full - d),
                g.a_crown * d, 1e-12);
}

TEST(TpaKernel, NegativeDeltaAShrinksNotDries) {
    // "Shrinkage" of the section: ΔA < 0 keeps a near-full area, and the
    // celerity stays acoustic (√(gA/t_slot)), not shallow-water.
    const FvGeometry g = tpaGeom();
    const double a = k::tpaAreaOfDepth(g, g.y_full - 2.0);   // hs = -2 ft
    EXPECT_GT(a, 0.9 * g.a_full);
    const double c = k::celerity(a, g.t_slot);
    EXPECT_GT(c, 30.0);   // ~√(32.2·0.75/0.02) ≈ 35 ft/s for this geometry
}

// ===========================================================================
// System level
// ===========================================================================

namespace {

const char* kOutDir = "fv_tpa_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

// Sealed siphon: supply tank -> up-and-over 1-ft pipe (hump raised above the
// downstream hydraulic grade) -> receiving tank, ends vented, hump sealed.
// The hump interior is a VIRTUAL junction (sealed splice). `hump_open`
// replaces it with an ordinary junction (SUR_DEPTH 0 => atmosphere contact:
// the venting rule must break the siphon exactly like the slot closure).
// Supply starts 2 ft above the hump crown so the system primes itself; once
// primed, the falling supply level pulls the hump sub-atmospheric.
std::string siphonModel(const std::string& closure, bool hump_open) {
    std::ostringstream ss;
    ss << "[OPTIONS]\n"
          "FLOW_UNITS           CFS\n"
          "FLOW_ROUTING         FV\n"
          "START_DATE           01/01/2026\n"
          "START_TIME           00:00:00\n"
          "END_DATE             01/01/2026\n"
          "END_TIME             00:30:00\n"
          "REPORT_STEP          2\n"
          "ROUTING_STEP         0.5\n"
          "FV_MIN_CELLS         6\n"
          "FV_SLOT_CELERITY     100\n"
       << closure
       << "\n[STORAGE]\n"
          ";;Name Elev MaxDepth InitDepth Shape      A1 A2 A0\n"
          "SUP    0.0  20.0     8.0       FUNCTIONAL 0  0  50\n"
          "REC    0.0  20.0     0.5       FUNCTIONAL 0  0  50\n";
    if (hump_open) {
        ss << "\n[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth SurDepth\n"
              "HMP    6.0  4.0      0.0       0\n";
    } else {
        ss << "\n[VIRTUAL_JUNCTIONS]\n;;Name Elev\nHMP    6.0\n";
    }
    ss << "\n[OUTFALLS]\n;;Name Elev Type Gated\nO_OUT  0.0  FREE NO\n"
          "\n[WEIRS]\n;;Name From To    Type       CrestHt Cd\n"
          "W_OVF  REC  O_OUT TRANSVERSE 6.5     3.33\n"
          "\n[CONDUITS]\n;;Name From To  Length N     Z1 Z2\n"
          "C_UP   SUP  HMP 60.0   0.013 0  0\n"
          "C_DN   HMP  REC 60.0   0.013 0  0\n"
          "\n[XSECTIONS]\n;;Link Shape    G1  G2 G3 G4 Barrels\n"
          "C_UP   CIRCULAR 1.0 0  0  0  1\n"
          "C_DN   CIRCULAR 1.0 0  0  0  1\n"
          "W_OVF  RECT_OPEN 3.0 1.0 0 0\n";
    return ss.str();
}

struct SiphonResult {
    double late_flow = 0.0;    ///< mean C_DN flow over the last 10 min (cfs)
    double min_hump_head = 1e9;///< min piezometric head at HMP (ft, abs elev)
    bool ok = false;
};

SiphonResult runSiphon(const std::string& base, const std::string& deck) {
    SiphonResult r;
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
    const int hmp = swmm_node_index(e, "HMP");
    const int cdn = swmm_link_index(e, "C_DN");
    EXPECT_GE(hmp, 0);
    EXPECT_GE(cdn, 0);
    double elapsed = 0.0, sum_q = 0.0;
    long n_q = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            ADD_FAILURE() << "step failed: " << swmm_get_last_error_msg(e);
            break;
        }
        const double t_s = elapsed * 86400.0;
        double q = 0.0, d = 0.0;
        swmm_link_get_flow(e, cdn, &q);
        swmm_node_get_depth(e, hmp, &d);
        r.min_hump_head = std::min(r.min_hump_head, 6.0 + d);  // invert 6 ft
        if (t_s >= 1200.0) { sum_q += q; ++n_q; }   // last 10 of 30 min
    } while (elapsed > 0.0);
    swmm_engine_end(e);
    swmm_engine_destroy(e);
    if (n_q > 0) r.late_flow = sum_q / static_cast<double>(n_q);
    r.ok = true;
    return r;
}

std::string fileBytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const char* kTpa = "FV_PRESSURE_CLOSURE  TPA\n";

} // namespace

TEST(FvTpa, AbsentEqualsExplicitSlot) {
    // A-G5 analog: option absent ≡ FV_PRESSURE_CLOSURE SLOT, byte-for-byte.
    const auto a = runSiphon("tpa_absent", siphonModel("", false));
    const auto b = runSiphon("tpa_slot",
                             siphonModel("FV_PRESSURE_CLOSURE  SLOT\n", false));
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_TRUE(fileBytes(outPath("tpa_absent.out")) ==
                fileBytes(outPath("tpa_slot.out")));
}

TEST(FvTpa, FreeSurfaceDeckIsBitIdentical) {
    // A deck that never pressurizes must be bit-identical under TPA: no cell
    // ever flags, and unflagged cells evaluate the unchanged table closure.
    // (Supply below the hump crown => the siphon never primes.)
    std::string base_deck = siphonModel("", false);
    std::string tpa_deck  = siphonModel(kTpa, false);
    for (auto* d : {&base_deck, &tpa_deck}) {
        const auto pos = d->find("SUP    0.0  20.0     8.0");
        ASSERT_NE(pos, std::string::npos);
        d->replace(pos, std::string("SUP    0.0  20.0     8.0").size(),
                   "SUP    0.0  20.0     3.0");
    }
    const auto a = runSiphon("tpa_fs_base", base_deck);
    const auto b = runSiphon("tpa_fs_on", tpa_deck);
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_TRUE(fileBytes(outPath("tpa_fs_base.out")) ==
                fileBytes(outPath("tpa_fs_on.out")));
}

TEST(FvTpa, SealedSiphonSustainsSubAtmosphericFlow) {
    // THE discriminator (A-G4 analog). Sealed hump (virtual junction):
    //  - TPA: the pipe stays full over the hump at sub-atmospheric head and
    //    the siphon keeps delivering;
    //  - SLOT: when the hump head drops below the crown the closure reverts
    //    to free surface — the siphon breaks.
    const auto slot = runSiphon("tpa_siphon_slot", siphonModel("", false));
    const auto tpa  = runSiphon("tpa_siphon_tpa", siphonModel(kTpa, false));
    ASSERT_TRUE(slot.ok && tpa.ok);
    // TPA reaches sub-atmospheric pressure at the hump (piezometric head
    // below the hump crown elevation 7.0 ft while flowing):
    EXPECT_LT(tpa.min_hump_head, 6.9);
    // and sustains materially more late siphon flow than the slot run:
    EXPECT_GT(tpa.late_flow, slot.late_flow + 0.05)
        << "TPA late " << tpa.late_flow << " vs SLOT " << slot.late_flow;
}

TEST(FvTpa, OpenHumpVentsAndBreaksTheSiphon) {
    // The venting rule (A2): with an ORDINARY junction at the hump
    // (SUR_DEPTH 0 = atmosphere contact) the flag must clear when the head
    // reaches the crown, and TPA behaves like the slot closure — no
    // sub-atmospheric siphon.
    const auto tpa_open  = runSiphon("tpa_hump_open",
                                     siphonModel(kTpa, true));
    const auto slot_open = runSiphon("tpa_hump_open_slot",
                                     siphonModel("", true));
    ASSERT_TRUE(tpa_open.ok && slot_open.ok);
    EXPECT_LT(std::fabs(tpa_open.late_flow - slot_open.late_flow), 0.05)
        << "vented TPA " << tpa_open.late_flow
        << " vs vented SLOT " << slot_open.late_flow;
}

TEST(FvTpa, SealedSiphonMassConserved) {
    // The flag flips regime, not mass: continuity on the TPA siphon run must
    // stay tight (checked via the report's routing continuity line).
    const auto tpa = runSiphon("tpa_mass", siphonModel(kTpa, false));
    ASSERT_TRUE(tpa.ok);
    std::ifstream f(outPath("tpa_mass.rpt"));
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string rpt = ss.str();
    const auto pos = rpt.find("Continuity Error");
    ASSERT_NE(pos, std::string::npos) << "no continuity line in report";
    // Parse the LAST "(%)"-style number on the flow-routing continuity line.
    const auto line_end = rpt.find('\n', pos);
    std::string line = rpt.substr(pos, line_end - pos);
    double pct = 1e9;
    for (auto i = line.size(); i-- > 0;) {
        if ((line[i] >= '0' && line[i] <= '9') || line[i] == '.' ||
            line[i] == '-') {
            auto j = i + 1;
            while (i > 0 && ((line[i - 1] >= '0' && line[i - 1] <= '9') ||
                             line[i - 1] == '.' || line[i - 1] == '-'))
                --i;
            pct = std::stod(line.substr(i, j - i));
            break;
        }
    }
    EXPECT_LT(std::fabs(pct), 2.0) << "routing continuity: " << line;
}
