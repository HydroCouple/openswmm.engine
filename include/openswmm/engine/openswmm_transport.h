/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright 2026 Caleb Buahin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file openswmm_transport.h
 * @brief E6 — the ARD transport configuration surface ([TRANSPORT_OPTIONS],
 *        [TRANSPORT_BOUNDARIES], [TRANSPORT_SOURCES]).
 *
 * @details The last quality subsystem with no C API of its own: heat,
 *          reactions, water age and the pollutant tables all have theirs,
 *          and G7g (the dispersion/boundary property editors) was gated on
 *          this header existing. Follows `openswmm_water_age.h`'s
 *          conventions: values cross the boundary in the CONFIG FILE'S
 *          units, the engine's internal units stay internal, and setters
 *          mark the config `configured` so a save renders it.
 *
 *          **Boundary/source rows are read-only in this revision.** The
 *          rows resolve names to indices once, at open
 *          (`resolveArdTransportRows`), with failures fatal there where the
 *          row text is available for the diagnostic; a runtime add would
 *          need that resolution re-entrant and its failures survivable,
 *          which is its own round (recorded in the E6 handoff). Editors
 *          read here and write through the component file.
 */

#ifndef OPENSWMM_TRANSPORT_H
#define OPENSWMM_TRANSPORT_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief `[TRANSPORT_OPTIONS] DISPERSION` mode. */
typedef enum SWMM_TransportDispersionMode {
    SWMM_DISPERSION_OFF     = 0, /**< No global dispersion (per-conduit
                                      overrides still apply). */
    SWMM_DISPERSION_FISCHER = 1, /**< Fischer et al. (1979) closure from the
                                      local hydraulics. */
    SWMM_DISPERSION_VALUE   = 2  /**< One user coefficient, display len²/s. */
} SWMM_TransportDispersionMode;

/** @brief 1 if a transport.ard component is configured on the model. */
SWMM_ENGINE_API int swmm_transport_get_configured(SWMM_Engine engine,
                                                  int* configured);

/** @brief The dispersion mode (SWMM_TransportDispersionMode). */
SWMM_ENGINE_API int swmm_transport_get_dispersion_mode(SWMM_Engine engine,
                                                       int* mode);

/**
 * @brief Set the dispersion mode.
 * @details Setting VALUE without a coefficient leaves the previous
 *          coefficient in force — set both. Marks the config present.
 */
SWMM_ENGINE_API int swmm_transport_set_dispersion_mode(SWMM_Engine engine,
                                                       int mode);

/** @brief VALUE-mode coefficient, display units (ft²/s or m²/s). */
SWMM_ENGINE_API int swmm_transport_get_dispersion_value(SWMM_Engine engine,
                                                        double* value);

/** @brief Set the VALUE-mode coefficient (must be >= 0 and finite). */
SWMM_ENGINE_API int swmm_transport_set_dispersion_value(SWMM_Engine engine,
                                                        double value);

/** @brief `TARGET_DX`, display length units; 0 means engine default. */
SWMM_ENGINE_API int swmm_transport_get_target_dx(SWMM_Engine engine,
                                                 double* dx);

/** @brief Set `TARGET_DX` (must be >= 0 and finite; 0 restores default). */
SWMM_ENGINE_API int swmm_transport_set_target_dx(SWMM_Engine engine,
                                                 double dx);

/** @brief Number of per-conduit dispersion override rows. */
SWMM_ENGINE_API int swmm_transport_conduit_disp_count(SWMM_Engine engine,
                                                      int* count);

/** @brief Override row `index`: link index + coefficient (display units). */
SWMM_ENGINE_API int swmm_transport_get_conduit_disp(SWMM_Engine engine,
                                                    int index,
                                                    int* link_index,
                                                    double* value);

/** @brief Number of `[TRANSPORT_BOUNDARIES]` rows (raw, as configured). */
SWMM_ENGINE_API int swmm_transport_boundary_count(SWMM_Engine engine,
                                                  int* count);

/**
 * @brief Read boundary row `index`.
 * @param element   Node name buffer (may be NULL to skip).
 * @param elem_len  Its capacity, including the terminator.
 * @param species   Species name buffer (may be NULL).
 * @param spec_len  Its capacity.
 * @param is_ts     1 if TIMESERIES mode. The series name is returned in
 *                  `ts_name` when set; `value` is the VALUE-mode
 *                  concentration otherwise (species units).
 */
SWMM_ENGINE_API int swmm_transport_get_boundary(SWMM_Engine engine, int index,
                                                char* element, int elem_len,
                                                char* species, int spec_len,
                                                int* is_ts, double* value,
                                                char* ts_name, int ts_len);

/** @brief Number of `[TRANSPORT_SOURCES]` rows (raw, as configured). */
SWMM_ENGINE_API int swmm_transport_source_count(SWMM_Engine engine,
                                                int* count);

/**
 * @brief Read source row `index`. Same contract as the boundary reader;
 *        `value` is the VALUE-mode mass rate (species mass units per
 *        second; negative = extraction, D-NS1).
 */
SWMM_ENGINE_API int swmm_transport_get_source(SWMM_Engine engine, int index,
                                              char* element, int elem_len,
                                              char* species, int spec_len,
                                              int* is_ts, double* value,
                                              char* ts_name, int ts_len);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_TRANSPORT_H */
