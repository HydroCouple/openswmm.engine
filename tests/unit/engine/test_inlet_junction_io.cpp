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
 * @file test_inlet_junction_io.cpp
 * @brief Inlet design / inlet-usage / [INLET_JUNCTIONS] input-output tests:
 *        the legacy two-line COMBO encoding, curb throat angles, CUSTOM
 *        capture-curve resolution, [INLET_JUNCTIONS] parse + .inp round-trip,
 *        each validation rule code (623-635), the split/fuse edit pair and the
 *        design / usage C API.
 *
 * @details See plans/INLET_JUNCTION_IMPLEMENTATION_PLAN_2026-09-05.md §2.3-2.6.
 *          Every input and output file is written under ./inlets/ (the working
 *          directory is tests/unit/engine/data) so a reviewer can read them —
 *          no temp files (CLAUDE.md §4.1). Refactored engine only.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_edit.h>
#include <openswmm/engine/openswmm_infrastructure.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "inlets_out";  // data/*_out/ is the gitignored scratch convention

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

const char* kOptions =
    "[OPTIONS]\n"
    "FLOW_UNITS           CFS\n"
    "FLOW_ROUTING         DYNWAVE\n"
    "START_DATE           01/01/2026\n"
    "START_TIME           00:00:00\n"
    "END_DATE             01/01/2026\n"
    "END_TIME             00:10:00\n"
    "REPORT_STEP          00:01:00\n"
    "ROUTING_STEP         1\n"
    "ALLOW_PONDING        NO\n"
    "\n";

// A street reach J_IN -> IJ1 -> O_ST beside a sewer MH1 -> O_SEW. `xsect` and
// `extra` let one deck serve every rule case.
std::string ijModel(const std::string& inlets,
                    const std::string& inlet_junctions,
                    const std::string& street_xsect = "STREET    ST1",
                    const std::string& extra = "") {
    return std::string(kOptions) +
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth\n"
        "J_IN    10.0  5.0\n"
        "MH1     2.0   8.0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev  Type  Gated\n"
        "O_ST    8.0   FREE  NO\n"
        "O_SEW   0.0   FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name  From   To      Length  N      Z1  Z2\n"
        "C_UP    J_IN   IJ1     100.0   0.016  0   0\n"
        "C_DN    IJ1    O_ST    100.0   0.016  0   0\n"
        "C_SEW   MH1    O_SEW   100.0   0.013  0   0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link   Shape     G1   G2  G3  G4  Barrels\n"
        "C_UP     " + street_xsect + "\n"
        "C_DN     " + street_xsect + "\n"
        "C_SEW    CIRCULAR  1.0  0   0   0   1\n"
        "\n"
        "[STREETS]\n"
        ";;Name Tcrown Hcurb Sx nRoad a W Sides Tback Sback nBack\n"
        "ST1  20  0.5  4  0.016  0  0  1  20  4  0.016\n"
        "\n"
        + inlets + "\n"
        + inlet_junctions + extra +
        "\n"
        "[COORDINATES]\n"
        ";;Node  X      Y\n"
        "J_IN    0.0    0.0\n"
        "IJ1     100.0  0.0\n"
        "O_ST    200.0  0.0\n"
        "MH1     100.0  -50.0\n"
        "O_SEW   200.0  -50.0\n";
}

// The same network without the inlet junction: a single street conduit, used
// as the split/fuse baseline.
std::string unsplitModel() {
    return std::string(kOptions) +
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth\n"
        "J_IN    10.0  5.0\n"
        "MH1     2.0   8.0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev  Type  Gated\n"
        "O_ST    8.0   FREE  NO\n"
        "O_SEW   0.0   FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name  From   To      Length  N      Z1  Z2\n"
        "C_ST    J_IN   O_ST    200.0   0.016  0   0\n"
        "C_SEW   MH1    O_SEW   100.0   0.013  0   0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link   Shape     G1   G2  G3  G4  Barrels\n"
        "C_ST     STREET    ST1\n"
        "C_SEW    CIRCULAR  1.0  0   0   0   1\n"
        "\n"
        "[STREETS]\n"
        ";;Name Tcrown Hcurb Sx nRoad a W Sides Tback Sback nBack\n"
        "ST1  20  0.5  4  0.016  0  0  1  20  4  0.016\n"
        "\n"
        "[INLETS]\n"
        "Curb1  CURB  2.0  0.5  VERTICAL\n"
        "\n"
        "[COORDINATES]\n"
        ";;Node  X      Y\n"
        "J_IN    0.0    0.0\n"
        "O_ST    200.0  0.0\n"
        "MH1     100.0  -50.0\n"
        "O_SEW   200.0  -50.0\n";
}

