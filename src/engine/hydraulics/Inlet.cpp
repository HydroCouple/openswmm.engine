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
 * @file Inlet.cpp
 * @brief Street inlet capture — numerically identical to legacy inlet.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Inlet.hpp"
#include "Link.hpp"
#include "XSectBatch.hpp"
#include "../core/Constants.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../data/InfraData.hpp"
#include "../data/TableData.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace openswmm {
namespace inlet {

// ============================================================================
// Constant tables (HEC-22 charts, legacy inlet.c:152-170)
// ============================================================================

// Cubic fits to the Splash-Over Velocity vs. Grate Length curves of HEC-22
// Chart 5B. PARITY NOTE: legacy declares this table with SEVEN rows for EIGHT
// grate types (inlet.c:152-159) — a GENERIC grate index would read past the
// end. Legacy never does because every caller special-cases GENERIC, but the
// out-of-range read is one memcpy away; getSplashOverVelocity() guards it here
// instead of relying on the callers.
static const double SPLASH_COEFFS[7][4] = {
    {2.22, 4.03, 0.65, 0.06},     // P_BAR-50
    {0.74, 2.44, 0.27, 0.02},     // P_BAR-50x100
    {1.76, 3.12, 0.45, 0.03},     // P_BAR-30
    {0.30, 4.85, 1.31, 0.15},     // CURVED_VANE
    {0.99, 2.64, 0.36, 0.03},     // TILT_BAR-45
    {0.51, 2.34, 0.20, 0.01},     // TILT_BAR-30
    {0.28, 2.28, 0.18, 0.01},     // RETICULINE
};

// Grate opening ratios (HEC-22 Chart 9B, inlet.c:162-170)
static const double GRATE_OPEN_RATIOS[8] = {
    0.90,     // P_BAR-50
    0.80,     // P_BAR-50x100
    0.60,     // P_BAR-30
    0.35,     // CURVED_VANE
    0.17,     // TILT_BAR-45 (assumed)
    0.34,     // TILT_BAR-30
    0.80,     // RETICULINE
    1.00,     // GENERIC
};

// ============================================================================
// InletSoA
// ============================================================================

void InletSoA::resize(int n) {
    count = n;
    auto un = static_cast<std::size_t>(n);

    link_idx.assign(un, -1);
    host_node.assign(un, -1);
    node_idx.assign(un, -1);
    bypass_node.assign(un, -1);
    up_link.assign(un, -1);
    up_link2.assign(un, -1);
    two_inbound.assign(un, uint8_t{0});

    inlet_type.assign(un, static_cast<int>(InletType::CUSTOM));
    grate_type.assign(un, static_cast<int>(GrateType::GENERIC));
    grate_length.assign(un, 0.0);
    grate_width.assign(un, 0.0);
    open_area.assign(un, 0.0);
    splash_veloc.assign(un, 0.0);
    curb_length.assign(un, 0.0);
    curb_height.assign(un, 0.0);
    curb_throat.assign(un, static_cast<int>(ThroatAngle::VERTICAL));
    slotted_length.assign(un, 0.0);
    slotted_width.assign(un, 0.0);
    curve_index.assign(un, -1);
    curve_kind.assign(un, static_cast<int>(CurveKind::UNRESOLVED));

    num_inlets.assign(un, 1);
    clog_factor.assign(un, 1.0);
    flow_limit.assign(un, 0.0);
    local_depress.assign(un, 0.0);
    local_width.assign(un, 0.0);
    placement.assign(un, static_cast<int>(Placement::AUTOMATIC));
    is_sag.assign(un, uint8_t{0});

    sx.assign(un, 0.01);
    gutter_depression.assign(un, 0.0);
    gutter_width.assign(un, 0.0);
    road_roughness.assign(un, 0.016);
    t_crown.assign(un, 100.0);
    n_sides.assign(un, 1);
    slope.assign(un, 0.0);
    flow_factor.assign(un, 0.0);

    q_approach.assign(un, 0.0);
    flow_capture.assign(un, 0.0);
    backflow.assign(un, 0.0);
    backflow_ratio.assign(un, 0.0);

    flow_periods.assign(un, 0);
    capture_periods.assign(un, 0);
    backflow_periods.assign(un, 0);
    peak_flow.assign(un, 0.0);
    peak_flow_capture.assign(un, 0.0);
    avg_flow_capture.assign(un, 0.0);
    bypass_freq.assign(un, 0.0);

    stat_capture_vol.assign(un, 0.0);
    stat_bypass_vol.assign(un, 0.0);
    stat_backflow_vol.assign(un, 0.0);
    stat_peak_flow.assign(un, 0.0);
}

// ============================================================================
// Pure kernel — 1:1 ports of the legacy static functions
// ============================================================================

double getEo(double Sr, double Ts, double w) {
    // HEC-22 Eq(4-4) with Ts/w substituted for (T/w) − 1 (inlet.c:1248).
    double x = Sr / (Ts / w);
    x = std::pow((1.0 + x), 2.67) - 1.0;
    x = 1.0 + Sr / x;
    return 1.0 / x;
}

double getFlowSpread(const Geom& g, double Q) {
    // inlet.c:1200 — Izzard's form of Manning's equation, Q = f·T^2.67.
    const double f = g.qfactor;
    double Ts1;

    if (g.a == 0.0) {
        Ts1 = std::pow(Q / f, 0.375);                       // HEC-22 Eq(4-2)
    } else {
        // --- check if spread is within curb width
        const double f1 = f * std::pow((g.a / g.w) / g.sx, 1.67);
        const double Tw = std::pow(Q / f1, 0.375);          // HEC-22 Eq(4-2)
        if (Tw <= g.w) {
            Ts1 = Tw;
        } else {
            // --- spread extends beyond curb width
            const double Sr = (g.sx + g.a / g.w) / g.sx;
            int iter = 1;
            double Ts2 = 0.0;
            Ts1 = std::pow(Q / f, 0.375) - g.w;
            if (Ts1 <= 0) Ts1 = Tw - g.w;
            while (iter < 11) {
                const double Eo = getEo(Sr, Ts1, g.w);
                const double Qs = (1.0 - Eo) * Q;           // HEC-22 Eq(4-6)
                Ts2 = std::pow(Qs / f, 0.375);              // HEC-22 Eq(4-2)
                if (std::fabs(Ts2 - Ts1) < 0.01) break;
                Ts1 = Ts2;
                iter++;
            }
            Ts1 = Ts2 + g.w;
        }
    }
    return std::min(Ts1, g.t_crown);
}

double getGutterFlowRatio(const Geom& g, double w) {
    // inlet.c:1518
    if (g.t <= w) return 1.0;
    else if (g.a > 0.0) return getEo(g.sw / g.sx, g.t - w, w);
    else return 1.0 - std::pow((1.0 - w / g.t), 2.67);      // HEC-22 Eq(4-16)
}

double getGutterAreaRatio(const Geom& g, double Wg, double A) {
    // inlet.c:1535
    if (Wg >= g.w)  return 1.0;
    if (g.t <= Wg)  return 1.0;
    if (g.t <= g.w) return Wg / g.t;
    const double As = 0.5 * (g.t - g.w) * (g.t - g.w) * g.sx;
    const double Ag = Wg * ((g.t * g.sx) + g.a - (Wg * g.sw / 2.0));
    return Ag / (A - As);
}

double getSplashOverVelocity(int grate_type, double L) {
    // inlet.c:1557. GENERIC (and any out-of-range code) has no fitted curve —
    // its splash-over velocity is the user-supplied value on the design.
    if (grate_type < 0 || grate_type >= 7) return 0.0;
    const auto gt = static_cast<std::size_t>(grate_type);
    return SPLASH_COEFFS[gt][0] +
           SPLASH_COEFFS[gt][1] * L -
           SPLASH_COEFFS[gt][2] * L * L +
           SPLASH_COEFFS[gt][3] * L * L * L;
}

/// Grate open-area ratio for a design (legacy GrateOpeningRatios lookup with
/// the GENERIC/out-of-range guard the 7-row SplashCoeffs table lacks).
static double grateOpenRatio(const Design& d) {
    const int gt = d.grate_type;
    if (gt < 0 || gt >= static_cast<int>(GrateType::GENERIC))
        return d.frac_open_area;
    return GRATE_OPEN_RATIOS[static_cast<std::size_t>(gt)];
}

double getGrateInletCapture(const Design& d, Geom& g, double Q) {
    // inlet.c:1387. g.t (spread), g.a, g.w and g.sx come from the caller
    // exactly as legacy's shared T / a / W / Sx do.
    const double Lg = d.grate_length;
    double Wg = d.grate_width;
    double A;                 // total cross-section flow area (ft2)
    double Eo;                // ratio of gutter to total flow
    double Qo = Q;            // flow over street area (cfs)
    double Rf = 1.0;          // ratio of intercepted to total frontal flow
    double Rs = 0.0;          // ratio of intercepted to total side flow

    const bool channel = (g.xs != nullptr) &&
        (g.xs->type == static_cast<int>(XSectShape::TRAPEZOIDAL) ||
         g.xs->type == static_cast<int>(XSectShape::RECT_OPEN));

    // --- flow ratio for drop inlet in a rectangular/trapezoidal channel
    if (channel) {
        A = xsect::getAofS(*g.xs, Q / g.beta);
        const double Y = xsect::getYofA(*g.xs, A);
        g.t = xsect::getWofY(*g.xs, Y);
        Eo = g.beta * std::pow(Y * Wg, 1.67) / std::pow(Wg + 2 * Y, 0.67) / Q;
        if (Wg > 0.99 * g.xs->y_bot &&
            g.xs->type == static_cast<int>(XSectShape::TRAPEZOIDAL) &&
            g.xs->s_bot > 0.0) {
            Wg   = g.xs->y_bot;
            g.sx = 1.0 / g.xs->s_bot;
        }
    }

    // --- flow ratio & area for conventional street gutter
    else if (g.a == 0.0) {
        A  = g.t * g.t * g.sx / 2.0;
        Eo = getGutterFlowRatio(g, Wg);   // flow ratio based on grate width
        if (g.t >= g.t_crown) Qo = g.qfactor * std::pow(g.t_crown, 2.67);
    }

    // --- flow ratio & area for composite street gutter
    else {
        if (g.t <= g.w) A = g.t * g.t * g.sw / 2.0;        // spread within gutter
        else            A = (g.t * g.t * g.sx + g.a * g.w) / 2.0;

        // flow ratio based on gutter width corrected for grate width
        Eo = getGutterFlowRatio(g, g.w);
        if (Eo < 1.0) {
            if (g.t >= g.t_crown)
                Qo = g.qfactor * std::pow(g.t_crown, 2.67) / (1.0 - Eo);
            Eo = Eo * getGutterAreaRatio(g, Wg, A);        // HEC-22 Eq(4-20a)
        }
    }

    // --- flow and splash-over velocities
    const double V = Qo / A;
    double Vo;
    if (d.grate_type < 0 || d.grate_type == static_cast<int>(GrateType::GENERIC))
        Vo = d.splash_veloc;
    else
        Vo = getSplashOverVelocity(d.grate_type, Lg);

    // --- frontal flow capture efficiency
    if (V > Vo) Rf = 1.0 - 0.09 * (V - Vo);                // HEC-22 Eq(4-18)

    // --- side flow capture efficiency
    if (Eo < 1.0)
        Rs = 1.0 / (1.0 + (0.15 * std::pow(V, 1.8) /
             g.sx / std::pow(Lg, 2.3)));                   // HEC-22 Eq(4-19)

    return Q * (Rf * Eo + Rs * (1.0 - Eo));                // HEC-22 Eq(4-21)
}

double getCurbInletCapture(const Geom& g, double Q, double L) {
    // inlet.c:1477
    double Se = g.sx;     // equivalent gutter slope
    double E  = 1.0;      // capture efficiency

    // --- for depressed gutter section
    if (g.a > 0.0) {
        const double Sr = g.sw / g.sx;
        const double Eo = getEo(Sr, g.t - g.w, g.w);
        Se = g.sx + (g.a / g.w) * Eo;                      // HEC-22 Eq(4-24)
    }

    // --- opening length for full capture
    const double Lt = 0.6 * std::pow(Q, 0.42) * std::pow(g.sl, 0.3) *
                      std::pow(1.0 / (g.n * Se), 0.6);     // HEC-22 Eq(4-22a)

    // --- capture efficiency for actual opening length
    if (L < Lt) {
        E = 1.0 - (L / Lt);
        E = 1 - std::pow(E, 1.8);                          // HEC-22 Eq(4-23)
    }
    E = std::min(E, 1.0);
    E = std::max(E, 0.0);
    return E * Q;
}

double getOnGradeInletCapture(const Design& d, Geom& g, double Q, double depth) {
    // inlet.c:1325
    double Q1 = Q, Qc = 0.0;

    // --- drop curb inlet (in non-Street conduit) only operates in on-sag mode
    if (d.type == static_cast<int>(InletType::DROP_CURB)) {
        Qc = getOnSagInletCapture(d, g, depth);
        return std::min(Qc, Q);
    }

    // --- drop grate inlet (in non-Street conduit)
    if (d.type == static_cast<int>(InletType::DROP_GRATE)) {
        Qc = getGrateInletCapture(d, g, Q);
        return std::min(Qc, Q);
    }

    // --- remaining inlet types apply to Street conduits
    g.t = getFlowSpread(g, Q);

    // --- slotted inlet (behaves as a curb opening inlet per HEC-22)
    if (d.type == static_cast<int>(InletType::SLOTTED)) {
        Qc = getCurbInletCapture(g, Q, d.slotted_length);
        return std::min(Qc, Q);
    }

    const double Lcurb  = d.curb_length;
    const double Lgrate = d.grate_length;

    // --- curb opening inlet (the sweeper length ahead of the grate)
    if (Lcurb > 0.0) {
        const double Lsweep = Lcurb - Lgrate;
        if (Lsweep > 0.0) {
            Qc = getCurbInletCapture(g, Q1, Lsweep);
            Q1 -= Qc;
        }
    }

    // --- grate inlet
    if (Lgrate > 0.0 && Q1 > 0.0) {
        if (Q1 != Q) g.t = getFlowSpread(g, Q1);
        Qc += getGrateInletCapture(d, g, Q1);
    }
    return Qc;
}

double getOnGradeCapturedFlow(const Design& d, const UsageParams& u, Geom& g,
                              double q, double depth) {
    // inlet.c:1267 — replicate inlets in series: each one sees the flow the
    // previous ones bypassed.
    if (u.num_inlets == 0) return 0.0;

    double qApproach = q;
    if (qApproach < MIN_RUNOFF_FLOW) return 0.0;

    // --- adjust flow for 2-sided street
    qApproach /= g.nsides;
    double qBypassed = qApproach;
    double qCaptured = 0.0;

    // --- set limit on max. flow captured per inlet
    double qMax = INLET_BIG;
    if (u.flow_limit > 0.0) qMax = u.flow_limit;

    for (int i = 1; i <= u.num_inlets; ++i) {
        double qc = getOnGradeInletCapture(d, g, qBypassed, depth) * u.clog_factor;
        qc = std::min(qc, qMax);
        qc = std::min(qc, qBypassed);
        qCaptured += qc;
        qBypassed -= qc;
        if (qBypassed < MIN_RUNOFF_FLOW) break;
    }
    return qCaptured * g.nsides;
}

void findOnSagGrateFlows(const Design& d, const Geom& g, double depth,
                         double* Qw, double* Qo) {
    // inlet.c:1644
    const double Lg = d.grate_length;
    double Wg = d.grate_width;
    double P;      // grate perimeter (ft)
    double di;     // average flow depth across grate (ft)

    // --- for drop grate inlets
    if (d.type == static_cast<int>(InletType::DROP_GRATE)) {
        di = depth;
        P  = 2.0 * (Lg + Wg);
    }
    // --- for gutter grate inlets
    else {
        if (depth <= Wg * g.sw) Wg = depth / g.sw;   // spread within grate width
        di = depth - (Wg / 2.0) * g.sw;              // average depth over grate
        P  = Lg + 2.0 * Wg;                          // effective grate perimeter
    }

    const double Ao = Lg * Wg * grateOpenRatio(d);   // grate opening area (ft2)

    // --- weir flow applies (below the depth where the weir and orifice
    //     equations agree), otherwise orifice flow
    if (depth <= 1.79 * Ao / P)
        *Qw = 3.0 * P * std::pow(di, 1.5);           // HEC-22 Eq(4-26)
    else
        *Qo = 0.67 * Ao * std::sqrt(2.0 * 32.16 * di); // HEC-22 Eq(4-27)
}

double getCurbOrificeFlow(double di, double h, double L, int throat_angle) {
    // inlet.c:1764
    double d = di;
    if (throat_angle == static_cast<int>(ThroatAngle::HORIZONTAL))
        d = di - h / 2.0;
    else if (throat_angle == static_cast<int>(ThroatAngle::INCLINED))
        d = di - (h / 2.0) * 0.7071;
    return 0.67 * h * L * std::sqrt(2.0 * 32.16 * d);  // HEC-22 Eq(4-31a)
}

void findOnSagCurbFlows(const Design& d, const Geom& g, double depth, double L,
                        double* Qw, double* Qo) {
    // inlet.c:1703
    const int    throat = d.curb_throat;
    const double h      = d.curb_height;
    double Qweir = 0.0, dweir;

    if (L <= 0.0) return;
    // A drop curb opening in a channel is open on all four sides.
    if (d.type == static_cast<int>(InletType::DROP_CURB)) L = L * 4.0;

    // --- check for orifice flow
    const double dorif = 1.4 * h;
    if (depth > dorif) {
        *Qo = getCurbOrificeFlow(depth, h, L, throat);
        return;
    }

    // --- for uniform cross slope or very long opening
    if (g.a == 0.0 || L > 12.0) {
        dweir = h;
        if (depth < dweir) {
            *Qw = 3.0 * L * std::pow(depth, 1.5);          // HEC-22 Eq(4-30)
            return;
        }
        Qweir = 3.0 * L * std::pow(dweir, 1.5);
    }
    // --- for depressed gutter
    else {
        const double P = L + 1.8 * g.w;
        dweir = h + g.a;
        if (depth < dweir) {
            *Qw = 2.3 * P * std::pow(depth, 1.5);          // HEC-22 Eq(4-28)
            return;
        }
        Qweir = 2.3 * P * std::pow(dweir, 1.5);
    }

    // --- interpolate between Qweir at dweir and Qorif at dorif
    const double Qorif = getCurbOrificeFlow(dorif, h, L, throat);
    const double r = (depth - dweir) / (dorif - dweir);
    *Qw = (1.0 - r) * Qweir;
    *Qo = r * Qorif;
}

double getOnSagSlottedFlow(const Design& d, double depth) {
    // inlet.c:1785 — weir flow = orifice flow at d = 2.587 · inlet width.
    const double L = d.slotted_length;
    const double w = d.slotted_width;
    if (depth <= 2.587 * w)
        return 2.48 * L * std::pow(depth, 1.5);            // HEC-22 Eq(4-32)
    return 0.8 * L * w * std::sqrt(64.32 * depth);         // HEC-22 Eq(4-33)
}

double getOnSagInletCapture(const Design& d, const Geom& g, double depth) {
    // inlet.c:1610
    double Qsw = 0.0,   // sweeper curb opening weir flow
           Qso = 0.0,   // sweeper curb opening orifice flow
           Qgw = 0.0,   // grate weir flow
           Qgo = 0.0,   // grate orifice flow
           Qcw = 0.0,   // curb opening weir flow (deliberately not summed)
           Qco = 0.0;   // curb opening orifice flow

    if (d.slotted_length > 0.0) return getOnSagSlottedFlow(d, depth);

    const double Lgrate = d.grate_length;
    if (Lgrate > 0.0) findOnSagGrateFlows(d, g, depth, &Qgw, &Qgo);

    const double Lcurb = d.curb_length;
    if (Lcurb > 0.0) {
        const double Lsweep = Lcurb - Lgrate;
        if (Lsweep > 0.0) findOnSagCurbFlows(d, g, depth, Lsweep, &Qsw, &Qso);
        if (Qgo > 0.0)    findOnSagCurbFlows(d, g, depth, Lgrate, &Qcw, &Qco);
    }
    // Qcw is excluded: once the grate is in orifice mode the curb opening over
    // the grate can only add its orifice share (legacy inlet.c:1639).
    (void)Qcw;
    return Qgw + Qgo + Qsw + Qso + Qco;
}

double getOnSagCapturedFlow(const Design& d, const UsageParams& u,
                            const Geom& g, double depth) {
    // inlet.c:1574
    if (u.num_inlets == 0) return 0.0;
    const int totalInlets = g.nsides * u.num_inlets;

    double qMax = INLET_BIG;
    if (u.flow_limit > 0.0) qMax = u.flow_limit;

    double qCaptured = getOnSagInletCapture(d, g, std::fabs(depth));
    qCaptured *= u.clog_factor;
    qCaptured = std::min(qCaptured, qMax);
    qCaptured *= static_cast<double>(totalInlets);
    return qCaptured;
}

double getCustomCapturedFlow(const UsageParams& u, const Geom& g,
                             const Table& curve, int curve_kind,
                             double q, double depth,
                             double ucf_flow, double ucf_len) {
    // inlet.c:1902
    if (u.num_inlets == 0) return 0.0;

    double qMax = INLET_BIG;
    if (u.flow_limit > 0.0) qMax = u.flow_limit;

    // --- adjust flow for 2-sided street (1 for a non-Street conduit)
    const int sides = g.nsides;
    double qBypassed = q / sides;
    double qCaptured = 0.0;

    if (curve_kind == static_cast<int>(CurveKind::DIVERSION)) {
        // --- curve is captured flow v. approach flow
        for (int j = 1; j <= u.num_inlets; ++j) {
            double qIncrement = u.clog_factor *
                table_lookupEx(curve, qBypassed * ucf_flow) / ucf_flow;
            qIncrement = std::min(qIncrement, qMax);
            qIncrement = std::min(qIncrement, qBypassed);
            qCaptured += qIncrement;
            qBypassed -= qIncrement;
            if (qBypassed < MIN_RUNOFF_FLOW) break;
        }
    } else if (curve_kind == static_cast<int>(CurveKind::RATING)) {
        // --- curve is captured flow v. downstream node depth
        qCaptured = u.num_inlets * u.clog_factor *
            table_lookupEx(curve, depth * ucf_len) / ucf_flow;
    }
    return qCaptured * sides;
}

double getInletArea(const Design& d, const UsageParams& u) {
    // inlet.c:1870
    double area = 0.0;
    if (d.grate_length > 0.0)
        area = d.grate_length * d.grate_width * grateOpenRatio(d);

    const double curbLength = d.curb_length - d.grate_length;
    if (curbLength > 0.0) area += curbLength * d.curb_height;

    if (d.slotted_length > 0.0) area = d.slotted_length * d.slotted_width;

    return area * u.num_inlets * u.clog_factor;
}

// ============================================================================
// InletSolver — per-row accessors
// ============================================================================

Design InletSolver::designOf(int ii) const {
    const auto ui = static_cast<std::size_t>(ii);
    Design d;
    d.type           = soa_.inlet_type[ui];
    d.grate_type     = soa_.grate_type[ui];
    d.grate_length   = soa_.grate_length[ui];
    d.grate_width    = soa_.grate_width[ui];
    d.frac_open_area = soa_.open_area[ui];
    d.splash_veloc   = soa_.splash_veloc[ui];
    d.curb_length    = soa_.curb_length[ui];
    d.curb_height    = soa_.curb_height[ui];
    d.curb_throat    = soa_.curb_throat[ui];
    d.slotted_length = soa_.slotted_length[ui];
    d.slotted_width  = soa_.slotted_width[ui];
    return d;
}

UsageParams InletSolver::usageOf(int ii) const {
    const auto ui = static_cast<std::size_t>(ii);
    UsageParams u;
    u.num_inlets  = soa_.num_inlets[ui];
    u.clog_factor = soa_.clog_factor[ui];
    u.flow_limit  = soa_.flow_limit[ui];
    return u;
}

Geom InletSolver::geomOf(const SimulationContext& ctx, int ii,
                         const XSectParams* xs) const {
    (void)ctx;
    const auto ui = static_cast<std::size_t>(ii);
    Geom g;
    g.sx      = soa_.sx[ui];
    g.sl      = soa_.slope[ui];
    g.a       = soa_.gutter_depression[ui];
    g.w       = soa_.gutter_width[ui];
    g.n       = soa_.road_roughness[ui];
    g.nsides  = soa_.n_sides[ui];
    g.t_crown = soa_.t_crown[ui];
    g.qfactor = soa_.flow_factor[ui];
    g.xs      = xs;

    // --- add the inlet's local depression to the street's continuous one
    //     (inlet.c:1175-1180)
    if (soa_.local_depress[ui] * soa_.local_width[ui] > 0.0) {
        g.a += soa_.local_depress[ui];
        g.w  = soa_.local_width[ui];
    }

    // --- slope of depressed gutter section (inlet.c:1182-1183)
    g.sw = (g.w * g.a > 0.0) ? g.sx + g.a / g.w : g.sx;

    // --- Beta = 1.486·√SL / n (Conduit[k].beta), used only by the channel
    //     drop-grate branch of getGrateInletCapture.
    g.beta = (g.n > 0.0 && g.sl > 0.0) ? 1.486 * std::sqrt(g.sl) / g.n : 0.0;
    return g;
}

// ============================================================================
// init() — resolve designs, geometry, host topology and placement
// ============================================================================

void InletSolver::init(SimulationContext& ctx) {
    auto& usages = ctx.inlet_usages;
    const int n = usages.count();
    soa_.resize(n);
    if (n == 0) return;

    const int nn = ctx.n_nodes();
    const int nl = ctx.n_links();
    node_inlet_flow_.assign(static_cast<std::size_t>(nn), 0.0);
    node_inflow_.assign(static_cast<std::size_t>(nn), 0.0);

    // Stores keep user (display) units; convert once here (same treatment the
    // street transect builder gives StreetStore, PostParseResolver.cpp:2287).
    const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    const double inv_len  = ucf::Ucf_inv[ucf::LENGTH][static_cast<std::size_t>(us)];
    const double inv_flow = ucf::Qcf_inv[static_cast<std::size_t>(ctx.options.flow_units)];

    // Legacy Node[].degree = outflow-link count (toposort.c:70-91), consulted
    // by getInletPlacement for AUTOMATIC placement. ctx.nodes.degree is the DW
    // hybrid (both ends counted), so rebuild the legacy quantity here.
    std::vector<int> out_degree(static_cast<std::size_t>(nn), 0);
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        int m = (ctx.links.direction[uj] < 0) ? ctx.links.node2[uj]
                                              : ctx.links.node1[uj];
        if (m < 0 || m >= nn) continue;
        if (ctx.nodes.type[static_cast<std::size_t>(m)] == NodeType::OUTFALL) {
            m = (ctx.links.direction[uj] < 0) ? ctx.links.node1[uj]
                                              : ctx.links.node2[uj];
            if (m < 0 || m >= nn) continue;
        }
        out_degree[static_cast<std::size_t>(m)]++;
    }

    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const int li = usages.link_index[ui];
        const int nh = usages.node_host[ui];
        const int di = usages.design_index[ui];

