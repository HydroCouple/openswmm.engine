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
 * @file      openswmm_subcatchments.h
 * @brief     OpenSWMM Engine — Subcatchment C API.
 *
 * @details Subcatchment add (BUILDING state), property setters, state get,
 *          rainfall forcing, bulk access, quality.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_SUBCATCHMENTS_H
#define OPENSWMM_SUBCATCHMENTS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the total number of subcatchments in the model.
 * @param[in] engine Engine handle.
 * @returns Number of subcatchments, or -1 on error.
 *
 *
 * @note Null engine returns -1.
 */
SWMM_ENGINE_API int swmm_subcatch_count(SWMM_Engine engine);

/**
 * @brief Look up a subcatchment's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated subcatchment identifier.
 * @returns Zero-based index, or -1 if not found.
 *
 *
 * @note Case-insensitive name lookup; null engine/name or no match returns -1.
 */
SWMM_ENGINE_API int swmm_subcatch_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a subcatchment by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note Returned text borrows engine storage; do not free. Copy before a registry/table edit or
 *     engine close/destruction. Null engine/invalid index returns NULL.
 */
SWMM_ENGINE_API const char* swmm_subcatch_id(SWMM_Engine engine, int idx);

/* =========================================================================
 * Creation (BUILDING or OPENED — "editable" states)
 * ========================================================================= */

/**
 * @brief Add a new subcatchment to the model.
 *
 * @details The engine must be in SWMM_STATE_BUILDING or SWMM_STATE_OPENED.
 *          Returns SWMM_ERR_LIFECYCLE for any other state. After creation,
 *          use the property setters to configure area, slope, imperviousness,
 *          etc.
 *
 * @param[in] engine Engine handle.
 * @param[in] id Unique null-terminated identifier for the new subcatchment.
 * @returns SWMM_OK on success, SWMM_ERR_LIFECYCLE if not in an editable
 *          state, or another error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. Copies ID and rejects case-insensitive duplicates; NULL is refused, but
 *     this wrapper has no explicit empty-ID check.
 */
SWMM_ENGINE_API int swmm_subcatch_add(SWMM_Engine engine, const char* id);

/* =========================================================================
 * Property setters (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set the outlet node that receives runoff from this subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] node_idx Zero-based node index of the receiving node.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 *     Only the owning subcatchment index is checked; the supplied target reference is stored
 *     without target range, cycle or cross-field validation.
 */
SWMM_ENGINE_API int swmm_subcatch_set_outlet(SWMM_Engine engine, int idx, int node_idx);

/**
 * @brief Set the subcatchment area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] area Area in project area units (acres or hectares).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_area(SWMM_Engine engine, int idx, double area);

/**
 * @brief Set the characteristic overland flow width.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] width Width in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_width(SWMM_Engine engine, int idx, double width);

/**
 * @brief Set the average surface slope.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] slope Slope as a percentage (e.g., 2.0 = 2%).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_slope(SWMM_Engine engine, int idx, double slope);

/**
 * @brief Set the percentage of impervious area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] pct Imperviousness as a percentage (0–100).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_imperv_pct(SWMM_Engine engine, int idx, double pct);

/**
 * @brief Set the percentage of the impervious area having no depression storage.
 * @details The [SUBAREAS] @c PctZero column; legacy @c Subcatch[i].fracImperv2.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] pct Zero-depression-storage impervious area as a percentage
 *                (0–100) of the impervious area.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_zero_imperv_pct(SWMM_Engine engine, int idx, double pct);

/**
 * @brief Set Manning's n for the impervious area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] n Manning's roughness coefficient.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_n_imperv(SWMM_Engine engine, int idx, double n);

/**
 * @brief Set Manning's n for the pervious area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] n Manning's roughness coefficient.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_n_perv(SWMM_Engine engine, int idx, double n);

/**
 * @brief Set the depression storage depth for the impervious area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] ds Depression storage in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_ds_imperv(SWMM_Engine engine, int idx, double ds);

/**
 * @brief Set the depression storage depth for the pervious area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] ds Depression storage in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_ds_perv(SWMM_Engine engine, int idx, double ds);

/**
 * @brief Assign a rain gage to a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] gage_idx Zero-based rain gage index.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 *     Only the owning subcatchment index is checked; the supplied target reference is stored
 *     without target range, cycle or cross-field validation.
 */
SWMM_ENGINE_API int swmm_subcatch_set_gage(SWMM_Engine engine, int idx, int gage_idx);

/* --- Infiltration parameters (BUILDING or OPENED) --- */

/**
 * @brief Select ordinary Horton infiltration and store its input parameters.
 *
 * Requires BUILDING/OPENED. Sets model=0 and parameter slots 1..4 without finite/range checks;
 * leaves the fifth slot unchanged. Selecting a modified model later requires
 * swmm_subcatch_set_infil_model().
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] f0 Initial maximum capacity, inches/hour or mm/hour.
 * @param[in] fmin Minimum capacity, inches/hour or mm/hour.
 * @param[in] decay Exponential capacity decay constant, per hour.
 * @param[in] dry_time Drying/regeneration time, days.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_subcatch_set_infil_horton(SWMM_Engine engine, int idx, double f0,
                                                   double fmin, double decay, double dry_time);

/**
 * @brief Select ordinary Green-Ampt infiltration and store input parameters.
 *
 * Requires BUILDING/OPENED. Sets model=2 and slots 1..3 without finite/range checks; later
 * parameter slots remain unchanged.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] suction Wetting-front suction head, inches/mm.
 * @param[in] conductivity Saturated hydraulic conductivity, inches/hour or mm/hour.
 * @param[in] initial_deficit Initial volumetric soil-moisture deficit fraction.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_subcatch_set_infil_green_ampt(SWMM_Engine engine, int idx, double suction,
                                                       double conductivity, double initial_deficit);

/**
 * @brief Set Curve Number infiltration parameters.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] cn SCS curve number (clamped to 10–99 by the runoff solver).
 * @param[in] drying_time Days for a fully saturated soil to dry, matching the
 *                     third [INFILTRATION] column and legacy
 *                     @c curvenum_setParams p[2]. Drives the regeneration
 *                     constant; a value <= 0 disables regeneration.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_infil_curve_number(SWMM_Engine engine, int idx, double cn,
                                                         double drying_time);

/**
 * @brief Set ONLY the infiltration model code for a subcatchment.
 *
 * @details model: 0=HORTON, 1=MOD_HORTON, 2=GREEN_AMPT, 3=MOD_GREEN_AMPT,
 *          4=CURVE_NUMBER. The per-model parameters are positionally
 *          overloaded, so after switching the model code callers should set
 *          the matching parameters via swmm_subcatch_set_infil_horton /
 *          _green_ampt / _curve_number.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] model Infiltration model code (0..4).
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM if model is out of range.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_set_infil_model(SWMM_Engine engine, int idx, int model);

/* =========================================================================
 * Property getters
 * ========================================================================= */

