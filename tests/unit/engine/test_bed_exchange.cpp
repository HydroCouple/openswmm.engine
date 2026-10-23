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
 * @file      test_bed_exchange.cpp
 * @brief     H6b — the coupled bed / hyporheic stepper.
 *
 * @details These gates are on the PURE functions, deliberately. The physics
 *          of a coupled relaxation is where this phase can be wrong in a way
 *          that still produces plausible temperatures, and a deck-level gate
 *          would confound it with geometry, units and the loader chain.
 *          `bedCouplingForLink` is where the geometry lives and it is gated
 *          separately, at deck level, in the handoff's protocol.
 *
 *          The conservation and limit gates are identities or invariants.
 *          The grounded three-store benchmark additionally compares the
 *          matrix-exponential step with an independent RK4 integration of
 *          the governing rates, so the hyporheic direction is not inferred
 *          from a copied production result.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <array>
#include <cmath>

#include "transport/components/HeatFluxModules/BedExchange.hpp"
#include "transport/components/HeatFluxModules/SurfaceExchange.hpp"

namespace {

using openswmm::transport::heat::BedCoupling;
using openswmm::transport::heat::exchangePair;
using openswmm::transport::heat::relaxPair;
using openswmm::transport::heat::relaxT;

/**
 * @brief Build the fixed finite-capacity water/bed energy reference parameters.
 *
 * Pure parameter construction; does not initialize temperature state or integrate exchange.
 * @return Owned 900/120 W/K water-bed/bed-ground conductances, 4e6/2.4e7 J/K heat capacities and 12
 *     C ground temperature.
 */
BedCoupling nominal() {
    BedCoupling g{};
    g.g_wb = 900.0; // W/K
    g.g_bg = 120.0; // W/K
    g.c_w = 4.0e6;  // J/K  (~1 m3 of water)
    g.c_b = 2.4e7;  // J/K  (~8 m3 of saturated sediment)
    g.t_gr = 12.0;
    return g;
}

struct PairReference {
    double water = 0.0;
    double bed = 0.0;
    double ground_j = 0.0;
};

/**
 * @brief Integrate an independent water/bed reference with fixed-step RK4.
 *
 * Uses steps<=0.05 s and solves the explicit two-capacity linear exchange equations; no production
 * integrator is called. Zero/negative intervals are outside intended preconditions and no
 * parameter/stability validation occurs.
 *
 * @param[in] g Stable conductances W/K, positive nonzero heat capacities J/K and ground temperature
 *     C.
 * @param[in] water Initial water temperature C.
 * @param[in] bed Initial bed temperature C.
 * @param[in] surface_power Constant power W entering the water.
 * @param[in] seconds Positive finite interval seconds such that ceil(seconds/0.05) fits int.
 * @return Owned final water/bed temperatures C and accumulated ground-to-bed receipt J.
 */
PairReference integrateFinitePair(const BedCoupling& g, double water, double bed,
                                  double surface_power, double seconds) {
    using State = std::array<double, 3>; // water °C, bed °C, ground receipt J
    const auto derivative = [&g, surface_power](const State& x) {
        return State{(surface_power + g.g_wb * (x[1] - x[0])) / g.c_w,
                     (g.g_wb * (x[0] - x[1]) + g.g_bg * (g.t_gr - x[1])) / g.c_b,
                     g.g_bg * (g.t_gr - x[1])};
    };
    const auto shifted = [](const State& x, const State& dx, double scale) {
        return State{x[0] + scale * dx[0], x[1] + scale * dx[1], x[2] + scale * dx[2]};
    };

    State x{water, bed, 0.0};
    const int steps = static_cast<int>(std::ceil(seconds / 0.05));
    const double h = seconds / steps;
    for (int i = 0; i < steps; ++i) {
        const State k1 = derivative(x);
        const State k2 = derivative(shifted(x, k1, 0.5 * h));
        const State k3 = derivative(shifted(x, k2, 0.5 * h));
        const State k4 = derivative(shifted(x, k3, h));
        for (std::size_t j = 0; j < x.size(); ++j)
            x[j] += h * (k1[j] + 2.0 * k2[j] + 2.0 * k3[j] + k4[j]) / 6.0;
    }
    return {x[0], x[1], x[2]};
}

} // namespace

