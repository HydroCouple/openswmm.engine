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
 * @file test_water_age.cpp
 * @brief A1a gates: water-age tracking on the ARD mesh + the waterage
 *        component ([WATER_AGE_SOURCES])
 *        (plans/transport/WATER_AGE_TRACKING_PLAN.md §1–§2, §7 A1).
 *
 * @details Observation paths:
 *          - PureAgeDeckTracksResidenceTime: NO [POLLUTANTS], no reactions
 *            component — the pure-age model IS A1a's motivating
 *            configuration (lesson 20). Steady clean inflow at age 0: the
 *            outfall-adjacent link's age must settle near the system
 *            residence time V/Q (measured from the run itself) and grow
 *            monotonically down the chain.
 *          - SourceAgeShiftsEffluentExactly: aging and volume-weighted
 *            mixing are LINEAR in age, so an EXTERNAL_INFLOW initial age
 *            of 6 h shifts the steady-state effluent age by EXACTLY
 *            21600 s relative to the no-component run — a sharp analytic
 *            band, the age analogue of E5a's units gate.
 *          - LevelPoolAgingIsExact: zero-flow equilibrium with
 *            INITIAL_STATE 1 h: every element's age after t seconds is
 *            3600 + t exactly (mixing equal ages is the identity).
 *          - AgeRowLeavesOtherRowsBitwise: lesson-15 symmetric razor for
 *            the NEW row class — turning WATER_AGE ON must leave TSS and
 *            MSX trajectories bit-identical (the age row is LAST; any
 *            stride slip in loads/publish/reactions guard fails here).
 *            This also observes the reactArdStage guard fix: the old
 *            ns_total equality check would have silently skipped ALL MSX
 *            reactions once the age row appeared.
 *          - ConfigErrors: TIMESERIES/SUBCATCH/EDGE_BC deferral wording,
 *            unknown source/node, NODE scope restricted to
 *            DWF/EXTERNAL_INFLOW, never-half-apply.
 *          - BypassWarnings: component with WATER_AGE OFF; WATER_AGE ON
 *            under LEGACY names A1b (lessons 10/20).
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

#include "core/InpWriter.hpp"
#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kQ = 5.0;  ///< steady inflow, cfs

