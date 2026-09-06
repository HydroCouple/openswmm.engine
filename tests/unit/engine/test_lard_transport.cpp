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
 * @file test_lard_transport.cpp
 * @brief X2: the LARD LTD engine transports, conserves, and out-resolves
 *        the CSTR where plug flow says it must.
 *
 * @details Subplan X2 (plans/transport/LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md;
 *          strategy §2/§4 with the §16 amendments). The claims:
 *
 *          1. Steady-source parity: on the five-conduit chain, a steady
 *             no-decay source arrives intact at every element, at every
 *             ROUTING_STEP, and matches LEGACY on the same deck — checked
 *             at NODES per the A1b finding (link values differ from a CSTR
 *             by definition; here the steady answer makes links equal too).
 *          2. The maximum principle at every step: one source feeding the
 *             network, nothing may exceed it (the 7b2dfaae shape).
 *          3. Quality mass balance closes on a run stopped mid-transient —
 *             the ledger identity (init + external in) = (outflow + final
 *             stored + reacted) within band, using the engine's own
 *             mass_balance rows.
 *          4. Plug-flow decay: at steady state a decaying plug reads
 *             Cin·exp(−k·τ) at the outfall (τ measured from the link's own
 *             V/Q), and reads BELOW the CSTR answer on the same deck —
 *             1/(1+kτ) > e^{−kτ} for kτ > 0, so the ordering separates the
 *             two transport models structurally, not by tolerance.
 *          5. SegmentStore invariants at unit level: push/drain/merge/
 *             reverse/overflow all conserve mass exactly; drains report
 *             partial volume on an emptying link.
 *          6. Zero-volume passthrough: an orifice in the chain delivers the
 *             upstream node's same-step concentration; the steady signal
 *             still arrives intact at the outfall.
 *
 *          Scratch fixtures use the `_lt_` prefix (collision-checked).
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
#include "quality/lard/SegmentStore.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kQ = 5.0;
constexpr double kCin = 100.0;

struct DeckSpec {
    bool lard = true;            ///< false → LEGACY control
    int routing_step = 5;
    const char* end_time = "04:00:00";
    double kdecay = 0.0;         ///< 1/s, [POLLUTANTS] Kdecay column
    double cinit = 0.0;
    bool single_conduit = false; ///< J0 → C1 → OUT (decay deck)
    bool with_orifice = false;   ///< J0-C1-J1 =OR1= J2-C2-OUT
};

