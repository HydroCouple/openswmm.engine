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
// These three go through the geometry's own evaluator (XSectKernels.hpp) — the
// SAME bodies XSection.cpp's public accessors and XSectBatch's kernels run.
// Only where the evaluator's tables live differs between the host solver and
// the device backend, so the §6.8 parity harness compares two instantiations of
// one implementation rather than two implementations.

/// Section area at depth h, EXCLUDING the slot (h clamped to the crown).
OPENSWMM_KERNEL_FN double sectionArea(const FvGeometry& g, double h) noexcept {
    if (h <= 0.0) return 0.0;
    return g.eval->getAofY(g.xs, (h < g.y_full) ? h : g.y_full);
}

/// Section top width at depth h, EXCLUDING the slot. RECT_CLOSED returns 0 at
/// exactly y_full (the crown is a point); the slot term added by widthOfDepth
/// is what keeps the total width — and hence the celerity — finite there.
OPENSWMM_KERNEL_FN double sectionWidth(const FvGeometry& g, double h) noexcept {
    if (h <= 0.0) return 0.0;
    return g.eval->getWofY(g.xs, (h < g.y_full) ? h : g.y_full);
}

/// Section hydraulic radius at depth h. Frozen at r_full above the crown —
/// the slot is a numerical device and must not contribute wetted perimeter
/// (same convention as legacy dwflow.c::getHydRad).
OPENSWMM_KERNEL_FN double sectionHydRad(const FvGeometry& g, double h) noexcept {
    if (h <= 0.0) return 0.0;
    if (h >= g.y_full) return g.r_full;
    return g.eval->getRofY(g.xs, h);
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
    double mass  = 0.0;  ///< F[0] — volumetric flux (cfs)
    double mom   = 0.0;  ///< F[1] — momentum flux (ft⁴/s²)
    double sstar = 0.0;  ///< contact-wave speed; sign selects the species upwind
    double sl    = 0.0;  ///< left  signal speed (kept for the species flux)
    double sr    = 0.0;  ///< right signal speed
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
 * @brief Flux for the conservative St. Venant system, plus the contact speed.
 *
 * @details **The hydrodynamic flux is HLL, and that is not a shortcut — it is
 *          what HLLC reduces to here.** The system U = [A, Q] is 2x2 with two
 *          genuinely nonlinear fields and NO middle wave: the contact appears
 *          only once a third component is carried, U = [A, Q, A(phi)], whose
 *          eigenvalues are u-c, **u**, u+c. Applying the Euler-style HLLC
 *          star-state construction to the 2x2 subsystem is inconsistent — the
 *          resulting momentum flux violates the HLL consistency condition, and
 *          on a pressurized/part-full interface (a surcharged manhole feeding a
 *          half-full pipe — the case this solver exists for) it disagrees with
 *          HLL by more than an order of magnitude and drives the flow backwards.
 *
 *          So S* is computed here for its actual job: selecting the upwind side
 *          for the ADVECTED SCALAR (see speciesFlux). That is exactly the role
 *          plan §3.2 assigns it — "λ = v IS the contact discontinuity that
 *          carries the species" — and it is why FV_RIEMANN changes the transport
 *          answer while leaving the hydraulics identical.
 */
OPENSWMM_KERNEL_FN FaceFlux riemannFlux(const FaceState& L,
                                        const FaceState& R) noexcept {
    FaceFlux out;
    if (L.a <= kDryArea && R.a <= kDryArea) return out;

    double sl = 0.0, sr = 0.0;
    waveSpeeds(L, R, sl, sr);
    out.sl = sl;
    out.sr = sr;

    double fal = 0.0, fql = 0.0, far = 0.0, fqr = 0.0;
    physicalFlux(L, fal, fql);
    physicalFlux(R, far, fqr);

    if (sl >= 0.0) { out.mass = fal; out.mom = fql; out.sstar = sl; return out; }
    if (sr <= 0.0) { out.mass = far; out.mom = fqr; out.sstar = sr; return out; }

    const double dsr = sr - sl;
    out.mass = (sr * fal - sl * far + sl * sr * (R.a - L.a)) / dsr;
    out.mom  = (sr * fql - sl * fqr + sl * sr * (R.q - L.q)) / dsr;

    // Contact speed (plan §3.2). Falls back to the HLL-averaged velocity when
    // the denominator degenerates, which happens only when both sides are
    // vanishing — where the species flux is zero anyway.
    const double den = R.a * (R.u - sr) - L.a * (L.u - sl);
    if (std::fabs(den) > 1.0e-14) {
        out.sstar = (sl * R.a * (R.u - sr) - sr * L.a * (L.u - sl)) / den;
    } else {
        const double a_hll = (sr * R.a - sl * L.a - (far - fal)) / dsr;
        out.sstar = (a_hll > kDryArea) ? out.mass / a_hll : 0.0;
    }
    return out;
}

/**
 * @brief Species flux for the A(phi) component.
 *
 * @details HLLC upwinds on the sign of the contact speed and multiplies by the
 *          SAME mass flux the water used. Flux consistency is a hard
 *          requirement, not a detail: computing this from a separately-evaluated
 *          velocity — the usual trap when transport is bolted onto a hydraulic
 *          solver — decouples solute mass from water mass and produces spurious
 *          extrema and non-conservation. Reusing the mass flux guarantees exact
 *          solute conservation, a discrete maximum principle, and that a uniform
 *          field stays uniform under any flow (plan §3.2).
 *
 *          HLL instead averages the whole fan into one intermediate state,
 *          smearing precisely the wave that carries the species. It still
 *          preserves a uniform field (the average is linear in phi when phi is
 *          constant), so it is a legitimate baseline — just a diffusive one.
 *          The §6.11(d) front-width comparison separates this layer's
 *          contribution from the reconstruction's.
 */
OPENSWMM_KERNEL_FN double speciesFlux(const FaceState& L, const FaceState& R,
                                      const FaceFlux& f, double phi_l,
                                      double phi_r, bool hllc) noexcept {
    if (hllc) return f.mass * ((f.sstar >= 0.0) ? phi_l : phi_r);

    // HLL on the A(phi) component, with the same supersonic branches as the
    // hydrodynamic flux so the two can never disagree about which side is
    // upstream of the whole fan.
    if (f.sl >= 0.0) return L.q * phi_l;
    if (f.sr <= 0.0) return R.q * phi_r;
    const double dsr = f.sr - f.sl;
    return (f.sr * L.q * phi_l - f.sl * R.q * phi_r +
            f.sl * f.sr * (R.a * phi_r - L.a * phi_l)) / dsr;
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
