/**
 * @file openswmm_engine.h
 * @brief OpenSWMM Engine — primary transparent C API (master header).
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
 *            - openswmm_model.h         — model building, validation, serialisation, options, user flags
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
 * @defgroup engine_api New Engine C API
 * @ingroup  new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_H
#define OPENSWMM_ENGINE_H

#include "openswmm_callbacks.h"

/* =========================================================================
 * Export macro
 * ========================================================================= */

#ifdef OPENSWMM_ENGINE_STATIC
#  define SWMM_ENGINE_API
#else
#  ifdef _WIN32
#    ifdef openswmm_engine_EXPORTS
#      define SWMM_ENGINE_API __declspec(dllexport)
#    else
#      define SWMM_ENGINE_API __declspec(dllimport)
#    endif
#  else
#    define SWMM_ENGINE_API __attribute__((visibility("default")))
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Error codes
 * ========================================================================= */

typedef enum SWMM_ErrorCode {
    SWMM_OK              =  0,
    SWMM_ERR_NOMEM       =  1,
    SWMM_ERR_INPFILE     =  2,
    SWMM_ERR_RPTFILE     =  3,
    SWMM_ERR_OUTFILE     =  4,
    SWMM_ERR_PARSE       =  5,
    SWMM_ERR_LIFECYCLE   =  6,
    SWMM_ERR_BADHANDLE   =  7,
    SWMM_ERR_BADINDEX    =  8,
    SWMM_ERR_BADPARAM    =  9,
    SWMM_ERR_PLUGIN      = 10,
    SWMM_ERR_IO          = 11,
    SWMM_ERR_HOTSTART    = 12,
    SWMM_ERR_CRS         = 13,
    SWMM_ERR_NUMERICAL   = 14,
    SWMM_ERR_DEPENDENCY  = 15,  /**< Object has dependents that would be affected by deletion. */
    SWMM_ERR_INTERNAL    = 99
} SWMM_ErrorCode;

typedef enum SWMM_WarnCode {
    SWMM_WARN_NONE              = 0,
    SWMM_WARN_HOTSTART_MISSING  = 1,
    SWMM_WARN_UNKNOWN_SECTION   = 2,
    SWMM_WARN_UNKNOWN_OPTION    = 3,
    SWMM_WARN_DEPRECATED_KW     = 4,
    SWMM_WARN_PLUGIN_INIT       = 5,
    SWMM_WARN_NUMERICAL         = 6,
    SWMM_WARN_STABILITY_LIMIT   = 7
} SWMM_WarnCode;

/* =========================================================================
 * Engine lifecycle state
 * ========================================================================= */

typedef enum SWMM_EngineState {
    SWMM_STATE_NONE        = 0,
    SWMM_STATE_CREATED     = 1,
    SWMM_STATE_OPENED      = 2,
    SWMM_STATE_INITIALIZED = 3,
    SWMM_STATE_STARTED     = 4,
    SWMM_STATE_RUNNING     = 5,
    SWMM_STATE_ENDED       = 6,
    SWMM_STATE_CLOSED      = 7,
    SWMM_STATE_BUILDING    = 8  /**< Programmatic model construction in progress */
} SWMM_EngineState;

/* =========================================================================
 * Object type identifiers
 * ========================================================================= */

typedef enum SWMM_ObjectType {
    SWMM_OBJ_GAGE      = 0,
    SWMM_OBJ_SUBCATCH  = 1,
    SWMM_OBJ_NODE      = 2,
    SWMM_OBJ_LINK      = 3,
    SWMM_OBJ_POLLUT    = 4,
    SWMM_OBJ_LANDUSE   = 5,
    SWMM_OBJ_TIMESER   = 6,
    SWMM_OBJ_TABLE     = 7,
    SWMM_OBJ_RDII      = 8,
    SWMM_OBJ_UNITHYD   = 9,
    SWMM_OBJ_SNOWMELT  = 10,
    SWMM_OBJ_SHAPE     = 11,
    SWMM_OBJ_LID       = 12
} SWMM_ObjectType;

