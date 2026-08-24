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
 * @file test_negative_sources.cpp
 * @brief X6 / D-NS1: negative sources extract, warn, clamp to available,
 *        and the ledger carries the mass ACTUALLY removed — in all three
 *        engines.
 *
 * @details Subplan §3.1 (user requirement, 2026-08-23). The claims:
 *
 *          1. A negative [INFLOWS] MASS row extracts: the outfall reads
 *             below the no-extraction control under LEGACY, EULERIAN_ARD
 *             and LAGRANGIAN alike, and the ledger identity closes with
 *             the signed booking — the row that separates "extraction
 *             works" from "extraction is silently dropped" (the ARD
 *             max(0,·) defect this round removed).
 *          2. Over-extraction clamps: no concentration anywhere ever goes
 *             negative, the clamp counter advances, the first-clamp
 *             warning fires, and the ledger STILL closes because the
 *             shortfall is un-booked — the contract's hardest claim.
 *          3. The three warnings of the contract fire exactly when they
 *             should, in both directions: parse warnings on configured
 *             negative rows (mass and age), and NO D-NS1 output of any
 *             kind on an ordinary positive deck.
 *          4. A negative age source extracts age-volume (water reads
 *             younger), floors at zero, and counts its clamps.
 *
 *          Scratch fixtures use the `_nx_` prefix (collision-checked).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    const char* solver = "LEGACY";  ///< LEGACY | EULERIAN_ARD | LAGRANGIAN
    double extract = 0.0;           ///< J2 TSS MASS baseline (negative = out)
    bool water_age = false;
    const char* age_source = nullptr;  ///< [WATER_AGE_SOURCES] body
    bool single_conduit = false;
    const char* end_time = "04:00:00";
    bool dwf = false;               ///< dry-weather inflow at J0 (gate 4)
};

void write_deck(const std::string& path, const std::string& tag,
                const DeckSpec& s) {
    std::ofstream f(path);
    f << "[TITLE]\nD-NS1 X6\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "QUALITY_SOLVER " << s.solver << "\n";
    if (s.water_age) f << "WATER_AGE ON\n";
    f << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << s.end_time << "\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:05:00\n\n";
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
    f << "[POLLUTANTS]\nTSS MG/L 0 0 0 0 NO * 0 0 0\n\n";
    if (s.age_source != nullptr) {
        const std::string age_path = tag + ".age";
        std::ofstream a(age_path);
        a << "[WATER_AGE_SOURCES]\n" << s.age_source;
        a.close();
        f << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.waterage config=\"" << age_path
          << "\"\n\n";
    }
    if (s.dwf) f << "[DWF]\nJ0 FLOW " << kQ << "\n\n";
    f << "[INFLOWS]\n"
      << "J0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n"
      << "J0 TSS  \"\" CONCEN 1.0 1.0 " << kCin << "\n";
    if (s.extract != 0.0)
        f << "J2 TSS  \"\" MASS 1.0 1.0 " << s.extract << "\n";
    f << "\n[REPORT]\nINPUT NO\n";
}

struct NsRun {
    double outfall_conc = 0.0;
    double outfall_age = 0.0;      ///< seconds
    double min_conc = 1.0e300;     ///< min over ALL nodes+links, ALL steps
    double min_age = 1.0e300;      ///< min node age over all steps
    double mb_init = 0.0, mb_ex_in = 0.0, mb_outflow = 0.0, mb_final = 0.0,
           mb_reacted = 0.0;
    long clamp_events = 0, age_clamp_events = 0;
    long clamp_events_10min = -1;  ///< counter when the run passed 600 s
    std::vector<std::string> warnings;
    bool ok = false;
};

