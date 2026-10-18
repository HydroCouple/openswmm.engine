// SPDX-License-Identifier: Apache-2.0
#include "ChannelGround.hpp"
#include "../subsurface/SubsurfaceSections.hpp"
#include "../subsurface/SubsurfaceTransportState.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../gw/GwTransportData.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../hydraulics/XSectBatch.hpp"
#include "../../transport/components/HeatFluxModules/HeatOverrides.hpp"
#include "../../transport/components/HeatFluxModules/HeatFluxes.hpp"
#include "../../transport/components/HeatFluxModules/SurfaceExchange.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
namespace openswmm::twoD {
namespace {
using namespace transport::heat;
bool explicitlySelected(const SimulationContext& ctx,int link) {
    const auto& name=ctx.link_names.name_of(link);
    const auto& tag=ctx.links.tags[link];
    for(const auto& row:ctx.heat_config.overrides.rows)
        if(row.attr==HeatAttr::SED_GROUND_SOURCE&&row.value==1&&
           ((row.scope==HeatScope::LINK&&row.name==name)||(row.scope==HeatScope::TAG&&row.name==tag)))return true;
    return false;
}
}
std::string ChannelGroundState::initialize(SimulationContext& ctx,const MeshData& mesh,GroundHeatState& soil,
    SubsurfaceTransportState& transport,SurfaceStateData& surface,double coordinate_scale) {
    clear();ctx.heat_state.mesh_contact_link.clear();
    if(!ctx.options.heat_transport||!ctx.heat_config.sediment_exchange)return {};
    bool selected=false;for(int link=0;link<ctx.n_links();++link)if(sedimentFor(ctx,HeatElement::link(link)).mesh_ground)selected=true;
    if(!selected)return {};
    if(ctx.n_pollutants()>0||ctx.options.water_age||transport.n_msx>0)
        return "MESH_CONTACTS channel bed tracer/age ownership is awaiting conservative porewater partition qualification.";
    ChannelGroundState next;next.context_=&ctx;next.mesh_=&mesh;next.ground_=&soil;next.transport_=&transport;next.surface_=&surface;
    next.cell_owned_.assign(mesh.n_cells(),0);next.link_owned_.assign(ctx.n_links(),0);
    for(int link=0;link<ctx.n_links();++link) {
        const auto& material=sedimentFor(ctx,HeatElement::link(link));if(!material.mesh_ground)continue;
        const auto& name=ctx.link_names.name_of(link);
        if(ctx.link_subtypes.conduit_row(link)<0||!xsect::isOpen(ctx.links.xsect_batch_shape[link])) {
            if(explicitlySelected(ctx,link))return "Channel '"+name+"': MESH_CONTACTS requires an open conduit; use PRESCRIBED for this element.";
            continue;
        }
        if(!soil.active())return "Channel '"+name+"': MESH_CONTACTS requires active POROUS_EQUILIBRIUM groundwater heat and a spatial aquifer.";
        if(!(coordinate_scale>0)||!std::isfinite(coordinate_scale))return "Channel contact coordinate conversion must be finite and positive.";
        const int n1=ctx.links.node1[link],n2=ctx.links.node2[link];
        const auto hasPoint=[&](int n){return n>=0&&std::size_t(n)<ctx.spatial.node_has_xy.size()&&ctx.spatial.node_has_xy[n]&&
            std::size_t(n)<ctx.spatial.node_x.size()&&std::size_t(n)<ctx.spatial.node_y.size();};
        if(!hasPoint(n1)||!hasPoint(n2))return "Channel '"+name+"': add coordinates for both endpoints before using mesh contacts.";
        std::vector<double> x={ctx.spatial.node_x[n1]*coordinate_scale},y={ctx.spatial.node_y[n1]*coordinate_scale};
        if(std::size_t(link)<ctx.spatial.link_vertices_x.size()&&std::size_t(link)<ctx.spatial.link_vertices_y.size()) {
            const auto& vx=ctx.spatial.link_vertices_x[link];const auto& vy=ctx.spatial.link_vertices_y[link];
            if(vx.size()!=vy.size())return "Channel '"+name+"': polyline coordinate arrays differ.";
            for(std::size_t p=0;p<vx.size();++p){x.push_back(vx[p]*coordinate_scale);y.push_back(vy[p]*coordinate_scale);}
        }
        x.push_back(ctx.spatial.node_x[n2]*coordinate_scale);y.push_back(ctx.spatial.node_y[n2]*coordinate_scale);
        const auto mapping=mapMeshPolyline(mesh,x,y);if(!mapping.error.empty())return "Channel '"+name+"': "+mapping.error;
        if(mapping.length-mapping.covered_length>1e-9*std::max(1.,mapping.length))
            return "Channel '"+name+"': mesh contacts leave "+std::to_string(mapping.length-mapping.covered_length)+" m outside the mesh; correct the geometry or select PRESCRIBED.";
        const double z0=(ctx.nodes.invert_elev[n1]+ctx.links.offset1[link])*.3048;
        const double z1=(ctx.nodes.invert_elev[n2]+ctx.links.offset2[link])*.3048,dz=z1-z0;
        for(const auto& interval:mapping.intervals) {
            const int cell=interval.cell;
            if(std::size_t(cell)>=soil.cells.size()||soil.cells[cell].column.energy.empty())
                return "Channel '"+name+"': mesh cell "+std::to_string(cell+1)+" has no porous thermal aquifer owner.";
            const auto& column=soil.cells[cell].column;
            const double a=interval.begin/mapping.length,b=interval.end/mapping.length;
            std::vector<double> cuts={a,b};
            if(dz!=0)for(double depth:column.depth_edges)for(double shift:{0.,material.bed_thickness}) {
                const double crossing=(mesh.tri_cz[cell]-depth+shift-z0)/dz;
                if(crossing>a&&crossing<b)cuts.push_back(crossing);
            }
            std::sort(cuts.begin(),cuts.end());cuts.erase(std::unique(cuts.begin(),cuts.end()),cuts.end());
            for(std::size_t p=1;p<cuts.size();++p) {
                const double begin=cuts[p-1],end=cuts[p],mid=begin+(end-begin)*.5,invert=z0+dz*mid;
                const double depth=mesh.tri_cz[cell]-invert;
                if(depth<-1e-9||depth>column.depth_edges.back()+1e-9)
                    return "Channel '"+name+"': invert lies outside soil depths in mesh cell "+std::to_string(cell+1)+"; adjust the aquifer geometry or select PRESCRIBED.";
                const int layer=std::clamp(int(std::upper_bound(column.depth_edges.begin(),column.depth_edges.end(),depth)-column.depth_edges.begin())-1,
                    0,int(column.energy.size())-1);
                next.contacts_.push_back({link,cell,layer,begin,end,interval.share,invert});next.cell_owned_[cell]=1;
            }
        }
        next.link_owned_[link]=1;
    }
    if(next.contacts_.empty())return {};
    for(int i=0;i<mesh.n_cells();++i)if(next.cell_owned_[i])next.cells_.push_back(i);
    std::sort(next.contacts_.begin(),next.contacts_.end(),[](const auto& a,const auto& b){
        if(a.link!=b.link)return a.link<b.link;if(a.begin!=b.begin)return a.begin<b.begin;return a.cell<b.cell;});
    next.contact_ranges_.resize(ctx.n_links());std::size_t cursor=0;
    for(int link=0;link<ctx.n_links();++link) {
        const auto begin=cursor;while(cursor<next.contacts_.size()&&next.contacts_[cursor].link==link)++cursor;
        next.contact_ranges_[link]={begin,cursor};
    }
    next.channel_atmospheric_j_.assign(ctx.n_links(),0);next.channel_ground_j_.assign(ctx.n_links(),0);next.channel_increment_j_.assign(ctx.n_links(),0);
    for(int cell:next.cells_)soil.cells[cell].surface_owned=true;
    ctx.heat_state.mesh_contact_link=next.link_owned_;
    *this=std::move(next);return {};
}
std::string ChannelGroundState::advance(const SolverOptions2D& options,const std::vector<SurfaceThermalSample>& samples,
    double dt,std::vector<double>& surface_mass) {
    if(!active())return {};
    try {
        if(!(dt>0)||!std::isfinite(dt))return "Channel/soil thermal step must be finite and positive.";
        auto& ctx=*context_;auto& mesh_heat=surface_->heat;const auto& mesh=*mesh_;
        if(!ctx.state_accessors.get_channel_thermal_regions||!ctx.state_accessors.apply_channel_thermal_increments)
            return "Channel thermal storage binding is unavailable.";
        const bool coupled_surface=mesh_heat.enabled&&mesh_heat.ground_column;
        if(coupled_surface&&samples.size()!=std::size_t(mesh.n_cells()))return "Shared channel contact lacks the mixed surface source samples.";
        auto regions=ctx.state_accessors.get_channel_thermal_regions();
        regions.erase(std::remove_if(regions.begin(),regions.end(),[&](const auto& r){return !ownsLink(r.link);}),regions.end());
        std::sort(regions.begin(),regions.end(),[](const auto& a,const auto& b){if(a.link!=b.link)return a.link<b.link;return a.begin<b.begin;});
        std::vector<double> footprint(mesh.n_cells(),0);
        for(const auto& r:regions) {
            if(!(r.volume>0)||!(r.end>r.begin)||r.begin<0||r.end>1||!std::isfinite(r.temperature)||
               !std::isfinite(r.volume)||r.surface_area<0||r.bed_area<0||!std::isfinite(r.surface_area)||!std::isfinite(r.bed_area))
                return "Nonfinite or invalid channel thermal region/geometry.";
            const auto range=contact_ranges_[r.link];
            for(std::size_t k=range.first;k<range.second;++k) {
                const auto& contact=contacts_[k];
                const double overlap=std::min(r.end,contact.end)-std::max(r.begin,contact.begin);
                if(overlap>0)footprint[contact.cell]+=r.surface_area*overlap/(r.end-r.begin)*contact.share;
            }
        }
        for(int cell:cells_)if(footprint[cell]>mesh.tri_area[cell]*(1+1e-10))
            return "Channel thermal footprints exceed mesh cell "+std::to_string(cell+1)+" area; resolve overlapping contacts or refine the mesh.";
        std::vector<ThermalStore> stores;std::vector<ThermalContact> contacts;std::vector<ThermalBoundary> boundaries;
        std::vector<GroundColumnAssembly> columns;columns.reserve(cells_.size());
        std::vector<int> column_offset(mesh.n_cells(),-1);
        for(int cell:cells_) {
            const auto sample=coupled_surface?samples[cell]:SurfaceThermalSample{};
            auto built=appendGroundColumnSystem(mesh,options,*ground_,cell,coupled_surface?&mesh_heat:nullptr,
                sample.mass,sample.volume,sample.evaporated,dt,std::min(footprint[cell],mesh.tri_area[cell]),stores,contacts,boundaries);
            column_offset[cell]=built.first_store;columns.push_back(built);
        }
        struct Water {int store=0;std::size_t boundary=0;double initial_j=0;};
        struct Transfer {std::size_t contact;int link,cell;};
        std::vector<Water> water;water.reserve(regions.size());std::vector<Transfer> transfers;
        const double cp=ground_->water_capacity;
        for(const auto& r:regions) {
            const int index=int(stores.size());stores.push_back({cp*r.volume,cp*r.volume*r.temperature});
            const auto elem=HeatElement::link(r.link);const auto& sc=sedimentFor(ctx,elem);
            const double j0=netFluxOut(ctx,elem,r.temperature),j1=netFluxOut(ctx,elem,r.temperature+kProbeC);
            const double slope=std::max(0.,(j1-j0)/kProbeC);
            water.push_back({index,boundaries.size(),cp*r.volume*r.temperature});
            boundaries.push_back({index,r.surface_area*slope,r.temperature,-r.surface_area*j0});
            const auto add=[&](int cell,int layer,double g) {
                if(!ctx.heat_config.sediment_exchange)return;
                if(!(g>0))return;transfers.push_back({contacts.size(),r.link,cell});
                contacts.push_back({index,column_offset[cell]+layer,g});
            };
            const auto range=contact_ranges_[r.link];
            for(std::size_t k=range.first;k<range.second;++k) {
                const auto& c=contacts_[k];
                const double overlap=std::min(r.end,c.end)-std::max(r.begin,c.begin);if(!(overlap>0))continue;
                const double area=r.bed_area*overlap/(r.end-r.begin)*c.share;
                const auto& column=ground_->cells[c.cell].column;
                if(ground_->config->options.conduction)
                    add(c.cell,c.layer,area*column.conductivity[c.layer]/(.5*(column.depth_edges[c.layer+1]-column.depth_edges[c.layer])));
                double pores=0;std::vector<double> weights(column.energy.size());
                for(std::size_t j=0;j<weights.size();++j) {
                    const double top=mesh.tri_cz[c.cell]-column.depth_edges[j],bottom=mesh.tri_cz[c.cell]-column.depth_edges[j+1];
                    weights[j]=column.theta[j]*std::max(0.,std::min(top,c.invert)-std::max(bottom,c.invert-sc.bed_thickness));pores+=weights[j];
                }
                if(pores>0&&sc.hyporheic_velocity>0)for(std::size_t j=0;j<weights.size();++j)
                    if(weights[j]>0)add(c.cell,int(j),cp*sc.hyporheic_velocity*area*weights[j]/pores);
            }
        }
        auto solved=solveThermalContacts(stores,contacts,boundaries,dt);if(!solved.error.empty())return solved.error;
        std::vector<ChannelThermalIncrement> increments;increments.reserve(regions.size());
        for(std::size_t k=0;k<regions.size();++k)
            increments.push_back({regions[k].link,regions[k].region,solved.temperature[water[k].store]-regions[k].temperature});
        std::vector<double> next_mass(mesh.n_cells(),0);
        // Validate/publish the external canonical channel binding before the
        // nonthrowing soil commit. A refusal leaves both thermal sides intact.
        if(!increments.empty())if(auto error=ctx.state_accessors.apply_channel_thermal_increments(increments);!error.empty())return error;
        for(const auto& column:columns)next_mass[column.cell]=commitGroundColumnSystem(column,solved);
        for(const auto& transfer:transfers) {
            const double joules=solved.contact_j[transfer.contact];
            ground_->cells[transfer.cell].channel_contact_j+=joules;channel_ground_j_[transfer.link]+=joules;
        }
        for(std::size_t k=0;k<regions.size();++k) {
            const int link=regions[k].link;
            channel_atmospheric_j_[link]+=solved.boundary_j[water[k].boundary];
            channel_increment_j_[link]+=solved.energy[water[k].store]-water[k].initial_j;
        }
        for(int cell:cells_)ground_->refreshView(cell,*transport_);
        if(coupled_surface)mesh_heat.channel_footprint=std::move(footprint);
        surface_mass=std::move(next_mass);return {};
    }catch(const std::exception& e){return e.what();}
     catch(...){return "Channel/soil thermal contact failed.";}
}
} // namespace openswmm::twoD
