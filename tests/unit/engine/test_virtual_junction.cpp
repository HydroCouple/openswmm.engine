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
 * @file test_virtual_junction.cpp
 * @brief Virtual junction feature tests: parse/round-trip, validation rule
 *        codes, zero-storage behaviour, steady equivalence vs a single
 *        conduit, split/fuse edit operations, and runtime inflow guards.
 *
 * @details See plans/VIRTUAL_JUNCTION_IMPLEMENTATION_PLAN.md. All inputs and
 *          outputs are written under ./virtual_junction_out/ (working dir is
 *          tests/unit/engine/data) for review — no temp files (project
 *          convention, CLAUDE.md §4.1). Refactored engine only; the legacy
 *          engine is untouched by the feature.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_edit.h>
#include <openswmm/engine/openswmm_massbalance.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "virtual_junction_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Common [OPTIONS] block. J_IN (invert 10) -> 200 ft of 1-ft circular pipe
// on a constant slope -> free outfall O_OUT (invert 8). DWF supplies a
// constant 1.5 cfs at J_IN.
std::string options(const std::string& routing, const std::string& extra = "") {
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         " + routing + "\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:30:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         1\n"
        "ALLOW_PONDING        NO\n"
        + extra +
        "\n";
}

std::string singlePipeModel(const std::string& extra_options = "") {
    return options("DYNWAVE", extra_options) +
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth\n"
        "J_IN    10.0  5.0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev  Type  Gated\n"
        "O_OUT   8.0   FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name    From   To     Length  N      Z1  Z2\n"
        "C_MAIN    J_IN   O_OUT  200.0   0.013  0   0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link    Shape     G1   G2  G3  G4  Barrels\n"
        "C_MAIN    CIRCULAR  1.0  0   0   0   1\n"
        "\n"
        "[DWF]\n"
        ";;Node  Param  Value\n"
        "J_IN    FLOW   1.5\n"
        "\n"
        "[COORDINATES]\n"
        ";;Node  X    Y\n"
        "J_IN    0.0    0.0\n"
        "O_OUT   200.0  0.0\n";
}

// Same reach split at midpoint by node MID: declared either as a virtual
// junction (mid_section = "[VIRTUAL_JUNCTIONS]\nMID  9.0\n") or as a regular
// junction (mid_section = "" and MID appears in [JUNCTIONS]).
std::string splitModel(bool virtual_mid,
                       const std::string& extra_options = "",
                       const std::string& xsect2 = "CIRCULAR  1.0  0   0   0   1",
                       const std::string& c2_offsets = "0   0",
                       const std::string& extra_sections = "") {
    std::string junctions =
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth\n"
        "J_IN    10.0  5.0\n";
    if (!virtual_mid) junctions += "MID     9.0   0.0\n";

    std::string mid_section = virtual_mid
        ? "[VIRTUAL_JUNCTIONS]\n;;Name  Elev\nMID     9.0\n\n"
        : "";

    return options("DYNWAVE", extra_options) + junctions + "\n" + mid_section +
        "[OUTFALLS]\n"
        ";;Name  Elev  Type  Gated\n"
        "O_OUT   8.0   FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name    From   To     Length  N      Z1  Z2\n"
        "C_UP      J_IN   MID    100.0   0.013  0   0\n"
        "C_DN      MID    O_OUT  100.0   0.013  " + c2_offsets + "\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link    Shape     G1   G2  G3  G4  Barrels\n"
        "C_UP      CIRCULAR  1.0  0   0   0   1\n"
        "C_DN      " + xsect2 + "\n"
        "\n"
        "[DWF]\n"
        ";;Node  Param  Value\n"
        "J_IN    FLOW   1.5\n"
        + extra_sections +
        "\n"
        "[COORDINATES]\n"
        ";;Node  X    Y\n"
        "J_IN    0.0    0.0\n"
        "MID     100.0  0.0\n"
        "O_OUT   200.0  0.0\n";
}

// Open a model from text; returns the engine (nullptr on open failure when
// expect_ok is false). rpt/out land next to the inp for review.
SWMM_Engine openModel(const std::string& base, const std::string& text,
                      bool expect_ok, int* open_rc = nullptr) {
    const std::string inp = outPath(base + ".inp");
    writeFile(inp, text);
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, inp.c_str(),
                                    outPath(base + ".rpt").c_str(),
                                    outPath(base + ".out").c_str(), nullptr);
    if (open_rc) *open_rc = rc;
    if (expect_ok) {
        EXPECT_EQ(rc, 0) << "open failed for " << base << ": "
                         << swmm_get_last_error_msg(e);
    }
    return e;
}

void destroy(SWMM_Engine e) {
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

// Collected engine error text (for asserting specific ERROR codes).
std::string allErrors(SWMM_Engine e) {
    std::string s;
    const int n = swmm_get_error_count(e);
    for (int i = 0; i < n; ++i) {
        const char* m = swmm_get_error_at(e, i);
        if (m) { s += m; s += "\n"; }
    }
    const char* last = swmm_get_last_error_msg(e);
    if (last) { s += last; }
    return s;
}

// Run a model to completion; returns final flow of `link_name` (cfs) and
// captures the node depth/volume/overflow of `probe_node` at every step.
struct RunProbe {
    double final_flow = 0.0;
    double max_probe_volume = 0.0;
    double max_probe_overflow = 0.0;
    double final_probe_depth = 0.0;
    bool ran = false;
};

RunProbe runModel(SWMM_Engine e, const std::string& link_name,
                  const std::string& probe_node) {
    RunProbe p;
    EXPECT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_start(e, 1), 0) << swmm_get_last_error_msg(e);

    const int link = swmm_link_index(e, link_name.c_str());
    const int node = probe_node.empty() ? -1
                     : swmm_node_index(e, probe_node.c_str());
    EXPECT_GE(link, 0) << "missing link " << link_name;
    if (link < 0) return p;

    double elapsed = 0.0;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            ADD_FAILURE() << "step failed: " << swmm_get_last_error_msg(e);
            return p;
        }
        if (node >= 0) {
            double v = 0.0, ov = 0.0;
            swmm_node_get_volume(e, node, &v);
            swmm_node_get_overflow(e, node, &ov);
            p.max_probe_volume = std::max(p.max_probe_volume, std::fabs(v));
            p.max_probe_overflow = std::max(p.max_probe_overflow, std::fabs(ov));
        }
    } while (elapsed > 0.0);

    swmm_link_get_flow(e, link, &p.final_flow);
    if (node >= 0) swmm_node_get_depth(e, node, &p.final_probe_depth);
    swmm_engine_end(e);
    p.ran = true;
    return p;
}

} // namespace

