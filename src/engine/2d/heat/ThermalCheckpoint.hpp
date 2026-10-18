// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "GroundHeat.hpp"
#include "MeshHeat.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/HeatData.hpp"
#include "../gw/GwTransportData.hpp"
#include <cmath>
#include <stdexcept>

namespace openswmm::twoD::checkpoint {
using Archive=detail::CompletedSourceArchive;

inline void material(Archive& a,ThermalMaterial& m) {
    a(m.porosity,m.grain_density,m.grain_specific_heat,m.grain_conductivity,
      m.water_conductivity,m.air_conductivity,m.water_heat_capacity,m.geometric);
}
inline void radiative(Archive& a,RadiativeConfig& r) {
    a(r.shortwave_wm2,r.albedo,r.shade_factor,r.sky_view,r.emiss_water,r.emiss_landcover,
      r.atm_emiss_coeff,r.lw_reflection,r.landcover_temp,r.sw_mode,r.sw_ts_index);
}
inline void sediment(Archive& a,SedimentConfig& s) {
    a(s.mesh_ground,s.ground_source_configured,s.mesh_porosity,s.thermal_diffusivity,s.solute_diffusivity,
      s.bed_thickness,s.ground_depth,s.ground_temp,s.has_ground_temp,s.ground_ts_index,
      s.hyporheic_velocity,s.sed_density,s.sed_specific_heat,s.initial_temp,s.has_initial_temp);
}
inline void heatConfig(Archive& a,HeatConfigData& h) {
    a(h.configured,h.radiative_exchange,h.surface_exchange,h.dry_temp_policy,h.layer_conduction,
      h.sediment_exchange);
    radiative(a,h.radiative);sediment(a,h.sediment);
    auto rows=h.overrides.rows;
    a.sequence(rows,[&](auto& r){a(r.attr,r.scope,r.name,r.value);});
    auto& s=h.solar;
    a(s.latitude_deg,s.longitude_deg,s.timezone_hours,s.has_latitude,s.has_longitude,s.has_timezone,
      s.elevation_m,s.has_elevation,s.aod380,s.aod500,s.precip_water_cm,s.ozone_cm,s.ground_albedo);
    auto& c=h.cloud;a(c.configured,c.use_timeseries,c.ts_index,c.fraction,c.sw_atten_k,c.sw_atten_n,c.lw_cloud_k);
    for(double& value:h.global_temp)a(value);
}

inline void groundHeat(Archive& a,GroundHeatState& h,bool model) {
    bool active=h.active();a(active);
    if(!active)return;
    if(!h.config)throw std::runtime_error("Active thermal checkpoint has no groundwater configuration.");
    if(model) {
        a(h.density,h.water_capacity,h.top_series,h.deep_series);
        auto o=h.config->options;
        a(o.thermal_model,o.thermal_layers,o.lateral_conduction,o.thermal_max_step,o.water_conductivity,
          o.air_conductivity,o.surface_resistance,o.soil_et_heat,o.transport_pollutants,o.transport_msx,
          o.transport_age,o.transport_temperature,o.dispersion,o.conduction,o.surface_thermal_bc,
          o.surface_thermal_arg,o.deep_thermal_bc,o.deep_thermal_arg,o.deep_depth,o.thermal_mixing,
          o.c_diff,o.authored);
        std::vector<std::string> authored(o.thermal_authored.begin(),o.thermal_authored.end());a(authored);
        auto params=h.config->params;
        a.sequence(params,[&](auto& p){a(p.scope,p.tag,p.cell,p.rho_s,p.c_s,p.lambda_s,p.a_s,p.alpha_L,
            p.alpha_T,p.D_m,p.D_v,p.geo_flux);});
        a.sequence(h.cells,[&](auto& c){
            material(a,c.column.material);a(c.column.area,c.column.depth_edges,c.column.initial_energy,
                c.initial_j,c.geo_flux,c.surface_owned);
            auto& p=c.porewater;a(p.n_species,p.age_row,p.temp_row,p.retention,p.initial_mass);
        },true);
        return;
    }
    std::string runtime_profile="ground heat runtime v2";a(runtime_profile);
    a(h.top_now,h.deep_now);
    // Ground columns may be initialized lazily on the first accepted thermal
    // step. Their model identity is frozen separately, so restore the
    // accepted cell payload into a fresh target whose runtime vector is still
    // empty.
    a.sequence(h.cells,[&](auto& c){
        auto& col=c.column;auto& p=c.porewater;
        a.fixed(col.theta,col.capacity,col.energy,col.conductivity,c.saturated_water,c.reserved_out_water,
                p.mass,p.contact_mass,p.reserved_out,p.generated_age);
        a(c.advective_j,c.source_j,c.surface_j,c.deep_j,c.latent_j,c.lateral_j,c.water_contact_j,
          c.channel_contact_j,c.vertical_absolute_j,c.failed,c.error);
    });
    if(a.reading()) {
        const auto bad=[](const char* reason){throw std::runtime_error(std::string("Invalid thermal ground checkpoint state: ")+reason+".");};
        for(const auto& c:h.cells) {
            const auto& col=c.column;
            if(col.theta.empty()&&col.depth_edges.empty()&&col.capacity.empty()&&col.energy.empty()&&
               col.conductivity.empty()&&c.saturated_water.empty()&&c.reserved_out_water.empty()&&c.porewater.mass.empty()&&
               c.porewater.n_species==0)continue;
            if(col.theta.size()+1!=col.depth_edges.size()||col.capacity.size()!=col.theta.size()||
               col.energy.size()!=col.theta.size()||col.conductivity.size()!=col.theta.size()||
               c.saturated_water.size()!=col.theta.size()||c.reserved_out_water.size()!=col.theta.size()||
               c.porewater.mass.size()!=c.porewater.n_species*col.theta.size()||
               c.porewater.reserved_out.size()!=c.porewater.mass.size()) {
                throw std::runtime_error("Invalid thermal ground checkpoint layer shapes (layers="+
                    std::to_string(col.theta.size())+", edges="+std::to_string(col.depth_edges.size())+
                    ", capacity="+std::to_string(col.capacity.size())+", energy="+std::to_string(col.energy.size())+
                    ", conductivity="+std::to_string(col.conductivity.size())+
                    ", saturation="+std::to_string(c.saturated_water.size())+
                    ", porewater="+std::to_string(c.porewater.mass.size())+", porewater species="+
                    std::to_string(c.porewater.n_species)+").");
            }
            for(std::size_t j=0;j<col.theta.size();++j)
                if(!std::isfinite(col.theta[j])||col.theta[j]<0||col.theta[j]>col.material.porosity||
                   !(col.capacity[j]>0)||!std::isfinite(col.capacity[j])||!std::isfinite(col.energy[j])||
                   !(col.conductivity[j]>0)||!std::isfinite(col.conductivity[j])||
                   !std::isfinite(c.saturated_water[j])||c.saturated_water[j]<0||
                   !std::isfinite(c.reserved_out_water[j])||c.reserved_out_water[j]<0||
                   !(col.capacity[j]-col.material.water_heat_capacity*c.reserved_out_water[j]>0))
                    bad("invalid layer thermodynamic value or outgoing heat-capacity reservation");
            const auto& p=c.porewater;
            if(p.mass.size()!=p.reserved_out.size())bad("invalid porewater reservation shape");
            for(std::size_t k=0;k<p.mass.size();++k)
                if(!std::isfinite(p.mass[k])||p.mass[k]<0||!std::isfinite(p.reserved_out[k])||
                   p.reserved_out[k]<0||p.reserved_out[k]>p.mass[k])bad("invalid porewater inventory or outgoing reservation");
        }
    }
}

inline std::string groundHeatModelIdentity(const GroundHeatState& state) {
    Archive a;auto copy=state;groundHeat(a,copy,true);return a.bytes();
}

inline std::string meshHeatModelIdentity(const MeshHeatState& state) {
    Archive a;auto h=state;
    a(h.enabled,h.surface,h.radiative,h.bed,h.surface_inherits,h.radiative_inherits,h.ground_column,
      h.bed_species,h.bed_age_row,h.bed_temp_row);
    if(!h.enabled)return a.bytes();
    a.fixed(h.aquifer_owned,h.hyporheic_eligible,h.initial_bed_energy,h.bed_pore_volume,
            h.bed_initial_mass);
    auto ground=groundHeatModelIdentity(h.standalone_ground);a(ground);
    if(h.context) {auto config=h.context->heat_config;heatConfig(a,config);}
    return a.bytes();
}

inline void meshHeat(Archive& a,MeshHeatState& h) {
    if(!h.enabled)return;
    a(h.ground_now);
    a(h.bed_energy,h.bed_capacity,h.reported_temperature,h.atmospheric_j,h.radiative_j,
      h.sensible_j,h.ground_j,h.water_bed_j,h.water_j,h.latent_j,h.evaporated_m3,
      h.failures,h.channel_footprint,h.bed_carried_mass,h.bed_exchange_mass,h.bed_generated_age);
    groundHeat(a,h.standalone_ground,false);
    if(!h.standalone_ground.cells.empty() &&
       h.standalone_ground.cells.size()!=h.reported_temperature.size())
        throw std::runtime_error("Thermal ground checkpoint cell shape differs from the mesh.");
    if(a.reading()) {
        const auto bad=[](){throw std::runtime_error("Invalid thermal mesh checkpoint state.");};
        for(const auto* row:{&h.bed_energy,&h.atmospheric_j,&h.radiative_j,&h.sensible_j,&h.ground_j,
            &h.water_bed_j,&h.water_j,&h.latent_j,&h.evaporated_m3,&h.bed_carried_mass,
            &h.bed_exchange_mass,&h.bed_generated_age})for(double v:*row)if(!std::isfinite(v))bad();
        for(double v:h.bed_capacity)if(!(v>0)||!std::isfinite(v))bad();
        for(double v:h.reported_temperature)if(!std::isfinite(v))bad();
        for(double v:h.evaporated_m3)if(v<0)bad();
        for(int v:h.failures)if(v<0)bad();
        for(double v:h.bed_carried_mass)if(v<0)bad();
        const auto n=h.reported_temperature.size();
        for(const auto* row:{&h.atmospheric_j,&h.radiative_j,&h.sensible_j,&h.ground_j,
            &h.water_bed_j,&h.water_j,&h.latent_j,&h.evaporated_m3})if(row->size()!=n)bad();
        if(h.failures.size()!=n || (!h.channel_footprint.empty()&&h.channel_footprint.size()!=n) ||
           h.bed_energy.size()!=h.bed_capacity.size() ||
           (!h.bed_energy.empty()&&h.bed_energy.size()!=n) ||
           (!h.bed_carried_mass.empty()&&h.bed_carried_mass.size()!=n*std::size_t(h.bed_species)) ||
           (!h.bed_exchange_mass.empty()&&h.bed_exchange_mass.size()!=n*std::size_t(h.bed_species)) ||
           (!h.bed_generated_age.empty()&&h.bed_generated_age.size()!=n))bad();
    }
}
} // namespace openswmm::twoD::checkpoint
