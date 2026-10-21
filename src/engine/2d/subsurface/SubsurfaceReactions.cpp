// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "SubsurfaceSolver.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../transport/components/ReactionModule/ReactionsWriter.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <functional>

namespace openswmm::twoD {
void SubsurfaceSolver::publishNodeConcentrations(const SimulationContext& ctx) {
    const auto count=std::size_t(ctx.n_nodes());
    node_concentrations_.assign(count*tr_.n_species,0);
    for(std::size_t node=0;node<count;++node)for(int row=0;row<tr_.n_species;++row) {
        double value=0;
        if(row<tr_.n_pollut) {
            const auto k=node*ctx.n_pollutants()+row;
            if(k<ctx.nodes.conc.size())value=ctx.nodes.conc[k];
        } else if(row<tr_.n_pollut+tr_.n_msx) {
            const int msx=ctx.reactions.find_species(tr_.row_names[row]);
            const auto k=node*ctx.reactions.n_species()+msx;
            if(msx>=0&&k<ctx.reactions.msx_node_conc.size())value=ctx.reactions.msx_node_conc[k];
        } else if(row==tr_.age_row&&node<ctx.water_age_state.node_age.size())value=ctx.water_age_state.node_age[node];
        else if(row==tr_.temp_row&&node<ctx.heat_state.node_temp.size())value=ctx.heat_state.node_temp[node];
        node_concentrations_[node*tr_.n_species+row]=value;
    }
    node_row_conc_=node_concentrations_.data();node_row_ns_=tr_.n_species;
}
std::string SubsurfaceSolver::initReactions(SimulationContext& ctx) {
    reactions_=nullptr;reactors_.clear();reaction_identity_.clear();
    const auto& rx=ctx.reactions;if(rx.subsurface_laws.empty())return {};
    if(!rx.compiled)return "[REACTION_SUBSURFACE] reaction system is not compiled.";
    if(!tr_.active()||tr_.n_msx<=0)return "[REACTION_SUBSURFACE] requires enabled groundwater MSX transport.";
    const int stores=tr_.n_layers>0?tr_.n_layers+1:2;
    reactors_.resize(std::size_t(state_.n_cells)*stores);
    for(auto& reactor:reactors_) {
        reactor.forms.assign(rx.n_species(),ReactionExprForm::NONE);
        reactor.expressions.resize(rx.n_species());
    }
    std::function<bool(const RxExprSpan&)> unavailable_pollutant;
    unavailable_pollutant=[&](const RxExprSpan& expression) {
        for(int i=expression.begin;i<expression.begin+expression.len;++i) {
            const auto& token=rx.token_pool[i];
            if(token.op==RxToken::PUSH_POLLUT&&token.idx>=tr_.n_pollut)return true;
            if(token.op==RxToken::PUSH_TERM&&unavailable_pollutant(rx.term_expr[token.idx]))return true;
        }
        return false;
    };
    for(const auto& law:rx.subsurface_laws) {
        if(unavailable_pollutant(law.compiled))return "[REACTION_SUBSURFACE] expression depends on disabled groundwater pollutant transport.";
        bool matched=false;
        if(law.zone==2 && (tr_.n_layers==0||law.layer<0||law.layer>=tr_.n_layers))
            return "[REACTION_SUBSURFACE] LAYER requires LAYERED SIGMA transport and a valid layer.";
        for(int cell=0;cell<state_.n_cells;++cell)if(covers(cell)&&
            scopeCoversCell(cell,static_cast<GwScope>(law.scope),law.tag,law.cell)) {
            matched=true;
            const int row=tr_.rowIndex(rx.species_name[law.species]);
            if(row<0)return "[REACTION_SUBSURFACE] species is disabled in groundwater.";
            if(tr_.decay[tr_.idx(row,cell)]>0)return "[REACTION_SUBSURFACE] cannot also apply first-order GW_SORPTION decay to the same species.";
        }
        if(!matched)return "[REACTION_SUBSURFACE] scope does not cover an active aquifer cell.";
    }
    for(int scope=0;scope<3;++scope)for(int layer=0;layer<2;++layer)for(const auto& law:rx.subsurface_laws) {
        if(law.scope!=scope||(law.zone==2)!=bool(layer))continue;
        for(int cell=0;cell<state_.n_cells;++cell)if(covers(cell)&&
            scopeCoversCell(cell,static_cast<GwScope>(law.scope),law.tag,law.cell))
            for(int zone=0;zone<stores;++zone) {
                if((law.zone==0&&zone!=0)||(law.zone==1&&zone==0)||(law.zone==2&&zone!=law.layer+1))continue;
                auto& reactor=reactors_[std::size_t(cell)*stores+zone];
                reactor.forms[law.species]=law.form;reactor.expressions[law.species]=law.compiled;
            }
    }
    reactions_=&rx;reaction_workspace_.init(rx);reaction_identity_=transport::serializeReactionSystem(ctx);
    return {};
}
void SubsurfaceSolver::reactCell(int cell,double dt,double hg) {
    if(!reactions_||reactors_.empty())return;
    if(heat_.active()&&tr_.n_layers==0){reactPorewaterCell(cell,dt,hg);return;}
    const auto& rx=*reactions_;const int count=rx.n_species();
    std::vector<int> rows(count);for(int s=0;s<count;++s)rows[s]=tr_.rowIndex(rx.species_name[s]);
    const int stores=tr_.n_layers>0?tr_.n_layers+1:2;
    // Stage all zones together so a later reactor failure cannot publish
    // half of this cell's reaction step.
    std::vector<double> staged(std::size_t(stores)*count),ledger(count,0);
    const double area=state_.area[cell],length=std::max(0.,state_.zs[cell]-hg);
    const int temp=tr_.temp_row;
    for(int zone=0;zone<stores;++zone) {
        const auto& reactor=reactors_[std::size_t(cell)*stores+zone];
        const double bulk=area*(zone==0?hg:tr_.n_layers>0?length/tr_.n_layers:length);
        const double theta=zone==0?state_.theta_s[cell]:tr_.n_layers>0?
            state_.theta_sigma[std::size_t(zone-1)*state_.n_cells+cell]:bulk>0?unsatVolume(cell)/bulk:0;
        const double volume=theta*bulk;
        const auto mass=[&](int row) {
            if(zone==0)return tr_.sat_mass[tr_.idx(row,cell)];
            if(tr_.n_layers>0)return tr_.layer_mass[tr_.layerIdx(row,zone-1,cell)];
            return tr_.unsat_mass[tr_.idx(row,cell)];
        };
        std::vector<double> aqueous(count),capacities(count),scale(count),pollutants(tr_.n_pollut);
        for(int s=0;s<count;++s) {
            const int row=rows[s];if(row<0)continue;const double original=mass(row);
            staged[std::size_t(zone)*count+s]=original;
            capacities[s]=volume+bulk*tr_.kd[tr_.idx(row,cell)]*tr_.rho_b[cell];
            aqueous[s]=capacities[s]>0?original/capacities[s]:0;
            scale[s]=capacities[s]>0?volume/capacities[s]:0;
        }
        if(!(volume>0))continue;
        if(!std::isfinite(theta)||theta<state_.theta_r[cell]-1e-12||theta>state_.theta_s[cell]+1e-12)
            throw std::runtime_error("[REACTION_SUBSURFACE] invalid aqueous moisture state.");
        for(int s=0;s<tr_.n_pollut;++s) {
            const double capacity=volume+bulk*tr_.kd[tr_.idx(s,cell)]*tr_.rho_b[cell];
            pollutants[s]=capacity>0?mass(s)/capacity:0;
        }
        double variables[int(RxHydVar::COUNT_)]={};
        variables[int(RxHydVar::TEMP)]=temp>=0?mass(temp)/volume:rx.default_temp_c;
        variables[int(RxHydVar::GW_THETA)]=theta;
        variables[int(RxHydVar::GW_POROSITY)]=state_.theta_s[cell];
        variables[int(RxHydVar::GW_SE)]=std::clamp((theta-state_.theta_r[cell])/
            (state_.theta_s[cell]-state_.theta_r[cell]),0.,1.);
        variables[int(RxHydVar::GW_DEPTH_M)]=zone==0?length+.5*hg:
            tr_.n_layers>0?(zone-.5)*length/tr_.n_layers:.5*length;
        const auto report=opts_&&opts_->reconstruction_order==2?
            transport::ReactionIntegrator::stepSubcycled(rx,reactor.forms,reactor.expressions,dt,
                aqueous.data(),variables,reaction_workspace_,pollutants.data(),scale.data()):
            transport::ReactionIntegrator::step(rx,reactor.forms,reactor.expressions,dt,
                aqueous.data(),variables,reaction_workspace_,pollutants.data(),scale.data());
        if(!report.ok)throw std::runtime_error("[REACTION_SUBSURFACE] "+report.error);
        for(int s=0;s<count;++s) {
            if(rows[s]<0)continue;
            const double changed=aqueous[s]*capacities[s];
            if(!std::isfinite(changed)||changed<0)throw std::runtime_error("[REACTION_SUBSURFACE] inadmissible reactor inventory.");
            auto& saved=staged[std::size_t(zone)*count+s];ledger[s]+=saved-changed;saved=changed;
        }
    }
    for(int s=0;s<count;++s) {
        const int row=rows[s];if(row<0)continue;tr_.lost_reaction[row]+=ledger[s];
        tr_.sat_mass[tr_.idx(row,cell)]=staged[s];
        if(tr_.n_layers>0) {
            for(int j=0;j<tr_.n_layers;++j)tr_.layer_mass[tr_.layerIdx(row,j,cell)]=staged[std::size_t(j+1)*count+s];
            tr_.refreshUnsat(row,cell);
        } else tr_.unsat_mass[tr_.idx(row,cell)]=staged[count+s];
    }
}

void SubsurfaceSolver::reactPorewaterCell(int cell,double dt,double hg) {
    auto& thermal=heat_.cells[cell];auto& pore=thermal.porewater;
    if(thermal.failed)throw std::runtime_error(thermal.error);
    if(!pore.active())throw std::runtime_error("[REACTION_SUBSURFACE] porous reactor has no canonical inventory.");
    const auto& column=thermal.column;const auto nl=column.theta.size();
    const auto& rx=*reactions_;const int count=rx.n_species();
    const double table=state_.zs[cell]-hg,area=column.area;
    bool shared_rates=opts_&&opts_->reconstruction_order==2;
    if(shared_rates)for(int zone=0;zone<2;++zone)for(auto form:reactors_[std::size_t(cell)*2+zone].forms)
        if(form!=ReactionExprForm::NONE&&form!=ReactionExprForm::RATE)shared_rates=false;
    std::vector<int> rows(count);for(int s=0;s<count;++s)rows[s]=tr_.rowIndex(rx.species_name[s]);
    auto staged=pore.mass;std::vector<double> ledger(count,0);
    for(std::size_t j=0;j<nl;++j) {
        const double top=column.depth_edges[j],bottom=column.depth_edges[j+1];
        const double bulk=area*(bottom-top),water=column.theta[j]*bulk;
        const double sat=thermal.saturated_water[j];
        const double sat_bulk=area*std::max(0.,bottom-std::max(top,table));
        const double sat_fraction=sat_bulk/bulk;
        const double reserved_water=thermal.reserved_out_water[j];
        if(!std::isfinite(water)||!std::isfinite(sat)||!std::isfinite(reserved_water)||
           sat<0||sat>water+1e-12*std::max(1.,water)||reserved_water<0||reserved_water>sat+1e-12*std::max(1.,sat))
            throw std::runtime_error("[REACTION_SUBSURFACE] invalid porous aqueous water or reservation.");
        const double active_sat=std::max(0.,sat-reserved_water);
        std::vector<double> active_capacity(count),original(count);
        for(int s=0;s<count;++s)if(rows[s]>=0) {
            const auto k=std::size_t(rows[s])*nl+j;
            original[s]=pore.contactAvailableMass(rows[s],j);
            active_capacity[s]=std::max(0.,water-reserved_water)+pore.retention[k];
            if(!std::isfinite(original[s])||original[s]<0)
                throw std::runtime_error("[REACTION_SUBSURFACE] invalid available porous inventory.");
            staged[k]=pore.reserved_out[k]; // accepted producer packets never react a second time
        }
        if(shared_rates) {
            std::vector<double> aqueous(count),pollutants(tr_.n_pollut);
            std::vector<ReactionExprForm> forms(count,ReactionExprForm::NONE);
            std::vector<RxExprSpan> expressions(count);
            std::array<std::array<double,int(RxHydVar::COUNT_)>,2> variables{};
            std::array<std::vector<double>,2> scales;
            std::vector<transport::RxRateContext> contexts;contexts.reserve(2);
            for(int s=0;s<count;++s)if(rows[s]>=0&&active_capacity[s]>0)aqueous[s]=original[s]/active_capacity[s];
            for(int r=0;r<tr_.n_pollut;++r) {
                const auto k=std::size_t(r)*nl+j;
                const double cap=std::max(0.,water-reserved_water)+pore.retention[k];
                pollutants[r]=cap>0?pore.contactAvailableMass(r,j)/cap:0;
            }
            for(int zone=0;zone<2;++zone) {
                const double phase_bulk=zone==0?sat_bulk:bulk-sat_bulk;
                const double phase_water=zone==0?active_sat:std::max(0.,water-sat);
                const double full_water=zone==0?sat:water-sat;
                const double theta=phase_bulk>0?full_water/phase_bulk:0;
                if(phase_bulk>0&&(!std::isfinite(theta)||theta<state_.theta_r[cell]-1e-12||theta>state_.theta_s[cell]+1e-12))
                    throw std::runtime_error("[REACTION_SUBSURFACE] invalid porous phase moisture.");
                if(!(phase_water>0))continue;
                auto& vars=variables[zone];scales[zone].resize(count);
                vars[int(RxHydVar::TEMP)]=heat_.activeTemperature(cell,j);
                vars[int(RxHydVar::GW_THETA)]=theta;vars[int(RxHydVar::GW_POROSITY)]=state_.theta_s[cell];
                vars[int(RxHydVar::GW_SE)]=std::clamp((theta-state_.theta_r[cell])/(state_.theta_s[cell]-state_.theta_r[cell]),0.,1.);
                vars[int(RxHydVar::GW_DEPTH_M)]=zone==0?.5*(std::max(top,table)+bottom):.5*(top+std::min(bottom,table));
                const auto& reactor=reactors_[std::size_t(cell)*2+zone];
                for(int s=0;s<count;++s) {
                    scales[zone][s]=active_capacity[s]>0?phase_water/active_capacity[s]:0;
                    if(reactor.forms[s]==ReactionExprForm::RATE){forms[s]=ReactionExprForm::RATE;expressions[s]=reactor.expressions[s];}
                }
                contexts.push_back({&reactor.forms,&reactor.expressions,vars.data(),pollutants.data(),scales[zone].data()});
            }
            if(!contexts.empty()) {
                const auto report=transport::ReactionIntegrator::stepSubcycled(rx,forms,expressions,dt,
                    aqueous.data(),variables[0].data(),reaction_workspace_,pollutants.data(),nullptr,nullptr,contexts);
                if(!report.ok)throw std::runtime_error("[REACTION_SUBSURFACE] "+report.error);
            }
            for(int s=0;s<count;++s)if(rows[s]>=0) {
                const double changed=forms[s]==ReactionExprForm::RATE?aqueous[s]*active_capacity[s]:original[s];
                if(!std::isfinite(changed)||changed<0)throw std::runtime_error("[REACTION_SUBSURFACE] inadmissible porous reactor inventory.");
                staged[std::size_t(rows[s])*nl+j]+=changed;ledger[s]+=original[s]-changed;
            }
            continue;
        }
        for(int zone=0;zone<2;++zone) {
            const double phase_bulk=zone==0?sat_bulk:bulk-sat_bulk;
            const double phase_water=zone==0?active_sat:std::max(0.,water-sat);
            const double full_water=zone==0?sat:water-sat;
            const double theta=phase_bulk>0?full_water/phase_bulk:0;
            if(phase_bulk>0 && (!std::isfinite(theta)||theta<state_.theta_r[cell]-1e-12||theta>state_.theta_s[cell]+1e-12))
                throw std::runtime_error("[REACTION_SUBSURFACE] invalid porous phase moisture.");
            std::vector<double> capacity(count),mass(count),aqueous(count),scale(count),pollutants(tr_.n_pollut);
            const double fraction=zone==0?sat_fraction:1-sat_fraction;
            for(int s=0;s<count;++s)if(rows[s]>=0) {
                const auto k=std::size_t(rows[s])*nl+j;
                capacity[s]=phase_water+pore.retention[k]*fraction;
                mass[s]=active_capacity[s]>0?original[s]*capacity[s]/active_capacity[s]:original[s]*fraction;
                aqueous[s]=capacity[s]>0?mass[s]/capacity[s]:0;
                scale[s]=capacity[s]>0?phase_water/capacity[s]:0;
            }
            if(phase_water>0) {
                for(int r=0;r<tr_.n_pollut;++r) {
                    const auto k=std::size_t(r)*nl+j;
                    const double cap=std::max(0.,water-reserved_water)+pore.retention[k];
                    pollutants[r]=cap>0?pore.contactAvailableMass(r,j)/cap:0;
                }
                double variables[int(RxHydVar::COUNT_)]={};
                variables[int(RxHydVar::TEMP)]=heat_.activeTemperature(cell,j);
                variables[int(RxHydVar::GW_THETA)]=theta;
                variables[int(RxHydVar::GW_POROSITY)]=state_.theta_s[cell];
                variables[int(RxHydVar::GW_SE)]=std::clamp((theta-state_.theta_r[cell])/
                    (state_.theta_s[cell]-state_.theta_r[cell]),0.,1.);
                variables[int(RxHydVar::GW_DEPTH_M)]=zone==0?.5*(std::max(top,table)+bottom):.5*(top+std::min(bottom,table));
                const auto& reactor=reactors_[std::size_t(cell)*2+zone];
                const auto report=opts_&&opts_->reconstruction_order==2?
                    transport::ReactionIntegrator::stepSubcycled(rx,reactor.forms,reactor.expressions,dt,
                        aqueous.data(),variables,reaction_workspace_,pollutants.data(),scale.data()):
                    transport::ReactionIntegrator::step(rx,reactor.forms,reactor.expressions,dt,
                        aqueous.data(),variables,reaction_workspace_,pollutants.data(),scale.data());
                if(!report.ok)throw std::runtime_error("[REACTION_SUBSURFACE] "+report.error);
            }
            for(int s=0;s<count;++s)if(rows[s]>=0) {
                const double changed=phase_water>0?aqueous[s]*capacity[s]:mass[s];
                if(!std::isfinite(changed)||changed<0)
                    throw std::runtime_error("[REACTION_SUBSURFACE] inadmissible porous reactor inventory.");
                staged[std::size_t(rows[s])*nl+j]+=changed;ledger[s]+=mass[s]-changed;
            }
        }
    }
    // Publish one canonical fixed-depth inventory only after every reactor
    // succeeds; SAT/UNSAT outputs are then regenerated from that same owner.
    pore.mass=std::move(staged);
    for(int s=0;s<count;++s)if(rows[s]>=0)tr_.lost_reaction[rows[s]]+=ledger[s];
    heat_.refreshView(cell,tr_);
}

bool SubsurfaceSolver::hasRk2Reactions() const noexcept {
    return !reactors_.empty()||std::any_of(tr_.decay.begin(),tr_.decay.end(),[](double rate){return rate>0;});
}
void SubsurfaceSolver::advanceRk2Reactions(double seconds) {
    if(!hasRk2Reactions()||!(seconds>0))return;
    if(!std::isfinite(seconds))throw std::runtime_error("Nonfinite coupled reaction interval.");
    for(int cell=0;cell<state_.n_cells;++cell)if(covers(cell)) {
        const auto decay=[&](double interval) {
            for(int species=0;species<tr_.n_species;++species) {
                if(species==tr_.age_row||species==tr_.temp_row)continue;
                const auto k=tr_.idx(species,cell);const double rate=tr_.decay[k];if(!(rate>0))continue;
                const double keep=std::exp(-rate*interval);double removed=0;
                const auto apply=[&](double& mass) {removed+=mass*(1-keep);mass*=keep;};
                if(heat_.active()&&!tr_.n_layers) {
                    auto& pore=heat_.cells[cell].porewater;const auto n=heat_.cells[cell].column.theta.size();
                    for(std::size_t j=0;j<n;++j) {
                        const auto index=std::size_t(species)*n+j;const double held=pore.reserved_out[index];
                        double active=pore.mass[index]-held;
                        if(!std::isfinite(active)||active<0)throw std::runtime_error("Invalid porous reaction donor reservation.");
                        apply(active);pore.mass[index]=held+active;
                    }
                } else {
                    apply(tr_.sat_mass[k]);
                    if(tr_.n_layers) {
                        for(int layer=0;layer<tr_.n_layers;++layer)apply(tr_.layer_mass[tr_.layerIdx(species,layer,cell)]);
                        tr_.refreshUnsat(species,cell);
                    } else apply(tr_.unsat_mass[k]);
                }
                tr_.lost_reaction[species]+=removed;
            }
        };
        // A decaying precursor can drive another species' RATE law. Keep
        // this composition symmetric too; duplicate decay ownership was
        // already refused when the groundwater reaction context was bound.
        decay(reactors_.empty()?seconds:.5*seconds);
        reactCell(cell,seconds,state_.hg[cell]);
        if(!reactors_.empty())decay(.5*seconds);
        if(heat_.active()) {
            if(tr_.n_layers)if(auto error=refreshSigmaPorewater(cell);!error.empty())throw std::runtime_error(error);
            heat_.refreshView(cell,tr_);
        }
    }
}

}
