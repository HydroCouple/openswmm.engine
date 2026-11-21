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
 * @file      SectionGeometry.hpp
 * @brief     Exact cross-section geometry — area, top width and wetted perimeter
 *        as closed forms of the section's defining dimensions.
 *
 * @details Build-time only (plan FV1D_CLOSURE_KERNEL_PERF_PLAN_2026-09-11 §2a′):
 *          the FV closure tables are sampled from these functions, never from
 *          the legacy 51-row lookup tables, so the solver's geometry is the
 *          section to interpolation accuracy rather than to table accuracy.
 *          Full-precision π; no legacy operand-order constraints — this header
 *          is not under the dynamic-wave bit-parity contract and must not be
 *          used by the dynamic-wave path.
 *
 *          Closed forms exist for CIRCULAR, FORCE_MAIN, FILLED_CIRCULAR,
 *          RECT_CLOSED, RECT_OPEN, TRAPEZOIDAL, TRIANGULAR, PARABOLIC and
 *          POWERFUNC (`hasClosedForm`). Every other shape — the composites
 *          RECT_TRIANG / RECT_ROUND / MOD_BASKET, the table-defined ARCH /
 *          EGGSHAPED / HORSESHOE / GOTHIC / CATENARY / SEMIELLIPTICAL /
 *          BASKETHANDLE / SEMICIRCULAR, and the transect shapes IRREGULAR /
 *          CUSTOM / STREET — is evaluated through the legacy evaluator, whose
 *          table (or piecewise formula) is that shape's definition. The
 *          closure built from them still gains the derivative pair T ≡ dA/dh
 *          and I₁ ≡ ∫A, which the legacy tables lack.
 *
 *          HORIZ_ELLIPSE and VERT_ELLIPSE are table-defined on purpose: SWMM's
 *          elliptical pipes are the standard elliptical concrete-pipe profiles,
 *          not mathematical ellipses — measured against a true ellipse of the
 *          same height and width, the legacy 3×4 ft horizontal section carries
 *          21 % more full area and a different perimeter. Treating them as
 *          ellipses would silently change what the shape keyword means.
 *
 *          Everything here is a plain function over `XSectParams`, host-only
 *          (`acos`, adaptive quadrature), and may be freely reused by a later
 *          dynamic-wave program under its own gates.
 *
 * @ingroup engine_fv
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_SECTION_GEOMETRY_HPP
#define OPENSWMM_ENGINE_FV_SECTION_GEOMETRY_HPP

#include <algorithm>
#include <cmath>

#include "../XSectBatch.hpp"

