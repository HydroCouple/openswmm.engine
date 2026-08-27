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
 * @file test_lard_dt_reference.cpp
 * @brief X3a: the LARD dt-refinement instrument, and the reverse-flow deck.
 *
 * @details Two debts collected in one suite.
 *
 *          **The instrument** (the `0e8e57df` pattern, sharpened): four
 *          falsifier rows across X2/X4 are dt-order errors invisible to
 *          steady observables — X2.ii (release at stale conc), X2.vi
 *          (stale passthrough), X2.viii (old-vs-new mix volume), X4.ii
 *          (age after transport). Each converges to the CORRECT limit, so
 *          only convergence RATE discriminates. The LARD form is sharper
 *          than the heat instrument's: refining `QUALITY_STEP` under a
 *          FIXED `ROUTING_STEP` leaves the flow solution untouched, so the
 *          contraction is pure transport — the hydraulic caveat that kept
 *          the heat instrument's ratio reported-not-asserted does not
 *          apply here. The ratio is still reported first; the validating
 *          round measures before pinning (bands below are placeholders in
 *          the marked positions and MUST be measured per the standing
 *          rule: correct-form vs defective-form, band between them).
 *
 *          Gates 1–2 run one washout deck at dtq = {40, 20, 10} s under
 *          ROUTING_STEP 40 and assert (a) contraction |A(40)−A(20)| >
 *          |A(20)−A(10)| and (b) the coarse answer within a fraction of
 *          the deck's own source spread. Gate 3 asserts the steady answer
 *          is dtq-INVARIANT (substep accounting: consuming the full
 *          per-step external volume every substep moves the steady state —
 *          that is the falsifier).
 *
 *          **The reverse-flow deck** (owed since X2 §7): a FIXED high-stage
 *          outfall floods the network backward before inflow establishes
 *          forward drainage. The gate asserts the reversal actually
 *          happened (min C2 flow < 0 — liveness, on the LEGACY control
 *          too, so a deck that stops reversing reads as a broken premise),
 *          the maximum principle across the whole run, finite state
 *          everywhere, and ledger closure at the end.
 *
 *          Scratch fixtures use the `_ld_` prefix (collision-checked).
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
constexpr double kCinit = 80.0;

struct DeckSpec {
    bool lard = true;
    int routing_step = 40;
    int quality_step = 0;         ///< 0 = omit the key
    const char* end_time = "00:20:00";
    bool washout = true;          ///< Cinit high, fresh inflow (no TSS row)
    bool water_age = false;       ///< age washout variant (INITIAL_STATE)
    bool reverse = false;         ///< FIXED high-stage outfall deck
    bool orifice = false;         ///< washout variant: C3 becomes ORIFICE O3
    bool dry_start = false;       ///< gate 5: init depth 0, fill transient
    bool storage = false;         ///< gate 5: J2 becomes a storage unit
};

