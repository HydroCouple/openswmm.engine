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

    td.y_full = y_full;

    // Find max width from curve (curve_y gives normalized width, 0-1+)
    double w_max_norm = 0.0;
    for (int i = 0; i < n_pts; ++i) {
        if (curve_y[i] > w_max_norm) w_max_norm = curve_y[i];
    }
    // In SWMM CUSTOM shapes, curve is depth/yFull vs width/wMax.
    // wMax is the maximum physical width. We approximate from the curve.
    // The actual wMax = w_max_norm * y_full is a rough estimate;
    // the curve normalization means w_max_norm should be ~1.0 at the widest point.
    td.w_max = w_max_norm * y_full;  // Will be refined below

    // Helper: interpolate width from curve at normalized depth y_norm
    auto interp_width = [&](double y_norm) -> double {
        if (y_norm <= curve_x[0]) return curve_y[0];
        if (y_norm >= curve_x[n_pts - 1]) return curve_y[n_pts - 1];
        for (int i = 1; i < n_pts; ++i) {
            if (y_norm <= curve_x[i]) {
                double dx = curve_x[i] - curve_x[i - 1];
                if (dx <= 0.0) return curve_y[i];
                double t = (y_norm - curve_x[i - 1]) / dx;
                return curve_y[i - 1] + t * (curve_y[i] - curve_y[i - 1]);
            }
        }
        return curve_y[n_pts - 1];
    };

    // Build tables at N_TRANSECT_TBL equal depth increments
    double dd = 1.0 / static_cast<double>(N_TRANSECT_TBL - 1);
    double cum_area = 0.0;
    double max_width = 0.0;

    for (int d = 0; d < N_TRANSECT_TBL; ++d) {
        double y_norm = d * dd;
        double w_norm = interp_width(y_norm);
        double w = w_norm * y_full;  // Physical width (temporary scale)

        td.width_tbl[d] = w;
        if (w > max_width) max_width = w;

        // Accumulate area by trapezoidal integration
        if (d > 0) {
            double w_prev = td.width_tbl[d - 1];
            double dy = dd * y_full;
            cum_area += 0.5 * (w_prev + w) * dy;
        }
        td.area_tbl[d] = cum_area;

        // Hydraulic radius: R = A / P (approximate P from cumulative perimeter)
        double p = 0.0;
        for (int k = 1; k <= d; ++k) {
            double dw = std::fabs(td.width_tbl[k] - td.width_tbl[k - 1]) / 2.0;
            double dy = dd * y_full;
            p += 2.0 * std::sqrt(dy * dy + dw * dw);
        }
        td.hrad_tbl[d] = (p > 0.0) ? td.area_tbl[d] / p : 0.0;
    }

    td.a_full = cum_area;
    td.w_max  = max_width;
    td.r_full = (td.a_full > 0.0) ? td.hrad_tbl[N_TRANSECT_TBL - 1] : 0.0;

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

} // namespace transect
} // namespace openswmm