// ---------------------------------------------------------------------------
// Parse + round-trip
// ---------------------------------------------------------------------------

TEST(VirtualJunction, ParseFlagAndRoundTrip) {
    SWMM_Engine e = openModel("vj_roundtrip", splitModel(true), true);

    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    int isv = -1, type = -1;
    ASSERT_EQ(swmm_node_is_virtual(e, mid, &isv), SWMM_OK);
    EXPECT_EQ(isv, 1);
    // Type-code space unchanged: a virtual junction IS a junction (this is
    // what keeps the binary .out node-type record identical).
    ASSERT_EQ(swmm_node_get_type(e, mid, &type), SWMM_OK);
    EXPECT_EQ(type, 0 /* SWMM_NODE_JUNCTION */);

    // Derived geometry: full depth == pipe crown (1 ft circular).
    double maxd = -1.0;
    ASSERT_EQ(swmm_node_get_max_depth(e, mid, &maxd), SWMM_OK);
    EXPECT_NEAR(maxd, 1.0, 1e-9);

    // Writer round-trip: MID emits into [VIRTUAL_JUNCTIONS] (with its
    // invert), and NOT into [JUNCTIONS].
    const std::string rt = outPath("vj_roundtrip_rt.inp");
    ASSERT_EQ(swmm_model_write(e, rt.c_str()), 0);
    const std::string text = readFile(rt);
    EXPECT_NE(text.find("[VIRTUAL_JUNCTIONS]"), std::string::npos);
    const auto vj_pos = text.find("[VIRTUAL_JUNCTIONS]");
    EXPECT_NE(text.find("MID", vj_pos), std::string::npos);
    const auto j_pos = text.find("[JUNCTIONS]");
    ASSERT_NE(j_pos, std::string::npos);
    const auto j_end = text.find('[', j_pos + 1);
    EXPECT_EQ(text.substr(j_pos, j_end - j_pos).find("MID"), std::string::npos)
        << "virtual junction leaked into [JUNCTIONS]";
    destroy(e);

    // Re-open the written file: flag and invert survive.
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e2, rt.c_str(),
                               outPath("vj_roundtrip_rt.rpt").c_str(),
                               outPath("vj_roundtrip_rt.out").c_str(),
                               nullptr), 0)
        << swmm_get_last_error_msg(e2);
    const int mid2 = swmm_node_index(e2, "MID");
    ASSERT_GE(mid2, 0);
    int isv2 = -1;
    ASSERT_EQ(swmm_node_is_virtual(e2, mid2, &isv2), SWMM_OK);
    EXPECT_EQ(isv2, 1);
    double inv2 = 0.0;
    ASSERT_EQ(swmm_node_get_invert_elev(e2, mid2, &inv2), SWMM_OK);
    EXPECT_NEAR(inv2, 9.0, 1e-6);
    destroy(e2);
}

// ---------------------------------------------------------------------------
// Optional MaxDepth (third token) — RENDERING ONLY
// ---------------------------------------------------------------------------

namespace {
// splitModel(true) with a rendering MaxDepth on the [VIRTUAL_JUNCTIONS] row.
std::string splitModelWithRim(const std::string& rim,
                              const std::string& extra_options = "") {
    std::string m = splitModel(true, extra_options);
    const auto pos = m.find("MID     9.0");
    EXPECT_NE(pos, std::string::npos);
    m.replace(pos, 11, "MID     9.0  " + rim);
    return m;
}
} // namespace

TEST(VirtualJunction, RimDepthParsesAndRoundTrips) {
    SWMM_Engine e = openModel("vj_rim", splitModelWithRim("4.0"), true);

    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);

    // The solver's max depth is STILL the derived pipe crown (1 ft circular);
    // the third token only supplies the ground surface for drawings.
    double maxd = -1.0, rim = -1.0;
    ASSERT_EQ(swmm_node_get_max_depth(e, mid, &maxd), SWMM_OK);
    EXPECT_NEAR(maxd, 1.0, 1e-9);
    ASSERT_EQ(swmm_node_get_rim_depth(e, mid, &rim), SWMM_OK);
    EXPECT_NEAR(rim, 4.0, 1e-9);

    // Writer emits the third column (header + value).
    const std::string rt = outPath("vj_rim_rt.inp");
    ASSERT_EQ(swmm_model_write(e, rt.c_str()), 0);
    const std::string text = readFile(rt);
    const auto vj_pos = text.find("[VIRTUAL_JUNCTIONS]");
    ASSERT_NE(vj_pos, std::string::npos);
    const auto vj_end = text.find('[', vj_pos + 1);
    const std::string section = text.substr(vj_pos, vj_end - vj_pos);
    EXPECT_NE(section.find("MaxDepth"), std::string::npos) << section;
    EXPECT_NE(section.find("4.0000"), std::string::npos) << section;
    destroy(e);

    // Re-open the written file: the rim survives and the crown is unchanged.
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e2, rt.c_str(), outPath("vj_rim_rt.rpt").c_str(),
                               outPath("vj_rim_rt.out").c_str(), nullptr), 0)
        << swmm_get_last_error_msg(e2);
    const int mid2 = swmm_node_index(e2, "MID");
    ASSERT_GE(mid2, 0);
    double rim2 = -1.0, maxd2 = -1.0;
    ASSERT_EQ(swmm_node_get_rim_depth(e2, mid2, &rim2), SWMM_OK);
    ASSERT_EQ(swmm_node_get_max_depth(e2, mid2, &maxd2), SWMM_OK);
    EXPECT_NEAR(rim2, 4.0, 1e-6);
    EXPECT_NEAR(maxd2, 1.0, 1e-9);
    destroy(e2);
}

// A model with no MaxDepth keeps writing the two-column section byte for byte.
TEST(VirtualJunction, RimDepthAbsentKeepsTwoColumnSection) {
    SWMM_Engine e = openModel("vj_norim", splitModel(true), true);
    const int mid = swmm_node_index(e, "MID");
    double rim = -1.0;
    ASSERT_EQ(swmm_node_get_rim_depth(e, mid, &rim), SWMM_OK);
    EXPECT_EQ(rim, 0.0) << "absent MaxDepth must read back as unset";

    const std::string rt = outPath("vj_norim_rt.inp");
    ASSERT_EQ(swmm_model_write(e, rt.c_str()), 0);
    const std::string text = readFile(rt);
    const auto vj_pos = text.find("[VIRTUAL_JUNCTIONS]");
    ASSERT_NE(vj_pos, std::string::npos);
    const auto vj_end = text.find('[', vj_pos + 1);
    EXPECT_EQ(text.substr(vj_pos, vj_end - vj_pos).find("MaxDepth"), std::string::npos);
    destroy(e);
}

