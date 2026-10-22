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
 * @file      openswmm_hotstart.h
 * @brief     Hot start file management — transparent C API.
 *
 * @details Hot start files capture the hydraulic and water quality state of a
 *          SWMM simulation so it can be resumed later without running a warm-up
 *          period from dry-weather initial conditions.
 *
 *          The new hot start format (OPENSWMM_HS_V1) extends the legacy SWMM
 *          binary hot start format with:
 *          - An object UUID map for robust missing-object detection
 *          - CRS string for spatial consistency checking
 *          - User flag values
 *          - A CRC32 integrity checksum
 *
 * @section hotstart_api_workflow Workflow
 *
 * @code{.c}
 * // ---- Save a hot start ----
 * SWMM_Engine e = swmm_engine_create();
 * swmm_engine_open(e, "warmup.inp", "warmup.rpt", "warmup.out", NULL);
 * swmm_engine_initialize(e);
 * swmm_engine_start(e, 0);
 * double t = 0.0;
 * while (swmm_engine_step(e, &t) == SWMM_OK && t > 0.0) {}
 * swmm_hotstart_save(e, "warmup.hs");
 * swmm_engine_end(e);
 * swmm_engine_close(e);
 * swmm_engine_destroy(e);
 *
 * // ---- Apply a hot start ----
 * SWMM_HotStart hs = NULL;
 * swmm_hotstart_open("warmup.hs", &hs);
 *
 * SWMM_Engine e2 = swmm_engine_create();
 * swmm_engine_open(e2, "storm.inp", "storm.rpt", "storm.out", NULL);
 * swmm_engine_initialize(e2);
 * swmm_hotstart_apply(e2, hs);   // warnings issued for missing objects
 *
 * int nwarn = swmm_hotstart_warning_count(hs);
 * for (int i = 0; i < nwarn; i++) {
 *     printf("Warning: %s\n", swmm_hotstart_warning(hs, i));
 * }
 *
 * swmm_hotstart_close(hs);
 * @endcode
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_hotstart Hot Start File API
 * @ingroup  new_engine
 *
 * @see Legacy reference: src/solver/hotstart.c — swmm_saveHotstart(), swmm_useHotstart()
 */

#ifndef OPENSWMM_ENGINE_HOTSTART_H
#define OPENSWMM_ENGINE_HOTSTART_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle to an open hot start file. */
typedef void* SWMM_HotStart;

/** @brief Magic string written at the start of every OPENSWMM_HS_V1 file. */
#define OPENSWMM_HOTSTART_MAGIC "OPENSWMM_HS_V1\0"

/** @brief Base format version. Writers select compatible extensions for the configured model state, including Richards cell water/material
 * identity, full-engine checkpoints, snow and groundwater transport state.
 * The macro identifies the base format, not the highest extension version. */
#define OPENSWMM_HOTSTART_VERSION 1

/* =========================================================================
 * [FILES] section — `SAVE HOTSTART` entry management (Slice BV-01)
 *
 * These functions manage the *static* list of scheduled hot-start save
 * entries that live in the `[FILES]` section of an `.inp` file
 * (`SimulationContext::hotstart_saves`).  They are distinct from the
 * runtime `swmm_hotstart_save()` function below, which writes a single
 * hot-start file at the current sim time.
 *
 * Each entry is a `{path, datetime, format}` triple.  A `datetime` of `0.0`
 * means "no datetime — save at end of run" (matches legacy semantics);
 * `format` is one of `SWMM_HOTSTART_FORMAT_*` (default AUTO).
 *
 * The legacy singular `swmm_files_set("HOTSTART_SAVE_PATH", …)` /
 * `_DATETIME` keys remain as slot-0 sugar.
 * ========================================================================= */

