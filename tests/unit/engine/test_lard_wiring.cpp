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
 * @file test_lard_wiring.cpp
 * @brief X1: `QUALITY_SOLVER LAGRANGIAN` is a real, warned, inert dispatch.
 *
 * @details Subplan X1 (plans/transport/LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md,
 *          strategy §12 Phase 0). The claims, each with the probe that would
 *          falsify it:
 *
 *          1. The option parses and survives a save-as (the A1a defect
 *             shape: a save-as must not silently reopen as LEGACY).
 *             Falsify: drop the InpWriter LAGRANGIAN branch.
 *          2. [X2 FLIP of X1's inert-no-op claim] LARD transports: the
 *             steady deck reads kCin at every link like the LEGACY control,
 *             under the maximum principle, with NO bypass warning on a
 *             pollutant-only deck. Falsify: break the dispatch branch, or
 *             re-widen the warning predicate to X1's.
 *          3. Hydraulics are bitwise-identical LEGACY vs LAGRANGIAN —
 *             quality is passive, so the dispatch may not perturb flow.
 *             Falsify: make the solver touch any hydraulic array.
 *          4. The default (no QUALITY_SOLVER line) still runs LEGACY and
 *             transports — guards the reordered else-chain.
 *             Falsify: break the dispatch default arm.
 *          5. The bypass warning (X4 wording) fires for a HEAT deck under
 *             LARD only — age is live as of X4 — not for a pollutant-only,
 *             age-only, or bare deck, and never under IGNORE_QUALITY (R4
 *             lesson 5, both directions). Falsify: drop the warning or
 *             move its guard off stepRouting's condition.
 *
 *          Scratch fixtures use the `_lw_` prefix — unique per the
 *          configure-time collision check (b85b802d).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/InpWriter.hpp"
#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kQ = 5.0;      ///< steady inflow, cfs
constexpr double kCin = 100.0;  ///< steady inflow concentration, mg/L

/// Deck knobs. The base deck is the five-conduit chain the node-store suite
/// established: one steady flow + concentration source at the head, no decay,
/// so under any WORKING transport engine the answer everywhere is kCin.
struct DeckSpec {
    const char* solver = "LAGRANGIAN";  ///< QUALITY_SOLVER value; "" = omit line
    bool pollutants = true;             ///< [POLLUTANTS] TSS + its inflow row
    bool water_age = false;             ///< WATER_AGE ON
    bool heat = false;                  ///< HEAT_TRANSPORT ON (live under
                                        ///< LARD as of H7b)
    bool ignore_quality = false;        ///< IGNORE_QUALITY YES
    int quality_step = 0;               ///< X3a: QUALITY_STEP seconds; 0 = omit
    int max_segs = 0;                   ///< X3a: MAX_SEGMENTS_PER_LINK; 0 = omit
    bool rwpt = false;                  ///< X3b: DISPERSION RWPT
    int rwpt_seed = 0;                  ///< X3b: RWPT_SEED; 0 = omit
};

void write_deck(const std::string& path, const DeckSpec& s) {
    std::ofstream f(path);
    f << "[TITLE]\nLARD X1 wiring\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n";
    if (s.solver[0] != '\0') f << "QUALITY_SOLVER " << s.solver << "\n";
    if (s.water_age) f << "WATER_AGE ON\n";
    if (s.heat) f << "HEAT_TRANSPORT ON\n";
    if (s.ignore_quality) f << "IGNORE_QUALITY YES\n";
    if (s.quality_step > 0) f << "QUALITY_STEP " << s.quality_step << "\n";
    if (s.max_segs > 0) f << "MAX_SEGMENTS_PER_LINK " << s.max_segs << "\n";
    if (s.rwpt) f << "DISPERSION RWPT\n";
    if (s.rwpt_seed != 0) f << "RWPT_SEED " << s.rwpt_seed << "\n";
    f << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      // 4 h, not 1: at 1 h the LEGACY control's exponential approach to
      // steady state leaves the tail links at 99.998 against kCin's 1e-6
      // band -- measured 2026-08-23, the handoff's anticipated failure #1.
      // The fix is a longer run, not a wider band (lesson 55).
      << "END_DATE 01/01/2026\nEND_TIME 04:00:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:05:00\n\n"
      << "[JUNCTIONS]\n"
      << "J0 10.0 10 1.5 0 0\nJ1 9.4  10 1.5 0 0\nJ2 8.8  10 1.5 0 0\n"
      << "J3 8.2  10 1.5 0 0\nJ4 7.6  10 1.5 0 0\n\n"
      << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
      << "[CONDUITS]\n"
      << "C1 J0 J1 500 0.013 0 0 0\nC2 J1 J2 500 0.013 0 0 0\n"
      << "C3 J2 J3 500 0.013 0 0 0\nC4 J3 J4 500 0.013 0 0 0\n"
      << "C5 J4 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
      << "C3 CIRCULAR 2.0 0 0 0\nC4 CIRCULAR 2.0 0 0 0\n"
      << "C5 CIRCULAR 2.0 0 0 0\n\n";
    if (s.pollutants) {
        // Last field is Cinit = 50: the junctions start wet (Y0 = 1.5), so
        // initQuality seeds real state — the state gate 2's zeroing claim
        // must be able to fail against.
        f << "[POLLUTANTS]\nTSS MG/L 0 0 0 0 NO * 0 0 50\n\n";
    }
    f << "[INFLOWS]\n"
      << "J0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n";
    if (s.pollutants)
        f << "J0 TSS  \"\" CONCEN 1.0 1.0 " << kCin << "\n";
    f << "\n[REPORT]\nINPUT NO\n";
}

/// Everything one run observes. Peaks are sampled at EVERY step: a transient
/// nonzero that relaxes away before the end must still fail the inertness
/// claim.
struct WiringRun {
    double peak_node_conc = 0.0;
    double peak_link_conc = 0.0;
    std::vector<double> depth_trace;  ///< J2 depth per step
    std::vector<double> flow_trace;   ///< C3 flow per step
    std::vector<double> link_final;   ///< [link] conc at end
    std::vector<std::string> warnings;
    int solver_kind = -1;             ///< ctx value after open
    bool ok = false;
};

WiringRun run_deck(const std::string& tag, const DeckSpec& s) {
    WiringRun r;
    const std::string inp = tag + ".inp";
    const std::string rpt = tag + ".rpt";
    const std::string out = tag + ".out";
    write_deck(inp, s);

    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) { ADD_FAILURE() << "engine create"; return r; }
    if (swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr) !=
            SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        ADD_FAILURE() << "open/init/start failed for " << inp;
        swmm_engine_destroy(e);
        return r;
    }
    auto& ctx = as_cpp_engine(e).context();
    r.solver_kind = static_cast<int>(ctx.options.quality_solver);
    const int np = ctx.n_pollutants();
    const int nl = ctx.n_links();
    const int nn = ctx.n_nodes();
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
            ADD_FAILURE() << "step failed for " << inp;
            swmm_engine_destroy(e);
            return r;
        }
        if (np > 0) {
            for (int l = 0; l < nl; ++l)
                r.peak_link_conc = std::max(
                    r.peak_link_conc,
                    ctx.links.conc[static_cast<std::size_t>(l * np)]);
            for (int j = 0; j < nn; ++j)
                r.peak_node_conc = std::max(
                    r.peak_node_conc,
                    ctx.nodes.conc[static_cast<std::size_t>(j * np)]);
        }
        r.depth_trace.push_back(ctx.nodes.depth[2]);  // J2
        r.flow_trace.push_back(ctx.links.flow[2]);    // C3
    } while (elapsed > 0.0 && ++guard < 200000);
    swmm_engine_end(e);
    if (np > 0) {
        r.link_final.assign(static_cast<std::size_t>(nl), 0.0);
        for (int l = 0; l < nl; ++l)
            r.link_final[static_cast<std::size_t>(l)] =
                ctx.links.conc[static_cast<std::size_t>(l * np)];
    }
    r.warnings = ctx.warnings;
    r.ok = true;
    swmm_engine_destroy(e);
    return r;
}

