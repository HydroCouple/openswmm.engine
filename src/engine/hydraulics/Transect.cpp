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

} // namespace transect
} // namespace openswmm