/**
 * @brief Get the subcatchment area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] area Receives the area in project area units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note area may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_area(SWMM_Engine engine, int idx, double* area);

/**
 * @brief Get the percentage of impervious area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] pct Receives the imperviousness percentage (0–100).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note pct may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_imperv_pct(SWMM_Engine engine, int idx, double* pct);

/**
 * @brief Get the percentage of the impervious area having no depression storage.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] pct Receives the zero-depression-storage percentage (0–100).
 * @returns SWMM_OK on success, or an error code.
 * @see swmm_subcatch_set_zero_imperv_pct
 *
 *
 * @note pct may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_zero_imperv_pct(SWMM_Engine engine, int idx, double* pct);

/**
 * @brief Get the outlet node index for a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] node_idx Receives the outlet node index.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note node_idx may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_outlet(SWMM_Engine engine, int idx, int* node_idx);

/**
 * @brief Get the characteristic overland flow width.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] w Receives the width in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note w may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_width(SWMM_Engine engine, int idx, double* w);

/**
 * @brief Get the average surface slope.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] s Receives the slope percentage.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note s may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_slope(SWMM_Engine engine, int idx, double* s);

/**
 * @brief Get Manning's n for the impervious area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] n Receives Manning's n.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note n may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_n_imperv(SWMM_Engine engine, int idx, double* n);

/**
 * @brief Get Manning's n for the pervious area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] n Receives Manning's n.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note n may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_n_perv(SWMM_Engine engine, int idx, double* n);

/**
 * @brief Get the depression storage depth for the impervious area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] ds Receives the depression storage depth.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note ds may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_ds_imperv(SWMM_Engine engine, int idx, double* ds);

/**
 * @brief Get the depression storage depth for the pervious area.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] ds Receives the depression storage depth.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note ds may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_ds_perv(SWMM_Engine engine, int idx, double* ds);

/**
 * @brief Get the rain gage index assigned to a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] gage_idx Receives the gage index.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note gage_idx may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_gage(SWMM_Engine engine, int idx, int* gage_idx);

/**
 * @brief Set a subcatchment's outlet to another subcatchment (cascading).
 *
 * @details When a subcatchment drains to another subcatchment instead of
 *          directly to a node, use this function. Mutually exclusive with
 *          swmm_subcatch_set_outlet().
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] sc_idx Zero-based index of the receiving subcatchment.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 *     Only the owning subcatchment index is checked; the supplied target reference is stored
 *     without target range, cycle or cross-field validation.
 */
SWMM_ENGINE_API int swmm_subcatch_set_outlet_subcatch(SWMM_Engine engine, int idx, int sc_idx);

/**
 * @brief Get the downstream subcatchment index (for cascading outlets).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] sc_idx Receives the downstream subcatchment index, or -1 if none.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note sc_idx may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_outlet_subcatch(SWMM_Engine engine, int idx, int* sc_idx);

/* =========================================================================
 * Infiltration getters
 * ========================================================================= */

/**
 * @brief Get the infiltration model type for a subcatchment.
 *
 * @details Returns an integer code: 0=HORTON, 1=MOD_HORTON, 2=GREEN_AMPT,
 *          3=MOD_GREEN_AMPT, 4=CURVE_NUMBER.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] model Receives the infiltration model code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note model may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil_model(SWMM_Engine engine, int idx, int* model);

/**
 * @brief Get Horton infiltration parameters for a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] f0 Receives the maximum infiltration rate.
 * @param[out] fmin Receives the minimum infiltration rate.
 * @param[out] decay Receives the decay constant (1/hr).
 * @param[out] dry_time Receives the time to fully dry (hours).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note f0 may be NULL to suppress that output. fmin may be NULL to suppress that output. decay may
 *     be NULL to suppress that output. dry_time may be NULL to suppress that output. A null engine
 *     returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Reads the overloaded input slots without checking the active infiltration
 *     model; select the getter matching swmm_subcatch_get_infil_model().
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil_horton(SWMM_Engine engine, int idx, double* f0,
                                                   double* fmin, double* decay, double* dry_time);

/**
 * @brief Get Green–Ampt infiltration parameters for a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] suction Receives the soil capillary suction head.
 * @param[out] conductivity Receives the saturated hydraulic conductivity.
 * @param[out] deficit Receives the initial moisture deficit (fraction).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note suction may be NULL to suppress that output. conductivity may be NULL to suppress that
 *     output. deficit may be NULL to suppress that output. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 *     Reads the overloaded input slots without checking the active infiltration model; select the
 *     getter matching swmm_subcatch_get_infil_model().
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil_green_ampt(SWMM_Engine engine, int idx, double* suction,
                                                       double* conductivity, double* deficit);

/**
 * @brief Get the Curve Number infiltration parameters for a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] cn Receives the SCS curve number. May be NULL.
 * @param[out] drying_time Receives the drying time in days. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 * @see swmm_subcatch_set_infil_curve_number
 *
 *
 * @note cn may be NULL to suppress that output. drying_time may be NULL to suppress that output. A
 *     null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Reads the overloaded input slots without checking the active infiltration
 *     model; select the getter matching swmm_subcatch_get_infil_model().
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil_curve_number(SWMM_Engine engine, int idx, double* cn,
                                                         double* drying_time);

/* =========================================================================
 * Subcatchment statistics
 * ========================================================================= */

/**
 * @brief Get the total precipitation volume at a subcatchment.
 * @param[in] engine Engine handle (ENDED or RUNNING state).
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] vol Receives the precipitation volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note vol may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_stat_precip(SWMM_Engine engine, int idx, double* vol);

/**
 * @brief Get the total runoff volume from a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] vol Receives the runoff volume in project volume units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note vol may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_stat_runoff_vol(SWMM_Engine engine, int idx, double* vol);

/**
 * @brief Get the maximum runoff rate from a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] rate Receives the maximum runoff rate in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note rate may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_stat_max_runoff(SWMM_Engine engine, int idx, double* rate);

/* =========================================================================
 * Subcatchment landuse coverage
 * ========================================================================= */

/**
 * @brief Set the land use coverage percent for a subcatchment.
 *
 * @details Assigns what percent of a subcatchment's area is covered by
 *          a particular land use category (for buildup/washoff modeling).
 *          Stored and reported verbatim in PERCENT (0–100), matching the
 *          [COVERAGES] .inp convention.
 *
 * @param[in] engine Engine handle.
 * @param[in] sc_idx Zero-based subcatchment index.
 * @param[in] lu_idx Zero-based land use index.
 * @param[in] fraction Coverage in percent (0–100).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here.
 */
SWMM_ENGINE_API int swmm_subcatch_set_coverage(SWMM_Engine engine, int sc_idx, int lu_idx,
                                               double fraction);

/**
 * @brief Get the land use coverage percent for a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] sc_idx Zero-based subcatchment index.
 * @param[in] lu_idx Zero-based land use index.
 * @param[out] fraction Receives the coverage in percent (0–100).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note fraction may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_coverage(SWMM_Engine engine, int sc_idx, int lu_idx,
                                               double* fraction);

/**
 * @brief Get all land use coverage percents for a subcatchment in one call.
 *
 * @details Bulk peer of swmm_subcatch_get_coverage for grid editors: fills
 *          out[0..n-1] with the coverage percent for land uses 0..n-1.
 *          n must not exceed the engine's land use count.
 *
 * @param[in] engine Engine handle.
 * @param[in] sc_idx Zero-based subcatchment index.
 * @param[out] out Caller-owned array receiving n percents (0–100).
 * @param[in] n Number of land uses to fetch (array capacity).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Requires non-null out even for n=0. n must be in [0,landuse count]
 *     or BADINDEX. Writes exactly n prefix percentages; unsized coverage returns zeros.
 */
SWMM_ENGINE_API int swmm_subcatch_get_coverages(SWMM_Engine engine, int sc_idx, double* out, int n);

/* =========================================================================
 * Initial pollutant loadings ([LOADINGS])
 * ========================================================================= */

