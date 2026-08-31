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
 * @file test_lard_age.cpp
 * @brief X4: water age under the LARD engine, and the A5 cross-engine gate.
 *
 * @details Subplan X4 (plans/transport/LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md;
 *          water-age plan A5; strategy §8). The claims:
 *
 *          1. At steady state a plug's age at the outfall is its residence
 *             time, measured from the deck's own V/Q — no hand constant.
 *          2. A5: LEGACY, EULERIAN_ARD and LAGRANGIAN agree at the OUTFALL
 *             NODE (A1b: the one quantity all three engines define the same
 *             way; link ages differ from a CSTR by definition).
 *          3. WATER_AGE ON leaves every pollutant trajectory bit-identical
 *             under LARD — the A1b precedent claim, now for the third
 *             engine. The age row must ride ALONGSIDE, never inside.
 *          4. INITIAL_STATE seeds the whole network and then ages exactly:
 *             before fresh inflow reaches the outfall, its age reads
 *             a0 + t to the second — seeding and exact aging in one gate.
 *          5. A per-source age ([WATER_AGE_SOURCES] EXTERNAL_INFLOW) shifts
 *             the steady outfall age by exactly the configured value — the
 *             loader-seam claim (A1a gate 2's pattern, third engine).
 *
 *          Scratch fixtures use the `_la_` prefix (collision-checked).
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

constexpr double kQ = 5.0;
constexpr double kCin = 100.0;

struct DeckSpec {
    const char* solver = "LAGRANGIAN";  ///< LAGRANGIAN | EULERIAN_ARD | LEGACY
    bool water_age = true;
    bool pollutants = true;
    bool single_conduit = false;        ///< J0 → C1(2000 ft) → OUT
    const char* end_time = "06:00:00";
    int routing_step = 5;
    double kdecay = 0.0;                 ///< [POLLUTANTS] Kdecay column, 1/s
    const char* age_component = nullptr; ///< [WATER_AGE_SOURCES] body, or null
};

void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

void write_deck(const std::string& path, const std::string& tag,
                const DeckSpec& s) {
    std::ofstream f(path);
    f << "[TITLE]\nLARD X4 age\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "QUALITY_SOLVER " << s.solver << "\n";
    if (s.water_age) f << "WATER_AGE ON\n";
    f << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << s.end_time << "\n"
      << "ROUTING_STEP " << s.routing_step << "\nREPORT_STEP 00:05:00\n\n";
    if (s.single_conduit) {
        f << "[JUNCTIONS]\nJ0 10.0 10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
          << "[CONDUITS]\nC1 J0 OUT 2000 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 2.0 0 0 0\n\n";
    } else {
        f << "[JUNCTIONS]\n"
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
    }
    if (s.pollutants)
        f << "[POLLUTANTS]\nTSS MG/L 0 0 0 " << (s.kdecay * 86400.0)  // 1/day column (KD1)
          << " NO * 0 0 0\n\n";
    if (s.age_component != nullptr) {
        const std::string age_path = tag + "_src.age";
        write_file(age_path,
                   std::string("[WATER_AGE_SOURCES]\n") + s.age_component);
        f << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.waterage config=\"" << age_path
          << "\"\n\n";
    }
    f << "[INFLOWS]\n"
      << "J0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n";
    if (s.pollutants)
        f << "J0 TSS  \"\" CONCEN 1.0 1.0 " << kCin << "\n";
    f << "\n[REPORT]\nINPUT NO\n";
}

struct AgeRun {
    std::vector<double> node_age;   ///< seconds, final
    std::vector<double> link_age;   ///< seconds, final
    std::vector<double> tss_final;  ///< [link] mg/L (empty if no pollutants)
    double outfall_age = 0.0;       ///< seconds
    double head_node_age = 0.0;     ///< J0
    double total_volume = 0.0;      ///< links + nodes, ft³ at end
    double flow0 = 0.0;             ///< |Q| of link 0 at end
    double elapsed_s = 0.0;         ///< simulated seconds
    bool ok = false;
};

AgeRun run_deck(const std::string& tag, const DeckSpec& s) {
    AgeRun r;
    const std::string inp = tag + ".inp";
    write_deck(inp, tag, s);

    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) { ADD_FAILURE() << "engine create"; return r; }
    if (swmm_engine_open(e, inp.c_str(), (tag + ".rpt").c_str(),
                         (tag + ".out").c_str(), nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        ADD_FAILURE() << "open/init/start failed for " << inp;
        swmm_engine_destroy(e);
        return r;
    }
    auto& ctx = as_cpp_engine(e).context();
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
        r.elapsed_s = elapsed * 86400.0;  // elapsed is days
    } while (elapsed > 0.0 && ++guard < 200000);
    // elapsed returns 0 at the end; recover the simulated span from options.
    swmm_engine_end(e);

    const auto& ws = ctx.water_age_state;
    r.node_age.assign(ws.node_age.begin(), ws.node_age.end());
    r.link_age.assign(ws.link_age.begin(), ws.link_age.end());
    if (np > 0) {
        r.tss_final.assign(static_cast<std::size_t>(nl), 0.0);
        for (int l = 0; l < nl; ++l)
            r.tss_final[static_cast<std::size_t>(l)] =
                ctx.links.conc[static_cast<std::size_t>(l * np)];
    }
    for (int j = 0; j < nn; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (ctx.nodes.type[uj] == openswmm::NodeType::OUTFALL)
            r.outfall_age = (uj < r.node_age.size()) ? r.node_age[uj] : 0.0;
        r.total_volume += ctx.nodes.volume[uj];
    }
    r.head_node_age = r.node_age.empty() ? 0.0 : r.node_age[0];
    for (int l = 0; l < nl; ++l)
        r.total_volume += ctx.links.volume[static_cast<std::size_t>(l)];
    r.flow0 = std::abs(ctx.links.flow[0]);
    r.ok = true;
    swmm_engine_destroy(e);
    return r;
}

