/**
 * @file openswmm_tables_impl.cpp
 * @brief C API implementation — tables (time series, curves) and patterns.
 *
 * @see include/openswmm/engine/openswmm_tables.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../edit/ObjectDeleter.hpp"
#include "../../../include/openswmm/engine/openswmm_tables.h"

namespace {

// Visit every pattern-name reference site with @p fn(std::string& slot).
// Centralised so remove (clear matching) and rename (rewrite matching)
// share one walk and stay in sync with the data layout.
template <class F>
void for_each_pattern_name_ref(openswmm::SimulationContext& ctx, F&& fn) {
    for (auto& s : ctx.ext_inflows.pattern_name)  fn(s);
    for (auto& s : ctx.dwf_inflows.pat1)           fn(s);
    for (auto& s : ctx.dwf_inflows.pat2)           fn(s);
    for (auto& s : ctx.dwf_inflows.pat3)           fn(s);
    for (auto& s : ctx.dwf_inflows.pat4)           fn(s);
    for (auto& s : ctx.aquifers.upper_evap_pat)    fn(s);
    fn(ctx.options.evap_recovery_pat);
}

} // namespace

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

SWMM_ENGINE_API int swmm_table_get_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_tables());
    if (!type) return SWMM_ERR_BADPARAM;
    *type = static_cast<int>(ctx.tables[idx].type);
    return SWMM_OK;
}

// ============================================================================
// Creation (BUILDING or OPENED — table refs resolve by name at initialize())
// ============================================================================

SWMM_ENGINE_API int swmm_timeseries_add(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);

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
    CHECK_EDITABLE(ctx);

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
    CHECK_EDITABLE(ctx);

    ctx.patterns.add(id, type, {});
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pattern_set_factors(SWMM_Engine engine, int idx, const double* factors, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.patterns.count());
    if (!factors || count <= 0) return SWMM_ERR_BADPARAM;
    ctx.patterns.factors[static_cast<std::size_t>(idx)].assign(factors, factors + count);
    // Propagate the edit to the inflow solver's per-step pattern cache so a
    // mid-run change takes effect on the next step (DWF/external inflow read
    // a cached copy; groundwater-evap patterns already read ctx.patterns live).
    to_engine(engine)->inflowSolver().refreshPatterns(ctx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pattern_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().patterns.count();
}

SWMM_ENGINE_API int swmm_pattern_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    const auto& names = to_engine(engine)->context().patterns.names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == id) return static_cast<int>(i);
    }
    return -1;
}

SWMM_ENGINE_API const char* swmm_pattern_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& names = to_engine(engine)->context().patterns.names;
    if (idx < 0 || idx >= static_cast<int>(names.size())) return nullptr;
    return names[static_cast<std::size_t>(idx)].c_str();
}

SWMM_ENGINE_API int swmm_pattern_get_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.patterns.count());
    if (!type) return SWMM_ERR_BADPARAM;
    *type = ctx.patterns.types[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pattern_get_factor_count(SWMM_Engine engine, int idx, int* count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.patterns.count());
    if (!count) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(ctx.patterns.factors[static_cast<std::size_t>(idx)].size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pattern_get_factor(SWMM_Engine engine, int idx, int i, double* v) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.patterns.count());
    const auto& f = ctx.patterns.factors[static_cast<std::size_t>(idx)];
    CHECK_INDEX(i >= 0 && i < static_cast<int>(f.size()));
    if (!v) return SWMM_ERR_BADPARAM;
    *v = f[static_cast<std::size_t>(i)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pattern_remove(SWMM_Engine engine, int idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    // Idempotent: silently ignore stale indices so the GUI's "delete then
    // delete again on a re-resolved-too-late button" path is safe.
    if (idx < 0 || idx >= ctx.patterns.count()) return SWMM_OK;

    // Same code path as swmm_pattern_delete, minus the impact report.
    openswmm::edit::delete_pattern(ctx, idx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pattern_rename(SWMM_Engine engine, int idx, const char* newId) {
    CHECK_HANDLE(engine);
    if (!newId || newId[0] == '\0') return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.patterns.count());

    const std::string next = newId;
    const auto u = static_cast<std::size_t>(idx);
    // Renaming to the same name is a no-op (also avoids a false collision).
    if (ctx.patterns.names[u] == next) return SWMM_OK;

    for (std::size_t j = 0; j < ctx.patterns.names.size(); ++j) {
        if (j != u && ctx.patterns.names[j] == next) return SWMM_ERR_BADPARAM;
    }

    const std::string prev = ctx.patterns.names[u];
    ctx.patterns.names[u] = next;

    for_each_pattern_name_ref(ctx, [&](std::string& slot) {
        if (slot == prev) slot = next;
    });
    return SWMM_OK;
}

} /* extern "C" */
