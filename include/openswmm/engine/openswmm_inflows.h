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
 * @file      openswmm_inflows.h
 * @brief     OpenSWMM Engine — Inflows (External, DWF, RDII) C API.
 *
 * @details External inflow addition, dry weather flow, RDII assignment,
 *          and count queries.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_INFLOWS_H
#define OPENSWMM_INFLOWS_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * External inflows
 * ========================================================================= */

/**
 * @brief Add an external inflow to a node.
 *
 * @details External inflows define time-varying flows or pollutant loads
 *          applied at a node, optionally driven by a time series, with
 *          scaling, baseline, and pattern modifiers.
 *
 * @param[in] engine Engine handle.
 * @param[in] node_idx Zero-based index of the receiving node.
 * @param[in] constituent Constituent name ("FLOW" for flow, a pollutant name,
 *                   or the reserved species "__WATER_AGE__" — such a row
 *                   prescribes the AGE, in HOURS, carried by the node's
 *                   external inflow water, and wins over the
 *                   [WATER_AGE_SOURCES] EXTERNAL_INFLOW entry (Z1,
 *                   amendment D-Y4). It adds no water and no mass.
 * @param[in] ts_name Time series name (NULL or "" for constant baseline only).
 * @param[in] type Inflow type: "FLOW", "CONCEN", or "MASS".
 * @param[in] m_factor Multiplier applied to the time series values.
 * @param[in] s_factor Scale factor (unit conversion).
 * @param[in] baseline Constant baseline value added to the time series.
 * @param[in] pattern Time pattern name (NULL or "" for none).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard; mutations rebuild the inflow cache outside
 *     BUILDING/OPENED. Input strings are copied; unresolved names and numeric values are stored
 *     without validation here. A null type defaults to FLOW; constituent must be non-null. Appends
 *     a row rather than replacing an existing constituent/node pair.
 */
SWMM_ENGINE_API int swmm_ext_inflow_add(SWMM_Engine engine, int node_idx, const char* constituent,
                                        const char* ts_name, const char* type, double m_factor,
                                        double s_factor, double baseline, const char* pattern);

/**
 * @brief Read back an external inflow entry by index.
 *
 * @details The model stores external inflows as a flat SoA, indexed
 *          0..swmm_ext_inflow_count()-1. To list a single node's inflows,
 *          iterate all entries and filter by @p node_idx.
 *
 * @param[in] engine Engine handle.
 * @param[in] entry_idx Zero-based entry index.
 * @param[out] node_idx Receiving node index.
 * @param[in] constituent_buf Constituent name buffer (NUL-terminated).
 * @param[in] constituent_buflen Size of @p constituent_buf.
 * @param[out] ts_buf Time series name buffer (NUL-terminated; empty if none).
 * @param[in] ts_buflen Size of @p ts_buf.
 * @param[out] type_buf Inflow type buffer (NUL-terminated; "FLOW"/"CONCEN"/"MASS").
 * @param[in] type_buflen Size of @p type_buf.
 * @param[out] m_factor Multiplier factor.
 * @param[out] s_factor Scale factor.
 * @param[out] baseline Baseline value.
 * @param[out] pattern_buf Pattern name buffer (NUL-terminated; empty if none).
 * @param[in] pattern_buflen Size of @p pattern_buf.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Every declared output pointer is required; every text capacity must be
 *     positive, otherwise SWMM_ERR_BADPARAM. Text is copied into caller-owned buffers,
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_ext_inflow_get(SWMM_Engine engine, int entry_idx, int* node_idx,
                                        char* constituent_buf, int constituent_buflen, char* ts_buf,
                                        int ts_buflen, char* type_buf, int type_buflen,
                                        double* m_factor, double* s_factor, double* baseline,
                                        char* pattern_buf, int pattern_buflen);

/**
 * @brief Remove an external inflow entry by index.
 *
 * @details Entries with index > @p entry_idx shift down by one. Callers
 *          holding cached indices must re-resolve via swmm_ext_inflow_count()
 *          / iteration.
 *
 * @param[in] engine Engine handle.
 * @param[in] entry_idx Zero-based entry index (0..swmm_ext_inflow_count()-1).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard; mutations rebuild the inflow cache outside
 *     BUILDING/OPENED. Input strings are copied; unresolved names and numeric values are stored
 *     without validation here.
 */
SWMM_ENGINE_API int swmm_ext_inflow_remove(SWMM_Engine engine, int entry_idx);

