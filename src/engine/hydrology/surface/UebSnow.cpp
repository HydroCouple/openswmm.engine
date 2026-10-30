// SPDX-License-Identifier: Apache-2.0

/**
 * @file      UebSnow.cpp
 * @brief     Utah Energy Balance snow accumulation and energy integration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "UebSnow.hpp"
#include "UebReference.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace openswmm::surface::ueb {
namespace {
constexpr double water_density = 1000, latent_heat = 333.5, ice_heat = 2.09;
/** @brief Test numeric finiteness without mutation.
 * @param v Numeric value in its field's units.
 * @return False for NaN/infinity, true otherwise; no range qualification. */
bool finite(double v) {
    return std::isfinite(v);
}
/** @brief Test a finite nonnegative numeric field.
 * @param v Numeric value in its field's units.
 * @return True exactly for finite v>=0; no upper-bound qualification. */
bool nonnegative(double v) {
    return finite(v) && v >= 0;
}
/** @brief Test a finite unit-interval field.
 * @param v Proposed dimensionless fraction.
 * @return True exactly for finite v in [0,1]. */
bool fraction(double v) {
    return nonnegative(v) && v <= 1;
}
/** @brief Read duration-weighted preceding point temperature.
 * @param state Consistent positive-duration thermal history, borrowed unchanged.
 * @param surface True selects surface, false bulk Celsius.
 * @param fallback Returned when total history duration is not positive.
 * @return Weighted Celsius mean or fallback; no local field validation. */
double mean(const State& state, bool surface, double fallback) {
    double sum = 0, time = 0;
    for (const auto& h : state.history) {
        time += h.seconds;
        sum += h.seconds * (surface ? h.surface : h.bulk);
    }
    return time > 0 ? sum / time : fallback;
}
/** @brief Append an accepted thermal sample and trim the oldest time beyond 24 hours.
 * @param[in,out] state Owned chronological thermal history; vector allocation may throw.
 * @param dt Positive accepted sample duration seconds.
 * @param ts Accepted surface Celsius.
 * @param tb Accepted bulk Celsius.
 * Removes complete old samples then shortens the oldest retained sample to exactly
 * 86400 seconds when needed. Does not validate inputs or alter physical inventory. */
void remember(State& state, double dt, double ts, double tb) {
    state.history.push_back({dt, ts, tb});
    double duration = 0;
    for (const auto& h : state.history)
        duration += h.seconds;
    double excess = std::max(0., duration - 86400.);
    std::size_t erase = 0;
    while (erase < state.history.size() && excess >= state.history[erase].seconds) {
        excess -= state.history[erase].seconds;
        ++erase;
    }
    state.history.erase(state.history.begin(), state.history.begin() + erase);
    if (!state.history.empty())
        state.history.front().seconds -= excess;
}
/** @brief Validate one point inventory and preceding thermal history.
 * @param s Borrowed energy/water/optical-age/refreezing inventory and samples.
 * @return Empty if finite, water/age/depths nonnegative, samples positive duration and
 * temperatures above -273.15 Celsius, and finite total<=86400+1e-7 seconds; first
 * diagnostic otherwise. Does not check state against a specific material or forcing. */
std::string stateError(const State& s) {
    if (!finite(s.energy) || !nonnegative(s.water) || !nonnegative(s.age) ||
        !nonnegative(s.refreezing_depth) || !nonnegative(s.total_refreezing_depth))
        return "Invalid UEB snow inventory.";
    double duration = 0;
    for (const auto& h : s.history) {
        if (!finite(h.seconds) || h.seconds <= 0 || !finite(h.surface) || h.surface <= -273.15 ||
            !finite(h.bulk) || h.bulk <= -273.15)
            return "Invalid UEB thermal history.";
        duration += h.seconds;
    }
    if (!finite(duration) || duration > 86400. + 1e-7)
        return "UEB thermal history exceeds 24 hours.";
    return {};
}
/** @brief Trial one reference solve and qualify finite physical/conservation outputs.
 * @param[in,out] state Private valid point candidate; updated only after numerical
 * checks, then thermal history appended (allocation failure can follow field updates).
 * @param p Valid formulation/material coefficients in adapter/reference units.
 * @param site Valid held site settings.
 * @param f Valid positive-duration meteorology; precipitation is integrated m.
 * @param[out] flux One accepted leaf's integrated m and kJ/m2 outputs, endpoint
 * Celsius/albedo and diagnostics, replaced on true; unchanged on ordinary rejection.
 * @return True after finite output/state, nonnegative storage/age/refreezing/liquid,
 * albedo and absolute-temperature bounds, relative 1e-10 water/energy closure and
 * zero reference diagnostics. False discards reference-array results. Converts seconds
 * to hours and W/m2 to kJ/(m2 h), uses padded one-based buffers, and passes preceding
 * weighted temperatures to modified force restore. Exceptions propagate; outer caller
 * owns transaction/retry. No engine/global ledger effects. */
