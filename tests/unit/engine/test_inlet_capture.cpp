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
 * @file test_inlet_capture.cpp
 * @brief Street inlet capture: HEC-22 kernel values and end-to-end capture
 *        for both host kinds (conduit attribute and inlet junction).
 *
 * @details The kernel cases re-derive each HEC-22 equation inline from the
 *          stated inputs and compare against the kernel, plus a hand-computed
 *          anchor value so a transcription slip in the test itself is caught.
 *          Deck I/O lands under ./inlets/ (working dir is
 *          tests/unit/engine/data) for review — no temp files
 *          (project convention, CLAUDE.md §4.1).
 *
 * @see plans/INLET_JUNCTION_IMPLEMENTATION_PLAN_2026-09-05.md
 * @see src/legacy/engine/inlet.c — the parity oracle
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_massbalance.h>

#include "hydraulics/Inlet.hpp"
#include "data/TableData.hpp"

namespace fs = std::filesystem;
using namespace openswmm;
using namespace openswmm::inlet;

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

/// The reference street used by every on-grade kernel case:
/// Sx = 2 %, SL = 1 %, n = 0.016, no gutter depression, crown far away.
Geom refStreet() {
    Geom g;
    g.sx      = 0.02;
    g.sl      = 0.01;
    g.sw      = 0.02;
    g.a       = 0.0;
    g.w       = 0.0;
    g.n       = 0.016;
    g.nsides  = 1;
    g.t_crown = 20.0;
    g.qfactor = (0.56 / 0.016) * std::sqrt(0.01) * std::pow(0.02, 1.67);
    g.beta    = 1.486 * std::sqrt(0.01) / 0.016;
    return g;
}

Design grateP50_2x2() {
    Design d;
    d.type         = static_cast<int>(InletType::GRATE);
    d.grate_type   = static_cast<int>(GrateType::P_BAR_50);
    d.grate_length = 2.0;
    d.grate_width  = 2.0;
    return d;
}

} // namespace

// ============================================================================
// Spread — HEC-22 Eq(4-2)
// ============================================================================

TEST(InletKernel, FlowSpreadUniformCrossSlope) {
    const Geom g = refStreet();
    const double Q = 2.0;

    const double expect = std::pow(Q / g.qfactor, 0.375);   // Eq(4-2)
    EXPECT_NEAR(getFlowSpread(g, Q), expect, 1e-12);
    // Hand anchor: f = 3.5 · 0.1 · 0.02^1.67 ≈ 0.005086 → T ≈ 9.39 ft
    EXPECT_NEAR(getFlowSpread(g, Q), 9.394, 0.02);
}

TEST(InletKernel, FlowSpreadClippedAtCrown) {
    Geom g = refStreet();
    g.t_crown = 5.0;
    EXPECT_NEAR(getFlowSpread(g, 2.0), 5.0, 1e-12);
}

// ============================================================================
// On-grade grate — HEC-22 Eq(4-16)/(4-18)/(4-19)/(4-21)
// ============================================================================

TEST(InletKernel, OnGradeGrateP50) {
    Geom g = refStreet();
    const Design d = grateP50_2x2();
    const double Q = 2.0;

    // --- independent re-derivation of the HEC-22 chain
    const double T  = std::pow(Q / g.qfactor, 0.375);       // Eq(4-2)
    const double A  = T * T * g.sx / 2.0;
    const double V  = Q / A;
    const double Eo = 1.0 - std::pow(1.0 - d.grate_width / T, 2.67);  // Eq(4-16)
    const double Vo = 2.22 + 4.03 * 2.0 - 0.65 * 4.0 + 0.06 * 8.0;    // Chart 5B
    const double Rf = (V > Vo) ? 1.0 - 0.09 * (V - Vo) : 1.0;         // Eq(4-18)
    const double Rs = 1.0 / (1.0 + 0.15 * std::pow(V, 1.8)
                             / g.sx / std::pow(d.grate_length, 2.3)); // Eq(4-19)
    const double expect = Q * (Rf * Eo + Rs * (1.0 - Eo));            // Eq(4-21)

    g.t = getFlowSpread(g, Q);
    EXPECT_NEAR(getGrateInletCapture(d, g, Q), expect, 1e-10);
    // Hand anchor: V ≈ 2.27 ft/s < Vo = 8.16 ⇒ Rf = 1, Eo ≈ 0.472,
    // Rs ≈ 0.131 ⇒ Qc ≈ 1.083 cfs
    EXPECT_NEAR(expect, 1.083, 0.01);
    EXPECT_LT(expect, Q);
}

