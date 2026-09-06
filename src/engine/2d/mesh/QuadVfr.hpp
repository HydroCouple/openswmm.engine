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
 * @file QuadVfr.hpp
 * @brief Volume–free-surface relationship (VFR) for a quadrilateral cell.
 *
 * @details Begnudelli & Sanders (2007), "Conservative Wetting and Drying
 *          Methodology for Quadrilateral Grid Finite-Volume Models", JHE
 *          133(3):312–322. Four vertices do not in general lie on a plane, so
 *          the cell is split along a diagonal into two planar sub-triangles
 *          for the purpose of budgeting storage; the diagonal is chosen from
 *          the elevation ordering n1 ≤ n2 ≤ n3 ≤ n4 (their Cases 1–3):
 *
 *            Case 1  n1, n4 diagonally opposite     → diagonal n1–n4,
 *                    sub-triangles (n1,n2,n4), (n1,n3,n4)
 *            Case 2  n1, n4 adjacent, n2 adjacent to n1 → diagonal n2–n4,
 *                    sub-triangles (n1,n2,n4), (n2,n3,n4)
 *            Case 3  n1, n4 adjacent, n2 adjacent to n4 → diagonal n3–n4,
 *                    sub-triangles (n1,n3,n4), (n2,n3,n4)
 *
 *          The paper's Eqs. 8–13 (piecewise cubic / quadratic / linear
 *          h(η) with the Appendix coefficient tables) are exactly the SUM of
 *          the two planar-triangle VFRs (B&S 2006, VfrClosure.hpp) weighted
 *          by the sub-triangle areas:
 *
 *            h̄(η) = [ A₁·d̄(η; z_a,z_b,z_d) + A₂·d̄(η; z_a,z_c,z_d) ] / (A₁+A₂)
 *
 *          This header implements that sum directly (the plan's "Q-A"
 *          generic form), reusing the tested, ε-regularised triangle closure
 *          so quads and triangles share one regularisation and one
 *          round-trip contract. The inverse η(h̄) is a safeguarded Newton on
 *          the monotone sum (closed form when fully wet). The closed-form
 *          Appendix coefficients are an optional optimisation ("Q-B") and
 *          would be unit-tested against these functions.
 *
 *          Per-quad data is precomputed once (quadVfrPrecompute): the six
 *          sub-triangle vertex elevations, each triple sorted, and the two
 *          planimetric sub-triangle areas. Everything here is device-callable
 *          scalar arithmetic like the rest of the 2D kernels.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_QUAD_VFR_HPP
#define OPENSWMM_ENGINE_2D_QUAD_VFR_HPP

#include <cmath>

#include "VfrClosure.hpp"