/**
 * @brief Count scheduled SAVE HOTSTART authoring entries.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @param[out] count Non-null destination for the stored entry count.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_saves_count(SWMM_Engine engine, int* count);

/**
 * @brief Copy the authored path of one scheduled save entry.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @param[in] idx Zero-based existing SAVE HOTSTART slot index; invalid indices return BADPARAM
 *     rather than BADINDEX.
 * @param[out] buf Non-null caller-owned destination, NUL-terminated and silently truncated to
 *     buflen-1 bytes.
 * @param[in] buflen Positive buffer capacity in bytes, including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_saves_get_path(SWMM_Engine engine, int idx, char* buf,
                                                 int buflen);

/**
 * @brief Read the scheduled calendar date of one save entry.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @param[in] idx Zero-based existing SAVE HOTSTART slot index; invalid indices return BADPARAM
 *     rather than BADINDEX.
 * @param[out] datetime Non-null destination for SWMM DateTime decimal days since 1899-12-30; 0
 *     denotes end of run.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_saves_get_datetime(SWMM_Engine engine, int idx, double* datetime);

/**
 * @brief Replace a scheduled save path without removing its row.
 *
 * Unlike the legacy slot-0 swmm_files_set() sugar, an empty path with zero datetime does not
 * auto-remove this entry. Use remove/clear explicitly.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @param[in] idx Zero-based existing SAVE HOTSTART slot index; invalid indices return BADPARAM
 *     rather than BADINDEX.
 * @param[in] path Non-null NUL-terminated path to copy; empty is permitted and clears the
 *     token/cached absolute path.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_saves_set_path(SWMM_Engine engine, int idx, const char* path);

/**
 * @brief Assign a scheduled save calendar date directly.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @param[in] idx Zero-based existing SAVE HOTSTART slot index; invalid indices return BADPARAM
 *     rather than BADINDEX.
 * @param[in] datetime SWMM DateTime decimal days since 1899-12-30; 0 saves at run end. No
 *     finite/range/order validation is performed.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_saves_set_datetime(SWMM_Engine engine, int idx, double datetime);

/**
 * @brief File formats a `SAVE HOTSTART` entry can write.
 *
 * @details In the `.inp` the format is an optional keyword after the path
 *          and optional datetime: `SAVE HOTSTART "f.hsf" [date time]
 *          LEGACY|NATIVE`. A row without one is AUTO, the behaviour of
 *          every earlier file: the legacy SWMM5 `.hsf` routing format, or
 *          the native format when the model has node LIDs.
 *          `USE HOTSTART` reads either; it goes by the file's own stamp.
 *          NATIVE also carries what the legacy format has no room for
 *          (see swmm_hotstart_legacy_omits()); legacy SWMM 5 cannot read it.
 */
#define SWMM_HOTSTART_FORMAT_AUTO 0   /**< Legacy, or native with node LIDs */
#define SWMM_HOTSTART_FORMAT_LEGACY 1 /**< EPA SWMM5 `.hsf`                 */
#define SWMM_HOTSTART_FORMAT_NATIVE 2 /**< OPENSWMM_HS_V1                   */

/**
 * @brief Read the authored format selector of one scheduled save.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @param[in] idx Zero-based existing SAVE HOTSTART slot index; invalid indices return BADPARAM
 *     rather than BADINDEX.
 * @param[out] format Non-null destination for SWMM_HOTSTART_FORMAT_AUTO, LEGACY or NATIVE.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_saves_get_format(SWMM_Engine engine, int idx, int* format);

/**
 * @brief Select the authored format of an existing scheduled save.
 *
 * This setter stores a preference; actual restart-profile support is evaluated when saving.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @param[in] idx Zero-based existing SAVE HOTSTART slot index; invalid indices return BADPARAM
 *     rather than BADINDEX.
 * @param[in] format SWMM_HOTSTART_FORMAT_AUTO (0), LEGACY (1) or NATIVE (2). Other values are
 *     refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_saves_set_format(SWMM_Engine engine, int idx, int format);

/** @brief Bits of swmm_hotstart_legacy_omits(). */
#define SWMM_HOTSTART_OMITS_WATER_AGE 0x1        /**< `[OPTIONS] WATER_AGE YES`  */
#define SWMM_HOTSTART_OMITS_REACTION_SPECIES 0x2 /**< reactions-component species */
#define SWMM_HOTSTART_OMITS_AQUIFER 0x4          /**< 2D aquifer rows            */
#define SWMM_HOTSTART_OMITS_NODE_LIDS 0x8        /**< LID units at storage nodes */

