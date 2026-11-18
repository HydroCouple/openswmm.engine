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
 * @file      openswmm_quality.h
 * @brief     OpenSWMM Engine — Water Quality (Landuse / Buildup / Washoff / Treatment) C API.
 *
 * @details Provides functions for managing landuse definitions, buildup/washoff
 *          functions per (landuse x pollutant), and treatment expressions per
 *          (node x pollutant).
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_H
#define OPENSWMM_QUALITY_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Landuse API
 * ========================================================================= */

/**
 * @brief Count land-use definitions in the current model.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access.
 * @return Nonnegative count, or -1 for null engine.
 */
SWMM_ENGINE_API int swmm_landuse_count(SWMM_Engine engine);

/**
 * @brief Resolve a land-use identifier to its row index.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access.
 * @param[in] id Non-null NUL-terminated identifier, borrowed for the lookup.
 * @return Zero-based index, or -1 for null engine/ID or absent name.
 */
SWMM_ENGINE_API int swmm_landuse_index(SWMM_Engine engine, const char* id);

/**
 * @brief Borrow a land-use identifier from the model name index.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access.
 * @param[in] idx Zero-based existing land-use index.
 * @return Engine-owned NUL-terminated ID, or NULL for invalid handle/index. Never free it; copy
 *     before name/topology edits, close/reopen or destruction.
 */
SWMM_ENGINE_API const char* swmm_landuse_id(SWMM_Engine engine, int idx);

/**
 * @brief Append a land use while preserving existing model parameter columns.
 *
 * Grows sweeping and buildup/washoff rows and repacks subcatchment land-use coverage when its
 * existing dimensions match. New rows default to zero. Configure parameters/coverage before
 * initialization.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access.
 * @param[in] id Non-null copied identifier; an existing land-use ID is refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for null/duplicate ID.
 */
SWMM_ENGINE_API int swmm_landuse_add(SWMM_Engine engine, const char* id);

/**
 * @brief Rename a land use in place.
 *
 * @details Land uses are referenced positionally (matrix dimensions), so
 *          the rename only touches the name registry — buildup/washoff
 *          rows and subcatchment coverages follow automatically.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Land use index.
 * @param[in] new_id New unique name (non-empty).
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM on empty/duplicate name.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed land-use/node/pollutant/pollutant index check returns
 *     SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_landuse_rename(SWMM_Engine engine, int idx, const char* new_id);

/**
 * @brief Assign the configured interval between street sweeps.
 *
 * No lifecycle guard is imposed. This stores the parameter without rescheduling a previously
 * completed sweep.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access.
 * @param[in] idx Zero-based existing land-use index.
 * @param[in] days Interval in days; stored without finite/nonnegative validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid land-use
 *     index.
 */
SWMM_ENGINE_API int swmm_landuse_set_sweep_interval(SWMM_Engine engine, int idx, double days);

/**
 * @brief Read the configured interval between street sweeps.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access.
 * @param[in] idx Zero-based existing land-use index.
 * @param[out] days Optional destination in days.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid land-use
 *     index.
 */
SWMM_ENGINE_API int swmm_landuse_get_sweep_interval(SWMM_Engine engine, int idx, double* days);

/**
 * @brief Assign the land-use sweep-removal fraction.
 *
 * No lifecycle guard is imposed.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access.
 * @param[in] idx Zero-based existing land-use index.
 * @param[in] frac Dimensionless removal fraction, conventionally [0,1]; stored without
 *     clamping/finite/range checks.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid land-use
 *     index.
 */
SWMM_ENGINE_API int swmm_landuse_set_sweep_removal(SWMM_Engine engine, int idx, double frac);

/**
 * @brief Read the land-use sweep-removal fraction.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access.
 * @param[in] idx Zero-based existing land-use index.
 * @param[out] frac Optional destination for the stored dimensionless fraction.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid land-use
 *     index.
 */
SWMM_ENGINE_API int swmm_landuse_get_sweep_removal(SWMM_Engine engine, int idx, double* frac);

/* =========================================================================
 * Buildup API — indexed by (landuse_idx, pollutant_idx)
 * ========================================================================= */

