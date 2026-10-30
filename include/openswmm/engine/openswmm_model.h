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
 * @file      openswmm_model.h
 * @brief     OpenSWMM Engine — Model building and options C API.
 *
 * @details Covers:
 *   - Programmatic model construction (swmm_engine_new, object add)
 *   - Model finalisation and validation
 *   - Serialisation to .inp file
 *   - Standard and extension OPTIONS
 *   - CRS access
 *   - User flags
 *
 * @note Include this header independently or get it via openswmm_engine.h.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h — lifecycle and error codes
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_MODEL_H
#define OPENSWMM_MODEL_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Model building — programmatic construction (state guard: BUILDING only)
 * ========================================================================= */

/**
 * @brief Create an empty programmatically editable engine in SWMM_STATE_BUILDING.
 *
 * Add model objects through their APIs, then call swmm_finalize_model() before start. No input file
 * is required; do not call ordinary open() on this BUILDING handle.
 * @return Caller-owned engine handle, or NULL when construction fails. Destroy with
 *     swmm_engine_destroy().
 */
SWMM_ENGINE_API SWMM_Engine swmm_engine_new(void);

/* =========================================================================
 * Model finalisation and validation
 * ========================================================================= */

/**
 * @brief Check the minimal programmatic topology prerequisites without changing state.
 *
 * Checks at least one node, at least one outfall and that every link endpoint indexes an existing
 * node. Does not perform a duplicate-ID scan, connectivity analysis, full input validation or emit
 * validation warnings in this wrapper.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED.
 * @return SWMM_OK when checks pass; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED, or SWMM_ERR_BADPARAM for missing nodes/outfall or invalid link endpoints.
 */
SWMM_ENGINE_API int swmm_validate_model(SWMM_Engine engine);

/**
 * @brief Resolve and initialize a programmatically built model.
 *
 * Runs swmm_validate_model(), resolves cross-references and display-to-internal units/derived
 * geometry, sets OPENED and delegates to ordinary initialization. A later failure need not leave
 * BUILDING intact; inspect state/errors before retrying. On success call swmm_engine_start().
 *
 * @param[in] engine Non-null BUILDING engine.
 * @return SWMM_OK on transition to INITIALIZED; SWMM_ERR_BADHANDLE for null engine,
 *     SWMM_ERR_LIFECYCLE outside BUILDING, a topology-validation error, or the initialization
 *     error.
 */
SWMM_ENGINE_API int swmm_finalize_model(SWMM_Engine engine);

/* =========================================================================
 * Model serialisation
 * ========================================================================= */

/**
 * @brief Serialize the current model with the built-in INP writer.
 *
 * Appends writer diagnostics to the engine warning list, including substitutions/omissions that do
 * not prevent successful serialization. Uses full native format and may write mesh/component
 * companions. Success is serialization, not an atomic project publication; inspect warnings. No
 * lifecycle guard is imposed by this wrapper.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] new_inp_path Non-null NUL-terminated destination path; borrowed for this call.
 *     Existing output files may be replaced.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. Otherwise returns the built-in writer error.
 */
SWMM_ENGINE_API int swmm_model_write(SWMM_Engine engine, const char* new_inp_path);

/**
 * @brief Map a final writer output path to its private physical staging destination.
 *
 * Called synchronously once for each output actually written. Must not throw, mutate or reenter the
 * engine. The mapped path must differ from final outputs and other staged destinations.
 *
 * @param[in] user_data Borrowed context supplied to swmm_model_write_staged(); may be NULL.
 * @param[in] final_path Borrowed NUL-terminated UTF-8 intended final destination, valid during this
 *     callback.
 * @param[in] kind Output category: 0=model, 1=mesh, 2=component configuration.
 * @return Borrowed NUL-terminated UTF-8 staging path; NULL/empty refuses the output. Storage must
 *     remain valid until at least the next callback; the writer copies it before use.
 */
typedef const char* (*SWMM_StageOutputCallback)(void* user_data, const char* final_path, int kind);
/**
 * @brief Serialize built-in INP outputs through a mandatory caller staging mapper.
 *
 * Only mapped output directories may be created. References remain relative to final_inp_path; the
 * caller owns staging cleanup, validation and publication. Plugins are excluded, and diagnostics
 * append to the engine warning list. Callback must not throw or mutate/reenter the engine.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] final_inp_path Non-null, nonempty final model path used to form all serialized
 *     references; not the mapped staging filename.
 * @param[in] mapper Non-null synchronous output mapper. Every actual model/mesh/component output
 *     must map to a separate staging destination distinct from all final outputs.
 * @param[in] user_data Borrowed opaque pointer passed unchanged to mapper; may be NULL and must
 *     remain valid until return.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. Otherwise returns the built-in writer error.
 */
