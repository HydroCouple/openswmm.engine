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
 * @file test_fv_lts.cpp
 * @brief Plan §6.12 — local time stepping: equivalence, tier-interface
 *        conservation, fronts crossing tiers, and the dt-collapse relief.
 *
 * @details The gates here are deliberately split by what LTS is allowed to
 *          change. Scheduling is free to change the integration PATH — a
 *          tiered run and a global-dt run take different sequences of steps and
 *          land on slightly different states, exactly as two different
 *          timestep sizes would. What scheduling may never change is the
 *          BUDGET: every flux that leaves a fine cell must arrive in its
 *          coarse neighbour, to the last bit. So the conservation assertions
 *          are at machine precision and the state-comparison assertions are
 *          bounded-and-decaying.
 *
 *          Artefacts land under tests/output/fv (CLAUDE.md §4.1).
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §3.3, §6.12
 * @ingroup engine_fv
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

#include "fv_test_support.hpp"

using namespace fvtest;
namespace k = openswmm::fv::kernels;

namespace {

/// A walled channel whose cell length VARIES along the reach. This is the
/// configuration LTS exists for and the one a uniform mesh cannot exercise:
/// a sewer network's pipes span 5 ft to 500 ft, and without tiering the
/// shortest cell in the model sets the step for every other cell in it.
///
/// @p dxfn maps cell index → Δx.
template <class DxFn, class BedFn>
Channel makeGradedChannel(const openswmm::XSectParams& xs, int n, DxFn dxfn,
                          BedFn bedfn, double manning,
                          double slot_celerity = 100.0) {
    // Start from the uniform builder so the geometry, chain and cell→face maps
    // are constructed by exactly the code the other gates use, then restate the
    // lengths and the elevations that depend on them.
    Channel ch = makeWalledChannel(xs, n, 1.0, [](double) { return 0.0; },
                                   manning, slot_celerity);
    ch.dx = 0.0;

    std::vector<double> xface(static_cast<std::size_t>(n) + 1, 0.0);
    for (int i = 0; i < n; ++i)
        xface[static_cast<std::size_t>(i) + 1] =
            xface[static_cast<std::size_t>(i)] + dxfn(i);

    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double dx = dxfn(i);
        const double xc = 0.5 * (xface[ui] + xface[ui + 1]);
        ch.mesh.cell_dx[ui]   = dx;
        ch.mesh.cell_zb[ui]   = bedfn(xc);
        ch.mesh.cell_dzdx[ui] = (bedfn(xface[ui + 1]) - bedfn(xface[ui])) / dx;
    }
    // Face 0 is the upstream wall, face i the interface below cell i-1, face n
    // the downstream wall — the ordering makeWalledChannel builds.
    for (int f = 0; f <= n; ++f) {
        const auto uf = static_cast<std::size_t>(f);
        ch.mesh.face_zb[uf] = bedfn(xface[uf]);
        if (f == 0)      ch.mesh.face_dx[uf] = 0.5 * dxfn(0);
        else if (f == n) ch.mesh.face_dx[uf] = 0.5 * dxfn(n - 1);
        else             ch.mesh.face_dx[uf] = 0.5 * (dxfn(f - 1) + dxfn(f));
    }
    return ch;
}

/// Δx profile of the §6.12 reference case: a long reach interrupted by a run of
/// short pipes. The 100× length ratio is what a real network presents, and it
/// is the ratio the plan's "one short pipe sets the step for 10,000" argument
/// is about.
double gradedDx(int i) { return (i >= 40 && i < 60) ? 5.0 : 200.0; }

double bedSlope(double x) { return 0.001 * (8000.0 - x); }

double totalVol(const Channel& ch) {
    double v = 0.0;
    for (int i = 0; i < ch.n; ++i)
        v += ch.state.cell_a[static_cast<std::size_t>(i)] *
             ch.mesh.cell_dx[static_cast<std::size_t>(i)];
    return v;
}

struct RunResult {
    std::vector<double> a, q;
    double  vol_drift = 0.0;     ///< |V(t) − V(0)| / V(0)
    long    substeps  = 0;
    long    face_work = 0;       ///< face updates actually performed
    int     tiers     = 1;
};

