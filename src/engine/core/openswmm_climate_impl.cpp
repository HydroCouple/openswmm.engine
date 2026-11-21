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
 * @file      openswmm_climate_impl.cpp
 * @brief     C API implementation — climatology configuration get/set.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_climate.h"
#include "../hydrology/Climate.hpp"
#include "../hydrology/ClimateFile.hpp"
#include "DateTime.hpp"
#include "PathResolver.hpp"
#include "SimulationContext.hpp"
#include "UnitConversion.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

// NUL-terminated copy helper (mirrors gage_fill_buf in openswmm_gages_impl.cpp;
// kept local to avoid exporting a shared symbol across translation units).
/**
 * @brief Copy a truncated string prefix into a NUL-terminated caller buffer.
 *
 * Copies at most sz-1 bytes, without required-length reporting or UTF-8 boundary handling.
 *
 * @param[out] buf Writable sz-byte span or NULL for no-op.
 * @param[in] sz Capacity including terminator; <=0 no-op.
 * @param[in] s Borrowed bytes; must not overlap destination.
 */
void climate_fill_buf(char* buf, int sz, const std::string& s) {
    if (!buf || sz <= 0)
        return;
    const std::size_t n = std::min(static_cast<std::size_t>(sz - 1), s.size());
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
}

// Copy a fixed-size monthly/curve array out to the caller's buffer.
/**
 * @brief Copy a fixed-size climate array only when caller count matches exactly.
 *
 * Values retain source units and are not finite/range checked.
 *
 * @param[out] buf Non-NULL writable n-double span.
 * @param[in] count Caller capacity; must equal n rather than merely exceed it.
 * @param[in] src Readable n-double span, nonnegative n; no NULL/overlap check.
 * @param[in] n Required element count supplied by caller.
 * @return SWMM_OK after copying, BADPARAM for NULL buf or count mismatch; output preserved on
 *     refusal.
 */
int copy_array_out(double* buf, int count, const double* src, int n) {
    if (!buf || count != n)
        return SWMM_ERR_BADPARAM;
    std::copy(src, src + n, buf);
    return SWMM_OK;
}

// Copy a fixed-size monthly/curve array in from the caller's buffer.
/**
 * @brief Copy a caller climate array only when its count matches exactly.
 *
 * Does not validate finite values, ranges or units.
 *
 * @param[out] dst Writable n-double span supplied by caller; no NULL/overlap check.
 * @param[in] values Non-NULL readable n-double span.
 * @param[in] count Number supplied, required to equal n.
 * @param[in] n Nonnegative expected fixed array length.
 * @return SWMM_OK after copy, BADPARAM for NULL values or count mismatch; dst preserved on refusal.
 */
int copy_array_in(double* dst, const double* values, int count, int n) {
    if (!values || count != n)
        return SWMM_ERR_BADPARAM;
    std::copy(values, values + n, dst);
    return SWMM_OK;
}

/**
 * @brief Copy optional diagnostic text with truncation and NUL termination.
 *
 * No status/required-length return; can split a multibyte UTF-8 character.
 *
 * @param[out] out Writable capacity-byte span, NULL performs no work.
 * @param[in] capacity Capacity including terminator; nonpositive no-op.
 * @param[in] message Borrowed text, no overlap with destination.
 */
void climate_diagnostic(char* out, int capacity, const std::string& message) {
    if (!out || capacity <= 0)
        return;
    const auto n = std::min(message.size(), static_cast<std::size_t>(capacity - 1));
    std::memcpy(out, message.data(), n);
    out[n] = '\0';
}

/**
 * @brief Interpolate a sorted climate table with endpoint holding.
 *
 * Exact x uses its lower_bound row; bracket width below1e-20 averages y endpoints. Does not
 * validate sorting, duplicates, finite values or shape.
 *
 * @param[in] table Borrowed ascending x vector with matching y length; sample count must fit int.
 * @param[in] date Finite OADate query comparable with table x; no cursor mutation.
 * @return Zero for no samples, endpoint value outside range, otherwise linear interpolation in y
 *     units.
 */
