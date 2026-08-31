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
 *          - Z1 (amendment D-Y4) gates: `__WATER_AGE__` as an [INFLOWS]
 *            constituent — equivalence with the source table, precedence
 *            (the inflow row wins, warned), TIMESERIES-driven ages, the
 *            silent-drop path replaced by warnings, and writer round-trip.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_hotstart.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_water_age.h>

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
                const std::string& extra_options = "",
                int routing_step = 5, bool dry = false,
                const std::string& extra_inflows = "",
                const std::string& extra_sections = "") {
    std::ofstream f(path);
    f << "[TITLE]\nA1a water age gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "QUALITY_SOLVER EULERIAN_ARD\n"
      << (water_age ? "WATER_AGE ON\n" : "")
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME "
      << (stagnant ? "00:02:00" : "01:00:00") << "\n"
      // A1a pinned this at 1 s because the ARD node store lost
      // external-inflow mass above ROUTING_STEP 2, which dragged gate 2's
      // 6-hour shift down to 15246 s and made the age row look broken when
      // it was faithfully tracking a broken carrier. That defect was fixed
      // in 7b2dfaae (the store now mixes before it discharges), so the pin
      // is retired and these decks run at an ordinary step. Re-measured
      // across rs 1/2/5/10/20/60 after the fix: the ARD shift is
      // 21599.998 -> 21599.9999 and the level pool is 3720.0000 at every
      // step, so nothing here depends on the choice any more.
      << "ROUTING_STEP " << routing_step
      << "\nREPORT_STEP 00:01:00\n"
      << extra_options << "\n";
    if (stagnant) {
        f << "[JUNCTIONS]\n"
          << "J0 10.0 10 1.5 0 0\nJ1 10.0 10 1.5 0 0\n"
          << "J2 10.0 10 1.5 0 0\nJ3 10.0 10 1.5 0 0\n"
          << "J4 10.0 10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 10.0 FIXED 11.5 NO\n\n";
    } else if (dry) {
        // Bone dry: InitDepth 0 everywhere, FREE outfall, and no inflow
        // below. The age STATE still advances here (cells are seeded and
        // aged regardless of wetness), which is precisely what makes this
        // deck the observer for the state/report separation.
        f << "[JUNCTIONS]\n"
          << "J0 10.0 10 0 0 0\nJ1 9.4  10 0 0 0\nJ2 8.8  10 0 0 0\n"
          << "J3 8.2  10 0 0 0\nJ4 7.6  10 0 0 0\n\n"
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n";
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
    if (!stagnant && !dry)
        f << "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n"
          << extra_inflows << "\n";
    if (!extra_sections.empty()) f << extra_sections << "\n";
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
        // Z1 (amendment D-Y4) FLIP: the A1a deferral ("arrive with a
        // later water-age phase") became a REDIRECT — time-varying ages
        // are now prescribed as [INFLOWS] rows naming __WATER_AGE__, and
        // this table's refusal must say exactly where to go instead.
        {"_a1_e_ts", "[WATER_AGE_SOURCES]\nGW GLOBAL TIMESERIES ts1\n",
         "prescribed as [INFLOWS] rows naming __WATER_AGE__"},
        {"_a1_e_sc", "[WATER_AGE_SOURCES]\nGW SUBCATCH S1 10\n",
         "SUBCATCH scope arrives with plan phase A3"},
        {"_a1_e_uk", "[WATER_AGE_SOURCES]\nMAGIC GLOBAL 1\n",
         "unknown source 'MAGIC'"},
        {"_a1_e_ns", "[WATER_AGE_SOURCES]\nGW NODE J0 10\n",
         "NODE scope applies to DWF and EXTERNAL_INFLOW"},
        {"_a1_e_un", "[WATER_AGE_SOURCES]\nDWF NODE NOPE 10\n",
         "unknown node 'NOPE'"},
        // D-NS1 (X6) FLIP: "GW GLOBAL -1" is no longer an ERROR — a
        // negative source age extracts age-volume and parses with a
        // warning instead. The refusal row keeps this table's shape with a
        // genuinely non-numeric value; the negative-row WARNING has its
        // own gate in test_negative_sources.cpp.
        {"_a1_e_bad", "[WATER_AGE_SOURCES]\nGW GLOBAL abc\n",
         "is not an age in hours"},
        // The spelling plan §2 actually documents — the series NAME makes
        // the row one column wider than a constant row. Checking arity
        // first reported "malformed row" here and left the TIMESERIES
        // deferral unreachable in the only form a user would write.
        {"_a1_e_tsn",
         "[WATER_AGE_SOURCES]\nEXTERNAL_INFLOW NODE J0 TIMESERIES age_ts\n",
         "prescribed as [INFLOWS] rows naming __WATER_AGE__"},
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
    {   // A1b FLIP (lesson 21, in the retiring changeset): LEGACY now runs
        // its own age mirror, so the deferral warning must be GONE and the
        // pure-age LEGACY deck must TRACK — the positive coverage this leg
        // used to defer to.
        write_deck("_a1_leg.inp", "", /*water_age=*/true, /*pollutants=*/false,
                   false, "QUALITY_SOLVER LEGACY\n");
        const auto rec = run_recording("_a1_leg.inp", "_a1_leg.rpt",
                                       "_a1_leg.out");
        ASSERT_TRUE(rec.ok) << "WATER_AGE ON under LEGACY with no pollutants "
                               "did not survive a full run";
        EXPECT_FALSE(has_needle(rec.warnings, "arrives with plan phase A1b"))
            << "the retired A1b deferral warning still fires";
        ASSERT_FALSE(rec.age_link.empty());
        ASSERT_FALSE(rec.age_link[kC5].empty());
        EXPECT_GT(rec.age_link[kC5].back(), 0.0)
            << "LEGACY age mirror is dead on the pure-age deck (the A1b "
               "motivating configuration)";
    }
}

// ---------------------------------------------------------------------------
// Gates 8–10 — A1b: the LEGACY CSTR age mirror.
// ---------------------------------------------------------------------------
TEST(WaterAgeTest, LegacySourceAgeShiftsEffluentExactly) {
    // The mixing algebra is linear under the CSTR mirror too: an
    // EXTERNAL_INFLOW age of 6 h shifts the steady effluent age by exactly
    // 21600 s (same reasoning as gate 2, LEGACY engine).
    write_deck("_a1b_base.inp", "", true, false, false,
               "QUALITY_SOLVER LEGACY\n");
    write_file("_a1b_src.age",
               "[WATER_AGE_SOURCES]\nEXTERNAL_INFLOW NODE J0 6.0\n");
    write_deck("_a1b_src.inp",
               "org.hydrocouple.openswmm.waterage config=\"_a1b_src.age\"",
               true, false, false, "QUALITY_SOLVER LEGACY\n");
    const auto base = run_recording("_a1b_base.inp", "_a1b_base.rpt",
                                    "_a1b_base.out");
    const auto src  = run_recording("_a1b_src.inp", "_a1b_src.rpt",
                                    "_a1b_src.out");
    ASSERT_TRUE(base.ok);
    ASSERT_TRUE(src.ok);
    ASSERT_FALSE(base.age_link[kC5].empty());
    const double shift =
        src.age_link[kC5].back() - base.age_link[kC5].back();
    EXPECT_NEAR(shift, 21600.0, 0.05 * 21600.0)
        << "the LEGACY mirror's per-source wiring is wrong or dead";
}

TEST(WaterAgeTest, LegacyLevelPoolAgingIsExact) {
    write_file("_a1b_lp.age",
               "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 1.0\n");
    write_deck("_a1b_lp.inp",
               "org.hydrocouple.openswmm.waterage config=\"_a1b_lp.age\"",
               true, false, /*stagnant=*/true, "QUALITY_SOLVER LEGACY\n");
    const auto rec = run_recording("_a1b_lp.inp", "_a1b_lp.rpt",
                                   "_a1b_lp.out");
    ASSERT_TRUE(rec.ok);
    ASSERT_FALSE(rec.age_link[kC3].empty());
    // Symmetric with ARD's gate 3: INITIAL_STATE seeds on the mirror's
    // first step, then aging is the exact +dt integral and mixing of
    // equal ages is the identity.
    const double expected = 3600.0 + 120.0;
    EXPECT_NEAR(rec.age_link[kC3].back(), expected, 1.0e-3 * expected)
        << "LEGACY aging is not the exact +dt integral";
    ASSERT_FALSE(rec.age_node_final.empty());
    EXPECT_NEAR(rec.age_node_final[1], expected, 1.0e-3 * expected);
}

TEST(WaterAgeTest, LegacyAgeLeavesPollutantsBitwise) {
    // The mirror reads pollutant-side arrays and writes only
    // water_age_state: WATER_AGE ON must leave TSS trajectories
    // bit-identical under LEGACY.
    write_deck("_a1b_bw_off.inp", "", /*water_age=*/false,
               /*pollutants=*/true, false, "QUALITY_SOLVER LEGACY\n");
    write_deck("_a1b_bw_on.inp", "", /*water_age=*/true,
               /*pollutants=*/true, false, "QUALITY_SOLVER LEGACY\n");
    const auto off = run_recording("_a1b_bw_off.inp", "_a1b_bw_off.rpt",
                                   "_a1b_bw_off.out");
    const auto on  = run_recording("_a1b_bw_on.inp", "_a1b_bw_on.rpt",
                                   "_a1b_bw_on.out");
    ASSERT_TRUE(off.ok);
    ASSERT_TRUE(on.ok);
    for (int l = 0; l < 5; ++l) {
        const auto ul = static_cast<std::size_t>(l);
        ASSERT_EQ(off.tss_link[ul].size(), on.tss_link[ul].size());
        for (std::size_t t = 0; t < off.tss_link[ul].size(); ++t)
            ASSERT_EQ(off.tss_link[ul][t], on.tss_link[ul][t])
                << "link " << l << " step " << t
                << ": WATER_AGE ON changed a LEGACY pollutant trajectory";
    }
}

// ---------------------------------------------------------------------------
// A2a — hotstart persistence: save/load round trip + restart continuity.
// ---------------------------------------------------------------------------
namespace {
/// Run a deck, save a V3 hotstart at end, and return the final ages.
struct HsRun {
    std::vector<double> node_age, link_age;
    bool ok = false;
};
HsRun run_and_save(const char* inp, const char* hs_path) {
    HsRun out;
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) { ADD_FAILURE() << "create"; return out; }
    bool ok = swmm_engine_open(e, inp, "_a2_hs.rpt", "_a2_hs.out", nullptr) ==
                  SWMM_OK &&
              swmm_engine_initialize(e) == SWMM_OK &&
              swmm_engine_start(e, 1) == SWMM_OK;
    if (ok) {
        double elapsed = 0.0;
        int guard = 0;
        do {
            if (swmm_engine_step(e, &elapsed) != SWMM_OK) { ok = false; break; }
        } while (elapsed > 0.0 && ++guard < 20000);
        if (ok) {
            swmm_engine_end(e);
            ok = swmm_hotstart_save(e, hs_path) == SWMM_OK;
            out.node_age = as_cpp_engine(e).context().water_age_state.node_age;
            out.link_age = as_cpp_engine(e).context().water_age_state.link_age;
        }
    }
    if (!ok) ADD_FAILURE() << "run_and_save failed for " << inp;
    swmm_engine_destroy(e);
    out.ok = ok;
    return out;
}
}  // namespace

