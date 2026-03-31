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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
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
 * @param key     Option name (e.g., "FLOW_UNITS", "ROUTING_MODEL", "CRS").
 * @param buf     Caller-allocated buffer for the value string.
 * @param buflen  Size of buf in bytes.
 * @returns SWMM_OK if key found; SWMM_ERR_BADPARAM if not a standard key.
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
 * User flags
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_MODEL_H */