// THE contract: supplying MaxDepth cannot move a single number in the run.
TEST(VirtualJunction, RimDepthIsHydraulicallyInert) {
    SWMM_Engine plain = openModel("vj_inert_plain", splitModel(true), true);
    RunProbe pp = runModel(plain, "C_DN", "MID");
    destroy(plain);

    SWMM_Engine rimmed = openModel("vj_inert_rim", splitModelWithRim("4.0"), true);
    RunProbe pr = runModel(rimmed, "C_DN", "MID");
    destroy(rimmed);

    ASSERT_TRUE(pp.ran && pr.ran);
    EXPECT_EQ(pr.final_flow, pp.final_flow)
        << "a rendering-only MaxDepth changed the routed flow";
    EXPECT_EQ(pr.final_probe_depth, pp.final_probe_depth)
        << "a rendering-only MaxDepth changed the node depth";
    EXPECT_EQ(pr.max_probe_volume, 0.0);
    EXPECT_EQ(pr.max_probe_overflow, 0.0);
}

// Both unit-conversion passes (display→internal on load, internal→display on
// write) must carry the rim, or a metric model round-trips scaled by 3.2808.
TEST(VirtualJunction, RimDepthSurvivesMetricRoundTrip) {
    std::string m = splitModelWithRim("4.0");
    const auto pos = m.find("FLOW_UNITS           CFS");
    ASSERT_NE(pos, std::string::npos);
    m.replace(pos, 24, "FLOW_UNITS           CMS");

    SWMM_Engine e = openModel("vj_rim_metric", m, true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    double rim = -1.0;
    ASSERT_EQ(swmm_node_get_rim_depth(e, mid, &rim), SWMM_OK);
    EXPECT_NEAR(rim, 4.0, 1e-9) << "getter must report project length units";

    const std::string rt = outPath("vj_rim_metric_rt.inp");
    ASSERT_EQ(swmm_model_write(e, rt.c_str()), 0);
    const std::string text = readFile(rt);
    const auto vj_pos = text.find("[VIRTUAL_JUNCTIONS]");
    ASSERT_NE(vj_pos, std::string::npos);
    const auto vj_end = text.find('[', vj_pos + 1);
    EXPECT_NE(text.substr(vj_pos, vj_end - vj_pos).find("4.0000"), std::string::npos)
        << text.substr(vj_pos, vj_end - vj_pos);
    destroy(e);
}

// ---------------------------------------------------------------------------
// Validation rule codes (each rule produces its specific ERROR number)
// ---------------------------------------------------------------------------

namespace {
void expectOpenError(const std::string& base, const std::string& text,
                     const std::string& code) {
    int rc = 0;
    SWMM_Engine e = openModel(base, text, false, &rc);
    EXPECT_NE(rc, 0) << base << " unexpectedly opened clean";
    EXPECT_NE(allErrors(e).find(code), std::string::npos)
        << base << ": expected ERROR " << code << " in:\n" << allErrors(e);
    destroy(e);
}
} // namespace

TEST(VirtualJunction, ValidationRuleCodes) {
    // 611: cross-section mismatch (C_DN is a 2-ft pipe).
    expectOpenError("vj_err_xsect",
        splitModel(true, "", "CIRCULAR  2.0  0   0   0   1"), "611");

    // 613: nonzero offset at the virtual node (C_DN upstream offset 0.5).
    expectOpenError("vj_err_offset",
        splitModel(true, "", "CIRCULAR  1.0  0   0   0   1", "0.5  0"), "613");

    // 617: lateral inflow targets the virtual junction.
    expectOpenError("vj_err_inflow",
        splitModel(true, "", "CIRCULAR  1.0  0   0   0   1", "0   0",
                   "\n[DWF]\nMID  FLOW  0.1\n"), "617");

    // 619: KINWAVE routing.
    {
        std::string m = splitModel(true);
        const auto pos = m.find("DYNWAVE");
        ASSERT_NE(pos, std::string::npos);
        m.replace(pos, 7, "KINWAVE");
        expectOpenError("vj_err_routing", m, "619");
    }

    // 621: extra tokens on the [VIRTUAL_JUNCTIONS] line. Three tokens are
    // legal (the third is the rendering-only MaxDepth), four are not.
    {
        std::string m = splitModel(true);
        const auto pos = m.find("MID     9.0");
        ASSERT_NE(pos, std::string::npos);
        m.replace(pos, 11, "MID     9.0  5.0  0.0");
        expectOpenError("vj_err_tokens", m, "621");
    }

    // 609: only one conduit attached (drop C_DN; O_OUT dangles but the VJ
    // rule fires regardless).
    {
        std::string m = splitModel(true);
        const auto pos = m.find("C_DN      MID    O_OUT");
        ASSERT_NE(pos, std::string::npos);
        const auto eol = m.find('\n', pos);
        m.erase(pos, eol - pos + 1);
        expectOpenError("vj_err_linkcount", m, "609");
    }
}

// ---------------------------------------------------------------------------
// Zero storage + steady equivalence vs the unsplit conduit
// ---------------------------------------------------------------------------

namespace {
void steadyEquivalence(const std::string& tag, const std::string& extra_opts) {
    SWMM_Engine single = openModel("vj_single_" + tag,
                                   singlePipeModel(extra_opts), true);
    RunProbe ps = runModel(single, "C_MAIN", "");
    destroy(single);

    SWMM_Engine vj = openModel("vj_split_" + tag,
                               splitModel(true, extra_opts), true);
    RunProbe pv = runModel(vj, "C_DN", "MID");
    destroy(vj);

    ASSERT_TRUE(ps.ran && pv.ran);

    // Zero-storage contract: the virtual junction never stores or floods.
    EXPECT_EQ(pv.max_probe_volume, 0.0);
    EXPECT_EQ(pv.max_probe_overflow, 0.0);

    // Steady mass equivalence: after 30 min of constant 1.5 cfs DWF both
    // configurations pass the full inflow.
    EXPECT_NEAR(ps.final_flow, 1.5, 0.05);
    EXPECT_NEAR(pv.final_flow, ps.final_flow, 0.02)
        << "virtual-junction split diverges from the single conduit (" << tag << ")";
}
} // namespace

TEST(VirtualJunction, SteadyEquivalenceExplicit) {
    steadyEquivalence("explicit", "");
}

TEST(VirtualJunction, SteadyEquivalenceSemiImplicit) {
    steadyEquivalence("semi", "NODE_CONTINUITY      SEMI_IMPLICIT\n");
}

// VIRTUAL_JUNCTION_MOMENTUM FULL is RETIRED (2026-08-14). It must still parse
// — existing project files keep working — but it must now behave EXACTLY as
// BASIC. The term it used to add was sign-inverted with respect to the
// per-link convective term and was applied to both adjacent links, destroying
// 224-325 % of the routed volume on SWASHES macdonald-periodic; negating it
// restored mass but still left l1 5.24 % against BASIC's 0.163 %. See
// epaswmm5_qa suites/swashes plans/VJ_MOMENTUM_SCOPE.md Phase 3.
TEST(VirtualJunction, SteadyEquivalenceFullMomentumRetired) {
    steadyEquivalence("full",
        "VIRTUAL_JUNCTION_MOMENTUM  FULL\n"
        "NODE_CONTINUITY      SEMI_IMPLICIT\n");
}

TEST(VirtualJunction, FullMomentumRetiredMatchesBasic) {
    const std::string semi = "NODE_CONTINUITY      SEMI_IMPLICIT\n";
    SWMM_Engine basic = openModel("vj_retired_basic",
                                  splitModel(true, semi), true);
    RunProbe pb = runModel(basic, "C_DN", "MID");
    destroy(basic);

    SWMM_Engine full = openModel(
        "vj_retired_full",
        splitModel(true, "VIRTUAL_JUNCTION_MOMENTUM  FULL\n" + semi), true);
    RunProbe pf = runModel(full, "C_DN", "MID");
    destroy(full);

    ASSERT_TRUE(pb.ran && pf.ran);
    // Bit-equality, not a tolerance: FULL now selects the same code path.
    EXPECT_EQ(pf.final_flow, pb.final_flow)
        << "VIRTUAL_JUNCTION_MOMENTUM FULL is retired and must be inert";
    EXPECT_EQ(pf.max_probe_volume, pb.max_probe_volume);
    EXPECT_EQ(pf.max_probe_overflow, pb.max_probe_overflow);
}

TEST(VirtualJunction, SteadyEquivalenceAnderson) {
    steadyEquivalence("aa",
        "ANDERSON_ACCEL       YES\n"
        "NODE_CONTINUITY      SEMI_IMPLICIT\n");
}

// ---------------------------------------------------------------------------
// Slot surcharge across a virtual-junction chain (divergence regression)
// ---------------------------------------------------------------------------

namespace {

// A transmission main climbing a 200 ft crest on a 1 ft pipe at 500 cfs: the
// upstream leg has to surcharge to push flow over the summit while the
// downstream leg is still dry. That pairing — a slot-surcharged pass-through
// node, whose top width is only the slot, next to a dry one — is what made the
// head update diverge. A virtual junction carries no storage, so the explicit
// dV/A step divided a large volume change by a near-zero area; on TwinOaks
// (5028 virtual junctions) the head reached 1.9e14 by the fourth routing step.
// Reproduced compactly here with 12 chain nodes.
std::string hillChainModel(bool virtual_mid) {
    const int n = 12;
    const double seg = 300.0, dia = 1.0, rise = 200.0, q = 500.0;
    const double top = 10.0;
    const double half = (n + 1) / 2.0;

    std::vector<std::string> names;
    std::vector<double> inv;
    for (int i = 0; i < n; ++i) {
        char b[16];
        std::snprintf(b, sizeof(b), "MID%02d", i);
        names.emplace_back(b);
        const double x = (i + 1) / half;
        inv.push_back(top + rise * (x <= 1.0 ? x : 2.0 - x));
    }

    auto num = [](double v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%.2f", v);
        return std::string(b);
    };

    // The TwinOaks step regime: a 30 s nominal step the Courant limiter walks
    // down toward MINIMUM_STEP. dV = 0.5*(Qold+Q)*dt scales with the step, so
    // the large nominal step is part of what starves the update.
    std::string m =
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:30:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         30\n"
        "ALLOW_PONDING        NO\n"
        "SURCHARGE_METHOD     SLOT\n"
        "NODE_CONTINUITY      SEMI_IMPLICIT\n"
        "INERTIAL_DAMPING     PARTIAL\n"
        "NORMAL_FLOW_LIMITED  BOTH\n"
        "VARIABLE_STEP        0.75\n"
        "LENGTHENING_STEP     0\n"
        "MIN_SURFAREA         0\n"
        "MAX_TRIALS           8\n"
        "HEAD_TOLERANCE       0.005\n"
        "MINIMUM_STEP         0.5\n"
        "\n"
        "[JUNCTIONS]\n;;Name  Elev  MaxDepth\n"
        "J_IN    " + num(top) + "  " + num(rise) + "\n";
    if (!virtual_mid)
        for (int i = 0; i < n; ++i)
            m += names[i] + "   " + num(inv[i]) + "  0.0\n";
    m += "\n";

    if (virtual_mid) {
        m += "[VIRTUAL_JUNCTIONS]\n;;Name  Elev\n";
        for (int i = 0; i < n; ++i)
            m += names[i] + "   " + num(inv[i]) + "\n";
        m += "\n";
    }

    m += "[OUTFALLS]\n;;Name  Elev  Type  Gated\n"
         "O_OUT   " + num(top) + "   FREE  NO\n\n";

    std::vector<std::string> chain{"J_IN"};
    chain.insert(chain.end(), names.begin(), names.end());
    chain.emplace_back("O_OUT");

    m += "[CONDUITS]\n;;Name  From  To  Length  N  Z1  Z2\n";
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        char b[16];
        std::snprintf(b, sizeof(b), "C%02d", static_cast<int>(i));
        m += std::string(b) + "   " + chain[i] + "  " + chain[i + 1] + "  " +
             num(seg) + "  0.013  0  0\n";
    }
    m += "\n[XSECTIONS]\n;;Link  Shape  G1  G2  G3  G4  Barrels\n";
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        char b[16];
        std::snprintf(b, sizeof(b), "C%02d", static_cast<int>(i));
        m += std::string(b) + "   CIRCULAR  " + num(dia) + "  0  0  0  1\n";
    }

    m += "\n[DWF]\n;;Node  Param  Value\nJ_IN    FLOW   " + num(q) + "\n\n";
    m += "[COORDINATES]\n;;Node  X  Y\n";
    for (std::size_t i = 0; i < chain.size(); ++i)
        m += chain[i] + "   " + num(static_cast<double>(i) * seg) + "  0.0\n";
    return m;
}

