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
 * @file      openswmm_pollutants.h
 * @brief     OpenSWMM Engine — Pollutant / Water Quality C API.
 *
 * @details Pollutant identity, creation, property setters, and runtime
 *          quality injection at nodes and links.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_POLLUTANTS_H
#define OPENSWMM_POLLUTANTS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the total number of pollutants in the model.
 * @param[in] engine Engine handle.
 * @returns Number of pollutants, or -1 on error.
 */
SWMM_ENGINE_API int swmm_pollutant_count(SWMM_Engine engine);

/**
 * @brief Look up a pollutant's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated pollutant identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_pollutant_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a pollutant by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note ID is borrowed until name/topology changes, close/reopen or engine destruction; copy before
 *     retaining it across edits.
 */
SWMM_ENGINE_API const char* swmm_pollutant_id(SWMM_Engine engine, int idx);

/* =========================================================================
 * Creation (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Add a new pollutant to the model.
 * @param[in] engine Engine handle (SWMM_STATE_BUILDING).
 * @param[in] id Unique null-terminated identifier for the new pollutant.
 * @param[in] units Concentration units code (0=MG/L, 1=UG/L, 2=#/L).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. Requires BUILDING or OPENED, not BUILDING alone.
 *     Null/duplicate ID is BADPARAM. The unit code is stored without enum-range validation in this
 *     add operation (use 0=mg/L, 1=ug/L or 2=count/L); existing pollutant definition/quality
 *     columns are preserved as the new row/column is appended.
 */
SWMM_ENGINE_API int swmm_pollutant_add(SWMM_Engine engine, const char* id, int units);

/**
 * @brief Rename a pollutant in place.
 *
 * @details Updates the name registry and every name-stored reference:
 *          [INFLOWS]/[DWF] rows keyed by constituent name follow the new
 *          name. Index-stored references (co-pollutant, LID removals,
 *          buildup/washoff columns) are positional and unaffected.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] new_id New unique name (non-empty).
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM on empty/duplicate name.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed pollutant index check returns SWMM_ERR_BADINDEX.
 *     Updates constituent-name references in INFLOWS/DWF and stored node-LID treatment
 *     pollutant/expression references; index-based references remain positional. new_id must be
 *     non-null/nonempty/unique or BADPARAM is returned.
 */
SWMM_ENGINE_API int swmm_pollutant_rename(SWMM_Engine engine, int idx, const char* new_id);

/* =========================================================================
 * Property setters (per-function lifecycle guards)
 * ========================================================================= */

/**
 * @brief Change the concentration units of an existing pollutant.
 *
 * @details Inverse of @ref swmm_pollutant_get_units. The units are normally
 *          fixed at creation by @ref swmm_pollutant_add; this lets a model
 *          builder (e.g. the GUI pollutant editor) correct them afterwards.
 *          Editable in the BUILDING or OPENED (pre-start) states only — units
 *          define how every stored/reported concentration is interpreted, so a
 *          mid-run change has no well-defined meaning and is rejected with
 *          SWMM_ERR_LIFECYCLE.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] units Concentration units code (0=MG/L, 1=UG/L, 2=#/L).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed pollutant index check returns
 *     SWMM_ERR_BADINDEX. Requires BUILDING or OPENED; other states return LIFECYCLE. Unit codes
 *     outside 0..2 return BADPARAM. Does not rescale existing concentration numbers when changing
 *     their interpretation.
 */
SWMM_ENGINE_API int swmm_pollutant_set_units(SWMM_Engine engine, int idx, int units);

/**
 * @brief Set the first-order decay coefficient for a pollutant.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] k Decay coefficient (1/day).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard is imposed; values are assigned directly without
 *     finite/range validation unless noted in this function contract. Converts 1/day input to
 *     internal 1/s by dividing by 86400; the getter reverses this conversion.
 */
SWMM_ENGINE_API int swmm_pollutant_set_kdecay(SWMM_Engine engine, int idx, double k);

/**
 * @brief Set the pollutant concentration in rainfall.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] conc Concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard is imposed; values are assigned directly without
 *     finite/range validation unless noted in this function contract.
 */
