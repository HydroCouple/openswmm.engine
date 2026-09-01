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
 * @file openswmm_heat.h
 * @brief Heat-transport configuration as an editable surface (phase H6a).
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_HEAT_H
#define OPENSWMM_HEAT_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Flux modules of heat plan §2, each independently toggleable. */
typedef enum SWMM_HeatFluxModule {
    SWMM_HEAT_SURFACE_EXCHANGE   = 0, /**< Latent + sensible (H2). */
    SWMM_HEAT_RADIATIVE_EXCHANGE = 1, /**< Shortwave + longwave (H3). */
    SWMM_HEAT_LAYER_CONDUCTION   = 2  /**< LID vertical conduction (H5b). */
} SWMM_HeatFluxModule;

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
    SWMM_HEAT_SW_CONSTANT   = 0, /**< Fixed W/m². */
    SWMM_HEAT_SW_TIMESERIES = 1, /**< Measured record. */
    SWMM_HEAT_SW_COMPUTED   = 2  /**< Solar position + Bird clear-sky. */
} SWMM_HeatShortwaveMode;

/** @brief `[RADIATIVE_FLUXES]` scalar parameters. */
typedef enum SWMM_HeatRadiativeParam {
    SWMM_HEAT_RAD_SHORTWAVE        = 0, /**< W/m², CONSTANT mode only. */
    SWMM_HEAT_RAD_ALBEDO           = 1, /**< Water reflectance Rs [0,1]. */
    SWMM_HEAT_RAD_SHADE_FACTOR     = 2, /**< fs [0,1]. */
    SWMM_HEAT_RAD_SKY_VIEW         = 3, /**< fsky [0,1]. */
    SWMM_HEAT_RAD_EMISS_WATER      = 4, /**< εw [0,1]. */
    SWMM_HEAT_RAD_EMISS_LANDCOVER  = 5, /**< εlc [0,1]. */
    SWMM_HEAT_RAD_ATM_EMISS_COEFF  = 6, /**< Brunt Aa [0,1]. */
    SWMM_HEAT_RAD_LW_REFLECTION    = 7  /**< RL [0,1]. */
} SWMM_HeatRadiativeParam;

/** @brief `[SOLAR_RADIATION]` parameters (consulted under COMPUTED only). */
typedef enum SWMM_HeatSolarParam {
    SWMM_HEAT_SOLAR_LATITUDE      = 0, /**< Degrees, +N, [-90, 90]. */
    SWMM_HEAT_SOLAR_LONGITUDE     = 1, /**< Degrees, +E, [-180, 180]. */
    SWMM_HEAT_SOLAR_TIMEZONE      = 2, /**< Hours from UTC, +E. */
    SWMM_HEAT_SOLAR_ELEVATION     = 3, /**< Metres, [-500, 9000]. Below sea
                                        *   level is legal. Never written =
                                        *   take the climate state's elev. */
    SWMM_HEAT_SOLAR_TURBIDITY_380 = 4, /**< Bird AOD at 380 nm. */
    SWMM_HEAT_SOLAR_TURBIDITY_500 = 5, /**< Bird AOD at 500 nm. */
    SWMM_HEAT_SOLAR_PRECIP_WATER  = 6, /**< Precipitable water, cm. */
    SWMM_HEAT_SOLAR_OZONE         = 7, /**< Ozone column, cm. */
    SWMM_HEAT_SOLAR_GROUND_ALBEDO = 8  /**< LAND albedo [0,1] — NOT the
                                        *   water's SWMM_HEAT_RAD_ALBEDO. */
} SWMM_HeatSolarParam;