/* =========================================================================
 * Engine lifecycle functions
 * ========================================================================= */

/** @brief Create a new engine instance (SWMM_STATE_CREATED). */
SWMM_ENGINE_API SWMM_Engine swmm_engine_create(void);

/**
 * @brief Open and parse a SWMM input file; load plugins → SWMM_STATE_OPENED.
 *
 * @param engine          Engine handle.
 * @param inp             Path to the input file.
 * @param rpt             Path to the report file, or NULL.
 * @param out             Path to the binary output file, or NULL.
 * @param input_plugin_lib Path to a shared library (.so/.dylib/.dll) that
 *                         provides an IInputPlugin via openswmm_plugin_info().
 *                         Pass NULL to use the built-in .inp reader.
 */
SWMM_ENGINE_API int swmm_engine_open(SWMM_Engine engine,
                                      const char* inp, const char* rpt, const char* out,
                                      const char* input_plugin_lib);

/** @brief Initialize the simulation → SWMM_STATE_INITIALIZED. */
SWMM_ENGINE_API int swmm_engine_initialize(SWMM_Engine engine);

/** @brief Start the simulation → SWMM_STATE_STARTED. */
SWMM_ENGINE_API int swmm_engine_start(SWMM_Engine engine, int save_results);

/** @brief Advance one explicit timestep. elapsed_time==0 when done. */
SWMM_ENGINE_API int swmm_engine_step(SWMM_Engine engine, double* elapsed_time);

/**
 * @brief Advance up to n_steps routing steps in one call (Gap #50 — swmm_stride equivalent).
 *
 * @details Calls step() n_steps times and stops early on error or when the
 *          simulation ends (elapsed_time == 0).  elapsed_time receives the
 *          last non-zero elapsed value, or 0.0 when the simulation has ended.
 *          Passing n_steps <= 0 is a no-op (returns SWMM_OK).
 *
 * @param engine        Engine handle.
 * @param n_steps       Maximum number of routing steps to advance.
 * @param elapsed_time  Receives elapsed time (decimal days) after the last step.
 * @return SWMM_OK on success; first non-OK error code encountered otherwise.
 */
SWMM_ENGINE_API int swmm_engine_stride(SWMM_Engine engine, int n_steps, double* elapsed_time);

/** @brief End the simulation → SWMM_STATE_ENDED. */
SWMM_ENGINE_API int swmm_engine_end(SWMM_Engine engine);

/** @brief Write summary report (SWMM_STATE_ENDED). */
SWMM_ENGINE_API int swmm_engine_report(SWMM_Engine engine);

/** @brief Close all files → SWMM_STATE_CLOSED. */
SWMM_ENGINE_API int swmm_engine_close(SWMM_Engine engine);

/** @brief Destroy the engine handle (any state, NULL safe). */
SWMM_ENGINE_API void swmm_engine_destroy(SWMM_Engine engine);

/** @brief Query the current engine lifecycle state. */
SWMM_ENGINE_API int swmm_engine_get_state(SWMM_Engine engine, int* state);

/* =========================================================================
 * Convenience run functions
 * ========================================================================= */

/**
 * @brief Run a complete simulation to completion.
 *
 * @details Creates an engine, then chains:
 *          open → initialize → start → step loop → end → report → close → destroy.
 *          Mirrors the legacy swmm_run() function.
 *
 * @param inp              Path to SWMM input file (.inp).
 * @param rpt              Path to report file (.rpt), or NULL to skip reporting.
 * @param out              Path to binary output file (.out), or NULL.
 * @param input_plugin_lib Path to input plugin library, or NULL for built-in .inp reader.
 * @returns                SWMM_OK (0) on success, or an error code.
 */
SWMM_ENGINE_API int swmm_engine_run(const char* inp, const char* rpt, const char* out,
                                     const char* input_plugin_lib);

