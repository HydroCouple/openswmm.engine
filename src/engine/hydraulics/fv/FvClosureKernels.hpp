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
 * @file FvClosureKernels.hpp
 * @brief The explicit-FV cross-section closure as a single-source POD kernel:
 *        one monotone cubic-Hermite table per section, with T ≡ dA/dh and
 *        I₁ ≡ ∫A by construction, and a Newton inverse on the same cubic.
 *
 * @details Plan FV1D_CLOSURE_KERNEL_PERF_PLAN_2026-09-11 §2a/§2a‴. The table is
 *          built once at mesh build (NetworkMeshBuilder::buildClosure) by
 *          sampling the EXACT section geometry (SectionGeometry.hpp) at N+1
 *          uniform depths on [0, y_full], with the tapered Preissmann slot
 *          folded in; above y_full the closure is the analytic slot line, as
 *          it always was. Every function here is a plain inline over scalars
 *          and a `const FvClosure&` — no pointers, no allocation, no
 *          exceptions, no dispatch on shape — so the same bodies compile for
 *          the CPU solver and, annotated, for the device backend.
 *
 *          Two classes share the struct:
 *            - kTabulated: the Hermite table (closed shapes, table-defined
 *              shapes, transects). A per panel is a cubic in the local
 *              coordinate t ∈ [0,1]; T is its derivative; I₁ its integral
 *              plus the running sum; R is interpolated linearly.
 *            - kPolynomial: open sections whose area is a polynomial of
 *              degree ≤ 2 (RECT_OPEN, TRAPEZOIDAL, TRIANGULAR). Closed forms
 *              in the hot loop — cheaper than any table gather — with an exact
 *              quadratic-formula inverse (plan §2a″ cost weighting).
 *
 *          Why a Hermite table with the exact width as node slope: the legacy
 *          A and W tables are independent tabulations, so Newton on them was
 *          unsafe and the solver paid Brent's ~6-8 evaluations and a
 *          dependent divide chain per inversion (185 ns on a circle, of which
 *          only ~28 ns were closure calls). Here the derivative IS the
 *          closure's own, monotone by the Fritsch–Carlson limiter, so a
 *          safeguarded Newton converges in 2-3 steps and cannot fail.
 *
 * @ingroup engine_fv
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_CLOSURE_KERNELS_HPP
#define OPENSWMM_ENGINE_FV_CLOSURE_KERNELS_HPP

#include <cmath>
#include <cstdint>
#include <type_traits>

// Same portable-kernel marker as FvKernels.hpp / InertialKernels.hpp.
#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::fv {

/// Panels of the closure table, uniform in depth on [0, y_full].
inline constexpr int kClosurePanels = 128;

/// The closure's representation class.
enum : uint8_t { kClosureTabulated = 0, kClosurePolynomial = 1 };

/**
 * @brief One section's closure. Fixed-size POD: inline arrays, no pointers.
 *
 * Node i sits at h_i = i·dh, dh = y_full / kClosurePanels. `A` and `M` are the
 * area and its slope (top width) at the nodes, `I1` the running integral of the
 * panel cubics, `R` the hydraulic radius. `j_of_a[k]` is the panel containing
 * area k·a_crown/kClosurePanels — the inverse's O(1) locate.
 *
 * All areas and widths are per CELL (barrels folded in); R is per barrel, as
 * everywhere else in the solver.
 */
struct FvClosure {
    double y_full   = 0.0;
    double dh       = 0.0;
    double inv_dh   = 0.0;
    double a_crown  = 0.0;   ///< A(y_full) incl. the tapered slot = A[N]
    double t_slot   = 0.0;   ///< slot top width above the crown
    double i1_crown = 0.0;   ///< I₁(y_full) = I1[N]
    double r_full   = 0.0;   ///< R above the crown
    double inv_a_crown_n = 0.0;   ///< kClosurePanels / a_crown

    // kClosurePolynomial: A = c2·h² + c1·h, P = p0 + p1·h on [0, y_full].
    double c2 = 0.0, c1 = 0.0, p0 = 0.0, p1 = 0.0;

    uint8_t kind = kClosureTabulated;