struct ChainRun {
    bool ran = false;
    double max_depth = 0.0;
    double routing_error = 0.0;
};

// Steps the model to completion, tracking the largest depth reached anywhere.
ChainRun runChain(SWMM_Engine e) {
    ChainRun r;
    EXPECT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_start(e, 1), 0) << swmm_get_last_error_msg(e);

    const int n_nodes = swmm_node_count(e);
    std::vector<double> depths(static_cast<std::size_t>(n_nodes), 0.0);

    double elapsed = 0.0;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            ADD_FAILURE() << "step failed: " << swmm_get_last_error_msg(e);
            return r;
        }
        if (swmm_node_get_depths_bulk(e, depths.data(), n_nodes) == SWMM_OK)
            for (double d : depths)
                r.max_depth = std::max(r.max_depth, std::fabs(d));
    } while (elapsed > 0.0);

    swmm_engine_end(e);
    swmm_get_routing_continuity_error(e, &r.routing_error);
    r.ran = true;
    return r;
}

} // namespace

// Before the fix this run aborted with ERROR 14 (the divergence detector) —
// the explicit dV/A update ran the chain heads away. It must now complete with
// bounded heads and a mass balance no worse than the same network built from
// real junctions.
TEST(VirtualJunction, SlotChainOverCrestStaysBounded) {
    SWMM_Engine vj = openModel("vj_chain_slot", hillChainModel(true), true);
    ChainRun pv = runChain(vj);
    destroy(vj);

    ASSERT_TRUE(pv.ran) << "virtual-junction chain did not complete";

    // The crest is 200 ft above the inlet invert, so a legitimate slot
    // surcharge reaches a few hundred feet. The divergence produced 1e14.
    EXPECT_LT(pv.max_depth, 1.0e4)
        << "virtual-junction head ran away (max depth " << pv.max_depth << " ft)";

    // Real-junction twin: same network, same forcing, storage at each node.
    SWMM_Engine rj = openModel("vj_chain_slot_real", hillChainModel(false), true);
    ChainRun pr = runChain(rj);
    destroy(rj);
    ASSERT_TRUE(pr.ran);

    // The zero-storage chain must not be dramatically worse at conserving mass
    // than the stored one. Pre-fix this was -1.0e6 % against the twin's few %.
    EXPECT_LT(std::fabs(pv.routing_error), std::fabs(pr.routing_error) + 25.0)
        << "virtual-junction routing continuity " << pv.routing_error
        << " % vs real-junction " << pr.routing_error << " %";
}