TEST(WaterAgeTest, HotstartRoundTripsAgeAcrossBothEngines) {
    // INITIAL_STATE is a DISCRIMINATOR here, not decoration: 100 h. If the
    // restart seeded from INITIAL_STATE instead of the loaded state, ages
    // jump to 360000 s; the loaded ages are O(10²–10³) s. The same deck
    // shape runs under both engines.
    write_file("_a2_hs.age",
               "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 100.0\n");
    const std::string pc =
        "org.hydrocouple.openswmm.waterage config=\"_a2_hs.age\"";

    for (const bool legacy : {false, true}) {
        const char* inp = legacy ? "_a2_hs_leg.inp" : "_a2_hs_ard.inp";
        const char* hsf = legacy ? "_a2_hs_leg.bin" : "_a2_hs_ard.bin";
        write_deck(inp, pc, true, false, false,
                   legacy ? "QUALITY_SOLVER LEGACY\n" : "");
        const auto saved = run_and_save(inp, hsf);
        ASSERT_TRUE(saved.ok);
        ASSERT_FALSE(saved.node_age.empty());
        // The run started from INITIAL_STATE 100 h and flushed for an hour;
        // the saved outfall-adjacent ages must be FAR below 360000 (flushed)
        // and above 0 (liveness) or the discriminator has no teeth.
        ASSERT_GT(saved.link_age[kC5], 0.0);
        ASSERT_LT(saved.link_age[kC5], 200000.0)
            << (legacy ? "LEGACY" : "ARD")
            << ": the run never flushed the 100-h initial age — the "
               "discriminator is dead on this deck";

        // Round trip: open a FRESH engine on the same deck, apply the file,
        // and compare the restored state BIT-FOR-BIT (double → double).
        SWMM_Engine e2 = swmm_engine_create();
        ASSERT_NE(e2, nullptr);
        ASSERT_EQ(swmm_engine_open(e2, inp, "_a2_hs2.rpt", "_a2_hs2.out",
                                   nullptr),
                  SWMM_OK);
        // swmm_hotstart_apply requires EngineState::INITIALIZED — it
        // returns SWMM_ERR_LIFECYCLE from OPENED. initialize() must
        // therefore run BEFORE the apply, not after, and the apply's
        // writes are what survive into the run. (Delivered as
        // open -> apply -> initialize, which returned 6 and made both
        // hotstart gates exit before a single age assertion.)
        ASSERT_EQ(swmm_engine_initialize(e2), SWMM_OK);
        SWMM_HotStart hs = nullptr;
        ASSERT_EQ(swmm_hotstart_open(hsf, &hs), SWMM_OK);
        ASSERT_EQ(swmm_hotstart_apply(e2, hs), SWMM_OK);
        swmm_hotstart_close(hs);
        {
            const auto& ws = as_cpp_engine(e2).context().water_age_state;
            ASSERT_EQ(ws.node_age.size(), saved.node_age.size());
            for (std::size_t i = 0; i < saved.node_age.size(); ++i)
                ASSERT_EQ(ws.node_age[i], saved.node_age[i])
                    << "node " << i << " age did not round-trip bitwise";
            for (std::size_t i = 0; i < saved.link_age.size(); ++i)
                ASSERT_EQ(ws.link_age[i], saved.link_age[i])
                    << "link " << i << " age did not round-trip bitwise";
        }

        // Restart continuity: one routing step after the load, the age must
        // CONTINUE from the loaded value — not reset to 0, not jump to the
        // 100-h INITIAL_STATE.
        ASSERT_EQ(swmm_engine_start(e2, 1), SWMM_OK);
        double elapsed = 0.0;
        ASSERT_EQ(swmm_engine_step(e2, &elapsed), SWMM_OK);
        const auto& ws2 = as_cpp_engine(e2).context().water_age_state;
        const double a = ws2.link_age[kC5];
        // A band of (0.25*saved, 200000) separates "loaded" from "reset to
        // zero" and from "re-seeded at 360000" and nothing else — it would
        // pass a restart that restored a quarter of the state. Measured
        // continuity is much tighter: ARD 876.447 -> 837.909 (-4.4%) and
        // LEGACY 964.078 -> 948.457 (-1.6%) one step after the load.
        //
        // ARD's larger drop is structural and expected: the hotstart record
        // carries ONE age per link while the ARD mesh carries one per CELL,
        // so a save collapses the within-link age profile and the load
        // re-uniformizes it. 10% leaves 2.3x headroom over that without
        // admitting a partially restored state.
        EXPECT_NEAR(a, saved.link_age[kC5], 0.10 * saved.link_age[kC5])
            << (legacy ? "LEGACY" : "ARD")
            << ": one step after the restart the age is " << a
            << " against a loaded " << saved.link_age[kC5]
            << ". Near zero means the restart lost the state; near 360000 "
               "means it re-seeded from INITIAL_STATE.";
        swmm_engine_destroy(e2);
    }
}

