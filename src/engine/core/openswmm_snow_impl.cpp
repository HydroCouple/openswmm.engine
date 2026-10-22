// SPDX-License-Identifier: Apache-2.0

/**
 * @file      openswmm_snow_impl.cpp
 * @brief     Public C API implementation for snow models and meteorology.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_snow.h"
#include "../../../include/openswmm/engine/openswmm_snow2d.h"
#include "../input/Tokenizer.hpp"
#include "PathResolver.hpp"
#include "UnitConversion.hpp"
#include <iomanip>
#include <sstream>
#include <cmath>
#include <limits>

namespace {
/**
 * @brief Format a snow configuration scalar with17 significant digits.
 *
 * @param[in] x Scalar in its authored units; nonfinite values are not refused here.
 * @return Owned stream text under the current C++ global locale; no explicit classic-locale
 *     override.
 */
std::string numeric(double x) {
    std::ostringstream s;
    s << std::setprecision(17) << x;
    return s.str();
}
/**
 * @brief Copy an entire snow configuration string only when the destination fits.
 *
 * @param[in] value Borrowed string including any embedded NUL; no overlap with destination.
 * @param[out] out Writable size-byte span; required capacity>value.size().
 * @param[in] size Positive capacity including final NUL.
 * @return OK after copying value.size()+1 source bytes, BADPARAM for NULL/nonpositive/short output with no
 *     writes.
 */