/// The one phrase unique to the X2 reserved-species bypass warning (R1 probe
/// refinement: assert wording unique to the defense under test, so a
/// different warning family cannot satisfy this gate). X1's blanket
/// "transport not implemented" warning retired with X2 — pollutant transport
/// is live; what remains bypassed under LARD is age/heat (X4).
constexpr const char* kLardWarn = "does not advance under the LARD engine yet";

bool has_lard_warning(const std::vector<std::string>& warnings) {
    for (const auto& w : warnings)
        if (w.find(kLardWarn) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Gate 1 — the option parses, and survives a save-as (both spellings).
// ---------------------------------------------------------------------------
TEST(LardWiringTest, LagrangianOptionRoundTripsThroughSaveAs) {
    DeckSpec s;  // LAGRANGIAN, pollutants on
    s.quality_step = 2;   // X3a keys ride the same save-as rule
    s.max_segs = 50;
    s.rwpt = true;        // X3b keys too (handoff §5.viii's W1 extension)
    s.rwpt_seed = 42;
    write_deck("_lw_rt.inp", s);

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_lw_rt.inp", "_lw_rt.rpt", "_lw_rt.out",
                               nullptr),
              SWMM_OK);
    auto& ctx = as_cpp_engine(e).context();
    ASSERT_EQ(ctx.options.quality_solver,
              openswmm::QualitySolverKind::LAGRANGIAN)
        << "QUALITY_SOLVER LAGRANGIAN did not parse to the enum value";

    std::vector<std::string> warnings;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, "_lw_rt_saved.inp",
                                                 &warnings),
              0);
    swmm_engine_destroy(e);

    // The saved file must reopen as LAGRANGIAN — the A1a defect was exactly
    // this round-trip silently degrading to LEGACY.
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, "_lw_rt_saved.inp", "_lw_rt2.rpt",
                               "_lw_rt2.out", nullptr),
              SWMM_OK);
    EXPECT_EQ(as_cpp_engine(e2).context().options.quality_solver,
              openswmm::QualitySolverKind::LAGRANGIAN)
        << "save-as dropped QUALITY_SOLVER LAGRANGIAN (reopened as LEGACY)";
    // X3a: the stepping keys survive the same round-trip — dropping either
    // silently changes the transport discretization on reopen.
    EXPECT_DOUBLE_EQ(as_cpp_engine(e2).context().options.quality_step, 2.0)
        << "save-as dropped QUALITY_STEP";
    EXPECT_EQ(as_cpp_engine(e2).context().options.max_segments_per_link, 50)
        << "save-as dropped MAX_SEGMENTS_PER_LINK";
    EXPECT_TRUE(as_cpp_engine(e2).context().options.lard_rwpt)
        << "save-as dropped DISPERSION RWPT";
    EXPECT_EQ(as_cpp_engine(e2).context().options.rwpt_seed, 42)
        << "save-as dropped RWPT_SEED";
    swmm_engine_destroy(e2);

    // The short spelling parses too (the EULERIAN_ARD/ARD precedent).
    DeckSpec alias = s;
    alias.solver = "LARD";
    write_deck("_lw_alias.inp", alias);
    SWMM_Engine e3 = swmm_engine_create();
    ASSERT_NE(e3, nullptr);
    ASSERT_EQ(swmm_engine_open(e3, "_lw_alias.inp", "_lw_alias.rpt",
                               "_lw_alias.out", nullptr),
              SWMM_OK);
    EXPECT_EQ(as_cpp_engine(e3).context().options.quality_solver,
              openswmm::QualitySolverKind::LAGRANGIAN);
    swmm_engine_destroy(e3);
}