SWMM_ENGINE_API int swmm_model_write_staged(SWMM_Engine engine, const char* final_inp_path,
                                            SWMM_StageOutputCallback mapper, void* user_data);

/** Write profile for swmm_model_write_compat(). */
typedef enum SWMM_InpProfile {
    SWMM_INP_PROFILE_FULL = 0,       /**< native OpenSWMM format (= swmm_model_write) */
    SWMM_INP_PROFILE_SWMM5 = 1,      /**< readable by the OpenSWMM legacy 5.3.0 engine */
    SWMM_INP_PROFILE_SWMM5_STOCK = 2 /**< readable by a stock EPA SWMM 5 engine (5.2.4) */
} SWMM_InpProfile;

/**
 * @brief Serialize the model to a selected INP compatibility profile.
 *
 * `SWMM_INP_PROFILE_SWMM5` produces a file a SWMM 5.x engine reads:  *          v6-only sections
 * ([VIRTUAL_JUNCTIONS], [INLET_JUNCTIONS], [2D_*],  *          [PLUGINS], [PROCESS_COMPONENTS],
 * [USER_FLAGS], [USER_FLAG_VALUES],  *          [RDII_DECAY]) and option keys are omitted,
 * `FLOW_ROUTING FV` is  *          written as `DYNWAVE`, `SURCHARGE_METHOD DYNAMIC_SLOT`/`TPA` as
 * *          `SLOT`, a virtual junction becomes an ordinary junction, and an  *          inlet
 * junction becomes an ordinary junction plus an [INLET_USAGE]  *          row on its approach
 * conduit (same design, same capture node).  *          A `SAVE HOTSTART` row loses its LEGACY /
 * NATIVE format keyword.  *          Each substitution is appended to the engine's warning list  *
 * (swmm_get_warning_count / swmm_get_warning_at). The file is a run  *          artifact: the model
 * held by the engine is not changed.  *   *          `SWMM_INP_PROFILE_SWMM5_STOCK` is the same for
 * a STOCK EPA SWMM 5  *          engine: it also drops the positional extensions only the OpenSWMM
 * *          legacy 5.3.0 engine parses — subcatchment rain/snow scale factors  *          (and the
 * `*` snowpack placeholder that holds their position) and  *          the rain-gage scale factor —
 * which a stock parser rejects as an  *          undefined object. A dropped factor other than 1.0
 * is reported as  *          a warning. Use it for any 5.x engine that is not the in-tree one.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] new_inp_path Non-null NUL-terminated destination path, borrowed for this call;
 *     existing outputs can be replaced.
 * @param[in] profile SWMM_INP_PROFILE_FULL, SWMM_INP_PROFILE_SWMM5 or SWMM_INP_PROFILE_SWMM5_STOCK.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. An unknown profile is BADPARAM; otherwise returns the writer error.
 *     Successful substitutions/omissions append engine warnings.
 */
SWMM_ENGINE_API int swmm_model_write_compat(SWMM_Engine engine, const char* new_inp_path,
                                            int profile);

/**
 * @brief Serialize through a transient named input/writer plugin.
 *
 * Creates a temporary writer so the primary input plugin is not reused. Plugin finalize is
 * best-effort; its result does not override the write result. The built-in path appends writer
 * warnings; component lookup failures are returned without emitting a load warning. Does not
 * guarantee atomic publication.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] new_path Non-null NUL-terminated destination path; borrowed for the call.
 * @param[in] output_plugin_id Borrowed library path, plugin ID or id:version. NULL/empty selects
 *     the built-in INP writer.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADPARAM for null path
 *     or unresolved component, SWMM_ERR_PLUGIN when the component cannot provide/initialize/write
 *     through an input plugin, or a built-in writer error.
 */
SWMM_ENGINE_API int swmm_model_write_with_plugin(SWMM_Engine engine, const char* new_path,
                                                 const char* output_plugin_id);

/* =========================================================================
 * [PLUGINS] section access (Slice AA-3.1 Phase B)
 *
 * The [PLUGINS] section of an .inp file is a list of rows, one per
 * plugin to load.  Each row is `path arg1 arg2 …` where `path` resolves
 * via the same logic as the input plugin lib (a library path, an id, or
 * an `id:version` pair) and the trailing tokens are passed to the
 * plugin's initialize() method as `init_args`.
 *
 * The accessors below let a host (GUI) read, mutate, and remove rows
 * without re-parsing the .inp file.  Plugins themselves are
 * (re-)resolved by PluginFactory at swmm_engine_open / swmm_model_write
 * time — these accessors only mutate the in-memory `plugin_specs` list.
 * ========================================================================= */

