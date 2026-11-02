/**
 * @file RomStorePorts.hpp
 * @brief Monotone two-port flow laws for the per-member stores (PORT_HAMILTONIAN phase 5).
 *
 * docs/uncertainty/PH_STORES_AND_PORTS.md §1.2–§2.3. Every law here is a
 * function of absolute levels (or of a level offset from the main run) with
 *
 *   (P1) dQ/dh_up >= 0, dQ/dh_dn <= 0      (monotone)
 *   (P2) what leaves one end enters the other (the caller books it once)
 *   (P3) the member's extra flow is anchored on the main run: dQ = 0 at z = 0
 *
 * Under (P1)+(P2) the per-member backward-Euler step is an M-function: unique,
 * order-preserving and non-expansive in the l1 norm of volume differences
 * (Crandall–Tartar). It is NOT stable in the quadratic energy ½ zᵀ S z: a
 * one-way port dQ = d·z_up (weir, free outlet, supercritical pipe, pump)
 * between two stores has sym(L) = d·[[1, −½], [−½, 0]] with eigenvalue
 * d(1 − √2)/2 < 0 (note §2.2). Do not test the quadratic energy here.
 *
 * Regularisations (note §2.4, §1.2): the square root of a friction law is
 * linear below δ0 = 0.03 ft (finite slope for a nearly level pair); an
 * outlet that starts at a level (a node falling dry, a raised pipe end) is
 * ramped in over δ_dry = 0.01 ft instead of jumping. Both keep (P1)+(P2).
 *
 * Pure: no engine types.
 */
#ifndef OPENSWMM_ENGINE_UNCERTAINTY_ROM_STORE_PORTS_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_ROM_STORE_PORTS_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace openswmm::uncertainty::ports {

constexpr double kDelta0 = 0.03;    ///< ft: below this head the friction square root is linear
constexpr double kDeltaDry = 0.01;  ///< ft: an outlet that starts at a level ramps in over this

/// Regularised square root: sqrt(x) for x >= d0, x/sqrt(d0) for 0 < x < d0, 0 below.
/// Continuous, nondecreasing, slope bounded by 1/sqrt(d0).
inline double sqrtReg(double x, double d0 = kDelta0) {
    if (!(x > 0.0)) return 0.0;
    return x >= d0 ? std::sqrt(x) : x / std::sqrt(d0);
}

/// clamp(x / w, 0, 1).
inline double ramp(double x, double w = kDeltaDry) {
    if (!(x > 0.0)) return 0.0;
    return x >= w ? 1.0 : x / w;
}

/// Monotone envelope in place: v[k] = max(v[0..k]) (abscissae ascending).
/// Returns how many points were raised (0 for an already nondecreasing table).
/// A flow brake with a kickback dip is a negative resistance (multiple roots,
/// chatter); its envelope is the branch that drains at least as much (note §2.3).
inline int monotoneEnvelope(std::vector<double>& v) {
    int raised = 0;
    for (std::size_t k = 1; k < v.size(); ++k)
        if (v[k] < v[k - 1]) { v[k] = v[k - 1]; ++raised; }
    return raised;
}

/// A rating sampled at ascending level offsets y (ft) from the main run.
/// Linear in between; flat below the first point; above the last point it
/// continues with the last segment's slope (>= 0 after the envelope).
struct RatingTable {
    std::vector<double> y, q;
    int raised = 0;   ///< points the envelope raised

    void makeMonotone() { raised = monotoneEnvelope(q); }

    double eval(double yy) const {
        const std::size_t n = y.size();
        if (n == 0) return 0.0;
        if (n == 1 || yy <= y[0]) return q[0];
        if (yy >= y[n - 1]) {
            const double s = (y[n - 1] > y[n - 2]) ? (q[n - 1] - q[n - 2]) / (y[n - 1] - y[n - 2]) : 0.0;
            return q[n - 1] + std::max(s, 0.0) * (yy - y[n - 1]);
        }
        const auto it = std::upper_bound(y.begin(), y.end(), yy);
        const std::size_t k = static_cast<std::size_t>(it - y.begin()) - 1;
        const double f = (yy - y[k]) / (y[k + 1] - y[k]);
        // Clamped to the bracketing values so the interpolant is exactly
        // monotone in floating point (a flat segment stays flat).
        const double lo = std::min(q[k], q[k + 1]), hi = std::max(q[k], q[k + 1]);
        return std::clamp(q[k] + f * (q[k + 1] - q[k]), lo, hi);
    }
};

/// Clustered offsets y = Y·t|t|, t uniform in [−1, 1], n odd (the middle point is exactly 0):
/// a rating curves most near the main run's level, where members start and stop spilling.
inline std::vector<double> clusteredOffsets(double Y, int n = 17) {
    std::vector<double> y(static_cast<std::size_t>(n));
    const double dt = 2.0 / (n - 1);
    for (int m = 0; m < n; ++m) {
        const double t = -1.0 + m * dt;
        y[static_cast<std::size_t>(m)] = (m == (n - 1) / 2) ? 0.0 : Y * t * std::fabs(t);
    }
    return y;
}

