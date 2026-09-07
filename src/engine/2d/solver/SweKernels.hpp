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
 * @file SweKernels.hpp
 * @brief Face kernels for MOMENTUM_EQUATION FULL_SWE — the conservative
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SWE_KERNELS_HPP
#define OPENSWMM_ENGINE_2D_SWE_KERNELS_HPP

#include <algorithm>
#include <cmath>

#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::twoD::swe {

inline constexpr double kGravity = 9.80665;

/// One side of a face in the face-normal frame after hydrostatic
/// reconstruction. `h` is the reconstructed depth h*, (un, ut) the normal and
/// tangential velocity components (zero when the cell is dry).
struct FaceSide {
    double h  = 0.0;
    double un = 0.0;
    double ut = 0.0;
};

/// Flux through the face, positive L→R: mass (m²/s), momentum in the x/y
/// frame (m³/s²). `sstar` is the HLLC contact speed used to upwind passive
/// scalars (species ride the mass flux from the side S* points away from).
struct FaceFlux {
    double mass = 0.0;
    double mx   = 0.0;
    double my   = 0.0;
    double sstar = 0.0;
};

/// Physical flux of the SWE in the normal frame: [h·un, h·un² + ½g h², h·un·ut].
OPENSWMM_KERNEL_FN void physicalFlux(const FaceSide& s, double& fh, double& fn,
                                     double& ft) noexcept {
    fh = s.h * s.un;
    fn = s.h * s.un * s.un + 0.5 * kGravity * s.h * s.h;
    ft = s.h * s.un * s.ut;
}

/// Wave-speed estimates S_L ≤ S_R (Toro 2001 §10.4): two-rarefaction
/// estimate for wet–wet faces, the exact dry-bed front speeds otherwise.
OPENSWMM_KERNEL_FN void waveSpeeds(const FaceSide& L, const FaceSide& R,
                                   double& sl, double& sr) noexcept {
    const double cl = std::sqrt(kGravity * L.h);
    const double cr = std::sqrt(kGravity * R.h);
    if (L.h > 0.0 && R.h > 0.0) {
        const double ustar = 0.5 * (L.un + R.un) + cl - cr;
        const double cstar = 0.5 * (cl + cr) + 0.25 * (L.un - R.un);
        sl = std::min(L.un - cl, ustar - cstar);
        sr = std::max(R.un + cr, ustar + cstar);
    } else if (L.h > 0.0) {            // dry right
        sl = L.un - cl;
        sr = L.un + 2.0 * cl;
    } else if (R.h > 0.0) {            // dry left
        sl = R.un - 2.0 * cr;
        sr = R.un + cr;
    } else {
        sl = sr = 0.0;
    }
}

/**
 * @brief HLLC flux in the face-normal frame.
 *
 * @param L,R  reconstructed side states (h*, un, ut)
 * @param fh   out: mass flux (m²/s), positive L→R
 * @param fn   out: normal-momentum flux (m³/s²)
 * @param ft   out: tangential-momentum flux (m³/s²)
 * @param sstar out: contact speed (m/s)
 */
OPENSWMM_KERNEL_FN void hllcFlux(const FaceSide& L, const FaceSide& R,
                                 double& fh, double& fn, double& ft,
                                 double& sstar) noexcept {
    if (L.h <= 0.0 && R.h <= 0.0) {
        fh = fn = ft = 0.0; sstar = 0.0;
        return;
    }
    double sl, sr;
    waveSpeeds(L, R, sl, sr);
    double fhl, fnl, ftl, fhr, fnr, ftr;
    physicalFlux(L, fhl, fnl, ftl);
    physicalFlux(R, fhr, fnr, ftr);

    // Contact speed (Toro Eq. 10.70) — falls back to the mean normal
    // velocity when the denominator degenerates (both sides at rest).
    const double dl = L.h * (L.un - sl), dr = R.h * (R.un - sr);
    const double den = dr - dl;
    sstar = (std::fabs(den) > 1.0e-14) ? (sl * dr - sr * dl) / den
                                       : 0.5 * (L.un + R.un);

    if (sl >= 0.0) {
        fh = fhl; fn = fnl; ft = ftl;
    } else if (sr <= 0.0) {
        fh = fhr; fn = fnr; ft = ftr;
    } else {
        // HLL middle state for mass and normal momentum.
        const double inv = 1.0 / (sr - sl);
        fh = (sr * fhl - sl * fhr + sl * sr * (R.h - L.h)) * inv;
        fn = (sr * fnl - sl * fnr + sl * sr * (R.h * R.un - L.h * L.un)) * inv;
        // Tangential momentum: passive scalar upwinded on the contact.
        ft = fh * ((sstar >= 0.0) ? L.ut : R.ut);
    }
}

