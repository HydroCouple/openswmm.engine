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
 * @file test_lard_rwpt.cpp
 * @brief X3b: RWPT dispersion — resolved shear spreads fronts, keyed
 *        deterministically, conserving mass under the maximum principle.
 *
 * @details Subplan X3b (strategy §5, D-X3b1 in RwptDispersion.hpp). Claims:
 *
 *          1. A breakthrough front SPREADS under RWPT where the plug engine
 *             keeps it sharp — the structural liveness (σ_on vs σ_off) —
 *             and the emergent D_L sits near Elder's 5.93·u*·h, the
 *             vertical-shear-only reference that matches exactly what the
 *             kernel resolves. The Elder band is a ⚠ PLACEHOLDER (log-
 *             space factor 3) until the validating round measures it.
 *          2. D-L6 determinism: same RWPT_SEED ⇒ bit-identical traces;
 *             a different seed moves them (the liveness half).
 *          3. Mass conserves and the maximum principle holds under RWPT —
 *             by the D-X3b1 limiter's construction, so the gate is strict.
 *          4. The profile/RNG/geometry kernels obey their invariants at
 *             unit level (mean-free deviations, Rouse endpoints, drift =
 *             dD/dη, R_h known values) — no engine run involved.
 *          5. RWPT OFF (or absent) is bit-inert — the existing three LARD
 *             suites are the standing observers; this gate pins the
 *             DISPERSION OFF spelling specifically.
 *
 *          Scratch fixtures use the `_lr_` prefix (collision-checked).
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
#include "quality/lard/RwptDispersion.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kQ = 5.0;
constexpr double kCin = 100.0;

struct DeckSpec {
    bool rwpt = true;
    int seed = 7;
    bool explicit_off = false;   ///< write "DISPERSION OFF" instead of omit
    const char* end_time = "02:00:00";
    bool chain = false;          ///< 5-conduit chain (conservation deck)
    double cinit = 0.0;
    int length = 3000;           ///< single-conduit reach, ft
    bool delayed = false;        ///< step injection at t=1000 s (see gate 1)
    int dtq = 0;                 ///< QUALITY_STEP seconds; 0 = omit
};

void write_deck(const std::string& path, const DeckSpec& s) {
    std::ofstream f(path);
    f << "[TITLE]\nLARD X3b rwpt\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "QUALITY_SOLVER LAGRANGIAN\n";
    if (s.rwpt) f << "DISPERSION RWPT\nRWPT_SEED " << s.seed << "\n";
    else if (s.explicit_off) f << "DISPERSION OFF\n";
    if (s.dtq > 0) f << "QUALITY_STEP " << s.dtq << "\n";
    f << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << s.end_time << "\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:05:00\n\n";
    if (s.chain) {
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
    } else {
        f << "[JUNCTIONS]\nJ0 10.0 10 1.5 0 0\n\n"
          << "[OUTFALLS]\nOUT 5.0 FREE  NO\n\n"
          << "[CONDUITS]\nC1 J0 OUT " << s.length << " 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 2.0 0 0 0\n\n";
    }
    f << "[POLLUTANTS]\nTSS MG/L 0 0 0 0 NO * 0 0 " << s.cinit << "\n\n";
    if (s.delayed) {
        // The step waits out the startup hydraulic transient: the first
        // ~150 s of volume settling shed merge-scale parcels that read as
        // a ~47 s staircase at the outfall and swamped every width metric
        // (measured, X3b round). At steady hydraulics the OFF front stays
        // ~5 s sharp over 6000 ft and the instrument works.
        f << "[TIMESERIES]\n"
          << "STEP 0:00 0\nSTEP 0:16:40 0\nSTEP 0:16:41 " << kCin << "\n"
          << "STEP 9:00 " << kCin << "\n\n"
          << "[INFLOWS]\n"
          << "J0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n"
          << "J0 TSS STEP CONCEN 1.0 1.0 0\n\n";
    } else {
        f << "[INFLOWS]\n"
          << "J0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n"
          << "J0 TSS  \"\" CONCEN 1.0 1.0 " << kCin << "\n\n";
    }
    f << "[REPORT]\nINPUT NO\n";
}

struct RwptRun {
    std::vector<double> t;        ///< seconds
    std::vector<double> cout;     ///< outfall conc per step
    double peak_node = 0.0, peak_link = 0.0;
    double mb_init = 0.0, mb_ex_in = 0.0, mb_outflow = 0.0, mb_final = 0.0;
    double ubar = 0.0, h = 0.0, rh = 0.0;  ///< C1 end-state hydraulics
    bool ok = false;
};