TEST(InletKernel, SplashOverVelocityMatchesChart5B) {
    // The seven fitted curves (inlet.c:152-159). GENERIC has no curve — its
    // splash-over velocity is a design field — and the legacy table has only
    // seven rows, so index 7 must not read past the end.
    static const double c[7][4] = {
        {2.22, 4.03, 0.65, 0.06}, {0.74, 2.44, 0.27, 0.02},
        {1.76, 3.12, 0.45, 0.03}, {0.30, 4.85, 1.31, 0.15},
        {0.99, 2.64, 0.36, 0.03}, {0.51, 2.34, 0.20, 0.01},
        {0.28, 2.28, 0.18, 0.01}};
    const double L = 2.0;
    for (int t = 0; t < 7; ++t) {
        const double expect = c[t][0] + c[t][1] * L - c[t][2] * L * L
                            + c[t][3] * L * L * L;
        EXPECT_NEAR(getSplashOverVelocity(t, L), expect, 1e-12) << "type " << t;
    }
    EXPECT_DOUBLE_EQ(getSplashOverVelocity(
        static_cast<int>(GrateType::GENERIC), L), 0.0);
    EXPECT_DOUBLE_EQ(getSplashOverVelocity(99, L), 0.0);
}

TEST(InletKernel, SplashOverReducesFrontalCapture) {
    // Two GENERIC grates, identical but for the splash-over velocity: the one
    // the flow splashes over (Vo = 0) loses frontal capture through Eq(4-18).
    Design keeps = grateP50_2x2();
    keeps.grate_type   = static_cast<int>(GrateType::GENERIC);
    keeps.frac_open_area = 1.0;
    keeps.splash_veloc = 100.0;         // never splashed over ⇒ Rf = 1

    Design splashes = keeps;
    splashes.splash_veloc = 0.0;        // always splashed over ⇒ Rf < 1

    Geom g1 = refStreet();
    Geom g2 = refStreet();
    const double Q = 2.0;
    g1.t = getFlowSpread(g1, Q);
    g2.t = getFlowSpread(g2, Q);

    const double q_keeps    = getGrateInletCapture(keeps, g1, Q);
    const double q_splashes = getGrateInletCapture(splashes, g2, Q);
    EXPECT_LT(q_splashes, q_keeps);
}

// ============================================================================
// On-grade curb opening — HEC-22 Eq(4-22a)/(4-23)
// ============================================================================

TEST(InletKernel, OnGradeCurbOpening) {
    Geom g = refStreet();
    const double Q = 2.0;
    const double L = 5.0;

    const double Se = g.sx;                                  // no depression
    const double Lt = 0.6 * std::pow(Q, 0.42) * std::pow(g.sl, 0.3)
                    * std::pow(1.0 / (g.n * Se), 0.6);       // Eq(4-22a)
    ASSERT_GT(Lt, L);
    const double E = 1.0 - std::pow(1.0 - L / Lt, 1.8);      // Eq(4-23)

    g.t = getFlowSpread(g, Q);
    EXPECT_NEAR(getCurbInletCapture(g, Q, L), E * Q, 1e-10);
    // Hand anchor: Lt ≈ 25.2 ft, E ≈ 0.328 ⇒ Qc ≈ 0.656 cfs
    EXPECT_NEAR(E * Q, 0.656, 0.005);
}

TEST(InletKernel, OnGradeCurbFullCaptureAtOrAboveLt) {
    Geom g = refStreet();
    g.t = getFlowSpread(g, 2.0);
    // An opening longer than Lt intercepts everything (E clamped at 1).
    EXPECT_NEAR(getCurbInletCapture(g, 2.0, 60.0), 2.0, 1e-12);
}

// ============================================================================
// On-sag grate — HEC-22 Eq(4-26)/(4-27)
// ============================================================================

