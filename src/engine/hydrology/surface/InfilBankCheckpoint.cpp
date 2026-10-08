// SPDX-License-Identifier: Apache-2.0
#include "InfilBank.hpp"
#include "../../core/CompletedSourceArchive.hpp"
namespace openswmm::surface {
void InfilBank::checkpoint(detail::CompletedSourceArchive& a,bool model) {
    if(model){a(methods_,owners_,constant_);return;}
    a.fixed(cumulative_);
    a.sequence(horton_,[&](auto& h){a(h.f0,h.fmin,h.decay,h.regen,h.Fmax,h.tp,h.Fe,h.Fmh);},true);
    a.sequence(grnampt_,[&](auto& g){a(g.S,g.Ks,g.IMDmax,g.IMD,g.F,g.Fu,g.Fumax,g.Lu,g.T,g.saturated);},true);
    a.sequence(curvenum_,[&](auto& c){a(c.Smax,c.S,c.Se,c.P,c.F,c.f,c.regen,c.T,c.Tmax);},true);
}
}
