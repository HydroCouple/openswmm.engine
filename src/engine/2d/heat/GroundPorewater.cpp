// SPDX-License-Identifier: Apache-2.0
#include "GroundPorewater.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
namespace openswmm::twoD {
namespace {
double bulk(const PorousHeatColumn& soil,std::size_t j) {return soil.area*(soil.depth_edges[j+1]-soil.depth_edges[j]);}
double saturatedFraction(const PorousHeatColumn& soil,std::size_t j,double sat) {
    const double pore=soil.material.porosity*bulk(soil,j);return pore>0?std::clamp(sat/pore,0.,1.):0;
}
}
std::string GroundPorewaterColumn::initialize(const PorousHeatColumn& soil,const std::vector<double>& saturated,
    const std::vector<double>& sat_mass,const std::vector<double>& unsat_mass,const std::vector<double>& kd,int age,int temperature) {
    const auto ns=sat_mass.size(),nl=soil.theta.size();
    if(!ns||!nl||saturated.size()!=nl||unsat_mass.size()!=ns||kd.size()!=ns||age<-1||temperature<-1||
       age>=int(ns)||temperature>=int(ns)||(age>=0&&age==temperature))return "Ground porewater seed shapes or row indices differ.";
    for(std::size_t j=0;j<nl;++j) {
        const double water=soil.theta[j]*bulk(soil,j);
        if(!std::isfinite(saturated[j])||saturated[j]<0||saturated[j]>water+1e-12*std::max(1.,water))
            return "Ground porewater saturated overlap exceeds its physical layer water.";
    }
    GroundPorewaterColumn next;next.n_species=int(ns);next.age_row=age;next.temp_row=temperature;
    next.mass.assign(ns*nl,0);next.retention=next.mass;next.contact_mass=next.mass;
    next.reserved_out=next.mass;next.generated_age.assign(nl,0);
    for(std::size_t r=0;r<ns;++r) {
        if(!std::isfinite(kd[r])||kd[r]<0)return "Ground porewater Kd must be finite and nonnegative.";
        if(int(r)==temperature)continue;
        if(!std::isfinite(sat_mass[r])||!std::isfinite(unsat_mass[r])||sat_mass[r]<0||unsat_mass[r]<0)
            return "Ground porewater initial total masses must be finite and nonnegative.";
        std::vector<double> sat_capacity(nl),unsat_capacity(nl);double sat_total=0,unsat_total=0;
        for(std::size_t j=0;j<nl;++j) {
            const auto k=r*nl+j;
            const double retained=int(r)==age?0:(1-soil.material.porosity)*soil.material.grain_density*kd[r]*bulk(soil,j);
            if(!std::isfinite(retained))return "Ground porewater retention capacity overflow.";
            next.retention[k]=retained;
            sat_capacity[j]=saturated[j]+retained*saturatedFraction(soil,j,saturated[j]);
            unsat_capacity[j]=std::max(0.,soil.theta[j]*bulk(soil,j)-saturated[j])+retained*(1-saturatedFraction(soil,j,saturated[j]));
            sat_total+=sat_capacity[j];unsat_total+=unsat_capacity[j];
        }
        if(!std::isfinite(sat_total)||!std::isfinite(unsat_total)||
           (!(sat_total>0)&&sat_mass[r]>0)||(!(unsat_total>0)&&unsat_mass[r]>0))
            return "Nonzero ground porewater initial mass has no finite physical zone capacity.";
        for(std::size_t j=0;j<nl;++j) {
            next.mass[r*nl+j]=(sat_total>0?sat_mass[r]*(sat_capacity[j]/sat_total):0)+
                              (unsat_total>0?unsat_mass[r]*(unsat_capacity[j]/unsat_total):0);
            if(!std::isfinite(next.mass[r*nl+j]))return "Ground porewater initial mass overflow.";
        }
    }
    next.initial_mass=next.mass;*this=std::move(next);return {};
}
std::pair<std::vector<double>,std::vector<double>> GroundPorewaterColumn::zoneMasses(const PorousHeatColumn& soil,
    const std::vector<double>& saturated) const {
    const auto nl=soil.theta.size();
    if(saturated.size()!=nl||mass.size()!=std::size_t(n_species)*nl||retention.size()!=mass.size())
        throw std::invalid_argument("Ground porewater view shapes differ.");
    std::vector<double> sat(n_species,0),unsat(n_species,0);
    for(int r=0;r<n_species;++r){const auto pair=zoneMass(soil,saturated,r);sat[r]=pair.first;unsat[r]=pair.second;}
    return {std::move(sat),std::move(unsat)};
}
std::pair<double,double> GroundPorewaterColumn::zoneMass(const PorousHeatColumn& soil,
    const std::vector<double>& saturated,int r) const noexcept {
    double sat=0,unsat=0;const auto nl=soil.theta.size();
    if(r==temp_row||r<0||r>=n_species)return {0,0};
    for(std::size_t j=0;j<nl;++j) {
        const auto k=std::size_t(r)*nl+j;
        const double water=soil.theta[j]*bulk(soil,j),capacity=water+retention[k],fraction=saturatedFraction(soil,j,saturated[j]);
        const double share=capacity>0?std::clamp((saturated[j]+retention[k]*fraction)/capacity,0.,1.):fraction;
        const double carried=mass[k]*share;sat+=carried;unsat+=mass[k]-carried;
    }
    return {sat,unsat};
}
bool GroundPorewaterColumn::generateAge(const PorousHeatColumn& soil,double dt) noexcept {
    if(age_row<0)return true;
    const auto nl=soil.theta.size(),base=std::size_t(age_row)*nl;
    if(!std::isfinite(dt)||dt<0||generated_age.size()!=nl||mass.size()!=std::size_t(n_species)*nl)return false;
    for(std::size_t j=0;j<nl;++j) {
        const double generated=dt*soil.theta[j]*bulk(soil,j);
        if(!std::isfinite(generated)||!std::isfinite(mass[base+j]+generated)||!std::isfinite(generated_age[j]+generated))return false;
    }
    for(std::size_t j=0;j<nl;++j){const double generated=dt*soil.theta[j]*bulk(soil,j);mass[base+j]+=generated;generated_age[j]+=generated;}
    return true;
}
double GroundPorewaterColumn::contactAvailableMass(int r,std::size_t j) const noexcept {
    if(n_species<=0||r<0||r>=n_species||retention.size()%std::size_t(n_species)!=0||
       mass.size()!=retention.size()||reserved_out.size()!=mass.size()||r==temp_row)
        return std::numeric_limits<double>::quiet_NaN();
    const auto nl=retention.size()/std::size_t(n_species);if(j>=nl)return std::numeric_limits<double>::quiet_NaN();
    const auto k=std::size_t(r)*nl+j;
    const double available=mass[k]-reserved_out[k];
    if(!std::isfinite(mass[k])||!std::isfinite(reserved_out[k])||mass[k]<0||reserved_out[k]<0||available<0)
        return std::numeric_limits<double>::quiet_NaN();
    return available;
}
bool GroundPorewaterColumn::canReserveOutgoing(int r,std::size_t j,double amount) const noexcept {
    const double available=contactAvailableMass(r,j);
    const auto nl=retention.size()/std::size_t(std::max(1,n_species));
    if(!std::isfinite(available)||!std::isfinite(amount)||amount<0||r<0||r>=n_species||j>=nl)return false;
    const auto k=std::size_t(r)*nl+j;
    return std::isfinite(reserved_out[k]+amount)&&amount<=available;
}
bool GroundPorewaterColumn::reserveOutgoing(int r,std::size_t j,double amount) noexcept {
    if(!canReserveOutgoing(r,j,amount))return false;
    const auto nl=retention.size()/std::size_t(std::max(1,n_species)),k=std::size_t(r)*nl+j;
    reserved_out[k]+=amount;return true;
}
bool GroundPorewaterColumn::settleOutgoingReservation(int r,std::size_t j,double amount) noexcept {
    const auto nl=retention.size()/std::size_t(std::max(1,n_species));
    if(r<0||r>=n_species||j>=nl||reserved_out.size()!=mass.size()||!std::isfinite(amount)||amount<0)return false;
    const auto k=std::size_t(r)*nl+j;
    if(!std::isfinite(reserved_out[k])||amount>reserved_out[k])return false;
    reserved_out[k]=(amount==reserved_out[k])?0:reserved_out[k]-amount;
    return true;
}
bool GroundPorewaterColumn::canApplyContactMassIncrement(int r,std::size_t j,double increment) const noexcept {
    const auto nl=retention.size()/std::size_t(std::max(1,n_species));
    if(r<0||r>=n_species||j>=nl||mass.size()!=retention.size()||reserved_out.size()!=mass.size()||
       contact_mass.size()!=mass.size()||r==temp_row||!std::isfinite(increment))return false;
    const auto k=std::size_t(r)*nl+j;const double next=mass[k]+increment,receipt=contact_mass[k]+increment;
    return std::isfinite(next)&&std::isfinite(reserved_out[k])&&next>=reserved_out[k]&&reserved_out[k]>=0&&
        std::isfinite(receipt);
}
bool GroundPorewaterColumn::applyContactMassIncrement(int r,std::size_t j,double increment) noexcept {
    if(!canApplyContactMassIncrement(r,j,increment))return false;
    const auto nl=retention.size()/std::size_t(std::max(1,n_species)),k=std::size_t(r)*nl+j;
    mass[k]+=increment;contact_mass[k]+=increment;return true;
}
} // namespace openswmm::twoD