/**
 * @brief Run a complete simulation with a progress callback.
 *
 * @details Same as swmm_engine_run() but registers a progress callback
 *          before starting the simulation loop.
 *
 * @param inp              Path to SWMM input file (.inp).
 * @param rpt              Path to report file (.rpt), or NULL to skip reporting.
 * @param out              Path to binary output file (.out), or NULL.
 * @param input_plugin_lib Path to input plugin library, or NULL for built-in .inp reader.
 * @param callback         Progress callback (may be NULL).
 * @param user_data        Opaque pointer passed through to the callback.
 * @returns                SWMM_OK (0) on success, or an error code.
 */
SWMM_ENGINE_API int swmm_engine_run_with_callback(
    const char* inp, const char* rpt, const char* out,
    const char* input_plugin_lib,
    SWMM_ProgressCallback callback, void* user_data);

/* =========================================================================
 * Callback registration
 * ========================================================================= */

SWMM_ENGINE_API int swmm_set_progress_callback  (SWMM_Engine engine,
                                                   SWMM_ProgressCallback  callback, void* user_data);
SWMM_ENGINE_API int swmm_set_warning_callback   (SWMM_Engine engine,
                                                   SWMM_WarningCallback   callback, void* user_data);
SWMM_ENGINE_API int swmm_set_step_begin_callback(SWMM_Engine engine,
                                                   SWMM_StepBeginCallback callback, void* user_data);
SWMM_ENGINE_API int swmm_set_step_end_callback  (SWMM_Engine engine,
                                                   SWMM_StepEndCallback   callback, void* user_data);

/* =========================================================================
 * Error reporting
 * ========================================================================= */

SWMM_ENGINE_API int         swmm_get_last_error    (SWMM_Engine engine);
SWMM_ENGINE_API const char* swmm_get_last_error_msg(SWMM_Engine engine);
SWMM_ENGINE_API const char* swmm_error_message     (int code);

/* =========================================================================
 * Simulation timing
 * ========================================================================= */

SWMM_ENGINE_API int swmm_get_start_time  (SWMM_Engine engine, double* start);
SWMM_ENGINE_API int swmm_get_end_time    (SWMM_Engine engine, double* end);
SWMM_ENGINE_API int swmm_get_current_time(SWMM_Engine engine, double* current);
SWMM_ENGINE_API int swmm_get_routing_step(SWMM_Engine engine, double* dt);

/* =========================================================================
 * Routing event and steady-state status
 * ========================================================================= */

/** @brief Check if simulation is currently between routing events. */
SWMM_ENGINE_API int swmm_is_between_events(SWMM_Engine engine, int* is_between);

/** @brief Get number of routing events defined.
 *  @note Synonymous with swmm_events_count (Slice CW). */
SWMM_ENGINE_API int swmm_get_event_count(SWMM_Engine engine, int* count);

/* -------------------------------------------------------------------------
 * [EVENTS] section accessors (Slice CW — added 2026-05-21).
 *
 * Mirrors the per-row reader in `handle_events` (InfraHandler.cpp). All times
 * are DateTime decimal days (consistent with `swmm_get_start_time` etc.).
 * `swmm_events_count` is a synonym of the legacy `swmm_get_event_count` and
 * is provided so the [EVENTS] surface is self-consistent.
 * ------------------------------------------------------------------------- */

/** @brief Number of [EVENTS] entries. Alias of swmm_get_event_count. */
SWMM_ENGINE_API int swmm_events_count(SWMM_Engine engine, int* count);

/** @brief Get the start/end DateTime of the idx-th event.
 *  @returns SWMM_OK on success; SWMM_ERR_BADINDEX if idx is out of range. */
SWMM_ENGINE_API int swmm_events_get(SWMM_Engine engine, int idx,
                                    double* start, double* end);

/** @brief Overwrite the idx-th event's window.
 *  @returns SWMM_OK on success; SWMM_ERR_BADINDEX if idx out of range;
 *           SWMM_ERR_BADPARAM if start >= end. */
SWMM_ENGINE_API int swmm_events_set(SWMM_Engine engine, int idx,
                                    double start, double end);

