// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "UebSnow.hpp"
#include <array>

namespace openswmm::surface::ueb {
// The point state describes the snow-covered part; water is the average over
// the entire element. Curves contain the SWMM ordinates at 0, .1, ... .9,
// with an implicit final ordinate of 1. Mesh elements use the project
// pervious-area curve, matching their UEB snowpack ownership.
struct LumpedState {
    State point;
    double water=0, maximum=0, cover=0;
};
struct LumpedFluxes {
    Fluxes point;
    double rain=0, snow=0, liquid=0, throughfall=0, sublimation=0;
    double active_fraction=0, water_residual=0;
    // UEB's depletion adjustment preserves temperature/liquid fraction. Its
    // change in represented energy is reported separately from physical
    // forcing; it must never be disguised as numerical energy closure.
    double net_energy=0, geometry_energy=0, energy_residual=0;
};
std::string validateDepletion(const std::array<double,10>&);
double depletionCover(const std::array<double,10>&,double relative_water) noexcept;
std::string initializeLumped(LumpedState&,double water,double point_energy,
                             double maximum,const std::array<double,10>&);
std::string advanceLumped(LumpedState&,const Parameters&,const Site&,const Forcing&,
                         const std::array<double,10>&,LumpedFluxes&);
} // namespace openswmm::surface::ueb