bool step(State& state, const Parameters& p, const Site& site, const Forcing& f, Fluxes& flux) {
    const double hours = f.seconds / 3600.;
    // Padding belongs to the array objects: the reference's one-based pointer
    // adjustment never forms a pointer before an allocated object.
    std::array<double, 15> input{};
    input[8] = f.temperature;
    input[9] = f.precipitation / hours;
    input[10] = f.wind;
    input[11] = f.humidity;
    input[12] = f.shortwave * 3.6;
    input[13] = f.longwave * 3.6;
    input[14] = f.cosine;
    std::array<double, 6> location{0,
                                   site.forest,
                                   site.drift * f.snow_catch,
                                   site.pressure,
                                   site.ground_heat * 3.6,
                                   site.extinction_depth};
    std::array<double, 6> water{0,         state.energy,           state.water,
                                state.age, state.refreezing_depth, state.total_refreezing_depth};
    std::array<double, 15> output{};
    int diagnostics = 0;
    if (p.temperature == TemperatureMethod::Equilibrium) {
        std::array<double, 31> parameters{0,
                                          p.rain_temperature,
                                          p.snow_temperature,
                                          0,
                                          273.15,
                                          p.emissivity,
                                          2.0747e-7,
                                          latent_heat,
                                          2834,
                                          4.18,
                                          ice_heat,
                                          p.soil_heat_capacity,
                                          1.005,
                                          287,
                                          .4,
                                          p.measurement_height,
                                          p.roughness,
                                          3600,
                                          917,
                                          water_density,
                                          p.snow_density,
                                          p.soil_density,
                                          p.liquid_retention,
                                          p.drainage_conductivity,
                                          p.active_soil_depth,
                                          p.equilibrium_conductance,
                                          9.81,
                                          p.ground_albedo,
                                          p.visible_albedo,
                                          p.infrared_albedo,
                                          p.stability};
        diagnostics = ueb_reference::equilibriumStep(
            hours, input.data() + 8, location.data() + 1, water.data() + 1, parameters.data() + 1,
            output.data() + 1, p.age_dust, p.new_snow_depth, p.visible_age, p.infrared_age,
            f.snow_fraction);
    } else {
        std::array<double, 23> parameters{0,
                                          p.rain_temperature,
                                          p.snow_temperature,
                                          p.emissivity,
                                          p.soil_heat_capacity,
                                          p.measurement_height,
                                          p.roughness,
                                          p.snow_density,
                                          p.soil_density,
                                          p.liquid_retention,
                                          p.drainage_conductivity,
                                          p.active_soil_depth,
                                          p.ground_albedo,
                                          p.visible_albedo,
                                          p.infrared_albedo,
                                          p.snow_conductivity,
                                          p.soil_conductivity,
                                          p.low_frequency,
                                          p.amplitude,
                                          p.stability,
                                          0,
                                          p.new_snow_depth,
                                          0};
        const double initial_bulk = bulkTemperature(state, p);
        const double old_surface =
            state.history.empty() ? std::min(0., initial_bulk) : state.history.back().surface;
        const double old_bulk = state.history.empty() ? initial_bulk : state.history.back().bulk;
        std::array<double, 2> surface{0, old_surface}, average{0, old_bulk};
        diagnostics = ueb_reference::forceRestoreStep(
            hours, input.data() + 8, location.data() + 1, water.data() + 1, surface.data() + 1,
            average.data() + 1, 1, parameters.data() + 1, output.data() + 1, p.age_dust,
            mean(state, true, old_surface), mean(state, false, old_bulk), p.visible_age,
            p.infrared_age, f.snow_fraction);
    }
    Fluxes next;
    next.rain = output[1] * hours;
    next.snow = output[2] * hours;
    next.albedo = output[3];
    next.sublimation = output[6] * hours;
    next.liquid = output[7] * hours;
    next.outgoing_melt_energy = output[8] * hours;
    next.net_energy = output[9] * hours;
    next.bulk_temperature = output[11];
    next.surface_temperature = output[12];
    next.diagnostics = diagnostics;
    next.water_residual =
        state.water + next.rain + next.snow - next.sublimation - next.liquid - water[2];
    next.energy_residual = state.energy + next.net_energy - water[1];
    for (double x : water)
        if (!finite(x))
            return false;
    for (double x : output)
        if (!finite(x))
            return false;
    if (water[2] < 0 || water[3] < 0 || water[4] < 0 || water[5] < 0 || next.liquid < 0 ||
        !fraction(next.albedo) ||
        std::abs(next.water_residual) > 1e-10 * std::max(1., state.water + next.rain + next.snow) ||
        std::abs(next.energy_residual) >
            1e-10 * std::max({1., std::abs(state.energy), std::abs(next.net_energy)}) ||
        next.surface_temperature <= -273.15 || next.bulk_temperature <= -273.15 || diagnostics > 0)
        return false;
    state.energy = water[1];
    state.water = water[2];
    state.age = water[3];
    state.refreezing_depth = water[4];
    state.total_refreezing_depth = water[5];
    remember(state, f.seconds, next.surface_temperature, next.bulk_temperature);
    next.steps = 1;
    flux = next;
    return true;
}
/** @brief Accumulate accepted point fluxes while retaining the latest endpoint diagnostics.
 * @param[in,out] sum Running integrated water/energy/residual/counter totals.
 * @param x Next accepted flux in identical units.
 * Adds extensive fields and counts; overwrites endpoint surface/bulk Celsius and
 * albedo. Does not average, validate finiteness or mutate x. */