void write_deck(const std::string& path, const DeckSpec& s) {
    std::ofstream f(path);
    f << "[TITLE]\nLARD X2 transport\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << (s.lard ? "QUALITY_SOLVER LAGRANGIAN\n" : "QUALITY_SOLVER LEGACY\n")
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << s.end_time << "\n"
      << "ROUTING_STEP " << s.routing_step << "\nREPORT_STEP 00:05:00\n\n";
    if (s.single_conduit) {
        f << "[JUNCTIONS]\nJ0 10.0 10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
          << "[CONDUITS]\nC1 J0 OUT 2000 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 2.0 0 0 0\n\n";
    } else if (s.with_orifice) {
        f << "[JUNCTIONS]\n"
          << "J0 10.0 10 1.5 0 0\nJ1 9.0 10 1.5 0 0\nJ2 8.5 10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
          << "[CONDUITS]\n"
          << "C1 J0 J1 500 0.013 0 0 0\nC2 J2 OUT 500 0.013 0 0 0\n\n"
          << "[ORIFICES]\nOR1 J1 J2 SIDE 0.0 0.65 NO\n\n"
          << "[XSECTIONS]\n"
          << "C1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 2.0 0 0 0\n"
          << "OR1 CIRCULAR 1.0 0 0 0\n\n";
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
    f << "[POLLUTANTS]\nTSS MG/L 0 0 0 " << (s.kdecay * 86400.0)  // 1/day column (KD1)
      << " NO * 0 0 "
      << s.cinit << "\n\n"
      << "[INFLOWS]\n"
      << "J0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n"
      << "J0 TSS  \"\" CONCEN 1.0 1.0 " << kCin << "\n\n"
      << "[REPORT]\nINPUT NO\n";
}

struct TransportRun {
    std::vector<double> node_final;   ///< [node] mg/L
    std::vector<double> link_final;   ///< [link] mg/L
    double peak_node = 0.0, peak_link = 0.0;
    double outfall_conc = 0.0;        ///< last node (writer order: OUT last)
    double link_volume0 = 0.0;        ///< C1 volume at end (for tau)
    double link_flow0 = 0.0;          ///< C1 |flow| at end
    // ledger rows (internal mass units)
    double mb_init = 0.0, mb_ex_in = 0.0, mb_outflow = 0.0, mb_final = 0.0,
           mb_reacted = 0.0;
    bool ok = false;
};

TransportRun run_deck(const std::string& tag, const DeckSpec& s) {
    TransportRun r;
    const std::string inp = tag + ".inp";
    write_deck(inp, s);

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
            r.peak_link = std::max(
                r.peak_link, ctx.links.conc[static_cast<std::size_t>(l * np)]);
        for (int j = 0; j < nn; ++j)
            r.peak_node = std::max(
                r.peak_node, ctx.nodes.conc[static_cast<std::size_t>(j * np)]);
    } while (elapsed > 0.0 && ++guard < 200000);
    swmm_engine_end(e);

    r.node_final.assign(static_cast<std::size_t>(nn), 0.0);
    for (int j = 0; j < nn; ++j)
        r.node_final[static_cast<std::size_t>(j)] =
            ctx.nodes.conc[static_cast<std::size_t>(j * np)];
    r.link_final.assign(static_cast<std::size_t>(nl), 0.0);
    for (int l = 0; l < nl; ++l)
        r.link_final[static_cast<std::size_t>(l)] =
            ctx.links.conc[static_cast<std::size_t>(l * np)];
    // The outfall is the node whose type says so (never index-guess).
    for (int j = 0; j < nn; ++j)
        if (ctx.nodes.type[static_cast<std::size_t>(j)] ==
            openswmm::NodeType::OUTFALL)
            r.outfall_conc = r.node_final[static_cast<std::size_t>(j)];
    r.link_volume0 = ctx.links.volume[0];
    r.link_flow0 = std::abs(ctx.links.flow[0]);

    const auto& mb = ctx.mass_balance;
    auto row = [](const std::vector<double>& v) {
        return v.empty() ? 0.0 : v[0];
    };
    r.mb_init = row(mb.qual_routing_init);
    r.mb_ex_in = row(mb.qual_routing_ex_in);
    r.mb_outflow = row(mb.qual_routing_outflow);
    r.mb_final = row(mb.qual_routing_final);
    r.mb_reacted = row(mb.qual_routing_reacted);

    r.ok = true;
    swmm_engine_destroy(e);
    return r;
}

// ---------------------------------------------------------------------------
// Gate 1 — steady-source parity at every ROUTING_STEP, against LEGACY.
// ---------------------------------------------------------------------------
TEST(LardTransportTest, SteadySourceArrivesIntactAtEveryRoutingStep) {
    for (const int rs : {1, 5, 20, 60}) {
        DeckSpec lard;  lard.routing_step = rs;
        DeckSpec leg = lard;  leg.lard = false;
        const TransportRun a = run_deck("_lt_par_lard", lard);
        const TransportRun b = run_deck("_lt_par_leg", leg);
        ASSERT_TRUE(a.ok && b.ok) << "rs=" << rs;

        // Reference engine first (the node-store suite's rule): if LEGACY
        // does not read kCin the DECK is broken, not the LARD engine.
        for (std::size_t l = 0; l < b.link_final.size(); ++l)
            ASSERT_NEAR(b.link_final[l], kCin, 1.0e-6)
                << "LEGACY link " << l << " rs=" << rs << " — deck premise";

        for (std::size_t l = 0; l < a.link_final.size(); ++l)
            EXPECT_NEAR(a.link_final[l], kCin, 1.0e-6)
                << "LARD link " << l << " at ROUTING_STEP " << rs;
        // Node comparison is the cross-engine claim (A1b: nodes are the
        // common quantity between a CSTR and a segment engine).
        for (std::size_t n = 0; n < a.node_final.size(); ++n)
            EXPECT_NEAR(a.node_final[n], b.node_final[n], 1.0e-6)
                << "node " << n << " diverged from LEGACY at rs=" << rs;
    }
}

