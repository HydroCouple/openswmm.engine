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
 * @file      openswmm_heat.h
 * @brief     Heat-transport configuration as an editable surface (phase H6a).
 *
 * @details The `model.heat` component configuration — flux-module toggles,
 *          `[RADIATIVE_FLUXES]` parameters, and H6a's `[SOLAR_RADIATION]`
 *          and `[CLOUD_COVER]` sections — reachable from C. This is the
 *          header `UNIFIED_PLAN_STATUS_2026-08-29.md` §6 names as the
 *          blocker on GUI task **G4g** (heat configuration editor).
 *
 * @par [HEAT_SOURCES]
 *      The inlet-temperature table was the one section H6a left without an
 *      API; step 3 (2026-08-30) added it below — discovery + CRUD in
 *      `openswmm_reactions.h`'s count/get/add shape, refusing exactly what
 *      the parser refuses.
 *
 * @par Edits are live
 *      Setters mutate engine state; the flux modules re-read the config
 *      every step, so a mid-run edit takes effect on the next routing step.
 *      This is the same contract `openswmm_water_age.h` documents.
 *      New finite mesh/soil stores require model selection, initial state
 *      and capacity-defining parameters before initialization. Weather and
 *      flux inputs retain their live-update behavior.
 *
 * @warning `swmm_heat_set_shortwave_mode` with
 *          ::SWMM_HEAT_SW_COMPUTED returns ::SWMM_ERR_BADPARAM unless BOTH
 *          latitude and longitude have been set. The engine will not fall
 *          back on the `[TEMPERATURE]` SNOWMELT latitude: that field
 *          defaults to 0, so borrowing it would silently model equatorial
 *          noon (heat plan §2.5).
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §2.5, §5, §6 H6a
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_HEAT_H
#define OPENSWMM_HEAT_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Flux modules of heat plan §2, each independently toggleable. */
typedef enum SWMM_HeatFluxModule {
    SWMM_HEAT_SURFACE_EXCHANGE = 0,   /**< Latent + sensible (H2). */
    SWMM_HEAT_RADIATIVE_EXCHANGE = 1, /**< Shortwave + longwave (H3). */
    SWMM_HEAT_LAYER_CONDUCTION = 2,   /**< LID vertical conduction (H5b). */
    SWMM_HEAT_SEDIMENT_EXCHANGE = 3   /**< Channel bed conduction and hyporheic exchange. */
} SWMM_HeatFluxModule;

/** Pre-run climate/snow/heat transaction. The callback may use only climate,
 * snow configuration, heat configuration (including 2D heat and GW/aquifer
 * authoring), HEAT_TRANSPORT, component registrations and climate-file path
 * setters. Returning an error restores values and provenance. The callback
 * must not change topology, destroy the engine, run, or nest transactions. */
typedef int (*SWMM_HydrothermalEdit)(SWMM_Engine engine, void* user);
/**
 * @brief Apply one synchronous hydrothermal configuration transaction with rollback.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] edit Required synchronous callback, called once with engine and user;
 * return SWMM_OK to commit, another status to restore the captured configuration.
 * @param[in,out] user Opaque caller-owned context forwarded unchanged; may be NULL.
 * @note Callback storage is not retained. Transactions may not nest on the same
 * thread, even across different engines. Serialize all access to the engine.
 * Callback exceptions produce SWMM_ERR_BADPARAM and rollback; allocation failure
 * returns SWMM_ERR_NOMEM. Only the documented configuration domains are captured.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_hydrothermal_edit_atomic(SWMM_Engine engine, SWMM_HydrothermalEdit edit,
                                                  void* user);
/**
 * @brief Dry policy: HOLD=0, AIR=1, DEFAULT=2.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] policy Required result destination: HOLD=0, AIR=1 or DEFAULT=2.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_dry_policy(SWMM_Engine engine, int* policy);
/**
 * @brief Select the reported temperature policy for dry water elements.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] policy HOLD=0, AIR=1 or DEFAULT=2; other values are refused.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_set_dry_policy(SWMM_Engine engine, int policy);
/**
 * @brief Read-only LID material assumptions: water conductivity, soil conductivity,
 * soil density, soil specific heat, all SI.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] values Required array of four doubles: water conductivity W/(m K),
 * soil conductivity W/(m K), soil density kg/m3, and soil specific heat J/(kg K).
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_lid_material(SWMM_Engine engine, double values[4]);
/**
 * @brief Remove explicit shared solar coordinates and their provenance.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @note Refused while COMPUTED shortwave is selected. Removes the shared site
 * and explicit heat-coordinate flags without resetting numeric field values.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_heat_clear_solar_site(SWMM_Engine engine);
/** Exact pre-run memento for heat settings, 2D heat/GW/aquifer authoring, their shared site and component
 * registrations. Callers must check their authoring identity before Undo. It owns no
 * engine; the caller must ensure the original engine is still open before
 * restoring. Destroy is safe after closing the engine. */
/**
 * @brief Authoring provenance bits: configured=1, shared site=2, explicit
 * latitude/longitude/timezone/elevation=4/8/16/32.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] flags Required result destination: bitwise provenance flags listed above.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_authoring_flags(SWMM_Engine engine, unsigned* flags);
typedef void* SWMM_HeatSnapshot;
/**
 * @brief Allocate a reusable heat authoring snapshot for an editable engine.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @note Requires BUILDING/OPENED. Caller owns the returned reusable snapshot
 * and releases it exactly once using the matching destroy function. It captures
 * configuration and selected caches, with no ownership of the engine or runtime state.
 * @return Newly allocated SWMM_HeatSnapshot; NULL for a null handle, noneditable
 * state, or any allocation/copy failure.
 */
SWMM_ENGINE_API SWMM_HeatSnapshot swmm_heat_snapshot_create(SWMM_Engine engine);
/**
 * @brief Restore a reusable heat authoring snapshot to its original engine.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] snapshot Required live snapshot produced by swmm_heat_snapshot_create()
 * for this exact engine identity. The snapshot remains valid and reusable after restore.
 * @note Caller must ensure the original engine is still live. Captured storage
 * is copied before publication; this call does not consume the snapshot.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_heat_snapshot_restore(SWMM_Engine engine, SWMM_HeatSnapshot snapshot);
/**
 * @brief Release a heat authoring snapshot.
 * @param[in] snapshot Owned snapshot of the matching type; NULL is safe.
 * @note May be called after the engine is closed. Invalid, foreign or previously
 * freed snapshot pointers are unsupported. Does not close or destroy an engine.
 */
SWMM_ENGINE_API void swmm_heat_snapshot_destroy(SWMM_HeatSnapshot snapshot);

/** Exact pre-run authoring memento spanning simulation options, monthly
 * climate adjustments, climate/snow/UEB, heat (including 2D thermal/GW
 * authoring), shared site state and process-component registrations. It owns
 * no engine; restore requires the same editable engine used to create it.
 * The snapshot is reusable for Undo and Redo. */
typedef void* SWMM_ConfigSnapshot;
/**
 * @brief Allocate a reusable complete hydrothermal authoring snapshot.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @note Requires BUILDING/OPENED. Caller owns the returned reusable snapshot
 * and releases it exactly once using the matching destroy function. It captures
 * configuration and selected caches, with no ownership of the engine or runtime state.
 * @return Newly allocated SWMM_ConfigSnapshot; NULL for a null handle, noneditable
 * state, or any allocation/copy failure.
 */
