/**
 * @file FvKernels.hpp
 * @brief Single-source scalar kernels for the explicit FV 1D network solver.
 *
 * @details The numerical core of ExplicitFvSolver: the Preissmann-slot cross-
 *          section closure, hydrostatic (Audusse) face reconstruction, the
 *          HLL/HLLC flux for the conservative St. Venant system, semi-implicit
 *          Manning friction, and the CFL bound. Everything is a plain inline
 *          function over scalars and const references to POD — no owning
 *          containers, no allocation, no exceptions — so the identical bodies
 *          compile for the CPU solver and, annotated, for the Kokkos device
 *          backend. Same pattern as 2d/solver/InertialKernels.hpp.
 *
 *          **Scheme summary** (face between cells L,R; flux positive L→R;
 *          internal US units, g = 32.2 ft/s²):
 *
 *            U   = [A, Q]                          conserved per barrel
 *            F   = [Q, Q·u + g·I₁(h)]              physical flux
 *            I₁  = ∫₀ʰ A(η)dη                      hydrostatic first moment
 *            c   = √(g·A/T)                        wave celerity
 *
 *          Hydrostatic reconstruction (Audusse et al. 2004):
 *
 *            z*   = max(z_L, z_R)
 *            h*_K = max(0, η_K − z*),  u*_K = u_K
 *            F    = HLLC(U*_L, U*_R)
 *            F_L^corrected = F + [0, g(I₁(h_L) − I₁(h*_L))]
 *            F_R^corrected = F + [0, g(I₁(h_R) − I₁(h*_R))]
 *
 *          At rest (η_L = η_R) the HLLC flux reduces to [0, g·I₁(h*)] and the
 *          correction leaves g·I₁(h_i) at BOTH faces of cell i, so the momentum
 *          divergence is exactly zero for any bed — lake-at-rest holds to
 *          machine precision, and it holds regardless of the quadrature error
 *          in the I₁ table because only single-valuedness is required.
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §3.2, §3.3
 * @ingroup engine_fv
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_FV_KERNELS_HPP
#define OPENSWMM_ENGINE_FV_KERNELS_HPP

#include <algorithm>
#include <cmath>

#include "FvOptions.hpp"
#include "NetworkMeshData.hpp"

// Portable kernel-function marker — identical convention to
// 2d/solver/InertialKernels.hpp:45. Host builds get plain `inline`; the GPU
// plugin defines OPENSWMM_KERNEL_FN to KOKKOS_INLINE_FUNCTION *before*
// including this header so the same scalar bodies compile for the device.
#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::fv::kernels {

/// Gravity in internal units. Matches constants::GRAVITY — spelled here so the
/// header stays dependency-free for the plugin build.
inline constexpr double kGravity = 32.2;

/// Depth below which a cell is treated as dry: no velocity, no flux, no
/// celerity contribution. Small enough to be hydraulically irrelevant (3e-5 ft
/// ≈ 0.01 mm) and large enough to keep Q/A finite.
inline constexpr double kDryDepth = 1.0e-7;

/// Area floor paired with kDryDepth for the u = Q/A division.
inline constexpr double kDryArea = 1.0e-12;

/// Free-surface difference below which a face is treated as exactly level.
/// Without it the momentum update integrates 1-ulp closure round-trip noise;
/// 1e-12 ft is far below any physical head.
inline constexpr double kEtaDeadband = 1.0e-12;

// ===========================================================================
// Section evaluation — the ONE place the cross-section machinery is called
// ===========================================================================
//
// These four forward to the existing per-element accessors, which are the same
// single-source bodies XSectBatch's kernels use. The Kokkos backend replaces
// the bodies with the flattened device table lookups from DeviceXsectTables.hpp
// (plan §5.1) behind these exact signatures, and the §6.8 parity harness
// asserts host and device agree within the recorded ULP envelope.

/// Section area at depth h, EXCLUDING the slot (h clamped to the crown).
OPENSWMM_KERNEL_FN double sectionArea(const FvGeometry& g, double h) noexcept {
    if (h <= 0.0) return 0.0;
    return xsect::getAofY(g.xs, (h < g.y_full) ? h : g.y_full);
}

/// Section top width at depth h, EXCLUDING the slot. RECT_CLOSED returns 0 at
/// exactly y_full (the crown is a point); the slot term added by widthOfDepth
/// is what keeps the total width — and hence the celerity — finite there.
OPENSWMM_KERNEL_FN double sectionWidth(const FvGeometry& g, double h) noexcept {
    if (h <= 0.0) return 0.0;
    return xsect::getWofY(g.xs, (h < g.y_full) ? h : g.y_full);
}

/// Section hydraulic radius at depth h. Frozen at r_full above the crown —
/// the slot is a numerical device and must not contribute wetted perimeter
/// (same convention as legacy dwflow.c::getHydRad).
OPENSWMM_KERNEL_FN double sectionHydRad(const FvGeometry& g, double h) noexcept {
    if (h <= 0.0) return 0.0;
    if (h >= g.y_full) return g.r_full;
    return xsect::getRofY(g.xs, h);
}

// ===========================================================================
// Preissmann slot taper (plan §3.3.2)
// ===========================================================================

/// Smoothstep ramp opening the slot over [y_crown, y_full]. C¹ at both ends, so
/// dA/dh is continuous through the crown and the Riemann solver's wave-speed
/// estimates do not see a jump. Above the crown the ramp saturates at 1.
OPENSWMM_KERNEL_FN double slotRamp(double s) noexcept {
    if (s <= 0.0) return 0.0;
    if (s >= 1.0) return 1.0;
    return s * s * (3.0 - 2.0 * s);
}

/// ∫₀ˢ slotRamp — the normalized slot area accumulated up to s. Continuous and
/// exactly matched to slotRamp so A(h) = ∫T(h)dh holds through the taper.
OPENSWMM_KERNEL_FN double slotRampIntegral(double s) noexcept {
    if (s <= 0.0) return 0.0;
    if (s >= 1.0) return s - 0.5;               // 1 − ½ from the taper, then linear
    const double s3 = s * s * s;
    return s3 - 0.5 * s3 * s;                   // s³ − s⁴/2
}

// ===========================================================================
// Closure — one continuous geometry from dry bed to full pressurization
// ===========================================================================

/// Flow area at depth @p h, INCLUDING the tapered slot. Monotone in h for any
/// section, which is what makes the depth inversion well posed.
OPENSWMM_KERNEL_FN double areaOfDepth(const FvGeometry& g, double h) noexcept {
    if (h <= 0.0) return 0.0;
    if (h >= g.y_full) return g.a_crown + g.t_slot * (h - g.y_full);
    const double band = g.y_full - g.y_crown;
    const double ax = sectionArea(g, h);
    if (band <= 0.0) return ax;                 // open section: no taper band
    const double s = (h - g.y_crown) / band;
    return ax + g.t_slot * band * slotRampIntegral(s);
}

/// Top width dA/dh at depth @p h, INCLUDING the tapered slot.
OPENSWMM_KERNEL_FN double widthOfDepth(const FvGeometry& g, double h) noexcept {
    if (h <= 0.0) return 0.0;
    if (h >= g.y_full) return g.t_slot;
    const double band = g.y_full - g.y_crown;
    const double wx = sectionWidth(g, h);
    if (band <= 0.0) return wx;
    const double s = (h - g.y_crown) / band;
    return wx + g.t_slot * slotRamp(s);
}

/// Hydraulic radius at depth @p h.
OPENSWMM_KERNEL_FN double hydRadOfDepth(const FvGeometry& g, double h) noexcept {
    if (h <= 0.0) return 0.0;
    if (h >= g.y_full) return g.r_full;
    return sectionHydRad(g, h);
}

/**
 * @brief Hydrostatic first moment I₁(h) = ∫₀ʰ A(η)dη.
 *
 * Below the crown this reads the per-geometry table built at init (uniform on
 * [0, y_full]) and refines with one trapezoid step using the exact A(h) the
 * caller needs anyway — second-order accurate and, crucially, a single-valued
 * function of h, which is all the well-balanced property requires.
 *
 * Above the crown A is exactly linear (A = a_crown + t_slot·(h − y_full)), so
 * the extension is analytic — deep surcharge stays exact with a small table.
 */
