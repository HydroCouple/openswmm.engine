// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>

namespace openswmm::surface::ueb {
enum class TemperatureMethod { ModifiedForceRestore, Equilibrium };
struct Parameters {
    TemperatureMethod temperature=TemperatureMethod::ModifiedForceRestore;
    double rain_temperature=3, snow_temperature=-1, emissivity=.99;
    double roughness=.01, measurement_height=2, snow_density=200, soil_density=1700;
    double soil_heat_capacity=2.09, active_soil_depth=.1;
    double liquid_retention=.02, drainage_conductivity=200; // m/h, reference parameter units
    double equilibrium_conductance=.02; // m/h
    double snow_conductivity=.33, soil_conductivity=6.5; // kJ/(m K h)
    double low_frequency=.0654, amplitude=1, stability=1;
    double ground_albedo=.25, visible_albedo=.85, infrared_albedo=.65;
    double new_snow_depth=.002, age_dust=.03;
    double visible_age=.2, infrared_age=.5;
    double max_step=3600; // seconds; kernel subdivides longer forcing intervals
    std::string validate() const;
};
struct Site {
    double forest=0, drift=1, pressure=101325, ground_heat=0, extinction_depth=.1;
};
struct Forcing {
    double seconds=0, precipitation=0, temperature=0, wind=0, humidity=.5;
    double shortwave=0, longwave=0, cosine=0; // W/m2; cosine of incident sun angle
    double snow_catch=1;
    double snow_fraction=-1; // -1 uses UEB temperature partition; [0,1] prescribes it
};
struct ThermalSample { double seconds=0, surface=0, bulk=0; };
struct State {
    double energy=0, water=0, age=0, refreezing_depth=0, total_refreezing_depth=0;
    std::vector<ThermalSample> history; // at most the preceding 24 hours, duration weighted
};
struct Fluxes {
    double rain=0, snow=0, liquid=0, sublimation=0; // integrated m; sublimation may be negative
    double net_energy=0, outgoing_melt_energy=0; // integrated kJ/m2
    double surface_temperature=0, bulk_temperature=0, albedo=0;
    double water_residual=0, energy_residual=0;
    int steps=0, retries=0, diagnostics=0;
};
// One point pack for either a cell or the snow-covered share of a subcatchment.
// The whole requested interval publishes atomically. No forcing, state or
// thermal memory is shared between columns or concurrent engine instances.
std::string advance(State&,const Parameters&,const Site&,const Forcing&,Fluxes&);
double bulkTemperature(const State&,const Parameters&) noexcept;
}
