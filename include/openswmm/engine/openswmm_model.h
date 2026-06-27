/**
 * @file openswmm_model.h
 * @brief OpenSWMM Engine — Model building and options C API.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
 * @brief Create an empty engine in BUILDING state (no .inp file required).
 *
 * @details Use this instead of swmm_engine_create() + swmm_engine_open() when
 *          building a model entirely through the API. Objects may be added via
 *          swmm_node_add(), swmm_link_add(), etc. while in BUILDING state.
 *          Call swmm_finalize_model() to transition to INITIALIZED.
 *
 * @returns Opaque engine handle in SWMM_STATE_BUILDING, or NULL on failure.
 */
SWMM_ENGINE_API SWMM_Engine swmm_engine_new(void);

/* =========================================================================
 * Model finalisation and validation
 * ========================================================================= */

/**
 * @brief Validate model topology without changing state.
 *
 * @details Checks connectivity (no orphaned links, at least one outfall, no
 *          duplicate IDs). Emits warnings via the registered warning callback.
 *          Does NOT change state — safe to call multiple times.
 *
 * @param engine  Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @returns SWMM_OK if validation passes; SWMM_ERR_* on fatal topology error.
 */
SWMM_ENGINE_API int swmm_validate_model(SWMM_Engine engine);

/**
 * @brief Finalise a programmatically-built model.
 *
 * @details Runs full topology validation, builds CSR connectivity arrays,
 *          allocates all SoA state arrays, and transitions the engine to
 *          SWMM_STATE_INITIALIZED. Equivalent to swmm_engine_open() +
 *          swmm_engine_initialize() for file-based models.
 *
 * @param engine  Engine handle (must be in SWMM_STATE_BUILDING).
 * @returns SWMM_OK on success; SWMM_ERR_* on failure.
 */
SWMM_ENGINE_API int swmm_finalize_model(SWMM_Engine engine);

/* =========================================================================
 * Model serialisation
 * ========================================================================= */

/**
 * @brief Write the current model state to a SWMM input (.inp) file.
 *
 * @details Serializes the entire SimulationContext back to SWMM .inp format.
 *          The output is a valid SWMM input file that can be re-opened.
 *          Includes all modified objects, user flags, CRS, plugin specs,
 *          and extension options.
 *
 * @param engine       Engine handle (SWMM_STATE_OPENED or later).
 * @param new_inp_path Path where the new .inp file should be written.
 * @returns SWMM_OK on success; SWMM_ERR_* on failure.
 */
SWMM_ENGINE_API int swmm_model_write(SWMM_Engine engine, const char* new_inp_path);

/**
 * @brief Write the current model state via a named writer plugin.
 *
 * @details Routes the in-memory `SimulationContext` through the
 *          `IInputPlugin::write()` method of the plugin matching
 *          @p output_plugin_id.  Lets a host (e.g., the GUI's Save As
 *          dialog) pick a non-`.inp` container — GeoPackage, HDF5,
 *          GeoJSON, …— at write time without touching the rest of the
 *          engine API.
 *
 *          Pass NULL or an empty string for @p output_plugin_id to use
 *          the built-in `.inp` writer (equivalent to swmm_model_write).
 *
 *          @p output_plugin_id is resolved via the engine's
 *          PluginFactory::find_component() — accepts a plugin id
 *          (e.g., "org.hydrocouple.openswmm.plugins.geopackage"), an
 *          `id:version` pair, or a shared-library path.  The resolved
 *          plugin must advertise input capability
 *          (`IPluginComponentInfo::has_input()` returns true), since
 *          model write is delegated through `IInputPlugin::write()`.
 *
 *          The chosen plugin is instantiated transiently for the call:
 *          create → initialize(args=empty) → write → finalize → delete.
 *          The engine's primary input plugin is not disturbed so the
 *          model can be re-saved later with a different writer.
 *
 * @param engine            Engine handle (SWMM_STATE_OPENED or later).
 * @param new_path          Path where the new model file should be written.
 * @param output_plugin_id  Writer plugin id, version pair, or library
 *                          path; or NULL/empty for default `.inp` writer.
 * @returns SWMM_OK on success;
 *          SWMM_ERR_BADPARAM when @p new_path is NULL or the plugin id
 *          does not resolve;
 *          SWMM_ERR_PLUGIN when the resolved plugin has no input
 *          capability or its initialize/write/finalize fails.
 *
 * @see swmm_model_write — built-in `.inp` writer (this function with
 *      NULL plugin id is functionally equivalent).
 */
