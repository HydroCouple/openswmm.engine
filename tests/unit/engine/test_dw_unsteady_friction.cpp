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
 * @file test_dw_unsteady_friction.cpp
 * @brief Gates for the dynamic wave unsteady-friction terms (issue #156 P3).
 *
 * @details Valve-closure fixture under DYNWAVE (post-closure mass-oscillation
 *          damping — DW resolves the oscillation, not the acoustic front):
 *          bit-inertness of NONE, damping with k3 > 0 under both SLOT and
 *          EXTRAN (the two celerity arms), k3-monotonicity, stability at the
 *          paper's sweep ceiling, discrete-rest bit-identity, and Picard
 *          health (trials do not degrade — the dqdh consistency gate U-G6).
 *          Plan: plans/MIXED_FLOW_CLOSURES_TPA_UF_PLAN_2026-08-29.md §3.4/§3.5.
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

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "dw_uf_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

// Reservoir (FIXED 15 ft) -> 200 ft of 1-ft pipe -> surcharged junction ->
// orifice valve -> free outfall; valve slams shut at t = 60 s. JV rides at
// 13 ft on a 1-ft pipe, so the EXTRAN dQ/dH (or slot) surcharge machinery is
// exercised on every step — dqdh consistency is structural to this fixture.
std::string valveModel(const std::string& surcharge, const std::string& extra,
                       bool reverse_c3 = false) {
    std::ostringstream ss;
    ss << "[OPTIONS]\n"
          "FLOW_UNITS           CFS\n"
          "FLOW_ROUTING         DYNWAVE\n"
          "SURCHARGE_METHOD     " << surcharge << "\n"
       << "START_DATE           01/01/2026\n"
          "START_TIME           00:00:00\n"
          "END_DATE             01/01/2026\n"
          "END_TIME             00:10:00\n"
          "REPORT_STEP          0.5\n"
          "ROUTING_STEP         0.25\n"
          "RULE_STEP            0\n"
          "INERTIAL_DAMPING     NONE\n"
          "NORMAL_FLOW_LIMITED  NEITHER\n"
          "MAX_TRIALS           8\n"
       << extra
       << "\n[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth\n"
          // The pipe is split into 4 conduits through simple degree-2
          // junctions so the cross-link ∂V/∂x stencil is live — a single
          // full conduit has no internal velocity gradient and UF would be
          // pure added inertia (the measured anti-damping case).
          "J1     0.0  40.0     13.5\n"
          "J2     0.0  40.0     13.3\n"
          "J3     0.0  40.0     13.1\n"
          "JV     0.0  40.0     13.0\n"
          "\n[OUTFALLS]\n;;Name Elev Type  StageData Gated\n"
          "UP     0.0  FIXED 15.0      NO\n"
          "DN     0.0  FREE            NO\n"
          "\n[CONDUITS]\n;;Name From To Length N     Z1 Z2 Q0\n"
          "C_1    UP   J1 50.0   0.013 0  0  2.4\n"
          "C_2    J1   J2 50.0   0.013 0  0  2.4\n"
       // Mirror-symmetry probe (task 5): C_3 declared node-reversed (node2
       // upstream) carries q < 0 for the same physical flow, so the stencil
       // sign tables must map it back into its neighbours' frames (and
       // theirs into its own) for the cross-link ∂V/∂x to survive.
       << (reverse_c3
               ? "C_3    J3   J2 50.0   0.013 0  0  -2.4\n"
               : "C_3    J2   J3 50.0   0.013 0  0  2.4\n")
       << "C_P    J3   JV 50.0   0.013 0  0  2.4\n"
          "\n[ORIFICES]\n;;Name From To Type Offset Cd   Gated CloseTime\n"
          "VALVE  JV   DN SIDE 0.0    0.62 NO    0\n"
          "\n[XSECTIONS]\n;;Link Shape    G1   G2 G3 G4 Barrels\n"
          "C_1    CIRCULAR 1.0  0  0  0  1\n"
          "C_2    CIRCULAR 1.0  0  0  0  1\n"
          "C_3    CIRCULAR 1.0  0  0  0  1\n"
          "C_P    CIRCULAR 1.0  0  0  0  1\n"
          "VALVE  CIRCULAR 0.41 0  0  0  1\n"
          "\n[CONTROLS]\n"
          "RULE VCLOSE\nIF SIMULATION TIME >= 0.016667\n"
          "THEN ORIFICE VALVE SETTING = 0\n";
    return ss.str();
}

// Discretely-at-rest DW deck (flat, uniform level, junctions only).
std::string restModel(const std::string& extra) {
    std::ostringstream ss;
    ss << "[OPTIONS]\n"
          "FLOW_UNITS           CFS\n"
          "FLOW_ROUTING         DYNWAVE\n"
          "SURCHARGE_METHOD     SLOT\n"
          "START_DATE           01/01/2026\n"
          "START_TIME           00:00:00\n"
          "END_DATE             01/01/2026\n"
          "END_TIME             00:10:00\n"
          "REPORT_STEP          5\n"
          "ROUTING_STEP         1\n"
       << extra
       << "\n[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth\n"
          "TA     0.0  40.0     13.0\n"
          "TB     0.0  40.0     13.0\n"
          "\n[OUTFALLS]\n;;Name Elev Type Gated\nO_OUT  0.0  FREE NO\n"
          "\n[WEIRS]\n;;Name From To    Type       CrestHt Cd\n"
          "W_OVF  TB   O_OUT TRANSVERSE 39.5    3.33\n"
          "\n[CONDUITS]\n;;Name From To Length N     Z1 Z2\n"
          "C_P    TA   TB 200.0  0.013 0  0\n"
          "\n[XSECTIONS]\n;;Link Shape     G1  G2 G3 G4 Barrels\n"
          "C_P    CIRCULAR  3.0 0  0  0  1\n"
          "W_OVF  RECT_OPEN 2.0 1.0 0 0\n";
    return ss.str();
}

struct RunResult {
    double late_amp = 0.0;   ///< max |head_JV − final| over the last 3 min
    double max_q = 0.0;
    bool ok = false;
};

RunResult runDeck(const std::string& base, const std::string& deck,
                  const char* probe_node, const char* probe_link) {
    RunResult r;
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
    const int nd = swmm_node_index(e, probe_node);
    const int lk = swmm_link_index(e, probe_link);
    EXPECT_GE(nd, 0);
    EXPECT_GE(lk, 0);
    std::vector<std::pair<double, double>> trace;
    double elapsed = 0.0;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            ADD_FAILURE() << "step failed: " << swmm_get_last_error_msg(e);
            break;
        }
        double d = 0.0, q = 0.0;
        swmm_node_get_depth(e, nd, &d);
        swmm_link_get_flow(e, lk, &q);
        r.max_q = std::max(r.max_q, std::fabs(q));
        trace.emplace_back(elapsed * 86400.0, d);
    } while (elapsed > 0.0);
    swmm_engine_end(e);
    swmm_engine_destroy(e);
    if (trace.empty()) return r;
    const double fin = trace.back().second;
    for (const auto& [t, d] : trace)
        if (t >= 420.0)
            r.late_amp = std::max(r.late_amp, std::fabs(d - fin));
    r.ok = true;
    return r;
}

