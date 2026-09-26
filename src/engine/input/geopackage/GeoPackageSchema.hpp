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
 * @file GeoPackageSchema.hpp
 * @brief DDL and metadata registration for the OpenSWMM GeoPackage schema.
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GEOPACKAGE_SCHEMA_HPP
#define OPENSWMM_GEOPACKAGE_SCHEMA_HPP

#include <sqlite3.h>
#include <string>

namespace openswmm::gpkg {

/**
 * @brief Create all GeoPackage metadata tables and OpenSWMM application tables.
 *
 * @details Creates (idempotently):
 *   - gpkg_spatial_ref_sys, gpkg_contents, gpkg_geometry_columns (OGC standard)
 *   - Part A: options, nodes, links, subcatchments, rain_gages,
 *             node_links, subcatch_routing, curves, input_timeseries,
 *             patterns, pollutants, transects
 *   - Part B: simulations, variables, result_timeseries, result_summary
 *   - Part C: observed_series, observed_values
 *   - Part D (Slice IO-5): hotstart_slots + hotstart_{node,link,subcatch}_state
 *             + hotstart_{node,link,subcatch}_pollutant_state, raingage_data,
 *             climate_data, routing_interface_{node,subcatch,gage}, and
 *             routing_interface_node_pollutants. Composite foreign keys
 *             into the existing model tables enforce cascading delete /
 *             update and reject orphan rows.  See IO_PORTABILITY_PLAN.md
 *             §3.4 for the design rationale.
 *
 * @param db  Open SQLite database handle.
 * @throws GpkgError on failure.
 */
void create_schema(sqlite3* db);

/**
 * @brief Register a CRS in gpkg_spatial_ref_sys (if not already present).
 *
 * @param db       Open database handle.
 * @param srs_id   Numeric SRS ID (e.g., 4326).
 * @param org      Organization name (e.g., "EPSG").
 * @param org_id   Organization CRS ID (e.g., 4326).
 * @param srs_name Human-readable name (e.g., "WGS 84").
 * @param wkt      WKT definition string.
 */
void register_crs(sqlite3* db, int srs_id, const std::string& org,
                  int org_id, const std::string& srs_name, const std::string& wkt);

/**
 * @brief Register a feature table in gpkg_contents and gpkg_geometry_columns.
 *
 * @param db          Open database handle.
 * @param table_name  Table name (e.g., "nodes").
 * @param geom_type   Geometry type name (e.g., "POINT", "LINESTRING", "MULTIPOLYGON").
 * @param srs_id      SRS ID from gpkg_spatial_ref_sys.
 * @param identifier  Human-readable identifier.
 * @param description Table description.
 * @param min_x, min_y, max_x, max_y  Bounding box.
 */
void register_feature_table(sqlite3* db, const std::string& table_name,
                            const std::string& geom_type, int srs_id,
                            const std::string& identifier, const std::string& description,
                            double min_x, double min_y, double max_x, double max_y);

/**
 * @brief Pre-populate the variables catalog with known SWMM output variables.
 * @param db  Open database handle.
 */
void populate_default_variables(sqlite3* db);

} // namespace openswmm::gpkg

#endif // OPENSWMM_GEOPACKAGE_SCHEMA_HPP