// The regular-junction split still passes flow (baseline sanity for the
// comparison the .rpt residual diagnostic quantifies).
TEST(VirtualJunction, RegularSplitBaseline) {
    SWMM_Engine reg = openModel("vj_regsplit", splitModel(false), true);
    RunProbe pr = runModel(reg, "C_DN", "MID");
    destroy(reg);
    EXPECT_NEAR(pr.final_flow, 1.5, 0.05);
}

// ---------------------------------------------------------------------------
// Edit operations: set-virtual, split, fuse
// ---------------------------------------------------------------------------

TEST(VirtualJunction, SetVirtualApi) {
    // A conforming regular midpoint junction can be flagged...
    SWMM_Engine e = openModel("vj_setflag", splitModel(false), true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    EXPECT_EQ(swmm_node_set_virtual(e, mid, 1), SWMM_OK);
    int isv = 0;
    swmm_node_is_virtual(e, mid, &isv);
    EXPECT_EQ(isv, 1);
    // ...and un-flagged.
    EXPECT_EQ(swmm_node_set_virtual(e, mid, 0), SWMM_OK);
    swmm_node_is_virtual(e, mid, &isv);
    EXPECT_EQ(isv, 0);

    // J_IN has one conduit and a DWF inflow: rule 609 fires first.
    const int jin = swmm_node_index(e, "J_IN");
    EXPECT_EQ(swmm_node_set_virtual(e, jin, 1), 609);

    // Dry-run eligibility mirrors the same rules without changing state.
    int code = -1;
    EXPECT_EQ(swmm_node_virtual_eligible(e, mid, &code), SWMM_OK);
    EXPECT_EQ(code, 0);
    EXPECT_EQ(swmm_node_virtual_eligible(e, jin, &code), SWMM_OK);
    EXPECT_EQ(code, 609);
    swmm_node_is_virtual(e, mid, &isv);
    EXPECT_EQ(isv, 0);   // dry-run left the flag untouched
    destroy(e);
}

TEST(VirtualJunction, SplitFuseRoundTrip) {
    SWMM_Engine e = openModel("vj_splitfuse", singlePipeModel(), true);

    // Baseline .inp before the split.
    const std::string before = outPath("vj_splitfuse_before.inp");
    ASSERT_EQ(swmm_model_write(e, before.c_str()), 0);
    const int n_nodes0 = swmm_node_count(e);
    const int n_links0 = swmm_link_count(e);

    const int cmain = swmm_link_index(e, "C_MAIN");
    ASSERT_GE(cmain, 0);
    int new_node = -1, new_link = -1;
    ASSERT_EQ(swmm_conduit_split(e, cmain, 0.5, "VJX", "C_MAIN_B", 1,
                                 &new_node, &new_link), SWMM_OK);
    EXPECT_EQ(swmm_node_count(e), n_nodes0 + 1);
    EXPECT_EQ(swmm_link_count(e), n_links0 + 1);

    int isv = 0;
    ASSERT_EQ(swmm_node_is_virtual(e, new_node, &isv), SWMM_OK);
    EXPECT_EQ(isv, 1);

    // Interpolated break-point invert: midpoint of 10 → 8.
    double inv = 0.0;
    ASSERT_EQ(swmm_node_get_invert_elev(e, new_node, &inv), SWMM_OK);
    EXPECT_NEAR(inv, 9.0, 1e-9);

    // Lengths partitioned 50/50.
    double l1 = 0.0, l2 = 0.0;
    ASSERT_EQ(swmm_link_get_length(e, cmain, &l1), SWMM_OK);
    ASSERT_EQ(swmm_link_get_length(e, new_link, &l2), SWMM_OK);
    EXPECT_NEAR(l1, 100.0, 1e-9);
    EXPECT_NEAR(l2, 100.0, 1e-9);

    const std::string split_inp = outPath("vj_splitfuse_split.inp");
    ASSERT_EQ(swmm_model_write(e, split_inp.c_str()), 0);
    EXPECT_NE(readFile(split_inp).find("[VIRTUAL_JUNCTIONS]"), std::string::npos);

    // Fuse back: counts, wiring, and the written .inp are restored
    // byte-identically (t = 0.5 keeps the length sum exact).
    int surviving = -1;
    ASSERT_EQ(swmm_virtual_junction_fuse(e, new_node, &surviving), SWMM_OK);
    EXPECT_EQ(swmm_node_count(e), n_nodes0);
    EXPECT_EQ(swmm_link_count(e), n_links0);
    ASSERT_GE(surviving, 0);
    double lf = 0.0;
    ASSERT_EQ(swmm_link_get_length(e, surviving, &lf), SWMM_OK);
    EXPECT_NEAR(lf, 200.0, 1e-9);

    const std::string after = outPath("vj_splitfuse_after.inp");
    ASSERT_EQ(swmm_model_write(e, after.c_str()), 0);
    EXPECT_EQ(readFile(before), readFile(after))
        << "split→fuse did not restore the original .inp byte-identically";
    destroy(e);
}

// A virtual junction inserted by a split inherits an interpolated ground
// surface, so the drawn terrain runs through it instead of dropping to the
// pipe crown.
TEST(VirtualJunction, SplitInterpolatesRimDepth) {
    SWMM_Engine e = openModel("vj_split_rim", singlePipeModel(), true);

    // Pin both ends: J_IN rim = 10 + 5 = 15, O_OUT rim = 8 + 3 = 11.
    const int out = swmm_node_index(e, "O_OUT");
    ASSERT_GE(out, 0);
    ASSERT_EQ(swmm_node_set_max_depth(e, out, 3.0), SWMM_OK);

    const int cmain = swmm_link_index(e, "C_MAIN");
    ASSERT_GE(cmain, 0);
    int new_node = -1, new_link = -1;
    ASSERT_EQ(swmm_conduit_split(e, cmain, 0.5, "VJR", "C_MAIN_B", 1,
                                 &new_node, &new_link), SWMM_OK);

    // Midpoint rim 13.0 over the midpoint invert 9.0.
    double rim = -1.0, maxd = -1.0, inv = 0.0;
    ASSERT_EQ(swmm_node_get_invert_elev(e, new_node, &inv), SWMM_OK);
    ASSERT_EQ(swmm_node_get_rim_depth(e, new_node, &rim), SWMM_OK);
    ASSERT_EQ(swmm_node_get_max_depth(e, new_node, &maxd), SWMM_OK);
    EXPECT_NEAR(inv, 9.0, 1e-9);
    EXPECT_NEAR(rim, 4.0, 1e-9);
    EXPECT_NEAR(maxd, 1.0, 1e-9) << "the solver's depth is still the crown";
    destroy(e);
}

// A plain (non-virtual) split is untouched: the new junction gets the crown as
// its real full depth and no rendering rim.
TEST(VirtualJunction, PlainSplitLeavesRimUnset) {
    SWMM_Engine e = openModel("vj_split_plain_rim", singlePipeModel(), true);
    const int cmain = swmm_link_index(e, "C_MAIN");
    int new_node = -1, new_link = -1;
    ASSERT_EQ(swmm_conduit_split(e, cmain, 0.5, "PJ", "C_MAIN_B", 0,
                                 &new_node, &new_link), SWMM_OK);
    double rim = -1.0;
    ASSERT_EQ(swmm_node_get_rim_depth(e, new_node, &rim), SWMM_OK);
    EXPECT_EQ(rim, 0.0);
    destroy(e);
}

// Converting a real manhole to a virtual junction keeps its ground surface as
// the rendering rim, and converting back promotes it to the max depth again.
TEST(VirtualJunction, SetVirtualCarriesMaxDepthBothWays) {
    std::string m = splitModel(false);
    const auto pos = m.find("MID     9.0   0.0");
    ASSERT_NE(pos, std::string::npos);
    m.replace(pos, 17, "MID     9.0   4.0");

    SWMM_Engine e = openModel("vj_carry", m, true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    double maxd = 0.0, rim = -1.0;
    ASSERT_EQ(swmm_node_get_max_depth(e, mid, &maxd), SWMM_OK);
    EXPECT_NEAR(maxd, 4.0, 1e-9);

    ASSERT_EQ(swmm_node_set_virtual(e, mid, 1), SWMM_OK);
    ASSERT_EQ(swmm_node_get_max_depth(e, mid, &maxd), SWMM_OK);
    ASSERT_EQ(swmm_node_get_rim_depth(e, mid, &rim), SWMM_OK);
    EXPECT_NEAR(maxd, 1.0, 1e-9) << "solver depth becomes the pipe crown";
    EXPECT_NEAR(rim, 4.0, 1e-9)  << "the drawn ground surface is preserved";

    ASSERT_EQ(swmm_node_set_virtual(e, mid, 0), SWMM_OK);
    ASSERT_EQ(swmm_node_get_max_depth(e, mid, &maxd), SWMM_OK);
    ASSERT_EQ(swmm_node_get_rim_depth(e, mid, &rim), SWMM_OK);
    EXPECT_NEAR(maxd, 4.0, 1e-9) << "un-flagging restores the real max depth";
    EXPECT_EQ(rim, 0.0);
    destroy(e);
}

TEST(VirtualJunction, FuseRejectsNonVirtual) {
    SWMM_Engine e = openModel("vj_fusereject", splitModel(false), true);
    const int mid = swmm_node_index(e, "MID");
    EXPECT_EQ(swmm_virtual_junction_fuse(e, mid, nullptr), SWMM_ERR_BADPARAM);
    destroy(e);
}

// ---------------------------------------------------------------------------
// Runtime lateral-inflow guard
// ---------------------------------------------------------------------------

TEST(VirtualJunction, RuntimeLateralInflowRejected) {
    SWMM_Engine e = openModel("vj_runtime_guard", splitModel(true), true);
    ASSERT_EQ(swmm_engine_initialize(e), 0);
    ASSERT_EQ(swmm_engine_start(e, 0), 0);
    double elapsed = 0.0;
    ASSERT_EQ(swmm_engine_step(e, &elapsed), 0);

    const int mid = swmm_node_index(e, "MID");
    EXPECT_EQ(swmm_node_set_lateral_inflow(e, mid, 1.0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_node_set_lateral_inflow(e, mid, 0.0), SWMM_OK);

    swmm_engine_end(e);
    destroy(e);
}

// ---------------------------------------------------------------------------
// FV reporting of virtual-junction heads (Routing.cpp publishFv)
// ---------------------------------------------------------------------------

namespace {

// Chain J_IN → C_UP → VJ_A → C_FLAT → VJ_B → C_DN → O_OUT under FV routing.
// VJ_A and VJ_B share a bit-identical invert. The retired invert-matching
// join sent both spliced faces to the first matching node and left VJ_B
// permanently unreported (depth 0 for the whole run); the mesh now carries
// the face → node association directly (face_vj_node).
std::string fvCollidingInvertModel() {
    return options("FV") +
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth\n"
        "J_IN    11.0  5.0\n"
        "\n"
        "[VIRTUAL_JUNCTIONS]\n"
        ";;Name  Elev\n"
        "VJ_A    9.0\n"
        "VJ_B    9.0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev  Type  Gated\n"
        "O_OUT   8.0   FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name    From   To     Length  N      Z1  Z2\n"
        "C_UP      J_IN   VJ_A   100.0   0.013  0   0\n"
        "C_FLAT    VJ_A   VJ_B   100.0   0.013  0   0\n"
        "C_DN      VJ_B   O_OUT  100.0   0.013  0   0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link    Shape     G1   G2  G3  G4  Barrels\n"
        "C_UP      CIRCULAR  1.0  0   0   0   1\n"
        "C_FLAT    CIRCULAR  1.0  0   0   0   1\n"
        "C_DN      CIRCULAR  1.0  0   0   0   1\n"
        "\n"
        "[DWF]\n"
        ";;Node  Param  Value\n"
        "J_IN    FLOW   0.75\n"
        "\n"
        "[COORDINATES]\n"
        ";;Node  X      Y\n"
        "J_IN    0.0    0.0\n"
        "VJ_A    100.0  0.0\n"
        "VJ_B    200.0  0.0\n"
        "O_OUT   300.0  0.0\n";
}

// J_IN → C_SUP → VJ_S → C_SDN → O_OUT on 10 % slopes, with the downhill
// conduit listed FIRST so the spliced face's LEFT cell is the downhill one.
// The retired one-sided reconstruction read that cell's centre-datum WSE and
// clamped the virtual junction to zero whenever the flow depth was below half
// the conduit drop (5 ft here).
std::string fvSteepVJModel() {
    return options("FV") +
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth\n"
        "J_IN    20.0  5.0\n"
        "\n"
        "[VIRTUAL_JUNCTIONS]\n"
        ";;Name  Elev\n"
        "VJ_S    10.0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev  Type  Gated\n"
        "O_OUT   0.0   FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name    From   To     Length  N      Z1  Z2\n"
        "C_SDN     VJ_S   O_OUT  100.0   0.013  0   0\n"
        "C_SUP     J_IN   VJ_S   100.0   0.013  0   0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link    Shape     G1   G2  G3  G4  Barrels\n"
        "C_SDN     CIRCULAR  1.0  0   0   0   1\n"
        "C_SUP     CIRCULAR  1.0  0   0   0   1\n"
        "\n"
        "[DWF]\n"
        ";;Node  Param  Value\n"
        "J_IN    FLOW   0.75\n"
        "\n"
        "[COORDINATES]\n"
        ";;Node  X      Y\n"
        "J_IN    0.0    0.0\n"
        "VJ_S    100.0  0.0\n"
        "O_OUT   200.0  0.0\n";
}

// Step to completion WITHOUT swmm_engine_end so node state is still readable.
void stepToEnd(SWMM_Engine e) {
    ASSERT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);
    double elapsed = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &elapsed), 0)
            << swmm_get_last_error_msg(e);
    } while (elapsed > 0.0);
}

