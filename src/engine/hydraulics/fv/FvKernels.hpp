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
 * @file      FvKernels.hpp
 * @brief     Single-source scalar kernels for the explicit FV 1D network solver.
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
 *            U   = [A, Q]                          conserved across all barrels
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_KERNELS_HPP
#define OPENSWMM_ENGINE_FV_KERNELS_HPP

#include <algorithm>
#include <cmath>

#include "FvOptions.hpp"
#include "NetworkMeshData.hpp"
#include "../ChebSectionBatch.hpp"   // chebAWRofY — the fused closure path

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
/// celerity contribution. The threshold is 1e-7 ft; the separate area floor
/// guards Q/A divisions in callers.
inline constexpr double kDryDepth = 1.0e-7;

/// Area floor paired with kDryDepth for the u = Q/A division.
inline constexpr double kDryArea = 1.0e-12;

/// Free-surface difference below which a face is treated as exactly level.
/// Without it the momentum update integrates 1-ulp closure round-trip noise;
/// 1e-12 ft is far below any physical head.
inline constexpr double kEtaDeadband = 1.0e-12;

// ===========================================================================
// Preissmann slot taper (plan §3.3.2)
// ===========================================================================

/**
 * @brief Evaluate the smooth slot-mouth opening fraction.
 *
 * C1 continuous at both endpoints; no units, allocation or state.
 *
 * @param[in] s Finite normalized depth (h-y_crown)/(y_full-y_crown).
 * @return Zero for s<=0, one for s>=1, otherwise s*s*(3-2*s).
 */
OPENSWMM_KERNEL_FN double slotRamp(double s) noexcept {
    if (s <= 0.0)
        return 0.0;
    if (s >= 1.0)
        return 1.0;
    return s * s * (3.0 - 2.0 * s);
}

/**
 * @brief Integrate the normalized slot-mouth opening from zero to s.
 *
 * Multiply by slot width times taper height to obtain aggregate slot area in ft2.
 *
 * @param[in] s Finite normalized depth; negative values are treated as dry.
 * @return Zero for s<=0, s^3-s^4/2 in (0,1), and s-0.5 above the taper.
 */
OPENSWMM_KERNEL_FN double slotRampIntegral(double s) noexcept {
    if (s <= 0.0)
        return 0.0;
    if (s >= 1.0)
        return s - 0.5; // 1 − ½ from the taper, then linear
    const double s3 = s * s * s;
    return s3 - 0.5 * s3 * s; // s³ − s⁴/2
}

// ===========================================================================
// Closure — one continuous geometry from dry bed to full pressurization
// ===========================================================================
//
// This is the memoryless SLOT closure — every cell, every state, no history.
// TPA exception (issue #156): under FV_PRESSURE_CLOSURE TPA a cell whose
// regime flag is set evaluates the tpa* kernels at the end of this file
// instead. The flag is physical air-pathway history (cleared on cold start
// and hotstart restore), not numerical relaxation; unflagged cells — and the
// entire SLOT closure path — are untouched.

/**
 * @brief Evaluate memoryless slot area at a depth.
 *
 * Delegates to closureArea; the closure includes the tapered slot.
 *
 * @param[in] g Borrowed initialized geometry with valid closure table, positive full depth/slot
 *     width and monotone samples; no pointer is retained.
 * @param[in] h Finite native depth in ft; <=0 is dry and above-crown geometry follows the
 *     slot/vertical-wall continuation.
 * @return Aggregate flow area across barrels in ft2.
 */
OPENSWMM_KERNEL_FN double areaOfDepth(const FvGeometry& g, double h) noexcept {
    return closureArea(g.closure_tbl, h);
}

/**
 * @brief Evaluate memoryless slot top width at a depth.
 *
 * Delegates to closureWidth; at/above the crown returns the slot or open-section continuation
 * width.
 *
 * @param[in] g Borrowed initialized geometry with valid closure table, positive full depth/slot
 *     width and monotone samples; no pointer is retained.
 * @param[in] h Finite native depth in ft; <=0 is dry and above-crown geometry follows the
 *     slot/vertical-wall continuation.
 * @return Aggregate dA/dh in ft, zero at/below the invert.
 */