/**
 * @brief Set the initial pollutant buildup on a subcatchment.
 *
 * @details The [LOADINGS] value: initial buildup mass per unit area
 *          (normalizer units) present at the simulation start, overriding
 *          the DRY_DAYS-derived buildup for the pollutant.
 *
 * @param[in] engine Engine handle.
 * @param[in] sc_idx Zero-based subcatchment index.
 * @param[in] pollut_idx Zero-based pollutant index.
 * @param[in] buildup Initial buildup (mass per unit area).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. No
 *     independent finite/range validation of the numeric inputs is performed here. Constituent
 *     indices 0..np-1 select pollutants; np+m selects reaction species m. This is an initial
 *     authoring loading, not current runoff concentration; unset storage reads zero.
 */
SWMM_ENGINE_API int swmm_subcatch_set_initial_loading(SWMM_Engine engine, int sc_idx,
                                                      int pollut_idx, double buildup);

/**
 * @brief Get the initial pollutant buildup on a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] sc_idx Zero-based subcatchment index.
 * @param[in] pollut_idx Zero-based pollutant index.
 * @param[out] buildup Receives the initial buildup (mass per unit area).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note buildup may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. Constituent indices
 *     0..np-1 select pollutants; np+m selects reaction species m. This is an initial authoring
 *     loading, not current runoff concentration; unset storage reads zero.
 */
SWMM_ENGINE_API int swmm_subcatch_get_initial_loading(SWMM_Engine engine, int sc_idx,
                                                      int pollut_idx, double* buildup);

/* =========================================================================
 * Hydraulic state getters
 * ========================================================================= */

/**
 * @brief Get the current runoff rate from a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] runoff Receives the runoff rate in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note runoff may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_runoff(SWMM_Engine engine, int idx, double* runoff);

/**
 * @brief Get the current groundwater flow from a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] gw_flow Receives the groundwater flow in project flow units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note gw_flow may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_groundwater(SWMM_Engine engine, int idx, double* gw_flow);

/**
 * @brief Get the current rainfall intensity at a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] rainfall Receives the rainfall in project rate units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note rainfall may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_rainfall(SWMM_Engine engine, int idx, double* rainfall);

/**
 * @brief Get the current snow depth on a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] depth Receives the snow depth in project length units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note depth may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_snow_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Get the current evaporation rate at a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] evap Receives the evaporation rate.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note evap may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_evap(SWMM_Engine engine, int idx, double* evap);

/* =========================================================================
 * Groundwater configuration ([GROUNDWATER])
 * =========================================================================
 * Configure a subcatchment's [GROUNDWATER] flow routing. Values are stored
 * exactly as parsed from the input file (raw user units) so they round-trip
 * identically. Distinct from the runtime gw STATE (theta / lower_depth)
 * injected via swmm_subcatch_set_gw_state. All are BUILDING/OPENED editable.
 */

/**
 * @brief Assign the aquifer used for groundwater.
 *
 * Requires BUILDING/OPENED. Stores the reference verbatim without changing current groundwater
 * state.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] aquifer_idx Existing zero-based target index or -1 for none; the target range is NOT
 *     checked by this wrapper.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_subcatch_set_aquifer(SWMM_Engine engine, int idx, int aquifer_idx);

/**
 * @brief Read the assigned aquifer used for groundwater.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[out] aquifer_idx Optional stored target-index output; -1 denotes none.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_aquifer(SWMM_Engine engine, int idx, int* aquifer_idx);

/**
 * @brief Assign the node receiving groundwater.
 *
 * Requires BUILDING/OPENED. Stores the reference verbatim without changing current groundwater
 * state.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] node_idx Existing zero-based target index or -1 for none; the target range is NOT
 *     checked by this wrapper.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_subcatch_set_gw_node(SWMM_Engine engine, int idx, int node_idx);

/**
 * @brief Read the assigned node receiving groundwater.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[out] node_idx Optional stored target-index output; -1 denotes none.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_gw_node(SWMM_Engine engine, int idx, int* node_idx);

/**
 * @brief Assign the input groundwater flow-law parameters.
 *
 * Values retain input-file units. Requires BUILDING/OPENED. Stores values without finite/range
 * validation; does not inject runtime groundwater state.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] surf_elev Ground surface elevation, ft/m.
 * @param[in] a1 Groundwater outflow coefficient in the project input groundwater law.
 * @param[in] b1 Dimensionless groundwater outflow exponent.
 * @param[in] a2 Surface-water outflow coefficient in the project input groundwater law.
 * @param[in] b2 Dimensionless surface-water outflow exponent.
 * @param[in] a3 Surface/groundwater interaction coefficient in the project input law.
 * @param[in] tw Fixed receiving surface-water depth, ft/m; zero uses live node depth.
 * @param[in] hstar Lateral-flow threshold water-table elevation, ft/m; -1e10 uses receiving-node
 *     invert. Other values, including -99, are literal elevations.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_subcatch_set_gw_params(SWMM_Engine engine, int idx, double surf_elev,
                                                double a1, double b1, double a2, double b2,
                                                double a3, double tw, double hstar);

/**
 * @brief Read the input groundwater flow-law parameters.
 *
 * Values retain input-file units. No lifecycle check; does not read runtime theta/lower_depth.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[out] surf_elev Optional output. Ground surface elevation, ft/m.
 * @param[out] a1 Optional output. Groundwater outflow coefficient in the project input groundwater
 *     law.
 * @param[out] b1 Optional output. Dimensionless groundwater outflow exponent.
 * @param[out] a2 Optional output. Surface-water outflow coefficient in the project input
 *     groundwater law.
 * @param[out] b2 Optional output. Dimensionless surface-water outflow exponent.
 * @param[out] a3 Optional output. Surface/groundwater interaction coefficient in the project input
 *     law.
 * @param[out] tw Optional output. Fixed receiving surface-water depth, ft/m; zero uses live node
 *     depth.
 * @param[out] hstar Optional output. Lateral-flow threshold water-table elevation, ft/m; -1e10 uses
 *     receiving-node invert. Other values, including -99, are literal elevations.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_gw_params(SWMM_Engine engine, int idx, double* surf_elev,
                                                double* a1, double* b1, double* a2, double* b2,
                                                double* a3, double* tw, double* hstar);

/* =========================================================================
 * State injection (data assimilation)
 * ========================================================================= */

/**
 * @brief Set the groundwater state on a subcatchment.
 *
 * State injection for data assimilation / external coupling. Pass a
 * negative value to leave that component unchanged. Note that mass
 * balance reports will reflect the storage discontinuity, mirroring
 * hotstart loading.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Subcatchment index (must have groundwater).
 * @param[in] theta Upper zone moisture content (0..porosity), or < 0 to keep.
 * @param[in] lower_depth Saturated zone depth above aquifer bottom in user
 *                     length units (ft US, m SI), or < 0 to keep.
 * @returns SWMM_OK or error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard, but requires an initialized groundwater solver
 *     row with positive total depth or BADPARAM. Nonnegative theta is capped at porosity only (not
 *     raised to wilting point); nonnegative lower_depth is converted from ft/m and capped at
 *     aquifer total depth. Negative values leave each component unchanged.
 */
SWMM_ENGINE_API int swmm_subcatch_set_gw_state(SWMM_Engine engine, int idx, double theta,
                                               double lower_depth);

/**
 * @brief Get the groundwater state on a subcatchment.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Subcatchment index (must have groundwater).
 * @param[out] theta Receives the upper zone moisture content.
 * @param[out] lower_depth Receives the saturated zone depth (user length units).
 * @returns SWMM_OK or error code.
 *
 *
 * @note theta may be NULL to suppress that output. lower_depth may be NULL to suppress that output.
 *     A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard, but requires an initialized groundwater solver row
 *     with positive total depth or BADPARAM.
 */
