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
 * @file      openswmm_engine.h
 * @brief     OpenSWMM Engine — primary transparent C API (master header).
 *
 * @details This is the main public header for the openswmm.engine library
 *          (version 6.0.0-alpha.1). It defines:
 *            - Error / warning / state enums
 *            - Engine lifecycle functions (create, open, init, start, step, end, close, destroy)
 *            - Callback registration
 *            - Simulation timing
 *            - Error reporting
 *            - The SWMM_ENGINE_API export macro
 *
 *          Domain-specific APIs are in separate headers:
 *            - openswmm_datetime.h      — SWMM DateTime encode/decode utilities
 *            - openswmm_model.h         — model building, validation, serialisation, options, user
 * flags
 *            - openswmm_nodes.h         — node creation, properties, state, forcing, bulk
 *            - openswmm_links.h         — link creation, properties, cross-sections, state, bulk
 *            - openswmm_subcatchments.h — subcatchment creation, properties, state, bulk
 *            - openswmm_gages.h         — rain gage creation, properties, rainfall
 *            - openswmm_massbalance.h   — continuity errors and flux totals
 *            - openswmm_hotstart.h      — hot start file management
 *            - openswmm_callbacks.h     — callback typedefs
 *
 *          Including this header pulls in ALL of the above (master include).
 *          To use a single domain, include just that domain header.
 *
 * @section engine_api_lifecycle Lifecycle
 *
 * @code
 * CREATED → OPENED → INITIALIZED → STARTED → [RUNNING] → ENDED → CLOSED
 * @endcode
 *
 * Programmatic model building (no .inp):
 * @code
 * [BUILDING] → INITIALIZED → STARTED → [RUNNING] → ENDED → CLOSED
 * @endcode
 *
 * Typical usage:
 * @code{.c}
 * #include <openswmm/engine/openswmm_engine.h>
 *
 * SWMM_Engine e = swmm_engine_create();
 * swmm_engine_open(e, "model.inp", "model.rpt", "model.out", NULL);
 * swmm_engine_initialize(e);
 * swmm_engine_start(e, 1);
 *
 * double elapsed = 0.0;
 * while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {
 *     // read/write node depths, link flows, etc.
 * }
 *
 * swmm_engine_end(e);
 * swmm_engine_report(e);
 * swmm_engine_close(e);
 * swmm_engine_destroy(e);
 * @endcode
 *
 * @note This API maintains a C89-compatible ABI at the boundary.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup new_engine OpenSWMM Engine
 * @brief Modern engine implementation, data model and public interfaces.
 */

/**
 * @defgroup engine_api New Engine C API
 * @ingroup  new_engine
 */

#ifndef OPENSWMM_ENGINE_H
#define OPENSWMM_ENGINE_H

#include "openswmm_callbacks.h"

/* =========================================================================
 * Export macro
 * ========================================================================= */

#ifdef OPENSWMM_ENGINE_STATIC
#define SWMM_ENGINE_API
#else
#ifdef _WIN32
#ifdef openswmm_engine_EXPORTS
#define SWMM_ENGINE_API __declspec(dllexport)
#else
#define SWMM_ENGINE_API __declspec(dllimport)
#endif
#else
#define SWMM_ENGINE_API __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Error codes
 * ========================================================================= */

typedef enum SWMM_ErrorCode {
    SWMM_OK = 0,
    SWMM_ERR_NOMEM = 1,
    SWMM_ERR_INPFILE = 2,
    SWMM_ERR_RPTFILE = 3,
    SWMM_ERR_OUTFILE = 4,
    SWMM_ERR_PARSE = 5,
    SWMM_ERR_LIFECYCLE = 6,
    SWMM_ERR_BADHANDLE = 7,
    SWMM_ERR_BADINDEX = 8,
    SWMM_ERR_BADPARAM = 9,
    SWMM_ERR_PLUGIN = 10,
    SWMM_ERR_IO = 11,
    SWMM_ERR_HOTSTART = 12,
    SWMM_ERR_CRS = 13,
    SWMM_ERR_NUMERICAL = 14,
    SWMM_ERR_DEPENDENCY = 15, /**< Object has dependents that would be affected by deletion. */
    SWMM_ERR_CANCELLED = 16,  /**< A caller cancelled a cancellable read-only operation. */
    SWMM_ERR_BUFSIZE = 17,    /**< A caller's output buffer is too small for the value; nothing
                                   was written. Retry with a larger buffer. */
    SWMM_ERR_INTERNAL = 99
} SWMM_ErrorCode;

typedef enum SWMM_WarnCode {
    SWMM_WARN_NONE = 0,
    SWMM_WARN_HOTSTART_MISSING = 1,
    SWMM_WARN_UNKNOWN_SECTION = 2,
    SWMM_WARN_UNKNOWN_OPTION = 3,
    SWMM_WARN_DEPRECATED_KW = 4,
    SWMM_WARN_PLUGIN_INIT = 5,
    SWMM_WARN_NUMERICAL = 6,
    SWMM_WARN_STABILITY_LIMIT = 7
} SWMM_WarnCode;

/* =========================================================================
 * Engine lifecycle state
 * ========================================================================= */

