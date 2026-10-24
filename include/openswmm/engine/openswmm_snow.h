// SPDX-License-Identifier: Apache-2.0

/**
 * @file      openswmm_snow.h
 * @brief     Public C API for snow packs, meteorology, and snow model configuration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

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
/**
 * @brief Read the global snow-process model.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] model Non-null result slot: SWMM_SNOW_DEGREE_DAY (0) or SWMM_SNOW_UEB (1).
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Reads authored configuration, not the model resolved for a particular pack.
 */
SWMM_ENGINE_API int swmm_snow_get_model(SWMM_Engine engine, int* model);
/**
 * @brief Select the global snow-process model.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] model SWMM_SNOW_DEGREE_DAY (0) or SWMM_SNOW_UEB (1).
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_snow_set_model(SWMM_Engine engine, int model);
/**
 * @brief Read the snow-process cadence in seconds.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] seconds Non-null result slot for the authored cadence.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Does not advance the snow solver; returns the stored global cadence.
 */
SWMM_ENGINE_API int swmm_snow_get_step(SWMM_Engine engine, double* seconds);
/**
 * @brief Configure the snow-process cadence in seconds.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] seconds Finite cadence in [1, 21600] seconds, validated by the SNOW_STEP parser.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_snow_set_step(SWMM_Engine engine, double seconds);
/**
 * @brief Read a recognized UEB option in its input-section text format.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] key Non-null null-terminated UEB key, accepted by uebOptionValue().
 * @param[out] value Non-null writable string buffer; receives the complete value and terminator.
 * @param[in] capacity Positive byte capacity including the terminator; insufficient
 * capacity returns SWMM_ERR_BUFSIZE and writes nothing.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, SWMM_ERR_BUFSIZE for a
 * short buffer, or SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Unknown keys return SWMM_ERR_BADPARAM. Reads authored configuration.
 */
SWMM_ENGINE_API int swmm_ueb_option_get(SWMM_Engine engine, const char* key, char* value,
                                        int capacity);
/**
 * @brief Parse and assign one UEB option from a textual value.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] key Non-null null-terminated UEB option key.
 * @param[in] value Non-null null-terminated value text, tokenized using input-file syntax.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_ueb_option_set(SWMM_Engine engine, const char* key, const char* value);
// Each monthly mean diurnal range is °C; <=0 means derive from the temperature source.
/**
 * @brief Read monthly mean diurnal temperature ranges.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] values Non-null caller-owned array of twelve writable doubles,
 * January through December, in degrees Celsius. An inherited month reads as zero.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note No output array is retained and no solver state is advanced.
 */
SWMM_ENGINE_API int swmm_ueb_get_monthly_ranges(SWMM_Engine engine, double values[12]);
/**
 * @brief Replace monthly mean diurnal temperature ranges.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] values Non-null caller-owned array of twelve readable doubles,
 * January through December, in degrees Celsius. Each value must be finite and in [0, 100]; zero
 * selects temperature-source derivation. The whole array is validated before publication.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Input storage is borrowed only for the call.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_ueb_set_monthly_ranges(SWMM_Engine engine, const double values[12]);
/**
 * @brief Read a named pack's UEB/model overrides.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] pack Zero-based snowpack index, in [0, snowpack count).
 * @param[out] value Non-null destination structure. Forest/drift are
 * dimensionless, ground heat W/m2, extinction depth m and initial energy kJ/m2.
 * An absent row returns SWMM_SNOW_DEFAULT with standard site defaults.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Returns authored/default values without resolving every inherited pack parameter.
 */