/// The established chain deck. `water_age` writes [OPTIONS] WATER_AGE ON;
/// `stagnant` gives the level pool; `pollutants` toggles [POLLUTANTS].
void write_deck(const char* path, const std::string& pc_lines,
                bool water_age = true, bool pollutants = false,
                bool stagnant = false,
                const std::string& extra_options = "") {
    std::ofstream f(path);
    f << "[TITLE]\nA1a water age gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "QUALITY_SOLVER EULERIAN_ARD\n"
      << (water_age ? "WATER_AGE ON\n" : "")
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME "
      << (stagnant ? "00:02:00" : "01:00:00") << "\n"
      // ROUTING_STEP 1 is load-bearing, not a leftover. The ARD engine
      // loses external-inflow mass above ROUTING_STEP 2 on this deck shape
      // — a PRE-EXISTING defect, measured identically with WATER_AGE OFF
      // and absent under QUALITY_SOLVER LEGACY (a CONCEN 100 mg/L inflow
      // reads 100.000 at rs<=2, 70.594 at rs=5, and 7730 at rs=20). At
      // rs=5 it dragged gate 2's 6-hour shift down to 15246 s and made the
      // age row look broken when it was faithfully tracking the pollutant
      // path. At rs<=2 the transport is exact and these gates measure the
      // age row alone. See §5 of the A1a handoff.
      << "ROUTING_STEP 1\nREPORT_STEP 00:01:00\n"
      << extra_options << "\n";
    if (stagnant) {
        f << "[JUNCTIONS]\n"
          << "J0 10.0 10 1.5 0 0\nJ1 10.0 10 1.5 0 0\n"
          << "J2 10.0 10 1.5 0 0\nJ3 10.0 10 1.5 0 0\n"
          << "J4 10.0 10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 10.0 FIXED 11.5 NO\n\n";
    } else {
        f << "[JUNCTIONS]\n"
          << "J0 10.0 10 1.5 0 0\nJ1 9.4  10 1.5 0 0\nJ2 8.8  10 1.5 0 0\n"
          << "J3 8.2  10 1.5 0 0\nJ4 7.6  10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n";
    }
    f << "[CONDUITS]\n"
      << "C1 J0 J1 500 0.013 0 0 0\nC2 J1 J2 500 0.013 0 0 0\n"
      << "C3 J2 J3 500 0.013 0 0 0\nC4 J3 J4 500 0.013 0 0 0\n"
      << "C5 J4 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
      << "C3 CIRCULAR 2.0 0 0 0\nC4 CIRCULAR 2.0 0 0 0\n"
      << "C5 CIRCULAR 2.0 0 0 0\n\n";
    if (!stagnant)
        f << "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n\n";
    if (pollutants)
        f << "[POLLUTANTS]\n"
          << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac "
             "Cdwf Cinit\n"
          << "TSS    MG/L  0     0   0     0      NO       *        0      "
             "0    10\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

void write_file(const char* path, const std::string& body) {
    std::ofstream c(path);
    c << body;
}

struct RunRecord {
    std::vector<std::vector<double>> tss_link;   ///< [link][step]
    std::vector<std::vector<double>> msx_link;   ///< [link][step] species 0
    std::vector<std::vector<double>> age_link;   ///< [link][step], seconds
    std::vector<double> age_node_final;          ///< [node], seconds
    std::vector<std::string> warnings;
    double system_volume_final = 0.0;            ///< ft³ (nodes + links)
    bool ok = false;
};

RunRecord run_recording(const char* inp, const char* rpt, const char* out) {
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
        const int np = ctx.n_pollutants();
        const int nm = ctx.reactions.n_species();
        rec.tss_link.assign(static_cast<std::size_t>(nl), {});
        rec.msx_link.assign(static_cast<std::size_t>(nl), {});
        rec.age_link.assign(static_cast<std::size_t>(nl), {});
        double elapsed = 0.0;
        int guard = 0;
        do {
            if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
                ADD_FAILURE() << "step failed for " << inp;
                ok = false;
                break;
            }
            for (int l = 0; l < nl; ++l) {
                const auto ul = static_cast<std::size_t>(l);
                if (np > 0)
                    rec.tss_link[ul].push_back(
                        ctx.links.conc[static_cast<std::size_t>(l * np)]);
                if (nm > 0) {
                    const auto idx = ul * static_cast<std::size_t>(nm);
                    rec.msx_link[ul].push_back(
                        (idx < ctx.reactions.msx_link_conc.size())
                            ? ctx.reactions.msx_link_conc[idx]
                            : -1.0);
                }
                rec.age_link[ul].push_back(
                    (ul < ctx.water_age_state.link_age.size())
                        ? ctx.water_age_state.link_age[ul]
                        : -1.0);
            }
        } while (elapsed > 0.0 && ++guard < 20000);
        if (ok) swmm_engine_end(e);
        rec.age_node_final = ctx.water_age_state.node_age;
        for (int j = 0; j < ctx.n_nodes(); ++j)
            rec.system_volume_final +=
                ctx.nodes.volume[static_cast<std::size_t>(j)];
        for (int j = 0; j < ctx.n_links(); ++j)
            rec.system_volume_final +=
                ctx.links.volume[static_cast<std::size_t>(j)];
        rec.warnings = ctx.warnings;
    }
    swmm_engine_destroy(e);
    rec.ok = ok;
    return rec;
}

bool has_needle(const std::vector<std::string>& v, const std::string& n) {
    for (const auto& s : v)
        if (s.find(n) != std::string::npos) return true;
    return false;
}

constexpr int kC1 = 0, kC3 = 2, kC5 = 4;

// ---------------------------------------------------------------------------
// Gate 1 — the motivating configuration: pure-age deck, residence time.
// ---------------------------------------------------------------------------
TEST(WaterAgeTest, PureAgeDeckTracksResidenceTime) {
    write_deck("_a1_res.inp", "");   // no pollutants, no components
    const auto rec = run_recording("_a1_res.inp", "_a1_res.rpt", "_a1_res.out");
    ASSERT_TRUE(rec.ok);
    ASSERT_FALSE(rec.age_link[kC5].empty());

    const double age_c5 = rec.age_link[kC5].back();
    const double age_c1 = rec.age_link[kC1].back();
    ASSERT_GT(age_c5, 0.0)
        << "age never grew — the age row is dead on the pure-age deck "
           "(the A1a motivating configuration; lesson 20)";
    EXPECT_GT(age_c5, age_c1)
        << "age does not grow along the flow direction";

    // The residence-time theorem: at steady state the mean age of water
    // leaving a system is exactly V/Q. Measured on this deck: 0.9426 (the
    // 5.7% shortfall is cell discretization plus the volume-weighted
    // publish, and it is stable across ROUTING_STEP and cell length).
    // Tightened from the delivered ±40%, which admitted a ratio of 0.6 —
    // a band that wide passes on a deck where the aging stage runs at
    // half rate.
    const double residence = rec.system_volume_final / kQ;
    EXPECT_GT(age_c5, 0.88 * residence);
    EXPECT_LT(age_c5, 1.12 * residence);
}

