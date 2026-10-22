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
 * @file      openswmm_callbacks.h
 * @brief     Callback function typedefs for the OpenSWMM Engine C API.
 *
 * @details Callbacks allow host applications to receive runtime notifications
 *          from the simulation engine without polling. Register callbacks via
 *          the corresponding swmm_set_*_callback() functions.
 *
 *          Registered runtime callbacks run synchronously on the thread making
 *          the engine call, not necessarily the application UI thread. Callback
 *          functions must not throw across the C boundary or recursively step,
 *          close or destroy the engine. Only explicitly permitted runtime
 *          setters may mutate state from step callbacks. The engine borrows
 *          callback/context pointers; keep their storage alive until replaced
 *          or unregistered. Strings supplied to callbacks are borrowed for the
 *          callback invocation and must be copied if retained.
 *
 * @note All callback function pointers accept a `void* user_data` parameter
 *       that is passed through unmodified from registration. Use this to
 *       carry context (e.g., a Python capsule, a C++ object pointer, etc.).
 *
 * @ingroup engine_api
 *
 * @see openswmm_engine.h for registration functions.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
 * @brief Report the simulation fraction immediately before a physical step.
 *
 * Invoked synchronously inside swmm_engine_step() before the step-begin callback and before
 * physics. It is a notification and does not cancel the step. Keep it lightweight; it may run at
 * every physical timestep. Register/unregister with swmm_set_progress_callback().
 *
 * @param[in] engine Borrowed handle for the engine making the callback; do not close/destroy it
 *     during the invocation.
 * @param[in] elapsed_frac Elapsed simulation seconds divided by planned duration, normally in
 *     [0,1]; zero when the duration is nonpositive. This is the fraction at the beginning of the
 *     upcoming step.
 * @param[in] sim_time Calendar time at the start of the upcoming step, as SWMM DateTime decimal
 *     days since 1899-12-30.
 * @param[in] user_data Borrowed context pointer supplied at registration or to the calling
 *     operation; may be NULL and is passed through unchanged.
 */
typedef void (*SWMM_ProgressCallback)(SWMM_Engine engine, double elapsed_frac, double sim_time,
                                      void* user_data);

/**
 * @brief Report synchronous read-only operation progress and optionally request cancellation.
 *
 * Runs on the calling thread. Do not mutate, recursively invoke, close or destroy the engine. There
 * is no persistent registration: pass it to a supporting read-only API for the duration of that
 * call.
 *
 * @param[in] engine Borrowed handle for the engine making the callback; do not close/destroy it
 *     during the invocation.
 * @param[in] stage Operation-specific stage identifier; interpret using the API that accepted this
 *     callback.
 * @param[in] progress Stage progress percentage in [0,100].
 * @param[in] user_data Borrowed context pointer supplied at registration or to the calling
 *     operation; may be NULL and is passed through unchanged.
 * @return Zero to continue; any nonzero value requests cancellation. The enclosing operation
 *     determines its cancellation return code and publication behavior.
 */
typedef int (*SWMM_CancelCallback)(SWMM_Engine engine, int stage, int progress, void* user_data);

/* =========================================================================
 * Warning / error callback
 * ========================================================================= */

/**
 * @brief Receive a diagnostic synchronously when the engine emits it.
 *
 * This callback also runs on fatal set_error() paths; receiving a message does not guarantee the
 * simulation can continue. Check the operation return value and engine error state.
 * Register/unregister with swmm_set_warning_callback().
 *
 * @param[in] engine Borrowed handle for the engine making the callback; do not close/destroy it
 *     during the invocation.
 * @param[in] code Diagnostic code: an SWMM_WarnCode warning, an error code for a fatal engine
 *     error, or 0 for an advisory without a specific warning code.
 * @param[in] message Borrowed NUL-terminated human-readable diagnostic when supplied by the
 *     emitter; copy it during this invocation if it must be retained.
 * @param[in] user_data Borrowed context pointer supplied at registration or to the calling
 *     operation; may be NULL and is passed through unchanged.
 */