/** @brief `[CLOUD_COVER]` parameters. */
typedef enum SWMM_HeatCloudParam {
    SWMM_HEAT_CLOUD_FRACTION   = 0, /**< C [0,1], constant spelling. */
    SWMM_HEAT_CLOUD_SW_ATTEN_K = 1, /**< Kasten–Czeplak k. */
    SWMM_HEAT_CLOUD_SW_ATTEN_N = 2, /**< Kasten–Czeplak n. */
    SWMM_HEAT_CLOUD_LW_CLOUD_K = 3  /**< Bolz k_lw. */
} SWMM_HeatCloudParam;

/* ---------------------------------------------------------------- toggles */

/** @brief Is `[OPTIONS] HEAT_TRANSPORT` on? */
SWMM_ENGINE_API int swmm_heat_get_enabled(SWMM_Engine engine, int* enabled);

/** @brief Read one `[HEAT_FLUXES]` module toggle. */
SWMM_ENGINE_API int swmm_heat_get_module(SWMM_Engine engine, int module,
                                         int* on);
/** @brief Write one `[HEAT_FLUXES]` module toggle. */
SWMM_ENGINE_API int swmm_heat_set_module(SWMM_Engine engine, int module,
                                         int on);

/* ------------------------------------------------------------- radiative */

/** @brief Read one `[RADIATIVE_FLUXES]` parameter. */
SWMM_ENGINE_API int swmm_heat_get_radiative(SWMM_Engine engine, int param,
                                            double* value);
/** @brief Write one `[RADIATIVE_FLUXES]` parameter.
 *  @returns ::SWMM_ERR_BADPARAM if a fraction is outside [0,1]; if
 *           SHORTWAVE is negative; or if SHORTWAVE is written while the
 *           mode is not ::SWMM_HEAT_SW_CONSTANT — a constant is not read in
 *           the other two modes, and storing one there would look
 *           configured while changing nothing. Switch the mode first.
 *
 *           Values are REFUSED, not clamped — the parser's rule, so the API
 *           and the deck agree. A refused write does not take effect. */
SWMM_ENGINE_API int swmm_heat_set_radiative(SWMM_Engine engine, int param,
                                            double value);

/** @brief Read the incoming-shortwave mode (::SWMM_HeatShortwaveMode). */
SWMM_ENGINE_API int swmm_heat_get_shortwave_mode(SWMM_Engine engine,
                                                 int* mode);
/** @brief Set the incoming-shortwave mode.
 *  @returns ::SWMM_ERR_BADPARAM for ::SWMM_HEAT_SW_COMPUTED when latitude
 *           or longitude is unset, and for ::SWMM_HEAT_SW_TIMESERIES when
 *           no series has been bound by
 *           ::swmm_heat_set_shortwave_timeseries. */
SWMM_ENGINE_API int swmm_heat_set_shortwave_mode(SWMM_Engine engine,
                                                 int mode);

/** @brief Bind a `[TIMESERIES]` (by name) as the shortwave record and
 *         switch the mode to ::SWMM_HEAT_SW_TIMESERIES.
 *  @returns ::SWMM_ERR_BADPARAM if no timeseries of that name exists. */
SWMM_ENGINE_API int swmm_heat_set_shortwave_timeseries(SWMM_Engine engine,
                                                       const char* name);

/** @brief Name of the `[TIMESERIES]` bound as the shortwave record, or ""
 *         when none is bound. NUL-terminated, truncated to `buflen`.
 *  @details The read half `swmm_heat_set_shortwave_timeseries` never had —
 *           an editor could rebind a series but only display "(keep current
 *           series)" for the one already bound (the G4g gap, recorded
 *           2026-08-31). Valid in every mode: the binding survives a switch
 *           to CONSTANT or COMPUTED, exactly as the parser's does. */
SWMM_ENGINE_API int swmm_heat_get_shortwave_timeseries(SWMM_Engine engine,
                                                       char* buf, int buflen);

/** @brief Resolved incoming shortwave at the CURRENT step, W/m², cloud
 *         already applied. Read-only: this is state, not configuration.
 *         0 before the first step, and whenever radiative exchange is off. */