// ---------------------------------------------------------------------------
// Gate 2 — source age shifts the steady effluent age EXACTLY (linearity).
// ---------------------------------------------------------------------------
TEST(WaterAgeTest, SourceAgeShiftsEffluentExactly) {
    write_deck("_a1_base.inp", "");
    write_file("_a1_src.age",
               "[WATER_AGE_SOURCES]\nEXTERNAL_INFLOW NODE J0 6.0\n");
    write_deck("_a1_src.inp",
               "org.hydrocouple.openswmm.waterage config=\"_a1_src.age\"");
    const auto base = run_recording("_a1_base.inp", "_a1_base.rpt",
                                    "_a1_base.out");
    const auto src  = run_recording("_a1_src.inp", "_a1_src.rpt",
                                    "_a1_src.out");
    ASSERT_TRUE(base.ok);
    ASSERT_TRUE(src.ok);
    ASSERT_FALSE(base.age_link[kC5].empty());

    // Aging and volume-weighted mixing are linear in age: once the aged
    // inflow has flushed the chain, every parcel at C5 carries exactly
    // 21600 s more than in the base run. Measured: 21600.000 — the
    // linearity is EXACT here, not approximate, so the band is 5 s rather
    // than the delivered 1080 s. A band of 5% passed a run that was 29%
    // short (see the ROUTING_STEP note in write_deck).
    const double shift = src.age_link[kC5].back() - base.age_link[kC5].back();
    EXPECT_NEAR(shift, 21600.0, 5.0)
        << "an EXTERNAL_INFLOW initial age of 6 h did not shift the "
           "steady effluent age by 6 h — the per-source wiring "
           "(addAgeVolume → age-volume load) is wrong or dead.";
}

// ---------------------------------------------------------------------------
// Gate 3 — level pool: aging is exact, mixing equal ages is the identity.
// ---------------------------------------------------------------------------
TEST(WaterAgeTest, LevelPoolAgingIsExact) {
    write_file("_a1_lp.age",
               "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 1.0\n");
    write_deck("_a1_lp.inp",
               "org.hydrocouple.openswmm.waterage config=\"_a1_lp.age\"",
               true, false, /*stagnant=*/true);
    const auto rec = run_recording("_a1_lp.inp", "_a1_lp.rpt", "_a1_lp.out");
    ASSERT_TRUE(rec.ok);
    ASSERT_FALSE(rec.age_link[kC3].empty());

    // 2-minute horizon: age = 3600 + 120 everywhere (zero flow, equal
    // ages ⇒ mixing is the identity; aging is the exact +dt integral).
    const double expected = 3600.0 + 120.0;
    EXPECT_NEAR(rec.age_link[kC3].back(), expected, 1.0e-3 * expected);
    ASSERT_FALSE(rec.age_node_final.empty());
    EXPECT_NEAR(rec.age_node_final[1], expected, 1.0e-3 * expected)
        << "node store age did not advance by exactly dt per step";
}

