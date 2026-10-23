// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SnowCheckpoint.cpp
 * @brief     Checkpoint persistence for snow model state.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SnowCheckpoint.hpp"
#include "MeshSnow.hpp"
#include "DegreeDaySnow.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/UnitConversion.hpp"
#include <cmath>
#include <stdexcept>

namespace openswmm::surface {
namespace {
using Archive = detail::CompletedSourceArchive;
/** @brief Enforce one snow archive invariant.
 * @param yes True accepts without work.
 * @param error Borrowed diagnostic used to construct a runtime_error on false.
 * @throws std::runtime_error Invariant failed. Prior caller/archive changes remain. */
void require(bool yes, const char* error) {
    if (!yes)
        throw std::runtime_error(error);
}
/** @brief Reject one nonfinite streamed snow value.
 * @param x Stored numeric value in its field's native units.
 * @throws std::runtime_error NaN or infinity. Does not impose sign/range bounds. */
void finite(double x) {
    require(std::isfinite(x), "Nonfinite snow checkpoint field.");
}
/** @brief Stream a fixed-shape numeric array, then check all entries finite.
 * @param[in,out] a Matching archive, with partial emission/consumption possible.
 * @param[in,out] v Already sized native-unit vector; no shape rebuilding here.
 * @throws std::exception Archive/allocation/finite validation error. Reading may have
 * already replaced entries; use a private candidate when publication must be atomic. */
void values(Archive& a, std::vector<double>& v) {
    a.fixed(v);
    for (double x : v)
        finite(x);
}
template <class... T> /** @brief Stream a numeric field pack and require finite values.
 * @tparam T Numeric archive-supported field types, convertible to double for checks.
 * @param[in,out] a Native reader/writer; can advance before a later check fails.
 * @param[in,out] v Native-unit scalar references, streamed in argument order.
 * Throws on archive/nonfinite fields without rolling back earlier fields. */
void scalars(Archive& a, T&... v) {
    a(v...);
    (finite(v), ...);
}
/** @brief Build exact native restart identity from model and forcing values.
 * @param ctx Compatible initialized configuration, names, pack geometry and weather.
 * @param s Compatible external ownership, surface fractions and degree-day constants.
 * @return Binary serialized identity including forcing values and ordered model
 * layout. No lookup cursor mutation or cryptographic hashing; copy/archive errors throw. */
std::string identity(const SimulationContext& ctx, const snow::SnowSoA& s) {
    Archive a;
    const auto& store = ctx.snowpacks;
    auto forcing = snowForcingIdentity(ctx);
    a(forcing);
    std::vector<std::string> names;
    for (int i = 0; i < ctx.n_subcatches(); ++i)
        names.push_back(ctx.subcatch_names.name_of(i));
    auto packs = ctx.subcatches.snowpack;
    auto areas = ctx.subcatches.area;
    auto pack_names = store.names;
    auto plow = store.plowable, imperv = store.impervious, perv = store.pervious;
    auto removal = store.removal;
    auto targets = store.removal_subcatch;
    a(names, packs, areas, pack_names, plow, imperv, perv, removal, targets);
    auto cfg = store.config;
    auto rows = uebOptionRows(cfg);
    a(cfg.model, cfg.step, cfg.division_temperature, rows);
    bool has_site = cfg.site.has_value();
    a(has_site);
    if (has_site)
        a(cfg.site->latitude, cfg.site->longitude, cfg.site->timezone, cfg.site->elevation);
    a(cfg.met_file.original, cfg.shortwave.authored, cfg.shortwave.mode, cfg.shortwave.series,
      cfg.longwave.authored, cfg.longwave.mode, cfg.longwave.series);
    auto external = s.external;
    auto fractions = s.fArea;
    a(external, fractions);
    for (int i = 0; i < store.count(); ++i) {
        const auto found = cfg.packs.find(i);
        const bool exists = found != cfg.packs.end();
        bool present = exists;
        a(present);
        if (exists) {
            auto p = found->second;
            a(p.model, p.initial_energy, p.site.forest, p.site.drift, p.site.ground_heat,
              p.site.extinction_depth);
        }
    }
    auto si = s.si;
    auto conservative = s.conservative_plowing;
    a(si, conservative);
    for (int k = 0; k < 10; ++k) {
        double imp = s.adc_imperv[k], perv = s.adc_perv[k];
        a(imp, perv);
    }
    double tipm = s.tipm, rnm = s.rnm;
    a(tipm, rnm);
    return a.bytes();
}
/** @brief Stream one point UEB inventory and bounded thermal history.
 * @param[in,out] a Matching native archive; errors can consume/emit a prefix.
 * @param[in,out] s Point kJ/m2 energy, metre water/refreezing depths, optical age and
 * thermal history (seconds and Celsius). Requires finite fields, nonnegative water/
 * age/depths, positive history durations, temperatures above absolute zero and total
 * history at most 86400+1e-7 seconds. Energy may be signed. No identity qualification;
 * errors can leave partial state and should be handled using a private candidate. */
void point(Archive& a, ueb::State& s) {
    scalars(a, s.energy, s.water, s.age, s.refreezing_depth, s.total_refreezing_depth);
    require(s.water >= 0 && s.age >= 0 && s.refreezing_depth >= 0 && s.total_refreezing_depth >= 0,
            "Invalid snow checkpoint inventory.");
    double duration = 0;
    a.sequence(s.history, [&](auto& h) {
        scalars(a, h.seconds, h.surface, h.bulk);
        require(h.seconds > 0 && h.surface > -273.15 && h.bulk > -273.15,
                "Invalid snow checkpoint thermal history.");
        duration += h.seconds;
    });
    require(duration <= 86400. + 1e-7, "Snow checkpoint thermal history exceeds 24 hours.");
}
/** @brief Stream a covered-point state and verify whole-footprint water consistency.
 * @param[in,out] a Native reader/writer; can advance before validation fails.
 * @param[in,out] s Point inventory plus whole-footprint water/maximum metres and cover.
 * Requires finite nonnegative water, maximum>=water, cover in [0,1], and point.water
 * *cover matching whole water within 1e-10 m. Does not independently qualify energy
 * closure or model geometry. Throws without local rollback on archive/invariant error. */
void lumped(Archive& a, ueb::LumpedState& s) {
    point(a, s.point);
    scalars(a, s.water, s.maximum, s.cover);
    require(s.water >= 0 && s.maximum >= s.water && s.cover >= 0 && s.cover <= 1 &&
                std::abs(s.point.water * s.cover - s.water) <= 1e-10,
            "Inconsistent snow checkpoint covered inventory.");
}
/** @brief Stream lumped/point integrated fluxes and diagnostics with basic bounds.
 * @param[in,out] a Native reader/writer; partial changes possible on error.
 * @param[in,out] s Stored integrated water depths m, energy kJ/m2, temperature Celsius,
 * cover/albedo and diagnostic counters. All numeric fields must be finite; lumped
 * rain/snow/liquid/throughfall nonnegative, active fraction/albedo in [0,1], counters
 * nonnegative. Sublimation and energy can be signed. This does not check full flux
 * closure or all point-water signs; throws on archive/basic validation failures. */
void flux(Archive& a, ueb::LumpedFluxes& s) {
    scalars(a, s.rain, s.snow, s.liquid, s.throughfall, s.sublimation, s.active_fraction,
            s.water_residual, s.net_energy, s.geometry_energy, s.energy_residual);
    require(s.rain >= 0 && s.snow >= 0 && s.liquid >= 0 && s.throughfall >= 0 &&
                s.active_fraction >= 0 && s.active_fraction <= 1,
            "Invalid snow checkpoint flux.");
    auto& p = s.point;
    scalars(a, p.rain, p.snow, p.liquid, p.sublimation, p.net_energy, p.outgoing_melt_energy,
            p.surface_temperature, p.bulk_temperature, p.albedo, p.water_residual,
            p.energy_residual);
    a(p.steps, p.retries, p.diagnostics);
    require(p.steps >= 0 && p.retries >= 0 && p.diagnostics >= 0 && p.albedo >= 0 && p.albedo <= 1,
            "Invalid snow checkpoint diagnostics.");
}
/** @brief Stream native UEB runtime, ledgers, held columns and reporting clocks.
 * @param[in,out] a Native reader/writer; fixed sequences require compatible extents.
 * @param[in,out] r Initialized runtime. Preserves native override key units, cached
 * rain/snow rates, integrated m3/kJ ledgers and seconds. Validates finite arrays/keys,
 * selected nonnegative water/time ledgers, all held states/last fluxes and sample
 * layout with 0<=sample_seconds<=sample_end+1e-5. Does not bind model identity itself.
 * Throws on failure with partial candidate/archive changes; caller owns rollback. */
void runtime(Archive& a, ueb::NativeState& r) {
    a(r.active, r.forcing);
    for (double value : r.forcing)
        finite(value);
    values(a, r.rain);
    values(a, r.snow);
    scalars(a, r.sublimation, r.precipitation, r.liquid, r.throughfall, r.net_energy,
            r.geometry_energy, r.plowed_energy, r.initial_water, r.removed, r.transferred_water,
            r.last_seconds, r.completed_seconds, r.initial_energy);
    require(r.precipitation >= 0 && r.liquid >= 0 && r.throughfall >= 0 && r.initial_water >= 0 &&
                r.removed >= 0 && r.last_seconds >= 0 && r.completed_seconds >= 0,
            "Invalid snow checkpoint ledger.");
    a.sequence(r.columns, [&](auto& h) { archiveHeldSnow(a, h); }, true);
    a.sequence(r.last, [&](auto& f) { flux(a, f); }, true);
    values(a, r.sample_end);
    values(a, r.sample_seconds);
    require(r.sample_end.size() == r.columns.size() && r.sample_seconds.size() == r.columns.size(),
            "Invalid snow sample clock layout.");
    for (std::size_t i = 0; i < r.sample_end.size(); ++i)
        require(r.sample_end[i] >= 0 && r.sample_seconds[i] >= 0 &&
                    r.sample_seconds[i] <= r.sample_end[i] + 1e-5,
                "Invalid snow sample clock.");
}
/** @brief Stream and validate native degree-day/report dynamic arrays.
 * @param[in,out] a Native fixed-shape reader/writer; partial changes can precede error.
 * @param[in,out] s Compatible initialized dynamic arrays (depths feet and stored
 * history/energy units). Streams transfer energy, melt/storage/cold/ATI/depletion/
 * age/outflow/external histories, season/removal/precipitation age and tracking flag.
 * Requires finite fields, nonnegative water/age/melt/removal/precipitation age and
 * cover in [0,1]. Does not serialize complete geometry, parameters or ownership;
 * throws without local rollback and assumes internally consistent vector extents. */
void legacy(Archive& a, snow::SnowSoA& s) {
    for (auto* v : {&s.ueb_transfer_energy, &s.dhm, &s.wsnow, &s.fw, &s.coldc, &s.ati, &s.awe,
                    &s.imelt, &s.age, &s.out_age, &s.sba, &s.sbws, &s.asc, &s.external_net})
        values(a, *v);
    scalars(a, s.season, s.removed, s.precip_age);
    a(s.track_age);
    require(s.removed >= 0 && s.precip_age >= 0, "Invalid snow checkpoint removal or age.");
    for (std::size_t i = 0; i < s.wsnow.size(); ++i)
        require(s.wsnow[i] >= 0 && s.fw[i] >= 0 && s.age[i] >= 0 && s.out_age[i] >= 0 &&
                    s.imelt[i] >= 0 && s.asc[i] >= 0 && s.asc[i] <= 1,
                "Invalid degree-day snow checkpoint state.");
}
}
std::array<double, 4> snowRuntimeForcing(const SimulationContext& ctx) {
    const auto& f = ctx.forcing;
    return {double(f.climate_temperature_mode),
            f.climate_temperature_mode == ForcingMode::NONE ? 0 : f.climate_temperature_value,
            double(f.climate_wind_mode),
            f.climate_wind_mode == ForcingMode::NONE ? 0 : f.climate_wind_value};
}
bool snowTransientForcing(const SimulationContext& ctx) {
    const auto& f = ctx.forcing;
    return (f.climate_temperature_mode != ForcingMode::NONE &&
            f.climate_temperature_persist == ForcingPersist::RESET) ||
           (f.climate_wind_mode != ForcingMode::NONE &&
            f.climate_wind_persist == ForcingPersist::RESET);
}
std::string snowModelIdentity(const SimulationContext& ctx, const snow::SnowSoA& snow) {
    return identity(ctx, snow);
}
std::string snowForcingIdentity(const SimulationContext& ctx) {
    Archive a;
    auto cfg = ctx.snowpacks.config;
    auto options = ctx.options;
    auto cloud = ctx.heat_config.cloud;
    auto rad = ctx.heat_config.radiative;
    auto solar = ctx.heat_config.solar;
    auto rows = uebOptionRows(cfg);
    a(rows, cfg.model, cfg.step, cfg.division_temperature, cfg.bc_a, cfg.bc_c);
    for (auto range : cfg.monthly_range) {
        bool present = range.has_value();
        a(present);
        if (present) {
            auto x = *range;
            a(x);
        }
    }
    bool site = cfg.site.has_value();
    a(site);
    if (site)
        a(cfg.site->latitude, cfg.site->longitude, cfg.site->timezone, cfg.site->elevation);
    a(cfg.shortwave.authored, cfg.shortwave.mode, cfg.shortwave.series, cfg.longwave.authored,
      cfg.longwave.mode, cfg.longwave.series);
    a(cloud.configured, cloud.use_timeseries, cloud.fraction, cloud.sw_atten_k, cloud.sw_atten_n,
      cloud.lw_cloud_k);
    a(solar.has_latitude, solar.latitude_deg, solar.has_longitude, solar.longitude_deg,
      solar.has_timezone, solar.timezone_hours, solar.has_elevation, solar.elevation_m,
      solar.ozone_cm, solar.precip_water_cm, solar.aod380, solar.aod500, solar.ground_albedo);
    auto exchange = ctx.heat_config.radiative_exchange;
    a(rad.sw_mode, rad.shortwave_wm2, options.heat_transport, exchange);
    a(options.temp_source, options.temp_ts_name, options.temp_file.original,
      options.temp_file_start, options.temp_units, options.wind_type, options.humidity_var,
      options.snow_divt, options.snow_lat, options.snow_dtlong, options.snow_elev);
    for (int m = 0; m < 12; ++m) {
        auto adjustment = ctx.adjust_temp[m];
        a(adjustment, options.wind_speed[m], options.humidity[m]);
    }
    // Bind values, not paths alone: replacing a weather file or editing a
    // referenced series must invalidate its already predicted snow interval.
    auto daily = ctx.snowpacks.daily_weather;
    a.sequence(daily, [&](auto& d) { a(d.date, d.tmin, d.tmax, d.previous_max, d.wind); });
    auto met = ctx.snowpacks.meteorology.records();
    a.sequence(met, [&](auto& row) { a(row.date, row.values); });
    for (int index : {ctx.climate_state.temp_ts_index, ctx.climate_state.humidity_ts_index,
                      cloud.ts_index, rad.sw_ts_index, ctx.find_timeseries(cfg.shortwave.series),
                      ctx.find_timeseries(cfg.longwave.series)}) {
        bool present = index >= 0 && index < int(ctx.tables.count());
        a(present);
        if (present) {
            auto table = ctx.tables[index];
            a(table.id, table.x, table.y);
        }
    }
    return a.bytes();
}
void archiveHeldSnow(detail::CompletedSourceArchive& a, ueb::HeldState& h) {
    lumped(a, h.accepted);
    lumped(a, h.begin);
    lumped(a, h.end);
    flux(a, h.flux);
    scalars(a, h.duration, h.elapsed);
    require(h.duration >= 0 && h.elapsed >= 0 && h.elapsed <= h.duration,
            "Invalid snow checkpoint held clock.");
    if (h.pending()) {
        const double progress = h.elapsed / h.duration;
        require(std::abs(h.accepted.water -
                         (h.begin.water + progress * (h.end.water - h.begin.water))) <= 1e-10,
                "Snow checkpoint held inventory differs from its elapsed balance.");
    }
}
void archiveDegreeDaySnow(detail::CompletedSourceArchive& a, snow::SnowSoA& s) {
    legacy(a, s);
}
std::string saveSnowCheckpoint(const SimulationContext& ctx, const snow::SnowSoA& s) {
    Archive a;
    std::string magic = "OpenSWMM native snow 2", model = identity(ctx, s);
    a(magic, model);
    auto copy = s;
    auto r = ctx.snowpacks.runtime;
    legacy(a, copy);
    runtime(a, r);
    return a.bytes();
}
std::string restoreSnowCheckpoint(SimulationContext& ctx, snow::SnowSoA& snow,
                                  const std::string& bytes, bool apply, bool seed) {
    try {
        if (bytes.empty()) {
            if (!apply)
                return {};
            for (auto* row :
                 {&snow.ueb_transfer_energy, &snow.wsnow, &snow.fw, &snow.coldc, &snow.imelt,
                  &snow.age, &snow.out_age, &snow.sba, &snow.sbws, &snow.asc, &snow.external_net})
                std::fill(row->begin(), row->end(), 0);
            snow.ati = snow.tbase;
            std::fill(snow.awe.begin(), snow.awe.end(), 1);
            snow.removed = 0;
            auto& runtime = ctx.snowpacks.runtime;
            ueb::NativeState fresh;
            fresh.active = runtime.active;
            fresh.columns.resize(runtime.columns.size());
            fresh.last.resize(runtime.last.size());
            fresh.sample_end.resize(runtime.sample_end.size());
            fresh.sample_seconds.resize(runtime.sample_seconds.size());
            fresh.rain.assign(runtime.rain.size(), -1);
            fresh.snow.assign(runtime.snow.size(), -1);
            runtime = std::move(fresh);
            std::fill(ctx.subcatches.snow_depth.begin(), ctx.subcatches.snow_depth.end(), 0);
            std::fill(ctx.subcatches.old_snow_depth.begin(), ctx.subcatches.old_snow_depth.end(),
                      0);
            return {};
        }
        Archive a(bytes);
        std::string magic, model;
        a(magic, model);
        require(magic == "OpenSWMM native snow 2", "Unsupported snow checkpoint format.");
        require(model == identity(ctx, snow),
                "Snow checkpoint assignments, geometry or parameters differ from this model.");
        auto next = snow;
        auto r = ctx.snowpacks.runtime;
        legacy(a, next);
        runtime(a, r);
        a.finish();
        for (int sc = 0; sc < next.n_subcatch; ++sc)
            if (next.external[sc])
                for (int k = 0; k < 3; ++k) {
                    const int i = sc * 3 + k;
                    require(std::abs((next.wsnow[i] + next.fw[i]) * .3048 -
                                     r.columns[i].accepted.water) <= 1e-10,
                            "Snow checkpoint report and UEB inventories differ.");
                }
        if (seed) {
            r.initial_water = r.initial_energy = 0;
            r.precipitation = r.liquid = r.throughfall = r.sublimation = r.removed =
                r.transferred_water = 0;
            r.net_energy = r.geometry_energy = r.plowed_energy = 0;
            r.last_seconds = r.completed_seconds = 0;
            next.removed = 0;
            std::fill(r.rain.begin(), r.rain.end(), -1);
            std::fill(r.snow.begin(), r.snow.end(), -1);
            for (auto& h : r.columns)
                ueb::closeHeld(h);
            for (auto& f : r.last)
                f = {};
            std::fill(r.sample_end.begin(), r.sample_end.end(), 0);
            std::fill(r.sample_seconds.begin(), r.sample_seconds.end(), 0);
            std::fill(next.imelt.begin(), next.imelt.end(), 0);
            std::fill(next.external_net.begin(), next.external_net.end(), 0);
            std::fill(next.out_age.begin(), next.out_age.end(), 0);
            for (int sc = 0; sc < next.n_subcatch; ++sc)
                if (next.external[sc]) {
                    const double lid = sc < int(ctx.subcatches.total_lid_area_ft2.size())
                                           ? ctx.subcatches.total_lid_area_ft2[sc]
                                           : 0;
                    const double area = std::max(0., ctx.subcatches.area[sc] /
                                                             ucf::UCF(ucf::LANDAREA, ctx.options) -
                                                         lid) *
                                        .3048 * .3048;
                    for (int k = 0; k < 3; ++k) {
                        const auto& l = r.columns[sc * 3 + k].accepted;
                        const double a = area * next.fArea[sc * 3 + k];
                        r.initial_water += l.water * a;
                        r.initial_energy += (l.cover > 0 ? l.cover : 1) * l.point.energy * a;
                    }
                }
        }
        if (apply) {
            snow = std::move(next);
            ctx.snowpacks.runtime = std::move(r);
            for (int sc = 0; sc < snow.n_subcatch; ++sc) {
                double depth = 0;
                for (int k = 0; k < 3; ++k) {
                    const int i = sc * 3 + k;
                    depth += (snow.wsnow[i] + snow.fw[i]) * snow.fArea[i];
                }
                ctx.subcatches.snow_depth[sc] = depth;
                if (seed)
                    ctx.subcatches.old_snow_depth[sc] = depth;
            }
        }
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
std::string saveCombinedSnowCheckpoint(const SimulationContext& ctx, const snow::SnowSoA& snow,
                                       const MeshSnow* mesh) {
    const bool assigned = std::any_of(ctx.subcatches.snowpack.begin(),
                                      ctx.subcatches.snowpack.end(), [](int p) { return p >= 0; });
    auto native = assigned ? saveSnowCheckpoint(ctx, snow) : std::string{};
    if (!mesh || !mesh->active())
        return native;
    auto cells = mesh->saveCheckpoint();
    Archive archive;
    std::string magic = "OpenSWMM combined snow 1";
    archive(magic, native, cells);
    return archive.bytes();
}
std::string restoreCombinedSnowCheckpoint(SimulationContext& ctx, snow::SnowSoA& snow,
                                          MeshSnow* mesh, const std::string& bytes, bool apply,
                                          bool seed) {
    try {
        std::string native = bytes, cells;
        if (!bytes.empty()) {
            Archive archive(bytes);
            std::string magic;
            archive(magic);
            if (magic == "OpenSWMM combined snow 1") {
                archive(native, cells);
                archive.finish();
            }
        }
        if (auto error = restoreSnowCheckpoint(ctx, snow, native, false, seed); !error.empty())
            return error;
        auto candidate = mesh ? *mesh : MeshSnow{};
        if (auto error = candidate.restoreCheckpoint(cells, seed); !error.empty())
            return error;
        if (apply) {
            if (auto error = restoreSnowCheckpoint(ctx, snow, native, true, seed); !error.empty())
                return error;
            if (mesh)
                *mesh = std::move(candidate);
            if (seed && mesh && mesh->active() && cells.empty() && !bytes.empty())
                ctx.warnings.push_back(
                    "Hot start lacks mesh snow state: assigned mesh snow cells start bare; future snow forcing is retained.");
        }
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
} // namespace openswmm::surface