SWMM_ENGINE_API SWMM_ConfigSnapshot swmm_config_snapshot_create(SWMM_Engine engine);
/**
 * @brief Restore complete hydrothermal authoring configuration atomically.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] snapshot Required live snapshot produced by swmm_config_snapshot_create()
 * for this exact engine identity. The snapshot remains valid and reusable after restore.
 * @note Caller must ensure the original engine is still live. Captured storage
 * is copied before publication; this call does not consume the snapshot.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_config_snapshot_restore(SWMM_Engine engine, SWMM_ConfigSnapshot snapshot);
/**
 * @brief Release a config authoring snapshot.
 * @param[in] snapshot Owned snapshot of the matching type; NULL is safe.
 * @note May be called after the engine is closed. Invalid, foreign or previously
 * freed snapshot pointers are unsupported. Does not close or destroy an engine.
 */
SWMM_ENGINE_API void swmm_config_snapshot_destroy(SWMM_ConfigSnapshot snapshot);

/**
 * @brief Authored status and reset for ground/initial bed temperature.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] param SWMM_HEAT_SED_GROUND_TEMPERATURE or SWMM_HEAT_SED_INITIAL_TEMPERATURE.
 * @param[out] configured Required result, 1 for explicitly authored temperature or 0 for
 * inheritance.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_sediment_temperature_configured(SWMM_Engine engine, int param,
                                                              int* configured);
/**
 * @brief Reset one authored sediment temperature to inherited/default behavior.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] param SWMM_HEAT_SED_GROUND_TEMPERATURE or SWMM_HEAT_SED_INITIAL_TEMPERATURE.
 * @note Clearing initial temperature requires editable state when an enabled 2D bed
 * model captures it at initialization; clearing ground temperature also removes its series.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_clear_sediment_temperature(SWMM_Engine engine, int param);

/** @brief Where incoming shortwave comes from (plan §2.5, D-H6a-3).
 *
 *  The three are MUTUALLY EXCLUSIVE in EFFECT — exactly one is read, and
 *  there is no precedence ladder behind them.
 *
 *  Switching modes does NOT erase the other modes' settings: a constant
 *  stays stored while a timeseries is active, and vice versa, so a GUI can
 *  offer three radio buttons without destroying what the user typed under
 *  the other two. Only the selected one reaches the model. (A deck cannot
 *  express that ambiguity at all — the parser refuses a second SHORTWAVE
 *  row outright.) */
typedef enum SWMM_HeatShortwaveMode {
    SWMM_HEAT_SW_CONSTANT = 0,   /**< Fixed W/m². */
    SWMM_HEAT_SW_TIMESERIES = 1, /**< Measured record. */
    SWMM_HEAT_SW_COMPUTED = 2    /**< Solar position + Bird clear-sky. */
} SWMM_HeatShortwaveMode;

/** @brief `[RADIATIVE_FLUXES]` scalar parameters. */
typedef enum SWMM_HeatRadiativeParam {
    SWMM_HEAT_RAD_SHORTWAVE = 0,            /**< W/m², CONSTANT mode only. */
    SWMM_HEAT_RAD_ALBEDO = 1,               /**< Water reflectance Rs [0,1]. */
    SWMM_HEAT_RAD_SHADE_FACTOR = 2,         /**< fs [0,1]. */
    SWMM_HEAT_RAD_SKY_VIEW = 3,             /**< fsky [0,1]. */
    SWMM_HEAT_RAD_EMISS_WATER = 4,          /**< εw [0,1]. */
    SWMM_HEAT_RAD_EMISS_LANDCOVER = 5,      /**< εlc [0,1]. */
    SWMM_HEAT_RAD_ATM_EMISS_COEFF = 6,      /**< Brunt Aa [0,1]. */
    SWMM_HEAT_RAD_LW_REFLECTION = 7,        /**< RL [0,1]. */
    SWMM_HEAT_RAD_LANDCOVER_TEMPERATURE = 8 /**< °C; NaN restores air-temperature inheritance. */
} SWMM_HeatRadiativeParam;

/** @brief `[SOLAR_RADIATION]` parameters (consulted under COMPUTED only). */
typedef enum SWMM_HeatSolarParam {
    SWMM_HEAT_SOLAR_LATITUDE = 0,      /**< Degrees, +N, [-90, 90]. */
    SWMM_HEAT_SOLAR_LONGITUDE = 1,     /**< Degrees, +E, [-180, 180]. */
    SWMM_HEAT_SOLAR_TIMEZONE = 2,      /**< Hours from UTC, +E. */
    SWMM_HEAT_SOLAR_ELEVATION = 3,     /**< Metres, [-500, 9000]. Below sea
                                        *   level is legal. Never written =
                                        *   take the climate state's elev. */
    SWMM_HEAT_SOLAR_TURBIDITY_380 = 4, /**< Bird AOD at 380 nm. */
    SWMM_HEAT_SOLAR_TURBIDITY_500 = 5, /**< Bird AOD at 500 nm. */
    SWMM_HEAT_SOLAR_PRECIP_WATER = 6,  /**< Precipitable water, cm. */
    SWMM_HEAT_SOLAR_OZONE = 7,         /**< Ozone column, cm. */
    SWMM_HEAT_SOLAR_GROUND_ALBEDO = 8  /**< LAND albedo [0,1] — NOT the
                                        *   water's SWMM_HEAT_RAD_ALBEDO. */
} SWMM_HeatSolarParam;

/** @brief `[CLOUD_COVER]` parameters. */
typedef enum SWMM_HeatCloudParam {
    SWMM_HEAT_CLOUD_FRACTION = 0,   /**< C [0,1], constant spelling. */
    SWMM_HEAT_CLOUD_SW_ATTEN_K = 1, /**< Kasten–Czeplak k. */
    SWMM_HEAT_CLOUD_SW_ATTEN_N = 2, /**< Kasten–Czeplak n. */
    SWMM_HEAT_CLOUD_LW_CLOUD_K = 3  /**< Bolz k_lw. */
} SWMM_HeatCloudParam;

/* ---------------------------------------------------------------- toggles */

/**
 * @brief Is `[OPTIONS] HEAT_TRANSPORT` on?
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] enabled Required result destination: 1 when HEAT_TRANSPORT is enabled, otherwise 0.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_enabled(SWMM_Engine engine, int* enabled);

/**
 * @brief Read one `[HEAT_FLUXES]` module toggle.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] module SWMM_HeatFluxModule selector.
 * @param[out] on Required destination; zero disables, nonzero enables.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_get_module(SWMM_Engine engine, int module, int* on);
/**
 * @brief Write one `[HEAT_FLUXES]` module toggle.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] module SWMM_HeatFluxModule selector.
 * @param[in] on New value; zero disables, nonzero enables.
 * @note Runtime toggles are accepted except enabling previously unmapped channel
 * mesh-contact sediment exchange, which requires BUILDING/OPENED.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_set_module(SWMM_Engine engine, int module, int on);

/* ------------------------------------------------------------- radiative */