TEST(WaterAgeTest, PreV3HotstartFallsBackToInitialState) {
    // A file saved WITHOUT water age (V1/V2) applied to a WATER_AGE ON
    // model: the -1 sentinel must fall through to INITIAL_STATE seeding —
    // no crash, and the 100-h seed must be VISIBLE after one step.
    write_deck("_a2_v2.inp", "", /*water_age=*/false);
    const auto v2 = run_and_save("_a2_v2.inp", "_a2_v2.bin");
    ASSERT_TRUE(v2.ok);

    write_file("_a2_v2on.age",
               "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 100.0\n");
    write_deck("_a2_v2on.inp",
               "org.hydrocouple.openswmm.waterage config=\"_a2_v2on.age\"");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_a2_v2on.inp", "_a2_v2on.rpt",
                               "_a2_v2on.out", nullptr),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);   // apply needs INITIALIZED
    SWMM_HotStart hs = nullptr;
    ASSERT_EQ(swmm_hotstart_open("_a2_v2.bin", &hs), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_apply(e, hs), SWMM_OK);
    swmm_hotstart_close(hs);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);
    const auto& ws = as_cpp_engine(e).context().water_age_state;
    ASSERT_FALSE(ws.link_age.empty());
    EXPECT_GT(ws.link_age[kC3], 300000.0)
        << "a pre-V3 file suppressed INITIAL_STATE seeding (the -1 "
           "sentinel path is broken)";
    swmm_engine_destroy(e);

    // The LEGACY mirror reaches INITIAL_STATE by a DIFFERENT route — apply()
    // leaves legacy_seeded false so routeLegacyAge fills on its first step,
    // where the ARD engine seeds in init(). One leg cannot cover both.
    write_deck("_a2_v2on_leg.inp",
               "org.hydrocouple.openswmm.waterage config=\"_a2_v2on.age\"",
               true, false, false, "QUALITY_SOLVER LEGACY\n");
    SWMM_Engine el = swmm_engine_create();
    ASSERT_NE(el, nullptr);
    ASSERT_EQ(swmm_engine_open(el, "_a2_v2on_leg.inp", "_a2_v2on_leg.rpt",
                               "_a2_v2on_leg.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(el), SWMM_OK);
    SWMM_HotStart hs2 = nullptr;
    ASSERT_EQ(swmm_hotstart_open("_a2_v2.bin", &hs2), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_apply(el, hs2), SWMM_OK);
    swmm_hotstart_close(hs2);
    ASSERT_EQ(swmm_engine_start(el, 1), SWMM_OK);
    double el2 = 0.0;
    ASSERT_EQ(swmm_engine_step(el, &el2), SWMM_OK);
    const auto& wl = as_cpp_engine(el).context().water_age_state;
    ASSERT_FALSE(wl.link_age.empty());
    EXPECT_GT(wl.link_age[kC3], 300000.0)
        << "pre-V3 file under LEGACY suppressed INITIAL_STATE seeding — "
           "apply() must leave legacy_seeded false when no age was loaded";
    swmm_engine_destroy(el);
}

// ---------------------------------------------------------------------------
// Gates 11-12 — what the delivered A1b gates cannot see.
// ---------------------------------------------------------------------------
TEST(WaterAgeTest, LegacySourceShiftIsRoutingStepInvariant) {
    // The A1b mirror is a STORE-based scheme, which is the family the ARD
    // node store belonged to when it discharged at a concentration read
    // before the step's inflow mixed in (fixed in 7b2dfaae). That defect's
    // signature is loss of step invariance: it read exactly right at small
    // steps and sagged as the step grew, so a gate at ONE routing step
    // cannot see it. Gate 8 checks the 6-hour shift at the deck's step
    // only; this sweeps it.
    //
    // Linearity makes the shift exactly 21600 s regardless of step size,
    // even though the ABSOLUTE age is step-dependent under this scheme
    // (measured: the outfall age carries a clean O(dt) splitting bias of
    // one dt per element crossed). The shift cancels that bias, which is
    // what makes it the right invariant to gate.
    write_file("_a1b_inv.age",
               "[WATER_AGE_SOURCES]\nEXTERNAL_INFLOW NODE J0 6.0\n");
    for (const int rs : {1, 5, 20}) {
        write_deck("_a1b_inv_b.inp", "", true, false, false,
                   "QUALITY_SOLVER LEGACY\n", rs);
        write_deck("_a1b_inv_s.inp",
                   "org.hydrocouple.openswmm.waterage config=\"_a1b_inv.age\"",
                   true, false, false, "QUALITY_SOLVER LEGACY\n", rs);
        const auto b = run_recording("_a1b_inv_b.inp", "_a1b_inv_b.rpt",
                                     "_a1b_inv_b.out");
        const auto s2 = run_recording("_a1b_inv_s.inp", "_a1b_inv_s.rpt",
                                      "_a1b_inv_s.out");
        ASSERT_TRUE(b.ok) << "rs=" << rs;
        ASSERT_TRUE(s2.ok) << "rs=" << rs;
        ASSERT_FALSE(b.age_link[kC5].empty()) << "rs=" << rs;
        const double shift = s2.age_link[kC5].back() - b.age_link[kC5].back();
        EXPECT_NEAR(shift, 21600.0, 10.0)
            << "ROUTING_STEP " << rs << ": the 6-hour source shift came out "
            << shift << " s. Linearity makes it step-independent, so a shift "
               "that moves with the step means the mirror's ordering has "
               "come apart the way the ARD store's did.";
    }
}

TEST(WaterAgeTest, LegacyOutfallAgeConvergesToResidenceTime) {
    // The strongest correctness claim available for a steady system: the
    // mean age of water LEAVING it is exactly V/Q. No gate asserted this
    // for the mirror, and it is the one that says the scheme is RIGHT
    // rather than merely self-consistent.
    //
    // The outfall NODE is the common quantity across schemes. A LINK's
    // published age is not: LEGACY's fully-mixed tank publishes its outlet
    // value while ARD publishes the volume-weighted mean over the link's
    // cells, so the two differ by definition and comparing them measures
    // nothing (measured 6.5% apart on this deck, which is that definitional
    // gap, not an error in either).
    //
    // Measured deviation from V/Q under LEGACY: +4.504 s at ROUTING_STEP 1
    // and +24.504 at 5 — exactly 5*dt - 0.5, i.e. one spurious dt per
    // element a parcel crosses. It extrapolates to zero, so the mirror is
    // EXACT in the limit and carries a clean O(dt) operator-splitting bias
    // (age-then-mix at every element). The band below is set from that law
    // rather than from a single observation.
    write_deck("_a1b_rt.inp", "", true, false, false,
               "QUALITY_SOLVER LEGACY\n", 1);
    const auto rec = run_recording("_a1b_rt.inp", "_a1b_rt.rpt",
                                   "_a1b_rt.out");
    ASSERT_TRUE(rec.ok);
    ASSERT_FALSE(rec.age_node_final.empty());
    const double outfall  = rec.age_node_final.back();
    const double residence = rec.system_volume_final / kQ;
    ASSERT_GT(residence, 0.0);
    ASSERT_GT(outfall, 0.0) << "the outfall never aged — mirror dead";
    // At ROUTING_STEP 1 the splitting bias is ~5 s on a ~930 s residence
    // time. 3% leaves room for it and for the deck's hydraulics without
    // admitting a scheme that has lost the theorem.
    EXPECT_NEAR(outfall, residence, 0.03 * residence)
        << "LEGACY outfall age " << outfall << " s vs V/Q " << residence
        << " s — a steady system's outflow age IS its residence time; this "
           "much deviation means the mirror is not conserving age-volume.";
}

TEST(WaterAgeTest, LegacySplittingBiasIsOneStepPerElement) {
    // The lesson-32 gate. The mirror ages an element by dt and then mixes,
    // so a parcel crossing N elements picks up an O(dt) splitting bias.
    // Measured on this 5-conduit chain the outfall age is exactly
    // V/Q + 5*dt - 0.5: one routing step per element, extrapolating to the
    // residence-time theorem at dt -> 0.
    //
    // The ARD node store's defect was reading its donor BEFORE mixing in
    // what arrived that step. Written into this mirror (take the link's
    // upstream age from the PRE-mixing node value) the bias becomes exactly
    // 10*dt — two steps per element instead of one — while every other
    // gate stays green: the 6-hour SHIFT cancels the bias, which is exactly
    // what makes the shift the right invariant elsewhere and blind here.
    // Only the slope of the absolute bias separates the two orderings.
    double bias[2];
    const int steps[2] = {1, 5};
    for (int k = 0; k < 2; ++k) {
        write_deck("_a1b_sl.inp", "", true, false, false,
                   "QUALITY_SOLVER LEGACY\n", steps[k]);
        const auto rec = run_recording("_a1b_sl.inp", "_a1b_sl.rpt",
                                       "_a1b_sl.out");
        ASSERT_TRUE(rec.ok) << "rs=" << steps[k];
        ASSERT_FALSE(rec.age_node_final.empty());
        ASSERT_GT(rec.system_volume_final, 0.0);
        bias[k] = rec.age_node_final.back() - rec.system_volume_final / kQ;
    }
    // Per routing step, per element crossed. Five conduits on this deck.
    const double per_element =
        (bias[1] - bias[0]) / (steps[1] - steps[0]) / 5.0;
    EXPECT_GT(per_element, 0.5)
        << "the splitting bias vanished (" << per_element
        << " dt/element) — aging is not running at all, or the outfall is "
           "not being written";
    EXPECT_LT(per_element, 1.5)
        << "the splitting bias is " << per_element
        << " routing steps per element, not one. Two per element is the "
           "signature of the link stage reading its upstream node age "
           "BEFORE that node mixed in this step's inflow — the ARD node "
           "store's defect (7b2dfaae) written into the mirror.";
}

TEST(WaterAgeTest, IgnoreQualityWarnsWithoutAWaterageComponent) {
    // A1b MOVED this warning: it used to live in the waterage component's
    // apply hook, so it could only fire for decks that configured one. It
    // now fires engine-level, which is the point of the move — WATER_AGE ON
    // needs no component. Nothing observed it, and a warning that changes
    // homes is exactly when one goes missing.
    write_deck("_a1b_iq.inp", "", /*water_age=*/true, /*pollutants=*/true,
               false, "IGNORE_QUALITY YES\n");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_a1b_iq.inp", "_a1b_iq.rpt",
                               "_a1b_iq.out", nullptr), SWMM_OK);
    EXPECT_TRUE(has_needle(as_cpp_engine(e).context().warnings,
                           "IGNORE_QUALITY is YES"))
        << "WATER_AGE ON with IGNORE_QUALITY YES and no waterage component "
           "tracked nothing without a word — the engine-level move lost the "
           "warning the component used to carry";
    swmm_engine_destroy(e);
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

