// SPDX-License-Identifier: Apache-2.0

/**
 * @file      UebSnow.hpp
 * @brief     Utah Energy Balance snow accumulation and energy integration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <string>
#include <vector>

namespace openswmm::surface::ueb {
enum class TemperatureMethod { ModifiedForceRestore, Equilibrium };
/** @brief Point UEB material, radiation and integration parameters.
 * Temperatures are Celsius, lengths m, densities kg/m3, heat capacities kJ/(kg K),
 * conductivities kJ/(m K h), drainage/conductance m/h, max_step seconds. Fractions,
 * age responses and stability are dimensionless; low_frequency retains the
 * reference hourly convention. validate() checks all fields before public advance. */
struct Parameters {
    TemperatureMethod temperature = TemperatureMethod::ModifiedForceRestore;
    double rain_temperature = 3, snow_temperature = -1, emissivity = .99;
    double roughness = .01, measurement_height = 2, snow_density = 200, soil_density = 1700;
    double soil_heat_capacity = 2.09, active_soil_depth = .1;
    double liquid_retention = .02, drainage_conductivity = 200; // m/h, reference parameter units
    double equilibrium_conductance = .02;                       // m/h
    double snow_conductivity = .33, soil_conductivity = 6.5;    // kJ/(m K h)
    double low_frequency = .0654, amplitude = 1, stability = 1;
    double ground_albedo = .25, visible_albedo = .85, infrared_albedo = .65;
    double new_snow_depth = .002, age_dust = .03;
    double visible_age = .2, infrared_age = .5;
    double max_step = 3600; // seconds; kernel subdivides longer forcing intervals
    /** @brief Check formulation, finite fields and implemented physical parameter bounds.
     * @return Empty if all fields qualify; otherwise first diagnostic without mutation.
     * Checks ordered rain/snow temperatures, emissivity/albedo/response/stability fractions,
     * positive geometry/density/thermal coefficients, feasible retention, nonnegative
     * conductivity/new-snow/dust and max_step in [1,21600] seconds. Does not validate a
     * particular site, state, forcing or mathematical convergence. */
    std::string validate() const;
};
/** @brief Per-column environmental settings.
 * forest is canopy fraction, drift a snow multiplier, pressure Pa, ground_heat
 * prescribed W/m2 into the snow system, extinction_depth m. Public advance checks
 * forest [0,1], drift>=0, positive pressure/depth and finite signed ground heat. */
struct Site {
    double forest = 0, drift = 1, pressure = 101325, ground_heat = 0, extinction_depth = .1;
};
/** @brief Held point meteorology over one requested interval.
 * seconds is duration, precipitation integrated liquid-equivalent m, temperature
 * Celsius, wind m/s, humidity [0,1], incoming radiation W/m2, cosine [0,1]. Catch and
 * drift apply to snowfall; snow_fraction=-1 selects temperature partition. Other
 * fields remain held while advance splits time and apportions precipitation. */
struct Forcing {
    double seconds = 0, precipitation = 0, temperature = 0, wind = 0, humidity = .5;
    double shortwave = 0, longwave = 0, cosine = 0; // W/m2; cosine of incident sun angle
    double snow_catch = 1;
    double snow_fraction = -1; // -1 uses UEB temperature partition; [0,1] prescribes it
};
/** @brief One duration-weighted preceding thermal sample (seconds and Celsius). */
struct ThermalSample {
    double seconds = 0, surface = 0, bulk = 0;
};
/** @brief One owned point snow/active-soil energy and thermal-memory inventory.
 * energy kJ/m2 may be signed, water m SWE, age dimensionless optical age,
 * refreezing depths m. The duration-weighted history spans at most 24 hours.
 * No forcing/runtime context is shared between independent point states. */
struct State {
    double energy = 0, water = 0, age = 0, refreezing_depth = 0, total_refreezing_depth = 0;
    std::vector<ThermalSample> history; // at most the preceding 24 hours, duration weighted
};
/** @brief Integrated accepted point exchanges and final thermal diagnostics.
 * Water depths m and energy kJ/m2 cover the whole requested interval. Temperature
 * Celsius and albedo are final accepted substep values, not interval averages.
 * steps counts accepted leaves; retries counts successful binary splits. */
struct Fluxes {
    double rain = 0, snow = 0, liquid = 0,
           sublimation = 0;                          // integrated m; sublimation may be negative
    double net_energy = 0, outgoing_melt_energy = 0; // integrated kJ/m2
    double surface_temperature = 0, bulk_temperature = 0, albedo = 0;
    double water_residual = 0, energy_residual = 0;
    int steps = 0, retries = 0, diagnostics = 0;
};
/** @brief Advance one isolated point pack with bounded reference solves and whole-call publication.
 * @param[in,out] state Valid finite inventory with nonnegative water/age/refreezing
 * depths and positive-duration thermal samples above absolute zero spanning at most
 * 24 hours. Replaced only after the full requested interval is accepted.
 * @param parameters Valid material/integration configuration, checked on entry.
 * @param site Finite valid forest/drift/pressure/ground-heat/extinction settings.
 * @param forcing Positive finite duration seconds; nonnegative precipitation m, wind
 * m/s and incoming W/m2; temperature in (-100,70] Celsius; humidity/cosine [0,1];
 * nonnegative catch; snow_fraction exactly -1 or [0,1]. Meteorology is held constant.
 * @param[out] output Replaced on success with integrated m/kJ-per-m2 fluxes, summed
 * water/energy residuals and counts, plus endpoint temperatures/albedo. Retained on
 * normal failure. Signed sublimation includes deposition.
 * @return Empty on success; parameter/state/site/forcing, 100000-substep limit,
 * conservation/nonfinite/convergence or caught solver-exception diagnostic otherwise.
 * Splits by max_step, apportions precipitation exactly, retries rejected steps by
 * halving to depth 16 or 0.001 s, and publishes only a complete private candidate.
 * Candidate copy before the guarded loop can throw; no caller state is then changed.
 * Reference diagnostics reject a leaf. No global clocks, other packs or external
 * water/energy ledgers are mutated. Serialize access to the same state/output. */
std::string advance(State& state, const Parameters& parameters, const Site& site,
                    const Forcing& forcing, Fluxes& output);
/** @brief Convert signed represented snow/active-soil energy to bulk temperature.
 * @param state Finite energy kJ/m2 and water m; nonnegative water required.
 * @param parameters Positive density/depth/heat-capacity fields guaranteeing positive
 * combined snow/soil heat capacity; not validated here.
 * @return Celsius: cold energy/capacity, zero through the latent melt range, or
 * excess positive energy/capacity after complete melting. No state mutation/clamping
 * of invalid inputs; malformed capacity can produce nonfinite output. */
double bulkTemperature(const State& state, const Parameters& parameters) noexcept;
}