typedef enum SWMM_EngineState {
    SWMM_STATE_NONE = 0,
    SWMM_STATE_CREATED = 1,
    SWMM_STATE_OPENED = 2,
    SWMM_STATE_INITIALIZED = 3,
    SWMM_STATE_STARTED = 4,
    SWMM_STATE_RUNNING = 5,
    SWMM_STATE_ENDED = 6,
    SWMM_STATE_CLOSED = 7,
    SWMM_STATE_BUILDING = 8 /**< Programmatic model construction in progress */
} SWMM_EngineState;

/* =========================================================================
 * Object type identifiers
 * ========================================================================= */

typedef enum SWMM_ObjectType {
    SWMM_OBJ_GAGE = 0,
    SWMM_OBJ_SUBCATCH = 1,
    SWMM_OBJ_NODE = 2,
    SWMM_OBJ_LINK = 3,
    SWMM_OBJ_POLLUT = 4,
    SWMM_OBJ_LANDUSE = 5,
    SWMM_OBJ_TIMESER = 6,
    SWMM_OBJ_TABLE = 7,
    SWMM_OBJ_RDII = 8,
    SWMM_OBJ_UNITHYD = 9,
    SWMM_OBJ_SNOWMELT = 10,
    SWMM_OBJ_SHAPE = 11,
    SWMM_OBJ_LID = 12
} SWMM_ObjectType;

/* =========================================================================
 * Engine lifecycle functions
 * ========================================================================= */

/**
 * @brief Allocate an independent engine in SWMM_STATE_CREATED.
 * @return Caller-owned opaque engine handle, or NULL when construction fails. Release with
 *     swmm_engine_destroy(); close active resources first when explicit teardown is needed.
 */
SWMM_ENGINE_API SWMM_Engine swmm_engine_create(void);

/**
 * @brief Load a model and prepare its plugin configuration in SWMM_STATE_OPENED.
 *
 * Requires CREATED or CLOSED and resets the model context for a fresh load. Ordinary open resolves
 * and validates model references and may load configured plugins. Paths are consumed synchronously;
 * the engine keeps its own copies as needed. Query swmm_get_last_error_msg() and accumulated
 * errors/warnings after refusal. Existing report/output destinations can be written during the
 * lifecycle; use separate private paths for staged work.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] inp Borrowed NUL-terminated input-model path.
 * @param[in] rpt Borrowed report destination path, or NULL to omit it.
 * @param[in] out Borrowed binary-results destination path, or NULL to omit it.
 * @param[in] input_plugin_lib Borrowed input plugin library path, component ID or id:version;
 *     NULL/empty selects the built-in INP reader when no input plugin is already registered.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an invalid
 *     lifecycle state, or the underlying initialization/IO/plugin error.
 */
SWMM_ENGINE_API int swmm_engine_open(SWMM_Engine engine, const char* inp, const char* rpt,
                                     const char* out, const char* input_plugin_lib);

/**
 * @brief Open a model for configured metadata without preparing simulation plugins.
 *
 * Uses normal strict model validation and no report/result destinations. The returned OPENED model
 * supports metadata getters; initialize/start are refused until the engine is closed and reopened
 * normally. Model-declared SAVE paths are not opened by this metadata load.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] inp Borrowed NUL-terminated input-model path.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADPARAM for null/empty
 *     inp, or an open/parse/validation error.
 */
SWMM_ENGINE_API int swmm_engine_open_metadata(SWMM_Engine engine, const char* inp);

/**
 * @brief Allocate simulation modules and initial state, entering SWMM_STATE_INITIALIZED.
 *
 * Requires OPENED, or INITIALIZED for a pre-start rebuild. Resolves runtime physics/transport
 * guards, initializes forcing arrays and compiles stored control rules. Metadata-only handles are
 * refused. Reinitializing after a run requires a fresh open; initialization may change state to an
 * error state on failure.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an invalid
 *     lifecycle state, or the underlying initialization/IO/plugin error.
 */
SWMM_ENGINE_API int swmm_engine_initialize(SWMM_Engine engine);

/**
 * @brief Start an initialized simulation, entering SWMM_STATE_RUNNING.
 *
 * Requires INITIALIZED. Opens configured interface files and prepares run-time plugins/outputs
 * before stepping. Failure may leave partially prepared resources that must be closed; read the
 * last error before teardown.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] save_results 0 disables regular result saving; nonzero enables it through the
 *     configured output path/plugins.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an invalid
 *     lifecycle state, or the underlying initialization/IO/plugin error.
 */
SWMM_ENGINE_API int swmm_engine_start(SWMM_Engine engine, int save_results);

/**
 * @brief Advance to a specified elapsed-second exchange boundary without overshooting.
 *
 * Flushes pending 2D work before successful return. Due external driver updates must be supplied
 * first. Does not provide rollback or iterative coupling; inspect actual_seconds after errors.
 *
 * @param[in] engine Non-null RUNNING engine; cannot be invoked recursively from a step or its
 *     callbacks.
 * @param[in] target_seconds Finite absolute elapsed simulation time in seconds, at least the
 *     current coupling clock and no later than simulation end.
 * @param[out] actual_seconds Non-null accepted coupling-time output in seconds. With valid
 *     arguments, set before lifecycle/target checks and updated after advancement; may report
 *     partial progress on stepping failure.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADPARAM for invalid
 *     target/output, SWMM_ERR_LIFECYCLE for a running/driver/boundary refusal, or the first step
 *     error.
 */
