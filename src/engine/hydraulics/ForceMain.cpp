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