/**
 * @brief Replace an external-inflow timeseries scale factor.
 *
 * Rebuilds the inflow cache in states other than BUILDING/OPENED so subsequent evaluation uses the
 * new value. Does not change m_factor, baseline or other entries.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] entry_idx Zero-based existing row index in the corresponding flat entry list.
 * @param[in] scale Stored s_factor, used in the constituent input-unit conversion; no finite/range
 *     validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row index,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_ext_inflow_set_scale(SWMM_Engine engine, int entry_idx, double scale);

/**
 * @brief Replace one external-inflow baseline.
 *
 * Rebuilds the inflow cache in states other than BUILDING/OPENED. Does not change the timeseries,
 * pattern or scale factors.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] entry_idx Zero-based existing row index in the corresponding flat entry list.
 * @param[in] baseline Constant baseline in the constituent input/display units; no finite/range
 *     validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row index,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_ext_inflow_set_baseline(SWMM_Engine engine, int entry_idx,
                                                 double baseline);

/* =========================================================================
 * Dry weather flow
 * ========================================================================= */

/**
 * @brief Add a dry weather flow component to a node.
 *
 * @details Dry weather flow represents the base sanitary flow entering the
 *          system at a node, modulated by up to four time patterns.
 *
 * @param[in] engine Engine handle.
 * @param[in] node_idx Zero-based index of the receiving node.
 * @param[in] constituent Constituent name ("FLOW" or a pollutant name).
 * @param[in] avg_value Average DWF value.
 * @param[in] pat1 Monthly time pattern name (NULL for none).
 * @param[in] pat2 Daily time pattern name (NULL for none).
 * @param[in] pat3 Hourly time pattern name (NULL for none).
 * @param[in] pat4 Weekend time pattern name (NULL for none).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard; mutations rebuild the inflow cache outside
 *     BUILDING/OPENED. Input strings are copied; unresolved names and numeric values are stored
 *     without validation here.
 */
SWMM_ENGINE_API int swmm_dwf_add(SWMM_Engine engine, int node_idx, const char* constituent,
                                 double avg_value, const char* pat1, const char* pat2,
                                 const char* pat3, const char* pat4);

/**
 * @brief Read back a dry weather flow entry by index.
 *
 * @param[in] engine Engine handle.
 * @param[in] entry_idx Zero-based entry index (0..swmm_dwf_count()-1).
 * @param[out] node_idx Receiving node index.
 * @param[in] constituent_buf Constituent name (NUL-terminated).
 * @param[in] constituent_buflen Size of @p constituent_buf.
 * @param[out] avg_value Average value.
 * @param[out] pat1_buf Monthly pattern name (NUL-terminated; empty if none).
 * @param[in] pat1_buflen Size of @p pat1_buf.
 * @param[out] pat2_buf Daily pattern name.
 * @param[in] pat2_buflen Size of @p pat2_buf.
 * @param[out] pat3_buf Hourly pattern name.
 * @param[in] pat3_buflen Size of @p pat3_buf.
 * @param[out] pat4_buf Weekend pattern name.
 * @param[in] pat4_buflen Size of @p pat4_buf.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Every declared output pointer is required; every text capacity must be
 *     positive, otherwise SWMM_ERR_BADPARAM. Text is copied into caller-owned buffers,
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_dwf_get(SWMM_Engine engine, int entry_idx, int* node_idx,
                                 char* constituent_buf, int constituent_buflen, double* avg_value,
                                 char* pat1_buf, int pat1_buflen, char* pat2_buf, int pat2_buflen,
                                 char* pat3_buf, int pat3_buflen, char* pat4_buf, int pat4_buflen);

/**
 * @brief Remove a DWF entry by index. Subsequent entries shift down.
 * @param[in] engine Engine handle.
 * @param[in] entry_idx Zero-based entry index (0..swmm_dwf_count()-1).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. No lifecycle guard; mutations rebuild the inflow cache outside
 *     BUILDING/OPENED. Input strings are copied; unresolved names and numeric values are stored
 *     without validation here.
 */
SWMM_ENGINE_API int swmm_dwf_remove(SWMM_Engine engine, int entry_idx);

