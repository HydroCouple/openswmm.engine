// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "SigmaPorewaterMap.hpp"
#include "../subsurface/SubsurfaceTransportState.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace openswmm::twoD {
std::string SigmaPorewaterMap::configure(int m,int ns,std::vector<Overlap> entries,
    const std::vector<double>& partition,const std::vector<double>& canonical,const std::vector<double>& held) {
    if(m<=0||ns<=0||int(partition.size())!=ns)return "Invalid SIGMA contact mapping layout.";
    SigmaPorewaterMap next;next.layers=m;next.species=ns;next.overlaps=std::move(entries);
    const auto ne=next.overlaps.size();next.capacity.resize(std::size_t(ns)*ne);
    next.mass.resize(next.capacity.size());next.reserved.resize(next.capacity.size());
    next.pending.assign(std::size_t(ns)*(m+1),0);
    for(int s=0;s<ns;++s)for(std::size_t e=0;e<ne;++e) {
        const auto& o=next.overlaps[e];
        if(o.physical<0||o.reactor<0||o.reactor>m||!std::isfinite(o.water)||o.water<0||
           !std::isfinite(o.bulk)||o.bulk<=0||!std::isfinite(partition[s])||partition[s]<0)
            return "Invalid SIGMA contact overlap geometry.";
        const double value=o.water+partition[s]*o.bulk;
        if(!std::isfinite(value))return "SIGMA contact capacity overflow.";
        next.capacity[std::size_t(s)*ne+e]=value;
    }
    if(auto error=next.rebase(canonical,held);!error.empty())return error;
    *this=std::move(next);return {};
}
std::string SigmaPorewaterMap::rebase(const std::vector<double>& canonical,const std::vector<double>& held) {
    const auto ne=overlaps.size();const int nr=layers+1;
    if(canonical.size()!=std::size_t(species)*nr||held.size()!=std::size_t(species))
        return "SIGMA contact canonical shape differs.";
    auto values=mass,claims=reserved;
    for(int s=0;s<species;++s)for(int r=0;r<nr;++r) {
        const double total=canonical[std::size_t(s)*nr+r],promised=r==layers?held[s]:0;
        if(!std::isfinite(total)||total<0||!std::isfinite(promised)||promised<0||promised>total)
            return "SIGMA contact inventory or reservation is invalid.";
        double sum=0,largest=0;std::size_t anchor=ne;
        for(std::size_t e=0;e<ne;++e)if(overlaps[e].reactor==r) {
            const double c=capacity[std::size_t(s)*ne+e];sum+=c;
            if(c>largest){largest=c;anchor=e;}
        }
        if(!(sum>0)) { // Zero-capacity dry inventory remains with its canonical owner.
            if(promised>0)return "SIGMA contact reservation has no aqueous donor.";
            for(std::size_t e=0;e<ne;++e)if(overlaps[e].reactor==r)values[std::size_t(s)*ne+e]=claims[std::size_t(s)*ne+e]=0;
            continue;
        }
        double assigned=0,reservation=0;
        for(std::size_t e=0;e<ne;++e)if(overlaps[e].reactor==r && e!=anchor) {
            const auto k=std::size_t(s)*ne+e;const double fraction=capacity[k]/sum;
            values[k]=total*fraction;claims[k]=promised*fraction;assigned+=values[k];reservation+=claims[k];
        }
        const auto k=std::size_t(s)*ne+anchor;values[k]=total-assigned;claims[k]=promised-reservation;
        if(values[k]<claims[k]||claims[k]<0)return "SIGMA contact reservation cannot be partitioned.";
    }
    mass=std::move(values);reserved=std::move(claims);return {};
}
bool SigmaPorewaterMap::apply(int row,int physical,double increment) noexcept {
    try {
        if(row<0||row>=species||!std::isfinite(increment))return false;
        const auto ne=overlaps.size();double total=0,largest=0;std::size_t anchor=ne;
        for(std::size_t e=0;e<ne;++e)if(overlaps[e].physical==physical) {
            const auto k=std::size_t(row)*ne+e;
            const double weight=increment>=0?capacity[k]:mass[k]-reserved[k];
            if(!std::isfinite(weight)||weight<0)return false;
            total+=weight;if(weight>largest){largest=weight;anchor=e;}
        }
        if(increment==0)return true;
        if(!(total>0)||!std::isfinite(total)||anchor==ne)return false;
        auto next_mass=mass,next_pending=pending;double assigned=0;
        const auto add=[&](std::size_t e,double delta) {
            const auto k=std::size_t(row)*ne+e,q=std::size_t(row)*(layers+1)+overlaps[e].reactor;
            next_mass[k]+=delta;next_pending[q]+=delta;
            return std::isfinite(next_mass[k])&&next_mass[k]>=reserved[k]&&std::isfinite(next_pending[q]);
        };
        for(std::size_t e=0;e<ne;++e)if(overlaps[e].physical==physical && e!=anchor) {
            const auto k=std::size_t(row)*ne+e;
            const double weight=increment>=0?capacity[k]:mass[k]-reserved[k];
            const double delta=increment*(weight/total);
            if(!add(e,delta))return false;assigned+=delta;
        }
        if(!add(anchor,increment-assigned))return false;
        mass=std::move(next_mass);pending=std::move(next_pending);return true;
    } catch(...) {return false;}
}
std::string SigmaPorewaterMap::publish(SubsurfaceTransportState& tr,int cell) {
    if(layers!=tr.n_layers||species!=tr.n_species)return "SIGMA contact owner layout differs.";
    const int nr=layers+1;std::vector<double> canonical(std::size_t(species)*nr),held(species,0);
    for(int s=0;s<species;++s) {
        if(s==tr.temp_row)continue;
        for(int r=0;r<nr;++r) {
            const auto q=std::size_t(s)*nr+r;
            canonical[q]=(r==layers?tr.sat_mass[tr.idx(s,cell)]:tr.layer_mass[tr.layerIdx(s,r,cell)])+pending[q];
        }
        for(std::size_t e=0;e<overlaps.size();++e)held[s]+=reserved[std::size_t(s)*overlaps.size()+e];
    }
    auto next=*this;if(auto error=next.rebase(canonical,held);!error.empty())return error;
    for(int s=0;s<species;++s)if(s!=tr.temp_row) {
        for(int r=0;r<nr;++r) {
            const auto q=std::size_t(s)*nr+r;
            if(r==layers)tr.sat_mass[tr.idx(s,cell)]=canonical[q];
            else tr.layer_mass[tr.layerIdx(s,r,cell)]=canonical[q];
        }
        tr.refreshUnsat(s,cell);
    }
    std::fill(next.pending.begin(),next.pending.end(),0);*this=std::move(next);return {};
}
} // namespace openswmm::twoD