void write_deck(const std::string& path, const std::string& tag,
                const DeckSpec& s) {
    std::ofstream f(path);
    f << "[TITLE]\nLARD X3a dt-reference\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << (s.lard ? "QUALITY_SOLVER LAGRANGIAN\n" : "QUALITY_SOLVER LEGACY\n");
    if (s.water_age) f << "WATER_AGE ON\n";
    if (s.quality_step > 0) f << "QUALITY_STEP " << s.quality_step << "\n";
    f << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << s.end_time << "\n"
      << "ROUTING_STEP " << s.routing_step << "\nREPORT_STEP 00:05:00\n\n";
    if (s.reverse) {
        // Dry start; the FIXED stage at 11 ft floods C2 (and then C1)
        // backward until inflow raises the heads past the boundary.
        f << "[JUNCTIONS]\n"
          << "J0 10.0 10 0 0 0\nJ1 9.0  10 0 0 0\n\n"
          << "[OUTFALLS]\nOUT 8.0 FIXED 11.0 NO\n\n"
          << "[CONDUITS]\n"
          << "C1 J0 J1 400 0.013 0 0 0\nC2 J1 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\n"
          << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n\n";
    } else {
        const char* y0 = s.dry_start ? "0.0" : "1.5";
        f << "[JUNCTIONS]\n"
          << "J0 10.0 10 " << y0 << " 0 0\nJ1 9.4  10 " << y0 << " 0 0\n";
        // The storage variant gives the node mix a node whose volume is
        // STATE, not MIN_SURFAREA residue — the only place old-vs-new
        // volume (X2.viii) is a leading-order term.
        if (!s.storage)
            f << "J2 8.8  10 " << y0 << " 0 0\n";
        f << "J3 8.2  10 " << y0 << " 0 0\n"
          << "J4 7.6  10 " << y0 << " 0 0\n\n";
        if (s.storage)
            f << "[STORAGE]\n"
              << "J2 8.8 10 " << (s.dry_start ? "0.05" : "1.5")
              << " FUNCTIONAL 0 0 2000\n\n";
        f
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
          << "[CONDUITS]\n"
          << "C1 J0 J1 500 0.013 0 0 0\nC2 J1 J2 500 0.013 0 0 0\n";
        // The orifice variant swaps the middle conduit for O3: the ladder
        // then flows through the zero-volume PASSTHROUGH, the only path on
        // which a stale passthrough concentration (X2.vi) is observable —
        // conduit-only decks never execute that branch.
        if (!s.orifice) f << "C3 J2 J3 500 0.013 0 0 0\n";
        f << "C4 J3 J4 500 0.013 0 0 0\n"
          << "C5 J4 OUT 500 0.013 0 0 0\n\n";
        if (s.orifice)
            f << "[ORIFICES]\nO3 J2 J3 SIDE 0 0.65 NO 0\n\n";
        f << "[XSECTIONS]\n"
          << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
          << (s.orifice ? "O3 CIRCULAR 1.0 0 0 0\n" : "C3 CIRCULAR 2.0 0 0 0\n")
          << "C4 CIRCULAR 2.0 0 0 0\n"
          << "C5 CIRCULAR 2.0 0 0 0\n\n";
    }
    // Washout: Cinit high, inflow water FRESH (no TSS inflow row) — the
    // outfall traces a falling curve whose mid-point is the observable.
    // Reverse/steady variants carry a concentration source instead.
    f << "[POLLUTANTS]\nTSS MG/L 0 0 0 0 NO * 0 0 "
      << (s.washout ? kCinit : 0.0) << "\n\n";
    if (s.water_age) {
        const std::string age_path = tag + ".age";
        std::ofstream a(age_path);
        a << "[WATER_AGE_SOURCES]\nINITIAL_STATE GLOBAL 1.0\n";
        a.close();
        f << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.waterage config=\"" << age_path
          << "\"\n\n";
    }
    f << "[INFLOWS]\n"
      << "J0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n";
    if (!s.washout)
        f << "J0 TSS  \"\" CONCEN 1.0 1.0 " << kCin << "\n";
    f << "\n[REPORT]\nINPUT NO\n";
}

struct DtRun {
    double outfall_conc = 0.0;   ///< TSS at the outfall node, end of run
    double outfall_age = 0.0;    ///< seconds
    double mass_out = 0.0;       ///< ∫ conc·|q| dt on the LAST link, engine-
                                 ///< neutral (same formula both engines)
    std::vector<double> node_final;
    double peak_node = 0.0, peak_link = 0.0;
    double min_flow_c2 = 1.0e30, final_flow_c2 = 0.0;
    double mb_ex_in = 0.0, mb_init = 0.0, mb_outflow = 0.0, mb_final = 0.0;
    bool finite = true;
    bool ok = false;
};