/**
 * @brief Count stored [PLUGINS] specifications.
 *
 * Counts authoring rows, not loaded plugin instances.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] count Non-null destination for the number of stored plugin rows.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_plugins_count(SWMM_Engine engine, int* count);

/**
 * @brief Read one stored [PLUGINS] row without resolving/loading it.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] idx Zero-based specification index in [0,plugin count).
 * @param[out] path_buf Optional caller-owned path/ID buffer; NUL-terminated and silently truncated
 *     when capacity is positive.
 * @param[in] path_buf_sz Buffer capacity including NUL; nonpositive suppresses path output.
 * @param[out] args_buf Optional caller-owned argument buffer. Arguments are joined with spaces and
 *     quoted as needed to preserve token boundaries.
 * @param[in] args_buf_sz Buffer capacity including NUL; nonpositive suppresses argument output.
 * @return SWMM_OK, including truncated/suppressed output; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADINDEX for invalid idx.
 */
SWMM_ENGINE_API int swmm_plugin_get(SWMM_Engine engine, int idx, char* path_buf, int path_buf_sz,
                                    char* args_buf, int args_buf_sz);

/**
 * @brief Add or replace a copied plugin specification by exact path/ID key.
 *
 * Replaces only init_args when the exact key exists; otherwise appends. Does not load, validate or
 * reinitialize the plugin immediately. Stored specifications are serialized on a later model write.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] path_or_id Non-null, nonempty NUL-terminated library path, plugin ID or id:version;
 *     matched case-sensitively as stored.
 * @param[in] args Borrowed space-separated argument text with tokenizer quoting support; NULL/empty
 *     means no arguments.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_plugin_set(SWMM_Engine engine, const char* path_or_id, const char* args);

/**
 * @brief Remove the first exact-matching stored plugin specification.
 *
 * Idempotent when no row matches. Does not unload an existing runtime instance or alter other
 * plugin rows.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] path_or_id Non-null NUL-terminated stored path/ID, matched case-sensitively.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_plugin_remove(SWMM_Engine engine, const char* path_or_id);

/* =========================================================================
 * [FILES] section access (Slice AA-3 follow-up — secondary file refs)
 *
 * Read / write the legacy SWMM5 `[FILES]` section: rainfall, runoff,
 * RDII, inflows, outflows, and hot-start file references.  The
 * accessors are keyed by an uppercase string so callers don't depend
 * on the field layout of `FilesSpec`.
 *
 * Recognised keys (case-insensitive):
 *   "RAINFALL_PATH"          / "RAINFALL_MODE"
 *   "RUNOFF_PATH"            / "RUNOFF_MODE"
 *   "RDII_PATH"              / "RDII_MODE"
 *   "INFLOWS_PATH"
 *   "OUTFLOWS_PATH"
 *   "HOTSTART_USE_PATH"
 *   "HOTSTART_SAVE_PATH"     / "HOTSTART_SAVE_DATETIME"
 *
 * `*_MODE` keys take "SAVE" / "USE" / "" (empty clears the slot).
 * `HOTSTART_SAVE_DATETIME` is a SWMM decimal-day floating-point string
 * (0 == unset, write at end of run).
 * ========================================================================= */

/**
 * @brief Read a legacy [FILES] authoring field by case-insensitive key.
 *
 * Paths use the original authored token. Missing hotstart slot 0 reads as empty path or numeric
 * 0.000000 date; dates are decimal-day strings. Unknown keys are refused before writing output.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] key Non-null key listed above, plus read-only HOTSTART_SAVE_COUNT.
 *     HOTSTART_SAVE_PATH/DATETIME address slot 0.
 * @param[out] buf Non-null caller-owned buffer; success writes NUL-terminated text, silently
 *     truncated to buflen-1 bytes.
 * @param[in] buflen Positive buffer capacity in bytes, including the NUL terminator.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_files_get(SWMM_Engine engine, const char* key, char* buf, int buflen);

/**
 * @brief Set one copied legacy [FILES] authoring field.
 *
 * Slot-0 hotstart setters create a row if needed and remove it when both path and date are
 * empty/zero. Date 0 means save at run end. Path assignment clears cached absolute resolution.
 * These authoring changes do not reopen live interface files; no lifecycle guard is imposed.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] key Non-null case-insensitive writable key listed above; HOTSTART_SAVE_COUNT is
 *     read-only.
 * @param[in] value Non-null NUL-terminated value: paths accept empty to clear; modes accept SAVE,
 *     USE or empty case-insensitively; HOTSTART_SAVE_DATETIME accepts a full decimal-day number or
 *     empty for 0.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_files_set(SWMM_Engine engine, const char* key, const char* value);

/* =========================================================================
 * External-file path slots — typed, broader API (Slice IO-9)
 * =========================================================================
 *
 * Reaches every external-file slot in the model — not just the [FILES]
 * section — so the GUI's portability normalizer can walk a uniform list
 * before save without per-role plumbing.
 *
 * Each slot carries both:
 *   - the original token as it appeared in the source `.inp`
 *     (relative-or-absolute, with whichever separators the author used);
 *   - the resolved absolute path the engine uses for `fopen`.
 *
 * The slot getter exposes both strings to the caller; the slot setter
 * updates the original token and clears the cached absolute resolution
 * (PostParseResolver re-fills it the next time it runs).
 *
 * See openswmm.gui/docs/IO_PORTABILITY_PLAN.md §3.3 for the storage model
 * and §3.7 for the GUI editor contract.
 * ========================================================================= */

