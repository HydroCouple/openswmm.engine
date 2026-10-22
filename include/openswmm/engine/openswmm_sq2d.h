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
 * @file      openswmm_sq2d.h
 * @brief     S7 (2026-09-19) — C API for the 2D surface-quality rows
 *        (`[2D_COVERAGES]`, `[2D_LOADINGS]`, `[2D_CURB_LENGTH]`) and the
 *        cell buildup store.
 *
 * @details Rows are authored per SCOPE — the `*` row, a TAG row (every cell
 *          carrying the tag) or a CELL row — and resolve GLOBAL < TAG < CELL
 *          at initialize, exactly as the `.inp` sections do (one parser, one
 *          rule). A coverage row is the WHOLE land-use set of its scope:
 *          setting it replaces any earlier row with the same scope key.
 *
 *          Row edits need the engine OPENED or BUILDING (SWMM_ERR_LIFECYCLE
 *          otherwise); the read-backs work in every state, and the buildup
 *          store reads back once the run has started.
 *
 *          Units follow the project: percent for coverages, user mass per
 *          acre | hectare for loadings and buildup (lbs | kg; a reactions
 *          species declared in UG scales through its own unit factor), and
 *          the project's length unit for curb lengths.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_SQ2D_H
#define OPENSWMM_SQ2D_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SWMM_SQ2D_SCOPE_GLOBAL 0 /**< the `*` row */
#define SWMM_SQ2D_SCOPE_TAG 1    /**< every cell carrying a tag */
#define SWMM_SQ2D_SCOPE_CELL 2   /**< one cell, 0-based here (1-based in the file) */

/* =========================================================================
 * [2D_COVERAGES]
 * ========================================================================= */

/**
 * @brief Count authored 2D coverage scope rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Non-null caller-owned row-count slot; zero when no rows are authored.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Does not require initialized surface-quality runtime or resolve scope inheritance.
 */
SWMM_ENGINE_API int swmm_2d_coverage_count(SWMM_Engine engine, int* count);

/**
 * @brief Replace the complete land-use coverage set for one scope.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] scope SWMM_SQ2D_SCOPE_GLOBAL, SWMM_SQ2D_SCOPE_TAG or SWMM_SQ2D_SCOPE_CELL.
 * @param[in] tag Required nonempty tag for TAG scope; ignored otherwise and may be NULL.
 * @param[in] cell Nonnegative zero-based cell for CELL scope; ignored otherwise.
 * @param[in] landuses Non-null borrowed array of n non-null existing land-use names.
 * @param[in] percents Non-null borrowed n-element percentage array, validated
 * by the shared coverage parser; percentages sum to at most 100.
 * @param[in] n Positive number of land-use/percentage pairs.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Replaces the whole same-key scope row, rather than adding individual
 * uses. Resolution at initialize follows GLOBAL < TAG < CELL precedence.
 */
SWMM_ENGINE_API int swmm_2d_coverage_set(SWMM_Engine engine, int scope, const char* tag, int cell,
                                         const char* const* landuses, const double* percents,
                                         int n);

/**
 * @brief Count the land-use pairs in an authored coverage row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based authored row index.
 * @param[out] n Non-null caller-owned pair-count slot for that row.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Invalid row indices return SWMM_ERR_BADPARAM; no runtime resolution occurs.
 */
SWMM_ENGINE_API int swmm_2d_coverage_row_size(SWMM_Engine engine, int index, int* n);

/**
 * @brief Copy the scope and one land-use/percentage pair of a coverage row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based authored coverage-row index.
 * @param[in] k Zero-based pair index, below swmm_2d_coverage_row_size().
 * @param[out] scope Optional SWMM_SQ2D_SCOPE_* slot; NULL skips it.
 * @param[out] tag Optional writable tag string buffer.
 * @param[in] taglen Byte capacity; <= 0 skips the buffer, positive values
 * truncate to len - 1 and append NUL.
 * @param[out] cell Optional zero-based cell slot; meaningful for CELL scope.
 * @param[out] landuse Optional writable selected land-use-name buffer.
 * @param[in] lulen Name-buffer byte capacity, with the same skip/truncation convention.
 * @param[out] percent Optional selected coverage percentage slot.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note All outputs may be NULL. Invalid row/pair indices return SWMM_ERR_BADPARAM.
 */
SWMM_ENGINE_API int swmm_2d_coverage_get(SWMM_Engine engine, int index, int k, int* scope,
                                         char* tag, int taglen, int* cell, char* landuse, int lulen,
                                         double* percent);

/**
 * @brief Remove an authored 2D coverage scope row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based current row index; later row indices shift on removal.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Out-of-range row indices return SWMM_ERR_BADPARAM, not SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_2d_coverage_remove(SWMM_Engine engine, int index);

/* =========================================================================
 * [2D_LOADINGS]
 * ========================================================================= */

/**
 * @brief Count authored 2D loading scope rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Non-null caller-owned row-count slot; zero when no rows are authored.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Does not require initialized surface-quality runtime or resolve scope inheritance.
 */
SWMM_ENGINE_API int swmm_2d_loading_count(SWMM_Engine engine, int* count);

