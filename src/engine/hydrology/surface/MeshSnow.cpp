// SPDX-License-Identifier: Apache-2.0

/**
 * @file      MeshSnow.cpp
 * @brief     Snow state and advancement for mesh cells.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "core/ProcessPerf.hpp"
#include "core/ThreadInfo.hpp"
#include <exception>
#ifdef SWMM_USE_OPENMP
#include <omp.h>
#endif
#include "MeshSnow.hpp"
#include "SnowCheckpoint.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../Radiation.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/DateTime.hpp"
#include "../../core/UnitConversion.hpp"
#include "../../input/Tokenizer.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace openswmm::surface {
namespace {
constexpr double nan = std::numeric_limits<double>::quiet_NaN();
/** @brief Read accepted whole-footprint represented energy per unit horizontal area.
 * @param held Consistent held UEB state; future predicted end is ignored.
 * @return accepted.cover*accepted.point.energy, or full active-soil energy when
 * snow-free, in kJ/m2. No validation or state/clock mutation. */
double representedEnergy(const ueb::HeldState& held) {
    const auto& state = held.accepted;
    return (state.cover > 0 ? state.cover : 1.) * state.point.energy;
}
}
std::string MeshSnow::initialize(SimulationContext& ctx,
                                 const std::vector<MeshSnowGeometry>& geometry) {
    MeshSnow next;
    const auto& config = ctx.snowpacks.mesh_config;
    if (!ctx.options.ignore_snow_melt && config.mode == MeshSnowConfig::Mode::Yes &&
        !config.assigned())
        return "Mesh SNOWMELT YES requires a snowpack assignment.";
    if (ctx.options.ignore_snow_melt || config.mode == MeshSnowConfig::Mode::No ||
        !config.assigned()) {
        *this = std::move(next);
        return {};
    }
    const int n = static_cast<int>(geometry.size());
    for (const auto& [cell, row] : config.cells)
        if (cell < 0 || cell >= n)
            return "Mesh snow assignment references a missing cell.";
    next.geometry_ = geometry;
    next.step_ = config.step > 0 ? config.step : ctx.snowpacks.config.step;
    next.pack_.assign(n, -1);
    next.kind_.resize(n);
    next.site_.resize(n);
    next.held_.resize(n);
    next.precipitation_.assign(n, -1);
    next.catch_.assign(n, -1);
    next.liquid_rate_.assign(n, 0);
    next.exposed_.assign(n, 1);
    next.ground_contact_.assign(n, 0);
    next.output_.resize(n);
    next.ledger_.resize(n);
    next.degree_day_.init(n);
    auto& dd = next.degree_day_.state();
    dd.tipm = ctx.options.snow_ati_wt;
    dd.rnm = ctx.options.snow_nrg_ratio;
    next.parameters_ = ctx.snowpacks.config.parameters;
    std::copy_n(ctx.options.adc_perv, 10, next.depletion_curve_.begin());
    const int units = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    for (int i = 0; i < n; ++i) {
        const auto row = resolveMeshSnowRow(config, i, geometry[i].tag);
        if (row.pack.empty() || input::Tokenizer::to_upper(row.pack) == "NONE" ||
            geometry[i].area == 0)
            continue;
        const int pack = ctx.snowpack_names.find(row.pack);
        if (pack < 0 || pack >= int(ctx.snowpacks.pervious.size()))
            return "Unknown mesh snowpack: " + row.pack;
        auto& g = next.geometry_[i];
        if (row.slope)
            g.slope = *row.slope;
        if (row.aspect)
            g.aspect = *row.aspect;
        if (!std::isfinite(g.area) || g.area < 0 || !std::isfinite(g.slope) || g.slope < 0 ||
            g.slope >= 90 || !std::isfinite(g.aspect) || g.aspect < 0 || g.aspect > 360)
            return "Invalid mesh snow geometry.";
        next.active_cells_.push_back(i);
        next.pack_[i] = pack;
        const auto model = ctx.snowpacks.config.modelFor(pack);
        next.kind_[i] = model;
        auto& site = next.site_[i];
        double initial_energy = 0;
        if (auto it = ctx.snowpacks.config.packs.find(pack);
            it != ctx.snowpacks.config.packs.end()) {
            site = it->second.site;
            initial_energy = it->second.initial_energy;
        }
        if (row.forest)
            site.forest = *row.forest;
        if (row.drift)
            site.drift = *row.drift;
        if (row.ground_heat)
            site.ground_heat = *row.ground_heat;
        if (row.extinction_depth)
            site.extinction_depth = *row.extinction_depth;
        const auto& p = ctx.snowpacks.pervious[pack];
        const double factor = .3048 / ucf::Ucf[ucf::RAINDEPTH][units];
        double initial = 0;
        if (model == SnowKind::Ueb) {
            const double free_water = p[5] / ucf::Ucf[ucf::RAINDEPTH][units] * .3048;
            initial = p[4] / ucf::Ucf[ucf::RAINDEPTH][units] * .3048 + free_water;
            if (auto error = ueb::initializeLumped(next.held_[i].accepted, initial,
                                                   initial_energy + free_water * 333500, 0,
                                                   next.depletion_curve_);
                !error.empty())
                return error;
            next.output_[i].energy_kJm2 = next.held_[i].accepted.point.energy;
            next.ledger_[i].initial_energy = next.ledger_[i].stored_energy =
                next.output_[i].energy_kJm2 * geometry[i].area;
            next.output_[i].T_C =
                ueb::bulkTemperature(next.held_[i].accepted.point, ctx.snowpacks.config.parameters);
            next.output_[i].Ts_C = std::min(0., next.output_[i].T_C);
            next.output_[i].albedo = nan;
        } else {
            const int e = i * 3 + 2;
            dd.dhmin[e] = p[0] * ucf::Ucf[ucf::TEMPERATURE][units] / ucf::Ucf[ucf::RAINFALL][units];
            dd.dhmax[e] = p[1] * ucf::Ucf[ucf::TEMPERATURE][units] / ucf::Ucf[ucf::RAINFALL][units];
            dd.tbase[e] = units == 1 ? p[2] * 1.8 + 32 : p[2];
            dd.ati[e] = dd.tbase[e];
            dd.fwfrac[e] = p[3];
            dd.wsnow[e] = p[4] * factor / .3048;
            dd.fw[e] = std::min(p[5], p[3] * p[4]) * factor / .3048;
            dd.fArea[e] = 1;
            initial = (dd.wsnow[e] + dd.fw[e]) * .3048;
            next.output_[i].energy_kJm2 = next.output_[i].T_C = next.output_[i].Ts_C =
                next.output_[i].albedo = nan;
        }
        next.output_[i].swe_m = initial;
        next.output_[i].cover_frac = initial > 0 ? 1 : 0;
        next.exposed_[i] = initial > 0 ? 0 : 1;
        next.ledger_[i].initial = next.ledger_[i].stored = initial * g.area;
    }
    if (config.mode == MeshSnowConfig::Mode::Yes && !next.active())
        return "Mesh SNOWMELT YES has no assigned weather footprint.";
    detail::CompletedSourceArchive legacy_identity;
    auto parameters = uebOptionRows(ctx.snowpacks.config);
    auto options = meshSnowOptionRows(config);
    auto defaults = meshSnowRows(config, false), cells = meshSnowRows(config, true);
    auto weather = snowForcingIdentity(ctx);
    legacy_identity(parameters, options, defaults, cells, next.pack_, next.kind_, next.step_,
                    weather);
    for (int p : next.pack_) {
        double initial_energy = 0;
        if (auto it = ctx.snowpacks.config.packs.find(p); it != ctx.snowpacks.config.packs.end())
            initial_energy = it->second.initial_energy;
        legacy_identity(initial_energy);
    }
    legacy_identity.sequence(next.geometry_,
                             [&](auto& g) { legacy_identity(g.tag, g.area, g.slope, g.aspect); });
    legacy_identity.sequence(next.site_, [&](auto& site) {
        legacy_identity(site.forest, site.drift, site.ground_heat, site.extinction_depth);
    });
    auto pervious = ctx.snowpacks.pervious;
    legacy_identity(pervious, dd.tipm, dd.rnm);
    next.legacy_identity_ = legacy_identity.bytes();
    const bool uses_ueb = std::any_of(next.active_cells_.begin(), next.active_cells_.end(),
                                      [&](int cell) { return next.kind_[cell] == SnowKind::Ueb; });
    detail::CompletedSourceArchive identity;
    if (uses_ueb)
        identity(parameters, options, defaults, cells, next.pack_, next.kind_, next.step_,
                 next.depletion_curve_, weather);
    else
        identity(parameters, options, defaults, cells, next.pack_, next.kind_, next.step_, weather);
    for (int p : next.pack_) {
        double initial_energy = 0;
        if (auto it = ctx.snowpacks.config.packs.find(p); it != ctx.snowpacks.config.packs.end())
            initial_energy = it->second.initial_energy;
        identity(initial_energy);
    }
    identity.sequence(next.geometry_, [&](auto& g) { identity(g.tag, g.area, g.slope, g.aspect); });
    identity.sequence(next.site_, [&](auto& site) {
        identity(site.forest, site.drift, site.ground_heat, site.extinction_depth);
    });
    identity(pervious, dd.tipm, dd.rnm);
    next.identity_ = identity.bytes();
    *this = std::move(next);
    return {};
}
std::string MeshSnow::advance(SimulationContext& ctx, double start, double end,
                              const std::vector<double>& rain,
                              const std::vector<double>& catch_factor) {
    return advanceTrial(ctx, start, end, rain, catch_factor, nullptr);
}
std::string MeshSnow::advanceTrial(SimulationContext& ctx, double start, double end,
                                   const std::vector<double>& rain,
                                   const std::vector<double>& catch_factor,
                                   const MeshSnow* configuration) {
    perf::ProcessTimer process_timer(perf::ProcessPhase::mesh_snow);
    if (!active())
        return {};
    if (!std::isfinite(start) || !std::isfinite(end) || start != completed_ || end <= start ||
        rain.size() != pack_.size() || catch_factor.size() != pack_.size())
        return "Invalid mesh snow interval or forcing layout.";
    // The source scheduler can close an interval with a sub-nanosecond
    // floating-point remainder at an exact synchronization boundary. The
    // snow loop intentionally skips such a span; keep the accepted coverage
    // and fluxes rather than converting the empty interval into a bare cell.
    if (end - start <= 1e-9) {
        completed_ = end;
        return {};
    }
    if (configuration)
        return advanceInPlace(*configuration, ctx, start, end, rain, catch_factor);
    perf::count(perf::ProcessCounter::mesh_snow_copies);
    MeshSnow candidate;
    // Copy only mutable runtime; this owner supplies stable initialized configuration.
    candidate.forcing_ = forcing_;
    candidate.completed_ = completed_;
    candidate.last_seconds_ = last_seconds_;
    candidate.site_ = site_;
    candidate.held_ = held_;
    candidate.precipitation_ = precipitation_;
    candidate.catch_ = catch_;
    candidate.liquid_rate_ = liquid_rate_;
    candidate.exposed_ = exposed_;
    candidate.output_ = output_;
    candidate.ledger_ = ledger_;
    candidate.degree_day_ = degree_day_;
    if (auto error = candidate.advanceInPlace(*this, ctx, start, end, rain, catch_factor);
        !error.empty())
        return error;
    forcing_ = std::move(candidate.forcing_);
    completed_ = std::move(candidate.completed_);
    last_seconds_ = std::move(candidate.last_seconds_);
    site_ = std::move(candidate.site_);
    held_ = std::move(candidate.held_);
    precipitation_ = std::move(candidate.precipitation_);
    catch_ = std::move(candidate.catch_);
    liquid_rate_ = std::move(candidate.liquid_rate_);
    exposed_ = std::move(candidate.exposed_);
    output_ = std::move(candidate.output_);
    ledger_ = std::move(candidate.ledger_);
    degree_day_ = std::move(candidate.degree_day_);
    return {};
}
void MeshSnow::useAcceptedExposure() {
    for (int cell : active_cells_)
        exposed_[cell] = 1 - output_[cell].cover_frac;
}
void MeshSnow::reanchorCompletedEnd(double end) {
    const double tolerance = 8 * std::numeric_limits<double>::epsilon() *
                             std::max({1., std::abs(end), std::abs(completed_)});
    if (!std::isfinite(end) || end < 0 || std::abs(end - completed_) > tolerance)
        throw std::invalid_argument("Snow source clock is not an ulp-sized driver landing.");
    completed_ = end;
}
std::string MeshSnow::setGroundContactMask(const std::vector<unsigned char>& mask) {
    if (mask.size() != pack_.size())
        return "Mesh snow ground-contact mask shape differs from the mesh.";
    for (std::size_t i = 0; i < mask.size(); ++i) {
        if (mask[i] > 1)
            return "Mesh snow ground-contact mask is not Boolean.";
        if (mask[i] && (!owns(int(i)) || kind_[i] != SnowKind::Ueb))
            return "Reciprocal mesh ground contact is only supported for assigned UEB cells.";
    }
    ground_contact_ = mask;
    return {};
}
std::string MeshSnow::applyGroundContactEnergy(int cell, double soil_j) {
    if (!std::isfinite(soil_j))
        return "Nonfinite reciprocal UEB/soil contact energy.";
    if (!groundContactEnabled(cell) || !owns(cell) || kind_[cell] != SnowKind::Ueb)
        return "Reciprocal UEB/soil energy was sent to a cell without contact ownership.";
    if (soil_j == 0)
        return {};
    auto& held = held_[cell];
    auto& output = output_[cell];
    auto& book = ledger_[cell];
    auto& point = held.accepted.point;
    const double area = geometry_[cell].area;
    const double represented_fraction = held.accepted.cover > 0 ? held.accepted.cover : 1.;
    if (!(area > 0) || !(represented_fraction > 0) || !std::isfinite(point.energy))
        return "UEB contact cell has no finite energy storage area.";
    const double snow_j = -soil_j;
    const double delta_kj_m2 = snow_j / (1000. * area * represented_fraction);
    const double old_bulk = ueb::bulkTemperature(point, parameters_);
    const double next_energy = point.energy + delta_kj_m2;
    if (!std::isfinite(delta_kj_m2) || !std::isfinite(next_energy))
        return "Reciprocal UEB/soil contact overflows snow energy.";
    auto next_point = point;
    next_point.energy = next_energy;
    const double next_bulk = ueb::bulkTemperature(next_point, parameters_);
    const double delta_temperature = next_bulk - old_bulk;
    const double next_surface = point.history.empty()
                                    ? std::min(0., next_bulk)
                                    : point.history.back().surface + delta_temperature;
    const double delta_total = snow_j / 1000.;
    const double next_contact = book.ground_contact_energy + delta_total;
    const double next_net = book.net_energy + delta_total,
                 next_stored = book.stored_energy + delta_total;
    const double residual = book.initial_energy + next_net - next_stored;
    if (!std::isfinite(next_bulk) || !std::isfinite(next_surface) || !std::isfinite(next_contact) ||
        !std::isfinite(next_net) || !std::isfinite(next_stored) ||
        std::abs(residual) >
            1e-8 * std::max({1., std::abs(book.initial_energy), std::abs(next_net)}))
        return "Reciprocal UEB/soil energy balance failed.";
    if (!next_point.history.empty()) {
        next_point.history.back().bulk = next_bulk;
        next_point.history.back().surface = next_surface;
    }
    point = std::move(next_point);
    output.T_C = next_bulk;
    output.Ts_C = next_surface;
    output.energy_kJm2 = point.energy;
    book.ground_contact_energy = next_contact;
    book.net_energy = next_net;
    book.stored_energy = next_stored;
    if (held.pending())
        ueb::closeHeld(held);
    return {};
}
std::string MeshSnow::advanceInPlace(const MeshSnow& configuration, SimulationContext& ctx,
                                     double start, double end,
                                     const std::vector<double>& rain,
                                     const std::vector<double>& catch_factor) {
    const auto& config = ctx.snowpacks.config;
    const auto forcing_key = snowRuntimeForcing(ctx);
    if (forcing_key != forcing_)
        for (auto& column : held_)
            ueb::closeHeld(column);
    forcing_ = forcing_key;
#ifdef SWMM_USE_OPENMP
    const int threads = configuration.active_cells_.size() >= 512 && ctx.options.num_threads != 1 &&
                                !omp_in_parallel()
                            ? (ctx.options.num_threads > 0 ? ctx.options.num_threads
                                                          : threadinfo::ompMaxThreads())
                            : 1;
    if (threads > 1) {
        const double span = end - start;
        const double boundary =
            (std::floor((start + 1e-7) / configuration.step_) + 1) * configuration.step_;
        double horizon = std::min(boundary - start, ctx.options.totalDurationMs() / 1000. - start);
        if (!ctx.snowpacks.meteorology.empty()) {
            const double hour =
                (ctx.options.start_date - std::floor(ctx.options.start_date)) * 86400. + start;
            horizon = std::min(horizon, (std::floor((hour + 1e-5) / 3600.) + 1) * 3600. - hour);
        }
        if (snowTransientForcing(ctx))
            horizon = std::min(horizon, span);
        // Only consume an existing single UEB prediction. Preparing weather
        // changes shared table cursors/radiation; contact and DD stay scalar.
        const bool prepared = start < end - 1e-9 && !(start + span < end - 1e-9) &&
            span <= horizon &&
            std::all_of(configuration.active_cells_.begin(), configuration.active_cells_.end(),
                        [&](int i) {
                            return configuration.kind_[i] == SnowKind::Ueb &&
                                   !configuration.groundContactEnabled(i) && held_[i].pending() &&
                                   span <= held_[i].duration - held_[i].elapsed &&
                                   std::isfinite(rain[i]) && rain[i] >= 0 &&
                                   std::isfinite(catch_factor[i]) && catch_factor[i] >= 0 &&
                                   precipitation_[i] == rain[i] && catch_[i] == catch_factor[i];
                        });
        if (prepared) {
            auto consumeCell = [&](int i) -> std::string {
                auto& held = held_[i];
                auto& result = output_[i];
                auto& book = ledger_[i];
                result.melt_m = result.throughfall_m = result.sublim_m = 0;
                const double dt = span, prescribed_qg = site_[i].ground_heat;
                double covered_seconds = 0;
                ueb::LumpedFluxes flux;
                if (auto error = ueb::consumeHeld(held, dt, configuration.depletion_curve_, flux); !error.empty())
                    return error;
                covered_seconds += flux.active_fraction * dt;
                result.melt_m += flux.liquid;
                result.throughfall_m += flux.throughfall;
                result.sublim_m += flux.sublimation;
                result.swe_m = held.accepted.water;
                result.cover_frac = held.accepted.cover;
                result.energy_kJm2 = held.accepted.point.energy;
                result.albedo = flux.point.albedo;
                result.T_C = ueb::bulkTemperature(held.accepted.point, config.parameters);
                result.Ts_C = held.accepted.point.history.empty()
                                  ? std::min(0., result.T_C)
                                  : held.accepted.point.history.back().surface;
                book.precipitation += (flux.rain + flux.snow) * configuration.geometry_[i].area;
                book.net_energy += flux.net_energy * configuration.geometry_[i].area;
                book.prescribed_ground_energy +=
                    prescribed_qg * flux.active_fraction * dt * configuration.geometry_[i].area / 1000.;
                book.geometry_energy += flux.geometry_energy * configuration.geometry_[i].area;
                const double area = configuration.geometry_[i].area;
                book.raw_precipitation += rain[i] * (end - start) * area;
                book.liquid += (result.melt_m + result.throughfall_m) * area;
                book.sublimation += result.sublim_m * area;
                book.stored = result.swe_m * area;
                if (configuration.kind_[i] == SnowKind::Ueb)
                    book.stored_energy = representedEnergy(held) * area;
                if (!std::isfinite(book.stored) ||
                    std::abs(book.residual()) > 1e-10 * std::max(1., book.initial + book.precipitation))
                    return "Mesh snow water balance failed for cell " + std::to_string(i + 1) + '.';
                liquid_rate_[i] = (result.melt_m + result.throughfall_m) / (end - start);
                exposed_[i] = std::clamp(1 - covered_seconds / (end - start), 0., 1.);
                precipitation_[i] = rain[i];
                catch_[i] = catch_factor[i];
                return {};
            };
            const int count = static_cast<int>(configuration.active_cells_.size());
            int failed_slot = count;
            std::string failure;
            std::exception_ptr exception;
#pragma omp parallel for schedule(static) num_threads(threads)
            for (int slot = 0; slot < count; ++slot) {
                try {
                    auto error = consumeCell(configuration.active_cells_[slot]);
                    if (!error.empty()) {
#pragma omp critical(openswmm_mesh_snow_failure)
                        {
                            if (slot < failed_slot) {
                                failed_slot = slot;
                                failure = std::move(error);
                                exception = nullptr;
                            }
                        }
                    }
                } catch (...) {
#pragma omp critical(openswmm_mesh_snow_failure)
                    {
                        if (slot < failed_slot) {
                            failed_slot = slot;
                            failure.clear();
                            exception = std::current_exception();
                        }
                    }
                }
            }
            // The caller publishes this private trial only on complete success.
            if (exception)
                std::rethrow_exception(exception);
            if (!failure.empty())
                return failure;
            last_seconds_ = span;
            completed_ = end;
            return {};
        }
    }
#endif
    int melt_day = -1;
    for (int i : configuration.active_cells_) {
        if (!std::isfinite(rain[i]) || rain[i] < 0 || !std::isfinite(catch_factor[i]) ||
            catch_factor[i] < 0)
            return "Invalid mesh snow precipitation.";
        auto& held = held_[i];
        auto& result = output_[i];
        auto& book = ledger_[i];
        result.melt_m = result.throughfall_m = result.sublim_m = 0;
        if (precipitation_[i] != rain[i] || catch_[i] != catch_factor[i])
            ueb::closeHeld(held);
        const bool ground_contact = configuration.groundContactEnabled(i) && configuration.kind_[i] == SnowKind::Ueb;
        if (ground_contact && held.pending())
            ueb::closeHeld(held);
        double at = start, covered_seconds = 0;
        while (at < end - 1e-9) {
            // Contact flux is resolved against the accepted snow cover for
            // this forcing segment. Do not reuse a look-ahead prediction if
            // a previous segment changed the covered area.
            if (ground_contact && held.pending())
                ueb::closeHeld(held);
            const double date = ctx.options.start_date + at / 86400.;
            double prescribed_qg = site_[i].ground_heat;
            const double boundary = (std::floor((at + 1e-7) / configuration.step_) + 1) * configuration.step_;
            double horizon = std::min(boundary - at, ctx.options.totalDurationMs() / 1000. - at);
            if (!ctx.snowpacks.meteorology.empty()) {
                const double hour =
                    (ctx.options.start_date - std::floor(ctx.options.start_date)) * 86400. + at;
                horizon = std::min(horizon, (std::floor((hour + 1e-5) / 3600.) + 1) * 3600. - hour);
            }
            if (snowTransientForcing(ctx))
                horizon = std::min(horizon, end - at);
            if (!(horizon > 0))
                return "Mesh snow forcing ends before the requested interval.";
            double dt = std::min(end - at, horizon);
            if (configuration.kind_[i] == SnowKind::Ueb) {
                if (!held.pending()) {
                    ueb::Forcing forcing;
                    if (auto error = climate::resolveUebForcing(
                            ctx, date, horizon, configuration.geometry_[i].slope, configuration.geometry_[i].aspect, forcing,
                            site_[i].pressure);
                        !error.empty())
                        return error;
                    forcing.precipitation = rain[i] * horizon;
                    forcing.snow_catch = catch_factor[i];
                    auto site = site_[i];
                    const bool modeled_contact =
                        ground_contact && (held.accepted.cover > 0 || (forcing.precipitation > 0 &&
                                                                       forcing.snow_fraction > 0));
                    if (modeled_contact)
                        site.ground_heat = 0;
                    prescribed_qg = site.ground_heat;
                    if (auto error = ueb::prepareHeld(held, config.parameters, site, forcing,
                                                      configuration.depletion_curve_);
                        !error.empty())
                        return error;
                }
                dt = std::min(dt, held.duration - held.elapsed);
                ueb::LumpedFluxes flux;
                if (auto error = ueb::consumeHeld(held, dt, configuration.depletion_curve_, flux); !error.empty())
                    return error;
                covered_seconds += flux.active_fraction * dt;
                result.melt_m += flux.liquid;
                result.throughfall_m += flux.throughfall;
                result.sublim_m += flux.sublimation;
                result.swe_m = held.accepted.water;
                result.cover_frac = held.accepted.cover;
                result.energy_kJm2 = held.accepted.point.energy;
                result.albedo = flux.point.albedo;
                result.T_C = ueb::bulkTemperature(held.accepted.point, config.parameters);
                result.Ts_C = held.accepted.point.history.empty()
                                  ? std::min(0., result.T_C)
                                  : held.accepted.point.history.back().surface;
                book.precipitation += (flux.rain + flux.snow) * configuration.geometry_[i].area;
                book.net_energy += flux.net_energy * configuration.geometry_[i].area;
                book.prescribed_ground_energy +=
                    prescribed_qg * flux.active_fraction * dt * configuration.geometry_[i].area / 1000.;
                book.geometry_energy += flux.geometry_energy * configuration.geometry_[i].area;
            } else {
                climate::RadiationState weather;
                if (auto error = climate::resolveRadiation(ctx, date, dt, weather); !error.empty())
                    return error;
                const double fraction =
                    weather.temperature <= (ctx.climate_state.snow_divt - 32) * 5 / 9 ? 1 : 0;
                SnowInputs forcing;
                forcing.dt_s = dt;
                forcing.rain_m = (1 - fraction) * rain[i] * dt;
                forcing.snow_m = fraction * rain[i] * dt * catch_factor[i];
                forcing.Ta_C = weather.temperature;
                forcing.wind_ms = weather.wind;
                forcing.gamma = ctx.climate_state.gamma;
                const double temp = weather.temperature * 1.8 + 32;
                forcing.ea = 8.1175e6 * std::exp(-7701.544 / (temp + 405.0265));
                const int day = datetime::dayOfYear(date);
                if (day != melt_day) {
                    degree_day_.setMeltCoeffs(day);
                    melt_day = day;
                }
                SnowOutputs flux;
                const double before = result.cover_frac;
                degree_day_.step(i * 3 + 2, forcing, flux);
                covered_seconds += .5 * (before + flux.cover_frac) * dt;
                result.melt_m += flux.melt_m;
                result.throughfall_m += flux.throughfall_m;
                result.swe_m = flux.swe_m;
                result.cover_frac = flux.cover_frac;
                book.precipitation += (forcing.rain_m + forcing.snow_m) * configuration.geometry_[i].area;
            }
            at += dt;
        }
        const double area = configuration.geometry_[i].area;
        book.raw_precipitation += rain[i] * (end - start) * area;
        book.liquid += (result.melt_m + result.throughfall_m) * area;
        book.sublimation += result.sublim_m * area;
        book.stored = result.swe_m * area;
        if (configuration.kind_[i] == SnowKind::Ueb)
            book.stored_energy = representedEnergy(held) * area;
        if (!std::isfinite(book.stored) ||
            std::abs(book.residual()) > 1e-10 * std::max(1., book.initial + book.precipitation))
            return "Mesh snow water balance failed for cell " + std::to_string(i + 1) + '.';
        liquid_rate_[i] = (result.melt_m + result.throughfall_m) / (end - start);
        exposed_[i] = std::clamp(1 - covered_seconds / (end - start), 0., 1.);
        precipitation_[i] = rain[i];
        catch_[i] = catch_factor[i];
    }
    last_seconds_ = end - start;
    completed_ = end;
    return {};
}
namespace {
/** @brief Stream one native mesh-snow runtime profile in a fixed field order.
 * @param[in,out] a Matching native archive reader/writer; errors can partially mutate/emit.
 * @param[in,out] completed Accepted elapsed seconds.
 * @param[in,out] held Fixed-shape per-cell accepted/predicted UEB windows.
 * @param[in,out] dd Native degree-day history/geometry, streamed by its helper.
 * @param[in,out] rain Fixed-shape cached raw precipitation m/s, -1 sentinel allowed.
 * @param[in,out] catch_factor Fixed-shape cached catch multipliers, -1 sentinel allowed.
 * @param[in,out] rate Fixed-shape accepted liquid m/s.
 * @param[in,out] exposed Fixed-shape ET exposure fractions.
 * @param[in,out] output Fixed-shape accepted snow depth/thermal output rows.
 * @param[in,out] ledger Fixed-shape cumulative m3/kJ accounts.
 * @param contact_profile True includes prescribed/reciprocal ground-energy extension.
 * @param geometry_profile True includes explicit cover-geometry energy extension.
 * No complete compatibility/finite/closure validation here; restoreCheckpoint owns
 * candidate validation and surrounding code qualifies version/mask/model identity. */
void archiveMeshSnowState(detail::CompletedSourceArchive& a, double& completed,
                          std::vector<ueb::HeldState>& held, snow::SnowSoA& dd,
                          std::vector<double>& rain, std::vector<double>& catch_factor,
                          std::vector<double>& rate, std::vector<double>& exposed,
                          std::vector<SnowOutputs>& output, std::vector<MeshSnowLedger>& ledger,
                          bool contact_profile, bool geometry_profile) {
    a(completed);
    a.fixed(rain, catch_factor, rate, exposed);
    a.sequence(held, [&](auto& column) { archiveHeldSnow(a, column); }, true);
    archiveDegreeDaySnow(a, dd);
    a.sequence(
        output,
        [&](auto& s) {
            a(s.melt_m, s.throughfall_m, s.sublim_m, s.swe_m, s.cover_frac, s.Ts_C, s.T_C, s.albedo,
              s.energy_kJm2);
        },
        true);
    a.sequence(
        ledger,
        [&](auto& b) {
            a(b.initial, b.precipitation, b.raw_precipitation, b.liquid, b.sublimation, b.stored,
              b.net_energy);
            if (geometry_profile)
                a(b.geometry_energy);
            a(b.initial_energy, b.stored_energy);
            if (contact_profile)
                a(b.prescribed_ground_energy, b.ground_contact_energy);
        },
        true);
}
}
std::string MeshSnow::saveCheckpoint() const {
    if (!active())
        return {};
    auto copy = *this;
    detail::CompletedSourceArchive archive;
    std::string magic = "OpenSWMM mesh snow 4";
    archive(magic, copy.identity_, copy.last_seconds_, copy.forcing_);
    archive(copy.ground_contact_);
    archiveMeshSnowState(archive, copy.completed_, copy.held_, copy.degree_day_.state(),
                         copy.precipitation_, copy.catch_, copy.liquid_rate_, copy.exposed_,
                         copy.output_, copy.ledger_, true, true);
    return archive.bytes();
}
std::string MeshSnow::restoreCheckpoint(const std::string& bytes, bool seed) {
    try {
        if (!active())
            return bytes.empty() ? "" : "Snow checkpoint has mesh cells absent from this model.";
        auto next = *this;
        if (bytes.empty()) {
            // Old hot starts have no snow inventory. Keep assignments/forcing.
            for (int i : next.active_cells_) {
                next.held_[i] = {};
                next.output_[i] = {};
                next.output_[i].albedo = nan;
                if (next.kind_[i] == SnowKind::DegreeDay)
                    next.output_[i].energy_kJm2 = next.output_[i].T_C = next.output_[i].Ts_C = nan;
                next.ledger_[i] = {};
                auto& dd = next.degree_day_.state();
                const int e = i * 3 + 2;
                dd.wsnow[e] = dd.fw[e] = dd.coldc[e] = dd.imelt[e] = dd.sba[e] = dd.sbws[e] =
                    dd.asc[e] = dd.age[e] = dd.out_age[e] = 0;
                dd.awe[e] = 1;
                dd.ati[e] = dd.tbase[e];
            }
            seed = true;
        } else {
            detail::CompletedSourceArchive archive(bytes);
            std::string magic, identity;
            archive(magic, identity, next.last_seconds_, next.forcing_);
            for (double value : next.forcing_)
                if (!std::isfinite(value))
                    return "Invalid mesh snow checkpoint forcing.";
            const bool geometry_profile = magic == "OpenSWMM mesh snow 4";
            const bool contact_profile = geometry_profile || magic == "OpenSWMM mesh snow 3";
            const bool legacy_identity =
                !geometry_profile && identity == next.legacy_identity_ &&
                std::all_of(next.depletion_curve_.begin(), next.depletion_curve_.end(),
                            [](double value) { return value == 1.; });
            if ((!geometry_profile && magic != "OpenSWMM mesh snow 3" &&
                 magic != "OpenSWMM mesh snow 2") ||
                (identity != identity_ && !legacy_identity))
                return "Mesh snow checkpoint configuration or geometry differs.";
            if (contact_profile) {
                std::vector<unsigned char> mask;
                archive(mask);
                if (mask != next.ground_contact_)
                    return "Mesh snow checkpoint ground-contact ownership differs.";
            } else if (std::any_of(next.ground_contact_.begin(), next.ground_contact_.end(),
                                   [](auto v) { return v != 0; }))
                return "Mesh snow checkpoint predates reciprocal UEB/soil contact state.";
            archiveMeshSnowState(archive, next.completed_, next.held_, next.degree_day_.state(),
                                 next.precipitation_, next.catch_, next.liquid_rate_, next.exposed_,
                                 next.output_, next.ledger_, contact_profile, geometry_profile);
            archive.finish();
        }
        if (!std::isfinite(next.last_seconds_) || next.last_seconds_ < 0 ||
            next.last_seconds_ > next.completed_ || !std::isfinite(next.completed_) ||
            next.completed_ < 0)
            return "Invalid mesh snow checkpoint clock.";
        for (int i : next.active_cells_) {
            auto& out = next.output_[i];
            auto& book = next.ledger_[i];
            for (double v : {out.swe_m, out.melt_m, out.throughfall_m, out.cover_frac, book.initial,
                             book.precipitation, book.raw_precipitation, book.liquid, book.stored,
                             next.liquid_rate_[i]})
                if (!std::isfinite(v) || v < 0)
                    return "Invalid mesh snow checkpoint inventory.";
            if (!std::isfinite(next.exposed_[i]) || next.exposed_[i] < 0 || next.exposed_[i] > 1 ||
                !std::isfinite(out.sublim_m) || !std::isfinite(book.sublimation) ||
                !std::isfinite(book.net_energy) || !std::isfinite(book.prescribed_ground_energy) ||
                !std::isfinite(book.ground_contact_energy) ||
                !std::isfinite(next.precipitation_[i]) || next.precipitation_[i] < -1 ||
                !std::isfinite(next.catch_[i]) || next.catch_[i] < -1 || out.cover_frac > 1 ||
                std::abs(out.swe_m * geometry_[i].area - book.stored) >
                    1e-10 * std::max(1., book.stored)) {
                if (!bytes.empty())
                    return "Inconsistent mesh snow checkpoint inventory.";
            }
            const double water = next.kind_[i] == SnowKind::Ueb
                                     ? next.held_[i].accepted.water
                                     : (next.degree_day_.state().wsnow[i * 3 + 2] +
                                        next.degree_day_.state().fw[i * 3 + 2]) *
                                           .3048;
            if (std::abs(out.swe_m - water) > 1e-10)
                return "Mesh snow checkpoint report and inventory differ.";
            if (next.kind_[i] == SnowKind::Ueb &&
                (!std::isfinite(out.energy_kJm2) || !std::isfinite(out.T_C) ||
                 !std::isfinite(out.Ts_C) ||
                 out.energy_kJm2 != next.held_[i].accepted.point.energy ||
                 (!std::isnan(out.albedo) &&
                  (!std::isfinite(out.albedo) || out.albedo < 0 || out.albedo > 1))))
                return "Invalid mesh snow checkpoint thermal report.";
            if (!bytes.empty() &&
                std::abs(book.residual()) > 1e-10 * std::max(1., book.initial + book.precipitation))
                return "Mesh snow checkpoint water balance differs.";
            if (next.kind_[i] == SnowKind::Ueb &&
                (!std::isfinite(book.initial_energy) || !std::isfinite(book.stored_energy) ||
                 std::abs(book.stored_energy -
                          representedEnergy(next.held_[i]) * geometry_[i].area) >
                     1e-9 * std::max(1., std::abs(book.stored_energy)) ||
                 std::abs(book.energyResidual()) >
                     1e-8 * std::max({1., std::abs(book.initial_energy), std::abs(book.net_energy),
                                      std::abs(book.geometry_energy)})))
                return "Mesh snow checkpoint energy balance differs.";
            if (seed) {
                ueb::closeHeld(next.held_[i]);
                next.precipitation_[i] = next.catch_[i] = -1;
                next.liquid_rate_[i] = 0;
                book = {};
                book.initial = book.stored = water * geometry_[i].area;
                if (next.kind_[i] == SnowKind::Ueb)
                    book.initial_energy = book.stored_energy =
                        representedEnergy(next.held_[i]) * geometry_[i].area;
                out.melt_m = out.throughfall_m = out.sublim_m = 0;
                next.exposed_[i] = 1 - out.cover_frac;
            }
        }
        if (seed)
            next.completed_ = next.last_seconds_ = 0;
        *this = std::move(next);
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
} // namespace openswmm::surface
