/**
 * @file GeoPackageSchema.hpp
 * @brief DDL and metadata registration for the OpenSWMM GeoPackage schema.
 * @ingroup engine_geopackage
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
