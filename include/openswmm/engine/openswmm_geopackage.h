/**
 * @file openswmm_geopackage.h
 * @brief OpenSWMM Engine — GeoPackage C API.
 *
 * @details Provides a C89-compatible API for interacting with GeoPackage files
 *          produced by the OpenSWMM engine. The API has two modes:
 *
 *          **Read-only access** to model definitions, simulation results, and
 *          summary statistics. These are written by the engine's plugin system
 *          (GeoPackageInputPlugin, GeoPackageOutputPlugin, GeoPackageReportPlugin)
 *          and are not modifiable through this C API.
 *
 *          **Read-write access** to observed / sensor timeseries data. This API
 *          is designed for calibration and comparison workflows: import measured
 *          data from external sources, associate it with model objects, and
 *          query it alongside simulation results.
 *
 *          Transactions are available for bulk operations. Wrapping observed
 *          data imports in a transaction (swmm_gpkg_begin / swmm_gpkg_commit)
 *          can improve insert performance by 10-100x.
 *
 *          This header is only available when compiled with
 *          OPENSWMM_WITH_GEOPACKAGE=ON. Link against openswmm_geopackage.
 *
 *          Usage:
 *          @code{.c}
 *          #include <openswmm/engine/openswmm_geopackage.h>
 *
 *          SWMM_Gpkg gpkg = swmm_gpkg_open("model.gpkg");
 *
 *          // Read model metadata
 *          int nn = swmm_gpkg_node_count(gpkg, "run_1");
 *
 *          // Read simulation results
 *          double values[100];
 *          double times[100];
 *          int n = swmm_gpkg_read_result_ts(gpkg, "run_1", "NODE", "J1",
 *                                            "depth", times, values, 100);
 *
 *          // Import observed data (in a transaction for speed)
 *          swmm_gpkg_begin(gpkg);
 *          int sid = swmm_gpkg_create_observed_series(gpkg, "USGS_flow",
 *                       "flow", "LINK", "C1", "USGS", "CMS");
 *          double ts_vals[] = {1.5, 2.3, 1.8};
 *          const char* ts_times[] = {"2026-01-15T08:00:00Z",
 *                                     "2026-01-15T09:00:00Z",
 *                                     "2026-01-15T10:00:00Z"};
 *          swmm_gpkg_write_observed_values(gpkg, sid, ts_times, ts_vals, NULL, 3);
 *          swmm_gpkg_commit(gpkg);
 *
 *          swmm_gpkg_close(gpkg);
 *          @endcode
 *
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GEOPACKAGE_H
#define OPENSWMM_GEOPACKAGE_H

#include "openswmm_engine_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Opaque handle
 * ========================================================================= */

/** @brief Opaque handle to an open GeoPackage database. */
typedef void* SWMM_Gpkg;

/* =========================================================================
 * Return codes
 * ========================================================================= */

#define SWMM_GPKG_OK     0  /**< Success. */
#define SWMM_GPKG_ERR   -1  /**< General error (call swmm_gpkg_last_error). */

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * @brief Open an existing GeoPackage file.
 *
 * @details Opens the database in read-write mode. Model definitions and
 *          simulation results are read-only by convention — only the
 *          observed data tables are writable through this API.
 *
 * @param path  Path to the .gpkg file (must exist).
 * @returns Opaque handle, or NULL on failure.
 */
SWMM_ENGINE_API SWMM_Gpkg swmm_gpkg_open(const char* path);

/**
 * @brief Close the GeoPackage and free all resources.
 *
 * @details Rolls back any uncommitted transaction. NULL-safe.
 *
 * @param gpkg  GeoPackage handle.
 */
SWMM_ENGINE_API void swmm_gpkg_close(SWMM_Gpkg gpkg);

/**
 * @brief Get the last error message.
 * @param gpkg  GeoPackage handle.
 * @returns Null-terminated error message, or empty string if no error.
 */
SWMM_ENGINE_API const char* swmm_gpkg_last_error(SWMM_Gpkg gpkg);

/* =========================================================================
 * Transactions
 * ========================================================================= */

/**
 * @brief Begin a transaction for bulk operations.
 *
 * @details Wrapping multiple observed data inserts in a single transaction
 *          dramatically improves performance (SQLite commits per-statement
 *          by default, which is slow for bulk inserts).
 *
 * @param gpkg  GeoPackage handle.
 * @returns SWMM_GPKG_OK on success, SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_begin(SWMM_Gpkg gpkg);

/**
 * @brief Commit the current transaction.
 * @param gpkg  GeoPackage handle.
 * @returns SWMM_GPKG_OK on success, SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_commit(SWMM_Gpkg gpkg);

/**
 * @brief Roll back the current transaction.
 * @param gpkg  GeoPackage handle.
 * @returns SWMM_GPKG_OK on success, SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_rollback(SWMM_Gpkg gpkg);

/* =========================================================================
 * Registration
 * ========================================================================= */

