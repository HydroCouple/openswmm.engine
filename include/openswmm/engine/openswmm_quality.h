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
 * @file openswmm_quality.h
 * @brief OpenSWMM Engine — Water Quality (Landuse / Buildup / Washoff / Treatment) C API.
 *
 * @details Provides functions for managing landuse definitions, buildup/washoff
 *          functions per (landuse x pollutant), and treatment expressions per
 *          (node x pollutant).
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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

/** @brief Number of land uses defined. */
SWMM_ENGINE_API int         swmm_landuse_count(SWMM_Engine engine);

/** @brief Look up a land use by name. Returns -1 if not found. */
SWMM_ENGINE_API int         swmm_landuse_index(SWMM_Engine engine, const char* id);

/** @brief Return the name of a land use by index. Returns NULL on error. */
SWMM_ENGINE_API const char* swmm_landuse_id   (SWMM_Engine engine, int idx);

/** @brief Add a new land use (BUILDING state only). */
SWMM_ENGINE_API int swmm_landuse_add(SWMM_Engine engine, const char* id);

/**
 * @brief Rename a land use in place.
 *
 * @details Land uses are referenced positionally (matrix dimensions), so
 *          the rename only touches the name registry — buildup/washoff
 *          rows and subcatchment coverages follow automatically.
 *
 * @param engine  Engine handle.
 * @param idx     Land use index.
 * @param new_id  New unique name (non-empty).
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM on empty/duplicate name.
 */
SWMM_ENGINE_API int swmm_landuse_rename(SWMM_Engine engine, int idx, const char* new_id);

/** @brief Set sweep interval (days between street sweeps). */
SWMM_ENGINE_API int swmm_landuse_set_sweep_interval(SWMM_Engine engine, int idx, double days);

/** @brief Get sweep interval (days between street sweeps). */
SWMM_ENGINE_API int swmm_landuse_get_sweep_interval(SWMM_Engine engine, int idx, double* days);

/** @brief Set sweep removal fraction (0-1). */
SWMM_ENGINE_API int swmm_landuse_set_sweep_removal(SWMM_Engine engine, int idx, double frac);

/** @brief Get sweep removal fraction (0-1). */
SWMM_ENGINE_API int swmm_landuse_get_sweep_removal(SWMM_Engine engine, int idx, double* frac);

/* =========================================================================
 * Buildup API — indexed by (landuse_idx, pollutant_idx)
 * ========================================================================= */

/**
 * @brief Set buildup function parameters for a (landuse, pollutant) pair.
 *
 * @param engine     Engine handle.
 * @param lu_idx     Landuse index.
 * @param pollut_idx Pollutant index.
 * @param func_type  Buildup function type (0=NONE, 1=POW, 2=EXP, 3=SAT, 4=EXT).
 * @param c1         Coefficient 1.
 * @param c2         Coefficient 2.
 * @param c3         Coefficient 3.
 * @param normalizer Normalizer (0=PER_AREA, 1=PER_CURB).
 */
SWMM_ENGINE_API int swmm_buildup_set(SWMM_Engine engine, int lu_idx, int pollut_idx,
                                       int func_type, double c1, double c2, double c3,
                                       int normalizer);

/**
 * @brief Get buildup function parameters for a (landuse, pollutant) pair.
 *
 * @param engine     Engine handle.
 * @param lu_idx     Landuse index.
 * @param pollut_idx Pollutant index.
 * @param[out] func_type  Buildup function type (0=NONE, 1=POW, 2=EXP, 3=SAT, 4=EXT).
 * @param[out] c1         Coefficient 1.
 * @param[out] c2         Coefficient 2.
 * @param[out] c3         Coefficient 3.
 * @param[out] normalizer Normalizer (0=PER_AREA, 1=PER_CURB).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_buildup_get(SWMM_Engine engine, int lu_idx, int pollut_idx,
                                       int* func_type, double* c1, double* c2, double* c3,
                                       int* normalizer);

/* =========================================================================
 * Washoff API — indexed by (landuse_idx, pollutant_idx)
 * ========================================================================= */

/**
 * @brief Set washoff function parameters for a (landuse, pollutant) pair.
 *
 * @param engine       Engine handle.
 * @param lu_idx       Landuse index.
 * @param pollut_idx   Pollutant index.
 * @param func_type    Washoff function type (0=NONE, 1=EXP, 2=RC, 3=EMC).
 * @param coeff        Washoff coefficient.
 * @param expon        Washoff exponent.
 * @param sweep_effic  Sweep efficiency (0-100).
 * @param bmp_effic    BMP efficiency (0-100).
 */
SWMM_ENGINE_API int swmm_washoff_set(SWMM_Engine engine, int lu_idx, int pollut_idx,
                                       int func_type, double coeff, double expon,
                                       double sweep_effic, double bmp_effic);

/**
 * @brief Get washoff function parameters for a (landuse, pollutant) pair.
 *
 * @param engine       Engine handle.
 * @param lu_idx       Landuse index.
 * @param pollut_idx   Pollutant index.
 * @param[out] func_type    Washoff function type (0=NONE, 1=EXP, 2=RC, 3=EMC).
 * @param[out] coeff        Washoff coefficient.
 * @param[out] expon        Washoff exponent.
 * @param[out] sweep_effic  Sweep efficiency (0-100).
 * @param[out] bmp_effic    BMP efficiency (0-100).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_washoff_get(SWMM_Engine engine, int lu_idx, int pollut_idx,
                                       int* func_type, double* coeff, double* expon,
                                       double* sweep_effic, double* bmp_effic);

/* =========================================================================
 * Treatment API — indexed by (node_idx, pollutant_idx)
 * ========================================================================= */

/**
 * @brief Set a treatment expression for a (node, pollutant) pair.
 *
 * @param engine     Engine handle.
 * @param node_idx   Node index.
 * @param pollut_idx Pollutant index.
 * @param expression Treatment expression string (e.g. "R = 0.5 * exp(-0.1 * DT)").
 */
SWMM_ENGINE_API int swmm_treatment_set(SWMM_Engine engine, int node_idx, int pollut_idx,
                                         const char* expression);

/**
 * @brief Get a treatment expression for a (node, pollutant) pair.
 *
 * @param engine     Engine handle.
 * @param node_idx   Node index.
 * @param pollut_idx Pollutant index.
 * @param buf        Buffer to receive the expression string.
 * @param buflen     Size of buf.
 */
SWMM_ENGINE_API int swmm_treatment_get(SWMM_Engine engine, int node_idx, int pollut_idx,
                                         char* buf, int buflen);

/**
 * @brief Clear a treatment expression for a (node, pollutant) pair.
 *
 * @param engine     Engine handle.
 * @param node_idx   Node index.
 * @param pollut_idx Pollutant index.
 * @returns SWMM_OK on success, or an error code.
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
 * @param engine        Engine handle (grammar only; no model data is read).
 * @param expr          Expression string ("R = ..." or "C = ...").
 * @param[out] errbuf   Receives the diagnostic on failure ("" on success).
 *                      May be NULL.
 * @param buflen        Size of errbuf in bytes.
 * @param[out] col_out  Receives the 0-based character offset of the error,
 *                      or -1 when not attributable. May be NULL.
 * @returns SWMM_OK when the expression is valid, SWMM_ERR_BADPARAM when
 *          invalid (diagnostic in errbuf/col_out) or on NULL expr.
 */
SWMM_ENGINE_API int swmm_treatment_validate_expression(SWMM_Engine engine, const char* expr,
                                                        char* errbuf, int buflen, int* col_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_QUALITY_H */