SWMM_ENGINE_API int swmm_subcatch_get_gw_state(SWMM_Engine engine, int idx, double* theta,
                                               double* lower_depth);

/**
 * @brief Set degree-day snow state on one snow subarea of a subcatchment.
 *
 * State injection for data assimilation (e.g. observed SWE). Pass a
 * negative value to leave that component unchanged.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Subcatchment index (must have a snow pack).
 * @param[in] surface Snow subarea: 0 plowable, 1 impervious, 2 pervious.
 * @param[in] swe Snow water equivalent in user depth units (in US, mm SI), or < 0 to keep.
 * @param[in] fw Free water in user depth units, or < 0 to keep.
 * @param[in] ati Antecedent temperature index (deg F US, deg C SI); pass
 *                 <= -999 to keep (negative temperatures are valid).
 * @param[in] coldc Cold content in user depth units of melt equivalent, or < 0 to keep.
 * @returns SWMM_OK or error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard, but requires assigned degree-day snowpack and
 *     allocated solver state. UEB snowpacks or missing solver rows return BADPARAM; invalid surface
 *     returns BADINDEX. Negative swe/fw/coldc preserve each component; ati<=-999 preserves ATI.
 *     After any edit, free water is capped to pack SWE, even if its input was negative. No
 *     independent finite check.
 */
SWMM_ENGINE_API int swmm_subcatch_set_snow_state(SWMM_Engine engine, int idx, int surface,
                                                 double swe, double fw, double ati, double coldc);

/**
 * @brief Get degree-day snow state on one snow subarea of a subcatchment.
 * UEB callers use swmm_subcatch_get_snow_process_state in openswmm_snow.h.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Subcatchment index (must have a snow pack).
 * @param[in] surface Snow subarea: 0 plowable, 1 impervious, 2 pervious.
 * @param[out] swe Receives SWE (user depth units); may be NULL.
 * @param[out] fw Receives free water (user depth units); may be NULL.
 * @param[out] ati Receives ATI (user temperature units); may be NULL.
 * @param[out] coldc Receives cold content (user depth units); may be NULL.
 * @returns SWMM_OK or error code.
 *
 *
 * @note swe may be NULL to suppress that output. fw may be NULL to suppress that output. ati may be
 *     NULL to suppress that output. coldc may be NULL to suppress that output. A null engine
 *     returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard, but requires assigned degree-day snowpack and
 *     allocated solver state. UEB snowpacks or missing solver rows return BADPARAM; invalid surface
 *     returns BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_snow_state(SWMM_Engine engine, int idx, int surface,
                                                 double* swe, double* fw, double* ati,
                                                 double* coldc);

/**
 * @brief Get the current infiltration rate at a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] infil Receives the infiltration rate.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note infil may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil(SWMM_Engine engine, int idx, double* infil);

/* --- Runtime forcing (RUNNING state only) --- */

/**
 * @brief Overwrite the current subcatchment rainfall state during RUNNING.
 *
 * Directly replaces the stored rainfall value; the next hydrology/forcing update can overwrite it.
 * Does not register a persistent forcing provider.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] rainfall Rainfall intensity, inches/hour or mm/hour; converted to internal ft/s
 *     without finite/range validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE unless RUNNING.
 */
SWMM_ENGINE_API int swmm_subcatch_set_rainfall(SWMM_Engine engine, int idx, double rainfall);

/* =========================================================================
 * Water quality
 * ========================================================================= */

/**
 * @brief Get the pollutant concentration in subcatchment runoff.
 * @param[in] engine Engine handle.
 * @param[in] subcatch_idx Zero-based subcatchment index.
 * @param[in] pollutant_idx Zero-based pollutant index.
 * @param[out] conc Receives the concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note conc may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX. Pollutant indices
 *     exclude reaction/reserved species. Concentration storage must already be sized (normal
 *     initialized model); this wrapper does not lazily allocate it.
 */
SWMM_ENGINE_API int swmm_subcatch_get_quality(SWMM_Engine engine, int subcatch_idx,
                                              int pollutant_idx, double* conc);

/* =========================================================================
 * Bulk access
 * ========================================================================= */

/**
 * @brief Get runoff rates for all subcatchments in a single call.
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements (should equal swmm_subcatch_count()).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A null buffer or nonpositive count returns
 *     SWMM_ERR_BADPARAM. Copies min(count,subcatchment count) records and leaves any buffer tail
 *     unchanged.
 */
SWMM_ENGINE_API int swmm_subcatch_get_runoff_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get pollutant concentrations for all subcatchments for one pollutant.
 * @param[in] engine Engine handle.
 * @param[in] pollutant_idx Zero-based pollutant index.
 * @param[out] buf Caller-allocated buffer of at least @p count doubles.
 * @param[in] count Number of elements (should equal swmm_subcatch_count()).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX. A null buffer or nonpositive count returns SWMM_ERR_BADPARAM.
 *     Copies min(count,subcatchment count) records and leaves any buffer tail unchanged. Pollutant
 *     indices exclude reaction/reserved species. Concentration storage must already be sized
 *     (normal initialized model); this wrapper does not lazily allocate it.
 */
SWMM_ENGINE_API int swmm_subcatch_get_quality_bulk(SWMM_Engine engine, int pollutant_idx,
                                                   double* buf, int count);

/* =========================================================================
 * Phase 3 bulk getters — added in OpenSWMM 6.0.0 to eliminate the N
 * round-trip cost of per-subcatchment scalar accessors. All return a
 * caller-allocated @c double buffer of length @c count (clipped at
 * @c swmm_subcatch_count()); the IDs variant returns a stride-packed
 * UTF-8 buffer following the same format as @ref swmm_node_get_ids_bulk.
 * ========================================================================= */

/**
 * @brief Copy rainfall values for a prefix of subcatchments.
 *
 * Writes min(count,subcatchment count) elements and preserves any remaining buffer tail. No
 * lifecycle check; returns the current stored state.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[out] buf Non-null caller-owned array with room for min(count,subcatchment count) doubles.
 *     Rainfall intensity in inches/hour or mm/hour.
 * @param[in] count Positive destination element capacity; a short buffer selects the corresponding
 *     model prefix.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_rainfall_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy evap values for a prefix of subcatchments.
 *
 * Writes min(count,subcatchment count) elements and preserves any remaining buffer tail. No
 * lifecycle check; returns the current stored state.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[out] buf Non-null caller-owned array with room for min(count,subcatchment count) doubles.
 *     Evaporation loss rate in inches/day or mm/day.
 * @param[in] count Positive destination element capacity; a short buffer selects the corresponding
 *     model prefix.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_evap_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy infil values for a prefix of subcatchments.
 *
 * Writes min(count,subcatchment count) elements and preserves any remaining buffer tail. No
 * lifecycle check; returns the current stored state.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[out] buf Non-null caller-owned array with room for min(count,subcatchment count) doubles.
 *     Infiltration loss rate in inches/hour or mm/hour.
 * @param[in] count Positive destination element capacity; a short buffer selects the corresponding
 *     model prefix.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_infil_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Copy snow depth values for a prefix of subcatchments.
 *
 * Writes min(count,subcatchment count) elements and preserves any remaining buffer tail. This
 * implementation zero-fills the prefix; unlike the scalar snow-depth getter it does not sample
 * integrated snow state.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[out] buf Non-null caller-owned array with room for min(count,subcatchment count) doubles.
 *     Placeholder zero, not a sampled SnowSolver depth.
 * @param[in] count Positive destination element capacity; a short buffer selects the corresponding
 *     model prefix.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_snow_depth_bulk(SWMM_Engine engine, double* buf, int count);

/**
 * @brief Get subcatchment IDs for all subcatchments in a single call
 *        (stride-packed UTF-8).
 * @details Format identical to @ref swmm_node_get_ids_bulk.
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer of @c stride*count bytes.
 * @param[in] stride Per-ID slot size in bytes (must be > 1).
 * @param[in] count Number of IDs to read.
 * @returns @c SWMM_OK on success; @c SWMM_ERR_BADHANDLE if @p engine is
 *          invalid; @c SWMM_ERR_BADPARAM if @p buf is NULL,
 *          @p stride < 2, or @p count <= 0.
 * @see swmm_subcatch_id, swmm_node_get_ids_bulk
 * @since 6.0.0
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE.
 */
