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
 * @file test_virtual_junction_quality.cpp
 * @brief Water-quality loads carried by a lateral inflow at a virtual
 *        junction (plans/VJ_LATERAL_INFLOW_PLAN_2026-09-04.md §E4), across
 *        every routing × quality-solver combination.
 *
 * @details J_IN feeds 1.0 cfs at 20 mg/L TSS; the midpoint node MID feeds
 *          0.5 cfs at 100 mg/L. At steady state the outlet conduit carries
 *          (1.0·20 + 0.5·100)/1.5 = 46.67 mg/L. Each combination is run with
 *          MID as a virtual junction and again as a regular junction (the
 *          reference); the two must agree. Under EULERIAN_ARD the virtual
 *          junction owns no mesh face, so before §E4 its node store received
 *          the load and the per-step resync destroyed it. All inputs and
 *          outputs land under ./virtual_junction_out/ (working dir is
 *          tests/unit/engine/data) — no temp files (CLAUDE.md §4.1).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_massbalance.h>
#include <openswmm/engine/openswmm_nodes.h>

#include "core/SWMMEngine.hpp"   // ctx.links.conc (no C getter for link quality)

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "virtual_junction_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

constexpr double kQin   = 1.0;    // cfs at J_IN
constexpr double kCin   = 20.0;   // mg/L at J_IN
constexpr double kQlat  = 0.5;    // cfs at MID
constexpr double kClat  = 100.0;  // mg/L at MID
constexpr double kCmix  = (kQin * kCin + kQlat * kClat) / (kQin + kQlat);

// J_IN -> C_UP -> MID -> C_DN -> O_OUT, 1 ft circular pipes on 1 % slopes,
// constant inflows via [INFLOWS] baselines.
std::string deck(bool virtual_mid, const std::string& routing,
                 const std::string& quality_solver) {
    std::string s =
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         " + routing + "\n";
    if (!quality_solver.empty())
        s += "QUALITY_SOLVER       " + quality_solver + "\n";
    s +=
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             01:00:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         1\n"
        "ALLOW_PONDING        NO\n"
        "\n"
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth\n"
        "J_IN    10.0  5.0\n";
    if (!virtual_mid) s += "MID     9.0   0.0\n";
    s += "\n";
    if (virtual_mid)
        s += "[VIRTUAL_JUNCTIONS]\n;;Name  Elev\nMID     9.0\n\n";
    s +=
        "[OUTFALLS]\n"
        ";;Name  Elev  Type  Gated\n"
        "O_OUT   8.0   FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name    From   To     Length  N      Z1  Z2\n"
        "C_UP      J_IN   MID    100.0   0.013  0   0\n"
        "C_DN      MID    O_OUT  100.0   0.013  0   0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link    Shape     G1   G2  G3  G4  Barrels\n"
        "C_UP      CIRCULAR  1.0  0   0   0   1\n"
        "C_DN      CIRCULAR  1.0  0   0   0   1\n"
        "\n"
        "[POLLUTANTS]\n"
        ";;Name  Units  Crain  Cgw  Crdii  Kdecay  SnowOnly  CoPollut  CoFrac  Cdwf  Cinit\n"
        "TSS     MG/L   0.0    0.0  0.0    0.0     NO        *         0.0     0.0   0.0\n"
        "\n"
        "[INFLOWS]\n"
        ";;Node  Constituent  Timeseries  Type    Mfactor  Sfactor  Baseline\n"
        "J_IN    FLOW         \"\"          FLOW    1.0      1.0      " + std::to_string(kQin) + "\n"
        "J_IN    TSS          \"\"          CONCEN  1.0      1.0      " + std::to_string(kCin) + "\n"
        "MID     FLOW         \"\"          FLOW    1.0      1.0      " + std::to_string(kQlat) + "\n"
        "MID     TSS          \"\"          CONCEN  1.0      1.0      " + std::to_string(kClat) + "\n"
        "\n"
        "[REPORT]\n"
        "CONTINUITY YES\n"
        "\n"
        "[COORDINATES]\n"
        ";;Node  X    Y\n"
        "J_IN    0.0    0.0\n"
        "MID     100.0  0.0\n"
        "O_OUT   200.0  0.0\n";
    return s;
}

struct QualProbe {
    double outlet_conc = -1.0;   // C_DN, mg/L, end of run
    double outlet_flow = 0.0;    // C_DN, cfs
    double qual_error  = 1.0;    // quality continuity error (fraction)
    bool   ran = false;
};

