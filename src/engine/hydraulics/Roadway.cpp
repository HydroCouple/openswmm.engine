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
 * @file Roadway.cpp
 * @brief Roadway weir overflow — legacy roadway.c, op for op.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Roadway.hpp"
#include <algorithm>
#include <cmath>

namespace openswmm {
namespace roadway {

// The discharge coefficients and submergence factors below were derived
// from Figure 10 in "Bridge Waterways Analysis Model: Research Report",
// FHWA/RD-86/108, July 1986 — legacy roadway.c tables verbatim.

// Discharge coefficients for (head / road width) <= 0.15, keyed on the HEAD
static const double Cr_Low_Paved[4][2] = {
    {0.0, 2.85}, {0.2, 2.95}, {0.7, 3.03}, {4.0, 3.05}};

static const double Cr_Low_Gravel[8][2] = {
    {0.0, 2.5}, {0.5, 2.7},  {1.0, 2.8}, {1.5, 2.9}, {2.0, 2.98},
    {2.5, 3.02}, {3.0, 3.03}, {4.0, 3.05}};

// Discharge coefficients for (head / road width) > 0.15, keyed on the ratio
static const double Cr_High_Paved[2][2] = {{0.15, 3.05}, {0.25, 3.10}};

static const double Cr_High_Gravel[2][2] = {{0.15, 2.95}, {0.30, 3.10}};

// Submergence factors, keyed on downstream / upstream head
static const double Kt_Paved[9][2] = {
    {0.8, 1.0}, {0.85, 0.98}, {0.90, 0.92}, {0.93, 0.85}, {0.95, 0.80},
    {0.97, 0.70}, {0.98, 0.60}, {0.99, 0.50}, {1.00, 0.40}};

static const double Kt_Gravel[12][2] = {
    {0.75, 1.00}, {0.80, 0.985}, {0.83, 0.97}, {0.86, 0.93}, {0.89, 0.90},
    {0.90, 0.87}, {0.92, 0.80},  {0.94, 0.70}, {0.96, 0.60}, {0.98, 0.50},
    {0.99, 0.40}, {1.00, 0.24}};

// legacy getY
static double getY(double x, const double table[][2], const int n) {
    if (x <= table[0][0]) return table[0][1];
    if (x >= table[n - 1][0]) return table[n - 1][1];
    for (int i = 1; i < n; i++) {
        if (x <= table[i][0]) {
            const double x1 = table[i - 1][0];
            const double dx = table[i][0] - x1;
            const double y1 = table[i - 1][1];
            const double dy = table[i][1] - y1;
            return y1 + (x - x1) * dy / dx;
        }
    }
    return table[n - 1][1];
}

// legacy getCd
static double getCd(double hWr, double ht, double roadWidth, int roadSurf) {
    double kT = 1.0;
    double cR;
    if (hWr <= 0.0) return 0.0;
    const double hL = hWr / roadWidth;
    if (hL <= 0.15) {
        if (roadSurf == SURFACE_PAVED) cR = getY(hWr, Cr_Low_Paved, 4);
        else                           cR = getY(hWr, Cr_Low_Gravel, 8);
    } else {
        if (roadSurf == SURFACE_PAVED) cR = getY(hL, Cr_High_Paved, 2);
        else                           cR = getY(hL, Cr_High_Gravel, 2);
    }
    if (ht > 0.0) {
        const double htH = ht / hWr;
        if (roadSurf == SURFACE_PAVED) kT = getY(htH, Kt_Paved, 9);
        else                           kT = getY(htH, Kt_Gravel, 12);
    }
    return cR * kT;
}

double getInflow(double dir, double h_road, double h1, double h2,
                 double cd_user, bool si, double road_width, int road_surf,
                 double length, double& dqdh, double& depth) {
    constexpr double FUDGE = 0.0001;
    double q = 0.0;
    dqdh = 0.0;

    // --- user-supplied discharge coeff.
    double cD = cd_user;
    if (si) cD = cD / 0.552;

    // --- check if there's enough info to use a variable cD value
    const bool useVariableCd = (road_width > 0.0 && road_surf >= 1);

    // --- upstream and downstream heads
    const double hWr = h1 - h_road;
    const double ht  = h2 - h_road;
    if (hWr > FUDGE) {
        // --- get discharge coeff. as function of heads
        if (useVariableCd) cD = getCd(hWr, ht, road_width, road_surf);

        // --- weir eqn. for discharge across roadway
        q = cD * length * std::pow(hWr, 1.5);
        dqdh = 1.5 * q / hWr;
    }

    depth = std::max(h1 - h_road, 0.0);
    return dir * q;
}

} // namespace roadway
} // namespace openswmm
