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
 * @brief D-H5d gates — the semi-implicit surface-balance step.
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
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6.2 D-H5d
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
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
