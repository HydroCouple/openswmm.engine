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
 * @file test_2d_exchange_law.cpp
 * @brief Properties of the WEIR_ORIFICE exchange law (2d/coupling/ExchangeLaw.hpp):
 *        sign, monotonicity, G ≥ 0 and G = −∂Q/∂h1, C⁰/C¹ across every regime
 *        boundary, the weir/orifice crossover, for every coefficient preset.
 *
 * @details plans/1D2D_FINE_SCALE_COUPLING_PLAN_2026-10-19.md §8.2 / §0R R5–R6.
 *          No engine, no data: the law is a pure function.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "2d/coupling/ExchangeLaw.hpp"

using namespace openswmm::twoD::exchange;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kZ  = 10.0;   // crest

struct Preset { std::string name; Coefficients c; };

std::vector<Preset> presets() {
    Coefficients dflt;                                  // 0.540, ⅔·0.540, 0.167, 0.160
    dflt.c_sw = 2.0 / 3.0 * dflt.c_w;
    Coefficients rub;   rub.c_w = 0.540; rub.c_sw = 0.056; rub.c_oi = 0.167; rub.c_oo = 0.160;
    Coefficients lower; lower.c_w = 0.493; lower.c_sw = 0.055; lower.c_oi = 0.160; lower.c_oo = 0.159;
    Coefficients upper; upper.c_w = 0.587; upper.c_sw = 0.057; upper.c_oi = 0.174; upper.c_oo = 0.161;
    Coefficients open;  open.c_w = open.c_oi = open.c_oo = 1.0; open.c_sw = 2.0 / 3.0;
    return {{"DEFAULT", dflt}, {"RUBINATO2017", rub}, {"RUBINATO2017_LOWER", lower},
            {"RUBINATO2017_UPPER", upper}, {"OPEN", open}};
}

Geometry manhole(double d = 1.2) { return {kZ, kPi * d, kPi * d * d / 4.0}; }

double Q(const Geometry& g, const Coefficients& c, double h2, double h1) {
    return exchange(g, c, h2, h1).q;
}

// One-sided slopes on either side of x along h1 (dir 1) or h2 (dir 2), step d.
struct Sides { double jump, slope_left, slope_right; };
Sides sides(const Geometry& g, const Coefficients& c, double h2, double h1, int dir,
            double d = 1.0e-7) {
    auto at = [&](double off) {
        return dir == 1 ? Q(g, c, h2, h1 + off) : Q(g, c, h2 + off, h1);
    };
    return {at(1e-10) - at(-1e-10), (at(-d) - at(-2 * d)) / d, (at(2 * d) - at(d)) / d};
}

} // namespace

// Grid scan around the crest: finite, the right sign, monotone (Q grows with
// the surface head and falls with the sewer head), G ≥ 0.
TEST(ExchangeLaw, SignMonotoneAndNonNegativeConductanceOnGrid) {
    const auto g = manhole();
    constexpr int n = 400;
    const double lo = kZ - 0.25, hi = kZ + 0.5, step = (hi - lo) / (n - 1);
    for (const auto& p : presets()) {
        for (int i = 0; i < n; ++i) {
            const double h2 = lo + i * step;
            double prev_h1 = 0.0;
            for (int j = 0; j < n; ++j) {
                const double h1 = lo + j * step;
                const auto r = exchange(g, p.c, h2, h1);
                ASSERT_TRUE(std::isfinite(r.q) && std::isfinite(r.g)) << p.name;
                if (h2 > h1) ASSERT_GE(r.q, 0.0) << p.name << " h2=" << h2 << " h1=" << h1;
                if (h1 > h2) ASSERT_LE(r.q, 0.0) << p.name << " h2=" << h2 << " h1=" << h1;
                if (h2 <= kZ && h1 <= kZ) ASSERT_EQ(r.q, 0.0) << p.name;
                ASSERT_GE(r.g, 0.0) << p.name << " h2=" << h2 << " h1=" << h1;
                if (j > 0) ASSERT_LE(r.q, prev_h1 + 1e-12)
                    << p.name << ": Q rose with the sewer head at h2=" << h2 << " h1=" << h1;
                prev_h1 = r.q;
                if (i > 0) ASSERT_GE(r.q, Q(g, p.c, h2 - step, h1) - 1e-12)
                    << p.name << ": Q fell with the surface head at h2=" << h2 << " h1=" << h1;
            }
        }
    }
}