OPENSWMM_KERNEL_FN double i1OfDepth(const FvGeometry& g, double h,
                                    double area_at_h) noexcept {
    if (h <= 0.0) return 0.0;
    if (h >= g.y_full) {
        const double d = h - g.y_full;
        return g.i1_crown + g.a_crown * d + 0.5 * g.t_slot * d * d;
    }
    const int n = static_cast<int>(kI1Samples);
    const double dh = g.y_full / static_cast<double>(n - 1);
    int i = static_cast<int>(h / dh);
    if (i < 0) i = 0;
    if (i > n - 2) i = n - 2;
    const double h_i = static_cast<double>(i) * dh;
    // i1_tbl stores I₁ at the sample points; the companion area sample is the
    // second half of the same buffer (see buildI1Table).
    const double i1_i = g.i1_tbl[static_cast<std::size_t>(i)];
    const double a_i  = g.i1_tbl[static_cast<std::size_t>(n + i)];
    return i1_i + 0.5 * (a_i + area_at_h) * (h - h_i);
}

/**
 * @brief Invert A → h — the EXACT inverse of areaOfDepth above.
 *
 * @details This must be a true inverse, not merely an accurate one. The solver
 *          carries A and derives the free surface as η = z_b + depthOfArea(A);
 *          if the composition depthOfArea∘areaOfDepth were not the identity,
 *          cells sitting at the same η but different bed elevations would
 *          reconstruct DIFFERENT surfaces, and lake-at-rest — the property the
 *          whole well-balanced construction exists to deliver — would fail on
 *          every partly-full pipe.
 *
 *          `xsect::getYofA` cannot be used for this. The legacy geometry tables
 *          are INDEPENDENT tabulations of the same shape — `A_Circ` gives area
 *          from depth, `Y_Circ` gives depth from area — and they round-trip only
 *          to table resolution (measured: 0.016 ft on a 3 ft circular pipe near
 *          the crown, ~0.5 % of the diameter). That is fine for the legacy
 *          solver, which never composes them; it is fatal here.
 *
 *          So the inverse is built from the forward closure itself: bracket on
 *          the A samples stored alongside the I₁ table (both were produced by
 *          areaOfDepth, so the bracket is exact), then Illinois-regula-falsi
 *          inside the panel. A is strictly increasing — a monotone section plus
 *          a strictly increasing slot term — so the bracket always holds and
 *          convergence is unconditional; within one panel A is nearly linear, so
 *          it typically takes four or five evaluations.
 *
 *          Above the crown A is exactly linear, so that branch is closed-form.
 */