// ---------------------------------------------------------------------------
// The state/report separation, observed at last.
//
// `584d1065` masks the REPORTED age of a dry element to 0 while leaving the
// aged value in `water_age_state`. Its validation found that masking the
// STATE instead passes the entire suite — 140/141, identical to clean — so
// the separation was a design claim with no observer.
//
// Only ONE consequence of the separation is real (the "refilling pipe would
// jump" half of the original rationale does not survive: the stale state
// occupies 0.0107 ft³ against 1263 ft³ arriving, ~1e-5 influence). That
// consequence is HOTSTART FIDELITY: a save taken while an element is dry
// must carry the aged value forward, because the restart may refill it.
//
// So: run a bone-dry deck, save, and assert the SAVED state is non-zero and
// round-trips. Masking the state instead of the report zeroes the saved
// value and fails here.
// ---------------------------------------------------------------------------
TEST(WaterAgeTest, DryElementHotstartCarriesTheAgedState) {
    write_file("_a2_dry.age",
               "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 6.0\n");
    write_deck("_a2_dry.inp",
               "org.hydrocouple.openswmm.waterage config=\"_a2_dry.age\"",
               /*water_age=*/true, /*pollutants=*/false, /*stagnant=*/false,
               /*extra_options=*/"", /*routing_step=*/5, /*dry=*/true);

    const auto saved = run_and_save("_a2_dry.inp", "_a2_dry.bin");
    ASSERT_TRUE(saved.ok);
    ASSERT_FALSE(saved.link_age.empty());

    // The discriminator. INITIAL_STATE 6 h = 21600 s, plus an hour of
    // aging: the STATE must hold ~25200 s even though every element is dry
    // and the .out column reports 0. If the state were masked instead of
    // the report, this reads 0 and the gate fails — which is the whole
    // point of the gate.
    EXPECT_GT(saved.link_age[kC5], 21600.0)
        << "a dry link's SAVED age is " << saved.link_age[kC5]
        << " s — the aged state was not carried, so a restart would lose "
           "it. (The .out column reporting 0 for this element is correct "
           "and separate.)";

    // And it must survive the round trip, like any other age (A2a).
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, "_a2_dry.inp", "_a2_dry2.rpt",
                               "_a2_dry2.out", nullptr),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e2), SWMM_OK);
    SWMM_HotStart hs = nullptr;
    ASSERT_EQ(swmm_hotstart_open("_a2_dry.bin", &hs), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_apply(e2, hs), SWMM_OK);
    swmm_hotstart_close(hs);
    {
        const auto& ws = as_cpp_engine(e2).context().water_age_state;
        ASSERT_EQ(ws.link_age.size(), saved.link_age.size());
        for (std::size_t i = 0; i < saved.link_age.size(); ++i)
            EXPECT_EQ(ws.link_age[i], saved.link_age[i])
                << "dry link " << i << " age did not round-trip";
    }
    swmm_engine_destroy(e2);
}


