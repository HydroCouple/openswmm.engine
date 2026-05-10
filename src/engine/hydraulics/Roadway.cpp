/**
 * @file Roadway.cpp
 * @brief Roadway weir overflow — numerically identical to legacy roadway.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "Roadway.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace roadway {

// Discharge coefficients — low head/width ratio (h/w <= 0.15)
static const double CR_LOW_PAVED[][2] = {
    {0.00, 2.84}, {0.04, 2.87}, {0.08, 2.93}, {0.12, 3.03}, {0.15, 3.08}
};
static const double CR_LOW_GRAVEL[][2] = {
    {0.00, 2.50}, {0.04, 2.54}, {0.08, 2.61}, {0.12, 2.70}, {0.15, 2.74}
};
// High ratio (h/w > 0.15)
static const double CR_HIGH_PAVED[][2] = {
    {0.15, 3.08}, {0.20, 3.20}, {0.30, 3.28}, {0.40, 3.30}
};
static const double CR_HIGH_GRAVEL[][2] = {
    {0.15, 2.74}, {0.20, 2.81}, {0.30, 2.87}, {0.40, 2.90}
};

// Submergence correction factor
static const double KT_PAVED[][2] = {
    {0.00, 1.00}, {0.76, 1.00}, {0.80, 0.99}, {0.84, 0.98}, {0.88, 0.95},
    {0.90, 0.91}, {0.92, 0.86}, {0.94, 0.78}, {0.96, 0.65}
};
static const double KT_GRAVEL[][2] = {
    {0.00, 1.00}, {0.76, 1.00}, {0.80, 0.99}, {0.84, 0.97}, {0.88, 0.93},
    {0.90, 0.88}, {0.92, 0.80}, {0.94, 0.68}, {0.96, 0.50}, {0.98, 0.25}
};

static double interpolate(const double table[][2], int n, double x) {
    if (x <= table[0][0]) return table[0][1];
    if (x >= table[n-1][0]) return table[n-1][1];
    for (int i = 1; i < n; ++i) {
        if (x <= table[i][0]) {
            double frac = (x - table[i-1][0]) / (table[i][0] - table[i-1][0]);
            return table[i-1][1] + frac * (table[i][1] - table[i-1][1]);
        }
    }
    return table[n-1][1];
}

double getDischargeCoeff(double hw_ratio, SurfaceType surface) {
    if (hw_ratio <= 0.15) {
        return (surface == SurfaceType::PAVED)
            ? interpolate(CR_LOW_PAVED, 5, hw_ratio)
            : interpolate(CR_LOW_GRAVEL, 5, hw_ratio);
    } else {
        return (surface == SurfaceType::PAVED)
            ? interpolate(CR_HIGH_PAVED, 4, hw_ratio)
            : interpolate(CR_HIGH_GRAVEL, 4, hw_ratio);
    }
}

double getSubmergenceFactor(double ht_ratio, SurfaceType surface) {
    if (ht_ratio <= 0.0) return 1.0;
    return (surface == SurfaceType::PAVED)
        ? interpolate(KT_PAVED, 9, ht_ratio)
        : interpolate(KT_GRAVEL, 10, ht_ratio);
}

double getFlow(double head_up, double head_down,
               double road_width, double road_length,
               SurfaceType surface, double& dqdh) {
    dqdh = 0.0;
    if (head_up <= 0.0 || road_length <= 0.0 || road_width <= 0.0) return 0.0;

    double hw_ratio = head_up / road_width;
    double Cr = getDischargeCoeff(hw_ratio, surface);

    double Kt = 1.0;
    if (head_down > 0.0) {
        double ht_ratio = head_down / head_up;
        Kt = getSubmergenceFactor(ht_ratio, surface);
    }

    double Cd = Cr * Kt;
    double q = Cd * road_length * head_up * std::sqrt(head_up);
    dqdh = 1.5 * q / head_up;

    return q;
}

} // namespace roadway
} // namespace openswmm