OPENSWMM_KERNEL_FN double depthOfArea(const FvGeometry& g, double a) noexcept {
    if (a <= 0.0) return 0.0;
    if (a >= g.a_crown) return g.y_full + (a - g.a_crown) / g.t_slot;

    const int n = static_cast<int>(kI1Samples);
    const double dh = g.y_full / static_cast<double>(n - 1);

    int lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (g.i1_tbl[static_cast<std::size_t>(n + mid)] <= a) lo = mid;
        else                                                  hi = mid;
    }

    double xa = static_cast<double>(lo) * dh;
    double xb = static_cast<double>(hi) * dh;
    double fa = g.i1_tbl[static_cast<std::size_t>(n + lo)] - a;
    double fb = g.i1_tbl[static_cast<std::size_t>(n + hi)] - a;
    if (fa >= 0.0) return xa;
    if (fb <= 0.0) return xb;

    for (int it = 0; it < 40; ++it) {
        double x = xb - fb * (xb - xa) / (fb - fa);
        if (!(x > xa && x < xb)) x = 0.5 * (xa + xb);
        const double f = areaOfDepth(g, x) - a;
        if (f == 0.0) return x;
        if (f < 0.0) { xa = x; fa = f; fb *= 0.5; }   // Illinois down-weighting
        else         { xb = x; fb = f; fa *= 0.5; }
        if (xb - xa <= 1.0e-15 * g.y_full) break;
    }
    return 0.5 * (xa + xb);
}

/// Gravity-wave celerity √(g·A/T). Guarded so a vanishing top width (dry, or a
/// section evaluated exactly at a cusp) cannot produce a non-finite dt bound.
OPENSWMM_KERNEL_FN double celerity(double a, double t) noexcept {
    if (a <= kDryArea || t <= 0.0) return 0.0;
    return std::sqrt(kGravity * a / t);
}

