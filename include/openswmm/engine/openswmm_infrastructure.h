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
 * @file      openswmm_infrastructure.h
 * @brief     OpenSWMM Engine — Infrastructure (Transects, Streets, Inlets, LIDs) C API.
 *
 * @details Transect creation and station data, street parameters, inlet
 *          definitions, LID control layers, and LID usage assignment.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_INFRASTRUCTURE_H
#define OPENSWMM_INFRASTRUCTURE_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Transects
 * ========================================================================= */

/**
 * @brief Add a new transect for irregular cross-sections.
 * @param[in] engine Engine handle; see this operation's lifecycle note.
 * @param[in] id Unique null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. No lifecycle guard in this wrapper; authoring
 *     edits do not generally rebuild running hydraulic/geometry caches. Rejects case-insensitive
 *     duplicate IDs; NULL is refused but an empty ID is accepted.
 */
SWMM_ENGINE_API int swmm_transect_add(SWMM_Engine engine, const char* id);

/**
 * @brief Set Manning's roughness for left overbank, right overbank, and channel.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[in] n_left Manning's n for the left overbank.
 * @param[in] n_right Manning's n for the right overbank.
 * @param[in] n_channel Manning's n for the main channel.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches. Numeric values are stored verbatim
 *     without finite/range validation.
 */
SWMM_ENGINE_API int swmm_transect_set_roughness(SWMM_Engine engine, int idx, double n_left,
                                                double n_right, double n_channel);

/**
 * @brief Add a station–elevation data point to a transect.
 *
 * @details Stations must be added in order from left to right across the
 *          cross-section.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[in] station Horizontal distance (station) from a reference.
 * @param[in] elevation Elevation at this station.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Numeric values are stored verbatim without finite/range
 *     validation.
 */
SWMM_ENGINE_API int swmm_transect_add_station(SWMM_Engine engine, int idx, double station,
                                              double elevation);

/**
 * @brief Get the total number of transects in the model.
 * @param[in] engine Engine handle.
 * @returns Number of transects, or -1 on error.
 *
 *
 * @note Null engine returns -1.
 */
SWMM_ENGINE_API int swmm_transect_count(SWMM_Engine engine);

/**
 * @brief Look up a transect's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated transect identifier.
 * @returns Zero-based index, or -1 if not found.
 *
 *
 * @note Name lookup is case-insensitive; null engine/name or no match returns -1.
 */
SWMM_ENGINE_API int swmm_transect_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a transect by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note Returned text borrows engine storage: do not free; copy before editing the table or
 *     closing/destroying the engine. NULL is returned for a null engine or invalid index.
 */
SWMM_ENGINE_API const char* swmm_transect_id(SWMM_Engine engine, int idx);

/* -------------------------------------------------------------------------
 * Per-field getters / setters (DA-ENG-09 + BQ-TR-02)
 *
 * GUI editors (Slice BQ Phase 6.7.4 TransectEditor) need round-trip access
 * to every transect field; the legacy 3-function surface (add / set_roughness
 * / add_station) only covers a fraction. The functions below close that gap.
 * ------------------------------------------------------------------------- */

/**
 * @brief Get the Manning's roughness values for a transect.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[out] n_left Manning's n for the left overbank. May be NULL.
 * @param[out] n_right Manning's n for the right overbank. May be NULL.
 * @param[out] n_channel Manning's n for the main channel. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note n_left may be NULL to suppress that output. n_right may be NULL to suppress that output.
 *     n_channel may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed infrastructure/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_transect_get_roughness(SWMM_Engine engine, int idx, double* n_left,
                                                double* n_right, double* n_channel);

/**
 * @brief Set the left and right bank stations for a transect.
 *
 * @details Bank stations delimit the main channel from the overbanks; they
 *          are independent of the encroachment stations (BQ-TR-02).
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[in] x_left Station of the left bank.
 * @param[in] x_right Station of the right bank.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches. Numeric values are stored verbatim
 *     without finite/range validation.
 */
SWMM_ENGINE_API int swmm_transect_set_bank_stations(SWMM_Engine engine, int idx, double x_left,
                                                    double x_right);

/**
 * @brief Get the left and right bank stations for a transect.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[out] x_left Station of the left bank. May be NULL.
 * @param[out] x_right Station of the right bank. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note x_left may be NULL to suppress that output. x_right may be NULL to suppress that output. A
 *     null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check returns
 *     SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_transect_get_bank_stations(SWMM_Engine engine, int idx, double* x_left,
                                                    double* x_right);

/**
 * @brief Set the left and right encroachment stations for a transect (BQ-TR-02).
 *
 * @details Encroachment stations are distinct from bank stations and identify
 *          floodplain encroachment limits (HEC-RAS convention). On legacy
 *          `[TRANSECTS]` X1 records that omit the trailing encroachment
 *          columns, the INP parser may default these to the bank stations
 *          to preserve backward compatibility; callers writing programmatic
 *          values via this API are setting them explicitly.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[in] x_left Station of the left encroachment limit.
 * @param[in] x_right Station of the right encroachment limit.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches. Numeric values are stored verbatim
 *     without finite/range validation.
 */
SWMM_ENGINE_API int swmm_transect_set_encroachment_stations(SWMM_Engine engine, int idx,
                                                            double x_left, double x_right);

/**
 * @brief Get the left and right encroachment stations for a transect (BQ-TR-02).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[out] x_left Station of the left encroachment limit. May be NULL.
 * @param[out] x_right Station of the right encroachment limit. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note x_left may be NULL to suppress that output. x_right may be NULL to suppress that output. A
 *     null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check returns
 *     SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_transect_get_encroachment_stations(SWMM_Engine engine, int idx,
                                                            double* x_left, double* x_right);

/**
 * @brief Set the station, elevation, and meander modifiers for a transect.
 *
 * @details Maps to the `xFactor`, `yFactor`, and `lengthFactor` parameters
 *          on the `[TRANSECTS]` X1 record:
 *          - `x_factor`      = station-spacing multiplier (default 1.0).
 *          - `y_factor`      = elevation offset added to every station
 *                              elevation (default 0.0 — engine stores 1.0
 *                              as a no-op marker at `swmm_transect_add` time).
 *          - `length_factor` = meander factor = channel / floodplain length
 *                              ratio (default 1.0).
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[in] x_factor Station spacing multiplier.
 * @param[in] y_factor Elevation offset.
 * @param[in] length_factor Meander factor (channel/floodplain length ratio).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches. Numeric values are stored verbatim
 *     without finite/range validation.
 */
SWMM_ENGINE_API int swmm_transect_set_modifiers(SWMM_Engine engine, int idx, double x_factor,
                                                double y_factor, double length_factor);

/**
 * @brief Get the station, elevation, and meander modifiers for a transect.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[out] x_factor Station spacing multiplier. May be NULL.
 * @param[out] y_factor Elevation offset. May be NULL.
 * @param[out] length_factor Meander factor. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note x_factor may be NULL to suppress that output. y_factor may be NULL to suppress that output.
 *     length_factor may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE.
 *     A failed infrastructure/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_transect_get_modifiers(SWMM_Engine engine, int idx, double* x_factor,
                                                double* y_factor, double* length_factor);

/**
 * @brief Set the free-form comments / description for a transect.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[in] text Null-terminated comment string. NULL clears the comment.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches. Numeric values are stored verbatim
 *     without finite/range validation.
 */
SWMM_ENGINE_API int swmm_transect_set_comments(SWMM_Engine engine, int idx, const char* text);