SWMM_ENGINE_API int
swmm_model_write_with_plugin(SWMM_Engine engine,
                              const char* new_path,
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
 * @brief Number of [PLUGINS] entries currently registered on the engine.
 * @param count [out] Number of entries.  Always set on SWMM_OK return.
 */
SWMM_ENGINE_API int swmm_plugins_count(SWMM_Engine engine, int* count);

/**
 * @brief Read one [PLUGINS] row by index.
 *
 * @details Either output buffer may be NULL (caller doesn't want that
 *          field).  When non-NULL, the buffer is NUL-terminated and
 *          truncated at @p path_buf_sz - 1 / @p args_buf_sz - 1 if too
 *          small (no error returned).  The args string is reconstructed
 *          by joining the row's `init_args` vector with single spaces.
 *
 * @param engine        Engine handle.
 * @param idx           Index in [0, count).
 * @param path_buf      [out] UTF-8 plugin path / id; may be NULL.
 * @param path_buf_sz   Size of path_buf in bytes (incl. NUL).
 * @param args_buf      [out] UTF-8 space-joined args; may be NULL.
 * @param args_buf_sz   Size of args_buf in bytes (incl. NUL).
 * @returns SWMM_OK on success; SWMM_ERR_BADINDEX when idx is out of range.
 */
SWMM_ENGINE_API int
swmm_plugin_get(SWMM_Engine engine,
                int          idx,
                char*        path_buf,
                int          path_buf_sz,
                char*        args_buf,
                int          args_buf_sz);

/**
 * @brief Add or replace a [PLUGINS] row keyed by @p path_or_id.
 *
 * @details If a row with the same @p path_or_id exists, its args are
 *          replaced.  Otherwise a new row is appended.  The engine
 *          re-serialises the [PLUGINS] section on the next
 *          swmm_model_write* call.
 *
 * @param engine        Engine handle.
 * @param path_or_id    Library path, plugin id, or `id:version` string.
 * @param args          Space-separated argument tokens; may be NULL or
 *                      empty for "no arguments".
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM when @p path_or_id is
 *          NULL or empty.
 */
SWMM_ENGINE_API int
swmm_plugin_set(SWMM_Engine engine,
                const char*  path_or_id,
                const char*  args);

/**
 * @brief Remove the [PLUGINS] row matching @p path_or_id.
 *
 * @details Idempotent: returns SWMM_OK even when no row matches.
 *
 * @param engine        Engine handle.
 * @param path_or_id    Library path, plugin id, or `id:version` string.
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM when @p path_or_id is NULL.
 */
SWMM_ENGINE_API int
swmm_plugin_remove(SWMM_Engine engine, const char* path_or_id);

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
 * @brief Read one [FILES] field by key.
 *
 * @param engine  Engine handle.
 * @param key     Field key (see header comment for recognised values).
 * @param buf     [out] UTF-8 result; NUL-terminated and truncated at
 *                @p buflen - 1 if too small.
 * @param buflen  Size of @p buf in bytes (including NUL).
 * @returns SWMM_OK on success;
 *          SWMM_ERR_BADPARAM when @p key is NULL, unknown, or buf args
 *          are invalid.
 */
SWMM_ENGINE_API int
swmm_files_get(SWMM_Engine engine, const char* key, char* buf, int buflen);

/**
 * @brief Write one [FILES] field by key.
 *
 * @details Pass an empty @p value to clear a path slot.  Pass an empty
 *          @p value to a `*_MODE` key to clear the mode (slot becomes
 *          inactive).  The engine re-emits `[FILES]` on the next
 *          `swmm_model_write*` call.
 *
 * @returns SWMM_OK on success;
 *          SWMM_ERR_BADPARAM when @p key is NULL, unknown, or @p value
 *          is NULL.
 */
SWMM_ENGINE_API int
swmm_files_set(SWMM_Engine engine, const char* key, const char* value);

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
    SWMM_FILE_RAINFALL          = 1,  /**< ctx.files.rainfall_path        */
    SWMM_FILE_RUNOFF            = 2,  /**< ctx.files.runoff_path          */
    SWMM_FILE_RDII              = 3,  /**< ctx.files.rdii_path            */
    SWMM_FILE_INFLOWS           = 4,  /**< ctx.files.inflows_path         */
    SWMM_FILE_OUTFLOWS          = 5,  /**< ctx.files.outflows_path        */
    SWMM_FILE_HOTSTART_USE      = 6,  /**< ctx.files.hotstart_use_path    */
    SWMM_FILE_CLIMATE_TEMP      = 7,  /**< ctx.options.temp_file          */

    /* Vector slots — `owner` selects the entry. */
    SWMM_FILE_HOTSTART_SAVE     = 8,  /**< ctx.files.hotstart_saves[i],
                                       *  `owner` is decimal index "0".."N-1" */
    SWMM_FILE_RAINGAGE_DATA     = 9,  /**< ctx.gages.file_path[i],
                                       *  `owner` is the gage id     */
    SWMM_FILE_TIMESERIES_DATA   = 10  /**< ctx.tables.tables[i].file_path,
                                       *  `owner` is the series id   */
} SWMM_FilePathRole;

