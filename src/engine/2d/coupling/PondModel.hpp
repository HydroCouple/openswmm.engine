/**
 * @file PondModel.hpp
 * @brief Above-rim pond of a 2D-coupled junction: a C¹ surface-area ramp,
 *        its closed-form volume and inverse, and the head-equalisation cap.
 *
 * @details x is the water depth above the rim (any length unit; areas in the
 *          same unit squared). The pond area rises from 0 at the rim to A_eq
 *          over the band [0, δ] with the Hermite smoothstep and stays A_eq
 *          beyond, so dh/dt = ΣQ / A_s has no jump at the rim. The band lies
 *          entirely ABOVE the rim: the dynamic wave books a junction's volume
 *          below the rim as zero, so storage below it would vanish
 *          (plans/1D2D_FINE_SCALE_COUPLING_PLAN §0R R8). δ = 0 is the legacy
 *          step (A_eq from the first wet millimetre).
 *
 *            area(x)   = A·S(x/δ),                     S(t) = 3t² − 2t³
 *            volume(x) = A·δ·(t³ − t⁴/2), t = x/δ       (x ≤ δ)
 *                      = A·(x − δ/2)                    (x > δ)
 *
 *          Header-only and host/device callable, like ExchangeLaw.hpp.
 *
 * @ingroup engine_2d
 */

#ifndef OPENSWMM_ENGINE_2D_POND_MODEL_HPP
#define OPENSWMM_ENGINE_2D_POND_MODEL_HPP

#include <cmath>

#if defined(KOKKOS_INLINE_FUNCTION)
#define OPENSWMM_POND_HD KOKKOS_INLINE_FUNCTION
#else
#define OPENSWMM_POND_HD inline
#endif

namespace openswmm::twoD::pond {

/// Pond surface area at depth x above the rim.
OPENSWMM_POND_HD double area(double x, double a_eq, double band) {
    if (!(x > 0.0)) return 0.0;
    if (!(band > 0.0) || x >= band) return a_eq;
    const double t = x / band;
    return a_eq * t * t * (3.0 - 2.0 * t);
}

/// Pond volume held between the rim and depth x above it.
OPENSWMM_POND_HD double volume(double x, double a_eq, double band) {
    if (!(x > 0.0)) return 0.0;
    if (!(band > 0.0)) return a_eq * x;
    if (x >= band) return a_eq * (x - 0.5 * band);
    const double t = x / band;
    return a_eq * band * t * t * t * (1.0 - 0.5 * t);
}

/// Depth above the rim at which the pond holds v (inverse of volume()).
OPENSWMM_POND_HD double depthForVolume(double v, double a_eq, double band) {
    if (!(v > 0.0) || !(a_eq > 0.0)) return 0.0;
    if (!(band > 0.0)) return v / a_eq;
    const double full = 0.5 * a_eq * band;            // volume at x = band
    if (v >= full) return v / a_eq + 0.5 * band;
    // t³ − t⁴/2 = r on [0, 1], monotone: safeguarded Newton from t ≈ ∛r.
    const double r = v / (a_eq * band);
    double lo = 0.0, hi = 1.0, t = std::cbrt(r);
    if (t > 1.0) t = 1.0;
    for (int it = 0; it < 60; ++it) {
        const double f = t * t * t * (1.0 - 0.5 * t) - r;
        if (f > 0.0) hi = t; else lo = t;
        const double df = t * t * (3.0 - 2.0 * t);
        double tn = (df > 0.0) ? t - f / df : 0.5 * (lo + hi);
        if (!(tn > lo && tn < hi)) tn = 0.5 * (lo + hi);
        if (std::fabs(tn - t) <= 1e-15) { t = tn; break; }
        t = tn;
    }
    return t * band;
}

/// Head-equalisation cap on one exchange held for a window T over which the
/// 1D head is frozen: moving more than |Δh|·(1/A1 + 1/A2)⁻¹ in the window
/// would carry the heads past each other (the generalised Rubinato Eq. 10
/// limiter). Returns the largest admissible |Q|; +∞ when an area is unknown.
OPENSWMM_POND_HD double equalizeCap(double dh, double window, double a1, double a2) {
    if (!(window > 0.0) || !(a1 > 0.0) || !(a2 > 0.0)) return HUGE_VAL;
    return std::fabs(dh) / (window * (1.0 / a1 + 1.0 / a2));
}

} // namespace openswmm::twoD::pond

#undef OPENSWMM_POND_HD

#endif // OPENSWMM_ENGINE_2D_POND_MODEL_HPP
