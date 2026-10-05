/* SPDX-License-Identifier: Apache-2.0 */
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
/* Callback returns zero to cancel. It may not edit or destroy the engine. */
typedef int (*SWMM_SurfaceOwnerProgress)(int done, int total, void* user);
/* Read authored SUBCATCH UNIFORM records. Query with rows=NULL, capacity=0. */
SWMM_ENGINE_API int swmm_surface_owner_get(SWMM_Engine, int* rows, int capacity, int* count);
/* count=-1 previews stored records; otherwise rows is the proposed complete
 * set. Query sizes with NULL output arrays. Outputs include unreviewed peers.
 * valid=0 reports blocking diagnostics; the API itself still returns OK.
 * token identifies the current inputs (including current authored records),
 * independently of the proposal. It needs at least 17 bytes.
 * Both preview and replacement require OPENED/BUILDING. */
SWMM_ENGINE_API int swmm_surface_owner_preview(SWMM_Engine, const int* rows, int count,
    SWMM_SurfaceOwnerObject* objects, int object_capacity, int* object_count,
    SWMM_SurfaceOwnerShare* shares, int share_capacity, int* share_count,
    double* mesh_weather_area, int cell_capacity, int* cell_count,
    int* valid, char* token, int token_capacity, char* diagnostics, int diagnostic_capacity,
    SWMM_SurfaceOwnerProgress progress, void* user);
/* Atomic, revision-checked replacement. Empty set restores legacy absence.
 * Invalid proposals and stale tokens leave the model unchanged. */
SWMM_ENGINE_API int swmm_surface_owner_replace(SWMM_Engine, const int* rows, int count,
    const char* expected_token, char* diagnostics, int diagnostic_capacity);
#ifdef __cplusplus
}
#endif
#endif