SWMM_ENGINE_API int swmm_engine_advance_to(SWMM_Engine engine, double target_seconds,
                                           double* actual_seconds);
/**
 * @brief Read the routing elapsed clock, including during step callbacks.
 *
 * No lifecycle guard is applied. This getter uses the context routing clock;
 * swmm_engine_advance_to() reports the runtime coupling clock used for exchange boundaries.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] seconds Non-null destination for context elapsed_ms/1000 in seconds since simulation
 *     start.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null seconds.
 */
SWMM_ENGINE_API int swmm_engine_get_elapsed_seconds(SWMM_Engine engine, double* seconds);
/**
 * @brief Advance one physical simulation step and report elapsed days.
 *
 * Requires RUNNING and refuses recursive stepping or a due external driver update. Invokes progress
 * and begin callbacks before physics and the end callback after physics/output. Runtime source
 * expiries may shorten the proposed step. A failure after accepted physics or output recording does
 * not promise rollback; query state and diagnostics.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] elapsed_time Optional destination for elapsed simulation DAYS after the accepted
 *     step; 0 when the simulation is complete. NULL suppresses this output. Some early refusals
 *     leave it unchanged.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an invalid
 *     lifecycle state, or the underlying initialization/IO/plugin error.
 */
SWMM_ENGINE_API int swmm_engine_step(SWMM_Engine engine, double* elapsed_time);

/**
 * @brief Perform up to a requested number of physical steps synchronously.
 *
 * Stops early on an error or a step result <=0. A failed early step can overwrite the output with
 * zero even when prior physics remains accepted. Each individual step performs its usual callbacks
 * and lifecycle checks.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] n_steps Maximum step count; nonpositive values are a successful no-op after checking
 *     the handle.
 * @param[out] elapsed_time Optional elapsed-days output, initially set to zero and then to each
 *     step result; zero denotes completion.
 * @return SWMM_OK on success/no-op, SWMM_ERR_BADHANDLE for null engine, or the first error returned
 *     by swmm_engine_step().
 */
SWMM_ENGINE_API int swmm_engine_stride(SWMM_Engine engine, int n_steps, double* elapsed_time);

/**
 * @brief Finalize a running or completed simulation and enter SWMM_STATE_ENDED.
 *
 * Requires RUNNING or ENDED. Finalizes balances, pending IO and end-of-run output/hotstarts. If a
 * prior error has placed the engine in an error state, preserves and returns the existing
 * root-cause error instead of replacing it with a teardown-state complaint.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an invalid
 *     lifecycle state, or the underlying initialization/IO/plugin error.
 */
SWMM_ENGINE_API int swmm_engine_end(SWMM_Engine engine);

/**
 * @brief Write final model/simulation reports after ending the simulation.
 *
 * Requires the internal ENDED state and transitions to REPORTED (publicly exposed as
 * SWMM_STATE_ENDED). Preserves the root-cause error for failed runs. Report plugins/configuration
 * determine the destinations and content.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for an invalid
 *     lifecycle state, or the underlying initialization/IO/plugin error.
 */
SWMM_ENGINE_API int swmm_engine_report(SWMM_Engine engine);

/**
 * @brief Stop IO and release active model files, plugins and checkpoint attachments.
 *
 * May be called during teardown from any live engine state. Enters CLOSED and permits a fresh open;
 * it does not free the handle. Flushes recorded fatal diagnostics on failed runs. Engine-owned
 * pointers to mutable model data must not be retained across close/reopen.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_engine_close(SWMM_Engine engine);

/**
 * @brief Delete an engine and all resources it owns.
 *
 * Do not call concurrently with any operation or from inside a callback. Callback user_data remains
 * caller-owned and is never freed by this function.
 *
 * @param[in] engine Caller-owned engine handle, or NULL for a no-op. Do not use a non-null handle
 *     afterward or destroy it twice.
 */
SWMM_ENGINE_API void swmm_engine_destroy(SWMM_Engine engine);

/**
 * @brief Read the public lifecycle state of a live engine.
 *
 * Internal PAUSED maps to RUNNING, REPORTED maps to ENDED, and the internal error state maps to
 * SWMM_STATE_NONE.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] state Optional destination for an SWMM_EngineState integer. NULL performs only the
 *     handle check.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_engine_get_state(SWMM_Engine engine, int* state);

/* =========================================================================
 * Convenience run functions
 * ========================================================================= */