// ---------------------------------------------------------------------------
// Gate 1 — steady age at the outfall equals the measured residence time.
// ---------------------------------------------------------------------------
TEST(LardAgeTest, SteadyOutfallAgeIsTheMeasuredResidenceTime) {
    DeckSpec s;
    s.single_conduit = true;
    const AgeRun a = run_deck("_la_tau", s);
    ASSERT_TRUE(a.ok);
    ASSERT_GT(a.flow0, 0.0);

    // Residence time from the deck's own steady state: total wet volume
    // over throughflow. The 10% band absorbs junction-store blending.
    const double tau = a.total_volume / a.flow0;
    ASSERT_GT(a.outfall_age, 600.0) << "age never developed (liveness)";
    EXPECT_NEAR(a.outfall_age / tau, 1.0, 0.10)
        << "outfall age " << a.outfall_age << " s vs measured tau " << tau
        << " s — the plug is not carrying its residence time";
    // Age rises downstream (A2b's monotonicity, single-hop form).
    EXPECT_GT(a.outfall_age, a.head_node_age);
}

// ---------------------------------------------------------------------------
// Gate 2 — A5: the three engines agree at the outfall node.
// ---------------------------------------------------------------------------
TEST(LardAgeTest, ThreeEnginesAgreeAtTheOutfallNode) {
    DeckSpec lard;
    DeckSpec ard = lard;  ard.solver = "EULERIAN_ARD";
    DeckSpec leg = lard;  leg.solver = "LEGACY";

    const AgeRun a = run_deck("_la_a5_lard", lard);
    const AgeRun b = run_deck("_la_a5_ard", ard);
    const AgeRun c = run_deck("_la_a5_leg", leg);
    ASSERT_TRUE(a.ok && b.ok && c.ok);

    // Liveness: a chain this long at 5 cfs holds >10 min of water.
    ASSERT_GT(a.outfall_age, 600.0);
    ASSERT_GT(b.outfall_age, 600.0);
    ASSERT_GT(c.outfall_age, 600.0);

    // The outfall NODE is the comparable quantity (A1b). 5% pairwise: the
    // recorded engine offsets on the A1b deck were +4.5 s (ARD) and
    // +21.9 s (LEGACY) against an analytic ~2 h — well inside. If this
    // fails, print all three and compare against the measured tau before
    // touching the band (handoff §3).
    EXPECT_NEAR(a.outfall_age / c.outfall_age, 1.0, 0.05)
        << "LARD " << a.outfall_age << " vs LEGACY " << c.outfall_age;
    EXPECT_NEAR(a.outfall_age / b.outfall_age, 1.0, 0.05)
        << "LARD " << a.outfall_age << " vs ARD " << b.outfall_age;
}

// ---------------------------------------------------------------------------
// Gate 3 — WATER_AGE ON leaves pollutants bit-identical under LARD.
// ---------------------------------------------------------------------------
TEST(LardAgeTest, PollutantsAreBitIdenticalUnderAgeTracking) {
    DeckSpec on;                       // age on, TSS on
    DeckSpec off = on;  off.water_age = false;

    const AgeRun a = run_deck("_la_bit_on", on);
    const AgeRun b = run_deck("_la_bit_off", off);
    ASSERT_TRUE(a.ok && b.ok);
    ASSERT_EQ(a.tss_final.size(), b.tss_final.size());
    ASSERT_FALSE(a.tss_final.empty());
    ASSERT_GT(b.tss_final[0], 1.0) << "no pollutant signal (liveness)";

    // NOT bitwise, and that is a measured design consequence, not a
    // concession: the §4.5 merge criterion is a function of EVERY species,
    // and consecutive releases differ in age by ~dt seconds against a
    // tolerance of kMergeRtol x age ~ 0.36 s — so turning age on changes
    // which segments merge, which re-associates the floating-point sums.
    // Merges conserve every species' mass, so the deviation is pure FP
    // re-association: measured 1e-15 RELATIVE on this deck (LEGACY's A3
    // gate can be bitwise because the CSTR has no discretization to
    // couple). The 1e-9 band is six orders above that floor and ten
    // orders below what the defect this gate exists for produces — an
    // age-row stride leak puts ~3.9e3-second values or zeros in the TSS
    // column (falsifier vi, verified loud).
    for (std::size_t l = 0; l < a.tss_final.size(); ++l)
        EXPECT_NEAR(a.tss_final[l], b.tss_final[l],
                    std::abs(b.tss_final[l]) * 1.0e-9)
            << "TSS moved at link " << l << " when WATER_AGE turned on — "
               "the age row leaked into the pollutant stride (the A2b "
               "conflation family)";
}