/**
 * @brief Read one `[RADIATIVE_FLUXES]` parameter.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] param SWMM_HeatRadiativeParam selector.
 * @param[out] value Required destination; units and allowed range follow the selected enum member.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_get_radiative(SWMM_Engine engine, int param, double* value);
/**
 * @brief Write one `[RADIATIVE_FLUXES]` parameter.
 *  @returns ::SWMM_ERR_BADPARAM if a fraction is outside [0,1]; if
 *           SHORTWAVE is negative; or if SHORTWAVE is written while the
 *           mode is not ::SWMM_HEAT_SW_CONSTANT — a constant is not read in
 *           the other two modes, and storing one there would look
 *           configured while changing nothing. Switch the mode first.
 *
 *           Values are REFUSED, not clamped — the parser's rule, so the API
 *           and the deck agree. A refused write does not take effect.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] param SWMM_HeatRadiativeParam selector.
 * @param[in] value New value; units and allowed range follow the selected enum member.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_set_radiative(SWMM_Engine engine, int param, double value);

/**
 * @brief Read the incoming-shortwave mode (::SWMM_HeatShortwaveMode).
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] mode Required result destination: SWMM_HeatShortwaveMode code.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_shortwave_mode(SWMM_Engine engine, int* mode);
/**
 * @brief Set the incoming-shortwave mode.
 *  @returns ::SWMM_ERR_BADPARAM for ::SWMM_HEAT_SW_COMPUTED when latitude
 *           or longitude is unset, and for ::SWMM_HEAT_SW_TIMESERIES when
 *           no series has been bound by
 *           ::swmm_heat_set_shortwave_timeseries.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] mode SWMM_HEAT_SW_CONSTANT, SWMM_HEAT_SW_TIMESERIES or SWMM_HEAT_SW_COMPUTED.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_set_shortwave_mode(SWMM_Engine engine, int mode);

/**
 * @brief Bind a `[TIMESERIES]` (by name) as the shortwave record and
 *         switch the mode to ::SWMM_HEAT_SW_TIMESERIES.
 *  @returns ::SWMM_ERR_BADPARAM if no timeseries of that name exists.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] name Required nonempty existing model time-series name; resolved during this call.
 * @note No input string storage is retained.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_set_shortwave_timeseries(SWMM_Engine engine, const char* name);

/**
 * @brief Name of the `[TIMESERIES]` bound as the shortwave record, or ""
 *         when none is bound. NUL-terminated, truncated to `buflen`.
 *  @details The read half `swmm_heat_set_shortwave_timeseries` never had —
 *           an editor could rebind a series but only display "(keep current
 *           series)" for the one already bound (the G4g gap, recorded
 *           2026-08-31). Valid in every mode: the binding survives a switch
 *           to CONSTANT or COMPUTED, exactly as the parser's does.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] buf Required writable string destination, safely truncated with a trailing NUL.
 * @param[in] buflen Positive capacity in bytes including the terminator.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_shortwave_timeseries(SWMM_Engine engine, char* buf, int buflen);

/**
 * @brief Resolved incoming shortwave at the CURRENT step, W/m², cloud
 *         already applied. Read-only: this is state, not configuration.
 *         0 before the first step, and whenever radiative exchange is off.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] wm2 Required result destination: last resolved incoming shortwave in W/m2.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_current_shortwave(SWMM_Engine engine, double* wm2);

/* ----------------------------------------------------------------- solar */

/**
 * @brief Read one `[SOLAR_RADIATION]` parameter.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] param SWMM_HeatSolarParam selector.
 * @param[out] value Required destination; units and allowed range follow the selected enum member.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_get_solar(SWMM_Engine engine, int param, double* value);
/**
 * @brief Write one `[SOLAR_RADIATION]` parameter.
 *  @returns ::SWMM_ERR_BADPARAM if out of range. Setting LATITUDE or
 *           LONGITUDE also marks it as explicitly provided, which is what
 *           ::SWMM_HEAT_SW_COMPUTED checks for.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] param SWMM_HeatSolarParam selector.
 * @param[in] value New value; units and allowed range follow the selected enum member.
 * @note Requires finite values: timezone [-14,14] hours, latitude [-90,90],
 * longitude [-180,180], elevation [-500,9000] m, nonnegative optical depths,
 * water/ozone columns, and ground albedo [0,1]. Updates a shared site if present.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_set_solar(SWMM_Engine engine, int param, double value);

/**
 * @brief Have latitude AND longitude both been explicitly set?
 *  @details The precondition for ::SWMM_HEAT_SW_COMPUTED. A GUI should
 *           gate the COMPUTED radio button on this rather than discovering
 *           the refusal after the fact.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] sited Required result destination: 1 for a shared explicit site or both explicit
 * heat latitude/longitude, otherwise 0.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_solar_sited(SWMM_Engine engine, int* sited);

/* ----------------------------------------------------------------- cloud */

/**
 * @brief Is a `[CLOUD_COVER]` section in effect?
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] configured Required result destination: 1 for authored cloud cover, otherwise 0.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_cloud_configured(SWMM_Engine engine, int* configured);

/**
 * @brief Read one `[CLOUD_COVER]` parameter.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] param SWMM_HeatCloudParam selector.
 * @param[out] value Required destination; units and allowed range follow the selected enum member.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_get_cloud(SWMM_Engine engine, int param, double* value);
/**
 * @brief Write one `[CLOUD_COVER]` parameter. Writing any of them marks
 *         cloud cover as configured.
 *  @returns ::SWMM_ERR_BADPARAM if FRACTION is outside [0,1] (it is a
 *           fraction, not a percent) or a coefficient is negative.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] param SWMM_HeatCloudParam selector.
 * @param[in] value New value; units and allowed range follow the selected enum member.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_set_cloud(SWMM_Engine engine, int param, double value);

/**
 * @brief Bind a `[TIMESERIES]` (by name) as the cloud-fraction record.
 *  @returns ::SWMM_ERR_BADPARAM if no timeseries of that name exists.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] name Required nonempty existing model time-series name; resolved during this call.
 * @note No input string storage is retained.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_set_cloud_timeseries(SWMM_Engine engine, const char* name);

/**
 * @brief Name of the `[TIMESERIES]` bound as the cloud-fraction record, or
 *         "" when none is bound. NUL-terminated, truncated to `buflen`.
 *  @details The shortwave getter's sibling — see
 *           ::swmm_heat_get_shortwave_timeseries.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] buf Required writable string destination, safely truncated with a trailing NUL.
 * @param[in] buflen Positive capacity in bytes including the terminator.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_cloud_timeseries(SWMM_Engine engine, char* buf, int buflen);

/**
 * @brief Clear `[CLOUD_COVER]` entirely — back to clear sky.
 *  @details Restores the exact H3 longwave path, not an approximation of
 *           it: the cloud factor becomes a literal 1.0 that
 *           `atmosphericEmissivity` short-circuits.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @note Also clears current cloud and invalidates the resolved shortwave cache.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_clear_cloud(SWMM_Engine engine);

/**
 * @brief Cloud fraction in effect at the CURRENT step, [0,1]. Read-only.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] fraction Required result destination: last resolved dimensionless cloud fraction
 * in [0,1].
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_current_cloud(SWMM_Engine engine, double* fraction);

/* -------------------------------------------------------- [HEAT_SOURCES]
 *
 * The per-source inlet temperature table: a GLOBAL °C for each of the seven
 * water sources, plus NODE-scope overrides. Added for the G4g editor, which
 * cannot round-trip a table the API cannot read.
 *
 * Every refusal below mirrors `HeatComponent.cpp`'s parser exactly, so a
 * value the deck rejects is a value this API rejects, and the two cannot
 * disagree about what a model contains. Values are REFUSED, never clamped.
 */