TEST(InletKernel, OnSagGrateWeirRegime) {
    const Geom g = refStreet();          // Sw = Sx = 0.02
    const Design d = grateP50_2x2();
    const double depth = 0.02;

    // Spread is inside the grate width ⇒ Wg collapses to d/Sw.
    const double Wg = depth / g.sw;                 // 1.0 ft
    const double di = depth - (Wg / 2.0) * g.sw;    // 0.01 ft
    const double P  = d.grate_length + 2.0 * Wg;    // 4.0 ft
    const double Ao = d.grate_length * Wg * 0.90;   // P_BAR-50 opening ratio
    ASSERT_LE(depth, 1.79 * Ao / P);
    const double expect = 3.0 * P * std::pow(di, 1.5);       // Eq(4-26)

    EXPECT_NEAR(getOnSagInletCapture(d, g, depth), expect, 1e-12);
    EXPECT_NEAR(expect, 0.012, 1e-6);                        // hand anchor
}

TEST(InletKernel, OnSagGrateOrificeRegime) {
    const Geom g = refStreet();
    const Design d = grateP50_2x2();
    const double depth = 1.5;

    const double Wg = d.grate_width;                // depth > Wg·Sw
    const double di = depth - (Wg / 2.0) * g.sw;    // 1.48 ft
    const double P  = d.grate_length + 2.0 * Wg;    // 6.0 ft
    const double Ao = d.grate_length * Wg * 0.90;   // 3.6 ft²
    ASSERT_GT(depth, 1.79 * Ao / P);
    const double expect = 0.67 * Ao * std::sqrt(2.0 * 32.16 * di);  // Eq(4-27)

    EXPECT_NEAR(getOnSagInletCapture(d, g, depth), expect, 1e-10);
    EXPECT_NEAR(expect, 23.53, 0.02);                        // hand anchor
}

// ============================================================================
// On-sag curb opening — HEC-22 Eq(4-30)/(4-31a) + the transition band
// ============================================================================

namespace {
Design curbHalfFoot3ft() {
    Design d;
    d.type        = static_cast<int>(InletType::CURB);
    d.curb_length = 3.0;
    d.curb_height = 0.5;
    d.curb_throat = static_cast<int>(ThroatAngle::VERTICAL);
    return d;
}
} // namespace

TEST(InletKernel, OnSagCurbWeirRegime) {
    const Geom g = refStreet();          // a = 0 ⇒ uniform cross slope form
    const Design d = curbHalfFoot3ft();
    const double depth = 0.3;            // < h ⇒ weir

    const double expect = 3.0 * d.curb_length * std::pow(depth, 1.5);  // Eq(4-30)
    EXPECT_NEAR(getOnSagInletCapture(d, g, depth), expect, 1e-12);
    EXPECT_NEAR(expect, 1.4789, 1e-3);                       // hand anchor
}

TEST(InletKernel, OnSagCurbOrificeRegime) {
    const Geom g = refStreet();
    const Design d = curbHalfFoot3ft();
    const double depth = 0.9;            // > 1.4·h = 0.7 ⇒ orifice

    const double expect = 0.67 * d.curb_height * d.curb_length
                        * std::sqrt(2.0 * 32.16 * depth);    // Eq(4-31a), vertical
    EXPECT_NEAR(getOnSagInletCapture(d, g, depth), expect, 1e-10);
    EXPECT_NEAR(expect, 7.6465, 1e-3);                       // hand anchor
}

TEST(InletKernel, OnSagCurbTransitionBandInterpolates) {
    const Geom g = refStreet();
    const Design d = curbHalfFoot3ft();
    const double dweir = d.curb_height;          // 0.5
    const double dorif = 1.4 * d.curb_height;    // 0.7
    const double depth = 0.6;                    // mid-band

    const double Qweir = 3.0 * d.curb_length * std::pow(dweir, 1.5);
    const double Qorif = 0.67 * d.curb_height * d.curb_length
                       * std::sqrt(2.0 * 32.16 * dorif);
    const double r = (depth - dweir) / (dorif - dweir);
    const double expect = (1.0 - r) * Qweir + r * Qorif;

    EXPECT_NEAR(getOnSagInletCapture(d, g, depth), expect, 1e-10);
    EXPECT_NEAR(expect, 4.9628, 1e-3);                       // hand anchor
    // Monotone across the band.
    EXPECT_LT(getOnSagInletCapture(d, g, 0.55),
              getOnSagInletCapture(d, g, 0.65));
}