// ===========================================================================
// Riemann solver
// ===========================================================================

/// One side of a face after hydrostatic reconstruction.
struct FaceState {
    double a  = 0.0;  ///< reconstructed flow area (ft²)
    double q  = 0.0;  ///< reconstructed discharge (cfs) = a·u
    double u  = 0.0;  ///< velocity (ft/s)
    double c  = 0.0;  ///< celerity (ft/s)
    double i1 = 0.0;  ///< hydrostatic first moment at the reconstructed depth
};

/// Result of a face flux evaluation.
struct FaceFlux {
    double mass = 0.0;   ///< F[0] — volumetric flux (cfs)
    double mom  = 0.0;   ///< F[1] — momentum flux (ft⁴/s²)
    double sstar = 0.0;  ///< contact-wave speed; sign selects the species upwind
};

/// Davis/Einfeldt wave-speed estimates with dry-state handling. A dry side has
/// no celerity of its own, so the wet side's rarefaction-tail speed (u ∓ 2c) is
/// used — the standard dry-bed estimate that keeps the front speed correct.
OPENSWMM_KERNEL_FN void waveSpeeds(const FaceState& L, const FaceState& R,
                                   double& sl, double& sr) noexcept {
    const bool wetL = (L.a > kDryArea);
    const bool wetR = (R.a > kDryArea);
    if (wetL && wetR) {
        sl = std::min(L.u - L.c, R.u - R.c);
        sr = std::max(L.u + L.c, R.u + R.c);
    } else if (wetR) {                      // dry left
        sl = R.u - 2.0 * R.c;
        sr = R.u + R.c;
    } else if (wetL) {                      // dry right
        sl = L.u - L.c;
        sr = L.u + 2.0 * L.c;
    } else {
        sl = 0.0;
        sr = 0.0;
    }
}

/// Physical flux of one state.
OPENSWMM_KERNEL_FN void physicalFlux(const FaceState& S,
                                     double& fa, double& fq) noexcept {
    fa = S.q;
    fq = S.q * S.u + kGravity * S.i1;
}

/**
 * @brief HLLC (or HLL) flux for the conservative St. Venant system.
 *
 * HLLC is the default because this mesh is intended to carry advected scalars:
 * for U = [A, Q, Aφ] the eigenvalues are u−c, u, u+c and λ = u IS the contact
 * discontinuity that carries the species. HLL averages that wave into the
 * single intermediate state, smearing concentration fronts at a rate set by the
 * scheme rather than by physical dispersion. The contact speed costs one extra
 * algebraic expression, so there is no performance argument for HLL.
 */
OPENSWMM_KERNEL_FN FaceFlux riemannFlux(const FaceState& L, const FaceState& R,
                                        bool hllc) noexcept {
    FaceFlux out;
    if (L.a <= kDryArea && R.a <= kDryArea) return out;

    double sl = 0.0, sr = 0.0;
    waveSpeeds(L, R, sl, sr);

    double fal = 0.0, fql = 0.0, far = 0.0, fqr = 0.0;
    physicalFlux(L, fal, fql);
    physicalFlux(R, far, fqr);

    if (sl >= 0.0) { out.mass = fal; out.mom = fql; out.sstar = sl; return out; }
    if (sr <= 0.0) { out.mass = far; out.mom = fqr; out.sstar = sr; return out; }

    const double dsr = sr - sl;
    // HLL intermediate state — also the fallback when the contact speed is
    // indeterminate (both sides vanishing).
    const double a_hll = (sr * R.a - sl * L.a - (far - fal)) / dsr;
    const double f_hll_a = (sr * fal - sl * far + sl * sr * (R.a - L.a)) / dsr;
    const double f_hll_q = (sr * fql - sl * fqr + sl * sr * (R.q - L.q)) / dsr;

    const double den = R.a * (R.u - sr) - L.a * (L.u - sl);
    double sstar;
    if (std::fabs(den) > 1.0e-14) {
        sstar = (sl * R.a * (R.u - sr) - sr * L.a * (L.u - sl)) / den;
    } else {
        sstar = (a_hll > kDryArea) ? f_hll_a / a_hll : 0.0;
    }
    out.sstar = sstar;

    if (!hllc) { out.mass = f_hll_a; out.mom = f_hll_q; return out; }

    // HLLC: restore the contact wave. The star states carry the same mass flux
    // as HLL by construction, so the species flux built on out.mass is
    // automatically consistent with the water it rides on.
    if (sstar >= 0.0) {
        const double fac = L.a * (sl - L.u) / (sl - sstar);
        out.mass = fal + sl * (fac - L.a);
        out.mom  = fql + sl * (fac * sstar - L.q);
    } else {
        const double fac = R.a * (sr - R.u) / (sr - sstar);
        out.mass = far + sr * (fac - R.a);
        out.mom  = fqr + sr * (fac * sstar - R.q);
    }
    return out;
}