double nodeDepth(SWMM_Engine e, const char* name) {
    const int n = swmm_node_index(e, name);
    EXPECT_GE(n, 0) << "missing node " << name;
    double d = -1.0;
    if (n >= 0) swmm_node_get_depth(e, n, &d);
    return d;
}

} // namespace

TEST(VirtualJunction, FvReportsBothCollidingInvertVJs) {
    SWMM_Engine e = openModel("vj_fv_invert_collision",
                              fvCollidingInvertModel(), true);
    stepToEnd(e);

    // Steady 0.75 cfs through the whole chain: both virtual junctions are
    // wet, and BOTH must report it — not just the first one sharing the
    // invert.
    EXPECT_GT(nodeDepth(e, "VJ_A"), 0.02);
    EXPECT_GT(nodeDepth(e, "VJ_B"), 0.02);

    swmm_engine_end(e);
    destroy(e);
}

TEST(VirtualJunction, FvSteepVJDepthTracksFlow) {
    SWMM_Engine e = openModel("vj_fv_steep_datum", fvSteepVJModel(), true);
    stepToEnd(e);

    // Shallow supercritical flow through the junction: the reported depth
    // must be a flow depth (order of the conduits'), neither clamped to zero
    // by the downhill cell's centre datum nor inflated by half the 10 ft
    // conduit drop from the uphill cell's.
    const double d = nodeDepth(e, "VJ_S");
    EXPECT_GT(d, 0.02);
    EXPECT_LT(d, 2.0);

    swmm_engine_end(e);
    destroy(e);
}