TEST(InletKernel, OnSagThroatAngleChangesOrificeHead) {
    const double h = 0.5, L = 3.0, di = 0.9;
    const double vert = getCurbOrificeFlow(di, h, L,
                            static_cast<int>(ThroatAngle::VERTICAL));
    const double incl = getCurbOrificeFlow(di, h, L,
                            static_cast<int>(ThroatAngle::INCLINED));
    const double horz = getCurbOrificeFlow(di, h, L,
                            static_cast<int>(ThroatAngle::HORIZONTAL));
    // Effective head: di, di − 0.3536·h, di − 0.5·h  (inlet.c:1776-1779)
    EXPECT_NEAR(vert, 0.67 * h * L * std::sqrt(2.0 * 32.16 * di), 1e-12);
    EXPECT_NEAR(incl, 0.67 * h * L * std::sqrt(2.0 * 32.16 * (di - h / 2.0 * 0.7071)), 1e-12);
    EXPECT_NEAR(horz, 0.67 * h * L * std::sqrt(2.0 * 32.16 * (di - h / 2.0)), 1e-12);
    EXPECT_GT(vert, incl);
    EXPECT_GT(incl, horz);
}

// ============================================================================
// On-sag slotted drain — HEC-22 Eq(4-32)/(4-33)
// ============================================================================

TEST(InletKernel, OnSagSlottedWeirAndOrifice) {
    Design d;
    d.type           = static_cast<int>(InletType::SLOTTED);
    d.slotted_length = 5.0;
    d.slotted_width  = 0.15;
    const Geom g = refStreet();

    const double d_weir = 0.2;   // ≤ 2.587·w = 0.388
    const double d_orif = 0.5;   // >  2.587·w
    ASSERT_LE(d_weir, 2.587 * d.slotted_width);
    ASSERT_GT(d_orif, 2.587 * d.slotted_width);

    EXPECT_NEAR(getOnSagInletCapture(d, g, d_weir),
                2.48 * d.slotted_length * std::pow(d_weir, 1.5), 1e-12);   // Eq(4-32)
    EXPECT_NEAR(getOnSagInletCapture(d, g, d_orif),
                0.8 * d.slotted_length * d.slotted_width
                    * std::sqrt(64.32 * d_orif), 1e-12);                   // Eq(4-33)
    EXPECT_NEAR(2.48 * 5.0 * std::pow(0.2, 1.5), 1.1091, 1e-3);  // hand anchors
    EXPECT_NEAR(0.8 * 5.0 * 0.15 * std::sqrt(64.32 * 0.5), 3.4026, 1e-3);
}

// ============================================================================
// DROP_CURB — four-sided opening, always the sag routine
// ============================================================================

TEST(InletKernel, DropCurbOpeningActsOnFourSides) {
    Design drop;
    drop.type        = static_cast<int>(InletType::DROP_CURB);
    drop.curb_length = 2.0;
    drop.curb_height = 0.5;
    drop.curb_throat = static_cast<int>(ThroatAngle::VERTICAL);

    Design curb = drop;
    curb.type = static_cast<int>(InletType::CURB);

    Geom g = refStreet();
    const double depth = 0.3;    // weir regime for both

    const double q_drop = getOnSagInletCapture(drop, g, depth);
    const double q_curb = getOnSagInletCapture(curb, g, depth);
    EXPECT_NEAR(q_drop, 4.0 * q_curb, 1e-10);
    EXPECT_NEAR(q_drop, 3.0 * 8.0 * std::pow(depth, 1.5), 1e-12);
    EXPECT_NEAR(q_drop, 3.9436, 1e-3);                       // hand anchor
}

TEST(InletKernel, DropCurbOnGradeUsesSagRoutine) {
    // inlet.c:1337-1341 — a DROP_CURB always runs the on-sag kernel, capped
    // by the approach flow.
    Design drop;
    drop.type        = static_cast<int>(InletType::DROP_CURB);
    drop.curb_length = 2.0;
    drop.curb_height = 0.5;
    drop.curb_throat = static_cast<int>(ThroatAngle::VERTICAL);

    Geom g = refStreet();
    const double depth = 0.3;
    const double sag = getOnSagInletCapture(drop, g, depth);

    EXPECT_NEAR(getOnGradeInletCapture(drop, g, 100.0, depth), sag, 1e-10);
    EXPECT_NEAR(getOnGradeInletCapture(drop, g, 0.5, depth), 0.5, 1e-12);
}

