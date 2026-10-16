// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_SNOW_H
#define OPENSWMM_SNOW_H
#include "openswmm_engine.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { SWMM_SNOW_DEFAULT = -1, SWMM_SNOW_DEGREE_DAY = 0, SWMM_SNOW_UEB = 1 };
// All new UEB/radiation values are SI in both US and SI projects. The existing
// three surface rows keep their existing project units. Setters require an
// editable model and never change it after a validation error.
typedef struct SWMM_UebSnowpack {
    int model; // DEFAULT uses the global model and removes the UEB row
    double forest_fraction, drift_factor, ground_heat_wm2;
    double extinction_depth_m, initial_energy_kjm2;
} SWMM_UebSnowpack;
SWMM_ENGINE_API int swmm_snow_get_model(SWMM_Engine, int* model);
SWMM_ENGINE_API int swmm_snow_set_model(SWMM_Engine, int model);
SWMM_ENGINE_API int swmm_snow_get_step(SWMM_Engine, double* seconds);
SWMM_ENGINE_API int swmm_snow_set_step(SWMM_Engine, double seconds);
SWMM_ENGINE_API int swmm_ueb_option_get(SWMM_Engine, const char* key, char* value, int capacity);
SWMM_ENGINE_API int swmm_ueb_option_set(SWMM_Engine, const char* key, const char* value);
// Each monthly mean diurnal range is °C; <=0 means derive from the temperature source.
SWMM_ENGINE_API int swmm_ueb_get_monthly_ranges(SWMM_Engine, double values[12]);
SWMM_ENGINE_API int swmm_ueb_set_monthly_ranges(SWMM_Engine, const double values[12]);
SWMM_ENGINE_API int swmm_snowpack_get_ueb(SWMM_Engine, int pack, SWMM_UebSnowpack*);
SWMM_ENGINE_API int swmm_snowpack_set_ueb(SWMM_Engine, int pack, const SWMM_UebSnowpack*);

typedef struct SWMM_SnowpackEdit {
    const char* name;
    double surface[3][7]; // existing PLOWABLE, IMPERVIOUS, PERVIOUS row units/order
    double removal[6];
    const char* removal_subcatch;
    SWMM_UebSnowpack ueb;
} SWMM_SnowpackEdit;
// Validate and upsert all named packs as one edit. Existing assignments and
// packs omitted from the batch are preserved. No partial saves on error.
SWMM_ENGINE_API int swmm_snowpack_apply(SWMM_Engine,const SWMM_SnowpackEdit*,int count,
    char* diagnostic,int capacity);

typedef struct SWMM_SnowState {
    int model; // DEGREE_DAY or UEB
    double swe_m, cover_fraction, melt_ms, surface_input_ms;
    double energy_kjm2, surface_temperature_c, bulk_temperature_c, albedo, surface_age;
    double sublimation_ms, water_age_s;
} SWMM_SnowState;
// Surface 0 plowable, 1 impervious, 2 pervious. Energy and dimensionless
// surface age describe the covered pack; SWE is averaged over the subarea.
// UEB-only quantities are NaN for the degree-day model, never invented zeros.
SWMM_ENGINE_API int swmm_subcatch_get_snow_process_state(SWMM_Engine, int subcatch, int surface, SWMM_SnowState*);
typedef struct SWMM_SnowLedger {
    double elapsed_s, initial_m3, precipitation_m3, liquid_m3, throughfall_m3;
    double sublimation_m3, removed_m3, transferred_m3, stored_m3, residual_m3;
    double net_energy_kj, geometry_energy_kj, plowed_energy_kj;
    double initial_energy_kj, stored_energy_kj, energy_residual_kj;
} SWMM_SnowLedger;
// UEB subcatchment inventory only; transfers are signed into that inventory.
SWMM_ENGINE_API int swmm_snow_get_ueb_ledger(SWMM_Engine, SWMM_SnowLedger*);

typedef struct SWMM_RadiationConfig {
    int explicit_site;
    double latitude, longitude, timezone, elevation_m;
    int shortwave_mode, longwave_mode; // 0 computed, 1 timeseries, 2 met file
    int shortwave_authored, longwave_authored; // 0 preserves legacy heat aliases
} SWMM_RadiationConfig;
SWMM_ENGINE_API int swmm_climate_get_radiation(SWMM_Engine, SWMM_RadiationConfig*);
SWMM_ENGINE_API int swmm_climate_set_radiation(SWMM_Engine, const SWMM_RadiationConfig*);
// channel 0 shortwave series, 1 longwave series, 2 met file path. These only
// bind names; the source selector is set through swmm_climate_set_radiation.
SWMM_ENGINE_API int swmm_climate_get_radiation_source(SWMM_Engine, int channel, char* value, int capacity);
SWMM_ENGINE_API int swmm_climate_set_radiation_source(SWMM_Engine, int channel, const char* value);
// Atomically replace the global snow/radiation configuration from section
// bodies (no [section] headers). Snowpack assignments are preserved. Omitted
// keys return to defaults. A diagnostic is returned before any publication.
SWMM_ENGINE_API int swmm_snow_apply_configuration(SWMM_Engine, const char* temperature_rows,
    const char* ueb_rows, char* diagnostic, int capacity);
typedef struct SWMM_ClimateCloudConfig {
    int mode; // 0 default (UEB transmissivity / legacy clear sky), 1 constant, 2 series
    double fraction, shortwave_k, shortwave_n, longwave_k;
} SWMM_ClimateCloudConfig;
SWMM_ENGINE_API int swmm_climate_get_cloud(SWMM_Engine,SWMM_ClimateCloudConfig*,char* series,int capacity);
typedef struct SWMM_ClimateMetSummary {
    int records;
    double first_date, last_date; // OLE dates, covered half-open interval
    double minimum[5], maximum[5]; // °C, RH %, m/s, shortwave W/m2, longwave W/m2
} SWMM_ClimateMetSummary;
SWMM_ENGINE_API int swmm_climate_preview_meteorology(SWMM_Engine,const char* path,
    SWMM_ClimateMetSummary*,char* diagnostic,int capacity);
// A single atomic edit for the Climatology view. Null cloud preserves the
// existing shared Heat/Climatology cloud record.
SWMM_ENGINE_API int swmm_snow_apply_configuration_with_cloud(SWMM_Engine,const char* temperature_rows,
    const char* ueb_rows,const SWMM_ClimateCloudConfig* cloud,const char* cloud_series,char* diagnostic,int capacity);
#ifdef __cplusplus
}
#endif
#endif
