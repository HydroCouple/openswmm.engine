/**
 * @file openswmm_callbacks.h
 * @brief Callback function typedefs for the OpenSWMM Engine C API.
 *
 * @details Callbacks allow host applications to receive runtime notifications
 *          from the simulation engine without polling. Register callbacks via
 *          the corresponding swmm_set_*_callback() functions.
 *
 *          All callbacks are invoked on the **main simulation thread** unless
 *          otherwise noted. Callbacks must be thread-safe with respect to the
 *          IO thread (i.e., do not modify simulation state from within a
 *          callback unless the engine is in a safe state).
 *
 * @note All callback function pointers accept a `void* user_data` parameter
 *       that is passed through unmodified from registration. Use this to
 *       carry context (e.g., a Python capsule, a C++ object pointer, etc.).
 *
 * @ingroup engine_api
 *
 * @see openswmm_engine.h for registration functions.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_CALLBACKS_H
#define OPENSWMM_ENGINE_CALLBACKS_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque handle to an OpenSWMM Engine instance. */
typedef void* SWMM_Engine;

/* =========================================================================
 * Progress callback
 * ========================================================================= */

/**
 * @brief Called after each simulation timestep to report progress.
 *
 * @details Invoked from inside swmm_engine_step() after advancing the
 *          simulation by one timestep. The callback receives the current
 *          elapsed fraction (0.0 to 1.0) and absolute simulation time.
 *
 * @param engine        The engine handle that fired the callback.
 * @param elapsed_frac  Fraction of simulation complete [0.0, 1.0].
 * @param sim_time      Current simulation time in decimal days (Julian date).
 * @param user_data     User-supplied context pointer from registration.
 *
 * @note This callback is called on every physical timestep, which may be
 *       very frequent. Keep the callback implementation lightweight.
 *
 * Example (C):
 * @code{.c}
 * void my_progress(SWMM_Engine e, double frac, double t, void* ud) {
 *     printf("Progress: %.1f%%  t=%.4f days\n", frac * 100.0, t);
 * }
 * swmm_set_progress_callback(engine, my_progress, NULL);
 * @endcode
 */
typedef void (*SWMM_ProgressCallback)(
    SWMM_Engine engine,
    double      elapsed_frac,
    double      sim_time,
    void*       user_data
);

/* =========================================================================
 * Warning / error callback
 * ========================================================================= */

/**
 * @brief Called when the engine emits a warning or non-fatal error.
 *
 * @details Warnings are issued for recoverable situations such as:
 *          - Missing objects during hot start application
 *          - Deprecated input section keywords
 *          - Numerical instabilities that were handled gracefully
 *          - Plugin initialization issues
 *
 * @param engine    The engine handle that fired the callback.
 * @param code      Warning/error code (see SWMM_WarnCode enum in openswmm_engine.h).
 * @param message   Null-terminated human-readable warning message.
 * @param user_data User-supplied context pointer from registration.
 */
typedef void (*SWMM_WarningCallback)(
    SWMM_Engine engine,
    int         code,
    const char* message,
    void*       user_data
);

/* =========================================================================
 * Timestep begin/end callbacks
 * ========================================================================= */

/**
 * @brief Called at the beginning of each simulation timestep, before physics.
 *
 * @details This is the correct place to inject external forcings or modify
 *          boundary conditions before the engine advances the simulation.
 *
 * @param engine    The engine handle.
 * @param sim_time  Current simulation time (start of this step) in decimal days.
 * @param dt        Timestep duration in seconds.
 * @param user_data User-supplied context pointer.
 */
typedef void (*SWMM_StepBeginCallback)(
    SWMM_Engine engine,
    double      sim_time,
    double      dt,
    void*       user_data
);

/**
 * @brief Called at the end of each simulation timestep, after physics.
 *
 * @details This is the correct place to read simulation results for
 *          real-time monitoring or control decisions.
 *
 * @param engine    The engine handle.
 * @param sim_time  Simulation time at the END of this step in decimal days.
 * @param dt        Timestep duration in seconds that was just completed.
 * @param user_data User-supplied context pointer.
 */
typedef void (*SWMM_StepEndCallback)(
    SWMM_Engine engine,
    double      sim_time,
    double      dt,
    void*       user_data
);

/* =========================================================================
 * Plugin event callbacks (fired by the plugin system)
 * ========================================================================= */

/**
 * @brief Called when a plugin changes state.
 *
 * @param engine      The engine handle.
 * @param plugin_id   Null-terminated plugin identifier (reverse-DNS string).
 * @param old_state   Previous plugin state (SWMM_PluginState enum value).
 * @param new_state   New plugin state.
 * @param user_data   User-supplied context pointer.
 */
typedef void (*SWMM_PluginStateCallback)(
    SWMM_Engine engine,
    const char* plugin_id,
    int         old_state,
    int         new_state,
    void*       user_data
);

/* =========================================================================
 * Hot start callbacks
 * ========================================================================= */

/**
 * @brief Called for each object that was missing when applying a hot start.
 *
 * @details Allows the host application to log or react to missing objects.
 *
 * @param engine       The engine handle.
 * @param object_type  Type string: "NODE", "LINK", "SUBCATCH", "FLAG".
 * @param object_id    Null-terminated identifier of the missing object.
 * @param user_data    User-supplied context pointer.
 */
typedef void (*SWMM_HotStartMissingCallback)(
    SWMM_Engine engine,
    const char* object_type,
    const char* object_id,
    void*       user_data
);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_ENGINE_CALLBACKS_H */