double lookup_table(const openswmm::Table& table, double date) {
    const int n = static_cast<int>(table.x.size());
    if (n <= 0)
        return 0.0;
    if (n == 1 || date <= table.x.front())
        return table.y.front();
    if (date > table.x.back())
        return table.y.back();
    const auto upper = std::lower_bound(table.x.begin(), table.x.end(), date);
    if (upper == table.x.end())
        return table.y.back();
    const auto i = static_cast<std::size_t>(upper - table.x.begin());
    if (*upper == date || i == 0)
        return table.y[i];
    const auto lo = i - 1;
    const double dx = table.x[i] - table.x[lo];
    if (std::fabs(dx) < 1.0e-20)
        return (table.y[lo] + table.y[i]) / 2.0;
    return table.y[lo] + (date - table.x[lo]) * (table.y[i] - table.y[lo]) / dx;
}

/**
 * @brief Convert a calendar date to a clamped zero-based month.
 *
 * @param[in] date Calendar-encodable OADate value; no explicit validity check.
 * @return monthOfYear(date)-1 clamped into0..11.
 */
int month_index(double date) {
    return std::clamp(openswmm::datetime::monthOfYear(date) - 1, 0, 11);
}

/**
 * @brief Find a named climate time series without registering one.
 *
 * @param[in] ctx Borrowed coherent table/name store.
 * @param[in] name Borrowed series name; empty means unbound.
 * @return Matching timeseries index, or -1 for empty/unknown name.
 */
int table_index(const openswmm::SimulationContext& ctx, const std::string& name) {
    return name.empty() ? -1 : ctx.find_timeseries(name);
}

/**
 * @brief Copy the effective temperature-file path using the INP directory as fallback base.
 *
 * No file open, existence check, path-slot update or ownership transfer.
 *
 * @param[in] ctx Borrowed context/path pair; absolute token takes precedence.
 * @return Owned effective UTF-8 path, empty when no authoring path; fallback resolved relative to
 *     parentDir(inp_file_path).
 */
std::string resolved_climate_path(const openswmm::SimulationContext& ctx) {
    const auto& file = ctx.options.temp_file;
    if (!file.absolute.empty())
        return file.absolute;
    if (file.original.empty())
        return {};
    return openswmm::io::resolveRelative(file.original, openswmm::io::parentDir(ctx.inp_file_path));
}

/**
 * @brief Open a configured climate reader and align its day offset to simulation start.
 *
 * Filesystem/path/reader/allocation exceptions propagate; no changes to model path/source
 * selection.
 *
 * @param[in] ctx Borrowed model providing INP path base and simulation start OADate.
 * @param[in] unit_system Climate-reader unit-system code, validated by reader/caller.
 * @param[in] configured_path Borrowed UTF-8 path resolved relative to INP parent; empty is refused.
 * @param[in] file_start OADate filter; positive aligns integer days with model start, otherwise
 *     offset0.
 * @param[in] temp_units Temperature-unit selector passed to reader.open.
 * @param[in,out] reader Borrowed reader opened/reconfigured in place; caller owns closing/lifetime.
 * @param[out] error Assigned on empty path/open failure, unchanged on success.
 * @return True after open and day-offset assignment, false with diagnostic on missing/unrecognized
 *     file.
 */
bool open_climate_file(const openswmm::SimulationContext& ctx, int unit_system,
                       const std::string& configured_path, double file_start, int temp_units,
                       openswmm::climate::ClimateFileReader& reader, std::string& error) {
    if (configured_path.empty()) {
        error = "No climate file is configured for this evaporation source.";
        return false;
    }
    const std::string path =
        openswmm::io::resolveRelative(configured_path, openswmm::io::parentDir(ctx.inp_file_path));
    if (!reader.open(path, file_start, unit_system, temp_units)) {
        error = "The configured climate file could not be opened or its format was not recognized.";
        return false;
    }
    reader.setDayOffset(
        file_start > 0.0 ? std::floor(file_start) - std::floor(ctx.options.start_date) : 0.0);
    return true;
}

// Validate every element of an ADC curve is a fraction in [0, 1].
/**
 * @brief Reject depletion-curve entries comparing outside inclusive0..1.
 *
 * This comparison guard does not establish finiteness; caller must separately reject NaN when
 * required.
 *
 * @param[in] values Readable n-double span; NULL permitted only for n<=0.
 * @param[in] n Entries to inspect; <=0 succeeds without reads.
 * @return False for any value<0 or>1; true otherwise, including NaN entries.
 */