RwptRun run_deck(const std::string& tag, const DeckSpec& s) {
    RwptRun r;
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
    int out_node = -1;
    for (int j = 0; j < nn; ++j)
        if (ctx.nodes.type[static_cast<std::size_t>(j)] ==
            openswmm::NodeType::OUTFALL)
            out_node = j;
    if (out_node < 0) {
        ADD_FAILURE() << "no outfall in deck";
        swmm_engine_destroy(e);
        return r;
    }

    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) {
            ADD_FAILURE() << "step failed for " << inp;
            swmm_engine_destroy(e);
            return r;
        }
        r.t.push_back(elapsed * 86400.0);
        r.cout.push_back(
            ctx.nodes.conc[static_cast<std::size_t>(out_node * np)]);
        for (int l = 0; l < nl; ++l)
            r.peak_link = std::max(
                r.peak_link, ctx.links.conc[static_cast<std::size_t>(l * np)]);
        for (int j = 0; j < nn; ++j)
            r.peak_node = std::max(
                r.peak_node, ctx.nodes.conc[static_cast<std::size_t>(j * np)]);
    } while (elapsed > 0.0 && ++guard < 200000);
    swmm_engine_end(e);

    // C1 end-state hydraulics for the Elder reference.
    {
        const int row = ctx.link_subtypes.conduit_row(0);
        const double len =
            (row >= 0)
                ? ctx.link_subtypes.conduits.length[static_cast<std::size_t>(
                      row)]
                : 0.0;
        const double vol = ctx.links.volume[0];
        const double q = std::abs(ctx.links.flow[0]);
        if (len > 0.0 && vol > 0.0 && q > 0.0) {
            const double a = vol / len;
            r.ubar = q / a;
            r.h = ctx.links.depth[0];
            r.rh = openswmm::lard::rwpt_hyd_radius(
                a, r.h, ctx.links.xsect_geom1[0], true);
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
    r.ok = true;
    swmm_engine_destroy(e);
    return r;
}

/// Truncated rising-mass temporal moments of a breakthrough curve: the
/// variance of dC-weighted arrival times, truncated at the first 99.5%
/// crossing so end-of-run MC jitter (tiny late rises at large (t-tbar)^2)
/// cannot dominate. Returns {tbar, var}.
std::pair<double, double> rise_moments(const RwptRun& r) {
    std::size_t end = r.cout.size();
    for (std::size_t i = 0; i < r.cout.size(); ++i)
        if (r.cout[i] >= 0.995 * kCin) { end = i + 1; break; }
    double w = 0.0, m1 = 0.0;
    for (std::size_t i = 1; i < end; ++i) {
        const double dc = r.cout[i] - r.cout[i - 1];
        if (dc <= 0.0) continue;
        w += dc;
        m1 += dc * 0.5 * (r.t[i] + r.t[i - 1]);
    }
    if (w <= 0.0) return {0.0, 0.0};
    const double tbar = m1 / w;
    double m2 = 0.0;
    for (std::size_t i = 1; i < end; ++i) {
        const double dc = r.cout[i] - r.cout[i - 1];
        if (dc <= 0.0) continue;
        const double tm = 0.5 * (r.t[i] + r.t[i - 1]) - tbar;
        m2 += dc * tm * tm;
    }
    return {tbar, m2 / w};
}

// ---------------------------------------------------------------------------
// Gate 1 — the front spreads, and the emergent D_L sits near Elder.
// ---------------------------------------------------------------------------
TEST(LardRwptTest, FrontSpreadsAndEmergentDispersionNearsElder) {
    // Redesigned in the X3b round from the immediate-injection 3000 ft
    // form, for measured reasons:
    // - the startup hydraulic transient sheds merge-scale parcels that
    //   arrive as a ~47 s staircase and swamp any width metric (16/84
    //   quantiles were plateau-vs-level coin flips: sigma_off read 47 s
    //   or 25 s depending on whether one plateau sat above 84). The step
    //   now waits 1000 s at steady hydraulics; the OFF front is ~5 s.
    // - QUALITY_STEP 1 puts the eta-walk in the resolved-mixing-time
    //   regime (T_mix ~ h^2/D_t ~ 80 s; at dt = 5 s the walk teleports
    //   across the depth and the emergent D is a dt artifact).
    // - the width is the truncated rising-mass second moment, and the
    //   RWPT contribution is the EXCESS variance over the OFF control
    //   (variances of independent broadenings add).
    DeckSpec on;
    on.delayed = true;
    on.length = 6000;
    on.dtq = 1;
    on.end_time = "03:00:00";
    DeckSpec off = on;  off.rwpt = false;

    const RwptRun a = run_deck("_lr_front_on", on);
    const RwptRun b = run_deck("_lr_front_off", off);
    ASSERT_TRUE(a.ok && b.ok);

    const auto [tbar_on, var_on] = rise_moments(a);
    const auto [tbar_off, var_off] = rise_moments(b);
    ASSERT_GT(tbar_on, 1000.0) << "RWPT breakthrough never arrived";
    ASSERT_GT(tbar_off, 1000.0) << "control breakthrough never arrived";

    const double sig_on = std::sqrt(var_on);
    const double sig_off = std::max(std::sqrt(var_off), 2.5);  // ≥ dt/2
    std::printf("[ RWPT     ] sigma_on=%.2f s  sigma_off=%.2f s\n",
                sig_on, sig_off);
    // Structural liveness: resolved shear must beat the plug engine's
    // numerical front width by a clear factor.
    EXPECT_GT(sig_on, 2.0 * sig_off)
        << "RWPT did not spread the front beyond the numerical width";

    // Emergent D_L from the EXCESS temporal variance vs Elder.
    ASSERT_GT(a.ubar, 0.0);
    const double x = 6000.0;
    const double var_ex = std::max(var_on - var_off, 0.0);
    const double d_meas =
        a.ubar * a.ubar * a.ubar * var_ex / (2.0 * x);
    const double n = 0.013;
    const double sf = (n * a.ubar) * (n * a.ubar) /
                      (2.208 * std::pow(std::max(a.rh, 1e-3), 4.0 / 3.0));
    const double ustar = std::sqrt(32.174 * std::max(a.rh, 1e-3) * sf);
    const double d_elder = 5.93 * ustar * a.h;
    std::printf("[ RWPT     ] D_meas=%.4f  D_elder=%.4f  (u*=%.4f h=%.3f)\n",
                d_meas, d_elder, ustar, a.h);
    ASSERT_GT(d_elder, 0.0);
    // MEASURED (X3b round), seeds {7,8,9,11,13} on this deck:
    // D_meas/D_elder = {1.44, 1.24, 0.96, 1.28, 1.20} — max |ln ratio|
    // 0.364, and 3x that is 1.09 ~= ln 3, so the factor-3 band IS the
    // measured pin (systematically ~20% high: wide-channel R_h, the
    // kEtaMin floor, finite-dt walk — recorded, within band). The
    // fixed-V/N-quantum defect this round repaired measured 10x HIGH;
    // this band catches a recurrence at 6 sigma-equivalents.
    EXPECT_LT(std::abs(std::log(d_meas / d_elder)), std::log(3.0))
        << "emergent D_L is not in Elder's neighborhood";
}

// ---------------------------------------------------------------------------
// Gate 2 — D-L6 determinism, both directions.
// ---------------------------------------------------------------------------
TEST(LardRwptTest, SameSeedIsBitIdenticalNewSeedIsNot) {
    DeckSpec s;
    s.end_time = "01:00:00";
    const RwptRun a = run_deck("_lr_det_a", s);
    const RwptRun b = run_deck("_lr_det_b", s);
    DeckSpec s2 = s;  s2.seed = 8;
    const RwptRun c = run_deck("_lr_det_c", s2);
    ASSERT_TRUE(a.ok && b.ok && c.ok);
    ASSERT_EQ(a.cout.size(), b.cout.size());

    for (std::size_t i = 0; i < a.cout.size(); ++i)
        ASSERT_EQ(a.cout[i], b.cout[i])
            << "same seed diverged at step " << i << " — the counter RNG "
               "is not a pure function of its key (D-L6)";

    // Liveness: a different seed must move SOMETHING mid-front.
    bool differs = false;
    for (std::size_t i = 0; i < std::min(a.cout.size(), c.cout.size()); ++i)
        if (a.cout[i] != c.cout[i]) { differs = true; break; }
    EXPECT_TRUE(differs)
        << "RWPT_SEED changed nothing — the seed is not reaching the RNG";
}

// ---------------------------------------------------------------------------
// Gate 3 — conservation + maximum principle under RWPT, chain deck.
// ---------------------------------------------------------------------------
TEST(LardRwptTest, MassConservesAndMaxPrincipleHoldsUnderRwpt) {
    DeckSpec s;
    s.chain = true;
    s.cinit = 50.0;
    s.end_time = "04:00:00";
    const RwptRun a = run_deck("_lr_cons", s);
    ASSERT_TRUE(a.ok);

    // Strict, not banded: the D-X3b1 limiter makes both claims structural.
    EXPECT_LE(a.peak_link, kCin * (1.0 + 1.0e-9))
        << "a link exceeded the source under RWPT — the limiter failed";
    EXPECT_LE(a.peak_node, kCin * (1.0 + 1.0e-9));

    const double in = a.mb_init + a.mb_ex_in;
    const double out = a.mb_outflow + a.mb_final;
    ASSERT_GT(in, 0.0);
    // Steady closure per the T3 conventions (transient family shares the
    // X2-measured ±% envelope; RWPT adds NO ledger terms of its own —
    // exchange is internal to the link).
    EXPECT_NEAR(out / in, 1.0, 0.01)
        << "ledger moved under RWPT: in=" << in << " out=" << out;
}

// ---------------------------------------------------------------------------
// Gate 4 — kernel invariants at unit level (no engine).
// ---------------------------------------------------------------------------
TEST(LardRwptTest, ProfileRngAndGeometryKernelsObeyInvariants) {
    using namespace openswmm::lard;
    const double ubar = 2.5, h = 1.0, ustar = 0.13;

    // Mean-free deviations: quadrature over eta.
    double sum_t = 0.0, sum_l = 0.0;
    const int N = 20000;
    for (int i = 0; i < N; ++i) {
        const double eta = (i + 0.5) / N;
        sum_t += rwpt_u_dev(eta, ubar, ustar, true);
        sum_l += rwpt_u_dev(eta, ubar, ustar, false);
    }
    EXPECT_LT(std::abs(sum_t / N), 5.0e-3 * ustar)
        << "turbulent log-law deviation is not mean-free";
    EXPECT_LT(std::abs(sum_l / N), 1.0e-6 * ubar)
        << "laminar parabola deviation is not mean-free";

    // Rouse endpoints and the drift = dD/dη identity (finite difference).
    EXPECT_NEAR(rwpt_d_eta(0.0, h, ustar, true), 0.0, 1e-12);
    EXPECT_NEAR(rwpt_d_eta(1.0, h, ustar, true), 0.0, 1e-12);
    const double e0 = 0.3, de = 1e-6;
    const double fd = (rwpt_d_eta(e0 + de, h, ustar, true) -
                       rwpt_d_eta(e0 - de, h, ustar, true)) /
                      (2.0 * de);
    EXPECT_NEAR(rwpt_d_eta_grad(e0, h, ustar, true), fd,
                1e-6 * std::abs(fd) + 1e-12)
        << "the Itô drift is not the diffusivity gradient";

    // RNG: in (0,1); key-sensitive in every component; reproducible.
    const double u = rwpt_uniform(1, 2, 3, 4, 5);
    EXPECT_GT(u, 0.0);
    EXPECT_LT(u, 1.0);
    EXPECT_EQ(u, rwpt_uniform(1, 2, 3, 4, 5));
    EXPECT_NE(u, rwpt_uniform(2, 2, 3, 4, 5));
    EXPECT_NE(u, rwpt_uniform(1, 3, 3, 4, 5));
    EXPECT_NE(u, rwpt_uniform(1, 2, 4, 4, 5));
    EXPECT_NE(u, rwpt_uniform(1, 2, 3, 5, 5));
    EXPECT_NE(u, rwpt_uniform(1, 2, 3, 4, 6));

    // Circular R_h: full and half-full both read D/4 exactly.
    EXPECT_NEAR(rwpt_hyd_radius(0.0, 2.0, 2.0, true), 0.5, 1e-9);
    EXPECT_NEAR(rwpt_hyd_radius(0.0, 1.0, 2.0, true), 0.5, 1e-9);
}

// ---------------------------------------------------------------------------
// Gate 5 — DISPERSION OFF is bit-inert against the omitted key.
// ---------------------------------------------------------------------------
TEST(LardRwptTest, ExplicitOffMatchesOmittedKeyBitwise) {
    DeckSpec omit;  omit.rwpt = false;
    DeckSpec off = omit;  off.explicit_off = true;
    omit.end_time = off.end_time = "01:00:00";

    const RwptRun a = run_deck("_lr_off_omit", omit);
    const RwptRun b = run_deck("_lr_off_expl", off);
    ASSERT_TRUE(a.ok && b.ok);
    ASSERT_EQ(a.cout.size(), b.cout.size());
    for (std::size_t i = 0; i < a.cout.size(); ++i)
        ASSERT_EQ(a.cout[i], b.cout[i])
            << "DISPERSION OFF is not the omitted-key path at step " << i;
}

}  // namespace
