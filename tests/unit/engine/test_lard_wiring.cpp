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
 *          2. A LARD run publishes exactly zero quality while a LEGACY run
 *             of the SAME deck publishes nonzero — the control run is the
 *             liveness half, so this gate cannot pass vacuously on a deck
 *             that delivers no quality signal (E3 lesson 9).
 *             Falsify: remove the skeleton's zeroing fill.
 *          3. Hydraulics are bitwise-identical LEGACY vs LAGRANGIAN —
 *             quality is passive, so the dispatch may not perturb flow.
 *             Falsify: make the skeleton touch any hydraulic array.
 *          4. The default (no QUALITY_SOLVER line) still runs LEGACY and
 *             transports — guards the reordered else-chain.
 *             Falsify: break the dispatch default arm.
 *          5. The bypass warning fires exactly when the quality stage would
 *             have been live, in BOTH directions (R4 lesson 5): present for
 *             pollutants and for a reserved-species-only deck, absent when
 *             there is nothing to transport, absent under IGNORE_QUALITY
 *             (whose own warning family covers that configuration).
 *             Falsify: remove the open()-time warning, or widen/narrow its
 *             predicate away from stepRouting's.
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
    bool ignore_quality = false;        ///< IGNORE_QUALITY YES
};

void write_deck(const std::string& path, const DeckSpec& s) {
    std::ofstream f(path);
    f << "[TITLE]\nLARD X1 wiring\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n";
    if (s.solver[0] != '\0') f << "QUALITY_SOLVER " << s.solver << "\n";
    if (s.water_age) f << "WATER_AGE ON\n";
    if (s.ignore_quality) f << "IGNORE_QUALITY YES\n";
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

/// The one phrase unique to the X1 bypass warning (R1 probe refinement:
/// assert wording unique to the defense under test, so a different warning
/// family cannot satisfy this gate).
constexpr const char* kLardWarn = "LARD transport engine is not yet implemented";

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
// Gate 2 — inert no-op, with the control run carrying the liveness claim.
// ---------------------------------------------------------------------------
TEST(LardWiringTest, LagrangianPublishesZeroWhereLegacyPublishesSignal) {
    DeckSpec lard;                      // LAGRANGIAN
    DeckSpec leg;  leg.solver = "LEGACY";

    const WiringRun a = run_deck("_lw_zero_lard", lard);
    const WiringRun b = run_deck("_lw_zero_leg", leg);
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);

    // Liveness first: if LEGACY reads no quality on this deck, the deck is
    // broken and the zero assertion below would be measuring nothing.
    ASSERT_GT(b.peak_link_conc, 1.0)
        << "the control run carries no quality signal — deck premise broken";

    // The claim: not one nonzero concentration, at any element, at any step.
    // Cinit = 50 seeds real state at start, so a skeleton that stopped
    // zeroing fails here on the very first sample.
    EXPECT_EQ(a.peak_node_conc, 0.0)
        << "a LARD node published nonzero quality — the skeleton leaked "
           "state (frozen Cinit masquerading as a result)";
    EXPECT_EQ(a.peak_link_conc, 0.0)
        << "a LARD link published nonzero quality";

    // And the run says so: the bypass has its observer.
    EXPECT_TRUE(has_lard_warning(a.warnings))
        << "LARD ran as a silent no-quality run — the E1-era rule";
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
    // (a) Reserved-species-only: WATER_AGE ON with no [POLLUTANTS]. The
    // stage-liveness predicate includes the age family (the lesson-52
    // shape: a temperature/age-only deck has no pollutant row to gate on).
    DeckSpec age_only;
    age_only.pollutants = false;
    age_only.water_age = true;
    const WiringRun a = run_deck("_lw_warn_age", age_only);
    ASSERT_TRUE(a.ok);
    EXPECT_TRUE(has_lard_warning(a.warnings))
        << "an age-only LARD deck ran silently — the predicate lost the "
           "reserved-species families";

    // (b) Nothing to transport: no pollutants, no age, no heat. A warning
    // here would be noise about a bypass of nothing.
    DeckSpec bare;
    bare.pollutants = false;
    const WiringRun b = run_deck("_lw_warn_bare", bare);
    ASSERT_TRUE(b.ok);
    EXPECT_FALSE(has_lard_warning(b.warnings))
        << "the LARD warning fired with nothing to transport";

    // (c) IGNORE_QUALITY wins: that configuration's own warning family
    // covers it, and stepRouting's guard skips the stage before the solver
    // choice is ever consulted — the LARD warning must match.
    DeckSpec ignored;
    ignored.ignore_quality = true;
    const WiringRun c = run_deck("_lw_warn_ign", ignored);
    ASSERT_TRUE(c.ok);
    EXPECT_FALSE(has_lard_warning(c.warnings))
        << "the LARD warning fired under IGNORE_QUALITY, where the quality "
           "stage does not run for ANY solver";
}

}  // namespace
