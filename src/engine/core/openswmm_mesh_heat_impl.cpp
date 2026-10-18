// SPDX-License-Identifier: Apache-2.0
#include "openswmm_api_common.hpp"
#include <openswmm/engine/openswmm_heat.h>
#ifdef OPENSWMM_HAS_2D
#include "../2d/SurfaceRouter2D.hpp"
#include "../transport/components/HeatFluxModules/HeatOverrides.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
namespace {
bool thermalResultsReady(const openswmm::SimulationContext& ctx,const openswmm::twoD::SurfaceRouter2D& router) {
    return router.isActive() || ctx.state==openswmm::EngineState::ENDED || ctx.state==openswmm::EngineState::REPORTED;
}
const openswmm::twoD::GroundHeatState* groundOwner(const openswmm::twoD::SurfaceRouter2D& router,int i) {
    const auto& h=router.state().heat;
    const auto& owner=h.ground_column?h.groundOwner(i):router.subsurface().heat();
    return std::size_t(i)<owner.cells.size()&&!owner.cells[i].column.energy.empty()?&owner:nullptr;
}
const openswmm::twoD::GroundThermalCell* groundCell(const openswmm::twoD::SurfaceRouter2D& router,int i) {
    const auto* owner=groundOwner(router,i);return owner?&owner->cells[i]:nullptr;
}
double soilResidual(const openswmm::twoD::GroundThermalCell& c,double energy) {
    return energy-c.initial_j-c.advective_j-c.source_j-c.surface_j-c.deep_j-c.latent_j-c.lateral_j-c.water_contact_j-c.channel_contact_j;
}
}
#endif

int swmm_heat_get_channel_ground_status(SWMM_Engine engine,SWMM_ChannelGroundStatus* out) {
    CHECK_HANDLE(engine);if(!out||out->struct_size<sizeof(SWMM_ChannelGroundStatus))return SWMM_ERR_BADPARAM;
    SWMM_ChannelGroundStatus status{};status.struct_size=sizeof status;
#ifdef OPENSWMM_HAS_2D
    const auto& ctx=to_engine(engine)->context();const auto& router=to_engine(engine)->surfaceRouter2D();
    const auto& contacts=router.channelGround();
    for(int link=0;link<ctx.n_links();++link) {
        status.requested|=openswmm::transport::heat::sedimentFor(ctx,openswmm::HeatElement::link(link)).mesh_ground;
        status.mapped_links+=contacts.ownsLink(link);
    }
    status.initialized=thermalResultsReady(ctx,router);status.experimental=status.requested;
    status.contact_cells=int(contacts.cells().size());status.contact_intervals=int(contacts.contacts().size());
    status.exchange_active=router.isActive()&&contacts.active()&&ctx.options.heat_transport&&ctx.heat_config.sediment_exchange;
    const char* reason=!status.requested?"Channels use their prescribed standalone ground configuration.":
        !ctx.options.heat_transport?"Saved mesh contacts; inactive because Heat transport is off.":
        !ctx.heat_config.sediment_exchange?"Saved mesh contacts; sediment exchange is off.":
        !status.initialized?"Requested; initialize to validate geometry and measure contact counts.":
        !contacts.active()?"No eligible open channel has an initialized mesh contact.":
        !router.isActive()?"Completed contact results are available; the simulation is no longer running.":
        "Experimental reciprocal channel/soil heat is active; tracer, snow/LID and restart qualification remain open.";
    std::strncpy(status.reason,reason,sizeof status.reason-1);
#else
    std::strcpy(status.reason,"This engine was built without 2D routing.");
#endif
    *out=status;return SWMM_OK;
}