/**
 * @brief Identifies a single external-file slot on the model.
 *
 * @details Vector slots (per-gage data, per-timeseries data, per-hot-start
 *          save entry) require an `owner` key to disambiguate; scalar
 *          slots ignore the `owner` argument.
 */
typedef enum SWMM_FilePathRole {
    /* Scalar slots — `owner` ignored. */
    SWMM_FILE_RAINFALL = 1,     /**< ctx.files.rainfall_path        */
    SWMM_FILE_RUNOFF = 2,       /**< ctx.files.runoff_path          */
    SWMM_FILE_RDII = 3,         /**< ctx.files.rdii_path            */
    SWMM_FILE_INFLOWS = 4,      /**< ctx.files.inflows_path         */
    SWMM_FILE_OUTFLOWS = 5,     /**< ctx.files.outflows_path        */
    SWMM_FILE_HOTSTART_USE = 6, /**< ctx.files.hotstart_use_path    */
    SWMM_FILE_CLIMATE_TEMP = 7, /**< ctx.options.temp_file          */

    /* Vector slots — `owner` selects the entry. */
    SWMM_FILE_HOTSTART_SAVE = 8,    /**< ctx.files.hotstart_saves[i],
                                       *  `owner` is decimal index "0".."N-1" */
    SWMM_FILE_RAINGAGE_DATA = 9,    /**< ctx.gages.file_path[i],
                                       *  `owner` is the gage id     */
    SWMM_FILE_TIMESERIES_DATA = 10, /**< ctx.tables.tables[i].file_path,
                                       *  `owner` is the series id   */

    /* Scalar slots — `owner` ignored. Unavailable (SWMM_ERR_BADPARAM) in
     * builds without 2D support. */
    SWMM_FILE_MESH_2D = 11,   /**< [2D_MESH_FILE] external .2dm   */
    SWMM_FILE_OUTPUT_2D = 12, /**< [2D_OPTIONS] OUTPUT_FILE .h5   */

    /* Vector slot — `owner` selects the entry. */
    SWMM_FILE_LID_REPORT = 13 /**< ctx.lid_usage.rpt_file[i],
                                       *  `owner` is decimal index "0".."N-1" */
} SWMM_FilePathRole;

/**
 * @brief Read resolved and original strings for an existing external-file slot.
 *
 * Unknown roles, unavailable stores or unresolved owner keys are BADPARAM. This call returns stored
 * strings and does not resolve paths or access their files.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] role SWMM_FilePathRole selecting a scalar or vector slot; 2D roles require a
 *     2D-enabled engine/options store.
 * @param[in] owner Borrowed vector selector: decimal hotstart/LID-report index, gage ID or
 *     timeseries ID. Required for vector roles; ignored (may be NULL) for scalar roles.
 * @param[out] absolute_buf Non-null caller-owned resolved-path buffer; may return empty when not
 *     yet resolved. NUL-terminated and silently truncated to capacity-1.
 * @param[in] absolute_buflen Positive absolute_buf capacity including NUL.
 * @param[out] original_buf Non-null caller-owned authored-token buffer; may return empty,
 *     NUL-terminated and silently truncated.
 * @param[in] original_buflen Positive original_buf capacity including NUL.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_file_path_get(SWMM_Engine engine, SWMM_FilePathRole role,
                                       const char* owner, char* absolute_buf, int absolute_buflen,
                                       char* original_buf, int original_buflen);

/**
 * @brief Replace an external-file token and invalidate its cached absolute path.
 *
 * Does not create vector owners, resolve paths, reopen files or validate file existence.
 * Unknown/unavailable roles and missing owners return BADPARAM.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] role SWMM_FilePathRole selecting an existing scalar/vector slot.
 * @param[in] owner Vector owner as for swmm_file_path_get(); may be NULL for scalar roles. The
 *     referenced gage/series/hotstart/LID-report row must already exist.
 * @param[in] new_path Non-null NUL-terminated token to copy; empty clears the slot. Relative paths
 *     remain relative until a later resolver pass.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_file_path_set(SWMM_Engine engine, SWMM_FilePathRole role,
                                       const char* owner, const char* new_path);

/* =========================================================================
 * Title / notes access
 * ========================================================================= */

