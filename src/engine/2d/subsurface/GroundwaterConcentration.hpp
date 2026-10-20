// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_GROUNDWATER_CONCENTRATION_HPP
#define OPENSWMM_GROUNDWATER_CONCENTRATION_HPP
#include "SubsurfaceSolver.hpp"
#include <algorithm>
namespace openswmm::twoD {
// Legacy BULK reports total inventory per water volume. The explicit LAYERED
// profile reports a capacity-weighted dissolved concentration for both zones.
inline double groundwaterZoneConcentration(const SubsurfaceSolver& gw,int cell,int species,bool saturated) {
    const auto& tr=gw.transport();const auto& state=gw.state();const auto k=tr.idx(species,cell);
    double capacity=saturated?gw.satVolume(cell):gw.unsatVolume(cell);
    if(tr.n_layers>0&&species!=tr.age_row&&species!=tr.temp_row) {
        const double thickness=saturated?state.hg[cell]:std::max(0.,state.zs[cell]-state.hg[cell]);
        capacity+=state.area[cell]*thickness*tr.kd[k]*tr.rho_b[cell];
    }
    const double mass=saturated?tr.sat_mass[k]:tr.unsat_mass[k];
    return capacity>0?mass/capacity:0;
}
}
#endif
