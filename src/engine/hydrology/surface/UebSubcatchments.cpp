// SPDX-License-Identifier: Apache-2.0

/**
 * @file      UebSubcatchments.cpp
 * @brief     Utah Energy Balance snow processing for subcatchments.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "core/ProcessPerf.hpp"
#include "UebSubcatchments.hpp"
#include "SnowCheckpoint.hpp"
#include "DegreeDaySnow.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/UnitConversion.hpp"
#include "../Radiation.hpp"
#include "../Gage.hpp"
#include <algorithm>
#include <cmath>

namespace openswmm::surface::ueb {
namespace {
/** @brief Copy the native depletion curve for one snow surface kind.
 * @param s Initialized native impervious/pervious ten-point curves.
 * @param surface Native surface enum; SNOW_PERV selects pervious, all others impervious.
 * @return Independent ten-point fraction array. No range/curve validation or mutation. */
std::array<double, 10> curveFor(const snow::SnowSoA& s, int surface) {
    std::array<double, 10> result;
    std::copy_n(surface == snow::SNOW_PERV ? s.adc_perv : s.adc_imperv, 10, result.begin());
    return result;
}
/** @brief Compute a native snow column's non-LID horizontal footprint.
 * @param ctx Compatible project area units and subcatchment geometry; missing LID
 * area rows are treated as zero.
 * @param s Compatible native surface fractions.
 * @param column Valid zero-based three-surface index; caller guarantees array extents.
 * @return max(0,project_area_in_ft2-lid_ft2)*fraction converted to m2. No independent
 * fraction/finite validation; native report geometry is not changed. */
double area(const SimulationContext& ctx, const snow::SnowSoA& s, int column) {
    const int sc = column / 3;
    const double lid = sc < int(ctx.subcatches.total_lid_area_ft2.size())
                           ? ctx.subcatches.total_lid_area_ft2[sc]
                           : 0;
    return std::max(0., ctx.subcatches.area[sc] / ucf::UCF(ucf::LANDAREA, ctx.options) - lid) *
           s.fArea[column] * .3048 * .3048;
}
/** @brief Mirror accepted UEB storage and latest flux into native report arrays.
 * @param[in,out] s Compatible initialized native arrays; selected row is replaced.
 * @param state Borrowed compatible accepted columns and latest integrated flux rows.
 * @param i Valid three-surface row in both owners.
 * @param dt Span seconds; positive converts liquid and liquid+throughfall m to ft/s,
 * nonpositive leaves prior rate fields unchanged. Water becomes whole-footprint feet,
 * free water zero, cover accepted fraction and negative energy cold-water equivalent.
 * Does not book balances, change UEB state or publish other report fields/clocks. */
void publish(snow::SnowSoA& s, const NativeState& state, int i, double dt) {
    const auto& column = state.columns[i].accepted;
    s.wsnow[i] = column.water / .3048;
    s.fw[i] = 0;
    s.asc[i] = column.cover;
    s.coldc[i] = std::max(0., -column.point.energy * column.cover) / (333500 * .3048);
    if (dt > 0) {
        s.imelt[i] = state.last[i].liquid / .3048 / dt;
        s.external_net[i] = (state.last[i].liquid + state.last[i].throughfall) / .3048 / dt;
    }
}
/** @brief Apply signed plowed whole-footprint water and enthalpy to one UEB receiver.
 * @param[in,out] held Consistent accepted owner; closes future prediction on success.
 * @param water Signed added water metres over receiver/donor horizontal footprint.
 * @param heat Signed added kJ/m2; added to existing cover-weighted represented energy.
 * @param curve Valid ten-point depletion fractions, used to resolve new cover.
 * @return Empty after replacement or invalid finite/water/receiving-cover diagnostic
 * retaining held state. Allows water down to -1e-12 m then clips to zero; keeps/reset
 * maximum with inventory and rescales point water/energy to cover. Does not age/mix
 * thermal history, book global ledgers or debit the counterpart owner. */