        soa_.link_idx[ui]      = li;
        soa_.host_node[ui]     = nh;
        soa_.node_idx[ui]      = usages.node_index[ui];
        soa_.num_inlets[ui]    = usages.num_inlets[ui];
        soa_.clog_factor[ui]   = usages.clog_factor[ui];
        soa_.flow_limit[ui]    = usages.flow_limit[ui] * inv_flow;
        soa_.local_depress[ui] = usages.local_depress[ui] * inv_len;
        soa_.local_width[ui]   = usages.local_width[ui] * inv_len;
        soa_.placement[ui]     = usages.placement[ui];

        // ------------------------------------------------------------------
        // Design (InletStore rows are in user units)
        // ------------------------------------------------------------------
        if (di >= 0 && di < ctx.inlets.count()) {
            const auto ud = static_cast<std::size_t>(di);
            const std::string& type_str = ctx.inlets.inlet_type[ud];
            int itype = static_cast<int>(InletType::CUSTOM);
            if      (type_str == "GRATE")      itype = static_cast<int>(InletType::GRATE);
            else if (type_str == "CURB")       itype = static_cast<int>(InletType::CURB);
            else if (type_str == "COMBO")      itype = static_cast<int>(InletType::COMBO);
            else if (type_str == "SLOTTED")    itype = static_cast<int>(InletType::SLOTTED);
            else if (type_str == "DROP_GRATE") itype = static_cast<int>(InletType::DROP_GRATE);
            else if (type_str == "DROP_CURB")  itype = static_cast<int>(InletType::DROP_CURB);
            else if (type_str == "CUSTOM")     itype = static_cast<int>(InletType::CUSTOM);
            soa_.inlet_type[ui] = itype;

            const std::string& gt_str = ctx.inlets.grate_type[ud];
            int gt = static_cast<int>(GrateType::GENERIC);
            if      (gt_str == "P_BAR-50")     gt = static_cast<int>(GrateType::P_BAR_50);
            else if (gt_str == "P_BAR-50x100") gt = static_cast<int>(GrateType::P_BAR_50x100);
            else if (gt_str == "P_BAR-30")     gt = static_cast<int>(GrateType::P_BAR_30);
            else if (gt_str == "CURVED_VANE")  gt = static_cast<int>(GrateType::CURVED_VANE);
            else if (gt_str == "TILT_BAR-45")  gt = static_cast<int>(GrateType::TILT_BAR_45);
            else if (gt_str == "TILT_BAR-30")  gt = static_cast<int>(GrateType::TILT_BAR_30);
            else if (gt_str == "RETICULINE")   gt = static_cast<int>(GrateType::RETICULINE);
            soa_.grate_type[ui] = gt;

            // `length`/`width` are the grate dimensions for the grate family
            // and the slot dimensions for SLOTTED (InfraData.hpp column map).
            if (itype == static_cast<int>(InletType::SLOTTED)) {
                soa_.slotted_length[ui] = ctx.inlets.length[ud] * inv_len;
                soa_.slotted_width[ui]  = ctx.inlets.width[ud]  * inv_len;
            } else if (itype == static_cast<int>(InletType::GRATE) ||
                       itype == static_cast<int>(InletType::DROP_GRATE) ||
                       itype == static_cast<int>(InletType::COMBO)) {
                soa_.grate_length[ui] = ctx.inlets.length[ud] * inv_len;
                soa_.grate_width[ui]  = ctx.inlets.width[ud]  * inv_len;
            }
            soa_.open_area[ui]    = ctx.inlets.open_area[ud];
            soa_.splash_veloc[ui] = ctx.inlets.splash_veloc[ud] * inv_len;

            if (itype == static_cast<int>(InletType::CURB) ||
                itype == static_cast<int>(InletType::DROP_CURB) ||
                itype == static_cast<int>(InletType::COMBO)) {
                soa_.curb_length[ui] = ctx.inlets.curb_length[ud] * inv_len;
                soa_.curb_height[ui] = ctx.inlets.curb_height[ud] * inv_len;
                soa_.curb_throat[ui] = ctx.inlets.curb_throat[ud];
            }

            soa_.curve_index[ui] = ctx.inlets.curve_index[ud];
            soa_.curve_kind[ui]  = ctx.inlets.curve_kind[ud];
        }