SWMM_ENGINE_API int swmm_snowpack_get_ueb(SWMM_Engine engine, int pack, SWMM_UebSnowpack* value);
/**
 * @brief Replace a named pack's UEB/model overrides.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] pack Zero-based snowpack index, in [0, snowpack count).
 * @param[in] value Non-null configuration structure. Forest/drift are
 * dimensionless, ground heat W/m2, extinction depth m and initial energy kJ/m2.
 * SWMM_SNOW_DEFAULT removes the explicit override; other models and site values are validated
 * by the snowpack parser.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_snowpack_set_ueb(SWMM_Engine engine, int pack,
                                          const SWMM_UebSnowpack* value);

typedef struct SWMM_SnowpackEdit {
    const char* name;
    double surface[3][7]; // existing PLOWABLE, IMPERVIOUS, PERVIOUS row units/order
    double removal[6];
    const char* removal_subcatch;
    SWMM_UebSnowpack ueb;
} SWMM_SnowpackEdit;
// Validate and upsert all named packs as one edit. Existing assignments and
// packs omitted from the batch are preserved. No partial saves on error.
/**
 * @brief Validate and upsert a batch of named snowpacks as one atomic edit.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] edits Borrowed count-element edit array; may be NULL only for count == 0.
 * Pack names must be nonempty, unique case-insensitively within the batch,
 * and contain no newline/quote characters. Surface/removal units follow the existing
 * three snowpack rows; UEB fields use SI units from SWMM_UebSnowpack.
 * @param[in] count Nonnegative number of pack edits; omitted existing packs are preserved.
 * @param[out] diagnostic Non-null writable diagnostic buffer; empty on successful validation.
 * @param[in] capacity Positive diagnostic byte capacity; error text may be truncated with a
 * terminator.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Validates on copies before publishing names and pack arrays. No partial
 * pack changes are saved on an argument/validation error. Requires an editable BUILDING or
 * OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_snowpack_apply(SWMM_Engine engine, const SWMM_SnowpackEdit* edits,
                                        int count, char* diagnostic, int capacity);

typedef struct SWMM_SnowState {
    int model; // DEGREE_DAY or UEB
    double swe_m, cover_fraction, melt_ms, surface_input_ms;
    double energy_kjm2, surface_temperature_c, bulk_temperature_c, albedo, surface_age;
    double sublimation_ms, water_age_s;
} SWMM_SnowState;
// Surface 0 plowable, 1 impervious, 2 pervious. Energy and dimensionless
// surface age describe the covered pack; SWE is averaged over the subarea.
// UEB-only quantities are NaN for the degree-day model, never invented zeros.
/**
 * @brief Snapshot one subcatchment snow surface's accepted process state.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] subcatch Zero-based valid subcatchment index.
 * @param[in] surface 0 plowable, 1 impervious, or 2 pervious.
 * @param[out] value Non-null caller-owned state. SWE/depth is subarea-averaged m,
 * liquid/sublimation rates m/s, temperatures degrees C, covered-pack energy kJ/m2,
 * water age seconds, and other fractions/age terms dimensionless.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADPARAM for invalid indices/output,
 * SWMM_ERR_LIFECYCLE when runtime state is unavailable, or SWMM_ERR_BADINDEX
 * when the subcatchment has no snowpack assignment.
 * @note UEB-only fields are NaN for degree-day snow. Completed source-driver
 * state is selected when it owns the subcatchment; no time integration occurs.
 */
SWMM_ENGINE_API int swmm_subcatch_get_snow_process_state(SWMM_Engine engine, int subcatch,
                                                         int surface, SWMM_SnowState* value);
typedef struct SWMM_SnowLedger {
    double elapsed_s, initial_m3, precipitation_m3, liquid_m3, throughfall_m3;
    double sublimation_m3, removed_m3, transferred_m3, stored_m3, residual_m3;
    double net_energy_kj, geometry_energy_kj, plowed_energy_kj;
    double initial_energy_kj, stored_energy_kj, energy_residual_kj;
} SWMM_SnowLedger;
// UEB subcatchment inventory only; transfers are signed into that inventory.
/**
 * @brief Aggregate the active UEB subcatchment water and energy inventory.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] value Non-null caller-owned ledger: elapsed_s in seconds,
 * water categories in m3, and energy categories in kJ. Transfers are signed
 * into the inventory; inactive duplicate native columns are excluded.
 * @return SWMM_OK, SWMM_ERR_BADHANDLE, SWMM_ERR_BADPARAM for a null result,
 * or SWMM_ERR_LIFECYCLE when UEB runtime is inactive.
 * @note Recomputes stored inventory and residuals from accepted states without advancing them.
 */
