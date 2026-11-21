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
 * @file      openswmm_forcing.h
 * @brief     Runtime forcing API — inject fluxes with mass-balance tracking.
 *
 * @details Provides per-element runtime forcing of lateral inflows, head
 *          boundaries, rainfall, evaporation, link settings, and quality mass
 *          fluxes. Each forcing specifies:
 *
 *            - **mode**: NONE (disabled), OVERRIDE (replace computed value),
 *                        ADD (add to computed value)
 *            - **persistence**: RESET (auto-clear after each timestep),
 *                               PERSIST (keep until explicitly cleared)
 *
 *          Forced volumes are tracked in the mass balance and reported
 *          through the applicable routing/quality forcing ledgers.
 *
 *          Ordinary flow/quality/weather registrations require RUNNING. Clear,
 *          climate getters/DRY_ONLY and per-element climate APIs document their
 *          separate lifecycle behavior below.
 *
 * @section forcing_lifecycle Lifecycle
 *
 * @code
 * swmm_engine_start(e, 1);
 * while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0) {
 *     // Set forcings for this step (or let PERSIST carry forward)
 *     swmm_forcing_node_lat_inflow(e, j1, 10.0,
 *         SWMM_FORCING_ADD, SWMM_FORCING_PERSIST);
 *
 *     // ... step runs, forcing is applied, mass balance tracks it ...
 *     // RESET forcings auto-clear; PERSIST forcings remain
 * }
 * swmm_engine_end(e);
 * @endcode
 *
 * @note All value parameters are in the simulation's internal units (ft, sec,
 *       CFS) unless otherwise noted. Rainfall values are in user display units
 *       (in/hr for US, mm/hr for SI) — the engine converts internally.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_forcing Runtime Forcing API
 * @ingroup  engine_api
 */

#ifndef OPENSWMM_FORCING_H
#define OPENSWMM_FORCING_H

#include "openswmm_callbacks.h"

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
 * Enumerations
 * ========================================================================= */

/** @brief How the forcing value is applied relative to the computed value. */
typedef enum SWMM_ForcingMode {
    SWMM_FORCING_NONE = 0,     /**< No forcing — use model-computed value. */
    SWMM_FORCING_OVERRIDE = 1, /**< Replace computed value with user value. */
    SWMM_FORCING_ADD = 2       /**< Add user value to computed value. */
} SWMM_ForcingMode;

/** @brief Whether the forcing persists across timesteps. */
typedef enum SWMM_ForcingPersist {
    SWMM_FORCING_RESET = 0,  /**< Auto-clear after each timestep. */
    SWMM_FORCING_PERSIST = 1 /**< Keep until explicitly cleared. */
} SWMM_ForcingPersist;

/** @brief Forcing channel identifier (for targeted clear). */
typedef enum SWMM_ForcingType {
    SWMM_FORCE_NODE_LAT_INFLOW = 0,
    SWMM_FORCE_NODE_HEAD_BOUNDARY = 1,
    SWMM_FORCE_NODE_QUALITY = 2,
    SWMM_FORCE_LINK_FLOW = 3,
    SWMM_FORCE_LINK_SETTING = 4,
    SWMM_FORCE_SUBCATCH_RAINFALL = 5,
    SWMM_FORCE_SUBCATCH_EVAP = 6,
    SWMM_FORCE_GAGE_RAINFALL = 7,
    SWMM_FORCE_CLIMATE_TEMPERATURE = 8, /**< System-wide; idx ignored. */
    SWMM_FORCE_CLIMATE_WIND = 9,        /**< System-wide; idx ignored. */
    SWMM_FORCE_SUBCATCH_SNOWFALL = 10,
    SWMM_FORCE_CLIMATE_EVAP = 11, /**< System-wide; idx ignored. */
    SWMM_FORCE_LINK_QUALITY = 12,
    /* PE4 — per-ELEMENT climate. `idx` is the element index and the kind is
       given separately; see swmm_forcing_element_climate. */
    SWMM_FORCE_ELEM_AIR_TEMPERATURE = 13,
    SWMM_FORCE_ELEM_HUMIDITY = 14,
    SWMM_FORCE_ELEM_WIND_SPEED = 15,
    SWMM_FORCE_ELEM_SHORTWAVE = 16,
    SWMM_FORCE_LINK_SEEPAGE = 17 /**< G-X4: conduit ⇄ groundwater exchange. */
} SWMM_ForcingType;