        // ------------------------------------------------------------------
        // Host topology: bypass node, approach conduit(s)
        // ------------------------------------------------------------------
        int bypass = -1, up = -1, up2 = -1;
        if (nh >= 0 && nh < nn) {
            // Inlet junction: the host node is its own bypass node. Its two
            // attached conduits are found by scan; node1/node2 are already the
            // post-reversal orientation (PostParseResolver.cpp:2539 swaps the
            // ends of an adverse-slope conduit under DW/FV), so "flows toward
            // the host" is exactly node2 == host.
            bypass = nh;
            for (int j = 0; j < nl && up2 < 0; ++j) {
                const auto uj = static_cast<std::size_t>(j);
                if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
                if (ctx.links.node2[uj] != nh) continue;
                if (up < 0) up = j; else up2 = j;
            }
            if (up < 0) {
                // Both conduits leave the node (a crest): fall back to either
                // one for geometry; the approach flow is then 0 by definition.
                for (int j = 0; j < nl; ++j) {
                    const auto uj = static_cast<std::size_t>(j);
                    if (ctx.links.type[uj] == LinkType::CONDUIT &&
                        ctx.links.node1[uj] == nh) { up = j; break; }
                }
            }
            if (up2 >= 0) {
                soa_.two_inbound[ui] = 1;
                // Geometry comes from the steeper of the two approaches
                // (plan §4 item 5 — the one that governs the gutter flow).
                const int ca  = ctx.link_subtypes.conduit_row(up);
                const int cb  = ctx.link_subtypes.conduit_row(up2);
                const double sa = (ca >= 0) ? ctx.link_subtypes.conduits
                                      .slope[static_cast<std::size_t>(ca)] : 0.0;
                const double sb = (cb >= 0) ? ctx.link_subtypes.conduits
                                      .slope[static_cast<std::size_t>(cb)] : 0.0;
                if (sb > sa) std::swap(up, up2);
            }
        } else if (li >= 0 && li < nl) {
            // Conduit-attribute usage: legacy books bypass at node2 of the
            // host link — post-reversal, so an adverse conduit's bypass is its
            // true downstream end (plan G7).
            bypass = ctx.links.node2[static_cast<std::size_t>(li)];
            up = li;
        }
        soa_.bypass_node[ui] = bypass;
        soa_.up_link[ui]     = up;
        soa_.up_link2[ui]    = up2;