/** @brief `[HEAT_SOURCES]` water sources. Mirrors `openswmm::HeatSource`. */
typedef enum SWMM_HeatSourceKind {
    SWMM_HEAT_SRC_RAINFALL = 0,        /**< washoff runoff. */
    SWMM_HEAT_SRC_DWF = 1,             /**< dry-weather flow. */
    SWMM_HEAT_SRC_GW = 2,              /**< groundwater. */
    SWMM_HEAT_SRC_RDII = 3,            /**< rainfall-derived infiltration. */
    SWMM_HEAT_SRC_EXTERNAL_INFLOW = 4, /**< `[INFLOWS]`. */
    SWMM_HEAT_SRC_IFACE = 5,           /**< interface file. */
    SWMM_HEAT_SRC_INITIAL_STATE = 6    /**< water present at t = 0. */
} SWMM_HeatSourceKind;

/**
 * @brief Number of sources the table carries (7). Never fails on a model
 *         with no heat configured — the table is a fixed enum extent, not a
 *         parsed list.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Required destination for the current fixed seven-source extent.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_source_count(SWMM_Engine engine, int* count);

/**
 * @brief Read one source's GLOBAL inlet temperature (°C).
 *  @returns ::SWMM_ERR_BADINDEX for a source outside the enum. A source with
 *           no row reads the 20 °C default — use
 *           ::swmm_heat_get_source_configured to tell the two apart.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] source SWMM_HeatSourceKind code, in [0,6].
 * @param[out] temp_c Required temperature destination in degrees Celsius.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_get_source_temp(SWMM_Engine engine, int source, double* temp_c);

/**
 * @brief Write one source's GLOBAL inlet temperature (°C).
 *  @returns ::SWMM_ERR_BADPARAM outside [-50, 100] — the parser's own range
 *           (`HeatComponent.cpp` `parse_celsius`). REFUSED, not clamped, and
 *           a refused write does not take effect. Marks the source
 *           configured.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] source SWMM_HeatSourceKind code, in [0,6].
 * @param[in] temp_c Finite source temperature in [-50,100] degrees Celsius.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_set_source_temp(SWMM_Engine engine, int source, double temp_c);

/**
 * @brief Did the model set this source explicitly, or is it taking the
 *         default? The editor needs the distinction to avoid writing rows a
 *         user never asked for.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] source SWMM_HeatSourceKind code, in [0,6].
 * @param[out] configured Required result, 1 for an authored global row or 0 for its default.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_get_source_configured(SWMM_Engine engine, int source,
                                                    int* configured);

/**
 * @brief Return a source to the 20 °C default and mark it unconfigured, so
 *         the writer emits no row for it. NODE overrides are untouched —
 *         they are separate rows and removing them silently would delete
 *         model the caller did not name.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] source SWMM_HeatSourceKind code, in [0,6].
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_clear_source_temp(SWMM_Engine engine, int source);

/**
 * @brief Number of NODE-scope override rows.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Required destination for the current override-row count.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_node_override_count(SWMM_Engine engine, int* count);

/**
 * @brief Read one NODE override by row index. Any out-pointer may be NULL.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based NODE override row index.
 * @param[out] source Optional source-kind destination.
 * @param[out] node Optional zero-based node-index destination.
 * @param[out] temp_c Optional temperature destination in degrees Celsius.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_get_node_override(SWMM_Engine engine, int index, int* source,
                                                int* node, double* temp_c);

/**
 * @brief Add or update the NODE override for (@p source, @p node).
 *  @returns ::SWMM_ERR_BADPARAM if @p source is not DWF or EXTERNAL_INFLOW —
 *           **the H1 scope rule, refused rather than deferred silently**, the
 *           same answer the deck gets; if @p node is out of range; or if
 *           @p temp_c is outside [-50, 100].
 *  @note An existing (source, node) pair is UPDATED rather than duplicated.
 *        The parser refuses a duplicate row because a deck cannot mean two
 *        temperatures at once; through an API, setting the same pair twice
 *        is an edit, and refusing it would make the editor unable to change
 *        a value it just wrote. Same invariant — one row per pair — reached
 *        the way each caller means it.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] source DWF or EXTERNAL_INFLOW source code.
 * @param[in] node Zero-based existing node index.
 * @param[in] temp_c Finite new value in [-50,100] in degrees Celsius.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_set_node_override(SWMM_Engine engine, int source, int node,
                                                double temp_c);

/**
 * @brief Remove one NODE override by row index. Later rows shift down, so a
 *         caller iterating by index must re-read the count after removing.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based override row to erase; later rows shift down.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_remove_node_override(SWMM_Engine engine, int index);

/**
 * @brief The temperature @p source water actually enters @p node at (°C) —
 *         the NODE override when one exists, else the GLOBAL value. This is
 *         `HeatConfigData::source_temp`, exposed so a caller reads the same
 *         resolution the engine uses rather than re-deriving the precedence
 *         and drifting from it.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] source SWMM_HeatSourceKind code [0,6].
 * @param[in] node Zero-based existing node index.
 * @param[out] temp_c Required resolved-temperature destination in degrees Celsius.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_get_effective_source_temp(SWMM_Engine engine, int source, int node,
                                                        double* temp_c);

/* =========================================================================
 * [SEDIMENT_EXCHANGE] and per-element attribute overrides
 * (closed C-API gaps 2026-10-11)
 *
 * Values are REFUSED, never clamped, by the same rules as the parser
 * (HeatComponent.cpp). Edits persist: the heat serializer writes the
 * configuration and the override rows back to the component file on save.
 * ========================================================================= */

/** @brief `[SEDIMENT_EXCHANGE]` GLOBAL parameters (bed zone beneath conduits). */
typedef enum SWMM_HeatSedimentParam {
    SWMM_HEAT_SED_THERMAL_DIFFUSIVITY = 0, /**< m²/s, > 0. */
    SWMM_HEAT_SED_SOLUTE_DIFFUSIVITY = 1,  /**< m²/s, >= 0 (0 = advection-only storage). */
    SWMM_HEAT_SED_BED_THICKNESS = 2,       /**< m, > 0. */
    SWMM_HEAT_SED_GROUND_DEPTH = 3,        /**< m, > 0. */
    SWMM_HEAT_SED_GROUND_TEMPERATURE = 4,  /**< °C, constant spelling; setting it
                                            *   replaces a ground timeseries. */
    SWMM_HEAT_SED_HYPORHEIC_VELOCITY = 5,  /**< m/s, >= 0. */
    SWMM_HEAT_SED_DENSITY = 6,             /**< kg/m³, > 0. */
    SWMM_HEAT_SED_SPECIFIC_HEAT = 7,       /**< J/kg/K, > 0. */
    SWMM_HEAT_SED_INITIAL_TEMPERATURE = 8, /**< °C; unset = start at ground temperature. */
    SWMM_HEAT_SED_MESH_POROSITY = 9,       /**< [0,1], new mesh bed only. */
    SWMM_HEAT_SED_GROUND_SOURCE =
        10 /**< 0 PRESCRIBED, 1 MESH_CONTACTS; routed channels only, pre-run. */
} SWMM_HeatSedimentParam;

