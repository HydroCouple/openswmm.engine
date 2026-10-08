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
 * @file Culvert.cpp
 * @brief Culvert inlet control — numerically identical to legacy culvert.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Culvert.hpp"
#include "XSectBatch.hpp"
#include "XSectKernels.hpp"
#include "../core/Constants.hpp"
#include "../math/FindRoot.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace culvert {

/// Complete 58-entry FHWA HEC-5 culvert coefficient table.
/// Legacy reference: Params[58][5] in src/legacy/engine/culvert.c.
/// Each entry stores {K, M, C, Y} (FORM is handled via the code ranges).
static const CulvertCoeffs COEFFS[] = {
    // 0: unused
    {0.0, 0.0,    0.0,   0.0,    0.00},

    // Circular concrete
    {1.0, 0.0098, 2.00,  0.0398, 0.67},   //  1: Square edge w/headwall
    {1.0, 0.0018, 2.00,  0.0292, 0.74},   //  2: Groove end w/headwall
    {1.0, 0.0045, 2.00,  0.0317, 0.69},   //  3: Groove end projecting

    // Circular Corrugated Metal Pipe
    {1.0, 0.0078, 2.00,  0.0379, 0.69},   //  4: Headwall
    {1.0, 0.0210, 1.33,  0.0463, 0.75},   //  5: Mitered to slope
    {1.0, 0.0340, 1.50,  0.0553, 0.54},   //  6: Projecting

    // Circular Pipe, Beveled Ring Entrance
    {1.0, 0.0018, 2.50,  0.0300, 0.74},   //  7: Beveled ring, 45 deg bevels
    {1.0, 0.0018, 2.50,  0.0243, 0.83},   //  8: Beveled ring, 33.7 deg bevels

    // Rectangular Box with Flared Wingwalls
    {1.0, 0.026,  1.0,   0.0347, 0.81},   //  9: 30-75 deg wingwall flares
    {1.0, 0.061,  0.75,  0.0400, 0.80},   // 10: 90 or 15 deg wingwall flares
    {1.0, 0.061,  0.75,  0.0423, 0.82},   // 11: 0 deg wingwall flares (straight sides)

    // Rectangular Box with Flared Wingwalls & Top Edge Bevel
    {2.0, 0.510,  0.667, 0.0309, 0.80},   // 12: 45 deg flare; 0.43D top edge bevel
    {2.0, 0.486,  0.667, 0.0249, 0.83},   // 13: 18-33.7 deg flare; 0.083D top edge bevel

    // Rectangular Box; 90-deg Headwall; Chamfered or Beveled Inlet Edges
    {2.0, 0.515,  0.667, 0.0375, 0.79},   // 14: chamfered 3/4-in
    {2.0, 0.495,  0.667, 0.0314, 0.82},   // 15: beveled 1/2-in/ft at 45 deg (1:1)
    {2.0, 0.486,  0.667, 0.0252, 0.865},  // 16: beveled 1-in/ft at 33.7 deg (1:1.5)

    // Rectangular Box; Skewed Headwall; Chamfered or Beveled Inlet Edges
    {2.0, 0.545,  0.667, 0.04505,0.73},   // 17: 3/4" chamfered edge, 45 deg skewed headwall
    {2.0, 0.533,  0.667, 0.0425, 0.705},  // 18: 3/4" chamfered edge, 30 deg skewed headwall
    {2.0, 0.522,  0.667, 0.0402, 0.68},   // 19: 3/4" chamfered edge, 15 deg skewed headwall
    {2.0, 0.498,  0.667, 0.0327, 0.75},   // 20: 45 deg beveled edge, 10-45 deg skewed headwall

    // Rectangular box, Non-offset Flared Wingwalls; 3/4" Chamfer at Top of Inlet
    {2.0, 0.497,  0.667, 0.0339, 0.803},  // 21: 45 deg (1:1) wingwall flare
    {2.0, 0.493,  0.667, 0.0361, 0.806},  // 22: 18.4 deg (3:1) wingwall flare
    {2.0, 0.495,  0.667, 0.0386, 0.71},   // 23: 18.4 deg (3:1) wingwall flare, 30 deg inlet skew

    // Rectangular box, Offset Flared Wingwalls, Beveled Edge at Inlet Top
    {2.0, 0.497,  0.667, 0.0302, 0.835},  // 24: 45 deg (1:1) flare, 0.042D top edge bevel
    {2.0, 0.495,  0.667, 0.0252, 0.881},  // 25: 33.7 deg (1.5:1) flare, 0.083D top edge bevel
    {2.0, 0.493,  0.667, 0.0227, 0.887},  // 26: 18.4 deg (3:1) flare, 0.083D top edge bevel

    // Corrugated Metal Box
    {1.0, 0.0083, 2.00,  0.0379, 0.69},   // 27: 90 deg headwall
    {1.0, 0.0145, 1.75,  0.0419, 0.64},   // 28: Thick wall projecting
    {1.0, 0.0340, 1.50,  0.0496, 0.57},   // 29: Thin wall projecting

    // Horizontal Ellipse Concrete
    {1.0, 0.0100, 2.00,  0.0398, 0.67},   // 30: Square edge w/headwall
    {1.0, 0.0018, 2.50,  0.0292, 0.74},   // 31: Grooved end w/headwall
    {1.0, 0.0045, 2.00,  0.0317, 0.69},   // 32: Grooved end projecting

    // Vertical Ellipse Concrete
    {1.0, 0.0100, 2.00,  0.0398, 0.67},   // 33: Square edge w/headwall
    {1.0, 0.0018, 2.50,  0.0292, 0.74},   // 34: Grooved end w/headwall
    {1.0, 0.0095, 2.00,  0.0317, 0.69},   // 35: Grooved end projecting

    // Pipe Arch, 18" Corner Radius, Corrugated Metal
    {1.0, 0.0083, 2.00,  0.0379, 0.69},   // 36: 90 deg headwall
    {1.0, 0.0300, 1.00,  0.0463, 0.75},   // 37: Mitered to slope
    {1.0, 0.0340, 1.50,  0.0496, 0.57},   // 38: Projecting

    // Pipe Arch, 18" Corner Radius, Corrugated Metal (cont.)
    {1.0, 0.0300, 1.50,  0.0496, 0.57},   // 39: Projecting
    {1.0, 0.0088, 2.00,  0.0368, 0.68},   // 40: No bevels
    {1.0, 0.0030, 2.00,  0.0269, 0.77},   // 41: 33.7 deg bevels

    // Pipe Arch, 31" Corner Radius, Corrugated Metal
    {1.0, 0.0300, 1.50,  0.0496, 0.57},   // 42: Projecting
    {1.0, 0.0088, 2.00,  0.0368, 0.68},   // 43: No bevels
    {1.0, 0.0030, 2.00,  0.0269, 0.77},   // 44: 33.7 deg bevels

    // Arch, Corrugated Metal
    {1.0, 0.0083, 2.00,  0.0379, 0.69},   // 45: 90 deg headwall
    {1.0, 0.0300, 1.00,  0.0473, 0.75},   // 46: Mitered to slope
    {1.0, 0.0340, 1.50,  0.0496, 0.57},   // 47: Thin wall projecting

    // Circular Culvert
    {2.0, 0.534,  0.555, 0.0196, 0.90},   // 48: Smooth tapered inlet throat
    {2.0, 0.519,  0.640, 0.0210, 0.90},   // 49: Rough tapered inlet throat

    // Elliptical Inlet Face
    {2.0, 0.536,  0.622, 0.0368, 0.83},   // 50: Tapered inlet, beveled edges
    {2.0, 0.5035, 0.719, 0.0478, 0.80},   // 51: Tapered inlet, square edges
    {2.0, 0.547,  0.800, 0.0598, 0.75},   // 52: Tapered inlet, thin edge projecting

    // Rectangular
    {2.0, 0.475,  0.667, 0.0179, 0.97},   // 53: Tapered inlet throat

    // Rectangular Concrete
    {2.0, 0.560,  0.667, 0.0446, 0.85},   // 54: Side tapered, less favorable edges
    {2.0, 0.560,  0.667, 0.0378, 0.87},   // 55: Side tapered, more favorable edges

    // Rectangular Concrete
    {2.0, 0.500,  0.667, 0.0446, 0.65},   // 56: Slope tapered, less favorable edges
    {2.0, 0.500,  0.667, 0.0378, 0.71},   // 57: Slope tapered, more favorable edges
};
static constexpr int MAX_CULVERT_CODE = 57;
static constexpr int N_COEFFS = sizeof(COEFFS) / sizeof(COEFFS[0]);

