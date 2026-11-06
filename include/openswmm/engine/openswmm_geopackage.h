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
 * @file      openswmm_geopackage.h
 * @brief     OpenSWMM Engine — GeoPackage C API.
 *
 * @details Provides a C89-compatible API for interacting with GeoPackage files
 *          produced by the OpenSWMM engine. The API has two modes:
 *
 *          **Read-only access** to model definitions, simulation results, and
 *          summary statistics. These are written by the engine's plugin system
 *          (GeoPackageInputPlugin, GeoPackageOutputPlugin, GeoPackageReportPlugin)
 *          and are read by the domain-specific accessors. The low-level SQL
 *          helpers execute caller-provided statements without a read-only guard.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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

#define SWMM_GPKG_OK 0   /**< Success. */
#define SWMM_GPKG_ERR -1 /**< General error (call swmm_gpkg_last_error). */

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * @brief Open or create a writable GeoPackage and ensure its OpenSWMM schema exists.
 *
 * Uses SQLite READWRITE|CREATE and idempotently creates schema/default variable rows, so this is
 * not a read-only open even when the caller intends only to query. Model/result APIs below read
 * those tables; observed-series APIs perform user writes.
 *
 * @param[in] path Non-null NUL-terminated database path, borrowed for the call. An absent file may
 *     be created.
 * @return Caller-owned handle, or NULL for a null path or caught database/schema-open failure.
 *     Release with swmm_gpkg_close().
 */
SWMM_ENGINE_API SWMM_Gpkg swmm_gpkg_open(const char* path);

/**
 * @brief Roll back an outstanding transaction and delete the database handle.
 *
 * Attempts rollback when marked in a transaction, closes the database and frees its wrapper. Does
 * not report rollback failure.
 *
 * @param[in] gpkg Caller-owned handle, or NULL for a no-op. All borrowed diagnostics become
 *     invalid; do not use the handle afterward.
 */
SWMM_ENGINE_API void swmm_gpkg_close(SWMM_Gpkg gpkg);

/**
 * @brief Borrow the last stored handle diagnostic.
 *
 * Not every refusal stores a diagnostic and successful operations do not uniformly clear old text.
 * Use the operation return code as the primary result.
 *
 * @param[in] gpkg Live handle, or NULL to obtain an empty static string.
 * @return Borrowed NUL-terminated string; do not free it and copy before another operation replaces
 *     it or the handle is closed.
 */
SWMM_ENGINE_API const char* swmm_gpkg_last_error(SWMM_Gpkg gpkg);

/* =========================================================================
 * Transactions
 * ========================================================================= */

/**
 * @brief Begin an immediate SQLite transaction on this handle.
 *
 * Uses BEGIN IMMEDIATE. Further observed inserts share this transaction until commit/rollback;
 * closing the handle attempts rollback.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @return SWMM_GPKG_OK on success, SWMM_GPKG_ERR for null handle, an already active transaction or
 *     a caught database error.
 */
SWMM_ENGINE_API int swmm_gpkg_begin(SWMM_Gpkg gpkg);

/**
 * @brief Commit this handle active transaction.
 *
 * The active flag clears only after successful commit; after refusal the caller can inspect the
 * diagnostic and roll back.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @return SWMM_GPKG_OK on success, SWMM_GPKG_ERR for null handle, no active transaction or a caught
 *     commit error.
 */
SWMM_ENGINE_API int swmm_gpkg_commit(SWMM_Gpkg gpkg);

/**
 * @brief Roll back this handle active transaction if one exists.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @return SWMM_GPKG_OK after rollback or when no transaction is active; SWMM_GPKG_ERR for null
 *     handle or a caught database error.
 */
SWMM_ENGINE_API int swmm_gpkg_rollback(SWMM_Gpkg gpkg);

/* =========================================================================
 * Registration
 * ========================================================================= */

/**
 * @brief Submit process-global GeoPackage plugin registration metadata.
 *
 * Does not take or return a database handle; registration does not itself open a file.
 *
 * @param[in] license_key Optional borrowed NUL-terminated registration key/token; NULL becomes
 *     empty.
 * @param[in] organization Optional borrowed organization name; NULL becomes empty.
 * @param[in] contact_email Optional borrowed contact email; NULL becomes empty.
 * @param[in] deployment_id Optional borrowed deployment identifier; NULL becomes empty.
 * @return 1 when the plugin registration hook accepts the copied metadata, otherwise 0.
 */