/**
 * @brief Read a `[SEDIMENT_EXCHANGE]` GLOBAL parameter.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] param SWMM_HeatSedimentParam selector.
 * @param[out] value Required destination; units and allowed range follow the selected enum member.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_get_sediment(SWMM_Engine engine, int param, double* value);

/**
 * @brief Write a `[SEDIMENT_EXCHANGE]` GLOBAL parameter.
 *  @returns ::SWMM_ERR_BADPARAM for a value the parser refuses (not finite,
 *           not positive where required, negative diffusivity/velocity);
 *           ::SWMM_ERR_BADINDEX for an unknown @p param. Temperatures mark
 *           the value as stated so the writer keeps it.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] param SWMM_HeatSedimentParam selector.
 * @param[in] value New value; units and allowed range follow the selected enum member.
 * @note Ground-source changes require BUILDING/OPENED. Bed thickness with channel
 * contacts, and material/initial-state properties used by an enabled 2D bed model,
 * also require editable state; other scalar changes are accepted at runtime.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_set_sediment(SWMM_Engine engine, int param, double value);

/**
 * @brief Ground-temperature timeseries name; empty when the constant
 *         spelling is in force.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] buf Required writable string destination, safely truncated with a trailing NUL.
 * @param[in] buflen Positive capacity in bytes including the terminator.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_sediment_ground_timeseries(SWMM_Engine engine, char* buf,
                                                             int buflen);

/**
 * @brief Drive the ground temperature from a `[TIMESERIES]`; NULL or ""
 *         returns to the constant value.
 *  @returns ::SWMM_ERR_BADPARAM if @p name is not a timeseries of the model.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] name Optional copied series name; NULL or empty clears the binding.
 * @note No input string storage is retained.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_set_sediment_ground_timeseries(SWMM_Engine engine, const char* name);

/** @brief Attributes that take per-element overrides. Mirrors `openswmm::HeatAttr`. */
typedef enum SWMM_HeatAttr {
    SWMM_HEAT_ATTR_ALBEDO = 0,                  /**< `[RADIATIVE_FLUXES]`, [0,1]. */
    SWMM_HEAT_ATTR_SHADE_FACTOR = 1,            /**< [0,1]. */
    SWMM_HEAT_ATTR_SKY_VIEW = 2,                /**< [0,1]. */
    SWMM_HEAT_ATTR_EMISS_WATER = 3,             /**< [0,1]. */
    SWMM_HEAT_ATTR_EMISS_LANDCOVER = 4,         /**< [0,1]. */
    SWMM_HEAT_ATTR_LANDCOVER_TEMPERATURE = 5,   /**< °C. */
    SWMM_HEAT_ATTR_THERMAL_DIFFUSIVITY = 6,     /**< `[SEDIMENT_EXCHANGE]`, > 0. */
    SWMM_HEAT_ATTR_SOLUTE_DIFFUSIVITY = 7,      /**< >= 0. */
    SWMM_HEAT_ATTR_BED_THICKNESS = 8,           /**< > 0. */
    SWMM_HEAT_ATTR_GROUND_DEPTH = 9,            /**< > 0. */
    SWMM_HEAT_ATTR_GROUND_TEMPERATURE = 10,     /**< °C. */
    SWMM_HEAT_ATTR_HYPORHEIC_VELOCITY = 11,     /**< >= 0. */
    SWMM_HEAT_ATTR_SEDIMENT_DENSITY = 12,       /**< > 0. */
    SWMM_HEAT_ATTR_SEDIMENT_SPECIFIC_HEAT = 13, /**< > 0. */
    SWMM_HEAT_ATTR_MESH_POROSITY = 14,          /**< [0,1], new mesh bed only. */
    SWMM_HEAT_ATTR_GROUND_SOURCE = 15 /**< 0 PRESCRIBED, 1 MESH_CONTACTS; GLOBAL/TAG/LINK only. */
} SWMM_HeatAttr;

/** @brief Scope of a per-element override row. GLOBAL values are set with
 *         ::swmm_heat_set_radiative / ::swmm_heat_set_sediment instead. */
typedef enum SWMM_HeatOverrideScope {
    SWMM_HEAT_SCOPE_TAG = 1,     /**< every element carrying the tag. */
    SWMM_HEAT_SCOPE_NODE = 2,    /**< one node (radiative attributes only). */
    SWMM_HEAT_SCOPE_LINK = 3,    /**< one link. */
    SWMM_HEAT_SCOPE_CELL2D = 6,  /**< one mesh cell; name is its 1-based file ID. */
    SWMM_HEAT_SCOPE_MESH_TAG = 7 /**< mesh region tag, independent of network TAG. */
} SWMM_HeatOverrideScope;

/**
 * @brief Number of per-element override rows (radiative and sediment).
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Required destination for the current override-row count.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_element_override_count(SWMM_Engine engine, int* count);

/**
 * @brief Read one override row. Any out-pointer may be NULL.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based element-override row index.
 * @param[out] attr Optional SWMM_HeatAttr destination.
 * @param[out] scope Optional SWMM_HeatOverrideScope destination.
 * @param[out] name Optional name destination; copied only for positive name_len, safely
 * truncated with NUL.
 * @param[in] name_len Capacity in bytes; nonpositive capacity skips the string.
 * @param[out] value Optional authored value, with units from its attribute.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_heat_get_element_override(SWMM_Engine engine, int index, int* attr,
                                                   int* scope, char* name, int name_len,
                                                   double* value);

/**
 * @brief Add or update the override for (@p attr, @p scope, @p name).
 *  @returns ::SWMM_ERR_BADPARAM for a refused value, a sediment attribute at
 *           NODE scope (the bed exists beneath conduits only), or a name
 *           that resolves to no element/tag (the configuration is then left
 *           unchanged); ::SWMM_ERR_LIFECYCLE once the run is initialized.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] attr SWMM_HeatAttr selector.
 * @param[in] scope TAG, NODE, LINK, CELL2D or MESH_TAG; GROUND_SOURCE permits TAG/LINK only.
 * @param[in] name Required nonempty copied identifier/tag; CELL2D is a one-based native cell ID.
 * @param[in] value New value with the selected attribute's units/range.
 * @note Failed name/override resolution restores the previous configuration.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_heat_set_element_override(SWMM_Engine engine, int attr, int scope,
                                                   const char* name, double value);

/**
 * @brief Remove one override row by index.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based override row to erase; later rows shift down.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_heat_remove_element_override(SWMM_Engine engine, int index);

/**
 * @brief Read the effective property for a named NODE, LINK or CELL2D. Source is
 *  GLOBAL=0, TAG=1, NODE=2, LINK=3, air temperature=4, ground series=5,
 *  CELL2D=6 or MESH_TAG=7. CELL2D names use one-based native cell IDs.
 *  Dynamic temperatures return NaN; source_name identifies the tag/series.
 *  This reports configuration precedence, independent of process activation.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] attr SWMM_HeatAttr selector; sediment attributes cannot use NODE scope.
 * @param[in] scope NODE, LINK or CELL2D.
 * @param[in] name Required existing element identifier; CELL2D uses a decimal one-based ID.
 * @param[out] value Required effective-value destination; units follow the attribute.
 * @param[out] source Required origin code destination from the list above.
 * @param[out] source_name Optional source description, copied only when capacity is positive.
 * @param[in] source_name_len Source-name byte capacity; positive capacity safely truncates with
 * NUL.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value. Invalid selector or row indices return SWMM_ERR_BADINDEX. CELL2D
 * reads return SWMM_ERR_PLUGIN in builds without 2D.
 */