// A design-only deck: one plain circular reach plus whatever [INLETS] (and
// optional extra sections) the caller wants exercised.
std::string designModel(const std::string& inlets, const std::string& extra = "") {
    return std::string(kOptions) +
        "[JUNCTIONS]\n"
        "J_IN    10.0  5.0\n"
        "\n"
        "[OUTFALLS]\n"
        "O_OUT   8.0   FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        "C_MAIN  J_IN  O_OUT  200.0  0.013  0  0\n"
        "\n"
        "[XSECTIONS]\n"
        "C_MAIN  CIRCULAR  1.0  0  0  0  1\n"
        "\n"
        + inlets + extra +
        "\n"
        "[COORDINATES]\n"
        "J_IN    0.0    0.0\n"
        "O_OUT   200.0  0.0\n";
}

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

std::string allErrors(SWMM_Engine e) {
    std::string s;
    const int n = swmm_get_error_count(e);
    for (int i = 0; i < n; ++i) {
        const char* m = swmm_get_error_at(e, i);
        if (m) { s += m; s += "\n"; }
    }
    const char* last = swmm_get_last_error_msg(e);
    if (last) s += last;
    return s;
}

void expectOpenError(const std::string& base, const std::string& text,
                     const std::string& code) {
    int rc = 0;
    SWMM_Engine e = openModel(base, text, false, &rc);
    const std::string errs = allErrors(e);
    EXPECT_NE(rc, 0) << base << " opened cleanly but should have failed with " << code;
    EXPECT_NE(errs.find(code), std::string::npos)
        << base << " did not report " << code << ":\n" << errs;
    destroy(e);
}

} // namespace

// ---------------------------------------------------------------------------
// [INLETS] grammar
// ---------------------------------------------------------------------------

// Legacy encodes a combination inlet as a GRATE line and a CURB line sharing
// one name; both halves must land in a single COMBO design.
TEST(InletJunctionIO, ComboMergeFromTwoLines) {
    SWMM_Engine e = openModel("inlet_combo", designModel(
        "[INLETS]\n"
        "Combo1  GRATE  2.0  2.0  P_BAR-50\n"
        "Combo1  CURB   3.0  0.5  HORIZONTAL\n"), true);

    ASSERT_EQ(swmm_inlet_count(e), 1);
    char type[32] = {0};
    ASSERT_EQ(swmm_inlet_get_type(e, 0, type, sizeof(type)), SWMM_OK);
    EXPECT_STREQ(type, "COMBO");

    SWMM_InletDesign d{};
    ASSERT_EQ(swmm_inlet_get_design(e, 0, &d), SWMM_OK);
    EXPECT_EQ(d.type, SWMM_INLET_COMBO);
    EXPECT_DOUBLE_EQ(d.grate_length, 2.0);
    EXPECT_DOUBLE_EQ(d.grate_width, 2.0);
    EXPECT_EQ(d.grate_type, SWMM_GRATE_P_BAR_50);
    EXPECT_DOUBLE_EQ(d.curb_length, 3.0);
    EXPECT_DOUBLE_EQ(d.curb_height, 0.5);
    EXPECT_EQ(d.throat, SWMM_THROAT_HORIZONTAL);

    // A COMBO is written back as the legacy two-line form.
    const std::string out = outPath("inlet_combo_rt.inp");
    ASSERT_EQ(swmm_model_write(e, out.c_str()), 0);
    const std::string txt = readFile(out);
    {
        std::size_t n = 0;
        for (std::size_t p = txt.find("Combo1"); p != std::string::npos;
             p = txt.find("Combo1", p + 1)) ++n;
        EXPECT_EQ(n, 2u) << "a COMBO must be written as two lines:\n" << txt;
    }
    EXPECT_NE(txt.find("HORIZONTAL"), std::string::npos) << txt;
    destroy(e);

    // ...and reading it back yields the same single COMBO design.
    SWMM_Engine e2 = openModel("inlet_combo2", txt, true);
    EXPECT_EQ(swmm_inlet_count(e2), 1);
    SWMM_InletDesign d2{};
    ASSERT_EQ(swmm_inlet_get_design(e2, 0, &d2), SWMM_OK);
    EXPECT_EQ(d2.type, SWMM_INLET_COMBO);
    EXPECT_DOUBLE_EQ(d2.curb_length, 3.0);
    EXPECT_EQ(d2.throat, SWMM_THROAT_HORIZONTAL);
    destroy(e2);
}

