// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "ConservativeMixing.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace openswmm::quality {
bool ConservativeMixing::prepare(const MixingWater& water, double tolerance) {
    prepared_=false;
    const auto n=water.old_volume.size();
    if (!n || water.new_volume.size()!=n || water.source_volume.size()!=n ||
        water.exported.size()!=n || water.seeped.size()!=n || water.evaporated.size()!=n ||
        !std::isfinite(tolerance) || tolerance<0) return false;
    for (const auto* v : {&water.old_volume,&water.new_volume,&water.source_volume,
                           &water.exported,&water.seeped,&water.evaporated})
        for (double x : *v) if (!std::isfinite(x) || x<0) return false;
    water_=water;
    diagonal_.resize(n); rhs_.resize(n); image_.resize(n); guess_.resize(n);
    incoming_=water.source_volume; outgoing_.resize(n);
    auto& incoming=incoming_; auto& outgoing=outgoing_;
    for (std::size_t i=0;i<n;++i) {
        incoming[i]+=water.old_volume[i];
        outgoing[i]=water.new_volume[i]+water.exported[i]+water.seeped[i]+water.evaporated[i];
        diagonal_[i]=water.new_volume[i]+water.exported[i]+water.seeped[i];
    }
    for (const auto& t:water.transfers) {
        if (t.donor>=n || t.receiver>=n || !std::isfinite(t.volume) || t.volume<0) return false;
        outgoing[t.donor]+=t.volume;
        incoming[t.receiver]+=t.volume;
        diagonal_[t.donor]+=t.volume;
    }
    water_residual_=0;
    for (std::size_t i=0;i<n;++i) {
        const double residual=std::abs(incoming[i]-outgoing[i]);
        water_residual_=std::max(water_residual_,residual/std::max({1.0,incoming[i],outgoing[i]}));
        if (residual>tolerance*std::max({1.0,incoming[i],outgoing[i]})) return false;
    }
    prepared_=true;
    return true;
}

ConservativeMixing::Result ConservativeMixing::solve(const MixingScalar& scalar, MixedScalar& output) {
    Result result;
    result.max_water_residual=water_residual_;
    const auto n=diagonal_.size();
    if (!prepared_ || scalar.old_concentration.size()!=n || scalar.source_mass.size()!=n ||
        (!scalar.held_dry_mass.empty() && scalar.held_dry_mass.size()!=n)) return result;
    auto& accepted=candidate_;
    accepted.held_dry_mass.assign(n,0.0);
    for (std::size_t i=0;i<n;++i) {
        const double held=scalar.held_dry_mass.empty()?0.0:scalar.held_dry_mass[i];
        if (!std::isfinite(scalar.old_concentration[i]) || !std::isfinite(scalar.source_mass[i]) ||
            !std::isfinite(held)) return result;
        const double mass=water_.old_volume[i]*scalar.old_concentration[i]+scalar.source_mass[i]+held;
        // A mass-only source at a genuinely dry disconnected control volume
        // remains inventory; it is never interpreted as a concentration.
        if (diagonal_[i]==0) {
            accepted.held_dry_mass[i]=mass;
            rhs_[i]=0; guess_[i]=0;
        } else {
            rhs_[i]=mass/diagonal_[i];
            guess_[i]=scalar.old_concentration[i];
        }
    }
    auto matrix=[&](const double* x,double* y) {
        for (std::size_t i=0;i<n;++i) y[i]=x[i];
        for (const auto& t:water_.transfers)
            if (t.volume>0 && diagonal_[t.receiver]>0)
                y[t.receiver]-=t.volume/diagonal_[t.receiver]*x[t.donor];
        return true;
    };
    matrix(guess_.data(),image_.data());
    for (std::size_t i=0;i<n;++i) rhs_[i]-=image_[i];
    fv::NativeGmres::Controls controls;
    controls.relative_tolerance=1e-13;
    controls.absolute_tolerance=1e-14;
    controls.max_iterations=std::max(200,static_cast<int>(std::min<std::size_t>(n*2,2000)));
    const auto linear=gmres_.solve(rhs_,matrix,{},controls,correction_);
    result.iterations=linear.iterations;
    if (!linear.converged()) return result;
    accepted.concentration.resize(n);
    accepted.exported_mass.resize(n); accepted.seeped_mass.resize(n);
    double scale=1,initial=0,final=0;
    for (std::size_t i=0;i<n;++i) {
        const double c=guess_[i]+correction_[i];
        const double held=scalar.held_dry_mass.empty()?0.0:scalar.held_dry_mass[i];
        if (!std::isfinite(c) || (scalar.nonnegative &&
            (c < -1e-12*std::max(1.0,std::abs(guess_[i])) || accepted.held_dry_mass[i]<0))) return result;
        accepted.concentration[i]=scalar.nonnegative?std::max(0.0,c):c;
        accepted.exported_mass[i]=water_.exported[i]*accepted.concentration[i];
        accepted.seeped_mass[i]=water_.seeped[i]*accepted.concentration[i];
        const double before=water_.old_volume[i]*scalar.old_concentration[i];
        initial+=before+scalar.source_mass[i]+held;
        scale+=std::abs(before)+std::abs(scalar.source_mass[i])+std::abs(held);

    }
    accepted.transfer_mass.clear(); accepted.transfer_mass.reserve(water_.transfers.size());
    for (const auto& t:water_.transfers) {
        const double mass=t.volume*accepted.concentration[t.donor];
        accepted.transfer_mass.push_back(mass);
        if (diagonal_[t.receiver]==0) accepted.held_dry_mass[t.receiver]+=mass;
    }
    for (std::size_t i=0;i<n;++i)
        final+=water_.new_volume[i]*accepted.concentration[i]+accepted.held_dry_mass[i]+
               accepted.exported_mass[i]+accepted.seeped_mass[i];
    result.mass_residual=(initial-final)/scale;
    if (std::abs(result.mass_residual)>1e-9) return result;
    result.converged=true;
    std::swap(output,accepted); // retain both buffers; no partial state on failure
    return result;
}
} // namespace openswmm::quality