/**
 * @brief Read both strings from an external-file slot.
 *
 * @param engine        Engine handle.
 * @param role          Which slot to read.
 * @param owner         Owner key for vector slots; NULL/"" for scalar slots.
 * @param absolute_buf  [out] UTF-8 absolute path (NUL-terminated, truncated
 *                      at `absolute_buflen - 1`). May be empty if the slot
 *                      was set programmatically and never resolved.
 * @param absolute_buflen  Size of `absolute_buf` in bytes.
 * @param original_buf  [out] UTF-8 original token as authored. May be empty.
 * @param original_buflen  Size of `original_buf` in bytes.
 * @returns SWMM_OK on success;
 *          SWMM_ERR_BADPARAM when `role` is unknown, when `owner` is
 *          required but NULL/missing in the model, or when buffer
 *          arguments are NULL.
 */
SWMM_ENGINE_API int
swmm_file_path_get(SWMM_Engine          engine,
                   SWMM_FilePathRole    role,
                   const char*          owner,
                   char*                absolute_buf,
                   int                  absolute_buflen,
                   char*                original_buf,
                   int                  original_buflen);

/**
 * @brief Set the original token for an external-file slot; clears the
 *        cached absolute resolution.
 *
 * @details For vector slots, an `owner` that is not yet present in the
 *          model results in `SWMM_ERR_BADPARAM` — callers must add the
 *          owning gage / timeseries / hot-start save row first.
 *
 *          Pass an empty `new_path` to clear the slot.
 *
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM otherwise.
 */
SWMM_ENGINE_API int
swmm_file_path_set(SWMM_Engine          engine,
                   SWMM_FilePathRole    role,
                   const char*          owner,
                   const char*          new_path);

/* =========================================================================
 * Title / notes access
 * ========================================================================= */

/**
 * @brief Get the number of title/note lines in the [TITLE] section.
 *
 * @param engine  Engine handle.
 * @param count   [out] Number of title lines.
 * @returns SWMM_OK on success.
 */
SWMM_ENGINE_API int swmm_title_get_count(SWMM_Engine engine, int* count);

/**
 * @brief Get a specific title/note line by index.
 *
 * @param engine  Engine handle.
 * @param index   Zero-based line index.
 * @param buf     Caller-allocated buffer for the line text.
 * @param buflen  Size of buf in bytes.
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM if index out of range.
 */
SWMM_ENGINE_API int swmm_title_get_line(
    SWMM_Engine engine,
    int         index,
    char*       buf,
    int         buflen
);

/**
 * @brief Add a new line to the end of the [TITLE] section.
 *
 * @param engine  Engine handle.
 * @param line    Null-terminated string to append.
 * @returns SWMM_OK on success.
 */