// ============================================================================
// Replicate inlets and clogging
// ============================================================================

TEST(InletKernel, ReplicateInletsCaptureInSeries) {
    Geom g1 = refStreet();
    Geom g3 = refStreet();
    const Design d = grateP50_2x2();

    UsageParams one;   one.num_inlets = 1;
    UsageParams three; three.num_inlets = 3;

    const double q1 = getOnGradeCapturedFlow(d, one,   g1, 2.0, 0.0);
    const double q3 = getOnGradeCapturedFlow(d, three, g3, 2.0, 0.0);
    EXPECT_GT(q3, q1);
    EXPECT_LE(q3, 2.0);
}

TEST(InletKernel, CloggingScalesCapture) {
    Geom g0 = refStreet();
    Geom g5 = refStreet();
    const Design d = grateP50_2x2();

    UsageParams clean;  clean.clog_factor = 1.0;
    UsageParams fouled; fouled.clog_factor = 0.5;

    const double q0 = getOnGradeCapturedFlow(d, clean,  g0, 2.0, 0.0);
    const double q5 = getOnGradeCapturedFlow(d, fouled, g5, 2.0, 0.0);
    EXPECT_NEAR(q5, 0.5 * q0, 1e-10);
}

TEST(InletKernel, FlowLimitCapsCapturePerInlet) {
    Geom g = refStreet();
    const Design d = grateP50_2x2();
    UsageParams u;
    u.flow_limit = 0.25;
    EXPECT_NEAR(getOnGradeCapturedFlow(d, u, g, 2.0, 0.0), 0.25, 1e-12);
}

// ============================================================================
// CUSTOM — diversion curve
// ============================================================================

TEST(InletKernel, CustomDiversionCurve) {
    Table curve;
    curve.id   = "DIV1";
    curve.type = TableType::CURVE_DIVERSION;
    curve.x = {0.0, 1.0, 2.0, 3.0};
    curve.y = {0.0, 0.5, 1.0, 1.2};

    const Geom g = refStreet();          // nsides = 1
    UsageParams u;                       // 1 inlet, no clogging, no limit

    // Approach 2 cfs → curve gives 1.0 cfs captured (CFS units ⇒ ucf = 1).
    EXPECT_NEAR(getCustomCapturedFlow(u, g, curve,
                    static_cast<int>(CurveKind::DIVERSION),
                    2.0, 0.0, 1.0, 1.0), 1.0, 1e-12);

    // Never more than the approach flow.
    EXPECT_LE(getCustomCapturedFlow(u, g, curve,
                  static_cast<int>(CurveKind::DIVERSION),
                  0.4, 0.0, 1.0, 1.0), 0.4);
}

TEST(InletKernel, CustomRatingCurveUsesDepth) {
    Table curve;
    curve.id   = "RAT1";
    curve.type = TableType::CURVE_RATING;
    curve.x = {0.0, 0.5, 1.0};
    curve.y = {0.0, 1.0, 1.6};

    const Geom g = refStreet();
    UsageParams u;
    EXPECT_NEAR(getCustomCapturedFlow(u, g, curve,
                    static_cast<int>(CurveKind::RATING),
                    99.0, 0.5, 1.0, 1.0), 1.0, 1e-12);
}

// ============================================================================
// Open area (backflow-ratio input)
// ============================================================================

TEST(InletKernel, InletAreaByType) {
    UsageParams u;
    EXPECT_NEAR(getInletArea(grateP50_2x2(), u), 2.0 * 2.0 * 0.90, 1e-12);
    EXPECT_NEAR(getInletArea(curbHalfFoot3ft(), u), 3.0 * 0.5, 1e-12);

    Design custom;
    custom.type = static_cast<int>(InletType::CUSTOM);
    EXPECT_NEAR(getInletArea(custom, u), 0.0, 1e-12);   // count-based ratio
}

// ============================================================================
// End-to-end decks
// ============================================================================