/// Dam break in a closed (walled) graded reach: no inflow, no outflow, so the
/// total volume is an exact invariant and any tier-interface leak shows up in
/// it directly.
RunResult runGradedDamBreak(int max_tiers, bool lts, double t_end,
                            double dt_report = 5.0) {
    Channel ch = makeGradedChannel(rectOpen(10.0, 20.0), 100, gradedDx,
                                   bedSlope, 0.013);
    for (int i = 0; i < ch.n; ++i)
        ch.state.cell_a[static_cast<std::size_t>(i)] =
            10.0 * ((i < 50) ? 6.0 : 1.0);

    openswmm::fv::FvOptions o = defaultOptions();
    o.lts           = lts;
    o.lts_max_tiers = max_tiers;

    openswmm::fv::ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    const double v0 = totalVol(ch);

    openswmm::fv::FvStepForcing f;
    f.n_nodes = 0;
    RunResult r;
    for (double t = 0.0; t < t_end; t += dt_report) {
        s.advance(t, t + dt_report, f);
        r.substeps += s.last_num_steps();
        r.tiers = std::max(r.tiers, s.lts_tiers());
    }
    r.a = ch.state.cell_a;
    r.q = ch.state.cell_q;
    r.face_work = s.run_stats().nflux;
    r.vol_drift = std::fabs(totalVol(ch) - v0) / v0;
    s.finalize();
    return r;
}

}  // namespace

// ===========================================================================
// §6.12(a) — equivalence
// ===========================================================================

// A single tier IS global stepping, and the solver must recognise that and
// take the untiered path rather than a K = 1 special case of the tiered one.
// The identity has to hold to the last bit: LTS is a scheduling change, and a
// scheduler that perturbs the arithmetic when it schedules nothing would make
// every gate in the rest of the suite depend on whether the option is on.
TEST(FvLts, ASingleTierReproducesGlobalSteppingBitForBit) {
    const RunResult off = runGradedDamBreak(6, /*lts=*/false, 120.0);
    const RunResult one = runGradedDamBreak(1, /*lts=*/true,  120.0);
    ASSERT_EQ(off.a.size(), one.a.size());
    for (std::size_t i = 0; i < off.a.size(); ++i) {
        EXPECT_EQ(one.a[i], off.a[i]) << "area differs at cell " << i;
        EXPECT_EQ(one.q[i], off.q[i]) << "discharge differs at cell " << i;
    }
}

// Tiering must not manufacture separation that is not there. Uniform Δx and a
// uniform state — a lake at rest over a sloping bed — is the case where every
// control volume has the same stable step, and the solver is required to see
// that and fall through. (Note what does NOT qualify: a uniform MESH carrying
// a dam break tiers legitimately, because the tier comes from the wave speed
// √(gA/T), and a 5 ft column and a 1 ft column do not share one.)
TEST(FvLts, AUniformStateOnAUniformMeshDoesNotTier) {
    Channel ch = makeWalledChannel(rectOpen(10.0, 20.0), 60, 25.0,
                                   [](double x) { return 0.001 * (1500.0 - x); },
                                   0.013);
    seedLevel(ch, 6.0);
    openswmm::fv::FvOptions o = defaultOptions();
    openswmm::fv::ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    openswmm::fv::FvStepForcing f;
    f.n_nodes = 0;
    for (double t = 0.0; t < 60.0; t += 5.0) s.advance(t, t + 5.0, f);
    EXPECT_EQ(s.lts_tiers(), 1) << "a lake at rest tiered against itself";
    s.finalize();
}

// ===========================================================================
// §6.12(b) — tier-interface conservation is EXACT
// ===========================================================================

// The load-bearing gate. A closed reach cannot gain or lose water, and the
// only mechanism by which tiering could make it do so is the interface: a
// coarse cell that does not see every fine-side flux. The accumulator contract
// says what leaves is exactly what arrives, so this is asserted at machine
// precision at every tier count, not to a tolerance.
TEST(FvLts, TierInterfaceConservationIsExactAtEveryTierCount) {
    for (int K : {1, 2, 4, 6}) {
        const RunResult r = runGradedDamBreak(K, /*lts=*/true, 300.0);
        EXPECT_LE(r.vol_drift, 1.0e-12)
            << "volume drift at FV_LTS_MAX_TIERS=" << K;
    }
}

// Tiering has to be genuinely active for the gates above to mean anything —
// a run that quietly collapsed to K = 1 would pass every one of them.
TEST(FvLts, TieringIsGenuinelyActiveOnAGradedReach) {
    const RunResult r = runGradedDamBreak(6, /*lts=*/true, 300.0);
    EXPECT_GE(r.tiers, 2)
        << "a 100x length ratio produced no tier separation — the equivalence "
           "and conservation gates would be vacuous";
    std::printf("[fv-lts] graded reach: %d tiers, %ld substeps\n",
                r.tiers, r.substeps);
}

// ===========================================================================
// §6.12(c) — the tiered solution tracks the global-dt solution
// ===========================================================================