std::string transfer(HeldState& held, double water, double heat,
                     const std::array<double, 10>& curve) {
    auto next = held.accepted;
    const double total_energy = (next.cover > 0 ? next.cover : 1) * next.point.energy + heat;
    next.water += water;
    if (!std::isfinite(next.water) || next.water < -1e-12 || !std::isfinite(total_energy))
        return "Invalid plowed snow transfer.";
    next.water = std::max(0., next.water);
    next.maximum = next.water > 0 ? std::max(next.maximum, next.water) : 0;
    next.cover = next.water > 0 ? depletionCover(curve, next.water / next.maximum) : 0;
    if (next.water > 0 && next.cover <= 0)
        return "Plowed snow has no receiving covered area.";
    next.point.water = next.cover > 0 ? next.water / next.cover : 0;
    next.point.energy = total_energy / (next.cover > 0 ? next.cover : 1);
    held.accepted = std::move(next);
    closeHeld(held);
    return {};
}
/** @brief Move enabled UEB plowable inventory among removal, liquid and pack recipients.
 * @param ctx Compatible geometry/global UEB material parameters; borrowed unchanged.
 * @param[in,out] state Private staged columns, last fluxes and m3/kJ plowing/removal/
 * transfer ledgers. Consistent with soa before entry.
 * @param[in,out] soa Private staged native geometry/removal fractions/recipient indices
 * and age/report pools. Fractions must already be qualified at initialization.
 * @param dt Positive interval seconds used when publishing receiver/donor ft/s rates.
 * @return Empty on success or transfer diagnostic after possible staged-prefix
 * mutation; caller discards both candidates on failure. Applies threshold to donor
 * whole-footprint SWE, transfers snow/liquid enthalpy while retaining active soil
 * heat, mixes optional ages, skips invalid/self/nonpositive-area recipients without
 * removing their share, and updates DD cold content for non-UEB recipients. External
 * removal and liquid destinations leave UEB inventory; no engine/source publication
 * or physical external water debit occurs here. */