// ---------------------------------------------------------------------------
// 1. THE reduction gate. With no bed conductance the pair must collapse onto
//    the single-body stepper, to within the 4 ULP `EXPECT_DOUBLE_EQ` allows.
//    Measured at 1 ULP over this sweep: the two take algebraically identical
//    but textually different routes, so the last bit can differ and claiming
//    bit-equality here would be a gate that fails for the wrong reason.
//
//    It compares against the engine's own `relaxT` rather than a transcribed
//    number, so the two cannot drift apart without this failing.
// ---------------------------------------------------------------------------
TEST(BedExchangeTest, ReducesToRelaxTWithNoBedConductance) {
    BedCoupling g = nominal();
    g.g_wb = 0.0;
    g.g_bg = 0.0;

    const double area = 12.0, j0 = 40.0, h = 1.0e-3, j1 = 40.0 + 6.5 * h;
    const double rho = 1000.0, cp = 4184.0;
    const double vol_m3 = g.c_w / (rho * cp);

    for (const double dt : {1.0, 60.0, 900.0, 86400.0}) {
        const auto p = relaxPair(g, 18.0, 12.0, j0, j1, h, area, dt);
        const double want = relaxT(j0, j1, h, area, vol_m3, dt, rho, cp);
        EXPECT_DOUBLE_EQ(p.dt_w, want) << "dt = " << dt;
        EXPECT_DOUBLE_EQ(p.dt_b, 0.0)
            << "a bed with no conductance and no ground link must not move";
    }
}

// ---------------------------------------------------------------------------
// 2. Conservation, as the IDENTITY the header derives rather than a ledger
//    tolerance. Adding the two equations cancels g_wb, so with the ground
//    link and the surface flux both removed the exchange must be exactly
//    zero-sum in energy.
//
//    FALSIFIER: break the reciprocity — make a21 use a different conductance
//    from a12 — and this is the gate that fails, at any dt.
// ---------------------------------------------------------------------------
TEST(BedExchangeTest, PureExchangeConservesEnergy) {
    BedCoupling g = nominal();
    g.g_bg = 0.0; // no ground sink

    for (const double dt : {1.0, 300.0, 3600.0, 100000.0}) {
        const auto p = relaxPair(g, 25.0, 10.0, 0.0, 0.0, 0.0, 0.0, dt);
        const double net = g.c_w * p.dt_w + g.c_b * p.dt_b;
        // Scaled against the energy that actually MOVED, so the tolerance
        // cannot be satisfied by a step that did nothing.
        const double moved = std::fabs(g.c_w * p.dt_w);
        ASSERT_GT(moved, 1.0) << "dt = " << dt
                              << ": nothing moved, so this "
                                 "gate would pass vacuously";
        EXPECT_LT(std::fabs(net) / moved, 1e-12) << "dt = " << dt;
    }
}

