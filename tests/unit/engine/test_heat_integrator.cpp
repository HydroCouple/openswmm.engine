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
 * @file test_heat_integrator.cpp
 * @brief D-H5d/D-H5e gates — the semi-implicit surface-balance step and
 *        the single-relaxation composition of the flux families.
 *
 * @details H5a's validation found the surface energy balance diverging to
 *          NaN. The step was forward Euler with no stability limit, and heat
 *          capacity is `rho*cp*V`, so a thin film has almost none: a 0.52 ft3
 *          film over 27226 ft2 took a +862 C step in 60 s and the sequence
 *          ran 5 -> 182 -> -1.8e4 -> -3.9e9 -> inf -> NaN.
 *
 *          The gates here are deliberately built around a property rather
 *          than a reference value: **a relaxation step cannot overshoot the
 *          equilibrium temperature, whatever dt is.** That assertion needs no
 *          expected number, so no deck change, timestep change or future
 *          phase can stale it (lesson 72). It is also the exact property the
 *          explicit form lacked, so it fails on the code this replaces.
 *
 *          Gate 2 is the one that would have caught the original defect.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6.2 D-H5d, §6.3 D-H5e
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "transport/components/HeatFluxModules/HeatFluxes.hpp"
#include "transport/components/HeatFluxModules/RadiativeExchange.hpp"
#include <initializer_list>

#include "transport/components/HeatFluxModules/SurfaceExchange.hpp"

namespace se = openswmm::transport::heat;