/// Member flow (absolute, >= 0) through a one-way rated outlet (structure,
/// depth-rated pump, dry pipe): T_y = the monotone rating at the member's
/// offset y (times the dry ramp at the member's own depth, applied after the
/// table lookup: interpolating across the dry point would pass flow below the
/// invert), T_0 = the same at the main run, q_star = the main run's actual
/// flow, d_star = the main run's depth over the outlet's reference (ft).
///
///   y >= 0: q* + (T(y) − T(0)) + max(T(0) − q*, 0)·ramp(y)
///           a member above the main run runs on the rating; when the main run
///           sits below its rating (an empty wet well pumps only what arrives)
///           the spare capacity opens over δ_dry, not in a jump;
///   y <  0: q*·T(y)/T(0) (to zero as the member falls dry), or, when the main
///           run's rating is ~0 or its depth < δ_dry, q*·ramp(δ_dry + y):
///           a member below the main run cannot pass more than it has.
///
/// Continuous, nondecreasing in y, exactly q* at y = 0, 0 once dry (T(dry) = 0):
/// this replaces the kind-4 pump law and its "pump what arrives" balance (pump_bal).
inline double anchoredRatedFlow(double T_y, double T_0, double q_star, double d_star, double y) {
    if (y == 0.0) return q_star;
    if (y > 0.0) return q_star + (T_y - T_0) + std::max(T_0 - q_star, 0.0) * ramp(y);
    if (T_0 > 1.0e-12 && d_star > kDeltaDry) return q_star * std::max(T_y, 0.0) / T_0;
    return q_star * ramp(kDeltaDry + y);
}

/// Free-end conduit (note §1.2, kind 0): Q = q0·K(y_up)/K(y0)·s(H)/s(H0), with
/// H = h_up − max(h_dn, z_end) the head over the downstream control. K is the
/// monotone envelope of conveyance (circular conveyance falls above ≈0.94 D);
/// the caller passes K at the member's and the main run's upstream depth.
/// A supercritical reach (relay off) does not feel H.
inline double freeEndFlow(double q0, double K_y, double K_0, double H, double H0, bool use_dn) {
    if (!(K_0 > 0.0) || !(q0 > 0.0)) return 0.0;
    double f = (K_y == K_0) ? 1.0 : std::max(K_y, 0.0) / K_0;
    if (use_dn) {
        const double s0 = sqrtReg(H0);
        f *= (H == H0) ? 1.0 : (s0 > 0.0 ? sqrtReg(H) / s0 : 0.0);
    }
    return q0 * f;
}

/// Raised-end pipe between B (pipe end z_B) and F (pipe end z_F), positive B -> F
/// (note §1.3, corrected): water leaves a node only if it stands above that
/// node's pipe end, and drives against the higher of the far water and the far
/// end:
///   Q = K·[ s(h_B − max(h_F, z_F))·r(h_B − z_B) − s(h_F − max(h_B, z_B))·r(h_F − z_F) ]
/// s = regularised square root, r = dry ramp. Q > 0 only if h_B > max(h_F, z_F).
inline double raisedEndFlow(double K, double hB, double zB, double hF, double zF) {
    return K * (sqrtReg(hB - std::max(hF, zF)) * ramp(hB - zB) -
                sqrtReg(hF - std::max(hB, zB)) * ramp(hF - zF));
}

/// The law it replaces (kind 6 before phase 5), kept only for the mutation
/// tests: K·sign(Δ)·sqrt|Δ| with Δ = max(h_B, z_B) − max(h_F, z_F). Below both
/// ends it gives a constant ±K·sqrt|z_B − z_F|: a member below z_B whose main
/// run is above z_B keeps draining water it does not have (the phantom).
inline double raisedEndFlowLegacyKind6(double K, double hB, double zB, double hF, double zF) {
    const double d = std::max(hB, zB) - std::max(hF, zF);
    return (d >= 0.0 ? 1.0 : -1.0) * K * std::sqrt(std::fabs(d));
}

/// Dry factor at a store for a member at offset z from a main run of depth d*:
/// 1 at and above the main run, falling to 0 over the bottom δ_dry of the
/// store (below the invert by at most δ_dry when the main run itself is dry).
inline double dryFactor(double d_star, double z) {
    if (z >= 0.0) return 1.0;
    return d_star > kDeltaDry ? ramp(d_star + z) : ramp(kDeltaDry + z);
}

/// A member's absolute flow q (up -> dn) limited by the water on the side it
/// leaves: q·g_up when q > 0, q·g_dn when q < 0. A straight-line outlet
/// q* + ju·z_up + jd·z_dn keeps draining an empty member when q* > ju·d*, and
/// an anchored two-way law does when the main run's own flow differs from the
/// law at the main run; this is what makes a store's floor a port definition
/// (note §4). Monotone: nondecreasing in z_up, nonincreasing in z_dn, when q is
/// and g_up (g_dn) are nondecreasing in z_up (z_dn); exact at z = 0 (g = 1).
inline double flooredFlow(double q, double g_up, double g_dn) {
    return q > 0.0 ? q * g_up : q * g_dn;
}

/// Rim overflow out of a store (absolute, >= 0): the main run's own spill q_ovf*
/// plus k per ft above it; for a main run below its rim, k per ft above the rim.
/// z_rim = (rim + surcharge) − h* (ft, may be <= 0 when the main run spills).
inline double overflowFlow(double q_ovf_star, double k, double z_rim, double z) {
    if (q_ovf_star > 0.0 || z_rim <= 0.0) return std::max(q_ovf_star + k * z, 0.0);
    return std::max(k * (z - z_rim), 0.0);
}

}  // namespace openswmm::uncertainty::ports

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_STORE_PORTS_HPP