/**
 * @brief Set buildup function parameters for a (landuse, pollutant) pair.
 *
 * @param[in] engine Engine handle.
 * @param[in] lu_idx Landuse index.
 * @param[in] pollut_idx Surface-species index: pollutant, or n_pollutants + reaction species index.
 * @param[in] func_type Buildup function type (0=NONE, 1=POW, 2=EXP, 3=SAT, 4=EXT).
 * @param[in] c1 Coefficient 1.
 * @param[in] c2 Coefficient 2.
 * @param[in] c3 Coefficient 3.
 * @param[in] normalizer Normalizer (0=PER_AREA, 1=PER_CURB).
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed land-use/node/pollutant/pollutant index
 *     check returns SWMM_ERR_BADINDEX. No lifecycle guard is imposed. Coefficients, enum selectors
 *     and percentage/fraction ranges are stored without separate validation; units and meanings
 *     follow the selected input function. Pollutant rows refresh the runtime land-use cache for
 *     subsequent steps.
 */
SWMM_ENGINE_API int swmm_buildup_set(SWMM_Engine engine, int lu_idx, int pollut_idx, int func_type,
                                     double c1, double c2, double c3, int normalizer);

/**
 * @brief Get buildup function parameters for a (landuse, pollutant) pair.
 *
 * @param[in] engine Engine handle.
 * @param[in] lu_idx Landuse index.
 * @param[in] pollut_idx Surface-species index: a pollutant index, or
 *                   `n_pollutants + m` for the reactions component's m-th
 *                   species (BW-MSX, 2026-09-19 — MSX species build up and
 *                   wash off by the same relations).
 * @param[out] func_type Buildup function type (0=NONE, 1=POW, 2=EXP, 3=SAT, 4=EXT).
 * @param[out] c1 Coefficient 1.
 * @param[out] c2 Coefficient 2.
 * @param[out] c3 Coefficient 3.
 * @param[out] normalizer Normalizer (0=PER_AREA, 1=PER_CURB).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note func_type may be NULL to suppress that output. c1 may be NULL to suppress that output. c2
 *     may be NULL to suppress that output. c3 may be NULL to suppress that output. normalizer may
 *     be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     land-use/node/pollutant/pollutant index check returns SWMM_ERR_BADINDEX. pollut_idx addresses
 *     a pollutant row or n_pollutants+reaction_species_index. Reaction rows with no dimensioned
 *     parameter store read as zero/NONE; missing native pollutant matrix entries return BADINDEX.
 */
SWMM_ENGINE_API int swmm_buildup_get(SWMM_Engine engine, int lu_idx, int pollut_idx, int* func_type,
                                     double* c1, double* c2, double* c3, int* normalizer);

/* =========================================================================
 * Washoff API — indexed by (landuse_idx, pollutant_idx)
 * ========================================================================= */

/**
 * @brief Set washoff function parameters for a (landuse, pollutant) pair.
 *
 * @param[in] engine Engine handle.
 * @param[in] lu_idx Landuse index.
 * @param[in] pollut_idx Surface-species index (pollutant, or `n_pollutants + m`
 *                     for the m-th reactions-component species — BW-MSX).
 * @param[in] func_type Washoff function type (0=NONE, 1=EXP, 2=RC, 3=EMC).
 * @param[in] coeff Washoff coefficient.
 * @param[in] expon Washoff exponent.
 * @param[in] sweep_effic Sweep efficiency (0-100).
 * @param[in] bmp_effic BMP efficiency (0-100).
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed land-use/node/pollutant/pollutant index
 *     check returns SWMM_ERR_BADINDEX. No lifecycle guard is imposed. Coefficients, enum selectors
 *     and percentage/fraction ranges are stored without separate validation; units and meanings
 *     follow the selected input function. Pollutant rows refresh the runtime land-use cache for
 *     subsequent steps.
 */
SWMM_ENGINE_API int swmm_washoff_set(SWMM_Engine engine, int lu_idx, int pollut_idx, int func_type,
                                     double coeff, double expon, double sweep_effic,
                                     double bmp_effic);

/**
 * @brief Get washoff function parameters for a (landuse, pollutant) pair.
 *
 * @param[in] engine Engine handle.
 * @param[in] lu_idx Landuse index.
 * @param[in] pollut_idx Surface-species index: pollutant, or n_pollutants + reaction species index.
 * @param[out] func_type Washoff function type (0=NONE, 1=EXP, 2=RC, 3=EMC).
 * @param[out] coeff Washoff coefficient.
 * @param[out] expon Washoff exponent.
 * @param[out] sweep_effic Sweep efficiency (0-100).
 * @param[out] bmp_effic BMP efficiency (0-100).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note func_type may be NULL to suppress that output. coeff may be NULL to suppress that output.
 *     expon may be NULL to suppress that output. sweep_effic may be NULL to suppress that output.
 *     bmp_effic may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed land-use/node/pollutant/pollutant index check returns SWMM_ERR_BADINDEX. pollut_idx
 *     addresses a pollutant row or n_pollutants+reaction_species_index. Reaction rows with no
 *     dimensioned parameter store read as zero/NONE; missing native pollutant matrix entries return
 *     BADINDEX.
 */