SWMM_ENGINE_API int swmm_title_add_line(SWMM_Engine engine, const char* line);

/**
 * @brief Replace all title/note lines with a single block of text.
 *
 * @details The text is split on newline characters ('\\n') to form
 *          individual title lines. Any existing title lines are cleared.
 *
 * @param engine  Engine handle.
 * @param text    Null-terminated text (may contain '\\n' separators).
 * @returns SWMM_OK on success.
 */
SWMM_ENGINE_API int swmm_title_set(SWMM_Engine engine, const char* text);

/**
 * @brief Remove all lines from the [TITLE] section.
 *
 * @param engine  Engine handle.
 * @returns SWMM_OK on success.
 */
SWMM_ENGINE_API int swmm_title_clear(SWMM_Engine engine);

/* =========================================================================
 * OPTIONS access
 * ========================================================================= */

/**
 * @brief Retrieve a standard OPTIONS value as a string.
 *
 * @param engine  Engine handle.
 * @param key     Option name (e.g., "FLOW_UNITS", "ROUTING_MODEL",
 *                "LINK_OFFSETS", "CRS").
 * @param buf     Caller-allocated buffer for the value string.
 * @param buflen  Size of buf in bytes.
 * @returns SWMM_OK if key found; SWMM_ERR_BADPARAM if not a standard key.
 *
 * @details Encoding is symmetric with swmm_options_set: the returned string
 *          is one that swmm_options_set accepts for the same key. Enum keys
 *          return their canonical token (e.g. "CMS", "DYNWAVE", "ELEVATION"),
 *          never the underlying int.
 */
SWMM_ENGINE_API int swmm_options_get(
    SWMM_Engine engine,
    const char* key,
    char*       buf,
    int         buflen
);

/**
 * @brief Set a standard OPTIONS value.
 *
 * @param engine  Engine handle (valid before swmm_engine_start()).
 * @param key     Option name.
 * @param value   New value string (parsed by the engine).
 * @returns SWMM_OK or SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_options_set(
    SWMM_Engine engine,
    const char* key,
    const char* value
);

/**
 * @brief Retrieve an extension OPTIONS value (keys unknown to standard SWMM).
 *
 * @param engine  Engine handle.
 * @param key     Extension option key.
 * @param buf     Caller-allocated buffer.
 * @param buflen  Buffer size.
 * @returns SWMM_OK or SWMM_ERR_BADPARAM if key not found.
 */
SWMM_ENGINE_API int swmm_options_get_ext(
    SWMM_Engine engine,
    const char* key,
    char*       buf,
    int         buflen
);

/**
 * @brief Set (or create) an extension OPTIONS value.
 *
 * @param engine  Engine handle.
 * @param key     Extension option key.
 * @param value   New value string.
 * @returns SWMM_OK or error code.
 */
SWMM_ENGINE_API int swmm_options_set_ext(
    SWMM_Engine engine,
    const char* key,
    const char* value
);

/**
 * @brief Retrieve the CRS string (e.g., "EPSG:4326" or PROJ string).
 *
 * @param engine  Engine handle.
 * @param buf     Caller-allocated buffer.
 * @param buflen  Buffer size.
 * @returns SWMM_OK; SWMM_ERR_CRS if no CRS was specified in [OPTIONS].
 */
SWMM_ENGINE_API int swmm_get_crs(SWMM_Engine engine, char* buf, int buflen);

/* =========================================================================
 * Typed time-control accessors
 *
 * Date values are SWMM OADate doubles: the integer part is days since
 * 1899-12-30 and the fractional part is the time-of-day fraction.
 * ========================================================================= */

/**
 * @brief Retrieve the simulation start date/time as an OADate.
 * @param engine  Engine handle.
 * @param value   [out] OADate (decimal days since 1899-12-30).
 * @returns SWMM_OK; SWMM_ERR_BADPARAM on null engine/value.
 */
SWMM_ENGINE_API int swmm_options_get_start_date(SWMM_Engine engine, double* value);

