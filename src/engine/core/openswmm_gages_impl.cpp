/**
 * @file openswmm_gages_impl.cpp
 * @brief C API implementation — rain gage identity, creation, properties, state, bulk.
 *
 * @see include/openswmm/engine/openswmm_gages.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
    ctx.gages.resize(n);

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
    int ts_idx = ctx.table_names.find(ts_id);
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
    ctx.gages.file_path[uidx] = path;
    ctx.gages.col_name[uidx] = station_id ? station_id : "";
    ctx.gages.source[uidx] = openswmm::RainSource::FILE_RAIN;
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

} /* extern "C" */