/**
 * @brief Replace one dry-weather-flow average value.
 *
 * Rebuilds the inflow cache in states other than BUILDING/OPENED; preserves all four pattern
 * bindings.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] entry_idx Zero-based existing row index in the corresponding flat entry list.
 * @param[in] avg_value Average constituent value in input/display units (project flow units for
 *     FLOW); no finite/range validation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row index,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_dwf_set_baseline(SWMM_Engine engine, int entry_idx, double avg_value);

/* =========================================================================
 * RDII (Rainfall-Dependent Infiltration/Inflow)
 * ========================================================================= */

/**
 * @brief Add RDII inflow to a node using a unit hydrograph.
 *
 * @details Associates a node with a unit hydrograph group and its sewershed
 *          area to compute rainfall-dependent infiltration/inflow.
 *
 * @param[in] engine Engine handle.
 * @param[in] node_idx Zero-based index of the receiving node.
 * @param[in] uh_name Unit hydrograph group name.
 * @param[in] area Sewershed area in project area units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Copies the group name and stores area verbatim; group existence, area
 *     range and lifecycle are not checked. Does not rebuild the running RDII cache.
 */
SWMM_ENGINE_API int swmm_rdii_add(SWMM_Engine engine, int node_idx, const char* uh_name,
                                  double area);

/**
 * @brief Read back an RDII assignment by entry index.
 *
 * @param[in] engine Engine handle.
 * @param[in] entry_idx Zero-based index into the RDII assignment list (0..swmm_rdii_count()-1).
 * @param[out] node_idx Receiving node index.
 * @param[out] uh_buf Buffer to receive the unit hydrograph name (NUL-terminated, truncated if too
 *     small).
 * @param[in] buflen Size of @p uh_buf in bytes.
 * @param[out] area Sewershed area in project area units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Every declared output pointer is required; every text capacity must be
 *     positive, otherwise SWMM_ERR_BADPARAM. Text is copied into caller-owned buffers,
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_rdii_get(SWMM_Engine engine, int entry_idx, int* node_idx, char* uh_buf,
                                  int buflen, double* area);

/**
 * @brief Remove an RDII entry by index. Subsequent entries shift down.
 * @param[in] engine Engine handle.
 * @param[in] entry_idx Zero-based entry index (0..swmm_rdii_count()-1).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_rdii_remove(SWMM_Engine engine, int entry_idx);

/* =========================================================================
 * Unit hydrographs ([HYDROGRAPHS] section)
 * =========================================================================
 *
 * A unit hydrograph group is identified by name and has two kinds of input
 * lines:
 *   - A gage assignment line: "UHname  RainGage"
 *   - One or more parameter lines: "UHname Month Response R T K [Dmax Drecov Dinit]"
 *
 * Both kinds are added separately. `swmm_hydrograph_add` adds a parameter
 * line, `swmm_hydrograph_add_gage` adds the gage assignment. Counts and
 * getters are also separated.
 *
 * `response` is encoded as: 0 = SHORT, 1 = MEDIUM, 2 = LONG.
 * `month` is encoded as: 0..11 = JAN..DEC, or -1 = ALL.
 * ========================================================================= */

/**
 * @brief Add a unit hydrograph parameter line.
 *
 * @param[in] engine Engine handle.
 * @param[in] uh_name Unit hydrograph group name.
 * @param[in] month 0..11 = JAN..DEC, or -1 for ALL months.
 * @param[in] response 0 = SHORT, 1 = MEDIUM, 2 = LONG.
 * @param[in] r Fraction of rainfall volume that becomes RDII.
 * @param[in] t Time to peak (hours).
 * @param[in] k Ratio of recession-limb time to time-to-peak (>= 0).
 *                  Base time = t * (1 + k); the falling limb spans k * t hours.
 * @param[in] dmax Maximum initial-abstraction depth (project depth units; 0 if unused).
 * @param[in] drecov Linear-model IA recovery rate (project depth/day; 0 if unused or if
 *     [RDII_DECAY] is configured).
 * @param[in] dinit Initial IA already used at start of simulation (project depth units; 0 if
 *     unused).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. Always appends; duplicate keys are accepted.
 *     Required name pointers must be non-null, but empty/unresolved names are not rejected. No
 *     lifecycle guard or runtime RDII cache rebuild. Only month/response codes are range-checked;
 *     RTK and abstraction values are stored without finite/range validation.
 */
SWMM_ENGINE_API int swmm_hydrograph_add(SWMM_Engine engine, const char* uh_name, int month,
                                        int response, double r, double t, double k, double dmax,
                                        double drecov, double dinit);