        // ------------------------------------------------------------------
        // Street / channel geometry (inlet.c:1147-1196)
        // ------------------------------------------------------------------
        const int si = usages.street_index[ui];
        if (si >= 0 && si < ctx.streets.count()) {
            const auto su = static_cast<std::size_t>(si);
            soa_.sx[ui]                = ctx.streets.sx[su] / 100.0;  // % → fraction
            soa_.gutter_depression[ui] = ctx.streets.gutter_depres[su] * inv_len;
            soa_.gutter_width[ui]      = ctx.streets.gutter_width[su] * inv_len;
            soa_.road_roughness[ui]    = ctx.streets.n_road[su];
            soa_.n_sides[ui]           = ctx.streets.sides[su];
            soa_.t_crown[ui]           = ctx.streets.t_crown[su] * inv_len;
        } else {
            // Rectangular / trapezoidal channel defaults (inlet.c:1188-1195).
            // PARITY NOTE: legacy leaves the shared Tcrown at whatever the
            // previous street inlet set it to; use an unreachable spread
            // instead so the result does not depend on evaluation order.
            const int cr = (up >= 0) ? ctx.link_subtypes.conduit_row(up) : -1;
            soa_.sx[ui]                = 0.01;
            soa_.gutter_depression[ui] = 0.0;
            soa_.gutter_width[ui]      = 0.0;
            soa_.road_roughness[ui]    = (cr >= 0)
                ? ctx.link_subtypes.conduits.roughness[static_cast<std::size_t>(cr)]
                : 0.01;
            soa_.n_sides[ui]           = 1;
            soa_.t_crown[ui]           = 100.0;
        }

