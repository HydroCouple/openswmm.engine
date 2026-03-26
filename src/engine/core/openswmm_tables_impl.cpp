/**
 * @file openswmm_tables_impl.cpp
 * @brief C API implementation — tables (time series, curves) and patterns.
 *
 * @see include/openswmm/engine/openswmm_tables.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_tables.h"

extern "C" {

// ============================================================================
// Identity
// ============================================================================

SWMM_ENGINE_API int swmm_table_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().n_tables();
}

SWMM_ENGINE_API int swmm_table_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    return to_engine(engine)->context().table_names.find(id);
}

SWMM_ENGINE_API const char* swmm_table_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& ctx = to_engine(engine)->context();
    if (idx < 0 || idx >= ctx.n_tables()) return nullptr;
    return ctx.table_names.name_of(idx).c_str();
}

// ============================================================================
// Creation (BUILDING state only)
// ============================================================================

SWMM_ENGINE_API int swmm_timeseries_add(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    if (ctx.state != openswmm::EngineState::BUILDING)
        return SWMM_ERR_LIFECYCLE;

    // Check for duplicate ID
    if (ctx.table_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    // Add to name index and table data
    ctx.table_names.add(id);
    ctx.tables.add(id, openswmm::TableType::TIMESERIES);

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_curve_add(SWMM_Engine engine, const char* id, int type) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    if (ctx.state != openswmm::EngineState::BUILDING)
        return SWMM_ERR_LIFECYCLE;

    // Check for duplicate ID
    if (ctx.table_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    // Add to name index and table data
    ctx.table_names.add(id);
    ctx.tables.add(id, static_cast<openswmm::TableType>(type));

    return SWMM_OK;
}

// ============================================================================
// Data points
// ============================================================================

SWMM_ENGINE_API int swmm_table_add_point(SWMM_Engine engine, int idx, double x, double y) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_tables());
    auto& tbl = ctx.tables[idx];
    tbl.x.push_back(x);
    tbl.y.push_back(y);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_table_get_point_count(SWMM_Engine engine, int idx, int* count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_tables());
    if (count) *count = static_cast<int>(ctx.tables[idx].size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_table_get_point(SWMM_Engine engine, int idx, int pt_idx, double* x, double* y) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_tables());
    const auto& tbl = ctx.tables[idx];
    CHECK_INDEX(pt_idx >= 0 && pt_idx < static_cast<int>(tbl.size()));
    const auto upt = static_cast<std::size_t>(pt_idx);
    if (x) *x = tbl.x[upt];
    if (y) *y = tbl.y[upt];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_table_clear(SWMM_Engine engine, int idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_tables());
    auto& tbl = ctx.tables[idx];
    tbl.x.clear();
    tbl.y.clear();
    tbl.cursor.reset();
    return SWMM_OK;
}

// ============================================================================
// Lookup (cursor-optimized)
// ============================================================================

SWMM_ENGINE_API int swmm_table_lookup(SWMM_Engine engine, int idx, double x, double* y) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_tables());
    if (!y) return SWMM_ERR_BADPARAM;
    *y = openswmm::table_lookup_cursor(ctx.tables[idx], x);
    return SWMM_OK;
}

// ============================================================================
// Patterns
// ============================================================================

SWMM_ENGINE_API int swmm_pattern_add(SWMM_Engine engine, const char* id, int type) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    if (ctx.state != openswmm::EngineState::BUILDING)
        return SWMM_ERR_LIFECYCLE;

    ctx.patterns.add(id, type, {});
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pattern_set_factors(SWMM_Engine engine, int idx, const double* factors, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.patterns.count());
    if (!factors || count <= 0) return SWMM_ERR_BADPARAM;
    ctx.patterns.factors[static_cast<std::size_t>(idx)].assign(factors, factors + count);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pattern_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().patterns.count();
}

} /* extern "C" */