SWMM_ENGINE_API int swmm_snow_get_ueb_ledger(SWMM_Engine engine, SWMM_SnowLedger* value);

typedef struct SWMM_RadiationConfig {
    int explicit_site;
    double latitude, longitude, timezone, elevation_m;
    int shortwave_mode, longwave_mode;         // 0 computed, 1 timeseries, 2 met file
    int shortwave_authored, longwave_authored; // 0 preserves legacy heat aliases
} SWMM_RadiationConfig;
/**
 * @brief Read the shared radiation site and source selectors.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] value Non-null configuration. Site latitude/longitude are degrees,
 * timezone hours and elevation m. Modes are 0 computed, 1 time series, 2 met file;
 * authored flags control legacy-alias preservation.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note If no explicit site exists, resolves a fallback from shared heat/climate location settings.
 */
SWMM_ENGINE_API int swmm_climate_get_radiation(SWMM_Engine engine, SWMM_RadiationConfig* value);
/**
 * @brief Replace the shared radiation site and source selectors.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] value Non-null configuration. Site latitude/longitude are degrees,
 * timezone hours and elevation m. Modes are 0 computed, 1 time series, 2 met file;
 * authored flags control legacy-alias preservation.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_climate_set_radiation(SWMM_Engine engine,
                                               const SWMM_RadiationConfig* value);
// channel 0 shortwave series, 1 longwave series, 2 met file path. These only
// bind names; the source selector is set through swmm_climate_set_radiation.
/**
 * @brief Read one radiation source name or meteorology path.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] channel 0 shortwave time series, 1 longwave time series, 2 met-file path.
 * @param[out] value Non-null writable string buffer.
 * @param[in] capacity Positive byte capacity including the terminator; insufficient capacity
 * returns SWMM_ERR_BUFSIZE and writes nothing.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, SWMM_ERR_BUFSIZE for a
 * short buffer, or SWMM_ERR_BADPARAM for invalid arguments (including an unknown channel).
 * Output storage is caller-owned.
 * @note Binds text only; change the source mode separately with
 * swmm_climate_set_radiation(). The returned text is copied and output storage is not retained.
 */
SWMM_ENGINE_API int swmm_climate_get_radiation_source(SWMM_Engine engine, int channel, char* value,
                                                      int capacity);
/**
 * @brief Bind one radiation source name or meteorology path.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] channel 0 shortwave time series, 1 longwave time series, 2 met-file path.
 * @param[in] value Non-null null-terminated source text; newline and quote characters are rejected.
 *
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Binds text only; change the source mode separately with
 * swmm_climate_set_radiation(). Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_climate_set_radiation_source(SWMM_Engine engine, int channel,
                                                      const char* value);
// Atomically replace the global snow/radiation configuration from section
// bodies (no [section] headers). Snowpack assignments are preserved. Omitted
// keys return to defaults. A diagnostic is returned before any publication.
/**
 * @brief Atomically replace global snow/radiation section configuration.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] temperature_rows Non-null null-terminated TEMPERATURE section body without headers.
 * @param[in] ueb_rows Non-null null-terminated UEB section body without headers.
 * Empty bodies restore defaults; existing named-pack assignments are preserved.
 * @param[out] diagnostic Non-null writable buffer, empty on success or a validation message on
 * failure.
 * @param[in] capacity Positive byte capacity; diagnostics are truncated safely to fit.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Parses and validates copies, meteorology coverage and named radiation
 * series before publishing any configuration. Relative met paths resolve from
 * the input-file directory. Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int swmm_snow_apply_configuration(SWMM_Engine engine, const char* temperature_rows,
                                                  const char* ueb_rows, char* diagnostic,
                                                  int capacity);
typedef struct SWMM_ClimateCloudConfig {
    int mode; // 0 default (UEB transmissivity / legacy clear sky), 1 constant, 2 series
    double fraction, shortwave_k, shortwave_n, longwave_k;
} SWMM_ClimateCloudConfig;
/**
 * @brief Snapshot the shared heat/climatology cloud configuration and series name.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[out] config Non-null configuration: mode 0 default, 1 constant or 2 series,
 * fraction dimensionless and attenuation coefficients in their authored convention.
 * @param[out] series Non-null writable name buffer; empty when no valid series binding exists.
 * @param[in] capacity Positive byte capacity including the terminator; a short buffer returns
 * SWMM_ERR_BUFSIZE and writes nothing.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, SWMM_ERR_BUFSIZE for a
 * short buffer, or SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note The config structure is assigned only after the series text fits.
 */
