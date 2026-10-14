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
 * @file openswmm_heat_impl.cpp
 * @brief Heat-configuration C API (phase H6a).
 *
 * @details Every setter here enforces the SAME range rule the `model.heat`
 *          parser does, and enforces it the same way: **refuse, do not
 *          clamp**. Two entry points into one configuration that disagree
 *          about what is legal is how a deck and a GUI come to describe
 *          different models, and the parser's rule is the older one.
 *
 * @see include/openswmm/engine/openswmm_heat.h
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §2.5, §5
 */

#include "openswmm_api_common.hpp"

#include <cmath>
#include <string>
#include <vector>
#include "../../../include/openswmm/engine/openswmm_heat.h"
#include "../transport/components/HeatModule/HeatComponent.hpp"
#include "../transport/components/HeatFluxModules/HeatOverrides.hpp"

namespace {

using openswmm::HeatConfigData;
using openswmm::HeatSource;
using openswmm::ShortwaveMode;

/// The parser's own range (`HeatComponent.cpp` kMinTemp/kMaxTemp, used by
/// `parse_celsius`). Duplicated as a literal here rather than shared,
/// because the constants are file-local to the component and hoisting them
/// into a header is a wider change than this round earns — but they MUST
/// agree: a value the deck refuses and the API accepts is exactly the
/// deck/API disagreement the refuse-don't-clamp rule exists to prevent.
/// A gate pins the two together.
constexpr double kApiMinTempC = -50.0;
constexpr double kApiMaxTempC = 100.0;

bool temp_ok(double c) { return c >= kApiMinTempC && c <= kApiMaxTempC; }

/// Copy a bound-series name out, NUL-terminated and truncated to the
/// caller's buffer — the `climate_fill_buf` convention, one API over.
/// An out-of-range index (no binding, or a table list that shrank) is "".
void ts_name_out(const openswmm::SimulationContext& ctx, int ts_index,
                 char* buf, int buflen) {
    std::string name;
    if (ts_index >= 0 && ts_index < static_cast<int>(ctx.tables.count()))
        name = ctx.tables[ts_index].id;
    const auto n = std::min(static_cast<std::size_t>(buflen - 1), name.size());
    std::memcpy(buf, name.data(), n);
    buf[n] = '\0';
}

/// Sources that accept a NODE-scope override in H1 (HeatComponent.cpp's
/// rule: "NODE scope applies to DWF and EXTERNAL_INFLOW in H1").
bool node_scope_ok(int src) {
    return src == static_cast<int>(HeatSource::DWF) ||
           src == static_cast<int>(HeatSource::EXTERNAL_INFLOW);
}

bool source_ok(int src) {
    return src >= 0 && src < static_cast<int>(HeatSource::COUNT_);
}

/// Fraction guard, shared by every [0,1] parameter below.
bool frac_ok(double v) { return v >= 0.0 && v <= 1.0; }

}  // namespace

// ===========================================================================
// Toggles
// ===========================================================================