SWMM_ENGINE_API int swmm_subcatch_get_ids_bulk(SWMM_Engine engine, char* buf, int stride,
                                               int count);

/* =========================================================================
 * Ponded quality (mass in standing water between events)
 * ========================================================================= */

/**
 * @brief Read a pollutant's ponded surface mass.
 *
 * Mass uses the surface-quality kernel's internal constituent mass convention (mg for mass-based
 * pollutant transport). No lifecycle check. A missing slot returns zero.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] subcatch_idx Zero-based existing subcatchment index.
 * @param[in] pollutant_idx Zero-based pollutant index in the allocated concentration stride;
 *     reaction/reserved species are not selected by this API.
 * @param[out] mass Optional output of stored mass, zero when no ponded slot exists.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_ponded_quality(SWMM_Engine engine, int subcatch_idx,
                                                     int pollutant_idx, double* mass);

/**
 * @brief Replace a pollutant's ponded surface mass.
 *
 * Mass uses the surface-quality kernel's internal constituent mass convention (mg for mass-based
 * pollutant transport). No lifecycle check. A valid index with no allocated ponded slot succeeds
 * without writing; injection is not booked as an external source in this wrapper.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] subcatch_idx Zero-based existing subcatchment index.
 * @param[in] pollutant_idx Zero-based pollutant index in the allocated concentration stride;
 *     reaction/reserved species are not selected by this API.
 * @param[in] mass Raw stored ponded mass; no finite/range validation or unit conversion.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_set_ponded_quality(SWMM_Engine engine, int subcatch_idx,
                                                     int pollutant_idx, double mass);

/**
 * @brief Rename a subcatchment in its name registry.
 *
 * Requires BUILDING/OPENED. Uses the registry collision policy; returns BADPARAM on collision. This
 * wrapper does not explicitly migrate name-keyed GWF expression options.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] newId Non-null, nonempty copied unique ID; case-only respelling of this row is
 *     permitted.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_subcatch_rename(SWMM_Engine engine, int idx, const char* newId);

/* =========================================================================
 * Aquifer definitions ([AQUIFERS] section) — Slice BM.0 / BP.6.6.4
 * ========================================================================= */

/**
 * @brief Get the total number of aquifer definitions in the model.
 * @param[in] engine Engine handle.
 * @returns Number of aquifer definitions, or -1 on error.
 *
 *
 * @note Null engine returns -1.
 */
SWMM_ENGINE_API int swmm_aquifer_count(SWMM_Engine engine);

/**
 * @brief Look up an aquifer's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated aquifer identifier.
 * @returns Zero-based index, or -1 if not found.
 *
 *
 * @note Case-insensitive name lookup; null engine/name or no match returns -1.
 */
SWMM_ENGINE_API int swmm_aquifer_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of an aquifer definition by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based aquifer index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note Returned text borrows engine storage; do not free. Copy before a registry/table edit or
 *     engine close/destruction. Null engine/invalid index returns NULL.
 */
SWMM_ENGINE_API const char* swmm_aquifer_id(SWMM_Engine engine, int idx);

/**
 * @brief Add a new aquifer definition with default (zero) parameters.
 *
 * @details Parameters default to 0.0 / empty strings — use the property
 *          setters from Slice BP `AquiferEditor` to configure porosity,
 *          conductivity, etc. Lifecycle: BUILDING or OPENED.
 *
 * @param[in] engine Engine handle.
 * @param[in] id Unique null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. Copies ID and rejects case-insensitive duplicates; NULL is refused, but
 *     this wrapper has no explicit empty-ID check.
 */
SWMM_ENGINE_API int swmm_aquifer_add(SWMM_Engine engine, const char* id);

/**
 * @brief Rename an aquifer in place.
 *
 * @details Updates both the name vector and the name registry so
 *          swmm_aquifer_id() and swmm_aquifer_index() stay in agreement.
 *          [GROUNDWATER] holds an aquifer INDEX, not a name, so subcatchment
 *          references follow automatically.
 *          Lifecycle: BUILDING or OPENED.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Aquifer index.
 * @param[in] new_id New unique name (non-empty). A pure case-respelling of this
 *                same aquifer is allowed.
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM on an empty or duplicate name.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_aquifer_rename(SWMM_Engine engine, int idx, const char* new_id);

/**
 * @brief Aquifer parameter codes for swmm_aquifer_get_param / _set_param.
 *
 * @details Values use input-file units (the same columns as the [AQUIFERS]
 *          line). The flux-coefficient parameters (CONDUCTIVITY,
 *          CONDUCT_SLOPE, TENSION_SLOPE, UPPER_EVAP_FRAC, LOWER_EVAP_DEPTH,
 *          LOWER_LOSS_COEFF) are settable both before the simulation starts
 *          and while it is running; the structural / initial-condition
 *          parameters (POROSITY, WILTING_POINT, FIELD_CAPACITY, BOTTOM_ELEV,
 *          WATER_TABLE_ELEV, UPPER_MOISTURE) bound or seed the groundwater
 *          state and are pre-start-only.
 */
typedef enum SWMM_AquiferParam {
    SWMM_AQUIFER_POROSITY = 0,
    SWMM_AQUIFER_WILTING_POINT = 1,
    SWMM_AQUIFER_FIELD_CAPACITY = 2,
    SWMM_AQUIFER_CONDUCTIVITY = 3,
    SWMM_AQUIFER_CONDUCT_SLOPE = 4,
    SWMM_AQUIFER_TENSION_SLOPE = 5,
    SWMM_AQUIFER_UPPER_EVAP_FRAC = 6,
    SWMM_AQUIFER_LOWER_EVAP_DEPTH = 7,
    SWMM_AQUIFER_LOWER_LOSS_COEFF = 8,
    SWMM_AQUIFER_BOTTOM_ELEV = 9,
    SWMM_AQUIFER_WATER_TABLE_ELEV = 10,
    SWMM_AQUIFER_UPPER_MOISTURE = 11
} SWMM_AquiferParam;

/**
 * @brief Get an aquifer parameter (input-file units).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based aquifer index.
 * @param[in] param A SWMM_AquiferParam code.
 * @param[out] value Receives the parameter value.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_aquifer_get_param(SWMM_Engine engine, int idx, int param, double* value);

/**
 * @brief Set an aquifer parameter (input-file units).
 *
 * @details Flux-coefficient parameters take effect on the next step when set
 *          mid-run (the groundwater solver's per-subcatchment copies are
 *          refreshed); structural / initial-condition parameters return
 *          SWMM_ERR_LIFECYCLE while the simulation is running.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based aquifer index.
 * @param[in] param A SWMM_AquiferParam code.
 * @param[in] value New parameter value.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Structural/initial parameters require BUILDING/OPENED; flux
 *     parameters have no lifecycle restriction and refresh cached aquifer parameters outside those
 *     states. Numeric bounds use comparisons only; provide finite inputs.
 */
