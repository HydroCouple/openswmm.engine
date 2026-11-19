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
 * @file      openswmm_restart.h
 * @brief     Restart records in GeoPackage and HDF5 files: the [RESTART] section and inspection.
 *
 * @details A run can record restart states into a GeoPackage or an HDF5 file, and a later run can
 *          start from one of them. Each record holds the bytes of a native hot start file captured
 *          at an accepted step, under an immutable run ID and record ID.
 *
 *          - MODE INITIAL_CONDITIONS seeds object state at the target model's own start date and
 *            opens new balance ledgers.
 *          - MODE CONTINUE installs the engine checkpoint the record carries, restoring clocks,
 *            histories and ledgers. Only records written with RECORD_CONTINUATION YES carry one.
 *
 *          In a run that saves binary results, a continuation record also carries the output
 *          written so far, so a run continued from it writes the same binary output file; that
 *          needs a new output file (see swmm_restart_uses_checkpoint()).
 *
 *          The section in an input file:
 *
 * @code{.unparsed}
 * [RESTART]
 * SOURCE_FILE          "previous.gpkg"
 * SOURCE_RUN           run_20261119T120000Z_3fa9c2d1
 * SOURCE_RECORD        rec_000002
 * MODE                 INITIAL_CONDITIONS
 * RECORD_FILE          "states.h5"
 * RECORD_FORMAT        HDF5
 * RECORD_AT            END
 * RECORD_AT            01/01/2020 00:30:00
 * RECORD_EVERY         00:15:00
 * RECORD_CONTINUATION  NO
 * @endcode
 *
 * @code{.c}
 * // List the runs and records of a file without changing it.
 * size_t need = 0;
 * swmm_restart_inspect_json("previous.gpkg", NULL, 0, &need);
 * char* json = malloc(need);
 * swmm_restart_inspect_json("previous.gpkg", json, need, &need);
 *
 * // Start the next run from a chosen record.
 * swmm_restart_set(engine, "SOURCE_FILE", "previous.gpkg");
 * swmm_restart_set(engine, "SOURCE_RUN", "run_20261119T120000Z_3fa9c2d1");
 * swmm_restart_set(engine, "SOURCE_RECORD", "rec_000002");
 * swmm_restart_set(engine, "MODE", "INITIAL_CONDITIONS");
 * @endcode
 */

#ifndef OPENSWMM_ENGINE_RESTART_H
#define OPENSWMM_ENGINE_RESTART_H

#include <stddef.h>

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief swmm_restart_probe(): the path does not exist or is an empty file. */
#define SWMM_RESTART_BACKEND_NONE 0
/** @brief swmm_restart_probe(): a native or SWMM5 hot start file. */
#define SWMM_RESTART_BACKEND_HOTSTART 1
/** @brief swmm_restart_probe(): an SQLite database, such as a GeoPackage. */
#define SWMM_RESTART_BACKEND_GEOPACKAGE 2
/** @brief swmm_restart_probe(): an HDF5 file. */
#define SWMM_RESTART_BACKEND_HDF5 3
/** @brief swmm_restart_probe(): any other file, or a directory. */
#define SWMM_RESTART_BACKEND_OTHER 4

/**
 * @brief Read one [RESTART] setting as text.
 *
 * Keys, case-insensitive: SOURCE_FILE, SOURCE_RUN, SOURCE_RECORD, MODE (INITIAL_CONDITIONS or
 * CONTINUE), RECORD_FILE, RECORD_FORMAT (GEOPACKAGE, HDF5 or empty), RECORD_EVERY (seconds as a
 * decimal number, 0 for none) and RECORD_CONTINUATION (YES or NO). Paths are returned as authored.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] key Non-null key listed above.
 * @param[out] buf Non-null caller-owned buffer; receives NUL-terminated text, silently truncated to
 *     buflen-1 bytes.
 * @param[in] buflen Positive capacity in bytes including the NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for a null engine; SWMM_ERR_BADPARAM for an unknown key or
 *     invalid buffer.
 */
SWMM_ENGINE_API int swmm_restart_get(SWMM_Engine engine, const char* key, char* buf, int buflen);

/**
 * @brief Change one [RESTART] setting.
 *
 * Accepts the keys of swmm_restart_get(). An empty value clears a path, ID or format, sets MODE to
 * INITIAL_CONDITIONS, RECORD_EVERY to 0 and RECORD_CONTINUATION to NO. Whether settings agree with
 * each other is checked when the engine initializes, not here.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] key Non-null key.
 * @param[in] value Non-null NUL-terminated value: MODE, RECORD_FORMAT and RECORD_CONTINUATION take
 *     their keywords case-insensitively; RECORD_EVERY takes nonnegative seconds.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for a null engine; SWMM_ERR_BADPARAM for an unknown key or a
 *     value the key does not accept.
 */