// ===========================================================================
// Z1 (amendment D-Y4) — reserved species as [INFLOWS] constituents.
// ===========================================================================

// Z1 gate 1 — equivalence: an [INFLOWS] __WATER_AGE__ row with baseline
// 6 h is the SAME statement as the source table's EXTERNAL_INFLOW 6 h.
// Same loader, same q·age, same arithmetic — the trajectories must agree
// exactly, and the shift against the clean base run is gate 2's razor.
TEST(WaterAgeTest, InflowAgeRowMatchesSourceTableExactly) {
    write_deck("_z1_base.inp", "");
    write_file("_z1_tab.age",
               "[WATER_AGE_SOURCES]\nEXTERNAL_INFLOW NODE J0 6.0\n");
    write_deck("_z1_tab.inp",
               "org.hydrocouple.openswmm.waterage config=\"_z1_tab.age\"");
    write_deck("_z1_row.inp", "", true, false, false, "", 5, false,
               "J0 __WATER_AGE__ \"\" CONCEN 1.0 1.0 6.0\n");
    const auto base = run_recording("_z1_base.inp", "_z1_base.rpt",
                                    "_z1_base.out");
    const auto tab  = run_recording("_z1_tab.inp", "_z1_tab.rpt",
                                    "_z1_tab.out");
    const auto row  = run_recording("_z1_row.inp", "_z1_row.rpt",
                                    "_z1_row.out");
    ASSERT_TRUE(base.ok);
    ASSERT_TRUE(tab.ok);
    ASSERT_TRUE(row.ok);
    ASSERT_FALSE(row.age_link[kC5].empty());

    const double shift =
        row.age_link[kC5].back() - base.age_link[kC5].back();
    EXPECT_NEAR(shift, 21600.0, 5.0)
        << "an [INFLOWS] __WATER_AGE__ row of 6 h did not shift the steady "
           "effluent age by 6 h — the row is dead or mis-scaled (hours vs "
           "seconds).";

    ASSERT_EQ(tab.age_link[kC5].size(), row.age_link[kC5].size());
    for (std::size_t t = 0; t < tab.age_link[kC5].size(); ++t)
        ASSERT_EQ(tab.age_link[kC5][t], row.age_link[kC5][t])
            << "step " << t << ": the [INFLOWS] pathway and the source "
               "table disagree for the identical prescription — the two "
               "must be the same statement through the same loader.";
}