SWMM_ENGINE_API int swmm_gpkg_register(const char* license_key, const char* organization,
                                       const char* contact_email, const char* deployment_id);

/**
 * @brief Read process-global GeoPackage plugin registration state.
 * @return 1 when registered, otherwise 0.
 */
SWMM_ENGINE_API int swmm_gpkg_is_registered(void);

/* =========================================================================
 * Read: Simulation metadata
 * ========================================================================= */

/**
 * @brief Count stored simulation-run rows.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @return Nonnegative count, or -1 for null handle/caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_simulation_count(SWMM_Gpkg gpkg);

/**
 * @brief Copy one simulation-run ID in creation-time order.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] index Zero-based query offset. Supply a valid nonnegative index; ordering ties follow
 *     the database result order.
 * @param[out] buf Non-null caller-owned ID destination, NUL-terminated and silently truncated to
 *     bufsz-1 bytes.
 * @param[in] bufsz Positive byte capacity including NUL.
 * @return SWMM_GPKG_OK after copying a row; SWMM_GPKG_ERR for invalid buffer/handle, no row or
 *     caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_simulation_id(SWMM_Gpkg gpkg, int index, char* buf, int bufsz);

/* =========================================================================
 * Read: Model object counts
 * ========================================================================= */

/**
 * @brief Count node rows for one simulation.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] simulation_id Non-null NUL-terminated simulation ID as stored in the database;
 *     borrowed for the call.
 * @return Nonnegative row count (zero for an absent simulation key), or -1 for null
 *     arguments/caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_node_count(SWMM_Gpkg gpkg, const char* simulation_id);
/**
 * @brief Count link rows for one simulation.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] simulation_id Non-null NUL-terminated simulation ID as stored in the database;
 *     borrowed for the call.
 * @return Nonnegative row count (zero for an absent simulation key), or -1 for null
 *     arguments/caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_link_count(SWMM_Gpkg gpkg, const char* simulation_id);
/**
 * @brief Count subcatchment rows for one simulation.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] simulation_id Non-null NUL-terminated simulation ID as stored in the database;
 *     borrowed for the call.
 * @return Nonnegative row count (zero for an absent simulation key), or -1 for null
 *     arguments/caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_subcatch_count(SWMM_Gpkg gpkg, const char* simulation_id);
/**
 * @brief Count rain-gage rows for one simulation.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] simulation_id Non-null NUL-terminated simulation ID as stored in the database;
 *     borrowed for the call.
 * @return Nonnegative row count (zero for an absent simulation key), or -1 for null
 *     arguments/caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_gage_count(SWMM_Gpkg gpkg, const char* simulation_id);
/**
 * @brief Count node-link topology edge rows for one simulation.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] simulation_id Non-null NUL-terminated simulation ID as stored in the database;
 *     borrowed for the call.
 * @return Nonnegative row count (zero for an absent simulation key), or -1 for null
 *     arguments/caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_topology_edge_count(SWMM_Gpkg gpkg, const char* simulation_id);
/**
 * @brief Count catalogue variables, including added observed-data variables.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @return Nonnegative count, or -1 for null handle/caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_variable_count(SWMM_Gpkg gpkg);

/* =========================================================================
 * Read: Simulation result timeseries
 * ========================================================================= */

/**
 * @brief Copy the first time-ordered result rows for one simulation/object/variable.
 *
 * Ordered by elapsed_time; ties follow query order. Does not interpolate, transform units or report
 * an additional required size. On failure earlier rows may already have been copied.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] simulation_id Non-null NUL-terminated simulation ID as stored in the database;
 *     borrowed for the call.
 * @param[in] object_type Non-null object category token as stored (for example NODE, LINK, SUBCATCH
 *     or SYSTEM); matched by the database query.
 * @param[in] object_id Non-null NUL-terminated object ID as stored.
 * @param[in] variable_name Non-null NUL-terminated variable name resolved with object_type in the
 *     variable catalogue.
 * @param[out] times Non-null caller-owned max_count-double array for elapsed seconds.
 * @param[out] values Non-null caller-owned max_count-double array for stored values; units follow
 *     the database variable metadata, with no conversion here.
 * @param[in] max_count Positive array capacity/SQL row limit.
 * @return Number of rows copied (>=0), or SWMM_GPKG_ERR for null required arguments/nonpositive
 *     capacity/caught query error. An unknown variable sets a diagnostic and returns 0.
 */