/**
 * @brief Read back a hydrograph parameter entry by index.
 *
 * @param[in] engine Engine handle.
 * @param[in] entry_idx Zero-based index (0..swmm_hydrograph_count()-1).
 * @param[out] uh_buf UH group name buffer (NUL-terminated).
 * @param[in] buflen Size of @p uh_buf.
 * @param[out] month Month code (-1, or 0..11).
 * @param[out] response Response code (0..2).
 * @param[out] r Rainfall fraction.
 * @param[out] t Time to peak (hours).
 * @param[out] k Recession ratio.
 * @param[out] dmax IA max depth.
 * @param[out] drecov Linear IA recovery rate.
 * @param[out] dinit Initial IA used.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Every declared output pointer is required; every text capacity must be
 *     positive, otherwise SWMM_ERR_BADPARAM. Text is copied into caller-owned buffers,
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_hydrograph_get(SWMM_Engine engine, int entry_idx, char* uh_buf, int buflen,
                                        int* month, int* response, double* r, double* t, double* k,
                                        double* dmax, double* drecov, double* dinit);

/**
 * @brief Count stored hydrograph parameter rows, including duplicate keys.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @return Nonnegative row count, or -1 for a null engine.
 */
SWMM_ENGINE_API int swmm_hydrograph_count(SWMM_Engine engine);

/**
 * @brief Assign a rain gage to a unit hydrograph group.
 *
 * @param[in] engine Engine handle.
 * @param[in] uh_name Unit hydrograph group name.
 * @param[in] gage_name Rain gage name.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. Always appends; duplicate keys are accepted.
 *     Required name pointers must be non-null, but empty/unresolved names are not rejected. No
 *     lifecycle guard or runtime RDII cache rebuild.
 */
SWMM_ENGINE_API int swmm_hydrograph_add_gage(SWMM_Engine engine, const char* uh_name,
                                             const char* gage_name);

/**
 * @brief Read back a UH-to-gage assignment by index.
 *
 * @param[in] engine Engine handle.
 * @param[in] entry_idx Zero-based index (0..swmm_hydrograph_gage_count()-1).
 * @param[out] uh_buf UH group name (NUL-terminated).
 * @param[in] uh_buflen Size of @p uh_buf.
 * @param[out] gage_buf Rain gage name (NUL-terminated).
 * @param[in] gage_buflen Size of @p gage_buf.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Every declared output pointer is required; every text capacity must be
 *     positive, otherwise SWMM_ERR_BADPARAM. Text is copied into caller-owned buffers,
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_hydrograph_get_gage(SWMM_Engine engine, int entry_idx, char* uh_buf,
                                             int uh_buflen, char* gage_buf, int gage_buflen);

/**
 * @brief Count stored hydrograph-to-gage assignment rows.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @return Nonnegative row count, or -1 for a null engine.
 */
SWMM_ENGINE_API int swmm_hydrograph_gage_count(SWMM_Engine engine);

/**
 * @brief Count the unique unit-hydrograph group names defined.
 *
 * @details A unit-hydrograph "group" is identified by name; the engine stores
 *          one entry per `(group, month, response)`. This count is the number
 *          of *distinct* nonempty group names across parameter entries and gage assignments, useful
 * for
 *          enumerating groups in a UI without manually de-duplicating the
 *          per-entry list.
 *
 * @param[in] engine Engine handle.
 * @returns Number of unique groups, or -1 on error.
 */
SWMM_ENGINE_API int swmm_hydrograph_group_count(SWMM_Engine engine);