// Z1 gate 2 — precedence: with BOTH stated, the [INFLOWS] row wins (the
// more specific statement, amendment §6), and the conflict is warned
// rather than silently resolved.
TEST(WaterAgeTest, InflowAgeRowWinsOverSourceTable) {
    write_file("_z1_conf.age",
               "[WATER_AGE_SOURCES]\nEXTERNAL_INFLOW NODE J0 2.0\n");
    write_deck("_z1_conf.inp",
               "org.hydrocouple.openswmm.waterage config=\"_z1_conf.age\"",
               true, false, false, "", 5, false,
               "J0 __WATER_AGE__ \"\" CONCEN 1.0 1.0 6.0\n");
    write_deck("_z1_row6.inp", "", true, false, false, "", 5, false,
               "J0 __WATER_AGE__ \"\" CONCEN 1.0 1.0 6.0\n");
    const auto conf = run_recording("_z1_conf.inp", "_z1_conf.rpt",
                                    "_z1_conf.out");
    const auto row6 = run_recording("_z1_row6.inp", "_z1_row6.rpt",
                                    "_z1_row6.out");
    ASSERT_TRUE(conf.ok);
    ASSERT_TRUE(row6.ok);
    ASSERT_FALSE(conf.age_link[kC5].empty());

    EXPECT_EQ(conf.age_link[kC5].back(), row6.age_link[kC5].back())
        << "with both an [INFLOWS] __WATER_AGE__ row (6 h) and a source-"
           "table override (2 h) at the same node, the effluent age is not "
           "the row's — the precedence decided in Z1 does not hold.";
    EXPECT_TRUE(has_needle(conf.warnings, "the inflow row wins"))
        << "the conflict between the two prescriptions was resolved "
           "silently — one of them has to be told it lost.";
}

// Z1 gate 3 — a TIMESERIES drives the age. (a) A flat series at 6 h
// reproduces the baseline-6 h run: the series pathway carries the number.
// (b) A 1 h → 9 h step at 00:30 shifts the effluent on the schedule.
TEST(WaterAgeTest, InflowAgeTimeseriesDrivesTheValue) {
    const std::string ts6 =
        "[TIMESERIES]\n"
        "age6 01/01/2026 00:00 6.0\n"
        "age6 01/01/2026 12:00 6.0\n";
    write_deck("_z1_ts6.inp", "", true, false, false, "", 5, false,
               "J0 __WATER_AGE__ age6 CONCEN 1.0 1.0 0.0\n", ts6);
    write_deck("_z1_b6.inp", "", true, false, false, "", 5, false,
               "J0 __WATER_AGE__ \"\" CONCEN 1.0 1.0 6.0\n");
    const auto ts  = run_recording("_z1_ts6.inp", "_z1_ts6.rpt",
                                   "_z1_ts6.out");
    const auto b6  = run_recording("_z1_b6.inp", "_z1_b6.rpt", "_z1_b6.out");
    ASSERT_TRUE(ts.ok);
    ASSERT_TRUE(b6.ok);
    ASSERT_FALSE(ts.age_link[kC5].empty());
    EXPECT_NEAR(ts.age_link[kC5].back(), b6.age_link[kC5].back(), 1.0e-6)
        << "a flat 6 h TIMESERIES and a 6 h baseline disagree — the series "
           "value is not reaching the age pathway.";

    const std::string tsStep =
        "[TIMESERIES]\n"
        "agestep 01/01/2026 00:00 1.0\n"
        "agestep 01/01/2026 00:29 1.0\n"
        "agestep 01/01/2026 00:30 9.0\n"
        "agestep 01/01/2026 12:00 9.0\n";
    write_deck("_z1_tsstep.inp", "", true, false, false, "", 5, false,
               "J0 __WATER_AGE__ agestep CONCEN 1.0 1.0 0.0\n", tsStep);
    write_deck("_z1_b1.inp", "", true, false, false, "", 5, false,
               "J0 __WATER_AGE__ \"\" CONCEN 1.0 1.0 1.0\n");
    write_deck("_z1_b9.inp", "", true, false, false, "", 5, false,
               "J0 __WATER_AGE__ \"\" CONCEN 1.0 1.0 9.0\n");
    const auto st = run_recording("_z1_tsstep.inp", "_z1_tsstep.rpt",
                                  "_z1_tsstep.out");
    const auto b1 = run_recording("_z1_b1.inp", "_z1_b1.rpt", "_z1_b1.out");
    const auto b9 = run_recording("_z1_b9.inp", "_z1_b9.rpt", "_z1_b9.out");
    ASSERT_TRUE(st.ok);
    ASSERT_TRUE(b1.ok);
    ASSERT_TRUE(b9.ok);
    const auto& stc5 = st.age_link[kC5];
    const auto& b1c5 = b1.age_link[kC5];
    const auto& b9c5 = b9.age_link[kC5];
    ASSERT_GE(stc5.size(), std::size_t(50));
    ASSERT_EQ(stc5.size(), b1c5.size());

    // Before the schedule moves (first 25 min) the two decks are the same
    // deck — the trajectories agree to interpolation noise.
    for (std::size_t t = 0; t < std::size_t(25); ++t)
        ASSERT_NEAR(stc5[t], b1c5[t], 1.0e-6)
            << "step " << t << ": the step-series deck diverged from the "
               "constant deck BEFORE the schedule moved.";
    // After it moves, the effluent shifts on the schedule: by the end the
    // step deck has left 1 h decisively behind and cannot exceed the
    // always-9 h deck.
    EXPECT_GT(stc5.back(), b1c5.back() + 0.5 * 8.0 * 3600.0)
        << "30 min after the series stepped 1 h -> 9 h the effluent age "
           "has not moved — the schedule is not being read.";
    EXPECT_LE(stc5.back(), b9c5.back() + 1.0)
        << "the step deck's effluent is OLDER than an always-9 h run — "
           "the series value is being over-applied.";
}

