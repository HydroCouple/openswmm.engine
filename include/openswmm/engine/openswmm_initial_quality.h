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
 * @file openswmm_initial_quality.h
 * @brief C API — [INITIAL_QUALITY] per-element initial concentrations (E-A4).
 *
 * @details Row-based surface over the section store, mirroring the
 *          swmm_ext_inflow_* family: entries are indexed 0..count-1 in file
 *          order, `set` is an UPSERT keyed on (is_link, elem_idx,
 *          constituent), and `remove` deletes by entry index (subsequent
 *          entries shift down by one).
 *
 *          Constituent is a name: a [POLLUTANTS] pollutant, or the reserved
 *          species "__WATER_AGE__" (value in HOURS, signed) or
 *          "__TEMPERATURE__" (value in degC). Pollutant values are
 *          concentrations in the pollutant's own units and must be
 *          non-negative.
 *
 *          Rows only seed state at initialize(), so mutation is guarded to
 *          the editable states (BUILDING/OPENED) — the same contract as
 *          swmm_pollutant_set_init_conc.
 *
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_INITIAL_QUALITY_H
#define OPENSWMM_INITIAL_QUALITY_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Number of [INITIAL_QUALITY] entries.
 * @param engine Engine handle.
 * @return Entry count, or -1 on a bad handle.
 */
SWMM_ENGINE_API int swmm_init_quality_count(SWMM_Engine engine);

/**
 * @brief Read one entry.
 * @param engine           Engine handle.
 * @param entry_idx        Entry index (0..count-1).
 * @param is_link          [out] 0 = node row, 1 = link row.
 * @param elem_idx         [out] Resolved node/link index (-1 if unresolved).
 * @param constituent_buf  [out] Constituent name (NUL-terminated).
 * @param constituent_len  Buffer size in bytes.
 * @param value            [out] Raw value (pollutant units / hours / degC).
 * @return SWMM_OK, SWMM_ERR_BADINDEX, or SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_init_quality_get(SWMM_Engine engine, int entry_idx,
                                          int* is_link, int* elem_idx,
                                          char* constituent_buf,
                                          int constituent_len, double* value);

/**
 * @brief Upsert one entry keyed on (is_link, elem_idx, constituent).
 * @details BUILDING/OPENED only. Unknown constituent name, bad element
 *          index, or a negative pollutant value returns SWMM_ERR_BADPARAM.
 *          (Negative values are legal for "__WATER_AGE__" and
 *          "__TEMPERATURE__".)
 * @param engine       Engine handle.
 * @param is_link      0 = node row, 1 = link row.
 * @param elem_idx     Node/link index.
 * @param constituent  Pollutant name or reserved species name.
 * @param value        Raw value (pollutant units / hours / degC).
 * @return SWMM_OK, SWMM_ERR_LIFECYCLE, SWMM_ERR_BADINDEX, or
 *         SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_init_quality_set(SWMM_Engine engine, int is_link,
                                          int elem_idx,
                                          const char* constituent,
                                          double value);

/**
 * @brief Remove the entry at @p entry_idx. Subsequent entries shift down by
 *        one — callers holding cached indices must re-enumerate.
 * @details BUILDING/OPENED only.
 * @return SWMM_OK, SWMM_ERR_LIFECYCLE, or SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_init_quality_remove(SWMM_Engine engine,
                                             int entry_idx);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_INITIAL_QUALITY_H */
