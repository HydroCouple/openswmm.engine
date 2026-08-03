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
#include "../edit/VirtualJunctionOps.hpp"
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

// Unit-hydrograph groups are name-keyed; a group "exists" if any parameter
// line or gage assignment carries the name.
static bool uh_group_exists(const openswmm::SimulationContext& ctx,
                            const char* uh_name) {
    if (!uh_name || !*uh_name) return false;
    const std::string name(uh_name);
    for (const auto& e : ctx.unit_hyds.entries)
        if (e.name == name) return true;
    for (const auto& a : ctx.unit_hyds.gage_assignments)
        if (a == name) return true;
    return false;
}

SWMM_ENGINE_API int swmm_pollutant_analyze_impact(SWMM_Engine engine, int idx,
                                                   SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    if (report_out)
        cascade_to_c(openswmm::edit::analyze_pollutant_impact(ctx, idx), report_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pattern_analyze_impact(SWMM_Engine engine, int idx,
                                                 SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.patterns.count());
    if (report_out)
        cascade_to_c(openswmm::edit::analyze_pattern_impact(ctx, idx), report_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_aquifer_analyze_impact(SWMM_Engine engine, int idx,
                                                 SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.aquifers.count());
    if (report_out)
        cascade_to_c(openswmm::edit::analyze_aquifer_impact(ctx, idx), report_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_snowpack_analyze_impact(SWMM_Engine engine, int idx,
                                                  SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.snowpacks.count());
    if (report_out)
        cascade_to_c(openswmm::edit::analyze_snowpack_impact(ctx, idx), report_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_analyze_impact(SWMM_Engine engine, int idx,
                                             SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.lid_controls.count());
    if (report_out)
        cascade_to_c(openswmm::edit::analyze_lid_impact(ctx, idx), report_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_street_analyze_impact(SWMM_Engine engine, int idx,
                                                SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.streets.count());
    if (report_out)
        cascade_to_c(openswmm::edit::analyze_street_impact(ctx, idx), report_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_inlet_analyze_impact(SWMM_Engine engine, int idx,
                                               SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.inlets.count());
    if (report_out)
        cascade_to_c(openswmm::edit::analyze_inlet_impact(ctx, idx), report_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_landuse_analyze_impact(SWMM_Engine engine, int idx,
                                                 SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_landuses());
    if (report_out)
        cascade_to_c(openswmm::edit::analyze_landuse_impact(ctx, idx), report_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_analyze_impact(SWMM_Engine engine,
                                                    const char* uh_name,
                                                    SWMM_ImpactReport* report_out) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_READABLE(ctx);
    if (!uh_group_exists(ctx, uh_name)) return SWMM_ERR_BADPARAM;
    if (report_out)
        cascade_to_c(openswmm::edit::analyze_hydrograph_impact(ctx, uh_name), report_out);
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
    // Relational refactor (Phase 4): delete_node erases the node's subtype row and
    // renumbers the side-table join keys in-place (ctx.node_subtypes is authoritative).
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

SWMM_ENGINE_API int swmm_pollutant_delete(SWMM_Engine engine, int idx,
                                           SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_pollutants());
    cascade_to_c(openswmm::edit::delete_pollutant(ctx, idx), cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_pattern_delete(SWMM_Engine engine, int idx,
                                         SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.patterns.count());
    cascade_to_c(openswmm::edit::delete_pattern(ctx, idx), cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_aquifer_delete(SWMM_Engine engine, int idx,
                                         SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.aquifers.count());
    cascade_to_c(openswmm::edit::delete_aquifer(ctx, idx), cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_snowpack_delete(SWMM_Engine engine, int idx,
                                          SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.snowpacks.count());
    cascade_to_c(openswmm::edit::delete_snowpack(ctx, idx), cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_delete(SWMM_Engine engine, int idx,
                                     SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.lid_controls.count());
    cascade_to_c(openswmm::edit::delete_lid(ctx, idx), cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_street_delete(SWMM_Engine engine, int idx,
                                        SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.streets.count());
    cascade_to_c(openswmm::edit::delete_street(ctx, idx), cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_inlet_delete(SWMM_Engine engine, int idx,
                                       SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.inlets.count());
    cascade_to_c(openswmm::edit::delete_inlet(ctx, idx), cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_landuse_delete(SWMM_Engine engine, int idx,
                                         SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_landuses());
    cascade_to_c(openswmm::edit::delete_landuse(ctx, idx), cascade_out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_hydrograph_delete(SWMM_Engine engine, const char* uh_name,
                                            SWMM_ImpactReport* cascade_out) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (!uh_group_exists(ctx, uh_name)) return SWMM_ERR_BADPARAM;
    cascade_to_c(openswmm::edit::delete_hydrograph(ctx, uh_name), cascade_out);
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

    // Relational refactor (Phase 4): convert_node moves the node's subtype row
    // (set_node_type) so ctx.node_subtypes stays the single source of truth.
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

// ============================================================================
// Conduit split / virtual-junction fusion
// ============================================================================

SWMM_ENGINE_API int swmm_conduit_split(SWMM_Engine engine, int link_idx, double t,
                                       const char* new_node_name,
                                       const char* new_link_name,
                                       int make_virtual,
                                       int* new_node_idx, int* new_link_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(link_idx >= 0 && link_idx < ctx.n_links());
    if (!new_node_name || !new_link_name) return SWMM_ERR_BADPARAM;

    const auto res = openswmm::edit::vj_split_conduit(
        ctx, link_idx, t, new_node_name, new_link_name, make_virtual != 0);
    if (res.new_node_idx < 0)
        return SWMM_ERR_BADPARAM;   // split rejected (bad t / names / type)
    if (new_node_idx) *new_node_idx = res.new_node_idx;
    if (new_link_idx) *new_link_idx = res.new_link_idx;
    // Split succeeded; a nonzero err here is a make_virtual rule code — the
    // node stands as a regular junction and the caller decides what to do.
    return (res.err == 0) ? SWMM_OK : res.err;
}

SWMM_ENGINE_API int swmm_virtual_junction_fuse(SWMM_Engine engine, int node_idx,
                                               int* surviving_link_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());

    int surviving = -1;
    const int code = openswmm::edit::vj_fuse(ctx, node_idx, &surviving);
    if (code == -1) return SWMM_ERR_BADPARAM;   // not a virtual junction
    if (code != 0)  return code;                // ERR_VJ_LINK_COUNT
    if (surviving_link_idx) *surviving_link_idx = surviving;
    return SWMM_OK;
}

} // extern "C"