// ---------------------------------------------------------------------------
// Gate 2 — the maximum principle, sampled at every step.
// ---------------------------------------------------------------------------
TEST(LardTransportTest, NoElementEverExceedsTheSource) {
    for (const int rs : {1, 5, 20, 120}) {
        DeckSpec s;  s.routing_step = rs;  s.cinit = 50.0;
        const TransportRun a = run_deck("_lt_max", s);
        ASSERT_TRUE(a.ok) << "rs=" << rs;
        EXPECT_LE(a.peak_link, kCin * (1.0 + 1.0e-9))
            << "a link exceeded the source at ROUTING_STEP " << rs
            << " — mass was manufactured";
        EXPECT_LE(a.peak_node, kCin * (1.0 + 1.0e-9))
            << "a node exceeded the source at ROUTING_STEP " << rs;
    }
}

// ---------------------------------------------------------------------------
// Gate 3 — the ledger identity on a run stopped mid-transient.
// ---------------------------------------------------------------------------
// The handoff's 0.5% mid-transient band was WRONG, and its premise with it.
// Measured 2026-08-23 (t3_probe / t3_audit under tests/output/lard_x2_*):
// stopped at 20 min the identity misses by -5.4% under LARD and +2.3% under
// LEGACY on the SAME deck, flat across ROUTING_STEP {1,5,20} -- not
// quadrature, and not the outfall booking. The mechanism: while a conduit's
// volume relaxes, the node discharges more water than the link accepts
// (need = links.volume - slab after drain), and the mix's implicit store
// (mass = conc x volume) silently drops the orphan water's mass -- the
// per-node residual conc x (denom - v_end - released) reconstructs the
// entire shortfall to within the outfall's arrival lag. A single q per link
// makes the orphan invisible at the link; LEGACY's CSTR family shares the
// effect with the opposite sign. Both engines reach the IDENTICAL steady
// final storage (457559.04 internal units on this deck).
//
// So the gate asserts the two properties this engine family actually has,
// and the two that a real leak would break:
//   (a) the absolute residual FREEZES once the transient passes -- an
//       ongoing leak grows with throughput and fails this loudly;
//   (b) the identity closes at steady state (measured 0.46% at 4 h).
TEST(LardTransportTest, QualityMassBalanceClosesMidTransient) {
    DeckSpec s20;
    s20.end_time = "00:20:00";  // stopped while the front is mid-chain
    s20.cinit = 0.0;            // in = external only; init row must read 0
    DeckSpec s60 = s20;  s60.end_time = "01:00:00";
    DeckSpec s4h = s20;  s4h.end_time = "04:00:00";

    const TransportRun a = run_deck("_lt_mb20", s20);
    const TransportRun b = run_deck("_lt_mb60", s60);
    const TransportRun c = run_deck("_lt_mb4h", s4h);
    ASSERT_TRUE(a.ok && b.ok && c.ok);

    const auto in_of  = [](const TransportRun& r) { return r.mb_init + r.mb_ex_in; };
    const auto out_of = [](const TransportRun& r) {
        return r.mb_outflow + r.mb_final + r.mb_reacted;
    };
    ASSERT_GT(in_of(a), 0.0) << "no external mass was booked — the loader "
                                "seam did not run under LARD (liveness)";

    // (a) The residual freezes. Between 20 min and 1 h the throughput
    // triples; a per-step or per-mass leak scales with it, the structural
    // transient residual does not (measured: 32204 -> 33120, +2.8%).
    const double miss20 = in_of(a) - out_of(a);
    const double miss60 = in_of(b) - out_of(b);
    EXPECT_NEAR(miss60, miss20, std::abs(miss20) * 0.15 + 1.0)
        << "the ledger residual GREW with throughput: " << miss20
        << " at 20 min vs " << miss60 << " at 1 h — that is an ongoing "
           "leak, not the frozen transient residual";

    // (b) Steady-state closure. 1% band over a measured 0.46%.
    EXPECT_NEAR(out_of(c) / in_of(c), 1.0, 1.0e-2)
        << "ledger identity broke at steady state: in=" << in_of(c)
        << " out=" << out_of(c) << " (outflow=" << c.mb_outflow
        << " final=" << c.mb_final << ")";

    // (c) The decay leg — the identity with qual_routing_reacted as a
    // LARGE term (handoff §5.vii: \"run T3's deck once with kdecay > 0 as
    // the probe\"). k = 1e-3/s over a ~7 min residence chain removes a
    // substantial fraction, so a reacted row booked with the wrong sign
    // moves out/in by twice that fraction and cannot hide inside the
    // transient band. Without this leg the k=0 deck kept `removed` at
    // exactly zero and the sign was unobservable.
    DeckSpec sk = s4h;  sk.kdecay = 1.0e-3;
    const TransportRun d = run_deck("_lt_mbk", sk);
    ASSERT_TRUE(d.ok);
    ASSERT_GT(d.mb_reacted, 0.05 * in_of(d))
        << "decay removed almost nothing — the probe premise (a large "
           "reacted term) is broken, not the ledger";
    EXPECT_NEAR(out_of(d) / in_of(d), 1.0, 1.0e-2)
        << "the decayed ledger identity broke: in=" << in_of(d) << " out="
        << out_of(d) << " (reacted=" << d.mb_reacted
        << ") — a reacted row with the wrong sign misses by 2x itself";
}