/**
 * @brief Read back the name of a unit-hydrograph group by its zero-based index.
 *
 * @details Groups are enumerated in first-occurrence order across the parameter
 *          entry list, then gage assignments; names are deduplicated case-insensitively.
 *          Gage-only groups are included; decay-only names are not.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based group index (0..swmm_hydrograph_group_count()-1).
 * @param[out] buf Caller-allocated buffer to receive the group name.
 * @param[in] buflen Size of @p buf in bytes.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Every declared output pointer is required; every text capacity must be
 *     positive, otherwise SWMM_ERR_BADPARAM. Text is copied into caller-owned buffers,
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_hydrograph_group_id(SWMM_Engine engine, int idx, char* buf, int buflen);

/* =========================================================================
 * Mutation surface (BS-02) — upsert + key-based remove + rename
 * =========================================================================
 *
 * The legacy `swmm_hydrograph_add` / `swmm_hydrograph_add_gage` surface above
 * is append-only, which prevents in-place edits from a UI. The following
 * setters/removers support the `HydrographGroupEditor` MVC layer in
 * openswmm.gui: every mutation in the editor routes through one of these
 * symbols, and the GUI's `SWMMModelLayer` emits a `hydrographChanged(uhName)`
 * signal so all subscribed views (Object Browser, property panel, picker
 * combos, etc.) refresh in lock-step.
 *
 * Upsert contract (set_rtk, set_ia, decay_set):
 *   - If an entry matching the supplied key exists, update only the fields
 *     this setter owns and leave the rest untouched.
 *   - Otherwise append a new entry with the supplied fields set and the
 *     unspecified fields zeroed (so set_rtk followed by set_ia on the same
 *     key composes correctly).
 *
 * Remove contract (remove_entry, remove_group, decay_remove):
 *   - Key-based, not index-based — indices shift as entries are removed and
 *     a UI cannot keep them in sync. Idempotent: returns SWMM_OK if no
 *     match is found.
 *
 * Group remove cascades to: [HYDROGRAPHS] parameter rows, gage assignments,
 * [RDII_DECAY] rows, AND any [RDII] node assignments referencing the group
 * (so the engine never sees a dangling UH-name reference after a delete).
 * ========================================================================= */

/**
 * @brief Upsert R/T/K parameters for one (group, month, response) row.
 *
 * @param[in] engine Engine handle.
 * @param[in] uh_name Unit hydrograph group name (non-null, non-empty).
 * @param[in] month 0..11 = JAN..DEC, or -1 for ALL.
 * @param[in] response 0 = SHORT, 1 = MEDIUM, 2 = LONG.
 * @param[in] r Rainfall fraction.
 * @param[in] t Time to peak (hours).
 * @param[in] k Recession-limb-to-peak-time ratio (>= 0). Base time = t * (1 + k).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. Matches the first case-insensitive
 *     (group,month,response) key; updates only the named fields, or appends with other numeric
 *     fields zero. Required names and month/response codes are checked, numeric values are not. No
 *     lifecycle guard or runtime RDII cache rebuild.
 */
SWMM_ENGINE_API int swmm_hydrograph_set_rtk(SWMM_Engine engine, const char* uh_name, int month,
                                            int response, double r, double t, double k);

/**
 * @brief Upsert linear-IA parameters for one (group, month, response) row.
 *
 * @param[in] engine Engine handle.
 * @param[in] uh_name Unit hydrograph group name (non-null, non-empty).
 * @param[in] month 0..11 = JAN..DEC, or -1 for ALL.
 * @param[in] response 0 = SHORT, 1 = MEDIUM, 2 = LONG.
 * @param[in] dmax Maximum initial-abstraction depth.
 * @param[in] drecov Linear IA recovery rate.
 * @param[in] dinit Initial IA already used.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. Matches the first case-insensitive
 *     (group,month,response) key; updates only the named fields, or appends with other numeric
 *     fields zero. Required names and month/response codes are checked, numeric values are not. No
 *     lifecycle guard or runtime RDII cache rebuild.
 */
SWMM_ENGINE_API int swmm_hydrograph_set_ia(SWMM_Engine engine, const char* uh_name, int month,
                                           int response, double dmax, double drecov, double dinit);

/**
 * @brief Remove the first matching hydrograph parameter row.
 *
 * Case-insensitive group key; absent rows succeed without mutation. Later flat indices shift down.
 * No lifecycle guard or runtime RDII cache rebuild.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] uh_name Non-null, nonempty copied unit-hydrograph group name; matched
 *     case-insensitively for hydrograph rows.
 * @param[in] month -1 for ALL months or 0..11 for January..December.
 * @param[in] response 0=SHORT, 1=MEDIUM or 2=LONG.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row index,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_hydrograph_remove_entry(SWMM_Engine engine, const char* uh_name, int month,
                                                 int response);

/**
 * @brief Remove all authoring rows and RDII assignments for a hydrograph group.
 *
 * Cascades through parameter rows, rain-gage assignments, exponential-decay rows and node RDII
 * references using the edit deletion helper. Absent groups succeed. No lifecycle guard or runtime
 * RDII cache rebuild.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] uh_name Non-null, nonempty borrowed group name; all deletion matches use exact case.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row index,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_hydrograph_remove_group(SWMM_Engine engine, const char* uh_name);

/**
 * @brief Remove a group's month-specific parameter rows while retaining ALL rows.
 *
 * Case-insensitive name comparison; retains every month=-1 row, other groups, gage assignments and
 * decay rows. Missing groups succeed. No lifecycle guard or runtime RDII cache rebuild.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] uh_name Non-null, nonempty copied unit-hydrograph group name; matched
 *     case-insensitively for hydrograph rows.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row index,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_hydrograph_clear_group_months(SWMM_Engine engine, const char* uh_name);

/**
 * @brief Set, replace, or clear the rain gage assigned to a UH group.
 *
 * @param[in] engine Engine handle.
 * @param[in] uh_name Unit hydrograph group name (non-null, non-empty).
 * @param[in] gage_name Rain gage name, or NULL/empty to clear an existing
 *                   assignment.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. Upserts the first case-insensitive group
 *     assignment; NULL/empty gage removes that first assignment if present. The referenced gage is
 *     not resolved here. No lifecycle guard or runtime RDII cache rebuild.
 */