std::string fileBytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const char* kUfOn =
    "UNSTEADY_FRICTION    VITKOVSKY\n"
    "UF_K3                0.020\n";

} // namespace

TEST(DwUnsteadyFriction, NoneIsBitIdenticalToAbsent) {
    const auto a = runDeck("dw_absent", valveModel("SLOT", ""), "JV", "C_P");
    const auto b = runDeck("dw_none",
                           valveModel("SLOT",
                                      "UNSTEADY_FRICTION    NONE\n"
                                      "UF_K3                0.02\n"),
                           "JV", "C_P");
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_EQ(fileBytes(outPath("dw_absent.out")), fileBytes(outPath("dw_none.out")))
        << "UNSTEADY_FRICTION NONE must be bit-inert under DYNWAVE (U-G7)";
}

TEST(DwUnsteadyFriction, DiscreteRestIsBitIdentical) {
    const auto a = runDeck("dw_rest_base", restModel(""), "TA", "C_P");
    const auto b = runDeck("dw_rest_on", restModel(kUfOn), "TA", "C_P");
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_EQ(fileBytes(outPath("dw_rest_base.out")),
              fileBytes(outPath("dw_rest_on.out")));
}

TEST(DwUnsteadyFriction, ValveClosureK3DampsUnderSlot) {
    const auto base = runDeck("dw_slot_k0", valveModel("SLOT", ""), "JV", "C_P");
    const auto damped = runDeck("dw_slot_k20", valveModel("SLOT", kUfOn),
                                "JV", "C_P");
    ASSERT_TRUE(base.ok && damped.ok);
    ASSERT_GT(base.late_amp, 1e-4)
        << "fixture must still be oscillating in the late window";
    EXPECT_LT(damped.late_amp, base.late_amp)
        << "k3=0.02 must damp the post-closure oscillation (SLOT celerity arm)";
}

