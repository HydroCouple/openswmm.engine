/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file      openswmm_surface_ownership.h
 * @brief     Public C API for surface ownership assignments and validation.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_SURFACE_OWNERSHIP_H
#define OPENSWMM_SURFACE_OWNERSHIP_H
#include "openswmm_engine.h"
#ifdef __cplusplus
extern "C" {
#endif
/* R4 authoring only. Areas are m2; cells/subcatchments are engine indices.
 * These reviewed records do not activate recharge. Initialization refuses
 * them until the completed-interval source adapter is qualified.
 * No records preserves the existing hydrology and weather clocks. */
typedef struct SWMM_SurfaceOwnerObject {
    int subcatch, reviewed, lumped, status; /* outside/unreviewed/reviewed/invalid = 0/1/2/3 */
    double declared_area, polygon_area, lid_area, pervious_area, impervious_area;
    double native_lid_area, inside_area, outside_area;
    char name[256], tag[256], reason[512];
} SWMM_SurfaceOwnerObject;
typedef struct SWMM_SurfaceOwnerShare {
    int subcatch, cell;
    double weather_area, pervious_area, impervious_area, lid_area, native_lid_area;
} SWMM_SurfaceOwnerShare;
/**
 * @brief Report surface-ownership preview work on the calling thread.
 *
 * Must not throw or edit, close or destroy the engine. Callback/context pointers are borrowed only
 * for the synchronous preview call.
 *
 * @param[in] done Number of work items completed so far.
 * @param[in] total Total work items for this preview stage.
 * @param[in] user Borrowed context passed through from swmm_surface_owner_preview(); may be NULL.
 * @return Nonzero to continue; zero to cancel.
 */
typedef int (*SWMM_SurfaceOwnerProgress)(int done, int total, void* user);
/* Read authored SUBCATCH UNIFORM records. Query with rows=NULL, capacity=0. */
/**
 * @brief Read the currently authored surface-owner subcatchment indices.
 *
 * A size query does not allocate caller storage. A short buffer returns the required count without
 * copying rows.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; exclusive access is required.
 * @param[out] rows Caller-owned array of capacity indices, or NULL with capacity=0 to query the
 *     count.
 * @param[in] capacity Nonnegative array capacity in records, not bytes.
 * @param[out] count Non-null required-row output; set before short-buffer refusal. Each returned
 *     row is the current subcatchment index or -1 if an authored name no longer resolves.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside editable states,
 *     or SWMM_ERR_BADPARAM for invalid count/capacity/buffer or insufficient capacity.
 */
SWMM_ENGINE_API int swmm_surface_owner_get(SWMM_Engine engine, int* rows, int capacity, int* count);
/* count=-1 previews stored records; otherwise rows is the proposed complete
 * set. Query sizes with NULL output arrays. Outputs include unreviewed peers.
 * valid=0 reports blocking diagnostics; the API itself still returns OK.
 * token identifies the current inputs (including current authored records),
 * independently of the proposal. It needs at least 17 bytes.
 * Both preview and replacement require OPENED/BUILDING. */
/**
 * @brief Resolve proposed surface ownership, overlap diagnostics and a revision token.
 *
 * Does not modify authored ownership. Counts, valid, token and diagnostics are published before
 * cancellation/capacity refusal; object/share/area arrays are copied only after all capacity
 * checks. Areas are m2; indices address engine cells/subcatchments.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; exclusive access is required.
 * @param[in] rows Borrowed array of count zero-based subcatchment indices forming the complete
 *     proposed SUBCATCH UNIFORM set; NULL is allowed when count=0 or -1.
 * @param[in] count -1 previews stored records and ignores rows; otherwise nonnegative proposal
 *     size.
 * @param[out] objects Optional caller-owned object array including unreviewed peers; NULL requests
 *     only its required count.
 * @param[in] object_capacity Nonnegative object-array capacity in records; checked only when
 *     objects is non-null.
 * @param[out] object_count Non-null required object count, updated after preview even on
 *     cancellation/short output.
 * @param[out] shares Optional caller-owned cell-share array; NULL queries its required count.
 * @param[in] share_capacity Nonnegative share capacity in records; checked only when shares is
 *     non-null.
 * @param[out] share_count Non-null required share count, updated after preview.
 * @param[out] mesh_weather_area Optional array of per-cell weather areas in m2; NULL queries its
 *     required count.
 * @param[in] cell_capacity Nonnegative mesh_weather_area capacity in cells.
 * @param[out] cell_count Non-null required per-cell area count, updated after preview.
 * @param[out] valid Non-null result: 1 only for a valid, uncancelled preview; 0 indicates blocking
 *     diagnostics or cancellation. Invalid ownership can still return SWMM_OK.
 * @param[out] token Non-null caller-owned revision-token buffer. Identifies current model inputs
 *     INCLUDING current authored records, independently of the proposal; NUL-terminated.
 * @param[in] token_capacity Token capacity in bytes; must be at least 17.
 * @param[out] diagnostics Optional caller-owned newline-separated diagnostic buffer, NUL-terminated
 *     and truncated when capacity is positive.
 * @param[in] diagnostic_capacity Diagnostic capacity including NUL; nonpositive suppresses text
 *     output.
 * @param[in] progress Optional synchronous callback; nonzero continues, zero cancels. Must not
 *     edit/destroy the engine.
 * @param[in] user Borrowed opaque progress context; may be NULL.
 * @return SWMM_OK for a completed preview even if valid=0; SWMM_ERR_BADHANDLE for null engine,
 *     SWMM_ERR_LIFECYCLE outside editable states, or SWMM_ERR_BADPARAM for invalid input,
 *     cancellation or short non-null arrays.
 */
SWMM_ENGINE_API int
swmm_surface_owner_preview(SWMM_Engine engine, const int* rows, int count,
                           SWMM_SurfaceOwnerObject* objects, int object_capacity, int* object_count,
                           SWMM_SurfaceOwnerShare* shares, int share_capacity, int* share_count,
                           double* mesh_weather_area, int cell_capacity, int* cell_count,
                           int* valid, char* token, int token_capacity, char* diagnostics,
                           int diagnostic_capacity, SWMM_SurfaceOwnerProgress progress, void* user);
/* Atomic, revision-checked replacement. Empty set restores legacy absence.
 * Invalid proposals and stale tokens leave the model unchanged. */
/**
 * @brief Atomically replace authored surface-owner rows after revision checking.
 *
 * Recomputes the proposal and compares its current-input token before publishing. Nonempty invalid
 * proposals and stale tokens preserve authored records. Empty proposals are permitted after
 * revision checking. These are authoring records; publication does not itself activate recharge or
 * initialize a source adapter.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED; exclusive access is required.
 * @param[in] rows Borrowed array of count zero-based subcatchment indices forming the complete
 *     proposed SUBCATCH UNIFORM set; NULL is allowed when count=0 or -1.
 * @param[in] count Nonnegative complete replacement count; zero removes all authored records and
 *     restores legacy absence.
 * @param[in] expected_token Non-null NUL-terminated token from a preview of the current inputs;
 *     stale tokens are refused.
 * @param[out] diagnostics Optional caller-owned refusal text buffer; emptied on success,
 *     NUL-terminated/truncated when capacity is positive.
 * @param[in] diagnostic_capacity Diagnostic buffer capacity including NUL; nonpositive suppresses
 *     output.
 * @return SWMM_OK on replacement; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside
 *     editable states, or SWMM_ERR_BADPARAM for invalid proposal, indices or stale token.
 */
SWMM_ENGINE_API int swmm_surface_owner_replace(SWMM_Engine engine, const int* rows, int count,
                                               const char* expected_token, char* diagnostics,
                                               int diagnostic_capacity);
#ifdef __cplusplus
}
#endif
#endif