SWMM_ENGINE_API int swmm_heat_get_effective_element(SWMM_Engine engine, int attr, int scope,
                                                    const char* name, double* value, int* source,
                                                    char* source_name, int source_name_len);

/** Requested configuration and measured 2D runtime activation are distinct.
 * Initialize struct_size before calling. Values apply to the mesh domain.
 * New coupled physics remains experimental until its restart/owner matrix is
 * qualified; the reason field states this explicitly. */
typedef struct SWMM_Heat2DStatus {
    unsigned int struct_size;
    int requested, initialized, surface_active, radiative_active, bed_active;
    int bed_model; /**< 0 NONE, 1 LOCAL_STORAGE, 2 GROUND_COLUMN. */
    int experimental;
    double water_heat_capacity; /**< J/(m3 K), shared by transport/heat receipts. */
    char reason[256];
} SWMM_Heat2DStatus;
/**
 * @brief Read requested and initialized coupled surface-heat status.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in,out] status Required caller-owned record with struct_size at least
 * sizeof(SWMM_Heat2DStatus).
 * @note Available before initialization; reports requested and measured state separately.
 * Builds without 2D return success with inactive fields and an explanatory reason.
 * Invalid handle/record leaves the output unchanged.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_2d_status(SWMM_Engine engine, SWMM_Heat2DStatus* status);

/** SI runtime fields, cumulative since initialization. Positive J enters the
 * named store; WATER_TO_BED is positive water -> bed. Cell indices are zero
 * based at this bulk API. Bed-only fields are unavailable without a bed. */
typedef enum SWMM_Heat2DValue {
    SWMM_HEAT_2D_WATER_TEMPERATURE = 0, /**< degC; dry reporting policy only. */
    SWMM_HEAT_2D_BED_TEMPERATURE = 1,   /**< degC. */
    SWMM_HEAT_2D_BED_ENERGY = 2,        /**< J relative to 0 degC. */
    SWMM_HEAT_2D_BED_CAPACITY = 3,      /**< J/K. */
    SWMM_HEAT_2D_ATMOSPHERIC_ENERGY = 4,
    SWMM_HEAT_2D_RADIATIVE_ENERGY = 5,
    SWMM_HEAT_2D_SENSIBLE_ENERGY = 6,
    SWMM_HEAT_2D_LATENT_ENERGY = 7,
    SWMM_HEAT_2D_DEEP_ENERGY = 8,
    SWMM_HEAT_2D_WATER_TO_BED_ENERGY = 9,
    SWMM_HEAT_2D_WATER_THERMAL_INCREMENT = 10, /**< J; excludes water advection. */
    SWMM_HEAT_2D_ACCEPTED_EVAPORATION = 11,    /**< m3. */
    SWMM_HEAT_2D_THERMAL_RESIDUAL = 12 /**< J; bed change + water increment - external heat. */
} SWMM_Heat2DValue;
/**
 * @brief Read a contiguous range of accepted surface-heat runtime values.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] kind SWMM_Heat2DValue selector; units/signs follow the enum member.
 * @param[in] first_cell Nonnegative zero-based starting cell index.
 * @param[in] count Nonnegative number of consecutive values to copy; span must fit the model.
 * @param[out] values Caller-owned count-double destination; may be NULL only when count == 0.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for a null engine; SWMM_ERR_BADPARAM for
 * invalid selector/count/storage; SWMM_ERR_BADINDEX for an out-of-range span;
 * SWMM_ERR_LIFECYCLE for unavailable runtime; or SWMM_ERR_PLUGIN without 2D.
 * @note Accepted spans are validated before writing. Runtime reads remain available
 * after ENDED/REPORTED when the corresponding stored thermal owner exists.
 */
SWMM_ENGINE_API int swmm_heat_get_2d_values(SWMM_Engine engine, int kind, int first_cell, int count,
                                            double* values);
/**
 * @brief Exact authoring state for the new channel ground selector. Clear restores
 * the absent/default PRESCRIBED choice; both operations are pre-run only.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] configured Required result destination: 1 for an explicit channel ground-source
 * selection, otherwise 0.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_ground_source_configured(SWMM_Engine engine, int* configured);
/**
 * @brief Restore the absent/default prescribed channel ground-source choice.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @note Restores absent/default PRESCRIBED selection and re-resolves overrides.
 * Refused resolution restores the prior config; allocation failure returns
 * SWMM_ERR_NOMEM and other exceptions return SWMM_ERR_INTERNAL.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_heat_clear_ground_source(SWMM_Engine engine);

/** Soil state is canonical solid-plus-water energy, not a second copy of the
 * transported water-temperature row. Initialize struct_size before reading. */
typedef struct SWMM_GroundHeatStatus {
    unsigned int struct_size;
    int requested, initialized, aquifer_cells, standalone_cells, thermal_layers;
    int conduction_active, lateral_active, experimental;
    double water_heat_capacity; /**< J/(m3 K). */
    char reason[256];
} SWMM_GroundHeatStatus;
/**
 * @brief Read canonical ground thermal ownership and runtime status.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in,out] status Required caller-owned record with struct_size at least
 * sizeof(SWMM_GroundHeatStatus).
 * @note Available before initialization; reports requested and measured state separately.
 * Builds without 2D return success with inactive fields and an explanatory reason.
 * Invalid handle/record leaves the output unchanged.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_ground_status(SWMM_Engine engine, SWMM_GroundHeatStatus* status);
/** Per-cell canonical fields. Positive receipts enter soil. Unmodeled cells
 * return NaN; an entirely inactive model returns SWMM_ERR_LIFECYCLE. All
 * energies are J relative to 0 degC, accumulated since initialization. */