int swmm_heat_get_channel_ground_values(SWMM_Engine engine,int kind,int first,int count,double* out) {
    CHECK_HANDLE(engine);
    if(kind<0||kind>SWMM_CHANNEL_GROUND_INTERVAL_COUNT||first<0||count<0||(count&&!out))return SWMM_ERR_BADPARAM;
#ifdef OPENSWMM_HAS_2D
    const auto& ctx=to_engine(engine)->context();const auto& router=to_engine(engine)->surfaceRouter2D();
    if(first>ctx.n_links()||count>ctx.n_links()-first)return SWMM_ERR_BADINDEX;
    const auto& contacts=router.channelGround();
    if(!thermalResultsReady(ctx,router)||!contacts.active())return SWMM_ERR_LIFECYCLE;
    for(int k=0;k<count;++k) {
        const int link=first+k;
        if(!contacts.ownsLink(link)){out[k]=std::numeric_limits<double>::quiet_NaN();continue;}
        switch(kind) {
            case SWMM_CHANNEL_GROUND_BED_TEMPERATURE:out[k]=contacts.bedTemperature(link);break;
            case SWMM_CHANNEL_GROUND_ATMOSPHERIC_ENERGY:out[k]=contacts.channelAtmosphere()[link];break;
            case SWMM_CHANNEL_GROUND_TO_SOIL_ENERGY:out[k]=contacts.channelToGround()[link];break;
            case SWMM_CHANNEL_GROUND_WATER_INCREMENT:out[k]=contacts.channelThermalIncrement()[link];break;
            case SWMM_CHANNEL_GROUND_INTERVAL_COUNT:out[k]=double(contacts.contactCount(link));break;
        }
    }
    return SWMM_OK;
#else
    return SWMM_ERR_PLUGIN;
#endif
}

int swmm_heat_get_channel_ground_contact(SWMM_Engine engine,int index,SWMM_ChannelGroundContact* out) {
    CHECK_HANDLE(engine);if(!out||out->struct_size<sizeof(SWMM_ChannelGroundContact))return SWMM_ERR_BADPARAM;
#ifdef OPENSWMM_HAS_2D
    const auto& router=to_engine(engine)->surfaceRouter2D();
    if(!thermalResultsReady(to_engine(engine)->context(),router))return SWMM_ERR_LIFECYCLE;
    const auto& contacts=router.channelGround().contacts();
    if(index<0||std::size_t(index)>=contacts.size())return SWMM_ERR_BADINDEX;
    const auto& c=contacts[index];SWMM_ChannelGroundContact result{};result.struct_size=sizeof result;
    result.link=c.link;result.cell=c.cell;result.layer=c.layer;
    result.begin_fraction=c.begin;result.end_fraction=c.end;result.share=c.share;result.invert_m=c.invert;
    *out=result;return SWMM_OK;
#else
    return SWMM_ERR_PLUGIN;
#endif
}

int swmm_heat_get_2d_status(SWMM_Engine engine,SWMM_Heat2DStatus* out) {
    CHECK_HANDLE(engine);
    if(!out || out->struct_size<sizeof(SWMM_Heat2DStatus))return SWMM_ERR_BADPARAM;
    SWMM_Heat2DStatus status{};status.struct_size=sizeof status;
#ifdef OPENSWMM_HAS_2D
    const auto& ctx=to_engine(engine)->context();
    const auto& router=to_engine(engine)->surfaceRouter2D();
    const auto& options=router.options();const auto& heat=router.state().heat;
    status.requested=options.coupled_heat;status.initialized=thermalResultsReady(ctx,router);
    status.surface_active=status.initialized&&heat.enabled&&heat.surface;
    status.radiative_active=status.initialized&&heat.enabled&&heat.radiative;
    status.bed_active=status.initialized&&heat.enabled&&heat.bed;
    status.bed_model=options.bed_heat_model;status.experimental=options.coupled_heat?1:0;
    status.water_heat_capacity=options.water_heat_capacity;
    const char* reason=!options.coupled_heat?"Transport only; coupled 2D heat is off.":
        !ctx.options.heat_transport?"Saved; inactive because Heat transport is off.":
        !options.transport_temperature?"Saved; inactive because mesh temperature transport is off.":
        !status.initialized?"Requested; initialize a model with an enabled surface mesh to measure activation.":
        !heat.enabled?"Saved; inactive because no mesh temperature row is available.":
        !heat.surface&&!heat.radiative&&!heat.bed?"Coupled model is initialized; all mesh heat exchanges are off.":
        "Experimental coupled 2D heat is active; restart and coupled snow/LID owners are not yet qualified.";
    std::strncpy(status.reason,reason,sizeof status.reason-1);
#else
    std::strcpy(status.reason,"This engine was built without 2D routing.");
#endif
    *out=status;return SWMM_OK;
}