// C⁰ and C¹ across every regime boundary, for every preset (§0R R5: the
// free/submerged switch is C¹ only because the σ-band blend is always on).
// The blend bends sharply over its narrow band, so the one-sided slopes are
// compared as the step shrinks: across a kink their gap stays put, across a
// C¹ point it closes in proportion to the step.
TEST(ExchangeLaw, ContinuousWithContinuousSlopeAcrossRegimeBoundaries) {
    const auto g = manhole();
    for (const auto& p : presets()) {
        auto expect_c1 = [&](const char* where, double h2, double h1, int dir) {
            const auto coarse = sides(g, p.c, h2, h1, dir, 1.0e-6);
            const auto fine   = sides(g, p.c, h2, h1, dir, 1.0e-8);
            const double scale = std::max({1.0, std::fabs(fine.slope_left), std::fabs(fine.slope_right)});
            const double gap_coarse = std::fabs(coarse.slope_right - coarse.slope_left);
            const double gap_fine   = std::fabs(fine.slope_right - fine.slope_left);
            EXPECT_LT(std::fabs(fine.jump), 1e-8) << p.name << " " << where << ": jump";
            EXPECT_LT(gap_fine, 0.05 * gap_coarse + 1e-5 * scale)
                << p.name << " " << where << ": slope gap " << gap_coarse << " -> " << gap_fine
                << " does not close with the step (a kink)";
        };
        for (double hs : {0.03, 0.12, 0.3}) {
            const double h2 = kZ + hs;
            expect_c1("sewer head at the crest (free/submerged)", h2, kZ, 1);
            expect_c1("submergence band edge", h2, kZ + 0.1 * hs, 1);
            expect_c1("surface head over a free sewer", h2, kZ - 0.3, 2);
        }
        for (double hp : {0.05, 0.3}) {
            expect_c1("surface wets over a spilling sewer", kZ, kZ + hp, 2);
            expect_c1("spill band edge", kZ + 0.1 * hp, kZ + hp, 2);
        }
        // Heads equal: C⁰ only — the drain and spill branches meet at Q = 0
        // with different slopes. This kink must stay visible to the method
        // above (the negative control for the C¹ checks).
        const auto kc = sides(g, p.c, kZ + 0.2, kZ + 0.2, 1, 1.0e-6);
        const auto kf = sides(g, p.c, kZ + 0.2, kZ + 0.2, 1, 1.0e-8);
        EXPECT_LT(std::fabs(kf.jump), 1e-8) << p.name << " heads equal: jump";
        const double kink_coarse = std::fabs(kc.slope_right - kc.slope_left);
        ASSERT_GT(kink_coarse, 1e-3) << p.name;
        EXPECT_GT(std::fabs(kf.slope_right - kf.slope_left), 0.5 * kink_coarse)
            << p.name << ": the slope-gap test cannot see a kink";
    }
}

// The weir governs shallow surface water, the orifice deeper water; with the
// default coefficients the controls cross at h_s = 1.5·C_oi·A/(C_w·L) ≈ 0.116 D,
// smoothly (Q ≤ the smaller term, within 4.3 % of it at the crossing).
TEST(ExchangeLaw, WeirGovernsShallowOrificeGovernsDeep) {
    const double d = 1.2;
    const auto g = manhole(d);
    Coefficients c;
    c.c_sw = 2.0 / 3.0 * c.c_w;
    const double s2g = std::sqrt(2.0 * kGravity);
    const double h_cross = 1.5 * c.c_oi * g.a_orif / (c.c_w * g.l_weir);
    EXPECT_NEAR(h_cross / d, 0.116, 0.001);
    for (double hs : {0.03, 0.05, 0.3, 0.5}) {
        const double weir = 2.0 / 3.0 * c.c_w * g.l_weir * s2g * std::pow(hs, 1.5);
        const double orif = c.c_oi * g.a_orif * s2g * std::sqrt(hs);
        const double q = Q(g, c, kZ + hs, kZ - 1.0);
        const double lim = std::min(weir, orif);
        EXPECT_LE(q, lim * (1 + 1e-12)) << "hs=" << hs;
        EXPECT_GT(q, 0.99 * lim) << "hs=" << hs << ": far from the crossing the limiting control governs";
        EXPECT_EQ(weir < orif, hs < h_cross) << "hs=" << hs;
    }
    const double at = Q(g, c, kZ + h_cross, kZ - 1.0);
    const double lim = c.c_oi * g.a_orif * s2g * std::sqrt(h_cross);
    EXPECT_NEAR(at / lim, std::pow(2.0, -1.0 / 16.0), 1e-9) << "p-norm blend at the crossing";
}