// ---------------------------------------------------------------------------
// Gate 4 — the symmetric-row razor: age ON leaves TSS and MSX bitwise.
// ---------------------------------------------------------------------------
TEST(WaterAgeTest, AgeRowLeavesOtherRowsBitwise) {
    write_file("_a1_bw.rxn",
               "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
               "[REACTION_SPECIES]\nBULK X MG\n"
               "[REACTION_PIPES]\nRATE X -0.001 * X\n"
               "[REACTION_TANKS]\nRATE X -0.001 * X\n"
               "[REACTION_QUALITY]\nGLOBAL X 8\n");
    const std::string pc =
        "org.hydrocouple.openswmm.reactions config=\"_a1_bw.rxn\"";
    write_deck("_a1_bw_off.inp", pc, /*water_age=*/false, /*pollutants=*/true);
    write_deck("_a1_bw_on.inp",  pc, /*water_age=*/true,  /*pollutants=*/true);
    const auto off = run_recording("_a1_bw_off.inp", "_a1_bw_off.rpt",
                                   "_a1_bw_off.out");
    const auto on  = run_recording("_a1_bw_on.inp", "_a1_bw_on.rpt",
                                   "_a1_bw_on.out");
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(on.ok);
    for (int l = 0; l < 5; ++l) {
        const auto ul = static_cast<std::size_t>(l);
        ASSERT_EQ(off.tss_link[ul].size(), on.tss_link[ul].size());
        for (std::size_t t = 0; t < off.tss_link[ul].size(); ++t) {
            ASSERT_EQ(off.tss_link[ul][t], on.tss_link[ul][t])
                << "link " << l << " step " << t
                << ": WATER_AGE ON changed a POLLUTANT trajectory";
            ASSERT_EQ(off.msx_link[ul][t], on.msx_link[ul][t])
                << "link " << l << " step " << t
                << ": WATER_AGE ON changed an MSX trajectory — check the "
                   "reactArdStage ns_total guard (an equality check would "
                   "silently skip ALL reactions with the age row present)";
        }
    }
    // And the age row itself must be ALIVE in the ON run (reacting deck).
    ASSERT_FALSE(on.age_link[kC5].empty());
    EXPECT_GT(on.age_link[kC5].back(), 0.0);
}

// ---------------------------------------------------------------------------
// Gate 5 — config errors are precise; a failed apply never half-applies.
// ---------------------------------------------------------------------------
TEST(WaterAgeTest, ConfigErrorsArePrecise) {
    struct Case { const char* tag; const char* body; const char* needle; };
    const Case cases[] = {
        {"_a1_e_ts", "[WATER_AGE_SOURCES]\nGW GLOBAL TIMESERIES ts1\n",
         "TIMESERIES ages arrive with a later"},
        {"_a1_e_sc", "[WATER_AGE_SOURCES]\nGW SUBCATCH S1 10\n",
         "SUBCATCH scope arrives with plan phase A3"},
        {"_a1_e_uk", "[WATER_AGE_SOURCES]\nMAGIC GLOBAL 1\n",
         "unknown source 'MAGIC'"},
        {"_a1_e_ns", "[WATER_AGE_SOURCES]\nGW NODE J0 10\n",
         "NODE scope applies to DWF and EXTERNAL_INFLOW"},
        {"_a1_e_un", "[WATER_AGE_SOURCES]\nDWF NODE NOPE 10\n",
         "unknown node 'NOPE'"},
        {"_a1_e_bad", "[WATER_AGE_SOURCES]\nGW GLOBAL -1\n",
         "not a non-negative age"},
        // The spelling plan §2 actually documents — the series NAME makes
        // the row one column wider than a constant row. Checking arity
        // first reported "malformed row" here and left the TIMESERIES
        // deferral unreachable in the only form a user would write.
        {"_a1_e_tsn",
         "[WATER_AGE_SOURCES]\nEXTERNAL_INFLOW NODE J0 TIMESERIES age_ts\n",
         "TIMESERIES ages arrive with a later"},
        // A NODE row with the age omitted. The value token used to be bound
        // as toks[3] BEFORE the arity check, reading one past the end of a
        // 3-token row.
        {"_a1_e_short", "[WATER_AGE_SOURCES]\nDWF NODE J0\n",
         "malformed row"},
    };
    for (const auto& c : cases) {
        write_file((std::string(c.tag) + ".age").c_str(), c.body);
        write_deck((std::string(c.tag) + ".inp").c_str(),
                   std::string("org.hydrocouple.openswmm.waterage config=\"") +
                       c.tag + ".age\"");
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        EXPECT_NE(swmm_engine_open(e, (std::string(c.tag) + ".inp").c_str(),
                                   (std::string(c.tag) + ".rpt").c_str(),
                                   (std::string(c.tag) + ".out").c_str(),
                                   nullptr),
                  SWMM_OK)
            << c.tag;
        auto& ctx = as_cpp_engine(e).context();
        bool found = false;
        for (const auto& err : ctx.errors)
            if (err.find(c.needle) != std::string::npos) found = true;
        EXPECT_TRUE(found) << c.tag << ": no error contains '" << c.needle
                           << "'";
        EXPECT_FALSE(ctx.water_age_config.configured)
            << c.tag << ": a failed apply half-applied";
        swmm_engine_destroy(e);
    }
}