namespace {

// Water. SI throughout, matching the module's own units.
constexpr double kRho = 1000.0;    ///< kg/m3
constexpr double kCp  = 4184.0;    ///< J/kg/C

/// The measured pathological case, converted once: a 0.52 ft3 film spread
/// over 27226 ft2. This is not a synthetic worst case — it is the subarea
/// that produced the NaN.
constexpr double kFilmVolM3  = 0.52  * 0.028316846592;
constexpr double kFilmAreaM2 = 27226.0 * 0.09290304;

/// A linear net-outward flux with slope `slope` W/m2/C and a zero at `t_eq`.
/// Linear on purpose: relaxation is EXACT for a linear flux, so any
/// disagreement in these gates is the integrator and not the linearization.
double linearFlux(double t, double t_eq, double slope) {
    return slope * (t - t_eq);
}

/// What the old forward-Euler step would have produced.
double explicitStep(double j0, double area_m2, double vol_m3, double dt) {
    return -j0 * area_m2 * dt / (kRho * kCp * vol_m3);
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — small dt agrees with forward Euler. The scheme must not change any
//          answer H2 and H3 already produce in the resolved regime.
// ---------------------------------------------------------------------------
TEST(HeatIntegratorTest, SmallStepsAgreeWithTheExplicitForm) {
    constexpr double kTeq = 12.0, kSlope = 8.0, kT0 = 25.0;
    constexpr double kAreaM2 = 100.0, kVolM3 = 50.0;   // a well-resolved pond

    const double j0 = linearFlux(kT0, kTeq, kSlope);
    const double j1 = linearFlux(kT0 + se::kProbeC, kTeq, kSlope);

    // 0.1 s on 50 m3: k*dt ~ 1.6e-6, deep in the linear regime.
    constexpr double kDt = 0.1;
    const double got = se::relaxT(j0, j1, se::kProbeC, kAreaM2, kVolM3, kDt,
                                  kRho, kCp);
    const double want = explicitStep(j0, kAreaM2, kVolM3, kDt);

    // Relative agreement, because both numbers are tiny in absolute terms.
    ASSERT_NE(want, 0.0);
    EXPECT_NEAR(got / want, 1.0, 1.0e-6)
        << "the semi-implicit step disagrees with forward Euler at k*dt << 1 "
           "(got " << got << ", explicit " << want << "). expm1's small-x "
           "limit is what keeps every resolved answer unchanged from H2/H3; "
           "if this fails the scheme is not a drop-in and every prior heat "
           "number moved";

    // The limit must hold at EVERY small step, not only at this one — and
    // that is what makes this gate observe `expm1` rather than merely use
    // it. At the dt above, k*dt is ~4e-7 and `exp(x) - 1` is already
    // accurate to ~1e-10, four orders inside the band: swapping expm1 for
    // exp(x)-1 changed nothing and the gate passed. Cancellation only bites
    // once k*dt approaches the machine epsilon, where `exp(-k*dt)` rounds to
    // exactly 1.0 and the subtraction yields a hard zero while expm1 keeps
    // full precision.
    for (double small_dt : {1.0e-2, 1.0e-6, 1.0e-10, 1.0e-14}) {
        const double g = se::relaxT(j0, j1, se::kProbeC, kAreaM2, kVolM3,
                                    small_dt, kRho, kCp);
        const double w = explicitStep(j0, kAreaM2, kVolM3, small_dt);
        ASSERT_NE(w, 0.0) << "dt=" << small_dt;
        EXPECT_NEAR(g / w, 1.0, 1.0e-6)
            << "dt=" << small_dt << ": got " << g << ", explicit " << w
            << ". A zero here is the signature of exp(-k*dt) - 1 cancelling "
               "to nothing at k*dt = " << (small_dt * 100.0 * 8.0 /
                                           (kRho * kCp * 50.0));
    }
}

// ---------------------------------------------------------------------------
// Gate 2 — THE GATE THAT FAILS ON THE CODE THIS REPLACES. The measured film,
//          the measured step, no divergence and no overshoot.
// ---------------------------------------------------------------------------
TEST(HeatIntegratorTest, TheFilmThatDivergedIsBoundedByItsEquilibrium) {
    // slope 14 W/m2/C over a 25 C offset reproduces the MEASURED excursion:
    // the explicit step on this film is +862 C in 60 s, which is the number
    // H5a's validation reported. Verified numerically before this gate was
    // written rather than assumed from the report.
    constexpr double kT0 = 5.0, kTeq = 30.0, kSlope = 14.0;
    constexpr double kDt = 60.0;

    const double j0 = linearFlux(kT0, kTeq, kSlope);
    const double j1 = linearFlux(kT0 + se::kProbeC, kTeq, kSlope);

    // SETUP: confirm this configuration really is the pathological one, or
    // the gate is asserting stability about a case that was never unstable
    // (lesson 59). The explicit step must be enormous.
    const double explicit_dT = explicitStep(j0, kFilmAreaM2, kFilmVolM3, kDt);
    EXPECT_NEAR(explicit_dT, 862.17, 0.5)
        << "this is meant to be the measured pathological case; the explicit "
           "step came out at " << explicit_dT << " C rather than the +862 C "
           "H5a's validation reported";
    ASSERT_GT(std::fabs(explicit_dT), 500.0)
        << "the explicit step here is only " << explicit_dT << " C, so this "
           "deck is not the unstable regime and the gate proves nothing";

    const double eq = se::equilibriumT(kT0, j0, j1, se::kProbeC);
    EXPECT_NEAR(eq, kTeq, 1.0e-6)
        << "the probe did not recover the flux's own zero";

    const double dT = se::relaxT(j0, j1, se::kProbeC, kFilmAreaM2, kFilmVolM3,
                                 kDt, kRho, kCp);
    const double t_new = kT0 + dT;

    EXPECT_TRUE(std::isfinite(t_new)) << "the step is not finite";
    // The whole property, stated without a reference value: the result lies
    // between where it started and where it is heading. Nothing else is
    // physically admissible, and the explicit form violated it by 800 C.
    EXPECT_GE(t_new, kT0 - 1.0e-9)
        << "the step moved AWAY from equilibrium";
    EXPECT_LE(t_new, kTeq + 1.0e-9)
        << "the step overshot equilibrium: " << t_new << " C past a "
        << kTeq << " C fixed point. This is the divergence H5a shipped — "
           "with dt this large relative to the film's thermal mass, an "
           "explicit step lands at " << (kT0 + explicit_dT) << " C";
}

// ---------------------------------------------------------------------------
// Gate 3 — no overshoot holds for ARBITRARY dt, in both directions.
// ---------------------------------------------------------------------------
TEST(HeatIntegratorTest, NoStepOvershootsEquilibriumInEitherDirection) {
    constexpr double kSlope = 8.0;
    const double dts[] = {1.0e-3, 1.0, 60.0, 3600.0, 86400.0, 1.0e9};

    // Warming (T0 below equilibrium) and cooling (above), same machinery.
    struct Case { double t0, t_eq; } cases[] = {{5.0, 30.0}, {30.0, 5.0},
                                                {-4.0, 2.0}, {2.0, -4.0}};

    for (const auto& c : cases) {
        const double j0 = linearFlux(c.t0, c.t_eq, kSlope);
        const double j1 = linearFlux(c.t0 + se::kProbeC, c.t_eq, kSlope);
        const double lo = std::min(c.t0, c.t_eq);
        const double hi = std::max(c.t0, c.t_eq);

        for (double dt : dts) {
            const double t_new =
                c.t0 + se::relaxT(j0, j1, se::kProbeC, kFilmAreaM2,
                                  kFilmVolM3, dt, kRho, kCp);
            EXPECT_TRUE(std::isfinite(t_new))
                << "dt=" << dt << " from " << c.t0 << " toward " << c.t_eq;
            EXPECT_GE(t_new, lo - 1.0e-9)
                << "dt=" << dt << ": " << t_new << " below [" << lo << ", "
                << hi << "]";
            EXPECT_LE(t_new, hi + 1.0e-9)
                << "dt=" << dt << ": " << t_new << " above [" << lo << ", "
                << hi << "]";
        }
    }
}

// ---------------------------------------------------------------------------
// Gate 4 — the step is monotone in dt and converges to equilibrium.
//          Distinguishes "bounded" from "bounded and still integrating":
//          a scheme that simply clamped would pass gate 3 and fail here.
// ---------------------------------------------------------------------------
TEST(HeatIntegratorTest, LongerStepsMoveMonotonicallyTowardEquilibrium) {
    constexpr double kT0 = 5.0, kTeq = 30.0, kSlope = 8.0;
    const double j0 = linearFlux(kT0, kTeq, kSlope);
    const double j1 = linearFlux(kT0 + se::kProbeC, kTeq, kSlope);

    double prev = kT0;
    for (double dt : {1.0, 10.0, 60.0, 600.0, 3600.0}) {
        const double t_new = kT0 + se::relaxT(j0, j1, se::kProbeC,
                                              kFilmAreaM2, kFilmVolM3, dt,
                                              kRho, kCp);
        EXPECT_GT(t_new, prev - 1.0e-12)
            << "dt=" << dt << " moved less far than a shorter step — the "
               "scheme is not integrating, it is clamping";
        prev = t_new;
    }
    // And the limit really is equilibrium, not some other fixed point.
    const double t_inf = kT0 + se::relaxT(j0, j1, se::kProbeC, kFilmAreaM2,
                                          kFilmVolM3, 1.0e12, kRho, kCp);
    EXPECT_NEAR(t_inf, kTeq, 1.0e-6)
        << "an unbounded step settles at " << t_inf << " C rather than the "
           "equilibrium " << kTeq << " C";
}

// ---------------------------------------------------------------------------
// Gate 5 — anti-damping and degenerate inputs fall back rather than blow up.
// ---------------------------------------------------------------------------
TEST(HeatIntegratorTest, DegenerateInputsFallBackInsteadOfDividing) {
    constexpr double kT0 = 15.0;

    // J' < 0: the outward flux SHRINKS as the water warms, so the linearized
    // system has no fixed point to relax onto. The documented behaviour is
    // the explicit step, not a NaN and not a silent zero.
    const double j0 = 100.0;
    const double j1 = 100.0 - 5.0 * se::kProbeC;      // negative slope
    const double dT = se::relaxT(j0, j1, se::kProbeC, 10.0, 1.0, 1.0,
                                 kRho, kCp);
    EXPECT_TRUE(std::isfinite(dT));
    EXPECT_NEAR(dT, explicitStep(j0, 10.0, 1.0, 1.0), 1.0e-12)
        << "J' <= 0 did not fall back to the explicit step";
    EXPECT_NEAR(se::equilibriumT(kT0, j0, j1, se::kProbeC), kT0, 1.0e-12)
        << "equilibriumT invented a fixed point where none exists";

    // Zero volume, zero area, zero dt: no thermal mass, no exchanging
    // surface, no time. Each must return exactly 0, never a division.
    EXPECT_EQ(se::relaxT(j0, j1, se::kProbeC, 10.0, 0.0, 1.0, kRho, kCp), 0.0);
    EXPECT_EQ(se::relaxT(j0, j1, se::kProbeC, 0.0, 1.0, 1.0, kRho, kCp), 0.0);
    EXPECT_EQ(se::relaxT(j0, j1, se::kProbeC, 10.0, 1.0, 0.0, kRho, kCp), 0.0);

    // A zero probe cannot yield a slope; fall back rather than divide by it.
    EXPECT_TRUE(std::isfinite(
        se::relaxT(j0, j1, 0.0, 10.0, 1.0, 1.0, kRho, kCp)));
}

// ---------------------------------------------------------------------------
// Gate 6 — a flux already AT equilibrium does not move.
// ---------------------------------------------------------------------------
TEST(HeatIntegratorTest, AnElementAtEquilibriumDoesNotMove) {
    constexpr double kTeq = 18.0, kSlope = 8.0;
    const double j0 = linearFlux(kTeq, kTeq, kSlope);      // exactly zero
    const double j1 = linearFlux(kTeq + se::kProbeC, kTeq, kSlope);
    ASSERT_EQ(j0, 0.0);

    for (double dt : {1.0, 60.0, 86400.0})
        EXPECT_NEAR(se::relaxT(j0, j1, se::kProbeC, kFilmAreaM2, kFilmVolM3,
                               dt, kRho, kCp), 0.0, 1.0e-12)
            << "dt=" << dt << ": a zero flux produced a temperature change";
}

// ---------------------------------------------------------------------------
// Gate 7 — D-H5e. Two flux families must compose into ONE relaxation.
//          This is the gate that fails on the code D-H5e replaces.
// ---------------------------------------------------------------------------
TEST(HeatIntegratorTest, TwoFluxFamiliesRelaxToTheirCombinedEquilibrium) {
    // Two equal-slope families with equilibria at 30 C and 10 C. Their SUM
    // has slope 2s and equilibrium 20 C — the physically correct fixed
    // point, and a number that is neither module's own.
    constexpr double kS = 7.0, kT0 = 5.0;
    constexpr double kEq1 = 30.0, kEq2 = 10.0, kEqBoth = 20.0;
    const auto f1   = [](double t) { return kS * (t - kEq1); };
    const auto f2   = [](double t) { return kS * (t - kEq2); };
    const auto both = [&](double t) { return f1(t) + f2(t); };

    // SETUP: the summed flux really does vanish at 20 C, or the gate is
    // asserting against an equilibrium the fluxes do not have.
    ASSERT_NEAR(both(kEqBoth), 0.0, 1.0e-12);

    for (double dt : {1.0e-3, 0.1, 10.0}) {
        // How the SPLIT bindings behaved: relax against one family, then
        // relax the RESULT against the other. Each sub-step relaxes fully
        // toward its own module's equilibrium.
        double seq = kT0;
        seq += se::relaxT(f1(seq), f1(seq + se::kProbeC), se::kProbeC,
                          kFilmAreaM2, kFilmVolM3, dt, kRho, kCp);
        seq += se::relaxT(f2(seq), f2(seq + se::kProbeC), se::kProbeC,
                          kFilmAreaM2, kFilmVolM3, dt, kRho, kCp);

        // How the merged binding behaves: sum first, relax once.
        const double comb =
            kT0 + se::relaxT(both(kT0), both(kT0 + se::kProbeC), se::kProbeC,
                             kFilmAreaM2, kFilmVolM3, dt, kRho, kCp);

        // The combined step can never pass the combined equilibrium. No
        // reference value: 20 C is where the summed flux vanishes.
        EXPECT_GE(comb, kT0 - 1.0e-9) << "dt=" << dt;
        EXPECT_LE(comb, kEqBoth + 1.0e-9)
            << "dt=" << dt << ": the merged step reached " << comb
            << " C, past the combined equilibrium " << kEqBoth << " C";

        // And at small k*dt the two agree, which is why this was invisible
        // until D-H5d: under forward Euler the increments were LINEAR and
        // added exactly, so splitting cost nothing.
        if (dt <= 1.0e-3) {
            EXPECT_NEAR(seq, comb, 1.0e-5)
                << "dt=" << dt << ": the split and merged forms disagree "
                   "even in the linear regime, which means the discrepancy "
                   "is not the operator split";
        }
    }

    // The failure the merge exists to prevent, stated as a number: at large
    // k*dt the sequential form lands well short, dragged to the LAST
    // module's equilibrium (measured 11.05 C against a true 19.95 C).
    constexpr double kBigDt = 10.0;
    double seq = kT0;
    seq += se::relaxT(f1(seq), f1(seq + se::kProbeC), se::kProbeC,
                      kFilmAreaM2, kFilmVolM3, kBigDt, kRho, kCp);
    seq += se::relaxT(f2(seq), f2(seq + se::kProbeC), se::kProbeC,
                      kFilmAreaM2, kFilmVolM3, kBigDt, kRho, kCp);
    const double comb =
        kT0 + se::relaxT(both(kT0), both(kT0 + se::kProbeC), se::kProbeC,
                         kFilmAreaM2, kFilmVolM3, kBigDt, kRho, kCp);
    ASSERT_GT(comb - seq, 5.0)
        << "the split and merged forms differ by only " << (comb - seq)
        << " C at k*dt ~ 5.7, so this configuration does not reach the "
           "regime where operator order matters and the gate proves nothing";
}

// ---------------------------------------------------------------------------
// Gate 8 — D-H5e AT THE BINDING. Gate 7 proves the arithmetic; this proves
//          `applyHeatFluxes` uses it.
//
// The handoff predicted this gate could not be built, on the strength of
// D-H5d's round finding that a 1D node cannot be driven to a large `k*dt`.
// That finding was measured with RADIATIVE ALONE, where J' ~ 5.5 W/m2/C and
// the depth required sits under the router's 1e-4 ft floor. Summing the
// surface family in raises J' to ~46 (20 mph wind, 20% RH), so the depth
// needed rises by the same factor: measured k*dt = 1.80 at 2e-4 ft, which is
// twice the floor and entirely ordinary for a storage node.
//
// The assertion needs no reference value. A node whose depth never changes
// and whose temperature has settled is at steady state, and at steady state
// the SUMMED outward flux must vanish. Relaxing the two families separately
// settles somewhere else entirely — each step drives fully to its own
// family's equilibrium, so the pair parks where neither flux is zero:
//
//     k*dt   split      merged     (combined equilibrium -0.3942 C)
//     0.18   -0.6176    -0.3939
//     0.72   -1.3212    -0.3942
//     1.80   -2.8384    -0.3942
//
// Expressed as the residual flux divided by its own slope, the split sits
// 2.44 C from where the summed flux vanishes; the merged form sits at 0.
// ---------------------------------------------------------------------------
TEST(HeatIntegratorTest, ASteadyNodeSitsWhereTheSUMMEDFluxVanishes) {
    {
        std::ofstream f("_hi8.heat");
        f << "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
          << "[HEAT_FLUXES]\nSURFACE_EXCHANGE ON\nRADIATIVE_EXCHANGE ON\n";
    }
    {
        std::ofstream f("_hi8.inp");
        f << "[TITLE]\nD-H5e composition gate deck\n\n[OPTIONS]\n"
          << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT ON\n"
          << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
          << "END_DATE 01/01/2026\nEND_TIME 00:10:00\n"
          << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
          // 20 mph wind and 20% humidity: the latent term carries most of
          // J', and J' is what puts this deck in the regime the gate is
          // about. Weakening either drops k*dt below 0.4 and the two
          // compositions converge -- see the SETUP assertion below.
          << "[TEMPERATURE]\nTIMESERIES air_ts\n"
          << "WINDSPEED MONTHLY 20 20 20 20 20 20 20 20 20 20 20 20\n"
          << "HUMIDITY 20 20 20 20 20 20 20 20 20 20 20 20\n\n"
          << "[TIMESERIES]\nair_ts 01/01/2026 00:00 50.0\n"
          << "air_ts 01/02/2026 00:00 50.0\n\n"
          // A 2e-4 ft sheet over a million square feet: tiny thermal mass
          // per unit of exchanging surface, which is what makes k*dt large.
          << "[STORAGE]\nS1 10.0 12 0.0002 FUNCTIONAL 0 0 1000000\n\n"
          << "[JUNCTIONS]\nJ1 9.0 10 0 0 0\n\n"
          << "[OUTFALLS]\nOUT 8.0 FREE  NO\n\n"
          << "[CONDUITS]\nC1 S1 J1 500 0.013 0 0 0\n"
          << "C2 J1 OUT 500 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\nC2 CIRCULAR 3.0 0 0 0\n\n"
          << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.heat config=\"_hi8.heat\"\n\n"
          << "[REPORT]\nINPUT NO\n";
    }

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_hi8.inp", "_hi8.rpt", "_hi8.out", nullptr),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
    } while (elapsed > 0.0 && ++guard < 20000);
    swmm_engine_end(e);

    const auto& ctx =
        static_cast<openswmm::SWMMEngine*>(e)->context();
    ASSERT_TRUE(ctx.heat_config.surface_exchange);
    ASSERT_TRUE(ctx.heat_config.radiative_exchange)
        << "both families must be enabled or there is no composition to test";

    // Find the storage node: the only one with a free surface.
    int n = -1;
    for (int i = 0; i < ctx.n_nodes(); ++i)
        if (ctx.nodes.volume[static_cast<std::size_t>(i)] > 0.0 &&
            ctx.nodes.depth[static_cast<std::size_t>(i)] > 0.0) { n = i; break; }
    ASSERT_GE(n, 0) << "no node holds water — nothing exchanged";
    const auto un = static_cast<std::size_t>(n);

    const double t_w  = ctx.heat_state.node_temp[un];
    // PE1: flux evaluators take the element token; the node's identity here.
    const openswmm::HeatElement he_n = openswmm::HeatElement::node(n);
    const double j0   = se::netFluxOut(ctx, he_n, t_w);
    const double j1   = se::netFluxOut(ctx, he_n, t_w + se::kProbeC);
    const double jp   = (j1 - j0) / se::kProbeC;
    ASSERT_GT(jp, 0.0);

    // SETUP: the deck must actually be in the stiff regime. Below k*dt ~ 0.4
    // the split and the merged forms agree and this gate proves nothing
    // about either (lesson 56).
    const double depth_m = ctx.nodes.depth[un] * 0.3048;
    const double k = jp / (ctx.options.water_density *
                           ctx.options.water_specific_heat * depth_m);
    EXPECT_GT(k * 10.0, 0.4)
        << "k*dt is only " << (k * 10.0) << " on this deck (J' = " << jp
        << " W/m2/C, depth = " << ctx.nodes.depth[un] << " ft). Sequential "
           "and summed relaxation agree in the linear regime, so the gate "
           "would pass without observing anything.";

    // THE ASSERTION. Residual flux over its own slope is the distance, in
    // degrees, from where the summed flux vanishes.
    const double implied_offset_c = std::fabs(j0) / jp;
    EXPECT_LT(implied_offset_c, 0.05)
        << "the node settled at " << t_w << " C, which is "
        << implied_offset_c << " C away from where the SUMMED outward flux "
           "is zero (residual " << j0 << " W/m2). A steady element must sit "
           "at the equilibrium of every enabled family taken together. "
           "Relaxing each family separately parks it at neither — measured "
           "2.44 C away on this deck.";
    swmm_engine_destroy(e);

    // ---- And the other half of the contract: each evaluator must honour
    //      its OWN toggle, because `netFluxOut` sums unconditionally. With
    //      SURFACE_EXCHANGE off and RADIATIVE_EXCHANGE on, the node must
    //      settle where the RADIATIVE flux alone vanishes.
    //
    //      Nothing else in the suite observes this. `applyHeatFluxes`
    //      returns early when both toggles are off, so an all-off deck never
    //      reaches the evaluators, and the only mixed-toggle deck elsewhere
    //      (H3's pool) asserts a DIRECTION — sun warms, night cools — which
    //      a spurious latent term does not reverse.
    {
        std::ofstream f("_hi8b.heat");
        f << "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
          << "[HEAT_FLUXES]\nSURFACE_EXCHANGE OFF\nRADIATIVE_EXCHANGE ON\n";
    }
    std::string deck;
    {
        std::ifstream in("_hi8.inp");
        deck.assign(std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>());
        const auto at = deck.find("_hi8.heat");
        ASSERT_NE(at, std::string::npos);
        deck.replace(at, std::string("_hi8.heat").size(), "_hi8b.heat");
        std::ofstream out("_hi8b.inp");
        out << deck;
    }
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, "_hi8b.inp", "_hi8b.rpt", "_hi8b.out",
                               nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e2), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e2, 1), SWMM_OK);
    elapsed = 0.0;
    guard = 0;
    do {
        if (swmm_engine_step(e2, &elapsed) != SWMM_OK) break;
    } while (elapsed > 0.0 && ++guard < 20000);
    swmm_engine_end(e2);

    const auto& c2 = static_cast<openswmm::SWMMEngine*>(e2)->context();
    ASSERT_FALSE(c2.heat_config.surface_exchange);
    ASSERT_TRUE(c2.heat_config.radiative_exchange);
    const double t2 = c2.heat_state.node_temp[un];
    const double r0 = se::radiativeFluxOut(c2, openswmm::HeatElement::node(n), t2);
    const double r1 = se::radiativeFluxOut(c2, openswmm::HeatElement::node(n),
                                           t2 + se::kProbeC);
    const double rp = (r1 - r0) / se::kProbeC;
    ASSERT_GT(rp, 0.0);
    // SETUP: the two answers must be far apart, or the assertion cannot tell
    // a leaking surface term from none.
    EXPECT_GT(std::fabs(t2 - t_w), 1.0)
        << "radiative-only settled at " << t2 << " C against " << t_w
        << " C with both families — too close to discriminate";
    EXPECT_LT(std::fabs(r0) / rp, 0.05)
        << "with SURFACE_EXCHANGE off the node settled at " << t2
        << " C, which is " << (std::fabs(r0) / rp) << " C from where the "
           "RADIATIVE flux alone vanishes (residual " << r0 << " W/m2). A "
           "disabled family is still contributing to netFluxOut.";
    swmm_engine_destroy(e2);
}