SWMM_ENGINE_API int swmm_heat_get_enabled(SWMM_Engine engine, int* enabled) {
    CHECK_HANDLE(engine);
    if (enabled == nullptr) return SWMM_ERR_BADPARAM;
    *enabled = to_engine(engine)->context().options.heat_transport ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_module(SWMM_Engine engine, int module,
                                         int* on) {
    CHECK_HANDLE(engine);
    if (on == nullptr) return SWMM_ERR_BADPARAM;
    const auto& cfg = to_engine(engine)->context().heat_config;
    switch (module) {
        case SWMM_HEAT_SURFACE_EXCHANGE:   *on = cfg.surface_exchange ? 1 : 0;
                                           return SWMM_OK;
        case SWMM_HEAT_RADIATIVE_EXCHANGE: *on = cfg.radiative_exchange ? 1 : 0;
                                           return SWMM_OK;
        case SWMM_HEAT_LAYER_CONDUCTION:   *on = cfg.layer_conduction ? 1 : 0;
                                           return SWMM_OK;
        default:                           return SWMM_ERR_BADINDEX;
    }
}

SWMM_ENGINE_API int swmm_heat_set_module(SWMM_Engine engine, int module,
                                         int on) {
    CHECK_HANDLE(engine);
    auto& cfg = to_engine(engine)->context().heat_config;
    const bool b = (on != 0);
    switch (module) {
        case SWMM_HEAT_SURFACE_EXCHANGE:   cfg.surface_exchange   = b; break;
        case SWMM_HEAT_RADIATIVE_EXCHANGE: cfg.radiative_exchange = b; break;
        case SWMM_HEAT_LAYER_CONDUCTION:   cfg.layer_conduction   = b; break;
        default:                           return SWMM_ERR_BADINDEX;
    }
    cfg.configured = true;
    return SWMM_OK;
}

// ===========================================================================
// [RADIATIVE_FLUXES]
// ===========================================================================

SWMM_ENGINE_API int swmm_heat_get_radiative(SWMM_Engine engine, int param,
                                            double* value) {
    CHECK_HANDLE(engine);
    if (value == nullptr) return SWMM_ERR_BADPARAM;
    const auto& rc = to_engine(engine)->context().heat_config.radiative;
    switch (param) {
        case SWMM_HEAT_RAD_SHORTWAVE:       *value = rc.shortwave_wm2;   break;
        case SWMM_HEAT_RAD_ALBEDO:          *value = rc.albedo;          break;
        case SWMM_HEAT_RAD_SHADE_FACTOR:    *value = rc.shade_factor;    break;
        case SWMM_HEAT_RAD_SKY_VIEW:        *value = rc.sky_view;        break;
        case SWMM_HEAT_RAD_EMISS_WATER:     *value = rc.emiss_water;     break;
        case SWMM_HEAT_RAD_EMISS_LANDCOVER: *value = rc.emiss_landcover; break;
        case SWMM_HEAT_RAD_ATM_EMISS_COEFF: *value = rc.atm_emiss_coeff; break;
        case SWMM_HEAT_RAD_LW_REFLECTION:   *value = rc.lw_reflection;   break;
        default:                            return SWMM_ERR_BADINDEX;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_radiative(SWMM_Engine engine, int param,
                                            double value) {
    CHECK_HANDLE(engine);
    auto& cfg = to_engine(engine)->context().heat_config;
    auto& rc  = cfg.radiative;
    switch (param) {
        case SWMM_HEAT_RAD_SHORTWAVE:
            if (value < 0.0) return SWMM_ERR_BADPARAM;
            // A constant only means anything in CONSTANT mode. Silently
            // accepting it under TIMESERIES/COMPUTED would store a number
            // nothing reads — the bypass lessons 10/20 name.
            //
            // BOTH checks run BEFORE the assignment. Written the other way
            // round this refused the call and kept the write, which is the
            // half-apply `applyHeatSections` guards against at the end of
            // every parse — an API that errors and mutates anyway is worse
            // than one that only errors.
            if (rc.sw_mode != ShortwaveMode::CONSTANT) return SWMM_ERR_BADPARAM;
            rc.shortwave_wm2 = value;
            break;
        case SWMM_HEAT_RAD_ALBEDO:
            if (!frac_ok(value)) return SWMM_ERR_BADPARAM;
            rc.albedo = value; break;
        case SWMM_HEAT_RAD_SHADE_FACTOR:
            if (!frac_ok(value)) return SWMM_ERR_BADPARAM;
            rc.shade_factor = value; break;
        case SWMM_HEAT_RAD_SKY_VIEW:
            if (!frac_ok(value)) return SWMM_ERR_BADPARAM;
            rc.sky_view = value; break;
        case SWMM_HEAT_RAD_EMISS_WATER:
            if (!frac_ok(value)) return SWMM_ERR_BADPARAM;
            rc.emiss_water = value; break;
        case SWMM_HEAT_RAD_EMISS_LANDCOVER:
            if (!frac_ok(value)) return SWMM_ERR_BADPARAM;
            rc.emiss_landcover = value; break;
        case SWMM_HEAT_RAD_ATM_EMISS_COEFF:
            if (!frac_ok(value)) return SWMM_ERR_BADPARAM;
            rc.atm_emiss_coeff = value; break;
        case SWMM_HEAT_RAD_LW_REFLECTION:
            if (!frac_ok(value)) return SWMM_ERR_BADPARAM;
            rc.lw_reflection = value; break;
        default:
            return SWMM_ERR_BADINDEX;
    }
    cfg.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_shortwave_mode(SWMM_Engine engine,
                                                 int* mode) {
    CHECK_HANDLE(engine);
    if (mode == nullptr) return SWMM_ERR_BADPARAM;
    *mode = static_cast<int>(
        to_engine(engine)->context().heat_config.radiative.sw_mode);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_shortwave_mode(SWMM_Engine engine,
                                                 int mode) {
    CHECK_HANDLE(engine);
    auto& cfg = to_engine(engine)->context().heat_config;
    switch (mode) {
        case SWMM_HEAT_SW_CONSTANT:
            cfg.radiative.sw_mode = ShortwaveMode::CONSTANT;
            break;
        case SWMM_HEAT_SW_TIMESERIES:
            // Refuse a mode with nothing behind it. Accepting it would leave
            // the model reading 0 W/m2 forever and looking configured.
            if (cfg.radiative.sw_ts_index < 0) return SWMM_ERR_BADPARAM;
            cfg.radiative.sw_mode = ShortwaveMode::TIMESERIES;
            break;
        case SWMM_HEAT_SW_COMPUTED:
            // The §2.5 trap, enforced at the API exactly as at the parser:
            // no coordinates, no COMPUTED. Never fall back on the SNOWMELT
            // latitude — it defaults to 0 and would model equatorial noon.
            if (!cfg.solar.has_latitude || !cfg.solar.has_longitude)
                return SWMM_ERR_BADPARAM;
            cfg.radiative.sw_mode = ShortwaveMode::COMPUTED;
            break;
        default:
            return SWMM_ERR_BADINDEX;
    }
    cfg.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_shortwave_timeseries(SWMM_Engine engine,
                                                       const char* name) {
    CHECK_HANDLE(engine);
    if (name == nullptr || *name == '\0') return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    // BADPARAM, not BADINDEX: this is a NAME that did not resolve, and the
    // sibling convention reserves BADINDEX for out-of-range integers
    // (openswmm_gages_impl.cpp:121, openswmm_api_common.hpp:74).
    const int ts = ctx.find_timeseries(name);
    if (ts < 0) return SWMM_ERR_BADPARAM;
    ctx.heat_config.radiative.sw_ts_index = ts;
    ctx.heat_config.radiative.sw_mode     = ShortwaveMode::TIMESERIES;
    ctx.heat_config.configured            = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_shortwave_timeseries(SWMM_Engine engine,
                                                       char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (buf == nullptr || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    ts_name_out(ctx, ctx.heat_config.radiative.sw_ts_index, buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_current_shortwave(SWMM_Engine engine,
                                                    double* wm2) {
    CHECK_HANDLE(engine);
    if (wm2 == nullptr) return SWMM_ERR_BADPARAM;
    // `shortwave_now` carries a NEGATIVE "not yet resolved" sentinel
    // internally (HeatData.hpp). The documented API contract is 0 before
    // the first step, so the sentinel is folded here rather than leaking a
    // magic number to callers.
    const double sw = to_engine(engine)->context().heat_state.shortwave_now;
    *wm2 = (sw < 0.0) ? 0.0 : sw;
    return SWMM_OK;
}

// ===========================================================================
// [SOLAR_RADIATION]
// ===========================================================================

SWMM_ENGINE_API int swmm_heat_get_solar(SWMM_Engine engine, int param,
                                        double* value) {
    CHECK_HANDLE(engine);
    if (value == nullptr) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const auto& sc = ctx.heat_config.solar;
    if (ctx.snowpacks.config.site) {
        const auto& site = *ctx.snowpacks.config.site;
        if (param == SWMM_HEAT_SOLAR_LATITUDE) { *value = site.latitude; return SWMM_OK; }
        if (param == SWMM_HEAT_SOLAR_LONGITUDE) { *value = site.longitude; return SWMM_OK; }
        if (param == SWMM_HEAT_SOLAR_TIMEZONE) { *value = site.timezone; return SWMM_OK; }
        if (param == SWMM_HEAT_SOLAR_ELEVATION) { *value = site.elevation; return SWMM_OK; }
    }
    switch (param) {
        case SWMM_HEAT_SOLAR_LATITUDE:      *value = sc.latitude_deg;    break;
        case SWMM_HEAT_SOLAR_LONGITUDE:     *value = sc.longitude_deg;   break;
        case SWMM_HEAT_SOLAR_TIMEZONE:      *value = sc.timezone_hours;  break;
        case SWMM_HEAT_SOLAR_ELEVATION:     *value = sc.elevation_m;     break;
        case SWMM_HEAT_SOLAR_TURBIDITY_380: *value = sc.aod380;          break;
        case SWMM_HEAT_SOLAR_TURBIDITY_500: *value = sc.aod500;          break;
        case SWMM_HEAT_SOLAR_PRECIP_WATER:  *value = sc.precip_water_cm; break;
        case SWMM_HEAT_SOLAR_OZONE:         *value = sc.ozone_cm;        break;
        case SWMM_HEAT_SOLAR_GROUND_ALBEDO: *value = sc.ground_albedo;   break;
        default:                            return SWMM_ERR_BADINDEX;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_solar(SWMM_Engine engine, int param,
                                        double value) {
    CHECK_HANDLE(engine);
    if (!std::isfinite(value)) return SWMM_ERR_BADPARAM;
    auto& cfg = to_engine(engine)->context().heat_config;
    auto& sc  = cfg.solar;
    switch (param) {
        case SWMM_HEAT_SOLAR_LATITUDE:
            if (value < -90.0 || value > 90.0) return SWMM_ERR_BADPARAM;
            sc.latitude_deg = value;
            sc.has_latitude = true;   // what COMPUTED checks for
            break;
        case SWMM_HEAT_SOLAR_LONGITUDE:
            if (value < -180.0 || value > 180.0) return SWMM_ERR_BADPARAM;
            sc.longitude_deg = value;
            sc.has_longitude = true;
            break;
        case SWMM_HEAT_SOLAR_TIMEZONE:
            if (value < -14.0 || value > 14.0) return SWMM_ERR_BADPARAM;
            sc.timezone_hours = value;
            sc.has_timezone   = true;
            break;
        case SWMM_HEAT_SOLAR_ELEVATION:
            // The SAME band the parser enforces. This used to refuse only
            // absurd positives, on the theory that a negative was the
            // "use climate elev" sentinel — but that sentinel is gone
            // (SolarConfig::has_elevation), and the divergence contradicted
            // this file's own docblock.
            if (value < -500.0 || value > 9000.0) return SWMM_ERR_BADPARAM;
            sc.elevation_m   = value;
            sc.has_elevation = true;
            break;
        case SWMM_HEAT_SOLAR_TURBIDITY_380:
            if (value < 0.0) return SWMM_ERR_BADPARAM;
            sc.aod380 = value; break;
        case SWMM_HEAT_SOLAR_TURBIDITY_500:
            if (value < 0.0) return SWMM_ERR_BADPARAM;
            sc.aod500 = value; break;
        case SWMM_HEAT_SOLAR_PRECIP_WATER:
            if (value < 0.0) return SWMM_ERR_BADPARAM;
            sc.precip_water_cm = value; break;
        case SWMM_HEAT_SOLAR_OZONE:
            if (value < 0.0) return SWMM_ERR_BADPARAM;
            sc.ozone_cm = value; break;
        case SWMM_HEAT_SOLAR_GROUND_ALBEDO:
            if (!frac_ok(value)) return SWMM_ERR_BADPARAM;
            sc.ground_albedo = value; break;
        default:
            return SWMM_ERR_BADINDEX;
    }
    auto& site = to_engine(engine)->context().snowpacks.config.site;
    if (site) {
        if (param == SWMM_HEAT_SOLAR_LATITUDE) site->latitude = value;
        if (param == SWMM_HEAT_SOLAR_LONGITUDE) site->longitude = value;
        if (param == SWMM_HEAT_SOLAR_TIMEZONE) site->timezone = value;
        if (param == SWMM_HEAT_SOLAR_ELEVATION) site->elevation = value;
    }
    cfg.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_solar_sited(SWMM_Engine engine, int* sited) {
    CHECK_HANDLE(engine);
    if (sited == nullptr) return SWMM_ERR_BADPARAM;
    const auto& sc = to_engine(engine)->context().heat_config.solar;
    *sited = (to_engine(engine)->context().snowpacks.config.site || (sc.has_latitude && sc.has_longitude)) ? 1 : 0;
    return SWMM_OK;
}

// ===========================================================================
// [CLOUD_COVER]
// ===========================================================================

SWMM_ENGINE_API int swmm_heat_get_cloud_configured(SWMM_Engine engine,
                                                   int* configured) {
    CHECK_HANDLE(engine);
    if (configured == nullptr) return SWMM_ERR_BADPARAM;
    *configured =
        to_engine(engine)->context().heat_config.cloud.configured ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_cloud(SWMM_Engine engine, int param,
                                        double* value) {
    CHECK_HANDLE(engine);
    if (value == nullptr) return SWMM_ERR_BADPARAM;
    const auto& cc = to_engine(engine)->context().heat_config.cloud;
    switch (param) {
        case SWMM_HEAT_CLOUD_FRACTION:   *value = cc.fraction;   break;
        case SWMM_HEAT_CLOUD_SW_ATTEN_K: *value = cc.sw_atten_k; break;
        case SWMM_HEAT_CLOUD_SW_ATTEN_N: *value = cc.sw_atten_n; break;
        case SWMM_HEAT_CLOUD_LW_CLOUD_K: *value = cc.lw_cloud_k; break;
        default:                         return SWMM_ERR_BADINDEX;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_cloud(SWMM_Engine engine, int param,
                                        double value) {
    CHECK_HANDLE(engine);
    auto& cfg = to_engine(engine)->context().heat_config;
    auto& cc  = cfg.cloud;
    switch (param) {
        case SWMM_HEAT_CLOUD_FRACTION:
            // Refused, not clamped: a 75 meant as 75% must not quietly
            // become an overcast sky that looks deliberate.
            if (!frac_ok(value)) return SWMM_ERR_BADPARAM;
            cc.fraction       = value;
            cc.use_timeseries = false;
            break;
        case SWMM_HEAT_CLOUD_SW_ATTEN_K:
            if (value < 0.0) return SWMM_ERR_BADPARAM;
            cc.sw_atten_k = value; break;
        case SWMM_HEAT_CLOUD_SW_ATTEN_N:
            if (value < 0.0) return SWMM_ERR_BADPARAM;
            cc.sw_atten_n = value; break;
        case SWMM_HEAT_CLOUD_LW_CLOUD_K:
            if (value < 0.0) return SWMM_ERR_BADPARAM;
            cc.lw_cloud_k = value; break;
        default:
            return SWMM_ERR_BADINDEX;
    }
    cc.configured  = true;
    cfg.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_cloud_timeseries(SWMM_Engine engine,
                                                   const char* name) {
    CHECK_HANDLE(engine);
    if (name == nullptr || *name == '\0') return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    const int ts = ctx.find_timeseries(name);   // BADPARAM: see above
    if (ts < 0) return SWMM_ERR_BADPARAM;
    auto& cc = ctx.heat_config.cloud;
    cc.ts_index        = ts;
    cc.use_timeseries  = true;
    cc.configured      = true;
    ctx.heat_config.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_cloud_timeseries(SWMM_Engine engine,
                                                   char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (buf == nullptr || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    ts_name_out(ctx, ctx.heat_config.cloud.ts_index, buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_clear_cloud(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    // Whole-struct reset, not field-by-field: the coefficients go back to
    // their documented defaults along with the fraction, so "clear" means
    // the same thing as "no [CLOUD_COVER] section" rather than "the
    // section, with the last k you happened to type".
    ctx.heat_config.cloud = openswmm::CloudConfig{};
    ctx.heat_state.cloud_now = 0.0;
    // Back to unresolved, not to the last CLOUDED value: the cached
    // shortwave was attenuated by the cloud cover just removed, and a
    // reader between now and the next step should not see it.
    ctx.heat_state.shortwave_now = -1.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_current_cloud(SWMM_Engine engine,
                                                double* fraction) {
    CHECK_HANDLE(engine);
    if (fraction == nullptr) return SWMM_ERR_BADPARAM;
    *fraction = to_engine(engine)->context().heat_state.cloud_now;
    return SWMM_OK;
}

/* -------------------------------------------------------- [HEAT_SOURCES]
 *
 * Every refusal mirrors HeatComponent.cpp's parser, so the deck and the API
 * agree about what a model may contain. Values are refused, never clamped,
 * and a refused call does not mutate — the half-apply the radiative setter's
 * comment names.
 */

SWMM_ENGINE_API int swmm_heat_source_count(SWMM_Engine engine, int* count) {
    CHECK_HANDLE(engine);
    if (count == nullptr) return SWMM_ERR_BADPARAM;
    // A fixed enum extent, not a parsed list: this answers on a model with
    // no heat configured, which is the MCP's first call on most models.
    *count = static_cast<int>(HeatSource::COUNT_);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_source_temp(SWMM_Engine engine, int source,
                                              double* temp_c) {
    CHECK_HANDLE(engine);
    if (temp_c == nullptr) return SWMM_ERR_BADPARAM;
    if (!source_ok(source)) return SWMM_ERR_BADINDEX;
    *temp_c = to_engine(engine)->context()
                  .heat_config.global_temp[static_cast<std::size_t>(source)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_source_temp(SWMM_Engine engine, int source,
                                              double temp_c) {
    CHECK_HANDLE(engine);
    if (!source_ok(source)) return SWMM_ERR_BADINDEX;
    if (!temp_ok(temp_c))   return SWMM_ERR_BADPARAM;
    auto& cfg = to_engine(engine)->context().heat_config;
    const auto us = static_cast<std::size_t>(source);
    cfg.global_temp[us]       = temp_c;
    cfg.configured_source[us] = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_source_configured(SWMM_Engine engine,
                                                    int source,
                                                    int* configured) {
    CHECK_HANDLE(engine);
    if (configured == nullptr) return SWMM_ERR_BADPARAM;
    if (!source_ok(source)) return SWMM_ERR_BADINDEX;
    *configured = to_engine(engine)->context()
                      .heat_config
                      .configured_source[static_cast<std::size_t>(source)]
                      ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_clear_source_temp(SWMM_Engine engine,
                                                int source) {
    CHECK_HANDLE(engine);
    if (!source_ok(source)) return SWMM_ERR_BADINDEX;
    auto& cfg = to_engine(engine)->context().heat_config;
    const auto us = static_cast<std::size_t>(source);
    cfg.global_temp[us]       = HeatConfigData::kDefaultTemp;
    cfg.configured_source[us] = false;
    // NODE overrides are deliberately untouched: they are separate rows, and
    // removing model the caller did not name is the data-loss shape this
    // program has already paid for once (the embedded-section round).
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_node_override_count(SWMM_Engine engine,
                                                  int* count) {
    CHECK_HANDLE(engine);
    if (count == nullptr) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(
        to_engine(engine)->context().heat_config.node_over_source.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_node_override(SWMM_Engine engine, int index,
                                                int* source, int* node,
                                                double* temp_c) {
    CHECK_HANDLE(engine);
    const auto& cfg = to_engine(engine)->context().heat_config;
    if (index < 0 ||
        static_cast<std::size_t>(index) >= cfg.node_over_source.size())
        return SWMM_ERR_BADINDEX;
    const auto ui = static_cast<std::size_t>(index);
    if (source) *source = cfg.node_over_source[ui];
    if (node)   *node   = cfg.node_over_node[ui];
    if (temp_c) *temp_c = cfg.node_over_temp[ui];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_node_override(SWMM_Engine engine,
                                                int source, int node,
                                                double temp_c) {
    CHECK_HANDLE(engine);
    if (!source_ok(source)) return SWMM_ERR_BADINDEX;
    // H1 scope, refused rather than deferred silently — the same answer the
    // deck gets from HeatComponent.cpp ("NODE scope applies to DWF and
    // EXTERNAL_INFLOW in H1").
    if (!node_scope_ok(source)) return SWMM_ERR_BADPARAM;
    if (!temp_ok(temp_c))       return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    if (node < 0 || node >= ctx.n_nodes()) return SWMM_ERR_BADINDEX;
    auto& cfg = ctx.heat_config;
    // ALL checks precede any mutation. Written the other way round this
    // would refuse the call and keep the write.
    for (std::size_t i = 0; i < cfg.node_over_source.size(); ++i)
        if (cfg.node_over_source[i] == source &&
            cfg.node_over_node[i] == node) {
            // Update, not refuse. The parser refuses a duplicate ROW because
            // one deck cannot mean two temperatures; an API caller setting
            // the same pair twice is editing, and refusing would leave the
            // editor unable to change a value it had just written. The
            // invariant — one row per (source, node) — is identical.
            cfg.node_over_temp[i] = temp_c;
            return SWMM_OK;
        }
    cfg.node_over_source.push_back(source);
    cfg.node_over_node.push_back(node);
    cfg.node_over_temp.push_back(temp_c);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_remove_node_override(SWMM_Engine engine,
                                                   int index) {
    CHECK_HANDLE(engine);
    auto& cfg = to_engine(engine)->context().heat_config;
    if (index < 0 ||
        static_cast<std::size_t>(index) >= cfg.node_over_source.size())
        return SWMM_ERR_BADINDEX;
    const auto ui = static_cast<std::ptrdiff_t>(index);
    cfg.node_over_source.erase(cfg.node_over_source.begin() + ui);
    cfg.node_over_node.erase(cfg.node_over_node.begin() + ui);
    cfg.node_over_temp.erase(cfg.node_over_temp.begin() + ui);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_effective_source_temp(SWMM_Engine engine,
                                                        int source, int node,
                                                        double* temp_c) {
    CHECK_HANDLE(engine);
    if (temp_c == nullptr) return SWMM_ERR_BADPARAM;
    if (!source_ok(source)) return SWMM_ERR_BADINDEX;
    auto& ctx = to_engine(engine)->context();
    if (node < 0 || node >= ctx.n_nodes()) return SWMM_ERR_BADINDEX;
    // Delegate to the engine's own resolver rather than re-deriving the
    // override-beats-global precedence here: two copies of a precedence rule
    // drift, and the API's copy would drift silently.
    *temp_c = ctx.heat_config.source_temp(
        static_cast<HeatSource>(source), node);
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [SEDIMENT_EXCHANGE] and per-element overrides
// ---------------------------------------------------------------------------

namespace {

using openswmm::HeatAttr;
using openswmm::HeatScope;

static_assert(SWMM_HEAT_ATTR_SEDIMENT_SPECIFIC_HEAT ==
                  static_cast<int>(HeatAttr::SED_SPECIFIC_HEAT) &&
              SWMM_HEAT_ATTR_THERMAL_DIFFUSIVITY ==
                  static_cast<int>(HeatAttr::SED_THERMAL_DIFFUSIVITY) &&
              SWMM_HEAT_ATTR_LANDCOVER_TEMPERATURE ==
                  static_cast<int>(HeatAttr::LANDCOVER_TEMP) &&
              SWMM_HEAT_ATTR_SEDIMENT_SPECIFIC_HEAT + 1 ==
                  static_cast<int>(HeatAttr::COUNT_),
              "SWMM_HeatAttr must mirror openswmm::HeatAttr");
static_assert(SWMM_HEAT_SCOPE_TAG == static_cast<int>(HeatScope::TAG) &&
              SWMM_HEAT_SCOPE_NODE == static_cast<int>(HeatScope::NODE) &&
              SWMM_HEAT_SCOPE_LINK == static_cast<int>(HeatScope::LINK),
              "SWMM_HeatOverrideScope must mirror openswmm::HeatScope");

/// The GLOBAL sediment parameters that share an override attribute (and so
/// its value rule). Temperatures have no rule beyond being finite.
int sediment_attr(int param) {
    switch (param) {
        case SWMM_HEAT_SED_THERMAL_DIFFUSIVITY: return static_cast<int>(HeatAttr::SED_THERMAL_DIFFUSIVITY);
        case SWMM_HEAT_SED_SOLUTE_DIFFUSIVITY:  return static_cast<int>(HeatAttr::SED_SOLUTE_DIFFUSIVITY);
        case SWMM_HEAT_SED_BED_THICKNESS:       return static_cast<int>(HeatAttr::SED_BED_THICKNESS);
        case SWMM_HEAT_SED_GROUND_DEPTH:        return static_cast<int>(HeatAttr::SED_GROUND_DEPTH);
        case SWMM_HEAT_SED_HYPORHEIC_VELOCITY:  return static_cast<int>(HeatAttr::SED_HYPORHEIC_VELOCITY);
        case SWMM_HEAT_SED_DENSITY:             return static_cast<int>(HeatAttr::SED_DENSITY);
        case SWMM_HEAT_SED_SPECIFIC_HEAT:       return static_cast<int>(HeatAttr::SED_SPECIFIC_HEAT);
        default:                                return -1;
    }
}

/// Re-resolve the override rows into the per-element configs. The resolver
/// returns early on an empty row list, so the resolved arrays are cleared
/// first: removing the last row must not leave stale per-link copies.
std::vector<std::string> reresolve(openswmm::SimulationContext& ctx) {
    auto& ov = ctx.heat_config.overrides;
    ov.rad_link.clear();
    ov.rad_node.clear();
    ov.sed_link.clear();
    return openswmm::transport::heat::resolveHeatOverrides(ctx);
}

}  // namespace

SWMM_ENGINE_API int swmm_heat_get_sediment(SWMM_Engine engine, int param,
                                           double* value) {
    CHECK_HANDLE(engine);
    if (value == nullptr) return SWMM_ERR_BADPARAM;
    const auto& sd = to_engine(engine)->context().heat_config.sediment;
    switch (param) {
        case SWMM_HEAT_SED_THERMAL_DIFFUSIVITY: *value = sd.thermal_diffusivity; break;
        case SWMM_HEAT_SED_SOLUTE_DIFFUSIVITY:  *value = sd.solute_diffusivity;  break;
        case SWMM_HEAT_SED_BED_THICKNESS:       *value = sd.bed_thickness;       break;
        case SWMM_HEAT_SED_GROUND_DEPTH:        *value = sd.ground_depth;        break;
        case SWMM_HEAT_SED_GROUND_TEMPERATURE:  *value = sd.ground_temp;         break;
        case SWMM_HEAT_SED_HYPORHEIC_VELOCITY:  *value = sd.hyporheic_velocity;  break;
        case SWMM_HEAT_SED_DENSITY:             *value = sd.sed_density;         break;
        case SWMM_HEAT_SED_SPECIFIC_HEAT:       *value = sd.sed_specific_heat;   break;
        case SWMM_HEAT_SED_INITIAL_TEMPERATURE: *value = sd.initial_temp;        break;
        default: return SWMM_ERR_BADINDEX;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_sediment(SWMM_Engine engine, int param,
                                           double value) {
    CHECK_HANDLE(engine);
    if (param < SWMM_HEAT_SED_THERMAL_DIFFUSIVITY ||
        param > SWMM_HEAT_SED_INITIAL_TEMPERATURE)
        return SWMM_ERR_BADINDEX;
    if (!std::isfinite(value)) return SWMM_ERR_BADPARAM;
    const int attr = sediment_attr(param);
    // ALL checks precede the mutation (the half-apply rule of this file).
    if (attr >= 0 && !openswmm::transport::heatAttrValueOk(static_cast<HeatAttr>(attr), value))
        return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    auto& sd = ctx.heat_config.sediment;
    switch (param) {
        case SWMM_HEAT_SED_THERMAL_DIFFUSIVITY: sd.thermal_diffusivity = value; break;
        case SWMM_HEAT_SED_SOLUTE_DIFFUSIVITY:  sd.solute_diffusivity  = value; break;
        case SWMM_HEAT_SED_BED_THICKNESS:       sd.bed_thickness       = value; break;
        case SWMM_HEAT_SED_GROUND_DEPTH:        sd.ground_depth        = value; break;
        case SWMM_HEAT_SED_GROUND_TEMPERATURE:
            sd.ground_temp     = value;
            sd.ground_ts_index = -1;      // the constant spelling replaces a series
            sd.has_ground_temp = true;
            break;
        case SWMM_HEAT_SED_HYPORHEIC_VELOCITY:  sd.hyporheic_velocity  = value; break;
        case SWMM_HEAT_SED_DENSITY:             sd.sed_density         = value; break;
        case SWMM_HEAT_SED_SPECIFIC_HEAT:       sd.sed_specific_heat   = value; break;
        case SWMM_HEAT_SED_INITIAL_TEMPERATURE:
            sd.initial_temp     = value;
            sd.has_initial_temp = true;
            break;
        default: break;
    }
    ctx.heat_config.configured = true;
    // Per-link sediment configs are copies of the GLOBAL base taken at
    // resolution; refresh them so an overridden link keeps the new base for
    // every attribute it does not override.
    if (!ctx.heat_config.overrides.rows.empty()) reresolve(ctx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_sediment_ground_timeseries(SWMM_Engine engine,
                                                             char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (buf == nullptr || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    ts_name_out(ctx, ctx.heat_config.sediment.ground_ts_index, buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_sediment_ground_timeseries(SWMM_Engine engine,
                                                             const char* name) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    auto& sd = ctx.heat_config.sediment;
    if (name == nullptr || *name == '\0') {
        sd.ground_ts_index = -1;
        return SWMM_OK;
    }
    const int ts = ctx.find_timeseries(name);
    if (ts < 0) return SWMM_ERR_BADPARAM;
    sd.ground_ts_index = ts;
    sd.has_ground_temp = true;
    ctx.heat_config.configured = true;
    if (!ctx.heat_config.overrides.rows.empty()) reresolve(ctx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_element_override_count(SWMM_Engine engine,
                                                     int* count) {
    CHECK_HANDLE(engine);
    if (count == nullptr) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(
        to_engine(engine)->context().heat_config.overrides.rows.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_get_element_override(SWMM_Engine engine, int index,
                                                   int* attr, int* scope,
                                                   char* name, int name_len,
                                                   double* value) {
    CHECK_HANDLE(engine);
    const auto& rows = to_engine(engine)->context().heat_config.overrides.rows;
    if (index < 0 || index >= static_cast<int>(rows.size())) return SWMM_ERR_BADINDEX;
    const auto& r = rows[static_cast<std::size_t>(index)];
    if (attr)  *attr  = static_cast<int>(r.attr);
    if (scope) *scope = static_cast<int>(r.scope);
    if (value) *value = r.value;
    if (name && name_len > 0) {
        const auto n = std::min(static_cast<std::size_t>(name_len - 1), r.name.size());
        std::memcpy(name, r.name.data(), n);
        name[n] = '\0';
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_set_element_override(SWMM_Engine engine, int attr,
                                                   int scope, const char* name,
                                                   double value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (attr < 0 || attr >= static_cast<int>(HeatAttr::COUNT_)) return SWMM_ERR_BADINDEX;
    if (scope != SWMM_HEAT_SCOPE_TAG && scope != SWMM_HEAT_SCOPE_NODE &&
        scope != SWMM_HEAT_SCOPE_LINK)
        return SWMM_ERR_BADPARAM;
    if (name == nullptr || *name == '\0') return SWMM_ERR_BADPARAM;
    const auto a = static_cast<HeatAttr>(attr);
    // The bed zone exists beneath conduits only (parser rule).
    if (openswmm::isSedimentAttr(a) && scope == SWMM_HEAT_SCOPE_NODE)
        return SWMM_ERR_BADPARAM;
    if (!openswmm::transport::heatAttrValueOk(a, value)) return SWMM_ERR_BADPARAM;

    auto& cfg = ctx.heat_config;
    const auto before = cfg;
    const auto sc = static_cast<HeatScope>(scope);
    bool updated = false;
    for (auto& r : cfg.overrides.rows)
        if (r.attr == a && r.scope == sc && r.name == name) {
            r.value = value;   // an edit, not the duplicate the parser refuses
            updated = true;
            break;
        }
    if (!updated) cfg.overrides.rows.push_back(openswmm::HeatOverrideRow{a, sc, name, value});
    cfg.configured = true;
    if (!reresolve(ctx).empty()) {   // unknown element or tag: leave no trace
        cfg = before;
        return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_heat_remove_element_override(SWMM_Engine engine,
                                                      int index) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    auto& rows = ctx.heat_config.overrides.rows;
    if (index < 0 || index >= static_cast<int>(rows.size())) return SWMM_ERR_BADINDEX;
    rows.erase(rows.begin() + index);
    reresolve(ctx);
    return SWMM_OK;
}
