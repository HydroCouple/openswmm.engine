// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_SNOW2D_H
#define OPENSWMM_SNOW2D_H
#include "openswmm_snow.h"
#ifdef __cplusplus
extern "C" {
#endif
// Shared named packs; all overrides are SI, slope/aspect in degrees.
// Priority: whole mesh (*), tag, cell. NONE explicitly disables a cell/tag.
// Cell keys in section text are 1-based, as in the other 2D input sections.
SWMM_ENGINE_API int swmm_snow2d_get_options(SWMM_Engine,int* mode,double* step_s); // 0 AUTO, 1 YES, 2 NO; 0 step inherits
SWMM_ENGINE_API int swmm_snow2d_defaults_count(SWMM_Engine);
SWMM_ENGINE_API int swmm_snow2d_cells_count(SWMM_Engine);
// Section 0 options, 1 defaults, 2 cells. Required capacity includes the
// terminal NUL. Query with buffer=NULL and capacity=0 to obtain that size.
SWMM_ENGINE_API int swmm_snow2d_get_configuration(SWMM_Engine,int section,char* buffer,int capacity,int* required);
typedef struct SWMM_MeshSnowOverrides {
    // NaN means inherited. Slope/aspect are either both set or both inherited.
    double forest,drift,ground_heat_wm2,extinction_depth_m,slope_deg,aspect_deg;
} SWMM_MeshSnowOverrides;
// Section 1 defaults (cell=-1, tag=* or region); section 2 cells (zero-based
// cell, empty tag). Reads do not resolve/invent inherited parameter values.
SWMM_ENGINE_API int swmm_snow2d_get_row(SWMM_Engine,int section,int row,int* cell,
    char* tag,int tag_capacity,char* pack,int pack_capacity,SWMM_MeshSnowOverrides*);
typedef struct SWMM_MeshSnowLedger {
    double completed_s,initial_m3,raw_precipitation_m3,precipitation_m3,liquid_m3;
    double sublimation_m3,stored_m3,residual_m3,net_energy_kj, initial_energy_kj, stored_energy_kj, energy_residual_kj;
} SWMM_MeshSnowLedger;
// Bulk snapshots in mesh cell order. Unassigned cells have model DEFAULT
// and NaN quantities, distinguishing unavailable results from a bare pack.
SWMM_ENGINE_API int swmm_snow2d_get_states(SWMM_Engine,int first,int count,SWMM_SnowState*);
SWMM_ENGINE_API int swmm_snow2d_get_ledgers(SWMM_Engine,int first,int count,SWMM_MeshSnowLedger*);
// Atomic replacement through the same strict parsers as the INP reader.
// Empty section bodies clear that section. No section headers are supplied.
SWMM_ENGINE_API int swmm_snow2d_apply_configuration(SWMM_Engine,const char* options,
    const char* defaults,const char* cells,char* diagnostic,int capacity);
#ifdef __cplusplus
}
#endif
#endif