void add(Fluxes& sum, const Fluxes& x) {
    sum.rain += x.rain;
    sum.snow += x.snow;
    sum.liquid += x.liquid;
    sum.sublimation += x.sublimation;
    sum.net_energy += x.net_energy;
    sum.outgoing_melt_energy += x.outgoing_melt_energy;
    sum.water_residual += x.water_residual;
    sum.energy_residual += x.energy_residual;
    sum.steps += x.steps;
    sum.retries += x.retries;
    sum.diagnostics += x.diagnostics;
    sum.surface_temperature = x.surface_temperature;
    sum.bulk_temperature = x.bulk_temperature;
    sum.albedo = x.albedo;
}
/** @brief Retry one point interval by recursively bisecting rejected reference solves.
 * @param[in,out] state Valid point owner; accepted candidate replaces it on true,
 * normal false retains it even when a first half succeeded privately.
 * @param p Valid material/integration coefficients.
 * @param site Valid site settings.
 * @param forcing Held interval with integrated precipitation m and positive seconds.
 * @param[out] flux Accepted integrated exchanges, replaced on true; do not use on false.
 * @param depth Current split depth, initially zero; must be in [0,16].
 * @return True for one accepted leaf or two accepted half-intervals. False at depth
 * 16 or duration<=0.001 s if the solve still fails, or if either half fails. Halves
 * both duration and precipitation, sums fluxes and counts one successful split retry.
 * Allocation/reference exceptions propagate; state copies isolate partial trials. */