/**
 * @brief Report selected model features that legacy routing hotstarts cannot retain.
 *
 * Reads parsed authoring before initialization and does not write or validate a complete restart.
 * The mask is not an exhaustive restart-completeness certificate: legacy/native formats and
 * qualified full-engine checkpoints retain different state.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @param[out] flags Non-null OR-mask of SWMM_HOTSTART_OMITS_WATER_AGE, REACTION_SPECIES, AQUIFER
 *     and NODE_LIDS; zero when none of these features are configured.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_legacy_omits(SWMM_Engine engine, int* flags);

/**
 * @brief Append an AUTO-format scheduled save authoring entry.
 *
 * Appending does not validate collisions or file writability and does not emit the file
 * immediately.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @param[in] path Non-null NUL-terminated path to copy; empty creates a placeholder row.
 * @param[in] datetime SWMM DateTime decimal days since 1899-12-30; 0 means run end. Stored without
 *     finite/date validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_saves_add(SWMM_Engine engine, const char* path, double datetime);

/**
 * @brief Remove a scheduled save entry and shift later indices down.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @param[in] idx Zero-based existing SAVE HOTSTART slot index; invalid indices return BADPARAM
 *     rather than BADINDEX.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_saves_remove(SWMM_Engine engine, int idx);

/**
 * @brief Remove all scheduled save authoring entries.
 *
 * Does not delete any existing files already written by earlier entries.
 *
 * @param[in] engine Non-null live engine with exclusive caller access. Scheduled-entry authoring
 *     APIs impose no lifecycle guard and do not write a file immediately.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_hotstart_saves_clear(SWMM_Engine engine);

/* =========================================================================
 * Create / save
 * ========================================================================= */

/**
 * @brief Write current simulation state through the native checkpoint or state-IO path.
 *
 * A heat model with a qualified attached full-engine checkpoint uses its accepted native restart.
 * Otherwise state-IO plugins are tried in order until one succeeds, with a built-in record-format
 * fallback when no state plugin is registered. Record files match objects by ID; full-engine
 * checkpoints require matching model/provider/runtime state and are restored at a subsequent start.
 * This is not the exclusive new-path checkpoint API and does not promise no-replace publication.
 *
 * @param[in] engine Non-null engine in RUNNING or ENDED; exclusive access is required.
 * @param[in] path Non-null, nonempty NUL-terminated destination path, borrowed for the synchronous
 *     call.
 * @return SWMM_OK on successful write; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADPARAM for
 *     null/empty path, SWMM_ERR_LIFECYCLE outside RUNNING/ENDED, or SWMM_ERR_HOTSTART on
 *     checkpoint/state-writer failure.
 */
SWMM_ENGINE_API int swmm_hotstart_save(SWMM_Engine engine, const char* path);

/**
 * @brief Borrow the built-in hot-start reader/writer diagnostic for this thread.
 *
 * This diagnostic belongs to HotStartManager, not the independent checkpoint diagnostic or every
 * possible state-IO plugin.
 * @return NUL-terminated thread-local string, empty when no detailed IO diagnostic is available. Do
 *     not free it; copy before another hot-start IO call on this thread.
 */
SWMM_ENGINE_API const char* swmm_hotstart_last_io_error(void);

/* =========================================================================
 * Open / read
 * ========================================================================= */

/**
 * @brief Load a native OPENSWMM hot-start file into an independent handle.
 *
 * Reads the file into owned in-memory records and restart extensions; no engine handle is retained.
 * Release with swmm_hotstart_close(). This reader is distinct from the model-bound legacy USE
 * HOTSTART loader.
 *
 * @param[in] path Non-null, nonempty NUL-terminated native hot-start path; borrowed for the call.
 * @param[out] hs Non-null output slot. Set to NULL before path/read validation, then to a newly
 *     allocated caller-owned handle on success. A previous handle in this variable is not freed.
 * @return SWMM_OK; SWMM_ERR_BADPARAM for null output or null/empty path; SWMM_ERR_HOTSTART when
 *     native magic/version/checksum/content validation fails.
 */
SWMM_ENGINE_API int swmm_hotstart_open(const char* path, SWMM_HotStart* hs);

/* =========================================================================
 * Apply
 * ========================================================================= */

