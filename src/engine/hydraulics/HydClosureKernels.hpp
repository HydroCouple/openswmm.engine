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
 * @file      HydClosureKernels.hpp
 * @brief     Portable scalar closures shared by the host solvers and the device
 *        backend: FHWA HEC-5 culvert inlet control and force-main friction.
 *
 * @details These are pure functions of scalars — no context, no allocation, no
 *          exceptions — and both the dynamic wave solver and the finite-volume
 *          solver already call them per element. The FV solver now calls them
 *          from inside its own substep loop (culvert inlet control caps the
 *          flux at the culvert's upstream face; a full force main obeys
 *          Hazen-Williams rather than the equivalent Manning n), which is
 *          exactly the position a device backend has to reach.
 *
 *          They therefore live here, marked with the same portable-kernel macro
 *          as FvKernels.hpp and XSectKernels.hpp, so the identical bodies
 *          compile for the host engine and for the Kokkos plugin. Culvert.cpp
 *          and ForceMain.cpp keep their public entry points and forward here —
 *          one implementation, not two, which is the whole point of the move.
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §5.1
 * @ingroup engine_hydraulics
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_HYD_CLOSURE_KERNELS_HPP
#define OPENSWMM_ENGINE_HYD_CLOSURE_KERNELS_HPP

#include <cmath>

// Same convention as InertialKernels.hpp:45 / FvKernels.hpp: host builds get
// plain `inline`; a device build defines this to KOKKOS_INLINE_FUNCTION before
// including the header.
#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::hydkernels {

// ===========================================================================
// Force-main friction
// ===========================================================================

/// Kinematic viscosity of water, ft²/s. Spelled here rather than included so
/// the header stays dependency-free for the plugin build; it must equal
/// constants::VISCOS, and a static_assert in ForceMain.cpp holds it to that.
inline constexpr double kViscosity = 1.1e-5;

/// Gravity in internal units, matching constants::GRAVITY.
inline constexpr double kGravity = 32.2;

/**
 * @brief Compute Hazen-Williams friction slope from velocity and hydraulic radius.
 *
 * Uses (abs(v)/(1.318*C*R^0.63))^(1/0.54); no general finite checks or source-direction sign.
 *
 * @param[in] velocity Signed ft/s velocity; formula uses magnitude.
 * @param[in] hyd_rad Positive hydraulic radius ft; <=0 returns0.
 * @param[in] c_hw Positive Hazen-Williams C coefficient; <=0 returns0.
 * @return Dimensionless nonnegative friction slope for finite physical inputs.
 */
OPENSWMM_KERNEL_FN double fricSlopeHW(double velocity, double hyd_rad, double c_hw) noexcept {
    if (c_hw <= 0.0 || hyd_rad <= 0.0)
        return 0.0;
    const double v_abs = (velocity < 0.0) ? -velocity : velocity;
    return std::pow(v_abs / (1.318 * c_hw * std::pow(hyd_rad, 0.63)), 1.0 / 0.54);
}

/**
 * @brief Compute Darcy-Weisbach slope with laminar or Swamee-Jain friction factor.
 *
 * Uses diameter4R, viscosity1.1e-5ft2/s and gravity32.2ft/s2. Re<=2000 selects64/Re; higher selects
 * explicit turbulent approximation. No transition blending or general finite guard.
 *
 * @param[in] velocity Signed native ft/s, magnitude used.
 * @param[in] hyd_rad Positive native ft radius; <=0 returns0.
 * @param[in] roughness Nonnegative absolute roughness height ft, caller-validated.
 * @return Dimensionless nonnegative friction slope for finite physical inputs;0 for nonpositive
 *     radius/Re or logarithm argument.
 */