SWMM_ENGINE_API int swmm_climate_get_cloud(SWMM_Engine engine, SWMM_ClimateCloudConfig* config,
                                           char* series, int capacity);
typedef struct SWMM_ClimateMetSummary {
    int records;
    double first_date, last_date;  // OLE dates, covered half-open interval
    double minimum[5], maximum[5]; // °C, RH %, m/s, shortwave W/m2, longwave W/m2
} SWMM_ClimateMetSummary;
/**
 * @brief Read and validate hourly meteorology without changing the model binding.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] path Non-null filename; relative paths resolve from the input file's parent directory.
 * @param[out] result Non-null summary with record count, half-open SWMM-date coverage,
 * and min/max columns in degrees C, relative-humidity percent, m/s, and W/m2 radiation.
 * @param[out] diagnostic Non-null writable text buffer; empty on success.
 * @param[in] capacity Positive byte capacity; an error message may be safely truncated.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Checks coverage of the configured simulation date interval when it
 * has positive duration. Model configuration and runtime are unchanged.
 */
SWMM_ENGINE_API int swmm_climate_preview_meteorology(SWMM_Engine engine, const char* path,
                                                     SWMM_ClimateMetSummary* result,
                                                     char* diagnostic, int capacity);
// A single atomic edit for the Climatology view. Null cloud preserves the
// existing shared Heat/Climatology cloud record.
/**
 * @brief Atomically replace global snow/radiation section configuration, optionally including
 * clouds.
 * @param[in] engine Borrowed live engine handle; not retained by this call.
 * @param[in] temperature_rows Non-null null-terminated TEMPERATURE section body without headers.
 * @param[in] ueb_rows Non-null null-terminated UEB section body without headers.
 * Empty bodies restore defaults; existing named-pack assignments are preserved.
 * @param[in] cloud Optional shared cloud configuration; NULL preserves it.
 * Mode is 0 default, 1 constant or 2 time series; fraction is [0, 1] and
 * attenuation coefficients/exponents are finite nonnegative values.
 * @param[in] cloud_series Required existing series name for cloud mode 2;
 * ignored when cloud is NULL or a non-series mode is selected.
 * @param[out] diagnostic Non-null writable buffer, empty on success or a validation message on
 * failure.
 * @param[in] capacity Positive byte capacity; diagnostics are truncated safely to fit.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for an invalid handle, or
 * SWMM_ERR_BADPARAM for invalid arguments. Output storage is caller-owned.
 * @note Parses and validates copies, meteorology coverage and named radiation
 * series before publishing any configuration. Relative met paths resolve from
 * the input-file directory. Requires an editable BUILDING or OPENED model; later states return
 * SWMM_ERR_LIFECYCLE. Input data is copied and no input storage is retained.
 */
SWMM_ENGINE_API int
swmm_snow_apply_configuration_with_cloud(SWMM_Engine engine, const char* temperature_rows,
                                         const char* ueb_rows, const SWMM_ClimateCloudConfig* cloud,
                                         const char* cloud_series, char* diagnostic, int capacity);
#ifdef __cplusplus
}
#endif
#endif