/** @brief Append a new event window.
 *  @param out_idx Receives the new event's index (optional, may be NULL).
 *  @returns SWMM_OK on success; SWMM_ERR_BADPARAM if start >= end. */
SWMM_ENGINE_API int swmm_events_add(SWMM_Engine engine,
                                    double start, double end, int* out_idx);

/** @brief Remove the idx-th event. Trailing entries shift down by one.
 *  @returns SWMM_OK on success; SWMM_ERR_BADINDEX if idx is out of range. */
SWMM_ENGINE_API int swmm_events_remove(SWMM_Engine engine, int idx);

/** @brief Remove all events. */
SWMM_ENGINE_API int swmm_events_clear(SWMM_Engine engine);

/** @brief Get/set steady-state skip flag. */
SWMM_ENGINE_API int swmm_get_steady_state_skip(SWMM_Engine engine, int* enabled);
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
 * @brief Open the runoff interface file for writing (SAVE mode).
 *
 * @param engine  Engine handle (any state).
 * @param path    Output file path.  Existing file is truncated.
 * @returns @c SWMM_OK on success.  Non-zero error codes include
 *          @c SWMM_ERR_BADHANDLE (invalid engine), @c SWMM_ERR_BADPARAM
 *          (null path), and a non-zero file-I/O error code when the
 *          file could not be opened or a runoff file is already open.
 */
SWMM_ENGINE_API int swmm_runoff_iface_open_write(SWMM_Engine engine, const char* path);

/**
 * @brief Open the runoff interface file for reading (USE mode).
 *
 * @param engine  Engine handle.
 * @param path    Path to an existing runoff interface file.
 * @returns @c SWMM_OK on success; non-zero on file-open failure or
 *          header mismatch (subcatchment count, pollutant count, or
 *          flow units differ from the current model).
 */
SWMM_ENGINE_API int swmm_runoff_iface_open_read(SWMM_Engine engine, const char* path);

/**
 * @brief Manually emit one runoff substep record to the open SAVE file.
 *
 * @details Normally the engine emits records automatically inside
 *          @c stepRunoff().  This entry point is exposed so plugin
 *          authors and tests can force a snapshot at well-defined
 *          times; it is a no-op when no file is open or the file is
 *          in USE mode.
 *
 * @param engine  Engine handle.
 * @param dt      Substep duration (seconds) to record in the file for
 *                this snapshot.
 */
SWMM_ENGINE_API int swmm_runoff_iface_save_step(SWMM_Engine engine, double dt);

/**
 * @brief Read one runoff substep record from the open USE file into
 *        the current subcatchment state.
 *
 * @param engine     Engine handle.
 * @param[out] has_data  Set to @c 1 when a record was successfully
 *                       read, @c 0 on EOF.  May be NULL.
 * @returns @c SWMM_OK in either case (use @p has_data to tell EOF
 *          apart from a successful read).  Returns @c SWMM_ERR_BADHANDLE
 *          on null engine; @c SWMM_ERR_BADPARAM if no USE-mode file is
 *          open.
 */
SWMM_ENGINE_API int swmm_runoff_iface_read_step(SWMM_Engine engine, int* has_data);

/**
 * @brief Close the runoff interface file.
 *
 * Safe to call multiple times; safe to call when no file was opened.
 * Also called automatically as part of @c swmm_engine_close.
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

#include "openswmm_model.h"
#include "openswmm_nodes.h"
#include "openswmm_links.h"
#include "openswmm_subcatchments.h"
#include "openswmm_gages.h"
#include "openswmm_massbalance.h"
#include "openswmm_hotstart.h"
#include "openswmm_spatial.h"
#include "openswmm_pollutants.h"
#include "openswmm_tables.h"
#include "openswmm_inflows.h"
#include "openswmm_controls.h"
#include "openswmm_infrastructure.h"
#include "openswmm_quality.h"
#include "openswmm_statistics.h"
#include "openswmm_forcing.h"
#include "openswmm_edit.h"

#ifdef OPENSWMM_HAS_2D
#include "openswmm_2d.h"
#endif

#endif /* OPENSWMM_ENGINE_H */