/**
 * @brief Create an engine, run the full lifecycle synchronously, and destroy it.
 *
 * Chains open, initialize, start(save_results=1), step loop, end, report, close and destroy. Early
 * failures close/destroy as appropriate; the temporary engine and its detailed diagnostics are not
 * available after return. All input strings remain caller-owned.
 *
 * @param[in] inp Borrowed NUL-terminated input-model path.
 * @param[in] rpt Borrowed report destination path, or NULL to omit it.
 * @param[in] out Borrowed binary-results destination path, or NULL to omit it.
 * @param[in] input_plugin_lib Borrowed input plugin library path, component ID or id:version;
 *     NULL/empty selects the built-in INP reader when no input plugin is already registered.
 * @return SWMM_OK on success; SWMM_ERR_NOMEM when engine creation fails, otherwise an
 *     open/initialize/start/step or recorded engine error.
 */
SWMM_ENGINE_API int swmm_engine_run(const char* inp, const char* rpt, const char* out,
                                    const char* input_plugin_lib);

/**
 * @brief Run the full synchronous lifecycle with a borrowed progress callback.
 *
 * Uses a temporary engine and destroys it before returning, as swmm_engine_run() does. Callbacks
 * must not throw, close/destroy the temporary engine or recursively advance it; progress
 * notifications cannot cancel the run.
 *
 * @param[in] inp Borrowed NUL-terminated input-model path.
 * @param[in] rpt Borrowed report destination path, or NULL to omit it.
 * @param[in] out Borrowed binary-results destination path, or NULL to omit it.
 * @param[in] input_plugin_lib Borrowed input plugin library path, component ID or id:version;
 *     NULL/empty selects the built-in INP reader when no input plugin is already registered.
 * @param[in] callback Optional progress function, invoked on the calling thread before each
 *     physical step; NULL disables progress notifications.
 * @param[in] user_data Borrowed opaque context passed unchanged to callback; may be NULL and must
 *     remain valid throughout this call.
 * @return SWMM_OK on success, SWMM_ERR_NOMEM when engine creation fails, or the lifecycle/recorded
 *     error.
 */
SWMM_ENGINE_API int swmm_engine_run_with_callback(const char* inp, const char* rpt, const char* out,
                                                  const char* input_plugin_lib,
                                                  SWMM_ProgressCallback callback, void* user_data);

/* =========================================================================
 * Callback registration
 * ========================================================================= */

/**
 * @brief Register, replace or disable pre-step progress.
 *
 * No lifecycle guard is applied. Callbacks run synchronously on the thread making the triggering
 * engine call; they must not throw or recursively step/close/destroy the engine. See the callback
 * typedef for timing and permitted updates.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] callback SWMM_ProgressCallback function pointer, or NULL to unregister. Borrowed until
 *     replaced/unregistered or engine destruction.
 * @param[in] user_data Borrowed opaque pointer passed through unchanged; may be NULL. Keep the
 *     context alive for every callback invocation.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_set_progress_callback(SWMM_Engine engine, SWMM_ProgressCallback callback,
                                               void* user_data);
/**
 * @brief Register, replace or disable diagnostics.
 *
 * No lifecycle guard is applied. Callbacks run synchronously on the thread making the triggering
 * engine call; they must not throw or recursively step/close/destroy the engine. See the callback
 * typedef for timing and permitted updates.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] callback SWMM_WarningCallback function pointer, or NULL to unregister. Borrowed until
 *     replaced/unregistered or engine destruction.
 * @param[in] user_data Borrowed opaque pointer passed through unchanged; may be NULL. Keep the
 *     context alive for every callback invocation.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_set_warning_callback(SWMM_Engine engine, SWMM_WarningCallback callback,
                                              void* user_data);
/**
 * @brief Register, replace or disable pre-physics step notifications.
 *
 * No lifecycle guard is applied. Callbacks run synchronously on the thread making the triggering
 * engine call; they must not throw or recursively step/close/destroy the engine. See the callback
 * typedef for timing and permitted updates.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] callback SWMM_StepBeginCallback function pointer, or NULL to unregister. Borrowed
 *     until replaced/unregistered or engine destruction.
 * @param[in] user_data Borrowed opaque pointer passed through unchanged; may be NULL. Keep the
 *     context alive for every callback invocation.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_set_step_begin_callback(SWMM_Engine engine,
                                                 SWMM_StepBeginCallback callback, void* user_data);
/**
 * @brief Register, replace or disable post-physics step notifications.
 *
 * No lifecycle guard is applied. Callbacks run synchronously on the thread making the triggering
 * engine call; they must not throw or recursively step/close/destroy the engine. See the callback
 * typedef for timing and permitted updates.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] callback SWMM_StepEndCallback function pointer, or NULL to unregister. Borrowed until
 *     replaced/unregistered or engine destruction.
 * @param[in] user_data Borrowed opaque pointer passed through unchanged; may be NULL. Keep the
 *     context alive for every callback invocation.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_set_step_end_callback(SWMM_Engine engine, SWMM_StepEndCallback callback,
                                               void* user_data);

/* =========================================================================
 * Error reporting
 * ========================================================================= */

/**
 * @brief Read the stored simulation error code without clearing it.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @return Stored error code (SWMM_OK if no error), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_last_error(SWMM_Engine engine);
/**
 * @brief Borrow the stored simulation error text.
 *
 * Reading does not clear the error. Diagnostic lifetime is tied to the underlying stored string,
 * which subsequent operations can replace.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @return Engine-owned NUL-terminated string, or the static string invalid engine handle for NULL.
 *     Never free it; copy before another error/model operation or engine destruction.
 */
