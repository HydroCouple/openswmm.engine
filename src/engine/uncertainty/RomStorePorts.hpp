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

/// A conduit cannot carry a member's water against that member's own head (O-13).
/// The member laws of a pipe are anchored on the main run's regime: a dry pipe's
/// normal-flow rating Q_n(y_up), a supercritical free end (relay off), a straight
/// line q* + ju·z_up + jd·z_dn with |jd| < ju. Each holds while the member's
/// downstream water stays below its upstream water; none of them sees a member
/// whose downstream has risen above its upstream, and each keeps pushing water
/// uphill there (Bellinge 2026-11-17: G70S214 -> G70S213 delivered the dry pipe's
/// full capacity, 0.99 cfs, into a node standing 89 ft above it; G71F044 ->
/// G71F043 1.3 cfs against 1.8 ft of adverse member head). The bound is the
/// pipe's full-bore friction flow at the member's head over the downstream
/// control,
///     Q <= K_b · s(h_up − max(h_dn, z_dn_end)),   s = sqrtReg,
/// with K_b the full-bore conveyance factor PHI/n·max(A·R^(2/3))·n_barrels/sqrt(L),
/// raised to q*/s(H0) where the main run carries more than that at its own head H0
/// (so the bound never binds at the main run: Q(0,0) = q* exactly). Where the main
/// run's own head over the downstream control is below δ0 (a drowned or adverse
/// main run) the bound is off: the straight line with its jd is the law there.
/// Q = min(law, bound): nondecreasing in z_up, nonincreasing in z_dn when the law
/// is; zero once the member's downstream water reaches its upstream water.
struct HeadBound {
    double K = 0.0, hu = 0.0, hd = 0.0, z_end = 0.0;
    bool active = false;
    static HeadBound make(double K_full, double q_star, double hu, double hd, double z_end) {
        HeadBound b;
        const double H0 = hu - std::max(hd, z_end);
        if (!(H0 > kDelta0) || !(K_full > 0.0) || !std::isfinite(K_full)) return b;
        b.K = std::max(K_full, std::max(q_star, 0.0) / sqrtReg(H0));
        b.hu = hu; b.hd = hd; b.z_end = z_end;
        b.active = true;
        return b;
    }
    double cap(double zu, double zd) const { return K * sqrtReg(hu + zu - std::max(hd + zd, z_end)); }
    double apply(double q, double zu, double zd) const { return active ? std::min(q, cap(zu, zd)) : q; }
};

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

/// Street exchange at a coupled manhole (note §1.2 last row; PORT_HAMILTONIAN
/// card O-3): an orifice between the manhole (head h_mh) and its street pond
/// (depth p over the coupling cell's ground z_or), positive from the manhole
/// into the street:
///   Q = c·sqrt(2g)·sign(Δ)·s(|Δ|),   Δ = h_mh − (z_or + p),
/// s = the regularised square root (linear below d0, in the law's length
/// unit). Odd and nondecreasing in Δ, so nondecreasing in h_mh and
/// nonincreasing in p (P1), with slope <= c·sqrt(2g)/sqrt(d0) everywhere: the
/// Newton Jacobian stays finite when manhole and pond are nearly level (the
/// bare square root has an infinite slope there). The street level is never
/// below the ground (p is clamped at 0): in deviation form a member whose pond
/// gains less than the main run's over a step reads p_star + z < 0 while its
/// own water is still >= 0 (the main run's pond rose meanwhile).
inline double orificeFlow(double c, double two_g, double h_mh, double z_or, double p, double d0) {
    const double d = h_mh - (z_or + std::max(p, 0.0));
    const double s = sqrtReg(std::fabs(d), d0);
    return c * std::sqrt(two_g) * (d >= 0.0 ? s : -s);
}

/// The street pond over a coupled manhole as a store (O-3): linear volume
/// V(p) = a·p over the footprint area a; the unknown is the pond depth's
/// deviation z from the main run's pond depth p_star, so dV(z) = a·z. Floor 0:
/// water leaves the pond only while it has some, through the dry factor
/// ramp(p_star + z) on the pond's own absolute depth (0 once empty, 1 above
/// δ_dry). Absolute, not relative to the main run like dryFactor(): the pond's
/// reference flow is the same law at the main run (no outside q*), and a dry
/// main-run pond must not pour water it does not have.
struct PondStore {
    double a = 0.0;        ///< footprint area (ft^2 in the per-member graph)
    double p_star = 0.0;   ///< main run's pond depth (ft)
    double dV(double z, double* area) const {
        if (area) *area = a;
        return a * z;
    }
    double dry(double z) const { return ramp(p_star + z); }
};

/// The manhole–pond two-port: orifice flow manhole -> pond, limited by the
/// water on the side it leaves (g_mh for flow into the street, g_pond for
/// flow out of the pond, both nondecreasing in their own level). Monotone
/// (P1), conservative between the two stores (P2), and the same expression
/// gives the main run's reference flow at z = 0 (P3).
inline double orificePortFlow(double c, double two_g, double h_mh, double z_or, double p, double d0,
                              double g_mh, double g_pond) {
    return flooredFlow(orificeFlow(c, two_g, h_mh, z_or, p, d0), g_mh, g_pond);
}

