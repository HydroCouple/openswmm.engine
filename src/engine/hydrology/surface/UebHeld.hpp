// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "UebLumped.hpp"

namespace openswmm::surface::ueb {
// A snow solve can span many runoff/marcher steps. The end state is private
// until consumed: readers and checkpoints see only the elapsed share of the
// water/energy balance. Forcing must remain constant over the prepared window;
// drivers end a window at precipitation changes and at SNOW_STEP boundaries.
struct HeldState {
    LumpedState accepted, begin, end;
    LumpedFluxes flux;
    double duration=0, elapsed=0;
    bool pending() const noexcept { return elapsed<duration; }
};
std::string prepareHeld(HeldState&,const Parameters&,const Site&,const Forcing&,
                        const std::array<double,10>&);
std::string consumeHeld(HeldState&,double seconds,const std::array<double,10>&,LumpedFluxes&);
// Discard only the unused prediction, preserving the elapsed inventory. Used
// when a runtime forcing edit ends the old held interval early.
void closeHeld(HeldState&) noexcept;
} // namespace openswmm::surface::ueb
