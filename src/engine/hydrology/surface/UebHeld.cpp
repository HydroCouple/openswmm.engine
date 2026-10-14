// SPDX-License-Identifier: Apache-2.0
#include "UebHeld.hpp"
#include <algorithm>
#include <cmath>

namespace openswmm::surface::ueb {
namespace {
double energy(const LumpedState& s) {return (s.cover>0?s.cover:1)*s.point.energy;}
double blend(double a,double b,double fraction) {return a+fraction*(b-a);}
void historyAt(State& state,const State& start,double elapsed,const Fluxes& f) {
    state.history=start.history;
    if(elapsed>0)state.history.push_back({elapsed,f.surface_temperature,f.bulk_temperature});
    double total=0;for(const auto& h:state.history)total+=h.seconds;
    double excess=std::max(0.,total-86400.);std::size_t remove=0;
    while(remove<state.history.size()&&excess>=state.history[remove].seconds) {
        excess-=state.history[remove].seconds;++remove;
    }
    state.history.erase(state.history.begin(),state.history.begin()+remove);
    if(!state.history.empty())state.history.front().seconds-=excess;
}
}
std::string prepareHeld(HeldState& s,const Parameters& p,const Site& site,const Forcing& f,
                        const std::array<double,10>& curve) {
    if(s.pending())return "The previous snow interval has not been consumed.";
    auto end=s.accepted;LumpedFluxes flux;
    if(auto error=advanceLumped(end,p,site,f,curve,flux);!error.empty())return error;
    s.begin=s.accepted;s.end=std::move(end);s.flux=std::move(flux);
    s.duration=f.seconds;s.elapsed=0;return {};
}
std::string consumeHeld(HeldState& s,double seconds,const std::array<double,10>& curve,LumpedFluxes& output) {
    // Input dates resolve to a few tenths of a microsecond at current OLE
    // epochs. Accept only that clock roundoff, never a physical extra step.
    constexpr double clock_tolerance=1e-5;
    if(!std::isfinite(seconds)||seconds<=0||!s.pending()||seconds>s.duration-s.elapsed+clock_tolerance)
        return "Snow consumption must lie inside its prepared interval.";
    if(auto error=validateDepletion(curve);!error.empty())return error;
    const double end=std::abs(s.duration-s.elapsed-seconds)<=clock_tolerance?s.duration:s.elapsed+seconds;
    const double share=(end-s.elapsed)/s.duration,progress=end/s.duration;
    auto next=s.accepted;LumpedFluxes result=s.flux;
    result.rain*=share;result.snow*=share;result.liquid*=share;
    result.throughfall*=share;result.sublimation*=share;result.net_energy*=share;
    if(end==s.duration)next=s.end;
    else {
        next.water=blend(s.begin.water,s.end.water,progress);
        next.maximum=next.water>0?std::max(s.begin.maximum,next.water):0;
        next.cover=next.water>0?depletionCover(curve,next.water/next.maximum):0;
        if(next.water>0&&next.cover<=0)return "Snow depletion curve gives no covered area for a nonempty pack.";
        next.point.water=next.cover>0?next.water/next.cover:0;
        next.point.energy=blend(energy(s.begin),energy(s.end),progress)/(next.cover>0?next.cover:1);
        next.point.age=blend(s.begin.point.age,s.end.point.age,progress);
        next.point.refreezing_depth=blend(s.begin.point.refreezing_depth,s.end.point.refreezing_depth,progress);
        next.point.total_refreezing_depth=blend(s.begin.point.total_refreezing_depth,s.end.point.total_refreezing_depth,progress);
        historyAt(next.point,s.begin.point,end,s.flux.point);
    }
    result.geometry_energy=energy(next)-energy(s.accepted)-result.net_energy;
    result.energy_residual=energy(s.accepted)+result.net_energy+result.geometry_energy-energy(next);
    result.water_residual=s.accepted.water+result.rain+result.snow-result.liquid-result.throughfall-result.sublimation-next.water;
    if(!std::isfinite(next.point.energy)||std::abs(result.water_residual)>1e-10*std::max(1.,next.water))
        return "Held snow consumption failed its conservation checks.";
    // Point fluxes describe the prepared solve, not another interval solve.
    // Integrated totals here are the basin fields; diagnostics publish once.
    if(s.elapsed>0) {result.point.steps=0;result.point.retries=0;result.point.diagnostics=0;}
    s.accepted=std::move(next);s.elapsed=end;output=std::move(result);return {};
}
void closeHeld(HeldState& s) noexcept {s.duration=0;s.elapsed=0;}
} // namespace openswmm::surface::ueb