SWMM_ENGINE_API int swmm_hydrograph_set_gage(SWMM_Engine engine, const char* uh_name,
                                             const char* gage_name);

/**
 * @brief Rename a UH group, propagating the new name to parameter rows,
 *        gage assignments, [RDII_DECAY] rows, and [RDII] node assignments.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based group index (0..swmm_hydrograph_group_count()-1).
 * @param[in] new_id New group name (non-null, non-empty, must not already exist).
 * @returns SWMM_OK on success, SWMM_ERR_BADPARAM if new_id is invalid or
 *          duplicates an existing group, or another error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Unique names use case-insensitive identity across parameter rows followed
 *     by gage assignments. Case-only renaming is permitted. Rewrites parameter/gage/decay/RDII
 *     references case-insensitively. No lifecycle guard or runtime RDII cache rebuild.
 */
SWMM_ENGINE_API int swmm_hydrograph_group_rename(SWMM_Engine engine, int idx, const char* new_id);

/**
 * @brief Upsert an exponential-depletion and temperature-dependent IA-recovery row.
 *
 * Updates the first exact-case (group,response) match or appends. Negative rate/melt-factor
 * comparisons are checked; provide finite values because no independent finiteness guard exists. No
 * lifecycle guard or runtime RDII cache rebuild.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] uh_name Copied group name; non-null, nonempty, matched case-sensitively. Group
 *     existence is not resolved here.
 * @param[in] response 0=SHORT, 1=MEDIUM or 2=LONG.
 * @param[in] k_dep Nonnegative depletion rate, inverse project rainfall-depth units.
 * @param[in] k_0 Nonnegative base recovery rate, per hour.
 * @param[in] k_T Nonnegative thermal recovery rate at T_ref, per hour.
 * @param[in] T_ref Reference temperature, degrees C.
 * @param[in] theta_rec Thermal recovery sensitivity, inverse degrees C.
 * @param[in] T_freeze Air-temperature threshold below which recovery is suppressed, degrees C.
 * @param[in] snow_on Zero disables; any nonzero value enables this row's degree-day snow model.
 * @param[in] snow_T Rain/snow partition and melt-base temperature, degrees C.
 * @param[in] snow_ddf Nonnegative melt factor, project rainfall-depth units per degree C per day.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row index,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_rdii_decay_set(SWMM_Engine engine, const char* uh_name, int response,
                                        double k_dep, double k_0, double k_T, double T_ref,
                                        double theta_rec, double T_freeze, int snow_on,
                                        double snow_T, double snow_ddf);

/**
 * @brief Remove the first exponential-decay row matching a group and response.
 *
 * Absent keys succeed. Later flat row indices shift down. Does not remove hydrograph parameters or
 * RDII node assignments; no runtime RDII cache rebuild.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] uh_name Non-null, nonempty group name; this decay-row lookup is case-sensitive.
 * @param[in] response 0=SHORT, 1=MEDIUM or 2=LONG.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row index,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_rdii_decay_remove(SWMM_Engine engine, const char* uh_name, int response);

/* =========================================================================
 * Exponential IA decay ([RDII_DECAY] section)
 * =========================================================================
 *
 * Physics-based replacement for the legacy linear IA recovery. When a row
 * is present for a (UH group, response) pair, the exponential depletion /
 * temperature-dependent recovery model is used in place of the linear
 * `drecov` rate from the corresponding [HYDROGRAPHS] entry.
 *
 * @see docs/RDII_ExpDecay_Implementation.md
 * ========================================================================= */