/** @brief Which element a per-element climate forcing targets (PE4). */
typedef enum SWMM_HeatElemKind {
    SWMM_HEAT_ELEM_NODE = 0,
    SWMM_HEAT_ELEM_LINK = 1
} SWMM_HeatElemKind;

/* =========================================================================
 * Node forcing
 * ========================================================================= */

/**
 * @brief Register lateral user-flow overlay for the next step(s).
 *
 * Overlays the persistent user_lat_flow contribution: OVERRIDE replaces that runtime contribution
 * and ADD adds to it; other authored inflow pathways remain separately assembled. An ADD
 * prescription is reapplied per step without compounding its stored baseline.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] idx Zero-based node index.
 * @param[in] value internal ft3/s (CFS) for every project, including SI.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_node_lat_inflow(SWMM_Engine engine, int idx, double value,
                                                 int mode, int persist);

/**
 * @brief Register outfall head-boundary overlay for the next step(s).
 *
 * Requires an OUTFALL. Only OVERRIDE is consumed by outfall stage evaluation; NONE clears and
 * restores authored behavior. ADD passes this setter validation but is not applied by the stage
 * consumer. Does not rewrite the authored outfall type or parameter.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] idx Zero-based node index.
 * @param[in] value internal datum feet (ft) in every project.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_node_head_boundary(SWMM_Engine engine, int idx, double value,
                                                    int mode, int persist);

/**
 * @brief Register direct pollutant concentration forcing on a node.
 *
 * Current ADD implementation increments concentration directly and books value*dt in the
 * forcing-quality ledger; it does not normalize a supplied amount rate by water volume. OVERRIDE is
 * a direct published-state assignment rather than a conservative mass injection. For a conservative
 * persistent node amount-rate injection, use swmm_node_set_quality_mass_flux().
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] node_idx Zero-based existing node index.
 * @param[in] pollutant_idx Zero-based existing pollutant index.
 * @param[in] mass_rate Raw value interpreted by the current application path: OVERRIDE sets
 *     concentration in pollutant concentration units; ADD directly adds this value to concentration
 *     each step.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_node_quality(SWMM_Engine engine, int node_idx, int pollutant_idx,
                                              double mass_rate, int mode, int persist);

/**
 * @brief Register reserved-species node water temperature forcing.
 *
 * Requires HEAT_TRANSPORT enabled, otherwise BADPARAM. OVERRIDE directly assigns the published
 * state and is not a conservative injection. ADD enters the loader accumulator so it mixes with
 * held water. Age hours are converted to seconds internally; temperature uses degrees C in all
 * project unit systems.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] node_idx Zero-based existing node with initialized reserved-species forcing storage.
 * @param[in] value OVERRIDE: degrees C; ADD: degrees C*ft3/s temperature-volume rate. The setter
 *     does not validate finiteness or range.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_node_temperature(SWMM_Engine engine, int node_idx, double value,
                                                  int mode, int persist);

/**
 * @brief Register reserved-species node water age forcing.
 *
 * Requires WATER_AGE enabled, otherwise BADPARAM. OVERRIDE directly assigns the published state and
 * is not a conservative injection. ADD enters the loader accumulator so it mixes with held water.
 * Age hours are converted to seconds internally; temperature uses degrees C in all project unit
 * systems.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] node_idx Zero-based existing node with initialized reserved-species forcing storage.
 * @param[in] value OVERRIDE: hours; ADD: hours*ft3/s age-volume rate. The setter does not validate
 *     finiteness or range.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_node_age(SWMM_Engine engine, int node_idx, double value, int mode,
                                          int persist);

/* =========================================================================
 * Link forcing
 * ========================================================================= */

