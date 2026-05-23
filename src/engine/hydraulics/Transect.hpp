/**
 * @file Transect.hpp
 * @brief Irregular transect cross-section geometry (HEC-2 style).
 *
 * @details Processes station-elevation pairs into tabulated A/R/W arrays
 *          that can be used by the XSectBatch lookup system. Each transect
 *          produces N_TRANSECT_TBL entries (typically 51) of normalized
 *          area, hydraulic radius, and width vs. depth.
 *
 * @note Legacy reference: src/legacy/engine/transect.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_TRANSECT_HPP
#define OPENSWMM_TRANSECT_HPP

#include <vector>
#include <string>

namespace openswmm {

namespace transect {

constexpr int N_TRANSECT_TBL = 51;

struct TransectData {
    std::string name;
    double n_left   = 0.0;   ///< Left overbank Manning's n
    double n_right  = 0.0;   ///< Right overbank Manning's n
    double n_channel = 0.0;  ///< Main channel Manning's n

    // Station-elevation pairs
    std::vector<double> stations;    ///< x positions (ft)
    std::vector<double> elevations;  ///< y elevations (ft)

    double x_left_bank  = 0.0;  ///< Left bank station
    double x_right_bank = 0.0;  ///< Right bank station

    // Computed geometry tables (normalized)
    double y_full = 0.0;
    double w_max  = 0.0;
    double a_full = 0.0;
    double r_full = 0.0;

    double area_tbl[N_TRANSECT_TBL]  = {};
    double hrad_tbl[N_TRANSECT_TBL]  = {};
    double width_tbl[N_TRANSECT_TBL] = {};
};

/**
 * @brief Build tabulated geometry from station-elevation data.
 *
 * @details Computes area, hydraulic radius, and width at each of
 *          N_TRANSECT_TBL equal depth increments from thalweg to
 *          maximum depth, then normalizes.
 *
 * @param td  [in/out] TransectData with stations/elevations filled.
 *            On output, tables and full-depth properties are set.
 */
void buildTables(TransectData& td);

/**
 * @brief Build tabulated geometry from a CUSTOM shape curve.
 *
 * @details The shape curve defines normalized (y/yFull) vs (width/wMax).
 *          This function integrates area and computes hyd-radius tables
 *          that can be used by XSectBatch's per-link lookup kernel.
 *
 * @param td        [out] TransectData to populate (tables and full properties).
 * @param y_full    Full depth of the cross-section.
 * @param curve_x   Normalized depth values (y/yFull) from shape curve.
 * @param curve_y   Normalized width values (w/wMax) from shape curve.
 * @param n_pts     Number of curve points.
 */
void buildCustomTables(TransectData& td, double y_full,
                       const double* curve_x, const double* curve_y, int n_pts);

} // namespace transect
} // namespace openswmm

#endif // OPENSWMM_TRANSECT_HPP