// ---------------------------------------------------------------------------
// Initial-state seeding (issue #156)
// ---------------------------------------------------------------------------
// [VIRTUAL_JUNCTIONS] has no init-depth column; the post-parse resolver seeds
// each VJ's head by distance-weighted interpolation between the nearest
// non-virtual nodes along the spliced chain, so an initial pool defined by
// the real nodes has no dry hole at the splice. Dry decks stay bitwise dry.

namespace {

// J_IN (invert 10, init d_in) -> 100 ft -> MID (VJ, invert 9) -> 100 ft ->
// J_OUT (invert 8, init d_out); high-crest weir to O_OUT satisfies the
// >=1-outfall rule without draining the pool at t=0.
std::string seededPoolModel(double d_in, double d_out, double mid_invert = 9.0,
                            const std::string& routing = "DYNWAVE",
                            double len_a = 100.0, double len_b = 100.0) {
    std::ostringstream ss;
    ss << options(routing)
       << "[JUNCTIONS]\n"
          ";;Name  Elev  MaxDepth  InitDepth\n"
          "J_IN    10.0  8.0       " << d_in << "\n"
          "J_OUT   8.0   8.0       " << d_out << "\n\n"
          "[VIRTUAL_JUNCTIONS]\n;;Name  Elev\nMID     " << mid_invert << "\n\n"
          "[OUTFALLS]\n;;Name  Elev  Type  Gated\nO_OUT   8.0   FREE  NO\n\n"
          "[WEIRS]\n;;Name  From   To     Type        CrestHt  Cd\n"
          "W_OVF   J_OUT  O_OUT  TRANSVERSE  7.9      3.33\n\n"
          "[CONDUITS]\n;;Name  From  To     Length  N      Z1  Z2\n"
          "C_A     J_IN  MID    " << len_a << "   0.013  0   0\n"
          "C_B     MID   J_OUT  " << len_b << "   0.013  0   0\n\n"
          "[XSECTIONS]\n;;Link  Shape     G1   G2  G3  G4  Barrels\n"
          "C_A     CIRCULAR  1.0  0   0   0   1\n"
          "C_B     CIRCULAR  1.0  0   0   0   1\n"
          "W_OVF   RECT_OPEN 2.0  1.0 0   0\n";
    return ss.str();
}

} // namespace