// Z1 gate 4 — the silent-drop path has words now: a typo'd pollutant, a
// deferred __TEMPERATURE__ row, an inert row under WATER_AGE OFF, and a
// MASS-typed age row all WARN — and the runs still complete.
TEST(WaterAgeTest, InflowConstituentSilentDropReplacedByWarnings) {
    write_deck("_z1_typo.inp", "", true, /*pollutants=*/true, false, "", 5,
               false, "J0 TSSX \"\" CONCEN 1.0 1.0 5.0\n");
    const auto typo = run_recording("_z1_typo.inp", "_z1_typo.rpt",
                                    "_z1_typo.out");
    ASSERT_TRUE(typo.ok);
    EXPECT_TRUE(has_needle(typo.warnings,
                           "matches no pollutant or reserved species"))
        << "a misspelled [INFLOWS] constituent still vanishes silently.";

    write_deck("_z1_temp.inp", "", true, false, false, "", 5, false,
               "J0 __TEMPERATURE__ \"\" CONCEN 1.0 1.0 20.0\n");
    const auto temp = run_recording("_z1_temp.inp", "_z1_temp.rpt",
                                    "_z1_temp.out");
    ASSERT_TRUE(temp.ok);
    EXPECT_TRUE(has_needle(temp.warnings, "arrive with heat's round"))
        << "__TEMPERATURE__ rows must defer with words, not vanish "
           "(amendment §6: temperature keeps the old treatment).";

    write_deck("_z1_off.inp", "", /*water_age=*/false, false, false, "", 5,
               false, "J0 __WATER_AGE__ \"\" CONCEN 1.0 1.0 6.0\n");
    const auto off = run_recording("_z1_off.inp", "_z1_off.rpt",
                                   "_z1_off.out");
    ASSERT_TRUE(off.ok);
    EXPECT_TRUE(has_needle(off.warnings, "WATER_AGE is OFF"))
        << "an age row on a WATER_AGE OFF deck is inert and must say so "
           "(the silent-bypass rule, lessons 10/20).";

    write_deck("_z1_mbase.inp", "");
    write_deck("_z1_mass.inp", "", true, false, false, "", 5, false,
               "J0 __WATER_AGE__ \"\" MASS 1.0 1.0 6.0\n");
    const auto mbase = run_recording("_z1_mbase.inp", "_z1_mbase.rpt",
                                     "_z1_mbase.out");
    const auto mass  = run_recording("_z1_mass.inp", "_z1_mass.rpt",
                                     "_z1_mass.out");
    ASSERT_TRUE(mbase.ok);
    ASSERT_TRUE(mass.ok);
    EXPECT_TRUE(has_needle(mass.warnings, "MASS has no meaning"))
        << "a MASS-typed age row was reinterpreted without a word.";
    EXPECT_NEAR(mass.age_link[kC5].back() - mbase.age_link[kC5].back(),
                21600.0, 5.0)
        << "the MASS-typed age row did not land as 6 h (CONCEN "
           "semantics) — the warning promised one thing and the engine "
           "did another.";
}

// Z1 gate 5 — writer round-trip: the row survives swmm_model_write and
// the reopened deck reproduces the run.
TEST(WaterAgeTest, InflowAgeRowRoundTripsThroughTheWriter) {
    write_deck("_z1_rt.inp", "", true, false, false, "", 5, false,
               "J0 __WATER_AGE__ \"\" CONCEN 1.0 1.0 6.0\n");
    {
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        ASSERT_EQ(swmm_engine_open(e, "_z1_rt.inp", "_z1_rt.rpt",
                                   "_z1_rt.out", nullptr),
                  SWMM_OK);
        ASSERT_EQ(swmm_model_write(e, "_z1_rt_w.inp"), SWMM_OK);
        swmm_engine_destroy(e);
    }
    {
        std::ifstream f("_z1_rt_w.inp");
        ASSERT_TRUE(f.good());
        std::string text((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("__WATER_AGE__"), std::string::npos)
            << "the writer dropped the __WATER_AGE__ [INFLOWS] row — the "
               "prescription dies on the first save.";
    }
    const auto orig = run_recording("_z1_rt.inp", "_z1_rt2.rpt",
                                    "_z1_rt2.out");
    const auto rt   = run_recording("_z1_rt_w.inp", "_z1_rt_w.rpt",
                                    "_z1_rt_w.out");
    ASSERT_TRUE(orig.ok);
    ASSERT_TRUE(rt.ok);
    ASSERT_FALSE(orig.age_link[kC5].empty());
    EXPECT_NEAR(rt.age_link[kC5].back(), orig.age_link[kC5].back(), 1.0e-6)
        << "the round-tripped deck runs to a different effluent age.";
}


}  // namespace