SWMM_ENGINE_API const char* swmm_get_last_error_msg(SWMM_Engine engine);
/**
 * @brief Translate a known public error code to a static English description.
 *
 * Does not require an engine or include operation-specific context; use swmm_get_last_error_msg()
 * for the recorded diagnostic.
 *
 * @param[in] code SWMM_OK, an SWMM_ERR_* code, or a supported validation code.
 * @return Pointer to immutable static NUL-terminated storage; Unknown error for an unrecognized
 *     code. Never free it.
 */
SWMM_ENGINE_API const char* swmm_error_message(int code);

/**
 * @brief Select permissive model validation for subsequent editor loads.
 *
 * Lenient open keeps parsed objects editable in OPENED despite post-parse reference/validation
 * errors, which remain available through swmm_get_error_count()/swmm_get_error_at(). Hard
 * input-reader failures still fail. Running the repaired model requires a fresh strict open.
 *
 * @param[in] engine Live engine handle; NULL is a no-op.
 * @param[in] on 0 selects strict validation (default); nonzero enables lenient open.
 */
SWMM_ENGINE_API void swmm_engine_set_lenient_open(SWMM_Engine engine, int on);

/* Enumerate the errors/warnings accumulated on the engine during open (e.g.
 * after a lenient open). Index is 0-based; out-of-range returns "". */
/**
 * @brief Count accumulated error messages.
 *
 * This getter does not clear the list and is distinct from the stored last-error code.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @return Number of accumulated messages; 0 for null engine.
 */
SWMM_ENGINE_API int swmm_get_error_count(SWMM_Engine engine);
/**
 * @brief Borrow one accumulated error message.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] index Zero-based message index.
 * @return Engine-owned NUL-terminated message, or an empty string for null engine/out-of-range
 *     index. Never free; copy before operations that change the list or destroy the engine.
 */
SWMM_ENGINE_API const char* swmm_get_error_at(SWMM_Engine engine, int index);
/**
 * @brief Count accumulated warning messages.
 *
 * This getter does not clear the list and is distinct from the stored last-error code.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @return Number of accumulated messages; 0 for null engine.
 */
SWMM_ENGINE_API int swmm_get_warning_count(SWMM_Engine engine);
/**
 * @brief Borrow one accumulated warning message.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] index Zero-based message index.
 * @return Engine-owned NUL-terminated message, or an empty string for null engine/out-of-range
 *     index. Never free; copy before operations that change the list or destroy the engine.
 */
SWMM_ENGINE_API const char* swmm_get_warning_at(SWMM_Engine engine, int index);

/* =========================================================================
 * Thread capability query (engine-independent)
 * ========================================================================= */

/**
 * Hardware / OpenMP thread limits as seen by the engine process. Lets a host
 * show the user what [OPTIONS] THREADS can usefully be set to and why the
 * effective count may differ from the request.
 */
typedef struct {
    int logical_cpus;       /**< Logical processors (SMT included); 0 if unknown.   */
    int omp_max_threads;    /**< omp_get_max_threads() in this process. Lower than
                                 logical_cpus when OMP_NUM_THREADS / OMP_THREAD_LIMIT
                                 / CPU affinity limit the process. 1 without OpenMP. */
    int omp_available;      /**< 1 if the engine was built with OpenMP.             */
    int perf_cores;         /**< macOS: logical CPUs on the performance cluster;
                                 0 elsewhere / unknown.                              */
    int kokkos_omp_threads; /**< Threads the Kokkos OpenMP 2D backend initialised
                                 with in this process; 0 = not initialised.         */
} SWMM_ThreadInfo;

/**
 * @brief Query hardware and parallel-runtime limits without an engine.
 *
 * Reports process-visible logical CPUs, OpenMP limits/availability, macOS performance-cluster CPUs
 * and initialized Kokkos OpenMP team size. Zero indicates unavailable/unknown for fields whose
 * struct comments permit it.
 *
 * @param[out] out Non-null caller-owned SWMM_ThreadInfo destination; all fields are filled on
 *     success.
 * @return SWMM_OK, or SWMM_ERR_BADPARAM for null out.
 */
SWMM_ENGINE_API int swmm_get_thread_info(SWMM_ThreadInfo* out);

/**
 * @brief Preview thread-team sizes for a request and the current model.
 *
 * Read-only resolution using the same policy helpers as run setup. Model-size gates may reduce even
 * an explicit request; automatic requests also apply applicable performance-core policy. Does not
 * change the stored THREADS option or initialize a parallel backend.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] threads_option [OPTIONS] THREADS request: 0 is automatic; positive requests are
 *     resolved against model-size/runtime policies.
 * @param[out] global_threads Optional general OpenMP team-size destination.
 * @param[out] dw_threads Optional dynamic-wave team-size destination; zero when routing is not
 *     DYNWAVE.
 * @param[out] twod_threads Optional 2D CPU team-size destination; zero without an enabled nonempty
 *     mesh or 2D build support.
 * @return SWMM_OK, or SWMM_ERR_BADPARAM for null engine (this query uses BADPARAM rather than
 *     BADHANDLE).
 */