/**
 * @brief Set the simulation start date/time from an OADate.
 * @param engine  Engine handle.
 * @param value   OADate (decimal days since 1899-12-30).
 * @returns SWMM_OK; SWMM_ERR_BADPARAM on null engine.
 */
SWMM_ENGINE_API int swmm_options_set_start_date(SWMM_Engine engine, double value);

/**
 * @brief Retrieve the simulation end date/time as an OADate.
 * @param engine  Engine handle.
 * @param value   [out] OADate (decimal days since 1899-12-30).
 * @returns SWMM_OK; SWMM_ERR_BADPARAM on null engine/value.
 */
SWMM_ENGINE_API int swmm_options_get_end_date(SWMM_Engine engine, double* value);

/**
 * @brief Set the simulation end date/time from an OADate.
 * @param engine  Engine handle.
 * @param value   OADate (decimal days since 1899-12-30).
 * @returns SWMM_OK; SWMM_ERR_BADPARAM on null engine.
 */
SWMM_ENGINE_API int swmm_options_set_end_date(SWMM_Engine engine, double value);

/**
 * @brief Retrieve the report start date/time as an OADate.
 * @param engine  Engine handle.
 * @param value   [out] OADate (decimal days since 1899-12-30).
 * @returns SWMM_OK; SWMM_ERR_BADPARAM on null engine/value.
 */
SWMM_ENGINE_API int swmm_options_get_report_start(SWMM_Engine engine, double* value);

/**
 * @brief Set the report start date/time from an OADate.
 * @param engine  Engine handle.
 * @param value   OADate (decimal days since 1899-12-30).
 * @returns SWMM_OK; SWMM_ERR_BADPARAM on null engine.
 */
SWMM_ENGINE_API int swmm_options_set_report_start(SWMM_Engine engine, double value);

/* =========================================================================
 * User flags
 *
 * Flag names are case-insensitive: they are stored uppercase, matching the
 * [USER_FLAGS] INP handler.
 * ========================================================================= */

/**
 * @brief Get the value of a BOOLEAN user flag (schema-level).
 * @param engine  Engine handle.
 * @param name    Flag name (as defined in [USER_FLAGS]).
 * @param value   [out] 1 = YES/TRUE, 0 = NO/FALSE.
 * @returns SWMM_OK; SWMM_ERR_BADPARAM if flag not found or wrong type.
 */
SWMM_ENGINE_API int swmm_userflag_get_bool(SWMM_Engine engine, const char* name, int*    value);

/**
 * @brief Get the value of an INTEGER user flag.
 * @param engine  Engine handle.
 * @param name    Flag name.
 * @param value   [out] Integer value.
 * @returns SWMM_OK; SWMM_ERR_BADPARAM if not found or wrong type.
 */
SWMM_ENGINE_API int swmm_userflag_get_int (SWMM_Engine engine, const char* name, int*    value);

/**
 * @brief Get the value of a REAL user flag.
 * @param engine  Engine handle.
 * @param name    Flag name.
 * @param value   [out] Double value.
 * @returns SWMM_OK; SWMM_ERR_BADPARAM if not found or wrong type.
 */
SWMM_ENGINE_API int swmm_userflag_get_real(SWMM_Engine engine, const char* name, double* value);

/** @brief Set a BOOLEAN user flag at runtime. */
SWMM_ENGINE_API int swmm_userflag_set_bool(SWMM_Engine engine, const char* name, int    value);

/** @brief Set an INTEGER user flag at runtime. */
SWMM_ENGINE_API int swmm_userflag_set_int (SWMM_Engine engine, const char* name, int    value);

/** @brief Set a REAL user flag at runtime. */
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
 * @brief Number of user-flag schema definitions.
 * @param engine  Engine handle.
 * @param count   [out] Definition count.
 * @returns SWMM_OK; SWMM_ERR_BADPARAM on null count.
 */
SWMM_ENGINE_API int swmm_userflag_def_count(SWMM_Engine engine, int* count);