CulvertCoeffs getCoeffs(int code) {
    if (code >= 1 && code <= MAX_CULVERT_CODE) return COEFFS[code];
    return COEFFS[0];
}

// ---------------------------------------------------------------------------
// legacy culvert.c, op for op. The unsubmerged Form-1 flow comes out of
// Ridder's method on form1Eqn: the flow is whatever the LAST evaluation of
// form1Eqn left in qc (the root itself is discarded), so the root finder has
// to call the equation in legacy's exact sequence (findroot::ridder).
// ---------------------------------------------------------------------------
namespace {

constexpr double BIG = 1.0e10;   // legacy consts.h

struct TCulvert {
    double yFull = 0.0;   // full depth of culvert (ft)
    double scf   = 0.0;   // slope correction factor
    double dQdH  = 0.0;   // derivative of flow w.r.t. head
    double qc    = 0.0;   // unsubmerged critical flow
    double kk = 0.0, mm = 0.0;   // coeffs. for unsubmerged flow
    double ad    = 0.0;
    double hPlus = 0.0;   // intermediate terms
    const XSectParams* xsect = nullptr;
};

// h/yFull + 0.5*s = yc/yFull + yh/2/yFull + K[ac/aFull*sqrt(g*yh/yFull)]^M
double form1Eqn(double yc, TCulvert* culvert) {
    const double ac = xsect::getAofY(*culvert->xsect, yc);
    const double wc = xsect::getWofY(*culvert->xsect, yc);
    const double yh = ac / wc;
    culvert->qc = ac * std::sqrt(constants::GRAVITY * yh);
    return culvert->hPlus - yc / culvert->yFull - yh / 2.0 / culvert->yFull -
           culvert->kk * std::pow(culvert->qc / culvert->ad, culvert->mm);
}

double getForm1Flow(double h, TCulvert* culvert) {
    culvert->hPlus = h / culvert->yFull + culvert->scf;
    // Ridder's method between 0.01h and h; the result is culvert->qc
    findroot::ridder(0.01 * h, h, 0.001,
                     [culvert](double yc) { return form1Eqn(yc, culvert); });
    return culvert->qc;
}

double getUnsubmergedFlow(const CulvertCoeffs& P, double h, TCulvert* culvert) {
    culvert->kk = P.K;
    culvert->mm = P.M;
    const double arg = h / culvert->yFull / culvert->kk;
    double q;
    if (P.form == 1.0) q = getForm1Flow(h, culvert);
    else               q = culvert->ad * std::pow(arg, 1.0 / culvert->mm);
    culvert->dQdH = q / h / culvert->mm;
    return q;
}

double getSubmergedFlow(const CulvertCoeffs& P, double h, TCulvert* culvert) {
    const double cc = P.C;
    const double yy = P.Y;
    const double arg = (h / culvert->yFull - yy + culvert->scf) / cc;
    if (arg <= 0.0) {
        culvert->dQdH = 0.0;
        return BIG;
    }
    const double q = std::sqrt(arg) * culvert->ad;
    culvert->dQdH = 0.5 * q / arg / culvert->yFull / cc;
    return q;
}

double getTransitionFlow(const CulvertCoeffs& P, double h, double h1, double h2,
                         TCulvert* culvert) {
    const double q1 = getUnsubmergedFlow(P, h1, culvert);
    const double q2 = getSubmergedFlow(P, h2, culvert);
    const double q = q1 + (q2 - q1) * (h - h1) / (h2 - h1);
    culvert->dQdH = (q2 - q1) / (h2 - h1);
    return q;
}

} // namespace