/**
 * @brief Register link flow-state forcing for the next step(s).
 *
 * Applied to the current link-flow state before physics. It is not a persistent hydraulic boundary
 * condition or iterative solver constraint; later routing can change flow. ADD increments the
 * current state at each application.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] idx Zero-based link index.
 * @param[in] value internal ft3/s (CFS) in every project.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_link_flow(SWMM_Engine engine, int idx, double value, int mode,
                                           int persist);

/**
 * @brief Register link setting forcing for the next step(s).
 *
 * Application sets both current and target setting for OVERRIDE; ADD increments current setting
 * then sets the target to it. The setter does not clamp/range-check values or remove control rules.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] idx Zero-based link index.
 * @param[in] value dimensionless opening/speed (conventionally 0..1).
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_link_setting(SWMM_Engine engine, int idx, double value, int mode,
                                              int persist);

/**
 * @brief Register conduit groundwater-exchange forcing for the next step(s).
 *
 * OVERRIDE replaces native seepage and ADD augments it; negative exchange is a gaining reach booked
 * as SWMM_ROUTING_LINK_GW_INFLOW. Non-conduit links accept the stored prescription but have no
 * seepage consumer. With internal TWO_WAY/AUTO aquifer exchange active, the aquifer is also
 * debited/credited; external-only coupling must leave the internal aquifer out of the reach.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] idx Zero-based link index.
 * @param[in] value internal ft3/s (CFS), positive OUT of the conduit in every project.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_link_seepage(SWMM_Engine engine, int idx, double value, int mode,
                                              int persist);

/**
 * @brief Register direct pollutant concentration forcing on a link.
 *
 * Current ADD implementation increments concentration directly and books value*dt in the
 * forcing-quality ledger; it does not normalize a supplied amount rate by water volume. OVERRIDE is
 * a direct published-state assignment rather than a conservative mass injection. Link application
 * clamps the resulting concentration to nonnegative after quality routing.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] link_idx Zero-based existing link index.
 * @param[in] pollutant_idx Zero-based existing pollutant index.
 * @param[in] value Raw value interpreted by the current application path: OVERRIDE sets
 *     concentration in pollutant concentration units; ADD directly adds this value to concentration
 *     each step.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_link_quality(SWMM_Engine engine, int link_idx, int pollutant_idx,
                                              double value, int mode, int persist);

/* =========================================================================
 * Subcatchment forcing
 * ========================================================================= */

/**
 * @brief Register subcatchment rainfall forcing for the next step(s).
 *
 * Consumed in runoff rainfall assembly, bypassing/augmenting gage lookup according to mode. The
 * setter stores the display rate and does not independently reject negative or nonfinite values.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] value project rainfall intensity (in/hr US, mm/hr SI).
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_subcatch_rainfall(SWMM_Engine engine, int idx, double value,
                                                   int mode, int persist);

/**
 * @brief Register subcatchment potential-evaporation forcing for the next step(s).
 *
 * Converted to internal ft/s at registration. Overrides/augments climate PET for
 * surface/LID/upper-zone groundwater evaporation; OVERRIDE bypasses DRY_ONLY and monthly
 * adjustments. Actual loss is capped by available water. This setter does not reject negative or
 * nonfinite values.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] value project evaporation rate (in/day US, mm/day SI).
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_subcatch_evap(SWMM_Engine engine, int idx, double value, int mode,
                                               int persist);

/**
 * @brief Register subcatchment snowfall forcing for the next step(s).
 *
 * Converted to internal ft/s. OVERRIDE refuses values<0; ADD may be signed. Meaningful for an
 * assigned snow pack, replacing/augmenting temperature-split snowfall for accumulation, plowing and
 * melt. No independent finite-value validation is applied.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] value snow-water-equivalent intensity (in/hr US, mm/hr SI).
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_subcatch_snowfall(SWMM_Engine engine, int idx, double value,
                                                   int mode, int persist);

/**
 * @brief Read the current broadcast evaporation rate.
 *
 * Reads current stored climate state; before climate initialization/update this need not represent
 * a sampled source. Evaporation is the broadcast state, excluding a per-subcatchment PET override.
 *
 * @param[in] engine Non-null live engine; no RUNNING guard is applied to this getter.
 * @param[out] value Optional destination in in/day US, mm/day SI.
 * @return SWMM_OK, including NULL value; SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_evap_rate(SWMM_Engine engine, double* value);

/* =========================================================================
 * Climate forcing (system-wide)
 * ========================================================================= */