/**
 * @brief Append an exponential-depletion and temperature-dependent IA-recovery row.
 *
 * Always appends; duplicate keys are permitted. Negative rate/melt-factor comparisons are checked;
 * provide finite values because no independent finiteness guard exists. No lifecycle guard or
 * runtime RDII cache rebuild.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @param[in] uh_name Copied group name; non-null; empty names are not rejected. Group existence is
 *     not resolved here.
 * @param[in] response 0=SHORT, 1=MEDIUM or 2=LONG.
 * @param[in] k_dep Nonnegative depletion rate, inverse project rainfall-depth units.
 * @param[in] k_0 Nonnegative base recovery rate, per hour.
 * @param[in] k_T Nonnegative thermal recovery rate at T_ref, per hour.
 * @param[in] T_ref Reference temperature, degrees C.
 * @param[in] theta_rec Thermal recovery sensitivity, inverse degrees C.
 * @param[in] T_freeze Air-temperature threshold below which recovery is suppressed, degrees C.
 * @param[in] snow_on Zero disables; any nonzero value enables this row's degree-day snow model.
 * @param[in] snow_T Rain/snow partition and melt-base temperature, degrees C.
 * @param[in] snow_ddf Nonnegative melt factor, project rainfall-depth units per degree C per day.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid row index,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_rdii_decay_add(SWMM_Engine engine, const char* uh_name, int response,
                                        double k_dep, double k_0, double k_T, double T_ref,
                                        double theta_rec, double T_freeze, int snow_on,
                                        double snow_T, double snow_ddf);

/**
 * @brief Read back an exponential-decay parameter row by index.
 *
 * @param[in] engine Engine handle.
 * @param[in] entry_idx Zero-based index (0..swmm_rdii_decay_count()-1).
 * @param[out] uh_buf UH group name (NUL-terminated).
 * @param[in] buflen Size of @p uh_buf.
 * @param[out] response Response code (0..2).
 * @param[out] k_dep Depletion rate.
 * @param[out] k_0 Base recovery rate.
 * @param[out] k_T Thermal recovery rate.
 * @param[out] T_ref Reference temperature.
 * @param[out] theta_rec Temperature sensitivity.
 * @param[out] T_freeze Frozen-ground threshold.
 * @param[out] snow_on Degree-day snow model flag (0/1).
 * @param[out] snow_T Rain/snow partition threshold & melt base (deg C).
 * @param[out] snow_ddf Degree-day melt factor.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed inflow row/pollutant index check returns
 *     SWMM_ERR_BADINDEX. Every declared output pointer is required; every text capacity must be
 *     positive, otherwise SWMM_ERR_BADPARAM. Text is copied into caller-owned buffers,
 *     NUL-terminated and silently truncated to capacity minus one byte.
 */
SWMM_ENGINE_API int swmm_rdii_decay_get(SWMM_Engine engine, int entry_idx, char* uh_buf, int buflen,
                                        int* response, double* k_dep, double* k_0, double* k_T,
                                        double* T_ref, double* theta_rec, double* T_freeze,
                                        int* snow_on, double* snow_T, double* snow_ddf);

/**
 * @brief Count stored exponential-decay rows.
 *
 * @param[in] engine Non-null live engine with exclusive caller access.
 * @return Nonnegative row count, or -1 for a null engine.
 */
SWMM_ENGINE_API int swmm_rdii_decay_count(SWMM_Engine engine);

/* =========================================================================
 * Count queries
 * ========================================================================= */

/**
 * @brief Get the total number of external inflows defined.
 * @param[in] engine Engine handle.
 * @returns Number of external inflows, or -1 on error.
 */
SWMM_ENGINE_API int swmm_ext_inflow_count(SWMM_Engine engine);

/**
 * @brief Get the total number of dry weather flow entries defined.
 * @param[in] engine Engine handle.
 * @returns Number of DWF entries, or -1 on error.
 */
SWMM_ENGINE_API int swmm_dwf_count(SWMM_Engine engine);

/**
 * @brief Get the total number of RDII entries defined.
 * @param[in] engine Engine handle.
 * @returns Number of RDII entries, or -1 on error.
 */
SWMM_ENGINE_API int swmm_rdii_count(SWMM_Engine engine);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_INFLOWS_H */