        // Longitudinal slope of the approach conduit + Izzard's flow factor.
        {
            const int cr = (up >= 0) ? ctx.link_subtypes.conduit_row(up) : -1;
            const double sl = (cr >= 0)
                ? ctx.link_subtypes.conduits.slope[static_cast<std::size_t>(cr)]
                : 0.0;
            soa_.slope[ui] = sl;
            // inlet.c:520 — pow(Sx, 1.67), NOT the exact 5/3 exponent.
            soa_.flow_factor[ui] = (soa_.road_roughness[ui] > 0.0 && sl > 0.0)
                ? (0.56 / soa_.road_roughness[ui]) * std::pow(sl, 0.5)
                  * std::pow(soa_.sx[ui], 1.67)
                : 0.0;
        }

        // ------------------------------------------------------------------
        // Placement (inlet.c:1129-1143 for conduit hosts; plan D-E3 for nodes)
        // ------------------------------------------------------------------
        uint8_t sag = 0;
        if (soa_.placement[ui] == static_cast<int>(Placement::ON_SAG)) {
            sag = 1;
        } else if (soa_.placement[ui] == static_cast<int>(Placement::ON_GRADE)) {
            sag = 0;
        } else if (nh >= 0) {
            // AUTOMATIC at an inlet junction: legacy's "degree > 0 ⇒ ON_GRADE"
            // would make every inlet junction on-grade (it always has a
            // downstream conduit). Sag iff both attached conduits fall toward
            // the node, i.e. both far-end inverts sit above the host invert.
            const double z = ctx.nodes.invert_elev[static_cast<std::size_t>(nh)];
            int n_toward = 0, n_attached = 0;
            for (int j = 0; j < nl; ++j) {
                const auto uj = static_cast<std::size_t>(j);
                if (ctx.links.type[uj] != LinkType::CONDUIT) continue;
                const int a = ctx.links.node1[uj], b = ctx.links.node2[uj];
                int far = -1;
                if (a == nh) far = b; else if (b == nh) far = a; else continue;
                ++n_attached;
                if (far >= 0 && far < nn &&
                    ctx.nodes.invert_elev[static_cast<std::size_t>(far)]
                        > z + constants::FUDGE)
                    ++n_toward;
            }
            sag = (n_attached > 0 && n_toward == n_attached) ? 1 : 0;
        } else if (bypass >= 0 && bypass < nn) {
            sag = (out_degree[static_cast<std::size_t>(bypass)] > 0) ? 0 : 1;
        }
        soa_.is_sag[ui] = sag;
    }

    computeBackflowRatios();
}

