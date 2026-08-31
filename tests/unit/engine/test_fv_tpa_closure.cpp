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
#include <cstdlib>

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
#include <openswmm/engine/openswmm_output.h>

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
// System level — over-the-top drawdown (the E3 analog, TPA plan A-G4)
// ===========================================================================
//
// Two 100 sf tanks joined by an up-and-over 1-ft pipe (apex invert 6 ft);
// the apex interior is a JUNCTION so the fixture avoids the FV VJ
// initial-state gap (P1 finding). Everything starts FULL at head 8 (no
// self-priming — a priming siphon needs two-phase air transport, the
// paper's own out-of-scope case; the first fixture attempt proved it).
// A constant withdrawal drains TB. Once the shared head falls below the
// apex crown (7 ft):
//   - sealed apex (SUR_DEPTH 30) + TPA: the column holds at sub-atmospheric
//     apex pressure and BOTH tanks keep draining together;
//   - sealed apex + SLOT: the memoryless closure reverts the apex to free
//     surface — the column breaks and TA strands near the apex level;
//   - OPEN apex (SUR_DEPTH 0): air enters at the apex, TPA's venting rule
//     clears the flag, and TPA must behave like SLOT.

namespace {

const char* kOutDir = "fv_tpa_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

std::string overTopModel(const std::string& closure, bool apex_sealed,
                         double init_head = 8.0, double withdraw_cfs = 0.5) {
    std::ostringstream ss;
    const double js_init = init_head > 6.0 ? init_head - 6.0 : 0.0;
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
          "TA     0.0  20.0     " << init_head << "  FUNCTIONAL 0  0  100\n"
          "TB     0.0  20.0     " << init_head << "  FUNCTIONAL 0  0  100\n"
       << "\n[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth SurDepth\n"
          "JS     6.0  2.0      " << js_init << "     "
       << (apex_sealed ? "30" : "0") << "\n"
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

struct DrawdownResult {
    double final_ta = -1.0;     ///< final TA depth (ft)
    double final_tb = -1.0;     ///< final TB depth (ft)
    /// Minimum piezometric head at JS (ft, absolute elevation), read with
    /// swmm_node_get_head. NOT derived from the node DEPTH: a depth is a
    /// water depth floored at zero, so `invert + depth` can never express a
    /// sub-atmospheric column and this gate would be unfalsifiable.
    double min_apex_head = 1e9;
    double cont_err = 1e9;      ///< routing continuity error (%)
    bool ok = false;
};

DrawdownResult runDrawdown(const std::string& base, const std::string& deck) {
    DrawdownResult r;
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
    EXPECT_GE(js, 0);
    double elapsed = 0.0;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            ADD_FAILURE() << "step failed: " << swmm_get_last_error_msg(e);
            break;
        }
        double hh = 0.0;
        swmm_node_get_head(e, js, &hh);
        r.min_apex_head = std::min(r.min_apex_head, hh);
    } while (elapsed > 0.0);
    swmm_node_get_depth(e, ta, &r.final_ta);
    swmm_node_get_depth(e, tb, &r.final_tb);
    swmm_get_routing_continuity_error(e, &r.cont_err);
    swmm_engine_end(e);
    swmm_engine_destroy(e);
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
    const auto a = runDrawdown("tpa_absent", overTopModel("", true));
    const auto b = runDrawdown("tpa_slot",
                               overTopModel("FV_PRESSURE_CLOSURE  SLOT\n",
                                            true));
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_TRUE(fileBytes(outPath("tpa_absent.out")) ==
                fileBytes(outPath("tpa_slot.out")));
}

TEST(FvTpa, FreeSurfaceDeckIsBitIdentical) {
    // Shallow puddles only (init head 0.4 ft < the 1-ft crown, no
    // withdrawal): no cell can ever reach a_crown and flag, so TPA must be
    // bit-identical to the base run.
    const auto a = runDrawdown("tpa_fs_base",
                               overTopModel("", true, 0.4, 0.0));
    const auto b = runDrawdown("tpa_fs_on",
                               overTopModel(kTpa, true, 0.4, 0.0));
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_TRUE(fileBytes(outPath("tpa_fs_base.out")) ==
                fileBytes(outPath("tpa_fs_on.out")));
}