namespace openswmm::twoD {

/// Number of doubles of precomputed elevation data per quad cell: two sorted
/// sub-triangle triples.
inline constexpr int kQuadVfrZ = 6;

/// Wetted-area fraction of the quad at stage η (dh̄/dη away from the ε-tail).
/// @p zs: six sorted elevations (z1,z2,z3 of sub-triangle 1, then of 2).
OPENSWMM_KERNEL_FN double quadWetFraction(const double* zs, double A1, double A2,
                                          double eta) noexcept {
    const double w1 = vfrWetFraction(zs[0], zs[1], zs[2], eta);
    const double w2 = vfrWetFraction(zs[3], zs[4], zs[5], eta);
    return (A1 * w1 + A2 * w2) / (A1 + A2);
}

/// Regularised cell-mean depth h̄(η) of the quad — the area-weighted sum of the
/// two sub-triangle closures (exact for eps == 0).
OPENSWMM_KERNEL_FN double quadMeanDepthFromEta(const double* zs, double A1, double A2,
                                               double eta, double eps) noexcept {
    const double d1 = vfrMeanDepthFromEta(zs[0], zs[1], zs[2], eta, eps);
    const double d2 = vfrMeanDepthFromEta(zs[3], zs[4], zs[5], eta, eps);
    return (A1 * d1 + A2 * d2) / (A1 + A2);
}

/// Slope dh̄/dη of the REGULARISED sum at η: each sub-triangle contributes its
/// wet fraction above its switch stage, eps inside its linear tail, and 0
/// once its tail has reached zero depth. Used by the Newton inverse.
OPENSWMM_KERNEL_FN double quadMeanDepthSlope(const double* zs, double A1, double A2,
                                             double eta, double eps) noexcept {
    double s = 0.0;
    for (int k = 0; k < 2; ++k) {
        const double z1 = zs[3 * k], z2 = zs[3 * k + 1], z3 = zs[3 * k + 2];
        const double A  = (k == 0) ? A1 : A2;
        const double relief = z3 - z1;
        double w;
        if (eps <= 0.0 || relief < kVfrFlatRelief) {
            w = vfrWetFraction(z1, z2, z3, eta);
        } else {
            const double eta_s = vfrStageAtWetFraction(z1, z2, z3, eps);
            if (eta >= eta_s) {
                w = vfrWetFraction(z1, z2, z3, eta);
            } else {
                const double d = vfrMeanDepthFromEta(z1, z2, z3, eta, eps);
                w = (d > 0.0) ? eps : 0.0;
            }
        }
        s += A * w;
    }
    return s / (A1 + A2);
}

/// Regularised free-surface elevation η(h̄) of the quad — the solver closure.
/// Monotone inverse of quadMeanDepthFromEta for the same eps; fully wet
/// reduces to the flat closure η = z̄_w + h̄ exactly, where z̄_w is the
/// area-weighted mean of the two sub-triangle mean beds (== the quad's mean
/// bed over its two-plane model).
OPENSWMM_KERNEL_FN double quadEtaFromMeanDepth(const double* zs, double A1, double A2,
                                               double mean_depth, double eps) noexcept {
    const double A     = A1 + A2;
    const double zbar1 = (zs[0] + zs[1] + zs[2]) / 3.0;
    const double zbar2 = (zs[3] + zs[4] + zs[5]) / 3.0;
    const double zw    = (A1 * zbar1 + A2 * zbar2) / A;
    const double ztop  = (zs[2] > zs[5]) ? zs[2] : zs[5];
    const double zlow  = (zs[0] < zs[3]) ? zs[0] : zs[3];
    const double relief = ztop - zlow;

    // Flat (or degenerate) cell: the flat closure is exact.
    if (relief < kVfrFlatRelief)
        return zw + ((mean_depth > 0.0) ? mean_depth : 0.0);

    // Fully wet: both sub-triangles submerged — flat closure exact.
    if (mean_depth >= ztop - zw) return zw + mean_depth;

    // Dry limit: the stage at which the regularised sum reaches zero — the
    // lower of the two sub-triangle dry stages (each tail ends at η_s − h_s/ε).
    const double dry1 = vfrDryEta(zs[0], zs[1], zs[2], eps);
    const double dry2 = vfrDryEta(zs[3], zs[4], zs[5], eps);
    const double lo0  = (dry1 < dry2) ? dry1 : dry2;
    if (!(mean_depth > 0.0)) return lo0;

    // Safeguarded Newton on the monotone sum over [lo0, ztop].
    double lo = lo0, hi = ztop;
    double eta = zw + mean_depth;                     // flat-closure guess
    if (eta <= lo || eta >= hi) eta = 0.5 * (lo + hi);
    for (int it = 0; it < 80; ++it) {
        const double f = quadMeanDepthFromEta(zs, A1, A2, eta, eps) - mean_depth;
        if (f > 0.0) hi = eta; else lo = eta;
        const double df = quadMeanDepthSlope(zs, A1, A2, eta, eps);
        double next = (df > 1.0e-12) ? eta - f / df : 0.5 * (lo + hi);
        if (next <= lo || next >= hi) next = 0.5 * (lo + hi);   // safeguard
        if (std::abs(next - eta) < 1.0e-13 * (1.0 + relief)) return next;
        eta = next;
    }
    return eta;
}

/// dη/dh̄ of the regularised quad closure at stage η: 1/max(slope, eps).
OPENSWMM_KERNEL_FN double quadDEtaDMeanDepth(const double* zs, double A1, double A2,
                                             double eta, double eps) noexcept {
    double w = quadMeanDepthSlope(zs, A1, A2, eta, eps);
    if (w < eps) w = eps;
    if (w < 1.0e-12) w = 1.0e-12;
    return 1.0 / w;
}

/**
 * @brief Choose the B&S 2007 diagonal for a quad and emit its precomputed
 *        VFR data.
 *
 * @param x,y,z   the four vertices in CYCLIC order (either orientation)
 * @param zs      out: six elevations — sorted triple of sub-triangle 1, then
 *                sorted triple of sub-triangle 2
 * @param A1,A2   out: planimetric areas of the two sub-triangles
 * @return the case number (1, 2 or 3) — for tests / diagnostics
 */
inline int quadVfrPrecompute(const double* x, const double* y, const double* z,
                             double* zs, double& A1, double& A2) noexcept {
    // Sort the four cyclic positions by elevation (stable insertion sort so
    // ties keep cyclic order — any consistent choice is a valid split).
    int p[4] = {0, 1, 2, 3};
    for (int i = 1; i < 4; ++i) {
        const int key = p[i];
        int j = i - 1;
        while (j >= 0 && z[p[j]] > z[key]) { p[j + 1] = p[j]; --j; }
        p[j + 1] = key;
    }
    const int n1 = p[0], n2 = p[1], n3 = p[2], n4 = p[3];
    auto adjacent = [](int a, int b) { const int d = (a - b + 4) % 4; return d == 1 || d == 3; };

    int t1[3], t2[3], kase;
    if (!adjacent(n1, n4)) {           // Case 1: n1–n4 is a diagonal
        kase = 1;
        t1[0] = n1; t1[1] = n2; t1[2] = n4;
        t2[0] = n1; t2[1] = n3; t2[2] = n4;
    } else if (adjacent(n2, n1)) {     // Case 2: diagonal n2–n4
        kase = 2;
        t1[0] = n1; t1[1] = n2; t1[2] = n4;
        t2[0] = n2; t2[1] = n3; t2[2] = n4;
    } else {                           // Case 3: diagonal n3–n4
        kase = 3;
        t1[0] = n1; t1[1] = n3; t1[2] = n4;
        t2[0] = n2; t2[1] = n3; t2[2] = n4;
    }
    auto tri_area = [&](const int* t) {
        const double dx1 = x[t[1]] - x[t[0]], dy1 = y[t[1]] - y[t[0]];
        const double dx2 = x[t[2]] - x[t[0]], dy2 = y[t[2]] - y[t[0]];
        return 0.5 * std::abs(dx1 * dy2 - dx2 * dy1);
    };
    A1 = tri_area(t1);
    A2 = tri_area(t2);
    zs[0] = z[t1[0]]; zs[1] = z[t1[1]]; zs[2] = z[t1[2]];
    zs[3] = z[t2[0]]; zs[4] = z[t2[1]]; zs[5] = z[t2[2]];
    vfrSort3(zs[0], zs[1], zs[2]);
    vfrSort3(zs[3], zs[4], zs[5]);
    // Degenerate sub-triangle (collinear): fold everything into the other so
    // the closure stays well-defined (A1 + A2 > 0 was validated upstream).
    if (!(A1 > 0.0)) { A1 = 0.0; }
    if (!(A2 > 0.0)) { A2 = 0.0; }
    return kase;
}

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_QUAD_VFR_HPP