// ---------------------------------------------------------------------------
// Gate 6 — bypass warnings, both directions (lessons 10/20).
// ---------------------------------------------------------------------------
TEST(WaterAgeTest, BypassConfigurationsWarn) {
    {   // component present, option OFF
        write_file("_a1_off.age", "[WATER_AGE_SOURCES]\nGW GLOBAL 720\n");
        write_deck("_a1_off.inp",
                   "org.hydrocouple.openswmm.waterage config=\"_a1_off.age\"",
                   /*water_age=*/false);
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(swmm_engine_open(e, "_a1_off.inp", "_a1_off.rpt",
                                   "_a1_off.out", nullptr),
                  SWMM_OK);
        EXPECT_TRUE(has_needle(as_cpp_engine(e).context().warnings,
                               "WATER_AGE is OFF"))
            << "waterage component with WATER_AGE OFF ran without a word";
        swmm_engine_destroy(e);
    }
    {   // option ON, engine LEGACY — the A1b deferral must be LOUD, and the
        // deck must RUN. WATER_AGE ON is a third way into stepRouting's
        // quality branch, so a LEGACY deck with no [POLLUTANTS] now reaches
        // QualitySolver::execute at np == 0 where it never could before.
        // Opening alone cannot see that; this leg simulates to the end.
        write_deck("_a1_leg.inp", "", /*water_age=*/true, /*pollutants=*/false,
                   false, "QUALITY_SOLVER LEGACY\n");
        const auto rec = run_recording("_a1_leg.inp", "_a1_leg.rpt",
                                       "_a1_leg.out");
        ASSERT_TRUE(rec.ok) << "WATER_AGE ON under LEGACY with no pollutants "
                               "did not survive a full run";
        EXPECT_TRUE(has_needle(rec.warnings, "arrives with plan phase A1b"))
            << "WATER_AGE ON under LEGACY tracked nothing silently";
        // And the warning must be TRUE. Only ArdEngine::init sizes the age
        // state, so under LEGACY every recorded value is run_recording's
        // -1 "unsized" sentinel; a POSITIVE age would mean something ran
        // and the deferral warning lied.
        for (const auto& per_link : rec.age_link)
            for (const double a : per_link)
                EXPECT_LE(a, 0.0)
                    << "LEGACY published a real age while warning that age "
                       "tracking arrives with A1b";
    }
}

// ---------------------------------------------------------------------------
// Gate 7 — saving the model keeps the two keys that select this feature.
// ---------------------------------------------------------------------------
TEST(WaterAgeTest, SaveKeepsWaterAgeAndQualitySolver) {
    // Both keys were dropped by InpWriter: a saved EULERIAN_ARD + WATER_AGE
    // model reopened as LEGACY with age tracking off, silently and with no
    // warning. Round-trip through the writer and re-open the RESULT — the
    // observation path has to be the reopened options, not the text, or a
    // key spelled in a way the parser rejects still passes.
    write_deck("_a1_rt.inp", "");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_a1_rt.inp", "_a1_rt.rpt", "_a1_rt.out",
                               nullptr),
              SWMM_OK);
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(as_cpp_engine(e).context(),
                                                 "_a1_rt_saved.inp", nullptr),
              0);
    swmm_engine_destroy(e);

    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, "_a1_rt_saved.inp", "_a1_rt_saved.rpt",
                               "_a1_rt_saved.out", nullptr),
              SWMM_OK);
    const auto& o = as_cpp_engine(e2).context().options;
    EXPECT_TRUE(o.water_age)
        << "saving the model turned WATER_AGE off";
    EXPECT_EQ(o.quality_solver, openswmm::QualitySolverKind::EULERIAN_ARD)
        << "saving the model reverted QUALITY_SOLVER to LEGACY — with "
           "WATER_AGE ON that deck opens warning that no age is tracked";
    // And the saved deck must NOT have collected the A1b bypass warning,
    // which is what a half-written pair produces.
    EXPECT_FALSE(has_needle(as_cpp_engine(e2).context().warnings,
                            "arrives with plan phase A1b"));
    swmm_engine_destroy(e2);
}

}  // namespace