typedef enum SWMM_GroundHeatValue {
    SWMM_GROUND_HEAT_ENERGY = 0,
    SWMM_GROUND_HEAT_CAPACITY = 1,    /**< J/K. */
    SWMM_GROUND_HEAT_TEMPERATURE = 2, /**< capacity-weighted degC. */
    SWMM_GROUND_HEAT_INITIAL_ENERGY = 3,
    SWMM_GROUND_HEAT_ADVECTIVE_ENERGY = 4,
    SWMM_GROUND_HEAT_SOURCE_ENERGY = 5,
    SWMM_GROUND_HEAT_SURFACE_ENERGY = 6,
    SWMM_GROUND_HEAT_DEEP_ENERGY = 7,
    SWMM_GROUND_HEAT_LATENT_ENERGY = 8,
    SWMM_GROUND_HEAT_LATERAL_ENERGY = 9,
    SWMM_GROUND_HEAT_WATER_CONTACT_ENERGY = 10,
    SWMM_GROUND_HEAT_RESIDUAL = 11,
    SWMM_GROUND_HEAT_NORMALIZED_RESIDUAL = 12, /**< dimensionless, absolute-energy scaled. */
    SWMM_GROUND_HEAT_PENDING_LATERAL =
        13, /**< J awaiting this cell's LTS gather; add to stored energy for domain totals. */
    SWMM_GROUND_HEAT_CHANNEL_CONTACT = 14, /**< J, routed channel water -> soil. */
    SWMM_GROUND_HEAT_LID_CONTACT = 15      /**< J, mesh-connected LID -> soil. */
} SWMM_GroundHeatValue;
/**
 * @brief Read a contiguous cell range of canonical soil thermal values.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] kind SWMM_GroundHeatValue selector; units/signs follow the enum member.
 * @param[in] first_cell Nonnegative zero-based starting cell index.
 * @param[in] count Nonnegative number of consecutive values to copy; span must fit the model.
 * @param[out] values Caller-owned count-double destination; may be NULL only when count == 0.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for a null engine; SWMM_ERR_BADPARAM for
 * invalid selector/count/storage; SWMM_ERR_BADINDEX for an out-of-range span;
 * SWMM_ERR_LIFECYCLE for unavailable runtime; or SWMM_ERR_PLUGIN without 2D.
 * @note Accepted spans are validated before writing. Runtime reads remain available
 * after ENDED/REPORTED when the corresponding stored thermal owner exists.
 */
SWMM_ENGINE_API int swmm_heat_get_ground_values(SWMM_Engine engine, int kind, int first_cell,
                                                int count, double* values);
/** Fixed thermal depth grid, independent of hydraulic sigma layers. */
typedef enum SWMM_GroundHeatLayerValue {
    SWMM_GROUND_LAYER_TOP = 0,            /**< depth below terrain, m. */
    SWMM_GROUND_LAYER_BOTTOM = 1,         /**< depth below terrain, m. */
    SWMM_GROUND_LAYER_TEMPERATURE = 2,    /**< degC. */
    SWMM_GROUND_LAYER_ENERGY = 3,         /**< J relative to 0 degC. */
    SWMM_GROUND_LAYER_CAPACITY = 4,       /**< J/K. */
    SWMM_GROUND_LAYER_MOISTURE = 5,       /**< m3 water per m3 bulk soil. */
    SWMM_GROUND_LAYER_CONDUCTIVITY = 6,   /**< W/(m K). */
    SWMM_GROUND_LAYER_SATURATED_WATER = 7 /**< m3. */
} SWMM_GroundHeatLayerValue;
/**
 * @brief Zero-based cell/layer ranges. Invalid ranges leave all output untouched.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] cell Zero-based mesh cell with an initialized thermal column.
 * @param[in] kind SWMM_GroundHeatLayerValue selector; units follow the enum.
 * @param[in] first_layer Nonnegative zero-based starting thermal layer, surface first.
 * @param[in] count Nonnegative layer count; the complete span must fit the column.
 * @param[out] values Caller-owned count-double destination; NULL only when count == 0.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for a null engine; SWMM_ERR_BADPARAM for
 * invalid selector/count/storage; SWMM_ERR_BADINDEX for an out-of-range span;
 * SWMM_ERR_LIFECYCLE for unavailable runtime; or SWMM_ERR_PLUGIN without 2D.
 * @note Accepted spans are validated before writing. Runtime reads remain available
 * after ENDED/REPORTED when the corresponding stored thermal owner exists.
 */
SWMM_ENGINE_API int swmm_heat_get_ground_layers(SWMM_Engine engine, int cell, int kind,
                                                int first_layer, int count, double* values);

/** Routed-channel contact configuration and initialized mapping counts.
 * Initialize struct_size. Counts describe the measured topology, even when
 * a live parent switch pauses its exchange. All new physics is experimental. */
typedef struct SWMM_ChannelGroundStatus {
    unsigned int struct_size;
    int requested, initialized, exchange_active, mapped_links, contact_cells, contact_intervals,
        experimental;
    char reason[256];
} SWMM_ChannelGroundStatus;
/**
 * @brief Read requested and measured channel-to-ground contact status.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in,out] status Required caller-owned record with struct_size at least
 * sizeof(SWMM_ChannelGroundStatus).
 * @note Available before initialization; reports requested and measured state separately.
 * Builds without 2D return success with inactive fields and an explanatory reason.
 * Invalid handle/record leaves the output unchanged.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE for a null engine, or SWMM_ERR_BADPARAM for invalid
 * required storage/value.
 */
SWMM_ENGINE_API int swmm_heat_get_channel_ground_status(SWMM_Engine engine,
                                                        SWMM_ChannelGroundStatus* status);
/** Zero-based link ranges; unmapped links return NaN. Bed temperature is a
 * capacity-weighted view of the actual soil under the bed, available dry or
 * wet. It adds no reservoir and must not be counted as extra soil energy. */
typedef enum SWMM_ChannelGroundValue {
    SWMM_CHANNEL_GROUND_BED_TEMPERATURE = 0,    /**< degC. */
    SWMM_CHANNEL_GROUND_ATMOSPHERIC_ENERGY = 1, /**< J, atmosphere -> channel. */
    SWMM_CHANNEL_GROUND_TO_SOIL_ENERGY = 2,     /**< J, channel -> soil. */
    SWMM_CHANNEL_GROUND_WATER_INCREMENT = 3, /**< J, channel thermal change excluding advection. */
    SWMM_CHANNEL_GROUND_INTERVAL_COUNT = 4   /**< dimensionless, mapped chainage/layer intervals. */
} SWMM_ChannelGroundValue;
/**
 * @brief Read a contiguous link range of accepted channel-ground values.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] kind SWMM_ChannelGroundValue selector; units/signs follow the enum member.
 * @param[in] first_link Nonnegative zero-based starting link index.
 * @param[in] count Nonnegative number of consecutive values to copy; span must fit the model.
 * @param[out] values Caller-owned count-double destination; may be NULL only when count == 0.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for a null engine; SWMM_ERR_BADPARAM for
 * invalid selector/count/storage; SWMM_ERR_BADINDEX for an out-of-range span;
 * SWMM_ERR_LIFECYCLE for unavailable runtime; or SWMM_ERR_PLUGIN without 2D.
 * @note Accepted spans are validated before writing. Runtime reads remain available
 * after ENDED/REPORTED when the corresponding stored thermal owner exists.
 */
SWMM_ENGINE_API int swmm_heat_get_channel_ground_values(SWMM_Engine engine, int kind,
                                                        int first_link, int count, double* values);
/** One initialized contact interval. Fractions refer to the full polyline;
 * share divides legitimate common-edge ownership, never an entire reach.
 * Invert is its representative elevation in metres SI. */
typedef struct SWMM_ChannelGroundContact {
    unsigned int struct_size;
    int link, cell, layer; /**< zero-based indices. */
    double begin_fraction, end_fraction, share, invert_m;
} SWMM_ChannelGroundContact;
/**
 * @brief Read one initialized channel-to-ground contact interval.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based initialized contact-interval index.
 * @param[in,out] contact Required record with struct_size at least
 * sizeof(SWMM_ChannelGroundContact).
 * @note Output remains unchanged on failure; reads accepted runtime contact geometry.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for a null engine; SWMM_ERR_BADPARAM for
 * invalid selector/count/storage; SWMM_ERR_BADINDEX for an out-of-range span;
 * SWMM_ERR_LIFECYCLE for unavailable runtime; or SWMM_ERR_PLUGIN without 2D.
 * @note Accepted spans are validated before writing. Runtime reads remain available
 * after ENDED/REPORTED when the corresponding stored thermal owner exists.
 */
