/**
 * @file openswmm_edit_impl.cpp
 * @brief C API implementation — object deletion and type conversion.
 *
 * @see include/openswmm/engine/openswmm_edit.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "TypeHelpers.hpp"
#include "../edit/ObjectDeleter.hpp"
#include "../edit/TypeConverter.hpp"
#include "../../../include/openswmm/engine/openswmm_edit.h"

#include <cstring>
#include <cstdlib>

// ============================================================================
// Internal helpers — convert C++ results to C structs
// ============================================================================

static void cascade_to_c(const openswmm::edit::CascadeResult& res,
                          SWMM_ImpactReport* out) {
    if (!out) return;
    out->n_entries = static_cast<int>(res.entries.size());
    if (out->n_entries == 0) { out->entries = nullptr; return; }
    out->entries = new SWMM_ImpactEntry[static_cast<std::size_t>(out->n_entries)];
    for (int i = 0; i < out->n_entries; ++i) {
        const auto& e = res.entries[static_cast<std::size_t>(i)];
        out->entries[i].obj_type  = e.obj_type;
        out->entries[i].obj_idx   = e.obj_idx;
        out->entries[i].field     = e.field;   // static literal — safe
        out->entries[i].cascaded  = e.cascaded ? 1 : 0;
    }
}

static void conversion_to_c(const openswmm::edit::ConversionResult& res,
                             SWMM_ConversionResult* out) {
    if (!out) return;
    out->new_type = res.new_type;

    out->n_cleared = static_cast<int>(res.cleared_fields.size());
    if (out->n_cleared > 0) {
        out->cleared_fields = new const char*[static_cast<std::size_t>(out->n_cleared)];
        for (int i = 0; i < out->n_cleared; ++i)
            out->cleared_fields[i] = strdup(res.cleared_fields[static_cast<std::size_t>(i)].c_str());
    } else {
        out->cleared_fields = nullptr;
    }

    out->n_warnings = static_cast<int>(res.warnings.size());
    if (out->n_warnings > 0) {
        out->warnings = new const char*[static_cast<std::size_t>(out->n_warnings)];
        for (int i = 0; i < out->n_warnings; ++i)
            out->warnings[i] = strdup(res.warnings[static_cast<std::size_t>(i)].c_str());
    } else {
        out->warnings = nullptr;
    }
}

// ============================================================================
// extern "C" API
// ============================================================================

extern "C" {

// -------------------------------------------------------------------------
// Free functions
// -------------------------------------------------------------------------

SWMM_ENGINE_API void swmm_impact_report_free(SWMM_ImpactReport* report) {
    if (!report) return;
    delete[] report->entries;
    report->entries  = nullptr;
    report->n_entries = 0;
}

SWMM_ENGINE_API void swmm_conversion_result_free(SWMM_ConversionResult* result) {
    if (!result) return;
    for (int i = 0; i < result->n_cleared; ++i)
        free(const_cast<char*>(result->cleared_fields[i]));
    delete[] result->cleared_fields;
    result->cleared_fields = nullptr;
    result->n_cleared = 0;

    for (int i = 0; i < result->n_warnings; ++i)
        free(const_cast<char*>(result->warnings[i]));
    delete[] result->warnings;
    result->warnings  = nullptr;
    result->n_warnings = 0;
}

// -------------------------------------------------------------------------
// Impact analysis — read-only (CHECK_READABLE)
// -------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_node_analyze_impact(SWMM_Engine engine, int idx,
                                              SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (report_out) {
        auto res = openswmm::edit::analyze_node_impact(ctx, idx);
        cascade_to_c(res, report_out);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_analyze_impact(SWMM_Engine engine, int idx,
                                              SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (report_out) {
        auto res = openswmm::edit::analyze_link_impact(ctx, idx);
        cascade_to_c(res, report_out);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_analyze_impact(SWMM_Engine engine, int idx,
                                                  SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (report_out) {
        auto res = openswmm::edit::analyze_subcatch_impact(ctx, idx);
        cascade_to_c(res, report_out);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_analyze_impact(SWMM_Engine engine, int idx,
                                              SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    if (report_out) {
        auto res = openswmm::edit::analyze_gage_impact(ctx, idx);
        cascade_to_c(res, report_out);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_table_analyze_impact(SWMM_Engine engine, int idx,
                                               SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_tables());
    if (report_out) {
        auto res = openswmm::edit::analyze_table_impact(ctx, idx);
        cascade_to_c(res, report_out);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_analyze_impact(SWMM_Engine engine, int idx,
                                                   SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.transects.count());
    if (report_out) {
        auto res = openswmm::edit::analyze_transect_impact(ctx, idx);
        cascade_to_c(res, report_out);
    }
    return SWMM_OK;
}

// -------------------------------------------------------------------------
// Deletion — mutating (CHECK_EDITABLE)
// -------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_node_delete(SWMM_Engine engine, int idx,
                                      SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    auto res = openswmm::edit::delete_node(ctx, idx);
    cascade_to_c(res, cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_delete(SWMM_Engine engine, int idx,
                                      SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    auto res = openswmm::edit::delete_link(ctx, idx);
    cascade_to_c(res, cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_delete(SWMM_Engine engine, int idx,
                                          SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto res = openswmm::edit::delete_subcatch(ctx, idx);
    cascade_to_c(res, cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gage_delete(SWMM_Engine engine, int idx,
                                      SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    auto res = openswmm::edit::delete_gage(ctx, idx);
    cascade_to_c(res, cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_table_delete(SWMM_Engine engine, int idx,
                                       SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_tables());
    auto res = openswmm::edit::delete_table(ctx, idx);
    cascade_to_c(res, cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_delete(SWMM_Engine engine, int idx,
                                          SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.transects.count());
    auto res = openswmm::edit::delete_transect(ctx, idx);
    cascade_to_c(res, cascade_out);
    return SWMM_OK;
}

// -------------------------------------------------------------------------
// Type conversion — mutating (CHECK_EDITABLE)
// -------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_node_convert(SWMM_Engine engine, int idx, int new_type,
                                       SWMM_ConversionResult* result_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());

    openswmm::NodeType internal_new;
    if (!openswmm::c_to_internal_node_type(new_type, internal_new))
        return SWMM_ERR_BADPARAM;

    const auto ui = static_cast<std::size_t>(idx);
    if (ctx.nodes.type[ui] == internal_new)
        return SWMM_ERR_BADPARAM;  // same type — no-op

    auto res = openswmm::edit::convert_node(ctx, idx, internal_new);
    conversion_to_c(res, result_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_convert(SWMM_Engine engine, int idx, int new_type,
                                       SWMM_ConversionResult* result_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());

    openswmm::LinkType internal_new;
    if (!openswmm::c_to_internal_link_type(new_type, internal_new))
        return SWMM_ERR_BADPARAM;

    const auto ui = static_cast<std::size_t>(idx);
    if (ctx.links.type[ui] == internal_new)
        return SWMM_ERR_BADPARAM;  // same type — no-op

    auto res = openswmm::edit::convert_link(ctx, idx, internal_new);
    conversion_to_c(res, result_out);
    return SWMM_OK;
}

} // extern "C"