/**
 * @brief Register the GeoPackage plugin.
 *
 * @param license_key    License key or activation token (may be NULL).
 * @param organization   Registering organization name (may be NULL).
 * @param contact_email  Contact email (may be NULL).
 * @param deployment_id  Deployment identifier (may be NULL).
 * @returns 1 if registration succeeded, 0 otherwise.
 */
SWMM_ENGINE_API int swmm_gpkg_register(const char* license_key,
                                        const char* organization,
                                        const char* contact_email,
                                        const char* deployment_id);

/**
 * @brief Check whether the GeoPackage plugin is registered.
 * @returns 1 if registered, 0 otherwise.
 */
SWMM_ENGINE_API int swmm_gpkg_is_registered(void);

/* =========================================================================
 * Read: Simulation metadata
 * ========================================================================= */

/**
 * @brief Get the number of simulation runs in the GeoPackage.
 * @param gpkg  GeoPackage handle.
 * @returns Simulation count, or -1 on error.
 */
SWMM_ENGINE_API int swmm_gpkg_simulation_count(SWMM_Gpkg gpkg);

/**
 * @brief Get the simulation_id at the given index.
 *
 * @param gpkg   GeoPackage handle.
 * @param index  Zero-based index (ordered by creation time).
 * @param buf    Buffer to receive the null-terminated simulation_id.
 * @param bufsz  Size of the buffer.
 * @returns SWMM_GPKG_OK on success, SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_simulation_id(SWMM_Gpkg gpkg, int index,
                                              char* buf, int bufsz);

/* =========================================================================
 * Read: Model object counts
 * ========================================================================= */

SWMM_ENGINE_API int swmm_gpkg_node_count(SWMM_Gpkg gpkg, const char* simulation_id);
SWMM_ENGINE_API int swmm_gpkg_link_count(SWMM_Gpkg gpkg, const char* simulation_id);
SWMM_ENGINE_API int swmm_gpkg_subcatch_count(SWMM_Gpkg gpkg, const char* simulation_id);
SWMM_ENGINE_API int swmm_gpkg_gage_count(SWMM_Gpkg gpkg, const char* simulation_id);
SWMM_ENGINE_API int swmm_gpkg_topology_edge_count(SWMM_Gpkg gpkg, const char* simulation_id);
SWMM_ENGINE_API int swmm_gpkg_variable_count(SWMM_Gpkg gpkg);

/* =========================================================================
 * Read: Simulation result timeseries
 * ========================================================================= */

/**
 * @brief Read a result timeseries for one object and variable.
 *
 * @details Fills the caller-supplied arrays with elapsed_time and value pairs,
 *          ordered by time. Returns the number of values actually read (may be
 *          less than max_count if fewer results exist).
 *
 * @param gpkg           GeoPackage handle.
 * @param simulation_id  Simulation run identifier.
 * @param object_type    "NODE", "LINK", "SUBCATCH", or "SYSTEM".
 * @param object_id      Object identifier (e.g., "J1").
 * @param variable_name  Variable name (e.g., "depth", "flow").
 * @param[out] times     Array to receive elapsed times (seconds). Must hold max_count doubles.
 * @param[out] values    Array to receive result values. Must hold max_count doubles.
 * @param max_count      Maximum number of values to read.
 * @returns Number of values read (>= 0), or SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_read_result_ts(SWMM_Gpkg gpkg,
                                               const char* simulation_id,
                                               const char* object_type,
                                               const char* object_id,
                                               const char* variable_name,
                                               double* times,
                                               double* values,
                                               int max_count);

/**
 * @brief Get the number of result timeseries records for a query.
 *
 * @details Use this to pre-allocate arrays before calling swmm_gpkg_read_result_ts().
 *
 * @param gpkg           GeoPackage handle.
 * @param simulation_id  Simulation run identifier.
 * @param object_type    "NODE", "LINK", "SUBCATCH", or "SYSTEM".
 * @param object_id      Object identifier.
 * @param variable_name  Variable name.
 * @returns Record count (>= 0), or SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_result_ts_count(SWMM_Gpkg gpkg,
                                                const char* simulation_id,
                                                const char* object_type,
                                                const char* object_id,
                                                const char* variable_name);

/* =========================================================================
 * Read: Summary statistics
 * ========================================================================= */

/**
 * @brief Read a single summary statistic value.
 *
 * @param gpkg           GeoPackage handle.
 * @param simulation_id  Simulation run identifier.
 * @param object_type    "NODE", "LINK", or "SUBCATCH".
 * @param object_id      Object identifier.
 * @param variable_name  Variable name (e.g., "max_depth", "max_flow").
 * @param[out] value     Pointer to receive the statistic value.
 * @returns SWMM_GPKG_OK on success, SWMM_GPKG_ERR if not found.
 */
SWMM_ENGINE_API int swmm_gpkg_read_summary(SWMM_Gpkg gpkg,
                                             const char* simulation_id,
                                             const char* object_type,
                                             const char* object_id,
                                             const char* variable_name,
                                             double* value);

/* =========================================================================
 * Write: Observed / sensor timeseries
 * ========================================================================= */