/**
 * @brief Count stored [TITLE] note lines.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] count Non-null destination for the line count.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_title_get_count(SWMM_Engine engine, int* count);

/**
 * @brief Copy one stored title/note line.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] index Zero-based title-line index.
 * @param[out] buf Non-null caller-owned buffer; success writes NUL-terminated text, silently
 *     truncated to buflen-1 bytes.
 * @param[in] buflen Positive buffer capacity in bytes, including the NUL terminator.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. An out-of-range index returns SWMM_ERR_BADPARAM (not BADINDEX).
 */
SWMM_ENGINE_API int swmm_title_get_line(SWMM_Engine engine, int index, char* buf, int buflen);

/**
 * @brief Append a copied title/note entry.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] line Non-null NUL-terminated entry; an empty string appends a blank entry. Embedded
 *     newlines are not split by this function.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_title_add_line(SWMM_Engine engine, const char* line);

/**
 * @brief Replace title entries by splitting copied text at newline characters.
 *
 * Splits on LF only; any CR bytes remain in the corresponding stored entry.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] text Non-null NUL-terminated text. Empty clears all lines; internal blank lines are
 *     retained, a trailing newline does not add another final empty entry.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_title_set(SWMM_Engine engine, const char* text);

/**
 * @brief Remove all title/note entries.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_title_clear(SWMM_Engine engine);

/* =========================================================================
 * OPTIONS access
 * ========================================================================= */

/**
 * @brief Format a supported standard option in input-compatible string form.
 *
 * Enums use canonical input tokens, Boolean/date/numeric options use their accepted textual forms,
 * and units follow the named input option. The formatted value is intended for swmm_options_set()
 * round trips. Unknown keys are refused; buffer truncation still returns success.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] key Non-null standard option name, matched case-insensitively.
 * @param[out] buf Non-null caller-owned buffer; success writes NUL-terminated text, silently
 *     truncated to buflen-1 bytes.
 * @param[in] buflen Positive buffer capacity in bytes, including the NUL terminator.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_options_get(SWMM_Engine engine, const char* key, char* buf, int buflen);

/**
 * @brief Parse and assign a supported standard simulation option.
 *
 * Unknown keys, malformed numeric conversions and option-specific validation failures return
 * BADPARAM. This is not a blanket BUILDING-only interface: thermal ownership guards apply to
 * selected keys and individual options have their own validation. Set run configuration before
 * initialization; the call does not automatically rebuild solver state.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] key Non-null case-insensitive standard option key.
 * @param[in] value Non-null NUL-terminated input-format value. Units/enum tokens are those of the
 *     corresponding SWMM [OPTIONS] key.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. SWMM_ERR_LIFECYCLE can refuse thermal-physics changes outside editable
 *     states.
 */
SWMM_ENGINE_API int swmm_options_set(SWMM_Engine engine, const char* key, const char* value);

/**
 * @brief Read a live 2D option or stored extension string.
 *
 * 2D keys read the live solver options, with applicable AUTO values resolved against authored
 * infiltration/aquifer data. Other keys must exist in the generic extension map. Unknown keys are
 * refused and short buffers are silently truncated.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] key Non-null extension key. Recognized 2D/MESH_FILE keys use their option parser
 *     matching; generic ext_options lookup uses the exact stored key.
 * @param[out] buf Non-null caller-owned buffer; success writes NUL-terminated text, silently
 *     truncated to buflen-1 bytes.
 * @param[in] buflen Positive buffer capacity in bytes, including the NUL terminator.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_options_get_ext(SWMM_Engine engine, const char* key, char* buf,
                                         int buflen);

/**
 * @brief Set a recognized 2D option or retain a generic extension key/value.
 *
 * Recognized 2D options parse into a copy and publish only on success; retired 2D keys are refused.
 * INFIL_STEP and GW_ET update their live authoring stores. WRITE_ABSOLUTE_PATHS updates the
 * standard Boolean field. Unrecognized keys are copied verbatim to ext_options without semantic
 * validation.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] key Non-null NUL-terminated extension name. Recognized built-in keys use
 *     case-insensitive dispatch; generic keys retain exact spelling.
 * @param[in] value Non-null NUL-terminated value, copied or parsed using the selected input
 *     grammar; empty MESH_FILE clears the external mesh reference.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. SWMM_ERR_LIFECYCLE refuses guarded thermal/mesh option edits outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_options_set_ext(SWMM_Engine engine, const char* key, const char* value);

/**
 * @brief Number of standard option keys accepted by swmm_options_get() and swmm_options_set().
 *
 * Keys are canonical spellings (aliases such as ROUTING_MODEL are omitted) and include the
 * [REPORT] keys under their RPT_ prefix. The list is fixed for a build and needs no engine.
 *
 * @return Key count, always positive.
 */