// ============================================================================
// computeAll() — capture, backflow and the lateral-flow coupling
// ============================================================================

void InletSolver::computeAll(SimulationContext& ctx, double dt) {
    const int ni = soa_.count;
    if (ni == 0) return;

    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    const int nn = ctx.n_nodes();
    const int nl = ctx.n_links();
    const bool is_dw = (ctx.options.routing_model == RoutingModel::DYNWAVE);
    const bool report_on = (ctx.current_date > ctx.options.report_start);

    const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    const double ucf_len  = ucf::Ucf[ucf::LENGTH][static_cast<std::size_t>(us)];
    const double ucf_flow = ucf::Qcf[static_cast<std::size_t>(ctx.options.flow_units)];

    // Sized in init(); re-sized only if the model grew through the edit API.
    if (static_cast<int>(node_inlet_flow_.size()) < nn) {
        node_inlet_flow_.assign(static_cast<std::size_t>(nn), 0.0);
        node_inflow_.assign(static_cast<std::size_t>(nn), 0.0);
    }
    std::fill(node_inlet_flow_.begin(), node_inlet_flow_.end(), 0.0);

    // --- For non-DW routing find conduit flow into each node (used to limit
    //     the max. amount of on-sag capture). Legacy writes this into
    //     Node[].inflow (inlet.c:572-579); keep it in a scratch column so the
    //     routing solvers' own node ledger is untouched.
    if (!is_dw) {
        for (int j = 0; j < nn; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            node_inflow_[uj] = std::max(0.0, nodes.lat_flow[uj]);
        }
        for (int j = 0; j < nl; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            const int n2 = links.node2[uj];
            if (n2 >= 0 && n2 < nn)
                node_inflow_[static_cast<std::size_t>(n2)] +=
                    std::max(0.0, links.flow[uj]);
        }
    }

    // ---- first pass: capture + backflow -----------------------------------
    for (int ii = 0; ii < ni; ++ii) {
        const auto ui = static_cast<std::size_t>(ii);
        soa_.flow_capture[ui] = 0.0;
        soa_.q_approach[ui]   = 0.0;
        soa_.backflow[ui]     = 0.0;

        const int host    = soa_.bypass_node[ui];
        const int capture = soa_.node_idx[ui];
        if (host < 0 || host >= nn) continue;
        if (capture < 0 || capture >= nn) continue;

        const int up  = soa_.up_link[ui];
        const int up2 = soa_.up_link2[ui];

        // --- approach flow
        double q = 0.0;
        if (soa_.host_node[ui] >= 0) {
            // Inlet junction: only conduits carrying flow TOWARD the node
            // contribute (plan D-E8); both of them at a true sag.
            if (up >= 0 && links.node2[static_cast<std::size_t>(up)] == host)
                q += std::max(0.0, links.flow[static_cast<std::size_t>(up)]);
            if (soa_.two_inbound[ui] && up2 >= 0)
                q += std::max(0.0, links.flow[static_cast<std::size_t>(up2)]);
        } else if (up >= 0) {
            q = std::fabs(links.flow[static_cast<std::size_t>(up)]);
        }
        soa_.q_approach[ui] = q;

        const double depth = nodes.depth[static_cast<std::size_t>(host)];

        const Design      d = designOf(ii);
        const UsageParams u = usageOf(ii);

        // The channel cross-section is needed only by the DROP_* designs.
        XSectParams xs{};
        const XSectParams* xsp = nullptr;
        if (up >= 0 &&
            (d.type == static_cast<int>(InletType::DROP_GRATE) ||
             d.type == static_cast<int>(InletType::DROP_CURB))) {
            xs  = link::buildXSectParams(links, static_cast<std::size_t>(up),
                                         &ctx.transect_tables);
            xsp = &xs;
        }
        Geom g = geomOf(ctx, ii, xsp);

        double qc = 0.0;
        if (d.type == static_cast<int>(InletType::CUSTOM)) {
            const int ci = soa_.curve_index[ui];
            if (ci >= 0 && ci < static_cast<int>(ctx.tables.tables.size()))
                qc = getCustomCapturedFlow(u, g,
                        ctx.tables.tables[static_cast<std::size_t>(ci)],
                        soa_.curve_kind[ui], q, depth, ucf_flow, ucf_len);
        } else if (soa_.is_sag[ui] == 0) {
            qc = getOnGradeCapturedFlow(d, u, g, q, depth);
        } else {
            // On-sag capture is depth-driven; the approach flow only caps it
            // (second pass). Under non-DW routing legacy feeds the node's
            // accumulated inflow instead of the link flow.
            qc = getOnSagCapturedFlow(d, u, g, depth);
        }
        if (std::fabs(qc) < INLET_FUDGE) qc = 0.0;
        soa_.flow_capture[ui] = qc;
        node_inlet_flow_[static_cast<std::size_t>(host)] += qc;

        // --- capture node's overflow becomes the inlet's backflow
        double qbf = nodes.overflow[static_cast<std::size_t>(capture)]
                     * soa_.backflow_ratio[ui];
        if (std::fabs(qbf) < INLET_FUDGE) qbf = 0.0;
        soa_.backflow[ui] = qbf;
    }

    // ---- second pass: throttle, couple, accumulate -------------------------
    for (int ii = 0; ii < ni; ++ii) {
        const auto ui = static_cast<std::size_t>(ii);
        const int host    = soa_.bypass_node[ui];
        const int capture = soa_.node_idx[ui];
        if (host < 0 || host >= nn) continue;
        if (capture < 0 || capture >= nn) continue;
        const auto uh = static_cast<std::size_t>(host);

        // For on-sag placement under non-DW routing, captured flow is limited
        // to the inlet's share of the bypass node's inflow plus stored volume
        // (inlet.c:633-639).
        if (!is_dw && soa_.is_sag[ui] != 0 && soa_.host_node[ui] < 0) {
            double qlim = nodes.volume[uh] / dt;
            qlim += std::max(node_inflow_[uh], 0.0);
            if (node_inlet_flow_[uh] > qlim)
                soa_.flow_capture[ui] *= qlim / node_inlet_flow_[uh];
        }

        // Plan D-E4: an inlet junction is a (near) zero-storage node — the
        // sink must never exceed what actually reaches it, in any routing
        // model, or the node is driven negative.
        if (soa_.host_node[ui] >= 0) {
            const double qlim = soa_.q_approach[ui] + nodes.volume[uh] / dt;
            soa_.flow_capture[ui] = std::min(soa_.flow_capture[ui], qlim);
        }

        const double qcap = soa_.flow_capture[ui];
        const double qbf  = soa_.backflow[ui];

        // Adjust lateral flows: subtract the captured flow from the bypass
        // node, add it to the capture node, return any backflow to the bypass.
        nodes.lat_flow[uh] -= (qcap - qbf);
        nodes.lat_flow[static_cast<std::size_t>(capture)] += qcap;

        // Volume statistics (Gap #68) — cumulative over the whole run.
        const double qnet = qcap - qbf;
        soa_.stat_capture_vol[ui]  += qcap * dt;
        soa_.stat_bypass_vol[ui]   += std::max(0.0, soa_.q_approach[ui] - qnet) * dt;
        soa_.stat_backflow_vol[ui] += qbf * dt;
        if (qcap > soa_.stat_peak_flow[ui]) soa_.stat_peak_flow[ui] = qcap;

        // Legacy performance statistics, gated on the reporting window
        // (inlet.c:648-649).
        if (report_on) updateStats(ii, soa_.q_approach[ui]);
    }
}