SWMM_ENGINE_API int swmm_washoff_get(SWMM_Engine engine, int lu_idx, int pollut_idx, int* func_type,
                                     double* coeff, double* expon, double* sweep_effic,
                                     double* bmp_effic);

/* =========================================================================
 * Treatment API — indexed by (node_idx, pollutant_idx)
 * ========================================================================= */

/**
 * @brief Set a treatment expression for a (node, pollutant) pair.
 *
 * @param[in] engine Engine handle.
 * @param[in] node_idx Node index.
 * @param[in] pollut_idx Pollutant index.
 * @param[in] expression Treatment expression string (e.g. "R = 0.5 * exp(-0.1 * DT)").
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed land-use/node/pollutant/pollutant index
 *     check returns SWMM_ERR_BADINDEX. expression must be non-null or BADPARAM is returned. Copies
 *     and compiles the expression immediately, including mid-run; parser refusal restores the
 *     previous text/compiled cell and returns BADPARAM. Does not install an uncompiled text-only
 *     placeholder.
 */
SWMM_ENGINE_API int swmm_treatment_set(SWMM_Engine engine, int node_idx, int pollut_idx,
                                       const char* expression);

/**
 * @brief Get a treatment expression for a (node, pollutant) pair.
 *
 * @param[in] engine Engine handle.
 * @param[in] node_idx Node index.
 * @param[in] pollut_idx Pollutant index.
 * @param[out] buf Buffer to receive the expression string.
 * @param[in] buflen Size of buf.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed land-use/node/pollutant/pollutant index
 *     check returns SWMM_ERR_BADINDEX. buf must be non-null and buflen positive or BADPARAM is
 *     returned. Empty when no stored expression exists; NUL-terminated and silently truncated to
 *     buflen-1 bytes.
 */
SWMM_ENGINE_API int swmm_treatment_get(SWMM_Engine engine, int node_idx, int pollut_idx, char* buf,
                                       int buflen);

/**
 * @brief Clear a treatment expression for a (node, pollutant) pair.
 *
 * @param[in] engine Engine handle.
 * @param[in] node_idx Node index.
 * @param[in] pollut_idx Pollutant index.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed land-use/node/pollutant/pollutant index
 *     check returns SWMM_ERR_BADINDEX. No lifecycle guard is imposed. Clearing an unset valid cell
 *     succeeds; clears/recompiles the cached treatment so the next step sees the removal.
 */
SWMM_ENGINE_API int swmm_treatment_clear(SWMM_Engine engine, int node_idx, int pollut_idx);

/**
 * @brief Validate a treatment expression WITHOUT modifying engine state.
 *
 * @details Editor-support peer of swmm_control_validate_rule: parses
 *          \p expr through the production treatment grammar and reports a
 *          human-readable diagnostic plus the 0-based character offset of
 *          the error. Unlike swmm_treatment_set (which recompiles the
 *          node's treatment on success), this call never mutates the
 *          engine, so it is safe to invoke per keystroke. Note the
 *          validator is deliberately stricter than the tokenizer in one
 *          way: unknown characters are an error rather than silently
 *          skipped. Co-pollutant references (R_name/C_name) are reported
 *          as unsupported.
 *
 * @param[in] engine Engine handle (grammar only; no model data is read).
 * @param[in] expr Expression string ("R = ..." or "C = ...").
 * @param[out] errbuf Receives the diagnostic on failure ("" on success).
 *                      May be NULL.
 * @param[in] buflen Size of errbuf in bytes.
 * @param[out] col_out Receives the 0-based character offset of the error,
 *                      or -1 when not attributable. May be NULL.
 * @returns SWMM_OK when the expression is valid, SWMM_ERR_BADPARAM when
 *          invalid (diagnostic in errbuf/col_out) or on NULL expr.
 *
 *
 * @note col_out may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. The
 *     engine must still be non-null or BADHANDLE is returned. Optional errbuf is
 *     NUL-terminated/truncated only when buflen>0; a nonpositive length suppresses message copying.
 *     Sets col_out=-1 before validating, including null-expression refusal.
 */
SWMM_ENGINE_API int swmm_treatment_validate_expression(SWMM_Engine engine, const char* expr,
                                                       char* errbuf, int buflen, int* col_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_QUALITY_H */