/// The manhole–pond pair as ONE composite port on the manhole (O-10).
///
/// The pond is a linear store (volume a·z) whose only port is the orifice, so
/// its level at the end of the step is a scalar function of the manhole's:
///
///   a·(z_p − z_p^n)/dt = E(z_m, z_p),   E = the orifice's extra flow manhole -> pond
///
/// E is nonincreasing in z_p, so the left side minus the right is strictly
/// increasing: one root, z_p*(z_m), bracketed by z_p^n and z_p^n + dt·E(z_m, z_p^n)/a
/// (the root lies on the side E points to, and no further than a full step of
/// that flow). The composite law Q(z_m) = E(z_m, z_p*(z_m)) is nondecreasing in
/// z_m (the root moves the same way as the manhole and E falls with it, never
/// past the manhole's own effect: implicit-function monotonicity), 0 at the main
/// run, and the water the pond gains is exactly dt·Q, so what leaves the
/// manhole is what the booking hands to the 2D ROM. A nested store: eliminating
/// it from the unknowns leaves the solution of the two-unknown step unchanged.
/// The root is found to roundoff (a safeguarded regula falsi), because Newton
/// differences the law at 1e-7 ft.
///
/// Lengths in ft inside the per-member graph; the orifice law runs in metres
/// (ft_to_m converts; 1.0 for a law kept in ft), its flow in m^3/s, the extra
/// flow handed back in cfs (cfs_per_cms; 1.0 for a law kept in cfs).
struct PondOrificePort {
    double c_or = 0.0, two_g = 0.0, d0 = kDelta0;   ///< orifice coefficient·area, 2g, regularisation length (law unit)
    double ft_to_m = 1.0, cfs_per_cms = 1.0;
    double h_det = 0.0, z_or = 0.0;                 ///< manhole head (ft), street ground (law unit)
    double p_det = 0.0, d_mh = 0.0;                 ///< main run's pond depth (ft), manhole depth (ft)
    double a = 0.0;                                 ///< pond footprint (ft^2)
    double q_det = 0.0;                             ///< the law at the main run (law flow unit)

    /// Fill q_det from the other fields (call once they are set).
    void anchor() {
        q_det = orificePortFlow(c_or, two_g, h_det * ft_to_m, z_or, p_det * ft_to_m, d0, ramp(d_mh), ramp(p_det));
    }
    /// The two-unknown port law (the O-9 form): extra flow manhole -> pond (cfs).
    double extraFlow(double cd_mult, double zu, double zp) const {
        if (zu == 0.0 && zp == 0.0 && cd_mult == 1.0) return 0.0;
        const double qi = orificePortFlow(c_or * cd_mult, two_g, (h_det + zu) * ft_to_m, z_or, (p_det + zp) * ft_to_m, d0,
                                          ramp(d_mh + zu), ramp(p_det + zp));
        return (qi - q_det) * cfs_per_cms;
    }
    /// The pond's level deviation at the end of the step for a pond that starts at zp_prev.
    double pondLevel(double cd_mult, double zu, double zp_prev, double dt) const {
        const double s = a / dt;
        auto g = [&](double zp) { return s * (zp - zp_prev) - extraFlow(cd_mult, zu, zp); };
        const double e0 = extraFlow(cd_mult, zu, zp_prev);
        if (e0 == 0.0 || !(s > 0.0) || !std::isfinite(e0)) return zp_prev;
        double lo = zp_prev, hi = zp_prev + e0 / s;
        if (hi < lo) std::swap(lo, hi);
        double glo = g(lo), ghi = g(hi);
        if (glo == 0.0) return lo;
        if (ghi == 0.0) return hi;
        if (!(glo < 0.0 && ghi > 0.0)) return glo * glo < ghi * ghi ? lo : hi;   // non-monotone law: not expected
        constexpr double eps = 2.220446049250313e-16;
        int side = 0;
        double x_prev = lo;
        for (int it = 0; it < 200; ++it) {
            double x = (lo * ghi - hi * glo) / (ghi - glo);
            if (!(x > lo && x < hi)) x = 0.5 * (lo + hi);
            if (it % 4 == 3) x = 0.5 * (lo + hi);   // bisection every few steps: bounded worst case
            const double gx = g(x);
            if (gx == 0.0) return x;
            if (gx > 0.0) { hi = x; ghi = gx; if (side == 1) glo *= 0.5; side = 1; }
            else          { lo = x; glo = gx; if (side == -1) ghi *= 0.5; side = -1; }
            const double scale = std::max({1.0, std::fabs(lo), std::fabs(hi)});
            if (hi - lo <= 4.0 * eps * scale || std::fabs(x - x_prev) <= 2.0 * eps * scale) return x;
            x_prev = x;
        }
        return 0.5 * (lo + hi);
    }
    /// The composite law: the extra flow manhole -> street at the pond's own level (cfs).
    double compositeFlow(double cd_mult, double zu, double zp_prev, double dt) const {
        if (zu == 0.0 && zp_prev == 0.0 && cd_mult == 1.0) return 0.0;
        return extraFlow(cd_mult, zu, pondLevel(cd_mult, zu, zp_prev, dt));
    }
};

}  // namespace openswmm::uncertainty::ports

#endif  // OPENSWMM_ENGINE_UNCERTAINTY_ROM_STORE_PORTS_HPP
