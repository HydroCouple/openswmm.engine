// SPDX-License-Identifier: Apache-2.0
#include "SourceSnow.hpp"
#include "SnowCheckpoint.hpp"
#include "UebSubcatchments.hpp"
#include "../Radiation.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../../core/UnitConversion.hpp"
#include "../../core/DateTime.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace openswmm::surface {
namespace {
void check(const std::string& error){if(!error.empty())throw std::runtime_error(error);}
constexpr double cubic_ft=.3048*.3048*.3048;
}
double SourceSnow::storage(int group)const {
    double v=0;for(int sc:groups_.at(group))v+=nativeSnowVolume(solver_.state(),sc,area_[sc]);return v;
}
std::string SourceSnow::initialize(SimulationContext& c,const std::vector<std::vector<int>>& groups,const std::vector<double>& areas) {
    SourceSnow next;next.groups_=groups;next.area_=areas;next.ledger_.resize(groups.size());
    if(c.options.ignore_snow_melt){*this=std::move(next);return {};}
    std::vector<bool> owned(c.n_subcatches(),false);
    for(const auto& group:groups)for(int sc:group)owned[sc]=true;
    auto snow_context=c;
    for(int sc=0;sc<c.n_subcatches();++sc) {
        if(!owned[sc])snow_context.subcatches.snowpack[sc]=-1;
        if(owned[sc]&&c.subcatches.snowpack[sc]>=0)next.active_=true;
    }
    next.active_=!groups.empty(); // snow-enabled weather still applies gage catch factors without a pack
    if(!next.active_){*this=std::move(next);return {};}
    if(!c.options.ignore_quality && c.n_pollutants()>0)return "Completed snow sources require water-only transport.";
    if(auto error=initializeNativeSnow(snow_context,next.solver_);!error.empty())return error;
    c.snowpacks.runtime=std::move(snow_context.snowpacks.runtime);
    next.solver_.state().conservative_plowing=true;
    for(int g=0;g<int(groups.size());++g)next.ledger_[g].initial=next.ledger_[g].stored=next.storage(g);
    *this=std::move(next);return {};
}
void SourceSnow::advance(SimulationContext& c,int group,double start,double end,std::vector<runoff::RunoffSourceForcing>& forcing) {
    if(!active_)return;
    auto& book=ledger_.at(group);if(book.end!=start || end<=start)throw std::runtime_error("Noncontiguous completed snow source clock.");
    const double dt=end-start,date=c.options.start_date+start/86400.;const int n=c.n_subcatches();
    auto& state=solver_.state();std::fill(state.enabled.begin(),state.enabled.end(),0);
    std::vector<double> rain(n,0),snow(n,0),before_cover=state.asc;
    for(std::size_t i=0;i<before_cover.size();++i)if(state.wsnow[i]+state.fw[i]<=0)before_cover[i]=0;
    climate::RadiationState weather;check(climate::resolveRadiation(c,date,dt,weather));
    const auto& cfg=c.snowpacks.config;
    for(auto& w:forcing) {
        const int sc=w.subcatch,p=c.subcatches.snowpack[sc],gage=c.subcatches.gage[sc];
        state.enabled[sc]=1;
        const bool ueb=p>=0&&cfg.modelFor(p)==SnowKind::Ueb;
        const double fraction=ueb&&!cfg.division_temperature ? std::clamp((cfg.parameters.rain_temperature-weather.temperature)/
            (cfg.parameters.rain_temperature-cfg.parameters.snow_temperature),0.,1.):(weather.temperature<=(c.climate_state.snow_divt-32)/1.8?1.:0.);
        const auto site=cfg.packs.find(p);const double drift=ueb&&site!=cfg.packs.end()?site->second.site.drift:1;
        const double raw=w.raw_precipitation>=0?w.raw_precipitation:w.rain;
        const double catch_factor=gage>=0?c.gages.snow_factor[gage]:1;
        rain[sc]=raw*(1-fraction)*c.subcatches.rain_scale_factor[sc];
        snow[sc]=raw*fraction*catch_factor*drift*c.subcatches.snow_scale_factor[sc];
        rain[sc]=c.forcing.effective_rainfall(sc,rain[sc]*ucf::UCF(ucf::RAINFALL,c.options))/ucf::UCF(ucf::RAINFALL,c.options);
        snow[sc]=c.forcing.effective_snowfall(sc,snow[sc]);
        if(!std::isfinite(rain[sc])||rain[sc]<0||!std::isfinite(snow[sc])||snow[sc]<0)throw std::runtime_error("Invalid snow source precipitation.");
        // LID footprints retain their existing direct-precipitation behavior.
        // Only the native non-LID snowpack diverts water into snow storage.
        w.rain=rain[sc]+snow[sc];
        if(p>=0)book.precipitation+=w.rain*dt*area_[sc];
    }
    const double removed=state.removed,sublimation=c.snowpacks.runtime.sublimation;
    solver_.setMeltCoeffs(datetime::dayOfYear(date));solver_.plowSnow(c,dt,snow.data());
    const double temp=weather.temperature*1.8+32;
    const double ea=8.1175e6*std::exp(-7701.544/(temp+405.0265));
    solver_.execute(c,dt,temp,weather.wind/.44704,rain.data(),snow.data(),c.climate_state.gamma,ea);
    check(ueb::advanceSubcatchments(c,state,date,start,dt,rain.data(),snow.data()));
    book.removed+=state.removed-removed;book.sublimation+=(c.snowpacks.runtime.sublimation-sublimation)/cubic_ft;
    for(auto& w:forcing) {
        const int sc=w.subcatch;if(c.subcatches.snowpack[sc]<0)continue;
        double net[3]{},cover[3]{};
        for(int k=0;k<3;++k) {
            const int i=sc*3+k;
            net[k]=state.external[sc]?state.external_net[i]:state.imelt[i]+rain[sc]*(1-state.asc[i]);
            cover[k]=state.external[sc]?c.snowpacks.runtime.last[i].active_fraction:.5*(before_cover[i]+state.asc[i]);
            book.liquid+=net[k]*dt*state.fArea[i]*area_[sc];
        }
        const double fi=c.subcatches.frac_imperv[sc];
        c.subcatches.snow_net_imperv[sc]=fi>0?(net[0]*state.fArea[sc*3]+net[1]*state.fArea[sc*3+1])/fi:0;
        c.subcatches.snow_net_perv[sc]=net[2];c.subcatches.snow_depth[sc]=nativeSnowDepth(state,sc);
        w.pet_imperv=w.pet*(fi>0?std::clamp(1-(cover[0]*state.fArea[sc*3]+cover[1]*state.fArea[sc*3+1])/fi,0.,1.):0);
        w.pet_perv=w.pet*std::clamp(1-cover[2],0.,1.);
    }
    book.stored=storage(group);book.end=end;
    if(!std::isfinite(book.residual()) || std::abs(book.residual())>1e-10*std::max(1.,book.initial+book.precipitation))
        throw std::runtime_error("Completed source snow water balance failed.");
}
void SourceSnow::exportAccepted(const SimulationContext& source,SimulationContext& target,snow::SnowSoA& output)const {
    if(!active_)return;
    const auto& input=solver_.state();
    std::vector<double> snow::SnowSoA::* rows[]={
        &snow::SnowSoA::dhm,&snow::SnowSoA::wsnow,&snow::SnowSoA::fw,&snow::SnowSoA::coldc,
        &snow::SnowSoA::ati,&snow::SnowSoA::awe,&snow::SnowSoA::imelt,&snow::SnowSoA::age,
        &snow::SnowSoA::out_age,&snow::SnowSoA::sba,&snow::SnowSoA::sbws,&snow::SnowSoA::asc,
        &snow::SnowSoA::external_net,&snow::SnowSoA::ueb_transfer_energy};
    auto& out=target.snowpacks.runtime;const auto& in=source.snowpacks.runtime;
    for(const auto& group:groups_)for(int sc:group) {
        if(source.subcatches.snowpack[sc]<0)continue;
        for(int k=0;k<3;++k) {
            const int i=sc*3+k;
            for(auto row:rows)(output.*row)[i]=(input.*row)[i];
            if(input.external[sc]) {out.columns.at(i)=in.columns.at(i);out.last.at(i)=in.last.at(i);out.sample_end.at(i)=in.sample_end.at(i);out.sample_seconds.at(i)=in.sample_seconds.at(i);}
        }
    }
}
void SourceSnow::checkpoint(detail::CompletedSourceArchive& a,bool model,SimulationContext& c) {
    if(!active_)return; // snow-free archives retain their layout
    if(model) {
        auto identity=snowModelIdentity(c,solver_.state());a(identity,groups_,area_,c.gages.snow_factor);return;
    }
    auto bytes=a.reading()?std::string{}:saveSnowCheckpoint(c,solver_.state());a(bytes);
    if(a.reading())check(restoreSnowCheckpoint(c,solver_.state(),bytes));
    a.sequence(ledger_,[&](auto& b){a(b.end,b.initial,b.precipitation,b.liquid,b.sublimation,b.removed,b.stored);},true);
    for(int g=0;g<int(ledger_.size());++g) {
        const auto& b=ledger_[g];
        for(double v:{b.end,b.initial,b.precipitation,b.liquid,b.removed,b.stored})if(!std::isfinite(v)||v<0)throw std::runtime_error("Invalid snow source checkpoint ledger.");
        if(!std::isfinite(b.sublimation)||std::abs(b.residual())>1e-10*std::max(1.,b.initial+b.precipitation)||
            std::abs(b.stored-storage(g))>1e-10*std::max(1.,b.stored))throw std::runtime_error("Snow source checkpoint inventory differs.");
    }
}
}
