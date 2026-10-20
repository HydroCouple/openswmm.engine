// SPDX-License-Identifier: Apache-2.0
#include "SubsurfaceSolver.hpp"
#include "../heat/PorewaterTransport.hpp"
#include "../gw/GwTransportData.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
namespace openswmm::twoD {
std::string SubsurfaceSolver::initPorewater() {
    if(!heat_.active()||tr_.n_species-(tr_.temp_row>=0?1:0)<=0)return {};
    try {
        const auto nl=std::size_t(heat_.config->options.thermal_layers),nc=std::size_t(state_.n_cells),ns=std::size_t(tr_.n_species);
        if(!nc||ns>std::numeric_limits<std::size_t>::max()/nc/nl)return "Ground porewater layer count exceeds the addressable state size.";
        std::vector<double> sat(ns),unsat(ns),kd(ns);
        for(int i=0;i<state_.n_cells;++i) {
            auto& cell=heat_.cells[i];if(cell.column.energy.empty())continue;
            for(int r=0;r<tr_.n_species;++r) {
                const auto k=tr_.idx(r,i);sat[r]=tr_.sat_mass[k];unsat[r]=tr_.unsat_mass[k];kd[r]=tr_.kd[k];
            }
            if(tr_.n_layers>0) {
                auto& pore=cell.porewater;pore.n_species=tr_.n_species;pore.age_row=tr_.age_row;pore.temp_row=tr_.temp_row;
                pore.mass.assign(ns*nl,0);pore.retention=pore.mass;pore.reserved_out=pore.mass;pore.contact_mass=pore.mass;
                pore.generated_age.assign(nl,0);
                if(auto error=refreshSigmaPorewater(i);!error.empty())return error;
                pore.initial_mass=pore.mass;
            } else if(auto error=cell.porewater.initialize(cell.column,cell.saturated_water,sat,unsat,kd,tr_.age_row,tr_.temp_row);!error.empty())return error;
        }
        porewater_preset_out_.assign(ns*nc*nl,0);tr_.generated_age.assign(ns,0);tr_.gained_contact.assign(ns,0);
        for(int i=0;i<state_.n_cells;++i)if(heat_.cells[i].porewater.active())heat_.refreshView(i,tr_);
        if(tr_.n_layers>0 && tr_.temp_row>=0)tr_.init_mass[tr_.temp_row]=tr_.storage(tr_.temp_row);
        return {};
    } catch(const std::exception& error){return error.what();}
}
std::string SubsurfaceSolver::refreshSigmaPorewater(int i) {
    if(tr_.n_layers<=0||!heat_.active())return {};
    auto& cell=heat_.cells[i];auto& pore=cell.porewater;
    if(cell.column.energy.empty())return {};
    if(auto error=pore.publishSigma(tr_,i);!error.empty())return error;
    const auto nl=cell.column.theta.size();const int m=tr_.n_layers,ns=tr_.n_species;
    const double area=state_.area[i],length=state_.zs[i]-state_.hg[i],dz=length/m;
    std::vector<SigmaPorewaterMap::Overlap> overlaps;
    for(std::size_t j=0;j<nl;++j)for(int layer=0;layer<=m;++layer) {
        const double top=layer==m?length:dz*layer,bottom=layer==m?state_.zs[i]:dz*(layer+1);
        const double overlap=std::max(0.,std::min(bottom,cell.column.depth_edges[j+1])-std::max(top,cell.column.depth_edges[j]));
        if(!(overlap>0))continue;
        const double theta=layer==m?state_.theta_s[i]:state_.theta_sigma[std::size_t(layer)*state_.n_cells+i];
        overlaps.push_back({int(j),layer,area*overlap*theta,area*overlap});
    }
    std::vector<double> partition(ns),canonical(std::size_t(ns)*(m+1)),held(ns);
    for(int s=0;s<ns;++s)if(s!=tr_.temp_row) {
        partition[s]=s==tr_.age_row?0:tr_.kd[tr_.idx(s,i)]*tr_.rho_b[i];
        for(int j=0;j<m;++j)canonical[std::size_t(s)*(m+1)+j]=tr_.layer_mass[tr_.layerIdx(s,j,i)];
        canonical[std::size_t(s)*(m+1)+m]=tr_.sat_mass[tr_.idx(s,i)];held[s]=pendingSaturatedMassExport(i,s);
    }
    if(auto error=pore.sigma.configure(m,ns,std::move(overlaps),partition,canonical,held);!error.empty())return error;
    pore.refreshSigmaView();
    // The fixed grid's grain capacity is geometric model identity. Derive it
    // directly, so changing overlap partitions cannot change its last bit.
    for(int s=0;s<ns;++s)for(std::size_t j=0;j<nl;++j)
        pore.retention[std::size_t(s)*nl+j]=partition[s]*area*(cell.column.depth_edges[j+1]-cell.column.depth_edges[j]);
    return {};
}
void SubsurfaceSolver::refreshSigmaPorewaterReservations(int cell) noexcept {
    try {
        if(auto error=refreshSigmaPorewater(cell);!error.empty()) {
            heat_.cells[cell].failed=1;heat_.cells[cell].error=error;
        }
    }catch(...) {heat_.cells[cell].failed=1;}
}
std::string SubsurfaceSolver::refreshPorewaterContactLedger() {
    if(!heat_.active()||tr_.gained_contact.empty())return {};
    if(tr_.n_layers>0)for(int i=0;i<state_.n_cells;++i)if(covers(i))heat_.refreshView(i,tr_);
    std::fill(tr_.gained_contact.begin(),tr_.gained_contact.end(),0.0);
    for(int i=0;i<state_.n_cells;++i) {
        const auto& cell=heat_.cells[std::size_t(i)];const auto& pore=cell.porewater;
        if(!pore.active())continue;
        const auto nl=cell.column.theta.size();
        if(pore.contact_mass.size()!=std::size_t(tr_.n_species)*nl)
            return "Ground porewater contact receipt layout differs from its species/layer state.";
        for(int r=0;r<tr_.n_species;++r)if(r!=tr_.temp_row)for(std::size_t j=0;j<nl;++j) {
            const double increment=pore.contact_mass[std::size_t(r)*nl+j];
            const double next=tr_.gained_contact[std::size_t(r)]+increment;
            if(!std::isfinite(increment)||!std::isfinite(next))
                return "Ground porewater contact receipt is nonfinite.";
            tr_.gained_contact[std::size_t(r)]=next;
        }
    }
    return {};
}
void SubsurfaceSolver::bookPresetPorewaterOut(int i,int r,double target) noexcept {
    if(!heat_.active()||tr_.n_layers>0||r==tr_.temp_row||!(target>0))return;
    auto& cell=heat_.cells[i];auto& pore=cell.porewater;if(!pore.active())return;
    const auto& column=cell.column;const auto nl=column.theta.size();const auto row=std::size_t(r)*nl;
    const auto base=(std::size_t(r)*state_.n_cells+i)*nl;
    const auto mobile=[&](std::size_t j) {
        const double water=column.theta[j]*column.area*(column.depth_edges[j+1]-column.depth_edges[j]);
        const double capacity=water+pore.retention[row+j];
        const double active=pore.contactAvailableMass(r,j);
        return capacity>0?active/capacity*cell.saturated_water[j]:0;
    };
    double total=0,largest=0;std::size_t anchor=0;
    for(std::size_t j=0;j<nl;++j){const double part=mobile(j);total+=part;if(part>largest){largest=part;anchor=j;}}
    const auto fail=[&](){cell.failed=1;try{cell.error="Accepted porewater export has no finite physical layer donor.";}catch(...) {}};
    if(!(total>0)||!std::isfinite(total)||!std::isfinite(target)||target>total){fail();return;}
    double sum=0;
    for(std::size_t j=0;j<nl;++j)sum+=target*(mobile(j)/total);
    const double correction=target-sum;
    for(std::size_t j=0;j<nl;++j) {
        const double receipt=target*(mobile(j)/total);
        const double adjusted=receipt+(j==anchor?correction:0);
        if(!std::isfinite(porewater_preset_out_[base+j]+adjusted)||!pore.canReserveOutgoing(r,j,adjusted)){fail();return;}
    }
    for(std::size_t j=0;j<nl;++j) {
        const double receipt=target*(mobile(j)/total);
        const double adjusted=receipt+(j==anchor?correction:0);
        if(!pore.reserveOutgoing(r,j,adjusted)){fail();return;}
        porewater_preset_out_[base+j]+=adjusted;
    }
}
double SubsurfaceSolver::saturatedPorewaterMobileMass(int i,int r) const noexcept {
    if(!heat_.active()||i<0||std::size_t(i)>=heat_.cells.size())return std::numeric_limits<double>::quiet_NaN();
    const auto& cell=heat_.cells[i];const auto& pore=cell.porewater;const auto& column=cell.column;
    if(!pore.active()||r<0||r>=pore.n_species||r==pore.temp_row||
       cell.saturated_water.size()!=column.theta.size())return std::numeric_limits<double>::quiet_NaN();
    const auto nl=column.theta.size(),row=std::size_t(r)*nl;double total=0;
    for(std::size_t j=0;j<nl;++j) {
        const double bulk=column.area*(column.depth_edges[j+1]-column.depth_edges[j]);
        const double capacity=column.theta[j]*bulk+pore.retention[row+j];
        const double active=pore.contactAvailableMass(r,j);
        if(!std::isfinite(capacity)||capacity<0||!std::isfinite(active))return std::numeric_limits<double>::quiet_NaN();
        if(capacity>0)total+=active/capacity*cell.saturated_water[j];
        if(!std::isfinite(total))return std::numeric_limits<double>::quiet_NaN();
    }
    return total;
}
void SubsurfaceSolver::fireGroundPorewaterSpecies(int i,const CellFlux& f) noexcept {
    auto& cell=heat_.cells[i];if(cell.failed||!cell.porewater.active())return;
    try {
        const auto& column=cell.column;const auto nl=column.theta.size();auto next=cell.porewater;
        std::vector<double> moisture,new_sat;
        if(auto error=projectGroundMoisture(state_,i,f.hg_final,column.depth_edges,moisture,&new_sat);!error.empty())throw std::runtime_error(error);
        auto new_column=column;if(auto error=new_column.setMoisture(moisture);!error.empty())throw std::runtime_error(error);
        std::vector<double> old_water(nl),new_water(nl),preset_water(nl),water(nl),weights=cell.saturated_water;
        double total=std::accumulate(weights.begin(),weights.end(),0.);
        if(!(total>0)){weights=new_sat;total=std::accumulate(weights.begin(),weights.end(),0.);}
        if(!(total>0)){std::fill(weights.begin(),weights.end(),0);weights.back()=1;total=1;}
        const auto water_base=std::size_t(i)*heat_.config->options.thermal_layers;
        double preset_volume=0;
        for(std::size_t j=0;j<nl;++j) {
            const double bulk=column.area*(column.depth_edges[j+1]-column.depth_edges[j]);
            old_water[j]=column.theta[j]*bulk;new_water[j]=moisture[j]*bulk;weights[j]/=total;
            // Heat owns the one water decomposition. Read its next accepted
            // slice without consuming it before the heat owner uses it.
            double pending=heat_preset_out_water_[water_base+j];preset_water[j]=consumePending(pending);preset_volume+=preset_water[j];
        }
        const double body=f.lateral-f.node_out+f.link+f.source_in-f.source_out+f.boundary_in-f.boundary_out;
        for(std::size_t j=0;j<nl;++j)water[j]=(body+preset_volume)*weights[j]-preset_water[j];
        water.front()+=f.infil_in-f.reject-f.et-f.dunne;water.back()-=f.deep;
        double node_volume=0;
        for(std::size_t b=0;b<node_beds_.size();++b)if(node_beds_[b].cell==i)node_volume+=heat_node_gathered_out_[b];
        node_volume=std::max(node_volume,std::max(0.,f.node_out));
        const bool link_packet=heat_link_out_volume_[i]>0;
        const double implicit_link_volume=link_packet?0:std::max(0.,-f.link),excess_volume=f.reject-f.paired_refund;
        for(int r=0;r<tr_.n_species;++r)if(r!=tr_.temp_row) {
            const auto k=tr_.idx(r,i),row=std::size_t(r)*nl,base=(std::size_t(r)*state_.n_cells+i)*nl;
            const double source=f.source_mass&&std::size_t(r)<f.source_mass->size()?(*f.source_mass)[r]:0;
            double edge_mass=0,edge_flux=0,edge_requested=0;
            if(f.boundaries)for(const auto& pulse:*f.boundaries) {
                edge_mass+=pulse.mass[r];edge_flux+=pulse.flux_in[r];edge_requested+=pulse.flux_out[r];
            }
            const double lateral=gatherLateralMass(i,r),infil=consumePending(tr_.xacc_from_surface[k]);
            const double link_in=consumePending(tr_.lacc_mass[k]),booked_link=consumePending(tr_.lout_mass[k]),node_in=gatherNodeMass(i,r);
            const double refund=f.refund_mass?(*f.refund_mass)[r]:0;
            std::vector<double> mass(nl),retention(nl),preset(nl),input(nl),withdrawal(nl);
            double preset_mass=0;
            for(std::size_t j=0;j<nl;++j) {
                mass[j]=next.mass[row+j];retention[j]=next.retention[row+j];
                preset[j]=consumePending(porewater_preset_out_[base+j]);preset_mass+=preset[j];
            }
            const double incoming=source+edge_mass+edge_flux+lateral+link_in-(link_packet?booked_link:0)+node_in+preset_mass;
            for(std::size_t j=0;j<nl;++j) {
                input[j]=incoming*weights[j]-preset[j];withdrawal[j]=(node_volume+f.source_out+f.boundary_out+implicit_link_volume)*weights[j];
            }
            input.front()+=infil-refund;withdrawal.front()+=excess_volume+f.dunne+(r==tr_.age_row?f.et:0);withdrawal.back()+=f.deep;
            auto advected=advectPorewater(old_water,new_water,retention,mass,water,input,withdrawal);
            if(!advected.error.empty())throw std::runtime_error(advected.error);
            double donor=0;for(std::size_t j=0;j<nl;++j)donor+=weights[j]*advected.concentration[j];
            const double link_out=link_packet?booked_link:implicit_link_volume*donor,node_out=node_volume*donor,source_out=f.source_out*donor;
            const double dunne=f.dunne*advected.concentration.front(),excess=excess_volume*advected.concentration.front();
            const double et=r==tr_.age_row?f.et*advected.concentration.front():0,deep=f.deep*advected.concentration.back();
            const double edge_out=f.boundary_out*donor;
            tr_.boundary_in[r]+=edge_mass;tr_.boundary_out[r]+=edge_out;tr_.boundary_flux_in[r]+=edge_flux;
            tr_.net_lateral[r]+=edge_mass+edge_flux-edge_out;
            if(f.boundaries)for(const auto& pulse:*f.boundaries) {
                auto& edge=boundaries_[pulse.boundary];edge.mass_in[r]+=pulse.mass[r];
                edge.mass_out[r]+=pulse.out*donor;edge.flux_in[r]+=pulse.flux_in[r];
            }
            tr_.gained_source[r]+=source;tr_.net_lateral[r]+=lateral;tr_.gained_infil[r]+=infil;
            tr_.gained_link[r]+=link_in;tr_.lost_link[r]+=link_out;tr_.gained_node[r]+=node_in;
            tr_.lost_node[r]+=node_out;tr_.lost_source[r]+=source_out;tr_.lost_deep[r]+=deep;
            tr_.lost_dunne[r]+=refund+excess+dunne;tr_.lost_et[r]+=et;
            if(f.dunne_mass){(*f.dunne_mass)[r]=dunne;(*f.excess_mass)[r]=excess;}
            else tr_.xacc_to_surface[k]+=excess+dunne;
            if(node_volume>0&&!tr_.node_out_mass.empty()) {
                const auto nn=tr_.node_out_mass.size()/tr_.n_species;
                for(std::size_t b=0;b<node_beds_.size();++b)if(node_beds_[b].cell==i) {
                    const int node=node_beds_[b].node;
                    if(node>=0&&std::size_t(node)<nn)tr_.node_out_mass[std::size_t(r)*nn+node]+=donor*heat_node_gathered_out_[b];
                }
            }
            if(std::size_t(i)<runtime_sources_.cells.size())for(auto index:runtime_sources_.cells[i]) {
                auto& source_row=runtime_sources_.rows[index];const double requested=std::max(0.,-source_row.flow)*f.runtime_dt;
                source_row.receipt.requested_mass[r]-=requested*donor;
                if(f.requested_source_out>0)source_row.receipt.applied_mass[r]-=requested/f.requested_source_out*source_out;
            }
            // Independent MASS sinks keep their existing saturated dissolved
            // availability rule. Their accepted mass is removed once from
            // the physical layers and exactly matched by provider receipts.
            if(edge_requested>0||std::size_t(i)<runtime_sources_.cells.size()) {
                double requested=edge_requested,available=0;
                std::vector<double> dissolved(nl);
                for(std::size_t j=0;j<nl;++j) {
                    const double cap=new_water[j]+retention[j];
                    dissolved[j]=cap>0?advected.mass[j]/cap*new_sat[j]:0;available+=dissolved[j];
                }
                if(std::size_t(i)<runtime_sources_.cells.size())for(auto index:runtime_sources_.cells[i])requested+=std::max(0.,-runtime_sources_.rows[index].rates[r])*f.runtime_dt;
                if(!std::isfinite(requested)||!std::isfinite(available))throw std::runtime_error("Ground porewater species removal receipt overflow.");
                const double accepted=std::min(requested,available),scale=requested>0?accepted/requested:0;
                if(available>0&&accepted>0)for(std::size_t j=0;j<nl;++j)advected.mass[j]-=accepted*(dissolved[j]/available);
                if(std::size_t(i)<runtime_sources_.cells.size())for(auto index:runtime_sources_.cells[i]) {
                    auto& source_row=runtime_sources_.rows[index];const double wanted=std::max(0.,-source_row.rates[r])*f.runtime_dt;
                    source_row.receipt.requested_mass[r]-=wanted;source_row.receipt.applied_mass[r]-=wanted*scale;
                }
                tr_.lost_source[r]+=accepted-edge_requested*scale;
                tr_.net_lateral[r]-=edge_requested*scale;tr_.boundary_flux_out[r]+=edge_requested*scale;
                if(f.boundaries)for(const auto& pulse:*f.boundaries)boundaries_[pulse.boundary].flux_out[r]+=pulse.flux_out[r]*scale;
            }
            const double decay=r==tr_.age_row?0:tr_.decay[k],keep=decay>0?std::exp(-decay*f.dt):1;
            for(std::size_t j=0;j<nl;++j) {
                if(!std::isfinite(advected.mass[j])||advected.mass[j]<0)throw std::runtime_error("Invalid accepted porewater species sink.");
                tr_.lost_reaction[r]+=advected.mass[j]*(1-keep);next.mass[row+j]=advected.mass[j]*keep;
                if(!next.settleOutgoingReservation(r,j,preset[j]))throw std::runtime_error("Accepted porewater export reservation does not match its gathered packet.");
            }
        }
        const double before=std::accumulate(next.generated_age.begin(),next.generated_age.end(),0.);
        if(!next.generateAge(new_column,f.dt))throw std::runtime_error("Ground porewater age overflow.");
        if(tr_.age_row>=0)tr_.generated_age[tr_.age_row]+=std::accumulate(next.generated_age.begin(),next.generated_age.end(),0.)-before;
        cell.porewater=std::move(next);
    }catch(const std::exception& error){cell.failed=1;try{cell.error=error.what();}catch(...) {}}
     catch(...){cell.failed=1;}
}
} // namespace openswmm::twoD
