/**
 * @file      ExchangeLaw.hpp
 * @brief     Regime-aware 1D↔2D exchange law (COUPLING_LAW WEIR_ORIFICE):
 *        free / submerged weir and orifice, surface→sewer and sewer→surface.
 *
 * @details Pure functions of the two heads and a point's geometry and
 *          coefficients — no mesh, no SimulationContext — so the CPU marcher,
 *          the Kokkos solver and a HydroCouple component evaluate one law.
 *          Header-only and host/device callable (KOKKOS_INLINE_FUNCTION when
 *          Kokkos is included first).
 *
 *          SI throughout; heads and the crest in the 2D metre frame.
 *          Sign: Q > 0 drains 2D→1D, Q < 0 spills 1D→2D.
 *
 *          h_s = max(0, h2 − z) is the surface depth over the crest z.
 *
 *          Surface → sewer (h2 > h1, h_s > 0):
 *            free weir        Qf = ⅔ C_w  L_w √(2g) h_s φ(h_s)          (h1 ≤ z)
 *            submerged weir   Qs =   C_sw L_w √(2g) h_s φ(h2 − h1)      (h1 > z)
 *            orifice          free Of = C_oi A_o √(2g) φ(h_s), submerged
 *                             Os = C_oi A_o √(2g) φ(h2 − h1)
 *          The free and submerged forms blend over the submergence band
 *          σ = (h1 − z)/h_s ∈ [0, σ_band] with the Hermite smoothstep, so the
 *          law is C¹ across h1 = z whatever C_sw is (Rubinato et al. 2017
 *          give C_sw per regime; with C_sw = ⅔ C_w the two weir forms already
 *          agree at h1 = z). The weir and the orifice then combine through
 *          the p-norm minimum (the limiting control governs):
 *            Q = (Qw^−p + Qo^−p)^(−1/p),  p = 16  (≤ min, 4.3 % under at Qw = Qo)
 *
 *          Sewer → surface (h1 > h2, h1 > z), orifice:
 *            Q = −C_oo A_o √(2g) [(1 − S) φ(h1 − z) + S φ(h1 − h2)],
 *          S the smoothstep of σ_out = (h2 − z)/(h1 − z) over [0, σ_band]:
 *          a dry surface (h2 ≤ z) discharges against the crest, a wet one
 *          against its own head, C¹ across h2 = z.
 *
 *          φ is the C¹-regularised square root (φ(x) = √x for x ≥ ε, a
 *          quadratic with finite slope below) that bounds the sensitivity as
 *          the heads meet. ε is a parameter: 0.02 m at field scale, scaled
 *          with the structure for a laboratory rig.
 *
 * @see plans/1D2D_FINE_SCALE_COUPLING_PLAN_2026-10-19.md §3 and §0R (R1–R6)
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_EXCHANGE_LAW_HPP
#define OPENSWMM_ENGINE_2D_EXCHANGE_LAW_HPP

#include <cmath>

#if defined(KOKKOS_INLINE_FUNCTION)
#define OPENSWMM_EXCHANGE_HD KOKKOS_INLINE_FUNCTION
#else
#define OPENSWMM_EXCHANGE_HD inline
#endif

namespace openswmm::twoD::exchange {

constexpr double kGravity = 9.80665; ///< m/s²
constexpr double kSigmaBand = 0.1;   ///< submergence blend width

/// A coupling point's structure, SI.
struct Geometry {
    double z_crest = 0.0; ///< crest / rim elevation (m, 2D frame)
    double l_weir = 0.0;  ///< weir crest length (m)
    double a_orif = 0.0;  ///< orifice / open area (m²)
};

/// Discharge coefficients and the φ regularisation head.
struct Coefficients {
    double c_w = 0.540;  ///< free weir
    double c_sw = 0.360; ///< submerged weir
    double c_oi = 0.167; ///< orifice, surface → sewer
    double c_oo = 0.160; ///< orifice, sewer → surface
    double h_eps = 0.02; ///< φ regularisation head (m)
};

/**
 * @brief C¹-regularised √x: √x for x ≥ ε, (3/(2√ε))x − x²/(2ε^{3/2}) below.
 *
 * @param[in] x Finite head difference in m; nonpositive values return zero.
 * @param[in] eps Finite strictly positive regularization head, m.
 * @return Regularized square root in sqrt(m).
 * @note Pure host/device scalar evaluation; invalid eps is not checked.
 */
OPENSWMM_EXCHANGE_HD double phi(double x, double eps) {
    if (!(x > 0.0))
        return 0.0;
    if (x >= eps)
        return std::sqrt(x);
    const double inv_sqrt_e = 1.0 / std::sqrt(eps);
    return (1.5 * inv_sqrt_e) * x - (0.5 * inv_sqrt_e / eps) * x * x;
}

/**
 * @brief dφ/dx (finite at 0).
 *
 * @param[in] x Finite head difference in m; nonpositive values use the positive-side
 * regularization derivative at zero.
 * @param[in] eps Finite strictly positive regularization head, m.
 * @return Positive-side derivative in 1/sqrt(m); for x <= 0 this is not the derivative of the
 * clamped negative branch.
 * @note Pure host/device scalar evaluation; invalid eps is not checked.
 */
OPENSWMM_EXCHANGE_HD double dphi(double x, double eps) {
    if (!(x > 0.0))
        return 1.5 / std::sqrt(eps);
    if (x >= eps)
        return 0.5 / std::sqrt(x);
    const double inv_sqrt_e = 1.0 / std::sqrt(eps);
    return 1.5 * inv_sqrt_e - (inv_sqrt_e / eps) * x;
}

