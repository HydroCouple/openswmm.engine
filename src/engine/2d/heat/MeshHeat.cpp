// SPDX-License-Identifier: Apache-2.0
#include "MeshHeat.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../solver/InertialKernels.hpp"
#include "../subsurface/SubsurfaceData.hpp"
#include "../gw/GwTransportData.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../transport/components/HeatFluxModules/BedExchange.hpp"
#include "../../transport/components/HeatFluxModules/SurfaceExchange.hpp"
#include "../../transport/components/HeatFluxModules/RadiativeExchange.hpp"
#include "../../transport/components/HeatFluxModules/HeatOverrides.hpp"
#include "../../transport/components/HeatFluxModules/SolarRadiation.hpp"
#include <algorithm>
#include <cmath>
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
struct AtmosphericFlux {
    double radiation=0,sensible=0,latent=0;
    double total() const noexcept {return radiation+sensible+latent;}
};
AtmosphericFlux outward(const MeshHeatState& h,int i,double t,double evaporation) noexcept {
    const auto& ctx=*h.context;const auto elem=HeatElement::cell2d(i);
    AtmosphericFlux flux;
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
    auto& owner=h.groundOwner(i);auto& cell=owner.cells[i];auto& c=cell.column;
    const auto& o=owner.config->options;const auto& sc=sedimentFor(*h.context,HeatElement::cell2d(i));
    const int water=int(c.energy.size());auto stores=c.stores();
    auto contacts=o.conduction?c.contacts():std::vector<ThermalContact>{};
    const auto vertical_count=contacts.size();
    double wet=0,tw=0;AtmosphericFlux f0,f1;double slope=0;
    if(volume>0) {
        tw=mass/volume;double eta=0,depth=0;inertial::cellEtaDepth(mesh,options,i,volume,eta,depth);
        wet=depth>0?std::clamp(volume/depth,0.,c.area):0;
        stores.push_back({owner.water_capacity*volume,owner.water_capacity*mass});
        if(o.conduction&&wet>0)contacts.push_back({water,0,wet/(.5*(c.depth_edges[1]-c.depth_edges[0])/c.conductivity[0])});
        double pores=0;
        for(std::size_t j=0;j<c.energy.size();++j)
            pores+=c.theta[j]*c.area*std::max(0.,std::min(sc.bed_thickness,c.depth_edges[j+1])-c.depth_edges[j]);
        if(h.hyporheic_eligible[i]&&pores>0&&sc.hyporheic_velocity>0)
            for(std::size_t j=0;j<c.energy.size();++j) {
                const double v=c.theta[j]*c.area*std::max(0.,std::min(sc.bed_thickness,c.depth_edges[j+1])-c.depth_edges[j]);
                if(v>0)contacts.push_back({water,int(j),owner.water_capacity*sc.hyporheic_velocity*wet*v/pores});
            }
        const double evaporation=wet>0?evaporated/(wet*dt):0;
        f0=outward(h,i,tw,evaporation);f1=outward(h,i,tw+kProbeC,evaporation);
        slope=std::max(0.,(f1.total()-f0.total())/kProbeC);
    }
    std::vector<ThermalBoundary> boundaries;
    const auto top_count=owner.boundaries(i,1-wet/c.area,boundaries);
    const auto ground_count=boundaries.size();
    if(volume>0)boundaries.push_back({water,wet*slope,tw,-wet*f0.total()});
    auto solved=solveThermalContacts(stores,contacts,boundaries,dt);
    if(!solved.error.empty())throw std::runtime_error(solved.error);
    double contact=0;
    for(std::size_t j=0;j<solved.contact_j.size();++j)
        if(j<vertical_count)cell.vertical_absolute_j+=std::abs(solved.contact_j[j]);else contact+=solved.contact_j[j];
    cell.water_contact_j+=contact;h.water_bed_j[i]+=contact;
    for(std::size_t j=0;j<ground_count;++j) {
        if(j<top_count)cell.surface_j+=solved.boundary_j[j];else cell.deep_j+=solved.boundary_j[j];
        h.ground_j[i]+=solved.boundary_j[j];
    }
    for(int j=0;j<water;++j)c.energy[j]=solved.energy[j];
    h.refreshBedView(i);
    if(!(volume>0))return mass;
    const double delta_t=solved.temperature[water]-tw;
    h.water_j[i]+=solved.energy[water]-stores[water].energy;
    h.atmospheric_j[i]+=solved.boundary_j.back();h.evaporated_m3[i]+=evaporated;
    const auto received=[&](double a,double b){return -wet*dt*(a+(slope>0?(b-a)/kProbeC:0)*delta_t);};
    h.radiative_j[i]+=received(f0.radiation,f1.radiation);h.sensible_j[i]+=received(f0.sensible,f1.sensible);
    h.latent_j[i]+=received(f0.latent,f1.latent);h.reported_temperature[i]=solved.temperature[water];
    return solved.energy[water]/owner.water_capacity;
}
}
std::string initializeMeshHeat(SimulationContext& ctx,const MeshData& mesh,SurfaceStateData& state,
                               const SolverOptions2D& options,GroundHeatState* aquifer,const std::vector<uint8_t>* coverage) {
    state.heat.clear();
    if(!options.coupled_heat || !ctx.options.heat_transport || state.transport.temp_row<0)return {};
    if(!(ctx.options.water_density>0)||!(ctx.options.water_specific_heat>0)||
       !std::isfinite(ctx.options.water_density)||!std::isfinite(ctx.options.water_specific_heat))
        return "WATER_DENSITY and WATER_SPECIFIC_HEAT_CAPACITY must be finite and positive.";
    if(state.snow.active())return "Coupled snow/soil thermal ownership is not yet qualified.";
    if(options.bed_heat_model==1 && ctx.twod_io.aquifer_state && ctx.twod_io.aquifer_state->active)
        return "LOCAL_STORAGE cannot overlap an active aquifer; use GROUND_COLUMN after porous thermal coupling is qualified.";
    auto& h=state.heat;h.context=&ctx;h.enabled=true;h.bed=options.bed_heat_model>0;
    h.ground_column=options.bed_heat_model==2;
    h.surface_inherits=options.heat_surface<0;h.radiative_inherits=options.heat_radiative<0;
    h.surface=options.heat_surface<0?ctx.heat_config.surface_exchange:options.heat_surface!=0;
    h.radiative=options.heat_radiative<0?ctx.heat_config.radiative_exchange:options.heat_radiative!=0;
    const auto errors=transport::heat::resolveHeatOverrides(ctx);
    if(!errors.empty())return errors.front();
    const auto n=std::size_t(mesh.n_cells());
    h.hyporheic_eligible.resize(n);
    for(std::size_t i=0;i<n;++i)h.hyporheic_eligible[i]=hyporheicEligible(ctx,mesh,int(i));
    h.reported_temperature.assign(n,ctx.heat_config.global_temp[static_cast<int>(HeatSource::INITIAL_STATE)]);
    for(auto* row:{&h.atmospheric_j,&h.radiative_j,&h.sensible_j,&h.ground_j,&h.water_bed_j,&h.water_j,&h.latent_j,&h.evaporated_m3})row->assign(n,0);
    h.failures.assign(n,0);
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
    }
    return {};
}
void MeshHeatState::refreshBedView(int i) {
    if(!ground_column)return;
    const auto& c=groundOwner(i).cells[i].column;
    const double thickness=sedimentFor(*context,HeatElement::cell2d(i)).bed_thickness;
    double e=0,cap=0;
    for(std::size_t j=0;j<c.energy.size();++j) {
        const double fraction=std::clamp((thickness-c.depth_edges[j])/(c.depth_edges[j+1]-c.depth_edges[j]),0.,1.);
        e+=fraction*c.energy[j];cap+=fraction*c.capacity[j];
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
        qwb=g.g_wb*((tw-tb)*dt+iw-ib);
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
} // namespace openswmm::twoD
