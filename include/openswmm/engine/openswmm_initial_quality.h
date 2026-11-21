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
 * @file      openswmm_initial_quality.h
 * @brief     C API — [INITIAL_QUALITY] per-element initial concentrations (E-A4).
 *
 * @details Row-based surface over the section store, mirroring the
 *          swmm_ext_inflow_* family: entries are indexed 0..count-1 in file
 *          order, `set` is an UPSERT keyed on (is_link, elem_idx,
 *          constituent), and `remove` deletes by entry index (subsequent
 *          entries shift down by one).
 *
 *          Constituent is a name: a [POLLUTANTS] pollutant, a configured reaction
 *          species, or the reserved
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_INITIAL_QUALITY_H
#define OPENSWMM_INITIAL_QUALITY_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Count stored per-element initial-quality rows.
 *
 * @param[in] engine Non-null live engine handle; exclusive access is required.
 * @return Nonnegative row count, or -1 for null engine.
 */
SWMM_ENGINE_API int swmm_init_quality_count(SWMM_Engine engine);

/**
 * @brief Read a stored initial-quality row without applying it.
 *
 * Validates all output pointers/capacity before copying. Does not validate whether a stored
 * unresolved row can initialize the current model.
 *
 * @param[in] engine Non-null live engine handle; exclusive access is required.
 * @param[in] entry_idx Zero-based row index in [0,swmm_init_quality_count(engine)).
 * @param[out] is_link Non-null destination: 0=node, 1=link.
 * @param[out] elem_idx Non-null destination for the resolved element index, or -1 when unresolved.
 * @param[out] constituent_buf Non-null caller-owned constituent buffer; NUL-terminated and silently
 *     truncated to constituent_len-1 bytes.
 * @param[in] constituent_len Positive constituent-buffer byte capacity including NUL.
 * @param[out] value Non-null raw-value destination: pollutant/species concentration units, age
 *     hours or temperature degrees C.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row/element
 *     index, or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_init_quality_get(SWMM_Engine engine, int entry_idx, int* is_link,
                                          int* elem_idx, char* constituent_buf, int constituent_len,
                                          double* value);

/**
 * @brief Upsert initial concentration/age/temperature by element and resolved constituent.
 *
 * Rows seed state during initialization; this does not alter running concentrations. Existing rows
 * match resolved constituent kind, not just text spelling; updates keep row order. Reaction-species
 * entries also mirror into the engine reaction seed tables. Does not enable age, heat or reaction
 * transport automatically.
 *
 * @param[in] engine Non-null live engine handle; exclusive access is required.
 * @param[in] is_link 0 selects node; any nonzero value selects link.
 * @param[in] elem_idx Zero-based existing index within the selected element table.
 * @param[in] constituent Non-null copied constituent name: a known pollutant, configured reaction
 *     species, or exact reserved __WATER_AGE__/__TEMPERATURE__ token.
 * @param[in] value Pollutant/reaction-species concentration in its row units (negative refused),
 *     reserved age in signed hours or temperature in degrees C. No separate finite-value guard is
 *     applied.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row/element
 *     index, or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_init_quality_set(SWMM_Engine engine, int is_link, int elem_idx,
                                          const char* constituent, double value);

/**
 * @brief Remove an initial-quality row and shift later indices down.
 *
 * Removes the corresponding reaction seed entry when applicable; does not remove or change the
 * global default concentration.
 *
 * @param[in] engine Non-null live engine handle; exclusive access is required.
 * @param[in] entry_idx Zero-based row index in [0,swmm_init_quality_count(engine)).
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADINDEX for an invalid row.
 */
SWMM_ENGINE_API int swmm_init_quality_remove(SWMM_Engine engine, int entry_idx);

/* ---- U2 (2026-09-07): the `[INITIAL_QUALITY] FILE <csv>` sidecar --------
 * Rows read from the sidecar carry is_file = 1 in swmm_init_quality_is_file
 * and are not written back inline; the FILE line is. The CSV format is
 * scope,element,constituent,value per line (comma / whitespace separated,
 * optional header). The file is read at the next open; set/clear here only
 * records the reference.
 */

/**
 * @brief Copy the authored initial-quality sidecar path token.
 *
 * Returns the authored reference rather than resolving or reading the sidecar.
 *
 * @param[in] engine Non-null live engine handle; exclusive access is required.
 * @param[out] buf Non-null caller-owned buffer; empty when absent, NUL-terminated and silently
 *     truncated to buflen-1 bytes.
 * @param[in] buflen Positive capacity in bytes including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row/element
 *     index, or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_init_quality_file_get(SWMM_Engine engine, char* buf, int buflen);

/**
 * @brief Set or clear the authored initial-quality sidecar reference.
 *
 * Does not load, clear or rewrite existing rows immediately; the sidecar is read on the next model
 * open. Sidecar-origin rows retain their provenance for serialization.
 *
 * @param[in] engine Non-null live engine handle; exclusive access is required.
 * @param[in] path Borrowed NUL-terminated path token to copy; NULL/empty clears the reference.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_init_quality_file_set(SWMM_Engine engine, const char* path);

/**
 * @brief Read whether an initial-quality row originated in a sidecar.
 *
 * This Boolean query does not return BADHANDLE/BADINDEX error codes; 0 is not proof that an
 * arbitrary index exists.
 *
 * @param[in] engine Non-null live engine handle; exclusive access is required.
 * @param[in] entry_idx Zero-based row index in [0,swmm_init_quality_count(engine)).
 * @return 1 for a sidecar-origin row, 0 for an inline row, null engine, invalid index or absent
 *     provenance flag.
 */
SWMM_ENGINE_API int swmm_init_quality_is_file(SWMM_Engine engine, int entry_idx);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_INITIAL_QUALITY_H */
