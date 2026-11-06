/**
 * @file      PondModel.hpp
 * @brief     Above-rim pond of a 2D-coupled junction: a C¹ surface-area ramp,
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
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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

/**
 * @brief Pond surface area at depth x above the rim.
 *
 * @param[in] x Finite depth above the rim; nonpositive values return zero.
 * @param[in] a_eq Finite nonnegative full pond area, in consistent length units squared.
 * @param[in] band Finite ramp depth in the same length units; nonpositive selects the abrupt
 * legacy step.
 * @note Pure host/device arithmetic; no state or allocation. Units may be SI or project units
 * but must be consistent.
 * @return Pond surface area in squared length units.
 */
OPENSWMM_POND_HD double area(double x, double a_eq, double band) {
    if (!(x > 0.0))
        return 0.0;
    if (!(band > 0.0) || x >= band)
        return a_eq;
    const double t = x / band;
    return a_eq * t * t * (3.0 - 2.0 * t);
}

/**
 * @brief Pond volume held between the rim and depth x above it.
 *
 * @param[in] x Finite depth above the rim; nonpositive values return zero.
 * @param[in] a_eq Finite nonnegative full pond area, in consistent length units squared.
 * @param[in] band Finite ramp depth in the same length units; nonpositive selects the abrupt
 * legacy step.
 * @note Pure host/device arithmetic; no state or allocation. Units may be SI or project units
 * but must be consistent.
 * @return Stored above-rim volume in cubed length units.
 */
OPENSWMM_POND_HD double volume(double x, double a_eq, double band) {
    if (!(x > 0.0))
        return 0.0;
    if (!(band > 0.0))
        return a_eq * x;
    if (x >= band)
        return a_eq * (x - 0.5 * band);
    const double t = x / band;
    return a_eq * band * t * t * t * (1.0 - 0.5 * t);
}

/**
 * @brief Depth above the rim at which the pond holds v (inverse of volume()).
 *
 * @param[in] v Pond volume in consistent cubic length units; nonpositive volume or area returns
 * zero.
 * @param[in] a_eq Finite nonnegative full pond area, in consistent length units squared.
 * @param[in] band Finite ramp depth in the same length units; nonpositive selects the abrupt
 * legacy step.
 * @note Pure host/device arithmetic; no state or allocation. Units may be SI or project units
 * but must be consistent.
 * @return Depth above the rim. The ramp inverse uses at most 60 safeguarded Newton steps and
 * returns the last iterate without a separate convergence code.
 */
OPENSWMM_POND_HD double depthForVolume(double v, double a_eq, double band) {
    if (!(v > 0.0) || !(a_eq > 0.0))
        return 0.0;
    if (!(band > 0.0))
        return v / a_eq;
    const double full = 0.5 * a_eq * band; // volume at x = band
    if (v >= full)
        return v / a_eq + 0.5 * band;
    // t³ − t⁴/2 = r on [0, 1], monotone: safeguarded Newton from t ≈ ∛r.
    const double r = v / (a_eq * band);
    double lo = 0.0, hi = 1.0, t = std::cbrt(r);
    if (t > 1.0)
        t = 1.0;
    for (int it = 0; it < 60; ++it) {
        const double f = t * t * t * (1.0 - 0.5 * t) - r;
        if (f > 0.0)
            hi = t;
        else
            lo = t;
        const double df = t * t * (3.0 - 2.0 * t);
        double tn = (df > 0.0) ? t - f / df : 0.5 * (lo + hi);
        if (!(tn > lo && tn < hi))
            tn = 0.5 * (lo + hi);
        if (std::fabs(tn - t) <= 1e-15) {
            t = tn;
            break;
        }
        t = tn;
    }
    return t * band;
}

/**
 * @brief Head-equalisation cap on one exchange (the generalised Rubinato Eq. 10 limiter). The 1D
 * head is frozen for the coupling window T while the 2D cell is re-evaluated every substep
 * against its live head, so the only head an exchange can carry past the other within the window
 * is the 1D node's: |Q| ≤ |Δh|·A1/T. (The first form also charged the coupling cell for the
 * whole window, |Δh|/(T(1/A1 + 1/A2)); that scaled the cap with the cell's area and choked the
 * exchange wherever cells are smaller than the structure, while the drain/spill churn it removes
 * is the 1D side's.) Returns the largest admissible |Q|; +∞ when the area or window is unknown.
 *
 * @param[in] dh Finite surface-minus-node head difference in consistent length units; sign is
 * ignored.
 * @param[in] window Coupling window in seconds; nonpositive means unavailable.
 * @param[in] a1 Finite effective 1D node area in squared length units; nonpositive means
 * unavailable.
 * @return Maximum admissible magnitude in cubic length units per second, or positive infinity
 * when window/area is unavailable.
 * @note Pure per-exchange cap. No mutation or allocation; the surface is reevaluated each
 * substep and only the frozen 1D node area enters this cap.
 */
OPENSWMM_POND_HD double equalizeCap(double dh, double window, double a1) {
    if (!(window > 0.0) || !(a1 > 0.0))
        return HUGE_VAL;
    return std::fabs(dh) * a1 / window;
}

} // namespace openswmm::twoD::pond

#undef OPENSWMM_POND_HD

#endif // OPENSWMM_ENGINE_2D_POND_MODEL_HPP