TEST(VirtualJunction, InitialStateSeededFromLevelPool) {
    // Level pool: both real nodes at head 13.5 ft -> MID (invert 9) seeds to
    // depth 4.5 ft at open, BEFORE any solver step.
    SWMM_Engine e = openModel("vj_seed_level", seededPoolModel(3.5, 5.5), true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    double d = -1.0;
    ASSERT_EQ(swmm_node_get_depth(e, mid, &d), SWMM_OK);
    EXPECT_NEAR(d, 4.5, 1e-9);
    destroy(e);
}

TEST(VirtualJunction, InitialStateSeededFromSlopedPool) {
    // Unequal heads: 13.5 and 12.5 ft, equal 100 ft legs -> midpoint head
    // 13.0 -> depth 4.0 at MID.
    SWMM_Engine e = openModel("vj_seed_sloped", seededPoolModel(3.5, 4.5), true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    double d = -1.0;
    ASSERT_EQ(swmm_node_get_depth(e, mid, &d), SWMM_OK);
    EXPECT_NEAR(d, 4.0, 1e-9);
    destroy(e);
}

TEST(VirtualJunction, InitialStateDryNeighborsStayDry) {
    // No initial depths anywhere: interpolated head sits below the VJ invert
    // -> seeding must leave the VJ exactly dry (inertness on dry decks).
    SWMM_Engine e = openModel("vj_seed_dry", seededPoolModel(0.0, 0.0), true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    double d = -1.0;
    ASSERT_EQ(swmm_node_get_depth(e, mid, &d), SWMM_OK);
    EXPECT_EQ(d, 0.0);
    destroy(e);
}

TEST(VirtualJunction, InitialStateDryDeckLowVJStaysDry) {
    // The P1 corpus finding (vj_fv_invert_collision analog): BOTH endpoints
    // dry, VJ invert BELOW the endpoint-invert interpolation line (8.5 vs
    // interpolated 9.0). A dry node's head is just its invert — seeding must
    // not manufacture 0.5 ft of water on a bone-dry deck. Requires the
    // wet-endpoint gate.
    SWMM_Engine e = openModel("vj_seed_dry_low",
                              seededPoolModel(0.0, 0.0, 8.5), true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    double d = -1.0;
    ASSERT_EQ(swmm_node_get_depth(e, mid, &d), SWMM_OK);
    EXPECT_EQ(d, 0.0);
    destroy(e);
}

TEST(VirtualJunction, InitialStateHumpVJAbovePoolStaysDry) {
    // Wet endpoints (heads 13.5) but the VJ sits on a hump above the pool
    // surface (invert 14.0): interpolated head < invert -> stays dry.
    SWMM_Engine e = openModel("vj_seed_hump",
                              seededPoolModel(3.5, 5.5, 14.0), true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    double d = -1.0;
    ASSERT_EQ(swmm_node_get_depth(e, mid, &d), SWMM_OK);
    EXPECT_EQ(d, 0.0);
    destroy(e);
}

TEST(VirtualJunction, InitialStateSurvivesTheColdStartReset) {
    // The tests above read the seeded depth straight after open(). That is not
    // where a solver reads it: SWMMEngine::initialize() calls
    // NodeData::reset_state(), which re-derives depth/old_depth/head from
    // init_depth. A seeding that wrote only depth and head therefore vanished
    // before the first routing step, and the study's e4 deck produced .out
    // files byte-identical to the un-seeded engine. This gate is the one that
    // observes the state the solver actually starts from.
    SWMM_Engine e = openModel("vj_seed_survives_init", seededPoolModel(3.5, 5.5),
                              true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);

    ASSERT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);

    double d = -1.0;
    ASSERT_EQ(swmm_node_get_depth(e, mid, &d), SWMM_OK);
    EXPECT_NEAR(d, 4.5, 1e-9);

    swmm_engine_end(e);
    destroy(e);
}

TEST(VirtualJunction, InitialStateIsNotSeededUnderFv) {
    // The seeding is DYNWAVE-only on purpose. Under FV the initial condition
    // lives in the cells, which Router::initFv lays at a uniform depth taken
    // from links.depth; a seeded node head its own cells contradict starts a
    // transient instead of a pool. Measured on the study's e3 deck when the
    // guard was absent: flow-routing continuity 0.000% -> -19.133%, VJ head
    // excursion 4371 m. This pins the restriction so that lifting it has to be
    // a deliberate act with the cell projection landed alongside.
    SWMM_Engine e = openModel("vj_seed_fv_unseeded",
                              seededPoolModel(3.5, 5.5, 9.0, "FV"), true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    double d = -1.0;
    ASSERT_EQ(swmm_node_get_depth(e, mid, &d), SWMM_OK);
    EXPECT_EQ(d, 0.0);
    destroy(e);
}

TEST(VirtualJunction, InitialStateWeightsByDistanceNotByEndOrder) {
    // SlopedPool above uses two 100 ft legs, so (hA*dB + hB*dA)/(dA+dB) and
    // (hA*dA + hB*dB)/(dA+dB) agree to the last bit — it pins the midpoint and
    // says nothing about the weighting. Unequal legs separate them: with heads
    // 13.5 / 12.5 and legs 100 / 300 ft, the near end must dominate
    // (13.5*300 + 12.5*100)/400 = 13.25, depth 4.25 over MID's invert 9.0.
    // The transposed form would give 12.75 -> depth 3.75.
    SWMM_Engine e = openModel("vj_seed_weighted",
                              seededPoolModel(3.5, 4.5, 9.0, "DYNWAVE",
                                              100.0, 300.0), true);
    const int mid = swmm_node_index(e, "MID");
    ASSERT_GE(mid, 0);
    double d = -1.0;
    ASSERT_EQ(swmm_node_get_depth(e, mid, &d), SWMM_OK);
    EXPECT_NEAR(d, 4.25, 1e-9);
    destroy(e);
}
