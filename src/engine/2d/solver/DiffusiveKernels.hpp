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
 * @file DiffusiveKernels.hpp
 * @brief Face law and step bound for MOMENTUM_EQUATION DIFFUSIVE_WAVE
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_DIFFUSIVE_KERNELS_HPP
#define OPENSWMM_ENGINE_2D_DIFFUSIVE_KERNELS_HPP

#include <algorithm>
#include <cmath>

#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::twoD::diffusive {

/// Manning quasi-steady face discharge (m²/s, positive L→R) for face depth
/// hf, surface slope S = (η_R − η_L)·inv_dx (positive ⇒ flow R→L), face
/// Manning n_f and slope floor s_eps.
OPENSWMM_KERNEL_FN double faceDischarge(double hf, double slope, double nf,
                                        double s_eps) noexcept {
    if (!(hf > 0.0) || !(nf > 0.0) || slope == 0.0) return 0.0;
    const double h53 = hf * std::cbrt(hf * hf);
    const double sa  = std::fabs(slope);
    const double sr  = std::sqrt((sa > s_eps) ? sa : s_eps);
    return -h53 * slope / (nf * sr);
}

/// Explicit-diffusion step bound for a cell of characteristic length L,
/// depth h, Manning n and the largest face slope |S| among its faces (floored
/// at s_eps): Δt = α · L² · n · √S / (4 · h^{5/3}). Returns a huge value for a
/// dry cell.
OPENSWMM_KERNEL_FN double cellDiffusiveDt(double alpha, double lchar, double h,
                                          double n, double slope_abs,
                                          double s_eps) noexcept {
    if (!(h > 0.0)) return 1.0e30;
    const double h53 = h * std::cbrt(h * h);
    const double s   = (slope_abs > s_eps) ? slope_abs : s_eps;
    return alpha * lchar * lchar * n * std::sqrt(s) / (4.0 * h53);
}

} // namespace openswmm::twoD::diffusive

#endif // OPENSWMM_ENGINE_2D_DIFFUSIVE_KERNELS_HPP
