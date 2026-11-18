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
 * @file      DiffusiveKernels.hpp
 * @brief     Face law and step bound for MOMENTUM_EQUATION DIFFUSIVE_WAVE
 *        (plans/2D_FULL_SWE_SHOCK_CAPTURING_PLAN_2026-09-05.md §2.3).
 *
 * @details The explicit diffusive wave of Hunter et al. (2005) /
 *          LISFLOOD-FP: no inertia, the face discharge is the Manning
 *          quasi-steady balance of the free-surface slope,
 *
 *            q_f = − h_f^{5/3} · S / (n_f · √max(|S|, S_ε)),   S = Δη·inv_dx
 *
 *          with the slope regularised below S_ε so the conductance
 *          h^{5/3}/(n√|S|) cannot diverge on a flat surface (the classic
 *          explicit-DW failure). The scheme is a nonlinear diffusion
 *          ∂h/∂t = ∇·(K∇η), K = h^{5/3}/(n√|S|), so the explicit step is
 *          bounded by Δt ≤ L²/(4K) — the Δx² restriction that the
 *          local-inertial term exists to remove. It is affordable here only
 *          because the tiered LTS lets each cell pay its own Δt.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_DIFFUSIVE_KERNELS_HPP
#define OPENSWMM_ENGINE_2D_DIFFUSIVE_KERNELS_HPP

#include <algorithm>
#include <cmath>

#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::twoD::diffusive {

/**
 * @brief Manning quasi-steady face discharge (m²/s, positive L→R) for face depth hf, surface
 * slope S = (η_R − η_L)·inv_dx (positive ⇒ flow R→L), face Manning n_f and slope floor s_eps.
 *
 * @param[in] hf Finite face flow depth, m; nonpositive returns zero.
 * @param[in] slope Signed dimensionless (eta_R-eta_L)/normal distance; positive drives
 * right-to-left.
 * @param[in] nf Finite positive Manning coefficient, s/m^(1/3); nonpositive returns zero.
 * @param[in] s_eps Finite positive dimensionless slope-magnitude floor.
 * @return Unit-width discharge in m2/s, positive left-to-right; exactly zero for zero slope.
 * @note Pure host/device arithmetic: no allocation, exceptions, retained pointers or model state
 * mutation. Caller supplies finite valid physical inputs; no general validation.
 */
OPENSWMM_KERNEL_FN double faceDischarge(double hf, double slope, double nf, double s_eps) noexcept {
    if (!(hf > 0.0) || !(nf > 0.0) || slope == 0.0)
        return 0.0;
    const double h53 = hf * std::cbrt(hf * hf);
    const double sa = std::fabs(slope);
    const double sr = std::sqrt((sa > s_eps) ? sa : s_eps);
    return -h53 * slope / (nf * sr);
}

/**
 * @brief Explicit-diffusion step bound for a cell of characteristic length L, depth h, Manning n
 * and the largest face slope |S| among its faces (floored at s_eps): Δt = α · L² · n · √S / (4 ·
 * h^{5/3}). Returns a huge value for a dry cell.
 *
 * @param[in] alpha Finite positive Courant/diffusion safety factor, dimensionless.
 * @param[in] lchar Finite positive cell characteristic length, m.
 * @param[in] h Finite nonnegative mean depth, m; nonpositive selects the dry sentinel.
 * @param[in] n Finite positive Manning coefficient, s/m^(1/3).
 * @param[in] slope_abs Finite nonnegative largest face-slope magnitude, dimensionless.
 * @param[in] s_eps Finite positive dimensionless slope floor.
 * @return Explicit diffusion step bound in seconds, or 1e30 s for a dry cell.
 * @note Pure host/device arithmetic: no allocation, exceptions, retained pointers or model state
 * mutation. Caller supplies finite valid physical inputs; no general validation.
 */
OPENSWMM_KERNEL_FN double cellDiffusiveDt(double alpha, double lchar, double h, double n,
                                          double slope_abs, double s_eps) noexcept {
    if (!(h > 0.0))
        return 1.0e30;
    const double h53 = h * std::cbrt(h * h);
    const double s = (slope_abs > s_eps) ? slope_abs : s_eps;
    return alpha * lchar * lchar * n * std::sqrt(s) / (4.0 * h53);
}

/**
 * @brief Square root regularized near zero: √x for x ≥ eps, a C¹ quadratic below it.
 *
 * @param[in] x Finite nonnegative argument.
 * @param[in] eps Regularization threshold; nonpositive selects the plain square root.
 * @return √x at or above eps, otherwise (1.5/√eps)·x − (0.5/(√eps·eps))·x².
 * @note Pure host/device arithmetic: no allocation, exceptions, retained pointers or model state
 * mutation. Caller supplies finite valid physical inputs; no general validation.
 */
OPENSWMM_KERNEL_FN double regSqrt(double x, double eps) noexcept {
    if (eps <= 0.0 || x >= eps)
        return std::sqrt(x);
    const double inv = 1.0 / std::sqrt(eps);
    return (1.5 * inv) * x - (0.5 * inv / eps) * x * x;
}

/**
 * @brief Collapsed-Manning discharge of a SPECIFIED_STAGE boundary edge (m³/s, inflow positive):
 * the interior diffusive operator with the ghost held at the prescribed stage.
 *
 * @param[in] h_up Finite positive upwind depth at the edge, m.
 * @param[in] dh Cell free surface minus prescribed stage, m; positive drains the cell.
 * @param[in] n Finite positive Manning coefficient, s/m^(1/3).
 * @param[in] dx_b Finite positive centroid-to-edge normal distance, m.
 * @param[in] length Finite nonnegative edge length, m.
 * @param[in] dh_eps Head-difference regularization threshold, m (FLUX_DH_EPS).
 * @return −h_up^{5/3}·sign(dh)·regSqrt(|dh|, dh_eps)·length / (n·√dx_b).
 * @note Pure host/device arithmetic: no allocation, exceptions, retained pointers or model state
 * mutation. Caller supplies finite valid physical inputs; no general validation.
 */
OPENSWMM_KERNEL_FN double stageBoundaryFlux(double h_up, double dh, double n, double dx_b,
                                            double length, double dh_eps) noexcept {
    const double h53 = h_up * std::cbrt(h_up * h_up);
    const double sign_dh = (dh > 0.0) ? 1.0 : (dh < 0.0 ? -1.0 : 0.0);
    return -h53 * sign_dh * regSqrt(std::fabs(dh), dh_eps) * length / (n * std::sqrt(dx_b));
}

} // namespace openswmm::twoD::diffusive

#endif // OPENSWMM_ENGINE_2D_DIFFUSIVE_KERNELS_HPP
