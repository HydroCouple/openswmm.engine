// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "SubsurfaceSolver.hpp"
#include "LayerTransport.hpp"
#include "../data/SolverOptions2D.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace openswmm::twoD {
void SubsurfaceSolver::fireLayerSpecies(int cell,const CellFlux& f) {
    if(!f.column || int(f.column->new_water.size())!=tr_.n_layers)
        throw std::runtime_error("Layer chemistry has no accepted SIGMA water receipt.");
    const auto& water=*f.column;
    const int layers=tr_.n_layers,sat=layers;
    const double area=state_.area[cell];
    const bool euler=opts_&&opts_->reconstruction_order==2;
    const double dz=(euler?water.old_length:water.new_length)/layers;
    for(int s=0;s<tr_.n_species;++s) {
        if(heat_.active() && s==tr_.temp_row)continue; // thermal energy has one fixed-depth owner
        const auto k=tr_.idx(s,cell);
        const bool intensive=s==tr_.age_row||s==tr_.temp_row;
        const double partition=intensive?0:tr_.kd[k]*tr_.rho_b[cell];
        std::vector<double> capacity(layers+1),rhs(layers+1);
        for(int j=0;j<layers;++j) {
            capacity[j]=area*((euler?water.old_water[j]:water.new_water[j])+partition*dz);
            rhs[j]=tr_.layer_mass[tr_.layerIdx(s,j,cell)];
        }
        capacity[sat]=(euler?f.v_sat0/state_.theta_s[cell]:area*f.hg_final)*(state_.theta_s[cell]+partition);
        rhs[sat]=tr_.sat_mass[k];const auto initial=rhs;auto donor_available=initial;
        donor_available[sat]=std::max(0.,initial[sat]-pendingSaturatedMassExport(cell,s));
        const double lateral=gatherLateralMass(cell,s);
        rhs[sat]+=lateral;tr_.net_lateral[s]+=lateral;
        const double infil=consumePending(tr_.xacc_from_surface[k]);
        // Rejected infiltration retains its producer's composition. It has
        // never entered a soil reactor and cannot acquire resident solute.
        const double refund=f.refund_mass?(*f.refund_mass)[s]:
            (f.infil_in>0?infil*std::min(1.,f.reject/f.infil_in):0);
        rhs[0]+=infil-refund;tr_.gained_infil[s]+=infil;
        const double node=gatherNodeMass(cell,s),link=consumePending(tr_.lacc_mass[k]);
        rhs[sat]+=node+link;tr_.gained_node[s]+=node;tr_.gained_link[s]+=link;
        for(int j=0;j<layers;++j) {
            const auto q=tr_.layerIdx(s,j,cell);
            const double from_node=consumePending(tr_.contact_node_mass[q]);
            const double from_link=consumePending(tr_.contact_link_mass[q]);
            rhs[j]+=from_node+from_link;
            tr_.gained_node[s]+=from_node;tr_.gained_link[s]+=from_link;
        }
        const double source=f.source_mass&&std::size_t(s)<f.source_mass->size()?(*f.source_mass)[s]:0;
        rhs[sat]+=source;tr_.gained_source[s]+=source;
        if(f.boundaries)for(const auto& pulse:*f.boundaries) {
            rhs[sat]+=pulse.mass[s]+pulse.flux_in[s];
            tr_.net_lateral[s]+=pulse.mass[s]+pulse.flux_in[s];
            tr_.boundary_in[s]+=pulse.mass[s];tr_.boundary_flux_in[s]+=pulse.flux_in[s];
            auto& row=boundaries_[pulse.boundary];row.mass_in[s]+=pulse.mass[s];row.flux_in[s]+=pulse.flux_in[s];
        }
        const double prepaid_link=consumePending(tr_.lout_mass[k]);
        rhs[sat]-=prepaid_link;tr_.lost_link[s]+=prepaid_link;
        // Independent sinks may use current saturated mobile inventory,
        // never a later internal transfer or future forcing interval.
        double requested_flux=0,requested_source=0;
        if(f.boundaries)for(const auto& pulse:*f.boundaries)requested_flux+=pulse.flux_out[s];
        if(std::size_t(cell)<runtime_sources_.cells.size())for(auto id:runtime_sources_.cells[cell])
            requested_source+=std::max(0.,-runtime_sources_.rows[id].rates[s])*f.runtime_dt;
        const double mobile=tr_.dissolvedFraction(s,cell,state_.theta_s[cell]);
        const double available=euler&&!runtime_sources_.rows.empty()?std::max(0.,donor_available[sat]-prepaid_link-initial[sat]*(1-mobile)):rhs[sat]*mobile;
        const double requested=requested_flux+requested_source;
        const double scale=tr_.signedRow(s)||requested<=0?1:std::min(1.,std::max(0.,available)/requested);
        rhs[sat]-=requested*scale;
        if(euler)donor_available[sat]=std::max(0.,donor_available[sat]-prepaid_link-requested*scale);
        tr_.net_lateral[s]-=requested_flux*scale;tr_.boundary_flux_out[s]+=requested_flux*scale;
        tr_.lost_source[s]+=requested_source*scale;
        if(f.boundaries)for(const auto& pulse:*f.boundaries)
            boundaries_[pulse.boundary].flux_out[s]+=pulse.flux_out[s]*scale;
        if(std::size_t(cell)<runtime_sources_.cells.size())for(auto id:runtime_sources_.cells[cell]) {
            auto& row=runtime_sources_.rows[id];const double wanted=std::max(0.,-row.rates[s])*f.runtime_dt;
            row.receipt.requested_mass[s]-=wanted;row.receipt.applied_mass[s]-=wanted*scale;
        }
        std::vector<LayerTransportMove> moves;
        const auto face=[&](int j,double depth) {
            const double amount=area*depth;
            if(amount>0)moves.push_back({j-1,j,amount});
            else if(amount<0)moves.push_back({j,j-1,-amount});
        };
        for(int j=1;j<=layers;++j) {
            face(j,water.faces[j]);face(j,water.compression[j]);face(j,water.support[j]);
            // Moving control faces also transport grains and sorbed mass.
            face(j,partition*water.grid[j]);
        }
        if(tr_.dispersion_on && dz>0)for(int j=1;j<layers;++j) {
            const double a=euler?water.old_water[j-1]/dz:state_.theta_sigma[std::size_t(j-1)*state_.n_cells+cell];
            const double b=euler?water.old_water[j]/dz:state_.theta_sigma[std::size_t(j)*state_.n_cells+cell];
            const double theta=(a>0&&b>0)?2*a*b/(a+b):0;
            const double velocity=theta>0?std::abs(water.faces[j])/(f.dt*theta):0;
            const double coefficient=area*theta*(tr_.D_m[cell]+tr_.alpha_L[cell]*velocity)*f.dt/dz;
            if(coefficient>0){moves.push_back({j-1,j,coefficient});moves.push_back({j,j-1,coefficient});}
        }
        const auto sink=[&](int owner,double volume) {
            const auto id=moves.size();moves.push_back({owner,-1,std::max(0.,volume)});return id;
        };
        const auto deep=sink(sat,f.deep),node_out=sink(sat,f.node_out),source_out=sink(sat,f.source_out);
        const auto edge_out=sink(sat,f.boundary_out),dunne=sink(sat,f.dunne);
        const auto implicit_link=sink(sat,prepaid_link==0?f.link_out:0);
        const auto et=sink(0,intensive?f.et:0);
        if(s==tr_.age_row) {
            if(tr_.generated_age.empty()){tr_.generated_age.assign(tr_.n_species,0);tr_.gained_contact.assign(tr_.n_species,0);}
            const double production=f.dt*std::accumulate(capacity.begin(),capacity.end(),0.);
            for(int j=0;j<=layers;++j)rhs[j]+=f.dt*capacity[j];
            tr_.generated_age[s]+=production;
        }
        const auto result=euler?advanceLayerTransportEuler(capacity,initial,rhs,moves,tr_.signedRow(s),&donor_available):
            solveLayerTransport(capacity,rhs,moves,tr_.signedRow(s));
        if(!result.error.empty())throw std::runtime_error("SIGMA chemistry: "+result.error);
        tr_.sat_mass[k]=result.mass[sat];
        for(int j=0;j<layers;++j)tr_.layer_mass[tr_.layerIdx(s,j,cell)]=result.mass[j];
        tr_.lost_deep[s]+=result.transferred[deep];tr_.lost_node[s]+=result.transferred[node_out];
        tr_.lost_source[s]+=result.transferred[source_out];tr_.lost_link[s]+=result.transferred[implicit_link];
        tr_.lost_et[s]+=result.transferred[et];
        const double export_edge=result.transferred[edge_out];
        tr_.net_lateral[s]-=export_edge;tr_.boundary_out[s]+=export_edge;
        if(f.boundaries)for(const auto& pulse:*f.boundaries)if(f.boundary_out>0)
            boundaries_[pulse.boundary].mass_out[s]+=export_edge*pulse.out/f.boundary_out;
        const double up=refund+result.transferred[dunne];tr_.lost_dunne[s]+=up;
        if(f.refund_mass){(*f.dunne_mass)[s]=result.transferred[dunne];(*f.excess_mass)[s]=0;}
        else tr_.xacc_to_surface[k]+=up;
        if(f.node_out>0&&!tr_.node_out_mass.empty()) {
            double total=0;for(std::size_t b=0;b<node_beds_.size();++b)
                if(node_beds_[b].cell==cell)total+=std::max(0.,bed_gathered_[b]);
            const auto count=tr_.node_out_mass.size()/tr_.n_species;
            for(std::size_t b=0;b<node_beds_.size();++b)if(node_beds_[b].cell==cell&&total>0)
                tr_.node_out_mass[std::size_t(s)*count+node_beds_[b].node]+=
                    result.transferred[node_out]*std::max(0.,bed_gathered_[b])/total;
        }
        if(std::size_t(cell)<runtime_sources_.cells.size())for(auto id:runtime_sources_.cells[cell]) {
            auto& row=runtime_sources_.rows[id];const double requested_out=std::max(0.,-row.flow)*f.runtime_dt;
            row.receipt.requested_mass[s]-=requested_out*result.concentration[sat];
            if(f.requested_source_out>0)row.receipt.applied_mass[s]-=
                result.transferred[source_out]*requested_out/f.requested_source_out;
        }
        double internal=0;for(std::size_t j=0;j<moves.size();++j) {
            if(moves[j].to==sat)internal+=result.transferred[j];
            else if(moves[j].from==sat&&moves[j].to==sat-1)internal-=result.transferred[j];
        }
        tr_.internal_recharge[s]+=internal;
        if(!euler&&tr_.decay[k]>0&&!intensive) {
            const double keep=std::exp(-tr_.decay[k]*f.dt);
            double removed=tr_.sat_mass[k]*(1-keep);tr_.sat_mass[k]*=keep;
            for(int j=0;j<layers;++j) {
                auto& mass=tr_.layer_mass[tr_.layerIdx(s,j,cell)];removed+=mass*(1-keep);mass*=keep;
            }
            tr_.lost_reaction[s]+=removed;
        }
        tr_.refreshUnsat(s,cell);
    }
}
}