SWMM_ENGINE_API int swmm_get_effective_threads(SWMM_Engine engine, int threads_option,
                                               int* global_threads, int* dw_threads,
                                               int* twod_threads);

/* =========================================================================
 * Transport matrix (E2, 2026-09-07) — which species classes run in which
 * domain, and why not. Engine-computed from the open model's options
 * (IGNORE_QUALITY, WATER_AGE, HEAT_TRANSPORT, the reactions component,
 * IGNORE_2D and the [2D_OPTIONS] TRANSPORT_* keys); the .rpt prints the
 * same table. Valid after swmm_engine_open; re-read after option edits.
 * ========================================================================= */

#define SWMM_TRANSPORT_DOMAIN_RUNOFF 0
#define SWMM_TRANSPORT_DOMAIN_GROUNDWATER 1
#define SWMM_TRANSPORT_DOMAIN_NETWORK_1D 2
#define SWMM_TRANSPORT_DOMAIN_SURFACE_2D 3
#define SWMM_TRANSPORT_DOMAIN_COUNT 4

#define SWMM_TRANSPORT_CLASS_POLLUTANTS 0
#define SWMM_TRANSPORT_CLASS_MSX 1
#define SWMM_TRANSPORT_CLASS_AGE 2
#define SWMM_TRANSPORT_CLASS_TEMPERATURE 3
#define SWMM_TRANSPORT_CLASS_COUNT 4

#define SWMM_TRANSPORT_ENABLED 0
#define SWMM_TRANSPORT_DISABLED_BY_USER 1 /**< reason = the option key */
#define SWMM_TRANSPORT_UNAVAILABLE 2      /**< reason = why nothing is carried */

typedef struct SWMM_TransportCell {
    int state;       /**< SWMM_TRANSPORT_ENABLED / _DISABLED_BY_USER / _UNAVAILABLE */
    int count;       /**< rows of this class carried in the domain (0 unless ENABLED) */
    char reason[96]; /**< key or explanation; "" when ENABLED */
} SWMM_TransportCell;

typedef struct SWMM_TransportMatrix {
    SWMM_TransportCell cell[SWMM_TRANSPORT_DOMAIN_COUNT][SWMM_TRANSPORT_CLASS_COUNT];
} SWMM_TransportMatrix;

/**
 * @brief Resolve planned transport classes for each configured model domain.
 *
 * Read after open and again after relevant option edits. An unopened/unavailable domain reports
 * unavailable cells. This is configured transport resolution, not proof of runtime initialization
 * or local heat-flux availability.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] out Non-null caller-owned matrix indexed
 *     [SWMM_TRANSPORT_DOMAIN_*][SWMM_TRANSPORT_CLASS_*]; reasons are NUL-terminated and truncated
 *     to their fixed capacity.
 * @return SWMM_OK, or SWMM_ERR_BADPARAM for null engine/out.
 */
SWMM_ENGINE_API int swmm_get_transport_matrix(SWMM_Engine engine, SWMM_TransportMatrix* out);

/** Side-effect-free pre-run preview. Boolean fields must be 0 or 1;
 * groundwater is -1 (Automatic), 0 (Off), 1 (On). This is planned transport,
 * not proof of initialized runtime activity or local heat-flux support. */
typedef struct SWMM_TransportDraft {
    int ignore_quality, heat, age, ignore_2d;
    int surface_class[SWMM_TRANSPORT_CLASS_COUNT];
    int groundwater;
} SWMM_TransportDraft;
/**
 * @brief Resolve transport for a temporary draft without mutating the model.
 *
 * Uses existing mesh/aquifer authoring and a temporary options overlay. Does not initialize
 * transport, validate runtime support, or persist the draft.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] draft Non-null borrowed draft: Boolean fields and surface_class entries must be 0/1;
 *     groundwater must be -1 (automatic), 0 (off), or 1 (on).
 * @param[out] out Non-null caller-owned resolved matrix with bounded NUL-terminated reasons;
 *     unchanged on argument refusal.
 * @return SWMM_OK, or SWMM_ERR_BADPARAM for null arguments or invalid draft flags.
 */
SWMM_ENGINE_API int swmm_preview_transport_matrix(SWMM_Engine engine,
                                                  const SWMM_TransportDraft* draft,
                                                  SWMM_TransportMatrix* out);

/**
 * @brief Get the display name of a transport-domain axis value.
 *
 * @param[in] domain SWMM_TRANSPORT_DOMAIN_* integer in [0,SWMM_TRANSPORT_DOMAIN_COUNT).
 * @return Immutable static NUL-terminated name, or an empty static string when out of range; never
 *     free it.
 */
SWMM_ENGINE_API const char* swmm_transport_domain_name(int domain);
/**
 * @brief Get the display name of a transport-species class.
 *
 * @param[in] species_class SWMM_TRANSPORT_CLASS_* integer in [0,SWMM_TRANSPORT_CLASS_COUNT).
 * @return Immutable static NUL-terminated name, or an empty static string when out of range; never
 *     free it.
 */