/**
 * @brief Register system air temperature forcing.
 *
 * OVERRIDE replaces the climate source before derived atmospheric/Hargreaves quantities and
 * bypasses monthly adjustment. ADD is a temperature DIFFERENCE; SI conversion scales by 1.8 without
 * a 32-degree offset.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] value degrees F US, degrees C SI.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_climate_temperature(SWMM_Engine engine, double value, int mode,
                                                     int persist);

/**
 * @brief Read the current air temperature.
 *
 * Reads current stored climate state; before climate initialization/update this need not represent
 * a sampled source. Evaporation is the broadcast state, excluding a per-subcatchment PET override.
 *
 * @param[in] engine Non-null live engine; no RUNNING guard is applied to this getter.
 * @param[out] value Optional destination in degrees F US, degrees C SI.
 * @return SWMM_OK, including NULL value; SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_temperature(SWMM_Engine engine, double* value);

/**
 * @brief Register system wind speed forcing.
 *
 * Converted to internal mph; OVERRIDE refuses value<0. No separate finite-value check; ADD may be
 * signed.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] value mph US, km/hr SI.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_climate_wind(SWMM_Engine engine, double value, int mode,
                                              int persist);

/**
 * @brief Read the current wind speed.
 *
 * Reads current stored climate state; before climate initialization/update this need not represent
 * a sampled source. Evaporation is the broadcast state, excluding a per-subcatchment PET override.
 *
 * @param[in] engine Non-null live engine; no RUNNING guard is applied to this getter.
 * @param[out] value Optional destination in mph US, km/hr SI.
 * @return SWMM_OK, including NULL value; SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_wind_speed(SWMM_Engine engine, double* value);

/**
 * @brief Register system evaporation rate forcing.
 *
 * Converted to internal ft/s after sources/monthly adjustments; OVERRIDE refuses value<0.
 * Per-subcatchment PET forcing takes precedence. No separate finite-value check.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] value in/day US, mm/day SI.
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_climate_evap(SWMM_Engine engine, double value, int mode,
                                              int persist);

/**
 * @brief Set the stored evaporation DRY_ONLY flag.
 *
 * A subcatchment PET OVERRIDE bypasses the native DRY_ONLY rule.
 *
 * @param[in] engine Non-null live engine; no RUNNING guard is imposed.
 * @param[in] flag 0 permits evaporation in rainfall; nonzero suppresses native evaporation during
 *     rainfall.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_set_dry_only(SWMM_Engine engine, int flag);

/**
 * @brief Read the stored evaporation DRY_ONLY flag.
 *
 * @param[in] engine Non-null live engine.
 * @param[out] flag Optional destination: 0 disabled, 1 enabled.
 * @return SWMM_OK, including NULL flag; SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_dry_only(SWMM_Engine engine, int* flag);

/* =========================================================================
 * Gage forcing
 * ========================================================================= */

/**
 * @brief Register gage rainfall forcing for the next step(s).
 *
 * Affects linked subcatchments through gage rainfall assembly, subject to IGNORE_RAINFALL. ADD
 * resolves the unforced rate each step to avoid repeated accumulation into a held rate. No
 * independent finite/nonnegative validation is applied.
 *
 * @param[in] engine Non-null RUNNING engine; exclusive access is required. Call between steps or
 *     from a callback where the selected runtime setter is permitted.
 * @param[in] idx Zero-based gage index.
 * @param[in] value project rainfall intensity (in/hr US, mm/hr SI).
 * @param[in] mode SWMM_FORCING_NONE (0) disables this channel, OVERRIDE (1) replaces its
 *     input/state, ADD (2) applies the channel-specific addition.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside RUNNING,
 *     SWMM_ERR_BADINDEX for invalid element/pollutant index, or SWMM_ERR_BADPARAM for invalid
 *     mode/persistence or channel-specific refusal.
 */
SWMM_ENGINE_API int swmm_forcing_gage_rainfall(SWMM_Engine engine, int idx, double value, int mode,
                                               int persist);

/* =========================================================================
 * Clear forcing
 * ========================================================================= */

/* =========================================================================
 * PE4 — per-element climate
 *
 * Air temperature, humidity, wind and incoming shortwave are GLOBAL in every
 * deck and per-element ONLY here. There is no deck syntax for per-element
 * meteorology at any phase, deliberately: `ClimateState` is shared with
 * hydrology, snowmelt and evaporation, so a hand-maintained file that could
 * set a per-conduit air temperature would assert something the snowpack on
 * the subcatchment above permanently disagrees with. A caller of THIS API is
 * a coupled driver asserting the heterogeneity per step and answerable for
 * it — which is how the reference works too: HydroCouple's RHEComponent and
 * CSHComponent receive per-element meteorology through EXCHANGE ITEMS.
 *
 * Values are resolved AT THE FLUX CALL, never written into the climate
 * state, so a per-link push cannot reach snowmelt or evaporation.
 * ========================================================================= */

