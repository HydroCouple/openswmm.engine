/**
 * @file openswmm_nodes_impl.cpp
 * @brief C API implementation — node identity, creation, properties, state, bulk.
 *
 * @see include/openswmm/engine/openswmm_nodes.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_nodes.h"

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
// Creation (BUILDING state only)
// ============================================================================

SWMM_ENGINE_API int swmm_node_add(SWMM_Engine engine, const char* id, int type) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    if (ctx.state != openswmm::EngineState::BUILDING)
        return SWMM_ERR_LIFECYCLE;

    // Check for duplicate ID
    if (ctx.node_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    // Add to name index (assigns next sequential index)
    int idx = ctx.node_names.add(id);

    // Resize SoA to accommodate new node
    int n = ctx.node_names.size();
    ctx.nodes.resize(n);

    // Set type
    ctx.nodes.type[static_cast<std::size_t>(idx)] = static_cast<openswmm::NodeType>(type);

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
    ctx.nodes.invert_elev[static_cast<std::size_t>(idx)] = elev;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_max_depth(SWMM_Engine engine, int idx, double depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.full_depth[static_cast<std::size_t>(idx)] = depth;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_surcharge_depth(SWMM_Engine engine, int idx, double depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.sur_depth[static_cast<std::size_t>(idx)] = depth;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_pond_area(SWMM_Engine engine, int idx, double area) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.ponded_area[static_cast<std::size_t>(idx)] = area;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_initial_depth(SWMM_Engine engine, int idx, double depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INITIAL_COND(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.depth[static_cast<std::size_t>(idx)] = depth;
    return SWMM_OK;
}

// ============================================================================
// Geometry getters
// ============================================================================

SWMM_ENGINE_API int swmm_node_get_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (type) *type = static_cast<int>(ctx.nodes.type[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_invert_elev(SWMM_Engine engine, int idx, double* elev) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (elev) *elev = ctx.nodes.invert_elev[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_max_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (depth) *depth = ctx.nodes.full_depth[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Hydraulic state getters/setters
// ============================================================================

SWMM_ENGINE_API int swmm_node_get_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (depth) *depth = ctx.nodes.depth[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_depth(SWMM_Engine engine, int idx, double depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.depth[static_cast<std::size_t>(idx)] = depth;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_head(SWMM_Engine engine, int idx, double* head) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (head) *head = ctx.nodes.head[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_volume(SWMM_Engine engine, int idx, double* volume) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (volume) *volume = ctx.nodes.volume[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_lateral_inflow(SWMM_Engine engine, int idx, double* inflow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (inflow) *inflow = ctx.nodes.lat_flow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_overflow(SWMM_Engine engine, int idx, double* overflow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (overflow) *overflow = ctx.nodes.overflow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_inflow(SWMM_Engine engine, int idx, double* inflow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    // Total inflow = lateral + upstream — upstream computed during routing
    if (inflow) *inflow = ctx.nodes.lat_flow[static_cast<std::size_t>(idx)];
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
    ctx.nodes.user_lat_flow[uidx] = flow;
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
    ctx.nodes.outfall_param[uidx] = head;
    ctx.nodes.outfall_type[uidx] = openswmm::OutfallType::FIXED;
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
    std::copy(ctx.nodes.depth.begin(), ctx.nodes.depth.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_heads_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.head.begin(), ctx.nodes.head.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_inflows_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.lat_flow.begin(), ctx.nodes.lat_flow.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_overflows_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.overflow.begin(), ctx.nodes.overflow.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_depths_bulk(SWMM_Engine engine, const double* buf, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    std::copy(buf, buf + n, ctx.nodes.depth.begin());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_lat_inflows_bulk(SWMM_Engine engine, const double* buf, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    std::copy(buf, buf + n, ctx.nodes.lat_flow.begin());
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
    ctx.nodes.storage_curve[static_cast<std::size_t>(idx)] = curve_idx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_storage_curve(SWMM_Engine engine, int idx, int* curve_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (curve_idx) *curve_idx = ctx.nodes.storage_curve[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_storage_functional(SWMM_Engine engine, int idx, double a, double b, double c) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.nodes.storage_a[uidx] = a;
    ctx.nodes.storage_b[uidx] = b;
    ctx.nodes.storage_c[uidx] = c;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_storage_functional(SWMM_Engine engine, int idx, double* a, double* b, double* c) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    auto uidx = static_cast<std::size_t>(idx);
    if (a) *a = ctx.nodes.storage_a[uidx];
    if (b) *b = ctx.nodes.storage_b[uidx];
    if (c) *c = ctx.nodes.storage_c[uidx];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_storage_seep_rate(SWMM_Engine engine, int idx, double rate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.storage_seep_rate[static_cast<std::size_t>(idx)] = rate;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_storage_seep_rate(SWMM_Engine engine, int idx, double* rate) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (rate) *rate = ctx.nodes.storage_seep_rate[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_exfil_params(SWMM_Engine engine, int idx, double suction, double ksat, double imd) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.nodes.exfil_suction[uidx] = suction;
    ctx.nodes.exfil_ksat[uidx]    = ksat;
    ctx.nodes.exfil_imd[uidx]     = imd;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_exfil_params(SWMM_Engine engine, int idx, double* suction, double* ksat, double* imd) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    auto uidx = static_cast<std::size_t>(idx);
    if (suction) *suction = ctx.nodes.exfil_suction[uidx];
    if (ksat)    *ksat    = ctx.nodes.exfil_ksat[uidx];
    if (imd)     *imd     = ctx.nodes.exfil_imd[uidx];
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
    ctx.nodes.outfall_type[static_cast<std::size_t>(idx)] = static_cast<openswmm::OutfallType>(type);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outfall_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (type) *type = static_cast<int>(ctx.nodes.outfall_type[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_outfall_stage(SWMM_Engine engine, int idx, double stage) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.nodes.outfall_param[uidx] = stage;
    ctx.nodes.outfall_type[uidx]  = openswmm::OutfallType::FIXED;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_outfall_tidal(SWMM_Engine engine, int idx, int curve_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.nodes.outfall_param[uidx] = static_cast<double>(curve_idx);
    ctx.nodes.outfall_type[uidx]  = openswmm::OutfallType::TIDAL;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_outfall_timeseries(SWMM_Engine engine, int idx, int ts_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.nodes.outfall_param[uidx] = static_cast<double>(ts_idx);
    ctx.nodes.outfall_type[uidx]  = openswmm::OutfallType::TIMESERIES;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outfall_param(SWMM_Engine engine, int idx, double* param) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (param) *param = ctx.nodes.outfall_param[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_set_outfall_flap_gate(SWMM_Engine engine, int idx, int has_gate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    ctx.nodes.outfall_has_flap_gate[static_cast<std::size_t>(idx)] = (has_gate != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outfall_flap_gate(SWMM_Engine engine, int idx, int* has_gate) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (has_gate) *has_gate = ctx.nodes.outfall_has_flap_gate[static_cast<std::size_t>(idx)] ? 1 : 0;
    return SWMM_OK;
}

// ============================================================================
// Node Geometry/State Getters
// ============================================================================

SWMM_ENGINE_API int swmm_node_get_surcharge_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (depth) *depth = ctx.nodes.sur_depth[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_ponded_area(SWMM_Engine engine, int idx, double* area) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (area) *area = ctx.nodes.ponded_area[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_initial_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (depth) *depth = ctx.nodes.init_depth[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_crown_elev(SWMM_Engine engine, int idx, double* elev) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (elev) *elev = ctx.nodes.crown_elev[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_full_volume(SWMM_Engine engine, int idx, double* vol) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (vol) *vol = ctx.nodes.full_volume[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_losses(SWMM_Engine engine, int idx, double* losses) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (losses) *losses = ctx.nodes.losses[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_outflow(SWMM_Engine engine, int idx, double* outflow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (outflow) *outflow = ctx.nodes.outflow[static_cast<std::size_t>(idx)];
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
    if (val) *val = ctx.nodes.stat_max_depth[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_stat_max_overflow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = ctx.nodes.stat_max_overflow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_stat_vol_flooded(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = ctx.nodes.stat_vol_flooded[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_node_get_stat_time_flooded(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = ctx.nodes.stat_time_flooded[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

} /* extern "C" */