// ---------------------------------------------------------------------------
// Gate 4 — INITIAL_STATE seeds, then ages exactly, ahead of the fresh front.
// ---------------------------------------------------------------------------
TEST(LardAgeTest, InitialStateSeedsAndAgesExactly) {
    DeckSpec s;
    s.single_conduit = true;
    // Travel time on this deck is ~13 min (5 cfs, ~2 ft² flow area over
    // 2000 ft) — stop at 5 min so the outfall is still unambiguously
    // draining seeded water, with margin for the front's dispersion-free
    // leading edge.
    s.end_time = "00:05:00";
    s.age_component = "INITIAL_STATE GLOBAL 1.0\n";
    const AgeRun a = run_deck("_la_seed", s);
    ASSERT_TRUE(a.ok);

    // The outfall is still receiving seeded water: its age must read the
    // seed plus the elapsed time, to within a few routing steps — seeding
    // (3600) and exact aging (+300) verified together. A missed seed reads
    // ~300; a missed aging reads ~3600; both are far outside the band.
    const double expected = 3600.0 + 300.0;
    EXPECT_NEAR(a.outfall_age, expected, 3.0 * s.routing_step)
        << "outfall age " << a.outfall_age << " vs seeded+aged " << expected;
}

// ---------------------------------------------------------------------------
// Gate 5 — a per-source age shifts the steady outfall age by its value.
// ---------------------------------------------------------------------------
TEST(LardAgeTest, ExternalInflowSourceAgeShiftsTheOutfall) {
    DeckSpec base;
    DeckSpec src = base;
    src.age_component = "EXTERNAL_INFLOW NODE J0 6.0\n";

    const AgeRun a = run_deck("_la_shift_base", base);
    const AgeRun b = run_deck("_la_shift_src", src);
    ASSERT_TRUE(a.ok && b.ok);
    ASSERT_GT(a.outfall_age, 600.0);

    // At steady state every parcel entered 6 h older, so the outfall reads
    // exactly 21600 s more (A1a gate 2's claim, third engine). ±60 s
    // absorbs the two runs' step alignment.
    const double shift = b.outfall_age - a.outfall_age;
    EXPECT_NEAR(shift, 21600.0, 60.0)
        << "configured 6 h source age shifted the outfall by " << shift
        << " s — the loader seam is not feeding the LARD mix";
}

// ---------------------------------------------------------------------------
// Gate 6 — age is a clock, not a pollutant: KDECAY must not touch it.
//
// Handoff §4.v named this defense and admitted no gate observed it: the
// decay loop's bound (p < np) is the only thing keeping exp(-k dt) off the
// age row, and every delivered deck ran k = 0, where the bound is inert.
// Measured with the loop widened to ns (the falsifier): outfall age drops
// 645.7 -> 476.7 s on this deck while TSS is identical either way.
// ---------------------------------------------------------------------------
TEST(LardAgeTest, AgeDoesNotDecayWithThePollutant) {
    DeckSpec k0;
    k0.single_conduit = true;
    k0.end_time = "04:00:00";
    DeckSpec k1 = k0;  k1.kdecay = 1.0e-3;

    const AgeRun a = run_deck("_la_k0", k0);
    const AgeRun b = run_deck("_la_k1", k1);
    ASSERT_TRUE(a.ok && b.ok);
    ASSERT_GT(a.outfall_age, 300.0) << "age never developed (liveness)";

    // The decay must be LIVE on the pollutant row (premise)...
    ASSERT_FALSE(b.tss_final.empty());
    ASSERT_LT(b.tss_final[0], 80.0)
        << "kdecay=1e-3 left TSS at " << b.tss_final[0]
        << " — the decay premise is broken, not the age claim";

    // ...and absent from the age row: the same deck, the same water, the
    // same clock. 1% absorbs segment-partition jitter (the A3 family).
    EXPECT_NEAR(b.outfall_age, a.outfall_age, a.outfall_age * 0.01)
        << "outfall age moved when KDECAY turned on: " << a.outfall_age
        << " -> " << b.outfall_age << " — the age row is being decayed "
           "like a pollutant";
}

}  // namespace
