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
 * @file test_reaction_ard_binding.cpp
 * @brief E4/R6 gates: reaction binding for the Eulerian ARD engine + MSX
 *        species transport on the mesh
 *        (plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md §6 E4;
 *        MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R6).
 *
 * @details Observation paths, one per claimed defense (coverage-geometry
 *          discipline):
 *          - ArdKdecayIsExactExponential…: stagnant wet chain, uniform
 *            Cinit; a uniform field is transport-invariant, so the
 *            published conc isolates the decay law. k·t = 3.6 puts the
 *            exponential 35% above the legacy linearized product (the R4
 *            validator's separation), and the E1 "not yet applied" warning
 *            must be gone.
 *          - MsxDilutionFrontProvesTransport: THE R6 razor. Inflow water
 *            carries zero MSX (sources are E5), so a sustained clean
 *            inflow dilutes the head store and the dilution front advects
 *            down the chain: the LAST link's MSX conc must fall below 90%
 *            of its initial value by end of run. An element-local
 *            implementation (the R4b limitation) holds the initial value
 *            forever and fails here; the max principle bounds everything
 *            above by the initial value.
 *          - MsxScopedKineticsUnderArd: pipe RATE k1 vs tank RATE k2 with
 *            k2 = 4·k1 on a stagnant chain — links must track exp(−k1·t),
 *            node stores exp(−k2·t); swapping the tank flag in the stage
 *            swaps the two and fails both legs.
 *          - MsxFormulaReadsPollutantPerCell: Z = 3·TSS with TSS decaying
 *            and advecting; publish is a volume-weighted mean and the
 *            formula is linear, so 3·links.conc(TSS) must match
 *            msx_link_conc(Z) tightly at every recorded step — falsified
 *            by dropping the per-cell pollutant gather.
 *          - MsxPresenceLeavesPollutantsBitwise: the STRIDE razor. Adding
 *            two inert MSX species (ns_total = 3) must leave every TSS
 *            trajectory bit-identical to the component-free run — any
 *            np/ns stride slip (loads, init, donor, publish) fails here
 *            first.
 *          - MsxOnlyModelRunsUnderArd: no [POLLUTANTS] at all (np = 0,
 *            pollutant pointer null end to end).
 *          - WallSpeciesFallsBackToLegacy: precise warning + LEGACY
 *            fallback (no transport semantics for attached species).
 *          - FailureContainmentNamesArdElement: an explosive RATE under
 *            EUL must fail LOUDLY (warning names the ARD element and the
 *            integrator's remedy) and non-fatally, with finite state.
 *          - InertMsxRowMatchesInertPollutantRow (gate 9, added in
 *            validation): an inert MSX row and an identically seeded inert
 *            pollutant row must trace the SAME numbers. Gates 1-8 could not
 *            observe a defect confined to an MSX row — gate 5 compares
 *            pollutant rows to each other, gate 2's front is a one-sided
 *            inequality — and one was there.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kC0     = 10.0;  ///< TSS Cinit
constexpr double kMsx0   = 8.0;   ///< MSX GLOBAL initial value
constexpr double kKdecay = 0.03;  ///< 1/s — k·t = 3.6 over the 2-min horizon

/// The E3 chain deck (validator-fixed WET junctions — initQuality seeds
/// Cinit only into wet elements; roadmap lesson 9), with knobs this suite
/// needs: kdecay, [POLLUTANTS] on/off (MSX-only models), and `stagnant`.
///
/// `stagnant` is not merely "no inflow": a sloped chain DRAINS, and a
/// draining system contaminates the kinetics gates — node stores receive
/// PIPE-decayed inflow from upstream cells, so a store no longer tracks its
/// own tank exponential. The stagnant variant is a LEVEL POOL in exact
/// hydrostatic equilibrium (flat inverts, FIXED outfall stage at the water
/// surface): zero flow, zero exchange, every element isolated — the
/// published concentration isolates the local reaction law.
void write_deck(const char* path, const std::string& pc_lines,
                const std::string& extra_options = "", double kdecay = 0.0,
                bool stagnant = false, bool pollutants = true,
                const char* end_time = "01:00:00", double cinit = kC0) {
    std::ofstream f(path);
    f << "[TITLE]\nE4/R6 ARD reaction binding gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "QUALITY_SOLVER EULERIAN_ARD\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << end_time << "\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:01:00\n"
      << extra_options << "\n";
    if (stagnant) {
        f << "[JUNCTIONS]\n"               // level pool: flat, equilibrium
          << "J0 10.0 10 1.5 0 0\nJ1 10.0 10 1.5 0 0\n"
          << "J2 10.0 10 1.5 0 0\nJ3 10.0 10 1.5 0 0\n"
          << "J4 10.0 10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 10.0 FIXED 11.5 NO\n\n";
    } else {
        f << "[JUNCTIONS]\n"               // Name Elev MaxDepth InitDepth …
          << "J0 10.0 10 1.5 0 0\n"
          << "J1 9.4  10 1.5 0 0\n"
          << "J2 8.8  10 1.5 0 0\n"
          << "J3 8.2  10 1.5 0 0\n"
          << "J4 7.6  10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n";
    }
    f << "[CONDUITS]\n"
      << "C1 J0 J1 500 0.013 0 0 0\n"
      << "C2 J1 J2 500 0.013 0 0 0\n"
      << "C3 J2 J3 500 0.013 0 0 0\n"
      << "C4 J3 J4 500 0.013 0 0 0\n"
      << "C5 J4 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
      << "C3 CIRCULAR 2.0 0 0 0\nC4 CIRCULAR 2.0 0 0 0\n"
      << "C5 CIRCULAR 2.0 0 0 0\n\n";
    if (!stagnant)
        f << "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 5\n\n";
    if (pollutants)
        f << "[POLLUTANTS]\n"
          << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac "
             "Cdwf Cinit\n"
          << "TSS    MG/L  0     0   0     " << (kdecay * 86400.0)  // 1/day column (KD1)
          << "      NO       *        0      0    " << cinit << "\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

void write_rxn(const char* path, const std::string& body) {
    std::ofstream c(path);
    c << body;
}

std::string pc_line(const char* rxn) {
    return std::string("org.hydrocouple.openswmm.reactions config=\"") + rxn +
           "\"";
}

/// Per-step trajectories over a full run: pollutant link conc (np-strided),
/// MSX link/node conc (nm-strided R4 arrays). ASSERT lifecycle failures at
/// the call site via the returned flag (R4 EXPECT-on-open lesson).
struct RunRecord {
    std::vector<std::vector<double>> tss_link;   ///< [link][step]
    std::vector<std::vector<double>> msx_link;   ///< [link*nm + m][step]
    std::vector<std::vector<double>> msx_node;   ///< [node*nm + m][step]
    std::vector<std::string> warnings;
    bool ok = false;
};

RunRecord run_recording(const char* inp, const char* rpt, const char* out,
                        int nm) {
    RunRecord rec;
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) { ADD_FAILURE() << "engine create"; return rec; }
    bool ok = swmm_engine_open(e, inp, rpt, out, nullptr) == SWMM_OK;
    if (!ok) ADD_FAILURE() << "open failed for " << inp;
    if (ok && (swmm_engine_initialize(e) != SWMM_OK ||
               swmm_engine_start(e, 1) != SWMM_OK)) {
        ADD_FAILURE() << "init/start failed for " << inp;
        ok = false;
    }
    if (ok) {
        auto& ctx = as_cpp_engine(e).context();
        const int nl = ctx.n_links();
        const int nn = ctx.n_nodes();
        const int np = ctx.n_pollutants();
        rec.tss_link.assign(static_cast<std::size_t>(nl), {});
        rec.msx_link.assign(static_cast<std::size_t>(nl * std::max(nm, 1)), {});
        rec.msx_node.assign(static_cast<std::size_t>(nn * std::max(nm, 1)), {});
        double elapsed = 0.0;
        int guard = 0;
        do {
            if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
                ADD_FAILURE() << "step failed for " << inp;
                ok = false;
                break;
            }
            for (int l = 0; l < nl; ++l) {
                if (np > 0)
                    rec.tss_link[static_cast<std::size_t>(l)].push_back(
                        ctx.links.conc[static_cast<std::size_t>(l * np)]);
                for (int m = 0; m < nm; ++m) {
                    const auto idx =
                        static_cast<std::size_t>(l) * static_cast<std::size_t>(nm) +
                        static_cast<std::size_t>(m);
                    rec.msx_link[static_cast<std::size_t>(l * nm + m)].push_back(
                        (idx < ctx.reactions.msx_link_conc.size())
                            ? ctx.reactions.msx_link_conc[idx]
                            : -1.0);
                }
            }
            for (int n2 = 0; n2 < nn; ++n2)
                for (int m = 0; m < nm; ++m) {
                    const auto idx =
                        static_cast<std::size_t>(n2) * static_cast<std::size_t>(nm) +
                        static_cast<std::size_t>(m);
                    rec.msx_node[static_cast<std::size_t>(n2 * nm + m)].push_back(
                        (idx < ctx.reactions.msx_node_conc.size())
                            ? ctx.reactions.msx_node_conc[idx]
                            : -1.0);
                }
        } while (elapsed > 0.0 && ++guard < 20000);
        if (ok) swmm_engine_end(e);
        rec.warnings = ctx.warnings;
    }
    swmm_engine_destroy(e);
    rec.ok = ok;
    return rec;
}

bool warned(const std::vector<std::string>& ws, const std::string& needle) {
    for (const auto& w : ws)
        if (w.find(needle) != std::string::npos) return true;
    return false;
}

// Link declaration order C1..C5 = 0..4; junctions J0..J4 = 0..4.
constexpr int kC3 = 2, kC5 = 4, kJ1 = 1;

// ---------------------------------------------------------------------------
// Gate 1 — kdecay under ARD is the exact exponential; the E1 warning is gone.
// ---------------------------------------------------------------------------
TEST(ReactionArdBindingTest, ArdKdecayIsExactExponentialAndWarningRetired) {
    // Level pool: zero flow, so the published concentration isolates the
    // decay law. 2-minute horizon.
    write_deck("_e4_kd.inp", "", "", kKdecay, /*stagnant=*/true,
               /*pollutants=*/true, "00:02:00");
    const auto rec = run_recording("_e4_kd.inp", "_e4_kd.rpt", "_e4_kd.out", 0);
    ASSERT_TRUE(rec.ok);
    ASSERT_FALSE(rec.tss_link[kC3].empty());

    const double t      = 120.0;
    const double exact  = kC0 * std::exp(-kKdecay * t);
    const double linear = kC0 * std::pow(1.0 - kKdecay * 5.0, 24);  // 35% below
    const double c      = rec.tss_link[kC3].back();

    EXPECT_NEAR(c, exact, 0.02 * exact)
        << "ARD kdecay does not track the exact exponential";
    EXPECT_GT(c, linear * 1.15)
        << "concentration matches the legacy linearized product — the exact-"
           "exponential stage is not running (or double decay is)";
    EXPECT_FALSE(warned(rec.warnings, "not yet applied"))
        << "the retired E1 kdecay warning still fires";
}

// ---------------------------------------------------------------------------
// Gate 2 — THE R6 razor: MSX species are transported on the mesh.
// ---------------------------------------------------------------------------
TEST(ReactionArdBindingTest, MsxDilutionFrontProvesTransport) {
    write_rxn("_e4_tr.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_PIPES]\nRATE X 0\n"
              "[REACTION_TANKS]\nRATE X 0\n"
              "[REACTION_QUALITY]\nGLOBAL X 8\n");
    write_deck("_e4_tr.inp", pc_line("_e4_tr.rxn"));
    const auto rec = run_recording("_e4_tr.inp", "_e4_tr.rpt", "_e4_tr.out", 1);
    ASSERT_TRUE(rec.ok);
    const auto& c5 = rec.msx_link[kC5 * 1 + 0];
    ASSERT_FALSE(c5.empty());

    // Seeding. NOT "c5 still holds 8.0 at step 1": the sloped chain is not
    // static at t=0 — it drains and mixes immediately, and the first
    // recorded step already shows ~7% dilution at C5. The POLLUTANT shows
    // the same 7% from its own Cinit (measured TSS/10 == X/8 == 0.92942 at
    // step 0, e4_probe.log), so the honest seeding claim is that the MSX
    // row starts where a pollutant row seeded the same way starts.
    ASSERT_FALSE(rec.tss_link[kC5].empty());
    EXPECT_NEAR(c5.front() / kMsx0, rec.tss_link[kC5].front() / kC0, 1.0e-6)
        << "MSX initial seeding on the mesh does not match the pollutant "
           "seeding of the same elements";
    // The clean inflow (zero MSX — sources are E5) dilutes the head store
    // and the front advects down the chain: element-local state (R4b) would
    // hold 8.0 forever.
    EXPECT_LT(c5.back(), 0.9 * kMsx0)
        << "no dilution front reached the last link — MSX species are NOT "
           "being transported on the mesh (element-local behavior)";
    // Max principle end to end: transport mixes, never manufactures.
    for (const auto& row : rec.msx_link)
        for (const double v : row)
            EXPECT_LE(v, kMsx0 * (1.0 + 1.0e-6));
}

// ---------------------------------------------------------------------------
// Gate 3 — pipe vs tank kinetics scopes on the ARD mesh.
// ---------------------------------------------------------------------------
TEST(ReactionArdBindingTest, MsxScopedKineticsUnderArd) {
    // Level pool (every element isolated); k_tank = 4·k_pipe so the two
    // scopes separate hard: links → 8·exp(−0.002·120) = 6.293, stores →
    // 8·exp(−0.008·120) = 3.062. In a DRAINING system this gate would be
    // unfair — stores receive pipe-decayed inflow — hence the level pool.
    write_rxn("_e4_sc.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK Y MG\n"
              "[REACTION_PIPES]\nRATE Y -0.002 * Y\n"
              "[REACTION_TANKS]\nRATE Y -0.008 * Y\n"
              "[REACTION_QUALITY]\nGLOBAL Y 8\n");
    write_deck("_e4_sc.inp", pc_line("_e4_sc.rxn"), "", 0.0, /*stagnant=*/true,
               /*pollutants=*/true, "00:02:00");
    const auto rec = run_recording("_e4_sc.inp", "_e4_sc.rpt", "_e4_sc.out", 1);
    ASSERT_TRUE(rec.ok);
    const double link_exp = kMsx0 * std::exp(-0.002 * 120.0);
    const double node_exp = kMsx0 * std::exp(-0.008 * 120.0);
    ASSERT_FALSE(rec.msx_link[kC3].empty());
    ASSERT_FALSE(rec.msx_node[kJ1].empty());
    EXPECT_NEAR(rec.msx_link[kC3].back(), link_exp, 0.05 * link_exp)
        << "conduit cells do not integrate the PIPE-scope expression";
    EXPECT_NEAR(rec.msx_node[kJ1].back(), node_exp, 0.05 * node_exp)
        << "node stores do not integrate the TANK-scope expression";
}

// ---------------------------------------------------------------------------
// Gate 4 — pollutants readable per cell (PUSH_POLLUT on the ARD path).
// ---------------------------------------------------------------------------
TEST(ReactionArdBindingTest, MsxFormulaReadsPollutantPerCell) {
    write_rxn("_e4_fm.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK Z MG\n"
              "[REACTION_PIPES]\nFORMULA Z 3 * TSS\n"
              "[REACTION_TANKS]\nFORMULA Z 3 * TSS\n");
    write_deck("_e4_fm.inp", pc_line("_e4_fm.rxn"), "", kKdecay);
    const auto rec = run_recording("_e4_fm.inp", "_e4_fm.rpt", "_e4_fm.out", 1);
    ASSERT_TRUE(rec.ok);
    const auto& z   = rec.msx_link[kC3];
    const auto& tss = rec.tss_link[kC3];
    ASSERT_EQ(z.size(), tss.size());
    ASSERT_FALSE(z.empty());
    // Publish is a volume-weighted mean and the formula is linear, so the
    // aggregated values must agree tightly at EVERY recorded step, with TSS
    // both decaying and advecting under it.
    for (std::size_t t = 0; t < z.size(); ++t)
        EXPECT_NEAR(z[t], 3.0 * tss[t], 1.0e-6 * std::max(1.0, 3.0 * tss[t]))
            << "step " << t
            << ": FORMULA does not see the cell's pollutant block";
}

// ---------------------------------------------------------------------------
// Gate 5 — the STRIDE razor: MSX presence leaves pollutants bit-identical.
// ---------------------------------------------------------------------------
TEST(ReactionArdBindingTest, MsxPresenceLeavesPollutantsBitwise) {
    write_deck("_e4_st_base.inp", "");
    write_rxn("_e4_st.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK X MG\nBULK W MG\n"
              "[REACTION_PIPES]\nRATE X 0\nRATE W 0\n"
              "[REACTION_TANKS]\nRATE X 0\nRATE W 0\n"
              "[REACTION_QUALITY]\nGLOBAL X 8\nGLOBAL W 2\n");
    write_deck("_e4_st.inp", pc_line("_e4_st.rxn"));
    const auto base = run_recording("_e4_st_base.inp", "_e4_st_base.rpt",
                                    "_e4_st_base.out", 0);
    const auto msx  = run_recording("_e4_st.inp", "_e4_st.rpt", "_e4_st.out", 2);
    ASSERT_TRUE(base.ok);
    ASSERT_TRUE(msx.ok);
    for (int l = 0; l < 5; ++l) {
        const auto& a = base.tss_link[static_cast<std::size_t>(l)];
        const auto& b = msx.tss_link[static_cast<std::size_t>(l)];
        ASSERT_EQ(a.size(), b.size()) << "link " << l;
        for (std::size_t t = 0; t < a.size(); ++t)
            ASSERT_EQ(a[t], b[t])
                << "link " << l << " step " << t
                << ": adding inert MSX species changed a POLLUTANT "
                   "trajectory — an np/ns_total stride slipped somewhere "
                   "(loads, init, donor, or publish).";
    }
}

// ---------------------------------------------------------------------------
// Gate 6 — MSX-only model (np = 0) runs under ARD end to end.
// ---------------------------------------------------------------------------
TEST(ReactionArdBindingTest, MsxOnlyModelRunsUnderArd) {
    write_rxn("_e4_mo.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_PIPES]\nRATE X 0\n"
              "[REACTION_TANKS]\nRATE X 0\n"
              "[REACTION_QUALITY]\nGLOBAL X 8\n");
    write_deck("_e4_mo.inp", pc_line("_e4_mo.rxn"), "", 0.0,
               /*stagnant=*/false, /*pollutants=*/false);
    const auto rec = run_recording("_e4_mo.inp", "_e4_mo.rpt", "_e4_mo.out", 1);
    ASSERT_TRUE(rec.ok);
    const auto& c5 = rec.msx_link[kC5];
    ASSERT_FALSE(c5.empty());
    // No pollutant exists here to calibrate the first step against, and the
    // deck is not static at t=0 (see gate 2), so this is a liveness bracket,
    // not a seeding assertion: the row must start near its seed and below
    // it (max principle), and end diluted.
    EXPECT_LE(c5.front(), kMsx0 * (1.0 + 1.0e-6));
    EXPECT_GT(c5.front(), 0.5 * kMsx0) << "MSX row was not seeded at all";
    EXPECT_LT(c5.back(), 0.9 * kMsx0)
        << "MSX-only model did not transport under ARD (np = 0 path)";
}

// ---------------------------------------------------------------------------
// Gate 7 — WALL species fall back to LEGACY with a precise warning.
// ---------------------------------------------------------------------------
TEST(ReactionArdBindingTest, WallSpeciesFallsBackToLegacy) {
    write_rxn("_e4_wl.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK X MG\nWALL W MG\n"
              "[REACTION_TANKS]\nRATE X 0\n"
              "[REACTION_QUALITY]\nGLOBAL X 8\n");
    write_deck("_e4_wl.inp", pc_line("_e4_wl.rxn"));
    const auto rec = run_recording("_e4_wl.inp", "_e4_wl.rpt", "_e4_wl.out", 0);
    ASSERT_TRUE(rec.ok);
    EXPECT_TRUE(warned(rec.warnings, "WALL species have no transport"))
        << "WALL species under EULERIAN_ARD fell through without a word";
    EXPECT_TRUE(warned(rec.warnings, "falling back to LEGACY"))
        << "the engine did not report the LEGACY fallback";
}

// ---------------------------------------------------------------------------
// Gate 8 — failure containment names the ARD element; never fatal.
// ---------------------------------------------------------------------------
TEST(ReactionArdBindingTest, FailureContainmentNamesArdElement) {
    // The R4 recipe verbatim: the DEFAULT solver (RK5) on kinetics far past
    // its stability cliff. `SOLVER EUL` cannot produce this: EUL takes one
    // explicit step with no error control, so it never reports failure —
    // measured ok=1, y=4e7, substeps=1 for Y' = +1e6*Y (e4_probe.log). RK5
    // on Y' = -1e6*Y hits the substep cap and returns the actionable
    // message. The run must COMPLETE, warn once naming the ARD element, and
    // leave finite state.
    write_rxn("_e4_fc.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK Y MG\n"
              "[REACTION_PIPES]\nRATE Y -1000000.0 * Y\n"
              "[REACTION_TANKS]\nRATE Y -1000000.0 * Y\n"
              "[REACTION_QUALITY]\nGLOBAL Y 8\n");
    write_deck("_e4_fc.inp", pc_line("_e4_fc.rxn"), "", 0.0,
               /*stagnant=*/false, /*pollutants=*/true, "00:02:00");
    const auto rec = run_recording("_e4_fc.inp", "_e4_fc.rpt", "_e4_fc.out", 1);
    ASSERT_TRUE(rec.ok) << "a contained reaction failure must not kill the run";
    // "at ARD cell", not just "at ARD": reactArdStage contains failures at
    // TWO call sites (cells, then node stores) and the warning latches after
    // the first, so a bare "ARD" needle lets either site cover for the other
    // — removing the cell-loop containment left this gate green (falsifier
    // vii). Cells are integrated first, so the cell wording is deterministic.
    EXPECT_TRUE(warned(rec.warnings, "Reaction step failed at ARD cell"))
        << "integration failure in a conduit cell was silent";
    for (const auto& row : rec.msx_link)
        for (const double v : row)
            EXPECT_TRUE(std::isfinite(v))
                << "containment leaked non-finite state into the mesh";
}

// ---------------------------------------------------------------------------
// Gate 9 — the CONGRUENCE razor: an inert MSX row and an inert POLLUTANT row
// seeded identically must follow IDENTICAL trajectories. Same mesh, same
// fluxes, same kernels — the only difference is which row of cell_phi they
// occupy, so any divergence is a defect in the MSX row's plumbing.
//
// None of gates 1-8 could see the defect this caught. Gate 5 (the stride
// razor) compares POLLUTANT trajectories with and without MSX present, so it
// is blind to anything that damages only an MSX row. Gate 2's dilution front
// is a one-sided inequality that a corrupted row still satisfies. The actual
// defect: the E4/R6 stride audit narrowed the node-store load loop to the
// pollutant rows — correctly, since qual_mass_in is np-strided — and swept
// the loop's non-negativity CLAMP along with it. MSX store masses lost their
// floor, so a node that repeatedly empties (J4, just upstream of the
// outfall) accumulated a large oscillating NEGATIVE mass that the boundary
// donor then fed into the adjoining conduit. Measured before the fix:
// |TSS - X| up to 6.70e-01, confined to the outfall-adjacent conduit, while
// every pollutant row stayed bit-identical. After: 1.8e-15.
// ---------------------------------------------------------------------------
TEST(ReactionArdBindingTest, InertMsxRowMatchesInertPollutantRow) {
    // Cinit == GLOBAL == 8 so the two initial FIELDS coincide, both inert,
    // every element wet. Equality, not proportionality — no scaling to argue
    // about.
    write_rxn("_e4_cg.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_QUALITY]\nGLOBAL X 8\n");
    write_deck("_e4_cg.inp", pc_line("_e4_cg.rxn"), "", /*kdecay=*/0.0,
               /*stagnant=*/false, /*pollutants=*/true, "00:30:00",
               /*cinit=*/kMsx0);
    const auto rec = run_recording("_e4_cg.inp", "_e4_cg.rpt", "_e4_cg.out", 1);
    ASSERT_TRUE(rec.ok);

    double worst = 0.0;
    int worst_link = -1;
    std::size_t worst_step = 0;
    for (int l = 0; l < 5; ++l) {
        const auto& t = rec.tss_link[static_cast<std::size_t>(l)];
        const auto& x = rec.msx_link[static_cast<std::size_t>(l)];
        ASSERT_EQ(t.size(), x.size()) << "link " << l;
        ASSERT_FALSE(t.empty());
        for (std::size_t k = 0; k < t.size(); ++k) {
            const double d = std::fabs(t[k] - x[k]);
            if (d > worst) { worst = d; worst_link = l; worst_step = k; }
        }
    }
    // The deck's Cinit is 8 mg/L, so 1e-9 is ~1e-10 relative — round-off
    // room and nothing more.
    EXPECT_LT(worst, 1.0e-9)
        << "an inert MSX row diverged from an identically seeded inert "
           "pollutant row by " << worst << " at link C" << (worst_link + 1)
        << " step " << worst_step
        << " — the MSX row's transport, store bookkeeping or publish differs "
           "from the pollutant path.";
}

}  // namespace