DtRun run_deck(const std::string& tag, const DeckSpec& s) {
    DtRun r;
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
    double prev_elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
            ADD_FAILURE() << "step failed for " << inp;
            swmm_engine_destroy(e);
            return r;
        }
        // Engine-neutral discharged mass: last link's published conc ×
        // flow × dt. The final step's flux is lost when elapsed reports 0
        // at end-of-run — identical loss for both engines at the same
        // ROUTING_STEP, so it cancels in any lard-vs-legacy difference.
        if (elapsed > prev_elapsed && nl > 0) {
            const double dt_s = (elapsed - prev_elapsed) * 86400.0;
            const auto ul = static_cast<std::size_t>((nl - 1) * np);
            r.mass_out += ctx.links.conc[ul] *
                          std::abs(ctx.links.flow[static_cast<std::size_t>(
                              nl - 1)]) * dt_s;
        }
        prev_elapsed = elapsed;
        for (int l = 0; l < nl; ++l) {
            const double c =
                ctx.links.conc[static_cast<std::size_t>(l * np)];
            if (!std::isfinite(c)) r.finite = false;
            r.peak_link = std::max(r.peak_link, c);
        }
        for (int j = 0; j < nn; ++j) {
            const double c =
                ctx.nodes.conc[static_cast<std::size_t>(j * np)];
            if (!std::isfinite(c)) r.finite = false;
            r.peak_node = std::max(r.peak_node, c);
        }
        if (nl > 1) {
            r.min_flow_c2 = std::min(r.min_flow_c2, ctx.links.flow[1]);
            r.final_flow_c2 = ctx.links.flow[1];
        }
    } while (elapsed > 0.0 && ++guard < 200000);
    swmm_engine_end(e);

    r.node_final.assign(static_cast<std::size_t>(nn), 0.0);
    for (int j = 0; j < nn; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        r.node_final[uj] = ctx.nodes.conc[static_cast<std::size_t>(j * np)];
        if (ctx.nodes.type[uj] == openswmm::NodeType::OUTFALL) {
            r.outfall_conc = r.node_final[uj];
            if (!ctx.water_age_state.node_age.empty())
                r.outfall_age = ctx.water_age_state.node_age[uj];
        }
    }
    const auto& mb = ctx.mass_balance;
    auto row = [](const std::vector<double>& v) {
        return v.empty() ? 0.0 : v[0];
    };
    r.mb_ex_in = row(mb.qual_routing_ex_in);
    r.mb_init = row(mb.qual_routing_init);
    r.mb_outflow = row(mb.qual_routing_outflow);
    r.mb_final = row(mb.qual_routing_final);
    r.ok = true;
    swmm_engine_destroy(e);
    return r;
}

// ---------------------------------------------------------------------------
// Gate 1 — pollutant washout converges under QUALITY_STEP refinement.
// ---------------------------------------------------------------------------
TEST(LardDtReferenceTest, WashoutConvergesUnderQualityStepRefinement) {
    DeckSpec s;  // washout, ROUTING_STEP 40
    // Measured (X3a round): the outfall front arrives ~min 15 and is fully
    // through by min 18 — at 20:00 the washout is FINISHED (1.2e-7) and the
    // liveness floor rails. 17:00 sits mid-front: ladder {33.9, 18.2, 14.8},
    // dtq=5 continues to 13.35 (ratio -> ~2.3, first order). 16:00 rails at
    // the top (dtq<=5 gives 80.0).
    s.end_time = "00:17:00";
    double a[3] = {0.0, 0.0, 0.0};
    const int ladder[3] = {40, 20, 10};
    for (int i = 0; i < 3; ++i) {
        DeckSpec si = s;
        si.quality_step = ladder[i];
        const DtRun r = run_deck("_ld_wash", si);
        ASSERT_TRUE(r.ok) << "dtq=" << ladder[i];
        a[i] = r.outfall_conc;
    }
    // Liveness: the observable must sit mid-washout, not at either rail —
    // otherwise refinement has nothing to move.
    ASSERT_GT(a[2], 0.02 * kCinit) << "washout finished — shorten the run";
    ASSERT_LT(a[2], 0.98 * kCinit) << "washout never started";

    const double d1 = std::abs(a[0] - a[1]);
    const double d2 = std::abs(a[1] - a[2]);
    // (a) Contraction. Hydraulics are FIXED across the ladder (same
    // ROUTING_STEP), so unlike the heat instrument nothing else moves.
    EXPECT_GT(d1, d2)
        << "no contraction: A=" << a[0] << "," << a[1] << "," << a[2];
    // Ratio reported for the record (validator: pin only from measurement).
    if (d2 > 0.0)
        std::printf("[ INSTR    ] washout contraction ratio d1/d2 = %.3f, "
                    "|a0-a2| = %.3f\n", d1 / d2, std::abs(a[0] - a[2]));
    // (b) The coarse answer is already close, on the deck's own scale.
    // MEASURED (X3a round): correct form 19.057; the smallest defective
    // form this band must catch is full-qual_vol_in-per-substep (X3a
    // falsifier v) at 27.206. Band at the geometric mean, 22.8. The other
    // constituents are caught elsewhere: stale release (X2.ii) rails the
    // liveness ASSERT at 80, a hardwired nsub=1 degenerates the ladder to
    // EXPECT_GT(0,0), and the stale passthrough (X2.vi) is the orifice
    // leg's row below.
    EXPECT_LT(std::abs(a[0] - a[2]), 22.8)
        << "coarse-step error is a large fraction of the source spread";

    // Orifice leg — X2.vi's observer. The stale-passthrough defect is
    // invisible to a conduit-only deck (the passthrough branch never runs);
    // with O3 in the chain the washout front crosses it and a stale
    // delivery both delays the front and widens the ladder spread.
    DeckSpec so;
    so.orifice = true;
    so.end_time = "00:15:00";
    double ao[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < 3; ++i) {
        DeckSpec si = so;
        si.quality_step = ladder[i];
        const DtRun r = run_deck("_ld_wash_o", si);
        ASSERT_TRUE(r.ok) << "orifice dtq=" << ladder[i];
        ao[i] = r.outfall_conc;
    }
    ASSERT_GT(ao[2], 0.02 * kCinit) << "orifice washout finished";
    ASSERT_LT(ao[2], 0.98 * kCinit) << "orifice washout never started";
    const double od1 = std::abs(ao[0] - ao[1]);
    const double od2 = std::abs(ao[1] - ao[2]);
    EXPECT_GT(od1, od2)
        << "no contraction: A=" << ao[0] << "," << ao[1] << "," << ao[2];
    if (od2 > 0.0)
        std::printf("[ INSTR    ] orifice contraction ratio d1/d2 = %.3f, "
                    "|a0-a2| = %.3f\n", od1 / od2, std::abs(ao[0] - ao[2]));
    // MEASURED (X3a round): correct form 8.178; stale passthrough (X2.vi,
    // falsifier ii) gives 14.502 with contraction weakening 3.506 -> 1.475.
    // Band at the geometric mean, 10.9.
    EXPECT_LT(std::abs(ao[0] - ao[2]), 10.9)
        << "orifice coarse-step error is large";
}