OPENSWMM_KERNEL_FN double widthOfDepth(const FvGeometry& g, double h) noexcept {
    return closureWidth(g.closure_tbl, h);
}

/**
 * @brief Evaluate memoryless closure friction radius.
 *
 * Delegates to closureHydRad; above-crown radius stays r_full.
 *
 * @param[in] g Borrowed initialized geometry with valid closure table, positive full depth/slot
 *     width and monotone samples; no pointer is retained.
 * @param[in] h Finite native depth in ft; <=0 is dry and above-crown geometry follows the
 *     slot/vertical-wall continuation.
 * @return Per-barrel hydraulic radius in ft.
 */
OPENSWMM_KERNEL_FN double hydRadOfDepth(const FvGeometry& g, double h) noexcept {
    return closureHydRad(g.closure_tbl, h);
}

/**
 * @brief Evaluate the memoryless closure hydrostatic area integral.
 *
 * Uses the exact integral of the same panel polynomial/slot continuation as areaOfDepth, ensuring a
 * single-valued pressure closure.
 *
 * @param[in] g Borrowed initialized geometry with valid closure table, positive full depth/slot
 *     width and monotone samples; no pointer is retained.
 * @param[in] h Finite native depth in ft; <=0 is dry and above-crown geometry follows the
 *     slot/vertical-wall continuation.
 * @param[in] area_at_h Unused compatibility value; the integral is computed entirely from the
 *     closure and h.
 * @return Integral of aggregate area over depth, in ft3.
 */
OPENSWMM_KERNEL_FN double i1OfDepth(const FvGeometry& g, double h, double area_at_h) noexcept {
    (void)area_at_h;
    return closureI1(g.closure_tbl, h);
}

/**
 * @brief Invert aggregate area on the memoryless closure.
 *
 * Polynomial inversion is direct; tabulated inversion uses up to 12 guarded Newton steps on the
 * same cubic as areaOfDepth. Returns the last iterate without convergence status if the iteration
 * limit is reached. Positive slot width and valid monotone samples/index map are required.
 *
 * @param[in] g Borrowed initialized geometry with valid closure table, positive full depth/slot
 *     width and monotone samples; no pointer is retained.
 * @param[in] a Finite aggregate flow area in ft2; <=0 returns zero.
 * @return Depth in ft from closureDepthOfArea.
 */
/**
 * @brief Evaluate the whole closure at one depth in a single pass.
 *
 * Semantically identical to calling areaOfDepth / widthOfDepth / hydRadOfDepth
 * / i1OfDepth separately, and asserted bit-identical to them by
 * `FvClosure.F1_ClosureAllIsBitIdenticalToUnfusedPath_*`.
 *
 * @note Kept as the fused facade over the four kernels above after the
 *       FvClosure rework replaced this branch's own fusion: the solver's hot
 *       path now calls `closureEval` directly (one panel locate for A, T and
 *       I₁), so this is the convenience entry point and the invariant the
 *       acceptance test pins, not a performance path.
 */
OPENSWMM_KERNEL_FN void closureAll(const FvGeometry& g, double h,
                                   double* a, double* w, double* r,
                                   double* i1) noexcept {
    const ClosureEval e = closureEval(g.closure_tbl, h);
    if (a)  *a  = e.a;
    if (w)  *w  = e.t;
    if (i1) *i1 = e.i1;
    if (r)  *r  = hydRadOfDepth(g, h);
}

OPENSWMM_KERNEL_FN double depthOfArea(const FvGeometry& g, double a) noexcept {
    // NOTE (promptperf.md Phase E, carried forward): a COMPILED inverse was
    // built here and rejected. Seeding from chebYofA and running Newton on the
    // true areaOfDepth measured 264 s -> 162 s on Bellinge FV EXACT (2 h), but
    // stopping on a 1e-15*y_full step leaves the iterate short of a true fixed
    // point and this function's contract is to be the EXACT inverse of
    // areaOfDepth: worst free-surface drift on a partly-full closed pipe went
    // 5.6e-5 -> 1.1e-4 ft, and lake-at-rest is the property the whole
    // well-balanced construction exists to deliver. Tightening to a
    // machine-precision fixed point is unreachable (areaOfDepth carries ~1e-16
    // relative noise, so the step jitters at the noise floor) and measured
    // SLOWER than doing nothing. The bracket is what makes the root reachable
    // on a noisy function; closureDepthOfArea keeps one. Anyone revisiting
    // should attack the number of CALLS, not the cost of one.
    return closureDepthOfArea(g.closure_tbl, a);
}

