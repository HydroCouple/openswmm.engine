// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "UebHeld.hpp"
#include <vector>
namespace openswmm {
struct SimulationContext;
namespace snow {struct SnowSoA;}
namespace surface::ueb {
struct NativeState {
    std::array<double,4> forcing{}; // modes and values of runtime climate overrides
    std::vector<HeldState> columns;
    std::vector<LumpedFluxes> last;
    std::vector<double> rain, snow;
    std::vector<double> sample_end, sample_seconds; // per column: independent owner clocks
    bool active=false;
    double sublimation=0, precipitation=0, liquid=0, throughfall=0;
    double net_energy=0, geometry_energy=0, plowed_energy=0, initial_energy=0;
    double initial_water=0, removed=0, transferred_water=0;
    double last_seconds=0, completed_seconds=0;
};
std::string initializeSubcatchments(SimulationContext&,snow::SnowSoA&);
// Rain and snow are already partitioned, catch-corrected native ft/s rates.
// Native date is the START of the runoff interval, elapsed is seconds since
// simulation start. Publishing and all per-column solves are transactional.
std::string advanceSubcatchments(SimulationContext&,snow::SnowSoA&,double date,
    double elapsed,double seconds,const double* rain,const double* snow);
double nextSnowBoundary(const SimulationContext&,double elapsed,double proposed) noexcept;
} // namespace surface::ueb
} // namespace openswmm