/**
 * @brief Hermite smoothstep of t clamped to [0, 1].
 *
 * @param[in] t Finite dimensionless blend coordinate; clamped to [0,1].
 * @return Dimensionless Hermite blend in [0,1], with zero slope at both endpoints.
 * @note Pure host/device calculation with no retained state.
 */
OPENSWMM_EXCHANGE_HD double smoothstep(double t) {
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    return t * t * (3.0 - 2.0 * t);
}

/**
 * @brief p-norm minimum, p = 16: (a^−16 + b^−16)^(−1/16) for a, b > 0, else 0. Evaluated as min
 * / (1 + r^16)^(1/16), r = min/max ∈ (0, 1], with four square roots — no overflow, no pow.
 * Weights ∂Q/∂a, ∂Q/∂b go to wa, wb.
 *
 * @param[in] a Finite nonnegative first candidate flow, m3/s.
 * @param[in] b Finite nonnegative second candidate flow, m3/s.
 * @param[out] wa Dimensionless sensitivity to a; set to zero if either candidate is nonpositive.
 * @param[out] wb Dimensionless sensitivity to b; set to zero if either candidate is nonpositive.
 * @return Smoothed limiting discharge in m3/s, no greater than min(a,b); zero if either
 * candidate is nonpositive.
 * @pre wa and wb refer to separate output scalars. No pointer/state ownership or allocation.
 */
OPENSWMM_EXCHANGE_HD double softmin(double a, double b, double& wa, double& wb) {
    wa = wb = 0.0;
    if (!(a > 0.0) || !(b > 0.0))
        return 0.0;
    const bool a_min = a <= b;
    const double lo = a_min ? a : b;
    const double r = lo / (a_min ? b : a);
    double r16 = r * r;
    r16 *= r16;
    r16 *= r16;
    r16 *= r16;
    const double root = std::sqrt(std::sqrt(std::sqrt(std::sqrt(1.0 + r16))));
    const double s = 1.0 / root; // Q / min
    const double q = lo * s;
    // ∂Q/∂x = (Q/x)^17 for each argument x.
    double s17 = s * s;
    s17 *= s17;
    s17 *= s17;
    s17 *= s17;
    s17 *= s;
    const double rs = r * s;
    double rs17 = rs * rs;
    rs17 *= rs17;
    rs17 *= rs17;
    rs17 *= rs17;
    rs17 *= rs;
    if (a_min) {
        wa = s17;
        wb = rs17;
    } else {
        wb = s17;
        wa = rs17;
    }
    return q;
}

/// Exchange Q (m³/s) and its head sensitivity G = −∂Q/∂h1 ≥ 0 (blend-weight
/// derivatives dropped, as for the gate and wet ramps: G is a pure damping
/// term on the 1D node continuity denominator and must never go negative).
struct Result {
    double q = 0.0;
    double g = 0.0;
};

/**
 * @brief Evaluate the bidirectional regularized weir/orifice exchange and damping.
 * @param[in] geo Borrowed validated geometry: crest m, weir length m, orifice area m2.
 * @param[in] c Borrowed nonnegative discharge coefficients and strictly positive h_eps in m.
 * @param[in] h2 Finite surface water head, m in the crest datum.
 * @param[in] h1 Finite network-node water head, m in the same datum.
 * @return Value-owned Result: q in m3/s, positive surface-to-network and negative
 * network-to-surface; g in m2/s is nonnegative damping, with blend-weight derivatives
 * deliberately omitted. Equal or below-crest inactive heads return zero.
 * @note Inputs are not validated. Pure host/device calculation; no availability limiter, ledger
 * mutation, or retained references.
 */
OPENSWMM_EXCHANGE_HD Result exchange(const Geometry& geo, const Coefficients& c, double h2,
                                     double h1) {
    Result r;
    const double s2g = std::sqrt(2.0 * kGravity);
    const double z = geo.z_crest;
    const double eps = c.h_eps;

    if (h2 > h1) {
        const double hs = h2 - z;
        if (!(hs > 0.0))
            return r;
        const double dh = h2 - h1;
        const double S = (h1 > z) ? smoothstep((h1 - z) / (kSigmaBand * hs)) : 0.0;

        const double weir_free = (2.0 / 3.0) * c.c_w * geo.l_weir * s2g * hs * phi(hs, eps);
        const double weir_sub = c.c_sw * geo.l_weir * s2g * hs * phi(dh, eps);
        const double orif_free = c.c_oi * geo.a_orif * s2g * phi(hs, eps);
        const double orif_sub = c.c_oi * geo.a_orif * s2g * phi(dh, eps);
        const double qw = (1.0 - S) * weir_free + S * weir_sub;
        const double qo = (1.0 - S) * orif_free + S * orif_sub;

        double ww = 0.0, wo = 0.0;
        r.q = softmin(qw, qo, ww, wo);
        // Only the submerged forms depend on h1.
        const double dqw = S * c.c_sw * geo.l_weir * s2g * hs * dphi(dh, eps);
        const double dqo = S * c.c_oi * geo.a_orif * s2g * dphi(dh, eps);
        r.g = ww * dqw + wo * dqo;
        return r;
    }
    if (h1 > h2 && h1 > z) {
        const double head_dry = h1 - z;
        const double S = (h2 > z) ? smoothstep((h2 - z) / (kSigmaBand * head_dry)) : 0.0;
        const double k = c.c_oo * geo.a_orif * s2g;
        r.q = -k * ((1.0 - S) * phi(head_dry, eps) + S * phi(h1 - h2, eps));
        r.g = k * ((1.0 - S) * dphi(head_dry, eps) + S * dphi(h1 - h2, eps));
        return r;
    }
    return r;
}

} // namespace openswmm::twoD::exchange

#undef OPENSWMM_EXCHANGE_HD

#endif // OPENSWMM_ENGINE_2D_EXCHANGE_LAW_HPP