SWMM_ENGINE_API const char* swmm_transport_class_name(int species_class);

/* =========================================================================
 * Simulation timing
 * ========================================================================= */

/**
 * @brief Read the configured simulation start calendar date.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] start Optional destination. SWMM DateTime decimal days since 1899-12-30.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_start_time(SWMM_Engine engine, double* start);
/**
 * @brief Read the configured simulation end calendar date.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] end Optional destination. SWMM DateTime decimal days since 1899-12-30.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_end_time(SWMM_Engine engine, double* end);
/**
 * @brief Read the current elapsed physics time.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] current Optional destination. Elapsed seconds since simulation start (context
 *     current_time), NOT a DateTime calendar value.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_current_time(SWMM_Engine engine, double* current);
/**
 * @brief Read the configured routing timestep limit.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] dt Optional destination. Configured routing-step seconds, not the last adaptive step
 *     duration.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_routing_step(SWMM_Engine engine, double* dt);

/* =========================================================================
 * Unit system query
 * =========================================================================
 * Most legacy model-object getters/setters use project DISPLAY units selected
 * by [OPTIONS] FLOW_UNITS. Individual APIs explicitly document exceptions,
 * including SI mesh/coupling quantities and elapsed/calendar time units. These
 * accessors report the project display-unit selection.
 */

/**
 * @brief Read the project display-flow unit code.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] flow_units Optional destination. 0=CFS, 1=GPM, 2=MGD, 3=CMS, 4=LPS, 5=MLD.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_flow_units(SWMM_Engine engine, int* flow_units);

/**
 * @brief Read the unit system implied by project flow units.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] unit_system Optional destination. 0=US/imperial or 1=SI/metric; CMS/LPS/MLD select
 *     SI.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_unit_system(SWMM_Engine engine, int* unit_system);

/* =========================================================================
 * Routing event and steady-state status
 * ========================================================================= */

/**
 * @brief Read the current implementation event-list presence flag.
 *
 * Despite the historical function name, the implementation does not compare the simulation clock
 * with event windows; it reports only whether the event list is nonempty.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] is_between Optional destination: 1 when at least one event entry is configured, 0 for
 *     an empty event list.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_is_between_events(SWMM_Engine engine, int* is_between);

/**
 * @brief Count configured routing-event entries.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] count Optional destination. Nonnegative number of stored event windows; alias of
 *     swmm_events_count().
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_event_count(SWMM_Engine engine, int* count);

/* -------------------------------------------------------------------------
 * [EVENTS] section accessors (Slice CW — added 2026-05-21).
 *
 * Mirrors the per-row reader in `handle_events` (InfraHandler.cpp). All times
 * are DateTime decimal days (consistent with `swmm_get_start_time` etc.).
 * `swmm_events_count` is a synonym of the legacy `swmm_get_event_count` and
 * is provided so the [EVENTS] surface is self-consistent.
 * ------------------------------------------------------------------------- */

/**
 * @brief Count configured routing-event entries.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] count Optional destination. Nonnegative number of stored event windows; alias of
 *     swmm_get_event_count().
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_events_count(SWMM_Engine engine, int* count);

/**
 * @brief Read the calendar window of a configured routing event.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] idx Zero-based event index in [0,event count).
 * @param[out] start Optional destination for event start as SWMM DateTime decimal days since
 *     1899-12-30.
 * @param[out] end Optional destination for event end in the same calendar-day units.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid idx.
 */
SWMM_ENGINE_API int swmm_events_get(SWMM_Engine engine, int idx, double* start, double* end);

/**
 * @brief Replace one event calendar window in place.
 *
 * Applies no lifecycle, overlap, sorting or finite-value validation; configure valid ordered event
 * data before running.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] idx Zero-based event index.
 * @param[in] start Start as SWMM DateTime decimal days since 1899-12-30; must compare less than
 *     end.
 * @param[in] end End in the same calendar-day units.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid idx, or
 *     SWMM_ERR_BADPARAM when start>=end.
 */
SWMM_ENGINE_API int swmm_events_set(SWMM_Engine engine, int idx, double start, double end);

/**
 * @brief Append a routing-event window in insertion order.
 *
 * Does not sort, merge overlaps or validate finite values. Caller retains no references into the
 * internal event vector.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] start Start as SWMM DateTime decimal days since 1899-12-30; must compare less than
 *     end.
 * @param[in] end End in the same calendar-day units.
 * @param[out] out_idx Optional destination for the appended zero-based index, written on success.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM when start>=end.
 */
SWMM_ENGINE_API int swmm_events_add(SWMM_Engine engine, double start, double end, int* out_idx);

/**
 * @brief Remove one configured event and shift later indices down.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] idx Zero-based event index to remove.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid idx.
 */
SWMM_ENGINE_API int swmm_events_remove(SWMM_Engine engine, int idx);