SWMM_ENGINE_API int swmm_aquifer_set_param(SWMM_Engine engine, int idx, int param, double value);

/**
 * @brief Read the aquifer's upper-zone evaporation pattern name.
 *
 * @details The optional monthly time pattern (`[PATTERNS]` MONTHLY) that scales
 *          the upper-zone evaporation fraction — the trailing `[ETupat]` column
 *          of the `[AQUIFERS]` line. The 12 numeric parameters are reached via
 *          @ref swmm_aquifer_get_param; this completes the round-trip for the
 *          one string-valued column. Writes a NUL-terminated string into `buf`
 *          (empty when no pattern is assigned), truncating if `buflen` is too
 *          small.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based aquifer index.
 * @param[out] buf Destination buffer for the pattern name.
 * @param[in] buflen Size of `buf` in bytes (must be > 0).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Output must be non-null with positive byte capacity; text is
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_aquifer_get_evap_pattern(SWMM_Engine engine, int idx, char* buf,
                                                  int buflen);

/**
 * @brief Set or clear the aquifer's upper-zone evaporation pattern name.
 *
 * @details Inverse of @ref swmm_aquifer_get_evap_pattern. The name is resolved
 *          to a `[PATTERNS]` entry at start(); a null or empty string clears
 *          the assignment. Pre-start-only (BUILDING / OPENED): the pattern
 *          index is bound when the simulation starts, so a mid-run change
 *          returns SWMM_ERR_LIFECYCLE.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based aquifer index.
 * @param[in] name Pattern name, or null/empty to clear.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_aquifer_set_evap_pattern(SWMM_Engine engine, int idx, const char* name);

/* =========================================================================
 * Snowpack definitions ([SNOWPACKS] section) — Slice BM.0 / BP.6.6.5
 * ========================================================================= */

/**
 * @brief Assign an existing snowpack before initialization.
 *
 * Requires BUILDING/OPENED. Unknown nonempty names return BADPARAM without changing the assignment;
 * stores both resolved index and canonical name.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] name Borrowed snowpack ID resolved case-insensitively; NULL/empty clears the
 *     assignment.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_subcatch_set_snowpack(SWMM_Engine engine, int idx, const char* name);
/**
 * @brief Borrow the assigned snowpack ID.
 *
 * Do not free; copy before renaming/removing the snowpack or closing/destroying the engine.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @return Engine-owned NUL-terminated name, an empty string for no assignment, or NULL for null
 *     engine/invalid subcatchment.
 */
SWMM_ENGINE_API const char* swmm_subcatch_get_snowpack(SWMM_Engine engine, int idx);

/**
 * @brief Get the total number of snowpack definitions in the model.
 * @param[in] engine Engine handle.
 * @returns Number of snowpack definitions, or -1 on error.
 *
 *
 * @note Null engine returns -1.
 */
SWMM_ENGINE_API int swmm_snowpack_count(SWMM_Engine engine);

/**
 * @brief Look up a snowpack's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated snowpack identifier.
 * @returns Zero-based index, or -1 if not found.
 *
 *
 * @note Case-insensitive name lookup; null engine/name or no match returns -1.
 */
SWMM_ENGINE_API int swmm_snowpack_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a snowpack definition by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based snowpack index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note Returned text borrows engine storage; do not free. Copy before a registry/table edit or
 *     engine close/destruction. Null engine/invalid index returns NULL.
 */
SWMM_ENGINE_API const char* swmm_snowpack_id(SWMM_Engine engine, int idx);

/**
 * @brief Add a new snowpack definition with default (zero) parameters.
 *
 * @details All three surface arrays (plowable / impervious / pervious) and
 *          the removal row are zero-initialised. Use the property setters
 *          from Slice BP `SnowpackEditor` to configure melt coefficients,
 *          ATI, negative-melt-ratio, and removal fractions.
 *          Lifecycle: BUILDING or OPENED.
 *
 * @param[in] engine Engine handle.
 * @param[in] id Unique null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. Copies ID and rejects case-insensitive duplicates; NULL is refused, but
 *     this wrapper has no explicit empty-ID check.
 */
SWMM_ENGINE_API int swmm_snowpack_add(SWMM_Engine engine, const char* id);

/**
 * @brief Rename a snow pack in place.
 *
 * @details Updates both the name vector (read by swmm_snowpack_id() and the
 *          [SNOWPACKS] writer) and the name registry (read by
 *          swmm_snowpack_index() and the [SUBCATCHMENTS] snow-pack column),
 *          plus the deferred-resolution name held per subcatchment. Melt
 *          parameters and subcatchment assignments are index-based and follow
 *          automatically.
 *          Lifecycle: BUILDING or OPENED.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Snow pack index.
 * @param[in] new_id New unique name (non-empty). A pure case-respelling of this
 *                same snow pack is allowed.
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM on an empty or duplicate name.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_snowpack_rename(SWMM_Engine engine, int idx, const char* new_id);

/* -------------------------------------------------------------------------
 * Snowpack surface parameters
 *
 * A snowpack carries three snow-melt surfaces — PLOWABLE, IMPERVIOUS and
 * PERVIOUS — plus an optional REMOVAL row, matching the four line types of
 * the legacy `[SNOWPACKS]` section. Each surface takes seven values in
 * input-file units:
 *
 *   cmin    Minimum melt coefficient                 (in or mm /hr/deg)
 *   cmax    Maximum melt coefficient                 (in or mm /hr/deg)
 *   tbase   Snow-melt base temperature               (deg F or C)
 *   fwfrac  Free-water capacity as a fraction of depth
 *   sd0     Initial snow depth                       (in or mm water equiv.)
 *   fw0     Initial free water                       (in or mm)
 *   last    PLOWABLE: fraction of the impervious area that is plowable;
 *           IMPERVIOUS / PERVIOUS: snow depth above which there is 100% cover
 *           (in or mm).
 *
 * Setters are pre-start-only (BUILDING / OPENED) — the surface parameters seed
 * per-subcatchment snow state at start(); a mid-run change returns
 * SWMM_ERR_LIFECYCLE. Each setter has an inverse getter so the GUI snowpack
 * editor can load existing definitions. Any getter out-param may be null.
 * ------------------------------------------------------------------------- */

