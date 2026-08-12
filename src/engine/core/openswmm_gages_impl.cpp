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
 * @file openswmm_gages_impl.cpp
 * @brief C API implementation — rain gage identity, creation, properties, state, bulk.
 *
 * @see include/openswmm/engine/openswmm_gages.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_gages.h"

extern "C" {

// ============================================================================
// Identity
// ============================================================================

SWMM_ENGINE_API int swmm_gage_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().n_gages();
}

SWMM_ENGINE_API int swmm_gage_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    return to_engine(engine)->context().gage_names.find(id);
}

SWMM_ENGINE_API const char* swmm_gage_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& ctx = to_engine(engine)->context();
    if (idx < 0 || idx >= ctx.n_gages()) return nullptr;
    return ctx.gage_names.name_of(idx).c_str();
}

// ============================================================================
// Creation (BUILDING or OPENED — "editable" states)
// ============================================================================

SWMM_ENGINE_API int swmm_gage_add(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);

    if (ctx.gage_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    ctx.gage_names.add(id);
    int n = ctx.gage_names.size();
    ctx.gages.grow_to(n);
    const auto un = static_cast<std::size_t>(n);
    if (ctx.spatial.gage_x.size() < un) ctx.spatial.gage_x.resize(un, 0.0);
    if (ctx.spatial.gage_y.size() < un) ctx.spatial.gage_y.resize(un, 0.0);

    return SWMM_OK;
}

// ============================================================================
// Property setters
// ============================================================================

SWMM_ENGINE_API int swmm_gage_set_rain_type(SWMM_Engine engine, int idx, int type) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    if (type < 0 || type > 2) return SWMM_ERR_BADPARAM;
    ctx.gages.rain_type[static_cast<std::size_t>(idx)] = type;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_set_rain_interval(SWMM_Engine engine, int idx, double seconds) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    ctx.gages.interval_sec[static_cast<std::size_t>(idx)] = static_cast<int>(seconds);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_set_data_source(SWMM_Engine engine, int idx, int source) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    ctx.gages.source[static_cast<std::size_t>(idx)] = static_cast<openswmm::RainSource>(source);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_set_timeseries(SWMM_Engine engine, int idx, const char* ts_id) {
    CHECK_HANDLE(engine);
    if (!ts_id) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    int ts_idx = ctx.find_timeseries(ts_id);
    if (ts_idx < 0) return SWMM_ERR_BADPARAM;
    ctx.gages.ts_index[static_cast<std::size_t>(idx)] = ts_idx;
    ctx.gages.source[static_cast<std::size_t>(idx)] = openswmm::RainSource::TIMESERIES;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_set_filename(SWMM_Engine engine, int idx, const char* path,
                                             const char* station_id) {
    CHECK_HANDLE(engine);
    if (!path) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    auto uidx = static_cast<std::size_t>(idx);
    // Standard SWMM rain file grammar: `Fname Station Units`. The station
    // token selects rows in the file by its first column, so it belongs in
    // station_id (not the CSV column-name slot used by the "path:col" form).
    ctx.gages.file_path[uidx]   = path;
    ctx.gages.station_id[uidx]  = station_id ? station_id : "";
    ctx.gages.file_format[uidx] = openswmm::RainFileFormat::STAN_PRCP;
    ctx.gages.source[uidx]      = openswmm::RainSource::FILE_RAIN;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_set_station_id(SWMM_Engine engine, int idx, const char* station_id) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.gages.station_id[uidx]  = station_id ? station_id : "";
    ctx.gages.file_format[uidx] = openswmm::RainFileFormat::STAN_PRCP;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_set_snow_factor(SWMM_Engine engine, int idx, double factor) {
    CHECK_HANDLE(engine);
    if (factor <= 0.0) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    // No CHECK_GEOMETRY: the snow catch factor (SCF) is a scalar precipitation
    // multiplier, not geometry — settable mid-run for calibration/RTC, matching
    // swmm_gage_set_scale_factor and the subcatchment scale-factor setters.
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    ctx.gages.snow_factor[static_cast<std::size_t>(idx)] = factor;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_set_rain_units(SWMM_Engine engine, int idx, int units) {
    CHECK_HANDLE(engine);
    if (units < 0 || units > 1) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    ctx.gages.rain_units[static_cast<std::size_t>(idx)] = units;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_set_scale_factor(SWMM_Engine engine, int idx, double factor) {
    CHECK_HANDLE(engine);
    if (factor <= 0.0) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    ctx.gages.scale_factor[static_cast<std::size_t>(idx)] = factor;
    return SWMM_OK;
}

// ============================================================================
// Property getters
// ============================================================================

SWMM_ENGINE_API int swmm_gage_get_rain_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    if (type) *type = ctx.gages.rain_type[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_get_data_source(SWMM_Engine engine, int idx, int* source) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    if (source) *source = static_cast<int>(ctx.gages.source[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_get_scale_factor(SWMM_Engine engine, int idx, double* factor) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    if (factor) *factor = ctx.gages.scale_factor[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// Local NUL-terminated copy helper (mirrors fill_buf in openswmm_model_impl.cpp;
// kept local to avoid exporting a shared symbol across translation units).
static void gage_fill_buf(char* buf, int sz, const std::string& s) {
    if (!buf || sz <= 0) return;
    const std::size_t n = std::min(static_cast<std::size_t>(sz - 1), s.size());
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
}

SWMM_ENGINE_API int swmm_gage_get_rain_interval(SWMM_Engine engine, int idx, double* seconds) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    if (seconds)
        *seconds = static_cast<double>(ctx.gages.interval_sec[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_get_snow_factor(SWMM_Engine engine, int idx, double* factor) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    if (factor) *factor = ctx.gages.snow_factor[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_get_timeseries(SWMM_Engine engine, int idx, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    const int ts = ctx.gages.ts_index[static_cast<std::size_t>(idx)];
    std::string id;
    if (ts >= 0 && ts < ctx.n_tables())
        id = ctx.tables[ts].id;
    else
        id = ctx.gages.ts_name[static_cast<std::size_t>(idx)];  // unresolved name, if any
    gage_fill_buf(buf, buflen, id);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_get_station_id(SWMM_Engine engine, int idx, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    gage_fill_buf(buf, buflen, ctx.gages.station_id[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_get_rain_units(SWMM_Engine engine, int idx, int* units) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    if (units) *units = ctx.gages.rain_units[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// State
// ============================================================================

SWMM_ENGINE_API int swmm_gage_get_rainfall(SWMM_Engine engine, int idx, double* rainfall) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    if (rainfall) *rainfall = ctx.gages.rainfall[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_set_rainfall(SWMM_Engine engine, int idx, double rainfall) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    ctx.gages.rainfall[static_cast<std::size_t>(idx)] = rainfall;
    return SWMM_OK;
}

// ============================================================================
// Bulk access
// ============================================================================

SWMM_ENGINE_API int swmm_gage_get_rainfall_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_gages());
    std::copy(ctx.gages.rainfall.begin(), ctx.gages.rainfall.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_rename(SWMM_Engine engine, int idx, const char* newId) {
    CHECK_HANDLE(engine);
    if (!newId || newId[0] == '\0') return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    return ctx.gage_names.rename(idx, newId) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

} /* extern "C" */