TEST(FvTpa, SealedApexSustainsSubAtmosphericColumn) {
    // THE discriminator (A-G4 analog / paper E3): after the shared head
    // falls below the apex crown, TPA keeps both tanks draining together
    // through a sub-atmospheric apex; SLOT breaks the column and strands TA.
    const auto slot = runDrawdown("tpa_dd_slot", overTopModel("", true));
    const auto tpa  = runDrawdown("tpa_dd_tpa", overTopModel(kTpa, true));
    ASSERT_TRUE(slot.ok && tpa.ok);
    // The discriminator is the PRESSURE SIGNAL, exactly what the paper
    // measures in E3. (Gross drainage cannot discriminate here: the table
    // closure keeps ~95% area just below the crown, so both closures keep
    // conveying, and the two runs end within 0.1 ft of each other — measured.)
    //
    // The two closures have DIFFERENT floors, and that is the contrast:
    //   * a free-surface closure cannot put a node below the lowest bed it
    //     touches — with the apex dry it sits on that bed (~5.55 ft here) and
    //     stops. It has no way to represent tension.
    //   * TPA holds a FULL pipe at negative gauge pressure, so the apex head
    //     keeps falling past the bed — measured 3.51 ft, i.e. 2.5 ft of
    //     vacuum below the 6.0 ft apex invert.
    // Hence both a hard threshold and a separation: the hard one alone passed
    // against a build where the node merely sat on its bed.
    EXPECT_LT(tpa.min_apex_head, 5.8)
        << "TPA apex never dropped below the invert (no true vacuum): "
        << tpa.min_apex_head;
    EXPECT_LT(tpa.min_apex_head, slot.min_apex_head - 1.0)
        << "the closures did not separate: TPA " << tpa.min_apex_head
        << " vs SLOT " << slot.min_apex_head;
    EXPECT_GT(slot.min_apex_head, 5.4)
        << "SLOT apex went below its own lowest bed — a free-surface closure "
           "cannot carry tension: " << slot.min_apex_head;
    // And the column stays coupled under TPA: both tanks drain together.
    EXPECT_LT(std::fabs(tpa.final_ta - tpa.final_tb), 1.5)
        << "TPA tanks decoupled: TA " << tpa.final_ta
        << " TB " << tpa.final_tb;
}

TEST(FvTpa, OpenApexVentsAndBreaksTheColumn) {
    // The venting rule (A2): with SUR_DEPTH 0 at the apex, air enters when
    // the apex head reaches the crown; the flag must clear and TPA must
    // strand TA like the slot closure does.
    const auto tpa_open  = runDrawdown("tpa_open", overTopModel(kTpa, false));
    ASSERT_TRUE(tpa_open.ok);
    // With air available at the apex the flag must clear at crown contact:
    // no vacuum can form — the apex head keeps the ordinary invert floor.
    // Measured 6.17 ft: the vented apex never goes below its own invert.
    // This is also the regression gate for the two export defects the
    // sub-atmospheric gate uncovered — with either of them back, the same
    // OPEN apex reports -95.06 ft (a flagged cell's piezometric level read as
    // ground) or 5.50 ft (the node dropped onto its bed).
    EXPECT_GE(tpa_open.min_apex_head, 5.99)
        << "vented TPA still built vacuum: " << tpa_open.min_apex_head;
}

TEST(FvTpa, ImplicitPressurizedPathBuildsTheSameVacuum) {
    // A3 site: PressurizedHeadSolver::cellPressurized IS the TPA flag under
    // TPA (not the crown test), so a latched sub-atmospheric cell is inside
    // the implicit acoustic set. What that buys is verified here: the sealed
    // apex still goes sub-atmospheric and the run still conserves mass.
    // What it does NOT buy is pinned as a known issue directly below.
    const auto imp = runDrawdown(
        "tpa_dd_tpa_implicit",
        overTopModel("FV_PRESSURE_CLOSURE  TPA\nFV_PRESSURIZED_IMPLICIT YES\n",
                     true));
    ASSERT_TRUE(imp.ok);
    EXPECT_LT(imp.min_apex_head, 5.8)
        << "implicit x TPA apex carried no vacuum: " << imp.min_apex_head;
    EXPECT_LT(std::fabs(imp.cont_err), 1.0)
        << "implicit x TPA continuity error " << imp.cont_err << "%";
}

TEST(FvTpa, SealedDrawdownMassConserved) {
    // The flag flips regime, not mass (C API — harness runs skip the report
    // writer).
    const auto tpa = runDrawdown("tpa_mass", overTopModel(kTpa, true));
    ASSERT_TRUE(tpa.ok);
    EXPECT_LT(std::fabs(tpa.cont_err), 2.0)
        << "routing continuity error % = " << tpa.cont_err;
}

// ===========================================================================
// Phase 5 additions (issue #156): implicit×TPA fix + signed-heads output
// ===========================================================================