// A third line with the same name is an ordinary duplicate ID.
TEST(InletJunctionIO, DuplicateInletNameRejected) {
    expectOpenError("inlet_dup", designModel(
        "[INLETS]\n"
        "G1  GRATE  2.0  2.0  P_BAR-50\n"
        "G1  GRATE  3.0  3.0  P_BAR-30\n"), "207");
}

TEST(InletJunctionIO, CurbThroatParses) {
    SWMM_Engine e = openModel("inlet_throat", designModel(
        "[INLETS]\n"
        "CurbH  CURB  2.0  0.5  HORIZONTAL\n"
        "CurbI  CURB  2.0  0.5  INCLINED\n"
        "CurbV  CURB  2.0  0.5  VERTICAL\n"
        "CurbD  CURB  2.0  0.5\n"
        "DropC  DROP_CURB  2.0  0.5\n"), true);

    const int throats[5] = {SWMM_THROAT_HORIZONTAL, SWMM_THROAT_INCLINED,
                            SWMM_THROAT_VERTICAL, SWMM_THROAT_VERTICAL,
                            SWMM_THROAT_VERTICAL};
    for (int i = 0; i < 5; ++i) {
        SWMM_InletDesign d{};
        ASSERT_EQ(swmm_inlet_get_design(e, i, &d), SWMM_OK) << "design " << i;
        EXPECT_EQ(d.throat, throats[i]) << "design " << i;
        EXPECT_DOUBLE_EQ(d.curb_length, 2.0);
        EXPECT_DOUBLE_EQ(d.curb_height, 0.5);
    }
    // DROP_CURB writes no throat token; CURB does.
    const std::string out = outPath("inlet_throat_rt.inp");
    ASSERT_EQ(swmm_model_write(e, out.c_str()), 0);
    const std::string txt = readFile(out);
    EXPECT_NE(txt.find("INCLINED"), std::string::npos) << txt;
    destroy(e);

    SWMM_Engine e2 = openModel("inlet_throat2", txt, true);
    SWMM_InletDesign d{};
    ASSERT_EQ(swmm_inlet_get_design(e2, 1, &d), SWMM_OK);
    EXPECT_EQ(d.throat, SWMM_THROAT_INCLINED);
    destroy(e2);
}

TEST(InletJunctionIO, CustomCurveKindResolves) {
    const std::string curves =
        "\n[CURVES]\n"
        ";;Name  Type       X     Y\n"
        "DIV1     DIVERSION  0.0   0.0\n"
        "DIV1                1.0   0.5\n"
        "RAT1     RATING     0.0   0.0\n"
        "RAT1                1.0   2.0\n";

    SWMM_Engine e = openModel("inlet_custom", designModel(
        "[INLETS]\n"
        "CustD  CUSTOM  DIV1\n"
        "CustR  CUSTOM  RAT1\n", curves), true);

    SWMM_InletDesign d{};
    ASSERT_EQ(swmm_inlet_get_design(e, 0, &d), SWMM_OK);
    EXPECT_EQ(d.type, SWMM_INLET_CUSTOM);
    EXPECT_STREQ(d.curve_id, "DIV1");
    EXPECT_EQ(d.curve_kind, SWMM_INLET_CURVE_DIVERSION);

    ASSERT_EQ(swmm_inlet_get_design(e, 1, &d), SWMM_OK);
    EXPECT_STREQ(d.curve_id, "RAT1");
    EXPECT_EQ(d.curve_kind, SWMM_INLET_CURVE_RATING);
    destroy(e);

    // An unknown curve name is an undefined-object error.
    expectOpenError("inlet_custom_bad", designModel(
        "[INLETS]\n"
        "CustX  CUSTOM  NOPE\n", curves), "209");
}