typedef void (*SWMM_WarningCallback)(SWMM_Engine engine, int code, const char* message,
                                     void* user_data);

/* =========================================================================
 * Timestep begin/end callbacks
 * ========================================================================= */

/**
 * @brief Run before physics at the start of a physical simulation step.
 *
 * Called after the progress callback. Use only runtime APIs that explicitly permit callback updates
 * to inject external forcing or boundary prescriptions. Recursive stepping/advancement and engine
 * lifecycle changes are prohibited. Register/unregister with swmm_set_step_begin_callback().
 *
 * @param[in] engine Borrowed handle for the engine making the callback; do not close/destroy it
 *     during the invocation.
 * @param[in] sim_time Start-of-step SWMM DateTime in decimal days since 1899-12-30.
 * @param[in] dt Proposed step duration in seconds. A runtime prescription installed by this
 *     callback can introduce an earlier expiry and shorten the final step.
 * @param[in] user_data Borrowed context pointer supplied at registration or to the calling
 *     operation; may be NULL and is passed through unchanged.
 */
typedef void (*SWMM_StepBeginCallback)(SWMM_Engine engine, double sim_time, double dt,
                                       void* user_data);

/**
 * @brief Run after physics and output snapshots for one physical simulation step.
 *
 * Read available state for monitoring or install runtime prescriptions for the following step
 * through APIs that permit callback updates. One-step rainfall prescriptions and expired coupling
 * rows have already been expired. Checkpoint-boundary acceptance/recording may still occur after
 * this callback; the enclosing step can therefore still fail. Register/unregister with
 * swmm_set_step_end_callback().
 *
 * @param[in] engine Borrowed handle for the engine making the callback; do not close/destroy it
 *     during the invocation.
 * @param[in] sim_time End-of-step SWMM DateTime in decimal days since 1899-12-30.
 * @param[in] dt Actual duration of the completed physical step in seconds.
 * @param[in] user_data Borrowed context pointer supplied at registration or to the calling
 *     operation; may be NULL and is passed through unchanged.
 */
typedef void (*SWMM_StepEndCallback)(SWMM_Engine engine, double sim_time, double dt,
                                     void* user_data);

/* =========================================================================
 * Plugin event callbacks (fired by the plugin system)
 * ========================================================================= */

/**
 * @brief Describe a plugin-state notification callback signature.
 *
 * This header declares the signature only; the current public engine C API does not provide a
 * registration function for it. Do not assume that declaring this callback installs a notification
 * handler.
 *
 * @param[in] engine Borrowed handle for the engine making the callback; do not close/destroy it
 *     during the invocation.
 * @param[in] plugin_id Borrowed NUL-terminated plugin identifier, conventionally reverse-DNS.
 * @param[in] old_state Previous plugin state integer, interpreted by the emitting plugin system.
 * @param[in] new_state New plugin state integer, interpreted by the emitting plugin system.
 * @param[in] user_data Borrowed context pointer supplied at registration or to the calling
 *     operation; may be NULL and is passed through unchanged.
 */
typedef void (*SWMM_PluginStateCallback)(SWMM_Engine engine, const char* plugin_id, int old_state,
                                         int new_state, void* user_data);

/* =========================================================================
 * Hot start callbacks
 * ========================================================================= */

/**
 * @brief Describe a missing-object hot-start notification callback signature.
 *
 * This header declares the signature only; the current public engine C API does not register or
 * invoke this callback. Use the available hot-start diagnostics to inspect actual application
 * results.
 *
 * @param[in] engine Borrowed handle for the engine making the callback; do not close/destroy it
 *     during the invocation.
 * @param[in] object_type Borrowed object category token, such as NODE, LINK, SUBCATCH or FLAG.
 * @param[in] object_id Borrowed NUL-terminated missing-object identifier.
 * @param[in] user_data Borrowed context pointer supplied at registration or to the calling
 *     operation; may be NULL and is passed through unchanged.
 */
typedef void (*SWMM_HotStartMissingCallback)(SWMM_Engine engine, const char* object_type,
                                             const char* object_id, void* user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_ENGINE_CALLBACKS_H */
