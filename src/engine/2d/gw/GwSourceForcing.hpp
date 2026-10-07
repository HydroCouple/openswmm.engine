// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_GW_SOURCE_FORCING_HPP
#define OPENSWMM_GW_SOURCE_FORCING_HPP
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>
namespace openswmm::twoD {
// Immutable linear series in elapsed simulation seconds. Endpoint hold matches
// the standard SWMM continuous table lookup. Empty times denotes a constant.
struct GwSourceSeries { std::vector<double> times, values; bool hasNegative = false; };
struct GwSourceSignal {
    double constant = 0.0;
    // Inline vectors support small programmatic signals; resolved named
    // series share one immutable allocation across all cell source rows.
    std::vector<double> times, values;
    std::shared_ptr<const GwSourceSeries> series;
    const std::vector<double>& timePoints() const noexcept { return series ? series->times : times; }
    const std::vector<double>& ordinates() const noexcept { return series ? series->values : values; }
    double at(double t) const noexcept {
        const auto& times = timePoints();
        const auto& values = ordinates();
        if(times.empty())return constant;
        if(t<=times.front())return values.front();
        if(t>=times.back())return values.back();
        const auto hi=std::upper_bound(times.begin(),times.end(),t);
        const auto i=static_cast<std::size_t>(hi-times.begin());
        const double f=(t-times[i-1])/(times[i]-times[i-1]);
        return values[i-1]+f*(values[i]-values[i-1]);
    }
};
struct GwResolvedSourceTerm {
    int row = -1;
    bool massRate = false; // pollutant native mass/s, converted by massScale
    double massScale = 1.0;
    GwSourceSignal signal;
};
struct GwResolvedSource {
    std::string name;
    double scale = 1.0;
    GwSourceSignal flow;
    std::vector<std::pair<int,double>> cells; // active cell, share of TOTAL source
    std::vector<GwResolvedSourceTerm> terms;
};
struct GwSourcePulse {
    double in = 0.0, out = 0.0; // separately integrated positive/negative flow
    std::vector<double> mass;
};
// Exact integral on each common linear interval, including sign changes in
// flow. CONC only enters on injection; extraction carries in-situ dissolved
// quality. MASS is an independent nonnegative mass loading (also at Q=0).
inline void integrateGwSource(const GwResolvedSource&s,double t,double dt,double weight,GwSourcePulse&pulse) {
    if(!(dt>0)||!(weight>0)||!(s.scale>0))return;
    const double end=t+dt,scale=weight*s.scale;
    std::vector<double> knots{t,end};
    auto add=[&](const GwSourceSignal&v){
        const auto& times = v.timePoints();
        for (auto it=std::upper_bound(times.begin(),times.end(),t);it!=times.end()&&*it<end;++it)
            knots.push_back(*it);
    };
    add(s.flow);for(const auto&term:s.terms)add(term.signal);
    std::sort(knots.begin(),knots.end());knots.erase(std::unique(knots.begin(),knots.end()),knots.end());
    for(std::size_t i=1;i<knots.size();++i) {
        const double a=knots[i-1],b=knots[i],qa=s.flow.at(a),qb=s.flow.at(b);
        std::vector<double> cuts{a,b};
        if((qa<0&&qb>0)||(qa>0&&qb<0))cuts.insert(cuts.begin()+1,a+(b-a)*(-qa)/(qb-qa));
        for(std::size_t k=1;k<cuts.size();++k) {
            const double lo=cuts[k-1],hi=cuts[k],h=hi-lo,q0=s.flow.at(lo),q1=s.flow.at(hi);
            const double volume=.5*(q0+q1)*h*scale;
            if(volume>=0)pulse.in+=volume;else pulse.out-=volume;
            for(const auto&term:s.terms) {
                if(term.row<0||static_cast<std::size_t>(term.row)>=pulse.mass.size())continue;
                const double c0=term.signal.at(lo),c1=term.signal.at(hi);
                double m=0;
                if(term.massRate)m=.5*(c0+c1)*h*term.massScale;
                else if(volume>0)m=h*(2*q0*c0+q0*c1+q1*c0+2*q1*c1)/6;
                pulse.mass[static_cast<std::size_t>(term.row)]+=m*scale;
            }
        }
    }
}
} // namespace openswmm::twoD
#endif