int swmm_heat_get_2d_values(SWMM_Engine engine,int kind,int first,int count,double* out) {
    CHECK_HANDLE(engine);
    if(kind<0 || kind>SWMM_HEAT_2D_THERMAL_RESIDUAL || first<0 || count<0 || (count && !out))return SWMM_ERR_BADPARAM;
#ifdef OPENSWMM_HAS_2D
    const auto& router=to_engine(engine)->surfaceRouter2D();
    const auto& state=router.state();const auto& h=state.heat;
    const auto n=router.mesh().n_cells();
    if(first>n || count>n-first)return SWMM_ERR_BADINDEX;
    if(!thermalResultsReady(to_engine(engine)->context(),router)||!h.enabled)return SWMM_ERR_LIFECYCLE;
    if(kind>=SWMM_HEAT_2D_BED_TEMPERATURE && kind<=SWMM_HEAT_2D_BED_CAPACITY && !h.bed)return SWMM_ERR_LIFECYCLE;
    const std::vector<double>* values=nullptr;
    switch(kind) {
        case SWMM_HEAT_2D_BED_ENERGY:values=&h.bed_energy;break;
        case SWMM_HEAT_2D_BED_CAPACITY:values=&h.bed_capacity;break;
        case SWMM_HEAT_2D_ATMOSPHERIC_ENERGY:values=&h.atmospheric_j;break;
        case SWMM_HEAT_2D_RADIATIVE_ENERGY:values=&h.radiative_j;break;
        case SWMM_HEAT_2D_SENSIBLE_ENERGY:values=&h.sensible_j;break;
        case SWMM_HEAT_2D_LATENT_ENERGY:values=&h.latent_j;break;
        case SWMM_HEAT_2D_DEEP_ENERGY:values=&h.ground_j;break;
        case SWMM_HEAT_2D_WATER_TO_BED_ENERGY:values=&h.water_bed_j;break;
        case SWMM_HEAT_2D_WATER_THERMAL_INCREMENT:values=&h.water_j;break;
        case SWMM_HEAT_2D_ACCEPTED_EVAPORATION:values=&h.evaporated_m3;break;
    }
    for(int k=0;k<count;++k) {
        const auto i=std::size_t(first+k);
        if(h.ground_column&&((kind>=SWMM_HEAT_2D_BED_TEMPERATURE&&kind<=SWMM_HEAT_2D_BED_CAPACITY)||kind==SWMM_HEAT_2D_THERMAL_RESIDUAL)) {
            const auto& cell=h.groundOwner(int(i)).cells[i];const auto& c=cell.column;
            double energy=0,capacity=0,total=0;
            const auto& sd=openswmm::transport::heat::sedimentFor(to_engine(engine)->context(),openswmm::HeatElement::cell2d(int(i)));
            for(std::size_t j=0;j<c.energy.size();++j) {
                total+=c.energy[j];const double f=std::clamp((sd.bed_thickness-c.depth_edges[j])/(c.depth_edges[j+1]-c.depth_edges[j]),0.,1.);
                energy+=f*c.energy[j];capacity+=f*c.capacity[j];
            }
            if(kind==SWMM_HEAT_2D_THERMAL_RESIDUAL)
                out[k]=soilResidual(cell,total)+
                    h.water_j[i]+h.water_bed_j[i]-h.atmospheric_j[i];
            else out[k]=kind==SWMM_HEAT_2D_BED_ENERGY?energy:kind==SWMM_HEAT_2D_BED_CAPACITY?capacity:energy/capacity;
            continue;
        }
        if(values)out[k]=(*values)[i];
        else if(kind==SWMM_HEAT_2D_BED_TEMPERATURE)out[k]=h.bed_energy[i]/h.bed_capacity[i];
        else if(kind==SWMM_HEAT_2D_WATER_TEMPERATURE)
            out[k]=state.volume[i]>0?state.transport.cell_mass[state.transport.idx(state.transport.temp_row,int(i))]/state.volume[i]:h.reported_temperature[i];
        else out[k]=(h.bed?h.bed_energy[i]-h.initial_bed_energy[i]:0)+h.water_j[i]-h.atmospheric_j[i]-h.ground_j[i];
    }
    return SWMM_OK;
#else
    return SWMM_ERR_PLUGIN;
#endif
}