QualProbe run(const std::string& base, const std::string& text) {
    QualProbe p;
    const std::string inp = outPath(base + ".inp");
    {
        std::ofstream f(inp);
        f << text;
    }
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, inp.c_str(), outPath(base + ".rpt").c_str(),
                                    outPath(base + ".out").c_str(), nullptr);
    EXPECT_EQ(rc, 0) << base << ": " << swmm_get_last_error_msg(e);
    if (rc != 0) { swmm_engine_destroy(e); return p; }
    EXPECT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_start(e, 1), 0) << swmm_get_last_error_msg(e);
    double elapsed = 0.0;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            ADD_FAILURE() << base << " step failed: " << swmm_get_last_error_msg(e);
            swmm_engine_close(e);
            swmm_engine_destroy(e);
            return p;
        }
    } while (elapsed > 0.0);

    const int cdn = swmm_link_index(e, "C_DN");
    EXPECT_GE(cdn, 0);
    const auto& ctx = static_cast<openswmm::SWMMEngine*>(e)->context();
    EXPECT_EQ(ctx.n_pollutants(), 1);
    if (cdn >= 0) {
        p.outlet_conc = ctx.links.conc[static_cast<std::size_t>(cdn)];
        swmm_link_get_flow(e, cdn, &p.outlet_flow);
    }
    swmm_engine_end(e);
    swmm_get_quality_continuity_error(e, 0, &p.qual_error);
    p.ran = true;
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return p;
}

void lateralLoad(const std::string& tag, const std::string& routing,
                 const std::string& solver) {
    const QualProbe ref = run("vj_qual_ref_" + tag, deck(false, routing, solver));
    const QualProbe vj  = run("vj_qual_" + tag,     deck(true,  routing, solver));
    ASSERT_TRUE(ref.ran && vj.ran) << tag;

    // Both carry the whole 1.5 cfs. Under FV the published conduit flow is
    // a length-mean over its cells and the cell adjoining the splice holds
    // half the lateral, so C_DN reads (1.25 + 9·1.5)/10 = 1.475 on a 10-cell
    // conduit — the same bias a diverted degree-2 junction shows.
    EXPECT_NEAR(vj.outlet_flow, kQin + kQlat, 0.05) << tag;

    // Flow-weighted mix at the outlet, and parity with the regular junction.
    // The regular-junction reference under FV + EULERIAN_ARD sits ~2 % low
    // (its water is diverted into cells while its mass rides the node store
    // — a pre-existing degree-2 inconsistency), so it gets the looser band;
    // the virtual junction, whose water and mass take the same cell path,
    // is held to the tight one.
    EXPECT_NEAR(ref.outlet_conc, kCmix, 0.05 * kCmix) << tag << " (reference)";
    EXPECT_NEAR(vj.outlet_conc,  kCmix, 0.02 * kCmix) << tag << " (virtual)";
    EXPECT_NEAR(vj.outlet_conc, ref.outlet_conc, 0.03 * kCmix) << tag;

    // Quality continuity: no worse than the same reach split at a regular
    // junction (FV quality closes to ~1.5 % on this deck either way; DW to
    // well under 1 %).
    EXPECT_LE(std::fabs(vj.qual_error), std::fabs(ref.qual_error) + 0.005)
        << tag << ": quality continuity error " << vj.qual_error
        << " vs regular-junction reference " << ref.qual_error;
}

} // namespace

TEST(VirtualJunctionQuality, LateralLoad_DW_LEGACY) {
    lateralLoad("dw_legacy", "DYNWAVE", "");
}
TEST(VirtualJunctionQuality, LateralLoad_DW_ARD) {
    lateralLoad("dw_ard", "DYNWAVE", "EULERIAN_ARD");
}
TEST(VirtualJunctionQuality, LateralLoad_DW_LARD) {
    lateralLoad("dw_lard", "DYNWAVE", "LAGRANGIAN");
}
TEST(VirtualJunctionQuality, LateralLoad_FV_LEGACY) {
    lateralLoad("fv_legacy", "FV", "");
}
TEST(VirtualJunctionQuality, LateralLoad_FV_ARD) {
    lateralLoad("fv_ard", "FV", "EULERIAN_ARD");
}
TEST(VirtualJunctionQuality, LateralLoad_FV_LARD) {
    lateralLoad("fv_lard", "FV", "LAGRANGIAN");
}