/**
 * @brief Create an observed data series.
 *
 * @details Associates the series with a model object for sim-vs-observed
 *          comparison. Pass NULL for object_type/object_id to create an
 *          unlinked series.
 *
 * @param gpkg           GeoPackage handle.
 * @param name           Unique series name (e.g., "USGS_01585200_flow").
 * @param variable_name  Variable being measured (e.g., "flow", "depth").
 * @param object_type    Model object type ("NODE", "LINK", "SUBCATCH"), or NULL.
 * @param object_id      Model object ID, or NULL.
 * @param source         Data source description (e.g., "USGS NWIS"), or NULL.
 * @param units          Measurement units (e.g., "CMS"), or NULL.
 * @returns Series ID (>= 0) on success, SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_create_observed_series(SWMM_Gpkg gpkg,
                                                       const char* name,
                                                       const char* variable_name,
                                                       const char* object_type,
                                                       const char* object_id,
                                                       const char* source,
                                                       const char* units);

/**
 * @brief Write a single observed data point.
 *
 * @param gpkg          GeoPackage handle.
 * @param series_id     Series ID from swmm_gpkg_create_observed_series().
 * @param timestamp     ISO 8601 timestamp (e.g., "2026-01-15T08:30:00Z").
 * @param value         Measured value.
 * @param quality_flag  Quality flag ("A", "P", "E"), or NULL.
 * @returns SWMM_GPKG_OK on success, SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_write_observed_value(SWMM_Gpkg gpkg,
                                                     int series_id,
                                                     const char* timestamp,
                                                     double value,
                                                     const char* quality_flag);

/**
 * @brief Bulk-write a vector of observed data points to a series.
 *
 * @details Inserts `count` timestamp/value pairs in a single prepared-statement
 *          loop. For best performance, wrap in a transaction:
 *          @code{.c}
 *          swmm_gpkg_begin(gpkg);
 *          swmm_gpkg_write_observed_values(gpkg, sid, times, vals, flags, 10000);
 *          swmm_gpkg_commit(gpkg);
 *          @endcode
 *
 * @param gpkg           GeoPackage handle.
 * @param series_id      Series ID from swmm_gpkg_create_observed_series().
 * @param timestamps     Array of ISO 8601 timestamp strings (count elements).
 * @param values         Array of measured values (count elements).
 * @param quality_flags  Array of quality flag strings (count elements), or NULL
 *                       to omit quality flags for all values.
 * @param count          Number of data points to write.
 * @returns SWMM_GPKG_OK on success, SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_write_observed_values(SWMM_Gpkg gpkg,
                                                      int series_id,
                                                      const char** timestamps,
                                                      const double* values,
                                                      const char** quality_flags,
                                                      int count);

/* =========================================================================
 * Read: Observed / sensor timeseries
 * ========================================================================= */

/**
 * @brief Get the number of observed series in the GeoPackage.
 * @param gpkg  GeoPackage handle.
 * @returns Series count (>= 0), or SWMM_GPKG_ERR on error.
 */
SWMM_ENGINE_API int swmm_gpkg_observed_series_count(SWMM_Gpkg gpkg);

/**
 * @brief Get the number of values in an observed series.
 * @param gpkg       GeoPackage handle.
 * @param series_id  Series ID.
 * @returns Value count (>= 0), or SWMM_GPKG_ERR on error.
 */
SWMM_ENGINE_API int swmm_gpkg_observed_value_count(SWMM_Gpkg gpkg, int series_id);

/**
 * @brief Read observed timeseries values into caller-supplied arrays.
 *
 * @param gpkg           GeoPackage handle.
 * @param series_id      Series ID.
 * @param[out] timestamps  Array to receive timestamp strings (each must be at
 *                         least 32 chars). Pass NULL to skip timestamps.
 * @param ts_buf_len     Length of each timestamp buffer (e.g., 32).
 * @param[out] values    Array to receive values. Must hold max_count doubles.
 * @param max_count      Maximum number of values to read.
 * @returns Number of values read (>= 0), or SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_read_observed_values(SWMM_Gpkg gpkg,
                                                     int series_id,
                                                     char* timestamps,
                                                     int ts_buf_len,
                                                     double* values,
                                                     int max_count);

/* =========================================================================
 * Read: Ad-hoc queries
 * ========================================================================= */

/**
 * @brief Execute a read-only SQL query and get the first integer result.
 * @param gpkg  GeoPackage handle.
 * @param sql   SELECT query string.
 * @returns Integer result, or SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_query_int(SWMM_Gpkg gpkg, const char* sql);

/**
 * @brief Execute a read-only SQL query and get the first double result.
 * @param gpkg    GeoPackage handle.
 * @param sql     SELECT query string.
 * @param[out] result  Pointer to receive the double result.
 * @returns SWMM_GPKG_OK on success, SWMM_GPKG_ERR on failure.
 */
SWMM_ENGINE_API int swmm_gpkg_query_double(SWMM_Gpkg gpkg, const char* sql,
                                             double* result);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_GEOPACKAGE_H */