/**
 * @brief Register an element-local atmospheric heat-flux forcing.
 *
 * Resolved at the element heat-flux evaluation and does not modify the global climate state used by
 * hydrology, snow or evaporation. The caller owns the heterogeneous forcing schedule; the API does
 * not validate a matching active heat model.
 *
 * @param[in] engine Non-null live engine; this setter has no RUNNING guard and lazily allocates its
 *     channel.
 * @param[in] kind SWMM_HEAT_ELEM_NODE (0) or SWMM_HEAT_ELEM_LINK (1); subcatchments are
 *     unsupported.
 * @param[in] index Zero-based index within the selected node/link table.
 * @param[in] variable SWMM_FORCE_ELEM_AIR_TEMPERATURE, HUMIDITY, WIND_SPEED or SHORTWAVE.
 * @param[in] value Air temperature degrees F/C by project system (ADD is a difference), humidity %,
 *     wind mph/km/hr, or shortwave W/m2. No finite/range validation is applied.
 * @param[in] mode SWMM_FORCING_OVERRIDE or ADD only; NONE is refused here. Use targeted clear to
 *     disable.
 * @param[in] persist SWMM_FORCING_RESET (0) clears after the next physical step;
 *     SWMM_FORCING_PERSIST (1) retains until replaced/cleared.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid element index,
 *     or SWMM_ERR_BADPARAM for unsupported kind/channel/mode/persistence.
 */
SWMM_ENGINE_API int swmm_forcing_element_climate(SWMM_Engine engine, int kind, int index,
                                                 int variable, double value, int mode, int persist);

/**
 * @brief Read an element-local registered climate prescription.
 *
 * Returns the prescription and mode, not the resolved atmospheric value including global
 * source/other heat calculations.
 *
 * @param[in] engine Non-null live engine; no RUNNING guard.
 * @param[in] kind SWMM_HEAT_ELEM_NODE (0) or SWMM_HEAT_ELEM_LINK (1); subcatchments are
 *     unsupported.
 * @param[in] index Zero-based index within the selected node/link table.
 * @param[in] variable SWMM_FORCE_ELEM_AIR_TEMPERATURE, HUMIDITY, WIND_SPEED or SHORTWAVE.
 * @param[out] value Optional output in the same display units as registration; zero when channel is
 *     absent/disabled. Temperature ADD is a difference.
 * @param[out] mode Optional output: NONE when absent/disabled, otherwise OVERRIDE or ADD.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid index, or
 *     SWMM_ERR_BADPARAM for unsupported kind/channel.
 */
SWMM_ENGINE_API int swmm_forcing_element_climate_get(SWMM_Engine engine, int kind, int index,
                                                     int variable, double* value, int* mode);

/**
 * @brief Disable selected forcing modes without clearing their stored values.
 *
 * Does not remove the separate persistent user_lat_flow/user_conc_mass_flux fields. Per-element
 * climate can be cleared for both kinds through the shared channel spelling; mode NONE restores use
 * of the global/model input.
 *
 * @param[in] engine Non-null live engine; this clear operation has no RUNNING guard.
 * @param[in] type SWMM_ForcingType channel. NODE_QUALITY/LINK_QUALITY clear every pollutant for the
 *     indexed element.
 * @param[in] idx Zero-based target element index; ignored for global climate channels. For
 *     element-climate channels, clears this index in BOTH node/link tables where present;
 *     out-of-range element-climate indices are harmless no-ops.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid ordinary
 *     element indices, or SWMM_ERR_BADPARAM for an unknown type.
 */
SWMM_ENGINE_API int swmm_forcing_clear(SWMM_Engine engine, int type, int idx);

/**
 * @brief Clear all ForcingData channels on the engine.
 *
 * Calls the forcing-store clear operation, including reserved-species and element-climate
 * prescriptions. Does not promise to reset state already changed by previous physics or the
 * separate persistent user inflow/quality fields.
 *
 * @param[in] engine Non-null live engine; no RUNNING guard is applied.
 * @return SWMM_OK, or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_forcing_clear_all(SWMM_Engine engine);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_FORCING_H */