NsRun run_deck(const std::string& tag, const DeckSpec& s) {
    NsRun r;
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
        for (int l = 0; l < nl; ++l)
            r.min_conc = std::min(
                r.min_conc, ctx.links.conc[static_cast<std::size_t>(l * np)]);
        for (int j = 0; j < nn; ++j)
            r.min_conc = std::min(
                r.min_conc, ctx.nodes.conc[static_cast<std::size_t>(j * np)]);
        if (s.water_age)
            for (int j = 0; j < nn; ++j)
                r.min_age = std::min(
                    r.min_age,
                    ctx.water_age_state.node_age[static_cast<std::size_t>(j)]);
        if (r.clamp_events_10min < 0 && elapsed * 86400.0 >= 600.0)
            r.clamp_events_10min = ctx.negsrc.clamp_events;
    } while (elapsed > 0.0 && ++guard < 200000);
    swmm_engine_end(e);

    for (int j = 0; j < nn; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (ctx.nodes.type[uj] == openswmm::NodeType::OUTFALL) {
            r.outfall_conc = ctx.nodes.conc[static_cast<std::size_t>(j * np)];
            if (s.water_age && uj < ctx.water_age_state.node_age.size())
                r.outfall_age = ctx.water_age_state.node_age[uj];
        }
    }
    const auto& mb = ctx.mass_balance;
    auto row0 = [](const std::vector<double>& v) {
        return v.empty() ? 0.0 : v[0];
    };
    r.mb_init = row0(mb.qual_routing_init);
    r.mb_ex_in = row0(mb.qual_routing_ex_in);
    r.mb_outflow = row0(mb.qual_routing_outflow);
    r.mb_final = row0(mb.qual_routing_final);
    r.mb_reacted = row0(mb.qual_routing_reacted);
    r.clamp_events = ctx.negsrc.clamp_events;
    r.age_clamp_events = ctx.negsrc.age_clamp_events;
    r.warnings = ctx.warnings;
    r.ok = true;
    swmm_engine_destroy(e);
    return r;
}

bool has_warning(const NsRun& r, const char* needle) {
    for (const auto& w : r.warnings)
        if (w.find(needle) != std::string::npos) return true;
    return false;
}

const char* kSolvers[] = {"LEGACY", "EULERIAN_ARD", "LAGRANGIAN"};

// ---------------------------------------------------------------------------
// Gate 1 — a negative MASS row extracts, in all three engines, with the
// ledger closing on the SIGNED booking.
// ---------------------------------------------------------------------------
TEST(NegativeSourcesTest, NegativeInflowExtractsUnderAllThreeEngines) {
    for (const char* solver : kSolvers) {
        DeckSpec ctrl;  ctrl.solver = solver;
        DeckSpec ext = ctrl;
        // MASS rows are mg/s; internal mass is mg/L*ft^3, so the row value
        // divides by 28.316846592 on the way in — MEASURED (X6 round): a
        // -100 row moved the outfall by exactly 100/28.3168/500 = 0.71%,
        // identical across all three engines. This row extracts 100
        // internal units/s = 20% of the 500/s throughflow.
        ext.extract = -2831.6846592;

        const NsRun a = run_deck(std::string("_nx_ctl_") + solver, ctrl);
        const NsRun b = run_deck(std::string("_nx_ext_") + solver, ext);
        ASSERT_TRUE(a.ok && b.ok) << solver;

        // Premise: the control delivers the steady source.
        ASSERT_GT(a.outfall_conc, 0.95 * kCin)
            << solver << " control never reached steady state";

        // Extraction is real: measurably below the control, far above the
        // silently-dropped answer (== control, the old ARD defect) and
        // above zero (not runaway).
        EXPECT_LT(b.outfall_conc, 0.92 * kCin)
            << solver << ": extraction had no effect — the negative load "
               "was dropped (the max(0,·) family)";
        EXPECT_GT(b.outfall_conc, 0.50 * kCin)
            << solver << ": extraction removed far more than requested";

        // Ledger identity with the SIGNED external row: what the ledger
        // says came in (net) equals outflow + final. Startup clamps ARE
        // legitimate here (J2 is dry-store until the front arrives ~min
        // 7 — you cannot extract what is not there yet; §3.3's predicted
        // case, measured 10/46/88 events across the engines); after the
        // chain wets, 20% extraction must never clamp again.
        ASSERT_GE(b.clamp_events_10min, 0) << solver;
        EXPECT_EQ(b.clamp_events, b.clamp_events_10min)
            << solver << ": clamps fired after minute 10 — the clamp is "
               "triggering where mass is plainly available";
        const double in = b.mb_init + b.mb_ex_in;
        const double out = b.mb_outflow + b.mb_final + b.mb_reacted;
        ASSERT_GT(in, 0.0);
        EXPECT_NEAR(out / in, 1.0, 0.02)
            << solver << ": ledger broke under signed extraction booking";

        // The parse warning names the row (both runs' warning lists come
        // from open(): only the extraction deck carries it).
        EXPECT_TRUE(has_warning(b, "the baseline is negative"))
            << solver;
        EXPECT_FALSE(has_warning(a, "the baseline is negative"))
            << solver;
    }
}