// swmm_inlet_set_design must persist curve_kind (the [INLETS] grammar has no
// kind token, so a lost kind silently degrades a RATING inlet to DIVERSION).
TEST(InletJunctionIO, CustomCurveKindRoundTripsThroughSetDesign) {
    const std::string curves =
        "\n[CURVES]\n"
        ";;Name  Type       X     Y\n"
        "DIV1     DIVERSION  0.0   0.0\n"
        "DIV1                1.0   0.5\n"
        "RAT1     RATING     0.0   0.0\n"
        "RAT1                1.0   2.0\n"
        "STO1     STORAGE    0.0   1.0\n"
        "STO1                1.0   2.0\n";

    SWMM_Engine e = openModel("inlet_custom_set", designModel(
        "[INLETS]\n"
        "CustD  CUSTOM  DIV1\n", curves), true);

    // Retarget the design at the RATING curve: get_design must read the new
    // kind back, not the DIVERSION kind the parser resolved.
    SWMM_InletDesign d{};
    ASSERT_EQ(swmm_inlet_get_design(e, 0, &d), SWMM_OK);
    ASSERT_EQ(d.curve_kind, SWMM_INLET_CURVE_DIVERSION);
    std::snprintf(d.curve_id, sizeof(d.curve_id), "RAT1");
    d.curve_kind = SWMM_INLET_CURVE_RATING;
    ASSERT_EQ(swmm_inlet_set_design(e, 0, &d), SWMM_OK);

    SWMM_InletDesign back{};
    ASSERT_EQ(swmm_inlet_get_design(e, 0, &back), SWMM_OK);
    EXPECT_STREQ(back.curve_id, "RAT1");
    EXPECT_EQ(back.curve_kind, SWMM_INLET_CURVE_RATING);

    // An unset kind is filled in from the existing curve's own type.
    std::snprintf(d.curve_id, sizeof(d.curve_id), "DIV1");
    d.curve_kind = SWMM_INLET_CURVE_NONE;
    ASSERT_EQ(swmm_inlet_set_design(e, 0, &d), SWMM_OK);
    ASSERT_EQ(swmm_inlet_get_design(e, 0, &back), SWMM_OK);
    EXPECT_EQ(back.curve_kind, SWMM_INLET_CURVE_DIVERSION);

    // A kind that contradicts the named curve is rejected, and the row is
    // left untouched.
    d.curve_kind = SWMM_INLET_CURVE_RATING;   // DIV1 is a DIVERSION curve
    EXPECT_EQ(swmm_inlet_set_design(e, 0, &d), SWMM_ERR_BADPARAM);
    // ...as is a curve that is neither DIVERSION nor RATING.
    std::snprintf(d.curve_id, sizeof(d.curve_id), "STO1");
    d.curve_kind = SWMM_INLET_CURVE_NONE;
    EXPECT_EQ(swmm_inlet_set_design(e, 0, &d), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_inlet_get_design(e, 0, &back), SWMM_OK);
    EXPECT_STREQ(back.curve_id, "DIV1");
    EXPECT_EQ(back.curve_kind, SWMM_INLET_CURVE_DIVERSION);
    destroy(e);
}

// ---------------------------------------------------------------------------
// [INLET_JUNCTIONS] parse + round-trip
// ---------------------------------------------------------------------------

namespace {
const char* kCurbInlets = "[INLETS]\nCurb1  CURB  2.0  0.5  VERTICAL\n";
const char* kIjSection =
    "[INLET_JUNCTIONS]\n"
    ";;Name Elev MaxDepth Inlet CaptureNode #Inlets %Clog Qmax aLocal wLocal Placement\n"
    "IJ1  9.0  0.5  Curb1  MH1  2  10  1.5  0.1  2.0  ON_SAG\n";
} // namespace