TEST(FvTpa, SealedApexHoldsUnderImplicit) {
    // The P4 known-issue, fixed in P5: FV_PRESSURIZED_IMPLICIT folded a
    // sealed junction to a Dirichlet row AT THE INVERT and broke the column
    // (measured |TA−TB| 3.743 vs 0.003 explicit). With the floor extended by
    // the column-separation bound, the implicit path must now match the
    // explicit physics.
    const auto imp = runDrawdown(
        "tpa_dd_imp",
        overTopModel("FV_PRESSURE_CLOSURE  TPA\n"
                     "FV_PRESSURIZED_IMPLICIT YES\n", true));
    ASSERT_TRUE(imp.ok);
    EXPECT_LT(imp.min_apex_head, 5.8)
        << "implicit apex never went below the invert";
    EXPECT_LT(std::fabs(imp.final_ta - imp.final_tb), 1.5)
        << "implicit column decoupled: TA " << imp.final_ta
        << " TB " << imp.final_tb;
    EXPECT_LT(std::fabs(imp.cont_err), 2.0);
}

TEST(FvTpa, SealedSlopedColumnSurvivesAcousticCelerity) {
    // P5 study finding (E3): every earlier TPA fixture ran a handful of cells
    // at the default 100 ft/s slot celerity, and the LOCAL-time-stepping
    // bounds (cellStableDt / nodeStableDt / algebraicNodeStableDt) still
    // derived a FLAGGED cell's speed from the free-surface table width — a
    // sealed sub-crown cell landed in a coarse tier its own acoustic exchange
    // then detonated (E3's at-rest pressurized sloped column NaN'd inside the
    // first report step). The global census had the TPA arm; the tiers were
    // the gap. This deck reproduces the regime at unit scale: fine cells,
    // a = 984 ft/s (300 m/s, the E3 paper value), sealed apex, fast drawdown.
    // NOTE (P5 verification, measured): this deck does NOT exercise the flag
    // save/restore across step rejection — n.rollback = 0 over its 236,627
    // substeps (OPENSWMM_PERF), as on every healthy TPA configuration probed.
    // Rollbacks were observed only inside the pre-existing high-celerity
    // filling divergence (see KnownIssueHighCelerityFillingDiverges), so a
    // meaningful rollback gate awaits that fix (P5 falsifier (b)).
    std::string deck = overTopModel(
        "FV_PRESSURE_CLOSURE  TPA\n"
        "FV_SLOT_CELERITY     984\n"
        "FV_CELL_LENGTH       0.5\n",
        true, 7.2, 2.0);
    const auto pos = deck.find("END_TIME             00:30:00");
    ASSERT_NE(pos, std::string::npos);
    deck.replace(pos, std::string("END_TIME             00:30:00").size(),
                 "END_TIME             00:02:00");
    const auto r = runDrawdown("tpa_sloped_acoustic", deck);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(std::isfinite(r.min_apex_head));
    EXPECT_LT(r.min_apex_head, 6.95)
        << "sealed sloped column never went sub-crown: " << r.min_apex_head;
    EXPECT_GT(r.min_apex_head, -30.0)
        << "apex head fell past the column-separation bound — slot-line "
           "garbage: " << r.min_apex_head;
    EXPECT_LT(std::fabs(r.cont_err), 2.0)
        << "routing continuity error % = " << r.cont_err;
}

TEST(FvTpa, SealedSlopedColumnHoldsUnderImplicitAcoustic) {
    // P5 task-3 (the matrix.yaml open item): implicit × TPA diverged at t=0
    // on E3's sloped pressurized start. The defect was the implicit solve's
    // per-cell storage coefficient: it took widthOfDepth(g, h) for a FLAGGED
    // cell, which is 0 (clamped to 1e-12) once the piezometric depth goes
    // negative and the free-surface width (~1e5 × t_slot at study
    // celerities) below the crown — so the linearized head update swung five
    // orders of magnitude as the settle transient crossed the crown, the
    // apex cell's head left the physical range within four substeps, and the
    // run NaN'd (measured: cell 199 h → 877208 ft by substep 50). A flagged
    // cell's storage derivative is the constant t_slot at ANY h. Same deck
    // as the explicit acoustic gate, implicit path on.
    std::string deck = overTopModel(
        "FV_PRESSURE_CLOSURE  TPA\n"
        "FV_SLOT_CELERITY     984\n"
        "FV_CELL_LENGTH       0.5\n"
        "FV_PRESSURIZED_IMPLICIT YES\n",
        true, 7.2, 2.0);
    const auto pos = deck.find("END_TIME             00:30:00");
    ASSERT_NE(pos, std::string::npos);
    deck.replace(pos, std::string("END_TIME             00:30:00").size(),
                 "END_TIME             00:02:00");
    const auto r = runDrawdown("tpa_sloped_acoustic_imp", deck);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(std::isfinite(r.min_apex_head));
    EXPECT_GT(r.min_apex_head, -30.0)
        << "apex head fell past the column-separation bound — the implicit "
           "storage width regressed: " << r.min_apex_head;
    EXPECT_LT(std::fabs(r.cont_err), 2.0)
        << "routing continuity error % = " << r.cont_err;
}

