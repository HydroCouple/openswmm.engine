/**
 * @file Transect.cpp
 * @brief Irregular transect — numerically identical to legacy transect.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "Transect.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace openswmm {
namespace transect {

static constexpr double PHI = 1.486;

// Faithful 1:1 port of legacy transect.c (transect_validate + createTables +
// getGeometry + getSliceGeom + getFlow). AREA and WIDTH are the same geometric
// integrals as a simple area/perimeter sweep, but the hydraulic radius is NOT
// area/perimeter — legacy back-solves an *effective* hyd-radius from the
// divided-channel composite-roughness conveyance, so that (used with the main
// channel n) it reproduces the total normal flow. A lumped geometric hrad is
// too SMALL on overbank / multi-roughness sections (e.g. user5 T171: R=2.46 vs
// legacy 3.22 at full depth), over-stating friction and collapsing the initial
// flow on q0 transect conduits (extran8a). See src/legacy/engine/transect.c.
void buildTables(TransectData& td) {
    int n_in = static_cast<int>(td.stations.size());
    if (n_in < 2 || static_cast<int>(td.elevations.size()) != n_in) return;

    double nChannel = td.n_channel;
    if (nChannel <= 0.0) return;
    double nLeft  = (td.n_left  > 0.0) ? td.n_left  : nChannel;
    double nRight = (td.n_right > 0.0) ? td.n_right : nChannel;

    double lFactor = (td.length_factor > 0.0) ? td.length_factor : 1.0;
    nChannel *= std::sqrt(lFactor);

    const double xLeftBank  = td.x_left_bank;
    const double xRightBank = td.x_right_bank;

    double ymin = td.elevations[0], ymax = td.elevations[0];
    for (int i = 1; i < n_in; ++i) {
        ymin = std::min(ymin, td.elevations[i]);
        ymax = std::max(ymax, td.elevations[i]);
    }
    td.y_full = ymax - ymin;
    if (td.y_full <= 0.0) return;

    // add vertical end-walls reaching full height (legacy transect_validate)
    int N = n_in;
    std::vector<double> X(static_cast<size_t>(N) + 2);
    std::vector<double> Y(static_cast<size_t>(N) + 2);
    X[0] = td.stations[0];              Y[0] = ymax;
    for (int i = 0; i < N; ++i) {
        X[static_cast<size_t>(i + 1)] = td.stations[static_cast<size_t>(i)];
        Y[static_cast<size_t>(i + 1)] = td.elevations[static_cast<size_t>(i)];
    }
    X[static_cast<size_t>(N + 1)] = td.stations[static_cast<size_t>(N - 1)];
    Y[static_cast<size_t>(N + 1)] = ymax;
    const int Nsta = N + 1;

    auto getFlow = [&](int k, double a, double wp, bool findFlow) -> double {
        if (!findFlow) {
            if (k == Nsta - 1) {
                findFlow = true;
            } else if (X[static_cast<size_t>(k)] == xLeftBank) {
                if (nLeft != nChannel &&
                    X[static_cast<size_t>(k)] != X[static_cast<size_t>(k - 1)])
                    findFlow = true;
            } else if (X[static_cast<size_t>(k)] == xRightBank) {
                if (nRight != nChannel &&
                    X[static_cast<size_t>(k)] != X[static_cast<size_t>(k + 1)])
                    findFlow = true;
            }
        }
        if (findFlow) {
            double n = nChannel;
            if (X[static_cast<size_t>(k - 1)] < xLeftBank)  n = nLeft;
            if (X[static_cast<size_t>(k)]     > xRightBank) n = nRight;
            return PHI / n * a * std::pow(a / wp, 2.0 / 3.0);
        }
        return 0.0;
    };

    const double dy = td.y_full / static_cast<double>(N_TRANSECT_TBL - 1);

    td.area_tbl[0] = 0.0;
    td.hrad_tbl[0] = 0.0;
    td.width_tbl[0] = 0.0;

    for (int idx = 1; idx < N_TRANSECT_TBL; ++idx) {
        const double y = ymin + static_cast<double>(idx) * dy;
        double wpSum = 0.0, aSum = 0.0, qSum = 0.0;
        double areaT = 0.0, widthT = 0.0;

        for (int k = 1; k <= Nsta; ++k) {
            const double ek  = Y[static_cast<size_t>(k)];
            const double ekm = Y[static_cast<size_t>(k - 1)];
            const double yhi = std::max(ekm, ek);
            const double ylo = std::min(ekm, ek);
            if (ylo >= y) continue;

            const double width = std::fabs(X[static_cast<size_t>(k)] -
                                           X[static_cast<size_t>(k - 1)]);
            double w  = width;
            double wp = std::sqrt(width * width + (yhi - ylo) * (yhi - ylo));
            double a  = 0.0;
            if (y > yhi) {
                a = width * ((y - yhi) + (y - ylo)) / 2.0;
            } else if (yhi > ylo) {
                const double ratio = (y - ylo) / (yhi - ylo);
                a   = width * (yhi - ylo) / 2.0 * ratio * ratio;
                w  *= ratio;
                wp *= ratio;
            }

            wpSum  += wp;
            aSum   += a;
            areaT  += a;
            widthT += w;

            const bool findFlow = (ek >= y);
            const double q = getFlow(k, aSum, wpSum, findFlow);
            if (q > 0.0) { qSum += q; aSum = 0.0; wpSum = 0.0; }
        }

        td.area_tbl[idx]  = areaT;
        td.width_tbl[idx] = widthT;
        if (areaT == 0.0)
            td.hrad_tbl[idx] = td.hrad_tbl[idx - 1];
        else
            td.hrad_tbl[idx] = std::pow(qSum * nChannel / PHI / areaT, 1.5);
    }

    const int nLast = N_TRANSECT_TBL - 1;
    td.a_full = td.area_tbl[nLast];
    td.r_full = td.hrad_tbl[nLast];
    td.w_max  = td.width_tbl[nLast];

    for (int i = 1; i <= nLast; ++i) {
        if (td.a_full > 0.0) td.area_tbl[i]  /= td.a_full;
        if (td.r_full > 0.0) td.hrad_tbl[i]  /= td.r_full;
        if (td.w_max  > 0.0) td.width_tbl[i] /= td.w_max;
    }
    // width at zero depth = width at first increment (legacy createTables:309)
    td.width_tbl[0] = td.width_tbl[1];
}

void buildCustomTables(TransectData& td, double y_full,
                       const double* curve_x, const double* curve_y, int n_pts) {
    if (n_pts < 2 || y_full <= 0.0) return;

    // ================================================================
    // Match legacy shape.c computeShapeTables() EXACTLY.
    // Work entirely in normalized space (unit height). Scale at end.
    //
    // curve_x = depth/yFull (0 to 1), curve_y = width/yFull (normalized)
    // ================================================================

    td.y_full = y_full;

    // --- Get first curve entry; ensure it starts at (0, 0) ---
    double y1 = curve_x[0], w1 = curve_y[0];
    double y2, w2;
    int ci = 1; // cursor into curve
    double wMax = w1;

    if (y1 != 0.0) {
        y2 = y1; w2 = w1;
        y1 = 0.0; w1 = 0.0;
    } else {
        if (ci < n_pts) {
            y2 = curve_x[ci]; w2 = curve_y[ci]; ci++;
            wMax = std::max(wMax, w2);
        } else return;
    }

    // --- Build tables at N_TRANSECT_TBL equal increments ---
    int n = N_TRANSECT_TBL - 1;
    double dd = 1.0 / static_cast<double>(n);
    double Ptotal = w1;  // initial perimeter = bottom width
    double Atotal = 0.0;

    td.width_tbl[0] = w1;
    td.area_tbl[0]  = 0.0;
    td.hrad_tbl[0]  = 0.0;

    double y = 0.0, w = w1;

    for (int d = 1; d <= n; ++d) {
        double yLast = y;
        double wLast = w;
        y += dd;

        // Clamp to 1.0
        if (std::fabs(y - 1.0) < 1.0e-6) y = 1.0;

        // Advance to next curve interval if needed
        while (y > y2 && ci < n_pts) {
            // Interpolate at interval boundary
            y1 = y2; w1 = w2;
            y2 = curve_x[ci]; w2 = curve_y[ci]; ci++;
            y2 = std::min(y2, 1.0);
            wMax = std::max(wMax, w2);

            // Add partial area/perimeter from yLast to y1
            if (y1 > yLast) {
                double wMid = wLast + (w1 - wLast) * (y1 - yLast) / (y1 - yLast + 1e-30);
                Atotal += 0.5 * (wLast + wMid) * (y1 - yLast);
                double dw_half = std::fabs(wMid - wLast) / 2.0;
                double dy_seg = y1 - yLast;
                Ptotal += 2.0 * std::sqrt(dy_seg * dy_seg + dw_half * dw_half);
                yLast = y1;
                wLast = w1;
            }
        }

        // Interpolate width at depth y
        if (y2 > y1) {
            w = w1 + (w2 - w1) * (y - y1) / (y2 - y1);
        } else {
            w = w2;
        }
        wMax = std::max(wMax, w);

        // Area increment: trapezoidal rule
        Atotal += 0.5 * (wLast + w) * (y - yLast);

        // Perimeter increment
        double dw_half = std::fabs(w - wLast) / 2.0;
        double dy_seg = y - yLast;
        Ptotal += 2.0 * std::sqrt(dy_seg * dy_seg + dw_half * dw_half);

        // Add top width to perimeter at y = 1.0 (matching legacy shape.c line 163)
        if (y >= 1.0) {
            Ptotal += w;
        }

        td.width_tbl[d] = w;
        td.area_tbl[d]  = Atotal;
        td.hrad_tbl[d]  = (Ptotal > 0.0) ? Atotal / Ptotal : 0.0;
    }

    // --- Full-depth properties (in normalized space) ---
    double aFull = td.area_tbl[n];
    double rFull = td.hrad_tbl[n];

    // --- Scale to physical units (matching legacy xsect.c lines 681-683) ---
    // aFull_physical = aFull_normalized * yFull^2
    // rFull_physical = rFull_normalized * yFull
    // wMax_physical  = wMax_normalized * yFull
    td.a_full = aFull * y_full * y_full;
    td.r_full = rFull * y_full;
    td.w_max  = wMax * y_full;

    // --- Normalize tables (matching legacy normalizeShapeTables) ---
    if (aFull > 0.0) {
        for (int d = 0; d <= n; ++d) td.area_tbl[d] /= aFull;
    }
    if (rFull > 0.0) {
        for (int d = 0; d <= n; ++d) td.hrad_tbl[d] /= rFull;
    }
    if (wMax > 0.0) {
        for (int d = 0; d <= n; ++d) td.width_tbl[d] /= wMax;
    }
}

} // namespace transect
} // namespace openswmm