/**
 * @brief Full face evaluation: hydrostatic reconstruction, rotation, HLLC,
 *        rotation back, and the per-side bed-slope corrections.
 *
 * @param etaL,hL,qxL,qyL   left cell free surface, depth, unit discharge
 * @param etaR,hR,qxR,qyR   right cell
 * @param nx,ny             unit normal L→R
 * @param h_dry             depth below which a cell's velocity is zero
 * @param out               flux (x/y frame), positive L→R
 * @param corrL_x,corrL_y   out: bed-slope correction for L, ½g(h*_L² − h_L²)·n̂
 *                          (per unit face length; multiply by ξ·Δt)
 * @param corrR_x,corrR_y   out: for R, ½g(h*_R² − h_R²)·(−n̂)
 * @return true when the face carries any flux (at least one side wet after
 *         reconstruction); false ⇒ every output is zero.
 */
OPENSWMM_KERNEL_FN bool faceFlux(double etaL, double hL, double qxL, double qyL,
                                 double etaR, double hR, double qxR, double qyR,
                                 double nx, double ny, double h_dry,
                                 FaceFlux& out,
                                 double& corrL_x, double& corrL_y,
                                 double& corrR_x, double& corrR_y) noexcept {
    // Beds consistent with the storage: z = η − h̄ (FLAT closure: the cell
    // mean bed; VFR: the flat-equivalent bed, which is what keeps a uniform η
    // an exact rest state under either closure).
    const double zL = etaL - hL, zR = etaR - hR;
    const double zf = (zL > zR) ? zL : zR;
    FaceSide L, R;
    L.h = (etaL - zf > 0.0) ? etaL - zf : 0.0;
    R.h = (etaR - zf > 0.0) ? etaR - zf : 0.0;
    // A side whose reconstructed depth is negligible carries no velocity
    // (avoids q/h blow-ups at fronts); the hydrostatic reconstruction alone
    // already removes the "dry cell standing higher" flux.
    const double tx = -ny, ty = nx;
    if (hL > h_dry && L.h > 0.0) {
        const double ux = qxL / hL, uy = qyL / hL;
        L.un = ux * nx + uy * ny;
        L.ut = ux * tx + uy * ty;
    }
    if (hR > h_dry && R.h > 0.0) {
        const double ux = qxR / hR, uy = qyR / hR;
        R.un = ux * nx + uy * ny;
        R.ut = ux * tx + uy * ty;
    }
    corrL_x = corrL_y = corrR_x = corrR_y = 0.0;
    if (L.h <= 0.0 && R.h <= 0.0) {
        out = FaceFlux{};
        return false;
    }
    double fh, fn, ft, sstar;
    hllcFlux(L, R, fh, fn, ft, sstar);
    out.mass  = fh;
    out.mx    = fn * nx + ft * tx;
    out.my    = fn * ny + ft * ty;
    out.sstar = sstar;
    // Audusse bed-slope corrections (½g(h*² − h²)·n̂_out per side).
    const double cL = 0.5 * kGravity * (L.h * L.h - hL * hL);
    const double cR = 0.5 * kGravity * (R.h * R.h - hR * hR);
    corrL_x =  cL * nx; corrL_y =  cL * ny;
    corrR_x = -cR * nx; corrR_y = -cR * ny;
    return true;
}

/**
 * @brief Face flux from RECONSTRUCTED side states (RECONSTRUCTION_ORDER 2).
 *
 * The free surface and velocities are the MUSCL-extrapolated face values and
 * the bed stays piecewise constant per cell (z_side = η_cell − h_cell); the
 * bed-slope correction uses the reconstructed face depth, so at rest (zero
 * gradients) the face reduces exactly to the first-order well-balanced form.
 *
 * @param etaLf,uxLf,uyLf   left face-extrapolated free surface and velocity
 * @param zL,hL_cell        left cell bed and cell-mean depth
 * @param etaRf,...         right side likewise
 */