bool adc_in_range(const double* values, int n) {
    for (int i = 0; i < n; ++i)
        if (values[i] < 0.0 || values[i] > 1.0)
            return false;
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
    if (source)
        *source = ctx.options.temp_source;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_temp_source(SWMM_Engine engine, int source) {
    CHECK_HANDLE(engine);
    if (source < 0 || source > 2)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.temp_source = source;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_temp_timeseries(SWMM_Engine engine, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    climate_fill_buf(buf, buflen, ctx.options.temp_ts_name);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_temp_timeseries(SWMM_Engine engine, const char* ts_id) {
    CHECK_HANDLE(engine);
    if (!ts_id)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.temp_ts_name = ts_id;
    ctx.options.temp_source = 1; // TIMESERIES
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_temp_file_start(SWMM_Engine engine, double* date) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (date)
        *date = ctx.options.temp_file_start;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_temp_file_start(SWMM_Engine engine, double date) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.temp_file_start = date;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_temp_units(SWMM_Engine engine, int* units) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (units)
        *units = ctx.options.temp_units;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_temp_units(SWMM_Engine engine, int units) {
    CHECK_HANDLE(engine);
    if (units < -1 || units > 2)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.temp_units = units;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_elevation(SWMM_Engine engine, double* elev) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (elev)
        *elev = ctx.options.snow_elev;
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
    if (latitude)
        *latitude = ctx.options.snow_lat;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_latitude(SWMM_Engine engine, double latitude) {
    CHECK_HANDLE(engine);
    if (latitude < -90.0 || latitude > 90.0)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.snow_lat = latitude;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_longitude_correction(SWMM_Engine engine, double* minutes) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (minutes)
        *minutes = ctx.options.snow_dtlong;
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
    if (type)
        *type = ctx.options.evap_type;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_evap_type(SWMM_Engine engine, int type) {
    CHECK_HANDLE(engine);
    if (type < 0 || type > 4)
        return SWMM_ERR_BADPARAM;
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

SWMM_ENGINE_API int swmm_climate_set_evap_monthly(SWMM_Engine engine, const double* values,
                                                  int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.evap_values, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_evap_timeseries(SWMM_Engine engine, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    climate_fill_buf(buf, buflen, ctx.options.evap_ts_name);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_evap_timeseries(SWMM_Engine engine, const char* ts_id) {
    CHECK_HANDLE(engine);
    if (!ts_id)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.evap_ts_name = ts_id;
    ctx.options.evap_type = 2; // TIMESERIES
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_preview_evaporation_with_config(
    SWMM_Engine engine, const SWMM_EvapPreviewConfig* config, double start_date, double end_date,
    double sample_interval_days, SWMM_EvapPreviewSample* samples, int capacity, int* sample_count,
    SWMM_EvapPreviewSummary* summary, char* diagnostic, int diagnostic_capacity) {
    CHECK_HANDLE(engine);
    if (!sample_count || !summary || !diagnostic || diagnostic_capacity <= 0 || capacity < 0 ||
        (capacity > 0 && !samples) || !std::isfinite(start_date) || !std::isfinite(end_date) ||
        !std::isfinite(sample_interval_days) || sample_interval_days <= 0.0 ||
        end_date <= start_date)
        return SWMM_ERR_BADPARAM;

    diagnostic[0] = '\0';
    const auto& ctx = to_engine(engine)->context();
    const auto& options = ctx.options;
    int engine_state = -1;
    if (swmm_engine_get_state(engine, &engine_state) != SWMM_OK)
        return SWMM_ERR_BADHANDLE;
    if (engine_state == SWMM_STATE_RUNNING) {
        climate_diagnostic(diagnostic, diagnostic_capacity,
                           "Evaporation preview is unavailable while the model is running.");
        return SWMM_ERR_LIFECYCLE;
    }
    if (start_date < options.start_date - 1.0e-10 || end_date > options.end_date + 1.0e-10) {
        climate_diagnostic(diagnostic, diagnostic_capacity,
                           "The preview window must fall within the simulation dates.");
        return SWMM_ERR_BADPARAM;
    }

    const double count_d = std::ceil((end_date - start_date) / sample_interval_days - 1.0e-12);
    if (!std::isfinite(count_d) || count_d < 1.0 || count_d > 1000000.0) {
        climate_diagnostic(diagnostic, diagnostic_capacity,
                           "The preview window produces an unsupported number of samples.");
        return SWMM_ERR_BADPARAM;
    }
    const int needed = static_cast<int>(count_d);
    *sample_count = needed;
    if (samples && capacity < needed) {
        climate_diagnostic(diagnostic, diagnostic_capacity,
                           "The sample buffer is too small for this preview window.");
        return SWMM_ERR_BADPARAM;
    }

    const int unit_system = openswmm::ucf::getUnitSystem(static_cast<int>(options.flow_units));
    int evap_type = options.evap_type;
    int temp_source = options.temp_source;
    double evap_monthly[SWMM_CLIMATE_MONTHS]{};
    double pan_coeff[SWMM_CLIMATE_MONTHS]{};
    double adjust_evap[SWMM_CLIMATE_MONTHS]{};
    double adjust_temp[SWMM_CLIMATE_MONTHS]{};
    for (int month = 0; month < SWMM_CLIMATE_MONTHS; ++month) {
        evap_monthly[month] = options.evap_values[month];
        pan_coeff[month] = options.pan_coeff[month];
        adjust_evap[month] = ctx.adjust_evap[month];
        adjust_temp[month] = ctx.adjust_temp[month];
    }
    std::string evap_ts_name = options.evap_ts_name;
    std::string temp_ts_name = options.temp_ts_name;
    std::string climate_path = resolved_climate_path(ctx);
    double climate_file_start = options.temp_file_start;
    int climate_file_temp_units = options.temp_units;
    double latitude = options.snow_lat;
    double longitude_correction_minutes = options.snow_dtlong;
    if (config) {
        evap_type = config->evap_type;
        temp_source = config->temp_source;
        evap_ts_name = config->evap_timeseries ? config->evap_timeseries : "";
        temp_ts_name = config->temp_timeseries ? config->temp_timeseries : "";
        climate_path = config->climate_file_path ? config->climate_file_path : "";
        climate_file_start = config->climate_file_start;
        climate_file_temp_units = config->climate_file_temp_units;
        latitude = config->latitude;
        longitude_correction_minutes = config->longitude_correction_minutes;
        for (int month = 0; month < SWMM_CLIMATE_MONTHS; ++month) {
            evap_monthly[month] = config->evap_monthly[month];
            pan_coeff[month] = config->pan_coeff[month];
            adjust_evap[month] = config->adjust_evaporation[month];
            adjust_temp[month] = config->adjust_temperature[month];
            if (!std::isfinite(evap_monthly[month]) || !std::isfinite(pan_coeff[month]) ||
                !std::isfinite(adjust_evap[month]) || !std::isfinite(adjust_temp[month])) {
                climate_diagnostic(diagnostic, diagnostic_capacity,
                                   "The draft contains a non-finite climate value.");
                return SWMM_ERR_BADPARAM;
            }
        }
        if (temp_source < SWMM_TEMP_NONE || temp_source > SWMM_TEMP_FILE ||
            climate_file_temp_units < -1 || climate_file_temp_units > 2 ||
            !std::isfinite(climate_file_start) || !std::isfinite(latitude) ||
            !std::isfinite(longitude_correction_minutes) || latitude < -90.0 || latitude > 90.0) {
            climate_diagnostic(diagnostic, diagnostic_capacity,
                               "The draft contains an invalid temperature source or site value.");
            return SWMM_ERR_BADPARAM;
        }
    }
    if (evap_type < SWMM_EVAP_CONSTANT || evap_type > SWMM_EVAP_FILE) {
        climate_diagnostic(diagnostic, diagnostic_capacity,
                           "The evaporation source type is invalid.");
        return SWMM_ERR_BADPARAM;
    }
    const int evap_ts = evap_type == SWMM_EVAP_TIMESERIES ? table_index(ctx, evap_ts_name) : -1;
    const openswmm::Table* evap_table =
        evap_ts >= 0 ? &ctx.tables.tables[static_cast<std::size_t>(evap_ts)] : nullptr;

    openswmm::climate::ClimateFileReader pan_file;
    openswmm::climate::ClimateFileReader temp_file;
    bool pan_file_open = false;
    bool temp_file_open = false;
    std::string pan_file_error;
    std::string temp_file_error;
    if (evap_type == SWMM_EVAP_FILE)
        pan_file_open = open_climate_file(ctx, unit_system, climate_path, climate_file_start,
                                          climate_file_temp_units, pan_file, pan_file_error);
    if (evap_type == SWMM_EVAP_TEMPERATURE && temp_source == SWMM_TEMP_FILE)
        temp_file_open = open_climate_file(ctx, unit_system, climate_path, climate_file_start,
                                           climate_file_temp_units, temp_file, temp_file_error);

    const int temp_ts_idx =
        temp_source == SWMM_TEMP_TIMESERIES ? table_index(ctx, temp_ts_name) : -1;
    const openswmm::Table* temp_table =
        temp_ts_idx >= 0 ? &ctx.tables.tables[static_cast<std::size_t>(temp_ts_idx)] : nullptr;

    // The evaporation time series is initialized once at model start, then
    // advances in steps at its timestamps. Replaying that cursor locally keeps
    // this preview read-only and preserves non-midnight simulation starts.
    double evap_current = 0.0;
    int evap_next = -1;
    if (evap_table && !evap_table->x.empty()) {
        const auto& x = evap_table->x;
        int initial = 0;
        if (x.front() < options.start_date) {
            const auto it = std::lower_bound(x.begin(), x.end(), options.start_date);
            initial =
                it == x.end() ? static_cast<int>(x.size()) - 1 : static_cast<int>(it - x.begin());
        }
        evap_current = evap_table->y[static_cast<std::size_t>(initial)];
        evap_next = initial + 1;
        while (evap_next >= 0 && evap_next < static_cast<int>(x.size()) &&
               x[static_cast<std::size_t>(evap_next)] > options.end_date)
            evap_next = -1;
    }

    // Hargreaves uses runtime air temperature and a seven-sample moving
    // average. A daily preview follows the same equation and temperature
    // conversions, while being explicit that runtime step frequency/forcing
    // can change the exact result.
    openswmm::climate::ClimateState hargreaves_state;
    hargreaves_state.latitude = latitude;
    hargreaves_state.dtlong = longitude_correction_minutes / 60.0;
    double file_tmin = 70.0, file_tmax = 70.0;
    double prev_tmax = 70.0;
    double last_pan_source = 0.0, last_pan_effective = 0.0;
    bool pan_rate_seen = false;
    bool temperature_seen = temp_source == SWMM_TEMP_NONE;
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    SWMM_EvapPreviewSummary next_summary{};
    next_summary.source_type = evap_type;
    next_summary.runtime_forcing_unknown = 1;
    next_summary.minimum_rate = std::numeric_limits<double>::quiet_NaN();
    next_summary.maximum_rate = std::numeric_limits<double>::quiet_NaN();

    for (int i = 0; i < needed; ++i) {
        const double date = start_date + static_cast<double>(i) * sample_interval_days;
        const int month = month_index(date);
        double source = 0.0;
        double adjustment = 0.0;
        double effective = 0.0;
        int status = SWMM_EVAP_PREVIEW_AVAILABLE;

        switch (evap_type) {
        case SWMM_EVAP_CONSTANT:
        case SWMM_EVAP_MONTHLY: {
            source = evap_monthly[evap_type == SWMM_EVAP_CONSTANT ? 0 : month];
            adjustment = adjust_evap[month];
            effective = source + adjustment;
            break;
        }
        case SWMM_EVAP_TIMESERIES: {
            if (!evap_table || evap_table->x.empty()) {
                status = SWMM_EVAP_PREVIEW_MISSING;
            } else {
                const auto& x = evap_table->x;
                while (evap_next >= 0 && evap_next < static_cast<int>(x.size()) &&
                       x[static_cast<std::size_t>(evap_next)] <= date) {
                    evap_current = evap_table->y[static_cast<std::size_t>(evap_next)];
                    ++evap_next;
                    if (evap_next >= static_cast<int>(x.size()) ||
                        x[static_cast<std::size_t>(evap_next)] > options.end_date)
                        evap_next = -1;
                }
                source = evap_current;
                adjustment = adjust_evap[month];
                effective = source + adjustment;
                if (date < x.front() || date > x.back())
                    status = SWMM_EVAP_PREVIEW_HELD;
            }
            break;
        }
        case SWMM_EVAP_FILE: {
            openswmm::climate::DailyClimateRecord record;
            const bool has_record = pan_file_open && pan_file.getRecord(date, record);
            if (has_record && std::isfinite(record.evap)) {
                source = record.evap * pan_coeff[month];
                adjustment = adjust_evap[month];
                effective = source + adjustment;
                last_pan_source = source;
                last_pan_effective = effective;
                pan_rate_seen = true;
            } else if (pan_rate_seen) {
                source = last_pan_source;
                effective = last_pan_effective;
                status = SWMM_EVAP_PREVIEW_HELD;
            } else {
                status = SWMM_EVAP_PREVIEW_MISSING;
            }
            break;
        }
        case SWMM_EVAP_TEMPERATURE: {
            double temperature = 70.0;
            double range = 0.0;
            bool available = temp_source == SWMM_TEMP_NONE;
            bool held = false;
            if (temp_table && !temp_table->x.empty()) {
                temperature = lookup_table(*temp_table, date);
                if (unit_system == 1)
                    temperature = (9.0 / 5.0) * temperature + 32.0;
                temperature += adjust_temp[month] * (unit_system == 1 ? 9.0 / 5.0 : 1.0);
                available = true;
                held = date < temp_table->x.front() || date > temp_table->x.back();
            } else if (temp_source == SWMM_TEMP_FILE && temp_file_open) {
                openswmm::climate::DailyClimateRecord record;
                const bool has_record = temp_file.getRecord(date, record);
                if (has_record) {
                    if (std::isfinite(record.tmin))
                        file_tmin = record.tmin;
                    if (std::isfinite(record.tmax))
                        file_tmax = record.tmax;
                    available = std::isfinite(record.tmin) || std::isfinite(record.tmax);
                    if (!available)
                        held = temperature_seen;
                } else {
                    held = temperature_seen;
                }
                const double tmin =
                    file_tmin + adjust_temp[month] * (unit_system == 1 ? 9.0 / 5.0 : 1.0);
                const double tmax =
                    file_tmax + adjust_temp[month] * (unit_system == 1 ? 9.0 / 5.0 : 1.0);
                range = tmax - tmin;
                if (hargreaves_state.last_temp_doy != openswmm::datetime::dayOfYear(date)) {
                    prev_tmax = temperature_seen ? hargreaves_state.tmax_daily : tmax;
                    hargreaves_state.tmin_daily = tmin;
                    hargreaves_state.tmax_daily = tmax;
                    hargreaves_state.has_minmax = true;
                    hargreaves_state.last_temp_doy = openswmm::datetime::dayOfYear(date);
                    hargreaves_state.prev_tmax = prev_tmax;
                    openswmm::climate::updateTempTimes(hargreaves_state,
                                                       openswmm::datetime::dayOfYear(date));
                }
                hargreaves_state.tmin_daily = tmin;
                hargreaves_state.tmax_daily = tmax;
                const double hour = (date - std::floor(date)) * 24.0;
                temperature = openswmm::climate::getSubdailyTemp(hargreaves_state, hour);
                temperature_seen = temperature_seen || available;
            }
            hargreaves_state.temperature = temperature;
            hargreaves_state.temp_range = range;
            hargreaves_state.temp_ma.push(temperature, range);
            source = openswmm::climate::hargreaves(latitude, openswmm::datetime::dayOfYear(date),
                                                   hargreaves_state.temp_ma.avg_temp(),
                                                   hargreaves_state.temp_ma.avg_range());
            if (unit_system == 0)
                source /= openswmm::climate::MM_PER_INCH;
            adjustment = adjust_evap[month];
            effective = source + adjustment;
            if (!available && temp_source != SWMM_TEMP_NONE)
                status = SWMM_EVAP_PREVIEW_MISSING;
            else if (held)
                status = SWMM_EVAP_PREVIEW_HELD;
            else
                status = SWMM_EVAP_PREVIEW_ESTIMATED;
            // This is a daily estimate, even when a file or time series
            // has complete weather coverage.
            if (status == SWMM_EVAP_PREVIEW_AVAILABLE)
                status = SWMM_EVAP_PREVIEW_ESTIMATED;
            break;
        }
        default:
            status = SWMM_EVAP_PREVIEW_MISSING;
            break;
        }

        if (!std::isfinite(effective)) {
            status = SWMM_EVAP_PREVIEW_MISSING;
        } else if (status != SWMM_EVAP_PREVIEW_MISSING) {
            minimum = std::min(minimum, effective);
            maximum = std::max(maximum, effective);
        }
        switch (status) {
        case SWMM_EVAP_PREVIEW_AVAILABLE:
            ++next_summary.available_count;
            break;
        case SWMM_EVAP_PREVIEW_HELD:
            ++next_summary.held_count;
            break;
        case SWMM_EVAP_PREVIEW_MISSING:
            ++next_summary.missing_count;
            break;
        case SWMM_EVAP_PREVIEW_ESTIMATED:
            ++next_summary.estimated_count;
            break;
        }
        if (samples)
            samples[i] = {date, source, adjustment, effective, status};
    }

    if (std::isfinite(minimum)) {
        next_summary.minimum_rate = minimum;
        next_summary.maximum_rate = maximum;
    }
    *summary = next_summary;
    if ((evap_type == SWMM_EVAP_FILE && !pan_file_open) ||
        (evap_type == SWMM_EVAP_TEMPERATURE && temp_source == SWMM_TEMP_FILE && !temp_file_open))
        climate_diagnostic(diagnostic, diagnostic_capacity,
                           evap_type == SWMM_EVAP_FILE ? pan_file_error : temp_file_error);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_preview_evaporation(SWMM_Engine engine, double start_date,
                                                     double end_date, double sample_interval_days,
                                                     SWMM_EvapPreviewSample* samples, int capacity,
                                                     int* sample_count,
                                                     SWMM_EvapPreviewSummary* summary,
                                                     char* diagnostic, int diagnostic_capacity) {
    return swmm_climate_preview_evaporation_with_config(
        engine, nullptr, start_date, end_date, sample_interval_days, samples, capacity,
        sample_count, summary, diagnostic, diagnostic_capacity);
}

SWMM_ENGINE_API int swmm_climate_get_pan_coeff(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.options.pan_coeff, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_pan_coeff(SWMM_Engine engine, const double* values,
                                               int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.pan_coeff, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_evap_recovery(SWMM_Engine engine, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    climate_fill_buf(buf, buflen, ctx.options.evap_recovery_pat);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_evap_recovery(SWMM_Engine engine, const char* pattern_id) {
    CHECK_HANDLE(engine);
    if (!pattern_id)
        return SWMM_ERR_BADPARAM;
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
    if (type)
        *type = ctx.options.wind_type;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_wind_type(SWMM_Engine engine, int type) {
    CHECK_HANDLE(engine);
    if (type < 0 || type > 1)
        return SWMM_ERR_BADPARAM;
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

SWMM_ENGINE_API int swmm_climate_set_wind_monthly(SWMM_Engine engine, const double* values,
                                                  int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.wind_speed, values, count, SWMM_CLIMATE_MONTHS);
}

// ============================================================================
// Humidity  ([TEMPERATURE] HUMIDITY)
// ============================================================================

SWMM_ENGINE_API int swmm_climate_get_humidity_type(SWMM_Engine engine, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (type)
        *type = ctx.options.humidity_type;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_humidity_type(SWMM_Engine engine, int type) {
    CHECK_HANDLE(engine);
    if (type < 0 || type > 2)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.humidity_type = type;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_humidity_variable(SWMM_Engine engine, int* var) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (var)
        *var = ctx.options.humidity_var;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_humidity_variable(SWMM_Engine engine, int var) {
    CHECK_HANDLE(engine);
    if (var < 0 || var > 1)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.humidity_var = var;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_humidity_monthly(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.options.humidity, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_humidity_monthly(SWMM_Engine engine, const double* values,
                                                      int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.humidity, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_humidity_timeseries(SWMM_Engine engine, char* buf,
                                                         int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    climate_fill_buf(buf, buflen, ctx.options.humidity_ts_name);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_humidity_timeseries(SWMM_Engine engine, const char* ts_id) {
    CHECK_HANDLE(engine);
    if (!ts_id)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.humidity_ts_name = ts_id;
    ctx.options.humidity_type = 2; // TIMESERIES
    return SWMM_OK;
}

// ============================================================================
// Snowmelt globals  ([TEMPERATURE] SNOWMELT)
// ============================================================================

SWMM_ENGINE_API int swmm_climate_get_snow_temp(SWMM_Engine engine, double* divide_temp) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (divide_temp)
        *divide_temp = ctx.options.snow_divt;
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
    if (tipm)
        *tipm = ctx.options.snow_ati_wt;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_ati_weight(SWMM_Engine engine, double tipm) {
    CHECK_HANDLE(engine);
    if (tipm < 0.0 || tipm > 1.0)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    ctx.options.snow_ati_wt = tipm;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_neg_melt_ratio(SWMM_Engine engine, double* rnm) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (rnm)
        *rnm = ctx.options.snow_nrg_ratio;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_neg_melt_ratio(SWMM_Engine engine, double rnm) {
    CHECK_HANDLE(engine);
    if (rnm < 0.0 || rnm > 1.0)
        return SWMM_ERR_BADPARAM;
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

SWMM_ENGINE_API int swmm_climate_set_adc_impervious(SWMM_Engine engine, const double* values,
                                                    int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_ADC_POINTS)
        return SWMM_ERR_BADPARAM;
    if (!adc_in_range(values, SWMM_CLIMATE_ADC_POINTS))
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.adc_imperv, values, count, SWMM_CLIMATE_ADC_POINTS);
}

SWMM_ENGINE_API int swmm_climate_get_adc_pervious(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.options.adc_perv, SWMM_CLIMATE_ADC_POINTS);
}

SWMM_ENGINE_API int swmm_climate_set_adc_pervious(SWMM_Engine engine, const double* values,
                                                  int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_ADC_POINTS)
        return SWMM_ERR_BADPARAM;
    if (!adc_in_range(values, SWMM_CLIMATE_ADC_POINTS))
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.options.adc_perv, values, count, SWMM_CLIMATE_ADC_POINTS);
}

// ============================================================================
// Monthly adjustments  ([ADJUSTMENTS])
// ============================================================================

SWMM_ENGINE_API int swmm_climate_get_adjust_temperature(SWMM_Engine engine, double* buf,
                                                        int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.adjust_temp, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_adjust_temperature(SWMM_Engine engine, const double* values,
                                                        int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.adjust_temp, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_adjust_evaporation(SWMM_Engine engine, double* buf,
                                                        int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.adjust_evap, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_adjust_evaporation(SWMM_Engine engine, const double* values,
                                                        int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.adjust_evap, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_adjust_rainfall(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.adjust_rain, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_adjust_rainfall(SWMM_Engine engine, const double* values,
                                                     int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return copy_array_in(ctx.adjust_rain, values, count, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_get_adjust_conductivity(SWMM_Engine engine, double* buf,
                                                         int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return copy_array_out(buf, count, ctx.adjust_hydcon, SWMM_CLIMATE_MONTHS);
}

SWMM_ENGINE_API int swmm_climate_set_adjust_conductivity(SWMM_Engine engine, const double* values,
                                                         int count) {
    CHECK_HANDLE(engine);
    if (!values || count != SWMM_CLIMATE_MONTHS)
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    // Legacy [ADJUSTMENTS] CONDUCT: non-positive multipliers reset to 1.0
    // (see InfraHandler.cpp handle_adjustments / climate_validate).
    for (int i = 0; i < SWMM_CLIMATE_MONTHS; ++i)
        ctx.adjust_hydcon[i] = (values[i] <= 0.0) ? 1.0 : values[i];
    return SWMM_OK;
}

} /* extern "C" */