double getInflow(double q0, double y, const XSectParams& xs, double slope,
                 int code, double& dqdh, bool& controls) {
    controls = false;
    if (code <= 0 || code > MAX_CULVERT_CODE) return q0;
    const CulvertCoeffs P = COEFFS[code];

    TCulvert culvert;
    culvert.xsect = &xs;
    culvert.yFull = xs.y_full;
    culvert.ad    = xs.a_full * std::sqrt(culvert.yFull);

    // slope correction factor (-7 for mitered inlets, 0.5 for others)
    switch (code) {
        case 5: case 37: case 46: culvert.scf = -7.0 * slope; break;
        default:                  culvert.scf =  0.5 * slope;
    }

    double q;
    // submerged flow (FHWA criterion Q/AD > 4)
    const double y2 = culvert.yFull * (16.0 * P.C + P.Y - culvert.scf);
    if (y >= y2) {
        q = getSubmergedFlow(P, y, &culvert);
    } else {
        // unsubmerged below the arbitrary 0.95-full limit, transition between
        const double y1 = 0.95 * culvert.yFull;
        if (y <= y1) q = getUnsubmergedFlow(P, y, &culvert);
        else         q = getTransitionFlow(P, y, y1, y2, &culvert);
    }

    if (q < q0) {
        controls = true;
        dqdh = culvert.dQdH;
        return q;
    }
    return q0;
}

} // namespace culvert
} // namespace openswmm