/**
 * @brief Upsert a scope/species initial buildup loading.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] scope SWMM_SQ2D_SCOPE_GLOBAL, SWMM_SQ2D_SCOPE_TAG or SWMM_SQ2D_SCOPE_CELL.
 * @param[in] tag Required nonempty tag for TAG scope; ignored otherwise and may be NULL.
 * @param[in] cell Nonnegative zero-based cell for CELL scope; ignored otherwise.
 * @param[in] species Non-null existing pollutant or reactions-species name.
 * @param[in] value Loading in project pollutant mass per acre/hectare;
 * validated by the shared loading parser and retained at exact double precision.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Removes earlier same-scope/species rows and appends the replacement;
 * other species and scopes are preserved.
 */
SWMM_ENGINE_API int swmm_2d_loading_set(SWMM_Engine engine, int scope, const char* tag, int cell,
                                        const char* species, double value);

/**
 * @brief Copy one authored initial-loading scope row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based current row index; invalid indices return SWMM_ERR_BADPARAM.
 * @param[out] scope Optional SWMM_SQ2D_SCOPE_* slot.
 * @param[out] tag Optional writable tag buffer; empty outside TAG scope.
 * @param[in] taglen Byte capacity; <= 0 skips, positive capacities safely truncate with NUL.
 * @param[out] cell Optional zero-based cell slot, meaningful for CELL scope.
 * @param[out] species Optional writable species-name buffer.
 * @param[in] splen Name-buffer byte capacity; <= 0 skips, positive capacities truncate with NUL.
 * @param[out] value Optional authored numeric slot in project pollutant mass per acre/hectare.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note All outputs may be NULL; reads raw authoring without applying scope precedence.
 */
SWMM_ENGINE_API int swmm_2d_loading_get(SWMM_Engine engine, int index, int* scope, char* tag,
                                        int taglen, int* cell, char* species, int splen,
                                        double* value);

/**
 * @brief Remove an authored 2D loading scope row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based current row index; later row indices shift on removal.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Out-of-range row indices return SWMM_ERR_BADPARAM, not SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_2d_loading_remove(SWMM_Engine engine, int index);

/* =========================================================================
 * [2D_CURB_LENGTH]
 * ========================================================================= */

/**
 * @brief Count authored 2D curb-length scope rows.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] count Non-null caller-owned row-count slot; zero when no rows are authored.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Does not require initialized surface-quality runtime or resolve scope inheritance.
 */
SWMM_ENGINE_API int swmm_2d_curb_length_count(SWMM_Engine engine, int* count);

/**
 * @brief Upsert a scope's authored curb length.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] scope SWMM_SQ2D_SCOPE_GLOBAL, SWMM_SQ2D_SCOPE_TAG or SWMM_SQ2D_SCOPE_CELL.
 * @param[in] tag Required nonempty tag for TAG scope; ignored otherwise and may be NULL.
 * @param[in] cell Nonnegative zero-based cell for CELL scope; ignored otherwise.
 * @param[in] length Positive curb length in project length units, validated by the shared parser.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Removes any earlier row with the same scope and appends its replacement.
 */
SWMM_ENGINE_API int swmm_2d_curb_length_set(SWMM_Engine engine, int scope, const char* tag,
                                            int cell, double length);

/**
 * @brief Copy one authored curb-length scope row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based current row index; invalid indices return SWMM_ERR_BADPARAM.
 * @param[out] scope Optional SWMM_SQ2D_SCOPE_* slot.
 * @param[out] tag Optional writable tag buffer; empty outside TAG scope.
 * @param[in] taglen Byte capacity; <= 0 skips, positive capacities safely truncate with NUL.
 * @param[out] cell Optional zero-based cell slot, meaningful for CELL scope.
 * @param[out] length Optional authored numeric slot in project length units.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note All outputs may be NULL; reads raw authoring without applying scope precedence.
 */
SWMM_ENGINE_API int swmm_2d_curb_length_get(SWMM_Engine engine, int index, int* scope, char* tag,
                                            int taglen, int* cell, double* length);

/**
 * @brief Remove an authored 2D curb-length scope row.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] index Zero-based current row index; later row indices shift on removal.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 * @note Out-of-range row indices return SWMM_ERR_BADPARAM, not SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_2d_curb_length_remove(SWMM_Engine engine, int index);

/* =========================================================================
 * Buildup store (running model)
 * ========================================================================= */

/**
 * @brief Read per-cell total land-use buildup for an active surface species.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] species Non-null exact active species name (pollutant or reactions species).
 * @param[out] out Non-null caller-owned array for every active surface-quality cell,
 * receiving project species mass per acre/hectare summed over land uses.
 * @param[in] n Output capacity in doubles, at least the active cell count;
 * only the cell-count prefix is written.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADPARAM for a missing name/
 * buffer, unknown species or short capacity, or SWMM_ERR_LIFECYCLE when the
 * surface-quality runtime is inactive.
 * @note No covering rows, IGNORE_QUALITY or RAINFALL_MODE NONE can leave the
 * runtime inactive. The call does not advance buildup or retain the buffer.
 */
SWMM_ENGINE_API int swmm_2d_get_buildup_bulk(SWMM_Engine engine, const char* species, double* out,
                                             int n);

#ifdef __cplusplus
}
#endif

#endif /* OPENSWMM_SQ2D_H */
