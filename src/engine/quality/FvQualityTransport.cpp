// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "FvQualityTransport.hpp"
#include "core/SimulationContext.hpp"
#include "quality/NegativeSources.hpp"
#include "transport/InitialQualitySeeds.hpp"
#include "transport/components/ReactionModule/ReactionLegacyBinding.hpp"
#include "transport/components/ReactionModule/MsxLegacyTransport.hpp"
#include "transport/components/HeatFluxModules/HeatFluxes.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <sstream>
#include <map>
#include <set>
#include <locale>

namespace openswmm::quality {
bool FvQualityTransport::execute(SimulationContext& ctx,double dt) {
    accepted_=false;
    if(!water.ready || !(dt>0))return false;
    // Keep the existing owners for coupled mobile/porous stores and nonlinear
    // flow-through treatment until their accepted mass ports are integrated.
    bool advanced=ctx.treatment.hasAny();
    for(const auto& s:ctx.node_subtypes.storages.lid_state) advanced|=!s.cells.empty();
    for(const auto& s:ctx.reactions.sources)advanced|=s.booster>0;
    if(advanced) {
        const std::string warning="FV conservative transport: treatment, LID or booster ports still use the existing quality path; inspect continuity.";
        if(std::find(ctx.warnings.begin(),ctx.warnings.end(),warning)==ctx.warnings.end())ctx.warnings.push_back(warning);
        return false;
    }
    const auto nn=static_cast<std::size_t>(ctx.n_nodes()),nl=static_cast<std::size_t>(ctx.n_links()),n=nn+nl;
    const auto np=static_cast<std::size_t>(ctx.n_pollutants()),ns=static_cast<std::size_t>(ctx.reactions.n_species());
    auto& w=water.water;
    if(w.old_volume.size()!=n)throw std::runtime_error("FV quality water graph has the wrong extent.");
    for(std::size_t i=0;i<nn;++i) {
        w.source_volume[i]+=ctx.nodes.qual_vol_in[i];
        if(ctx.nodes.type[i]==NodeType::OUTFALL)w.exported[i]+=ctx.nodes.qual_vol_in[i];
    }
    if(!solute_mixer_.prepare(w)) {
        if(const char* path=std::getenv("OPENSWMM_FV_WATER_GRAPH_TRACE")) {
            std::ofstream os(path,std::ios::app);os<<std::setprecision(17);
            std::vector<double> incoming=w.source_volume,outgoing=w.exported;
            for(const auto& t:w.transfers) {incoming[t.receiver]+=t.volume;outgoing[t.donor]+=t.volume;}
            for(std::size_t i=0;i<n;++i)os<<ctx.current_time<<','<<i<<','<<w.old_volume[i]<<','<<w.new_volume[i]<<','<<w.source_volume[i]<<','<<incoming[i]<<','<<outgoing[i]<<','<<w.seeped[i]<<','<<w.evaporated[i]<<','<<(w.old_volume[i]+incoming[i]-w.new_volume[i]-outgoing[i]-w.seeped[i]-w.evaporated[i])<<"\n";
        }
        throw std::runtime_error("FV quality accepted-water graph does not close.");
    }
    thermal_water_=w;
    auto& thermal=thermal_water_;
    for(std::size_t i=0;i<n;++i) {thermal.exported[i]+=thermal.evaporated[i];thermal.evaporated[i]=0;}
    if(!thermal_mixer_.prepare(thermal))throw std::runtime_error("FV thermal accepted-water graph does not close.");
    const auto rows=np+ns+(ctx.options.water_age?1:0)+(ctx.options.heat_transport?1:0);
    held_.resize(rows);for(auto& h:held_)if(h.size()!=n)h.assign(n,0);
    results_.resize(rows); inputs_.resize(rows);
    auto& result=results_;auto& inputs=inputs_;
    for(std::size_t row=0;row<rows;++row) {
        inputs[row].old_concentration.assign(n,0);inputs[row].source_mass.assign(n,0);inputs[row].held_dry_mass=held_[row];
    }
    for(std::size_t p=0;p<np;++p) {
        auto& input=inputs[p];
        for(std::size_t i=0;i<nn;++i) {
            input.old_concentration[i]=ctx.nodes.conc_old[i*np+p];
            input.source_mass[i]=ctx.nodes.qual_mass_in[i*np+p]*dt;
            const double available=w.old_volume[i]*input.old_concentration[i]+held_[p][i];
            if(input.source_mass[i]<-available) {
                bookNegativeSourceClamp(ctx,static_cast<int>(i),static_cast<int>(p),-available-input.source_mass[i]);
                input.source_mass[i]=-available;
            }
        }
        for(std::size_t j=0;j<nl;++j)input.old_concentration[nn+j]=ctx.links.conc_old[j*np+p];
        for(const auto& b:water.boundary_imports) {
            const double c=ctx.options.outfall_backflow_zero?0:ctx.nodes.conc_old[b.node*np+p];
            input.source_mass[b.receiver]+=b.volume*c;
        }
    }
    if(ns) {
        transport::ensureMsxState(ctx);transport::armMsxRoutingLedger(ctx);
        for(std::size_t s=0;s<ns;++s) {
            auto& input=inputs[np+s];
            for(std::size_t i=0;i<nn;++i) {
                input.old_concentration[i]=ctx.reactions.msx_node_conc[i*ns+s];
                if(i*ns+s<ctx.reactions.msx_ext_mass_in.size())input.source_mass[i]=ctx.reactions.msx_ext_mass_in[i*ns+s]*dt;
            }
            for(std::size_t j=0;j<nl;++j)input.old_concentration[nn+j]=ctx.reactions.msx_link_conc[j*ns+s];
            for(const auto& b:water.boundary_imports) {
                double c=ctx.options.outfall_backflow_zero?0:ctx.reactions.msx_node_conc[b.node*ns+s];
                input.source_mass[b.receiver]+=b.volume*c;
            }
        }
    }
    std::size_t age=np+ns,temp=age+(ctx.options.water_age?1:0);
    if(ctx.options.water_age) {
        auto& state=ctx.water_age_state;
        if(state.node_age.size()!=nn)state.resize(nn,nl,ctx.n_subcatches());
        if(!state.legacy_seeded) {
            const double a0=ctx.water_age_config.global_age[static_cast<int>(WaterAgeSource::INITIAL_STATE)];
            std::fill(state.node_age.begin(),state.node_age.end(),a0);std::fill(state.link_age.begin(),state.link_age.end(),a0);
            transport::applyInitialAgeOverrides(ctx);state.legacy_seeded=true;
        }
        for(std::size_t i=0;i<nn;++i)inputs[age].old_concentration[i]=state.node_age[i];
        for(std::size_t j=0;j<nl;++j)inputs[age].old_concentration[nn+j]=state.link_age[j];
        for(std::size_t i=0;i<n;++i)inputs[age].source_mass[i]=dt*w.new_volume[i]; // d(V*age)/dt includes V
        for(std::size_t i=0;i<nn;++i)inputs[age].source_mass[i]+=state.node_age_vol_in[i]*dt;
        for(const auto& b:water.boundary_imports)inputs[age].source_mass[b.receiver]+=b.volume*(ctx.options.outfall_backflow_zero?0:state.node_age[b.node]);
    }
    if(ctx.options.heat_transport) {
        auto& state=ctx.heat_state;
        const double t0=ctx.heat_config.global_temp[static_cast<int>(HeatSource::INITIAL_STATE)];
        if(state.node_temp.size()!=nn)state.resize(nn,nl,t0);
        if(!state.legacy_seeded) {
            std::fill(state.node_temp.begin(),state.node_temp.end(),t0);std::fill(state.link_temp.begin(),state.link_temp.end(),t0);
            transport::applyInitialTempOverrides(ctx);state.legacy_seeded=true;
        }
        inputs[temp].nonnegative=false;
        for(std::size_t i=0;i<nn;++i) {inputs[temp].old_concentration[i]=state.node_temp[i];inputs[temp].source_mass[i]=state.node_temp_vol_in[i]*dt;}
        for(std::size_t j=0;j<nl;++j)inputs[temp].old_concentration[nn+j]=state.link_temp[j];
        for(const auto& b:water.boundary_imports)inputs[temp].source_mass[b.receiver]+=b.volume*state.node_temp[b.node];
    }
    // Solve every active family before publishing concentrations or receipts.
    for(std::size_t row=0;row<rows;++row) {
        if(row>=np && row<np+ns && ctx.reactions.species_is_wall[row-np])continue;
        auto& mixer=row>=np+ns?thermal_mixer_:solute_mixer_;
        if(!mixer.solve(inputs[row],result[row]).converged)throw std::runtime_error("FV conservative scalar transfer did not converge; no scalar rows published.");
    }
    auto dryDelta=[&](std::size_t row) {
        return std::accumulate(result[row].held_dry_mass.begin(),result[row].held_dry_mass.end(),0.0)-std::accumulate(held_[row].begin(),held_[row].end(),0.0);
    };
    for(std::size_t p=0;p<np;++p) {
        const auto& r=result[p];
        for(std::size_t i=0;i<nn;++i)ctx.nodes.conc[i*np+p]=r.concentration[i];
        for(std::size_t j=0;j<nl;++j)ctx.links.conc[j*np+p]=r.concentration[nn+j];
        for(std::size_t i=0;i<n;++i) {
            const double flood=i<nn?water.flooding[i]*r.concentration[i]:0;
            ctx.mass_balance.qual_routing_flood[p]+=flood;
            ctx.mass_balance.qual_routing_outflow[p]+=r.exported_mass[i]-flood;
            ctx.mass_balance.qual_routing_seep[p]+=r.seeped_mass[i];
        }
        for(const auto& b:water.boundary_imports)ctx.mass_balance.qual_routing_ex_in[p]+=b.volume*(ctx.options.outfall_backflow_zero?0:inputs[p].old_concentration[b.node]);
        ctx.mass_balance.qual_routing_final_dry[p]+=dryDelta(p);held_[p]=r.held_dry_mass;
    }
    for(std::size_t s=0;s<ns;++s) {
        if(ctx.reactions.species_is_wall[s])continue;
        const auto row=np+s;const auto& r=result[row];auto& ledger=ctx.reactions.msx_ledger;
        for(std::size_t i=0;i<nn;++i)ctx.reactions.msx_node_conc[i*ns+s]=r.concentration[i];
        for(std::size_t j=0;j<nl;++j)ctx.reactions.msx_link_conc[j*ns+s]=r.concentration[nn+j];
        ledger.inflow[s]+=std::accumulate(inputs[row].source_mass.begin(),inputs[row].source_mass.end(),0.0);
        for(std::size_t i=0;i<n;++i) {
            const double flood=i<nn?water.flooding[i]*r.concentration[i]:0;
            ledger.flooding[s]+=flood;ledger.outflow[s]+=r.exported_mass[i]-flood;ledger.seepage[s]+=r.seeped_mass[i];
        }
        ledger.final_dry[s]+=dryDelta(row);held_[row]=r.held_dry_mass;
    }
    if(ctx.options.water_age) {
        const auto& r=result[age];
        for(std::size_t i=0;i<nn;++i)ctx.water_age_state.node_age[i]=r.concentration[i];
        for(std::size_t j=0;j<nl;++j)ctx.water_age_state.link_age[j]=r.concentration[nn+j];held_[age]=r.held_dry_mass;
    }
    if(ctx.options.heat_transport) {
        const auto& r=result[temp];
        for(std::size_t i=0;i<nn;++i)if(w.new_volume[i]>0 || w.exported[i]>0)ctx.heat_state.node_temp[i]=r.concentration[i];
        for(std::size_t j=0;j<nl;++j)ctx.heat_state.link_temp[j]=r.concentration[nn+j];held_[temp]=r.held_dry_mass;
        transport::heat::applyHeatFluxes(ctx,dt);
    }
    // Zero-volume structures report the same accepted transported mixture.
    for(std::size_t j=0;j<nl;++j)if(ctx.links.type[j]!=LinkType::CONDUIT || ctx.links.xsect_shape[j]==XsectShape::DUMMY) {
        int up=ctx.links.flow[j]>=0?ctx.links.node1[j]:ctx.links.node2[j];if(up<0)continue;
        for(std::size_t p=0;p<np;++p)ctx.links.conc[j*np+p]=ctx.nodes.conc[up*np+p];
        for(std::size_t s=0;s<ns;++s)if(!ctx.reactions.species_is_wall[s])ctx.reactions.msx_link_conc[j*ns+s]=ctx.reactions.msx_node_conc[up*ns+s];
        if(ctx.options.heat_transport)ctx.heat_state.link_temp[j]=ctx.heat_state.node_temp[up];
        if(ctx.options.water_age)ctx.water_age_state.link_age[j]=ctx.water_age_state.node_age[up];
    }
    accepted_=true;return true;
}
void FvQualityTransport::applyPollutantDecay(SimulationContext& ctx,double dt) {
    const auto nn=static_cast<std::size_t>(ctx.n_nodes()),nl=static_cast<std::size_t>(ctx.n_links()),np=static_cast<std::size_t>(ctx.n_pollutants());
    for(std::size_t p=0;p<np;++p) {
        const double factor=std::exp(-ctx.pollutants.k_decay[p]*dt);
        for(std::size_t i=0;i<nn+nl;++i) {
            double& c=i<nn?ctx.nodes.conc[i*np+p]:ctx.links.conc[(i-nn)*np+p];
            const double before=c;c*=factor;
            ctx.mass_balance.qual_routing_reacted[p]+=(before-c)*water.water.new_volume[i];
        }
    }
}
} // namespace openswmm::quality

