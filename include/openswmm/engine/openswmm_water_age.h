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
 * @file openswmm_water_age.h
 * @brief Water-age source-table CRUD (subplan X5 = water-age plan A6, the
 *        minimal GUI-facing subset; TRANSPORT_QUALITY_GUI_PLAN §6 prereq 5).
 *
 * @details The `[WATER_AGE_SOURCES]` configuration (waterage component,
 *          `model.age`) as a keyed table: a GLOBAL age per source pathway,
 *          plus per-NODE overrides for the DWF and EXTERNAL_INFLOW
 *          pathways (the A1a scope rule — other pathways refuse NODE
 *          scope, exactly like the parser). Values are HOURS, the config
 *          file's unit; **negative values are legal** — a negative source
 *          age EXTRACTS age-volume, clamped so age never goes below zero
 *          (D-NS1, subplan §3.1). Edits mutate live engine state (the
 *          loaders re-read the table every step, so mid-simulation edits
 *          take effect on the next routing step); `swmm_water_age_save`
 *          persists the table to a component file.
 *
 *          The full A6 surface (Python/MCP bindings, node/link age state
 *          getters) is deferred — this header is exactly what the GUI's
 *          Water Age Sources editor (G3g) binds to.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_WATER_AGE_H
#define OPENSWMM_WATER_AGE_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Source pathways (mirrors the engine's WaterAgeSource order — the
 *        values are the storage indices, do not renumber).
 */
typedef enum SWMM_WaterAgeSource {
    SWMM_AGE_SRC_RAINFALL        = 0,
    SWMM_AGE_SRC_DWF             = 1,
    SWMM_AGE_SRC_GW              = 2,
    SWMM_AGE_SRC_RDII            = 3,
    SWMM_AGE_SRC_EXTERNAL_INFLOW = 4,
    SWMM_AGE_SRC_IFACE           = 5,
    SWMM_AGE_SRC_INITIAL_STATE   = 6,
    SWMM_AGE_SRC_COUNT           = 7
} SWMM_WaterAgeSource;

/** @brief 1 if [OPTIONS] WATER_AGE is ON, else 0. */
SWMM_ENGINE_API int swmm_water_age_get_enabled(SWMM_Engine engine,
                                               int* enabled);

/** @brief GLOBAL age for one source pathway, HOURS (signed per D-NS1). */
SWMM_ENGINE_API int swmm_water_age_get_global_source(SWMM_Engine engine,
                                                     int source,
                                                     double* hours);

/**
 * @brief Set the GLOBAL age for one source pathway, HOURS.
 * @details Negative hours are legal (age-volume extraction, D-NS1). Marks
 *          the config as present so the engine consumes it.
 */
SWMM_ENGINE_API int swmm_water_age_set_global_source(SWMM_Engine engine,
                                                     int source,
                                                     double hours);

/** @brief Number of per-node override rows. */
SWMM_ENGINE_API int swmm_water_age_override_count(SWMM_Engine engine,
                                                  int* count);

/**
 * @brief Read override row `index` (0-based): its source, node index, and
 *        HOURS. Row order is stable across edits within a session.
 */
SWMM_ENGINE_API int swmm_water_age_get_override(SWMM_Engine engine, int index,
                                                int* source, int* node_index,
                                                double* hours);

/**
 * @brief Add or update the override for (source, node_index), HOURS.
 * @details Only SWMM_AGE_SRC_DWF and SWMM_AGE_SRC_EXTERNAL_INFLOW take
 *          NODE scope (the parser's A1a rule) — others return
 *          SWMM_ERR_BADPARAM. Negative hours are legal (D-NS1).
 */
SWMM_ENGINE_API int swmm_water_age_set_override(SWMM_Engine engine,
                                                int source, int node_index,
                                                double hours);

/** @brief Remove the override for (source, node_index); BADINDEX if none. */
SWMM_ENGINE_API int swmm_water_age_remove_override(SWMM_Engine engine,
                                                   int source,
                                                   int node_index);

/**
 * @brief Write the current table as a `[WATER_AGE_SOURCES]` component file
 *        (the `model.age` format the waterage component parses) — the
 *        GUI editor's save path.
 */
SWMM_ENGINE_API int swmm_water_age_save(SWMM_Engine engine,
                                        const char* path);

#ifdef __cplusplus
}
#endif

#endif  // OPENSWMM_WATER_AGE_H