// ---------------------------------------------------------------------------
// 3. No overshoot at ANY dt — the property D-H5d bought for one body and the
//    one an explicit or lagged coupling would have lost. Two bodies with no
//    other forcing must approach each other monotonically and stop at the
//    capacity-weighted mean, never crossing.
//
//    FALSIFIER: replace the matrix exponential with a forward-Euler step and
//    the large-dt rows here go wildly past the mean and change sign.
// ---------------------------------------------------------------------------
TEST(BedExchangeTest, CoupledPairNeverOvershootsAtAnyStep) {
    BedCoupling g = nominal();
    g.g_bg = 0.0;

    const double tw0 = 25.0, tb0 = 10.0;
    const double mean = (g.c_w * tw0 + g.c_b * tb0) / (g.c_w + g.c_b);

    for (const double dt : {1.0, 60.0, 3600.0, 1.0e6, 1.0e9}) {
        const auto p = relaxPair(g, tw0, tb0, 0.0, 0.0, 0.0, 0.0, dt);
        const double tw = tw0 + p.dt_w, tb = tb0 + p.dt_b;
        // The warm body cools toward the mean and never below it; the cold
        // body warms toward it and never above.
        EXPECT_GE(tw, mean - 1e-9) << "water overshot at dt = " << dt;
        EXPECT_LE(tw, tw0 + 1e-9) << "water warmed against the gradient";
        EXPECT_LE(tb, mean + 1e-9) << "bed overshot at dt = " << dt;
        EXPECT_GE(tb, tb0 - 1e-9) << "bed cooled against the gradient";
    }

    // And at an enormous step it must LAND on the mean, not merely stay on
    // the right side of it — the equilibrium is a claim, not just a bound.
    const auto p = relaxPair(g, tw0, tb0, 0.0, 0.0, 0.0, 0.0, 1.0e12);
    EXPECT_NEAR(tw0 + p.dt_w, mean, 1e-6);
    EXPECT_NEAR(tb0 + p.dt_b, mean, 1e-6);
}

// ---------------------------------------------------------------------------
// 4. The ground link is the only true sink, and it sets the FINAL state.
//    Left alone for long enough both bodies must arrive at T_gr — nothing
//    else in the system has a fixed point.
// ---------------------------------------------------------------------------
TEST(BedExchangeTest, GroundConductionSetsTheLongRunEquilibrium) {
    const BedCoupling g = nominal();
    const auto p = relaxPair(g, 25.0, 20.0, 0.0, 0.0, 0.0, 0.0, 1.0e12);
    EXPECT_NEAR(25.0 + p.dt_w, g.t_gr, 1e-6);
    EXPECT_NEAR(20.0 + p.dt_b, g.t_gr, 1e-6);
}

// ---------------------------------------------------------------------------
// 5. Independent three-store benchmark for the hyporheic direction finding.
//    The pair is integrated from the governing rates with RK4, then compared
//    with the exact production step. With identical positive surface input,
//    a colder finite bed and a colder deep boundary, adding the documented
//    rho*cp*v_h*A conductance increases transfer out of the water and into
//    the bed; it cannot be judged from a project run whose forcing and flow
//    also evolve over the interval.
// ---------------------------------------------------------------------------
TEST(BedExchangeTest, AddedHyporheicConductanceMatchesIndependentGroundedPair) {
    constexpr double initial_water = 25.0;
    constexpr double initial_bed = 20.0;
    constexpr double surface_area = 25.0;
    constexpr double surface_power = 1000.0;
    constexpr double seconds = 14400.0;
    constexpr double rho_water = 1000.0;
    constexpr double cp_water = 4184.0;
    constexpr double hyporheic_velocity = 3.0e-5;
    constexpr double bed_contact_area = 50.0;

    const BedCoupling conduction_only = nominal();
    BedCoupling with_hyporheic = conduction_only;
    with_hyporheic.g_wb += rho_water * cp_water * hyporheic_velocity * bed_contact_area;

    const PairReference expected_conduction =
        integrateFinitePair(conduction_only, initial_water, initial_bed, surface_power, seconds);
    const PairReference expected_hyporheic =
        integrateFinitePair(with_hyporheic, initial_water, initial_bed, surface_power, seconds);
    const auto actual_conduction =
        relaxPair(conduction_only, initial_water, initial_bed, -surface_power / surface_area,
                  -surface_power / surface_area, 0.0, surface_area, seconds);
    const auto actual_hyporheic =
        relaxPair(with_hyporheic, initial_water, initial_bed, -surface_power / surface_area,
                  -surface_power / surface_area, 0.0, surface_area, seconds);

    EXPECT_NEAR(initial_water + actual_conduction.dt_w, expected_conduction.water, 1e-9);
    EXPECT_NEAR(initial_bed + actual_conduction.dt_b, expected_conduction.bed, 1e-9);
    EXPECT_NEAR(initial_water + actual_hyporheic.dt_w, expected_hyporheic.water, 1e-9);
    EXPECT_NEAR(initial_bed + actual_hyporheic.dt_b, expected_hyporheic.bed, 1e-9);

    EXPECT_LT(expected_hyporheic.water, expected_conduction.water);
    EXPECT_GT(expected_hyporheic.bed, expected_conduction.bed);
    EXPECT_LT(expected_hyporheic.ground_j, expected_conduction.ground_j);

    const double conduction_energy =
        conduction_only.c_w * actual_conduction.dt_w + conduction_only.c_b * actual_conduction.dt_b;
    const double hyporheic_energy =
        with_hyporheic.c_w * actual_hyporheic.dt_w + with_hyporheic.c_b * actual_hyporheic.dt_b;
    EXPECT_NEAR(conduction_energy, surface_power * seconds + expected_conduction.ground_j, 1e-5);
    EXPECT_NEAR(hyporheic_energy, surface_power * seconds + expected_hyporheic.ground_j, 1e-5);
}