namespace openswmm::fv::secgeom {

inline constexpr double kPi = 3.14159265358979323846;

/// Legacy `RECT_ALFMAX` (xsect.c): the filling fraction above which a closed
/// rectangle's wetted perimeter is ramped toward the full-pipe value, so R(h)
/// reaches r_full continuously instead of jumping at the crown.
inline constexpr double kRectAlfMax = 0.97;

namespace detail {

/**
 * @brief Evaluate the central angle of a circular water segment.
 *
 * Clamps 1-y/r to [-1,1]; positive finite radius is required and NaN is not diagnosed.
 *
 * @param[in] r Positive circle radius in ft.
 * @param[in] y Finite water depth from the invert in ft.
 * @return Angle in radians in [0,2*pi].
 */
inline double circTheta(double r, double y) {
    double c = 1.0 - y / r;
    if (c > 1.0)
        c = 1.0;
    if (c < -1.0)
        c = -1.0;
    return 2.0 * std::acos(c);
}
/**
 * @brief Evaluate the exact wetted area of a circular segment.
 *
 * Uses the exact segment formula between invert and crown, without a lookup table.
 *
 * @param[in] r Positive circle radius in ft.
 * @param[in] y Finite water depth from the invert in ft.
 * @return Area in ft2; zero at/below the invert and full pi*r*r at/above the crown.
 */
inline double circArea(double r, double y) {
    if (y <= 0.0)
        return 0.0;
    if (y >= 2.0 * r)
        return kPi * r * r;
    const double th = circTheta(r, y);
    return 0.5 * r * r * (th - std::sin(th));
}
/**
 * @brief Evaluate the exact circular free-surface chord width.
 *
 * Uses 2*sqrt(y*(2*r-y)) only in the wet sub-crown interval.
 *
 * @param[in] r Positive circle radius in ft.
 * @param[in] y Finite water depth from the invert in ft.
 * @return Width in ft; zero at/below the invert or at/above the crown.
 */
inline double circWidth(double r, double y) {
    if (y <= 0.0 || y >= 2.0 * r)
        return 0.0;
    const double s = y * (2.0 * r - y);
    return (s > 0.0) ? 2.0 * std::sqrt(s) : 0.0;
}
/**
 * @brief Evaluate the exact submerged circular arc length.
 *
 * The chord/free surface is excluded from wetted perimeter.
 *
 * @param[in] r Positive circle radius in ft.
 * @param[in] y Finite water depth from the invert in ft.
 * @return Wetted perimeter in ft; zero at/below the invert and full circumference at/above the
 *     crown.
 */
inline double circPerim(double r, double y) {
    if (y <= 0.0)
        return 0.0;
    if (y >= 2.0 * r)
        return 2.0 * kPi * r;
    return r * circTheta(r, y);
}

/**
 * @brief Recursively refine a cached Simpson panel to an absolute error target.
 *
 * Samples two quarter points per call; stops when abs(delta)<=15*eps or the depth budget is
 * exhausted. No convergence status or nonfinite check is returned. Recursion uses stack storage,
 * and integrand exceptions propagate.
 *
 * @param[in,out] f Synchronous scalar integrand callable, borrowed by reference; its own side
 *     effects/exceptions are permitted.
 * @param[in] a Lower finite integration coordinate.
 * @param[in] b Upper finite coordinate, greater than a.
 * @param[in] fa Cached f(a).
 * @param[in] fm Cached f((a+b)/2).
 * @param[in] fb Cached f(b).
 * @param[in] whole Simpson estimate over [a,b], in integral units.
 * @param[in] eps Positive absolute error target in integral units; halved on each recursive child.
 * @param[in] depth Remaining subdivision depth; <=0 accepts the next refined estimate regardless of
 *     error.
 * @return Sum of refined Simpson panels plus delta/15 correction.
 */
template <class F>
double simpsonStep(F& f, double a, double b, double fa, double fm, double fb, double whole,
                   double eps, int depth) {
    const double m = 0.5 * (a + b);
    const double lm = 0.5 * (a + m), rm = 0.5 * (m + b);
    const double flm = f(lm), frm = f(rm);
    const double left = (m - a) / 6.0 * (fa + 4.0 * flm + fm);
    const double right = (b - m) / 6.0 * (fm + 4.0 * frm + fb);
    const double delta = left + right - whole;
    if (depth <= 0 || std::fabs(delta) <= 15.0 * eps)
        return left + right + delta / 15.0;
    return simpsonStep(f, a, m, fa, flm, fm, left, 0.5 * eps, depth - 1) +
           simpsonStep(f, m, b, fm, frm, fb, right, 0.5 * eps, depth - 1);
}
/**
 * @brief Integrate a scalar function with adaptive Simpson quadrature.
 *
 * No heap workspace is used by the quadrature itself. Callable copy/evaluation exceptions
 * propagate; numerical error bounds are not certified for singular or nonfinite integrands.
 *
 * @param[in] f Integrand copied for this call and invoked synchronously; no callable is retained.
 * @param[in] a Lower finite integration coordinate.
 * @param[in] b Upper finite coordinate; b<=a returns zero without evaluating f.
 * @param[in] eps Positive absolute integral error target; defaults to 1e-12.
 * @param[in] depth Maximum recursive refinement depth; default 40. Exhaustion accepts the latest
 *     corrected estimate.
 * @return Estimated integral in integrand-units times coordinate-units; no error/status object.
 */
template <class F> double integrate(F f, double a, double b, double eps = 1.0e-12, int depth = 40) {
    if (!(b > a))
        return 0.0;
    const double fa = f(a), fb = f(b), fm = f(0.5 * (a + b));
    const double whole = (b - a) / 6.0 * (fa + 4.0 * fm + fb);
    return simpsonStep(f, a, b, fa, fm, fb, whole, eps, depth);
}

/**
 * @brief Integrate the submerged arc of a mathematical ellipse.
 *
 * Uses adaptive angular quadrature with absolute target 1e-12*(ax+by). This mathematical ellipse
 * helper is not the table-defined SWMM HORIZ_ELLIPSE/VERT_ELLIPSE profile evaluator.
 *
 * @param[in] ax Positive horizontal semi-axis in ft.
 * @param[in] by Positive vertical semi-axis in ft.
 * @param[in] y Finite depth from the bottom in ft; <=0 returns zero and depths above 2*by saturate.
 * @return Both wetted side arcs in ft, excluding the free-surface chord.
 */
inline double ellipsePerim(double ax, double by, double y) {
    if (y <= 0.0)
        return 0.0;
    double c = 1.0 - y / by;
    if (c < -1.0)
        c = -1.0;
    if (c > 1.0)
        c = 1.0;
    const double phi_max = std::acos(c);
    auto ds = [&](double phi) {
        const double cx = ax * std::cos(phi), sy = by * std::sin(phi);
        return std::sqrt(cx * cx + sy * sy);
    };
    return 2.0 * integrate(ds, 0.0, phi_max, 1.0e-12 * (ax + by));
}

} // namespace detail

/**
 * @brief Identify shapes with an analytic area/width and direct or quadrature perimeter here.
 *
 * @param[in] type Kernel XSectShape numeric identifier; not the storage XsectShape numbering.
 * @return True for circular/force-main/filled-circular, rectangles, trapezoidal, triangular,
 *     parabolic and power-function shapes; false otherwise.
 */
inline bool hasClosedForm(int type) {
    switch (static_cast<XSectShape>(type)) {
    case XSectShape::CIRCULAR:
    case XSectShape::FORCE_MAIN:
    case XSectShape::FILLED_CIRCULAR:
    case XSectShape::RECT_CLOSED:
    case XSectShape::RECT_OPEN:
    case XSectShape::TRAPEZOIDAL:
    case XSectShape::TRIANGULAR:
    case XSectShape::PARABOLIC:
    case XSectShape::POWERFUNC:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Identify open sections represented by quadratic area and linear perimeter.
 *
 * @param[in] type Kernel XSectShape numeric identifier.
 * @return True only for RECT_OPEN, TRAPEZOIDAL and TRIANGULAR; false for unknown identifiers.
 */
inline bool isPolynomialOpen(int type) {
    switch (static_cast<XSectShape>(type)) {
    case XSectShape::RECT_OPEN:
    case XSectShape::TRAPEZOIDAL:
    case XSectShape::TRIANGULAR:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Evaluate per-barrel flow area for the FV closure builder.
 *
 * Uses exact analytic formulas for supported primitives; all other shapes dispatch to the legacy
 * evaluator using the shape-defining profiles. Requires valid supported non-DUMMY geometry for
 * fallback. No slot or barrel multiplier is included.
 *
 * @param[in] xs Borrowed initialized per-barrel native-ft geometry with positive y_full and valid
 *     shape coefficients/profile tables. No pointers are retained.
 * @param[in] h Finite depth in ft; values above y_full are clamped before geometry evaluation.
 * @return Area in ft2; h<=0 returns zero.
 */
inline double areaOfDepth(const XSectParams& xs, double h) {
    if (h <= 0.0)
        return 0.0;
    if (h > xs.y_full)
        h = xs.y_full;
    switch (static_cast<XSectShape>(xs.type)) {
    case XSectShape::CIRCULAR:
    case XSectShape::FORCE_MAIN:
        return detail::circArea(0.5 * xs.y_full, h);
    case XSectShape::FILLED_CIRCULAR: {
        const double r = 0.5 * (xs.y_full + xs.y_bot);
        return detail::circArea(r, xs.y_bot + h) - detail::circArea(r, xs.y_bot);
    }
    case XSectShape::RECT_CLOSED:
    case XSectShape::RECT_OPEN:
        return h * xs.w_max;
    case XSectShape::TRAPEZOIDAL:
        return (xs.y_bot + xs.s_bot * h) * h;
    case XSectShape::TRIANGULAR:
        return xs.s_bot * h * h;
    case XSectShape::PARABOLIC:
        return (4.0 / 3.0) * xs.r_bot * h * std::sqrt(h);
    case XSectShape::POWERFUNC:
        return xs.r_bot * std::pow(h, xs.s_bot + 1.0);
    default:
        return xsect::getAofY(xs, h);
    }
}

/**
 * @brief Evaluate per-barrel top width including the invert boundary value.
 *
 * Rectangles retain wall width at the crown and invert. Circular crown width is zero;
 * filled-circular invert width is the sediment chord. Unsupported analytic shapes use the legacy
 * evaluator. Slot extension and barrel factors are added by the mesh builder.
 *
 * @param[in] xs Borrowed initialized per-barrel native-ft geometry with positive y_full and valid
 *     shape coefficients/profile tables. No pointers are retained.
 * @param[in] h Finite depth in ft; values above y_full are clamped before geometry evaluation.
 * @return Width in ft; h<0 returns zero, but h==0 evaluates the shape width.
 */
inline double widthOfDepth(const XSectParams& xs, double h) {
    if (h < 0.0)
        return 0.0;
    if (h > xs.y_full)
        h = xs.y_full;
    switch (static_cast<XSectShape>(xs.type)) {
    case XSectShape::CIRCULAR:
    case XSectShape::FORCE_MAIN:
        return detail::circWidth(0.5 * xs.y_full, h);
    case XSectShape::FILLED_CIRCULAR:
        return detail::circWidth(0.5 * (xs.y_full + xs.y_bot), xs.y_bot + h);
    case XSectShape::RECT_CLOSED:
    case XSectShape::RECT_OPEN:
        return xs.w_max;
    case XSectShape::TRAPEZOIDAL:
        return xs.y_bot + 2.0 * xs.s_bot * h;
    case XSectShape::TRIANGULAR:
        return 2.0 * xs.s_bot * h;
    case XSectShape::PARABOLIC:
        return 2.0 * xs.r_bot * std::sqrt(h);
    case XSectShape::POWERFUNC:
        return (h > 0.0) ? (xs.s_bot + 1.0) * xs.r_bot * std::pow(h, xs.s_bot) : 0.0;
    default:
        return xsect::getWofY(xs, h);
    }
}

/**
 * @brief Evaluate per-barrel wetted perimeter for FV geometry sampling.
 *
 * Closed rectangles ramp the roof contribution over the final 3 percent of full area. Filled
 * circles include the flat sediment surface; open rectangles honor ignored-side count. POWERFUNC
 * uses adaptive side-arc integration plus a tiny-invert chord approximation. Other non-analytic
 * shapes use legacy A/R, returning zero if R<=0. No numerical status is reported.
 *
 * @param[in] xs Borrowed initialized per-barrel native-ft geometry with positive y_full and valid
 *     shape coefficients/profile tables. No pointers are retained.
 * @param[in] h Finite depth in ft; values above y_full are clamped before geometry evaluation.
 * @return Perimeter in ft; h<=0 returns zero.
 */
inline double perimeterOfDepth(const XSectParams& xs, double h) {
    if (h <= 0.0)
        return 0.0;
    if (h > xs.y_full)
        h = xs.y_full;
    switch (static_cast<XSectShape>(xs.type)) {
    case XSectShape::CIRCULAR:
    case XSectShape::FORCE_MAIN:
        return detail::circPerim(0.5 * xs.y_full, h);
    case XSectShape::FILLED_CIRCULAR: {
        const double r = 0.5 * (xs.y_full + xs.y_bot);
        // Arc above the sediment plus the flat sediment surface.
        return detail::circPerim(r, xs.y_bot + h) - detail::circPerim(r, xs.y_bot) +
               detail::circWidth(r, xs.y_bot);
    }
    case XSectShape::RECT_CLOSED: {
        double p = xs.w_max + 2.0 * h;
        const double alf = (xs.a_full > 0.0) ? (h * xs.w_max) / xs.a_full : 0.0;
        if (alf > kRectAlfMax)
            p += (alf - kRectAlfMax) / (1.0 - kRectAlfMax) * xs.w_max;
        return p;
    }
    case XSectShape::RECT_OPEN:
        return xs.w_max + (2.0 - xs.s_bot) * h;
    case XSectShape::TRAPEZOIDAL:
        return xs.y_bot + h * xs.r_bot;
    case XSectShape::TRIANGULAR:
        return 2.0 * h * xs.r_bot;
    case XSectShape::PARABOLIC: {
        const double x = 2.0 * std::sqrt(h) / xs.r_bot;
        const double t = std::sqrt(1.0 + x * x);
        return 0.5 * xs.r_bot * xs.r_bot * (x * t + std::log(x + t));
    }
    case XSectShape::POWERFUNC: {
        // Half-width x(y) = hc·y^m; arc length 2∫√(1 + x'(y)²) dy. The
        // integrand has an integrable singularity at y → 0 when m < 1;
        // adaptive Simpson from a tiny offset handles it at build time.
        const double hc = (xs.s_bot + 1.0) * xs.r_bot / 2.0;
        const double m = xs.s_bot;
        auto ds = [&](double y) {
            const double dxdy = (y > 0.0) ? hc * m * std::pow(y, m - 1.0) : 0.0;
            return std::sqrt(1.0 + dxdy * dxdy);
        };
        const double y0 = 1.0e-9 * h;
        return 2.0 *
               (detail::integrate(ds, y0, h, 1.0e-12 * h) + std::hypot(y0, hc * std::pow(y0, m)));
    }
    default: {
        const double r = xsect::getRofY(xs, h);
        return (r > 0.0) ? xsect::getAofY(xs, h) / r : 0.0;
    }
    }
}

/**
 * @brief Evaluate per-barrel hydraulic radius for the FV closure builder.
 *
 * Analytic shapes use area/perimeter from this header. Other shapes return the legacy hydraulic
 * radius directly, preserving composite transect conveyance rather than substituting a geometric
 * perimeter. No slot/barrel scaling is included.
 *
 * @param[in] xs Borrowed initialized per-barrel native-ft geometry with positive y_full and valid
 *     shape coefficients/profile tables. No pointers are retained.
 * @param[in] h Finite depth in ft; values above y_full are clamped before geometry evaluation.
 * @return Radius in ft; h<=0 or nonpositive analytic perimeter returns zero.
 */
inline double hydRadOfDepth(const XSectParams& xs, double h) {
    if (h <= 0.0)
        return 0.0;
    if (!hasClosedForm(xs.type))
        return xsect::getRofY(xs, std::min(h, xs.y_full));
    const double p = perimeterOfDepth(xs, h);
    return (p > 0.0) ? areaOfDepth(xs, h) / p : 0.0;
}

} // namespace openswmm::fv::secgeom

#endif // OPENSWMM_ENGINE_FV_SECTION_GEOMETRY_HPP
