/**
 * @file Transect.cpp
 * @brief Irregular transect — numerically identical to legacy transect.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Transect.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace openswmm {
namespace transect {

static constexpr double PHI = 1.486;

void buildTables(TransectData& td) {
    int n_sta = static_cast<int>(td.stations.size());
    if (n_sta < 2) return;

    // Find thalweg (minimum elevation)
    double y_min = *std::min_element(td.elevations.begin(), td.elevations.end());
    double y_max = *std::max_element(td.elevations.begin(), td.elevations.end());
    td.y_full = y_max - y_min;
    if (td.y_full <= 0.0) return;

    double dy = td.y_full / static_cast<double>(N_TRANSECT_TBL - 1);

    // Build tables at each depth increment
    for (int d = 0; d < N_TRANSECT_TBL; ++d) {
        double depth = static_cast<double>(d) * dy;
        double wse = y_min + depth;  // water surface elevation

        double area = 0.0;
        double perimeter = 0.0;
        double width = 0.0;

        // Integrate area and perimeter across station pairs
        for (int k = 1; k < n_sta; ++k) {
            auto uk = static_cast<size_t>(k);
            auto ukm = static_cast<size_t>(k - 1);

            double x0 = td.stations[ukm];
            double x1 = td.stations[uk];
            double y0 = td.elevations[ukm];
            double y1 = td.elevations[uk];
            double w = std::fabs(x1 - x0);

            if (y0 >= wse && y1 >= wse) continue;  // both above water

            double ylo = std::min(y0, y1);
            double yhi = std::max(y0, y1);

            if (wse >= yhi) {
                // Fully submerged
                area += w * ((wse - yhi) + (wse - ylo)) / 2.0;
                double dy_seg = yhi - ylo;
                perimeter += std::sqrt(w * w + dy_seg * dy_seg);
                width += w;
            } else if (wse > ylo) {
                // Partially submerged
                double ratio = (wse - ylo) / (yhi - ylo);
                double w_wet = w * ratio;
                double dy_wet = (yhi - ylo) * ratio;
                area += w_wet * (wse - ylo) / 2.0;
                perimeter += std::sqrt(w_wet * w_wet + dy_wet * dy_wet);
                width += w_wet;
            }
        }

        td.area_tbl[d]  = area;
        td.hrad_tbl[d]  = (perimeter > 0.0) ? area / perimeter : 0.0;
        td.width_tbl[d] = width;
    }

    // Full-depth properties
    td.a_full = td.area_tbl[N_TRANSECT_TBL - 1];
    td.r_full = td.hrad_tbl[N_TRANSECT_TBL - 1];
    td.w_max  = *std::max_element(td.width_tbl, td.width_tbl + N_TRANSECT_TBL);

    // Normalize tables
    if (td.a_full > 0.0) {
        for (int d = 0; d < N_TRANSECT_TBL; ++d) td.area_tbl[d] /= td.a_full;
    }
    if (td.r_full > 0.0) {
        for (int d = 0; d < N_TRANSECT_TBL; ++d) td.hrad_tbl[d] /= td.r_full;
    }
    if (td.w_max > 0.0) {
        for (int d = 0; d < N_TRANSECT_TBL; ++d) td.width_tbl[d] /= td.w_max;
    }
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
            if (w2 > wMax) wMax = w2;
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
            if (y2 > 1.0) y2 = 1.0;
            if (w2 > wMax) wMax = w2;

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
        if (w > wMax) wMax = w;

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
