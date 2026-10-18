// SPDX-License-Identifier: Apache-2.0
#include "openswmm_api_common.hpp"
#include <openswmm/engine/openswmm_heat.h>
#ifdef OPENSWMM_HAS_2D
#include "../2d/SurfaceRouter2D.hpp"
#include "../2d/mesh/MeshBuilder.hpp"
#include "../2d/subsurface/SubsurfaceSections.hpp"
#include "../transport/components/HeatFluxModules/HeatOverrides.hpp"
#include "../hydraulics/XSectBatch.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <vector>
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
    return energy-c.initial_j-c.advective_j-c.source_j-c.surface_j-c.deep_j-c.latent_j-c.lateral_j-c.water_contact_j-c.channel_contact_j-c.lid_contact_j;
}
int previewChannelGroundMap(const openswmm::SimulationContext& ctx,
    const openswmm::twoD::SurfaceRouter2D& router,int link,
    SWMM_ChannelGroundPreview& result,std::vector<SWMM_ChannelGroundContact>& rows) {
    result={};result.struct_size=sizeof result;result.link=link;rows.clear();
    const auto reason=[&](const std::string& text){std::strncpy(result.reason,text.c_str(),sizeof(result.reason)-1);};
    if(ctx.state!=openswmm::EngineState::OPENED&&ctx.state!=openswmm::EngineState::BUILDING)
        return SWMM_ERR_LIFECYCLE;
    if(link<0||link>=ctx.n_links())return SWMM_ERR_BADINDEX;
    const auto& source_mesh=router.mesh();
    if(source_mesh.n_cells()<=0){reason("The model has no parsed 2D mesh cells.");return SWMM_OK;}
    if(std::size_t(link)>=ctx.links.xsect_batch_shape.size()||ctx.link_subtypes.conduit_row(link)<0||
       !openswmm::xsect::isOpen(ctx.links.xsect_batch_shape[link])) {
        reason("MESH_CONTACTS requires an open conduit; this link is not eligible.");return SWMM_OK;
    }
    const int n1=ctx.links.node1[link],n2=ctx.links.node2[link];
    const auto hasPoint=[&](int n){return n>=0&&std::size_t(n)<ctx.spatial.node_has_xy.size()&&ctx.spatial.node_has_xy[n]&&
        std::size_t(n)<ctx.spatial.node_x.size()&&std::size_t(n)<ctx.spatial.node_y.size();};
    if(!hasPoint(n1)||!hasPoint(n2)){reason("Add coordinates for both channel endpoints before previewing mesh contacts.");return SWMM_OK;}
    if(std::size_t(link)>=ctx.spatial.link_vertices_x.size()||std::size_t(link)>=ctx.spatial.link_vertices_y.size()) {
        reason("Channel polyline coordinate arrays are unavailable.");return SWMM_OK;
    }
    const auto& vx=ctx.spatial.link_vertices_x[link];const auto& vy=ctx.spatial.link_vertices_y[link];
    if(vx.size()!=vy.size()){reason("Channel polyline coordinate arrays differ.");return SWMM_OK;}
    const int us=openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    const double coordinate_scale=us==0?.3048:1.0;
    auto mesh=source_mesh;
    const double mesh_scale=!router.options().mesh_units_si&&us==0?.3048:1.0;
    for(double& x:mesh.vx)x*=mesh_scale;
    for(double& y:mesh.vy)y*=mesh_scale;
    for(double& z:mesh.vz)z*=mesh_scale;
    const auto nc=static_cast<std::size_t>(mesh.n_cells());
    if(mesh.cell_nv.size()!=nc||mesh.cell_v.size()!=nc*openswmm::twoD::kMaxCellVerts||
       mesh.vx.size()!=mesh.vy.size()||mesh.vx.size()!=mesh.vz.size()) {
        reason("The parsed mesh connectivity is incomplete; initialize it to validate the mesh.");return SWMM_OK;
    }
    for(std::size_t i=0;i<mesh.vx.size();++i)if(!std::isfinite(mesh.vx[i])||!std::isfinite(mesh.vy[i])||!std::isfinite(mesh.vz[i])) {
        reason("The parsed mesh contains a nonfinite vertex coordinate.");return SWMM_OK;
    }
    for(std::size_t c=0;c<nc;++c) {
        if(mesh.cell_nv[c]<3||mesh.cell_nv[c]>openswmm::twoD::kMaxCellVerts) {
            reason("The parsed mesh contains a cell with invalid connectivity.");return SWMM_OK;
        }
        for(int k=0;k<mesh.cell_nv[c];++k) {
            const int vertex=mesh.cell_v[c*openswmm::twoD::kMaxCellVerts+std::size_t(k)];
            if(vertex<0||std::size_t(vertex)>=mesh.vx.size()) {
                reason("The parsed mesh contains an out-of-range vertex index.");return SWMM_OK;
            }
        }
    }
    openswmm::twoD::buildMeshTopology(mesh);
    if(const auto error=openswmm::twoD::validateMesh(mesh);!error.empty()) {
        reason("The parsed mesh cannot be previewed: "+error);return SWMM_OK;
    }
    std::vector<double> x={ctx.spatial.node_x[n1]*coordinate_scale};
    std::vector<double> y={ctx.spatial.node_y[n1]*coordinate_scale};
    for(std::size_t p=0;p<vx.size();++p){x.push_back(vx[p]*coordinate_scale);y.push_back(vy[p]*coordinate_scale);}
    x.push_back(ctx.spatial.node_x[n2]*coordinate_scale);y.push_back(ctx.spatial.node_y[n2]*coordinate_scale);
    const auto mapping=openswmm::twoD::mapMeshPolyline(mesh,x,y);
    if(!mapping.error.empty()){reason(mapping.error);return SWMM_OK;}
    result.length_m=mapping.length;result.covered_length_m=mapping.covered_length;
    result.uncovered_length_m=std::max(0.0,mapping.length-mapping.covered_length);
    std::set<int> cells;
    const double z0=(ctx.nodes.invert_elev[n1]+ctx.links.offset1[link])*.3048;
    const double z1=(ctx.nodes.invert_elev[n2]+ctx.links.offset2[link])*.3048;
    for(const auto& interval:mapping.intervals) {
        cells.insert(interval.cell);
        const double mid=(interval.begin+interval.end)*.5;
        const double fraction=mapping.length>0?mid/mapping.length:0;
        SWMM_ChannelGroundContact contact{};contact.struct_size=sizeof contact;
        contact.link=link;contact.cell=interval.cell;contact.layer=-1;
        contact.begin_fraction=interval.begin/mapping.length;contact.end_fraction=interval.end/mapping.length;
        contact.share=interval.share;contact.invert_m=z0+(z1-z0)*fraction;
        rows.push_back(contact);
    }
    if(rows.size()>std::size_t(std::numeric_limits<int>::max())||cells.size()>std::size_t(std::numeric_limits<int>::max()))
        return SWMM_ERR_BADPARAM;
    result.contact_intervals=static_cast<int>(rows.size());result.contact_cells=static_cast<int>(cells.size());
    result.valid=result.uncovered_length_m<=1e-9*std::max(1.0,result.length_m);
    reason(result.valid?"Complete XY coverage. Groundwater profile and bed-fit checks still run at initialization.":
        "Part of this channel lies outside the mesh; initialization will refuse MESH_CONTACTS until coverage is complete.");
    return SWMM_OK;
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
        "Experimental reciprocal channel/soil heat is active; bed tracer/age and broader snow/footprint qualification remain open.";
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

int swmm_heat_preview_channel_ground_map(SWMM_Engine engine,int link,SWMM_ChannelGroundPreview* out,
    SWMM_ChannelGroundContact* contacts,int capacity,int* required) {
    CHECK_HANDLE(engine);
    if(!out||out->struct_size<sizeof(SWMM_ChannelGroundPreview)||!required||capacity<0||
       (capacity>0&&!contacts))return SWMM_ERR_BADPARAM;
#ifdef OPENSWMM_HAS_2D
    SWMM_ChannelGroundPreview result{};std::vector<SWMM_ChannelGroundContact> rows;
    const int rc=previewChannelGroundMap(to_engine(engine)->context(),to_engine(engine)->surfaceRouter2D(),link,result,rows);
    if(rc!=SWMM_OK)return rc;
    if(rows.size()>std::size_t(std::numeric_limits<int>::max()))return SWMM_ERR_BADPARAM;
    *required=static_cast<int>(rows.size());*out=result;
    if(capacity==0&&!contacts)return SWMM_OK;
    if(capacity<*required)return SWMM_ERR_BADPARAM;
    if(!rows.empty())std::copy(rows.begin(),rows.end(),contacts);
    return SWMM_OK;
#else
    (void)link;(void)contacts;(void)capacity;return SWMM_ERR_PLUGIN;
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
        "Experimental coupled 2D heat is active; broader snow ownership and native hotstart qualification remain open.";
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
        "Experimental finite soil heat is active; native hotstarts and unsupported contact/transport combinations remain unavailable.";
    std::strncpy(status.reason,reason,sizeof status.reason-1);
#else
    std::strcpy(status.reason,"This engine was built without 2D routing.");
#endif
    *out=status;return SWMM_OK;
}
int swmm_heat_get_ground_values(SWMM_Engine engine,int kind,int first,int count,double* out) {
    CHECK_HANDLE(engine);
    if(kind<0||kind>SWMM_GROUND_HEAT_LID_CONTACT||first<0||count<0||(count&&!out))return SWMM_ERR_BADPARAM;
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
            case SWMM_GROUND_HEAT_LID_CONTACT:out[k]=c.lid_contact_j;break;
            default:{
                double scale=capacity+std::abs(c.initial_j);
                for(double v:{c.advective_j,c.source_j,c.surface_j,c.deep_j,c.latent_j,c.lateral_j,c.water_contact_j,c.channel_contact_j,c.lid_contact_j})scale+=std::abs(v);
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