/**
 * @brief Apply record state or queue a complete restart on an initialized engine.
 *
 * Record-based application matches string IDs, keeps missing target objects unchanged, records
 * missing source objects as warnings and reopens mass-balance initial storage on success. This
 * wrapper passes a no-op warning sink; use swmm_hotstart_warning_count()/warning() rather than
 * assuming a registered engine warning callback fires. Full-engine checkpoint payloads are queued
 * here and restored during subsequent engine start, after owners initialize; queue acceptance is
 * not completed restoration. Record application is not promised atomic on every extension failure.
 *
 * @param[in] engine Non-null INITIALIZED engine whose model and restart owners support the file
 *     profile.
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @return SWMM_OK when records apply or a full-engine restart is queued; SWMM_ERR_BADHANDLE for a
 *     null handle, SWMM_ERR_LIFECYCLE outside INITIALIZED, or SWMM_ERR_HOTSTART for a
 *     refused/incompatible restart.
 */
SWMM_ENGINE_API int swmm_hotstart_apply(SWMM_Engine engine, SWMM_HotStart hs);

/* =========================================================================
 * Modify hot start data
 * ========================================================================= */

/**
 * @brief Edit one stored node depth record before application/write-back.
 *
 * Marks the record file dirty for swmm_hotstart_close() write-back. Does not modify an already
 * running engine or recalculate related fields; a full-engine payload without this record returns
 * BADPARAM.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @param[in] node_id Non-null NUL-terminated node ID, matched exactly/case-sensitively against
 *     stored records.
 * @param[in] depth New depth in internal feet (ft), regardless of project display units; assigned
 *     without finite/range validation.
 * @return SWMM_OK when a matching record is modified; SWMM_ERR_BADHANDLE for null hs or
 *     SWMM_ERR_BADPARAM for null/unmatched ID.
 */
SWMM_ENGINE_API int swmm_hotstart_set_node_depth(SWMM_HotStart hs, const char* node_id,
                                                 double depth);

/**
 * @brief Edit one stored node head record before application/write-back.
 *
 * Marks the record file dirty for swmm_hotstart_close() write-back. Does not modify an already
 * running engine or recalculate related fields; a full-engine payload without this record returns
 * BADPARAM.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @param[in] node_id Non-null NUL-terminated node ID, matched exactly/case-sensitively against
 *     stored records.
 * @param[in] head New head in internal datum feet (ft); assigned without finite/range validation.
 * @return SWMM_OK when a matching record is modified; SWMM_ERR_BADHANDLE for null hs or
 *     SWMM_ERR_BADPARAM for null/unmatched ID.
 */
SWMM_ENGINE_API int swmm_hotstart_set_node_head(SWMM_HotStart hs, const char* node_id, double head);

/**
 * @brief Edit one stored link flow record before application/write-back.
 *
 * Marks the record file dirty for swmm_hotstart_close() write-back. Does not modify an already
 * running engine or recalculate related fields; a full-engine payload without this record returns
 * BADPARAM.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @param[in] link_id Non-null NUL-terminated link ID, matched exactly/case-sensitively against
 *     stored records.
 * @param[in] flow New flow in internal cubic feet per second (ft3/s), signed in stored link
 *     orientation; assigned without finite/range validation.
 * @return SWMM_OK when a matching record is modified; SWMM_ERR_BADHANDLE for null hs or
 *     SWMM_ERR_BADPARAM for null/unmatched ID.
 */
SWMM_ENGINE_API int swmm_hotstart_set_link_flow(SWMM_HotStart hs, const char* link_id, double flow);

/**
 * @brief Edit one stored link depth record before application/write-back.
 *
 * Marks the record file dirty for swmm_hotstart_close() write-back. Does not modify an already
 * running engine or recalculate related fields; a full-engine payload without this record returns
 * BADPARAM.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @param[in] link_id Non-null NUL-terminated link ID, matched exactly/case-sensitively against
 *     stored records.
 * @param[in] depth New depth in internal feet (ft); assigned without finite/range validation.
 * @return SWMM_OK when a matching record is modified; SWMM_ERR_BADHANDLE for null hs or
 *     SWMM_ERR_BADPARAM for null/unmatched ID.
 */
SWMM_ENGINE_API int swmm_hotstart_set_link_depth(SWMM_HotStart hs, const char* link_id,
                                                 double depth);

