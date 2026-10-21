// SPDX-License-Identifier: Apache-2.0
#include "GroundHeat.hpp"
#include "../../hydrology/surface/MeshSnow.hpp"
#include "../data/MeshData.hpp"
#include "../subsurface/SubsurfaceData.hpp"
#include "../subsurface/SubsurfaceTransportState.hpp"
#include "../gw/GwTransportData.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../data/TableData.hpp"
#include "../../transport/components/HeatFluxModules/SurfaceExchange.hpp"
#include "../../transport/components/HeatFluxModules/HeatOverrides.hpp"
#include "../../transport/components/HeatFluxModules/BedExchange.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
namespace openswmm::twoD {
namespace {
template<class Row>bool targets(const Row& row,const MeshData& mesh,int i) {
    return row.scope==GwScope::GLOBAL || (row.scope==GwScope::CELL&&row.cell==i) ||
           (row.scope==GwScope::TAG&&std::size_t(i)<mesh.tri_tag.size()&&mesh.tri_tag[i]==row.tag);
}
double total(const std::vector<double>& values){return std::accumulate(values.begin(),values.end(),0.);}
int resolveSeries(const SimulationContext& ctx,const std::string& kind,const std::string& arg) {
    if(kind!="TIMESERIES"&&kind!="FLUX_TIMESERIES")return -1;
    return ctx.find_timeseries(arg);
}
double boundaryValue(SimulationContext& ctx,const std::string& kind,const std::string& arg,int series) {
    if(series>=0) {
        auto& table=ctx.tables[series];
        if(kind=="TIMESERIES"&&(table.x.empty()||ctx.current_date<table.x.front()||ctx.current_date>table.x.back()))
            throw std::runtime_error("Ground temperature series does not cover the current simulation time.");
        return table_tseries_lookup_cursor(table,ctx.current_date);
    }
    if(arg.empty())return 0;
    if(kind=="ADIABATIC"||kind=="AIR"||kind=="SURFACE_WATER")return 0;
    return std::stod(arg);
}
double boundaryValueAt(SimulationContext& ctx,const std::string& kind,const std::string& arg,int series,double time) {
    if(series<0)return boundaryValue(ctx,kind,arg,series);
    const auto& table=ctx.tables[series];const double origin=ctx.options.start_date,date=origin+time/86400.;
    if(table.x.empty()||table.x.size()!=table.y.size())throw std::runtime_error("Ground thermal series has no matching time/value rows.");
    const double first=(table.x.front()-origin)*86400.,last=(table.x.back()-origin)*86400.;
    if(time<first||time>last) {
        if(kind=="TIMESERIES") {
            if(date<table.x.front()||date>table.x.back())throw std::runtime_error("Ground temperature series does not cover the coupled interval.");
            // An authored endpoint can differ from its elapsed-second value
            // by one date rounding unit. A reservoir holds that endpoint.
            return time<first?table.y.front():table.y.back();
        }
        return 0;
    }
    if(time==first)return table.y.front();
    if(time==last)return table.y.back();
    const auto upper=std::upper_bound(table.x.begin(),table.x.end(),time,
        [origin](double seconds,double date){return seconds<(date-origin)*86400.;});
    const auto j=std::size_t(upper-table.x.begin());
    const double a=(table.x[j-1]-origin)*86400.,b=(table.x[j]-origin)*86400.;
    return table.y[j-1]+(time-a)*(table.y[j]-table.y[j-1])/(b-a);
}
}
std::string GroundHeatState::initialize(SimulationContext& ctx,const MeshData& mesh,const SubsurfaceState& water,
    const SubsurfaceTransportState& tr,const GwTransportData& cfg,const std::vector<uint8_t>& coverage) {
    clear();
    if(cfg.options.thermal_model!="POROUS_EQUILIBRIUM"||!ctx.options.heat_transport||tr.temp_row<0)return {};
    if(!(ctx.options.water_density>0)||!(ctx.options.water_specific_heat>0)||
       !std::isfinite(ctx.options.water_density)||!std::isfinite(ctx.options.water_specific_heat))
        return "WATER_DENSITY and WATER_SPECIFIC_HEAT_CAPACITY must be finite and positive.";
    if(coverage.size()!=std::size_t(water.n_cells))return "Ground thermal aquifer coverage shape differs from the hydraulic owner mask.";
    const auto& options=cfg.options;
    const int ts=resolveSeries(ctx,options.surface_thermal_bc,options.surface_thermal_arg);
    const int ds=resolveSeries(ctx,options.deep_thermal_bc,options.deep_thermal_arg);
    if((options.surface_thermal_bc=="TIMESERIES"||options.surface_thermal_bc=="FLUX_TIMESERIES")&&ts<0)
        return "SURFACE_THERMAL_BC references an unknown project time series.";
    if((options.deep_thermal_bc=="TIMESERIES"||options.deep_thermal_bc=="FLUX_TIMESERIES")&&ds<0)
        return "DEEP_THERMAL_BC references an unknown project time series.";
    GroundHeatState next;next.context=&ctx;next.config=&cfg;next.top_series=ts;next.deep_series=ds;
    next.density=ctx.options.water_density;next.water_capacity=ctx.options.water_density*ctx.options.water_specific_heat;
    next.cells.resize(water.n_cells);
    for(int i=0;i<water.n_cells;++i) {
        if(!coverage[i]||!(water.zs[i]>0)||!(water.area[i]>0))continue;
        GwParamsRow p;
        for(int pass=0;pass<3;++pass)for(const auto& row:cfg.params)
            if(int(row.scope)==pass&&targets(row,mesh,i))p=row;
        ThermalMaterial material;material.porosity=water.theta_s[i];material.grain_density=p.rho_s;
        material.grain_specific_heat=p.c_s;material.grain_conductivity=p.lambda_s;
        material.water_conductivity=options.water_conductivity;material.air_conductivity=options.air_conductivity;
        material.water_heat_capacity=ctx.options.water_density*ctx.options.water_specific_heat;
        material.geometric=options.thermal_mixing=="GEOMETRIC";
        std::vector<double> edges(options.thermal_layers+1);
        for(int j=0;j<=options.thermal_layers;++j)edges[j]=water.zs[i]*j/options.thermal_layers;
        auto& cell=next.cells[i];std::vector<double> theta;
        if(auto error=projectGroundMoisture(water,i,water.hg[i],edges,theta,&cell.saturated_water);!error.empty())return error;
        double sat_temp=ctx.heat_config.global_temp[int(HeatSource::INITIAL_STATE)],unsat_temp=sat_temp;
        for(int pass=0;pass<3;++pass)for(const auto& row:cfg.initial_quality)
            if(row.species=="__TEMPERATURE__"&&int(row.scope)==pass&&targets(row,mesh,i)) {
                if(row.zone==GwZone::SAT)sat_temp=row.value;
                else if(row.zone==GwZone::UNSAT)unsat_temp=row.value;
            }
        if(auto error=cell.column.initialize(water.area[i],edges,material,theta,std::vector<double>(theta.size(),0));!error.empty())return error;
        cell.reserved_out_water.assign(theta.size(),0);
        const double table_depth=water.zs[i]-water.hg[i];
        for(int j=0;j<options.thermal_layers;++j) {
            const double sat_depth=std::max(0.,edges[j+1]-std::max(edges[j],table_depth));
            const double sat_capacity=water.area[i]*sat_depth*(1-material.porosity)*material.grain_density*material.grain_specific_heat+
                                      cell.saturated_water[j]*material.water_heat_capacity;
            cell.column.energy[j]=sat_capacity*sat_temp+(cell.column.capacity[j]-sat_capacity)*unsat_temp;
        }
        if(tr.n_layers==0)for(int pass=0;pass<3;++pass)for(const auto& row:cfg.initial_quality)
            if(row.species=="__TEMPERATURE__"&&row.zone==GwZone::LAYER&&int(row.scope)==pass&&targets(row,mesh,i)) {
                if(row.layer<1||row.layer>options.thermal_layers)return "Thermal LAYER initial target exceeds THERMAL_LAYERS.";
                const int layer=row.layer-1; // existing file/API LAYER contract is one-based
                cell.column.energy[layer]=cell.column.capacity[layer]*row.value;
            }
        if(tr.n_layers>0) {
            std::vector<double> initial_temperature(tr.n_layers+1,ctx.heat_config.global_temp[int(HeatSource::INITIAL_STATE)]);
            for(int pass=0;pass<3;++pass)for(int layers=0;layers<2;++layers)for(const auto& row:cfg.initial_quality)
                if(row.species=="__TEMPERATURE__"&&int(row.scope)==pass&&targets(row,mesh,i)&&
                   (row.zone==GwZone::LAYER)==bool(layers)) {
                    if(row.zone==GwZone::SAT)initial_temperature[tr.n_layers]=row.value;
                    else for(int layer=0;layer<tr.n_layers;++layer)
                        if(row.zone!=GwZone::LAYER||row.layer==layer+1)initial_temperature[layer]=row.value;
                }
            std::fill(cell.column.energy.begin(),cell.column.energy.end(),0);
            const double dz=table_depth/tr.n_layers;
            for(int j=0;j<options.thermal_layers;++j)for(int layer=0;layer<=tr.n_layers;++layer) {
                const bool saturated=layer==tr.n_layers;
                const double top=saturated?table_depth:dz*layer,bottom=saturated?water.zs[i]:dz*(layer+1);
                const double overlap=std::max(0.,std::min(bottom,edges[j+1])-std::max(top,edges[j]));
                if(!(overlap>0))continue;
                const double moisture=saturated?water.theta_s[i]:water.theta_sigma[std::size_t(layer)*water.n_cells+i];
                const double temperature=initial_temperature[layer];
                cell.column.energy[j]+=overlap*water.area[i]*material.volumetricCapacity(moisture)*temperature;
            }
        }
        cell.column.initial_energy=cell.column.energy;
        cell.initial_j=total(cell.column.energy);cell.geo_flux=p.geo_flux;
    }
    *this=std::move(next);prepare();return {};
}
std::string GroundHeatState::initializeStandalone(SimulationContext& ctx,const MeshData& mesh,const GwTransportData& cfg,
    const std::vector<unsigned char>& owned) {
    clear();GroundHeatState next;next.context=&ctx;next.config=&cfg;
    next.density=ctx.options.water_density;next.water_capacity=ctx.options.water_density*ctx.options.water_specific_heat;
    const auto& o=cfg.options;
    next.top_series=resolveSeries(ctx,o.surface_thermal_bc,o.surface_thermal_arg);
    next.deep_series=resolveSeries(ctx,o.deep_thermal_bc,o.deep_thermal_arg);
    if((o.surface_thermal_bc=="TIMESERIES"||o.surface_thermal_bc=="FLUX_TIMESERIES")&&next.top_series<0)
        return "SURFACE_THERMAL_BC references an unknown project time series.";
    if((o.deep_thermal_bc=="TIMESERIES"||o.deep_thermal_bc=="FLUX_TIMESERIES")&&next.deep_series<0)
        return "DEEP_THERMAL_BC references an unknown project time series.";
    next.cells.resize(mesh.n_cells());
    const double ground=transport::heat::groundTemperature(ctx);
    for(int i=0;i<mesh.n_cells();++i) {
        if(!owned[i])continue;
        const auto& s=transport::heat::sedimentFor(ctx,HeatElement::cell2d(i));
        ThermalMaterial material;material.porosity=s.mesh_porosity;material.grain_density=s.sed_density;
        material.grain_specific_heat=s.sed_specific_heat;
        material.grain_conductivity=s.thermal_diffusivity*s.sed_density*s.sed_specific_heat;
        material.water_conductivity=o.water_conductivity;material.air_conductivity=o.air_conductivity;
        material.water_heat_capacity=next.water_capacity;material.geometric=o.thermal_mixing=="GEOMETRIC";
        // Standalone transient storage has a fixed saturated moisture profile;
        // actual aquifer moisture is used wherever the aquifer owns the soil.
        const double depth=s.bed_thickness+s.ground_depth;
        std::vector<double> edges(o.thermal_layers+1);
        for(int j=0;j<=o.thermal_layers;++j)edges[j]=depth*j/o.thermal_layers;
        auto& cell=next.cells[i];const double initial=s.has_initial_temp?s.initial_temp:transport::heat::groundTempFor(ctx,s,ground);
        if(auto error=cell.column.initialize(mesh.tri_area[i],edges,material,
            std::vector<double>(o.thermal_layers,s.mesh_porosity),std::vector<double>(o.thermal_layers,initial));!error.empty())return error;
        cell.reserved_out_water.assign(o.thermal_layers,0);
        cell.saturated_water.resize(o.thermal_layers);
        for(int j=0;j<o.thermal_layers;++j)cell.saturated_water[j]=mesh.tri_area[i]*(edges[j+1]-edges[j])*s.mesh_porosity;
        cell.initial_j=total(cell.column.energy);cell.surface_owned=true;
    }
    *this=std::move(next);prepare();return {};
}
void GroundHeatState::prepare() {
    if(!active())return;
    const auto& o=config->options;
    top_now=o.surface_thermal_bc=="AIR"?transport::heat::airTempCelsius(*context):
        boundaryValue(*context,o.surface_thermal_bc,o.surface_thermal_arg,top_series);
    deep_now=boundaryValue(*context,o.deep_thermal_bc,o.deep_thermal_arg,deep_series);
    if(!std::isfinite(top_now)||!std::isfinite(deep_now))throw std::runtime_error("Nonfinite ground thermal boundary forcing.");
}
void GroundHeatState::prepareAt(double time) {
    if(!active())return;
    const auto& o=config->options;
    top_now=o.surface_thermal_bc=="AIR"?transport::heat::airTempCelsius(*context):
        boundaryValueAt(*context,o.surface_thermal_bc,o.surface_thermal_arg,top_series,time);
    deep_now=boundaryValueAt(*context,o.deep_thermal_bc,o.deep_thermal_arg,deep_series,time);
    if(!std::isfinite(top_now)||!std::isfinite(deep_now))throw std::runtime_error("Nonfinite ground thermal boundary forcing.");
}
void GroundHeatState::appendRk2Cuts(double start,double end,std::vector<double>& cuts) const {
    if(!active())return;
    const auto append=[&](int series,const std::string& kind) {
        if(series<0)return;
        if(!context||std::size_t(series)>=context->tables.count())throw std::runtime_error("Ground thermal series index is invalid.");
        const auto& table=context->tables[series];const double origin=context->options.start_date;
        if(!std::isfinite(origin)||table.x.empty()||table.x.size()!=table.y.size())throw std::runtime_error("Ground thermal series has no matching time/value rows.");
        for(std::size_t j=0;j<table.x.size();++j) {
            if(!std::isfinite(table.x[j])||!std::isfinite(table.y[j])||(j&&table.x[j]<=table.x[j-1]))
                throw std::runtime_error("Ground thermal series must have finite, strictly increasing times and finite values.");
            const double time=(table.x[j]-origin)*86400.;
            if(!std::isfinite(time))throw std::runtime_error("Ground thermal series elapsed time overflow.");
            if(time>start&&time<end)cuts.push_back(time);
        }
        if(kind=="TIMESERIES"&&(origin+start/86400.<table.x.front()||origin+end/86400.>table.x.back()))
            throw std::runtime_error("Ground temperature series does not cover the coupled interval.");
    };
    append(top_series,config->options.surface_thermal_bc);append(deep_series,config->options.deep_thermal_bc);
}
double GroundHeatState::zoneTemperature(int i,bool saturated) const {
    const auto& cell=cells[i];const auto& c=cell.column;double volume=0,mass=0;
    for(std::size_t j=0;j<c.energy.size();++j) {
        const double v=saturated?cell.saturated_water[j]:c.theta[j]*c.area*(c.depth_edges[j+1]-c.depth_edges[j])-cell.saturated_water[j];
        volume+=v;mass+=v*activeTemperature(i,j);
    }
    return volume>0?mass/volume:c.energy.empty()?0:
        activeTemperature(i,saturated?c.energy.size()-1:0);
}
double GroundHeatState::activeCapacity(int i,std::size_t j) const noexcept {
    const auto& cell=cells[i];
    const double reserved=j<cell.reserved_out_water.size()?cell.reserved_out_water[j]:0;
    return cell.column.capacity[j]-cell.column.material.water_heat_capacity*reserved;
}
double GroundHeatState::activeTemperature(int i,std::size_t j) const noexcept {
    return cells[i].column.energy[j]/activeCapacity(i,j);
}
bool GroundHeatState::snowContactEnabled(int i) const noexcept {
    return i>=0&&std::size_t(i)<cells.size()&&snow_owner&&config&&config->options.conduction&&
        !cells[i].column.energy.empty()&&snow_owner->groundContactEnabled(i);
}
double GroundHeatState::snowContactFraction(int i) const noexcept {
    return snowContactEnabled(i)?std::clamp(snow_owner->groundContactFraction(i),0.,1.):0.;
}
bool GroundHeatState::appendSnowContact(int i,double area,std::vector<ThermalBoundary>& bc) const {
    if(!snowContactEnabled(i)||!(area>0)||!std::isfinite(area))return false;
    const auto& c=cells[i].column;const auto& o=config->options;
    if(c.depth_edges.size()<2||c.conductivity.empty())throw std::runtime_error("UEB/soil contact has no top soil layer.");
    const double resistance=.5*(c.depth_edges[1]-c.depth_edges[0])/c.conductivity[0]+o.surface_resistance;
    const double temperature=snow_owner->groundContactSurfaceTemperature(i);
    if(!(resistance>0)||!std::isfinite(resistance)||!std::isfinite(temperature))throw std::runtime_error("UEB/soil contact has invalid resistance or temperature.");
    const double conductance=area/resistance;
    if(!(conductance>0)||!std::isfinite(conductance))throw std::runtime_error("UEB/soil contact conductance is invalid.");
    bc.push_back({0,conductance,temperature,0});
    return true;
}
std::string GroundHeatState::applySnowContactEnergy(int i,double soil_j) {
    if(!snowContactEnabled(i))return "Reciprocal UEB/soil contact was committed without matching contact ownership.";
    return snow_owner->applyGroundContactEnergy(i,soil_j);
}
void GroundHeatState::refreshView(int i,SubsurfaceTransportState& tr) {
    if(!active()||tr.temp_row<0)return;
    auto& cell=cells[i];const auto& c=cell.column;double sat=0,unsat=0;
    if(auto error=cell.porewater.publishSigma(tr,i);!error.empty())throw std::runtime_error(error);
    for(std::size_t j=0;j<c.energy.size();++j) {
        const double t=activeTemperature(i,j),vw=c.theta[j]*c.area*(c.depth_edges[j+1]-c.depth_edges[j]);
        sat+=cell.saturated_water[j]*t;unsat+=(vw-cell.saturated_water[j])*t;
    }
    tr.sat_mass[tr.idx(tr.temp_row,i)]=sat;tr.unsat_mass[tr.idx(tr.temp_row,i)]=unsat;
    if(tr.n_layers>0 && cell.porewater.sigma.active()) {
        for(int layer=0;layer<tr.n_layers;++layer)tr.layer_mass[tr.layerIdx(tr.temp_row,layer,i)]=0;
        for(const auto& o:cell.porewater.sigma.overlaps)if(o.reactor<tr.n_layers)
            tr.layer_mass[tr.layerIdx(tr.temp_row,o.reactor,i)]+=o.water*activeTemperature(i,o.physical);
        tr.refreshUnsat(tr.temp_row,i);
    }
    if(tr.n_layers==0 && cell.porewater.active())for(int r=0;r<tr.n_species;++r)if(r!=tr.temp_row) {
        const auto pair=cell.porewater.zoneMass(c,cell.saturated_water,r);
        tr.sat_mass[tr.idx(r,i)]=pair.first;tr.unsat_mass[tr.idx(r,i)]=pair.second;
    }
}
std::size_t GroundHeatState::boundaries(int i,double exposed,std::vector<ThermalBoundary>& bc) const {
    const auto& cell=cells[i];const auto& c=cell.column;const auto& o=config->options;
    if(c.energy.empty()||!o.conduction)return 0;
    const int bottom=int(c.energy.size()-1);
    const auto g=[&](int j,double resistance){return c.area/(.5*(c.depth_edges[j+1]-c.depth_edges[j])/c.conductivity[j]+resistance);};
    const auto& top=o.surface_thermal_bc;
    if(exposed>0&&(top=="AIR"||top=="FIXED"||top=="TIMESERIES"))bc.push_back({0,exposed*g(0,top=="AIR"?o.surface_resistance:0),top_now,0});
    else if(exposed>0&&(top=="HEAT_FLUX"||top=="FLUX_TIMESERIES"))bc.push_back({0,0,0,exposed*c.area*top_now});
    const auto top_count=bc.size();
    if(o.deep_thermal_bc=="GEOTHERMAL_FLUX"||o.deep_thermal_bc=="FLUX_TIMESERIES")
        bc.push_back({bottom,0,0,c.area*(o.deep_thermal_arg.empty()?cell.geo_flux:deep_now)});
    else if(o.deep_thermal_bc=="FIXED_TEMP"||o.deep_thermal_bc=="TIMESERIES")
        bc.push_back({bottom,g(bottom,o.deep_depth/c.conductivity[bottom]),deep_now,0});
    return top_count;
}
std::string GroundHeatState::conduct(int i,double dt,bool midpoint) {
    auto& cell=cells[i];auto& c=cell.column;const auto& o=config->options;
    if(c.energy.empty()||!o.conduction||cell.surface_owned)return {};
    if(o.surface_thermal_bc=="SURFACE_WATER")return "SURFACE_WATER thermal boundary requires a reciprocal mesh ground-column contact.";
    std::vector<ThermalBoundary> bc;const double snow_fraction=snowContactFraction(i);
    const auto top_count=boundaries(i,1-snow_fraction,bc);
    const int snow_boundary=appendSnowContact(i,c.area*snow_fraction,bc)?int(bc.size()-1):-1;
    auto stores=c.stores();
    for(std::size_t j=0;j<stores.size();++j)stores[j].capacity=activeCapacity(i,j);
    auto solved=midpoint?solveThermalContactsRk2(stores,c.contacts(),bc,dt):
        solveThermalContacts(stores,c.contacts(),bc,dt);
    if(!solved.error.empty())return solved.error;
    for(std::size_t b=0;b<solved.boundary_j.size();++b) {
        const double receipt=solved.boundary_j[b];
        if(int(b)==snow_boundary) {
            cell.surface_j+=receipt;
            if(auto error=applySnowContactEnergy(i,receipt);!error.empty())return error;
        } else if(b<top_count)cell.surface_j+=receipt;else cell.deep_j+=receipt;
    }
    for(double energy:solved.contact_j)cell.vertical_absolute_j+=std::abs(energy);
    c.energy=std::move(solved.energy);return {};
}
} // namespace openswmm::twoD