/**
 * @brief Remove all configured routing events.
 *
 * No lifecycle guard is applied; configure the event list before running.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_events_clear(SWMM_Engine engine);

/**
 * @brief Read the stored steady-state routing-skip option.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] enabled Optional destination. 1 when skipping is enabled, otherwise 0.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_get_steady_state_skip(SWMM_Engine engine, int* enabled);
/**
 * @brief Set the stored steady-state routing-skip option.
 *
 * Sets the option directly without a lifecycle guard. Routing logic determines whether individual
 * steps can be skipped.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] enabled 0 disables skipping; any nonzero value enables it.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_set_steady_state_skip(SWMM_Engine engine, int enabled);

/* =========================================================================
 * Phase 1b: Runoff interface file (legacy "Frunoff").
 *
 * Persists per-subcatchment runoff snapshots to a binary file so the
 * runoff phase of a long simulation can be cached and replayed in a
 * downstream routing-only run.  Mirrors the legacy SWMM-5 file format
 * (see src/engine/hydrology/RunoffInterface.hpp).
 *
 * SAVE mode is fully integrated: open the file in SAVE before
 * @c swmm_engine_start, run the simulation as normal, then close.  The
 * engine emits one record per runoff substep automatically.
 *
 * USE mode currently exposes the file but does NOT yet auto-skip the
 * engine's runoff computation — callers must invoke
 * @ref swmm_runoff_iface_read_step between simulation steps and
 * understand that the engine will overwrite the loaded state if runoff
 * still runs.  Full USE-mode auto-skip is tracked as a follow-up.
 *
 * @since 6.0.0
 * ========================================================================= */

/**
 * @brief Open a runoff cache for SAVE records using the current model schema.
 *
 * Call after configuring the model dimensions/units and before stepping. Automatic SAVE records are
 * emitted by runoff substeps; close with swmm_runoff_iface_close() or engine close.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] path Non-null, nonempty NUL-terminated output path. Existing contents are truncated;
 *     string is borrowed for the call.
 * @return 0 on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADPARAM for null/empty path,
 *     -10 when another runoff file is already open, or a runoff-interface file error.
 */
SWMM_ENGINE_API int swmm_runoff_iface_open_write(SWMM_Engine engine, const char* path);

/**
 * @brief Open a compatible runoff cache for USE-mode record reads.
 *
 * Header subcatchment count, pollutant count and flow units must match the current model. USE does
 * not automatically suppress native runoff computation; manually loaded states can be overwritten
 * by subsequent runoff work.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] path Non-null, nonempty NUL-terminated existing runoff-cache path; borrowed for the
 *     call.
 * @return 0 on success; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADPARAM for null/empty path,
 *     -10 when a runoff file is already open, or a file/header compatibility error.
 */
SWMM_ENGINE_API int swmm_runoff_iface_open_read(SWMM_Engine engine, const char* path);

/**
 * @brief Request a manual runoff-cache snapshot.
 *
 * Normally SAVE records are emitted automatically. With no open SAVE file this is a no-op; this
 * wrapper does not report a write/flush error from the underlying void save operation.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[in] dt Snapshot runoff substep duration in seconds, passed through without validation.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_runoff_iface_save_step(SWMM_Engine engine, double dt);

/**
 * @brief Read one available runoff record into the current subcatchment state.
 *
 * Returns SWMM_OK even when the underlying read returns false; has_data is the only per-call
 * availability signal. The next native runoff calculation may overwrite the imported state.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @param[out] has_data Optional destination: 1 when a record was read, 0 when no record is
 *     available (including EOF, no open file or an unsuitable read state).
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_runoff_iface_read_step(SWMM_Engine engine, int* has_data);

/**
 * @brief Close and release the runoff-cache file if present.
 *
 * Idempotent for a live engine; engine close also invokes this cleanup. Does not destroy the
 * engine.
 *
 * @param[in] engine Non-null live engine handle; caller provides exclusive access on the owning
 *     thread.
 * @return SWMM_OK on success, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_runoff_iface_close(SWMM_Engine engine);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* =========================================================================
 * Master include — pull in all domain headers
 *
 * Each domain header includes openswmm_engine.h itself (for SWMM_ENGINE_API
 * and the handle typedef), so include guards prevent recursion.
 * ========================================================================= */

#include "openswmm_datetime.h"
#include "openswmm_model.h"
#include "openswmm_nodes.h"
#include "openswmm_links.h"
#include "openswmm_subcatchments.h"
#include "openswmm_gages.h"
#include "openswmm_massbalance.h"
#include "openswmm_hotstart.h"
#include "openswmm_spatial.h"
#include "openswmm_pollutants.h"
#include "openswmm_initial_quality.h"
#include "openswmm_reactions.h"
#include "openswmm_process_components.h"
#include "openswmm_tables.h"
#include "openswmm_inflows.h"
#include "openswmm_controls.h"
#include "openswmm_infrastructure.h"
#include "openswmm_quality.h"
#include "openswmm_statistics.h"
#include "openswmm_forcing.h"
#include "openswmm_climate.h"
#include "openswmm_edit.h"
#include "openswmm_checkpoint.h"

#ifdef OPENSWMM_HAS_2D
#include "openswmm_2d.h"
#include "openswmm_gw_transport.h" /* U4 (2026-09-07) */
#endif

#endif /* OPENSWMM_ENGINE_H */