// ============================================================================
// updateStats() — legacy updateInletStats (inlet.c:985)
// ============================================================================

void InletSolver::updateStats(int ii, double q) {
    const auto ui = static_cast<std::size_t>(ii);
    const double qCapture  = soa_.flow_capture[ui];
    const double qBackflow = soa_.backflow[ui];
    const double qNet      = qCapture - qBackflow;
    const double qBypass   = q - qNet;
    double fCapture = 0.0;

    // --- check for no flow condition
    if (q < MIN_RUNOFF_FLOW && qBackflow <= 0.0) return;
    soa_.flow_periods[ui]++;

    // --- there is positive net flow from inlet to capture node
    if (qNet > 0.0) {
        soa_.capture_periods[ui]++;
        fCapture = qNet / q;
        fCapture = std::min(fCapture, 1.0);
        soa_.avg_flow_capture[ui] += fCapture;
        if (qBypass > MIN_RUNOFF_FLOW) soa_.bypass_freq[ui] += 1.0;
    }
    // --- otherwise inlet receives backflow from capture node
    else soa_.backflow_periods[ui]++;

    // --- update peak flow stats
    if (q > soa_.peak_flow[ui]) {
        soa_.peak_flow[ui] = q;
        soa_.peak_flow_capture[ui] = fCapture * 100.0;
    }
}