/**
 * @brief Get the free-form comments / description for a transect.
 *
 * @details Writes the comment into @p buf with NUL termination; truncates
 *          if the buffer is smaller than the comment. Always returns SWMM_OK
 *          (an empty comment results in @p buf[0] == '\0').
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[out] buf Destination buffer.
 * @param[in] buflen Size of @p buf in bytes (must be > 0).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Required output buffer must be non-null with positive capacity.
 *     Copies a terminating NUL and silently truncates to capacity-1 bytes.
 */
SWMM_ENGINE_API int swmm_transect_get_comments(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Get the number of station–elevation points stored for a transect.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @returns Station count, or -1 on error.
 *
 *
 * @note Null engine returns -1.
 */
SWMM_ENGINE_API int swmm_transect_get_station_count(SWMM_Engine engine, int idx);

/**
 * @brief Get a single station–elevation pair from a transect.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[in] station_idx Zero-based station-pair index.
 * @param[out] station Horizontal distance. May be NULL.
 * @param[out] elevation Elevation at this station. May be NULL.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note station may be NULL to suppress that output. elevation may be NULL to suppress that output.
 *     A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_transect_get_station(SWMM_Engine engine, int idx, int station_idx,
                                              double* station, double* elevation);

/**
 * @brief Remove all station–elevation pairs from a transect.
 *
 * @details Used by the GUI's snapshot-and-rewrite path: clear then re-add
 *          the full station list in one shot.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches.
 */
SWMM_ENGINE_API int swmm_transect_clear_stations(SWMM_Engine engine, int idx);

/**
 * @brief Rename an existing transect.
 *
 * @details Refuses on collision with another existing transect name (case
 *          insensitive); same-name is a no-op SWMM_OK.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @param[in] new_id New null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches.
 */
SWMM_ENGINE_API int swmm_transect_rename(SWMM_Engine engine, int idx, const char* new_id);

/**
 * @brief Remove a transect by index.
 *
 * @details Out-of-range indices are a SWMM_OK no-op (mirrors the pattern
 *          mutation API — see test_pattern_mutation_api.cpp). Remaining
 *          transects preserve their relative order; their indices shift
 *          down by one.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based transect index.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. No lifecycle guard in this wrapper; authoring
 *     edits do not generally rebuild running hydraulic/geometry caches. Out-of-range indices
 *     succeed without mutation. Erases only the transect arrays; does not nullify/renumber
 *     referring link cross-sections. Use swmm_transect_delete() for cascaded reference repair.
 */
SWMM_ENGINE_API int swmm_transect_remove(SWMM_Engine engine, int idx);

/* =========================================================================
 * Streets
 * ========================================================================= */

/**
 * @brief Add a new street cross-section definition.
 * @param[in] engine Engine handle; see this operation's lifecycle note.
 * @param[in] id Unique null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. No lifecycle guard in this wrapper; authoring
 *     edits do not generally rebuild running hydraulic/geometry caches. Required strings are
 *     copied; empty/duplicate IDs and unresolved type tokens are not rejected here.
 */
SWMM_ENGINE_API int swmm_street_add(SWMM_Engine engine, const char* id);

/**
 * @brief Rename a street cross-section in place.
 *
 * @details Unlike the other named infrastructure objects, a street is
 *          referenced BY NAME after parsing: STREET_XSECT links carry it in
 *          their named cross-section slot, which @ref swmm_link_get_xsect
 *          resolves and the .inp writer emits as [XSECTIONS] Geom1. This call
 *          rewrites every such reference, so renaming cannot leave a link
 *          pointing at a street that no longer exists.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Street index.
 * @param[in] new_id New unique name (non-empty), compared case-insensitively
 *                against the other streets. A pure case-respelling of this
 *                same street is allowed.
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM on an empty or duplicate name.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches.
 */
SWMM_ENGINE_API int swmm_street_rename(SWMM_Engine engine, int idx, const char* new_id);

/**
 * @brief Set the geometric parameters for a street cross-section.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based street index.
 * @param[in] t_crown Crown thickness (rise of the crown above the gutter).
 * @param[in] h_curb Curb height.
 * @param[in] sx Cross slope of the roadway.
 * @param[in] n_road Manning's n for the road surface.
 * @param[in] gutter_depres Gutter depression depth.
 * @param[in] gutter_width Gutter width.
 * @param[in] sides Number of sides (1 or 2).
 * @param[in] back_width Backing (sidewalk) width.
 * @param[in] back_slope Backing slope.
 * @param[in] back_n Manning's n for the backing area.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches. Numeric values are stored verbatim
 *     without finite/range validation.
 */
SWMM_ENGINE_API int swmm_street_set_params(SWMM_Engine engine, int idx, double t_crown,
                                           double h_curb, double sx, double n_road,
                                           double gutter_depres, double gutter_width, int sides,
                                           double back_width, double back_slope, double back_n);

/**
 * @brief Get the total number of street definitions in the model.
 * @param[in] engine Engine handle.
 * @returns Number of streets, or -1 on error.
 *
 *
 * @note Null engine returns -1.
 */
SWMM_ENGINE_API int swmm_street_count(SWMM_Engine engine);

/**
 * @brief Look up a street's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated street identifier.
 * @returns Zero-based index, or -1 if not found.
 *
 *
 * @note Name lookup is case-insensitive; null engine/name or no match returns -1.
 */
SWMM_ENGINE_API int swmm_street_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a street by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based street index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note Returned text borrows engine storage: do not free; copy before editing the table or
 *     closing/destroying the engine. NULL is returned for a null engine or invalid index.
 */
SWMM_ENGINE_API const char* swmm_street_id(SWMM_Engine engine, int idx);

/**
 * @brief Read back the geometric parameters of a street cross-section.
 *
 * @details Inverse of @ref swmm_street_set_params. Any out-pointer may be NULL
 *          if that field is not needed. Values are returned in the same units
 *          they were supplied (display units).
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based street index.
 * @param[out] t_crown Crown thickness.
 * @param[out] h_curb Curb height.
 * @param[out] sx Cross slope of the roadway.
 * @param[out] n_road Manning's n for the road surface.
 * @param[out] gutter_depres Gutter depression depth.
 * @param[out] gutter_width Gutter width.
 * @param[out] sides Number of sides (1 or 2).
 * @param[out] back_width Backing (sidewalk) width.
 * @param[out] back_slope Backing slope.
 * @param[out] back_n Manning's n for the backing area.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note t_crown may be NULL to suppress that output. h_curb may be NULL to suppress that output. sx
 *     may be NULL to suppress that output. n_road may be NULL to suppress that output.
 *     gutter_depres may be NULL to suppress that output. gutter_width may be NULL to suppress that
 *     output. sides may be NULL to suppress that output. back_width may be NULL to suppress that
 *     output. back_slope may be NULL to suppress that output. back_n may be NULL to suppress that
 *     output. A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index
 *     check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_street_get_params(SWMM_Engine engine, int idx, double* t_crown,
                                           double* h_curb, double* sx, double* n_road,
                                           double* gutter_depres, double* gutter_width, int* sides,
                                           double* back_width, double* back_slope, double* back_n);

/* =========================================================================
 * Inlets
 * ========================================================================= */

/**
 * @brief Add a new inlet definition.
 * @param[in] engine Engine handle; see this operation's lifecycle note.
 * @param[in] id Unique null-terminated identifier.
 * @param[in] type Inlet type string (e.g., "GRATE", "CURB", "SLOTTED", "CUSTOM").
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. No lifecycle guard in this wrapper; authoring
 *     edits do not generally rebuild running hydraulic/geometry caches. Required strings are
 *     copied; empty/duplicate IDs and unresolved type tokens are not rejected here.
 */
SWMM_ENGINE_API int swmm_inlet_add(SWMM_Engine engine, const char* id, const char* type);

/**
 * @brief Rename an inlet design in place.
 *
 * @details [INLET_USAGE] rows hold an inlet INDEX, not a name, so usages
 *          follow automatically.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Inlet index.
 * @param[in] new_id New unique name (non-empty), compared case-insensitively
 *                against the other inlets. A pure case-respelling of this
 *                same inlet is allowed.
 * @returns SWMM_OK, or SWMM_ERR_BADPARAM on an empty or duplicate name.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches.
 */
SWMM_ENGINE_API int swmm_inlet_rename(SWMM_Engine engine, int idx, const char* new_id);

/**
 * @brief Set the geometric parameters for an inlet.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based inlet index.
 * @param[in] length Inlet length.
 * @param[in] width Inlet width.
 * @param[in] grate_type Grate type string (e.g., "P-50", "GENERIC").
 * @param[in] open_area Open area fraction (0–1).
 * @param[in] splash_veloc Splash-over velocity threshold.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches. Numeric values are stored verbatim
 *     without finite/range validation.
 */
SWMM_ENGINE_API int swmm_inlet_set_params(SWMM_Engine engine, int idx, double length, double width,
                                          const char* grate_type, double open_area,
                                          double splash_veloc);

/**
 * @brief Get the total number of inlet definitions in the model.
 * @param[in] engine Engine handle.
 * @returns Number of inlets, or -1 on error.
 *
 *
 * @note Null engine returns -1.
 */
SWMM_ENGINE_API int swmm_inlet_count(SWMM_Engine engine);

/**
 * @brief Look up an inlet's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated inlet identifier.
 * @returns Zero-based index, or -1 if not found.
 *
 *
 * @note Name lookup is case-insensitive; null engine/name or no match returns -1.
 */
SWMM_ENGINE_API int swmm_inlet_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of an inlet by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based inlet index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note Returned text borrows engine storage: do not free; copy before editing the table or
 *     closing/destroying the engine. NULL is returned for a null engine or invalid index.
 */
SWMM_ENGINE_API const char* swmm_inlet_id(SWMM_Engine engine, int idx);

/**
 * @brief Read an inlet's geometric parameters. Inverse of @ref swmm_inlet_set_params.
 *
 * @details Lets the GUI inlet editor load an existing definition instead of
 *          only writing one. Any out-param may be null. The inlet @e type
 *          string (set at @ref swmm_inlet_add) is read separately via
 *          @ref swmm_inlet_get_type.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based inlet index.
 * @param[out] length Receives the inlet length.
 * @param[out] width Receives the inlet width.
 * @param[out] grate_type Buffer for the grate-type string (NUL-terminated,
 *                          truncated to `grate_buflen`); may be null to skip.
 * @param[in] grate_buflen Size of `grate_type` in bytes; ignored when
 *                          `grate_type` is null.
 * @param[out] open_area Receives the open-area fraction.
 * @param[out] splash_veloc Receives the splash-over velocity threshold.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note length may be NULL to suppress that output. width may be NULL to suppress that output.
 *     grate_type may be NULL to suppress that output. open_area may be NULL to suppress that
 *     output. splash_veloc may be NULL to suppress that output. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check returns SWMM_ERR_BADINDEX.
 *     For CURB/DROP_CURB, length/width map to curb length/height. Other kinds use grate/slot
 *     fields. A requested grate buffer must have positive capacity; numeric outputs may already be
 *     written if that capacity fails.
 */
SWMM_ENGINE_API int swmm_inlet_get_params(SWMM_Engine engine, int idx, double* length,
                                          double* width, char* grate_type, int grate_buflen,
                                          double* open_area, double* splash_veloc);

/**
 * @brief Read an inlet's type string (the value passed to @ref swmm_inlet_add).
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based inlet index.
 * @param[out] buf Destination buffer (NUL-terminated, truncated to `buflen`).
 * @param[in] buflen Size of `buf` in bytes (must be > 0).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Required output buffer must be non-null with positive capacity.
 *     Copies a terminating NUL and silently truncates to capacity-1 bytes.
 */
SWMM_ENGINE_API int swmm_inlet_get_type(SWMM_Engine engine, int idx, char* buf, int buflen);

/* -------------------------------------------------------------------------
 * Full inlet-design surface (2026-09-05). swmm_inlet_set_params/get_params
 * above remain as a GRATE/SLOTTED convenience; the struct API below covers
 * every field of the [INLETS] grammar including curb height, throat angle,
 * combination inlets and custom capture curves.
 * ------------------------------------------------------------------------- */

/** Inlet design type codes (match legacy InletType ordering). */
enum SWMM_InletType {
    SWMM_INLET_GRATE = 0,
    SWMM_INLET_CURB = 1,
    SWMM_INLET_COMBO = 2,
    SWMM_INLET_SLOTTED = 3,
    SWMM_INLET_DROP_GRATE = 4,
    SWMM_INLET_DROP_CURB = 5,
    SWMM_INLET_CUSTOM = 6
};

/** Grate bar patterns (HEC-22). */
enum SWMM_GrateType {
    SWMM_GRATE_P_BAR_50 = 0,
    SWMM_GRATE_P_BAR_50x100 = 1,
    SWMM_GRATE_P_BAR_30 = 2,
    SWMM_GRATE_CURVED_VANE = 3,
    SWMM_GRATE_TILT_BAR_45 = 4,
    SWMM_GRATE_TILT_BAR_30 = 5,
    SWMM_GRATE_RETICULINE = 6,
    SWMM_GRATE_GENERIC = 7
};

/** Curb-opening throat orientation (legacy ThroatAngleWords order). */
enum SWMM_ThroatType {
    SWMM_THROAT_HORIZONTAL = 0,
    SWMM_THROAT_INCLINED = 1,
    SWMM_THROAT_VERTICAL = 2
};

/** Custom-inlet curve kind. */
enum SWMM_InletCurveKind {
    SWMM_INLET_CURVE_NONE = 0,
    SWMM_INLET_CURVE_DIVERSION = 1, /**< captured flow vs approach flow */
    SWMM_INLET_CURVE_RATING = 2     /**< captured flow vs water depth */
};

/** Inlet placement mode. */
enum SWMM_InletPlacement {
    SWMM_INLET_AUTOMATIC = 0,
    SWMM_INLET_ON_GRADE = 1,
    SWMM_INLET_ON_SAG = 2
};

/**
 * @brief Complete description of an inlet design. All dimensions are in the
 *        project's display units (ft or m; ft/s or m/s).
 */
typedef struct SWMM_InletDesign {
    int type; /**< SWMM_InletType */
    /* GRATE / DROP_GRATE / COMBO */
    double grate_length;
    double grate_width;
    int grate_type;      /**< SWMM_GrateType */
    double open_area;    /**< GENERIC only, fraction (0,1] */
    double splash_veloc; /**< GENERIC only */
    /* CURB / DROP_CURB / COMBO */
    double curb_length;
    double curb_height;
    int throat; /**< SWMM_ThroatType (ignored for DROP_CURB) */
    /* SLOTTED */
    double slot_length;
    double slot_width;
    /* CUSTOM */
    char curve_id[64];
    int curve_kind; /**< SWMM_InletCurveKind */
} SWMM_InletDesign;

/**
 * @brief Read every field of an inlet design.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based inlet index.
 * @param[out] out Receives the design (must not be NULL).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_inlet_get_design(SWMM_Engine engine, int idx, SWMM_InletDesign* out);

/**
 * @brief Validate and replace the type-specific inlet design.
 *
 * Requires BUILDING/OPENED. Validates only fields used by type: positive geometric dimensions;
 * valid type/grate/throat codes; GENERIC open_area in (0,1]; nonnegative splash velocity. CUSTOM
 * requires nonempty curve_id and a valid curve_kind; existing curves must be DIVERSION/RATING and
 * match any explicit kind. Unresolved custom names are retained for later resolution. Clears fields
 * unused by the new type. Numeric comparison guards are not independent finite checks; supply
 * finite values.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[in] design Non-null borrowed design copied on success; embedded curve_id must be
 *     NUL-terminated within its 64-byte array. Dimensions use ft/m and velocities ft/s or m/s.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_inlet_set_design(SWMM_Engine engine, int idx,
                                          const SWMM_InletDesign* design);

/**
 * @brief Copy an inlet design comment.
 *
 * No lifecycle guard. Does not change inlet geometry or usages.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[out] buf Non-null caller-owned text buffer; NUL-terminated and silently truncated to
 *     buflen-1 bytes.
 * @param[in] buflen Positive capacity in bytes including the terminating NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_inlet_get_comment(SWMM_Engine engine, int idx, char* buf, int buflen);
/**
 * @brief Replace an inlet design comment.
 *
 * No lifecycle guard. Does not change inlet geometry or usages.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[in] text Borrowed NUL-terminated text copied into the model; NULL clears it.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_inlet_set_comment(SWMM_Engine engine, int idx, const char* text);

/* -------------------------------------------------------------------------
 * Inlet usage rows — one per placement. Two host kinds:
 *   host_kind = SWMM_INLET_HOST_LINK : a conduit ([INLET_USAGE] row)
 *   host_kind = SWMM_INLET_HOST_NODE : an inlet junction ([INLET_JUNCTIONS] row)
 * ------------------------------------------------------------------------- */

enum SWMM_InletHostKind { SWMM_INLET_HOST_LINK = 0, SWMM_INLET_HOST_NODE = 1 };

typedef struct SWMM_InletUsage {
    int host_kind;        /**< SWMM_InletHostKind */
    int host_idx;         /**< link index or node index per host_kind */
    int design_idx;       /**< inlet design index */
    int capture_node_idx; /**< receiving (underdrain) node index */
    int num_inlets;       /**< inlets per side (>= 1) */
    double pct_clogged;   /**< 0..99 */
    double flow_limit;    /**< max capture per inlet, display flow units; 0 = none */
    double local_depress; /**< local gutter depression, display length units */
    double local_width;   /**< local depression width, display length units */
    int placement;        /**< SWMM_InletPlacement */
} SWMM_InletUsage;

/**
 * @brief Count inlet-usage rows of both host kinds.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @return Nonnegative row count, or -1 for null engine.
 */
SWMM_ENGINE_API int swmm_inlet_usage_count(SWMM_Engine engine);

/**
 * @brief Find the first inlet-usage row hosted by a link.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] link_idx Zero-based host link index; this lookup does not separately validate the host
 *     table.
 * @return Matching usage index, or -1 for no match/null engine.
 */
SWMM_ENGINE_API int swmm_inlet_usage_find_link(SWMM_Engine engine, int link_idx);

/**
 * @brief Find the first inlet-usage row hosted by a node.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node_idx Zero-based host node index; this lookup does not separately validate the host
 *     table.
 * @return Matching usage index, or -1 for no match/null engine.
 */
SWMM_ENGINE_API int swmm_inlet_usage_find_node(SWMM_Engine engine, int node_idx);

/**
 * @brief Read a complete inlet placement row.
 *
 * Returns percentage clogged as (1-clog_factor)*100; performs no lifecycle check.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] usage_idx Zero-based existing usage-row index.
 * @param[out] out Non-null caller-owned struct receiving host/design/capture indices and
 *     display-unit parameters.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_inlet_usage_get(SWMM_Engine engine, int usage_idx, SWMM_InletUsage* out);

/**
 * @brief Validate and upsert one inlet placement per host.
 *
 * Requires BUILDING/OPENED. Copies the row and clears deferred design/capture names. Flow and local
 * dimensions are display flow/length units. Supply finite numbers because comparison guards do not
 * independently reject NaN.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] usage Non-null borrowed struct. Host must be a conduit or already flagged inlet
 *     junction, design/capture indices valid; capture must not be virtual or the host node.
 *     num_inlets>=1, clogging 0..99 percent, nonnegative flow_limit/local dimensions and valid
 *     placement code.
 * @param[out] usage_idx Optional row index, written only on success; existing host row is replaced,
 *     otherwise appended.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED. Returns ERR_INLET_USAGE_SHAPE for incompatible host/design cross-section.
 */
SWMM_ENGINE_API int swmm_inlet_usage_set(SWMM_Engine engine, const SWMM_InletUsage* usage,
                                         int* usage_idx);

/**
 * @brief Remove an inlet-usage row and shift later row indices.
 *
 * Requires BUILDING/OPENED. Removing an inlet junction usage does not fuse/delete the host node or
 * clear its inlet-junction flag.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] usage_idx Zero-based existing usage-row index.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_inlet_usage_remove(SWMM_Engine engine, int usage_idx);

/* =========================================================================
 * LID controls
 * ========================================================================= */

/**
 * @brief Add a new LID (Low Impact Development) control.
 *
 * @details LID types: 0=BIO_CELL, 1=RAIN_GARDEN, 2=GREEN_ROOF,
 *          3=INFIL_TRENCH, 4=PERM_PAVEMENT, 5=RAIN_BARREL,
 *          6=ROOFTOP_DISCONN, 7=VEGETATIVE_SWALE, 8=NODE.
 *
 * @param[in] engine Engine handle; see this operation's lifecycle note.
 * @param[in] id Unique null-terminated identifier.
 * @param[in] type LID type code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. Requires BUILDING/OPENED; other states return
 *     SWMM_ERR_LIFECYCLE.
 */
SWMM_ENGINE_API int swmm_lid_add(SWMM_Engine engine, const char* id, int type);

/** Arbitrary ordered NODE control layers; thickness/suction in in or mm,
 * conductivity in in/hr or mm/hr. Params: SURFACE=(thickness, vegetation),
 * MEDIA=(thickness, porosity, FC, WP, Ksat, Kslope, suction),
 * with gravity drainage Ksat*exp(-Kslope*(porosity-theta)) above FC.
 * Kslope is dimensionless and nonnegative, not a power exponent.
 * Surface entry into the first MEDIA reuses modified Green-Ampt;
 * accepted backwater wetting reconciles its finite-zone history.
 * AGGREGATE=(thickness, porosity, Ksat), BOTTOM=(seepage, clogging).
 * Unused parameters must be zero. BOTTOM is not a numbered physical layer. */
enum SWMM_LidNodeLayerKind {
    SWMM_LID_NODE_SURFACE = 0,
    SWMM_LID_NODE_MEDIA = 1,
    SWMM_LID_NODE_AGGREGATE = 2,
    SWMM_LID_NODE_BOTTOM = 3
};
typedef struct SWMM_LidNodeLayer {
    int kind;
    double params[7];
} SWMM_LidNodeLayer;
/** Alternative flow model. model=0 preserves the existing layer formulation with backwater;
 * model=1 is conservative semi-discrete Richards with adaptive stiff BDF1.
 * atol is water-content fraction, rtol dimensionless, max_step seconds. */
typedef struct SWMM_LidRichardsOptions {
    int model, cells_per_layer;
    double atol, rtol, max_step;
} SWMM_LidRichardsOptions;
/** Explicit van Genuchten-Mualem material; alpha and specific storage are
 * inverse metres in BOTH unit systems. n>1, l>=0, specific_storage>0. */
typedef struct SWMM_LidRichardsMaterial {
    double theta_r, alpha, n, l, specific_storage;
} SWMM_LidRichardsMaterial;
/**
 * @brief Read a control's Richards options.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] control Zero-based existing LID control index.
 * @param[out] out Non-null caller-owned output. Defaults are returned for an empty stack; model is
 *     0 or 1.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_richards_options_get(SWMM_Engine engine, int control,
                                                  SWMM_LidRichardsOptions* out);
/**
 * @brief Read a control's Richards material.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] control Zero-based existing LID control index.
 * @param[in] row Zero-based row index in the selected profile/table.
 * @param[out] out Non-null caller-owned output. alpha/specific_storage are inverse metres in both
 *     project unit systems; nonporous rows may contain unused defaults.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_richards_material_get(SWMM_Engine engine, int control, int row,
                                                   SWMM_LidRichardsMaterial* out);
/** Per-layer treatment. Layer is one-based; pollutant is its engine index.
 * Removal percent applies to outgoing mass; decay is first-order per day.
 * Optional R=/C= expression applies after fixed removal at the layer exit. */
typedef struct SWMM_LidLayerTreatment {
    int layer;
    int pollutant;
    double removal_percent;
    double decay_per_day;
    const char* expression;
} SWMM_LidLayerTreatment;
/**
 * @brief Count per-layer treatment rows flattened top-to-bottom.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] control Zero-based existing LID control index.
 * @return Nonnegative row count; -1 for null engine or invalid control.
 */
SWMM_ENGINE_API int swmm_lid_node_treatment_count(SWMM_Engine engine, int control);
/**
 * @brief Read a flattened per-layer treatment row.
 *
 * Reads explicitly authored node_layers storage; a control lacking that storage can return
 * BADINDEX. removal_percent is 0..100; decay_per_day is per day.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] control Zero-based existing LID control index.
 * @param[in] row Zero-based flattened treatment index; negative/out-of-range rows return BADPARAM.
 * @param[out] out Non-null struct. expression borrows engine storage; do not free and copy before
 *     editing/destroying the control/engine. layer is one-based and pollutant is an engine
 *     pollutant index.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_node_treatment_get(SWMM_Engine engine, int control, int row,
                                                SWMM_LidLayerTreatment* out);
/**
 * @brief Atomically configure a NODE control stack and treatment.
 *
 * Requires BUILDING/OPENED and a NODE control. Validates stack, treatment expressions and outlet
 * anchors before retaining the edit; rejects edits that orphan an anchored layer. Synchronizes
 * dependent MaxDepths/outlet offsets. Preserves existing flow/retention settings, requiring
 * compatible layer count/kinds when those settings are active.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] control Zero-based existing LID control index.
 * @param[in] layers Non-null borrowed array of count layers, copied synchronously. See
 *     SWMM_LidNodeLayerKind for parameter positions/units; unused params must be zero.
 * @param[in] count Positive authored layer count, including optional final BOTTOM. SURFACE may
 *     occur only first; at least one MEDIA/AGGREGATE is required.
 * @param[in] treatment Borrowed treatment_count rows, required if treatment_count>0; expressions
 *     are copied.
 *     Layer is one-based, cannot target BOTTOM; each layer/pollutant key must be unique.
 * @param[in] treatment_count Nonnegative number of treatment rows; zero clears treatment.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_lid_node_configure(SWMM_Engine engine, int control,
                                            const SWMM_LidNodeLayer* layers, int count,
                                            const SWMM_LidLayerTreatment* treatment,
                                            int treatment_count);
/**
 * @brief Atomically configure a NODE control stack, treatment and flow formulation.
 *
 * Requires BUILDING/OPENED and a NODE control. Validates stack, treatment expressions and outlet
 * anchors before retaining the edit; rejects edits that orphan an anchored layer. Synchronizes
 * dependent MaxDepths/outlet offsets. Richards requires SURFACE first and valid retention on every
 * porous layer; invalid configuration leaves stack and dependent geometry unchanged.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] control Zero-based existing LID control index.
 * @param[in] layers Non-null borrowed array of count layers, copied synchronously. See
 *     SWMM_LidNodeLayerKind for parameter positions/units; unused params must be zero.
 * @param[in] count Positive authored layer count, including optional final BOTTOM. SURFACE may
 *     occur only first; at least one MEDIA/AGGREGATE is required.
 * @param[in] treatment Borrowed treatment_count rows, required if treatment_count>0; expressions
 *     are copied.
 *     Layer is one-based, cannot target BOTTOM; each layer/pollutant key must be unique.
 * @param[in] treatment_count Nonnegative number of treatment rows; zero clears treatment.
 * @param[in] options Non-null flow options: model=0 classical/backwater, model=1 Richards. Richards
 *     requires finite positive tolerances/max_step and cells_per_layer in [1,256].
 * @param[in] material Borrowed array with count entries; required for Richards. Porous rows use
 *     residual theta, inverse-metre alpha/specific-storage in BOTH unit systems, n>1 and l>=0;
 *     SURFACE/BOTTOM rows are ignored.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_lid_node_configure_flow(SWMM_Engine engine, int control,
                                                 const SWMM_LidNodeLayer* layers, int count,
                                                 const SWMM_LidLayerTreatment* treatment,
                                                 int treatment_count,
                                                 const SWMM_LidRichardsOptions* options,
                                                 const SWMM_LidRichardsMaterial* material);
/**
 * @brief Count authored or derived stack layers, including a BOTTOM row when present.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] control Zero-based existing LID control index.
 * @return Nonnegative row count; -1 for null engine or invalid control.
 */
SWMM_ENGINE_API int swmm_lid_node_layer_count(SWMM_Engine engine, int control);
/**
 * @brief Read one authored or derived NODE layer.
 *
 * No lifecycle check. Uses the derived stack for conventional controls as well as explicitly
 * authored NODE stacks.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] control Zero-based existing LID control index.
 * @param[in] row Zero-based row index in the selected profile/table.
 * @param[out] out Non-null caller-owned output receiving kind and all seven input-unit parameters.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_node_layer_get(SWMM_Engine engine, int control, int row,
                                            SWMM_LidNodeLayer* out);
/**
 * @brief Replace NODE layers while preserving compatible treatment and flow settings.
 *
 * Requires BUILDING/OPENED and NODE type. Validates stack and existing anchor bounds. Existing
 * Richards/retention configuration requires same layer count/kinds; treatment-bearing layers must
 * retain their kind. Refusal preserves the authored stack. Success synchronizes dependent node
 * MaxDepths/outlet offsets.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] control Zero-based existing LID control index.
 * @param[in] rows Non-null copied array of count layers; unused parameters must be zero.
 * @param[in] count Positive authored layer count, including optional final BOTTOM. SURFACE may
 *     occur only first; at least one MEDIA/AGGREGATE is required.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_lid_node_layers_set(SWMM_Engine engine, int control,
                                             const SWMM_LidNodeLayer* rows, int count);
/**
 * @brief Read a node LID-control assignment and initial saturation.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node Zero-based existing node index.
 * @param[out] control Non-null control-index output; -1 for nonstorage/plain storage.
 * @param[out] saturation Non-null initial saturation output in percent; zero for nonstorage.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_node_get_lid(SWMM_Engine engine, int node, int* control,
                                      double* saturation);
/**
 * @brief Assign or clear a storage-node LID and synchronize its geometry.
 *
 * Requires BUILDING/OPENED and storage node. Checks existing outlet anchors and refuses a second
 * LID endpoint. Clears node runtime LID state and cached full volume. Assignment preserves bottom
 * mode; clearing resets CURRENT and removes attached outlet anchors.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node Zero-based existing node index.
 * @param[in] control Existing control with a valid derived/authored stack, or -1 to clear the
 *     assignment.
 * @param[in] saturation Finite initial saturation in percent [0,100]; ignored/reset to zero when
 *     clearing.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_node_set_lid(SWMM_Engine engine, int node, int control, double saturation);
/** Per-node bottom owner, independent of a shared control. Old assignments
 * use CURRENT. INP grammar: Node Control InitSat [BOTTOM CURRENT|SEALED|MESH_AQUIFER].
 * Nondefault modes are written explicitly. MESH_AQUIFER can be authored but
 * execution remains gated on the R5 signed-volume/restart qualification. */
enum SWMM_LidNodeBottom {
    SWMM_LID_BOTTOM_CURRENT = 0,
    SWMM_LID_BOTTOM_SEALED = 1,
    SWMM_LID_BOTTOM_MESH_AQUIFER = 2
};
/**
 * @brief Read a node's bottom exchange ownership mode.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node Zero-based existing node index.
 * @param[out] mode Non-null SWMM_LID_BOTTOM_* destination; CURRENT for nodes without a LID
 *     assignment.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_node_get_lid_bottom(SWMM_Engine engine, int node, int* mode);
/**
 * @brief Select bottom ownership for one assigned storage LID.
 *
 * Requires BUILDING/OPENED and an assigned storage LID. Authoring MESH_AQUIFER does not by itself
 * establish a qualified executable mesh connection.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node Zero-based existing node index.
 * @param[in] mode SWMM_LID_BOTTOM_CURRENT, SEALED or MESH_AQUIFER.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_node_set_lid_bottom(SWMM_Engine engine, int node, int mode);
/**
 * @brief Read the authored geometric area at a storage LID base.
 *
 * Requires BUILDING/OPENED, assigned storage LID, and no initialized runtime cells. Uses geometry
 * at depth zero without pore/retained-water factors; rejects nonfinite/negative computed area. Zero
 * requires explicit positive area when creating a connection.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node Zero-based existing node index.
 * @param[out] area Non-null contact-area destination in project square-length units, ft2 or m2; may
 *     be zero.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_lid_node_base_area(SWMM_Engine engine, int node, double* area);
/** Accepted mesh-bottom diagnostics, always SI. Positive net flow is from
 * LID to aquifer. The sample is the last completed exchange interval; the
 * current aquifer state has its own time. valid_sample=0 before first sampling.
 * Classical flow uses mobile HGL for sampled_lid_head; Richards uses the
 * bottom porous total head. Gross pending transfers cannot cancel each other.
 * Returns BADPARAM without an initialized explicit mesh-bottom connection;
 * failure leaves *out unchanged. This function does not advance either clock. */
typedef struct SWMM_LidBottomState {
    int flow_model; /* 0 existing/backwater, 1 Richards */
    int cell, valid_sample;
    double contact_area_m2, surface_head_m, aquifer_head_m, aquifer_time_s;
    double down_cumulative_m3, up_cumulative_m3, net_cumulative_m3;
    double pending_down_m3, pending_up_m3, groundwater_residual_m3;
    double sample_start_s, sample_end_s, sampled_aquifer_time_s;
    double sampled_lid_head_m, sampled_aquifer_head_m;
    double requested_down_m3, requested_up_m3, reserved_down_m3, reserved_up_m3;
    double accepted_down_m3, accepted_up_m3, net_rate_m3s;
} SWMM_LidBottomState;
/**
 * @brief Read the last accepted LID-to-mesh exchange diagnostics without advancing time.
 *
 * Requires an initialized explicit MESH_AQUIFER bottom connection with receipts enabled and a
 * 2D-enabled build; otherwise BADPARAM. Current aquifer and sample timestamps can differ. Classical
 * samples use mobile LID head; Richards uses bottom porous total head. Pending gross up/down
 * transfers remain separate.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node Zero-based existing node index.
 * @param[out] out Non-null caller-owned SI diagnostics struct; preserved on failure. Positive net
 *     volume/rate is LID to aquifer; valid_sample=0 before the first completed sample.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_node_bottom_state_get(SWMM_Engine engine, int node,
                                                   SWMM_LidBottomState* out);

/**
 * @brief Read a link's LID layer anchor.
 *
 * No lifecycle check.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] link Zero-based existing link index.
 * @param[out] layer Non-null output; zero when no anchor exists.
 * @param[out] top Non-null output: 0=BOTTOM, 1=TOP; zero when absent.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_node_outlet_get(SWMM_Engine engine, int link, int* layer, int* top);
/**
 * @brief Set or remove a link's LID layer anchor.
 *
 * Requires BUILDING/OPENED. Nonzero layer requires exactly one LID endpoint and a valid layer
 * index. Replaces any previous anchor, then synchronizes offsets; removal needs no LID endpoint.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] link Zero-based existing link index.
 * @param[in] layer Zero removes the anchor; otherwise one-based physical layer, top-to-bottom
 *     excluding BOTTOM.
 * @param[in] top 0=BOTTOM or 1=TOP, required even when removing.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_lid_node_outlet_set(SWMM_Engine engine, int link, int layer, int top);
/**
 * @brief Count initialized runtime profile cells.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node Zero-based existing node index.
 * @return Nonnegative row count; -1 for null engine or invalid selected node/control. Runtime state
 *     count is zero for nonstorage/plain storage and before initialization.
 */
SWMM_ENGINE_API int swmm_lid_node_state_count(SWMM_Engine engine, int node);
/**
 * @brief Read one initialized LID profile cell.
 *
 * Runtime rows are top-to-bottom; porous layers may produce several cells. No lifecycle check, but
 * node must be storage with an existing runtime row.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node Zero-based existing node index.
 * @param[in] row Zero-based row index in the selected profile/table.
 * @param[out] layer Non-null one-based physical layer identifier.
 * @param[out] bottom Non-null cell-bottom depth above node invert, ft/m.
 * @param[out] top Non-null cell-top depth above node invert, ft/m.
 * @param[out] moisture Non-null volumetric water fraction; Richards porous rows use theta,
 *     classical rows include mobile-head inundation.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_node_state_get(SWMM_Engine engine, int node, int row, int* layer,
                                            double* bottom, double* top, double* moisture);
/**
 * @brief Read one Richards cell's pressure, total head and complete water volume.
 *
 * Requires an initialized Richards storage profile; classical formulation returns BADPARAM. No
 * state is advanced.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node Zero-based existing node index.
 * @param[in] row Zero-based row index in the selected profile/table.
 * @param[out] pressure Non-null pressure head in project length units, ft/m.
 * @param[out] head Non-null total hydraulic head in ft/m, including node invert and cell elevation.
 * @param[out] water Non-null complete cell water in project volume units, ft3/m3; surface row uses
 *     node volume.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_richards_state_get(SWMM_Engine engine, int node, int row,
                                                double* pressure, double* head, double* water);
typedef struct SWMM_LidRichardsStatistics {
    int accepted, rejected, rhs, newton;
    double min_step, balance_m3;
} SWMM_LidRichardsStatistics;
/**
 * @brief Read the last stored Richards adaptive-solver report.
 *
 * Returns BADPARAM unless the node uses Richards; does not advance/reset statistics.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node Zero-based existing node index.
 * @param[out] out Non-null caller-owned statistics: accepted/rejected steps, RHS evaluations,
 *     Newton iterations, min_step in seconds and balance_m3 in SI cubic metres.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_richards_statistics_get(SWMM_Engine engine, int node,
                                                     SWMM_LidRichardsStatistics* out);

/**
 * @brief Rename an LID control in place.
 *
 * @details Updates both the name vector (read by swmm_lid_id() and the
 *          [LID_CONTROLS] writer) and the name registry (read by
 *          swmm_lid_index() and the [LID_USAGE] writer). Usage rows hold an
 *          LID INDEX, so subcatchment deployments follow automatically.
 *          Lifecycle: BUILDING or OPENED.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx LID control index.
 * @param[in] new_id New unique name (non-empty). A pure case-respelling of this
 *                same control is allowed.
 * @returns SWMM_OK, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED, or
 *          SWMM_ERR_BADPARAM on an empty or duplicate name.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Requires BUILDING/OPENED; other states return SWMM_ERR_LIFECYCLE.
 */
SWMM_ENGINE_API int swmm_lid_rename(SWMM_Engine engine, int idx, const char* new_id);

/**
 * @brief Set the surface layer properties for a LID control.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based LID index.
 * @param[in] storage Surface storage depth.
 * @param[in] roughness Surface Manning's n.
 * @param[in] slope Surface slope in percent.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Requires BUILDING/OPENED; other states return SWMM_ERR_LIFECYCLE.
 *     Rejects NODE controls; use the NODE stack configuration API. Supply finite values because
 *     scalar comparison guards are not independent finiteness checks. These are pre-initialization
 *     authoring parameters. Layer depths use inches/mm, conductivities inches/hour or mm/hour;
 *     surface slope is percent.
 */
SWMM_ENGINE_API int swmm_lid_set_surface(SWMM_Engine engine, int idx, double storage,
                                         double roughness, double slope);

/**
 * @brief Set the soil layer properties for a LID control.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based LID index.
 * @param[in] thick Soil thickness.
 * @param[in] porosity Soil porosity (fraction).
 * @param[in] fc Field capacity (fraction).
 * @param[in] wp Wilting point (fraction).
 * @param[in] ksat Saturated hydraulic conductivity.
 * @param[in] kslope Slope of the conductivity–moisture curve.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Requires BUILDING/OPENED; other states return SWMM_ERR_LIFECYCLE.
 *     Rejects NODE controls; use the NODE stack configuration API. Supply finite values because
 *     scalar comparison guards are not independent finiteness checks. These are pre-initialization
 *     authoring parameters. Layer depths use inches/mm, conductivities inches/hour or mm/hour;
 *     surface slope is percent.
 */
SWMM_ENGINE_API int swmm_lid_set_soil(SWMM_Engine engine, int idx, double thick, double porosity,
                                      double fc, double wp, double ksat, double kslope);

/**
 * @brief Set the storage layer properties for a LID control.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based LID index.
 * @param[in] thick Storage layer thickness.
 * @param[in] void_frac Void ratio (void volume / solid volume); porosity is e/(1+e).
 * @param[in] ksat Seepage rate through the storage layer bottom.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Requires BUILDING/OPENED; other states return SWMM_ERR_LIFECYCLE.
 *     Rejects NODE controls; use the NODE stack configuration API. Supply finite values because
 *     scalar comparison guards are not independent finiteness checks. These are pre-initialization
 *     authoring parameters. Layer depths use inches/mm, conductivities inches/hour or mm/hour;
 *     surface slope is percent.
 */
SWMM_ENGINE_API int swmm_lid_set_storage(SWMM_Engine engine, int idx, double thick,
                                         double void_frac, double ksat);

/**
 * @brief Set the underdrain properties for a LID control.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based LID index.
 * @param[in] coeff Drain coefficient.
 * @param[in] expon Drain exponent.
 * @param[in] offset Drain offset height above the storage layer bottom.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches. Rejects NODE controls; use the NODE
 *     stack configuration API. Supply finite values because scalar comparison guards are not
 *     independent finiteness checks. No lifecycle guard; refreshes cached unit drain coefficients
 *     for subsequent steps. offset uses inches/mm, expon is dimensionless and coeff follows the
 *     input depth/hour power law; all three must be nonnegative.
 */
SWMM_ENGINE_API int swmm_lid_set_drain(SWMM_Engine engine, int idx, double coeff, double expon,
                                       double offset);

/**
 * @brief Read a constituent underdrain-removal percentage.
 *
 * No lifecycle guard on get.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[in] constituent 0..np-1 selects pollutant; np+m selects reaction species m, included only
 *     when reactions are configured. Excludes reserved registry species.
 * @param[out] percent Non-null required output; zero if no row exists.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_get_removal(SWMM_Engine engine, int idx, int constituent,
                                         double* percent);
/**
 * @brief Assign a constituent underdrain-removal percentage.
 *
 * Requires BUILDING/OPENED and rejects NODE controls. Updates pollutant rows by index or
 * reaction-species rows by name and the aligned runtime removal array when present.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[in] constituent 0..np-1 selects pollutant; np+m selects reaction species m, included only
 *     when reactions are configured. Excludes reserved registry species.
 * @param[in] percent Percentage in [0,100]; zero removes the stored entry.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments. SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_lid_set_removal(SWMM_Engine engine, int idx, int constituent,
                                         double percent);

/**
 * @brief Set the porous-pavement layer properties for a LID control.
 *
 * @details Used by PERM_PAVEMENT (and any LID with a `PAVEMENT` line). The six
 *          values of the `[LID_CONTROLS] ... PAVEMENT` line, in input-file
 *          units. Pre-start-only (BUILDING/OPENED) — the layer seeds per-unit
 *          LID state at start().
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based LID index.
 * @param[in] thick Pavement layer thickness.
 * @param[in] void_ratio Void ratio (volume of voids / volume of solids).
 * @param[in] frac_imperv Impervious surface fraction (0–1).
 * @param[in] ksat Permeability / saturated conductivity.
 * @param[in] clog_factor Clogging factor (0 = no clogging).
 * @param[in] regen_days Days between clogging regeneration (0 = never).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Requires BUILDING/OPENED; other states return SWMM_ERR_LIFECYCLE.
 *     Rejects NODE controls; use the NODE stack configuration API. Supply finite values because
 *     scalar comparison guards are not independent finiteness checks. These are pre-initialization
 *     authoring parameters. Layer depths use inches/mm, conductivities inches/hour or mm/hour;
 *     surface slope is percent.
 */
SWMM_ENGINE_API int swmm_lid_set_pavement(SWMM_Engine engine, int idx, double thick,
                                          double void_ratio, double frac_imperv, double ksat,
                                          double clog_factor, double regen_days);

/**
 * @brief Set the drainage-mat layer properties for a LID control.
 *
 * @details Used by GREEN_ROOF (and any LID with a `DRAINMAT` line). The three
 *          values of the `[LID_CONTROLS] ... DRAINMAT` line, in input-file
 *          units. Pre-start-only (BUILDING/OPENED).
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based LID index.
 * @param[in] thick Drainage-mat thickness.
 * @param[in] void_frac Void fraction of the mat (0–1).
 * @param[in] roughness Manning's n for flow through the mat.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. Requires BUILDING/OPENED; other states return SWMM_ERR_LIFECYCLE.
 *     Rejects NODE controls; use the NODE stack configuration API. Supply finite values because
 *     scalar comparison guards are not independent finiteness checks. These are pre-initialization
 *     authoring parameters. Layer depths use inches/mm, conductivities inches/hour or mm/hour;
 *     surface slope is percent.
 */
SWMM_ENGINE_API int swmm_lid_set_drainmat(SWMM_Engine engine, int idx, double thick,
                                          double void_frac, double roughness);

/**
 * @brief Get the total number of LID controls in the model.
 * @param[in] engine Engine handle.
 * @returns Number of LID controls, or -1 on error.
 *
 *
 * @note Null engine returns -1.
 */
SWMM_ENGINE_API int swmm_lid_count(SWMM_Engine engine);

/**
 * @brief Look up a LID control's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated LID identifier.
 * @returns Zero-based index, or -1 if not found.
 *
 *
 * @note Name lookup is case-insensitive; null engine/name or no match returns -1.
 */
SWMM_ENGINE_API int swmm_lid_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a LID control by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based LID index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note Returned text borrows engine storage: do not free; copy before editing the table or
 *     closing/destroying the engine. NULL is returned for a null engine or invalid index.
 */
SWMM_ENGINE_API const char* swmm_lid_id(SWMM_Engine engine, int idx);

/**
 * @brief Read a LID control's type code. Inverse of the `type` argument to
 *        @ref swmm_lid_add.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based LID index.
 * @param[out] type Receives the LID type code (0=BIO_CELL … 7=VEGETATIVE_SWALE, 8=NODE; -1 for
 *     unknown stored code).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note type may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed infrastructure/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_lid_get_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Read the stored surface layer parameters.
 *
 * All outputs may be NULL; returns stored authoring values without runtime conversion or a
 * lifecycle guard.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[out] storage Optional output. Surface storage depth in inches/mm.
 * @param[out] roughness Optional output. Surface Manning roughness.
 * @param[out] slope Optional output. Surface slope in percent, not fraction.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_get_surface(SWMM_Engine engine, int idx, double* storage,
                                         double* roughness, double* slope);

/**
 * @brief Read the stored soil layer parameters.
 *
 * All outputs may be NULL; returns stored authoring values without runtime conversion or a
 * lifecycle guard.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[out] thick Optional output. Soil thickness in inches/mm.
 * @param[out] porosity Optional output. Volumetric porosity fraction.
 * @param[out] fc Optional output. Field-capacity moisture fraction.
 * @param[out] wp Optional output. Wilting-point moisture fraction.
 * @param[out] ksat Optional output. Saturated conductivity in inches/hour or mm/hour.
 * @param[out] kslope Optional output. Dimensionless exponential conductivity slope.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_get_soil(SWMM_Engine engine, int idx, double* thick, double* porosity,
                                      double* fc, double* wp, double* ksat, double* kslope);

/**
 * @brief Read the stored storage layer parameters.
 *
 * All outputs may be NULL; returns stored authoring values without runtime conversion or a
 * lifecycle guard.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[out] thick Optional output. Storage thickness in inches/mm.
 * @param[out] void_frac Optional output. Void ratio e (void/solid volume), converted to porosity
 *     e/(1+e) at initialization.
 * @param[out] ksat Optional output. Bottom seepage in inches/hour or mm/hour.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_get_storage(SWMM_Engine engine, int idx, double* thick,
                                         double* void_frac, double* ksat);

/**
 * @brief Read the stored drain layer parameters.
 *
 * All outputs may be NULL; returns stored authoring values without runtime conversion or a
 * lifecycle guard.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[out] coeff Optional output. User coefficient in the input depth/hour drain power law.
 * @param[out] expon Optional output. Dimensionless drain power-law exponent.
 * @param[out] offset Optional output. Drain height above storage bottom, inches/mm.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_get_drain(SWMM_Engine engine, int idx, double* coeff, double* expon,
                                       double* offset);

/**
 * @brief Read the stored pavement layer parameters.
 *
 * All outputs may be NULL; returns stored authoring values without runtime conversion or a
 * lifecycle guard.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[out] thick Optional output. Pavement thickness, inches/mm.
 * @param[out] void_ratio Optional output. Void/solid volume ratio.
 * @param[out] frac_imperv Optional output. Impervious surface fraction.
 * @param[out] ksat Optional output. Permeability, inches/hour or mm/hour.
 * @param[out] clog_factor Optional output. Dimensionless clogging factor; zero disables clogging.
 * @param[out] regen_days Optional output. Regeneration interval, days; zero disables it.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_get_pavement(SWMM_Engine engine, int idx, double* thick,
                                          double* void_ratio, double* frac_imperv, double* ksat,
                                          double* clog_factor, double* regen_days);

/**
 * @brief Read the stored drainmat layer parameters.
 *
 * All outputs may be NULL; returns stored authoring values without runtime conversion or a
 * lifecycle guard.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing object index in this function's table.
 * @param[out] thick Optional output. Drainage mat thickness, inches/mm.
 * @param[out] void_frac Optional output. Stored drainage-mat void parameter, as passed to its
 *     setter.
 * @param[out] roughness Optional output. Manning roughness.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_get_drainmat(SWMM_Engine engine, int idx, double* thick,
                                          double* void_frac, double* roughness);

/* =========================================================================
 * LID usage (assign LID to subcatchment)
 * ========================================================================= */

/**
 * @brief Assign a LID control to a subcatchment.
 *
 * @details Multiple LID units of the same or different types can be assigned
 *          to a single subcatchment.
 *
 * @param[in] engine Engine handle.
 * @param[in] subcatch_idx Zero-based subcatchment index.
 * @param[in] lid_idx Zero-based LID control index.
 * @param[in] number Number of replicate LID units.
 * @param[in] area Area of each LID unit in square feet/metres (not acres/hectares).
 * @param[in] width Top width of the overland flow surface per unit.
 * @param[in] init_sat Initial saturation of the soil layer in percent (0–100).
 * @param[in] from_imperv Percentage of impervious runoff treated by this LID (0–100).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed infrastructure/pollutant index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard in this wrapper; authoring edits do not
 *     generally rebuild running hydraulic/geometry caches. Stores an appended configuration row
 *     without numeric validation or runtime solver rebuild. Area is square feet/metres per unit
 *     (not acres/hectares); width is ft/m, init_sat and from_imperv are PERCENT, not fractions.
 *     NODE controls are refused.
 */
SWMM_ENGINE_API int swmm_lid_usage_add(SWMM_Engine engine, int subcatch_idx, int lid_idx,
                                       int number, double area, double width, double init_sat,
                                       double from_imperv);

/**
 * @brief Count LID-usage rows across all subcatchments.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @return Nonnegative row count, or -1 for null engine.
 */
SWMM_ENGINE_API int swmm_lid_usage_count(SWMM_Engine engine);

/**
 * @brief Read one LID usage row by global index.
 * @param[in] engine Engine handle.
 * @param[in] usage_idx Zero-based global usage index ([0, count)).
 * @param[out] subcatch_idx Owning subcatchment index (nullable).
 * @param[out] lid_idx LID control index (nullable).
 * @param[out] number Number of replicate units (nullable).
 * @param[out] area Area per unit (nullable).
 * @param[out] width Overland flow top width (nullable).
 * @param[out] init_sat Initial saturation in percent (nullable).
 * @param[out] from_imperv Percentage of impervious runoff treated (nullable).
 * @param[out] to_perv 1 = route outflow to pervious area (nullable).
 * @param[out] from_perv Percentage of pervious runoff treated (nullable).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note subcatch_idx may be NULL to suppress that output. lid_idx may be NULL to suppress that
 *     output. number may be NULL to suppress that output. area may be NULL to suppress that output.
 *     width may be NULL to suppress that output. init_sat may be NULL to suppress that output.
 *     from_imperv may be NULL to suppress that output. to_perv may be NULL to suppress that output.
 *     from_perv may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed infrastructure/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_lid_usage_get(SWMM_Engine engine, int usage_idx, int* subcatch_idx,
                                       int* lid_idx, int* number, double* area, double* width,
                                       double* init_sat, double* from_imperv, int* to_perv,
                                       double* from_perv);

/**
 * @brief Erase a subcatchment LID-usage row and shift later indices.
 *
 * No lifecycle guard. Erases configuration arrays and resizes balance arrays; does not rebuild
 * running LID solver groups.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] usage_idx Zero-based global LID-usage index.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for a failed index check,
 *     or SWMM_ERR_BADPARAM for invalid required arguments.
 */
SWMM_ENGINE_API int swmm_lid_usage_remove(SWMM_Engine engine, int usage_idx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_INFRASTRUCTURE_H */