// ===========================================================================
// IO3c — the water-age component writes its own config file.
// Until IO3c, model.age DECLINED the ComponentConfigSave hook, so
// swmm_model_write fell back to copying the file the model was read from —
// and every swmm_water_age_set_* edit was silently lost on save (the same
// step-3 loss IO3a/IO3b closed for reactions and heat).
// ===========================================================================

namespace {

std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

SWMM_Engine open_ok(const char* inp, const char* rpt, const char* out) {
    SWMM_Engine e = swmm_engine_create();
    EXPECT_NE(e, nullptr);
    EXPECT_EQ(swmm_engine_open(e, inp, rpt, out, nullptr), SWMM_OK)
        << swmm_get_last_error_msg(e);
    return e;
}

}  // namespace

// Gate 1 (base-FAILING): an API edit survives write → reopen.
TEST(WaterAgeIo3cTest, AgeSourceEditsSurviveASaveAndReopen) {
    write_file("_io3c_edit.age",
               "[WATER_AGE_SOURCES]\n"
               "EXTERNAL_INFLOW GLOBAL 6.0\n"
               "DWF NODE J0 2.5\n");
    write_deck("_io3c_edit.inp",
               "org.hydrocouple.openswmm.waterage config=\"_io3c_edit.age\"");

    SWMM_Engine e = open_ok("_io3c_edit.inp", "_io3c_edit.rpt",
                            "_io3c_edit.out");
    ASSERT_EQ(swmm_water_age_set_global_source(
                  e, SWMM_AGE_SRC_EXTERNAL_INFLOW, 12.0), SWMM_OK);
    ASSERT_EQ(swmm_model_write(e, "_io3c_edit_out.inp"), SWMM_OK)
        << swmm_get_last_error_msg(e);
    swmm_engine_destroy(e);

    e = open_ok("_io3c_edit_out.inp", "_io3c_edit_r.rpt", "_io3c_edit_r.out");
    double hours = -1.0;
    ASSERT_EQ(swmm_water_age_get_global_source(
                  e, SWMM_AGE_SRC_EXTERNAL_INFLOW, &hours), SWMM_OK);
    EXPECT_EQ(hours, 12.0)
        << "the API edit was lost on save — model.age still declines the "
           "ComponentConfigSave hook and the copy fallback restored 6.0";

    // The untouched NODE override rides along unharmed.
    int src = -1, node = -1;
    double oh = 0.0;
    int count = 0;
    ASSERT_EQ(swmm_water_age_override_count(e, &count), SWMM_OK);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(swmm_water_age_get_override(e, 0, &src, &node, &oh), SWMM_OK);
    EXPECT_EQ(src, SWMM_AGE_SRC_DWF);
    EXPECT_EQ(oh, 2.5);
    swmm_engine_destroy(e);
}

// Gate 2 — field-by-field exactness, the invented-row FILE leg, and the
// seconds↔hours fixed point (the reason IO3b deferred this component: the
// config stores SECONDS against a file in HOURS, so the serializer must
// survive ÷3600/×3600 without drift — shortest-exact formatting, and the
// written file must reach a fixed point by the second generation).
TEST(WaterAgeIo3cTest, AgeConfigRoundTripsExactlyAndIdempotently) {
    // 1.23456789 is the %g falsifier: six significant digits truncate it.
    // -0.25 exercises the D-NS1 negative (extraction) spelling. 0.1 is the
    // classic non-representable decimal.
    write_file("_io3c_rt.age",
               "[WATER_AGE_SOURCES]\n"
               "RAINFALL GLOBAL 0.1\n"
               "GW GLOBAL 1.23456789\n"
               "RDII GLOBAL -0.25\n"
               "EXTERNAL_INFLOW NODE J2 7.75\n"
               "DWF NODE J4 0.3\n");
    write_deck("_io3c_rt.inp",
               "org.hydrocouple.openswmm.waterage config=\"_io3c_rt.age\"");

    SWMM_Engine e = open_ok("_io3c_rt.inp", "_io3c_rt.rpt", "_io3c_rt.out");
    ASSERT_EQ(swmm_model_write(e, "_io3c_rt_out.inp"), SWMM_OK);
    swmm_engine_destroy(e);
    const std::string gen1 = slurp("_io3c_rt.age");

    // FILE leg (IO3b lesson 201): sources never configured must not appear —
    // there is no per-source configured flag, so an all-defaults emitter is
    // API-invisible and only the file betrays it.
    EXPECT_EQ(gen1.find("DWF GLOBAL"), std::string::npos);
    EXPECT_EQ(gen1.find("IFACE"), std::string::npos);
    EXPECT_EQ(gen1.find("INITIAL_STATE"), std::string::npos);

    e = open_ok("_io3c_rt_out.inp", "_io3c_rt_r.rpt", "_io3c_rt_r.out");
    double h = 0.0;
    ASSERT_EQ(swmm_water_age_get_global_source(e, SWMM_AGE_SRC_RAINFALL, &h),
              SWMM_OK);
    EXPECT_EQ(h, 0.1);
    ASSERT_EQ(swmm_water_age_get_global_source(e, SWMM_AGE_SRC_GW, &h),
              SWMM_OK);
    EXPECT_EQ(h, 1.23456789) << "shortest-exact formatting regressed (%g?)";
    ASSERT_EQ(swmm_water_age_get_global_source(e, SWMM_AGE_SRC_RDII, &h),
              SWMM_OK);
    EXPECT_EQ(h, -0.25);
    int count = 0;
    ASSERT_EQ(swmm_water_age_override_count(e, &count), SWMM_OK);
    EXPECT_EQ(count, 2);

    // Fixed point: generation 2 == generation 3, byte for byte.
    ASSERT_EQ(swmm_model_write(e, "_io3c_rt_out2.inp"), SWMM_OK);
    swmm_engine_destroy(e);
    const std::string gen2 = slurp("_io3c_rt.age");
    e = open_ok("_io3c_rt_out2.inp", "_io3c_rt_r2.rpt", "_io3c_rt_r2.out");
    ASSERT_EQ(swmm_model_write(e, "_io3c_rt_out3.inp"), SWMM_OK);
    swmm_engine_destroy(e);
    const std::string gen3 = slurp("_io3c_rt.age");
    EXPECT_EQ(gen2, gen3)
        << "the hours↔seconds round trip has no fixed point — successive "
           "saves oscillate";
}