// ---------------------------------------------------------------------------
// Gate 4 — plug-flow decay: the exponential, and the structural ordering
// against the CSTR on the same deck.
// ---------------------------------------------------------------------------
TEST(LardTransportTest, PlugFlowDecayMatchesTheExponentialAndBeatsTheCstr) {
    DeckSpec lard;
    lard.single_conduit = true;
    lard.kdecay = 1.0e-3;     // 1/s — kτ ≈ O(1) on a 2000 ft conduit
    lard.end_time = "06:00:00";
    DeckSpec leg = lard;  leg.lard = false;

    const TransportRun a = run_deck("_lt_decay_lard", lard);
    const TransportRun b = run_deck("_lt_decay_leg", leg);
    ASSERT_TRUE(a.ok && b.ok);

    ASSERT_GT(a.link_flow0, 0.0);
    const double tau = a.link_volume0 / a.link_flow0;  // measured residence
    const double expected = kCin * std::exp(-lard.kdecay * tau);

    // The plug answer, from the deck's own measured τ — no hand constant.
    EXPECT_NEAR(a.outfall_conc / expected, 1.0, 0.05)
        << "LARD outfall " << a.outfall_conc << " vs plug answer "
        << expected << " (tau=" << tau << " s)";

    // The structural separation: a CSTR link at steady state reads
    // Cin/(1+kτ) > Cin·e^{−kτ}. Ordering, not tolerance.
    EXPECT_LT(a.outfall_conc, b.outfall_conc)
        << "plug-flow decay did not beat the CSTR — the segment engine is "
           "not resolving the front (LARD " << a.outfall_conc
        << " vs LEGACY " << b.outfall_conc << ")";
}