// ---------------------------------------------------------------------------
// Gate 2 — over-extraction clamps, warns, stays non-negative, and the
// un-booking keeps the ledger closed.
// ---------------------------------------------------------------------------
TEST(NegativeSourcesTest, OverExtractionClampsWarnsAndStaysNonNegative) {
    for (const char* solver : kSolvers) {
        DeckSpec s;  s.solver = solver;
        // 10x the 500-internal-units/s throughflow, in mg/s (the MASS-row
        // unit, x28.316846592) — the X6 round recalibrated this from
        // -5000, which the units made a mere 0.35x (clamps were startup
        // -only and the un-booking became a ~1% effect falsifier ii could
        // hide inside).
        s.extract = -5000.0 * 28.316846592;
        const NsRun r = run_deck(std::string("_nx_over_") + solver, s);
        ASSERT_TRUE(r.ok) << solver;

        EXPECT_GE(r.min_conc, -1.0e-12)
            << solver << ": a concentration went negative under "
               "over-extraction";
        EXPECT_GT(r.clamp_events, 0)
            << solver << ": over-extraction never clamped — the counter "
               "has no observer path in this engine";
        EXPECT_TRUE(has_warning(r, "clamped to the available amount"))
            << solver << ": the first-clamp warning did not fire";
        EXPECT_TRUE(has_warning(r, "D-NS1 summary"))
            << solver << ": the end-of-run summary did not fire";

        // THE claim: with the shortfall un-booked, the identity still
        // closes. At true 10x extraction the NET booked inflow is nearly
        // zero (the extraction removes almost everything that arrives), so
        // the RELATIVE identity out/in is ill-conditioned — measured
        // (X6 round): LEGACY netted 184k of the 7.2e6 gross, turning a
        // 4.6k absolute error into a fake 2.5%. The closure is therefore
        // normalized by the GROSS inflow, which the deck pins exactly:
        // 100 mg/L x 5 cfs = 500 internal units/s over the 4 h run.
        const double in = r.mb_init + r.mb_ex_in;
        const double out = r.mb_outflow + r.mb_final + r.mb_reacted;
        ASSERT_GT(in, 0.0)
            << solver << ": net booked inflow went negative — the full "
               "extraction request is still booked (no un-booking)";
        const double gross = kCin * kQ * 4.0 * 3600.0;
        const double miss = std::abs(out - in) / gross;
        std::printf("[ NEGSRC   ] %-13s over-extraction |out-in|/gross = "
                    "%.6f (clamps %ld, net ex_in %.1f)\n",
                    solver, miss, r.clamp_events, r.mb_ex_in);
        // MEASURED (X6 round) at 10x: 0.00064 (LEGACY), 0.0033 (ARD),
        // 0.0033 (LARD). Band 1% = 3x the worst engine. Falsifier ii (no
        // un-booking) leaves ex_in at the full -64.8e6 request: in goes
        // NEGATIVE (the ASSERT above) and miss lands near 10 — three
        // orders outside.
        EXPECT_LT(miss, 0.01)
            << solver << ": the shortfall un-booking is not closing the "
               "ledger (in=" << in << " out=" << out << ")";
    }
}