// ---------------------------------------------------------------------------
// Gate 2 — X2 FLIP of X1's inert-no-op gate (the H1-precedent inversion:
// the behavior it asserted is retired by the phase that implements the
// engine). LARD now transports: the same deck reads the same steady answer
// as LEGACY, and the pollutant-only run carries NO bypass warning.
// ---------------------------------------------------------------------------
TEST(LardWiringTest, LagrangianTransportsWhereLegacyDoes) {
    DeckSpec lard;                      // LAGRANGIAN
    DeckSpec leg;  leg.solver = "LEGACY";

    const WiringRun a = run_deck("_lw_zero_lard", lard);
    const WiringRun b = run_deck("_lw_zero_leg", leg);
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);

    // Control liveness (unchanged from X1): if LEGACY reads no quality on
    // this deck, the deck premise is broken.
    ASSERT_GT(b.peak_link_conc, 1.0)
        << "the control run carries no quality signal — deck premise broken";

    // The X2 claim: LARD transports. Steady source, no decay — the exact
    // answer at every link is kCin, same as gate 4 asserts for LEGACY.
    ASSERT_EQ(a.link_final.size(), 5u);
    for (std::size_t l = 0; l < a.link_final.size(); ++l)
        EXPECT_NEAR(a.link_final[l], kCin, 1.0e-6)
            << "LARD link " << l << " did not carry the steady signal";
    // Max principle: one source feeding the network — nothing may exceed it
    // (mass manufacture, the 7b2dfaae shape).
    EXPECT_LE(a.peak_link_conc, kCin * (1.0 + 1.0e-9));
    EXPECT_LE(a.peak_node_conc, kCin * (1.0 + 1.0e-9));

    // A pollutant-only LARD run is no longer a bypass of anything: no
    // reserved-species warning may fire.
    EXPECT_FALSE(has_lard_warning(a.warnings))
        << "the age/heat bypass warning fired on a pollutant-only deck";
    EXPECT_FALSE(has_lard_warning(b.warnings))
        << "the LARD warning fired on a LEGACY run";
}