OPENSWMM_KERNEL_FN double fricSlopeDW(double velocity, double hyd_rad, double roughness) noexcept {
    if (hyd_rad <= 0.0)
        return 0.0;
    const double v_abs = (velocity < 0.0) ? -velocity : velocity;
    const double diameter = 4.0 * hyd_rad;

    const double re = v_abs * diameter / kViscosity;
    if (re <= 0.0)
        return 0.0;

    double f;
    if (re <= 2000.0) {
        f = 64.0 / re; // laminar
    } else {
        const double e_over_d = roughness / diameter;
        const double arg = e_over_d / 3.7 + 5.74 / std::pow(re, 0.9);
        if (arg <= 0.0)
            return 0.0;
        const double logarg = std::log10(arg);
        f = 0.25 / (logarg * logarg);
    }
    return f * v_abs * v_abs / (2.0 * kGravity * diameter);
}

// ===========================================================================
// Culvert inlet control (FHWA HEC-5)
// ===========================================================================

/// Inlet-control curve coefficients for one culvert type code.
struct CulvertCurve {
    double K = 0.0;
    double M = 0.0;
    double C = 0.0;
    double Y = 0.0;
};

/**
 * @brief Limit proposed culvert flow by a three-regime inlet-control curve.
 *
 * Uses unsubmerged h/D<=0.95, submerged above derived transition, linear interpolation between. No
 * global finite/geometry validation, donor cap or sign handling.
 *
 * @param[in] q_proposed Nonnegative proposed native cfs discharge.
 * @param[in] head Upstream headwater above invert ft; nonpositive bypasses control.
 * @param[in] y_full Positive full culvert depth ft; nonpositive bypasses.
 * @param[in] a_full Positive full area ft2; nonpositive bypasses.
 * @param[in] slope Finite conduit slope ft/ft used for inlet correction.
 * @param[in] cc Borrowed valid FHWA coefficients K/M/C/Y; K=0 bypasses, physical branches require
 *     valid positive divisors.
 * @param[in] mitered True uses-7*slope correction, false+0.5*slope.
 * @param[out] dqdh Set0 first, then derivative of the inlet curve in cfs/ft when evaluated; can
 *     remain nonzero even if q_proposed is the returned minimum.
 * @return min(proposed,inlet) cfs on evaluated branch; proposed unchanged for bypass/invalid curve
 *     arguments.
 */
OPENSWMM_KERNEL_FN double culvertInflow(double q_proposed, double head, double y_full,
                                        double a_full, double slope, const CulvertCurve& cc,
                                        bool mitered, double& dqdh) noexcept {
    dqdh = 0.0;
    if (head <= 0.0 || y_full <= 0.0 || a_full <= 0.0)
        return q_proposed;
    if (cc.K == 0.0)
        return q_proposed;

    const double AD = a_full * std::sqrt(y_full);
    const double y_norm = head / y_full;
    const double scf = mitered ? (-7.0 * slope) : (0.5 * slope);

    const double y1_norm = 0.95;
    double y2_norm = 16.0 * cc.C + cc.Y - scf;
    if (y2_norm < y1_norm)
        y2_norm = y1_norm + 0.01;

    double q_inlet;
    if (y_norm <= y1_norm) {
        const double arg = y_norm / cc.K;
        if (arg <= 0.0)
            return q_proposed;
        q_inlet = AD * std::pow(arg, 1.0 / cc.M);
        dqdh = q_inlet / (head * cc.M);
    } else if (y_norm >= y2_norm) {
        const double arg = (y_norm - cc.Y + scf) / cc.C;
        if (arg <= 0.0)
            return q_proposed;
        q_inlet = AD * std::sqrt(arg);
        dqdh = 0.5 * q_inlet / (arg * y_full * cc.C);
    } else {
        const double arg1 = y1_norm / cc.K;
        const double q1 = AD * std::pow(arg1, 1.0 / cc.M);
        const double arg2 = (y2_norm - cc.Y + scf) / cc.C;
        const double q2 = (arg2 > 0.0) ? AD * std::sqrt(arg2) : q1;

        const double frac = (y_norm - y1_norm) / (y2_norm - y1_norm);
        q_inlet = q1 + frac * (q2 - q1);
        dqdh = (q2 - q1) / ((y2_norm - y1_norm) * y_full);
    }
    return (q_inlet < q_proposed) ? q_inlet : q_proposed;
}

} // namespace openswmm::hydkernels

#endif // OPENSWMM_ENGINE_HYD_CLOSURE_KERNELS_HPP
