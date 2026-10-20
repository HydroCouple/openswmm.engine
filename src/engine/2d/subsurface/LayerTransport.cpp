// SPDX-License-Identifier: Apache-2.0
#include "LayerTransport.hpp"
#include <cmath>
#include <algorithm>
namespace openswmm::twoD {
LayerTransportResult solveLayerTransport(const std::vector<double>& capacity,
    const std::vector<double>& rhs,const std::vector<LayerTransportMove>& moves,bool signed_species) {
    LayerTransportResult r;
    const int n=int(capacity.size());
    if(n==0||rhs.size()!=capacity.size()){r.error="Layer transport shape differs";return r;}
    std::vector<double> diagonal=capacity,upper(n,0),lower(n,0),b=rhs,dry(n,0);
    for(int i=0;i<n;++i)if(!std::isfinite(capacity[i])||capacity[i]<0||!std::isfinite(rhs[i])||(!signed_species&&rhs[i]<0)) {
        r.error="Layer transport has inadmissible storage or paired mass";return r;
    }
    for(const auto& move:moves) {
        if(move.from<0||move.from>=n||move.to<-1||move.to>=n||move.from==move.to||
            (move.to>=0&&std::abs(move.to-move.from)!=1)||!std::isfinite(move.capacity)||move.capacity<0) {
            r.error="Invalid layer transport transfer";return r;
        }
        diagonal[move.from]+=move.capacity;
        if(move.to==move.from+1)lower[move.to]-=move.capacity;
        else if(move.to==move.from-1)upper[move.to]-=move.capacity;
    }
    for(int i=0;i<n;++i) {
        if(diagonal[i]==0) {
            if(lower[i]!=0||upper[i]!=0){r.error="Layer transport has a zero-capacity receiving store";return r;}
            dry[i]=b[i];b[i]=0;diagonal[i]=1;
        }
        if(i) {
            const double factor=lower[i]/diagonal[i-1];
            diagonal[i]-=factor*upper[i-1];b[i]-=factor*b[i-1];
        }
        if(!std::isfinite(diagonal[i])||!(diagonal[i]>0)||!std::isfinite(b[i])) {
            r.error="Layer transport system is singular or nonfinite";return r;
        }
    }
    r.concentration.resize(n);
    for(int i=n-1;i>=0;--i) {
        r.concentration[i]=(b[i]-(i+1<n?upper[i]*r.concentration[i+1]:0))/diagonal[i];
        if(!std::isfinite(r.concentration[i])||(!signed_species&&r.concentration[i]<0)) {
            r.error="Layer transport produced inadmissible concentration";return r;
        }
    }
    r.mass.resize(n);for(int i=0;i<n;++i)r.mass[i]=capacity[i]*r.concentration[i]+dry[i];
    r.transferred.reserve(moves.size());
    for(const auto& move:moves)r.transferred.push_back(move.capacity*r.concentration[move.from]);
    return r;
}
}
