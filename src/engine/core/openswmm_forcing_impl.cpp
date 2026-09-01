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
 * @file openswmm_forcing_impl.cpp
 * @brief C API implementation — runtime forcing with mass-balance tracking.
 *
 * @see include/openswmm/engine/openswmm_forcing.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_forcing.h"
#include "../data/ForcingData.hpp"
#include "UnitConversion.hpp"

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

    // value is a PET rate in user units (in/day US, mm/day SI) → ft/sec internal
    int unit_sys = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    double rate = value / openswmm::ucf::Ucf[openswmm::ucf::EVAPRATE][unit_sys];

    auto ui = static_cast<std::size_t>(idx);
    ctx.forcing.subcatch_evap_mode[ui]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.subcatch_evap_value[ui]   = rate;
    ctx.forcing.subcatch_evap_persist[ui] = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_evap_rate(SWMM_Engine engine, double* value)
{
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    // internal ft/sec → user units (in/day US, mm/day SI)
    int unit_sys = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    if (value) *value = ctx.climate_state.evap_rate
                      * openswmm::ucf::Ucf[openswmm::ucf::EVAPRATE][unit_sys];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_subcatch_snowfall(
    SWMM_Engine engine, int idx, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;
    if (mode == SWMM_FORCING_OVERRIDE && value < 0.0) return SWMM_ERR_BADPARAM;

    // value is a snowfall rate in user units (in/hr US, mm/hr SI) → ft/sec
    int unit_sys = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    double rate = value / openswmm::ucf::Ucf[openswmm::ucf::RAINFALL][unit_sys];

    auto ui = static_cast<std::size_t>(idx);
    ctx.forcing.subcatch_snowfall_mode[ui]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.subcatch_snowfall_value[ui]   = rate;
    ctx.forcing.subcatch_snowfall_persist[ui] = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

// ============================================================================
// Climate forcing (system-wide)
// ============================================================================

SWMM_ENGINE_API int swmm_forcing_climate_temperature(
    SWMM_Engine engine, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;

    // deg C → deg F for SI projects (affine — not a Ucf factor).
    // ADD mode passes a delta: 1 degC delta = 1.8 degF delta (no offset).
    int unit_sys = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    double temp_f = value;
    if (unit_sys == 1) {
        temp_f = (mode == SWMM_FORCING_ADD) ? value * 9.0 / 5.0
                                            : value * 9.0 / 5.0 + 32.0;
    }

    ctx.forcing.climate_temperature_mode    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.climate_temperature_value   = temp_f;
    ctx.forcing.climate_temperature_persist = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_temperature(SWMM_Engine engine, double* value)
{
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    int unit_sys = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    double t = ctx.climate_state.temperature;  // deg F internal
    if (value) *value = (unit_sys == 1) ? (t - 32.0) * 5.0 / 9.0 : t;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_climate_wind(
    SWMM_Engine engine, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;
    if (mode == SWMM_FORCING_OVERRIDE && value < 0.0) return SWMM_ERR_BADPARAM;

    // user units (mph US, km/hr SI) → mph internal
    int unit_sys = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    double ws = value / openswmm::ucf::Ucf[openswmm::ucf::WINDSPEED][unit_sys];

    ctx.forcing.climate_wind_mode    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.climate_wind_value   = ws;
    ctx.forcing.climate_wind_persist = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_wind_speed(SWMM_Engine engine, double* value)
{
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    int unit_sys = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    if (value) *value = ctx.climate_state.wind_speed
                      * openswmm::ucf::Ucf[openswmm::ucf::WINDSPEED][unit_sys];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_climate_evap(
    SWMM_Engine engine, double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;
    if (mode == SWMM_FORCING_OVERRIDE && value < 0.0) return SWMM_ERR_BADPARAM;

    // user units (in/day US, mm/day SI) → ft/sec internal
    int unit_sys = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    double rate = value / openswmm::ucf::Ucf[openswmm::ucf::EVAPRATE][unit_sys];

    ctx.forcing.climate_evap_mode    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.climate_evap_value   = rate;
    ctx.forcing.climate_evap_persist = static_cast<openswmm::ForcingPersist>(persist);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_set_dry_only(SWMM_Engine engine, int flag)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    ctx.options.evap_dry_only = (flag != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_climate_get_dry_only(SWMM_Engine engine, int* flag)
{
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (flag) *flag = ctx.options.evap_dry_only ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_link_quality(
    SWMM_Engine engine, int link_idx, int pollutant_idx,
    double value, int mode, int persist)
{
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(link_idx >= 0 && link_idx < ctx.n_links());
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    if (!valid_mode(mode) || !valid_persist(persist)) return SWMM_ERR_BADPARAM;

    auto flat = static_cast<std::size_t>(link_idx) * static_cast<std::size_t>(np)
              + static_cast<std::size_t>(pollutant_idx);
    ctx.forcing.link_quality_mode[flat]    = static_cast<openswmm::ForcingMode>(mode);
    ctx.forcing.link_quality_value[flat]   = value;
    ctx.forcing.link_quality_persist[flat] = static_cast<openswmm::ForcingPersist>(persist);
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

/* ---- PE4: per-element climate ------------------------------------------ */

namespace {

/// The channel a variable id selects, or nullptr. One lookup shared by the
/// setter, the getter and the targeted clear, so the three cannot disagree
/// about which enum value means which array.
openswmm::ForcingData::ElemClimateChannel* elem_channel(
    openswmm::ForcingData& f, int variable, int slot) {
    switch (variable) {
        case SWMM_FORCE_ELEM_AIR_TEMPERATURE: return &f.elem_air_temp[slot];
        case SWMM_FORCE_ELEM_HUMIDITY:        return &f.elem_humidity[slot];
        case SWMM_FORCE_ELEM_WIND_SPEED:      return &f.elem_wind[slot];
        case SWMM_FORCE_ELEM_SHORTWAVE:       return &f.elem_shortwave[slot];
        default: return nullptr;
    }
}

/// Element count for a kind, or -1 when the kind carries no per-element
/// climate. SUBCATCH is absent BY DESIGN: a subcatchment's air temperature
/// has competing consumers (snowmelt, evaporation), so honouring it here
/// would silently make the snowpack and the conduit disagree about the
/// atmosphere. See the header.
int elem_count(const openswmm::SimulationContext& ctx, int kind) {
    if (kind == SWMM_HEAT_ELEM_LINK) return ctx.n_links();
    if (kind == SWMM_HEAT_ELEM_NODE) return ctx.n_nodes();
    return -1;
}

int elem_slot(int kind) {
    return (kind == SWMM_HEAT_ELEM_LINK) ? 0
         : (kind == SWMM_HEAT_ELEM_NODE) ? 1 : -1;
}

/// User units → the internal units the flux evaluators read. Mirrors the
/// system-wide setters exactly: an ADD delta in degC is 1.8 degF with NO
/// offset, which is the trap the global path already documents.
double to_internal(const openswmm::SimulationContext& ctx, int variable,
                   double v, int mode) {
    const int us = openswmm::ucf::getUnitSystem(
        static_cast<int>(ctx.options.flow_units));
    if (us != 1) return v;                       // US: already internal
    if (variable == SWMM_FORCE_ELEM_AIR_TEMPERATURE)
        return (mode == SWMM_FORCING_ADD) ? v * 1.8 : v * 1.8 + 32.0;
    if (variable == SWMM_FORCE_ELEM_WIND_SPEED)
        return v / 1.609344;                     // km/hr -> mph
    return v;                                    // % and W/m2 are unitless-ish
}

double from_internal(const openswmm::SimulationContext& ctx, int variable,
                     double v, int mode) {
    const int us = openswmm::ucf::getUnitSystem(
        static_cast<int>(ctx.options.flow_units));
    if (us != 1) return v;
    if (variable == SWMM_FORCE_ELEM_AIR_TEMPERATURE)
        return (mode == SWMM_FORCING_ADD) ? v / 1.8 : (v - 32.0) * 5.0 / 9.0;
    if (variable == SWMM_FORCE_ELEM_WIND_SPEED) return v * 1.609344;
    return v;
}

}  // namespace

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
        case SWMM_FORCE_SUBCATCH_SNOWFALL:
            CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
            ctx.forcing.subcatch_snowfall_mode[static_cast<std::size_t>(idx)] = openswmm::ForcingMode::NONE;
            break;
        case SWMM_FORCE_CLIMATE_TEMPERATURE:
            ctx.forcing.climate_temperature_mode = openswmm::ForcingMode::NONE;
            break;
        case SWMM_FORCE_CLIMATE_WIND:
            ctx.forcing.climate_wind_mode = openswmm::ForcingMode::NONE;
            break;
        case SWMM_FORCE_CLIMATE_EVAP:
            ctx.forcing.climate_evap_mode = openswmm::ForcingMode::NONE;
            break;
        case SWMM_FORCE_LINK_QUALITY: {
            // Clear ALL pollutant forcings for this link
            CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
            int npl = ctx.n_pollutants();
            for (int p = 0; p < npl; ++p) {
                auto flat = static_cast<std::size_t>(idx) * static_cast<std::size_t>(npl)
                          + static_cast<std::size_t>(p);
                ctx.forcing.link_quality_mode[flat] = openswmm::ForcingMode::NONE;
            }
            break;
        }
        case SWMM_FORCE_ELEM_AIR_TEMPERATURE:
        case SWMM_FORCE_ELEM_HUMIDITY:
        case SWMM_FORCE_ELEM_WIND_SPEED:
        case SWMM_FORCE_ELEM_SHORTWAVE: {
            // PE4: `idx` alone cannot say which KIND, so a targeted clear
            // clears that index in BOTH kinds. Clearing something already
            // clear is a no-op, and the alternative — a separate clear entry
            // per kind — would double this enum for no gain.
            for (int slot = 0; slot < 2; ++slot) {
                auto* ch = elem_channel(ctx.forcing, type, slot);
                if (ch == nullptr) continue;
                const auto u = static_cast<std::size_t>(idx);
                if (idx >= 0 && u < ch->mode.size())
                    ch->mode[u] = openswmm::ForcingMode::NONE;
            }
            break;
        }
        default:
            return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_element_climate(
    SWMM_Engine engine, int kind, int index, int variable, double value,
    int mode, int persist) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    const int slot = elem_slot(kind);
    const int n    = elem_count(ctx, kind);
    if (slot < 0 || n < 0) return SWMM_ERR_BADPARAM;
    CHECK_INDEX(index >= 0 && index < n);
    if (mode != SWMM_FORCING_OVERRIDE && mode != SWMM_FORCING_ADD)
        return SWMM_ERR_BADPARAM;
    if (persist != SWMM_FORCING_RESET && persist != SWMM_FORCING_PERSIST)
        return SWMM_ERR_BADPARAM;
    auto* ch = elem_channel(ctx.forcing, variable, slot);
    if (ch == nullptr) return SWMM_ERR_BADPARAM;

    // Sized lazily (D-PE2): a model that never calls this allocates nothing
    // and pays one `.empty()` check per flux evaluation.
    ch->ensure(static_cast<std::size_t>(n));
    const auto u = static_cast<std::size_t>(index);
    ch->mode[u]    = (mode == SWMM_FORCING_ADD)
                         ? openswmm::ForcingMode::ADD
                         : openswmm::ForcingMode::OVERRIDE;
    ch->value[u]   = to_internal(ctx, variable, value, mode);
    ch->persist[u] = (persist == SWMM_FORCING_PERSIST)
                         ? openswmm::ForcingPersist::PERSIST
                         : openswmm::ForcingPersist::RESET;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_element_climate_get(
    SWMM_Engine engine, int kind, int index, int variable, double* value,
    int* mode) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    const int slot = elem_slot(kind);
    const int n    = elem_count(ctx, kind);
    if (slot < 0 || n < 0) return SWMM_ERR_BADPARAM;
    CHECK_INDEX(index >= 0 && index < n);
    auto* ch = elem_channel(ctx.forcing, variable, slot);
    if (ch == nullptr) return SWMM_ERR_BADPARAM;

    const auto u = static_cast<std::size_t>(index);
    if (u >= ch->mode.size() ||
        ch->mode[u] == openswmm::ForcingMode::NONE) {
        if (mode)  *mode  = SWMM_FORCING_NONE;
        if (value) *value = 0.0;
        return SWMM_OK;
    }
    const int m = (ch->mode[u] == openswmm::ForcingMode::ADD)
                      ? SWMM_FORCING_ADD : SWMM_FORCING_OVERRIDE;
    if (mode)  *mode  = m;
    if (value) *value = from_internal(ctx, variable, ch->value[u], m);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_forcing_clear_all(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    to_engine(engine)->context().forcing.clear_all();
    return SWMM_OK;
}

} /* extern "C" */