SWMM_ENGINE_API int swmm_heat_get_channel_ground_contact(SWMM_Engine engine, int index,
                                                         SWMM_ChannelGroundContact* contact);
/** Pre-run, read-only XY coverage preview for one routed channel. Contact
 * intervals use whole-polyline fractions, have layer=-1, and may be returned
 * even when valid is false so uncovered geometry remains visible. A valid
 * result means the complete polyline lies in the mesh; soil-profile, invert,
 * bed-thickness and thermal-owner checks still run at initialization.
 * Initialize struct_size and pass contacts=null/capacity=0 to query the
 * required interval count. If capacity is too small, status and required are
 * returned but the contact array is left untouched. Only OPENED/BUILDING
 * engines with a parsed mesh are accepted. */
typedef struct SWMM_ChannelGroundPreview {
    unsigned int struct_size;
    int link, valid, contact_cells, contact_intervals;
    double length_m, covered_length_m, uncovered_length_m;
    char reason[256];
} SWMM_ChannelGroundPreview;
/**
 * @brief Preview complete channel XY coverage on a private mesh copy.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] link Zero-based existing routed-channel link index.
 * @param[in,out] out Required summary with struct_size at least sizeof(SWMM_ChannelGroundPreview).
 * @param[out] contacts Caller-owned array of capacity contact records; NULL only when capacity
 * == 0.
 * @param[in] capacity Nonnegative contact-record capacity; zero permits a size query.
 * @param[out] required Required destination for the complete mapping's interval count.
 * @note Requires BUILDING/OPENED and works on a private geometry copy. Missing mesh
 * or ineligible geometry produces SWMM_OK with out->valid == 0 and a reason.
 * No hydraulic or thermal model state is initialized. Cancellation leaves all
 * outputs untouched; a short nonpaged buffer publishes summary/required only.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for a null engine; SWMM_ERR_BADPARAM for
 * invalid selector/count/storage; SWMM_ERR_BADINDEX for an out-of-range span;
 * SWMM_ERR_LIFECYCLE for unavailable runtime; or SWMM_ERR_PLUGIN without 2D.
 * @note Accepted spans are validated before writing. Runtime reads remain available
 * after ENDED/REPORTED when the corresponding stored thermal owner exists. Progress variants
 * additionally return SWMM_ERR_CANCELLED.
 */
SWMM_ENGINE_API int swmm_heat_preview_channel_ground_map(SWMM_Engine engine, int link,
                                                         SWMM_ChannelGroundPreview* out,
                                                         SWMM_ChannelGroundContact* contacts,
                                                         int capacity, int* required);
enum { SWMM_HEAT_PREVIEW_STAGE_MESH = 1, SWMM_HEAT_PREVIEW_STAGE_MAPPING = 2 };
/**
 * @brief Cancellable variant of the read-only channel-contact preview. The callback
 * may be invoked during mesh indexing and channel mapping. If it returns
 * nonzero, the function returns SWMM_ERR_CANCELLED and leaves `out`,
 * `contacts`, and `required` unchanged.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] link Zero-based existing routed-channel link index.
 * @param[in,out] out Required summary with struct_size at least sizeof(SWMM_ChannelGroundPreview).
 * @param[out] contacts Caller-owned array of capacity contact records; NULL only when capacity
 * == 0.
 * @param[in] capacity Nonnegative contact-record capacity; zero permits a size query.
 * @param[out] required Required destination for the complete mapping's interval count.
 * @param[in] callback Optional synchronous cancellation/progress callback; receives engine,
 * stage SWMM_HEAT_PREVIEW_STAGE_*, percentage [0,100], and user_data. Nonzero cancels.
 * @param[in,out] user_data Opaque caller-owned callback context; may be NULL and is not retained.
 * @note Requires BUILDING/OPENED and works on a private geometry copy. Missing mesh
 * or ineligible geometry produces SWMM_OK with out->valid == 0 and a reason.
 * No hydraulic or thermal model state is initialized. Cancellation leaves all
 * outputs untouched; a short nonpaged buffer publishes summary/required only.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for a null engine; SWMM_ERR_BADPARAM for
 * invalid selector/count/storage; SWMM_ERR_BADINDEX for an out-of-range span;
 * SWMM_ERR_LIFECYCLE for unavailable runtime; or SWMM_ERR_PLUGIN without 2D.
 * @note Accepted spans are validated before writing. Runtime reads remain available
 * after ENDED/REPORTED when the corresponding stored thermal owner exists. Progress variants
 * additionally return SWMM_ERR_CANCELLED.
 */
SWMM_ENGINE_API int swmm_heat_preview_channel_ground_map_with_progress(
    SWMM_Engine engine, int link, SWMM_ChannelGroundPreview* out,
    SWMM_ChannelGroundContact* contacts, int capacity, int* required, SWMM_CancelCallback callback,
    void* user_data);
/**
 * @brief Bounded, cancellable contact page. `first` is a zero-based interval offset;
 * at most `capacity` rows are copied while `required` reports the total count.
 * The summary counts describe the complete mapping. On cancellation, all
 * output arguments remain unchanged.
 *
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] link Zero-based existing routed-channel link index.
 * @param[in,out] out Required summary with struct_size at least sizeof(SWMM_ChannelGroundPreview).
 * @param[out] contacts Caller-owned array of capacity contact records; NULL only when capacity
 * == 0.
 * @param[in] capacity Nonnegative contact-record capacity; zero permits a size query.
 * @param[out] required Required destination for the complete mapping's interval count.
 * @param[in] first Zero-based interval offset, at most the complete interval count.
 * @param[in] callback Optional synchronous cancellation/progress callback; receives engine,
 * stage SWMM_HEAT_PREVIEW_STAGE_*, percentage [0,100], and user_data. Nonzero cancels.
 * @param[in,out] user_data Opaque caller-owned callback context; may be NULL and is not retained.
 * @note Requires BUILDING/OPENED and works on a private geometry copy. Missing mesh
 * or ineligible geometry produces SWMM_OK with out->valid == 0 and a reason.
 * No hydraulic or thermal model state is initialized. Cancellation leaves all
 * outputs untouched; a short nonpaged buffer publishes summary/required only.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for a null engine; SWMM_ERR_BADPARAM for
 * invalid selector/count/storage; SWMM_ERR_BADINDEX for an out-of-range span;
 * SWMM_ERR_LIFECYCLE for unavailable runtime; or SWMM_ERR_PLUGIN without 2D.
 * @note Accepted spans are validated before writing. Runtime reads remain available
 * after ENDED/REPORTED when the corresponding stored thermal owner exists. Progress variants
 * additionally return SWMM_ERR_CANCELLED.
 */
SWMM_ENGINE_API int swmm_heat_preview_channel_ground_map_page(
    SWMM_Engine engine, int link, SWMM_ChannelGroundPreview* out, int first,
    SWMM_ChannelGroundContact* contacts, int capacity, int* required, SWMM_CancelCallback callback,
    void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_HEAT_H */