TEST(InletJunctionIO, InletJunctionParsesAndRoundTrips) {
    SWMM_Engine e = openModel("ij_roundtrip", ijModel(kCurbInlets, kIjSection), true);

    const int ij = swmm_node_index(e, "IJ1");
    ASSERT_GE(ij, 0);
    int flag = 0;
    ASSERT_EQ(swmm_node_is_inlet(e, ij, &flag), SWMM_OK);
    EXPECT_EQ(flag, 1);
    ASSERT_EQ(swmm_node_is_virtual(e, ij, &flag), SWMM_OK);
    EXPECT_EQ(flag, 1) << "an inlet junction is always a virtual junction";

    const int row = swmm_inlet_usage_find_node(e, ij);
    ASSERT_GE(row, 0);
    SWMM_InletUsage u{};
    ASSERT_EQ(swmm_inlet_usage_get(e, row, &u), SWMM_OK);
    EXPECT_EQ(u.host_kind, SWMM_INLET_HOST_NODE);
    EXPECT_EQ(u.host_idx, ij);
    EXPECT_EQ(u.design_idx, swmm_inlet_index(e, "Curb1"));
    EXPECT_EQ(u.capture_node_idx, swmm_node_index(e, "MH1"));
    EXPECT_EQ(u.num_inlets, 2);
    EXPECT_NEAR(u.pct_clogged, 10.0, 1e-9);
    EXPECT_NEAR(u.flow_limit, 1.5, 1e-9);
    EXPECT_NEAR(u.local_depress, 0.1, 1e-9);
    EXPECT_NEAR(u.local_width, 2.0, 1e-9);
    EXPECT_EQ(u.placement, SWMM_INLET_ON_SAG);
    // No conduit-attribute rows exist, so the node row is the only one.
    EXPECT_EQ(swmm_inlet_usage_count(e), 1);
    EXPECT_EQ(swmm_inlet_usage_find_link(e, swmm_link_index(e, "C_UP")), -1);

    const std::string p1 = outPath("ij_roundtrip_pass1.inp");
    ASSERT_EQ(swmm_model_write(e, p1.c_str()), 0);
    destroy(e);

    const std::string t1 = readFile(p1);
    EXPECT_NE(t1.find("[INLET_JUNCTIONS]"), std::string::npos) << t1;
    EXPECT_EQ(t1.find("[VIRTUAL_JUNCTIONS]"), std::string::npos)
        << "an inlet junction must not also appear in [VIRTUAL_JUNCTIONS]";
    EXPECT_EQ(t1.find("[INLET_USAGE]"), std::string::npos)
        << "node-hosted usage rows belong to [INLET_JUNCTIONS] only";

    // From the second pass on the file is a fixed point. (The first pass may
    // still reorder [COORDINATES]: the writer emits [INLET_JUNCTIONS] between
    // [JUNCTIONS] and [OUTFALLS], so a hand-written deck that declares the
    // inlet junction later gets its node indices renumbered exactly once.)
    SWMM_Engine e2 = openModel("ij_roundtrip2", t1, true);
    const std::string p2 = outPath("ij_roundtrip_pass2.inp");
    ASSERT_EQ(swmm_model_write(e2, p2.c_str()), 0);
    destroy(e2);
    const std::string t2 = readFile(p2);

    SWMM_Engine e3 = openModel("ij_roundtrip3", t2, true);
    const std::string p3 = outPath("ij_roundtrip_pass3.inp");
    ASSERT_EQ(swmm_model_write(e3, p3.c_str()), 0);
    // The reloaded model still carries the inlet junction and its usage.
    const int ij3 = swmm_node_index(e3, "IJ1");
    ASSERT_GE(ij3, 0);
    int f3 = 0;
    ASSERT_EQ(swmm_node_is_inlet(e3, ij3, &f3), SWMM_OK);
    EXPECT_EQ(f3, 1);
    ASSERT_GE(swmm_inlet_usage_find_node(e3, ij3), 0);
    destroy(e3);
    EXPECT_EQ(t2, readFile(p3)) << "[INLET_JUNCTIONS] round-trip is not byte-stable";
}

// ---------------------------------------------------------------------------
// Validation rule codes 623-633
// ---------------------------------------------------------------------------