SWMM_ENGINE_API int swmm_gpkg_read_result_ts(SWMM_Gpkg gpkg, const char* simulation_id,
                                             const char* object_type, const char* object_id,
                                             const char* variable_name, double* times,
                                             double* values, int max_count);

/**
 * @brief Count result rows for one simulation/object/variable key.
 *
 * Use to allocate result arrays; count and later read are separate database queries and can observe
 * intervening writes.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] simulation_id Non-null NUL-terminated simulation ID as stored in the database;
 *     borrowed for the call.
 * @param[in] object_type Non-null object category token as stored (for example NODE, LINK, SUBCATCH
 *     or SYSTEM); matched by the database query.
 * @param[in] object_id Non-null NUL-terminated object ID as stored.
 * @param[in] variable_name Non-null NUL-terminated variable name resolved with object_type in the
 *     variable catalogue.
 * @return Nonnegative row count, including zero for an unknown variable or no matches;
 *     SWMM_GPKG_ERR for null required arguments or a caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_result_ts_count(SWMM_Gpkg gpkg, const char* simulation_id,
                                              const char* object_type, const char* object_id,
                                              const char* variable_name);

/* =========================================================================
 * Read: Summary statistics
 * ========================================================================= */

/**
 * @brief Read a stored summary value for one simulation/object/variable.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] simulation_id Non-null NUL-terminated simulation ID as stored in the database;
 *     borrowed for the call.
 * @param[in] object_type Non-null object category token as stored (for example NODE, LINK, SUBCATCH
 *     or SYSTEM); matched by the database query.
 * @param[in] object_id Non-null NUL-terminated object ID as stored.
 * @param[in] variable_name Non-null NUL-terminated variable name resolved with object_type in the
 *     variable catalogue.
 * @param[out] value Non-null stored-value destination; unchanged when no row is found. Units are
 *     those recorded by the summary writer; no conversion is applied.
 * @return SWMM_GPKG_OK for a row, SWMM_GPKG_ERR for null required arguments, unknown variable, no
 *     summary match or a caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_read_summary(SWMM_Gpkg gpkg, const char* simulation_id,
                                           const char* object_type, const char* object_id,
                                           const char* variable_name, double* value);

/* =========================================================================
 * Write: Observed / sensor timeseries
 * ========================================================================= */

/**
 * @brief Create a uniquely named observed series and resolve/create its variable row.
 *
 * Does not validate that the linked model object exists or convert values to the declared units.
 * Variable creation and series insertion are separate statements; use an explicit transaction when
 * they must roll back together.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] name Non-null copied series name; uniqueness is enforced by the database.
 * @param[in] variable_name Non-null copied measured variable name.
 * @param[in] object_type Optional copied category. NULL stores an unlinked series but uses NODE
 *     when resolving/creating its variable catalogue entry.
 * @param[in] object_id Optional copied model-object ID; NULL stores no linked ID.
 * @param[in] source Optional copied source description; NULL stores SQL NULL.
 * @param[in] units Optional copied units label; NULL stores SQL NULL for the series and an empty
 *     label for a newly created variable.
 * @return New nonnegative series ID, or SWMM_GPKG_ERR for null required arguments, a constraint
 *     failure or a caught database error.
 */
SWMM_ENGINE_API int swmm_gpkg_create_observed_series(SWMM_Gpkg gpkg, const char* name,
                                                     const char* variable_name,
                                                     const char* object_type, const char* object_id,
                                                     const char* source, const char* units);

/**
 * @brief Insert one observed timestamp/value into a series.
 *
 * Joins an active transaction when present; otherwise normal SQLite autocommit applies.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] series_id Stored observed-series ID; database constraints determine acceptance.
 * @param[in] timestamp Non-null NUL-terminated timestamp string, conventionally ISO 8601; copied
 *     without parsing or timezone conversion.
 * @param[in] value Measured numeric value in the series declared units; no finite/range/unit
 *     validation here.
 * @param[in] quality_flag Optional copied quality label such as A/P/E; NULL stores SQL NULL.
 * @return SWMM_GPKG_OK after SQLite accepts the insert, or SWMM_GPKG_ERR for null required
 *     arguments, constraint failure or caught database error.
 */
SWMM_ENGINE_API int swmm_gpkg_write_observed_value(SWMM_Gpkg gpkg, int series_id,
                                                   const char* timestamp, double value,
                                                   const char* quality_flag);