SWMM_ENGINE_API int swmm_options_key_count(void);

/**
 * @brief Copy one standard option key by position.
 *
 * @param[in] index Zero-based position, less than swmm_options_key_count().
 * @param[out] buf Non-null caller-owned buffer; success writes NUL-terminated text, silently
 *     truncated to buflen-1 bytes.
 * @param[in] buflen Positive buffer capacity in bytes, including the NUL terminator.
 * @return SWMM_OK on success; SWMM_ERR_BADPARAM for an out-of-range index or invalid buffer.
 */
SWMM_ENGINE_API int swmm_options_key_at(int index, char* buf, int buflen);

/**
 * @brief Number of keys swmm_options_get_ext() currently accepts on this engine.
 *
 * With a 2D surface configured these are MESH_FILE and every current [2D_OPTIONS] key; they are
 * followed by the generic extension keys stored on the model, in byte order. Generic keys can be
 * added by swmm_options_set_ext(), so re-count after edits.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] count Non-null destination, written only on success.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null
 *     count.
 */
SWMM_ENGINE_API int swmm_options_ext_key_count(SWMM_Engine engine, int* count);

/**
 * @brief Copy one key accepted by swmm_options_get_ext() by position.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] index Zero-based position, less than the swmm_options_ext_key_count() result.
 * @param[out] buf Non-null caller-owned buffer; success writes NUL-terminated text, silently
 *     truncated to buflen-1 bytes.
 * @param[in] buflen Positive buffer capacity in bytes, including the NUL terminator.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for an
 *     out-of-range index or invalid buffer.
 */
SWMM_ENGINE_API int swmm_options_ext_key_at(SWMM_Engine engine, int index, char* buf,
                                            int buflen);

/**
 * @brief Copy the configured coordinate-reference-system string.
 *
 * Prefers the [OPTIONS] CRS and falls back to the spatial frame CRS (used by some non-INP input
 * plugins). Does not validate or transform coordinates; missing CRS leaves the buffer unchanged.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] buf Non-null caller-owned buffer; success writes NUL-terminated text, silently
 *     truncated to buflen-1 bytes.
 * @param[in] buflen Positive buffer capacity in bytes, including the NUL terminator.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. SWMM_ERR_CRS indicates no CRS is configured.
 */
SWMM_ENGINE_API int swmm_get_crs(SWMM_Engine engine, char* buf, int buflen);

/* =========================================================================
 * Typed time-control accessors
 *
 * Date values are SWMM OADate doubles: the integer part is days since
 * 1899-12-30 and the fractional part is the time-of-day fraction.
 * ========================================================================= */

/**
 * @brief Read the configured simulation start calendar value.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] value Non-null destination for OADate/SWMM DateTime decimal days since 1899-12-30; no
 *     timezone conversion.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_options_get_start_date(SWMM_Engine engine, double* value);

/**
 * @brief Assign the simulation start calendar value directly.
 *
 * No lifecycle, finite-value or chronological validation is applied here. Invalidates the cached
 * total duration for later recomputation.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] value OADate/SWMM DateTime decimal days since 1899-12-30; caller must provide a
 *     finite, valid calendar value and consistent date ordering.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_options_set_start_date(SWMM_Engine engine, double value);

/**
 * @brief Read the configured simulation end calendar value.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] value Non-null destination for OADate/SWMM DateTime decimal days since 1899-12-30; no
 *     timezone conversion.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_options_get_end_date(SWMM_Engine engine, double* value);

/**
 * @brief Assign the simulation end calendar value directly.
 *
 * No lifecycle, finite-value or chronological validation is applied here. Invalidates the cached
 * total duration for later recomputation.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] value OADate/SWMM DateTime decimal days since 1899-12-30; caller must provide a
 *     finite, valid calendar value and consistent date ordering.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_options_set_end_date(SWMM_Engine engine, double value);

/**
 * @brief Read the configured report start calendar value.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] value Non-null destination for OADate/SWMM DateTime decimal days since 1899-12-30; no
 *     timezone conversion.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_options_get_report_start(SWMM_Engine engine, double* value);

/**
 * @brief Assign the report start calendar value directly.
 *
 * No lifecycle, finite-value or chronological validation is applied here. Does not change
 * simulation start/end or rebuild report scheduling.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] value OADate/SWMM DateTime decimal days since 1899-12-30; caller must provide a
 *     finite, valid calendar value and consistent date ordering.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_options_set_report_start(SWMM_Engine engine, double value);

/* =========================================================================
 * User flags
 *
 * Flag names are case-insensitive: they are stored uppercase, matching the
 * [USER_FLAGS] INP handler.
 * ========================================================================= */