// ---------------------------------------------------------------------------
// Gate 2 — age washout converges under QUALITY_STEP refinement (X4.ii's
// constituent: age-after-transport carries a larger error coefficient).
// ---------------------------------------------------------------------------
TEST(LardDtReferenceTest, AgeConvergesUnderQualityStepRefinement) {
    DeckSpec s;
    s.water_age = true;
    double a[3] = {0.0, 0.0, 0.0};
    const int ladder[3] = {40, 20, 10};
    for (int i = 0; i < 3; ++i) {
        DeckSpec si = s;
        si.quality_step = ladder[i];
        const DtRun r = run_deck("_ld_agew", si);
        ASSERT_TRUE(r.ok) << "dtq=" << ladder[i];
        a[i] = r.outfall_age;
    }
    // Liveness: mid-washout between fresh (≈ elapsed) and seeded (3600+).
    ASSERT_GT(a[2], 300.0);
    ASSERT_LT(a[2], 4800.0);

    const double d1 = std::abs(a[0] - a[1]);
    const double d2 = std::abs(a[1] - a[2]);
    EXPECT_GT(d1, d2)
        << "no contraction: A=" << a[0] << "," << a[1] << "," << a[2];
    if (d2 > 0.0)
        std::printf("[ INSTR    ] age contraction ratio d1/d2 = %.3f, "
                    "|a0-a2| = %.3f\n", d1 / d2, std::abs(a[0] - a[2]));
    // MEASURED (X3a round): correct form 8.031 s; age-after-transport
    // (X4.ii, falsifier iv) gives 20.552 s with the contraction itself
    // inverting (2.534 -> 0.393). Band at the geometric mean, 12.8 s.
    EXPECT_LT(std::abs(a[0] - a[2]), 12.8);
}

// ---------------------------------------------------------------------------
// Gate 3 — the steady answer is QUALITY_STEP-INVARIANT.
// ---------------------------------------------------------------------------
TEST(LardDtReferenceTest, SteadyAnswerIsQualityStepInvariant) {
    DeckSpec base;
    base.washout = false;          // steady concentration source
    base.end_time = "04:00:00";
    DeckSpec sub = base;
    sub.quality_step = 5;          // 8 substeps per routing step

    const DtRun a = run_deck("_ld_inv_base", base);
    const DtRun b = run_deck("_ld_inv_sub", sub);
    ASSERT_TRUE(a.ok && b.ok);
    ASSERT_GT(a.outfall_conc, 0.9 * kCin) << "steady state not reached";

    // The steady FIXED POINT is substep-independent exactly (per-substep
    // external volume is qual_vol_in·frac = q_ext·dt, so the balance
    // c* = rate/q_ext is the same at any nsub). A finite run only reaches
    // the fixed point to ~1e-6, and the two trajectories approach it
    // differently, so the band is 1e-5 RELATIVE: an order above the
    // transient tail, four orders below the falsifier — consuming the
    // FULL qual_vol_in every substep moves the fixed point itself to
    // rate/(q_ext·nsub), a factor nsub=8 here.
    ASSERT_EQ(a.node_final.size(), b.node_final.size());
    for (std::size_t n = 0; n < a.node_final.size(); ++n)
        EXPECT_NEAR(a.node_final[n], b.node_final[n],
                    1.0e-5 * std::max(1.0, a.node_final[n]))
            << "steady node " << n << " moved with QUALITY_STEP";
}