SWMM_ENGINE_API int swmm_heat_get_current_shortwave(SWMM_Engine engine,
                                                    double* wm2);

/* ----------------------------------------------------------------- solar */

/** @brief Read one `[SOLAR_RADIATION]` parameter. */
SWMM_ENGINE_API int swmm_heat_get_solar(SWMM_Engine engine, int param,
                                        double* value);
/** @brief Write one `[SOLAR_RADIATION]` parameter.
 *  @returns ::SWMM_ERR_BADPARAM if out of range. Setting LATITUDE or
 *           LONGITUDE also marks it as explicitly provided, which is what
 *           ::SWMM_HEAT_SW_COMPUTED checks for. */
SWMM_ENGINE_API int swmm_heat_set_solar(SWMM_Engine engine, int param,
                                        double value);

/** @brief Have latitude AND longitude both been explicitly set?
 *  @details The precondition for ::SWMM_HEAT_SW_COMPUTED. A GUI should
 *           gate the COMPUTED radio button on this rather than discovering
 *           the refusal after the fact. */
SWMM_ENGINE_API int swmm_heat_get_solar_sited(SWMM_Engine engine, int* sited);

/* ----------------------------------------------------------------- cloud */

/** @brief Is a `[CLOUD_COVER]` section in effect? */
SWMM_ENGINE_API int swmm_heat_get_cloud_configured(SWMM_Engine engine,
                                                   int* configured);

/** @brief Read one `[CLOUD_COVER]` parameter. */
SWMM_ENGINE_API int swmm_heat_get_cloud(SWMM_Engine engine, int param,
                                        double* value);
/** @brief Write one `[CLOUD_COVER]` parameter. Writing any of them marks
 *         cloud cover as configured.
 *  @returns ::SWMM_ERR_BADPARAM if FRACTION is outside [0,1] (it is a
 *           fraction, not a percent) or a coefficient is negative. */
SWMM_ENGINE_API int swmm_heat_set_cloud(SWMM_Engine engine, int param,
                                        double value);

/** @brief Bind a `[TIMESERIES]` (by name) as the cloud-fraction record.
 *  @returns ::SWMM_ERR_BADPARAM if no timeseries of that name exists. */
SWMM_ENGINE_API int swmm_heat_set_cloud_timeseries(SWMM_Engine engine,
                                                   const char* name);

/** @brief Name of the `[TIMESERIES]` bound as the cloud-fraction record, or
 *         "" when none is bound. NUL-terminated, truncated to `buflen`.
 *  @details The shortwave getter's sibling — see
 *           ::swmm_heat_get_shortwave_timeseries. */
SWMM_ENGINE_API int swmm_heat_get_cloud_timeseries(SWMM_Engine engine,
                                                   char* buf, int buflen);

/** @brief Clear `[CLOUD_COVER]` entirely — back to clear sky.
 *  @details Restores the exact H3 longwave path, not an approximation of
 *           it: the cloud factor becomes a literal 1.0 that
 *           `atmosphericEmissivity` short-circuits. */
SWMM_ENGINE_API int swmm_heat_clear_cloud(SWMM_Engine engine);

/** @brief Cloud fraction in effect at the CURRENT step, [0,1]. Read-only. */
SWMM_ENGINE_API int swmm_heat_get_current_cloud(SWMM_Engine engine,
                                                double* fraction);

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
    SWMM_HEAT_SRC_RAINFALL        = 0, /**< washoff runoff. */
    SWMM_HEAT_SRC_DWF             = 1, /**< dry-weather flow. */
    SWMM_HEAT_SRC_GW              = 2, /**< groundwater. */
    SWMM_HEAT_SRC_RDII            = 3, /**< rainfall-derived infiltration. */
    SWMM_HEAT_SRC_EXTERNAL_INFLOW = 4, /**< `[INFLOWS]`. */
    SWMM_HEAT_SRC_IFACE           = 5, /**< interface file. */
    SWMM_HEAT_SRC_INITIAL_STATE   = 6  /**< water present at t = 0. */
} SWMM_HeatSourceKind;