namespace {

// ST_UP ─C_ST1─▶ ST_MID ─C_ST2─▶ OUT_ST      (street reach, 1 % grade)
//                  │ capture
//                MH1 ─C_MH─▶ OUT_MH          (sewer)
//
// `mid_is_inlet_junction` swaps the [JUNCTIONS] row + [INLET_USAGE] row for a
// single [INLET_JUNCTIONS] row on the same node — the equivalence pair of
// plan Phase E4 (i).
//
// `undersized_trunk` shrinks C_MH so the sewer cannot pass what the inlet
// offers: MH1 sits at its rim and overflows nearly everything it captures, and
// that overflow comes back up the inlet as backflow. The transfer must not
// also be booked as flooding — see InletSolver::adjustFloodingTotals.
std::string streetModel(bool mid_is_inlet_junction,
                        bool undersized_trunk = false) {
    const char* mh_depth = undersized_trunk ? "1.0" : "10.0";
    const char* trunk    = undersized_trunk ? "0.25" : "1.0";

    std::string junctions =
        "[JUNCTIONS]\n"
        ";;Name  Elev   MaxDepth\n"
        "ST_UP   100.0  1.0\n"
        "MH1      90.0  ";
    junctions += mh_depth;
    junctions += "\n";
    if (!mid_is_inlet_junction) junctions += "ST_MID   99.5  1.0\n";

    std::string mid_section = mid_is_inlet_junction
        ? "[INLET_JUNCTIONS]\n"
          ";;Name  Elev  MaxDepth  Inlet   CaptureNode  #Inlets  %Clog  Qmax  aLocal  wLocal  Placement\n"
          "ST_MID  99.5  0.5       Grate1  MH1          1        0      0     0       0       ON_GRADE\n\n"
        : "";

    std::string usage = mid_is_inlet_junction
        ? ""
        : "[INLET_USAGE]\n"
          ";;Conduit  Inlet   Node  #Inlets  %Clog  Qmax  aLocal  wLocal  Placement\n"
          "C_ST1      Grate1  MH1   1        0      0     0       0       ON_GRADE\n\n";

    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:30:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         1\n"
        "ALLOW_PONDING        NO\n"
        "\n"
        + junctions + "\n"
        + mid_section +
        "[OUTFALLS]\n"
        ";;Name  Elev   Type  Gated\n"
        "OUT_ST   99.0  FREE  NO\n"
        "OUT_MH   88.0  FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name  From    To      Length  N      Z1  Z2\n"
        "C_ST1   ST_UP   ST_MID  50.0    0.016  0   0\n"
        "C_ST2   ST_MID  OUT_ST  50.0    0.016  0   0\n"
        "C_MH    MH1     OUT_MH  50.0    0.014  0   0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link  Shape     G1        G2  G3  G4  Barrels\n"
        "C_ST1   STREET    StreetA\n"
        "C_ST2   STREET    StreetA\n"
        "C_MH    CIRCULAR  " + std::string(trunk) + "       0   0   0   1\n"
        "\n"
        "[STREETS]\n"
        ";;Name  Tcrown  Hcurb  Sx  nRoad  Hdep  Wdep  Sides\n"
        "StreetA 20.0    0.5    4   0.016  0     0     1\n"
        "\n"
        "[INLETS]\n"
        ";;Name  Type   Length  Width  GrateType\n"
        "Grate1  GRATE  2.0     2.0    P_BAR-50\n"
        "\n"
        + usage +
        "[DWF]\n"
        ";;Node  Param  Value\n"
        "ST_UP   FLOW   2.0\n"
        "\n"
        "[COORDINATES]\n"
        ";;Node  X      Y\n"
        "ST_UP     0.0   0.0\n"
        "ST_MID   50.0   0.0\n"
        "OUT_ST  100.0   0.0\n"
        "MH1      50.0 -50.0\n"
        "OUT_MH  100.0 -50.0\n";
}

struct CaptureRun {
    bool   ran           = false;
    double peak_capture  = 0.0;   ///< peak lateral inflow at MH1 (cfs)
    double peak_approach = 0.0;   ///< peak flow in the approach conduit (cfs)
    double routing_error = 0.0;   ///< routing continuity error (%)
};

CaptureRun runStreetModel(const std::string& base, const std::string& text) {
    CaptureRun r;
    const std::string inp = outPath(base + ".inp");
    writeFile(inp, text);

    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, inp.c_str(),
                                    outPath(base + ".rpt").c_str(),
                                    outPath(base + ".out").c_str(), nullptr);
    if (rc != 0) {
        ADD_FAILURE() << "open failed for " << base << ": "
                      << swmm_get_last_error_msg(e);
        swmm_engine_destroy(e);
        return r;
    }

