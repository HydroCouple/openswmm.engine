/**
 * @file openswmm_spatial_impl.cpp
 * @brief C API implementation — spatial frame: CRS, coordinates, vertices, polygons.
 *
 * @see include/openswmm/engine/openswmm_spatial.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_spatial.h"

extern "C" {

// ============================================================================
// CRS
// ============================================================================

SWMM_ENGINE_API int swmm_spatial_set_crs(SWMM_Engine engine, const char* crs) {
    CHECK_HANDLE(engine);
    if (!crs) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    ctx.spatial.crs = crs;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_get_crs(SWMM_Engine engine, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const auto& crs = ctx.spatial.crs;
    const int copy_len = std::min(static_cast<int>(crs.size()), buflen - 1);
    std::memcpy(buf, crs.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
    return SWMM_OK;
}

// ============================================================================
// Node coordinates
// ============================================================================

SWMM_ENGINE_API int swmm_spatial_set_node_coord(SWMM_Engine engine, int idx, double x, double y) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const auto ui = static_cast<std::size_t>(idx);
    ctx.spatial.node_x[ui] = x;
    ctx.spatial.node_y[ui] = y;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_get_node_coord(SWMM_Engine engine, int idx, double* x, double* y) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    const auto ui = static_cast<std::size_t>(idx);
    if (x) *x = ctx.spatial.node_x[ui];
    if (y) *y = ctx.spatial.node_y[ui];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_get_node_coords_bulk(SWMM_Engine engine, double* x_buf, double* y_buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!x_buf || !y_buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.spatial.node_x.begin(), ctx.spatial.node_x.begin() + n, x_buf);
    std::copy(ctx.spatial.node_y.begin(), ctx.spatial.node_y.begin() + n, y_buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_set_node_coords_bulk(SWMM_Engine engine, const double* x_buf, const double* y_buf, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (!x_buf || !y_buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    std::copy(x_buf, x_buf + n, ctx.spatial.node_x.begin());
    std::copy(y_buf, y_buf + n, ctx.spatial.node_y.begin());
    return SWMM_OK;
}

// ============================================================================
// Link coordinates (centroid)
// ============================================================================

SWMM_ENGINE_API int swmm_spatial_set_link_coord(SWMM_Engine engine, int idx, double x, double y) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto ui = static_cast<std::size_t>(idx);
    ctx.spatial.link_x[ui] = x;
    ctx.spatial.link_y[ui] = y;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_get_link_coord(SWMM_Engine engine, int idx, double* x, double* y) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto ui = static_cast<std::size_t>(idx);
    if (x) *x = ctx.spatial.link_x[ui];
    if (y) *y = ctx.spatial.link_y[ui];
    return SWMM_OK;
}

// ============================================================================
// Link vertices (polyline)
// ============================================================================

SWMM_ENGINE_API int swmm_spatial_set_link_vertices(SWMM_Engine engine, int idx, const double* x, const double* y, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (count > 0 && (!x || !y)) return SWMM_ERR_BADPARAM;
    const auto ui = static_cast<std::size_t>(idx);
    ctx.spatial.link_vertices_x[ui].assign(x, x + count);
    ctx.spatial.link_vertices_y[ui].assign(y, y + count);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_get_link_vertex_count(SWMM_Engine engine, int idx, int* count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto ui = static_cast<std::size_t>(idx);
    const int interior_count = static_cast<int>(ctx.spatial.link_vertices_x[ui].size());
    const int from_node = ctx.links.node1[ui];
    const int to_node = ctx.links.node2[ui];
    const int endpoint_count =
        ((from_node >= 0 && from_node < ctx.n_nodes()) ? 1 : 0) +
        ((to_node >= 0 && to_node < ctx.n_nodes()) ? 1 : 0);
    if (count) *count = interior_count + endpoint_count;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_get_link_vertices(SWMM_Engine engine, int idx, double* x, double* y, int max_count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (!x || !y || max_count <= 0) return SWMM_ERR_BADPARAM;
    const auto ui = static_cast<std::size_t>(idx);
    const auto& vx = ctx.spatial.link_vertices_x[ui];
    const auto& vy = ctx.spatial.link_vertices_y[ui];
    const int from_node = ctx.links.node1[ui];
    const int to_node = ctx.links.node2[ui];

    int out_count = 0;
    if (from_node >= 0 && from_node < ctx.n_nodes() && out_count < max_count) {
        const auto uf = static_cast<std::size_t>(from_node);
        x[out_count] = ctx.spatial.node_x[uf];
        y[out_count] = ctx.spatial.node_y[uf];
        ++out_count;
    }

    const int interior_count = static_cast<int>(vx.size());
    const int interior_copy = std::min(max_count - out_count, interior_count);
    if (interior_copy > 0) {
        std::copy(vx.begin(), vx.begin() + interior_copy, x + out_count);
        std::copy(vy.begin(), vy.begin() + interior_copy, y + out_count);
        out_count += interior_copy;
    }

    if (to_node >= 0 && to_node < ctx.n_nodes() && out_count < max_count) {
        const auto ut = static_cast<std::size_t>(to_node);
        x[out_count] = ctx.spatial.node_x[ut];
        y[out_count] = ctx.spatial.node_y[ut];
    }
    return SWMM_OK;
}

// ============================================================================
// Subcatchment coordinates (centroid)
// ============================================================================

SWMM_ENGINE_API int swmm_spatial_set_subcatch_coord(SWMM_Engine engine, int idx, double x, double y) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    const auto ui = static_cast<std::size_t>(idx);
    ctx.spatial.subcatch_x[ui] = x;
    ctx.spatial.subcatch_y[ui] = y;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_get_subcatch_coord(SWMM_Engine engine, int idx, double* x, double* y) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    const auto ui = static_cast<std::size_t>(idx);
    if (x) *x = ctx.spatial.subcatch_x[ui];
    if (y) *y = ctx.spatial.subcatch_y[ui];
    return SWMM_OK;
}

// ============================================================================
// Subcatchment polygon
// ============================================================================

SWMM_ENGINE_API int swmm_spatial_set_subcatch_polygon(SWMM_Engine engine, int idx, const double* x, const double* y, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (count > 0 && (!x || !y)) return SWMM_ERR_BADPARAM;
    const auto ui = static_cast<std::size_t>(idx);
    ctx.spatial.subcatch_polygon_x[ui].assign(x, x + count);
    ctx.spatial.subcatch_polygon_y[ui].assign(y, y + count);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_get_subcatch_polygon_count(SWMM_Engine engine, int idx, int* count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (count) *count = static_cast<int>(ctx.spatial.subcatch_polygon_x[static_cast<std::size_t>(idx)].size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_get_subcatch_polygon(SWMM_Engine engine, int idx, double* x, double* y, int max_count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (!x || !y || max_count <= 0) return SWMM_ERR_BADPARAM;
    const auto ui = static_cast<std::size_t>(idx);
    const auto& px = ctx.spatial.subcatch_polygon_x[ui];
    const auto& py = ctx.spatial.subcatch_polygon_y[ui];
    const int n = std::min(max_count, static_cast<int>(px.size()));
    std::copy(px.begin(), px.begin() + n, x);
    std::copy(py.begin(), py.begin() + n, y);
    return SWMM_OK;
}

// ============================================================================
// Gage coordinates
// ============================================================================

SWMM_ENGINE_API int swmm_spatial_set_gage_coord(SWMM_Engine engine, int idx, double x, double y) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    const auto ui = static_cast<std::size_t>(idx);
    ctx.spatial.gage_x[ui] = x;
    ctx.spatial.gage_y[ui] = y;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_spatial_get_gage_coord(SWMM_Engine engine, int idx, double* x, double* y) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    const auto ui = static_cast<std::size_t>(idx);
    if (x) *x = ctx.spatial.gage_x[ui];
    if (y) *y = ctx.spatial.gage_y[ui];
    return SWMM_OK;
}

} /* extern "C" */