/**
 * @brief Compute the gravity-wave speed from aggregate area and top width.
 *
 * Guards do not validate NaN, infinity or overflow in positive inputs.
 *
 * @param[in] a Finite area in ft2; <=kDryArea returns zero.
 * @param[in] t Finite top width in ft; <=0 returns zero.
 * @return sqrt(32.2*a/t) in ft/s, or zero for dry/degenerate geometry.
 */
OPENSWMM_KERNEL_FN double celerity(double a, double t) noexcept {
    if (a <= kDryArea || t <= 0.0)
        return 0.0;
    return std::sqrt(kGravity * a / t);
}

// ===========================================================================
// Riemann solver
// ===========================================================================

/// One side of a face after hydrostatic reconstruction.
struct FaceState {
    double a = 0.0;  ///< reconstructed flow area (ft²)
    double q = 0.0;  ///< reconstructed discharge (cfs) = a·u
    double u = 0.0;  ///< velocity (ft/s)
    double c = 0.0;  ///< celerity (ft/s)
    double i1 = 0.0; ///< hydrostatic first moment at the reconstructed depth
    /// 1: the side stands in the Preissmann slot (or is TPA-flagged) and its
    /// `c` is the acoustic slot celerity; 0: free surface; 2: a vented
    /// junction's own-head ghost, which never takes part in the slot/free
    /// bore bound (see waveSpeeds). Set by the solver.
    uint8_t press = 0;
};

/// Result of a face flux evaluation.
struct FaceFlux {
    double mass = 0.0;  ///< F[0] — volumetric flux (cfs)
    double mom = 0.0;   ///< F[1] — momentum flux (ft⁴/s²)
    double sstar = 0.0; ///< contact-wave speed; sign selects the species upwind
    double sl = 0.0;    ///< left  signal speed (kept for the species flux)
    double sr = 0.0;    ///< right signal speed
};

/**
 * @brief Estimate left and right signal speeds with dry-front and pressure-bore bounds.
 *
 * Both dry gives zero speeds; one wet side uses its rarefaction tail u±2c. Wet/wet uses Davis
 * bounds, narrowed by a Rankine-Hugoniot bore estimate at interior slot/free interfaces (press 0
 * versus 1). press==2 node ghosts are excluded from that narrowing. Inputs must carry physically
 * consistent area/discharge/velocity and nonnegative celerity; no validation occurs.
 *
 * @param[in] L Finite left reconstructed face state; area ft2, discharge ft3/s, velocity/celerity
 *     ft/s and integral ft3.
 * @param[in] R Finite right reconstructed state in the same aggregate geometry and left-to-right
 *     orientation.
 * @param[out] sl Left signal speed in ft/s.
 * @param[out] sr Right signal speed in ft/s.
 */