// G = −∂Q/∂h1 wherever the blend weights are constant (fully submerged
// surface→sewer, sewer→surface over a dry surface); G = 0 over a free sewer.
TEST(ExchangeLaw, ConductanceMatchesHeadDerivativeOutsideBlendBands) {
    const auto g = manhole();
    for (const auto& p : presets()) {
        auto fd = [&](double h2, double h1) {
            constexpr double e = 1e-6;
            return -(Q(g, p.c, h2, h1 + e) - Q(g, p.c, h2, h1 - e)) / (2 * e);
        };
        // Fully submerged: σ = (h1 − z)/h_s > 0.1.
        for (auto [h2, h1] : {std::pair{kZ + 0.30, kZ + 0.10}, std::pair{kZ + 0.10, kZ + 0.05}}) {
            const double gl = exchange(g, p.c, h2, h1).g, gf = fd(h2, h1);
            EXPECT_NEAR(gl, gf, 1e-4 * std::max(1.0, gf)) << p.name << " submerged";
        }
        // Sewer → surface over a dry surface.
        for (double hp : {0.05, 0.3}) {
            const double gl = exchange(g, p.c, kZ - 0.1, kZ + hp).g, gf = fd(kZ - 0.1, kZ + hp);
            EXPECT_NEAR(gl, gf, 1e-4 * std::max(1.0, gf)) << p.name << " spill";
        }
        EXPECT_EQ(exchange(g, p.c, kZ + 0.2, kZ - 0.5).g, 0.0) << p.name << " free sewer";
    }
}

// The φ regularisation is a parameter: at laboratory scale a 2 cm ε would sit
// inside every calibrated head (§0R R4); a scaled ε recovers √Δh exactly.
TEST(ExchangeLaw, RegularisationHeadIsAParameter) {
    EXPECT_DOUBLE_EQ(phi(0.009, 0.02 / 6.0), std::sqrt(0.009));
    EXPECT_LT(phi(0.009, 0.02), 0.95 * std::sqrt(0.009));
    EXPECT_DOUBLE_EQ(phi(0.03, 0.02), std::sqrt(0.03));
    EXPECT_GT(dphi(0.0, 0.02), 0.0);
    EXPECT_TRUE(std::isfinite(dphi(0.0, 0.02)));
}

TEST(ExchangeLaw, SoftminIsBelowTheMinimumAndZeroWithAZeroTerm) {
    double wa = 0, wb = 0;
    EXPECT_EQ(softmin(0.0, 1.0, wa, wb), 0.0);
    EXPECT_EQ(softmin(1.0, 0.0, wa, wb), 0.0);
    for (double a : {1e-12, 1e-3, 0.5, 1.0, 7.0})
        for (double b : {1e-12, 1e-3, 0.5, 1.0, 7.0}) {
            const double q = softmin(a, b, wa, wb);
            ASSERT_LE(q, std::min(a, b) * (1 + 1e-15));
            ASSERT_GE(q, std::min(a, b) * std::pow(2.0, -1.0 / 16.0) * (1 - 1e-12));
            constexpr double e = 1e-9;
            double w1, w2;
            EXPECT_NEAR(wa, (softmin(a * (1 + e), b, w1, w2) - softmin(a * (1 - e), b, w1, w2)) / (2 * a * e), 1e-5);
        }
}