// LTS changes the integration path, not the physics. Per-cell states differ
// most where the coarse tier lags a moving front — that is bounded by the
// front height for any scheme of this class — so the robust statements are
// that the bulk distribution matches and the total is untouched.
TEST(FvLts, TieredSolutionTracksGlobalStepping) {
    const RunResult glob = runGradedDamBreak(1, /*lts=*/false, 400.0);
    const RunResult tier = runGradedDamBreak(6, /*lts=*/true,  400.0);
    ASSERT_EQ(glob.a.size(), tier.a.size());

    // Bulk: volume that has crossed into the downstream half.
    auto passed = [](const std::vector<double>& a) {
        double v = 0.0;
        for (int i = 50; i < 100; ++i)
            v += a[static_cast<std::size_t>(i)] * gradedDx(i);
        return v;
    };
    const double pg = passed(glob.a), pt = passed(tier.a);
    EXPECT_NEAR(pt, pg, 0.05 * pg)
        << "bulk volume past the dam site diverges between LTS and global dt";

    // Per-cell depth, as a fraction of the initial dam height.
    double worst = 0.0;
    for (std::size_t i = 0; i < glob.a.size(); ++i)
        worst = std::max(worst, std::fabs(tier.a[i] - glob.a[i]) / 10.0);
    EXPECT_LE(worst, 0.5) << "per-cell depth divergence (ft)";
    std::printf("[fv-lts] worst per-cell depth divergence %.4f ft\n", worst);
}

// ===========================================================================
// §6.12(c) — a wetting front crossing a tier boundary
// ===========================================================================

// The configuration LTS gets wrong when the accumulator contract is not
// honoured: water arrives at a coarse cell from a fine one, and the coarse
// cell has to receive every fine-side flux booked while it was not looking.
// A leak here is silent — the front simply advances at the wrong speed.
TEST(FvLts, WettingFrontCrossesTierBoundariesWithoutLoss) {
    Channel ch = makeGradedChannel(rectOpen(10.0, 20.0), 100, gradedDx,
                                   [](double) { return 0.0; }, 0.013);
    // Water fills the reach up to the short-pipe run and no further, so the
    // front starts ON the tier boundary: it must cross into the fine tier and
    // back out into the coarse cells beyond it.
    for (int i = 0; i < 40; ++i)
        ch.state.cell_a[static_cast<std::size_t>(i)] = 10.0 * 8.0;

    openswmm::fv::FvOptions o = defaultOptions();
    o.lts_max_tiers = 6;
    openswmm::fv::ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, o);
    const double v0 = totalVol(ch);

    openswmm::fv::FvStepForcing f;
    f.n_nodes = 0;
    for (double t = 0.0; t < 600.0; t += 5.0) {
        s.advance(t, t + 5.0, f);
        for (int i = 0; i < ch.n; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            ASSERT_GE(ch.state.cell_a[ui], 0.0) << "negative area at cell " << i;
            ASSERT_FALSE(std::isnan(ch.state.cell_a[ui]))
                << "NaN at cell " << i << " t=" << t;
        }
    }
    EXPECT_NEAR(totalVol(ch), v0, 1.0e-12 * v0)
        << "the front lost water crossing a tier interface";
    // It has to actually get there, or the test proves nothing.
    EXPECT_GT(ch.state.cell_a[static_cast<std::size_t>(62)], 0.0)
        << "the front never reached the coarse tier beyond the short pipes";
    s.finalize();
}

// ===========================================================================
// §6.12(d) — the dt-collapse relief, quantified
// ===========================================================================

// The quantitative basis for the plan's §2.1 claim, measured on the quantity
// LTS actually reduces. It does NOT reduce the base substep count — dt₀ is
// still the finest cell's requirement, and the cycle still walks it. What it
// reduces is the WORK inside those substeps: a tier-k face is evaluated once
// per 2^k substeps instead of every one. Counting substeps here would report
// 1.00x and conclude, wrongly, that tiering does nothing.
//
// A measurement with a loose gate, not a performance contract — it exists so
// the number is recorded and a regression that silently disables tiering shows
// up as a ratio of one.
TEST(FvLts, ShortPipesNoLongerSetTheWorkRateForTheWholeReach) {
    const RunResult glob = runGradedDamBreak(1, /*lts=*/false, 300.0);
    const RunResult tier = runGradedDamBreak(6, /*lts=*/true,  300.0);
    const double ratio = static_cast<double>(glob.face_work) /
                         static_cast<double>(std::max(1L, tier.face_work));
    std::printf("[fv-lts] face updates: global %ld vs tiered %ld (%.2fx); "
                "base substeps %ld vs %ld\n",
                glob.face_work, tier.face_work, ratio,
                glob.substeps, tier.substeps);
    EXPECT_GT(ratio, 1.5)
        << "tiering did not reduce face work on a 40x graded reach";
}
