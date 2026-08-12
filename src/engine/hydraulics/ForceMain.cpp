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
 * @file ForceMain.cpp
 * @brief Force main friction — numerically identical to legacy forcmain.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "ForceMain.hpp"
#include "HydClosureKernels.hpp"
#include "../core/Constants.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace forcemain {

// Both friction laws now live in HydClosureKernels.hpp so the identical bodies
// compile for the host solvers and for the device backend; these are the
// public entry points and nothing more.
//
// The kernel header cannot include Constants.hpp (it has to stay
// dependency-free for the plugin build), so it carries its own copy of the
// viscosity. Pin the two together here: getting them out of step shifted the
// Darcy-Weisbach slope by 0.7 % and the reference-curve benchmark caught it.
static_assert(hydkernels::kViscosity == constants::VISCOS,
              "HydClosureKernels::kViscosity must equal constants::VISCOS");
static_assert(hydkernels::kGravity == constants::GRAVITY,
              "HydClosureKernels::kGravity must equal constants::GRAVITY");

double getFricSlope_HW(double velocity, double hyd_rad, double c_hw) {
    return hydkernels::fricSlopeHW(velocity, hyd_rad, c_hw);
}

double getFricSlope_DW(double velocity, double hyd_rad, double roughness) {
    return hydkernels::fricSlopeDW(velocity, hyd_rad, roughness);
}

// ============================================================================
// getFricFactor — Darcy-Weisbach friction factor (matching legacy)
// ============================================================================

double getFricFactor(double r_bot, double hrad, double re) {
    if (hrad <= 0.0) return 0.0;
    double diameter = 4.0 * hrad;
    if (re <= 0.0) {
        // Fully turbulent approximation (Colebrook at Re→∞)
        re = 1.0e12;
    }
    double f;
    if (re <= 2000.0) {
        f = 64.0 / re;
    } else {
        double e_over_d = r_bot / diameter;
        double arg = e_over_d / 3.7 + 5.74 / std::pow(re, 0.9);
        if (arg <= 0.0) return 0.0;
        double logarg = std::log10(arg);
        f = 0.25 / (logarg * logarg);
    }
    return f;
}

// ============================================================================
// getEquivN — equivalent Manning's n for DW force main (Gap #22)
// Matches legacy forcemain_getEquivN() in forcmain.c
// ============================================================================

double getEquivN(FrictionModel model, double r_bot, double y_full,
                 double slope, double n_raw) {
    if (slope <= 0.0 || y_full <= 0.0) return n_raw;
    switch (model) {
        case FrictionModel::HAZEN_WILLIAMS:
            // legacy: 1.067 / rBot * pow(d/slope, 0.04)
            // where rBot = HW C-factor, d = y_full
            return 1.067 / r_bot * std::pow(y_full / slope, 0.04);

        case FrictionModel::DARCY_WEISBACH: {
            // legacy: sqrt(f/185.0) * pow(d, 1/6)
            // where f = getFricFactor(rBot, d/4, 1e12), d = y_full
            double hrad = y_full / 4.0;
            double f = getFricFactor(r_bot, hrad, 1.0e12);
            return std::sqrt(f / 185.0) * std::pow(y_full, 1.0 / 6.0);
        }
    }
    return n_raw;
}

// ============================================================================
// getRoughFactor — roughness adjustment for artificially-lengthened force main
// Matches legacy forcemain_getRoughFactor() in forcmain.c
// ============================================================================

double getRoughFactor(FrictionModel model, double r_bot, double length_factor) {
    if (length_factor <= 0.0) length_factor = 1.0;
    constexpr double G = 32.2;
    switch (model) {
        case FrictionModel::HAZEN_WILLIAMS: {
            // legacy: GRAVITY / pow(1.318 * rBot * pow(lengthFactor, 0.54), 1.852)
            double denom = 1.318 * r_bot * std::pow(length_factor, 0.54);
            return (denom > 0.0) ? G / std::pow(denom, 1.852) : 0.0;
        }
        case FrictionModel::DARCY_WEISBACH:
            // legacy: 1.0 / 8.0 / lengthFactor
            return 1.0 / (8.0 * length_factor);
    }
    return 0.0;
}

// ============================================================================
// Batch friction slope — VECTORISABLE
// ============================================================================

void batchFricSlope(const double* velocity, const double* hyd_rad,
                    const double* param, double* fric_slope,
                    FrictionModel model, int count) {
    if (model == FrictionModel::HAZEN_WILLIAMS) {
        for (int i = 0; i < count; ++i) {
            fric_slope[i] = getFricSlope_HW(velocity[i], hyd_rad[i], param[i]);
        }
    } else {
        for (int i = 0; i < count; ++i) {
            fric_slope[i] = getFricSlope_DW(velocity[i], hyd_rad[i], param[i]);
        }
    }
}

} // namespace forcemain
} // namespace openswmm
