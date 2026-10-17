// SPDX-License-Identifier: Apache-2.0
#include "MeshHeat.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../solver/InertialKernels.hpp"
#include "../subsurface/SubsurfaceData.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../transport/components/HeatFluxModules/BedExchange.hpp"
#include "../../transport/components/HeatFluxModules/SurfaceExchange.hpp"
#include "../../transport/components/HeatFluxModules/RadiativeExchange.hpp"
#include "../../transport/components/HeatFluxModules/HeatOverrides.hpp"
#include "../../transport/components/HeatFluxModules/SolarRadiation.hpp"
#include <algorithm>
#include <cmath>
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
}
std::string initializeMeshHeat(SimulationContext& ctx,const MeshData& mesh,SurfaceStateData& state,
                               const SolverOptions2D& options) {
    state.heat.clear();
    if(!options.coupled_heat || !ctx.options.heat_transport || state.transport.temp_row<0)return {};
    if(state.snow.active())return "Coupled snow/soil thermal ownership is not yet qualified.";
    if(options.bed_heat_model==1 && ctx.twod_io.aquifer_state && ctx.twod_io.aquifer_state->active)
        return "LOCAL_STORAGE cannot overlap an active aquifer; use GROUND_COLUMN after porous thermal coupling is qualified.";
    if(options.bed_heat_model==2)return "GROUND_COLUMN requires the qualified porous-ground thermal model.";
    auto& h=state.heat;h.context=&ctx;h.enabled=true;h.bed=options.bed_heat_model==1;
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
void prepareMeshHeat(SimulationContext& ctx,SurfaceStateData& state,double dt) {
    if(!state.heat.enabled)return;
    if(state.heat.radiative)climate::updateSolarForcing(ctx,dt,true);
    if(state.heat.bed)state.heat.ground_now=groundTemperature(ctx);
}
double applyMeshHeatCell(const MeshData& mesh,const SolverOptions2D& options,MeshHeatState& h,int i,
                         double mass,double volume,double evaporated,double dt) noexcept {
    if(!h.enabled||!(dt>0))return mass;
    const auto& ctx=*h.context;const auto elem=HeatElement::cell2d(i);
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