TEST(FvTpa, SignedHeadsReachTheOutFile) {
    // O-6: with REPORT_SIGNED_HEADS YES the .out HEAD field must carry the
    // sub-invert vacuum; without it the field stays floored at the invert.
    const auto on = runDrawdown(
        "tpa_dd_signed",
        overTopModel("FV_PRESSURE_CLOSURE  TPA\n"
                     "REPORT_SIGNED_HEADS  YES\n", true));
    ASSERT_TRUE(on.ok);
    // Minimum of the APEX node's HEAD series specifically — a global
    // minimum would trivially pick the outfall at elevation 0.
    auto min_out_head = [](const std::string& path) {
        SWMM_Output out = swmm_output_open(path.c_str());
        EXPECT_NE(out, nullptr);
        if (!out) return 1e9;
        const int periods = swmm_output_get_period_count(out);
        const int nn = swmm_output_get_node_count(out);
        double best = 1e9;
        std::vector<float> series(static_cast<std::size_t>(periods), 0.0f);
        for (int n = 0; n < nn; ++n) {
            const char* id = swmm_output_get_node_id(out, n);
            if (!id || std::string(id) != "JS") continue;
            // Returns 0 on success (not a count).
            if (swmm_output_get_node_series(out, n, SWMM_OUT_NODE_HEAD, 0,
                                            periods - 1, series.data()) != 0)
                continue;
            for (int i = 0; i < periods; ++i)
                best = std::min(best,
                                static_cast<double>(
                                    series[static_cast<std::size_t>(i)]));
        }
        swmm_output_close(out);
        return best;
    };
    EXPECT_LT(min_out_head(outPath("tpa_dd_signed.out")), 5.8)
        << "signed vacuum did not reach the .out HEAD field";
    // The unsigned control: the earlier sealed run (no option) must stay
    // floored at the apex invert.
    EXPECT_GE(min_out_head(outPath("tpa_dd_tpa.out")), 5.99)
        << "default-path .out heads went sub-invert — parity broken";
}

// ===========================================================================
// Filling-surge fixtures (P5b) — the e2 mini-analog: fill box, one 8 m
// near-full pipe (200 cells), small surge tank. The filling bore pressurizes
// cells front-to-back, hits the tank, and the reflected surge walks cells
// back and forth across the crown.
// ===========================================================================

namespace {

std::string fillingSurgeDeck(const std::string& celerity) {
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           LPS\n"
        "FLOW_ROUTING         FV\n"
        "LINK_OFFSETS         DEPTH\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:02:00\n"
        "REPORT_STEP          1\n"
        "ROUTING_STEP         0.01\n"
        "MIN_SURFAREA         0.05\n"
        "INERTIAL_DAMPING     NONE\n"
        "NORMAL_FLOW_LIMITED  NEITHER\n"
        "FV_CELL_LENGTH       0.04\n"
        "FV_MIN_CELLS         4\n"
        "FV_SLOT_CELERITY     " + celerity + "\n"
        "FV_PRESSURE_CLOSURE  TPA\n"
        "FV_PRESSURIZED_IMPLICIT NO\n"
        "\n[STORAGE]\n"
        ";;Name  Elev  MaxDepth InitDepth Shape      A1 A2 A0\n"
        "FB      0.0   0.40     0.073     FUNCTIONAL 0  0  0.03\n"
        "ST      0.0   1.50     0.073     FUNCTIONAL 0  0  0.005\n"
        "\n[OUTFALLS]\n;;Name Elev Type Gated\nDN  0.0  FREE NO\n"
        "\n[WEIRS]\n;;Name From To Type CrestHt Cd Gated EC\n"
        "OVF     ST    DN TRANSVERSE 1.50 1.84 NO 0\n"
        "\n[CONDUITS]\n;;Name From To Length N Zin Zout Q0\n"
        "P1      FB    ST 8.00   0.012 0 0 0\n"
        "\n[XSECTIONS]\n;;Link Shape Geom1 Geom2 Geom3 Geom4 Barrels\n"
        "P1      CIRCULAR 0.094 0 0 0 1\n"
        "OVF     RECT_OPEN 0.10 0.19 0 0\n"
        "\n[INFLOWS]\n;;Node Constituent Tseries Type Mfactor Sfactor Baseline\n"
        "FB      FLOW        \"\"      FLOW 1.0     1.0     3.1\n";
}

struct FillingSurgeResult {
    bool completed = false;   ///< every step succeeded to END_TIME
    double cont = 1e9;        ///< routing continuity error (%)
    double max_st = -1e9;     ///< max surge-tank depth seen (m)
};

FillingSurgeResult runFillingSurge(const std::string& base,
                                   const std::string& deck) {
    FillingSurgeResult r;
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
    const int st = swmm_node_index(e, "ST");
    EXPECT_GE(st, 0);
    double elapsed = 0.0;
    r.completed = true;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            r.completed = false;   // divergence: ERROR 14 class
            break;
        }
        double d = 0.0;
        swmm_node_get_depth(e, st, &d);
        r.max_st = std::max(r.max_st, d);
    } while (elapsed > 0.0);
    swmm_get_routing_continuity_error(e, &r.cont);
    swmm_engine_end(e);
    swmm_engine_destroy(e);
    return r;
}

} // namespace