OPENSWMM_KERNEL_FN void waveSpeeds(const FaceState& L, const FaceState& R, double& sl,
                                   double& sr) noexcept {
    const bool wetL = (L.a > kDryArea);
    const bool wetR = (R.a > kDryArea);
    if (wetL && wetR) {
        sl = std::min(L.u - L.c, R.u - R.c);
        sr = std::max(L.u + L.c, R.u + R.c);
        // Slot/free-surface interface (one side in the slot, the other not).
        // Davis's symmetric estimate carries the slot's acoustic celerity into
        // BOTH waves, but no signal enters the free-surface side at the slot
        // speed: the wave that crosses into it is the filling/emptying bore,
        // whose speed is the Rankine–Hugoniot jump (ΔQ/ΔA), bounded below by
        // that side's own u ± c and above by the Davis bound. With the slot
        // celerity on both waves the HLL diffusion term at a pressurized
        // entrance is O(c_slot·ΔA) — measured 52 cfs of spurious mass flux and
        // a −245 ft⁴/s² momentum sink on a 3 ft culvert whose junction then
        // flooded at 108 of 120 cfs however high its head rose.
        // A vented junction's ghost (press == 2) is excluded: bounding the
        // wave into a pressurized cell beside it cut the acoustic exchange at
        // every lateral junction of a pressurized tunnel (the node solve then
        // clamped at its rim and booked 63 000 m³ of flooding — Klaver
        // Example_02, 2026-09-13), and bounding the wave into a free cell
        // beside it drove a 2× over-conveying limit cycle at an overloaded
        // entrance. The bound acts at the INTERIOR slot/free faces, which is
        // where the entrance lock lived.
        if (L.press != R.press && L.press < 2 && R.press < 2) {
            if (L.press) {
                const double da = L.a - R.a;
                const double w = (da > 0.0) ? (L.q - R.q) / da : sr;
                sr = std::max(R.u + R.c, std::min(sr, w));
            } else {
                const double da = R.a - L.a;
                const double w = (da > 0.0) ? (R.q - L.q) / da : sl;
                sl = std::min(L.u - L.c, std::max(sl, w));
            }
        }
    } else if (wetR) { // dry left
        sl = R.u - 2.0 * R.c;
        sr = R.u + R.c;
    } else if (wetL) { // dry right
        sl = L.u - L.c;
        sr = L.u + 2.0 * L.c;
    } else {
        sl = 0.0;
        sr = 0.0;
    }
}

/**
 * @brief Evaluate the conservative Saint-Venant physical flux of one state.
 *
 * Output references must be distinct and must not alias S; no dry-state correction is applied.
 *
 * @param[in] S Finite, physically consistent aggregate face state.
 * @param[out] fa Mass/volume flux S.q in ft3/s.
 * @param[out] fq Momentum flux S.q*S.u+32.2*S.i1 in ft4/s2.
 */
OPENSWMM_KERNEL_FN void physicalFlux(const FaceState& S, double& fa, double& fq) noexcept {
    fa = S.q;
    fq = S.q * S.u + kGravity * S.i1;
}

/**
 * @brief Evaluate only the HLL volumetric flux for an algebraic node residual.
 *
 * Uses the same wave speeds and expression as riemannFlux.mass, including wet supersonic branches.
 * Valid states must yield a nonzero fan width when the fan straddles zero; no separate
 * degeneracy/error status is returned.
 *
 * @param[in] L Finite left reconstructed face state; area ft2, discharge ft3/s, velocity/celerity
 *     ft/s and integral ft3.
 * @param[in] R Finite right reconstructed state in the same aggregate geometry and left-to-right
 *     orientation.
 * @return Mass flux in ft3/s, positive left-to-right; zero when both areas are dry.
 */
OPENSWMM_KERNEL_FN double riemannMassFlux(const FaceState& L, const FaceState& R) noexcept {
    if (L.a <= kDryArea && R.a <= kDryArea)
        return 0.0;
    double sl = 0.0, sr = 0.0;
    waveSpeeds(L, R, sl, sr);
    double fal = 0.0, fql = 0.0, far = 0.0, fqr = 0.0;
    physicalFlux(L, fal, fql);
    physicalFlux(R, far, fqr);
    if (sl >= 0.0)
        return fal;
    if (sr <= 0.0)
        return far;
    return (sr * fal - sl * far + sl * sr * (R.a - L.a)) / (sr - sl);
}

/**
 * @brief Evaluate HLL hydraulic flux and the scalar contact-wave speed.
 *
 * Both dry returns a zero result. Supersonic flow selects one physical flux. A straddling fan uses
 * HLL for the two hydraulic equations; contact speed is supplied only for advected scalar
 * selection, with HLL-average fallback for a near-zero contact denominator. No hydrostatic side
 * correction, positivity scaling, gate or culvert limit is applied here; callers apply those
 * consistently afterward.
 *
 * @param[in] L Finite left reconstructed face state; area ft2, discharge ft3/s, velocity/celerity
 *     ft/s and integral ft3.
 * @param[in] R Finite right reconstructed state in the same aggregate geometry and left-to-right
 *     orientation.
 * @return Aggregate volumetric flux in ft3/s, momentum flux in ft4/s2 and signal/contact speeds in
 *     ft/s.
 */
