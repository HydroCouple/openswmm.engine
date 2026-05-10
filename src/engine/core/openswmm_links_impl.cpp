/**
 * @file openswmm_links_impl.cpp
 * @brief C API implementation — link identity, creation, properties, state, bulk.
 *
 * @see include/openswmm/engine/openswmm_links.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_links.h"

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern "C" {

// ============================================================================
// Identity
// ============================================================================

SWMM_ENGINE_API int swmm_link_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().n_links();
}

SWMM_ENGINE_API int swmm_link_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    return to_engine(engine)->context().link_names.find(id);
}

SWMM_ENGINE_API const char* swmm_link_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& ctx = to_engine(engine)->context();
    if (idx < 0 || idx >= ctx.n_links()) return nullptr;
    return ctx.link_names.name_of(idx).c_str();
}

// ============================================================================
// Creation (BUILDING or OPENED — "editable" states)
// ============================================================================

SWMM_ENGINE_API int swmm_link_add(SWMM_Engine engine, const char* id, int type) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);

    if (ctx.link_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    int idx = ctx.link_names.add(id);
    int n = ctx.link_names.size();
    ctx.links.resize(n);
    ctx.links.type[static_cast<std::size_t>(idx)] = static_cast<openswmm::LinkType>(type);

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_pop_last(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);

    const int n = ctx.link_names.size();
    if (n <= 0) return SWMM_ERR_BADINDEX;

    const int tail = n - 1;
    if (ctx.link_names.name_of(tail) != id)
        return SWMM_ERR_BADINDEX;

    ctx.link_names.pop_back();
    ctx.links.resize(n - 1);
    return SWMM_OK;
}

// ============================================================================
// Connectivity
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_nodes(SWMM_Engine engine, int idx,
                                         int from_node_idx, int to_node_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.node1[static_cast<std::size_t>(idx)] = from_node_idx;
    ctx.links.node2[static_cast<std::size_t>(idx)] = to_node_idx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_from_node(SWMM_Engine engine, int idx, int* node_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (node_idx) *node_idx = ctx.links.node1[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_to_node(SWMM_Engine engine, int idx, int* node_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (node_idx) *node_idx = ctx.links.node2[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Geometry setters
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_length(SWMM_Engine engine, int idx, double length) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.length[static_cast<std::size_t>(idx)] = length;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_roughness(SWMM_Engine engine, int idx, double n) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.roughness[static_cast<std::size_t>(idx)] = n;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_offset_up(SWMM_Engine engine, int idx, double offset) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.offset1[static_cast<std::size_t>(idx)] = offset;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_offset_dn(SWMM_Engine engine, int idx, double offset) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.offset2[static_cast<std::size_t>(idx)] = offset;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_initial_flow(SWMM_Engine engine, int idx, double flow) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INITIAL_COND(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.flow[static_cast<std::size_t>(idx)] = flow;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_max_flow(SWMM_Engine engine, int idx, double flow) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.q_limit[static_cast<std::size_t>(idx)] = flow;
    return SWMM_OK;
}

// ============================================================================
// Cross-section
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_xsect(SWMM_Engine engine, int idx,
                                          int shape, double geom1, double geom2,
                                          double geom3, double geom4) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    auto uidx = static_cast<std::size_t>(idx);

    auto xs = static_cast<openswmm::XsectShape>(shape);
    ctx.links.xsect_shape[uidx] = xs;

    switch (xs) {
        case openswmm::XsectShape::CIRCULAR: {
            double d = geom1;
            ctx.links.xsect_y_full[uidx] = d;
            ctx.links.xsect_a_full[uidx] = M_PI * d * d / 4.0;
            ctx.links.xsect_w_max[uidx]  = d;
            break;
        }
        case openswmm::XsectShape::RECT_CLOSED:
        case openswmm::XsectShape::RECT_OPEN: {
            double h = geom1, w = geom2;
            ctx.links.xsect_y_full[uidx] = h;
            ctx.links.xsect_a_full[uidx] = h * w;
            ctx.links.xsect_w_max[uidx]  = w;
            break;
        }
        case openswmm::XsectShape::TRAPEZOIDAL: {
            double h = geom1, bw = geom2, ss1 = geom3, ss2 = geom4;
            ctx.links.xsect_y_full[uidx] = h;
            double tw = bw + (ss1 + ss2) * h;
            ctx.links.xsect_a_full[uidx] = (bw + tw) * h / 2.0;
            ctx.links.xsect_w_max[uidx]  = tw;
            break;
        }
        case openswmm::XsectShape::TRIANGULAR: {
            double h = geom1, tw = geom2;
            ctx.links.xsect_y_full[uidx] = h;
            ctx.links.xsect_a_full[uidx] = tw * h / 2.0;
            ctx.links.xsect_w_max[uidx]  = tw;
            break;
        }
        default: {
            // Generic fallback: store geom1 as y_full, compute area if possible
            ctx.links.xsect_y_full[uidx] = geom1;
            ctx.links.xsect_a_full[uidx] = geom1 * geom2;
            ctx.links.xsect_w_max[uidx]  = geom2;
            break;
        }
    }

    (void)geom3; (void)geom4;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_xsect(SWMM_Engine engine, int idx,
                                          int* shape, double* geom1, double* geom2,
                                          double* geom3, double* geom4) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    auto uidx = static_cast<std::size_t>(idx);
    if (shape) *shape = static_cast<int>(ctx.links.xsect_shape[uidx]);
    if (geom1) *geom1 = ctx.links.xsect_y_full[uidx];
    if (geom2) *geom2 = ctx.links.xsect_w_max[uidx];
    if (geom3) *geom3 = ctx.links.xsect_a_full[uidx];
    if (geom4) *geom4 = 0.0;
    return SWMM_OK;
}

// ============================================================================
// Geometry getters
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (type) *type = static_cast<int>(ctx.links.type[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_length(SWMM_Engine engine, int idx, double* length) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (length) *length = ctx.links.length[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_roughness(SWMM_Engine engine, int idx, double* n) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (n) *n = ctx.links.roughness[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Hydraulic state
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_flow(SWMM_Engine engine, int idx, double* flow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (flow) *flow = ctx.links.flow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_flow(SWMM_Engine engine, int idx, double flow) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.flow[static_cast<std::size_t>(idx)] = flow;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (depth) *depth = ctx.links.depth[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_velocity(SWMM_Engine engine, int idx, double* velocity) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (velocity) {
        auto uidx = static_cast<std::size_t>(idx);
        double q = ctx.links.flow[uidx];
        double d = ctx.links.depth[uidx];
        double y_full = ctx.links.xsect_y_full[uidx];
        double a_full = ctx.links.xsect_a_full[uidx];
        // Approximate flow area from depth/y_full ratio times full area
        double area = (y_full > 0.0 && a_full > 0.0 && d > 0.0)
                      ? a_full * (d / y_full)
                      : 0.0;
        *velocity = (area > 1.0e-12) ? q / area : 0.0;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_capacity(SWMM_Engine engine, int idx, double* capacity) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (capacity) {
        auto uidx = static_cast<std::size_t>(idx);
        double q = ctx.links.flow[uidx];
        double qf = ctx.links.q_full[uidx];
        *capacity = (qf > 1.0e-12) ? q / qf : 0.0;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_volume(SWMM_Engine engine, int idx, double* volume) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (volume) *volume = ctx.links.volume[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Runtime forcing
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_control_setting(SWMM_Engine engine, int idx, double setting) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.setting[static_cast<std::size_t>(idx)] = setting;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_control_setting(SWMM_Engine engine, int idx, double* setting) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (setting) *setting = ctx.links.setting[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_target_setting(SWMM_Engine engine, int idx, double setting) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.target_setting[static_cast<std::size_t>(idx)] = setting;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_target_setting(SWMM_Engine engine, int idx, double* setting) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (setting) *setting = ctx.links.target_setting[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_closed(SWMM_Engine engine, int idx, int closed) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.is_closed[static_cast<std::size_t>(idx)] = (closed != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_closed(SWMM_Engine engine, int idx, int* closed) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (closed) *closed = ctx.links.is_closed[static_cast<std::size_t>(idx)] ? 1 : 0;
    return SWMM_OK;
}

// ============================================================================
// Water quality (Phase 8)
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_quality(SWMM_Engine engine, int link_idx,
                                           int pollutant_idx, double* conc) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(link_idx >= 0 && link_idx < ctx.n_links());
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    if (conc) *conc = ctx.links.conc[
        static_cast<std::size_t>(link_idx) * static_cast<std::size_t>(np) +
        static_cast<std::size_t>(pollutant_idx)];
    return SWMM_OK;
}

// ============================================================================
// Bulk access
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_flows_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.flow.begin(), ctx.links.flow.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_depths_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.depth.begin(), ctx.links.depth.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_flows_bulk(SWMM_Engine engine, const double* buf, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_links());
    std::copy(buf, buf + n, ctx.links.flow.begin());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_quality_bulk(SWMM_Engine engine, int pollutant_idx,
                                                double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    const int n = std::min(count, ctx.n_links());
    for (int i = 0; i < n; ++i) {
        buf[i] = ctx.links.conc[
            static_cast<std::size_t>(i) * static_cast<std::size_t>(np) +
            static_cast<std::size_t>(pollutant_idx)];
    }
    return SWMM_OK;
}

// ============================================================================
// Pump Link API
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_pump_curve(SWMM_Engine engine, int idx, int curve_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.pump_curve[static_cast<std::size_t>(idx)] = curve_idx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_pump_curve(SWMM_Engine engine, int idx, int* curve_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (curve_idx) *curve_idx = ctx.links.pump_curve[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_pump_init_state(SWMM_Engine engine, int idx, int on) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INITIAL_COND(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.pump_init_state[static_cast<std::size_t>(idx)] = (on != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_pump_init_state(SWMM_Engine engine, int idx, int* on) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (on) *on = ctx.links.pump_init_state[static_cast<std::size_t>(idx)] ? 1 : 0;
    return SWMM_OK;
}

// ============================================================================
// Weir Link API
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_crest_height(SWMM_Engine engine, int idx, double h) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.crest_height[static_cast<std::size_t>(idx)] = h;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_crest_height(SWMM_Engine engine, int idx, double* h) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (h) *h = ctx.links.crest_height[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_discharge_coeff(SWMM_Engine engine, int idx, double cd) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.cd[static_cast<std::size_t>(idx)] = cd;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_discharge_coeff(SWMM_Engine engine, int idx, double* cd) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (cd) *cd = ctx.links.cd[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_end_contractions(SWMM_Engine engine, int idx, double n) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.param2[static_cast<std::size_t>(idx)] = n;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_end_contractions(SWMM_Engine engine, int idx, double* n) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (n) *n = ctx.links.param2[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Conduit Loss Coefficients
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_loss_coeff(SWMM_Engine engine, int idx, double inlet, double outlet, double avg) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.links.loss_inlet[uidx]  = inlet;
    ctx.links.loss_outlet[uidx] = outlet;
    ctx.links.loss_avg[uidx]    = avg;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_loss_coeff(SWMM_Engine engine, int idx, double* inlet, double* outlet, double* avg) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    auto uidx = static_cast<std::size_t>(idx);
    if (inlet)  *inlet  = ctx.links.loss_inlet[uidx];
    if (outlet) *outlet = ctx.links.loss_outlet[uidx];
    if (avg)    *avg    = ctx.links.loss_avg[uidx];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_flap_gate(SWMM_Engine engine, int idx, int has_gate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.has_flap_gate[static_cast<std::size_t>(idx)] = (has_gate != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_flap_gate(SWMM_Engine engine, int idx, int* has_gate) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (has_gate) *has_gate = ctx.links.has_flap_gate[static_cast<std::size_t>(idx)] ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_seep_rate(SWMM_Engine engine, int idx, double rate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.seep_rate[static_cast<std::size_t>(idx)] = rate;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_seep_rate(SWMM_Engine engine, int idx, double* rate) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (rate) *rate = ctx.links.seep_rate[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_culvert_code(SWMM_Engine engine, int idx, int code) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.culvert_code[static_cast<std::size_t>(idx)] = code;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_culvert_code(SWMM_Engine engine, int idx, int* code) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (code) *code = ctx.links.culvert_code[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_barrels(SWMM_Engine engine, int idx, int n) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.barrels[static_cast<std::size_t>(idx)] = n;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_barrels(SWMM_Engine engine, int idx, int* n) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (n) *n = ctx.links.barrels[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_slope(SWMM_Engine engine, int idx, double* slope) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (slope) *slope = ctx.links.slope[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_offset_up(SWMM_Engine engine, int idx, double* offset) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (offset) *offset = ctx.links.offset1[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_offset_dn(SWMM_Engine engine, int idx, double* offset) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (offset) *offset = ctx.links.offset2[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Link Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_stat_max_flow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_max_flow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_max_velocity(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_max_veloc[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_max_filling(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_max_filling[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_vol_flow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_vol_flow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_surcharge_time(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_time_surcharged[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Pump utilization statistics
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_stat_pump_cycles(SWMM_Engine engine, int idx, int* cycles) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (cycles) *cycles = ctx.links.stat_pump_cycles[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_pump_on_time(SWMM_Engine engine, int idx, double* seconds) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (seconds) *seconds = ctx.links.stat_pump_on_time[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_pump_volume(SWMM_Engine engine, int idx, double* volume) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (volume) *volume = ctx.links.stat_pump_volume[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Hydraulic power
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_hyd_power(SWMM_Engine engine, int idx, double* power) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    auto ui = static_cast<std::size_t>(idx);
    int n1 = ctx.links.node1[ui];
    int n2 = ctx.links.node2[ui];
    double h1 = (n1 >= 0) ? ctx.nodes.head[static_cast<std::size_t>(n1)] : 0.0;
    double h2 = (n2 >= 0) ? ctx.nodes.head[static_cast<std::size_t>(n2)] : 0.0;
    constexpr double GAMMA = 62.4;
    if (power) *power = GAMMA * std::fabs(ctx.links.flow[ui]) * std::fabs(h1 - h2);
    return SWMM_OK;
}

} /* extern "C" */