// ---------------------------------------------------------------------------
// Gate 4 — reverse flow: the reversal happens, conserves, and stays bounded.
// ---------------------------------------------------------------------------
TEST(LardDtReferenceTest, ReverseFlowConservesAndStaysBounded) {
    DeckSpec lard;
    lard.reverse = true;
    lard.washout = false;          // source at J0
    lard.end_time = "02:00:00";
    lard.routing_step = 5;
    DeckSpec leg = lard;  leg.lard = false;

    const DtRun a = run_deck("_ld_rev_lard", lard);
    const DtRun b = run_deck("_ld_rev_leg", leg);
    ASSERT_TRUE(a.ok && b.ok);

    // Premise, on the CONTROL: the deck must actually reverse and then
    // recover forward drainage — otherwise this gate observes nothing and
    // the deck needs tuning, not the engine.
    ASSERT_LT(b.min_flow_c2, -1.0e-3)
        << "the LEGACY control never reversed — deck premise broken";
    ASSERT_GT(b.final_flow_c2, 0.0)
        << "the LEGACY control never recovered forward flow";
    // And the engine under test saw the same hydraulics (bit-identity is
    // the wiring suite's gate; here direction suffices).
    ASSERT_LT(a.min_flow_c2, -1.0e-3);

    // The claims: finite everywhere, bounded by the only source, and the
    // ledger closes. The boundary inflow carries concentration 0, so kCin
    // remains the network's maximum.
    EXPECT_TRUE(a.finite) << "state went non-finite under reversal";
    EXPECT_LE(a.peak_link, kCin * (1.0 + 1.0e-9));
    EXPECT_LE(a.peak_node, kCin * (1.0 + 1.0e-9));

    const double in = a.mb_init + a.mb_ex_in;
    const double out = a.mb_outflow + a.mb_final;
    ASSERT_GT(in, 0.0);
    std::printf("[ INSTR    ] reverse-flow ledger out/in = %.6f\n", out / in);
    // MEASURED (X3a round): out/in = 1.003449 on this deck — the reversal
    // transient books ~0.34% (the X2 T3 transient-residual family; both
    // engines carry one). Band at 1.5%: 4.4x the measured closure error,
    // 3.3x below the handoff's 5% refusal line.
    EXPECT_NEAR(out / in, 1.0, 0.015)
        << "ledger broke under reversal: in=" << in << " out=" << out;
}