SWMM_ENGINE_API int swmm_pollutant_set_rain_conc(SWMM_Engine engine, int idx, double conc);

/**
 * @brief Set the pollutant concentration in groundwater.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] conc Concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard is imposed; values are assigned directly without
 *     finite/range validation unless noted in this function contract.
 */
SWMM_ENGINE_API int swmm_pollutant_set_gw_conc(SWMM_Engine engine, int idx, double conc);

/**
 * @brief Set the initial concentration throughout the conveyance system.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] conc Initial concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_pollutant_set_init_conc(SWMM_Engine engine, int idx, double conc);

/**
 * @brief Get the concentration units code for a pollutant.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[out] units Receives the units code (0=MG/L, 1=UG/L, 2=#/L).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note units may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed pollutant/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_pollutant_get_units(SWMM_Engine engine, int idx, int* units);

/* =========================================================================
 * Property getters
 * ========================================================================= */

/**
 * @brief Get the first-order decay coefficient.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[out] k Receives the decay coefficient (1/day).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note k may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     pollutant/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_pollutant_get_kdecay(SWMM_Engine engine, int idx, double* k);

/**
 * @brief Get the pollutant concentration in rainfall.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[out] conc Receives the concentration.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note conc may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed pollutant/pollutant index check returns SWMM_ERR_BADINDEX. Returned concentration uses
 *     this pollutant row units without conversion.
 */
SWMM_ENGINE_API int swmm_pollutant_get_rain_conc(SWMM_Engine engine, int idx, double* conc);

/**
 * @brief Get the pollutant concentration in groundwater.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[out] conc Receives the concentration.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note conc may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed pollutant/pollutant index check returns SWMM_ERR_BADINDEX. Returned concentration uses
 *     this pollutant row units without conversion.
 */
SWMM_ENGINE_API int swmm_pollutant_get_gw_conc(SWMM_Engine engine, int idx, double* conc);

/**
 * @brief Get the initial concentration throughout the conveyance system.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[out] conc Receives the initial concentration.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note conc may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed pollutant/pollutant index check returns SWMM_ERR_BADINDEX. Returned concentration uses
 *     this pollutant row units without conversion.
 */
SWMM_ENGINE_API int swmm_pollutant_get_init_conc(SWMM_Engine engine, int idx, double* conc);

/**
 * @brief Set the RDII (Rainfall-Dependent Infiltration/Inflow) concentration.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] conc RDII concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard is imposed; values are assigned directly without
 *     finite/range validation unless noted in this function contract.
 */
SWMM_ENGINE_API int swmm_pollutant_set_rdii_conc(SWMM_Engine engine, int idx, double conc);

/**
 * @brief Get the RDII concentration.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[out] conc Receives the RDII concentration.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note conc may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed pollutant/pollutant index check returns SWMM_ERR_BADINDEX. Returned concentration uses
 *     this pollutant row units without conversion.
 */
SWMM_ENGINE_API int swmm_pollutant_get_rdii_conc(SWMM_Engine engine, int idx, double* conc);

/**
 * @brief Set the default dry weather sanitary flow concentration.
 * @details Runtime-settable: the value feeds the dry weather quality
 *          inflow term each routing step.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] conc DWF concentration in pollutant units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard is imposed; values are assigned directly without
 *     finite/range validation unless noted in this function contract.
 */
SWMM_ENGINE_API int swmm_pollutant_set_dwf_conc(SWMM_Engine engine, int idx, double conc);

/**
 * @brief Get the default dry weather sanitary flow concentration.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[out] conc Receives the DWF concentration.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note conc may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed pollutant/pollutant index check returns SWMM_ERR_BADINDEX. Returned concentration uses
 *     this pollutant row units without conversion.
 */
SWMM_ENGINE_API int swmm_pollutant_get_dwf_conc(SWMM_Engine engine, int idx, double* conc);

