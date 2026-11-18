// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      ImplicitWetKernels.hpp
 * @brief     Wet-state closure kernels used by implicit finite-volume experiments.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "FvKernels.hpp"
#include <array>

namespace openswmm::fv::implicit_wet {
// Native forward directional arithmetic. Five local variables at most: two
// cell (h,Q) pairs and one node head. No global seeds, allocation or inner solve.
struct Jet {
    double v = 0.0;
    std::array<double, 5> d{};
    /**
     * @brief Construct a zero primal value with all five directional derivatives zero.
     */
    Jet() = default;
    /**
     * @brief Lift a scalar constant into five-direction forward arithmetic.
     *
     * All derivative entries are zero. No allocation or retained references.
     *
     * @param[in] value Primal scalar, in the units of the represented variable; copied without
     *     validation.
     */
    Jet(double value) : v(value) {}
    /**
     * @brief Create one local independent variable for forward differentiation.
     *
     * @param[in] value Primal scalar value.
     * @param[in] column Derivative seed index in [0,4], unchecked.
     * @return Jet with primal value, derivative one in column and zeros in every other direction.
     */
    static Jet seed(double value, int column) {
        Jet x(value);
        x.d[column] = 1.0;
        return x;
    }
};
/**
 * @brief Add two forward-mode values.
 *
 * No validation, mutation or heap allocation.
 *
 * @param[in] a Left primal value and five derivatives in the current local variable basis.
 * @param[in] b Right primal value and derivatives using the same basis.
 * @return Primal sum and componentwise derivative sum.
 */
inline Jet operator+(const Jet& a, const Jet& b) {
    Jet r(a.v + b.v);
    for (int i = 0; i < 5; ++i)
        r.d[i] = a.d[i] + b.d[i];
    return r;
}
/**
 * @brief Subtract two forward-mode values.
 *
 * No validation, mutation or heap allocation.
 *
 * @param[in] a Left primal value and five derivatives in the current local variable basis.
 * @param[in] b Right primal value and derivatives using the same basis.
 * @return Primal difference and componentwise derivative difference.
 */
inline Jet operator-(const Jet& a, const Jet& b) {
    Jet r(a.v - b.v);
    for (int i = 0; i < 5; ++i)
        r.d[i] = a.d[i] - b.d[i];
    return r;
}
/**
 * @brief Multiply two forward-mode values using the product rule.
 *
 * Units multiply; overflow/nonfinite values propagate.
 *
 * @param[in] a Left primal value and five derivatives in the current local variable basis.
 * @param[in] b Right primal value and derivatives using the same basis.
 * @return Primal product with derivative a.d*b.v+a.v*b.d.
 */
inline Jet operator*(const Jet& a, const Jet& b) {
    Jet r(a.v * b.v);
    for (int i = 0; i < 5; ++i)
        r.d[i] = a.d[i] * b.v + a.v * b.d[i];
    return r;
}
/**
 * @brief Divide two forward-mode values using the quotient rule.
 *
 * Requires finite nonzero b.v for finite arithmetic; zero/overflow is not diagnosed.
 *
 * @param[in] a Left primal value and five derivatives in the current local variable basis.
 * @param[in] b Right primal value and derivatives using the same basis.
 * @return Primal a.v/b.v with derivative (a.d-result.v*b.d)/b.v.
 */
inline Jet operator/(const Jet& a, const Jet& b) {
    Jet r(a.v / b.v);
    for (int i = 0; i < 5; ++i)
        r.d[i] = (a.d[i] - r.v * b.d[i]) / b.v;
    return r;
}
/**
 * @brief Lift a known scalar function value and slope through an input jet.
 *
 * @param[in] value Primal f(x.v), supplied by the caller.
 * @param[in] slope Scalar derivative df/dx at x.v, in output/input units.
 * @param[in] x Input jet supplying five directional derivatives.
 * @return Jet with primal value and derivatives slope*x.d; no scalar function is evaluated here.
 */
inline Jet mapped(double value, double slope, const Jet& x) {
    Jet r(value);
    for (int i = 0; i < 5; ++i)
        r.d[i] = slope * x.d[i];
    return r;
}
/**
 * @brief Select the local minimum branch and preserve that branch derivative.
 *
 * No generalized derivative across the branch boundary is formed; NaN comparisons follow ordinary
 * floating-point rules.
 *
 * @param[in] a Left primal value and five derivatives in the current local variable basis.
 * @param[in] b Right primal value and derivatives using the same basis.
 * @return A copy of b if b.v<a.v, otherwise a; ties select the first operand.
 */
inline Jet minimum(const Jet& a, const Jet& b) {
    return b.v < a.v ? b : a;
}
/**
 * @brief Select the local maximum branch and preserve that branch derivative.
 *
 * Matches std::max first-operand tie behavior; does not freeze the selected numerical derivative.
 *
 * @param[in] a Left primal value and five derivatives in the current local variable basis.
 * @param[in] b Right primal value and derivatives using the same basis.
 * @return A copy of b if a.v<b.v, otherwise a; ties select the first operand.
 */
inline Jet maximum(const Jet& a, const Jet& b) {
    return a.v < b.v ? b : a;
}
/**
 * @brief Check a primal value and every directional derivative for finite arithmetic.
 *
 * @param[in] x Jet to inspect; no fields are modified.
 * @return True only if x.v and all five entries of x.d are finite.
 */
inline bool finite(const Jet& x) {
    return std::isfinite(x.v) &&
           std::all_of(x.d.begin(), x.d.end(), [](double d) { return std::isfinite(d); });
}
struct Closure {
    Jet area, width, moment;
};
/**
 * @brief Differentiate wet closure area, width and pressure integral with respect to depth.
 *
 * Uses the shared closureEval primal, maps dA/dh=width and dI1/dh=area, and derives dwidth/dh from
 * the active polynomial/cubic panel (zero above crown). At panel/branch boundaries this is the
 * selected one-sided derivative. Dry width clamping is not differentiated as an independent
 * nonsmooth operator; callers restrict admissible wet states. No allocation or mutation.
 *
 * @param[in] c Initialized immutable monotone native closure, with positive panel spacing and valid
 *     samples.
 * @param[in] h Strictly positive finite wet depth jet in ft; dry branches are outside this
 *     differentiation contract.
 * @return Area ft2, width ft and moment ft3 jets, including all five directional derivatives.
 */
inline Closure closure(const FvClosure& c, const Jet& h) {
    const auto e = kernels::closureEval(c, h.v);
    double second = 0.0;
    if (h.v < c.y_full) {
        if (c.kind == kClosurePolynomial)
            second = 2.0 * c.c2;
        else {
            int i;
            double t;
            kernels::closureLocate(c, h.v, i, t);
            second = ((12 * t - 6) * c.A[i] + (6 * t - 4) * c.M[i] * c.dh +
                      (-12 * t + 6) * c.A[i + 1] + (6 * t - 2) * c.M[i + 1] * c.dh) *
                     c.inv_dh * c.inv_dh;
        }
    }
    return {mapped(e.a, e.t, h), mapped(e.t, second, h), mapped(e.i1, e.a, h)};
}
/**
 * @brief Evaluate the active wet friction-radius derivative with respect to depth.
 *
 * Does not apply a dry floor, denominator safeguard or nonfinite validation. Table-panel ties use
 * the panel selected by closureLocate.
 *
 * @param[in] c Initialized immutable native closure; polynomial perimeter must be nonzero.
 * @param[in] h Positive finite depth in ft; valid table-locate range is required below crown.
 * @return dR/dh, dimensionless, from the polynomial quotient or active linear R panel; zero
 *     at/above crown.
 */
inline double radiusSlope(const FvClosure& c, double h) {
    if (h >= c.y_full)
        return 0.0;
    if (c.kind == kClosurePolynomial) {
        const auto e = kernels::closureEval(c, h);
        const double p = c.p0 + c.p1 * h;
        return (e.t * p - e.a * c.p1) / (p * p);
    }
    int i;
    double t;
    kernels::closureLocate(c, h, i, t);
    return (c.R[i + 1] - c.R[i]) * c.inv_dh;
}
struct State {
    Jet a, q, u, c, i1;
    uint8_t press = 0;
};
/**
 * @brief Construct a differentiated wet face state from depth and velocity.
 *
 * Celerity derivatives divide by both wet area and width. A dry/zero-width state can therefore
 * produce nonfinite derivatives even when the shared scalar celerity returns zero; callers must
 * reject such states before using this wet-only derivative path. Does not select a TPA closure from
 * press; the supplied table closure is always evaluated.
 *
 * @param[in] table Initialized immutable closure with positive wet area/top width.
 * @param[in] h Strictly wet depth jet in ft; scalar area must exceed kDryArea.
 * @param[in] velocity Velocity jet in ft/s using the same local derivative basis.
 * @param[in] press Regime flag copied into the result: 0 free, 1 pressure, 2 excluded node ghost.
 * @return Jets for aggregate area ft2, discharge ft3/s, velocity/celerity ft/s and moment ft3.
 */
inline State state(const FvClosure& table, const Jet& h, const Jet& velocity, uint8_t press) {
    const auto e = closure(table, h);
    const double speed = kernels::celerity(e.area.v, e.width.v);
    Jet c(speed);
    for (int i = 0; i < 5; ++i)
        c.d[i] = .5 * speed * (e.area.d[i] / e.area.v - e.width.d[i] / e.width.v);
    return {e.area, e.area * velocity, velocity, c, e.moment, press};
}
struct Flux {
    Jet mass, momentum;
};
// Differentiate exactly the wet branch of kernels::waveSpeeds/riemannFlux.
// Branches are selected at the current state, including RH min/max bounds;
// their numerical values (celerity, jump speed, pressure) are NOT frozen.
// At ties use the same first-operand choice as std::min/std::max. Residual
// evaluation still calls the shared kernels, and tests enforce primal parity.
/**
 * @brief Differentiate the active wet HLL hydraulic flux and pressure-bore bounds.
 *
 * Selects the same primal branches as wet waveSpeeds/riemannFlux, including rankine-hugoniot
 * min/max pressure bounds and first-operand ties. Supersonic branches use physical flux; a
 * straddling fan requires nonzero sr-sl. Branch derivatives include changing wave speeds rather
 * than frozen coefficients. Dry-state rarefaction branches, gates, positivity factors and
 * hydrostatic side corrections are not included; this routine is valid only on an admissible wet
 * branch.
 *
 * @param[in] l Finite strictly wet left differentiated face state in native aggregate units.
 * @param[in] r Finite strictly wet right state in the same geometry/orientation and derivative
 *     basis.
 * @return Volume flux jet in ft3/s and momentum flux jet in ft4/s2.
 */
inline Flux flux(const State& l, const State& r) {
    auto sl = minimum(l.u - l.c, r.u - r.c), sr = maximum(l.u + l.c, r.u + r.c);
    if (l.press != r.press && l.press < 2 && r.press < 2) {
        if (l.press) {
            const auto da = l.a - r.a;
            const auto w = da.v > 0.0 ? (l.q - r.q) / da : sr;
            sr = maximum(r.u + r.c, minimum(sr, w));
        } else {
            const auto da = r.a - l.a;
            const auto w = da.v > 0.0 ? (r.q - l.q) / da : sl;
            sl = minimum(l.u - l.c, maximum(sl, w));
        }
    }
    const auto fl = l.q * l.u + kernels::kGravity * l.i1, fr = r.q * r.u + kernels::kGravity * r.i1;
    if (sl.v >= 0.0)
        return {l.q, fl};
    if (sr.v <= 0.0)
        return {r.q, fr};
    return {(sr * l.q - sl * r.q + sl * sr * (r.a - l.a)) / (sr - sl),
            (sr * fl - sl * fr + sl * sr * (r.q - l.q)) / (sr - sl)};
}
} // namespace openswmm::fv::implicit_wet