// ---------------------------------------------------------------------------
// Gate 3 — the warnings, both directions.
// ---------------------------------------------------------------------------
TEST(NegativeSourcesTest, WarningsFireExactlyOnNegativeConfigs) {
    // (a) Negative age source parses with the extraction warning.
    DeckSpec age;
    age.solver = "LAGRANGIAN";
    age.water_age = true;
    age.single_conduit = true;
    age.age_source = "EXTERNAL_INFLOW NODE J0 -0.5\n";
    age.end_time = "00:30:00";
    const NsRun a = run_deck("_nx_warn_age", age);
    ASSERT_TRUE(a.ok);
    EXPECT_TRUE(has_warning(a, "EXTRACTS age-volume"))
        << "negative age source parsed silently";

    // (b) An ordinary positive deck emits NOTHING of D-NS1's: no parse
    // warning, no clamp warning, no summary, zero counters.
    DeckSpec pos;  pos.solver = "LEGACY";
    const NsRun b = run_deck("_nx_warn_pos", pos);
    ASSERT_TRUE(b.ok);
    EXPECT_FALSE(has_warning(b, "D-NS1"))
        << "a D-NS1 warning fired on a deck with no negative source";
    EXPECT_EQ(b.clamp_events, 0);
    EXPECT_EQ(b.age_clamp_events, 0);
}

// ---------------------------------------------------------------------------
// Gate 4 — negative age source extracts age-volume, floors at zero.
// ---------------------------------------------------------------------------
TEST(NegativeSourcesTest, NegativeAgeSourceLowersAgeAndFloorsAtZero) {
    // Redesigned in the X6 round: extraction at J0 against an
    // INITIAL_STATE seed measured only -3.3 s at the outfall — J0's own
    // age washes to ~0 in seconds (tiny node volume), so there is nothing
    // left to extract from by the time its water transits. The age-volume
    // loaders apply EXTERNAL_INFLOW rows as q*age at nodes WITH external
    // inflow, so a sustained observable needs an OLD supply concurrent
    // with the extraction: DWF at J0 carries +2 h while the external
    // inflow row extracts -0.5 h. Steady J0 mix: (5*7200 + 5*(-1800))/10
    // = 2700 s vs the base's (5*7200 + 5*0)/10 = 3600 s — a 900 s
    // signal instead of 3.3 s.
    DeckSpec base;
    base.solver = "LAGRANGIAN";
    base.water_age = true;
    base.single_conduit = true;
    base.dwf = true;
    base.age_source = "DWF NODE J0 2.0\n";
    base.end_time = "00:40:00";  // steady well past the ~7 min transit
    DeckSpec neg = base;
    neg.age_source = "DWF NODE J0 2.0\nEXTERNAL_INFLOW NODE J0 -0.5\n";

    const NsRun a = run_deck("_nx_age_base", base);
    const NsRun b = run_deck("_nx_age_neg", neg);
    ASSERT_TRUE(a.ok && b.ok);

    ASSERT_GT(a.outfall_age, 3000.0) << "the DWF age supply never arrived";
    // The mix arithmetic above says 900 s; ask for a comfortable half.
    EXPECT_LT(b.outfall_age, a.outfall_age - 450.0)
        << "the negative age source did not extract age-volume (base "
        << a.outfall_age << " s vs " << b.outfall_age << " s)";
    EXPECT_GE(b.min_age, 0.0)
        << "a node age went negative — the age floor failed";

    // Extreme extraction clamps and counts.
    DeckSpec extreme = base;
    extreme.age_source = "DWF NODE J0 2.0\nEXTERNAL_INFLOW NODE J0 -100.0\n";
    const NsRun c = run_deck("_nx_age_extreme", extreme);
    ASSERT_TRUE(c.ok);
    EXPECT_GE(c.min_age, 0.0);
    EXPECT_GT(c.age_clamp_events, 0)
        << "extreme age extraction never clamped — no observer path";
    EXPECT_TRUE(has_warning(c, "age-volume"));
}

}  // namespace