/**
 * @brief Read a model-level BOOLEAN flag assignment.
 *
 * Reads the MODEL category with an empty object key; it does not return a per-node or per-link
 * assignment. Existing assignments must be compatible with their current schema type.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] name Non-null NUL-terminated defined flag name, matched case-insensitively; schema
 *     must have type BOOLEAN.
 * @param[out] value Non-null destination for the MODEL/empty-key assignment; 0 (false) when defined
 *     but unset.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. Missing definitions or mismatched schema types return SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_userflag_get_bool(SWMM_Engine engine, const char* name, int* value);

/**
 * @brief Read a model-level INTEGER flag assignment.
 *
 * Reads the MODEL category with an empty object key; it does not return a per-node or per-link
 * assignment. Existing assignments must be compatible with their current schema type.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] name Non-null NUL-terminated defined flag name, matched case-insensitively; schema
 *     must have type INTEGER.
 * @param[out] value Non-null destination for the MODEL/empty-key assignment; 0 when defined but
 *     unset.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. Missing definitions or mismatched schema types return SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_userflag_get_int(SWMM_Engine engine, const char* name, int* value);

/**
 * @brief Read a model-level REAL flag assignment.
 *
 * Reads the MODEL category with an empty object key; it does not return a per-node or per-link
 * assignment. Existing assignments must be compatible with their current schema type.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] name Non-null NUL-terminated defined flag name, matched case-insensitively; schema
 *     must have type REAL.
 * @param[out] value Non-null destination for the MODEL/empty-key assignment; 0.0 when defined but
 *     unset.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. Missing definitions or mismatched schema types return SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_userflag_get_real(SWMM_Engine engine, const char* name, double* value);

/**
 * @brief Define a BOOLEAN flag and assign its model-level value.
 *
 * Defines/redefines this name as BOOLEAN with an empty description, then sets its MODEL/empty-key
 * value. Other existing object assignments are retained without conversion. No lifecycle guard is
 * applied.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] name Non-null NUL-terminated name, copied in uppercase.
 * @param[in] value 0 means false; any nonzero value means true.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_userflag_set_bool(SWMM_Engine engine, const char* name, int value);

/**
 * @brief Define a INTEGER flag and assign its model-level value.
 *
 * Defines/redefines this name as INTEGER with an empty description, then sets its MODEL/empty-key
 * value. Other existing object assignments are retained without conversion. No lifecycle guard is
 * applied.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] name Non-null NUL-terminated name, copied in uppercase.
 * @param[in] value INTEGER value copied without additional range validation.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_userflag_set_int(SWMM_Engine engine, const char* name, int value);

/**
 * @brief Define a REAL flag and assign its model-level value.
 *
 * Defines/redefines this name as REAL with an empty description, then sets its MODEL/empty-key
 * value. Other existing object assignments are retained without conversion. No lifecycle guard is
 * applied.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] name Non-null NUL-terminated name, copied in uppercase.
 * @param[in] value REAL value copied without additional range validation.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_userflag_set_real(SWMM_Engine engine, const char* name, double value);

/* -------------------------------------------------------------------------
 * User flag schema definitions ([USER_FLAGS]) and per-object values
 * ([USER_FLAG_VALUES]).
 *
 * Flag types (matching openswmm::UserFlagType):
 *   0 = BOOLEAN, 1 = INTEGER, 2 = REAL, 3 = STRING.
 *
 * Per-object values use string form symmetric with the INP encoding:
 * BOOLEAN as YES/NO, INTEGER as %d, REAL as %g, STRING verbatim (no quotes).
 * Object types and flag names are case-insensitive (stored uppercase);
 * object names are case-preserved.
 * ------------------------------------------------------------------------- */