// ============================================================================
// computeBackflowRatios() — legacy getBackflowRatios (inlet.c:1805)
// ============================================================================
//
// PARITY NOTE (plan D-E6): legacy assigns the capture-node index to the
// file-scope Manning roughness `n` and then indexes its per-node accumulator
// with a `nodeIndex` that is never advanced (inlet.c:1836/1853), so every
// inlet's contribution lands in slot 0 and the ratios are wrong whenever the
// model has more than one capture node. Fixed here; recorded as a documented
// divergence. Inlet junctions are counted alongside conduit-hosted usages.

void InletSolver::computeBackflowRatios() {
    const int ni = soa_.count;
    if (ni == 0) return;

    struct NodeInfo {
        int    n_links     = 0;   ///< total inlet usages discharging to this node
        int    n_std_links = 0;   ///< those with a standard (area > 0) inlet
        int    n_custom    = 0;   ///< Σ num_inlets over the custom ones
        double total_area  = 0.0;
    };
    std::vector<std::pair<int, NodeInfo>> node_map;

    auto find_node = [&](int node) -> NodeInfo& {
        for (auto& kv : node_map)
            if (kv.first == node) return kv.second;
        node_map.push_back({node, NodeInfo{}});
        return node_map.back().second;
    };

    for (int ii = 0; ii < ni; ++ii) {
        const int m = soa_.node_idx[static_cast<std::size_t>(ii)];
        if (m < 0) continue;
        NodeInfo& info = find_node(m);
        info.n_links++;
        const double area = getInletArea(designOf(ii), usageOf(ii));
        if (area > 0.0) {
            info.n_std_links++;
            info.total_area += area;
        } else {
            info.n_custom += soa_.num_inlets[static_cast<std::size_t>(ii)];
        }
    }

    for (int ii = 0; ii < ni; ++ii) {
        const auto ui = static_cast<std::size_t>(ii);
        soa_.backflow_ratio[ui] = 0.0;
        const int m = soa_.node_idx[ui];
        if (m < 0) continue;

        NodeInfo* info = nullptr;
        for (auto& kv : node_map)
            if (kv.first == m) { info = &kv.second; break; }
        if (!info || info->n_links == 0) continue;

        // f = ratio of usages with standard inlets to all usages on this node
        const double f = static_cast<double>(info->n_std_links)
                       / static_cast<double>(info->n_links);

        const double area = getInletArea(designOf(ii), usageOf(ii));
        if (area > 0.0) {
            if (info->total_area > 0.0)
                soa_.backflow_ratio[ui] = area / info->total_area * f;
        } else if (info->n_custom > 0) {
            soa_.backflow_ratio[ui] =
                static_cast<double>(soa_.num_inlets[ui])
                / static_cast<double>(info->n_custom) * (1.0 - f);
        }
    }
}

// ============================================================================
// adjustQualInflows() — legacy inlet_adjustQualInflows (inlet.c:655)
// ============================================================================

void InletSolver::adjustQualInflows(SimulationContext& ctx, double dt) {
    const int ni = soa_.count;
    if (ni == 0) return;

    const int np = ctx.n_pollutants();
    if (np == 0) return;

    auto& nodes = ctx.nodes;
    const int nn = ctx.n_nodes();
    const auto unp = static_cast<std::size_t>(np);

    for (int ii = 0; ii < ni; ++ii) {
        const auto ui = static_cast<std::size_t>(ii);
        // Bypass node = the conduit usage's downstream node, or the inlet
        // junction itself — resolved once in init().
        const int bypass  = soa_.bypass_node[ui];
        const int capture = soa_.node_idx[ui];
        if (bypass < 0 || bypass >= nn) continue;
        if (capture < 0 || capture >= nn) continue;

        const double qNet = soa_.flow_capture[ui] - soa_.backflow[ui];
        const auto ub = static_cast<std::size_t>(bypass);
        const auto uc = static_cast<std::size_t>(capture);

        if (qNet > 0.0) {
            // Net flow: bypass → capture
            nodes.qual_vol_in[uc] += qNet * dt;
            for (std::size_t p = 0; p < unp; ++p) {
                const std::size_t nd_idx = uc * unp + p;
                const std::size_t by_idx = ub * unp + p;
                if (nd_idx < nodes.qual_mass_in.size() &&
                    by_idx < nodes.conc_old.size())
                    nodes.qual_mass_in[nd_idx] += qNet * nodes.conc_old[by_idx];
            }
        } else if (qNet < 0.0) {
            // Net backflow: capture → bypass
            const double qBkf = -qNet;
            nodes.qual_vol_in[ub] += qBkf * dt;
            for (std::size_t p = 0; p < unp; ++p) {
                const std::size_t nd_idx = ub * unp + p;
                const std::size_t cp_idx = uc * unp + p;
                if (nd_idx < nodes.qual_mass_in.size() &&
                    cp_idx < nodes.conc_old.size())
                    nodes.qual_mass_in[nd_idx] += qBkf * nodes.conc_old[cp_idx];
            }
        }
    }
}

// ============================================================================
// gatherStats() — publish to InletUsageStore + ctx.inlet_diag for the report
// ============================================================================

void InletSolver::gatherStats(SimulationContext& ctx) const {
    const int n = std::min(soa_.count, ctx.inlet_usages.count());
    if (n <= 0) return;

    ctx.inlet_usages.resize_stats(ctx.inlet_usages.count());
    ctx.inlet_diag.resize(n);

    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        ctx.inlet_usages.stat_capture_vol[ui]  = soa_.stat_capture_vol[ui];
        ctx.inlet_usages.stat_bypass_vol[ui]   = soa_.stat_bypass_vol[ui];
        ctx.inlet_usages.stat_backflow_vol[ui] = soa_.stat_backflow_vol[ui];
        ctx.inlet_usages.stat_peak_flow[ui]    = soa_.stat_peak_flow[ui];

        auto& dg = ctx.inlet_diag;
        dg.host_node[ui]         = soa_.host_node[ui];
        dg.up_link[ui]           = soa_.up_link[ui];
        dg.is_sag[ui]            = soa_.is_sag[ui];
        dg.num_inlets[ui]        = soa_.num_inlets[ui];
        dg.flow_periods[ui]      = soa_.flow_periods[ui];
        dg.capture_periods[ui]   = soa_.capture_periods[ui];
        dg.backflow_periods[ui]  = soa_.backflow_periods[ui];
        dg.peak_flow[ui]         = soa_.peak_flow[ui];
        dg.peak_flow_capture[ui] = soa_.peak_flow_capture[ui];
        dg.avg_flow_capture[ui]  = soa_.avg_flow_capture[ui];
        dg.bypass_freq[ui]       = soa_.bypass_freq[ui];
    }
}

} // namespace inlet
} // namespace openswmm