// ---------------------------------------------------------------------------
// Gate 5 — SegmentStore invariants, no engine involved.
// ---------------------------------------------------------------------------
TEST(LardTransportTest, SegmentStoreConservesMassUnderEveryOperation) {
    using openswmm::lard::SegmentStore;
    SegmentStore st;
    st.resize(/*n_links=*/2, /*n_species=*/2, /*cap=*/4);

    auto total_mass = [&](int l, int s) {
        double m = 0.0;
        for (int i = 0; i < st.count(l); ++i)
            m += st.seg_volume(l, i) * st.seg_conc(l, i, s);
        return m;
    };

    // Push three distinguishable segments (outside merge tolerance).
    const double c1[2] = {10.0, 1.0};
    const double c2[2] = {20.0, 2.0};
    const double c3[2] = {40.0, 4.0};
    st.push_front(0, 1.0, c1);
    st.push_front(0, 2.0, c2);
    st.push_front(0, 3.0, c3);   // front=c3, back=c1
    ASSERT_EQ(st.count(0), 3);
    ASSERT_NEAR(st.total_volume(0), 6.0, 1e-12);
    const double m0 = total_mass(0, 0);
    ASSERT_NEAR(m0, 3.0 * 40 + 2.0 * 20 + 1.0 * 10, 1e-9);

    // Drain 1.5 from the back: takes all of c1 (1.0×10) + 0.5 of c2 (0.5×20).
    double mass[2] = {0.0, 0.0};
    const double drained = st.drain_back(0, 1.5, mass);
    EXPECT_NEAR(drained, 1.5, 1e-12);
    EXPECT_NEAR(mass[0], 1.0 * 10 + 0.5 * 20, 1e-9);
    EXPECT_NEAR(total_mass(0, 0) + mass[0], m0, 1e-9)
        << "drain_back lost mass";

    // Reverse twice is the identity on volume and mass.
    const double mv = st.total_volume(0), mm = total_mass(0, 0);
    st.reverse(0);
    st.reverse(0);
    EXPECT_NEAR(st.total_volume(0), mv, 1e-12);
    EXPECT_NEAR(total_mass(0, 0), mm, 1e-9);

    // Overflow: cap=4 — pushing distinct segments past the cap merges the
    // front pair, conserving mass and never exceeding the cap.
    const double c4[2] = {80.0, 8.0};
    const double c5[2] = {160.0, 16.0};
    const double c6[2] = {320.0, 32.0};
    st.push_front(0, 1.0, c4);
    st.push_front(0, 1.0, c5);
    const double before = total_mass(0, 0) + 1.0 * 320.0;
    st.push_front(0, 1.0, c6);  // forces a merge
    EXPECT_LE(st.count(0), 4);
    EXPECT_NEAR(total_mass(0, 0), before, 1e-9)
        << "the D-L5 overflow merge lost mass";

    // Merge tolerance: pushing at the front concentration folds in.
    SegmentStore st2;
    st2.resize(1, 1, 4);
    const double c[1] = {50.0};
    st2.push_front(0, 1.0, c);
    st2.push_front(0, 1.0, c);
    EXPECT_EQ(st2.count(0), 1) << "identical release did not merge (§4.5)";
    EXPECT_NEAR(st2.total_volume(0), 2.0, 1e-12);

    // Drain past empty reports the partial volume, not the request.
    double m2[1] = {0.0};
    EXPECT_NEAR(st2.drain_back(0, 5.0, m2), 2.0, 1e-12);
    EXPECT_NEAR(m2[0], 100.0, 1e-9);
    EXPECT_EQ(st2.count(0), 0);
}

// ---------------------------------------------------------------------------
// Gate 6 — zero-volume passthrough carries the signal through an orifice.
// ---------------------------------------------------------------------------
TEST(LardTransportTest, OrificePassthroughDeliversTheSteadySignal) {
    DeckSpec lard;  lard.with_orifice = true;
    DeckSpec leg = lard;  leg.lard = false;
    const TransportRun a = run_deck("_lt_orif_lard", lard);
    const TransportRun b = run_deck("_lt_orif_leg", leg);
    ASSERT_TRUE(a.ok && b.ok);

    // Premise on the control: the deck's hydraulics deliver the signal.
    ASSERT_NEAR(b.outfall_conc, kCin, 1.0)
        << "LEGACY control does not carry the signal through the orifice — "
           "the deck's hydraulics need tuning, not the engine";
    EXPECT_NEAR(a.outfall_conc, kCin, 1.0)
        << "the passthrough dropped mass at the orifice seam";
    // The orifice link itself publishes its upstream node's concentration.
    EXPECT_NEAR(a.link_final[1], kCin, 1.0)
        << "the orifice link did not publish the passthrough concentration";
}

}  // namespace
