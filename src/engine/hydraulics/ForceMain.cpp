/**
 * @file ForceMain.cpp
 * @brief Force main friction — numerically identical to legacy forcmain.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "ForceMain.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace forcemain {

double getFricSlope_HW(double velocity, double hyd_rad, double c_hw) {
    if (c_hw <= 0.0 || hyd_rad <= 0.0) return 0.0;
    // Sf = (v / (1.318 * C * R^0.63))^(1/0.54)
    // Simplified legacy form: Sf = sBot * |v|^0.852 / R^1.1667
    // where sBot is precomputed from C
    double v_abs = std::fabs(velocity);
    return std::pow(v_abs / (1.318 * c_hw * std::pow(hyd_rad, 0.63)), 1.0 / 0.54);
}

double getFricSlope_DW(double velocity, double hyd_rad, double roughness) {
    if (hyd_rad <= 0.0) return 0.0;
    double v_abs = std::fabs(velocity);
    double diameter = 4.0 * hyd_rad;

    // Reynolds number
    double Re = v_abs * diameter / VISCOS;
    if (Re <= 0.0) return 0.0;

    double f;
    if (Re <= 2000.0) {
        // Laminar: f = 64/Re
        f = 64.0 / Re;
    } else {
        // Swamee-Jain (explicit Colebrook-White approximation)
        double e_over_d = roughness / diameter;
        double arg = e_over_d / 3.7 + 5.74 / std::pow(Re, 0.9);
        if (arg <= 0.0) return 0.0;
        double logarg = std::log10(arg);
        f = 0.25 / (logarg * logarg);
    }

    // Sf = f * v^2 / (2 * g * D)
    return f * v_abs * v_abs / (2.0 * 32.2 * diameter);
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
