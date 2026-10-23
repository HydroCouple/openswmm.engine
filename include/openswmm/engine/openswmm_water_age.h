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
 * @file      openswmm_water_age.h
 * @brief     Water-age source-table CRUD (subplan X5 = water-age plan A6, the
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
    SWMM_AGE_SRC_RAINFALL = 0,
    SWMM_AGE_SRC_DWF = 1,
    SWMM_AGE_SRC_GW = 2,
    SWMM_AGE_SRC_RDII = 3,
    SWMM_AGE_SRC_EXTERNAL_INFLOW = 4,
    SWMM_AGE_SRC_IFACE = 5,
    SWMM_AGE_SRC_INITIAL_STATE = 6,
    SWMM_AGE_SRC_COUNT = 7
} SWMM_WaterAgeSource;

/**
 * @brief Read the model WATER_AGE option without changing it.
 *
 * Reports configuration, not whether every transport domain has initialized age storage.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. Setters update the live
 *     source table without a lifecycle guard.
 * @param[out] enabled Non-null destination: 1 when configured ON, otherwise 0.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null output.
 */
SWMM_ENGINE_API int swmm_water_age_get_enabled(SWMM_Engine engine, int* enabled);

/**
 * @brief Read one global pathway source age in hours.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. Setters update the live
 *     source table without a lifecycle guard.
 * @param[in] source SWMM_WaterAgeSource in [SWMM_AGE_SRC_RAINFALL,SWMM_AGE_SRC_COUNT); invalid
 *     source codes return BADINDEX.
 * @param[out] hours Non-null destination for the stored signed age in hours (internal seconds
 *     divided by 3600).
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADPARAM for null hours, or
 *     SWMM_ERR_BADINDEX for invalid source.
 */
SWMM_ENGINE_API int swmm_water_age_get_global_source(SWMM_Engine engine, int source, double* hours);

/**
 * @brief Assign a global pathway source age in hours.
 *
 * Marks the source configuration present but does not turn WATER_AGE on. Runtime loaders reread the
 * table; negative extraction is clamped at consumption so available age-volume cannot become
 * negative.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. Setters update the live
 *     source table without a lifecycle guard.
 * @param[in] source SWMM_WaterAgeSource in [SWMM_AGE_SRC_RAINFALL,SWMM_AGE_SRC_COUNT); invalid
 *     source codes return BADINDEX.
 * @param[in] hours Signed source age in hours, converted to internal seconds. Negative values
 *     request age-volume extraction; caller must provide a finite value because this setter does
 *     not validate it.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid source.
 */
SWMM_ENGINE_API int swmm_water_age_set_global_source(SWMM_Engine engine, int source, double hours);

/**
 * @brief Count stored per-node source-age override rows.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. Setters update the live
 *     source table without a lifecycle guard.
 * @param[out] count Non-null destination for the row count.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null count.
 */
SWMM_ENGINE_API int swmm_water_age_override_count(SWMM_Engine engine, int* count);

/**
 * @brief Read a per-node source-age override by row index.
 *
 * All outputs may be NULL. Updating an existing key keeps its row position; removing a row shifts
 * later indices down.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. Setters update the live
 *     source table without a lifecycle guard.
 * @param[in] index Zero-based override index in [0,override count).
 * @param[out] source Optional destination for the SWMM_WaterAgeSource code.
 * @param[out] node_index Optional destination for the zero-based node index.
 * @param[out] hours Optional destination for signed source age in hours.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid row.
 */
SWMM_ENGINE_API int swmm_water_age_get_override(SWMM_Engine engine, int index, int* source,
                                                int* node_index, double* hours);

/**
 * @brief Insert or update a source/node age override.
 *
 * Only SWMM_AGE_SRC_DWF and SWMM_AGE_SRC_EXTERNAL_INFLOW allow NODE scope. Existing (source,node)
 * rows update in place; new keys append. Marks configuration present without enabling WATER_AGE;
 * runtime changes are consumed on the next routing step.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. Setters update the live
 *     source table without a lifecycle guard.
 * @param[in] source SWMM_WaterAgeSource in [SWMM_AGE_SRC_RAINFALL,SWMM_AGE_SRC_COUNT); invalid
 *     source codes return BADINDEX.
 * @param[in] node_index Zero-based existing node index; invalid indices return BADINDEX.
 * @param[in] hours Signed hours, converted to internal seconds without finite-value validation.
 *     Negative values extract available age-volume at consumption.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid source/node,
 *     or SWMM_ERR_BADPARAM for a pathway that does not allow NODE scope.
 */
SWMM_ENGINE_API int swmm_water_age_set_override(SWMM_Engine engine, int source, int node_index,
                                                double hours);

/**
 * @brief Remove one source/node override and shift later row indices down.
 *
 * Not idempotent for missing rows. Retains the global pathway setting and configured flag.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. Setters update the live
 *     source table without a lifecycle guard.
 * @param[in] source Source code used as part of the stored key; this call locates the pair without
 *     a separate scope check.
 * @param[in] node_index Node index used as the second stored key.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX when the key is absent.
 */
SWMM_ENGINE_API int swmm_water_age_remove_override(SWMM_Engine engine, int source, int node_index);

/**
 * @brief Serialize the current source-age table to a component file.
 *
 * Uses the component serializer with hours in the output and read-back precision. An unconfigured
 * table still writes an empty [WATER_AGE_SOURCES] header. This wrapper does not inspect
 * fwrite/fclose results, update component path authoring, or provide atomic publication.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. Setters update the live
 *     source table without a lifecycle guard.
 * @param[in] path Non-null, nonempty NUL-terminated UTF-8 destination path; opened for text
 *     writing, truncating existing contents.
 * @return SWMM_OK after writing/closing; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADPARAM for
 *     null/empty path, or SWMM_ERR_IO when the destination cannot be opened.
 */
SWMM_ENGINE_API int swmm_water_age_save(SWMM_Engine engine, const char* path);

#ifdef __cplusplus
}
#endif

#endif // OPENSWMM_WATER_AGE_H