TEST(DwUnsteadyFriction, ValveClosureK3DampsUnderExtran) {
    const auto base = runDeck("dw_ext_k0", valveModel("EXTRAN", ""), "JV", "C_P");
    const auto damped = runDeck("dw_ext_k20", valveModel("EXTRAN", kUfOn),
                                "JV", "C_P");
    ASSERT_TRUE(base.ok && damped.ok);
    ASSERT_GT(base.late_amp, 1e-4);
    EXPECT_LT(damped.late_amp, base.late_amp)
        << "k3=0.02 must damp under the EXTRAN (frozen-width) celerity arm";
}

TEST(DwUnsteadyFriction, DampingMonotoneInK3) {
    const auto k5 = runDeck("dw_slot_k05",
                            valveModel("SLOT",
                                       "UNSTEADY_FRICTION    VITKOVSKY\n"
                                       "UF_K3                0.005\n"),
                            "JV", "C_P");
    const auto k20 = runDeck("dw_slot_k20b", valveModel("SLOT", kUfOn),
                             "JV", "C_P");
    ASSERT_TRUE(k5.ok && k20.ok);
    EXPECT_LE(k20.late_amp, k5.late_amp * 1.001);
}

TEST(DwUnsteadyFriction, StableAtPaperMaxK3) {
    const auto r = runDeck("dw_slot_k45",
                           valveModel("SLOT",
                                      "UNSTEADY_FRICTION    VITKOVSKY\n"
                                      "UF_K3                0.045\n"),
                           "JV", "C_P");
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(std::isfinite(r.late_amp));
    EXPECT_LT(r.max_q, 500.0) << "no blowup at k3=0.045 (U-G5)";
}

TEST(DwUnsteadyFriction, SemiImplicitNodeContinuityComposes) {
    // U-G6 flavor: the k3-widened denominator feeds dqdh_, which feeds the
    // SEMI_IMPLICIT node update — the pairing must run clean and damp the
    // same direction.
    const auto base = runDeck("dw_semi_k0",
                              valveModel("SLOT",
                                         "NODE_CONTINUITY      SEMI_IMPLICIT\n"),
                              "JV", "C_P");
    const auto damped = runDeck("dw_semi_k20",
                                valveModel("SLOT",
                                           "NODE_CONTINUITY      SEMI_IMPLICIT\n"
                                           "UNSTEADY_FRICTION    VITKOVSKY\n"
                                           "UF_K3                0.020\n"),
                                "JV", "C_P");
    ASSERT_TRUE(base.ok && damped.ok);
    EXPECT_LE(damped.late_amp, base.late_amp * 1.001);
}

