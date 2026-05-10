/**
 * @file openswmm_forcing_impl.cpp
 * @brief C API implementation — runtime forcing with mass-balance tracking.
 *
 * @see include/openswmm/engine/openswmm_forcing.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_forcing.h"
#include "../data/ForcingData.hpp"

extern "C" {

// ============================================================================
// Internal helpers
// ============================================================================

static bool valid_mode(int mode) {
    return mode >= SWMM_FORCING_NONE && mode <= SWMM_FORCING_ADD;
}

static bool valid_persist(int persist) {
    return persist >= SWMM_FORCING_RESET && persist <= SWMM_FORCING_PERSIST;
}

// ============================================================================
// Node forcing
// ============================================================================

SWMM_ENGINE_API int swmm_forcing_node_lat_inflow(
    SWMM_Engine engine, int idx, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;

    auto ui = static_cast<std::size_t>(idx);
    ctx.forcing.node_lat_inflow_mode[ui]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.node_lat_inflow_value[ui]   = value;
    ctx.forcing.node_lat_inflow_persist[ui] = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_node_head_boundary(
    SWMM_Engine engine, int idx, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;

    auto ui = static_cast<std::size_t>(idx);
    // Head boundary only meaningful for outfall nodes
    if (ctx.nodes.type[ui] != openswmm::NodeType::OUTFALL)
        return SWMM_ERR_BADPARAM;

    ctx.forcing.node_head_boundary_mode[ui]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.node_head_boundary_value[ui]   = value;
    ctx.forcing.node_head_boundary_persist[ui] = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_node_quality(
    SWMM_Engine engine, int node_idx, int pollutant_idx,
    double mass_rate, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(node_idx >= 0 && node_idx < ctx.n_nodes());
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;

    auto flat = static_cast<std::size_t>(node_idx) * static_cast<std::size_t>(np)
              + static_cast<std::size_t>(pollutant_idx);
    ctx.forcing.node_quality_mode[flat]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.node_quality_value[flat]   = mass_rate;
    ctx.forcing.node_quality_persist[flat] = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

// ============================================================================
// Link forcing
// ============================================================================

SWMM_ENGINE_API int swmm_forcing_link_flow(
    SWMM_Engine engine, int idx, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;

    auto ui = static_cast<std::size_t>(idx);
    ctx.forcing.link_flow_mode[ui]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.link_flow_value[ui]   = value;
    ctx.forcing.link_flow_persist[ui] = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_link_setting(
    SWMM_Engine engine, int idx, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;

    auto ui = static_cast<std::size_t>(idx);
    ctx.forcing.link_setting_mode[ui]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.link_setting_value[ui]   = value;
    ctx.forcing.link_setting_persist[ui] = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

// ============================================================================
// Subcatchment forcing
// ============================================================================

SWMM_ENGINE_API int swmm_forcing_subcatch_rainfall(
    SWMM_Engine engine, int idx, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;

    auto ui = static_cast<std::size_t>(idx);
    ctx.forcing.subcatch_rainfall_mode[ui]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.subcatch_rainfall_value[ui]   = value;
    ctx.forcing.subcatch_rainfall_persist[ui] = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_subcatch_evap(
    SWMM_Engine engine, int idx, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;

    auto ui = static_cast<std::size_t>(idx);
    ctx.forcing.subcatch_evap_mode[ui]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.subcatch_evap_value[ui]   = value;
    ctx.forcing.subcatch_evap_persist[ui] = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

// ============================================================================
// Gage forcing
// ============================================================================

SWMM_ENGINE_API int swmm_forcing_gage_rainfall(
    SWMM_Engine engine, int idx, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;

    auto ui = static_cast<std::size_t>(idx);
    ctx.forcing.gage_rainfall_mode[ui]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.gage_rainfall_value[ui]   = value;
    ctx.forcing.gage_rainfall_persist[ui] = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

// ============================================================================
// Clear forcing
// ============================================================================

SWMM_ENGINE_API int swmm_forcing_clear(SWMM_Engine engine, int type, int idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();

    switch (static_cast<SWMM_ForcingType>(type)) {
        case SWMM_FORCE_NODE_LAT_INFLOW:
            CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
            ctx.forcing.node_lat_inflow_mode[static_cast<std::size_t>(idx)] = openswmm::ForcingMode::NONE;
            break;
        case SWMM_FORCE_NODE_HEAD_BOUNDARY:
            CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
            ctx.forcing.node_head_boundary_mode[static_cast<std::size_t>(idx)] = openswmm::ForcingMode::NONE;
            break;
        case SWMM_FORCE_NODE_QUALITY: {
            // Clear ALL pollutant forcings for this node
            CHECK_INDEX(idx >= 0 && idx < ctx.n_nodes());
            int np = ctx.n_pollutants();
            for (int p = 0; p < np; ++p) {
                auto flat = static_cast<std::size_t>(idx) * static_cast<std::size_t>(np)
                          + static_cast<std::size_t>(p);
                ctx.forcing.node_quality_mode[flat] = openswmm::ForcingMode::NONE;
            }
            break;
        }
        case SWMM_FORCE_LINK_FLOW:
            CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
            ctx.forcing.link_flow_mode[static_cast<std::size_t>(idx)] = openswmm::ForcingMode::NONE;
            break;
        case SWMM_FORCE_LINK_SETTING:
            CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
            ctx.forcing.link_setting_mode[static_cast<std::size_t>(idx)] = openswmm::ForcingMode::NONE;
            break;
        case SWMM_FORCE_SUBCATCH_RAINFALL:
            CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
            ctx.forcing.subcatch_rainfall_mode[static_cast<std::size_t>(idx)] = openswmm::ForcingMode::NONE;
            break;
        case SWMM_FORCE_SUBCATCH_EVAP:
            CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
            ctx.forcing.subcatch_evap_mode[static_cast<std::size_t>(idx)] = openswmm::ForcingMode::NONE;
            break;
        case SWMM_FORCE_GAGE_RAINFALL:
            CHECK_INDEX(idx >= 0 && idx < ctx.n_gages());
            ctx.forcing.gage_rainfall_mode[static_cast<std::size_t>(idx)] = openswmm::ForcingMode::NONE;
            break;
        default:
            return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_clear_all(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    to_engine(engine)->context().forcing.clear_all();
    return SWMM_OK;
}

} /* extern "C" */