/**
 * @brief Assign the plowable snow-melt surface parameters.
 *
 * Requires BUILDING/OPENED. Values seed snow state at start; this setter stores them without
 * finite/range checks.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack definition index.
 * @param[in] cmin Minimum melt factor, inches/hour/degree F or mm/hour/degree C.
 * @param[in] cmax Maximum melt factor in the same units.
 * @param[in] tbase Base melt temperature, degrees F for US or C for SI.
 * @param[in] fwfrac Free-water capacity fraction.
 * @param[in] sd0 Initial snow water-equivalent depth, inches/mm.
 * @param[in] fw0 Initial free-water depth, inches/mm.
 * @param[in] last Plowable fraction of impervious area.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_snowpack_set_plowable(SWMM_Engine engine, int idx, double cmin,
                                               double cmax, double tbase, double fwfrac, double sd0,
                                               double fw0, double last);
/**
 * @brief Read the plowable snow-melt surface parameters.
 *
 * No lifecycle check; reads stored input values.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack definition index.
 * @param[out] cmin Optional output. Minimum melt factor, inches/hour/degree F or mm/hour/degree C.
 * @param[out] cmax Optional output. Maximum melt factor in the same units.
 * @param[out] tbase Optional output. Base melt temperature, degrees F for US or C for SI.
 * @param[out] fwfrac Optional output. Free-water capacity fraction.
 * @param[out] sd0 Optional output. Initial snow water-equivalent depth, inches/mm.
 * @param[out] fw0 Optional output. Initial free-water depth, inches/mm.
 * @param[out] last Optional output. Plowable fraction of impervious area.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_snowpack_get_plowable(SWMM_Engine engine, int idx, double* cmin,
                                               double* cmax, double* tbase, double* fwfrac,
                                               double* sd0, double* fw0, double* last);

/**
 * @brief Assign the impervious snow-melt surface parameters.
 *
 * Requires BUILDING/OPENED. Values seed snow state at start; this setter stores them without
 * finite/range checks.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack definition index.
 * @param[in] cmin Minimum melt factor, inches/hour/degree F or mm/hour/degree C.
 * @param[in] cmax Maximum melt factor in the same units.
 * @param[in] tbase Base melt temperature, degrees F for US or C for SI.
 * @param[in] fwfrac Free-water capacity fraction.
 * @param[in] sd0 Initial snow water-equivalent depth, inches/mm.
 * @param[in] fw0 Initial free-water depth, inches/mm.
 * @param[in] last Snow water-equivalent depth for complete cover, inches/mm.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_snowpack_set_impervious(SWMM_Engine engine, int idx, double cmin,
                                                 double cmax, double tbase, double fwfrac,
                                                 double sd0, double fw0, double last);
/**
 * @brief Read the impervious snow-melt surface parameters.
 *
 * No lifecycle check; reads stored input values.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack definition index.
 * @param[out] cmin Optional output. Minimum melt factor, inches/hour/degree F or mm/hour/degree C.
 * @param[out] cmax Optional output. Maximum melt factor in the same units.
 * @param[out] tbase Optional output. Base melt temperature, degrees F for US or C for SI.
 * @param[out] fwfrac Optional output. Free-water capacity fraction.
 * @param[out] sd0 Optional output. Initial snow water-equivalent depth, inches/mm.
 * @param[out] fw0 Optional output. Initial free-water depth, inches/mm.
 * @param[out] last Optional output. Snow water-equivalent depth for complete cover, inches/mm.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_snowpack_get_impervious(SWMM_Engine engine, int idx, double* cmin,
                                                 double* cmax, double* tbase, double* fwfrac,
                                                 double* sd0, double* fw0, double* last);

/**
 * @brief Assign the pervious snow-melt surface parameters.
 *
 * Requires BUILDING/OPENED. Values seed snow state at start; this setter stores them without
 * finite/range checks.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack definition index.
 * @param[in] cmin Minimum melt factor, inches/hour/degree F or mm/hour/degree C.
 * @param[in] cmax Maximum melt factor in the same units.
 * @param[in] tbase Base melt temperature, degrees F for US or C for SI.
 * @param[in] fwfrac Free-water capacity fraction.
 * @param[in] sd0 Initial snow water-equivalent depth, inches/mm.
 * @param[in] fw0 Initial free-water depth, inches/mm.
 * @param[in] last Snow water-equivalent depth for complete cover, inches/mm.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_snowpack_set_pervious(SWMM_Engine engine, int idx, double cmin,
                                               double cmax, double tbase, double fwfrac, double sd0,
                                               double fw0, double last);
/**
 * @brief Read the pervious snow-melt surface parameters.
 *
 * No lifecycle check; reads stored input values.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack definition index.
 * @param[out] cmin Optional output. Minimum melt factor, inches/hour/degree F or mm/hour/degree C.
 * @param[out] cmax Optional output. Maximum melt factor in the same units.
 * @param[out] tbase Optional output. Base melt temperature, degrees F for US or C for SI.
 * @param[out] fwfrac Optional output. Free-water capacity fraction.
 * @param[out] sd0 Optional output. Initial snow water-equivalent depth, inches/mm.
 * @param[out] fw0 Optional output. Initial free-water depth, inches/mm.
 * @param[out] last Optional output. Snow water-equivalent depth for complete cover, inches/mm.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_snowpack_get_pervious(SWMM_Engine engine, int idx, double* cmin,
                                               double* cmax, double* tbase, double* fwfrac,
                                               double* sd0, double* fw0, double* last);

/**
 * @brief Assign snow redistribution parameters.
 *
 * Requires BUILDING/OPENED. Does not validate individual values, fraction sum or destination;
 * stores authoring inputs verbatim.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack definition index.
 * @param[in] dsnow Snow depth at which removal begins, inches/mm.
 * @param[in] fout Fraction exported out of the watershed.
 * @param[in] fimp Fraction routed to impervious surface.
 * @param[in] fperv Fraction routed to pervious surface.
 * @param[in] fimelt Fraction converted to immediate melt.
 * @param[in] fsubcatch Fraction transferred to the separately named destination subcatchment.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_snowpack_set_removal(SWMM_Engine engine, int idx, double dsnow,
                                              double fout, double fimp, double fperv, double fimelt,
                                              double fsubcatch);
/**
 * @brief Read snow redistribution parameters.
 *
 * No lifecycle check.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack definition index.
 * @param[out] dsnow Optional output. Snow depth at which removal begins, inches/mm.
 * @param[out] fout Optional output. Fraction exported out of the watershed.
 * @param[out] fimp Optional output. Fraction routed to impervious surface.
 * @param[out] fperv Optional output. Fraction routed to pervious surface.
 * @param[out] fimelt Optional output. Fraction converted to immediate melt.
 * @param[out] fsubcatch Optional output. Fraction transferred to the separately named destination
 *     subcatchment.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_snowpack_get_removal(SWMM_Engine engine, int idx, double* dsnow,
                                              double* fout, double* fimp, double* fperv,
                                              double* fimelt, double* fsubcatch);

/**
 * @brief Set or clear the copied snow-removal destination name.
 *
 * Requires BUILDING/OPENED. Stores the name without resolving existence here.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack definition index.
 * @param[in] name Borrowed NUL-terminated subcatchment name; NULL/empty clears it.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_snowpack_set_removal_subcatch(SWMM_Engine engine, int idx,
                                                       const char* name);
/**
 * @brief Copy the snow-removal destination name.
 *
 * No lifecycle check; empty when no destination is assigned.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack definition index.
 * @param[out] buf Non-null caller-owned buffer, NUL-terminated and silently truncated to buflen-1
 *     bytes.
 * @param[in] buflen Positive capacity in bytes including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_snowpack_get_removal_subcatch(SWMM_Engine engine, int idx, char* buf,
                                                       int buflen);

/* =========================================================================
 * Tag — free-form string label from the INP `[TAGS]` section
 * ========================================================================= */