TEST(InletJunctionIO, ValidationRuleCodes) {
    // 623: the attached conduits are not STREET (a curb inlet needs one).
    expectOpenError("ij_err_not_street",
        ijModel(kCurbInlets, kIjSection, "CIRCULAR  2.0  0   0   0   1"), "623");

    // 625: unknown inlet design. With no design the node also has nothing
    // assigned, so 633 is raised alongside it.
    {
        const std::string m = ijModel(kCurbInlets,
            "[INLET_JUNCTIONS]\nIJ1  9.0  0.5  NoSuchInlet  MH1\n");
        expectOpenError("ij_err_design", m, "625");
        expectOpenError("ij_err_no_usage", m, "633");
    }

    // 627: capture node missing / the inlet junction itself / virtual.
    expectOpenError("ij_err_capture_missing", ijModel(kCurbInlets,
        "[INLET_JUNCTIONS]\nIJ1  9.0  0.5  Curb1  NoSuchNode\n"), "627");
    expectOpenError("ij_err_capture_self", ijModel(kCurbInlets,
        "[INLET_JUNCTIONS]\nIJ1  9.0  0.5  Curb1  IJ1\n"), "627");

    // 629: a conduit of the pair also carries an [INLET_USAGE] row.
    expectOpenError("ij_err_usage_on_pair", ijModel(kCurbInlets, kIjSection,
        "STREET    ST1",
        "\n[INLET_USAGE]\n;;Link Inlet Node\nC_UP  Curb1  MH1\n"), "629");

    // 631: more than 11 tokens.
    expectOpenError("ij_err_tokens", ijModel(kCurbInlets,
        "[INLET_JUNCTIONS]\n"
        "IJ1  9.0  0.5  Curb1  MH1  1  0  0  0  0  AUTOMATIC  EXTRA\n"), "631");
}

// ---------------------------------------------------------------------------
// Edit operations: split into an inlet junction, then fuse back
// ---------------------------------------------------------------------------

TEST(InletJunctionIO, SplitInletFuseRoundTrip) {
    SWMM_Engine e = openModel("ij_splitfuse", unsplitModel(), true);

    const std::string before = outPath("ij_splitfuse_before.inp");
    ASSERT_EQ(swmm_model_write(e, before.c_str()), 0);
    const int n_nodes0 = swmm_node_count(e);
    const int n_links0 = swmm_link_count(e);

    const int c_st = swmm_link_index(e, "C_ST");
    ASSERT_GE(c_st, 0);
    int new_node = -1, new_link = -1;
    ASSERT_EQ(swmm_conduit_split_inlet(e, c_st, 0.5, "IJ1", "C_ST_B",
                                       "Curb1", "MH1", &new_node, &new_link),
              SWMM_OK) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_node_count(e), n_nodes0 + 1);
    EXPECT_EQ(swmm_link_count(e), n_links0 + 1);

    int flag = 0;
    ASSERT_EQ(swmm_node_is_inlet(e, new_node, &flag), SWMM_OK);
    EXPECT_EQ(flag, 1);

    const int row = swmm_inlet_usage_find_node(e, new_node);
    ASSERT_GE(row, 0);
    SWMM_InletUsage u{};
    ASSERT_EQ(swmm_inlet_usage_get(e, row, &u), SWMM_OK);
    EXPECT_EQ(u.design_idx, swmm_inlet_index(e, "Curb1"));
    EXPECT_EQ(u.capture_node_idx, swmm_node_index(e, "MH1"));

    const std::string split_inp = outPath("ij_splitfuse_split.inp");
    ASSERT_EQ(swmm_model_write(e, split_inp.c_str()), 0);
    const std::string split_txt = readFile(split_inp);
    EXPECT_NE(split_txt.find("[INLET_JUNCTIONS]"), std::string::npos) << split_txt;

    // Reloading the split model is the real check that the new downstream half
    // kept the [STREETS] reference of the conduit it came from: without it
    // rule 623 would reject the inlet junction.
    {
        SWMM_Engine reload = openModel("ij_splitfuse_reload", split_txt, true);
        const int ij = swmm_node_index(reload, "IJ1");
        ASSERT_GE(ij, 0);
        int f2 = 0;
        ASSERT_EQ(swmm_node_is_inlet(reload, ij, &f2), SWMM_OK);
        EXPECT_EQ(f2, 1);
        destroy(reload);
    }

    // Bad references are refused without touching the model.
    int nn = -1, nl = -1;
    EXPECT_EQ(swmm_conduit_split_inlet(e, c_st, 0.5, "IJ2", "C_ST_C",
                                       "NoSuchInlet", "MH1", &nn, &nl), 625);
    EXPECT_EQ(swmm_conduit_split_inlet(e, c_st, 0.5, "IJ2", "C_ST_C",
                                       "Curb1", "NoSuchNode", &nn, &nl), 627);
    EXPECT_EQ(swmm_node_count(e), n_nodes0 + 1);

    int surviving = -1;
    ASSERT_EQ(swmm_inlet_junction_fuse(e, new_node, &surviving), SWMM_OK);
    EXPECT_EQ(swmm_node_count(e), n_nodes0);
    EXPECT_EQ(swmm_link_count(e), n_links0);
    EXPECT_EQ(swmm_inlet_usage_count(e), 0) << "fuse must drop the usage row";

    const std::string after = outPath("ij_splitfuse_after.inp");
    ASSERT_EQ(swmm_model_write(e, after.c_str()), 0);
    EXPECT_EQ(readFile(before), readFile(after))
        << "split→fuse did not restore the original .inp byte-identically";
    destroy(e);
}