/**
 * @brief Retrieve a user-flag schema definition by index (insertion order).
 * @param engine       Engine handle.
 * @param index        Zero-based definition index.
 * @param name_buf     [out] Flag name (NUL-terminated, truncated to fit). May be NULL.
 * @param name_buflen  Size of name_buf in bytes.
 * @param type         [out] Flag type (0=BOOLEAN, 1=INTEGER, 2=REAL, 3=STRING). May be NULL.
 * @param desc_buf     [out] Description (empty string if none). May be NULL.
 * @param desc_buflen  Size of desc_buf in bytes.
 * @returns SWMM_OK; SWMM_ERR_BADINDEX if index out of range.
 */
SWMM_ENGINE_API int swmm_userflag_def_get(
    SWMM_Engine engine,
    int         index,
    char*       name_buf,
    int         name_buflen,
    int*        type,
    char*       desc_buf,
    int         desc_buflen
);

/**
 * @brief Define (or redefine) a user flag.
 * @param engine       Engine handle.
 * @param name         Flag name (stored uppercase).
 * @param type         Flag type (0=BOOLEAN, 1=INTEGER, 2=REAL, 3=STRING).
 * @param description  Optional description (may be NULL or empty).
 * @returns SWMM_OK; SWMM_ERR_BADPARAM on null/empty name or invalid type.
 * @details Redefining an existing name overwrites its definition; previously
 *          assigned per-object values are kept as-is.
 */
SWMM_ENGINE_API int swmm_userflag_define(
    SWMM_Engine engine,
    const char* name,
    int         type,
    const char* description
);

/**
 * @brief Remove a user-flag definition and all per-object values assigned to it.
 * @param engine  Engine handle.
 * @param name    Flag name (case-insensitive).
 * @returns SWMM_OK; SWMM_ERR_BADPARAM if the flag is not defined.
 */
SWMM_ENGINE_API int swmm_userflag_undefine(SWMM_Engine engine, const char* name);

/**
 * @brief Read the flag value assigned to a specific object, as a string.
 * @param engine     Engine handle.
 * @param obj_type   Object type token (e.g. "NODE", "LINK", "SUBCATCHMENT").
 * @param obj_name   Object identifier (case-preserved).
 * @param flag_name  Flag name (case-insensitive).
 * @param buf        [out] Value string (empty when not assigned).
 * @param buflen     Size of buf in bytes.
 * @param found      [out] 1 if a value is assigned, 0 otherwise.
 * @returns SWMM_OK; SWMM_ERR_BADPARAM on null arguments.
 */
SWMM_ENGINE_API int swmm_userflag_value_get(
    SWMM_Engine engine,
    const char* obj_type,
    const char* obj_name,
    const char* flag_name,
    char*       buf,
    int         buflen,
    int*        found
);

/**
 * @brief Assign a flag value to a specific object from a string.
 * @param engine     Engine handle.
 * @param obj_type   Object type token (e.g. "NODE", "LINK", "SUBCATCHMENT").
 * @param obj_name   Object identifier (case-preserved).
 * @param flag_name  Flag name; must already be defined (its declared type
 *                   drives parsing).
 * @param value      Value string: BOOLEAN accepts YES/NO/TRUE/FALSE/1/0;
 *                   INTEGER a decimal integer; REAL a decimal number;
 *                   STRING is stored verbatim.
 * @returns SWMM_OK; SWMM_ERR_BADPARAM on null arguments, undefined flag,
 *          or a value that does not parse as the declared type.
 */
SWMM_ENGINE_API int swmm_userflag_value_set(
    SWMM_Engine engine,
    const char* obj_type,
    const char* obj_name,
    const char* flag_name,
    const char* value
);

/**
 * @brief Remove the flag value assigned to a specific object (mark unset).
 * @param engine     Engine handle.
 * @param obj_type   Object type token.
 * @param obj_name   Object identifier (case-preserved).
 * @param flag_name  Flag name (case-insensitive).
 * @returns SWMM_OK (idempotent: clearing an unassigned value succeeds);
 *          SWMM_ERR_BADPARAM on null arguments.
 */
SWMM_ENGINE_API int swmm_userflag_value_clear(
    SWMM_Engine engine,
    const char* obj_type,
    const char* obj_name,
    const char* flag_name
);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_MODEL_H */