/**
 * @brief Set the molecular weight of a pollutant.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] mwt Molecular weight (g/mol).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard is imposed; values are assigned directly without
 *     finite/range validation unless noted in this function contract.
 */
SWMM_ENGINE_API int swmm_pollutant_set_mwt(SWMM_Engine engine, int idx, double mwt);

/**
 * @brief Get the molecular weight of a pollutant.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[out] mwt Receives the molecular weight.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note mwt may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A failed
 *     pollutant/pollutant index check returns SWMM_ERR_BADINDEX. Molecular weight is returned in
 *     the stored g/mol convention.
 */
SWMM_ENGINE_API int swmm_pollutant_get_mwt(SWMM_Engine engine, int idx, double* mwt);

/**
 * @brief Set a co-pollutant relationship (concentration = fraction * co-pollutant).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] co_idx Zero-based index of the co-pollutant.
 * @param[in] frac Fraction of the co-pollutant concentration.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard is imposed; values are assigned directly without
 *     finite/range validation unless noted in this function contract. Only idx is bounds-checked;
 *     co_idx and frac are stored without validating a referenced pollutant, fraction bounds or
 *     cycles. Use -1 to remove the co-pollutant relationship.
 */
SWMM_ENGINE_API int swmm_pollutant_set_co_pollutant(SWMM_Engine engine, int idx, int co_idx,
                                                    double frac);

/**
 * @brief Get a co-pollutant relationship.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[out] co_idx Receives the co-pollutant index (-1 if none).
 * @param[out] frac Receives the fraction.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note co_idx may be NULL to suppress that output. frac may be NULL to suppress that output. A
 *     null engine returns SWMM_ERR_BADHANDLE. A failed pollutant index check returns
 *     SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_pollutant_get_co_pollutant(SWMM_Engine engine, int idx, int* co_idx,
                                                    double* frac);

/**
 * @brief Set whether a pollutant only builds up during snowfall events.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[in] flag Non-zero for snow-only; zero otherwise.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard is imposed; values are assigned directly without
 *     finite/range validation unless noted in this function contract.
 */
SWMM_ENGINE_API int swmm_pollutant_set_snow_only(SWMM_Engine engine, int idx, int flag);

/**
 * @brief Get whether a pollutant only builds up during snowfall events.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pollutant index.
 * @param[out] flag Receives 1 for snow-only, 0 otherwise.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note flag may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed pollutant/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_pollutant_get_snow_only(SWMM_Engine engine, int idx, int* flag);

/* =========================================================================
 * Runtime quality injection
 * ========================================================================= */

/**
 * @brief Assign one stored node pollutant concentration directly.
 *
 * No RUNNING/lifecycle guard is imposed by this wrapper. This writes the concentration field
 * without conservative mass accounting, updating all related storage, or installing a persistent
 * forcing; subsequent quality routing may overwrite it.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access.
 * @param[in] node_idx Zero-based existing node index with allocated quality storage.
 * @param[in] pollut_idx Zero-based existing pollutant index.
 * @param[in] conc Concentration in the pollutant row units (mg/L, ug/L or count/L). Assigned
 *     without finite/nonnegative validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid
 *     element/pollutant.
 */
SWMM_ENGINE_API int swmm_node_set_quality(SWMM_Engine engine, int node_idx, int pollut_idx,
                                          double conc);

/**
 * @brief Assign one stored link pollutant concentration directly.
 *
 * No RUNNING/lifecycle guard is imposed by this wrapper. This writes the concentration field
 * without conservative mass accounting, updating all related storage, or installing a persistent
 * forcing; subsequent quality routing may overwrite it.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access.
 * @param[in] link_idx Zero-based existing link index with allocated quality storage.
 * @param[in] pollut_idx Zero-based existing pollutant index.
 * @param[in] conc Concentration in the pollutant row units (mg/L, ug/L or count/L). Assigned
 *     without finite/nonnegative validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADINDEX for invalid
 *     element/pollutant.
 */
SWMM_ENGINE_API int swmm_link_set_quality(SWMM_Engine engine, int link_idx, int pollut_idx,
                                          double conc);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_POLLUTANTS_H */