int swmm_heat_get_ground_status(SWMM_Engine engine,SWMM_GroundHeatStatus* out) {
    CHECK_HANDLE(engine);if(!out||out->struct_size<sizeof(SWMM_GroundHeatStatus))return SWMM_ERR_BADPARAM;
    SWMM_GroundHeatStatus status{};status.struct_size=sizeof status;
#ifdef OPENSWMM_HAS_2D
    const auto& ctx=to_engine(engine)->context();const auto& router=to_engine(engine)->surfaceRouter2D();
    const auto& h=router.state().heat;const auto& cfg=router.gwTransport().options;
    status.requested=cfg.thermal_model=="POROUS_EQUILIBRIUM"||router.options().bed_heat_model==2;
    status.initialized=thermalResultsReady(ctx,router);status.thermal_layers=cfg.thermal_layers;
    status.experimental=status.requested;status.water_heat_capacity=router.options().water_heat_capacity;
    if(status.initialized)for(int i=0;i<router.mesh().n_cells();++i)if(groundCell(router,i)) {
        if(h.ground_column&&!h.aquifer_owned[i])++status.standalone_cells;else ++status.aquifer_cells;
    }
    const bool active=status.aquifer_cells+status.standalone_cells>0;
    status.conduction_active=active&&cfg.conduction;status.lateral_active=status.aquifer_cells>0&&status.conduction_active&&cfg.lateral_conduction;
    const char* reason=!status.requested?"Water-only model; porous ground heat is off.":
        !ctx.options.heat_transport?"Saved; inactive because Heat transport is off.":
        !status.initialized?"Requested; initialize to measure thermal storage and activation.":
        !active?"Saved; no active ground thermal owner or temperature row.":
        "Experimental finite soil heat is active; restart and other-owner contacts remain under qualification.";
    std::strncpy(status.reason,reason,sizeof status.reason-1);
#else
    std::strcpy(status.reason,"This engine was built without 2D routing.");
#endif
    *out=status;return SWMM_OK;
}
int swmm_heat_get_ground_values(SWMM_Engine engine,int kind,int first,int count,double* out) {
    CHECK_HANDLE(engine);
    if(kind<0||kind>SWMM_GROUND_HEAT_CHANNEL_CONTACT||first<0||count<0||(count&&!out))return SWMM_ERR_BADPARAM;
#ifdef OPENSWMM_HAS_2D
    const auto& router=to_engine(engine)->surfaceRouter2D();const int n=router.mesh().n_cells();
    if(first>n||count>n-first)return SWMM_ERR_BADINDEX;
    if(!thermalResultsReady(to_engine(engine)->context(),router)||
       (!router.state().heat.ground_column&&!router.subsurface().heat().active()))return SWMM_ERR_LIFECYCLE;
    for(int k=0;k<count;++k) {
        const auto* cell=groundCell(router,first+k);
        if(!cell){out[k]=std::numeric_limits<double>::quiet_NaN();continue;}
        const auto& c=*cell;const auto& column=c.column;
        const double energy=std::accumulate(column.energy.begin(),column.energy.end(),0.);
        const auto* owner=groundOwner(router,first+k);
        double capacity=0;for(std::size_t j=0;j<column.energy.size();++j)capacity+=owner->activeCapacity(first+k,j);
        switch(kind) {
            case SWMM_GROUND_HEAT_ENERGY:out[k]=energy;break;
            case SWMM_GROUND_HEAT_CAPACITY:out[k]=capacity;break;
            case SWMM_GROUND_HEAT_TEMPERATURE:out[k]=energy/capacity;break;
            case SWMM_GROUND_HEAT_INITIAL_ENERGY:out[k]=c.initial_j;break;
            case SWMM_GROUND_HEAT_ADVECTIVE_ENERGY:out[k]=c.advective_j;break;
            case SWMM_GROUND_HEAT_SOURCE_ENERGY:out[k]=c.source_j;break;
            case SWMM_GROUND_HEAT_SURFACE_ENERGY:out[k]=c.surface_j;break;
            case SWMM_GROUND_HEAT_DEEP_ENERGY:out[k]=c.deep_j;break;
            case SWMM_GROUND_HEAT_LATENT_ENERGY:out[k]=c.latent_j;break;
            case SWMM_GROUND_HEAT_LATERAL_ENERGY:out[k]=c.lateral_j;break;
            case SWMM_GROUND_HEAT_WATER_CONTACT_ENERGY:out[k]=c.water_contact_j;break;
            case SWMM_GROUND_HEAT_RESIDUAL:out[k]=soilResidual(c,energy);break;
            case SWMM_GROUND_HEAT_PENDING_LATERAL:out[k]=router.subsurface().pendingHeat(first+k);break;
            case SWMM_GROUND_HEAT_CHANNEL_CONTACT:out[k]=c.channel_contact_j;break;
            default:{
                double scale=capacity+std::abs(c.initial_j);
                for(double v:{c.advective_j,c.source_j,c.surface_j,c.deep_j,c.latent_j,c.lateral_j,c.water_contact_j,c.channel_contact_j})scale+=std::abs(v);
                for(double v:column.energy)scale+=std::abs(v);
                out[k]=soilResidual(c,energy)/std::max(1.,scale);break;
            }
        }
    }
    return SWMM_OK;
#else
    return SWMM_ERR_PLUGIN;
#endif
}
int swmm_heat_get_ground_layers(SWMM_Engine engine,int cell,int kind,int first,int count,double* out) {
    CHECK_HANDLE(engine);
    if(kind<0||kind>SWMM_GROUND_LAYER_SATURATED_WATER||first<0||count<0||(count&&!out))return SWMM_ERR_BADPARAM;
#ifdef OPENSWMM_HAS_2D
    const auto& router=to_engine(engine)->surfaceRouter2D();
    if(cell<0||cell>=router.mesh().n_cells())return SWMM_ERR_BADINDEX;
    if(!thermalResultsReady(to_engine(engine)->context(),router))return SWMM_ERR_LIFECYCLE;
    const auto* c=groundCell(router,cell);if(!c)return SWMM_ERR_LIFECYCLE;
    const auto* owner=groundOwner(router,cell);
    const int n=int(c->column.energy.size());if(first>n||count>n-first)return SWMM_ERR_BADINDEX;
    const auto& column=c->column;
    for(int k=0;k<count;++k) {
        const auto j=std::size_t(first+k);
        switch(kind) {
            case SWMM_GROUND_LAYER_TOP:out[k]=column.depth_edges[j];break;
            case SWMM_GROUND_LAYER_BOTTOM:out[k]=column.depth_edges[j+1];break;
            case SWMM_GROUND_LAYER_TEMPERATURE:out[k]=column.energy[j]/owner->activeCapacity(cell,j);break;
            case SWMM_GROUND_LAYER_ENERGY:out[k]=column.energy[j];break;
            case SWMM_GROUND_LAYER_CAPACITY:out[k]=owner->activeCapacity(cell,j);break;
            case SWMM_GROUND_LAYER_MOISTURE:out[k]=column.theta[j];break;
            case SWMM_GROUND_LAYER_CONDUCTIVITY:out[k]=column.conductivity[j];break;
            default:out[k]=c->saturated_water[j];break;
        }
    }
    return SWMM_OK;
#else
    return SWMM_ERR_PLUGIN;
#endif
}
