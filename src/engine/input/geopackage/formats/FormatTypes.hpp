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
 * @file FormatTypes.hpp
 * @brief Plain-data types shared by every external-file format parser.
 *
 * @details Slice IO-6. Each format parser converts bytes-on-disk into the
 *          row types declared here (the same shape the GeoPackage Part D
 *          tables persist). Each materialiser does the inverse: rows out
 *          to legacy-format bytes.
 *
 *          These types stay decoupled from `SimulationContext` so format
 *          tests can exercise the round trip without instantiating the
 *          engine.
 *
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_FORMAT_TYPES_HPP
#define OPENSWMM_GEOPACKAGE_FORMAT_TYPES_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace openswmm::gpkg::formats {

// ============================================================================
// Common row types
// ============================================================================

/** @brief One row of a timeseries (SWMM-native text format). */
struct TimeseriesRow {
    /// OADate-encoded timestamp (decimal days since 12/30/1899).
    double timestamp_oa = 0.0;
    /// Sample value.
    double value = 0.0;
};

/** @brief One row of a raingage data file ("Standard" SWMM columnar). */
struct RaingageRow {
    /// Station identifier — typically a short ASCII tag.
    std::string station_id;
    /// OADate-encoded timestamp.
    double timestamp_oa = 0.0;
    /// Rainfall reading (inches or millimetres per SWMM units convention).
    double rainfall = 0.0;
};

/** @brief One row of a climate file (user CSV form). */
struct ClimateRow {
    /// ISO-8601 date string (yyyy-mm-dd) for daily climate observations.
    std::string record_date;
    double tmin        = 0.0;
    double tmax        = 0.0;
    double evaporation = 0.0;
    double wind_speed  = 0.0;
    double sky_cover   = 0.0;
    double humidity    = 0.0;
};

/** @brief One per-object record in a routing interface file. */
struct RoutingInterfaceRow {
    std::string object_id;         ///< Node, subcatchment, or gage id.
    double      timestamp_oa = 0.0;
    double      flow_value   = 0.0; ///< Flow (or rainfall) value.
    /// Pollutant concentrations parallel to `pollutant_ids` in the
    /// surrounding metadata struct.
    std::vector<double> pollutant_values;
};

/** @brief Header / metadata captured at the top of a routing interface file. */
struct RoutingInterfaceMetadata {
    std::string title;
    int         report_step_sec = 0;
    std::string flow_units;        ///< e.g. "CFS", "CMS", ...
    /// Pollutant ids in file order (column order matches RoutingInterfaceRow::pollutant_values).
    std::vector<std::string> pollutant_ids;
    /// Pollutant units in file order (parallel to pollutant_ids).
    std::vector<std::string> pollutant_units;
    /// Object ids declared in the header (node ids for INFLOWS/OUTFLOWS/RDII).
    std::vector<std::string> object_ids;
};

// ============================================================================
// Hot-start file (HSF v4 binary)
// ============================================================================

/** @brief HSF header / counts (matches legacy hotstart.c layout). */
struct HotstartHeader {
    /// File-stamp string, including the version suffix ("SWMM5-HOTSTART4").
    std::string file_stamp;
    /// Numeric version derived from file_stamp suffix (1..4).
    int         file_version = 4;
    int         num_subcatch = 0;
    int         num_landuses = 0;   ///< Present in v3+.
    int         num_nodes    = 0;
    int         num_links    = 0;
    int         num_pollut   = 0;
    int         flow_units   = 0;   ///< Mirrors SimulationOptions enum (CFS=0..MLD=5).
};

/** @brief Per-subcatchment routing state snapshot (HSF v3+). */
struct HotstartSubcatchState {
    double runoff = 0.0;
    /// Infiltration model id + 6 state doubles (legacy layout).
    int    infil_model = 0;
    double infil_state[6] = {0,0,0,0,0,0};
    /// Groundwater zone state.
    double gw_theta_upper  = 0.0;
    double gw_lower_depth  = 0.0;
    /// Snowpack water-equivalent + free-water + ATI per surface.
    double snow_we_plowable = 0.0, snow_we_imperv = 0.0, snow_we_perv = 0.0;
    double snow_fw_plowable = 0.0, snow_fw_imperv = 0.0, snow_fw_perv = 0.0;
    double snow_ati         = 0.0;
    /// Surface buildup mass + ponded concentration per pollutant.
    /// Length must equal HotstartHeader::num_pollut.
    std::vector<double> surface_buildup;
    std::vector<double> ponded_concentration;
};

/** @brief Per-node routing state snapshot. */
struct HotstartNodeState {
    double depth          = 0.0;
    double lateral_inflow = 0.0;
    double overflow       = 0.0;
    /// Pollutant concentration array (length = HotstartHeader::num_pollut).
    std::vector<double> concentration;
};

/** @brief Per-link routing state snapshot. */
struct HotstartLinkState {
    double flow           = 0.0;
    double depth          = 0.0;
    double volume         = 0.0;
    double setting        = 0.0;
    double target_setting = 0.0;
    double time_open      = 0.0;
    double time_closed    = 0.0;
    /// Pollutant concentration array (length = HotstartHeader::num_pollut).
    std::vector<double> concentration;
};

/** @brief Top-level snapshot — header plus per-object vectors. */
struct HotstartSnapshot {
    HotstartHeader                    header;
    std::vector<HotstartSubcatchState> subcatch_state;  // size == header.num_subcatch
    std::vector<HotstartNodeState>     node_state;      // size == header.num_nodes
    std::vector<HotstartLinkState>     link_state;      // size == header.num_links
};

// ============================================================================
// Error handling
// ============================================================================

/**
 * @brief Lightweight outcome for format parse / materialise calls.
 *
 * @details `ok == true` means the operation succeeded. Otherwise `error`
 *          carries a human-readable diagnostic and the caller decides
 *          whether to surface it as a warning, throw, or abort.
 */
struct FormatResult {
    bool        ok    = true;
    std::string error;
};

inline FormatResult ok()                       { return {true,  {}};            }
inline FormatResult fail(std::string message)  { return {false, std::move(message)}; }

} // namespace openswmm::gpkg::formats

#endif // OPENSWMM_GEOPACKAGE_FORMAT_TYPES_HPP