namespace openswmm::quality {
namespace {
struct DryRow {std::string family,name,units;bool signed_mass=false;};
std::vector<DryRow> dryRows(const SimulationContext& ctx) {
    std::vector<DryRow> rows;
    for(int p=0;p<ctx.n_pollutants();++p)
        rows.push_back({"P",ctx.pollutant_names.name_of(p),std::to_string(int(ctx.pollutants.units[p]))});
    for(int s=0;s<ctx.reactions.n_species();++s)
        rows.push_back({ctx.reactions.species_is_wall[s]?"W":"S",ctx.reactions.species_name[s],ctx.reactions.species_units[s]});
    if(ctx.options.water_age)rows.push_back({"A","age","s"});
    if(ctx.options.heat_transport)rows.push_back({"T","temperature","degC",true});
    return rows;
}
}
std::string FvQualityTransport::dryRestartState(const SimulationContext& ctx) const {
    if(!hasRetainedDryMass())return {};
    const auto rows=dryRows(ctx);const auto nn=std::size_t(ctx.n_nodes()),n=nn+ctx.n_links();
    if(held_.size()!=rows.size())throw std::runtime_error("FV dry inventory row mismatch.");
    std::size_t count=0;for(const auto& row:held_) {
        if(row.size()!=n)throw std::runtime_error("FV dry inventory element mismatch.");
        for(double mass:row)count+=mass!=0;
    }
    std::ostringstream os;os.imbue(std::locale::classic());os<<std::setprecision(17)<<"FV_DRY_INVENTORY 1 "<<count<<'\n';
    for(std::size_t r=0;r<rows.size();++r)for(std::size_t i=0;i<n;++i)if(held_[r][i]!=0) {
        const auto& row=rows[r];const double mass=held_[r][i];
        if(!std::isfinite(mass)||(!row.signed_mass&&mass<0)||row.family=="W")
            throw std::runtime_error("Invalid FV dry scalar inventory.");
        os<<std::quoted(row.family)<<' '<<std::quoted(row.name)<<' '<<std::quoted(row.units)<<' '
          <<std::quoted(i<nn?"N":"L")<<' '
          <<std::quoted(i<nn?ctx.node_names.name_of(int(i)):ctx.link_names.name_of(int(i-nn)))<<' '<<mass<<'\n';
    }
    return os.str();
}
std::string FvQualityTransport::restoreDryRestart(SimulationContext& ctx,const std::string& blob,bool apply) {
    const auto rows=dryRows(ctx);const auto nn=std::size_t(ctx.n_nodes()),n=nn+ctx.n_links();
    std::vector<std::vector<double>> next(rows.size(),std::vector<double>(n,0));
    if(!blob.empty()) {
        if(ctx.options.routing_model!=RoutingModel::FV)return "FV dry inventory requires FV routing.";
        bool advanced=ctx.treatment.hasAny();
        for(const auto& lid:ctx.node_subtypes.storages.lid_state)advanced|=!lid.cells.empty();
        for(const auto& source:ctx.reactions.sources)advanced|=source.booster>0;
        if(advanced)return "FV dry inventory restart requires conservative quality ports.";
        if(blob.size()>64u*1024u*1024u)return "FV dry inventory exceeds the restart limit.";
        std::istringstream is(blob);is.imbue(std::locale::classic());
        std::string magic;unsigned version=0;std::size_t count=0;
        if(!(is>>magic>>version>>count)||magic!="FV_DRY_INVENTORY"||version!=1||count>rows.size()*n)
            return "Invalid FV dry inventory header.";
        std::map<std::pair<std::string,std::string>,std::size_t> index;
        for(std::size_t r=0;r<rows.size();++r)index[{rows[r].family,rows[r].name}]=r;
        std::set<std::pair<std::size_t,std::size_t>> seen;
        for(std::size_t entry=0;entry<count;++entry) {
            std::string family,name,units,kind,element;double mass=0;
            if(!(is>>std::quoted(family)>>std::quoted(name)>>std::quoted(units)>>std::quoted(kind)>>std::quoted(element)>>mass))
                return "Truncated FV dry inventory.";
            const auto found=index.find({family,name});
            if(found==index.end())return "Unknown FV dry scalar: "+name;
            const auto r=found->second;const auto& row=rows[r];
            if(units!=row.units||family=="W")return "Incompatible FV dry scalar units or phase.";
            const int element_index=kind=="N"?ctx.node_names.find(element):kind=="L"?ctx.link_names.find(element):-1;
            if(element_index<0)return "Unknown FV dry element: "+element;
            const auto i=std::size_t(element_index)+(kind=="L"?nn:0);
            if(!std::isfinite(mass)||(!row.signed_mass&&mass<0)||!seen.insert({r,i}).second)
                return "Invalid or duplicate FV dry inventory entry.";
            next[r][i]=mass;
        }
        is>>std::ws;if(!is.eof())return "Trailing FV dry inventory data.";
    }
    if(apply) {held_=std::move(next);accepted_=false;water.ready=false;
        if(ctx.options.routing_model==RoutingModel::FV)ctx.reactions.msx_ledger.armed=false;}
    return {};
}
std::vector<double> FvQualityTransport::retainedPollutantMass(const SimulationContext& ctx) const {
    std::vector<double> mass(ctx.n_pollutants(),0);
    for(std::size_t p=0;p<mass.size()&&p<held_.size();++p)
        mass[p]=std::accumulate(held_[p].begin(),held_[p].end(),0.0);
    return mass;
}
std::vector<double> FvQualityTransport::retainedSpeciesMass(const SimulationContext& ctx) const {
    std::vector<double> mass(ctx.reactions.n_species(),0);const auto np=std::size_t(ctx.n_pollutants());
    for(std::size_t s=0;s<mass.size()&&np+s<held_.size();++s)
        mass[s]=std::accumulate(held_[np+s].begin(),held_[np+s].end(),0.0);
    return mass;
}
}