/**
 * @brief Insert a vector of observed values using one prepared statement.
 *
 * Does not create an implicit all-or-nothing transaction. Earlier accepted inserts remain
 * committed/pending if a later row fails; wrap begin/commit and roll back on refusal when batch
 * atomicity is required.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] series_id Stored observed-series ID for all records.
 * @param[in] timestamps Non-null count-entry borrowed timestamp-pointer array. Each supplied string
 *     is copied by SQLite; provide valid timestamps consistent with the series.
 * @param[in] values Non-null count-double borrowed measurement array, in series units; no
 *     conversion/finite validation.
 * @param[in] quality_flags Optional count-entry borrowed flag-pointer array; NULL array or NULL
 *     entries store SQL NULL.
 * @param[in] count Positive record count.
 * @return SWMM_GPKG_OK after all inserts, or SWMM_GPKG_ERR for invalid required pointers/count, a
 *     failed insert or caught database error.
 */
SWMM_ENGINE_API int swmm_gpkg_write_observed_values(SWMM_Gpkg gpkg, int series_id,
                                                    const char** timestamps, const double* values,
                                                    const char** quality_flags, int count);

/* =========================================================================
 * Read: Observed / sensor timeseries
 * ========================================================================= */

/**
 * @brief Count observed-series definitions.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @return Nonnegative series count, or SWMM_GPKG_ERR for null handle/caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_observed_series_count(SWMM_Gpkg gpkg);

/**
 * @brief Count rows stored for an observed series ID.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] series_id Observed-series key; absent keys produce zero rows.
 * @return Nonnegative count, or SWMM_GPKG_ERR for null handle/caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_observed_value_count(SWMM_Gpkg gpkg, int series_id);

/**
 * @brief Copy a timestamp-ordered prefix of observed values.
 *
 * Orders by the stored timestamp TEXT, not a parsed calendar/timezone value. Use a consistently
 * sortable timestamp representation. No quality flags or needed-size output are returned; earlier
 * records may have been copied on failure.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] series_id Observed-series key to query.
 * @param[out] timestamps Optional flat caller-owned buffer of max_count*ts_buf_len bytes. Each
 *     timestamp uses one fixed-stride slot and is NUL-terminated/truncated when stride>0.
 * @param[in] ts_buf_len Per-timestamp byte stride including NUL. Nonpositive suppresses timestamp
 *     copying, even when timestamps is non-null.
 * @param[out] values Non-null caller-owned max_count-double array; raw stored measurement values
 *     without unit conversion.
 * @param[in] max_count Positive row limit and value-array capacity.
 * @return Number of rows copied (>=0), or SWMM_GPKG_ERR for null handle/values, nonpositive
 *     max_count or caught query error.
 */
SWMM_ENGINE_API int swmm_gpkg_read_observed_values(SWMM_Gpkg gpkg, int series_id, char* timestamps,
                                                   int ts_buf_len, double* values, int max_count);

/* =========================================================================
 * Read: Ad-hoc queries
 * ========================================================================= */

/**
 * @brief Execute supplied SQL and return the first column of its first result row as int.
 *
 * Executes one prepared statement and steps once. It is a direct database interface rather than a
 * constrained SELECT parser; no automatic protection of model/result tables is applied here.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] sql Non-null borrowed SQL text. Callers intending a read-only operation must supply a
 *     read-only query; this wrapper does not enforce SQL read-only status.
 * @return SQLite integer conversion of first row/column, or SWMM_GPKG_ERR (-1) for null arguments,
 *     no row or caught error. A legitimate -1 query result is indistinguishable from failure.
 */
SWMM_ENGINE_API int swmm_gpkg_query_int(SWMM_Gpkg gpkg, const char* sql);

/**
 * @brief Execute supplied SQL and copy the first row/column as double.
 *
 * Uses SQLite numeric conversion without validating the result schema or converting units;
 * prepares/steps one statement.
 *
 * @param[in] gpkg Non-null live GeoPackage handle; caller serializes access and retains ownership
 *     until swmm_gpkg_close().
 * @param[in] sql Non-null borrowed SQL text; callers requiring read-only access must supply a
 *     read-only query because this wrapper does not enforce it.
 * @param[out] result Non-null double destination, written only when SQLite returns a row.
 * @return SWMM_GPKG_OK for a returned row, SWMM_GPKG_ERR for null required arguments, no row or
 *     caught database error.
 */
SWMM_ENGINE_API int swmm_gpkg_query_double(SWMM_Gpkg gpkg, const char* sql, double* result);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_GEOPACKAGE_H */