// ---------------------------------------------------------------------------
// Gate 5 — the ROUTING_STEP axis: X2.viii's observer, at last
// (closeout P1.1).
//
// X2.viii — the node mix reading nodes.volume instead of old_volume — is
// dtq-INDEPENDENT by construction: volumes advance once per routing step,
// so gates 1–3 cannot see it (X3a measured the defect moving only the
// LIMIT, 19.057 → 17.952, with the dtq-ladder spread unchanged). The
// rs axis is where it lives — but the closeout plan's proposed form,
// contraction of the raw ladder |A(40)−A(20)| > |A(20)−A(10)|, is NOT
// achievable, and that is a MEASURED finding, not a guess:
//
//   · Raw A does not contract, clean: the point value expands (ratio
//     0.899 — the O(rs) hydraulic phase shift amplified by the front's
//     slope), and the integral oscillates (gaps 3232/463/1261 — the
//     dynamic-wave solution's own rs-dependence is not smooth).
//   · The lard-vs-legacy difference D removes the hydraulic drift
//     (common mode: identical routing under either QUALITY_SOLVER), but
//     D contracts toward a STRUCTURAL limit, not zero — legacy's CSTR
//     chain breaks through instantly while parcels arrive late, so |D|
//     legitimately GROWS as refinement strips the noise: measured
//     −114 → −129 → −225 → −316 clean.
//
// What discriminates is the LEVEL of D where the defect is largest — the
// coarsest rung, where old-vs-new volume differ most. The gate is
// therefore a band on D(80), pinned correct-form vs X2.viii-form.
// ---------------------------------------------------------------------------
TEST(LardDtReferenceTest, StorageFillLardVsLegacyGapBoundedOnRsLadder) {
    // Instrument design, all of it measured this round rather than assumed:
    //   · The washout deck cannot carry this gate — X2.viii's error term is
    //     the per-step volume change at the mixing node, and junction
    //     volumes are MIN_SURFAREA residue: the defect measured at ~1/20 of
    //     the ladder's own hydraulic noise (shift +186 at rs=80 against
    //     gaps of 400–3200). The deck here starts DRY and fills a STORAGE
    //     unit (volume that is state, not residue) from a steady
    //     concentration source: defect shift −324/−314/−243/−158 across
    //     the ladder — rs-dependent, exactly the signature the dtq axis
    //     could not see.
    //   · The raw observable still cannot be gated — refining rs moves the
    //     fill hydrograph itself (A spans 8336 → 12183, 12–24× the defect).
    //     So each rung is paired against the LEGACY control on the SAME
    //     deck: hydraulics are engine-independent, the drift is common
    //     mode, and the difference D(rs) = lard − legacy carries only the
    //     transport-scheme gap, which must shrink with rs.
    //   · D is built from an engine-neutral integral (∫ conc·|q| dt on C5,
    //     same formula both engines), not the ledger, whose rows the two
    //     families do not book identically.
    // 17:20 = 1040 s divides by every rung — a horizon one rung overshoots
    // and another truncates is an artifact the ladder would book as error.
    double dd[4] = {0.0, 0.0, 0.0, 0.0};
    double a_lard[4], a_leg[4];
    double liveness = 0.0;
    const int ladder[4] = {80, 40, 20, 10};
    for (int i = 0; i < 4; ++i) {
        DeckSpec si;              // QUALITY_STEP omitted → dtq = rs
        si.washout = false;       // steady CONCEN source at J0
        si.dry_start = true;      // fill transient: dV/dt large
        si.storage = true;        // J2's volume is state, not residue
        si.end_time = "00:17:20";
        si.routing_step = ladder[i];
        DeckSpec sl = si;
        sl.lard = false;
        const DtRun r = run_deck("_ld_fill_rs", si);
        const DtRun c = run_deck("_ld_fill_rs_leg", sl);
        ASSERT_TRUE(r.ok && c.ok) << "rs=" << ladder[i];
        a_lard[i] = r.mass_out;
        a_leg[i] = c.mass_out;
        dd[i] = r.mass_out - c.mass_out;
        liveness = r.outfall_conc;
    }
    // Liveness: mass must actually be discharging by the horizon and the
    // front must have arrived — a horizon before arrival observes nothing.
    ASSERT_GT(a_lard[3], 0.0) << "nothing discharged — lengthen the run";
    ASSERT_GT(liveness, 0.02 * kCin) << "front never arrived";

    std::printf("[ INSTR    ] rs lard   A = %.3f %.3f %.3f %.3f\n",
                a_lard[0], a_lard[1], a_lard[2], a_lard[3]);
    std::printf("[ INSTR    ] rs legacy A = %.3f %.3f %.3f %.3f\n",
                a_leg[0], a_leg[1], a_leg[2], a_leg[3]);
    std::printf("[ INSTR    ] rs D = %.3f %.3f %.3f %.3f  |D80/D10| = %.3f\n",
                dd[0], dd[1], dd[2], dd[3],
                dd[3] != 0.0 ? std::abs(dd[0] / dd[3]) : 0.0);
    // The razor. MEASURED (closeout P1.1 round): correct form D(80) =
    // −114.4; X2.viii applied (nodes.volume for old_volume in the node
    // mix) gives −329.8 — the defect loses arriving mass into the
    // volume it double-counts, hardest where the per-step volume change
    // is largest. Band at the geometric mean, 194. The finest rung
    // (clean −316.3 vs defective −422.5) is reported above but NOT
    // gated: its ±15% margins are inside plausible cross-platform FP
    // drift, and a flaky razor is worse than none.
    EXPECT_LT(std::abs(dd[0]), 194.0)
        << "the lard-vs-legacy gap at the coarsest step is outside the "
           "measured band — the X2.viii family (wrong mix volume) "
           "produces exactly this signature";
}

}  // namespace