SWMM_ENGINE_API int swmm_restart_set(SWMM_Engine engine, const char* key, const char* value);

/**
 * @brief Count the RECORD_AT rows.
 * @param[in] engine Non-null live engine handle.
 * @param[out] count Non-null destination.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE or SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_restart_record_at_count(SWMM_Engine engine, int* count);

/**
 * @brief Read one RECORD_AT row.
 * @param[in] engine Non-null live engine handle.
 * @param[in] idx Zero-based row index.
 * @param[out] datetime Non-null destination: decimal days, or 0.0 for END.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM for a null pointer or index out of range.
 */
SWMM_ENGINE_API int swmm_restart_record_at_get(SWMM_Engine engine, int idx, double* datetime);

/**
 * @brief Append one RECORD_AT row.
 * @param[in] engine Non-null live engine handle.
 * @param[in] datetime Decimal days, or 0.0 for END. Negative values are refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM for a negative value.
 */
SWMM_ENGINE_API int swmm_restart_record_at_add(SWMM_Engine engine, double datetime);

/**
 * @brief Remove every RECORD_AT row.
 * @param[in] engine Non-null live engine handle.
 * @return SWMM_OK or SWMM_ERR_BADHANDLE.
 */
SWMM_ENGINE_API int swmm_restart_record_at_clear(SWMM_Engine engine);

/**
 * @brief Whether the run restores or records an engine checkpoint through [RESTART].
 *
 * True for MODE CONTINUE with a source, and for recording with RECORD_CONTINUATION YES or for a
 * model whose restart needs the checkpoint (heat transport, frozen ground, an independent
 * groundwater clock). Such a run that saves binary results carries them in its checkpoints, and
 * so needs a new binary output file: an existing one is refused at start.
 *
 * @param[in] engine Non-null engine after swmm_engine_initialize(), which resolves the source.
 * @param[out] uses Non-null destination: 1 or 0.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE; SWMM_ERR_BADPARAM for a null pointer.
 */
SWMM_ENGINE_API int swmm_restart_uses_checkpoint(SWMM_Engine engine, int* uses);

/**
 * @brief Tell what kind of file a path holds, from its first bytes.
 *
 * Reads at most the first 1 KiB and never changes the file.
 *
 * @param[in] path Non-null NUL-terminated UTF-8 path.
 * @param[out] backend Non-null destination for one SWMM_RESTART_BACKEND_* value.
 * @return SWMM_OK; SWMM_ERR_BADPARAM for a null argument.
 */
SWMM_ENGINE_API int swmm_restart_probe(const char* path, int* backend);

/**
 * @brief Describe the runs and restart records of a GeoPackage or HDF5 file as JSON.
 *
 * Opens the file read-only and never changes it. The object has `path`, `backend`
 * (`GEOPACKAGE` or `HDF5`) and `runs`; each run has `run_id`, `created_at`, `engine_version`,
 * `parent_run_id`, `parent_record_id`, `restart_mode`, `restart_source` and `records`; each record
 * has `record_id`, `ordinal`, `created_at`, `model_datetime` (decimal days of the accepted step),
 * `elapsed_seconds`, `requested_datetime` (decimal days, 0 for END), `hsf_version`,
 * `continuation`, `binary_output`, `byte_count`, `sha256`, `platform`, `engine_version` and
 * `status`. Only `complete` records can be restored. A file with no restart records lists no runs.
 *
 * Call with `utf8` NULL and `capacity` 0 to learn the size, then again with a buffer.
 *
 * @param[in] path Non-null NUL-terminated UTF-8 path.
 * @param[out] utf8 Destination for the NUL-terminated JSON, or NULL to query the size.
 * @param[in] capacity Bytes available at `utf8`; must be 0 when `utf8` is NULL.
 * @param[out] required Non-null destination for the size including the NUL.
 * @return SWMM_OK; SWMM_ERR_BADPARAM for invalid arguments or a buffer that is too small;
 *     SWMM_ERR_HOTSTART when the file is missing, not a GeoPackage or HDF5 file, or unreadable.
 *     swmm_restart_last_error() gives the reason.
 */
SWMM_ENGINE_API int swmm_restart_inspect_json(const char* path, char* utf8, size_t capacity,
                                              size_t* required);

/**
 * @brief Reason for the calling thread's last failed swmm_restart_inspect_json().
 * @return Borrowed NUL-terminated text, valid until the thread's next restart call; empty after a
 *     success.
 */
SWMM_ENGINE_API const char* swmm_restart_last_error(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_ENGINE_RESTART_H */