/**
 * @brief Edit one stored subcatchment runoff record before application/write-back.
 *
 * Marks the record file dirty for swmm_hotstart_close() write-back. Does not modify an already
 * running engine or recalculate related fields; a full-engine payload without this record returns
 * BADPARAM.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @param[in] subcatch_id Non-null NUL-terminated subcatchment ID, matched exactly/case-sensitively
 *     against stored records.
 * @param[in] runoff New runoff in internal cubic feet per second (ft3/s); assigned without
 *     finite/range validation.
 * @return SWMM_OK when a matching record is modified; SWMM_ERR_BADHANDLE for null hs or
 *     SWMM_ERR_BADPARAM for null/unmatched ID.
 */
SWMM_ENGINE_API int swmm_hotstart_set_subcatch_runoff(SWMM_HotStart hs, const char* subcatch_id,
                                                      double runoff);

/* =========================================================================
 * Query hot start metadata
 * ========================================================================= */

/**
 * @brief Read the saved elapsed simulation time.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @param[out] sim_time Non-null destination for elapsed seconds since the saving run start calendar
 *     date.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_get_sim_time(SWMM_HotStart hs, double* sim_time);

/**
 * @brief Read the saving run start calendar date.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @param[out] start_date Non-null destination for OADate/SWMM DateTime decimal days since
 *     1899-12-30; no timezone conversion.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_get_start_date(SWMM_HotStart hs, double* start_date);

/**
 * @brief Copy the hot-start header CRS string.
 *
 * Returns stored metadata without validating coordinates or compatibility with another model.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @param[out] buf Non-null caller-owned destination; success NUL-terminates and silently truncates
 *     to buflen-1 bytes. An absent CRS is an empty string.
 * @param[in] buflen Positive byte capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null handle or SWMM_ERR_BADPARAM for an invalid required
 *     argument or slot.
 */
SWMM_ENGINE_API int swmm_hotstart_get_crs(SWMM_HotStart hs, char* buf, int buflen);

/**
 * @brief Count stored node records in a hot-start handle.
 *
 * A full-engine checkpoint payload need not expose its embedded state as editable record rows.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @return Stored node-record count, or -1 for null hs.
 */
SWMM_ENGINE_API int swmm_hotstart_node_count(SWMM_HotStart hs);

/**
 * @brief Count stored link records in a hot-start handle.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @return Stored link-record count, or -1 for null hs.
 */
SWMM_ENGINE_API int swmm_hotstart_link_count(SWMM_HotStart hs);

/* =========================================================================
 * Warning access (populated after swmm_hotstart_apply())
 * ========================================================================= */

/**
 * @brief Count warnings retained by the hot-start handle.
 *
 * Record-based apply clears/repopulates this list; it can contain missing-object and
 * restart-profile diagnostics. The full-checkpoint queue path does not promise to refresh this
 * list.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @return Warning count, or 0 for null hs.
 */
SWMM_ENGINE_API int swmm_hotstart_warning_count(SWMM_HotStart hs);

/**
 * @brief Borrow one retained hot-start warning.
 *
 * @param[in] hs Non-null live hot-start handle returned by swmm_hotstart_open(); caller retains
 *     ownership until close.
 * @param[in] index Zero-based index in [0,swmm_hotstart_warning_count(hs)).
 * @return Handle-owned NUL-terminated warning string, or NULL for null hs/out-of-range index. Never
 *     free it; copy before applying again or closing hs.
 */
SWMM_ENGINE_API const char* swmm_hotstart_warning(SWMM_HotStart hs, int index);

/* =========================================================================
 * Close
 * ========================================================================= */

/**
 * @brief Flush dirty record edits and destroy a hot-start handle.
 *
 * Unmodified handles do not rewrite the file. Setters mark their records dirty; close attempts to
 * persist those changes to the original path before freeing all storage.
 *
 * @param[in] hs Caller-owned live hot-start handle, or NULL for a successful no-op. The handle is
 *     destroyed even if flush fails; do not retry close or use it afterward.
 * @return SWMM_OK after close/no-op; SWMM_ERR_IO when dirty write-back fails.
 */
SWMM_ENGINE_API int swmm_hotstart_close(SWMM_HotStart hs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_ENGINE_HOTSTART_H */
