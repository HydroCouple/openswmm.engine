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
 * @file      test_2d_pond_continuity.cpp
 * @brief     The coupled-junction pond geometry (2d/coupling/PondModel.hpp): a C¹
 *        area ramp entirely above the rim, the closed-form volume and its
 *        exact inverse, volume-conserving relaxation of the pond area, and the
 *        head-equalisation cap.
 *
 * @details plans/1D2D_FINE_SCALE_COUPLING_PLAN_2026-10-19.md §4, §0R R8/R13.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>

#include "2d/coupling/PondModel.hpp"

using namespace openswmm::twoD::pond;

TEST(PondModel, AreaIsC1AndStartsAtTheRim) {
    const double a = 40.0, band = 0.05;
    EXPECT_EQ(area(0.0, a, band), 0.0) << "no pond area at or below the rim";
    EXPECT_EQ(area(-0.1, a, band), 0.0);
    EXPECT_EQ(area(band, a, band), a);
    EXPECT_EQ(area(1.0, a, band), a);
    constexpr double e = 1e-7;
    for (double x : {0.0, band}) { // band edges
        const double left = (area(x, a, band) - area(x - e, a, band)) / e;
        const double right = (area(x + e, a, band) - area(x, a, band)) / e;
        EXPECT_NEAR(left, right, 1e-3 * a / band) << "slope jump at x=" << x;
        EXPECT_NEAR(area(x + 1e-12, a, band), area(x - 1e-12, a, band), 1e-9);
    }
    EXPECT_EQ(area(0.01, a, 0.0), a) << "band 0 is the legacy step";
}

TEST(PondModel, VolumeIsTheIntegralOfArea) {
    const double a = 40.0, band = 0.05;
    double v = 0.0;
    const int n = 200000;
    const double dx = 0.2 / n;
    for (int i = 0; i < n; ++i) {
        const double x = (i + 0.5) * dx;
        v += area(x, a, band) * dx;
        if ((i + 1) % 20000 == 0)
            ASSERT_NEAR(volume((i + 1) * dx, a, band), v, 1e-9 * a) << "x=" << (i + 1) * dx;
    }
    EXPECT_NEAR(volume(0.3, a, 0.0), 12.0, 1e-12);
}

TEST(PondModel, InverseIsExact) {
    for (double band : {0.0, 0.01, 0.05, 0.3}) {
        for (double a : {0.5, 40.0, 2500.0}) {
            for (double x : {1e-6, 1e-3, 0.004, 0.02, 0.0499, 0.05, 0.07, 0.4, 3.0}) {
                const double v = volume(x, a, band);
                ASSERT_NEAR(depthForVolume(v, a, band), x, 1e-12 * std::max(1.0, x))
                    << "band " << band << " A " << a << " x " << x;
            }
        }
    }
    EXPECT_EQ(depthForVolume(0.0, 40.0, 0.05), 0.0);
}

// Relaxing the pond area toward a target at fixed held volume moves the depth
// (volume conserved to round-off), and the area converges to the target.
TEST(PondModel, RelaxationConservesVolumeAndConverges) {
    const double band = 0.05, target = 300.0, tau = 30.0, dt = 6.0;
    double a = 1.13;
    double x = 0.12;
    const double v0 = volume(x, a, band);
    for (int k = 0; k < 400; ++k) {
        const double v = volume(x, a, band);
        a += (dt / tau) * (target - a);
        x = depthForVolume(v, a, band);
        ASSERT_NEAR(volume(x, a, band), v0, 1e-12 * v0) << "step " << k;
    }
    EXPECT_NEAR(a, target, 1e-6 * target);
}

// Held for the window, the capped exchange never carries the 1D head past
// the 2D head: over the window the 1D side moves at most |Δh| — exactly |Δh|
// when the law is stronger than the cap. (The 2D cell is live: the law
// re-reads its head every substep.)
TEST(PondModel, EqualisationCapNeverCarriesThe1DHeadPastThe2D) {
    for (double a1 : {1.13, 10.0, 300.0})
        for (double dh : {-0.4, -0.01, 0.003, 0.2}) {
            const double T = 6.0;
            const double q = std::copysign(1e9, dh); // a law far stronger than the cap
            const double cap = equalizeCap(dh, T, a1);
            const double qc = std::copysign(std::min(std::fabs(q), cap), q);
            EXPECT_NEAR(qc * T / a1, dh, 1e-12 * std::fabs(dh)) << "a1 " << a1 << " dh " << dh;
        }
    EXPECT_EQ(equalizeCap(0.1, 0.0, 1.0), HUGE_VAL);
    EXPECT_EQ(equalizeCap(0.1, 1.0, 0.0), HUGE_VAL);
}