/** @brief Number of sources the table carries (7). Never fails on a model
 *         with no heat configured — the table is a fixed enum extent, not a
 *         parsed list. */
SWMM_ENGINE_API int swmm_heat_source_count(SWMM_Engine engine, int* count);

/** @brief Read one source's GLOBAL inlet temperature (°C).
 *  @returns ::SWMM_ERR_BADINDEX for a source outside the enum. A source with
 *           no row reads the 20 °C default — use
 *           ::swmm_heat_get_source_configured to tell the two apart. */
SWMM_ENGINE_API int swmm_heat_get_source_temp(SWMM_Engine engine, int source,
                                              double* temp_c);

/** @brief Write one source's GLOBAL inlet temperature (°C).
 *  @returns ::SWMM_ERR_BADPARAM outside [-50, 100] — the parser's own range
 *           (`HeatComponent.cpp` `parse_celsius`). REFUSED, not clamped, and
 *           a refused write does not take effect. Marks the source
 *           configured. */
SWMM_ENGINE_API int swmm_heat_set_source_temp(SWMM_Engine engine, int source,
                                              double temp_c);

/** @brief Did the model set this source explicitly, or is it taking the
 *         default? The editor needs the distinction to avoid writing rows a
 *         user never asked for. */
SWMM_ENGINE_API int swmm_heat_get_source_configured(SWMM_Engine engine,
                                                    int source,
                                                    int* configured);

/** @brief Return a source to the 20 °C default and mark it unconfigured, so
 *         the writer emits no row for it. NODE overrides are untouched —
 *         they are separate rows and removing them silently would delete
 *         model the caller did not name. */
SWMM_ENGINE_API int swmm_heat_clear_source_temp(SWMM_Engine engine,
                                                int source);

/** @brief Number of NODE-scope override rows. */
SWMM_ENGINE_API int swmm_heat_node_override_count(SWMM_Engine engine,
                                                  int* count);

/** @brief Read one NODE override by row index. Any out-pointer may be NULL. */
SWMM_ENGINE_API int swmm_heat_get_node_override(SWMM_Engine engine, int index,
                                                int* source, int* node,
                                                double* temp_c);

/** @brief Add or update the NODE override for (@p source, @p node).
 *  @returns ::SWMM_ERR_BADPARAM if @p source is not DWF or EXTERNAL_INFLOW —
 *           **the H1 scope rule, refused rather than deferred silently**, the
 *           same answer the deck gets; if @p node is out of range; or if
 *           @p temp_c is outside [-50, 100].
 *  @note An existing (source, node) pair is UPDATED rather than duplicated.
 *        The parser refuses a duplicate row because a deck cannot mean two
 *        temperatures at once; through an API, setting the same pair twice
 *        is an edit, and refusing it would make the editor unable to change
 *        a value it just wrote. Same invariant — one row per pair — reached
 *        the way each caller means it. */
SWMM_ENGINE_API int swmm_heat_set_node_override(SWMM_Engine engine,
                                                int source, int node,
                                                double temp_c);

/** @brief Remove one NODE override by row index. Later rows shift down, so a
 *         caller iterating by index must re-read the count after removing. */
SWMM_ENGINE_API int swmm_heat_remove_node_override(SWMM_Engine engine,
                                                   int index);

/** @brief The temperature @p source water actually enters @p node at (°C) —
 *         the NODE override when one exists, else the GLOBAL value. This is
 *         `HeatConfigData::source_temp`, exposed so a caller reads the same
 *         resolution the engine uses rather than re-deriving the precedence
 *         and drifting from it. */
SWMM_ENGINE_API int swmm_heat_get_effective_source_temp(SWMM_Engine engine,
                                                        int source, int node,
                                                        double* temp_c);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_HEAT_H */
