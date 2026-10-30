// SPDX-License-Identifier: Apache-2.0

/**
 * @file      UebLumped.cpp
 * @brief     Lumped Utah Energy Balance snow model integration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "UebLumped.hpp"
#include <algorithm>
#include <cmath>

namespace openswmm::surface::ueb {
namespace {
constexpr double rho = 1000, fusion = 333.5, ice_heat = 2.09;
/** @brief Compute active-soil areal heat capacity used by the depletion remap.
 * @param p Qualified soil density kg/m3, active depth m and heat capacity kJ/(kg K).
 * @return Product in kJ/(m2 K), without finite/range validation or state mutation. */
double soilCapacity(const Parameters& p) {
    return p.soil_density * p.active_soil_depth * p.soil_heat_capacity;
}
/** @brief Read whole-element represented thermal inventory.
 * @param s Qualified lumped state; point.energy kJ/m2 of snow-covered area.
 * @return cover*point.energy when cover>0, otherwise full snow-free active-soil
 * point energy. No validation/mutation; this is not an added physical flux. */
double representedEnergy(const LumpedState& s) {
    // The snow-free state still carries the active soil temperature.
    return (s.cover > 0 ? s.cover : 1) * s.point.energy;
}
/** @brief Adjust covered-point energy when cover geometry changes represented water.
 * @param[in,out] point Qualified point water m and energy kJ/m2. Replaces water and
 * energy using the same cold bulk temperature, or bounded liquid fraction plus
 * positive warm component. For positive energy with zero old water, energy is
 * retained while water changes.
 * @param water Finite nonnegative target covered-point SWE metres.
 * @param p Qualified soil/ice capacity parameters; capacity denominators positive.
 * No physical forcing, conservation diagnostic or external clock change; caller
 * records resulting represented-energy change separately as geometry energy. */
void remap(State& point, double water, const Parameters& p) {
    // SnowLSub.f (USU UEB 2.2, 26 July 2005) preserves bulk temperature
    // below freezing and liquid fraction at zero degrees while Wsc changes.
    const double old_water = point.water;
    if (point.energy <= 0) {
        const double temperature = point.energy / (soilCapacity(p) + rho * old_water * ice_heat);
        point.energy = temperature * (soilCapacity(p) + rho * water * ice_heat);
    } else if (old_water > 0) {
        const double liquid = std::min(1., point.energy / (rho * fusion * old_water));
        const double warm = std::max(0., point.energy - rho * fusion * old_water) /
                            (soilCapacity(p) + rho * old_water * ice_heat);
        point.energy =
            liquid * rho * fusion * water + warm * (soilCapacity(p) + rho * water * ice_heat);
    }
    point.water = water;
}
}
std::string validateDepletion(const std::array<double, 10>& curve) {
    double previous = 0;
    for (double value : curve) {
        if (!std::isfinite(value) || value < previous || value > 1)
            return "Snow depletion ordinates must be finite, nondecreasing fractions.";
        previous = value;
    }
    return {};
}
double depletionCover(const std::array<double, 10>& curve, double water) noexcept {
    if (water <= 0)
        return 0;
    if (water >= 1)
        return 1;
    const double x = water * 10;
    const int i = std::min(9, int(x));
    return curve[i] + (x - i) * ((i == 9 ? 1 : curve[i + 1]) - curve[i]);
}
std::string initializeLumped(LumpedState& state, double water, double energy, double maximum,
                             const std::array<double, 10>& curve) {
    if (auto error = validateDepletion(curve); !error.empty())
        return error;
    if (!std::isfinite(water) || water < 0 || !std::isfinite(energy) || !std::isfinite(maximum) ||
        maximum < 0)
        return "Invalid initial lumped snow inventory.";
    LumpedState next;
    next.water = water;
    next.maximum = water > 0 ? std::max(water, maximum) : 0;
    next.cover = water > 0 ? depletionCover(curve, water / next.maximum) : 0;
    if (water > 0 && next.cover <= 0)
        return "Snow depletion curve gives no covered area for a nonempty pack.";
    next.point.water = next.cover > 0 ? water / next.cover : 0;
    next.point.energy = energy;
    state = std::move(next);
    return {};
}
std::string advanceLumped(LumpedState& state, const Parameters& p, const Site& site,
                          const Forcing& f, const std::array<double, 10>& curve,
                          LumpedFluxes& output) {
    if (auto error = validateDepletion(curve); !error.empty())
        return error;
    if (!std::isfinite(state.water) || state.water < 0 || !std::isfinite(state.maximum) ||
        state.maximum < state.water || !std::isfinite(state.cover) || state.cover < 0 ||
        state.cover > 1 || (state.water > 0 && state.cover <= 0) ||
        std::abs(state.point.water * state.cover - state.water) > 1e-12 * std::max(1., state.water))
        return "Inconsistent lumped snow inventory.";
    auto next = state;
    LumpedFluxes result;
    if (auto error = advance(next.point, p, site, f, result.point); !error.empty())
        return error;
    const double change = next.point.water - state.point.water;
    // The positive branch of You (2004), Eq. 4-4 assumes snowfall.
    // SnowLSub.f also distinguishes positive storage from refreezing rain or
    // condensation: those act only on the existing covered area.
    const bool fresh_growth = result.point.snow > 0 && change > 0;
    const double fraction = fresh_growth ? 1 : state.cover;
    result.active_fraction = fraction;
    result.rain = result.point.rain;
    result.snow = result.point.snow;
    result.liquid = fraction * result.point.liquid;
    result.sublimation = fraction * result.point.sublimation;
    result.throughfall = (1 - fraction) * (result.rain + result.snow);
    next.water = state.cover == 1 ? next.point.water : state.water + fraction * change;
    // Negative roundoff is bounded by the point kernel's checked balance.
    if (next.water < -1e-12)
        return "Lumped snow step overdraws its water inventory.";
    next.water = std::max(0., next.water);
    next.maximum = next.water > 0 ? std::max(state.maximum, next.water) : 0;
    next.cover = next.water > 0 ? depletionCover(curve, next.water / next.maximum) : 0;
    if (next.water > 0 && next.cover <= 0)
        return "Snow depletion curve gives no covered area for a nonempty pack.";
    // Keep the full-cover path exactly identical to the point kernel.
    const double target_water = next.cover > 0 ? next.water / next.cover : 0;
    const double energy_fraction = state.cover > 0 ? fraction : 1;
    const double advanced_energy =
        representedEnergy(state) + energy_fraction * result.point.net_energy;
    if (target_water != next.point.water)
        remap(next.point, target_water, p);
    result.net_energy = energy_fraction * result.point.net_energy;
    result.geometry_energy = representedEnergy(next) - advanced_energy;
    result.energy_residual = representedEnergy(state) + result.net_energy + result.geometry_energy -
                             representedEnergy(next);
    result.water_residual = state.water + result.rain + result.snow - result.liquid -
                            result.throughfall - result.sublimation - next.water;
    if (!std::isfinite(result.geometry_energy) ||
        std::abs(result.water_residual) > 1e-10 * std::max(1., state.water))
        return "Lumped snow step failed its conservation checks.";
    state = std::move(next);
    output = std::move(result);
    return {};
}
} // namespace openswmm::surface::ueb