OPENSWMM_KERNEL_FN FaceFlux riemannFlux(const FaceState& L, const FaceState& R) noexcept {
    FaceFlux out;
    if (L.a <= kDryArea && R.a <= kDryArea)
        return out;

    double sl = 0.0, sr = 0.0;
    waveSpeeds(L, R, sl, sr);
    out.sl = sl;
    out.sr = sr;

    double fal = 0.0, fql = 0.0, far = 0.0, fqr = 0.0;
    physicalFlux(L, fal, fql);
    physicalFlux(R, far, fqr);

    if (sl >= 0.0) {
        out.mass = fal;
        out.mom = fql;
        out.sstar = sl;
        return out;
    }
    if (sr <= 0.0) {
        out.mass = far;
        out.mom = fqr;
        out.sstar = sr;
        return out;
    }

    const double dsr = sr - sl;
    out.mass = (sr * fal - sl * far + sl * sr * (R.a - L.a)) / dsr;
    out.mom = (sr * fql - sl * fqr + sl * sr * (R.q - L.q)) / dsr;

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
 * @brief Evaluate a conservative advected-scalar flux using the hydraulic fan.
 *
 * HLL uses the physical-state discharges on supersonic branches. For a dry/dry result with zero
 * signal speeds its left branch returns L.q*phi_l, so callers must supply zero discharge for dry
 * states. No concentration clamp or positivity correction is performed here.
 *
 * @param[in] L Finite left reconstructed face state; area ft2, discharge ft3/s, velocity/celerity
 *     ft/s and integral ft3.
 * @param[in] R Finite right reconstructed state in the same aggregate geometry and left-to-right
 *     orientation.
 * @param[in] f Hydraulic FaceFlux computed from the same L/R states; required for consistent
 *     water/scalar transport.
 * @param[in] phi_l Finite left scalar concentration in the caller chosen species units.
 * @param[in] phi_r Finite right concentration in the same units.
 * @param[in] hllc True selects contact-speed upwind times f.mass; false uses the HLL scalar fan.
 * @return Scalar flux in concentration-units*ft3/s, positive left-to-right.
 */
OPENSWMM_KERNEL_FN double speciesFlux(const FaceState& L, const FaceState& R, const FaceFlux& f,
                                      double phi_l, double phi_r, bool hllc) noexcept {
    if (hllc)
        return f.mass * ((f.sstar >= 0.0) ? phi_l : phi_r);

    // HLL on the A(phi) component, with the same supersonic branches as the
    // hydrodynamic flux so the two can never disagree about which side is
    // upstream of the whole fan.
    if (f.sl >= 0.0)
        return L.q * phi_l;
    if (f.sr <= 0.0)
        return R.q * phi_r;
    const double dsr = f.sr - f.sl;
    return (f.sr * L.q * phi_l - f.sl * R.q * phi_r + f.sl * f.sr * (R.a * phi_r - L.a * phi_l)) /
           dsr;
}

// ===========================================================================
// Source terms
// ===========================================================================

/**
 * @brief Apply a semi-implicit Manning discharge sink.
 *
 * For valid nonnegative inputs the denominator is >=1 and does not reverse flow. No finite check or
 * timestep limiter is applied.
 *
 * @param[in] q Candidate aggregate discharge in ft3/s, either sign.
 * @param[in] u Conveyance velocity in ft/s; magnitude determines resistance.
 * @param[in] r Per-barrel hydraulic radius in ft; <=0 disables the update.
 * @param[in] dt Nonnegative timestep in seconds.
 * @param[in] rough_factor Precomputed 32.2*(n/1.486)^2 in native Manning units, including any
 *     lengthening adjustment; <=0 disables the update.
 * @return q/(1+dt*rough_factor*abs(u)/r^(4/3)) in ft3/s.
 */
OPENSWMM_KERNEL_FN double frictionUpdate(double q, double u, double r, double dt,
                                         double rough_factor) noexcept {
    if (r <= 0.0 || rough_factor <= 0.0)
        return q;
    const double r43 = r * std::cbrt(r);
    const double den = 1.0 + dt * rough_factor * std::fabs(u) / r43;
    return q / den;
}

/**
 * @brief Apply a semi-implicit entrance/exit momentum loss over an end cell.
 *
 * @param[in] q Candidate aggregate discharge in ft3/s.
 * @param[in] u Conveyance velocity in ft/s.
 * @param[in] k Dimensionless local-loss coefficient; <=0 leaves q unchanged.
 * @param[in] dx Positive cell length in ft; <=0 leaves q unchanged.
 * @param[in] dt Nonnegative timestep in seconds.
 * @return q/(1+dt*k*abs(u)/(2*dx)) in ft3/s.
 */
OPENSWMM_KERNEL_FN double localLossUpdate(double q, double u, double k, double dx,
                                          double dt) noexcept {
    if (k <= 0.0 || dx <= 0.0)
        return q;
    return q / (1.0 + dt * k * std::fabs(u) / (2.0 * dx));
}

/**
 * @brief Apply the bounded unsteady-friction acceleration correction.
 *
 * Returns q unchanged if both old and candidate velocities are below 0.01 ft/s. Otherwise folds
 * local acceleration implicitly, adds the explicit gradient term, then caps the total discharge
 * change to half abs(q). q==0 therefore remains zero. No neighboring state is read or changed by
 * this scalar kernel.
 *
 * @param[in] q Candidate aggregate discharge in ft3/s.
 * @param[in] a Aggregate flow area in ft2; <=0 disables the update.
 * @param[in] u_old Consistent old-snapshot velocity in ft/s.
 * @param[in] k3 Nonnegative dimensionless unsteady-friction coefficient; <=0 disables the update.
 * @param[in] grad_term Old-snapshot celerity*sign(u_old)*abs(dV/dx), in ft/s2.
 * @param[in] dt Nonnegative substep duration in seconds.
 * @return Corrected aggregate discharge in ft3/s.
 */
OPENSWMM_KERNEL_FN double ufUpdate(double q, double a, double u_old, double k3, double grad_term,
                                   double dt) noexcept {
    if (k3 <= 0.0 || a <= 0.0)
        return q;
    // Dead-band (measured, issue #156): the implicit fold acts as added
    // inertia — it resists velocity DECAY too, so applied to the ~mm/s
    // numerical ripple of a storage-coupled pool it sustained noise that
    // steady friction was correctly killing (0.004 → 0.011 cfs on the
    // at-rest fixture). UF correlations are calibrated for real transients
    // (paper velocities O(0.1–1 m/s)); below 0.01 ft/s the term is noise
    // amplification, not physics. Both the old and candidate velocities must
    // clear the floor, so a genuinely at-rest deck stays bit-identical.
    constexpr double kUfVelFloor = 0.01; // ft/s, internal units
    if (std::fabs(u_old) < kUfVelFloor && std::fabs(q / a) < kUfVelFloor)
        return q;
    double qn = (q + k3 * a * u_old) / (1.0 + k3);
    qn -= dt * k3 * a * grad_term;
    const double dq = qn - q;
    const double cap = 0.5 * std::fabs(q);
    if (std::fabs(dq) > cap)
        qn = q + ((dq > 0.0) ? cap : -cap);
    return qn;
}

// ---------------------------------------------------------------------------
// TPA — two-component pressure approach (issue #156 Phase 4)
// ---------------------------------------------------------------------------
// Vasconcelos, Wright & Roe (2006). For a cell whose regime FLAG is set
// (state.cell_tpa — physical air-pathway history, updated once per substep by
// the solver, NOT evaluated here), the closure is the slot line extended to
// BOTH signs of ΔA = A − a_crown:
//     h(A) = y_full + (A − a_crown)/t_slot        (hs = h − y_full, signed)
//     A(h) = a_crown + t_slot·(h − y_full)
//     I₁(h) = i1_crown + a_crown·(h − y_full)     (paper Eq. 12b: the slot's
//                                                  ½·t_slot·d² numerical
//                                                  storage pressure is dropped)
//     T = t_slot,  R = r_full,  c = √(g·A/t_slot) ≈ acoustic celerity a.
// Free-surface (unflagged) cells use the table closure above UNCHANGED,
// including the crown taper. The pair is continuous at A = a_crown, h = y_full.

/**
 * @brief Evaluate the signed-pressure TPA area-to-head extension.
 *
 * Caller must select TPA-flagged closed cells and enforce admissible state; regime history is not
 * read here.
 *
 * @param[in] g Borrowed initialized geometry with valid closure table, positive full depth/slot
 *     width and monotone samples; no pointer is retained.
 * @param[in] a Finite aggregate area in ft2; no dry or lower-area clamp.
 * @return y_full+(a-a_crown)/t_slot in ft; may lie below the crown or invert.
 */
OPENSWMM_KERNEL_FN double tpaDepthOfArea(const FvGeometry& g, double a) noexcept {
    return g.y_full + (a - g.a_crown) / g.t_slot;
}

/**
 * @brief Evaluate aggregate TPA area on the slot line in either pressure direction.
 *
 * @param[in] g Borrowed initialized geometry with valid closure table, positive full depth/slot
 *     width and monotone samples; no pointer is retained.
 * @param[in] h Finite piezometric depth in ft; no invert/crown clamp.
 * @return a_crown+t_slot*(h-y_full) in ft2; can become negative for inadmissibly low head.
 */
OPENSWMM_KERNEL_FN double tpaAreaOfDepth(const FvGeometry& g, double h) noexcept {
    return g.a_crown + g.t_slot * (h - g.y_full);
}

/**
 * @brief Evaluate the TPA pressure integral without numerical slot-storage pressure.
 *
 * Intentionally omits the quadratic slot-area pressure term; this is not the integral of
 * tpaAreaOfDepth.
 *
 * @param[in] g Borrowed initialized geometry with valid closure table, positive full depth/slot
 *     width and monotone samples; no pointer is retained.
 * @param[in] h Finite signed piezometric depth in ft; no clamp.
 * @return i1_crown+a_crown*(h-y_full) in aggregate ft3.
 */
OPENSWMM_KERNEL_FN double tpaI1OfDepth(const FvGeometry& g, double h) noexcept {
    return g.i1_crown + g.a_crown * (h - g.y_full);
}

/**
 * @brief Compute a local advective/acoustic Courant timestep bound.
 *
 * @param[in] cfl Positive dimensionless Courant factor.
 * @param[in] dx Positive characteristic length in ft.
 * @param[in] u Finite advective velocity in ft/s.
 * @param[in] c Finite nonnegative wave celerity in ft/s.
 * @return cfl*dx/(abs(u)+c) in seconds when the speed exceeds 1e-12 ft/s, otherwise 1e30 seconds.
 */
OPENSWMM_KERNEL_FN double faceCflDt(double cfl, double dx, double u, double c) noexcept {
    const double s = std::fabs(u) + c;
    return (s > 1.0e-12) ? cfl * dx / s : 1.0e30;
}

/**
 * @brief Limit exported substep volume to the available donor water.
 *
 * Returns one if outflow*dt<=max(vol,0), otherwise max(vol,0)/(outflow*dt). Caller must apply the
 * identical scaled face flux to both incident ledgers to preserve conservation; this function does
 * not mutate either state.
 *
 * @param[in] vol Finite available donor volume in ft3; negative volume is treated as zero.
 * @param[in] outflow Sum of outgoing volume rates in ft3/s; <=0 returns one.
 * @param[in] dt Substep duration in seconds; <=0 returns one.
 * @return Dimensionless factor in [0,1] for finite physical inputs.
 */
OPENSWMM_KERNEL_FN double positivityScale(double vol, double outflow, double dt) noexcept {
    if (outflow <= 0.0 || dt <= 0.0)
        return 1.0;
    const double take = outflow * dt;
    const double avail = (vol > 0.0) ? vol : 0.0;
    return (take <= avail) ? 1.0 : avail / take;
}

} // namespace openswmm::fv::kernels

#endif // OPENSWMM_ENGINE_FV_KERNELS_HPP