TEST(DwUnsteadyFriction, MirrorSymmetryOnReversedConduit) {
    // Task-5 gate (P3 verification): declaring C_3 node-reversed (node2
    // upstream, q < 0 for the same physical flow) must leave the UF damping
    // intact and equal to the aligned deck within tolerance — this is the
    // behavioral gate on the uf_sg_up_/uf_sg_dn_ sign tables. A flipped sign
    // table reads the neighbour velocity with the wrong sense, manufactures a
    // spurious |∂V/∂x| ≈ 2|v| gradient, and moves late_amp far outside the
    // band (falsifier 7b).
    const auto al_base = runDeck("dw_mir_al_k0", valveModel("SLOT", ""),
                                 "JV", "C_P");
    const auto al_damp = runDeck("dw_mir_al_k20", valveModel("SLOT", kUfOn),
                                 "JV", "C_P");
    const auto rv_base = runDeck("dw_mir_rv_k0",
                                 valveModel("SLOT", "", true), "JV", "C_P");
    const auto rv_damp = runDeck("dw_mir_rv_k20",
                                 valveModel("SLOT", kUfOn, true), "JV", "C_P");
    ASSERT_TRUE(al_base.ok && al_damp.ok && rv_base.ok && rv_damp.ok);
    // Damping must still occur on the reversed deck…
    ASSERT_GT(rv_base.late_amp, 1e-4);
    EXPECT_LT(rv_damp.late_amp, rv_base.late_amp)
        << "k3=0.02 must damp with a node-reversed conduit in the chain";
    // …and must agree with the aligned deck. Measured (P3 verification,
    // certified build): the fully-surcharged fixture is EXACTLY antisymmetric
    // under reversal — al_damp = rv_damp = 0.1010545191678851 bit-for-bit
    // (sig = 0 when closed-full, so every momentum term flips sign cleanly,
    // and the sign-mapped stencil preserves that). 1e-6 relative keeps the
    // gate robust to benign FP reshuffles while a flipped sign table (which
    // manufactures a spurious ~2|v| gradient) fails it by orders of
    // magnitude (falsifier 7b).
    EXPECT_NEAR(rv_damp.late_amp, al_damp.late_amp,
                1e-6 * al_damp.late_amp + 1e-12)
        << "aligned damped=" << al_damp.late_amp
        << " reversed damped=" << rv_damp.late_amp
        << " (aligned base=" << al_base.late_amp
        << " reversed base=" << rv_base.late_amp << ")";
}

TEST(DwUnsteadyFriction, SingleLinkGetsNoStencilAndNoDamping) {
    // Plan §3.4 amendment (a), pinned: a single full conduit has equal end
    // areas, the within-link |v2−v1| estimator is structurally zero, and both
    // stencil sides are -1 (UP is an outfall; JV carries the orifice, and any
    // non-conduit attachment disqualifies the node) — so UF is added inertia
    // only and must NOT produce the cross-link damping. This is also the
    // death-free behavioural assertion that an orifice-bearing junction maps
    // to -1: were JV misclassified as a simple conduit junction, the gather
    // would index the conduit tile with the orifice's (invalid) slot.
    std::string base_deck = valveModel("SLOT", "");
    std::string uf_deck = valveModel("SLOT", kUfOn);
    auto singleLink = [](std::string d) {
        // Collapse the 4-conduit chain to one 200 ft conduit UP -> JV.
        auto cut = [&d](const std::string& from, const std::string& to) {
            const auto b = d.find(from);
            ASSERT_NE(b, std::string::npos) << from;
            const auto e = d.find(to, b);
            ASSERT_NE(e, std::string::npos) << to;
            d.erase(b, e - b);
        };
        cut("J1     0.0", "JV     0.0");
        cut("C_1    UP   J1", "C_P    J3   JV");
        d.replace(d.find("C_P    J3   JV 50.0"),
                  std::string("C_P    J3   JV 50.0").size(),
                  "C_P    UP   JV 200.");
        cut("C_1    CIRCULAR", "C_P    CIRCULAR");
        return d;
    };
    std::string sl_base, sl_uf;
    { SCOPED_TRACE("base"); sl_base = singleLink(base_deck); }
    { SCOPED_TRACE("uf");   sl_uf   = singleLink(uf_deck); }
    const auto b = runDeck("dw_single_k0", sl_base, "JV", "C_P");
    const auto u = runDeck("dw_single_k20", sl_uf, "JV", "C_P");
    ASSERT_TRUE(b.ok && u.ok);
    ASSERT_GT(b.late_amp, 1e-4);
    // No damping benefit without a live stencil (measured: slight ANTI-
    // damping from the added inertia — see plan §3.4 amendment (a)).
    EXPECT_GT(u.late_amp, b.late_amp * 0.90)
        << "single-link UF must not show cross-link damping: base="
        << b.late_amp << " uf=" << u.late_amp;
}
