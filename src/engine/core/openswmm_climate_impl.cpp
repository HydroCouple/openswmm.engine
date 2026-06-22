/**
 * @file openswmm_climate_impl.cpp
 * @brief C API implementation — climatology configuration get/set.
 *
 * @details Reads/writes the parsed climate configuration held in
 *          SimulationOptions (temperature, evaporation, wind, snowmelt globals,
 *          areal-depletion curves) and SimulationContext (monthly adjustments).
 *          Values are stored verbatim in the project's display units — the
 *          internal-unit conversion is applied later by SWMMEngine::initHydrology()
 *          — so these accessors are a straight pass-through (no UCF conversion).
 *
 * @see include/openswmm/engine/openswmm_climate.h
 * @see src/engine/core/SWMMEngine.cpp — initHydrology() propagates this config
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_climate.h"

#include <string>

namespace {

// NUL-terminated copy helper (mirrors gage_fill_buf in openswmm_gages_impl.cpp;
// kept local to avoid exporting a shared symbol across translation units).
void climate_fill_buf(char* buf, int sz, const std::string& s) {
    if (!buf || sz <= 0) return;
    const std::size_t n = std::min(static_cast<std::size_t>(sz - 1), s.size());
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
}

// Copy a fixed-size monthly/curve array out to the caller's buffer.
int copy_array_out(double* buf, int count, const double* src, int n) {
    if (!buf || count != n) return SWMM_ERR_BADPARAM;
    std::copy(src, src + n, buf);
    return SWMM_OK;
}

// Copy a fixed-size monthly/curve array in from the caller's buffer.
int copy_array_in(double* dst, const double* values, int count, int n) {
    if (!values || count != n) return SWMM_ERR_BADPARAM;
    std::copy(values, values + n, dst);
    return SWMM_OK;
}

// Validate every element of an ADC curve is a fraction in [0, 1].
bool adc_in_range(const double* values, int n) {
    for (int i = 0; i < n; ++i)
        if (values[i] < 0.0 || values[i] > 1.0) return false;
    return true;
}

} // namespace

extern "C" {

// ============================================================================
// Temperature  ([TEMPERATURE])
// ============================================================================

SWMM_ENGINE_API int swmm_climate_get_temp_source(SWMM_Engine engine, int* source) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (source) *source = ctx.options.temp_source;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_temp_source(SWMM_Engine engine, int source) {
    CHECK_HANDLE(engine);
    if (source < 0 || source > 2) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.temp_source = source;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_temp_timeseries(SWMM_Engine engine, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    climate_fill_buf(buf, buflen, ctx.options.temp_ts_name);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_temp_timeseries(SWMM_Engine engine, const char* ts_id) {
    CHECK_HANDLE(engine);
    if (!ts_id) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.temp_ts_name = ts_id;
    ctx.options.temp_source = 1;  // TIMESERIES
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_temp_file_start(SWMM_Engine engine, double* date) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (date) *date = ctx.options.temp_file_start;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_temp_file_start(SWMM_Engine engine, double date) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.temp_file_start = date;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_elevation(SWMM_Engine engine, double* elev) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (elev) *elev = ctx.options.snow_elev;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_elevation(SWMM_Engine engine, double elev) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.snow_elev = elev;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_latitude(SWMM_Engine engine, double* latitude) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (latitude) *latitude = ctx.options.snow_lat;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_latitude(SWMM_Engine engine, double latitude) {
    CHECK_HANDLE(engine);
    if (latitude < -90.0 || latitude > 90.0) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.snow_lat = latitude;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_longitude_correction(SWMM_Engine engine, double* minutes) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (minutes) *minutes = ctx.options.snow_dtlong;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_longitude_correction(SWMM_Engine engine, double minutes) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.snow_dtlong = minutes;
    return SWMM_OK;
}

// ============================================================================
// Evaporation  ([EVAPORATION])
// ============================================================================

SWMM_ENGINE_API int swmm_climate_get_evap_type(SWMM_Engine engine, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (type) *type = ctx.options.evap_type;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_evap_type(SWMM_Engine engine, int type) {
    CHECK_HANDLE(engine);
    if (type < 0 || type > 4) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.evap_type = type;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_evap_monthly(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.options.evap_values, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_evap_monthly(SWMM_Engine engine, const double* values, int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.evap_values, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_evap_timeseries(SWMM_Engine engine, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    climate_fill_buf(buf, buflen, ctx.options.evap_ts_name);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_evap_timeseries(SWMM_Engine engine, const char* ts_id) {
    CHECK_HANDLE(engine);
    if (!ts_id) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.evap_ts_name = ts_id;
    ctx.options.evap_type = 2;  // TIMESERIES
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_pan_coeff(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.options.pan_coeff, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_pan_coeff(SWMM_Engine engine, const double* values, int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.pan_coeff, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_evap_recovery(SWMM_Engine engine, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    climate_fill_buf(buf, buflen, ctx.options.evap_recovery_pat);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_evap_recovery(SWMM_Engine engine, const char* pattern_id) {
    CHECK_HANDLE(engine);
    if (!pattern_id) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.evap_recovery_pat = pattern_id;
    return SWMM_OK;
}

// ============================================================================
// Wind speed  ([TEMPERATURE] WINDSPEED)
// ============================================================================

SWMM_ENGINE_API int swmm_climate_get_wind_type(SWMM_Engine engine, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (type) *type = ctx.options.wind_type;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_wind_type(SWMM_Engine engine, int type) {
    CHECK_HANDLE(engine);
    if (type < 0 || type > 1) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.wind_type = type;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_wind_monthly(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.options.wind_speed, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_wind_monthly(SWMM_Engine engine, const double* values, int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.wind_speed, values, count, SWMM_CLIMATE_MONTHS);
}

// ============================================================================
// Snowmelt globals  ([TEMPERATURE] SNOWMELT)
// ============================================================================

SWMM_ENGINE_API int swmm_climate_get_snow_temp(SWMM_Engine engine, double* divide_temp) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (divide_temp) *divide_temp = ctx.options.snow_divt;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_snow_temp(SWMM_Engine engine, double divide_temp) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.snow_divt = divide_temp;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_ati_weight(SWMM_Engine engine, double* tipm) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (tipm) *tipm = ctx.options.snow_ati_wt;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_ati_weight(SWMM_Engine engine, double tipm) {
    CHECK_HANDLE(engine);
    if (tipm < 0.0 || tipm > 1.0) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.snow_ati_wt = tipm;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_neg_melt_ratio(SWMM_Engine engine, double* rnm) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (rnm) *rnm = ctx.options.snow_nrg_ratio;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_neg_melt_ratio(SWMM_Engine engine, double rnm) {
    CHECK_HANDLE(engine);
    if (rnm < 0.0 || rnm > 1.0) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.snow_nrg_ratio = rnm;
    return SWMM_OK;
}

// ============================================================================
// Areal-depletion curves  ([TEMPERATURE] ADC)
// ============================================================================

SWMM_ENGINE_API int swmm_climate_get_adc_impervious(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.options.adc_imperv, SWMM_CLIMATE_ADC_POINTS);
}

SWMM_ENGINE_API int swmm_climate_set_adc_impervious(SWMM_Engine engine, const double* values, int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_ADC_POINTS) return SWMM_ERR_BADPARAM;
    if (!adc_in_range(values, SWMM_CLIMATE_ADC_POINTS)) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.adc_imperv, values, count, SWMM_CLIMATE_ADC_POINTS);
}

SWMM_ENGINE_API int swmm_climate_get_adc_pervious(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.options.adc_perv, SWMM_CLIMATE_ADC_POINTS);
}

SWMM_ENGINE_API int swmm_climate_set_adc_pervious(SWMM_Engine engine, const double* values, int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_ADC_POINTS) return SWMM_ERR_BADPARAM;
    if (!adc_in_range(values, SWMM_CLIMATE_ADC_POINTS)) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.adc_perv, values, count, SWMM_CLIMATE_ADC_POINTS);
}

// ============================================================================
// Monthly adjustments  ([ADJUSTMENTS])
// ============================================================================

SWMM_ENGINE_API int swmm_climate_get_adjust_temperature(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.adjust_temp, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_adjust_temperature(SWMM_Engine engine, const double* values, int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.adjust_temp, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_adjust_evaporation(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.adjust_evap, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_adjust_evaporation(SWMM_Engine engine, const double* values, int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.adjust_evap, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_adjust_rainfall(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.adjust_rain, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_adjust_rainfall(SWMM_Engine engine, const double* values, int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.adjust_rain, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_adjust_conductivity(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.adjust_hydcon, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_adjust_conductivity(SWMM_Engine engine, const double* values, int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    // Legacy [ADJUSTMENTS] CONDUCT: non-positive multipliers reset to 1.0
    // (see InfraHandler.cpp handle_adjustments / climate_validate).
    for (int i = 0; i < SWMM_CLIMATE_MONTHS; ++i)
        ctx.adjust_hydcon[i] = (values[i] <= 0.0) ? 1.0 : values[i];
    return SWMM_OK;
}

} /* extern "C" */