    EXPECT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_start(e, 1), 0) << swmm_get_last_error_msg(e);

    const int mh1  = swmm_node_index(e, "MH1");
    const int cst1 = swmm_link_index(e, "C_ST1");
    EXPECT_GE(mh1, 0);
    EXPECT_GE(cst1, 0);
    if (mh1 < 0 || cst1 < 0) { swmm_engine_close(e); swmm_engine_destroy(e); return r; }

    double elapsed = 0.0;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            ADD_FAILURE() << "step failed: " << swmm_get_last_error_msg(e);
            break;
        }
        double lat = 0.0, q = 0.0;
        swmm_node_get_lateral_inflow(e, mh1, &lat);
        swmm_link_get_flow(e, cst1, &q);
        r.peak_capture  = std::max(r.peak_capture, lat);
        r.peak_approach = std::max(r.peak_approach, std::fabs(q));
    } while (elapsed > 0.0);

    swmm_engine_end(e);
    swmm_get_routing_continuity_error(e, &r.routing_error);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    r.ran = true;
    return r;
}

} // namespace

TEST(InletCaptureDeck, ConduitAttributeInletCapturesAndCloses) {
    const CaptureRun r = runStreetModel("street_usage", streetModel(false));
    ASSERT_TRUE(r.ran);
    EXPECT_GT(r.peak_capture, 0.0) << "no flow reached the capture node";
    EXPECT_LE(r.peak_capture, r.peak_approach + 1e-6)
        << "captured more than the approach flow";
    // routing_error() is a FRACTION, so this is half a percent — the previous
    // 0.5 here read as "< 50 %" and would not have caught a capture node
    // bleeding mass (see SurchargedCaptureNodeBackflowClosesContinuity).
    EXPECT_LT(std::fabs(r.routing_error), 0.005)
        << "routing continuity error " << r.routing_error * 100.0 << " %";
}

TEST(InletCaptureDeck, InletJunctionMatchesConduitAttributeCapture) {
    const CaptureRun usage = runStreetModel("street_usage",  streetModel(false));
    const CaptureRun ijunc = runStreetModel("street_ijunct", streetModel(true));
    ASSERT_TRUE(usage.ran);
    ASSERT_TRUE(ijunc.ran);

    EXPECT_GT(ijunc.peak_capture, 0.0);
    EXPECT_LE(ijunc.peak_capture, ijunc.peak_approach + 1e-6);
    EXPECT_LT(std::fabs(ijunc.routing_error), 0.005)   // fraction: half a percent
        << "routing continuity error " << ijunc.routing_error * 100.0 << " %";

    // Same reach, same inlet, same capture node — the materialised node must
    // reproduce the conduit-attribute capture (plan Phase E4 (i)).
    ASSERT_GT(usage.peak_capture, 0.0);
    const double rel = std::fabs(ijunc.peak_capture - usage.peak_capture)
                     / usage.peak_capture;
    EXPECT_LT(rel, 0.05) << "peak capture " << ijunc.peak_capture
                         << " vs " << usage.peak_capture;
}

// A capture node that cannot pass what the inlet offers overflows, and that
// overflow is handed back to the street as the inlet's backflow. Booking it as
// flooding as well counts the same water twice, and because the returned water
// is immediately re-captured the two feed each other: the error compounds
// instead of staying bounded. Measured -44 % on the parity set's
// street_grate_inlet_7_backflow deck and -255 % on a four-inlet street over a
// 1 ft trunk before InletSolver::adjustFloodingTotals existed.
TEST(InletCaptureDeck, SurchargedCaptureNodeBackflowClosesContinuity) {
    for (const bool as_junction : {false, true}) {
        const CaptureRun r = runStreetModel(
            as_junction ? "street_ijunct_surch" : "street_usage_surch",
            streetModel(as_junction, /*undersized_trunk=*/true));
        ASSERT_TRUE(r.ran) << "as_junction=" << as_junction;
        EXPECT_GT(r.peak_capture, 0.0)
            << "no flow reached the capture node, as_junction=" << as_junction;
        // routing_error() is a FRACTION (the report multiplies by 100), so
        // this is one percent, not one hundred.
        EXPECT_LT(std::fabs(r.routing_error), 0.01)
            << "routing continuity error " << r.routing_error * 100.0
            << " % with a surcharged capture node, as_junction=" << as_junction;
    }
}