/**
 * @brief Copy a subcatchment tag.
 *
 * Missing tags copy as empty text. No lifecycle check.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[out] buf Non-null caller-owned buffer, NUL-terminated and silently truncated to buflen-1
 *     bytes.
 * @param[in] buflen Positive capacity in bytes including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_tag(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Replace or clear a copied subcatchment tag.
 *
 * No lifecycle check. Stored per object, so a registry rename preserves the tag.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] tag Borrowed NUL-terminated tag; NULL/empty clears it.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_set_tag(SWMM_Engine engine, int idx, const char* tag);

/**
 * @brief Replace the rain precipitation multiplier.
 *
 * No lifecycle guard. The runoff precipitation path applies it on subsequent rain evaluation, in
 * addition to the gage factor.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] factor Dimensionless multiplier; values <=0 are refused. Supply a finite positive
 *     value because no explicit finiteness check is made.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_set_rain_scale_factor(SWMM_Engine engine, int idx, double factor);

/**
 * @brief Read the rain precipitation multiplier.
 *
 * No lifecycle guard. The runoff precipitation path applies it on subsequent rain evaluation, in
 * addition to the gage factor.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[out] factor Optional dimensionless stored factor output.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_rain_scale_factor(SWMM_Engine engine, int idx,
                                                        double* factor);

/**
 * @brief Replace the snow precipitation multiplier.
 *
 * No lifecycle guard. The runoff precipitation path applies it on subsequent snow evaluation, in
 * addition to the gage factor.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[in] factor Dimensionless multiplier; values <=0 are refused. Supply a finite positive
 *     value because no explicit finiteness check is made.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_set_snow_scale_factor(SWMM_Engine engine, int idx, double factor);

/**
 * @brief Read the snow precipitation multiplier.
 *
 * No lifecycle guard. The runoff precipitation path applies it on subsequent snow evaluation, in
 * addition to the gage factor.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[out] factor Optional dimensionless stored factor output.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_subcatch_get_snow_scale_factor(SWMM_Engine engine, int idx,
                                                        double* factor);

/* =========================================================================
 * Custom groundwater flow expressions ([GWF] section)
 * ========================================================================= */

/**
 * @brief Which of a subcatchment's two custom groundwater flow expressions.
 *
 * @details LATERAL is added to the standard A1/A2/A3 lateral flow; DEEP
 *          replaces the standard deep-percolation term. Expressions may use
 *          the variables listed by @ref swmm_gwf_variable_name, the functions
 *          listed by @ref swmm_gwf_function_name, and the operators
 *          `+ - * / ^ ( )`; names are case-insensitive.
 */
typedef enum SWMM_GwfType { SWMM_GWF_LATERAL = 0, SWMM_GWF_DEEP = 1 } SWMM_GwfType;

/**
 * @brief Read a subcatchment's custom groundwater flow expression.
 *
 * @details Writes a NUL-terminated string into `buf` (empty when no
 *          expression is set), truncating if `buflen` is too small.
 *
 * @param[in] engine Engine handle.
 * @param[in] index Zero-based subcatchment index.
 * @param[in] type A SWMM_GwfType code.
 * @param[out] buf Destination buffer for the expression text.
 * @param[in] buflen Size of `buf` in bytes (must be > 0).
 * @returns SWMM_OK on success, SWMM_ERR_BADPARAM for a bad type, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Output must be non-null with positive byte capacity; text is
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_subcatch_get_gwf_expression(SWMM_Engine engine, int index, int type,
                                                     char* buf, int buflen);

/**
 * @brief Set or clear a subcatchment's custom groundwater flow expression.
 *
 * @details Inverse of @ref swmm_subcatch_get_gwf_expression. A null or empty
 *          string clears the expression. The text is stored as given and is
 *          NOT validated here (so files with legacy quirks still load);
 *          use @ref swmm_gwf_validate_expression first. An invalid expression
 *          fails start() with ERROR 233. Pre-start-only (BUILDING / OPENED):
 *          the expression is compiled when the simulation starts, so a
 *          mid-run change returns SWMM_ERR_LIFECYCLE.
 *
 * @param[in] engine Engine handle.
 * @param[in] index Zero-based subcatchment index.
 * @param[in] type A SWMM_GwfType code.
 * @param[in] expr Expression text, or null/empty to clear.
 * @returns SWMM_OK on success, SWMM_ERR_BADPARAM for a bad type, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check returns SWMM_ERR_BADINDEX.
 *     Copies or clears source text without compiling it; call swmm_gwf_validate_expression() for an
 *     immediate syntax diagnostic.
 */
SWMM_ENGINE_API int swmm_subcatch_set_gwf_expression(SWMM_Engine engine, int index, int type,
                                                     const char* expr);

/**
 * @brief Validate a [GWF] expression WITHOUT modifying engine state.
 *
 * @details Editor-support peer of swmm_treatment_validate_expression:
 *          grammar-only, never mutates the engine, so it is safe to invoke
 *          per keystroke. Stricter than the runtime parser: unknown
 *          characters, unknown identifiers (must be a GW variable or a
 *          function), unbalanced parentheses, misplaced operators and wrong
 *          function arity (min/max take 2 arguments, all others 1) are
 *          errors. An empty or whitespace-only expression is INVALID
 *          ("expression is empty") — callers treat empty as "clear"
 *          themselves.
 *
 * @param[in] engine Engine handle (grammar only; no model data is read).
 * @param[in] expr Expression text.
 * @param[out] errbuf Receives the diagnostic on failure ("" on success).
 *                      May be NULL.
 * @param[in] buflen Size of errbuf in bytes.
 * @param[out] col_out Receives the 0-based character offset of the error,
 *                      or -1 when not attributable. May be NULL.
 * @returns SWMM_OK when the expression is valid, SWMM_ERR_BADPARAM when
 *          invalid (diagnostic in errbuf/col_out) or on NULL expr.
 *
 *
 * @note col_out may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE.
 */
SWMM_ENGINE_API int swmm_gwf_validate_expression(SWMM_Engine engine, const char* expr, char* errbuf,
                                                 int buflen, int* col_out);

/**
 * @brief Number of variables a [GWF] expression may reference.
 * @param[in] engine Engine handle.
 * @returns Variable count, or -1 on error.
 *
 *
 * @note Null engine returns -1.
 */
SWMM_ENGINE_API int swmm_gwf_variable_count(SWMM_Engine engine);

/**
 * @brief Name of the i-th [GWF] variable (upper-case, e.g. "HGW").
 * @param[in] engine Engine handle.
 * @param[in] i Zero-based index, < swmm_gwf_variable_count().
 * @param[out] buf Destination buffer (NUL-terminated, truncated if too small).
 * @param[in] buflen Size of `buf` in bytes (must be > 0).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Output must be non-null with positive byte capacity; text is
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_gwf_variable_name(SWMM_Engine engine, int i, char* buf, int buflen);

/**
 * @brief Short English description of the i-th [GWF] variable, with a units
 *        placeholder, e.g. "Water table height above aquifer bottom (ft or m)".
 * @param[in] engine Engine handle.
 * @param[in] i Zero-based index, < swmm_gwf_variable_count().
 * @param[out] buf Destination buffer (NUL-terminated, truncated if too small).
 * @param[in] buflen Size of `buf` in bytes (must be > 0).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Output must be non-null with positive byte capacity; text is
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_gwf_variable_description(SWMM_Engine engine, int i, char* buf, int buflen);

/**
 * @brief Number of built-in functions a [GWF] expression may call.
 * @param[in] engine Engine handle.
 * @returns Function count, or -1 on error.
 *
 *
 * @note Null engine returns -1.
 */
SWMM_ENGINE_API int swmm_gwf_function_count(SWMM_Engine engine);

/**
 * @brief Name of the i-th expression function (lower-case, stable order:
 *        abs, sgn, sqrt, log, exp, sin, cos, tan, asin, acos, atan, step, min,
 *        max, cot, sinh, cosh, tanh, coth, log10, acot).
 * @param[in] engine Engine handle.
 * @param[in] i Zero-based index, < swmm_gwf_function_count().
 * @param[out] buf Destination buffer (NUL-terminated, truncated if too small).
 * @param[in] buflen Size of `buf` in bytes (must be > 0).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed subcatchment/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Output must be non-null with positive byte capacity; text is
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_gwf_function_name(SWMM_Engine engine, int i, char* buf, int buflen);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_SUBCATCHMENTS_H */