    double  A [kClosurePanels + 1] = {};
    double  M [kClosurePanels + 1] = {};
    double  I1[kClosurePanels + 1] = {};
    double  R [kClosurePanels + 1] = {};
    int16_t j_of_a[kClosurePanels + 1] = {};
};

static_assert(std::is_trivially_copyable_v<FvClosure>,
              "FvClosure must be a plain buffer a device backend can copy");
static_assert(std::is_standard_layout_v<FvClosure>,
              "FvClosure must be standard-layout");

namespace kernels {

/// A, T and I₁ at one depth, from one panel locate.
struct ClosureEval {
    double a  = 0.0;
    double t  = 0.0;
    double i1 = 0.0;
};

/// Panel index and local coordinate for a sub-crown depth (0 ≤ h < y_full).
OPENSWMM_KERNEL_FN void closureLocate(const FvClosure& c, double h,
                                      int& i, double& t) noexcept {
    double x = h * c.inv_dh;
    i = static_cast<int>(x);
    if (i < 0) i = 0;
    if (i > kClosurePanels - 1) i = kClosurePanels - 1;
    t = x - static_cast<double>(i);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
}

/// Hermite cubic on panel i at local t: value, d/dh and ∫₀ᵗ (× dh).
OPENSWMM_KERNEL_FN void closureHermite(const FvClosure& c, int i, double t,
                                       double& a, double& dadh,
                                       double& int_dh) noexcept {
    const double a0 = c.A[i], a1 = c.A[i + 1];
    const double m0 = c.M[i] * c.dh, m1 = c.M[i + 1] * c.dh;   // slopes in t
    const double t2 = t * t, t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;
    a = h00 * a0 + h10 * m0 + h01 * a1 + h11 * m1;
    const double d00 = 6.0 * t2 - 6.0 * t;
    const double d10 = 3.0 * t2 - 4.0 * t + 1.0;
    const double d01 = -6.0 * t2 + 6.0 * t;
    const double d11 = 3.0 * t2 - 2.0 * t;
    dadh = (d00 * a0 + d10 * m0 + d01 * a1 + d11 * m1) * c.inv_dh;
    const double t4 = t2 * t2;
    const double i00 = 0.5 * t4 - t3 + t;
    const double i10 = 0.25 * t4 - (2.0 / 3.0) * t3 + 0.5 * t2;
    const double i01 = -0.5 * t4 + t3;
    const double i11 = 0.25 * t4 - t3 / 3.0;
    int_dh = (i00 * a0 + i10 * m0 + i01 * a1 + i11 * m1) * c.dh;
}

/// A, T, I₁ at depth h — one locate, one Hermite evaluation.
OPENSWMM_KERNEL_FN ClosureEval closureEval(const FvClosure& c, double h) noexcept {
    ClosureEval e;
    if (h <= 0.0) return e;
    if (h >= c.y_full) {
        const double d = h - c.y_full;
        e.a  = c.a_crown + c.t_slot * d;
        e.t  = c.t_slot;
        e.i1 = c.i1_crown + c.a_crown * d + 0.5 * c.t_slot * d * d;
        return e;
    }
    if (c.kind == kClosurePolynomial) {
        e.a  = (c.c2 * h + c.c1) * h;
        e.t  = 2.0 * c.c2 * h + c.c1;
        e.i1 = (c.c2 * h / 3.0 + 0.5 * c.c1) * h * h;
        return e;
    }
    int i; double t;
    closureLocate(c, h, i, t);
    double a, dadh, idh;
    closureHermite(c, i, t, a, dadh, idh);
    e.a  = a;
    e.t  = (dadh > 0.0) ? dadh : 0.0;
    e.i1 = c.I1[i] + idh;
    return e;
}

OPENSWMM_KERNEL_FN double closureArea(const FvClosure& c, double h) noexcept {
    if (h <= 0.0) return 0.0;
    if (h >= c.y_full) return c.a_crown + c.t_slot * (h - c.y_full);
    if (c.kind == kClosurePolynomial) return (c.c2 * h + c.c1) * h;
    int i; double t;
    closureLocate(c, h, i, t);
    double a, dadh, idh;
    closureHermite(c, i, t, a, dadh, idh);
    return a;
}

OPENSWMM_KERNEL_FN double closureWidth(const FvClosure& c, double h) noexcept {
    if (h <= 0.0) return 0.0;
    if (h >= c.y_full) return c.t_slot;
    if (c.kind == kClosurePolynomial) return 2.0 * c.c2 * h + c.c1;
    int i; double t;
    closureLocate(c, h, i, t);
    double a, dadh, idh;
    closureHermite(c, i, t, a, dadh, idh);
    return (dadh > 0.0) ? dadh : 0.0;
}

OPENSWMM_KERNEL_FN double closureI1(const FvClosure& c, double h) noexcept {
    return closureEval(c, h).i1;
}

/// Hydraulic radius: linear between nodes (friction only), r_full above.
OPENSWMM_KERNEL_FN double closureHydRad(const FvClosure& c, double h) noexcept {
    if (h <= 0.0) return 0.0;
    if (h >= c.y_full) return c.r_full;
    if (c.kind == kClosurePolynomial) {
        const double p = c.p0 + c.p1 * h;
        return (p > 0.0) ? (c.c2 * h + c.c1) * h / p : 0.0;
    }
    int i; double t;
    closureLocate(c, h, i, t);
    return c.R[i] + t * (c.R[i + 1] - c.R[i]);
}

/**
 * @brief Depth from area — the inverse of closureArea on the same closure.
 *
 * Polynomial class: the quadratic formula in its cancellation-free form.
 * Tabulated class: the area-uniform index picks the panel, a linear seed
 * starts Newton on the panel cubic (whose derivative is the closure's own
 * top width, ≥ 0 by the monotone limiter), and a bisection bracket guards
 * every step. Exit at 1e-15·y_full in depth, as the Brent inverse did.
 */
OPENSWMM_KERNEL_FN double closureDepthOfArea(const FvClosure& c, double a) noexcept {
    if (a <= 0.0) return 0.0;
    if (a >= c.a_crown) return c.y_full + (a - c.a_crown) / c.t_slot;
    if (c.kind == kClosurePolynomial) {
        if (c.c2 <= 0.0) return (c.c1 > 0.0) ? a / c.c1 : 0.0;
        return 2.0 * a / (c.c1 + std::sqrt(c.c1 * c.c1 + 4.0 * c.c2 * a));
    }

    int k = static_cast<int>(a * c.inv_a_crown_n);
    if (k < 0) k = 0;
    if (k > kClosurePanels) k = kClosurePanels;
    int i = c.j_of_a[k];
    while (i > 0 && c.A[i] > a) --i;
    while (i < kClosurePanels - 1 && c.A[i + 1] < a) ++i;

    const double a0 = c.A[i], a1 = c.A[i + 1];
    if (a <= a0) return static_cast<double>(i) * c.dh;
    if (a >= a1) return static_cast<double>(i + 1) * c.dh;

    const double tol = 1.0e-15 * c.y_full * c.inv_dh;   // in units of t
    double lo = 0.0, hi = 1.0;
    const double frac = (a - a0) / (a1 - a0);
    // Seed: the secant, except in a panel whose lower node has zero width
    // (the invert of a round section, A ∝ t² there): sqrt overestimates by
    // ≤ √1.5 and Newton on the convex panel then descends monotonically.
    double t = (c.M[i] > 0.0) ? frac : std::sqrt(frac);
    for (int it = 0; it < 12; ++it) {
        double f, dadh, idh;
        closureHermite(c, i, t, f, dadh, idh);
        f -= a;
        if (f == 0.0) break;
        if (f > 0.0) hi = t; else lo = t;
        // Newton in the panel coordinate: dA/dt = dA/dh · dh. A step at
        // round-off is convergence, not an excursion (it lands ON the
        // bracket edge just set, which the bracket test would reject).
        const double dadt = dadh * c.dh;
        double step = (dadt > 0.0) ? -f / dadt : 0.0;
        if (dadt > 0.0 && std::fabs(step) <= tol) break;
        double tn = t + step;
        if (!(dadt > 0.0) || !(tn > lo && tn < hi)) tn = 0.5 * (lo + hi);
        step = tn - t;
        t = tn;
        if (std::fabs(step) <= tol) break;
    }
    return static_cast<double>(i) * c.dh + t * c.dh;
}

} // namespace kernels
} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_CLOSURE_KERNELS_HPP