TEST(FvTpa, FillingSurgeSurvivesRegimeTransitions) {
    // P5b falsifier gate (issue #156): at the paper's own a = 25 m/s the
    // fixture must run clean — but ONLY because of the regime-consistent
    // state refresh at flag transitions (updateTpaFlags::refresh — a
    // transitioned cell otherwise carries its OLD regime's h through the
    // substep's reconstruction, exited cells at −200 ft class line-depths).
    // Without the refresh this deck NaNs at the reflected surge (measured:
    // ERROR 14 at 0.0186 h on the refresh-less build) exactly like
    // e2_2006:C3 at t≈21.6 s. With it: completion at 0.000% continuity.
    const auto r = runFillingSurge("tpa_filling_surge", fillingSurgeDeck("25"));
    EXPECT_TRUE(r.completed) << "filling surge diverged at a = 25 m/s";
    if (r.completed) {
        EXPECT_LT(std::fabs(r.cont), 2.0)
            << "routing continuity error % = " << r.cont;
        EXPECT_GT(r.max_st, 0.094)
            << "surge tank never rose above the pipe crown — the bore never "
               "arrived and the fixture is not exercising the surge";
    }
}

TEST(FvTpa, KnownIssueHighCelerityFillingDiverges) {
    // P5b PIN (issue #156, owner follow-up — e2_2025 × C3/C5): explicit FV
    // TPA still diverges on filling/reflection decks at high acoustic
    // celerity. Same fixture as above at a = 150 m/s (e2_2025's celerity):
    // ERROR 14 within the first seconds. On e2_2025 itself the mechanism is
    // a TEMPORAL odd–even pressure/vacuum oscillation inside the flagged
    // region (adjacent-substep hmin flipping 0 ↔ −7500 ft, heads to
    // 34,000 ft, cells ~124–150 around the C2/C3 dx transition
    // 0.31 → 0.19 ft) at a correctly acoustic-bounded dt (0.0002 s, verified
    // via OPENSWMM_FV_DT_TRACE). P5b verification MEASURED the TPA plan §7
    // filter contingency — a conservative, flagged-neighborhood-local
    // 3-point [0.05, 0.90, 0.05] smoothing of (A, Q) via pairwise
    // dx-weighted face exchanges, once per accepted substep — and it does
    // NOT rescue e2_2025 (diverges at ~0.0057 h with the filter ON at
    // kW = 0.05 and at kW = 0.25; OFF diverges at 0.0058 h): spatial
    // smoothing cannot damp a temporal mode. The remaining §7 path is the
    // Vasconcelos & Wright (2009) hybrid flux. Until that lands this pin
    // documents the limitation; when it flips, retire this test, unpin
    // e2_2025 in studies/mixed_flow_closures/config/matrix.yaml, and drop
    // the deck-level expectation there.
    const auto r =
        runFillingSurge("tpa_filling_surge_c150", fillingSurgeDeck("150"));
    EXPECT_FALSE(r.completed)
        << "HIGH-CELERITY FILLING NOW COMPLETES (cont err % = " << r.cont
        << ") — the known issue is fixed: retire this pin and unpin "
           "e2_2025:C3/C5 in the study matrix";
}