int copy(const std::string& value, char* out, int size) {
    if (!out || size <= 0 || value.size() >= static_cast<std::size_t>(size))
        return SWMM_ERR_BADPARAM;
    std::memcpy(out, value.c_str(), value.size() + 1);
    return SWMM_OK;
}
}
extern "C" {
SWMM_ENGINE_API int swmm_snow_get_model(SWMM_Engine engine, int* model) {
    CHECK_HANDLE(engine);
    if (!model)
        return SWMM_ERR_BADPARAM;
    *model = static_cast<int>(to_engine(engine)->context().snowpacks.config.model);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_snow_set_model(SWMM_Engine engine, int model) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (model < 0 || model > 1)
        return SWMM_ERR_BADPARAM;
    ctx.snowpacks.config.model = static_cast<openswmm::surface::SnowKind>(model);
    ctx.snowpacks.config.model_authored = true;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_snow_get_step(SWMM_Engine engine, double* seconds) {
    CHECK_HANDLE(engine);
    if (!seconds)
        return SWMM_ERR_BADPARAM;
    *seconds = to_engine(engine)->context().snowpacks.config.step;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_snow_set_step(SWMM_Engine engine, double seconds) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    return openswmm::surface::setSnowTemperature(ctx.snowpacks.config,
                                                 {"SNOW_STEP", numeric(seconds)})
                   .empty()
               ? SWMM_OK
               : SWMM_ERR_BADPARAM;
}
SWMM_ENGINE_API int swmm_ueb_option_get(SWMM_Engine engine, const char* key, char* out, int size) {
    CHECK_HANDLE(engine);
    if (!key)
        return SWMM_ERR_BADPARAM;
    const auto value =
        openswmm::surface::uebOptionValue(to_engine(engine)->context().snowpacks.config, key);
    return value ? copy(*value, out, size) : SWMM_ERR_BADPARAM;
}
SWMM_ENGINE_API int swmm_ueb_option_set(SWMM_Engine engine, const char* key, const char* value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (!key || !value)
        return SWMM_ERR_BADPARAM;
    auto tokens = openswmm::input::Tokenizer::tokenize(value);
    tokens.insert(tokens.begin(), key);
    return openswmm::surface::setUebOption(ctx.snowpacks.config, tokens).empty()
               ? SWMM_OK
               : SWMM_ERR_BADPARAM;
}
/**
 * @brief Read twelve authored UEB monthly daily-temperature-range overrides.
 *
 * Writes range in degC, with zero representing inheritance from current climate DTR; does not
 * compute the effective climate-derived monthly value. No allocation or clock advancement.
 *
 * @param[in] engine Live engine handle; no lifecycle-state guard beyond non-NULL handle.
 * @param[out] values Caller-owned writable array of at least twelve doubles in January..December
 *     order.
 * @return SWMM_OK on success, SWMM_ERR_BADHANDLE for NULL engine, SWMM_ERR_BADPARAM for NULL
 *     values.
 */
SWMM_ENGINE_API int swmm_ueb_get_monthly_ranges(SWMM_Engine engine, double* values) {
    CHECK_HANDLE(engine);
    if (!values)
        return SWMM_ERR_BADPARAM;
    const auto& ranges = to_engine(engine)->context().snowpacks.config.monthly_range;
    for (int i = 0; i < 12; ++i)
        values[i] = ranges[i].value_or(0.);
    return SWMM_OK;
}
/**
 * @brief Replace twelve UEB monthly daily-temperature-range overrides.
 *
 * Validates all values before any assignment. Positive values set authored overrides, zero restores
 * inheritance; no climate table mutation or simulation step.
 *
 * @param[in] engine Live editable engine handle in BUILDING or OPENED state.
 * @param[in] values Caller-owned readable array of twelve January..December values, each finite and
 *     within [0,100] degC.
 * @return SWMM_OK on success, BADHANDLE for NULL engine, LIFECYCLE outside editable states,
 *     BADPARAM for NULL/out-of-range array.
 */
SWMM_ENGINE_API int swmm_ueb_set_monthly_ranges(SWMM_Engine engine, const double* values) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (!values)
        return SWMM_ERR_BADPARAM;
    for (int i = 0; i < 12; ++i)
        if (!std::isfinite(values[i]) || values[i] < 0 || values[i] > 100)
            return SWMM_ERR_BADPARAM;
    for (int i = 0; i < 12; ++i)
        ctx.snowpacks.config.monthly_range[i] =
            values[i] > 0 ? std::optional<double>(values[i]) : std::nullopt;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_snowpack_get_ueb(SWMM_Engine engine, int pack, SWMM_UebSnowpack* value) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!value || pack < 0 || pack >= ctx.snowpacks.count())
        return SWMM_ERR_BADPARAM;
    const auto it = ctx.snowpacks.config.packs.find(pack);
    const openswmm::surface::SnowpackConfig defaults;
    const auto& p = it == ctx.snowpacks.config.packs.end() ? defaults : it->second;
    *value = {it == ctx.snowpacks.config.packs.end() ? SWMM_SNOW_DEFAULT
                                                     : static_cast<int>(p.model),
              p.site.forest,
              p.site.drift,
              p.site.ground_heat,
              p.site.extinction_depth,
              p.initial_energy};
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_snowpack_set_ueb(SWMM_Engine engine, int pack,
                                          const SWMM_UebSnowpack* value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (!value || pack < 0 || pack >= ctx.snowpacks.count() || value->model < -1 ||
        value->model > 1)
        return SWMM_ERR_BADPARAM;
    if (value->model == SWMM_SNOW_DEFAULT) {
        ctx.snowpacks.config.packs.erase(pack);
        return SWMM_OK;
    }
    return openswmm::surface::setSnowpackUeb(
               ctx.snowpacks.config, pack,
               {numeric(value->forest_fraction), numeric(value->drift_factor),
                numeric(value->ground_heat_wm2), numeric(value->extinction_depth_m),
                numeric(value->initial_energy_kjm2), "MODEL",
                value->model == SWMM_SNOW_UEB ? "UEB" : "DEGREE_DAY"})
                   .empty()
               ? SWMM_OK
               : SWMM_ERR_BADPARAM;
}
SWMM_ENGINE_API int swmm_snowpack_apply(SWMM_Engine engine, const SWMM_SnowpackEdit* edits,
                                        int count, char* diagnostic, int capacity) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (count < 0 || (count && !edits) || !diagnostic || capacity <= 0)
        return SWMM_ERR_BADPARAM;
    diagnostic[0] = '\0';
    const auto fail = [&](const std::string& error) {
        const auto size = std::min(error.size(), static_cast<std::size_t>(capacity - 1));
        std::memcpy(diagnostic, error.data(), size);
        diagnostic[size] = '\0';
        return SWMM_ERR_BADPARAM;
    };
    auto packs = ctx.snowpacks;
    auto names = ctx.snowpack_names;
    for (int row = 0; row < count; ++row) {
        const auto& edit = edits[row];
        if (!edit.name || !*edit.name || std::strpbrk(edit.name, "\r\n\""))
            return fail("Invalid snowpack name.");
        for (int earlier = 0; earlier < row; ++earlier)
            if (openswmm::ieq(edit.name, edits[earlier].name))
                return fail("Duplicate snowpack in one edit.");
        for (int surface = 0; surface < 3; ++surface) {
            const auto& v = edit.surface[surface];
            for (int k = 0; k < 7; ++k)
                if (!std::isfinite(v[k]) || (k != 2 && v[k] < 0))
                    return fail("Invalid snowpack surface value.");
            if (v[1] < v[0] || v[3] > 1 || (surface == 0 && v[6] > 1))
                return fail("Invalid melt range or snowpack fraction.");
        }
        double sum = 0;
        for (int k = 0; k < 6; ++k) {
            if (!std::isfinite(edit.removal[k]) || edit.removal[k] < 0)
                return fail("Invalid snow removal value.");
            if (k)
                sum += edit.removal[k];
        }
        if (sum > 1 + 1e-12)
            return fail("Snow removal fractions must sum to at most one.");
        const std::string destination = edit.removal_subcatch ? edit.removal_subcatch : "";
        if (destination.find_first_of("\r\n\"") != std::string::npos)
            return fail("Invalid snow removal destination.");
        if (edit.removal[5] > 0 && ctx.subcatch_names.find(destination) < 0)
            return fail("Snow removal destination was not found.");
        int index = names.find(edit.name);
        if (index < 0) {
            index = packs.count();
            names.add(edit.name);
            packs.names.push_back(edit.name);
            packs.plowable.push_back({});
            packs.impervious.push_back({});
            packs.pervious.push_back({});
            packs.removal.push_back({});
            packs.removal_subcatch.push_back({});
        }
        const auto& value = edit.ueb;
        if (value.model < -1 || value.model > 1)
            return fail("Invalid snow model.");
        if (value.model == SWMM_SNOW_DEFAULT)
            packs.config.packs.erase(index);
        else if (auto error = openswmm::surface::setSnowpackUeb(
                     packs.config, index,
                     {numeric(value.forest_fraction), numeric(value.drift_factor),
                      numeric(value.ground_heat_wm2), numeric(value.extinction_depth_m),
                      numeric(value.initial_energy_kjm2), "MODEL",
                      value.model == SWMM_SNOW_UEB ? "UEB" : "DEGREE_DAY"});
                 !error.empty())
            return fail(error);
        std::copy_n(edit.surface[0], 7, packs.plowable[index].begin());
        std::copy_n(edit.surface[1], 7, packs.impervious[index].begin());
        std::copy_n(edit.surface[2], 7, packs.pervious[index].begin());
        std::copy_n(edit.removal, 6, packs.removal[index].begin());
        packs.removal_subcatch[index] = destination;
    }
    ctx.snowpacks = std::move(packs);
    ctx.snowpack_names = std::move(names);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_subcatch_get_snow_process_state(SWMM_Engine engine, int sc, int surface,
                                                         SWMM_SnowState* value) {
    CHECK_HANDLE(engine);
    const auto& eng = *to_engine(engine);
    if (!value || sc < 0 || sc >= eng.context().n_subcatches() || surface < 0 || surface > 2)
        return SWMM_ERR_BADPARAM;
    const auto* context = &eng.context();
    const auto* state = &eng.snowSolver().state();
#ifdef OPENSWMM_HAS_2D
    if (const auto* source = eng.completedSourceDriver();
        source && source->snow().active() && source->clocks().groupForSubcatch(sc) >= 0) {
        context = &source->context();
        state = &source->snow().state();
    }
#endif
    const auto& ctx = *context;
    const auto& snow = *state;
    const int i = sc * 3 + surface;
    if (i >= int(snow.wsnow.size()))
        return SWMM_ERR_LIFECYCLE;
    if (ctx.subcatches.snowpack[sc] < 0)
        return SWMM_ERR_BADINDEX;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    SWMM_SnowState out{};
    out.swe_m = (snow.wsnow[i] + snow.fw[i]) * .3048;
    out.cover_fraction = snow.asc[i];
    out.melt_ms = snow.imelt[i] * .3048;
    out.water_age_s = snow.age[i];
    out.energy_kjm2 = out.surface_temperature_c = out.bulk_temperature_c = out.albedo =
        out.surface_age = nan;
    out.sublimation_ms = nan;
    out.surface_input_ms = nan;
    if (sc < int(snow.external.size()) && snow.external[sc]) {
        out.model = SWMM_SNOW_UEB;
        const auto& runtime = ctx.snowpacks.runtime;
        if (i >= int(runtime.columns.size()))
            return SWMM_ERR_LIFECYCLE;
        const auto& point = runtime.columns[i].accepted.point;
        const auto& flux = runtime.last[i];
        out.energy_kjm2 = point.energy;
        out.surface_age = point.age;
        out.bulk_temperature_c =
            openswmm::surface::ueb::bulkTemperature(point, ctx.snowpacks.config.parameters);
        out.surface_temperature_c = point.history.empty() ? std::min(0., out.bulk_temperature_c)
                                                          : point.history.back().surface;
        out.albedo = runtime.sample_seconds[i] > 0 ? flux.point.albedo : nan;
        out.sublimation_ms =
            runtime.sample_seconds[i] > 0 ? flux.sublimation / runtime.sample_seconds[i] : 0;
        out.surface_input_ms = snow.external_net[i] * .3048;
    }
    *value = out;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_snow_get_ueb_ledger(SWMM_Engine engine, SWMM_SnowLedger* value) {
    CHECK_HANDLE(engine);
    if (!value)
        return SWMM_ERR_BADPARAM;
    const auto& eng = *to_engine(engine);
    const auto& ctx = eng.context();
    if (!ctx.snowpacks.runtime.active)
        return SWMM_ERR_LIFECYCLE;
    SWMM_SnowLedger out{};
    const auto add = [&](const auto& c, const auto& soa, bool ordinary) {
        const auto& runtime = c.snowpacks.runtime;
        out.elapsed_s = std::max(out.elapsed_s, runtime.completed_seconds);
        out.initial_m3 += runtime.initial_water;
        out.precipitation_m3 += runtime.precipitation;
        out.liquid_m3 += runtime.liquid;
        out.throughfall_m3 += runtime.throughfall;
        out.sublimation_m3 += runtime.sublimation;
        out.removed_m3 += runtime.removed;
        out.transferred_m3 += runtime.transferred_water;
        out.initial_energy_kj += runtime.initial_energy;
        out.net_energy_kj += runtime.net_energy;
        out.geometry_energy_kj += runtime.geometry_energy;
        out.plowed_energy_kj += runtime.plowed_energy;
        for (int sc = 0; sc < soa.n_subcatch; ++sc)
            if (soa.external[sc]) {
                const double lid = sc < int(c.subcatches.total_lid_area_ft2.size())
                                       ? c.subcatches.total_lid_area_ft2[sc]
                                       : 0;
                const double area =
                    std::max(0., c.subcatches.area[sc] /
                                         openswmm::ucf::UCF(openswmm::ucf::LANDAREA, c.options) -
                                     lid) *
                    .3048 * .3048;
                double stored = 0, energy = 0;
                for (int k = 0; k < 3; ++k) {
                    const auto& l = runtime.columns[sc * 3 + k].accepted;
                    const double a = area * soa.fArea[sc * 3 + k];
                    if (!ordinary || soa.enabled[sc])
                        out.elapsed_s = std::max(out.elapsed_s, runtime.sample_end[sc * 3 + k]);
                    stored += a * l.water;
                    energy += a * (l.cover > 0 ? l.cover : 1) * l.point.energy;
                }
                // Disabled native columns retain their initial inventory after
                // ownership moves; subtract that duplicate initial ledger entry.
                if (ordinary && !soa.enabled[sc]) {
                    out.initial_m3 -= stored;
                    out.initial_energy_kj -= energy;
                } else {
                    out.stored_m3 += stored;
                    out.stored_energy_kj += energy;
                }
            }
    };
    add(ctx, eng.snowSolver().state(), true);
#ifdef OPENSWMM_HAS_2D
    if (const auto* source = eng.completedSourceDriver(); source && source->snow().active())
        add(source->context(), source->snow().state(), false);
#endif
    out.energy_residual_kj = out.initial_energy_kj + out.net_energy_kj + out.geometry_energy_kj -
                             out.plowed_energy_kj - out.stored_energy_kj;
    out.residual_m3 = out.initial_m3 + out.precipitation_m3 + out.transferred_m3 - out.liquid_m3 -
                      out.throughfall_m3 - out.sublimation_m3 - out.removed_m3 - out.stored_m3;
    *value = out;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_climate_get_radiation(SWMM_Engine engine, SWMM_RadiationConfig* value) {
    CHECK_HANDLE(engine);
    if (!value)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const auto& cfg = ctx.snowpacks.config;
    const auto& legacy = ctx.heat_config.solar;
    const bool si = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)) == 1;
    const openswmm::surface::ClimateSite fallback{
        legacy.has_latitude ? legacy.latitude_deg : ctx.options.snow_lat,
        legacy.has_longitude ? legacy.longitude_deg : -ctx.options.snow_dtlong / 4.,
        legacy.has_longitude ? legacy.timezone_hours : 0,
        legacy.has_elevation ? legacy.elevation_m : ctx.options.snow_elev * (si ? 1 : .3048)};
    const auto site = cfg.site.value_or(fallback);
    *value = {cfg.site ? 1 : 0,
              site.latitude,
              site.longitude,
              site.timezone,
              site.elevation,
              static_cast<int>(cfg.shortwave.mode),
              static_cast<int>(cfg.longwave.mode),
              cfg.shortwave.authored ? 1 : 0,
              cfg.longwave.authored ? 1 : 0};
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_climate_set_radiation(SWMM_Engine engine,
                                               const SWMM_RadiationConfig* value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (!value || value->shortwave_mode < 0 || value->shortwave_mode > 2 ||
        value->longwave_mode < 0 || value->longwave_mode > 2 ||
        (value->explicit_site != 0 && value->explicit_site != 1) ||
        (value->shortwave_authored != 0 && value->shortwave_authored != 1) ||
        (value->longwave_authored != 0 && value->longwave_authored != 1))
        return SWMM_ERR_BADPARAM;
    auto next = ctx.snowpacks.config;
    if (value->explicit_site) {
        const auto error = openswmm::surface::setSnowTemperature(
            next, {"SITE", numeric(value->latitude), numeric(value->longitude),
                   numeric(value->timezone), numeric(value->elevation_m)});
        if (!error.empty())
            return SWMM_ERR_BADPARAM;
    } else
        next.site.reset();
    next.shortwave.mode =
        static_cast<openswmm::surface::RadiationSource::Mode>(value->shortwave_mode);
    next.longwave.mode =
        static_cast<openswmm::surface::RadiationSource::Mode>(value->longwave_mode);
    next.shortwave.authored = value->shortwave_authored;
    next.longwave.authored = value->longwave_authored;
    ctx.snowpacks.config = std::move(next);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_climate_get_radiation_source(SWMM_Engine engine, int channel, char* value,
                                                      int capacity) {
    CHECK_HANDLE(engine);
    if (channel < 0 || channel > 2)
        return SWMM_ERR_BADPARAM;
    const auto& cfg = to_engine(engine)->context().snowpacks.config;
    return copy(channel == 0   ? cfg.shortwave.series
                : channel == 1 ? cfg.longwave.series
                               : cfg.met_file.original,
                value, capacity);
}
SWMM_ENGINE_API int swmm_climate_set_radiation_source(SWMM_Engine engine, int channel,
                                                      const char* value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (!value || channel < 0 || channel > 2 || std::strpbrk(value, "\r\n\""))
        return SWMM_ERR_BADPARAM;
    auto& cfg = ctx.snowpacks.config;
    if (channel == 0)
        cfg.shortwave.series = value;
    else if (channel == 1)
        cfg.longwave.series = value;
    else
        cfg.met_file = value;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_snow_apply_configuration(SWMM_Engine engine, const char* temperature_rows,
                                                  const char* ueb_rows, char* diagnostic,
                                                  int capacity) {
    return swmm_snow_apply_configuration_with_cloud(engine, temperature_rows, ueb_rows, nullptr,
                                                    nullptr, diagnostic, capacity);
}
SWMM_ENGINE_API int swmm_climate_preview_meteorology(SWMM_Engine engine, const char* path,
                                                     SWMM_ClimateMetSummary* result,
                                                     char* diagnostic, int capacity) {
    CHECK_HANDLE(engine);
    if (!path || !result || !diagnostic || capacity <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    diagnostic[0] = '\0';
    openswmm::climate::HourlyMeteorology met;
    auto error =
        met.load(openswmm::io::resolveRelative(path, openswmm::io::parentDir(ctx.inp_file_path)));
    if (error.empty() && ctx.options.end_date > ctx.options.start_date) {
        std::array<double, 5> values;
        error = met.average(ctx.options.start_date,
                            (ctx.options.end_date - ctx.options.start_date) * 86400., values);
    }
    if (!error.empty()) {
        const auto n = std::min(error.size(), static_cast<std::size_t>(capacity - 1));
        std::memcpy(diagnostic, error.data(), n);
        diagnostic[n] = '\0';
        return SWMM_ERR_BADPARAM;
    }
    SWMM_ClimateMetSummary next{};
    next.records = static_cast<int>(met.records().size());
    next.first_date = met.records().front().date;
    next.last_date = met.records().back().date + 1. / 24;
    for (int i = 0; i < 5; ++i) {
        next.minimum[i] = next.maximum[i] = met.records().front().values[i];
        for (const auto& row : met.records()) {
            next.minimum[i] = std::min(next.minimum[i], row.values[i]);
            next.maximum[i] = std::max(next.maximum[i], row.values[i]);
        }
    }
    *result = next;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_climate_get_cloud(SWMM_Engine engine, SWMM_ClimateCloudConfig* config,
                                           char* series, int capacity) {
    CHECK_HANDLE(engine);
    if (!config)
        return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const auto& c = ctx.heat_config.cloud;
    const auto name = c.use_timeseries && c.ts_index >= 0 && c.ts_index < int(ctx.tables.count())
                          ? ctx.tables[c.ts_index].id
                          : std::string{};
    if (const int result = copy(name, series, capacity); result != SWMM_OK)
        return result;
    *config = {c.configured ? (c.use_timeseries ? 2 : 1) : 0, c.fraction, c.sw_atten_k,
               c.sw_atten_n, c.lw_cloud_k};
    return SWMM_OK;
}
SWMM_ENGINE_API int
swmm_snow_apply_configuration_with_cloud(SWMM_Engine engine, const char* temperature_rows,
                                         const char* ueb_rows, const SWMM_ClimateCloudConfig* cloud,
                                         const char* cloud_series, char* diagnostic, int capacity) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (!temperature_rows || !ueb_rows || !diagnostic || capacity <= 0)
        return SWMM_ERR_BADPARAM;
    diagnostic[0] = '\0';
    const auto fail = [&](const std::string& error) {
        const auto n = std::min(error.size(), static_cast<std::size_t>(capacity - 1));
        std::memcpy(diagnostic, error.data(), n);
        diagnostic[n] = '\0';
        return SWMM_ERR_BADPARAM;
    };
    auto next_cloud = ctx.heat_config.cloud;
    if (cloud) {
        if (cloud->mode < 0 || cloud->mode > 2 || !std::isfinite(cloud->fraction) ||
            cloud->fraction < 0 || cloud->fraction > 1 || !std::isfinite(cloud->shortwave_k) ||
            cloud->shortwave_k < 0 || !std::isfinite(cloud->shortwave_n) ||
            cloud->shortwave_n < 0 || !std::isfinite(cloud->longwave_k) || cloud->longwave_k < 0)
            return fail("Invalid shared cloud configuration.");
        next_cloud.configured = cloud->mode != 0;
        next_cloud.use_timeseries = cloud->mode == 2;
        next_cloud.fraction = cloud->fraction;
        next_cloud.sw_atten_k = cloud->shortwave_k;
        next_cloud.sw_atten_n = cloud->shortwave_n;
        next_cloud.lw_cloud_k = cloud->longwave_k;
        next_cloud.ts_index =
            cloud->mode == 2 && cloud_series ? ctx.find_timeseries(cloud_series) : -1;
        if (cloud->mode == 2 && next_cloud.ts_index < 0)
            return fail("Cloud fraction time series was not found.");
    }
    openswmm::surface::SnowConfig next;
    next.packs = ctx.snowpacks.config.packs;
    for (int section = 0; section < 2; ++section) {
        std::istringstream input(section ? ueb_rows : temperature_rows);
        std::string line;
        while (std::getline(input, line)) {
            const auto tokens = openswmm::input::Tokenizer::tokenize(line);
            if (tokens.empty())
                continue;
            const auto error = section ? openswmm::surface::setUebOption(next, tokens, false)
                                       : openswmm::surface::setSnowTemperature(next, tokens);
            if (!error.empty())
                return fail(error);
        }
    }
    if (const auto error = next.parameters.validate(); !error.empty())
        return fail(error);
    openswmm::climate::HourlyMeteorology met;
    if (!next.met_file.empty()) {
        next.met_file.absolute = openswmm::io::resolveRelative(
            next.met_file.original, openswmm::io::parentDir(ctx.inp_file_path));
        if (const auto error = met.load(next.met_file.absolute); !error.empty())
            return fail(error);
        if (ctx.options.end_date > ctx.options.start_date) {
            std::array<double, 5> values;
            if (const auto error =
                    met.average(ctx.options.start_date,
                                (ctx.options.end_date - ctx.options.start_date) * 86400., values);
                !error.empty())
                return fail(error);
        }
    }
    for (const auto& source : {next.shortwave, next.longwave}) {
        using Mode = openswmm::surface::RadiationSource::Mode;
        if (source.mode == Mode::File && met.empty())
            return fail("FILE radiation requires an hourly met file.");
        if (source.mode == Mode::Timeseries && ctx.find_timeseries(source.series) < 0)
            return fail("Radiation time series not found: " + source.series);
    }
    ctx.snowpacks.config = std::move(next);
    ctx.snowpacks.meteorology = std::move(met);
    if (cloud) {
        ctx.heat_config.cloud = next_cloud;
        if (next_cloud.configured)
            ctx.heat_config.configured = true;
    }
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_snow2d_get_options(SWMM_Engine engine, int* mode, double* step) {
    CHECK_HANDLE(engine);
    if (!mode || !step)
        return SWMM_ERR_BADPARAM;
    const auto& config = to_engine(engine)->context().snowpacks.mesh_config;
    *mode = static_cast<int>(config.mode);
    *step = config.step;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_snow2d_defaults_count(SWMM_Engine engine) {
    return engine ? static_cast<int>(
                        to_engine(engine)->context().snowpacks.mesh_config.defaults.size())
                  : 0;
}
SWMM_ENGINE_API int swmm_snow2d_cells_count(SWMM_Engine engine) {
    return engine
               ? static_cast<int>(to_engine(engine)->context().snowpacks.mesh_config.cells.size())
               : 0;
}
SWMM_ENGINE_API int swmm_snow2d_get_configuration(SWMM_Engine engine, int section, char* buffer,
                                                  int capacity, int* required) {
    CHECK_HANDLE(engine);
    if (section < 0 || section > 2 || !required || capacity < 0)
        return SWMM_ERR_BADPARAM;
    const auto& config = to_engine(engine)->context().snowpacks.mesh_config;
    const auto rows = section == 0 ? openswmm::surface::meshSnowOptionRows(config)
                                   : openswmm::surface::meshSnowRows(config, section == 2);
    std::string text;
    for (const auto& row : rows)
        text += row + '\n';
    if (text.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return SWMM_ERR_BADPARAM;
    *required = static_cast<int>(text.size() + 1);
    if (!buffer && capacity == 0)
        return SWMM_OK;
    return copy(text, buffer, capacity);
}
SWMM_ENGINE_API int swmm_snow2d_apply_configuration(SWMM_Engine engine, const char* options,
                                                    const char* defaults, const char* cells,
                                                    char* diagnostic, int capacity) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    if (!options || !defaults || !cells || !diagnostic || capacity <= 0)
        return SWMM_ERR_BADPARAM;
    diagnostic[0] = '\0';
    const auto fail = [&](const std::string& error) {
        const auto size = std::min(error.size(), static_cast<std::size_t>(capacity - 1));
        std::memcpy(diagnostic, error.data(), size);
        diagnostic[size] = '\0';
        return SWMM_ERR_BADPARAM;
    };
    openswmm::surface::MeshSnowConfig config;
    const char* sections[]{options, defaults, cells};
    for (int section = 0; section < 3; ++section) {
        std::istringstream input(sections[section]);
        std::string line;
        while (std::getline(input, line)) {
            const auto tokens = openswmm::input::Tokenizer::tokenize(line);
            if (tokens.empty())
                continue;
            const auto error =
                section == 0 ? openswmm::surface::setMeshSnowOption(config, tokens)
                             : openswmm::surface::setMeshSnowRow(config, tokens, section == 2);
            if (!error.empty())
                return fail(error);
        }
    }
    const auto validate = [&](const auto& rows) -> std::string {
        for (const auto& [key, row] : rows)
            if (!openswmm::ieq(row.pack, "NONE") && ctx.snowpack_names.find(row.pack) < 0)
                return "Unknown snowpack: " + row.pack;
        return {};
    };
    if (auto e = validate(config.defaults); !e.empty())
        return fail(e);
    if (auto e = validate(config.cells); !e.empty())
        return fail(e);
    ctx.snowpacks.mesh_config = std::move(config);
    return SWMM_OK;
}
} // extern C

extern "C" {
SWMM_ENGINE_API int swmm_snow2d_get_row(SWMM_Engine engine, int section, int index, int* cell,
                                        char* tag, int tag_capacity, char* pack, int pack_capacity,
                                        SWMM_MeshSnowOverrides* values) {
    CHECK_HANDLE(engine);
    if (!cell || !tag || !pack || !values || index < 0 || tag_capacity <= 0 || pack_capacity <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& config = to_engine(engine)->context().snowpacks.mesh_config;
    const openswmm::surface::MeshSnowRow* row = nullptr;
    std::string key;
    int c = -1;
    if (section == 1 && index < int(config.defaults.size())) {
        auto it = config.defaults.begin();
        std::advance(it, index);
        key = it->first;
        row = &it->second;
    }
    if (section == 2 && index < int(config.cells.size())) {
        auto it = config.cells.begin();
        std::advance(it, index);
        c = it->first;
        row = &it->second;
    }
    if (!row || key.size() >= std::size_t(tag_capacity) ||
        row->pack.size() >= std::size_t(pack_capacity))
        return SWMM_ERR_BADPARAM;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    *values = {row->forest.value_or(nan),      row->drift.value_or(nan),
               row->ground_heat.value_or(nan), row->extinction_depth.value_or(nan),
               row->slope.value_or(nan),       row->aspect.value_or(nan)};
    *cell = c;
    copy(key, tag, tag_capacity);
    copy(row->pack, pack, pack_capacity);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_snow2d_get_states(SWMM_Engine engine, int first, int count,
                                           SWMM_SnowState* output) {
    CHECK_HANDLE(engine);
#ifdef OPENSWMM_HAS_2D
    const auto& router = to_engine(engine)->surfaceRouter2D();
    const auto& snow = router.state().snow;
    if (!router.isActive() || first < 0 || count < 0 || first > router.mesh().n_cells() - count ||
        (count && !output))
        return SWMM_ERR_BADPARAM;
    const double nan = std::numeric_limits<double>::quiet_NaN(), dt = snow.lastSeconds();
    for (int k = 0; k < count; ++k) {
        const int i = first + k;
        SWMM_SnowState s{SWMM_SNOW_DEFAULT, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan};
        if (snow.owns(i)) {
            const auto& out = snow.outputs()[i];
            s.model = int(snow.kind(i));
            s.swe_m = out.swe_m;
            s.cover_fraction = out.cover_frac;
            s.melt_ms = dt > 0 ? out.melt_m / dt : 0;
            s.surface_input_ms = snow.liquidRates()[i];
            s.sublimation_ms = dt > 0 ? out.sublim_m / dt : 0;
            s.energy_kjm2 = out.energy_kJm2;
            s.surface_temperature_c = out.Ts_C;
            s.bulk_temperature_c = out.T_C;
            s.albedo = out.albedo;
            if (s.model == SWMM_SNOW_UEB)
                s.surface_age = snow.columns()[i].accepted.point.age;
        }
        output[k] = s;
    }
    return SWMM_OK;
#else
    (void)first;
    (void)count;
    (void)output;
    return SWMM_ERR_BADPARAM;
#endif
}
SWMM_ENGINE_API int swmm_snow2d_get_ledgers(SWMM_Engine engine, int first, int count,
                                            SWMM_MeshSnowLedger* output) {
    CHECK_HANDLE(engine);
#ifdef OPENSWMM_HAS_2D
    const auto& router = to_engine(engine)->surfaceRouter2D();
    const auto& snow = router.state().snow;
    if (!router.isActive() || first < 0 || count < 0 || first > router.mesh().n_cells() - count ||
        (count && !output))
        return SWMM_ERR_BADPARAM;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (int k = 0; k < count; ++k) {
        const int i = first + k;
        SWMM_MeshSnowLedger s{nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan, nan};
        if (snow.owns(i)) {
            const auto& b = snow.ledgers()[i];
            s = {snow.completedEnd(),
                 b.initial,
                 b.raw_precipitation,
                 b.precipitation,
                 b.liquid,
                 b.sublimation,
                 b.stored,
                 b.residual(),
                 snow.kind(i) == openswmm::surface::SnowKind::Ueb
                     ? b.net_energy
                     : std::numeric_limits<double>::quiet_NaN(),
                 snow.kind(i) == openswmm::surface::SnowKind::Ueb
                     ? b.initial_energy
                     : std::numeric_limits<double>::quiet_NaN(),
                 snow.kind(i) == openswmm::surface::SnowKind::Ueb
                     ? b.stored_energy
                     : std::numeric_limits<double>::quiet_NaN(),
                 snow.kind(i) == openswmm::surface::SnowKind::Ueb
                     ? b.energyResidual()
                     : std::numeric_limits<double>::quiet_NaN()};
        }
        output[k] = s;
    }
    return SWMM_OK;
#else
    (void)first;
    (void)count;
    (void)output;
    return SWMM_ERR_BADPARAM;
#endif
}

SWMM_ENGINE_API int swmm_snow2d_get_geometry_energy(SWMM_Engine engine, int first, int count,
                                                    double* output) {
    CHECK_HANDLE(engine);
#ifdef OPENSWMM_HAS_2D
    const auto& router = to_engine(engine)->surfaceRouter2D();
    const auto& snow = router.state().snow;
    if (!router.isActive() || first < 0 || count < 0 || first > router.mesh().n_cells() - count ||
        (count && !output))
        return SWMM_ERR_BADPARAM;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (int k = 0; k < count; ++k) {
        const int i = first + k;
        output[k] = snow.owns(i) && snow.kind(i) == openswmm::surface::SnowKind::Ueb
                        ? snow.ledgers()[i].geometry_energy
                        : nan;
    }
    return SWMM_OK;
#else
    (void)first;
    (void)count;
    (void)output;
    return SWMM_ERR_BADPARAM;
#endif
}
}