/**
 * @brief Count user-flag schema definitions.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[out] count Non-null destination for the number of definitions.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_userflag_def_count(SWMM_Engine engine, int* count);

/**
 * @brief Read a flag definition by insertion-order index.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] index Zero-based schema-definition index.
 * @param[out] name_buf Optional caller-owned uppercase-name buffer; NUL-terminated and silently
 *     truncated when capacity is positive.
 * @param[in] name_buflen Name buffer capacity including NUL; nonpositive suppresses name output.
 * @param[out] type Optional type destination: 0=BOOLEAN, 1=INTEGER, 2=REAL, 3=STRING.
 * @param[out] desc_buf Optional caller-owned description buffer; empty when absent,
 *     NUL-terminated/truncated when capacity is positive.
 * @param[in] desc_buflen Description buffer capacity including NUL; nonpositive suppresses
 *     description output.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for null engine, or SWMM_ERR_BADINDEX for invalid index.
 */
SWMM_ENGINE_API int swmm_userflag_def_get(SWMM_Engine engine, int index, char* name_buf,
                                          int name_buflen, int* type, char* desc_buf,
                                          int desc_buflen);

/**
 * @brief Define or replace a flag schema without converting assigned values.
 *
 * Redefinition replaces the schema/type/description but retains previous per-object values without
 * converting them. Callers changing type must reconcile existing assignments before using typed
 * getters.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] name Non-null, nonempty NUL-terminated name, copied in uppercase.
 * @param[in] type 0=BOOLEAN, 1=INTEGER, 2=REAL or 3=STRING.
 * @param[in] description Optional copied NUL-terminated description; NULL means empty.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_userflag_define(SWMM_Engine engine, const char* name, int type,
                                         const char* description);

/**
 * @brief Remove a schema and all assignments of that flag.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] name Non-null NUL-terminated flag name, matched case-insensitively.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. An undefined name returns SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_userflag_undefine(SWMM_Engine engine, const char* name);

/**
 * @brief Format a stored per-object flag assignment as text.
 *
 * BOOLEAN uses YES/NO, INTEGER decimal, REAL %g, and STRING verbatim without quoting. Looks up the
 * supplied object key without validating object existence. Short buffers truncate silently while
 * found remains 1.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] obj_type Non-null NUL-terminated category token, uppercased internally (for example
 *     NODE, LINK, SUBCATCHMENT or MODEL).
 * @param[in] obj_name Non-null NUL-terminated object key, preserved exactly including case; MODEL
 *     values conventionally use an empty key.
 * @param[in] flag_name Non-null NUL-terminated flag name, matched case-insensitively.
 * @param[out] buf Non-null caller-owned buffer; success writes NUL-terminated text, silently
 *     truncated to buflen-1 bytes.
 * @param[in] buflen Positive buffer capacity in bytes, including the NUL terminator.
 * @param[out] found Non-null destination: 1 for a stored assignment, 0 when absent (including
 *     undefined flags). Missing values also write an empty buffer.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_userflag_value_get(SWMM_Engine engine, const char* obj_type,
                                            const char* obj_name, const char* flag_name, char* buf,
                                            int buflen, int* found);

/**
 * @brief Parse a copied assignment using an existing flag schema.
 *
 * Object category and flag name are uppercased; object name is case-preserved. The API does not
 * validate that the object exists. Boolean token handling is that of Tokenizer::parse_boolean(),
 * rather than a separate strict validator.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] obj_type Non-null NUL-terminated category token, uppercased internally (for example
 *     NODE, LINK, SUBCATCHMENT or MODEL).
 * @param[in] obj_name Non-null NUL-terminated object key, preserved exactly including case; MODEL
 *     values conventionally use an empty key.
 * @param[in] flag_name Non-null NUL-terminated flag name, matched case-insensitively.
 * @param[in] value Non-null NUL-terminated value: BOOLEAN uses the input Boolean tokenizer; INTEGER
 *     requires a complete decimal integer; REAL uses the engine numeric parser; STRING is stored
 *     verbatim.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments. Undefined flags or rejected integer/real parsing return
 *     SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_userflag_value_set(SWMM_Engine engine, const char* obj_type,
                                            const char* obj_name, const char* flag_name,
                                            const char* value);

/**
 * @brief Remove one per-object assignment, leaving its schema intact.
 *
 * Idempotent for missing assignments; does not validate object existence or require an existing
 * schema.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access.
 * @param[in] obj_type Non-null NUL-terminated category token, uppercased internally (for example
 *     NODE, LINK, SUBCATCHMENT or MODEL).
 * @param[in] obj_name Non-null NUL-terminated object key, preserved exactly including case; MODEL
 *     values conventionally use an empty key.
 * @param[in] flag_name Non-null NUL-terminated flag name, matched case-insensitively.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     required arguments.
 */
SWMM_ENGINE_API int swmm_userflag_value_clear(SWMM_Engine engine, const char* obj_type,
                                              const char* obj_name, const char* flag_name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_MODEL_H */
