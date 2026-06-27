/**
 * @file openswmm_nodes_impl.cpp
 * @brief C API implementation — node identity, creation, properties, state, bulk.
 *
 * @see include/openswmm/engine/openswmm_nodes.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "TypeHelpers.hpp"
#include "../../../include/openswmm/engine/openswmm_nodes.h"
#include "../hydraulics/Node.hpp"

#include <algorithm>
#include <cstring>
#include <string>

using openswmm::c_to_internal_node_type;
using openswmm::internal_to_c_node_type;

extern "C" {

// ============================================================================
// Identity
// ============================================================================

SWMM_ENGINE_API int swmm_node_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().n_nodes();
}

SWMM_ENGINE_API int swmm_node_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    return to_engine(engine)->context().node_names.find(id);
}

SWMM_ENGINE_API const char* swmm_node_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& ctx = to_engine(engine)->context();
    if (idx < 0 || idx >= ctx.n_nodes()) return nullptr;
    return ctx.node_names.name_of(idx).c_str();
}

// ============================================================================
// Creation (BUILDING or OPENED — "editable" states)
// ============================================================================
// Nodes may be appended in either BUILDING (programmatic construction) or
// OPENED (interactive editing after the .inp has been parsed). The engine's
// per-index SoA is pure append-resize plus value assignment; no solver state
// depends on node count being frozen until swmm_engine_initialize runs.

SWMM_ENGINE_API int swmm_node_add(SWMM_Engine engine, const char* id, int type) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    openswmm::NodeType internal_type = openswmm::NodeType::JUNCTION;
    if (!c_to_internal_node_type(type, internal_type))
        return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);

    // Check for duplicate ID
    if (ctx.node_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    // Add to name index (assigns next sequential index)
    int idx = ctx.node_names.add(id);

    // Grow SoA and spatial arrays to accommodate new node (preserving existing data)
    int n = ctx.node_names.size();
    ctx.nodes.grow_to(n);
    const auto un = static_cast<std::size_t>(n);
    if (ctx.spatial.node_x.size() < un) ctx.spatial.node_x.resize(un, 0.0);
    if (ctx.spatial.node_y.size() < un) ctx.spatial.node_y.resize(un, 0.0);

    // Set type and create the subtype side-table row (single source of truth).
    ctx.node_subtypes.set_node_type(ctx.nodes, idx, internal_type);

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_pop_last(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);

    const int n = ctx.node_names.size();
    if (n <= 0) return SWMM_ERR_BADINDEX;

    const int tail = n - 1;
    if (ctx.node_names.name_of(tail) != id)
        return SWMM_ERR_BADINDEX;

    // Guard against dangling link references. The GUI must cascade-remove
    // any link whose from/to endpoint is the tail node before popping it.
    for (int i = 0; i < ctx.n_links(); ++i) {
        if (ctx.links.node1[static_cast<std::size_t>(i)] == tail ||
            ctx.links.node2[static_cast<std::size_t>(i)] == tail) {
            return SWMM_ERR_BADPARAM;
        }
    }

    ctx.node_names.pop_back();
    ctx.nodes.erase_at(tail);
    ctx.node_subtypes.erase_node(tail, ctx.nodes.count());
    // Shrink spatial arrays to match reduced node count
    if (!ctx.spatial.node_x.empty()) ctx.spatial.node_x.pop_back();
    if (!ctx.spatial.node_y.empty()) ctx.spatial.node_y.pop_back();
    return SWMM_OK;
}

// ============================================================================
// Geometry setters (BUILDING or OPENED only)
// ============================================================================

SWMM_ENGINE_API int swmm_node_set_invert_elev(SWMM_Engine engine, int idx, double elev) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.invert_elev[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::LENGTH, elev); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_max_depth(SWMM_Engine engine, int idx, double depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.full_depth[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::LENGTH, depth); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_surcharge_depth(SWMM_Engine engine, int idx, double depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.sur_depth[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::LENGTH, depth); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_pond_area(SWMM_Engine engine, int idx, double area) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    // units: ponded area = LENGTH^2 (ft2/m2)
    double fi = openswmm::ucf::UCF_inv(openswmm::ucf::LENGTH, ctx.options);
    ctx.nodes.ponded_area[static_cast<std::size_t>(idx)] = area * fi * fi;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_initial_depth(SWMM_Engine engine, int idx, double depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INITIAL_COND(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.depth[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::LENGTH, depth); // units
    return SWMM_OK;
}

// ============================================================================
// Geometry getters
// ============================================================================

SWMM_ENGINE_API int swmm_node_get_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (type) *type = internal_to_c_node_type(ctx.nodes.type[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_invert_elev(SWMM_Engine engine, int idx, double* elev) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (elev) *elev = to_display(ctx, openswmm::ucf::LENGTH, ctx.nodes.invert_elev[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_max_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (depth) *depth = to_display(ctx, openswmm::ucf::LENGTH, ctx.nodes.full_depth[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

// ============================================================================
// Hydraulic state getters/setters
// ============================================================================

SWMM_ENGINE_API int swmm_node_get_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (depth) *depth = to_display(ctx, openswmm::ucf::LENGTH, ctx.nodes.depth[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_depth(SWMM_Engine engine, int idx, double depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.depth[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::LENGTH, depth); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_head(SWMM_Engine engine, int idx, double* head) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (head) *head = to_display(ctx, openswmm::ucf::LENGTH, ctx.nodes.head[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_volume(SWMM_Engine engine, int idx, double* volume) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (volume) *volume = to_display(ctx, openswmm::ucf::VOLUME, ctx.nodes.volume[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_lateral_inflow(SWMM_Engine engine, int idx, double* inflow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (inflow) *inflow = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.lat_flow[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_overflow(SWMM_Engine engine, int idx, double* overflow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (overflow) *overflow = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.overflow[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_inflow(SWMM_Engine engine, int idx, double* inflow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    // Total inflow = lateral + upstream — upstream computed during routing
    if (inflow) *inflow = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.lat_flow[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

// ============================================================================
// Runtime forcing (RUNNING state only)
// ============================================================================

SWMM_ENGINE_API int swmm_node_set_lateral_inflow(SWMM_Engine engine, int idx, double flow) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    auto uidx = static_cast<std::size_t>(idx);
    if (uidx >= ctx.nodes.user_lat_flow.size()) {
        // Lazily resize if not yet allocated (e.g. hot-started context)
        ctx.nodes.user_lat_flow.resize(ctx.nodes.lat_flow.size(), 0.0);
    }
    ctx.nodes.user_lat_flow[uidx] = to_internal(ctx, openswmm::ucf::FLOW, flow); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_quality_mass_flux(SWMM_Engine engine, int node_idx,
                                                     int pollutant_idx, double mass_rate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    auto flat = static_cast<std::size_t>(node_idx) * static_cast<std::size_t>(np)
              + static_cast<std::size_t>(pollutant_idx);
    if (flat >= ctx.nodes.user_conc_mass_flux.size()) {
        // Lazily resize if not yet allocated
        ctx.nodes.user_conc_mass_flux.resize(
            static_cast<std::size_t>(ctx.n_nodes()) * static_cast<std::size_t>(np), 0.0);
    }
    ctx.nodes.user_conc_mass_flux[flat] = mass_rate;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_head_boundary(SWMM_Engine engine, int idx, double head) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    auto uidx = static_cast<std::size_t>(idx);
    if (ctx.nodes.type[uidx] != openswmm::NodeType::OUTFALL)
        return SWMM_ERR_BADPARAM;
    const int r = ctx.node_subtypes.outfall_row(idx);
    if (r >= 0) {
        const auto ur = static_cast<std::size_t>(r);
        ctx.node_subtypes.outfalls.param[ur] = to_internal(ctx, openswmm::ucf::LENGTH, head); // units
        ctx.node_subtypes.outfalls.bc_type[ur] = openswmm::OutfallType::FIXED;
    }
    return SWMM_OK;
}

// ============================================================================
// Water quality (Phase 8)
// ============================================================================

SWMM_ENGINE_API int swmm_node_get_quality(SWMM_Engine engine, int node_idx,
                                           int pollutant_idx, double* conc) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    if (conc) *conc = ctx.nodes.conc[
        static_cast<std::size_t>(node_idx) * static_cast<std::size_t>(np) +
        static_cast<std::size_t>(pollutant_idx)];
    return SWMM_OK;
}

// ============================================================================
// Bulk access
// ============================================================================

SWMM_ENGINE_API int swmm_node_get_depths_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::LENGTH, ctx.nodes.depth[static_cast<std::size_t>(i)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_heads_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::LENGTH, ctx.nodes.head[static_cast<std::size_t>(i)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_inflows_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.lat_flow[static_cast<std::size_t>(i)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_overflows_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.overflow[static_cast<std::size_t>(i)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_depths_bulk(SWMM_Engine engine, const double* buf, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i)
        ctx.nodes.depth[static_cast<std::size_t>(i)] = to_internal(ctx, openswmm::ucf::LENGTH, buf[i]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_lat_inflows_bulk(SWMM_Engine engine, const double* buf, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i)
        ctx.nodes.lat_flow[static_cast<std::size_t>(i)] = to_internal(ctx, openswmm::ucf::FLOW, buf[i]); // units
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// Phase 3 bulk getters — volumes, outflows, losses, lateral_inflows, ids.
// Each follows the same "memcpy into caller's buffer" pattern as the bulk
// getters above. Non-pump links / non-existent state is not a concern here
// because every node maintains all of these fields after initialization.
//
// Naming note: `swmm_node_get_lateral_inflows_bulk` reads the same `lat_flow`
// SoA column as the pre-existing `swmm_node_get_inflows_bulk`; the older
// name was labelled "total inflows" in error and is retained for backward
// compatibility. New callers should prefer the explicitly-named variant.
// See docs/C_API_BINDINGS_MCP_IMPROVEMENT_PLAN.md Appendix A item 3.
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_node_get_volumes_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::VOLUME, ctx.nodes.volume[static_cast<std::size_t>(i)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outflows_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.outflow[static_cast<std::size_t>(i)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_losses_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.losses[static_cast<std::size_t>(i)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_lateral_inflows_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.lat_flow[static_cast<std::size_t>(i)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_ids_bulk(SWMM_Engine engine,
                                            char* buf,
                                            int stride,
                                            int count) {
    CHECK_HANDLE(engine);
    if (!buf || stride < 2 || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_nodes());
    const std::size_t s = static_cast<std::size_t>(stride);

    // Zero the requested region up front; that way any IDs shorter than
    // stride are NUL-terminated without an explicit per-slot write, and a
    // partial read leaves a clean tail.
    std::fill_n(buf, s * static_cast<std::size_t>(n), '\0');

    for (int i = 0; i < n; ++i) {
        const std::string& name = ctx.node_names.name_of(i);
        // Truncate (never overflow): leave the last byte of each slot as
        // the NUL terminator.
        const std::size_t copy_n =
            std::min(name.size(), s - 1);
        std::memcpy(buf + static_cast<std::size_t>(i) * s,
                    name.data(), copy_n);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_quality_bulk(SWMM_Engine engine, int pollutant_idx,
                                                double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    const int n = std::min(count, ctx.n_nodes());
    for (int i = 0; i < n; ++i) {
        buf[i] = ctx.nodes.conc[
            static_cast<std::size_t>(i) * static_cast<std::size_t>(np) +
            static_cast<std::size_t>(pollutant_idx)];
    }
    return SWMM_OK;
}

// ============================================================================
// Storage Node API
// ============================================================================

SWMM_ENGINE_API int swmm_node_set_storage_curve(SWMM_Engine engine, int idx, int curve_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.storage_row(idx);
    if (r >= 0) ctx.node_subtypes.storages.curve[static_cast<std::size_t>(r)] = curve_idx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_storage_curve(SWMM_Engine engine, int idx, int* curve_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.storage_row(idx);
    if (curve_idx)
        *curve_idx = (r >= 0) ? ctx.node_subtypes.storages.curve[static_cast<std::size_t>(r)] : -1;
    return SWMM_OK;
}

// TODO(units): storage/exfil rate-unit conversion unverified
SWMM_ENGINE_API int swmm_node_set_storage_functional(SWMM_Engine engine, int idx, double a, double b, double c) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.storage_row(idx);
    if (r >= 0) {
        const auto ur = static_cast<std::size_t>(r);
        ctx.node_subtypes.storages.a[ur] = a;
        ctx.node_subtypes.storages.b[ur] = b;
        ctx.node_subtypes.storages.c[ur] = c;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_storage_functional(SWMM_Engine engine, int idx, double* a, double* b, double* c) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.storage_row(idx);
    if (r >= 0) {
        const auto ur = static_cast<std::size_t>(r);
        if (a) *a = ctx.node_subtypes.storages.a[ur];
        if (b) *b = ctx.node_subtypes.storages.b[ur];
        if (c) *c = ctx.node_subtypes.storages.c[ur];
    } else {
        if (a) *a = 0.0;
        if (b) *b = 0.0;
        if (c) *c = 0.0;
    }
    return SWMM_OK;
}

// TODO(units): storage/exfil rate-unit conversion unverified
SWMM_ENGINE_API int swmm_node_set_storage_seep_rate(SWMM_Engine engine, int idx, double rate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.storage_row(idx);
    if (r >= 0) ctx.node_subtypes.storages.seep_rate[static_cast<std::size_t>(r)] = rate;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_storage_seep_rate(SWMM_Engine engine, int idx, double* rate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.storage_row(idx);
    if (rate) *rate = (r >= 0) ? ctx.node_subtypes.storages.seep_rate[static_cast<std::size_t>(r)] : 0.0;
    return SWMM_OK;
}

// TODO(units): storage/exfil rate-unit conversion unverified
SWMM_ENGINE_API int swmm_node_set_exfil_params(SWMM_Engine engine, int idx, double suction, double ksat, double imd) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.storage_row(idx);
    if (r >= 0) {
        const auto ur = static_cast<std::size_t>(r);
        ctx.node_subtypes.storages.exfil_suction[ur] = suction;
        ctx.node_subtypes.storages.exfil_ksat[ur]    = ksat;
        ctx.node_subtypes.storages.exfil_imd[ur]     = imd;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_exfil_params(SWMM_Engine engine, int idx, double* suction, double* ksat, double* imd) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.storage_row(idx);
    if (r >= 0) {
        const auto ur = static_cast<std::size_t>(r);
        if (suction) *suction = ctx.node_subtypes.storages.exfil_suction[ur];
        if (ksat)    *ksat    = ctx.node_subtypes.storages.exfil_ksat[ur];
        if (imd)     *imd     = ctx.node_subtypes.storages.exfil_imd[ur];
    } else {
        if (suction) *suction = 0.0;
        if (ksat)    *ksat    = 0.0;
        if (imd)     *imd     = 0.0;
    }
    return SWMM_OK;
}

// ============================================================================
// Outfall Node API
// ============================================================================

SWMM_ENGINE_API int swmm_node_set_outfall_type(SWMM_Engine engine, int idx, int type) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.outfall_row(idx);
    if (r >= 0) ctx.node_subtypes.outfalls.bc_type[static_cast<std::size_t>(r)] =
                    static_cast<openswmm::OutfallType>(type);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outfall_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.outfall_row(idx);
    if (type) *type = (r >= 0)
        ? static_cast<int>(ctx.node_subtypes.outfalls.bc_type[static_cast<std::size_t>(r)])
        : static_cast<int>(openswmm::OutfallType::FREE);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_outfall_stage(SWMM_Engine engine, int idx, double stage) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const double sv = to_internal(ctx, openswmm::ucf::LENGTH, stage); // units
    const int r = ctx.node_subtypes.outfall_row(idx);
    if (r >= 0) {
        const auto ur = static_cast<std::size_t>(r);
        ctx.node_subtypes.outfalls.param[ur]   = sv;
        ctx.node_subtypes.outfalls.bc_type[ur] = openswmm::OutfallType::FIXED;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_outfall_tidal(SWMM_Engine engine, int idx, int curve_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.outfall_row(idx);
    if (r >= 0) {
        const auto ur = static_cast<std::size_t>(r);
        ctx.node_subtypes.outfalls.param[ur]   = static_cast<double>(curve_idx);
        ctx.node_subtypes.outfalls.bc_type[ur] = openswmm::OutfallType::TIDAL;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_outfall_timeseries(SWMM_Engine engine, int idx, int ts_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.outfall_row(idx);
    if (r >= 0) {
        const auto ur = static_cast<std::size_t>(r);
        ctx.node_subtypes.outfalls.param[ur]   = static_cast<double>(ts_idx);
        ctx.node_subtypes.outfalls.bc_type[ur] = openswmm::OutfallType::TIMESERIES;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outfall_param(SWMM_Engine engine, int idx, double* param) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.outfall_row(idx);
    const auto otype = (r >= 0) ? ctx.node_subtypes.outfalls.bc_type[static_cast<std::size_t>(r)]
                                : openswmm::OutfallType::FREE;
    const double opar = (r >= 0) ? ctx.node_subtypes.outfalls.param[static_cast<std::size_t>(r)]
                                 : 0.0;
    if (param) {
        // units: param holds a LENGTH (stage) only when type==FIXED; for
        // TIDAL/TIMESERIES it is a raw curve/timeseries index — never convert.
        if (otype == openswmm::OutfallType::FIXED)
            *param = to_display(ctx, openswmm::ucf::LENGTH, opar); // units
        else
            *param = opar;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outfall_tidal(SWMM_Engine engine, int idx, int* curve_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (!curve_idx) return SWMM_ERR_BADPARAM;
    const int r = ctx.node_subtypes.outfall_row(idx);
    const auto otype = (r >= 0) ? ctx.node_subtypes.outfalls.bc_type[static_cast<std::size_t>(r)]
                                : openswmm::OutfallType::FREE;
    if (otype != openswmm::OutfallType::TIDAL)
        return SWMM_ERR_BADPARAM;
    *curve_idx = static_cast<int>(ctx.node_subtypes.outfalls.param[static_cast<std::size_t>(r)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outfall_timeseries(SWMM_Engine engine, int idx, int* ts_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (!ts_idx) return SWMM_ERR_BADPARAM;
    const int r = ctx.node_subtypes.outfall_row(idx);
    const auto otype = (r >= 0) ? ctx.node_subtypes.outfalls.bc_type[static_cast<std::size_t>(r)]
                                : openswmm::OutfallType::FREE;
    if (otype != openswmm::OutfallType::TIMESERIES)
        return SWMM_ERR_BADPARAM;
    *ts_idx = static_cast<int>(ctx.node_subtypes.outfalls.param[static_cast<std::size_t>(r)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_outfall_flap_gate(SWMM_Engine engine, int idx, int has_gate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.outfall_row(idx);
    if (r >= 0) ctx.node_subtypes.outfalls.has_flap_gate[static_cast<std::size_t>(r)] = (has_gate != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outfall_flap_gate(SWMM_Engine engine, int idx, int* has_gate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.outfall_row(idx);
    const int hf = (r >= 0) ? ctx.node_subtypes.outfalls.has_flap_gate[static_cast<std::size_t>(r)] : 0;
    if (has_gate) *has_gate = hf ? 1 : 0;
    return SWMM_OK;
}

// ============================================================================
// Divider Node API
// ============================================================================

SWMM_ENGINE_API int swmm_node_set_divider_type(SWMM_Engine engine, int idx, int type) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (type < 0 || type > static_cast<int>(openswmm::DividerType::WEIR))
        return SWMM_ERR_BADPARAM;
    const int r = ctx.node_subtypes.divider_row(idx);
    if (r >= 0) ctx.node_subtypes.dividers.method[static_cast<std::size_t>(r)] =
                    static_cast<openswmm::DividerType>(type);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_divider_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.divider_row(idx);
    if (type) *type = (r >= 0)
        ? static_cast<int>(ctx.node_subtypes.dividers.method[static_cast<std::size_t>(r)])
        : static_cast<int>(openswmm::DividerType::CUTOFF);
    return SWMM_OK;
}

// ============================================================================
// Node Geometry/State Getters
// ============================================================================

SWMM_ENGINE_API int swmm_node_get_surcharge_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (depth) *depth = to_display(ctx, openswmm::ucf::LENGTH, ctx.nodes.sur_depth[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_ponded_area(SWMM_Engine engine, int idx, double* area) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    // units: ponded area = LENGTH^2 (ft2/m2)
    if (area) {
        double f = openswmm::ucf::UCF(openswmm::ucf::LENGTH, ctx.options);
        *area = ctx.nodes.ponded_area[static_cast<std::size_t>(idx)] * f * f;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_initial_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (depth) *depth = to_display(ctx, openswmm::ucf::LENGTH, ctx.nodes.init_depth[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_crown_elev(SWMM_Engine engine, int idx, double* elev) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (elev) *elev = to_display(ctx, openswmm::ucf::LENGTH, ctx.nodes.crown_elev[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_full_volume(SWMM_Engine engine, int idx, double* vol) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (vol) *vol = to_display(ctx, openswmm::ucf::VOLUME, ctx.nodes.full_volume[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_losses(SWMM_Engine engine, int idx, double* losses) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (losses) *losses = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.losses[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outflow(SWMM_Engine engine, int idx, double* outflow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (outflow) *outflow = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.outflow[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_degree(SWMM_Engine engine, int idx, int* degree) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (degree) *degree = ctx.nodes.degree[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Node Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_node_get_stat_max_depth(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = to_display(ctx, openswmm::ucf::LENGTH, ctx.nodes.stat_max_depth[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_stat_max_overflow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = to_display(ctx, openswmm::ucf::FLOW, ctx.nodes.stat_max_overflow[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_stat_vol_flooded(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = to_display(ctx, openswmm::ucf::VOLUME, ctx.nodes.stat_vol_flooded[static_cast<std::size_t>(idx)]); // units
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_stat_time_flooded(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = ctx.nodes.stat_time_flooded[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Outfall route-to
// ============================================================================

SWMM_ENGINE_API int swmm_node_set_outfall_route_to(SWMM_Engine engine, int idx, int subcatch_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.outfall_row(idx);
    if (r >= 0) ctx.node_subtypes.outfalls.route_to[static_cast<std::size_t>(r)] = subcatch_idx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outfall_route_to(SWMM_Engine engine, int idx, int* subcatch_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const int r = ctx.node_subtypes.outfall_row(idx);
    if (subcatch_idx) *subcatch_idx = (r >= 0)
        ? ctx.node_subtypes.outfalls.route_to[static_cast<std::size_t>(r)] : -1;
    return SWMM_OK;
}

// ============================================================================
// Depth from volume
// ============================================================================

SWMM_ENGINE_API int swmm_node_get_depth_from_volume(SWMM_Engine engine, int idx,
                                                      double volume, double* depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (depth) {
        // units: input volume is display VOLUME, output is display LENGTH
        double vol_internal = to_internal(ctx, openswmm::ucf::VOLUME, volume);
        double depth_internal = openswmm::node::getDepth(ctx.nodes, idx, vol_internal, &ctx.tables,
                                                         0, &ctx.node_subtypes);
        *depth = to_display(ctx, openswmm::ucf::LENGTH, depth_internal);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_rename(SWMM_Engine engine, int idx, const char* newId) {
    CHECK_HANDLE(engine);
    if (!newId || newId[0] == '\0') return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    return ctx.node_names.rename(idx, newId) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_node_get_tag(SWMM_Engine engine, int idx,
                                       char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const auto u = static_cast<std::size_t>(idx);
    const std::string& s = (u < ctx.nodes.tags.size()) ? ctx.nodes.tags[u]
                                                       : std::string{};
    const int copy_len = std::min(static_cast<int>(s.size()), buflen - 1);
    if (copy_len > 0) std::memcpy(buf, s.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_tag(SWMM_Engine engine, int idx,
                                       const char* tag) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const auto u = static_cast<std::size_t>(idx);
    if (u >= ctx.nodes.tags.size()) ctx.nodes.tags.resize(u + 1);
    ctx.nodes.tags[u] = (tag != nullptr) ? std::string(tag) : std::string{};
    return SWMM_OK;
}

} /* extern "C" */
