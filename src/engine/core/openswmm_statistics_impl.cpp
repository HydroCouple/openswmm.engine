// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file openswmm_statistics_impl.cpp
 * @brief C API implementation — node, link, subcatchment statistics queries.
 *
 * @see include/openswmm/engine/openswmm_statistics.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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

// ----------------------------------------------------------------------------
// Phase 3 statistics bulk getters — node max_overflow, vol_flooded,
// time_flooded; subcatch max_runoff.
//
// All four are simple SoA memcpys; the rationale for adding them now is that
// they are the four most-hit scalar getters in the MCP server's flooding /
// capacity summary tools. Per-element loops there pay 4N ABI crossings; the
// bulk variants compress that to 4 single-pass copies.
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_stat_node_max_overflow_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.stat_max_overflow.begin(),
              ctx.nodes.stat_max_overflow.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_node_vol_flooded_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.stat_vol_flooded.begin(),
              ctx.nodes.stat_vol_flooded.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_node_time_flooded_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_nodes());
    std::copy(ctx.nodes.stat_time_flooded.begin(),
              ctx.nodes.stat_time_flooded.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_subcatch_max_runoff_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_subcatches());
    std::copy(ctx.subcatches.stat_max_runoff.begin(),
              ctx.subcatches.stat_max_runoff.begin() + n, buf);
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// Phase 4e: link-stat bulks (max_velocity, max_filling, vol_flow,
// surcharge_time).  All simple SoA memcpys from the corresponding scalar
// accessor's column.  Added to complete the per-link statistics surface so
// the MCP server's capacity_summary tool can be collapsed to a single-pass
// shape.
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_stat_link_max_velocity_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.stat_max_veloc.begin(),
              ctx.links.stat_max_veloc.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_max_filling_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.stat_max_filling.begin(),
              ctx.links.stat_max_filling.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_vol_flow_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.stat_vol_flow.begin(),
              ctx.links.stat_vol_flow.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_stat_link_surcharge_time_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.stat_time_surcharged.begin(),
              ctx.links.stat_time_surcharged.begin() + n, buf);
    return SWMM_OK;
}

} /* extern "C" */