// ---------------------------------------------------------------------------
// C API: usage rows and design read/write
// ---------------------------------------------------------------------------

TEST(InletJunctionIO, UsageSetGetRemove) {
    SWMM_Engine e = openModel("ij_usage_api", ijModel(kCurbInlets, kIjSection), true);

    const int ij   = swmm_node_index(e, "IJ1");
    const int mh1  = swmm_node_index(e, "MH1");
    const int c_up = swmm_link_index(e, "C_UP");
    const int curb = swmm_inlet_index(e, "Curb1");
    ASSERT_GE(ij, 0); ASSERT_GE(mh1, 0); ASSERT_GE(c_up, 0); ASSERT_GE(curb, 0);

    // Replace the node-hosted row in place (one row per host).
    SWMM_InletUsage u{};
    u.host_kind = SWMM_INLET_HOST_NODE;
    u.host_idx = ij;
    u.design_idx = curb;
    u.capture_node_idx = mh1;
    u.num_inlets = 3;
    u.pct_clogged = 25.0;
    u.flow_limit = 2.5;
    u.local_depress = 0.2;
    u.local_width = 3.0;
    u.placement = SWMM_INLET_ON_GRADE;
    int row = -1;
    ASSERT_EQ(swmm_inlet_usage_set(e, &u, &row), SWMM_OK);
    EXPECT_EQ(swmm_inlet_usage_count(e), 1);
    EXPECT_EQ(row, swmm_inlet_usage_find_node(e, ij));

    SWMM_InletUsage back{};
    ASSERT_EQ(swmm_inlet_usage_get(e, row, &back), SWMM_OK);
    EXPECT_EQ(back.num_inlets, 3);
    EXPECT_NEAR(back.pct_clogged, 25.0, 1e-9);
    EXPECT_EQ(back.placement, SWMM_INLET_ON_GRADE);

    // Rejections: bad clogging, a capture node equal to the host, a link host
    // that is not a conduit index, and a node host without is_inlet.
    SWMM_InletUsage bad = u;
    bad.pct_clogged = 120.0;
    EXPECT_EQ(swmm_inlet_usage_set(e, &bad, nullptr), SWMM_ERR_BADPARAM);
    bad = u; bad.capture_node_idx = ij;
    EXPECT_EQ(swmm_inlet_usage_set(e, &bad, nullptr), SWMM_ERR_BADPARAM);
    bad = u; bad.host_idx = mh1;   // a plain junction cannot host a node row
    EXPECT_EQ(swmm_inlet_usage_set(e, &bad, nullptr), SWMM_ERR_BADPARAM);

    // A link-hosted row on the sewer pipe violates the shape rule (635).
    SWMM_InletUsage link_usage{};
    link_usage.host_kind = SWMM_INLET_HOST_LINK;
    link_usage.host_idx = swmm_link_index(e, "C_SEW");
    link_usage.design_idx = curb;
    link_usage.capture_node_idx = mh1;
    link_usage.num_inlets = 1;
    EXPECT_EQ(swmm_inlet_usage_set(e, &link_usage, nullptr), 635);

    // The same design on the street conduit is accepted and appended.
    link_usage.host_idx = c_up;
    int link_row = -1;
    ASSERT_EQ(swmm_inlet_usage_set(e, &link_usage, &link_row), SWMM_OK);
    EXPECT_EQ(swmm_inlet_usage_count(e), 2);
    EXPECT_EQ(swmm_inlet_usage_find_link(e, c_up), link_row);

    ASSERT_EQ(swmm_inlet_usage_remove(e, link_row), SWMM_OK);
    EXPECT_EQ(swmm_inlet_usage_count(e), 1);
    EXPECT_EQ(swmm_inlet_usage_find_link(e, c_up), -1);
    destroy(e);
}