bool boundedStep(State& state, const Parameters& p, const Site& site, const Forcing& forcing,
                 Fluxes& flux, int depth) {
    auto candidate = state;
    if (step(candidate, p, site, forcing, flux)) {
        state = std::move(candidate);
        return true;
    }
    if (depth == 16 || forcing.seconds <= .001)
        return false;
    auto half = forcing;
    half.seconds *= .5;
    half.precipitation *= .5;
    Fluxes a, b;
    if (!boundedStep(candidate, p, site, half, a, depth + 1) ||
        !boundedStep(candidate, p, site, half, b, depth + 1))
        return false;
    add(a, b);
    ++a.retries;
    flux = a;
    state = std::move(candidate);
    return true;
}
}
std::string Parameters::validate() const {
    if (temperature != TemperatureMethod::ModifiedForceRestore &&
        temperature != TemperatureMethod::Equilibrium)
        return "Unknown UEB surface temperature method.";
    for (double x : {rain_temperature,
                     snow_temperature,
                     emissivity,
                     roughness,
                     measurement_height,
                     snow_density,
                     soil_density,
                     soil_heat_capacity,
                     active_soil_depth,
                     liquid_retention,
                     drainage_conductivity,
                     equilibrium_conductance,
                     snow_conductivity,
                     soil_conductivity,
                     low_frequency,
                     amplitude,
                     stability,
                     ground_albedo,
                     visible_albedo,
                     infrared_albedo,
                     new_snow_depth,
                     age_dust,
                     visible_age,
                     infrared_age,
                     max_step})
        if (!finite(x))
            return "Nonfinite UEB parameter.";
    if (rain_temperature <= snow_temperature || snow_temperature <= -273.15 ||
        !fraction(emissivity) || roughness <= 0 || measurement_height <= roughness ||
        snow_density <= 0 || snow_density >= 917 || soil_density <= 0 || soil_heat_capacity <= 0 ||
        active_soil_depth <= 0 || liquid_retention < 0 ||
        liquid_retention >= 1000 / snow_density - 1000 / 917. || drainage_conductivity < 0 ||
        equilibrium_conductance <= 0 || snow_conductivity <= 0 || soil_conductivity <= 0 ||
        low_frequency <= 0 || amplitude <= 0 || !fraction(stability) || !fraction(ground_albedo) ||
        !fraction(visible_albedo) || !fraction(infrared_albedo) || new_snow_depth < 0 ||
        age_dust < 0 || !fraction(visible_age) || !fraction(infrared_age) || max_step < 1 ||
        max_step > 21600)
        return "Invalid UEB parameter range.";
    return {};
}
double bulkTemperature(const State& s, const Parameters& p) noexcept {
    const double capacity = water_density * s.water * ice_heat +
                            p.soil_density * p.active_soil_depth * p.soil_heat_capacity;
    return s.energy <= 0
               ? s.energy / capacity
               : std::max(0., s.energy - s.water * water_density * latent_heat) / capacity;
}
std::string advance(State& state, const Parameters& p, const Site& site, const Forcing& f,
                    Fluxes& output) {
    if (auto error = p.validate(); !error.empty())
        return error;
    if (auto error = stateError(state); !error.empty())
        return error;
    if (!fraction(site.forest) || !nonnegative(site.drift) || !finite(site.pressure) ||
        site.pressure <= 0 || !finite(site.ground_heat) || !finite(site.extinction_depth) ||
        site.extinction_depth <= 0)
        return "Invalid UEB site parameters.";
    if (!finite(f.seconds) || f.seconds <= 0 || !nonnegative(f.precipitation) ||
        !finite(f.temperature) || f.temperature <= -100 || f.temperature > 70 ||
        !nonnegative(f.wind) || !fraction(f.humidity) || !nonnegative(f.shortwave) ||
        !nonnegative(f.longwave) || !fraction(f.cosine) || !nonnegative(f.snow_catch) ||
        (f.snow_fraction != -1 && !fraction(f.snow_fraction)))
        return "Invalid UEB meteorological forcing.";
    auto candidate = state;
    Fluxes result;
    const double count = std::ceil(f.seconds / p.max_step);
    if (count > 100000)
        return "UEB interval exceeds the kernel step limit.";
    const int steps = int(count);
    double elapsed = 0, precipitation = 0;
    try {
        for (int i = 0; i < steps; ++i) {
            const double end =
                i + 1 == steps ? f.seconds : std::min(f.seconds, (i + 1) * p.max_step);
            auto part = f;
            part.seconds = end - elapsed;
            part.precipitation = i + 1 == steps ? f.precipitation - precipitation
                                                : f.precipitation * (part.seconds / f.seconds);
            Fluxes value;
            if (!boundedStep(candidate, p, site, part, value, 0))
                return "UEB step failed its finite-state or conservation checks.";
            add(result, value);
            elapsed = end;
            precipitation += part.precipitation;
        }
    } catch (const std::exception& e) {
        return e.what();
    }
    state = std::move(candidate);
    output = result;
    return {};
}
}