std::string plow(const SimulationContext& ctx, NativeState& state, snow::SnowSoA& soa, double dt) {
    const auto& p = ctx.snowpacks.config.parameters;
    for (int sc = 0; sc < soa.n_subcatch; ++sc) {
        if (!soa.enabled[sc] || !soa.external[sc])
            continue;
        const int donor = sc * 3;
        const double a = area(ctx, soa, donor);
        const auto inventory = state.columns[donor].accepted;
        if (a <= 0 || inventory.water < soa.weplow[sc] * .3048)
            continue;
        const double age = soa.age[donor];
        // Only snow/liquid enthalpy moves. The active soil heat capacity
        // stays at the donor even when all its snow is removed.
        const double temperature = bulkTemperature(inventory.point, p);
        const double specific =
            inventory.point.energy <= 0
                ? 1000 * 2.09 * temperature
                : (inventory.point.water > 0
                       ? std::min(333500., inventory.point.energy / inventory.point.water)
                       : 0);
        double moved = 0;
        for (int destination = 0; destination < 5; ++destination) {
            const double depth = soa.sfrac[sc * 5 + destination] * inventory.water;
            if (depth <= 0)
                continue;
            const double volume = depth * a, heat = volume * specific;
            if (destination == 0) {
                soa.removed += volume / (.3048 * .3048 * .3048);
                state.plowed_energy += heat;
                state.removed += volume;
            } else if (destination == 3) {
                auto& flux = state.last[donor];
                if (soa.track_age)
                    soa.out_age[donor] =
                        (flux.liquid * soa.out_age[donor] + depth * age) / (flux.liquid + depth);
                flux.liquid += depth;
                state.liquid += volume;
                state.plowed_energy += heat;
            } else {
                const int receiver = destination == 1   ? donor + 1
                                     : destination == 2 ? donor + 2
                                                        : soa.to_subcatch[sc] * 3 + 2;
                if (receiver < 0 || receiver >= int(state.columns.size()) || receiver == donor)
                    continue;
                const double receiving_area = area(ctx, soa, receiver);
                if (receiving_area <= 0)
                    continue;
                const double added = volume / receiving_area;
                const double previous = soa.wsnow[receiver] * .3048 + soa.fw[receiver] * .3048;
                if (soa.track_age)
                    soa.age[receiver] =
                        (previous * soa.age[receiver] + added * age) / (previous + added);
                if (soa.external[receiver / 3]) {
                    if (auto error = transfer(state.columns[receiver], added, heat / receiving_area,
                                              curveFor(soa, receiver % 3));
                        !error.empty())
                        return error;
                    publish(soa, state, receiver, dt);
                } else {
                    // The degree-day model carries cold content, not an
                    // energy state. Retain the cold part of transferred snow.
                    soa.wsnow[receiver] += added / .3048;
                    soa.coldc[receiver] += std::max(0., -heat / receiving_area) / (333500 * .3048);
                    state.plowed_energy += heat;
                    state.transferred_water -= volume;
                }
            }
            moved += depth;
        }
        if (moved > 0) {
            if (auto error =
                    transfer(state.columns[donor], -moved, -moved * specific, curveFor(soa, 0));
                !error.empty())
                return error;
            if (state.columns[donor].accepted.water <= 0)
                soa.age[donor] = 0;
            publish(soa, state, donor, dt);
        }
    }
    return {};
}
}
std::string initializeSubcatchments(SimulationContext& ctx, snow::SnowSoA& s) {
    NativeState next;
    next.columns.resize(s.wsnow.size());
    next.last.resize(s.wsnow.size());
    next.sample_end.resize(s.wsnow.size());
    next.sample_seconds.resize(s.wsnow.size());
    next.rain.assign(s.n_subcatch, -1);
    next.snow.assign(s.n_subcatch, -1);
    auto external = s.external;
    const auto& config = ctx.snowpacks.config;
    for (int sc = 0; sc < s.n_subcatch; ++sc) {
        const int pack = ctx.subcatches.snowpack[sc];
        external[sc] = pack >= 0 && config.modelFor(pack) == SnowKind::Ueb;
        if (!external[sc])
            continue;
        next.active = true;
        double fractions = 0;
        for (int k = 0; k < 5; ++k) {
            const double fraction = s.sfrac[sc * 5 + k];
            if (!std::isfinite(fraction) || fraction < 0 || fraction > 1)
                return "Invalid UEB snow removal fraction.";
            fractions += fraction;
        }
        if (fractions > 1 + 1e-12)
            return "UEB snow removal fractions exceed one.";
        const auto found = config.packs.find(pack);
        const double initial_energy =
            found == config.packs.end() ? 0 : found->second.initial_energy;
        for (int k = 0; k < 3; ++k) {
            const int i = sc * 3 + k;
            if (s.fArea[i] <= 0)
                continue;
            const auto curve = curveFor(s, k);
            const auto& surfaces = k == 0   ? ctx.snowpacks.plowable
                                   : k == 1 ? ctx.snowpacks.impervious
                                            : ctx.snowpacks.pervious;
            const double free_water =
                pack < int(surfaces.size())
                    ? surfaces[pack][5] / ucf::UCF(ucf::RAINDEPTH, ctx.options) * .3048
                    : s.fw[i] * .3048;
            const double water = s.wsnow[i] * .3048 + free_water;
            const double energy = initial_energy + free_water * 333500;
            if (auto error = initializeLumped(next.columns[i].accepted, water, energy,
                                              s.si[i] * .3048, curve);
                !error.empty())
                return "Snowpack " + ctx.snowpacks.names[pack] + ": " + error;
            next.initial_water += water * area(ctx, s, i);
            const auto& l = next.columns[i].accepted;
            next.initial_energy += (l.cover > 0 ? l.cover : 1) * l.point.energy * area(ctx, s, i);
        }
    }
    ctx.snowpacks.runtime = std::move(next);
    s.external = std::move(external);
    for (int sc = 0; sc < s.n_subcatch; ++sc)
        if (s.external[sc])
            for (int k = 0; k < 3; ++k)
                publish(s, ctx.snowpacks.runtime, sc * 3 + k, 0);
    return {};
}
double nextSnowBoundary(const SimulationContext& ctx, double elapsed, double proposed) noexcept {
    if (!ctx.snowpacks.runtime.active || ctx.options.ignore_snow_melt)
        return proposed;
    const double step = ctx.snowpacks.config.step;
    const double next = (std::floor((elapsed + 1e-7) / step) + 1) * step;
    proposed = std::min(proposed, next - elapsed);
    if (!ctx.snowpacks.meteorology.empty()) {
        const double start = ctx.options.start_date;
        const double hour = (start - std::floor(start)) * 86400. + elapsed;
        const double boundary = (std::floor((hour + 1e-5) / 3600.) + 1) * 3600.;
        proposed = std::min(proposed, boundary - hour);
    }
    return proposed;
}
std::string advanceSubcatchments(SimulationContext& ctx, snow::SnowSoA& soa, double date,
                                 double elapsed, double seconds, const double* rain,
                                 const double* snow) {
    perf::ProcessTimer process_timer(perf::ProcessPhase::subcatch_snow);
    if (!ctx.snowpacks.runtime.active)
        return {};
    auto next = ctx.snowpacks.runtime;
    auto staged = soa;
    const auto forcing_key = snowRuntimeForcing(ctx);
    if (forcing_key != next.forcing)
        for (auto& column : next.columns)
            closeHeld(column);
    next.forcing = forcing_key;
    const auto& config = ctx.snowpacks.config;
    double evaporation_ft3 = 0;
    for (int sc = 0; sc < soa.n_subcatch; ++sc) {
        if (!soa.enabled[sc] || !soa.external[sc])
            continue;
        const int pack = ctx.subcatches.snowpack[sc];
        Site site;
        const auto found = config.packs.find(pack);
        if (found != config.packs.end())
            site = found->second.site;
        site.drift = 1; // already applied by the shared gage partition
        double horizon = nextSnowBoundary(ctx, elapsed, config.step);
        const int gage = ctx.subcatches.gage[sc];
        if (gage >= 0) {
            const double limit = date + horizon / 86400.;
            const double change = gage::gageNextRateChange(ctx, gage, date, limit);
            // Keep the exact seconds when no gage boundary shortened them;
            // converting an OLE day difference loses sub-microsecond bits.
            if (change < limit && change > date + 1e-10)
                horizon = std::min(horizon, (change - date) * 86400.);
        }
        horizon = std::min(horizon, ctx.options.totalDurationMs() / 1000. - elapsed);
        if (snowTransientForcing(ctx))
            horizon = std::min(horizon, seconds);
        if (horizon < seconds && seconds - horizon < 1e-5)
            horizon = seconds;
        if (horizon < seconds)
            return "Runoff step crosses a UEB forcing boundary.";
        Forcing forcing;
        bool resolved = false;
        for (int k = 0; k < 3; ++k) {
            const int i = sc * 3 + k;
            next.last[i] = {};
            if (soa.fArea[i] <= 0)
                continue;
            auto& held = next.columns[i];
            // The legacy plow pass can deliver snow from a degree-day donor.
            // It has already mixed the donor age into the reporting pool.
            const double incoming = (soa.wsnow[i] + soa.fw[i]) * .3048 - held.accepted.water;
            if (std::abs(incoming) > 1e-12) {
                const double heat = staged.ueb_transfer_energy[i];
                if (auto error = transfer(held, incoming, heat, curveFor(soa, k)); !error.empty())
                    return error;
                staged.ueb_transfer_energy[i] = 0;
                next.transferred_water += incoming * area(ctx, soa, i);
                next.plowed_energy -= heat * area(ctx, soa, i);
            }
            if (next.rain[sc] != rain[sc] || next.snow[sc] != snow[sc])
                closeHeld(held);
            if (!held.pending()) {
                if (!resolved) {
                    if (auto error = climate::resolveUebForcing(ctx, date, horizon, 0, 0, forcing,
                                                                site.pressure);
                        !error.empty())
                        return error;
                    forcing.precipitation = (rain[sc] + snow[sc]) * .3048 * horizon;
                    forcing.snow_fraction =
                        rain[sc] + snow[sc] > 0 ? snow[sc] / (rain[sc] + snow[sc]) : 0;
                    forcing.snow_catch = 1;
                    resolved = true;
                }
                if (auto error =
                        prepareHeld(held, config.parameters, site, forcing, curveFor(soa, k));
                    !error.empty())
                    return "UEB subcatchment " + std::to_string(sc) + ": " + error;
            }
            auto& flux = next.last[i];
            if (auto error = consumeHeld(held, seconds, curveFor(soa, k), flux); !error.empty())
                return error;
            next.sample_end[i] = elapsed + seconds;
            next.sample_seconds[i] = seconds;
            const double a = area(ctx, soa, i);
            next.precipitation += (flux.rain + flux.snow) * a;
            next.liquid += flux.liquid * a;
            next.throughfall += flux.throughfall * a;
            next.sublimation += flux.sublimation * a;
            next.net_energy += flux.net_energy * a;
            next.geometry_energy += flux.geometry_energy * a;
            evaporation_ft3 += flux.sublimation * a / (.3048 * .3048 * .3048);
            if (staged.track_age) {
                const double initial = soa.wsnow[i] + soa.fw[i];
                const double arrived = (flux.rain + flux.snow - flux.throughfall) / .3048;
                const double volume = initial + arrived;
                const double age =
                    volume > 0
                        ? (initial * (soa.age[i] + seconds) + arrived * soa.precip_age) / volume
                        : 0;
                staged.out_age[i] = flux.liquid > 0 ? age : 0;
                staged.age[i] = held.accepted.water > 0 ? age : 0;
            }
            publish(staged, next, i, seconds);
        }
        next.rain[sc] = rain[sc];
        next.snow[sc] = snow[sc];
    }
    if (auto error = plow(ctx, next, staged, seconds); !error.empty())
        return error;
    next.last_seconds = seconds;
    next.completed_seconds = elapsed + seconds;
    ctx.snowpacks.runtime = std::move(next);
    soa = std::move(staged);
    if (evaporation_ft3 >= 0)
        ctx.mass_balance.runoff_evap += evaporation_ft3;
    else
        ctx.mass_balance.runoff_rainfall -= evaporation_ft3; // atmospheric deposition is an input
    return {};
}
} // namespace openswmm::surface::ueb
