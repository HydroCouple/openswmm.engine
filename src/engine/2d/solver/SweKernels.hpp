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
 * @file      SweKernels.hpp
 * @brief     Face kernels for MOMENTUM_EQUATION FULL_SWE — the conservative
 *        shallow-water equations with the convective term and shock capturing
 *        (plans/2D_FULL_SWE_SHOCK_CAPTURING_PLAN_2026-09-05.md §2.2).
 *
 * @details Cell state (h, q⃗ = h·u⃗) per cell; a face between cells L and R
 *          with unit normal n̂ (L→R) evaluates one Godunov flux:
 *
 *            1. Hydrostatic reconstruction (Audusse et al. 2004) against the
 *               face bed z_f = max(z_L, z_R), z_side = η_side − h_side:
 *                 h*_L = max(0, η_L − z_f),  h*_R = max(0, η_R − z_f)
 *            2. Rotate the velocities into the (n̂, t̂) frame, t̂ = (−n_y, n_x).
 *            3. HLLC (Toro 2001) on U = [h, h·u_n, h·u_t]: HLL for mass and
 *               normal momentum, tangential momentum upwinded on the contact
 *               speed S*. Wave speeds from the two-rarefaction estimate with
 *               the dry-bed limits.
 *            4. Rotate the momentum flux back to (x, y).
 *            5. Bed-slope source per side, ½·g·(h*² − h²)·n̂ (the Audusse
 *               correction): at rest the HLLC flux is [0, ½g h*², 0] and the
 *               correction cancels it face by face, so a lake at rest over any
 *               bed is an exact steady state (C-property).
 *
 *          Friction is applied per cell after the flux update, semi-implicit
 *          (the same division the local-inertial face law uses), which can
 *          only shrink |q⃗| and never reverses it (Liang & Marche 2009's
 *          stopping condition is satisfied by construction).
 *
 *          Everything is plain scalar arithmetic over raw values so the same
 *          bodies can be annotated for Kokkos later (OPENSWMM_KERNEL_FN).
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SWE_KERNELS_HPP
#define OPENSWMM_ENGINE_2D_SWE_KERNELS_HPP

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::twoD::swe {

inline constexpr double kGravity = 9.80665;

/// True when @p x is neither NaN nor infinite.
/// @note Not std::isfinite: with the MSVC standard library it is a host-only template, and
/// nvcc then treats the device call as unreachable and prunes everything after it.
OPENSWMM_KERNEL_FN bool isFinite(double x) noexcept {
    return std::fabs(x) <= std::numeric_limits<double>::max();
}

/// One side of a face in the face-normal frame after hydrostatic
/// reconstruction. `h` is the reconstructed depth h*, (un, ut) the normal and
/// tangential velocity components (zero when the cell is dry).
template <class Real> struct FaceSideT {
    Real h = 0.0;
    Real un = 0.0;
    Real ut = 0.0;
};
using FaceSide = FaceSideT<double>;

/// Flux through the face, positive L→R: mass (m²/s), momentum in the x/y
/// frame (m³/s²). `sstar` is the HLLC contact speed used to upwind passive
/// scalars (species ride the mass flux from the side S* points away from).
template <class Real> struct FaceFluxT {
    Real mass = 0.0;
    Real mx = 0.0;
    Real my = 0.0;
    Real sstar = 0.0;
};
using FaceFlux = FaceFluxT<double>;

/**
 * @brief Physical flux of the SWE in the normal frame: [h·un, h·un² + ½g h², h·un·ut].
 *
 * @param[in] s Borrowed finite reconstructed face state: nonnegative h in m and
 * normal/tangential velocities in m/s.
 * @param[out] fh Mass/unit-width volume flux in m2/s.
 * @param[out] fn Normal momentum flux in m3/s2.
 * @param[out] ft Tangential momentum flux in m3/s2.
 * @note Pure scalar host/device evaluation; outputs must be distinct and must not alias input
 * fields. No validation, allocation or state mutation.
 */
template <class Real>
OPENSWMM_KERNEL_FN void physicalFlux(const FaceSideT<Real>& s, Real& fh, Real& fn,
                                     Real& ft) noexcept {
    fh = s.h * s.un;
    fn = s.h * s.un * s.un + Real(0.5) * Real(kGravity) * s.h * s.h;
    ft = s.h * s.un * s.ut;
}

/**
 * @brief Wave-speed estimates S_L ≤ S_R (Toro 2001 §10.4): two-rarefaction estimate for wet–wet
 * faces, the exact dry-bed front speeds otherwise.
 *
 * @param[in] L Borrowed finite reconstructed face state: nonnegative h in m and
 * normal/tangential velocities in m/s.
 * @param[in] R Borrowed finite reconstructed face state: nonnegative h in m and
 * normal/tangential velocities in m/s.
 * @param[out] sl Estimated leftmost wave speed, m/s.
 * @param[out] sr Estimated rightmost wave speed, m/s.
 * @note Both-dry faces return zero speeds. Caller supplies distinct output scalars and valid
 * finite depths; no allocation, mutation or robust-predicate error status.
 */
template <class Real>
OPENSWMM_KERNEL_FN void waveSpeeds(const FaceSideT<Real>& L, const FaceSideT<Real>& R, Real& sl,
                                   Real& sr) noexcept {
    const Real cl = std::sqrt(Real(kGravity) * L.h);
    const Real cr = std::sqrt(Real(kGravity) * R.h);
    if (L.h > Real(0.0) && R.h > Real(0.0)) {
        const Real ustar = Real(0.5) * (L.un + R.un) + cl - cr;
        const Real cstar = Real(0.5) * (cl + cr) + Real(0.25) * (L.un - R.un);
        sl = std::min(L.un - cl, ustar - cstar);
        sr = std::max(R.un + cr, ustar + cstar);
    } else if (L.h > Real(0.0)) { // dry right
        sl = L.un - cl;
        sr = L.un + Real(2.0) * cl;
    } else if (R.h > Real(0.0)) { // dry left
        sl = R.un - Real(2.0) * cr;
        sr = R.un + cr;
    } else {
        sl = sr = Real(0.0);
    }
}

/**
 * @brief HLLC flux in the face-normal frame.
 *
 * @param[in] L Borrowed finite reconstructed face state: nonnegative h in m and
 * normal/tangential velocities in m/s.
 * @param[in] R Borrowed finite reconstructed face state: nonnegative h in m and
 * normal/tangential velocities in m/s.
 * @param[out] fh Mass flux in m2/s, positive left-to-right.
 * @param[out] fn Normal momentum flux in m3/s2.
 * @param[out] ft Tangential momentum flux in m3/s2.
 * @param[out] sstar Contact-wave speed in m/s for passive-scalar donor selection.
 * @note All outputs are assigned, including zero for both-dry faces; degenerate contact
 * denominator falls back to mean normal velocity. Distinct outputs and finite nonnegative depths
 * are required. No allocation or model-state mutation.
 */
template <class Real>
OPENSWMM_KERNEL_FN void hllcFlux(const FaceSideT<Real>& L, const FaceSideT<Real>& R, Real& fh,
                                 Real& fn, Real& ft, Real& sstar) noexcept {
    if (L.h <= Real(0.0) && R.h <= Real(0.0)) {
        fh = fn = ft = Real(0.0);
        sstar = Real(0.0);
        return;
    }
    Real sl, sr;
    waveSpeeds(L, R, sl, sr);
    Real fhl, fnl, ftl, fhr, fnr, ftr;
    // Contact speed (Toro Eq. 10.70) — falls back to the mean normal
    // velocity when the denominator degenerates (both sides at rest).
    const Real dl = L.h * (L.un - sl), dr = R.h * (R.un - sr);
    const Real den = dr - dl;
    sstar =
        (std::fabs(den) > Real(1.0e-14)) ? (sl * dr - sr * dl) / den : Real(0.5) * (L.un + R.un);

    if (sl >= Real(0.0)) {
        physicalFlux(L, fhl, fnl, ftl);
        fh = fhl;
        fn = fnl;
        ft = ftl;
    } else if (sr <= Real(0.0)) {
        physicalFlux(R, fhr, fnr, ftr);
        fh = fhr;
        fn = fnr;
        ft = ftr;
    } else {
        // HLL middle state for mass and normal momentum.
        physicalFlux(L, fhl, fnl, ftl);
        physicalFlux(R, fhr, fnr, ftr);
        const Real inv = Real(1.0) / (sr - sl);
        fh = (sr * fhl - sl * fhr + sl * sr * (R.h - L.h)) * inv;
        fn = (sr * fnl - sl * fnr + sl * sr * (R.h * R.un - L.h * L.un)) * inv;
        // Tangential momentum: passive scalar upwinded on the contact.
        ft = fh * ((sstar >= Real(0.0)) ? L.ut : R.ut);
    }
}

/**
 * @brief Full face evaluation: hydrostatic reconstruction, rotation, HLLC, rotation back, and
 * the per-side bed-slope corrections.
 *
 * @return true when the face carries flux OR a bed-pressure correction. A blocked wet/dry face
 * still supplies the wet cell's wall pressure. false ⇒ every output is zero.
 *
 * @param[in] etaL Finite left cell free surface, m.
 * @param[in] hL Finite nonnegative left mean depth, m.
 * @param[in] qxL Finite left X unit-width discharge, m2/s.
 * @param[in] qyL Finite left Y unit-width discharge, m2/s.
 * @param[in] etaR Finite right cell free surface, m.
 * @param[in] hR Finite nonnegative right mean depth, m.
 * @param[in] qxR Finite right X unit-width discharge, m2/s.
 * @param[in] qyR Finite right Y unit-width discharge, m2/s.
 * @param[in] nx Finite X component of a unit normal directed left-to-right.
 * @param[in] ny Finite Y component of the same normal.
 * @param[in] h_dry Finite nonnegative depth threshold in m for shoreline velocity suppression.
 * @param[out] out Completely assigned Cartesian FaceFlux: mass m2/s, momentum m3/s2, sstar m/s.
 * @param[out] corrL_x Left-side X bed-source correction, m3/s2 per unit face length.
 * @param[out] corrL_y Left-side Y bed-source correction in the same units.
 * @param[out] corrR_x Right-side X bed-source correction with opposite outward normal, m3/s2.
 * @param[out] corrR_y Right-side Y bed-source correction, m3/s2.
 * @return True for a face flux or nonzero bed-pressure correction; false only when every output
 * is zero.
 * @note All outputs are assigned; keep outputs distinct and independent of input fields. Pure
 * host/device function; no validation, ownership transfer, allocation or timestep update.
 * Multiply momentum/source terms by face length and dt in the caller.
 */
template <class Real>
OPENSWMM_KERNEL_FN bool faceFlux(std::type_identity_t<Real> etaL, std::type_identity_t<Real> hL,
                                 std::type_identity_t<Real> qxL, std::type_identity_t<Real> qyL,
                                 std::type_identity_t<Real> etaR, std::type_identity_t<Real> hR,
                                 std::type_identity_t<Real> qxR, std::type_identity_t<Real> qyR,
                                 std::type_identity_t<Real> nx, std::type_identity_t<Real> ny,
                                 std::type_identity_t<Real> h_dry, FaceFluxT<Real>& out,
                                 Real& corrL_x, Real& corrL_y, Real& corrR_x,
                                 Real& corrR_y) noexcept {
    // Beds consistent with the storage: z = η − h̄ (FLAT closure: the cell
    // mean bed; VFR: the flat-equivalent bed, which is what keeps a uniform η
    // an exact rest state under either closure).
    const Real zL = etaL - hL, zR = etaR - hR;
    const Real zf = (zL > zR) ? zL : zR;
    FaceSideT<Real> L, R;
    L.h = (etaL - zf > Real(0.0)) ? etaL - zf : Real(0.0);
    R.h = (etaR - zf > Real(0.0)) ? etaR - zf : Real(0.0);
    // A side whose reconstructed depth is negligible carries no velocity
    // (avoids q/h blow-ups at fronts); the hydrostatic reconstruction alone
    // already removes the "dry cell standing higher" flux.
    const Real tx = -ny, ty = nx;
    if (hL > h_dry && L.h > Real(0.0)) {
        const Real ux = qxL / hL, uy = qyL / hL;
        L.un = ux * nx + uy * ny;
        L.ut = ux * tx + uy * ty;
    }
    if (hR > h_dry && R.h > Real(0.0)) {
        const Real ux = qxR / hR, uy = qyR / hR;
        R.un = ux * nx + uy * ny;
        R.ut = ux * tx + uy * ty;
    }
    // Hydrostatic pressure remains on the wet side of a blocked face.
    // Dropping it when BOTH reconstructed depths vanish accelerates a lake
    // away from its shoreline even though no water can cross this face.
    const Real cL = Real(0.5) * Real(kGravity) * (L.h * L.h - hL * hL);
    const Real cR = Real(0.5) * Real(kGravity) * (R.h * R.h - hR * hR);
    corrL_x = cL * nx;
    corrL_y = cL * ny;
    corrR_x = -cR * nx;
    corrR_y = -cR * ny;
    if (L.h <= Real(0.0) && R.h <= Real(0.0)) {
        out = FaceFluxT<Real>{};
        return cL != Real(0.0) || cR != Real(0.0);
    }
    Real fh, fn, ft, sstar;
    hllcFlux(L, R, fh, fn, ft, sstar);
    out.mass = fh;
    out.mx = fn * nx + ft * tx;
    out.my = fn * ny + ft * ty;
    out.sstar = sstar;
    return true;
}

/**
 * @brief Second-order face flux with independently reconstructed depth/bed.
 *
 * zLf/zRf are face beds from the limited eta and depth reconstruction; zL/zR are the
 * cell-equivalent beds. Add the hydrostatic face correction and the centered bed contribution
 * -g*(h_cell+h_face)/2*(z_face-z_cell). The latter integrates a linear depth along the
 * cell-to-face bed segment. For constant eta this reduces to g/2*(h_star^2-h_cell^2), preserving
 * lake-at-rest balance even next to a first-order cell. On a flat bed the within-cell
 * contribution vanishes. Face-depth positivity and cell-centered shoreline velocity are enforced
 * by computeLimitedGradientsSwe().
 *
 * @param[in] etaLf Finite reconstructed left face free surface, m.
 * @param[in] uxLf Finite reconstructed left X velocity, m/s.
 * @param[in] uyLf Finite reconstructed left Y velocity, m/s.
 * @param[in] zL Finite left cell-equivalent bed elevation, m.
 * @param[in] hL_cell Finite nonnegative left cell mean depth, m.
 * @param[in] etaRf Finite reconstructed right face free surface, m.
 * @param[in] uxRf Finite reconstructed right X velocity, m/s.
 * @param[in] uyRf Finite reconstructed right Y velocity, m/s.
 * @param[in] zR Finite right cell-equivalent bed elevation, m.
 * @param[in] hR_cell Finite nonnegative right cell mean depth, m.
 * @param[in] nx Finite X component of a unit normal directed left-to-right.
 * @param[in] ny Finite Y component of the same normal.
 * @param[in] h_dry Finite nonnegative depth threshold in m for shoreline velocity suppression.
 * @param[out] out Completely assigned Cartesian FaceFlux: mass m2/s, momentum m3/s2, sstar m/s.
 * @param[out] corrL_x Left-side X bed-source correction, m3/s2 per unit face length.
 * @param[out] corrL_y Left-side Y bed-source correction in the same units.
 * @param[out] corrR_x Right-side X bed-source correction with opposite outward normal, m3/s2.
 * @param[out] corrR_y Right-side Y bed-source correction, m3/s2.
 * @param[in] zLf Finite reconstructed left face bed elevation, m.
 * @param[in] zRf Finite reconstructed right face bed elevation, m.
 * @return True for a face flux or nonzero bed-pressure correction; false only when every output
 * is zero.
 * @note All outputs are assigned; keep outputs distinct and independent of input fields. Pure
 * host/device function; no validation, ownership transfer, allocation or timestep update.
 * Multiply momentum/source terms by face length and dt in the caller.
 */
OPENSWMM_KERNEL_FN bool faceFluxReconBed(double etaLf, double uxLf, double uyLf, double zL,
                                         double hL_cell, double etaRf, double uxRf, double uyRf,
                                         double zR, double hR_cell, double nx, double ny,
                                         double h_dry, FaceFlux& out, double& corrL_x,
                                         double& corrL_y, double& corrR_x, double& corrR_y,
                                         double zLf, double zRf) noexcept {
    const double zf = (zLf > zRf) ? zLf : zRf;
    FaceSide L, R;
    L.h = (etaLf - zf > 0.0) ? etaLf - zf : 0.0;
    R.h = (etaRf - zf > 0.0) ? etaRf - zf : 0.0;
    const double tx = -ny, ty = nx;
    if (hL_cell > h_dry && L.h > 0.0) {
        L.un = uxLf * nx + uyLf * ny;
        L.ut = uxLf * tx + uyLf * ty;
    }
    if (hR_cell > h_dry && R.h > 0.0) {
        R.un = uxRf * nx + uyRf * ny;
        R.ut = uxRf * tx + uyRf * ty;
    }
    const double hLf = std::max(0.0, etaLf - zLf);
    const double hRf = std::max(0.0, etaRf - zRf);
    // Hydrostatic face correction plus a centered within-cell bed source.
    const double cL = 0.5 * kGravity * (L.h * L.h - hLf * hLf - (hL_cell + hLf) * (zLf - zL));
    const double cR = 0.5 * kGravity * (R.h * R.h - hRf * hRf - (hR_cell + hRf) * (zRf - zR));
    corrL_x = cL * nx;
    corrL_y = cL * ny;
    corrR_x = -cR * nx;
    corrR_y = -cR * ny;
    if (L.h <= 0.0 && R.h <= 0.0) {
        out = FaceFlux{};
        return cL != 0.0 || cR != 0.0;
    }
    double fh, fn, ft, sstar;
    hllcFlux(L, R, fh, fn, ft, sstar);
    out.mass = fh;
    out.mx = fn * nx + ft * tx;
    out.my = fn * ny + ft * ty;
    out.sstar = sstar;
    return true;
}

/**
 * @brief Piecewise-constant-bed compatibility entry point.
 *
 * @param[in] etaLf Finite reconstructed left face free surface, m.
 * @param[in] uxLf Finite reconstructed left X velocity, m/s.
 * @param[in] uyLf Finite reconstructed left Y velocity, m/s.
 * @param[in] zL Finite left cell-equivalent bed elevation, m.
 * @param[in] hL_cell Finite nonnegative left cell mean depth, m.
 * @param[in] etaRf Finite reconstructed right face free surface, m.
 * @param[in] uxRf Finite reconstructed right X velocity, m/s.
 * @param[in] uyRf Finite reconstructed right Y velocity, m/s.
 * @param[in] zR Finite right cell-equivalent bed elevation, m.
 * @param[in] hR_cell Finite nonnegative right cell mean depth, m.
 * @param[in] nx Finite X component of a unit normal directed left-to-right.
 * @param[in] ny Finite Y component of the same normal.
 * @param[in] h_dry Finite nonnegative depth threshold in m for shoreline velocity suppression.
 * @param[out] out Completely assigned Cartesian FaceFlux: mass m2/s, momentum m3/s2, sstar m/s.
 * @param[out] corrL_x Left-side X bed-source correction, m3/s2 per unit face length.
 * @param[out] corrL_y Left-side Y bed-source correction in the same units.
 * @param[out] corrR_x Right-side X bed-source correction with opposite outward normal, m3/s2.
 * @param[out] corrR_y Right-side Y bed-source correction, m3/s2.
 * @return True for a face flux or nonzero bed-pressure correction; false only when every output
 * is zero.
 * @note All outputs are assigned; keep outputs distinct and independent of input fields. Pure
 * host/device function; no validation, ownership transfer, allocation or timestep update.
 * Multiply momentum/source terms by face length and dt in the caller. This compatibility wrapper
 * uses cell beds as face beds.
 */
OPENSWMM_KERNEL_FN bool faceFluxRecon(double etaLf, double uxLf, double uyLf, double zL,
                                      double hL_cell, double etaRf, double uxRf, double uyRf,
                                      double zR, double hR_cell, double nx, double ny, double h_dry,
                                      FaceFlux& out, double& corrL_x, double& corrL_y,
                                      double& corrR_x, double& corrR_y) noexcept {
    return faceFluxReconBed(etaLf, uxLf, uyLf, zL, hL_cell, etaRf, uxRf, uyRf, zR, hR_cell, nx, ny,
                            h_dry, out, corrL_x, corrL_y, corrR_x, corrR_y, zL, zR);
}

/**
 * @brief Barth–Jespersen limiter factor φ ∈ [0, 1] for one cell/variable given the cell value w,
 * the extrapolated face value wf and the min/max over the cell and its neighbours.
 *
 * @param[in] w Finite cell-centered variable in the variable's own units.
 * @param[in] wf Finite unlimited extrapolated face value in the same units.
 * @param[in] wmin Minimum over the cell and neighbors; must satisfy wmin <= w.
 * @param[in] wmax Maximum over the cell and neighbors; must satisfy w <= wmax.
 * @return Dimensionless limiter in [0,1] when the extrema bracket w; one when |wf-w| <= 1e-14.
 * The kernel does not clamp invalid negative ratios.
 * @note Pure host/device arithmetic: no allocation, exceptions, retained pointers or model state
 * mutation. Caller supplies finite valid physical inputs; no general validation.
 */
OPENSWMM_KERNEL_FN double bjLimiter(double w, double wf, double wmin, double wmax) noexcept {
    const double d = wf - w;
    if (d > 1.0e-14)
        return std::min(1.0, (wmax - w) / d);
    else if (d < -1.0e-14)
        return std::min(1.0, (wmin - w) / d);
    return 1.0;
}

/**
 * @brief Semi-implicit Manning friction on a cell's unit discharge over Δt: q⃗ ← q⃗ / (1 +
 * g·Δt·n²·|q⃗| / h^{7/3}). Only shrinks |q⃗|; never reverses it.
 *
 * @param[in,out] qx X unit-width discharge, m2/s.
 * @param[in,out] qy Y unit-width discharge, m2/s; distinct from qx.
 * @param[in] h Finite cell mean depth, m; nonpositive zeros both discharges.
 * @param[in] n Finite nonnegative Manning roughness, s/m^(1/3).
 * @param[in] dt Finite nonnegative update duration, s.
 * @note In-place pure host/device scalar update; semi-implicit denominator preserves direction
 * for valid inputs. No allocation, ledger update, or other model-state mutation.
 */
OPENSWMM_KERNEL_FN void frictionUpdate(double& qx, double& qy, double h, double n,
                                       double dt) noexcept {
    if (!(h > 0.0)) {
        qx = qy = 0.0;
        return;
    }
    const double qm = std::sqrt(qx * qx + qy * qy);
    if (qm == 0.0)
        return;
    const double h73 = h * h * std::cbrt(h);
    const double den = 1.0 + kGravity * dt * n * n * qm / h73;
    qx /= den;
    qy /= den;
}

/**
 * @brief Critical depth for a per-metre discharge q: (q²/g)^{1/3} — the ghost depth of a
 * prescribed-flow boundary entering a dry cell.
 *
 * @param[in] q Finite signed unit-width discharge, m2/s; sign is squared.
 * @return Nonnegative critical water depth, m; direct q*q requires caller-bounded magnitude.
 * @note Pure host/device arithmetic: no allocation, exceptions, retained pointers or model state
 * mutation. Caller supplies finite valid physical inputs; no general validation.
 */
OPENSWMM_KERNEL_FN double criticalDepth(double q) noexcept {
    return std::cbrt(q * q / kGravity);
}

/*!
 * \brief Ghost depth of a SUBCRITICAL prescribed-discharge boundary.
 *
 * One characteristic leaves the domain there, so its Riemann invariant
 * \p r = u + 2c is carried out from the interior and, with the prescribed
 * per-metre discharge \p q = u·h, fixes the boundary depth:
 *
 *     q/h + 2·sqrt(g·h) = r      ⇔      2·sqrt(g)·s³ − r·s² + q = 0,  s = sqrt(h)
 *
 * Newton on s from the interior depth with positive/finite-step guards (≤ 40
 * iterations); returns \p h_interior when it cannot converge — the caller's
 * mass flux is prescribed either way, so a fallback only costs accuracy in
 * the momentum flux, never conservation. Signs follow the outward normal:
 * an inflow has q < 0.
 */
/**
 * @param[in] r Finite outgoing normal Riemann invariant u+2*sqrt(g*h), m/s.
 * @param[in] q Finite prescribed normal unit-width discharge, m2/s; outward-positive, inflow
 * negative.
 * @param[in] h_interior Finite nonnegative interior depth/fallback, m.
 * @return Positive ghost depth in m when a physical Newton root passes convergence checks,
 * otherwise h_interior.
 * @note At most 40 Newton iterations with positive-step/finite-derivative guards, not a
 * bracketed root solve. Nonpositive interior depth starts the iteration at 1e-6 m. Pure
 * host/device computation; no separate failure code or retained state.
 */
OPENSWMM_KERNEL_FN double depthFromInvariantAndDischarge(double r, double q,
                                                         double h_interior) noexcept {
    const double rg = std::sqrt(kGravity);
    double s = std::sqrt(h_interior > 0.0 ? h_interior : 1.0e-6);
    for (int it = 0; it < 40; ++it) {
        const double f = 2.0 * rg * s * s * s - r * s * s + q;
        const double df = 6.0 * rg * s * s - 2.0 * r * s;
        if (!(std::fabs(df) > 1.0e-12))
            break;
        const double s1 = s - f / df;
        if (!(s1 > 0.0) || !isFinite(s1))
            break;
        const double step = std::fabs(s1 - s);
        s = s1;
        if (step < 1.0e-12 * (1.0 + s)) {
            const double h = s * s;
            return (h > 0.0 && isFinite(h)) ? h : h_interior;
        }
    }
    const double h = s * s;
    // Accept only a converged, physical root; otherwise keep the interior
    // depth (transmissive), which is what the pre-invariant code did.
    return (h > 0.0 && isFinite(h) &&
            std::fabs(2.0 * rg * s * s * s - r * s * s + q) < 1.0e-6 * (1.0 + std::fabs(q)))
               ? h
               : h_interior;
}

/// Result of swe::boundaryFlux for one boundary edge.
struct BoundaryFlux {
    double flow = 0.0; ///< requested volumetric flux into the cell, m³/s (inflow positive)
    double fmx = 0.0;  ///< x-momentum flux into the cell per metre of edge, m³/s²
    double fmy = 0.0;  ///< y-momentum flux into the cell per metre of edge, m³/s²
    bool wet = false;  ///< the Riemann solve ran; false leaves fmx/fmy at zero
};

/**
 * @brief FULL_SWE ghost-cell boundary: the ghost state for one edge type, the Riemann solve
 * against it and the prescribed-mass rule.
 *
 * @param[in] type Boundary type code: 0 wall, 1 normal flow, 2 specified stage, 3 specified
 * flow, 4 rating curve (the values of BoundaryType).
 * @param[in] length Finite positive edge length, m.
 * @param[in] nx Outward unit normal x component.
 * @param[in] ny Outward unit normal y component.
 * @param[in] eta_i Interior free surface, m.
 * @param[in] h_i Interior depth, m.
 * @param[in] qx_i Interior x unit discharge, m²/s.
 * @param[in] qy_i Interior y unit discharge, m²/s.
 * @param[in] n_man Interior Manning coefficient, s/m^(1/3) (normal flow).
 * @param[in] bed_slope Signed edge bed slope; positive drains the domain (normal flow).
 * @param[in] eta_bc Prescribed stage, m (specified stage).
 * @param[in] q_out Prescribed outward discharge per metre of edge, m²/s (flow, rating curve).
 * @param[in] conv Edge conveyance factor in [0, 1].
 * @param[in] dry Finite positive wet/dry depth threshold, m.
 * @return The requested inflow and, when the Riemann solve ran, the momentum flux the cell
 * receives per metre of edge. The caller scales the momentum by length·dt/area and applies its
 * own availability clamp to the mass.
 * @note Pure host/device arithmetic: no allocation, exceptions, retained pointers or model state
 * mutation. Caller supplies finite valid physical inputs; no general validation.
 */
OPENSWMM_KERNEL_FN BoundaryFlux boundaryFlux(int type, double length, double nx, double ny,
                                             double eta_i, double h_i, double qx_i, double qy_i,
                                             double n_man, double bed_slope, double eta_bc,
                                             double q_out, double conv, double dry) noexcept {
    const double L = length;
    const double z_i = eta_i - h_i;
    double u_n = 0.0, u_t = 0.0;
    if (h_i > dry) {
        const double ux = qx_i / h_i, uy = qy_i / h_i;
        u_n = ux * nx + uy * ny;
        u_t = -ux * ny + uy * nx;
    }
    // Ghost (h_g, u_ng, u_tg) in the outward-normal frame.
    double h_g = h_i, un_g = u_n, ut_g = u_t, eta_g = eta_i;
    // A prescribed-discharge boundary imposes its mass flux exactly; the
    // Riemann solve below then only supplies the momentum flux.
    bool prescribed_mass = false;
    double q_prescribed = 0.0; // outward-positive, per metre of edge
    switch (type) {
    case 0: // WALL
        un_g = -u_n;
        break;
    case 1: { // NORMAL_FLOW
        // Manning normal-flow outlet, the same law the local-inertial and
        // diffusive paths apply (SurfaceFluxCalculator::boundaryEdgeFlux):
        // per-metre outflow q = h^{5/3}·sqrt(S)/n. Under FULL_SWE this used
        // to be a bare zero-gradient ghost, i.e. the bed slope the modeller
        // set on the edge did nothing.
        // The slope is SIGNED: positive falls away from the domain and
        // drains it (unchanged), negative falls toward it and feeds it.
        // A negative slope cannot bootstrap a DRY cell — the law conveys
        // with the interior depth, and h_i = 0 carries nothing; use a
        // SPECIFIED_FLOW edge for dry-bed inflow, which reconstructs the
        // critical depth of its discharge.
        const double S = bed_slope;
        prescribed_mass = true;
        if (S != 0.0 && h_i > 0.0 && n_man > 0.0) {
            const double sgnS = (S > 0.0) ? 1.0 : -1.0;
            const double h53 = h_i * std::cbrt(h_i * h_i);
            q_prescribed = sgnS * h53 * std::sqrt(std::fabs(S)) / n_man; // outward +
            un_g = q_prescribed / h_i;
            ut_g = u_t;
        } else {
            // No slope, no depth, no roughness: the law conveys nothing.
            // The mirror ghost keeps the momentum consistent with that —
            // a bare zero-gradient ghost is an ABSORBING boundary and
            // drained a lake at rest through an inert outlet.
            q_prescribed = 0.0;
            un_g = -u_n;
        }
        break;
    }
    case 2: { // SPECIFIED_STAGE
        const double c_i = std::sqrt(kGravity * std::max(h_i, 0.0));
        if (h_i > dry && u_n >= c_i) {
            // Supercritical OUTFLOW: no characteristic enters the domain,
            // the prescribed stage cannot act — transmissive ghost.
            break;
        }
        eta_g = eta_bc;
        h_g = (eta_bc - z_i > 0.0) ? eta_bc - z_i : 0.0;
        // Subcritical: the outgoing Riemann invariant u_n + 2c fixes the
        // ghost velocity for the prescribed depth (characteristic
        // extrapolation), instead of a bare copy of the interior velocity.
        if (h_i > dry && h_g > 0.0)
            un_g = u_n + 2.0 * (c_i - std::sqrt(kGravity * h_g));
        // The invariant needs a characteristic leaving the domain. A
        // supercritical INFLOW has none, and the extrapolated ghost copied
        // the interior's inflow speed back so the stream fed itself (85 m³/s
        // at 12 m/s through a 0.5 m head on the complete 2D example). One
        // prescribed quantity admits at most a critical inflow at its depth.
        if (h_g > 0.0)
            un_g = std::max(un_g, -std::sqrt(kGravity * h_g));
        break;
    }
    case 3:   // SPECIFIED_FLOW
    case 4: { // RATING_CURVE
        // A prescribed discharge is a FLUX boundary: the mass flux is the
        // prescribed value (imposed below, as the local-inertial law does
        // in SurfaceFluxCalculator), not whatever a Riemann problem
        // against a ghost happens to deliver. The ghost still sets the
        // MOMENTUM flux and keeps the Audusse pressure balance, so its
        // depth should be the physical boundary depth:
        //   * dry cell            — critical depth (dry-bed inflow),
        //   * subcritical inflow  — from the outgoing invariant u + 2c
        //                           together with q (one characteristic
        //                           leaves, so depth is NOT free),
        //   * supercritical inflow / any outflow — the interior depth.
        if (h_i > dry) {
            h_g = h_i;
            if (q_out < 0.0) { // inflow
                const double c_i = std::sqrt(kGravity * h_i);
                if (std::fabs(u_n) < c_i) // subcritical
                    h_g = depthFromInvariantAndDischarge(u_n + 2.0 * c_i, q_out, h_i);
            }
            eta_g = z_i + h_g;
        } else {
            h_g = criticalDepth(std::fabs(q_out));
            eta_g = z_i + h_g;
        }
        un_g = (h_g > 0.0) ? q_out / h_g : 0.0;
        ut_g = 0.0;
        prescribed_mass = true;
        q_prescribed = q_out;
        break;
    }
    default:
        break;
    }
    // Interior = L, ghost = R, normal outward; velocities re-expressed as
    // (x, y) unit discharges for the shared kernel.
    const double qxg = h_g * (un_g * nx - ut_g * ny);
    const double qyg = h_g * (un_g * ny + ut_g * nx);
    FaceFlux F;
    double cLx, cLy, cRx, cRy;
    BoundaryFlux out;
    out.wet =
        faceFlux(eta_i, h_i, qx_i, qy_i, eta_g, h_g, qxg, qyg, nx, ny, dry, F, cLx, cLy, cRx, cRy);
    // A prescribed inflow must reach a DRY cell too — faceFlux reports "not
    // wet" there (both reconstructed depths vanish), and returning 0 would
    // make a dry-bed inflow boundary inert.
    if (!out.wet) {
        out.flow = prescribed_mass ? -q_prescribed * conv * L : 0.0;
        return out;
    }
    out.fmx = -F.mx * conv + cLx;
    out.fmy = -F.my * conv + cLy;
    // Mass: the prescribed discharge verbatim (the local-inertial law's
    // contract, SurfaceFluxCalculator::boundaryEdgeFlux) — the Riemann
    // solve's own mass flux is a wave-speed-weighted BLEND of the interior
    // and prescribed states, which under-delivered the SWASHES bump inflows
    // by 7-21 % and reversed a supercritical inlet outright.
    out.flow = prescribed_mass ? -q_prescribed * conv * L : -F.mass * conv * L;
    return out;
}

} // namespace openswmm::twoD::swe

#endif // OPENSWMM_ENGINE_2D_SWE_KERNELS_HPP
