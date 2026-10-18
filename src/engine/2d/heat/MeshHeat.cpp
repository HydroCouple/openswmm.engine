// SPDX-License-Identifier: Apache-2.0
#include "MeshHeat.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../solver/InertialKernels.hpp"
#include "../subsurface/SubsurfaceData.hpp"
#include "../../data/LidNodeData.hpp"
#include "../gw/GwTransportData.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../transport/components/HeatFluxModules/BedExchange.hpp"
#include "../../transport/components/HeatFluxModules/SurfaceExchange.hpp"
#include "../../transport/components/HeatFluxModules/RadiativeExchange.hpp"
#include "../../transport/components/HeatFluxModules/HeatOverrides.hpp"
#include "../../transport/components/HeatFluxModules/SolarRadiation.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
namespace openswmm::twoD {
namespace {
using namespace transport::heat;
bool hyporheicEligible(const SimulationContext& ctx,const MeshData& mesh,int i) {
    const auto& tag=mesh.tri_tag[i];
    if(tag.rfind("channel:",0)==0)return true;
    for(const auto& row:ctx.heat_config.overrides.rows)
        if(row.attr==HeatAttr::SED_HYPORHEIC_VELOCITY && row.value>0 &&
           ((row.scope==HeatScope::MESH_TAG && row.name==tag) ||
            (row.scope==HeatScope::CELL2D && row.name==std::to_string(i+1))))return true;
    return false;
}
double phi2(double z) noexcept {
    if(std::abs(z)<1e-3)return .5+z*(1./6+z*(1./24+z*(1./120+z/720)));
    return (std::expm1(z)-z)/(z*z);
}
double phi2prime(double z) noexcept {
    if(std::abs(z)<1e-3)return 1./6+z*(1./12+z*(1./40+z/180));
    return (z*std::expm1(z)-2*(std::expm1(z)-z))/(z*z*z);
}
// Integral of the temperature increments of the SAME exact linear pair.
// Keeping increments avoids cancellation against the absolute temperatures.
void pairIntegrals(const BedCoupling& g,double tw,double tb,double j0,double slope,
                   double area,double dt,double& iw,double& ib) noexcept {
    const double a=-(area*slope+g.g_wb)/g.c_w,b=g.g_wb/g.c_w;
    const double c=g.g_wb/g.c_b,d=-(g.g_wb+g.g_bg)/g.c_b;
    const double f=(-area*j0+g.g_wb*(tb-tw))/g.c_w;
    const double q=(g.g_wb*(tw-tb)+g.g_bg*(g.t_gr-tb))/g.c_b;
    const double root=std::sqrt((a-d)*(a-d)+4*b*c);
    const double fast=.5*(a+d-root),det=a*d-b*c;
    const double slow=fast!=0?det/fast:0;
    const double z1=slow*dt,z2=fast*dt;
    double beta,alpha;
    if(std::abs(z1-z2)>1e-7) {
        beta=(phi2(z1)-phi2(z2))/(z1-z2);alpha=phi2(z1)-beta*z1;
    } else {
        const double z=.5*(z1+z2);beta=phi2prime(z);alpha=phi2(z)-beta*z;
    }
    iw=dt*dt*(alpha*f+beta*dt*(a*f+b*q));
    ib=dt*dt*(alpha*q+beta*dt*(c*f+d*q));
}
MeshAtmosphericFlux outward(const MeshHeatState& h,int i,double t,double evaporation) noexcept {
    const auto& ctx=*h.context;const auto elem=HeatElement::cell2d(i);
    MeshAtmosphericFlux flux;
    flux.radiation=h.radiative?radiativeFluxOut(ctx,elem,t,true):0;
    if(h.surface) {
        const double air=(ctx.forcing.elementAirTempF(elem,ctx.climate_state.temperature)-32)*5./9;
        const double wind=ctx.forcing.elementWindMph(elem,ctx.climate_state.wind_speed)*kMphToMs;
        const double transfer=windFunction(wind,ctx.options.wind_func_coeff_a,ctx.options.wind_func_coeff_b);
        // The Bowen sensible term has a removable vapour-deficit singularity.
        // Evaporation itself is the hydrology's accepted volume, not this wind formula.
        flux.latent=ctx.options.water_density*latentHeatOfVaporization(t)*evaporation;
        flux.sensible=ctx.options.water_density*latentHeatOfVaporization(t)*
              transfer*kBowenCoeff*ctx.options.pressure_ratio*(t-air);
    }
    return flux;
}
double groundColumnStep(const MeshData& mesh,const SolverOptions2D& options,MeshHeatState& h,int i,
    double mass,double volume,double evaporated,double dt) {
    std::vector<ThermalStore> stores;std::vector<ThermalContact> contacts;std::vector<ThermalBoundary> boundaries;
    const double channel=h.channel_footprint.empty()?0:h.channel_footprint[i];
    auto assembled=appendGroundColumnSystem(mesh,options,h.groundOwner(i),i,&h,mass,volume,evaporated,dt,channel,stores,contacts,boundaries);
    auto solved=solveThermalContacts(stores,contacts,boundaries,dt);
    if(!solved.error.empty())throw std::runtime_error(solved.error);
    return commitGroundColumnSystem(assembled,solved);
}
}
GroundColumnAssembly appendGroundColumnSystem(const MeshData& mesh,const SolverOptions2D& options,GroundHeatState& owner,int i,
    MeshHeatState* h,double mass,double volume,double evaporated,double dt,double channel_area,
    std::vector<ThermalStore>& stores,std::vector<ThermalContact>& contacts,std::vector<ThermalBoundary>& boundaries) {
    auto& c=owner.cells[i].column;const auto& o=owner.config->options;
    if(channel_area<0||channel_area>c.area||!std::isfinite(channel_area))throw std::runtime_error("Channel thermal footprint exceeds its soil cell area.");
    GroundColumnAssembly a;a.owner=&owner;a.mesh_heat=h;a.cell=i;a.mass=mass;a.volume=volume;a.evaporated=evaporated;a.seconds=dt;
    static const std::vector<GroundLidThermalOwner> no_lid_owners;
    const auto& lid_owners=std::size_t(i)<owner.lid_owners.size()?owner.lid_owners[std::size_t(i)]:no_lid_owners;
    double lid_area=0;
    if(!lid_owners.empty()) {
        if(!owner.context)throw std::runtime_error("Mesh LID contact has no simulation context.");
        const double initial=owner.context->heat_config.global_temp[static_cast<int>(HeatSource::INITIAL_STATE)];
        for(const auto& binding:lid_owners) {
            if(binding.row<0||std::size_t(binding.row)>=owner.context->node_subtypes.storages.lid_state.size()||
               !(binding.area>0)||!std::isfinite(binding.area))
                throw std::runtime_error("Invalid resolved Mesh LID thermal owner.");
            const auto& state=owner.context->node_subtypes.storages.lid_state[std::size_t(binding.row)];
            if(!state.richards)throw std::runtime_error("Mesh LID ground contact requires a Richards layer state.");
            GroundColumnAssembly::LidEnergy draft;draft.row=binding.row;
            if(auto error=buildLidNodeThermalGraph(state,0,owner.water_capacity,
                owner.context->heat_config.conduction,initial,draft.graph);!error.empty())
                throw std::runtime_error(error);
            if(!owner.context->heat_config.layer_conduction)draft.graph.contacts.clear();
            draft.first_store=int(stores.size());
            draft.area=binding.area;
            stores.insert(stores.end(),draft.graph.stores.begin(),draft.graph.stores.end());
            draft.physical_energy=state.thermal_energy;
            if(draft.physical_energy.empty()) {
                draft.physical_energy.assign(state.cells.size(),0);
                for(std::size_t k=0;k<draft.graph.cells.size();++k)
                    draft.physical_energy[draft.graph.cells[k]]=draft.graph.stores[k].energy;
            }
            draft.physical_ground_contact_j=state.thermal_ground_contact_j;
            if(draft.physical_ground_contact_j.empty())
                draft.physical_ground_contact_j.assign(state.cells.size(),0.0);
            else if(draft.physical_ground_contact_j.size()!=state.cells.size()||
                    std::any_of(draft.physical_ground_contact_j.begin(),draft.physical_ground_contact_j.end(),
                                [](double x){return !std::isfinite(x);}))
                throw std::runtime_error("Mesh LID ground-contact receipt shape or value is invalid.");
            lid_area+=binding.area;a.lids.push_back(std::move(draft));
        }
    }
    if(!std::isfinite(lid_area)||lid_area+channel_area>c.area*(1+1e-12))
        throw std::runtime_error("Mesh LID and channel thermal footprints exceed their groundwater cell area.");
    a.first_store=int(stores.size());a.soil_count=int(c.energy.size());
    for(std::size_t j=0;j<c.energy.size();++j) {
        const double capacity=owner.activeCapacity(i,j);
        if(!(capacity>0)||!std::isfinite(capacity))
            throw std::runtime_error("Accepted groundwater export leaves no active thermal capacity in its soil layer.");
        stores.push_back({capacity,c.energy[j]});
    }
    a.first_contact=contacts.size();
    if(o.conduction)for(auto contact:c.contacts()) {contact.a+=a.first_store;contact.b+=a.first_store;contacts.push_back(contact);}
    a.vertical_count=contacts.size()-a.first_contact;
    if(h&&volume>0) {
        a.water=int(stores.size());a.temperature=mass/volume;a.water_initial_j=owner.water_capacity*mass;
        stores.push_back({owner.water_capacity*volume,a.water_initial_j});
        double eta=0,depth=0;inertial::cellEtaDepth(mesh,options,i,volume,eta,depth);
        a.wet=depth>0?std::clamp(volume/depth,0.,c.area):0;
        const double exposed_fraction=std::max(0.,1-(channel_area+lid_area)/c.area);
        const double contact_area=a.wet*exposed_fraction;
        const auto& sc=sedimentFor(*h->context,HeatElement::cell2d(i));
        if(o.conduction&&contact_area>0)contacts.push_back({a.water,a.first_store,contact_area/(.5*(c.depth_edges[1]-c.depth_edges[0])/c.conductivity[0])});
        double pores=0;
        for(std::size_t j=0;j<c.energy.size();++j)
            pores+=c.theta[j]*c.area*std::max(0.,std::min(sc.bed_thickness,c.depth_edges[j+1])-c.depth_edges[j]);
        if(h->hyporheic_eligible[i]&&pores>0&&sc.hyporheic_velocity>0)
            for(std::size_t j=0;j<c.energy.size();++j) {
                const double v=c.theta[j]*c.area*std::max(0.,std::min(sc.bed_thickness,c.depth_edges[j+1])-c.depth_edges[j]);
                if(v>0)contacts.push_back({a.water,a.first_store+int(j),owner.water_capacity*sc.hyporheic_velocity*contact_area*v/pores});
            }
        const double evaporation=a.wet>0?evaporated/(a.wet*dt):0;
        a.flux0=outward(*h,i,a.temperature,evaporation);a.flux1=outward(*h,i,a.temperature+kProbeC,evaporation);
        a.slope=std::max(0.,(a.flux1.total()-a.flux0.total())/kProbeC);
    }
    a.contact_count=contacts.size()-a.first_contact;a.first_boundary=boundaries.size();
    std::vector<ThermalBoundary> ground;
    const double snow_fraction=owner.snowContactFraction(i);
    const double exposed_fraction=std::max(0.,1-(channel_area+lid_area)/c.area);
    const double bare_fraction=(1-a.wet/c.area)*exposed_fraction;
    a.top_count=owner.boundaries(i,bare_fraction*(1-snow_fraction),ground);a.ground_count=ground.size();
    for(auto boundary:ground){boundary.store+=a.first_store;boundaries.push_back(boundary);}
    const double snow_area=std::max(0.,c.area-a.wet)*exposed_fraction*snow_fraction;
    if(owner.appendSnowContact(i,snow_area,boundaries))a.snow_boundary=int(boundaries.size()-1);
    a.atmosphere=boundaries.size();
    if(a.water>=0)boundaries.push_back({a.water,a.wet*a.slope,a.temperature,-a.wet*a.flux0.total()});
    for(auto& lid:a.lids) {
        for(const auto& vertical:lid.graph.contacts) {
            lid.vertical_contacts.push_back(contacts.size());
            contacts.push_back({int(lid.first_store)+vertical.a,int(lid.first_store)+vertical.b,vertical.conductance});
        }
        if(o.conduction) {
            const auto& graph=lid.graph;
            ThermalContact contact;
            if(auto error=buildLidNodeGroundThermalContact(graph,stores.size(),a.first_store,
                lid.area,c.area,c.depth_edges[1]-c.depth_edges[0],
                c.conductivity.front(),0,contact);!error.empty())throw std::runtime_error(error);
            contact.a+=lid.first_store;lid.ground_contact=contacts.size();contacts.push_back(contact);
        }
    }
    return a;
}
double commitGroundColumnSystem(GroundColumnAssembly& a,const ThermalSolution& solved) noexcept {
    auto& owner=*a.owner;auto& cell=owner.cells[a.cell];auto& c=cell.column;auto* h=a.mesh_heat;
    for(const auto& lid:a.lids) {
        if(lid.physical_ground_contact_j.size()!=lid.graph.physical_thickness_m.size()||
           (lid.ground_contact!=std::numeric_limits<std::size_t>::max()&&
            (lid.graph.cells.empty()||lid.graph.cells.back()>=lid.physical_ground_contact_j.size()||
             lid.ground_contact>=solved.contact_j.size()||
             !std::isfinite(lid.physical_ground_contact_j[lid.graph.cells.back()]-solved.contact_j[lid.ground_contact])))) {
            cell.failed=1;try{cell.error="Invalid solved Mesh LID ground-contact receipt.";}catch(...){}
            if(h)h->failures[a.cell]=1;return a.mass;
        }
    }
    for(auto& lid:a.lids) {
        for(std::size_t k=0;k<lid.graph.cells.size();++k) {
            const auto index=lid.graph.cells[k];const double energy=solved.energy[std::size_t(lid.first_store)+k];
            if(index>=lid.physical_energy.size()||!std::isfinite(energy)) {
                cell.failed=1;try{cell.error="Invalid solved Mesh LID thermal energy layout.";}catch(...){}
                if(h)h->failures[a.cell]=1;return a.mass;
            }
            lid.physical_energy[index]=energy;
        }
    }
    double contact=0;
    for(std::size_t k=0;k<a.contact_count;++k) {
        const double j=solved.contact_j[a.first_contact+k];
        if(k<a.vertical_count)cell.vertical_absolute_j+=std::abs(j);else contact+=j;
    }
    cell.water_contact_j+=contact;if(h)h->water_bed_j[a.cell]+=contact;
    for(std::size_t k=0;k<a.ground_count;++k) {
        const double j=solved.boundary_j[a.first_boundary+k];
        if(k<a.top_count)cell.surface_j+=j;else {cell.deep_j+=j;if(h)h->ground_j[a.cell]+=j;}
    }
    if(a.snow_boundary>=0) {
        const double j=solved.boundary_j[std::size_t(a.snow_boundary)];
        cell.surface_j+=j;
        if(auto error=owner.applySnowContactEnergy(a.cell,j);!error.empty()) {
            cell.failed=1;try{cell.error=error;}catch(...){}
            if(h)h->failures[a.cell]=1;
        }
    }
    for(auto& lid:a.lids) {
        auto& state=owner.context->node_subtypes.storages.lid_state[std::size_t(lid.row)];
        state.thermal_energy.swap(lid.physical_energy);
        for(const auto index:lid.vertical_contacts)cell.vertical_absolute_j+=std::abs(solved.contact_j[index]);
        if(lid.ground_contact!=std::numeric_limits<std::size_t>::max()) {
            cell.lid_contact_j+=solved.contact_j[lid.ground_contact];
            lid.physical_ground_contact_j[lid.graph.cells.back()]-=solved.contact_j[lid.ground_contact];
        }
        state.thermal_ground_contact_j.swap(lid.physical_ground_contact_j);
    }
    for(int j=0;j<a.soil_count;++j)c.energy[j]=solved.energy[a.first_store+j];
    if(h)h->refreshBedView(a.cell);
    if(a.water<0)return a.mass;
    const double delta=solved.temperature[a.water]-a.temperature;
    h->water_j[a.cell]+=solved.energy[a.water]-a.water_initial_j;
    h->atmospheric_j[a.cell]+=solved.boundary_j[a.atmosphere];h->evaporated_m3[a.cell]+=a.evaporated;
    const auto received=[&](double first,double second){return -a.wet*a.seconds*(first+(a.slope>0?(second-first)/kProbeC:0)*delta);};
    h->radiative_j[a.cell]+=received(a.flux0.radiation,a.flux1.radiation);h->sensible_j[a.cell]+=received(a.flux0.sensible,a.flux1.sensible);
    h->latent_j[a.cell]+=received(a.flux0.latent,a.flux1.latent);h->reported_temperature[a.cell]=solved.temperature[a.water];
    return solved.energy[a.water]/owner.water_capacity;
}
std::string initializeMeshHeat(SimulationContext& ctx,const MeshData& mesh,SurfaceStateData& state,
                               const SolverOptions2D& options,GroundHeatState* aquifer,const std::vector<uint8_t>* coverage) {
    state.heat.clear();
    if(!options.coupled_heat || !ctx.options.heat_transport || state.transport.temp_row<0)return {};
    if(!(ctx.options.water_density>0)||!(ctx.options.water_specific_heat>0)||
       !std::isfinite(ctx.options.water_density)||!std::isfinite(ctx.options.water_specific_heat))
        return "WATER_DENSITY and WATER_SPECIFIC_HEAT_CAPACITY must be finite and positive.";
    if(options.bed_heat_model==1 && ctx.twod_io.aquifer_state && ctx.twod_io.aquifer_state->active)
        return "LOCAL_STORAGE cannot overlap an active aquifer; use GROUND_COLUMN after porous thermal coupling is qualified.";
    auto& h=state.heat;h.context=&ctx;h.enabled=true;h.bed=options.bed_heat_model>0;
    h.ground_column=options.bed_heat_model==2;
    h.surface_inherits=options.heat_surface<0;h.radiative_inherits=options.heat_radiative<0;
    h.surface=options.heat_surface<0?ctx.heat_config.surface_exchange:options.heat_surface!=0;
    h.radiative=options.heat_radiative<0?ctx.heat_config.radiative_exchange:options.heat_radiative!=0;
    if(h.bed&&state.transport.n_species>0) {
        h.bed_species=state.transport.n_species;
        h.bed_age_row=state.transport.age_row;
        h.bed_temp_row=state.transport.temp_row;
        state.transport.gained_bed.assign(static_cast<std::size_t>(h.bed_species),0);
    }
    const auto errors=transport::heat::resolveHeatOverrides(ctx);
    if(!errors.empty())return errors.front();
    const auto n=std::size_t(mesh.n_cells());
    h.hyporheic_eligible.resize(n);
    for(std::size_t i=0;i<n;++i)h.hyporheic_eligible[i]=hyporheicEligible(ctx,mesh,int(i));
    h.reported_temperature.assign(n,ctx.heat_config.global_temp[static_cast<int>(HeatSource::INITIAL_STATE)]);
    for(auto* row:{&h.atmospheric_j,&h.radiative_j,&h.sensible_j,&h.ground_j,&h.water_bed_j,&h.water_j,&h.latent_j,&h.evaporated_m3})row->assign(n,0);
    h.failures.assign(n,0);
    h.ground_column_seconds.assign(n,0);
    if(h.ground_column) {
        if(!ctx.twod_io.gw)return "Ground-column boundary configuration is unavailable.";
        h.aquifer_ground=aquifer;h.aquifer_owned.assign(n,0);std::vector<unsigned char> standalone(n,1);
        for(std::size_t i=0;i<n;++i) {
            const bool covered=ctx.twod_io.aquifer_state&&ctx.twod_io.aquifer_state->active&&
                coverage&&i<coverage->size()&&(*coverage)[i];
            if(aquifer&&aquifer->active()&&!aquifer->cells[i].column.energy.empty()) {
                h.aquifer_owned[i]=1;standalone[i]=0;aquifer->cells[i].surface_owned=true;
            } else if(covered)return "GROUND_COLUMN over an aquifer requires POROUS_EQUILIBRIUM and groundwater temperature transport.";
        }
        if(auto error=h.standalone_ground.initializeStandalone(ctx,mesh,*ctx.twod_io.gw,standalone);!error.empty())return error;
        h.bed_energy.resize(n);h.bed_capacity.resize(n);
        for(std::size_t i=0;i<n;++i)h.refreshBedView(int(i));
        h.initial_bed_energy=h.bed_energy;return {};
    }
    if(h.bed) {
        h.bed_energy.resize(n);h.bed_capacity.resize(n);
        h.ground_now=groundTemperature(ctx);
        for(std::size_t i=0;i<n;++i) {
            const auto& sc=sedimentFor(ctx,HeatElement::cell2d(int(i)));
            // New-model density/cp describe the solid phase. Porewater is
            // an additional finite store; the legacy channel bed is untouched.
            const double cap=((1-sc.mesh_porosity)*sc.sed_density*sc.sed_specific_heat+
                sc.mesh_porosity*ctx.options.water_density*ctx.options.water_specific_heat)*
                mesh.tri_area[i]*sc.bed_thickness;
            if(!(cap>0)||!std::isfinite(cap))return "Mesh bed heat capacity must be finite and positive.";
            h.bed_capacity[i]=cap;
            const double initial=sc.has_initial_temp?sc.initial_temp:groundTempFor(ctx,sc,h.ground_now);
            h.bed_energy[i]=cap*initial;
        }
        h.initial_bed_energy=h.bed_energy;
        // The fixed local-bed pore volume is a physical storage property,
        // even when quality transport is disabled. Quality mass arrays below
        // are optional, but the result interface must still expose this store.
        h.bed_pore_volume.resize(n);
        for(std::size_t i=0;i<n;++i) {
            const auto& sc=sedimentFor(ctx,HeatElement::cell2d(int(i)));
            h.bed_pore_volume[i]=mesh.tri_area[i]*sc.bed_thickness*sc.mesh_porosity;
            if(!std::isfinite(h.bed_pore_volume[i]))return "Local bed porewater volume must be finite.";
        }
        if(state.transport.n_species>0) {
            h.bed_carried_mass.assign(n*h.bed_species,0);
            h.bed_initial_mass=h.bed_carried_mass;h.bed_exchange_mass=h.bed_carried_mass;h.bed_generated_age.assign(n,0);
        }
    }
    return {};
}
std::string configureGroundLidOwners(SimulationContext& ctx,const MeshData& mesh,GroundHeatState& owner,
    const std::vector<GwNodeBed>& beds) {
    owner.lid_owners.assign(mesh.n_cells(),{});
    if(!owner.active())return {};
    constexpr double ft2_to_m2=.09290304;
    std::vector<double> areas(mesh.n_cells(),0);
    std::set<int> seen;
    const auto& storage=ctx.node_subtypes.storages;
    for(const auto& bed:beds) {
        if(!bed.exchange||bed.node<0||bed.node>=ctx.n_nodes()||bed.cell<0||bed.cell>=mesh.n_cells())continue;
        const int row=ctx.node_subtypes.storage_row(bed.node);
        if(row<0||std::size_t(row)>=storage.lid.size()||std::size_t(row)>=storage.lid_state.size())continue;
        const auto& config=storage.lid[std::size_t(row)];
        if(config.control<0||config.bottom!=LidNodeBottom::MeshAquifer)continue;
        if(!seen.insert(bed.node).second)return "Mesh Richards LID has duplicate resolved groundwater bed owners.";
        const auto& lid=storage.lid_state[std::size_t(row)];
        if(!lid.richards||lid.cells.empty())return "Mesh LID heat requires a persistent Richards layer state.";
        const double bed_area=bed.area>0?bed.area:mesh.tri_area[std::size_t(bed.cell)];
        const double lid_area=lid.cells.back().area*ft2_to_m2;
        const double area=std::min({bed_area,lid_area,mesh.tri_area[std::size_t(bed.cell)]});
        if(!(area>0)||!std::isfinite(area))return "Mesh LID thermal contact has no positive mapped overlap area.";
        auto& total=areas[std::size_t(bed.cell)];total+=area;
        if(!std::isfinite(total)||total>mesh.tri_area[std::size_t(bed.cell)]*(1+1e-12))
            return "Mesh Richards LID footprints overlap beyond their groundwater cell area.";
        owner.lid_owners[std::size_t(bed.cell)].push_back({bed.node,row,area});
    }
    for(auto& rows:owner.lid_owners)
        std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.node<b.node;});
    return {};
}
double applyLocalBedSpecies(const MeshData& mesh,MeshHeatState& h,int i,int r,double mass,double volume,double dt) noexcept {
    if(!h.enabled||!h.bed||r==h.bed_temp_row||r<0||r>=h.bed_species||!(volume>0)||!(dt>0))return mass;
    const auto& sc=sedimentFor(*h.context,HeatElement::cell2d(i));
    if(h.ground_column) {
        if(i<0||std::size_t(i)>=h.aquifer_owned.size()||!h.aquifer_owned[i]||!h.aquifer_ground||
           std::size_t(i)>=h.aquifer_ground->cells.size()||!(sc.bed_thickness>0)||
           !std::isfinite(sc.bed_thickness)||sc.solute_diffusivity<0||!std::isfinite(sc.solute_diffusivity)||
           sc.hyporheic_velocity<0||!std::isfinite(sc.hyporheic_velocity)||!std::isfinite(mass)||mass<0) {
            h.failures[i]=1;return mass;
        }
        auto& ground=h.aquifer_ground->cells[std::size_t(i)];
        auto& pore=ground.porewater;const auto& column=ground.column;
        const auto nl=column.theta.size();
        if(!pore.active()||std::size_t(r)>=std::size_t(pore.n_species)||pore.n_species!=h.bed_species||
           column.depth_edges.size()!=nl+1||pore.mass.size()!=std::size_t(pore.n_species)*nl||
           pore.retention.size()!=pore.mass.size()||pore.reserved_out.size()!=pore.mass.size()||
           !std::isfinite(column.area)||!(column.area>0)) {h.failures[i]=1;return mass;}
        const double mesh_area=mesh.tri_area[std::size_t(i)];
        const double channel_area=h.channel_footprint.empty()?0.0:h.channel_footprint[std::size_t(i)];
        if(!(mesh_area>0)||!std::isfinite(mesh_area)||channel_area<0||!std::isfinite(channel_area)||
           channel_area>mesh_area*(1+1e-10)) {h.failures[i]=1;return mass;}
        const double contact_area=std::max(0.0,mesh_area-std::min(channel_area,mesh_area));
        const double velocity=std::size_t(i)<h.hyporheic_eligible.size()&&h.hyporheic_eligible[std::size_t(i)]
            ?sc.hyporheic_velocity:0.0;
        const double rate=sc.solute_diffusivity/sc.bed_thickness+velocity;
        if(!(contact_area>0)||!(rate>0))return mass;
        double total_pore_overlap=0;
        for(std::size_t j=0;j<nl;++j) {
            const double overlap=std::max(0.0,std::min(column.depth_edges[j+1],sc.bed_thickness)-column.depth_edges[j]);
            total_pore_overlap+=column.theta[j]*overlap;
        }
        if(!(total_pore_overlap>0))return mass;

        // The surface store contacts every fixed soil layer intersected by
        // the bed. Solve this star-shaped finite system as one backward-Euler
        // update; sequential pair updates would make the result depend on
        // layer order. Reserved accepted groundwater exports remain outside
        // the contact donor inventory until their owning gather settles them.
        std::vector<double> qdt(nl,0.0),active_mass(nl,0.0),capacity(nl,0.0);
        const auto base=std::size_t(r)*nl;
        double denominator=volume,numerator=mass;
        for(std::size_t j=0;j<nl;++j) {
            const double overlap=std::max(0.0,std::min(column.depth_edges[j+1],sc.bed_thickness)-column.depth_edges[j]);
            if(!(overlap>0)||!(column.theta[j]>0))continue;
            const double bulk=column.area*(column.depth_edges[j+1]-column.depth_edges[j]);
            capacity[j]=column.theta[j]*bulk+pore.retention[base+j];
            active_mass[j]=pore.contactAvailableMass(r,j);
            qdt[j]=dt*rate*contact_area*(column.theta[j]*overlap/total_pore_overlap);
            if(!(capacity[j]>0)||!std::isfinite(capacity[j])||!std::isfinite(active_mass[j])||active_mass[j]<0||
               !std::isfinite(qdt[j])||qdt[j]<0) {h.failures[i]=1;return mass;}
            if(qdt[j]==0)continue;
            const double conductance_dt=qdt[j];
            const double denominator_j=capacity[j]+conductance_dt;
            const double weight=conductance_dt*capacity[j]/denominator_j;
            denominator+=weight;
            numerator+=weight*(active_mass[j]/capacity[j]);
        }
        if(!(denominator>0)||!std::isfinite(denominator)||!std::isfinite(numerator)) {h.failures[i]=1;return mass;}
        const double surface_concentration=numerator/denominator;
        if(!std::isfinite(surface_concentration)||surface_concentration<0) {h.failures[i]=1;return mass;}
        auto next=pore;double total_transfer=0;
        for(std::size_t j=0;j<nl;++j)if(qdt[j]>0) {
            const double soil_concentration=(active_mass[j]+qdt[j]*surface_concentration)/(capacity[j]+qdt[j]);
            const double increment=qdt[j]*(surface_concentration-soil_concentration);
            if(!std::isfinite(increment)||!next.canApplyContactMassIncrement(r,j,increment)||
               !next.applyContactMassIncrement(r,j,increment)) {h.failures[i]=1;return mass;}
            total_transfer+=increment;
        }
        const double next_surface=mass-total_transfer;
        if(!std::isfinite(total_transfer)||!std::isfinite(next_surface)||next_surface<0) {h.failures[i]=1;return mass;}
        pore=std::move(next);return next_surface;
    }
    if(i<0||std::size_t(i)>=h.bed_pore_volume.size())return mass;
    const double pores=h.bed_pore_volume[std::size_t(i)];if(!(pores>0))return mass;
    const auto k=std::size_t(r)*h.bed_pore_volume.size()+std::size_t(i);
    const double q=mesh.tri_area[std::size_t(i)]*(sc.solute_diffusivity/sc.bed_thickness+
        (h.hyporheic_eligible[std::size_t(i)]?sc.hyporheic_velocity:0));
    if(!(q>0))return mass;
    const auto delta=exchangePair(mass/volume,h.bed_carried_mass[k]/pores,volume,pores,q,dt);
    const double next=volume*(mass/volume+delta.dc_w),transfer=mass-next,bed=h.bed_carried_mass[k]+transfer;
    if(!std::isfinite(q)||!std::isfinite(next)||!std::isfinite(bed)||next<0||bed<0||
       !std::isfinite(h.bed_exchange_mass[k]+transfer)){h.failures[i]=1;return mass;}
    h.bed_carried_mass[k]=bed;h.bed_exchange_mass[k]+=transfer;return next;
}
bool ageLocalBedPorewater(MeshHeatState& h,double dt) noexcept {
    if(!h.enabled||h.ground_column||h.bed_age_row<0||h.bed_pore_volume.empty())return true;
    if(!(dt>0)||!std::isfinite(dt))return false;
    const auto n=h.bed_pore_volume.size(),base=std::size_t(h.bed_age_row)*n;
    for(std::size_t i=0;i<n;++i)if(!std::isfinite(h.bed_carried_mass[base+i]+dt*h.bed_pore_volume[i])||
        !std::isfinite(h.bed_generated_age[i]+dt*h.bed_pore_volume[i]))return false;
    for(std::size_t i=0;i<n;++i){const double generated=dt*h.bed_pore_volume[i];h.bed_carried_mass[base+i]+=generated;h.bed_generated_age[i]+=generated;}
    return true;
}
void MeshHeatState::refreshBedView(int i) {
    if(!ground_column)return;
    const auto& c=groundOwner(i).cells[i].column;
    const double thickness=sedimentFor(*context,HeatElement::cell2d(i)).bed_thickness;
    double e=0,cap=0;
    for(std::size_t j=0;j<c.energy.size();++j) {
        const double fraction=std::clamp((thickness-c.depth_edges[j])/(c.depth_edges[j+1]-c.depth_edges[j]),0.,1.);
        e+=fraction*c.energy[j];cap+=fraction*groundOwner(i).activeCapacity(i,j);
    }
    bed_energy[i]=e;bed_capacity[i]=cap;
}
void advectStandaloneGround(MeshHeatState& h,int i,double volume,double donor) noexcept {
    if(!h.ground_column||h.aquifer_owned[i]||!(volume>0))return;
    auto& owner=h.standalone_ground;auto& cell=owner.cells[i];auto& c=cell.column;
    const double carried=volume*owner.water_capacity,input=carried*donor;
    // Downward implicit upwind throughflow. The forward substitution solves
    // every finite layer and the outlet together; large accepted volumes do
    // not drain more heat than is available or require temperature clipping.
    const double inlet=donor;
    if(!std::isfinite(carried)||!std::isfinite(input)){h.failures[i]=1;return;}
    for(std::size_t j=0;j<c.energy.size();++j) {
        const double next=(c.energy[j]+carried*donor)/(c.capacity[j]+carried);
        if(!std::isfinite(next)||!std::isfinite(c.energy[j]+carried*(donor-next))){h.failures[i]=1;return;}
        donor=next;
    }
    if(!std::isfinite(cell.advective_j+input-carried*donor)){h.failures[i]=1;return;}
    donor=inlet;
    for(std::size_t j=0;j<c.energy.size();++j) {
        const double next=(c.energy[j]+carried*donor)/(c.capacity[j]+carried);
        c.energy[j]+=carried*(donor-next);donor=next;
    }
    cell.advective_j+=input-carried*donor;h.refreshBedView(i);
}
void prepareMeshHeat(SimulationContext& ctx,SurfaceStateData& state,double dt) {
    if(!state.heat.enabled)return;
    std::fill(state.heat.ground_column_seconds.begin(),state.heat.ground_column_seconds.end(),0);
    if(state.heat.surface_inherits)state.heat.surface=ctx.heat_config.surface_exchange;
    if(state.heat.radiative_inherits)state.heat.radiative=ctx.heat_config.radiative_exchange;
    if(state.heat.radiative)climate::updateSolarForcing(ctx,dt,true);
    if(state.heat.bed)state.heat.ground_now=groundTemperature(ctx);
    if(state.heat.ground_column)state.heat.standalone_ground.prepare();
}
double applyMeshHeatCell(const MeshData& mesh,const SolverOptions2D& options,MeshHeatState& h,int i,
                         double mass,double volume,double evaporated,double dt) noexcept {
    if(!h.enabled||!(dt>0))return mass;
    const auto& ctx=*h.context;const auto elem=HeatElement::cell2d(i);
    if(h.ground_column) {
        if(std::size_t(i)<h.ground_column_seconds.size()) {
            auto& applied=h.ground_column_seconds[i];applied+=dt;
            if(!std::isfinite(applied)){applied=0;h.failures[i]=1;}
        }
        try {
            const double updated=groundColumnStep(mesh,options,h,i,mass,volume,evaporated,dt);
            if(!(volume>0)) {
                if(ctx.heat_config.dry_temp_policy==DryTempPolicy::AIR)h.reported_temperature[i]=airTempCelsius(ctx);
                else if(ctx.heat_config.dry_temp_policy==DryTempPolicy::DEFAULT)h.reported_temperature[i]=HeatConfigData::kDefaultTemp;
            }
            return updated;
        } catch(const std::exception& e) {
            h.failures[i]=1;try{h.groundOwner(i).cells[i].error=e.what();}catch(...){}return mass;
        } catch(...) {h.failures[i]=1;return mass;}
    }
    const double rho=ctx.options.water_density,cp=ctx.options.water_specific_heat;
    const auto& sc=sedimentFor(ctx,elem);
    const double area=mesh.tri_area[i],k=sc.thermal_diffusivity*sc.sed_density*sc.sed_specific_heat;
    const double tground=groundTempFor(ctx,sc,h.ground_now);
    const double gb=h.bed&&sc.ground_depth>0?k*area/sc.ground_depth:0;
    if(!(volume>0)) {
        if(h.bed) {
            const double old=h.bed_energy[i],tb=old/h.bed_capacity[i];
            const double delta=h.bed_capacity[i]*(tground-tb)*(-std::expm1(-gb*dt/h.bed_capacity[i]));
            h.bed_energy[i]+=delta;h.ground_j[i]+=delta;
        }
        if(ctx.heat_config.dry_temp_policy==DryTempPolicy::AIR)h.reported_temperature[i]=airTempCelsius(ctx);
        else if(ctx.heat_config.dry_temp_policy==DryTempPolicy::DEFAULT)h.reported_temperature[i]=HeatConfigData::kDefaultTemp;
        return mass;
    }
    const double tw=mass/volume;
    double eta=0,depth=0;inertial::cellEtaDepth(mesh,options,i,volume,eta,depth);
    const double wet=depth>0?std::clamp(volume/depth,0.0,area):0;
    const double evap_rate=wet>0?evaporated/(wet*dt):0;
    const auto flux0=outward(h,i,tw,evap_rate),flux1=outward(h,i,tw+kProbeC,evap_rate);
    const double j0=flux0.total(),j1=flux1.total();
    const double slope=std::max(0.0,(j1-j0)/kProbeC);
    double dw=0,db=0,qg=0,qwb=0,qa=0,iw=0;
    if(h.bed) {
        BedCoupling g;
        g.c_w=rho*cp*volume;g.c_b=h.bed_capacity[i];g.g_bg=gb;g.t_gr=tground;
        g.g_wb=k*wet/sc.bed_thickness+
            (h.hyporheic_eligible[i]?rho*cp*sc.hyporheic_velocity*wet:0);
        const double tb=h.bed_energy[i]/g.c_b;
        const auto step=relaxPair(g,tw,tb,j0,j1,kProbeC,wet,dt);dw=step.dt_w;db=step.dt_b;
        double ib=0;pairIntegrals(g,tw,tb,j0,slope,wet,dt,iw,ib);
        qg=gb*((tground-tb)*dt-ib);
        // Define reciprocal bed receipts from the accepted bed-energy
        // increment and its deep-ground receipt. The equivalent time
        // integral subtracts two O(g_wb*dt) values and becomes catastrophically
        // ill-conditioned for stiff hyporheic exchange.
        qwb=g.c_b*db-qg;
        qa=-wet*(j0*dt+slope*iw);
    } else {
        dw=relaxT(j0,j1,kProbeC,wet,volume,dt,rho,cp);
        iw=-wet*j0/(rho*cp*volume)*dt*dt*phi2(-wet*slope*dt/(rho*cp*volume));
        qa=rho*cp*volume*dw;
    }
    const double updated=mass+volume*dw;
    if(!std::isfinite(updated)||!std::isfinite(db)||!std::isfinite(qa)||!std::isfinite(qg)||!std::isfinite(qwb)) {
        h.failures[i]=1;return mass;
    }
    if(h.bed)h.bed_energy[i]+=h.bed_capacity[i]*db;
    h.ground_j[i]+=qg;h.atmospheric_j[i]+=qa;h.water_bed_j[i]+=qwb;
    h.water_j[i]+=rho*cp*volume*dw;h.evaporated_m3[i]+=evaporated;
    const auto received=[&](double a,double b){return -wet*(a*dt+(slope>0?(b-a)/kProbeC:0)*iw);};
    h.latent_j[i]+=received(flux0.latent,flux1.latent);
    h.radiative_j[i]+=received(flux0.radiation,flux1.radiation);
    h.sensible_j[i]+=received(flux0.sensible,flux1.sensible);
    h.reported_temperature[i]=tw+dw;
    return updated;
}
void advanceDryGroundColumns(const MeshData& mesh,const SolverOptions2D& options,
                             SurfaceStateData& state,double seconds) noexcept {
    auto& h=state.heat;
    if(!h.enabled||!h.ground_column||!(seconds>0))return;
    const int temp_row=state.transport.temp_row;
    for(int i=0;i<mesh.n_cells();++i) {
        if(i>=int(state.volume.size())||state.volume[i]>0)continue;
        const double applied=std::size_t(i)<h.ground_column_seconds.size()?h.ground_column_seconds[i]:0.;
        const double remaining=seconds-applied;
        if(!(remaining>std::numeric_limits<double>::epsilon()*std::max(1.,seconds)))continue;
        double mass=0;
        if(temp_row>=0&&temp_row<state.transport.n_species)
            mass=state.transport.cell_mass[state.transport.idx(temp_row,i)];
        mass=applyMeshHeatCell(mesh,options,h,i,mass,0,0,remaining);
        if(temp_row>=0&&temp_row<state.transport.n_species)
            state.transport.cell_mass[state.transport.idx(temp_row,i)]=mass;
    }
}
} // namespace openswmm::twoD
