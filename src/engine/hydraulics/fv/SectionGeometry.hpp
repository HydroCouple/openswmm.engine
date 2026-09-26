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
 * @file SectionGeometry.hpp
 * @brief Exact cross-section geometry — area, top width and wetted perimeter
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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

/// Circular segment of radius r at water depth y (from the invert).
inline double circTheta(double r, double y) {
    double c = 1.0 - y / r;
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return 2.0 * std::acos(c);
}
inline double circArea(double r, double y) {
    if (y <= 0.0) return 0.0;
    if (y >= 2.0 * r) return kPi * r * r;
    const double th = circTheta(r, y);
    return 0.5 * r * r * (th - std::sin(th));
}
inline double circWidth(double r, double y) {
    if (y <= 0.0 || y >= 2.0 * r) return 0.0;
    const double s = y * (2.0 * r - y);
    return (s > 0.0) ? 2.0 * std::sqrt(s) : 0.0;
}
inline double circPerim(double r, double y) {
    if (y <= 0.0) return 0.0;
    if (y >= 2.0 * r) return 2.0 * kPi * r;
    return r * circTheta(r, y);
}

/// Adaptive Simpson quadrature — build-time helper for the perimeters that
/// have no closed form (ellipse arc, power-function arc).
template <class F>
double simpsonStep(F& f, double a, double b, double fa, double fm, double fb,
                   double whole, double eps, int depth) {
    const double m = 0.5 * (a + b);
    const double lm = 0.5 * (a + m), rm = 0.5 * (m + b);
    const double flm = f(lm), frm = f(rm);
    const double left  = (m - a) / 6.0 * (fa + 4.0 * flm + fm);
    const double right = (b - m) / 6.0 * (fm + 4.0 * frm + fb);
    const double delta = left + right - whole;
    if (depth <= 0 || std::fabs(delta) <= 15.0 * eps)
        return left + right + delta / 15.0;
    return simpsonStep(f, a, m, fa, flm, fm, left, 0.5 * eps, depth - 1) +
           simpsonStep(f, m, b, fm, frm, fb, right, 0.5 * eps, depth - 1);
}
template <class F>
double integrate(F f, double a, double b, double eps = 1.0e-12, int depth = 40) {
    if (!(b > a)) return 0.0;
    const double fa = f(a), fb = f(b), fm = f(0.5 * (a + b));
    const double whole = (b - a) / 6.0 * (fa + 4.0 * fm + fb);
    return simpsonStep(f, a, b, fa, fm, fb, whole, eps, depth);
}

/// Wetted perimeter of an ellipse with horizontal semi-axis ax and vertical
/// semi-axis by, water depth y from the bottom: parametrize the boundary as
/// x = ax·sin φ, y = by·(1 − cos φ) and integrate the arc length.
inline double ellipsePerim(double ax, double by, double y) {
    if (y <= 0.0) return 0.0;
    double c = 1.0 - y / by;
    if (c < -1.0) c = -1.0;
    if (c > 1.0) c = 1.0;
    const double phi_max = std::acos(c);
    auto ds = [&](double phi) {
        const double cx = ax * std::cos(phi), sy = by * std::sin(phi);
        return std::sqrt(cx * cx + sy * sy);
    };
    return 2.0 * integrate(ds, 0.0, phi_max, 1.0e-12 * (ax + by));
}

} // namespace detail

/// True for the shapes this header evaluates in closed form.
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

/// True for the open sections whose A(h) is a polynomial of degree ≤ 2 and
/// whose perimeter is linear in h — the closure's polynomial class, evaluated
/// in closed form in the hot loop rather than tabulated (plan §2a″).
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

/// Flow area at depth h ∈ [0, y_full] (per barrel).
inline double areaOfDepth(const XSectParams& xs, double h) {
    if (h <= 0.0) return 0.0;
    if (h > xs.y_full) h = xs.y_full;
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

/// Top width at depth h ∈ [0, y_full] (per barrel). Evaluated AT h = 0 too
/// (a rectangle is w wide at its invert; a circle 0) — the closure takes it
/// as the first node's slope, and a spurious 0 there put a w·dh²/12 offset
/// into every I₁ of a flat-bottomed section. At the crown of a closed
/// rectangle this is the wall width, not the legacy table's 0 — the crown is a
/// line, and the closure's slot term is what the solver adds above it.
inline double widthOfDepth(const XSectParams& xs, double h) {
    if (h < 0.0) return 0.0;
    if (h > xs.y_full) h = xs.y_full;
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

/// Wetted perimeter at depth h ∈ [0, y_full] (per barrel). For shapes without
/// a closed form it is A/R of the legacy evaluator (R = 0 ⇒ 0).
inline double perimeterOfDepth(const XSectParams& xs, double h) {
    if (h <= 0.0) return 0.0;
    if (h > xs.y_full) h = xs.y_full;
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
            const double m  = xs.s_bot;
            auto ds = [&](double y) {
                const double dxdy = (y > 0.0) ? hc * m * std::pow(y, m - 1.0) : 0.0;
                return std::sqrt(1.0 + dxdy * dxdy);
            };
            const double y0 = 1.0e-9 * h;
            return 2.0 * (detail::integrate(ds, y0, h, 1.0e-12 * h) +
                          std::hypot(y0, hc * std::pow(y0, m)));
        }
        default: {
            const double r = xsect::getRofY(xs, h);
            return (r > 0.0) ? xsect::getAofY(xs, h) / r : 0.0;
        }
    }
}

/// Hydraulic radius A/P at depth h ∈ [0, y_full] (per barrel).
inline double hydRadOfDepth(const XSectParams& xs, double h) {
    if (h <= 0.0) return 0.0;
    if (!hasClosedForm(xs.type)) return xsect::getRofY(xs, std::min(h, xs.y_full));
    const double p = perimeterOfDepth(xs, h);
    return (p > 0.0) ? areaOfDepth(xs, h) / p : 0.0;
}

} // namespace openswmm::fv::secgeom

#endif // OPENSWMM_ENGINE_FV_SECTION_GEOMETRY_HPP