/**
 * @brief Species flux — the SAME mass flux the hydrodynamic update used,
 *        upwinded on the sign of the contact speed.
 *
 * Flux consistency is a hard requirement, not a detail: computing this from a
 * separately-evaluated velocity decouples solute mass from water mass and
 * produces spurious extrema and non-conservation. Reusing @p mass_flux
 * guarantees exact solute conservation, a discrete maximum principle, and that
 * a uniform concentration field stays uniform under any flow (plan §3.2).
 */
OPENSWMM_KERNEL_FN double speciesFlux(double mass_flux, double sstar,
                                      double phi_l, double phi_r) noexcept {
    return mass_flux * ((sstar >= 0.0) ? phi_l : phi_r);
}

// ===========================================================================
// Source terms
// ===========================================================================

/**
 * @brief Semi-implicit Manning friction — unconditionally stable, so friction
 *        imposes no time-step restriction.
 *
 * Q^{n+1} = Q* / (1 + Δt·g·(n/φ)²·|u|/R^{4/3}), with g·(n/φ)² supplied as
 * @p rough_factor (ConduitData::rough_factor, already adjusted for Courant
 * lengthening). R^{4/3} is written as R·cbrt(R) to avoid a libm pow().
 */
OPENSWMM_KERNEL_FN double frictionUpdate(double q, double u, double r,
                                         double dt, double rough_factor) noexcept {
    if (r <= 0.0 || rough_factor <= 0.0) return q;
    const double r43 = r * std::cbrt(r);
    const double den = 1.0 + dt * rough_factor * std::fabs(u) / r43;
    return q / den;
}

/**
 * @brief Semi-implicit entrance/exit loss at a node-coupling face.
 *
 * The local head loss K·u²/2g is spread over the end cell as a momentum sink
 * g·A·S_loss with S_loss·Δx = K·u²/2g, then integrated implicitly in |u| for the
 * same stability reason as friction. K = 0 leaves Q untouched, so calibrated
 * models that never set losses are bit-unaffected.
 */
OPENSWMM_KERNEL_FN double localLossUpdate(double q, double u, double k,
                                          double dx, double dt) noexcept {
    if (k <= 0.0 || dx <= 0.0) return q;
    return q / (1.0 + dt * k * std::fabs(u) / (2.0 * dx));
}

/// CFL-limited step for one face: α·Δx/(|u| + c).
OPENSWMM_KERNEL_FN double faceCflDt(double cfl, double dx, double u,
                                    double c) noexcept {
    const double s = std::fabs(u) + c;
    return (s > 1.0e-12) ? cfl * dx / s : 1.0e30;
}

/**
 * @brief Positivity scale for a cell about to export more volume than it holds.
 *
 * Outgoing fluxes may take at most the cell's available volume over Δt; faces
 * leaving the cell are scaled by λ and the identical scaled flux updates both
 * incident cells, so conservation is untouched. Same contract as the 2D
 * marcher's positivityScale.
 */
OPENSWMM_KERNEL_FN double positivityScale(double vol, double outflow, double dt) noexcept {
    if (outflow <= 0.0 || dt <= 0.0) return 1.0;
    const double take = outflow * dt;
    const double avail = (vol > 0.0) ? vol : 0.0;
    return (take <= avail) ? 1.0 : avail / take;
}

} // namespace openswmm::fv::kernels

#endif // OPENSWMM_ENGINE_FV_KERNELS_HPP