OPENSWMM_KERNEL_FN bool faceFluxRecon(double etaLf, double uxLf, double uyLf,
                                      double zL, double hL_cell,
                                      double etaRf, double uxRf, double uyRf,
                                      double zR, double hR_cell,
                                      double nx, double ny, double h_dry,
                                      FaceFlux& out,
                                      double& corrL_x, double& corrL_y,
                                      double& corrR_x, double& corrR_y) noexcept {
    const double zf = (zL > zR) ? zL : zR;
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
    corrL_x = corrL_y = corrR_x = corrR_y = 0.0;
    if (L.h <= 0.0 && R.h <= 0.0) { out = FaceFlux{}; return false; }
    double fh, fn, ft, sstar;
    hllcFlux(L, R, fh, fn, ft, sstar);
    out.mass  = fh;
    out.mx    = fn * nx + ft * tx;
    out.my    = fn * ny + ft * ty;
    out.sstar = sstar;
    // Audusse et al. (2004) second order with a piecewise-constant bed per
    // cell: the correction pairs h* with the RECONSTRUCTED face depth
    // h⁻ = η_f − z_cell (the centred bed term vanishes), so the flux keeps the
    // interface pressure difference a linear surface implies. Reduces to the
    // first-order form when the gradients are zero (rest state exact).
    const double hLf = (etaLf - zL > 0.0) ? etaLf - zL : 0.0;
    const double hRf = (etaRf - zR > 0.0) ? etaRf - zR : 0.0;
    const double cL = 0.5 * kGravity * (L.h * L.h - hLf * hLf);
    const double cR = 0.5 * kGravity * (R.h * R.h - hRf * hRf);
    corrL_x =  cL * nx; corrL_y =  cL * ny;
    corrR_x = -cR * nx; corrR_y = -cR * ny;
    return true;
}

/// Barth–Jespersen limiter factor φ ∈ [0, 1] for one cell/variable given the
/// cell value w, the extrapolated face value wf and the min/max over the
/// cell and its neighbours.
OPENSWMM_KERNEL_FN double bjLimiter(double w, double wf, double wmin, double wmax) noexcept {
    const double d = wf - w;
    if (d > 1.0e-14)       return std::min(1.0, (wmax - w) / d);
    else if (d < -1.0e-14) return std::min(1.0, (wmin - w) / d);
    return 1.0;
}

/// Semi-implicit Manning friction on a cell's unit discharge over Δt:
/// q⃗ ← q⃗ / (1 + g·Δt·n²·|q⃗| / h^{7/3}). Only shrinks |q⃗|; never reverses it.
OPENSWMM_KERNEL_FN void frictionUpdate(double& qx, double& qy, double h, double n,
                                       double dt) noexcept {
    if (!(h > 0.0)) { qx = qy = 0.0; return; }
    const double qm = std::sqrt(qx * qx + qy * qy);
    if (qm == 0.0) return;
    const double h73 = h * h * std::cbrt(h);
    const double den = 1.0 + kGravity * dt * n * n * qm / h73;
    qx /= den;
    qy /= den;
}

/// Critical depth for a per-metre discharge q: (q²/g)^{1/3} — the ghost
/// depth of a prescribed-flow boundary entering a dry cell.
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
 * Safeguarded Newton on s from the interior depth (bracketed to s > 0, ≤ 40
 * iterations); returns \p h_interior when it cannot converge — the caller's
 * mass flux is prescribed either way, so a fallback only costs accuracy in
 * the momentum flux, never conservation. Signs follow the outward normal:
 * an inflow has q < 0.
 */
OPENSWMM_KERNEL_FN double depthFromInvariantAndDischarge(double r, double q,
                                                         double h_interior) noexcept {
    const double rg = std::sqrt(kGravity);
    double s = std::sqrt(h_interior > 0.0 ? h_interior : 1.0e-6);
    for (int it = 0; it < 40; ++it) {
        const double f  = 2.0 * rg * s * s * s - r * s * s + q;
        const double df = 6.0 * rg * s * s - 2.0 * r * s;
        if (!(std::fabs(df) > 1.0e-12)) break;
        const double s1 = s - f / df;
        if (!(s1 > 0.0) || !std::isfinite(s1)) break;
        const double step = std::fabs(s1 - s);
        s = s1;
        if (step < 1.0e-12 * (1.0 + s)) {
            const double h = s * s;
            return (h > 0.0 && std::isfinite(h)) ? h : h_interior;
        }
    }
    const double h = s * s;
    // Accept only a converged, physical root; otherwise keep the interior
    // depth (transmissive), which is what the pre-invariant code did.
    return (h > 0.0 && std::isfinite(h) &&
            std::fabs(2.0 * rg * s * s * s - r * s * s + q) < 1.0e-6 * (1.0 + std::fabs(q)))
               ? h : h_interior;
}

} // namespace openswmm::twoD::swe

#endif // OPENSWMM_ENGINE_2D_SWE_KERNELS_HPP