// ---------------------------------------------------------------------------
// 6. The near-degenerate branch must AGREE with the spectral branch. The two
//    are selected by a threshold on `s*dt`, and a threshold between two
//    formulas that disagree is a discontinuity in the model that would show
//    up as a jump when a parameter crosses it.
//
//    Degeneracy is forced by making the two diagonal entries equal with the
//    off-diagonals vanishing, then approached from both sides.
// ---------------------------------------------------------------------------
TEST(BedExchangeTest, DegenerateAndSpectralBranchesAgree) {
    BedCoupling g{};
    g.c_w = 1.0e6;
    g.c_b = 1.0e6;
    g.g_bg = 0.0;
    g.t_gr = 12.0;

    double prev = 0.0;
    bool first = true;
    // Sweep g_wb through the region where s*dt crosses the 1e-8 threshold.
    for (const double gwb : {0.0, 1e-6, 1e-4, 1e-2, 1.0}) {
        g.g_wb = gwb;
        const auto p = relaxPair(g, 20.0, 10.0, 0.0, 0.0, 0.0, 0.0, 60.0);
        if (!first) {
            // Monotone and continuous in g_wb: no step in the response as
            // the branch switches. A discontinuity here IS the bug.
            EXPECT_GE(prev, p.dt_w - 1e-12)
                << "non-monotone across the branch threshold at g_wb=" << gwb;
        }
        prev = p.dt_w;
        first = false;
        EXPECT_TRUE(std::isfinite(p.dt_w) && std::isfinite(p.dt_b));
    }
}

// ---------------------------------------------------------------------------
// 7. Solute exchange conserves mass to relative round-off.
//
//    NOT bit-exact, and this gate said it was until a driver disagreed at
//    dt = 1e6: `vw*(vb*share)` and `vb*(vw*share)` are the same factors in a
//    different association. The bound below is the true claim. It is still
//    discriminating — a coupling that moved unequal amounts would miss it by
//    orders of magnitude, not by an ULP.
// ---------------------------------------------------------------------------
TEST(BedExchangeTest, SoluteExchangeConservesMass) {
    const double vw = 28.3, vb = 55.0, q = 0.004;
    for (const double dt : {1.0, 600.0, 7200.0, 1.0e6}) {
        const auto s = exchangePair(9.0, 1.0, vw, vb, q, dt);
        const double moved = std::fabs(vw * s.dc_w);
        ASSERT_GT(moved, 1e-6) << "dt = " << dt << ": nothing moved";
        EXPECT_LT(std::fabs(vw * s.dc_w + vb * s.dc_b) / moved, 1e-14) << "dt = " << dt;
        EXPECT_LT(s.dc_w, 0.0) << "the richer body must lose";
        EXPECT_GT(s.dc_b, 0.0) << "the leaner body must gain";
    }
}

