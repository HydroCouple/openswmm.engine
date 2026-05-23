/**
 * @file Culvert.cpp
 * @brief Culvert inlet control — numerically identical to legacy culvert.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "Culvert.hpp"
#include "../core/SimulationContext.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace culvert {

/// Complete 58-entry FHWA HEC-5 culvert coefficient table.
/// Legacy reference: Params[58][5] in src/legacy/engine/culvert.c.
/// Each entry stores {K, M, C, Y} (FORM is handled via the code ranges).
static const CulvertCoeffs COEFFS[] = {
    // 0: unused
    {0.0,    0.0,   0.0,    0.00},

    // Circular concrete
    {0.0098, 2.00,  0.0398, 0.67},   //  1: Square edge w/headwall
    {0.0018, 2.00,  0.0292, 0.74},   //  2: Groove end w/headwall
    {0.0045, 2.00,  0.0317, 0.69},   //  3: Groove end projecting

    // Circular Corrugated Metal Pipe
    {0.0078, 2.00,  0.0379, 0.69},   //  4: Headwall
    {0.0210, 1.33,  0.0463, 0.75},   //  5: Mitered to slope
    {0.0340, 1.50,  0.0553, 0.54},   //  6: Projecting

    // Circular Pipe, Beveled Ring Entrance
    {0.0018, 2.50,  0.0300, 0.74},   //  7: Beveled ring, 45 deg bevels
    {0.0018, 2.50,  0.0243, 0.83},   //  8: Beveled ring, 33.7 deg bevels

    // Rectangular Box with Flared Wingwalls
    {0.026,  1.0,   0.0347, 0.81},   //  9: 30-75 deg wingwall flares
    {0.061,  0.75,  0.0400, 0.80},   // 10: 90 or 15 deg wingwall flares
    {0.061,  0.75,  0.0423, 0.82},   // 11: 0 deg wingwall flares (straight sides)

    // Rectangular Box with Flared Wingwalls & Top Edge Bevel
    {0.510,  0.667, 0.0309, 0.80},   // 12: 45 deg flare; 0.43D top edge bevel
    {0.486,  0.667, 0.0249, 0.83},   // 13: 18-33.7 deg flare; 0.083D top edge bevel

    // Rectangular Box; 90-deg Headwall; Chamfered or Beveled Inlet Edges
    {0.515,  0.667, 0.0375, 0.79},   // 14: chamfered 3/4-in
    {0.495,  0.667, 0.0314, 0.82},   // 15: beveled 1/2-in/ft at 45 deg (1:1)
    {0.486,  0.667, 0.0252, 0.865},  // 16: beveled 1-in/ft at 33.7 deg (1:1.5)

    // Rectangular Box; Skewed Headwall; Chamfered or Beveled Inlet Edges
    {0.545,  0.667, 0.04505,0.73},   // 17: 3/4" chamfered edge, 45 deg skewed headwall
    {0.533,  0.667, 0.0425, 0.705},  // 18: 3/4" chamfered edge, 30 deg skewed headwall
    {0.522,  0.667, 0.0402, 0.68},   // 19: 3/4" chamfered edge, 15 deg skewed headwall
    {0.498,  0.667, 0.0327, 0.75},   // 20: 45 deg beveled edge, 10-45 deg skewed headwall

    // Rectangular box, Non-offset Flared Wingwalls; 3/4" Chamfer at Top of Inlet
    {0.497,  0.667, 0.0339, 0.803},  // 21: 45 deg (1:1) wingwall flare
    {0.493,  0.667, 0.0361, 0.806},  // 22: 18.4 deg (3:1) wingwall flare
    {0.495,  0.667, 0.0386, 0.71},   // 23: 18.4 deg (3:1) wingwall flare, 30 deg inlet skew

    // Rectangular box, Offset Flared Wingwalls, Beveled Edge at Inlet Top
    {0.497,  0.667, 0.0302, 0.835},  // 24: 45 deg (1:1) flare, 0.042D top edge bevel
    {0.495,  0.667, 0.0252, 0.881},  // 25: 33.7 deg (1.5:1) flare, 0.083D top edge bevel
    {0.493,  0.667, 0.0227, 0.887},  // 26: 18.4 deg (3:1) flare, 0.083D top edge bevel

    // Corrugated Metal Box
    {0.0083, 2.00,  0.0379, 0.69},   // 27: 90 deg headwall
    {0.0145, 1.75,  0.0419, 0.64},   // 28: Thick wall projecting
    {0.0340, 1.50,  0.0496, 0.57},   // 29: Thin wall projecting

    // Horizontal Ellipse Concrete
    {0.0100, 2.00,  0.0398, 0.67},   // 30: Square edge w/headwall
    {0.0018, 2.50,  0.0292, 0.74},   // 31: Grooved end w/headwall
    {0.0045, 2.00,  0.0317, 0.69},   // 32: Grooved end projecting

    // Vertical Ellipse Concrete
    {0.0100, 2.00,  0.0398, 0.67},   // 33: Square edge w/headwall
    {0.0018, 2.50,  0.0292, 0.74},   // 34: Grooved end w/headwall
    {0.0095, 2.00,  0.0317, 0.69},   // 35: Grooved end projecting

    // Pipe Arch, 18" Corner Radius, Corrugated Metal
    {0.0083, 2.00,  0.0379, 0.69},   // 36: 90 deg headwall
    {0.0300, 1.00,  0.0463, 0.75},   // 37: Mitered to slope
    {0.0340, 1.50,  0.0496, 0.57},   // 38: Projecting

    // Pipe Arch, 18" Corner Radius, Corrugated Metal (cont.)
    {0.0300, 1.50,  0.0496, 0.57},   // 39: Projecting
    {0.0088, 2.00,  0.0368, 0.68},   // 40: No bevels
    {0.0030, 2.00,  0.0269, 0.77},   // 41: 33.7 deg bevels

    // Pipe Arch, 31" Corner Radius, Corrugated Metal
    {0.0300, 1.50,  0.0496, 0.57},   // 42: Projecting
    {0.0088, 2.00,  0.0368, 0.68},   // 43: No bevels
    {0.0030, 2.00,  0.0269, 0.77},   // 44: 33.7 deg bevels

    // Arch, Corrugated Metal
    {0.0083, 2.00,  0.0379, 0.69},   // 45: 90 deg headwall
    {0.0300, 1.00,  0.0473, 0.75},   // 46: Mitered to slope
    {0.0340, 1.50,  0.0496, 0.57},   // 47: Thin wall projecting

    // Circular Culvert
    {0.534,  0.555, 0.0196, 0.90},   // 48: Smooth tapered inlet throat
    {0.519,  0.640, 0.0210, 0.90},   // 49: Rough tapered inlet throat

    // Elliptical Inlet Face
    {0.536,  0.622, 0.0368, 0.83},   // 50: Tapered inlet, beveled edges
    {0.5035, 0.719, 0.0478, 0.80},   // 51: Tapered inlet, square edges
    {0.547,  0.800, 0.0598, 0.75},   // 52: Tapered inlet, thin edge projecting

    // Rectangular
    {0.475,  0.667, 0.0179, 0.97},   // 53: Tapered inlet throat

    // Rectangular Concrete
    {0.560,  0.667, 0.0446, 0.85},   // 54: Side tapered, less favorable edges
    {0.560,  0.667, 0.0378, 0.87},   // 55: Side tapered, more favorable edges

    // Rectangular Concrete
    {0.500,  0.667, 0.0446, 0.65},   // 56: Slope tapered, less favorable edges
    {0.500,  0.667, 0.0378, 0.71},   // 57: Slope tapered, more favorable edges
};
static constexpr int MAX_CULVERT_CODE = 57;
static constexpr int N_COEFFS = sizeof(COEFFS) / sizeof(COEFFS[0]);

/// Determine the FHWA equation form for a culvert code.
/// Form 1 codes: 1-8, 27-47 (legacy Params[][FORM] == 1.0).
/// Form 2 codes: 9-26, 48-57 (legacy Params[][FORM] == 2.0).
static int getForm(int code) {
    if (code >= 1 && code <= 8)   return 1;
    if (code >= 27 && code <= 47) return 1;
    // codes 9-26 and 48-57 are form 2
    return 2;
}

CulvertCoeffs getCoeffs(int code) {
    if (code >= 1 && code <= MAX_CULVERT_CODE) return COEFFS[code];
    return COEFFS[0];
}

double getInflow(double q_proposed, double head, double y_full,
                 double a_full, double slope, int code, double& dqdh) {
    dqdh = 0.0;
    if (head <= 0.0 || y_full <= 0.0 || a_full <= 0.0) return q_proposed;

    auto cc = getCoeffs(code);
    if (cc.K == 0.0) return q_proposed;

    double AD = a_full * std::sqrt(y_full);
    double y_norm = head / y_full;

    // Slope correction factor: -7*slope for mitered inlets, +0.5*slope otherwise
    // Mitered inlet codes: 5, 37, 46 (legacy culvert.c switch statement)
    double scf;
    if (code == 5 || code == 37 || code == 46)
        scf = -7.0 * slope;
    else
        scf = 0.5 * slope;

    // Regime thresholds
    double y1_norm = 0.95;
    double y2_norm = 16.0 * cc.C + cc.Y - scf;
    if (y2_norm < y1_norm) y2_norm = y1_norm + 0.01;

    double q_inlet;

    if (y_norm <= y1_norm) {
        // Unsubmerged: Q = AD * (h/yFull/K)^(1/M)
        double arg = y_norm / cc.K;
        if (arg <= 0.0) return q_proposed;
        q_inlet = AD * std::pow(arg, 1.0 / cc.M);
        dqdh = q_inlet / (head * cc.M);
    }
    else if (y_norm >= y2_norm) {
        // Submerged: Q = AD * sqrt((h/yFull - Y + scf) / C)
        double arg = (y_norm - cc.Y + scf) / cc.C;
        if (arg <= 0.0) return q_proposed;
        q_inlet = AD * std::sqrt(arg);
        dqdh = 0.5 * q_inlet / (arg * y_full * cc.C);
    }
    else {
        // Transition: linear interpolation
        double arg1 = y1_norm / cc.K;
        double q1 = AD * std::pow(arg1, 1.0 / cc.M);
        double arg2 = (y2_norm - cc.Y + scf) / cc.C;
        double q2 = (arg2 > 0.0) ? AD * std::sqrt(arg2) : q1;

        double frac = (y_norm - y1_norm) / (y2_norm - y1_norm);
        q_inlet = q1 + frac * (q2 - q1);
        dqdh = (q2 - q1) / ((y2_norm - y1_norm) * y_full);
    }

    // Inlet controls only if q_inlet < q_proposed
    return std::min(q_inlet, q_proposed);
}

void batchComputeInletControl(const int* link_indices, int n,
                               SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    for (int ii = 0; ii < n; ++ii) {
        int j = link_indices[ii];

        // Only process conduit links with a valid culvert code
        if (links.type[j] != LinkType::CONDUIT) continue;
        int code = links.culvert_code[j];
        if (code <= 0 || code > MAX_CULVERT_CODE) continue;

        // Gather parameters
        double y_full = links.xsect_y_full[j];
        double a_full = links.xsect_a_full[j];
        double s      = links.slope[j];
        double q0     = links.flow[j];

        // Compute upstream head above culvert invert
        int n1 = links.node1[j];
        double head = nodes.head[n1]
                     - (nodes.invert_elev[n1] + links.offset1[j]);

        // Compute inlet-controlled flow
        double dq = 0.0;
        double q_inlet = getInflow(std::fabs(q0), head, y_full, a_full, s,
                                   code, dq);

        // Inlet controls only if q_inlet < |q0|
        if (q_inlet < std::fabs(q0)) {
            links.inlet_control[j] = true;
            links.dqdh[j] = dq;
            // Preserve flow sign (direction)
            links.flow[j] = (q0 >= 0.0) ? q_inlet : -q_inlet;
        }
    }
}

} // namespace culvert
} // namespace openswmm