TEST(InletJunctionIO, DesignGetSet) {
    SWMM_Engine e = openModel("ij_design_api", designModel(
        "[INLETS]\n"
        "G1  GRATE  2.0  2.0  P_BAR-50\n"), true);

    SWMM_InletDesign d{};
    ASSERT_EQ(swmm_inlet_get_design(e, 0, &d), SWMM_OK);
    EXPECT_EQ(d.type, SWMM_INLET_GRATE);
    EXPECT_DOUBLE_EQ(d.grate_length, 2.0);

    // Retype in place: GRATE -> COMBO with both halves and a GENERIC grate.
    d.type = SWMM_INLET_COMBO;
    d.grate_length = 3.0;
    d.grate_width = 1.5;
    d.grate_type = SWMM_GRATE_GENERIC;
    d.open_area = 0.8;
    d.splash_veloc = 4.0;
    d.curb_length = 5.0;
    d.curb_height = 0.6;
    d.throat = SWMM_THROAT_INCLINED;
    ASSERT_EQ(swmm_inlet_set_design(e, 0, &d), SWMM_OK);

    SWMM_InletDesign back{};
    ASSERT_EQ(swmm_inlet_get_design(e, 0, &back), SWMM_OK);
    EXPECT_EQ(back.type, SWMM_INLET_COMBO);
    EXPECT_DOUBLE_EQ(back.grate_length, 3.0);
    EXPECT_EQ(back.grate_type, SWMM_GRATE_GENERIC);
    EXPECT_DOUBLE_EQ(back.open_area, 0.8);
    EXPECT_DOUBLE_EQ(back.curb_length, 5.0);
    EXPECT_EQ(back.throat, SWMM_THROAT_INCLINED);

    // Constraint violations are refused (a COMBO needs both halves).
    SWMM_InletDesign bad = back;
    bad.curb_height = 0.0;
    EXPECT_EQ(swmm_inlet_set_design(e, 0, &bad), SWMM_ERR_BADPARAM);
    bad = back; bad.open_area = 1.5;
    EXPECT_EQ(swmm_inlet_set_design(e, 0, &bad), SWMM_ERR_BADPARAM);
    bad = back; bad.type = SWMM_INLET_CUSTOM; bad.curve_id[0] = '\0';
    EXPECT_EQ(swmm_inlet_set_design(e, 0, &bad), SWMM_ERR_BADPARAM);

    // The legacy 5-arg convenience API keeps working; for a curb design it
    // maps length/width onto the curb columns.
    SWMM_InletDesign curb{};
    curb.type = SWMM_INLET_CURB;
    curb.curb_length = 4.0;
    curb.curb_height = 0.5;
    curb.throat = SWMM_THROAT_VERTICAL;
    ASSERT_EQ(swmm_inlet_set_design(e, 0, &curb), SWMM_OK);

    double len = 0.0, wid = 0.0;
    ASSERT_EQ(swmm_inlet_get_params(e, 0, &len, &wid, nullptr, 0, nullptr, nullptr), SWMM_OK);
    EXPECT_DOUBLE_EQ(len, 4.0);
    EXPECT_DOUBLE_EQ(wid, 0.5);
    ASSERT_EQ(swmm_inlet_set_params(e, 0, 6.0, 0.75, "", 0.0, 0.0), SWMM_OK);
    ASSERT_EQ(swmm_inlet_get_design(e, 0, &back), SWMM_OK);
    EXPECT_DOUBLE_EQ(back.curb_length, 6.0);
    EXPECT_DOUBLE_EQ(back.curb_height, 0.75);

    // Comments round-trip through the design API.
    ASSERT_EQ(swmm_inlet_set_comment(e, 0, "standard 6 ft curb opening"), SWMM_OK);
    char cbuf[64] = {0};
    ASSERT_EQ(swmm_inlet_get_comment(e, 0, cbuf, sizeof(cbuf)), SWMM_OK);
    EXPECT_STREQ(cbuf, "standard 6 ft curb opening");
    destroy(e);
}