// ---------------------------------------------------------------------------
// Gate 3 — hydraulics are bitwise-identical: the dispatch is quality-only.
// ---------------------------------------------------------------------------
TEST(LardWiringTest, LagrangianLeavesHydraulicsBitIdentical) {
    DeckSpec lard;
    DeckSpec leg;  leg.solver = "LEGACY";

    const WiringRun a = run_deck("_lw_hyd_lard", lard);
    const WiringRun b = run_deck("_lw_hyd_leg", leg);
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    ASSERT_EQ(a.depth_trace.size(), b.depth_trace.size())
        << "the two runs took different step counts — already a divergence";
    ASSERT_FALSE(a.depth_trace.empty());

    for (std::size_t i = 0; i < a.depth_trace.size(); ++i) {
        // Exact equality on purpose: quality is passive, so the solver
        // switch must not move hydraulics by one ULP.
        ASSERT_EQ(a.depth_trace[i], b.depth_trace[i])
            << "J2 depth diverged at step " << i;
        ASSERT_EQ(a.flow_trace[i], b.flow_trace[i])
            << "C3 flow diverged at step " << i;
    }
}

// ---------------------------------------------------------------------------
// Gate 4 — the default arm still transports (guards the else-chain).
// ---------------------------------------------------------------------------
TEST(LardWiringTest, DefaultDeckStillRunsLegacyTransport) {
    DeckSpec s;
    s.solver = "";  // no QUALITY_SOLVER line at all
    const WiringRun r = run_deck("_lw_default", s);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.solver_kind,
              static_cast<int>(openswmm::QualitySolverKind::LEGACY));
    ASSERT_EQ(r.link_final.size(), 5u);
    // Steady source, no decay: the exact answer everywhere is kCin. This is
    // the transported-signal claim, not merely "nonzero".
    for (std::size_t l = 0; l < r.link_final.size(); ++l)
        EXPECT_NEAR(r.link_final[l], kCin, 1.0e-6)
            << "default-path LEGACY transport broken at link " << l;
    EXPECT_FALSE(has_lard_warning(r.warnings));
}

// ---------------------------------------------------------------------------
// Gate 5 — the warning predicate, exercised in both directions.
// ---------------------------------------------------------------------------
TEST(LardWiringTest, BypassWarningFiresExactlyWhenTheStageWouldBeLive) {
    // (a) H7b FLIP: a heat deck under LARD is now LIVE — temperature rides
    // the segments as the second reserved row and takes RWPT dispersion,
    // so the bypass warning must NOT fire. (Until 2026-08-30 this leg
    // asserted the opposite; retiring a deferral flips the assertion that
    // recorded it, lesson 21.)
    DeckSpec heat_only;
    heat_only.pollutants = false;
    heat_only.heat = true;
    const WiringRun a = run_deck("_lw_warn_heat", heat_only);
    ASSERT_TRUE(a.ok);
    EXPECT_FALSE(has_lard_warning(a.warnings))
        << "the bypass warning fired on a heat deck — temperature is live "
           "under LARD as of H7b, and warning about it would be a false "
           "claim";

    // (a2) X4 FLIP: an age deck under LARD is now LIVE — no bypass warning.
    DeckSpec age_only;
    age_only.pollutants = false;
    age_only.water_age = true;
    const WiringRun a2 = run_deck("_lw_warn_age", age_only);
    ASSERT_TRUE(a2.ok);
    EXPECT_FALSE(has_lard_warning(a2.warnings))
        << "the bypass warning fired on an age deck — age is live under "
           "LARD as of X4, and warning about it would be a false claim";

    // (b) Pollutants only, no age, no heat: transport is live, nothing is
    // bypassed — a warning here would be noise (also asserted by gate 2 on
    // its own deck; this leg keeps the direction explicit when gate 2's
    // deck changes).
    DeckSpec bare;
    bare.pollutants = false;
    const WiringRun b = run_deck("_lw_warn_bare", bare);
    ASSERT_TRUE(b.ok);
    EXPECT_FALSE(has_lard_warning(b.warnings))
        << "the LARD warning fired with nothing bypassed";

    // (c) IGNORE_QUALITY wins even over a deck that WOULD warn: the stage
    // is skipped before the solver choice is consulted, and that
    // configuration's own warning family covers it.
    DeckSpec ignored;
    ignored.ignore_quality = true;
    ignored.heat = true;  // the warning's trigger, suppressed by (c)
    const WiringRun c = run_deck("_lw_warn_ign", ignored);
    ASSERT_TRUE(c.ok);
    EXPECT_FALSE(has_lard_warning(c.warnings))
        << "the LARD warning fired under IGNORE_QUALITY, where the quality "
           "stage does not run for ANY solver";
}

}  // namespace
