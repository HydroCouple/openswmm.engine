/**
 * @file openswmm_statistics_impl.cpp
 * @brief C API implementation — node, link, subcatchment statistics queries.
 *
 * @see include/openswmm/engine/openswmm_statistics.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_statistics.h"

extern "C" {

// ============================================================================
// Node Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_stat_node_max_depth(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = ctx.nodes.stat_max_depth[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_node_max_overflow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = ctx.nodes.stat_max_overflow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_node_vol_flooded(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = ctx.nodes.stat_vol_flooded[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_node_time_flooded(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (val) *val = ctx.nodes.stat_time_flooded[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Link Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_stat_link_max_flow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_max_flow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_max_velocity(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_max_veloc[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_max_filling(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_max_filling[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_vol_flow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_vol_flow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_surcharge_time(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_time_surcharged[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Subcatchment Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_stat_subcatch_precip(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (val) *val = ctx.subcatches.stat_precip_vol[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_subcatch_runoff_vol(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (val) *val = ctx.subcatches.stat_runoff_vol[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_subcatch_max_runoff(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (val) *val = ctx.subcatches.stat_max_runoff[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Bulk Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_stat_node_max_depth_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.stat_max_depth.begin(),
              ctx.nodes.stat_max_depth.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_max_flow_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.stat_max_flow.begin(),
              ctx.links.stat_max_flow.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_subcatch_runoff_vol_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_subcatches());
    std::copy(ctx.subcatches.stat_runoff_vol.begin(),
              ctx.subcatches.stat_runoff_vol.begin() + n, buf);
    return SWMM_OK;
}

} /* extern "C" */