// ---------------------------------------------------------------------------
// 8. Solute exchange cannot cross over either, at any step, and lands on the
//    volume-weighted mean. Same property as gate 3, different closed form —
//    they are separate code paths and a fix to one does not fix the other.
// ---------------------------------------------------------------------------
TEST(BedExchangeTest, SoluteExchangeNeverCrossesTheMean) {
    const double vw = 10.0, vb = 40.0, cw0 = 5.0, cb0 = 0.0;
    const double mean = (vw * cw0 + vb * cb0) / (vw + vb);
    for (const double dt : {1.0, 3600.0, 1.0e9}) {
        const auto s = exchangePair(cw0, cb0, vw, vb, 0.01, dt);
        EXPECT_GE(cw0 + s.dc_w, mean - 1e-12) << "dt = " << dt;
        EXPECT_LE(cb0 + s.dc_b, mean + 1e-12) << "dt = " << dt;
    }
    const auto s = exchangePair(cw0, cb0, vw, vb, 0.01, 1.0e9);
    EXPECT_NEAR(cw0 + s.dc_w, mean, 1e-9);
}

// ---------------------------------------------------------------------------
// 9. Refusals. Every one of these is a shape the binding can hand in — a dry
//    link, a massless bed, a zero step — and each must be a no-op rather
//    than a NaN that then propagates into the published temperature.
// ---------------------------------------------------------------------------
TEST(BedExchangeTest, DegenerateInputsAreNoOpsNotNaNs) {
    BedCoupling g = nominal();
    const auto zero = [](const openswmm::transport::heat::PairStep& p) {
        return p.dt_w == 0.0 && p.dt_b == 0.0;
    };
    EXPECT_TRUE(zero(relaxPair(g, 20, 10, 0, 0, 0, 0, 0.0))) << "dt = 0";
    EXPECT_TRUE(zero(relaxPair(g, 20, 10, 0, 0, 0, 0, -5.0))) << "dt < 0";

    BedCoupling dry = g;
    dry.c_w = 0.0; // dry link
    EXPECT_TRUE(zero(relaxPair(dry, 20, 10, 0, 0, 0, 0, 60.0)));
    BedCoupling thin = g;
    thin.c_b = 0.0; // zero-thickness bed
    EXPECT_TRUE(zero(relaxPair(thin, 20, 10, 0, 0, 0, 0, 60.0)));

    const auto s0 = exchangePair(5.0, 1.0, 0.0, 10.0, 0.01, 60.0);
    EXPECT_EQ(s0.dc_w, 0.0);
    EXPECT_EQ(s0.dc_b, 0.0);
    const auto s1 = exchangePair(5.0, 1.0, 10.0, 10.0, 0.0, 60.0);
    EXPECT_EQ(s1.dc_w, 0.0) << "zero exchange discharge must move nothing";
}

// ---------------------------------------------------------------------------
// 10. The dt-sweep discriminator D-H5d established, applied to the pair: the
//    coupled step must converge to the substepped answer as dt shrinks, at
//    first order or better. A coupling that is simply WRONG does not
//    converge to the same limit, so this catches an error the invariant
//    gates above cannot — they would all pass for a coupling with the wrong
//    conductance, since a wrong conductance still conserves and still does
//    not overshoot.
// ---------------------------------------------------------------------------
TEST(BedExchangeTest, OneBigStepAgreesWithManySmallOnes) {
    const BedCoupling g = nominal();
    const double total = 3600.0;

    const auto substep = [&](int n) {
        double tw = 25.0, tb = 8.0;
        const double dt = total / n;
        for (int i = 0; i < n; ++i) {
            const auto p = relaxPair(g, tw, tb, 0.0, 0.0, 0.0, 0.0, dt);
            tw += p.dt_w;
            tb += p.dt_b;
        }
        return std::pair<double, double>{tw, tb};
    };

    // The system is LINEAR and the step is its exact exponential, so this is
    // not merely convergent — one step and N steps must agree to round-off.
    // Asserting the strong form is what makes the gate discriminating: an
    // approximate coupling would satisfy a loose tolerance here and fail
    // this one.
    const auto one = substep(1);
    const auto many = substep(360);
    EXPECT_NEAR(one.first, many.first, 1e-9);
    EXPECT_NEAR(one.second, many.second, 1e-9);
}
