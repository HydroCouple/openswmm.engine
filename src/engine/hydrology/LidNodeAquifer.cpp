// SPDX-License-Identifier: Apache-2.0
#include "LidNodeAquifer.hpp"
#ifdef OPENSWMM_HAS_2D
#include "LidNodeRichardsState.hpp"
#include "../core/SimulationContext.hpp"
#include "../hydraulics/Node.hpp"
#include "../2d/subsurface/LidNodeExchange.hpp"
#include "../2d/data/SurfaceStateData.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <type_traits>

namespace openswmm::lidnode {
namespace {
twoD::RichardsBedAcceptance acceptNodes(SimulationContext& ctx,twoD::SubsurfaceSolver& gw,
    twoD::SurfaceStateData& surface,double start,double end,double evaporation,const std::vector<int>& input,int tier) {
    using twoD::RichardsBedAcceptance;
    const auto fail=[](const std::string& error){RichardsBedAcceptance r;r.error=error;return r;};
    if(!std::isfinite(start) || start<0 || !std::isfinite(end) || end<=start ||
       !std::isfinite(end-start) || !std::isfinite(evaporation) || evaporation<0 || input.empty())
        return fail("Invalid Richards node owner interval/evaporation.");
    if(ctx.n_pollutants()>0 || ctx.options.water_age || ctx.options.heat_transport || ctx.reactions.configured)
        return fail("Joint Richards node adoption requires water-only routing.");
    if(!ctx.nodes.aquifer2d_bed.empty() && ctx.nodes.aquifer2d_bed.size()!=std::size_t(ctx.n_nodes()))
        return fail("Invalid Richards node aquifer ownership layout.");
    auto nodes=input;std::sort(nodes.begin(),nodes.end());
    if(std::adjacent_find(nodes.begin(),nodes.end())!=nodes.end())return fail("Duplicate Richards node owner.");
    const double dt=end-start;auto& st=ctx.node_subtypes.storages;
    std::vector<int> bed_by_node(ctx.n_nodes(),-1),slot(ctx.n_nodes(),-1);
    for(std::size_t b=0;b<gw.nodeBeds().size();++b) {
        const auto& bed=gw.nodeBeds()[b];
        if(!bed.exchange || bed.node<0 || bed.node>=ctx.n_nodes())continue;
        auto& owner=bed_by_node[bed.node];owner=owner==-1?int(b):-2;
    }
    std::vector<int> rows,beds;std::vector<richards::Column> columns;
    for(int n:nodes) {
        if(n<0 || n>=ctx.n_nodes())return fail("Unknown Richards node owner.");
        const int r=ctx.node_subtypes.storage_row(n);
        if(r<0 || ctx.nodes.type[n]!=NodeType::STORAGE || !st.lid_state[r].richards || st.lid[r].bottom!=LidNodeBottom::MeshAquifer)
            return fail("Richards node owner requires an initialized Mesh column.");
        const auto& s=st.lid_state[r];const auto stack=layers(ctx,st.lid[r].control);
        if(s.richards_completed_time!=start || s.richards_mesh_pending_finish || s.captured_flow!=0 ||
           s.mobile_delta!=0 || std::any_of(s.port_delta.begin(),s.port_delta.end(),[](double v){return v!=0;}))
            return fail("Richards node interval is stale or has unfinished forcing/port trials.");
        if(stack.empty() || s.cells.size()<2 || s.richards_water.size()!=s.cells.size() ||
           s.richards_pressure.size()!=s.cells.size() || s.port_delta.size()!=s.cells.size())
            return fail("Invalid Richards node geometry/state layout.");
        for(const auto& c:s.cells)if(c.layer<1 || c.layer>int(stack.size()))return fail("Invalid Richards node layer mapping.");
        if(!std::isfinite(ctx.nodes.lat_flow[n]) || !std::isfinite(ctx.nodes.invert_elev[n]) ||
           !std::isfinite(ctx.nodes.full_depth[n]) || !std::isfinite(ctx.nodes.sur_depth[n]) ||
           ctx.nodes.full_depth[n]<=0 || ctx.nodes.sur_depth[n]<0 ||
           (ctx.options.allow_ponding && ctx.nodes.ponded_area[n]>0))
            return fail("Invalid or unqualified ponded Richards hydraulic owner geometry.");
        if(!std::isfinite(ctx.climate_state.infil_factor) || ctx.climate_state.infil_factor<0 ||
           !std::isfinite(st.evap_frac[r]) || st.evap_frac[r]<0 || st.evap_frac[r]>1)
            return fail("Invalid Richards node climate/evaporation factors.");
        const int bed=bed_by_node[n];
        if(bed==-2)return fail("Richards node has multiple unresolved bed owners.");
        if(bed<0 || !gw.active() || gw.nodeBeds()[bed].cell<0 || gw.nodeBeds()[bed].cell>=gw.state().n_cells)
            return fail("Richards node has no enabled resolved aquifer bed.");
        twoD::SubsurfaceSolver::NodeWaterBudget budget;
        const auto budget_error=gw.nodeWaterBudget(gw.nodeBeds()[bed].cell,dt,budget);
        if(!budget_error.empty())return fail(budget_error);
        slot[n]=int(rows.size());rows.push_back(r);beds.push_back(bed);
        columns.push_back(detail::column(ctx,r,dt,evaporation,true));
    }
    constexpr double metres=.3048,cubic_metres=.028316846592;
    std::vector<double> port_heads(nodes.size(),0);
    // Scan native topology once for the vector batch, rather than once per node.
    for(int link=0;link<ctx.n_links();++link) {
        const int ends[]{ctx.links.node1[link],ctx.links.node2[link]};
        for(int side=0;side<2;++side) {
            const int n=ends[side],other=ends[1-side];
            if(n<0 || n>=ctx.n_nodes() || slot[n]<0)continue;
            if(other<0 || other>=ctx.n_nodes())return fail("Invalid Richards port topology.");
            const double h=(ctx.nodes.invert_elev[other]+portDepth(ctx,other,portOffset(ctx,link,other))-ctx.nodes.invert_elev[n])*metres;
            if(!std::isfinite(h))return fail("Invalid Richards port head snapshot.");
            port_heads[slot[n]]=std::max(port_heads[slot[n]],h);
        }
    }
    std::vector<twoD::RichardsBedOwner> owners;
    for(std::size_t j=0;j<nodes.size();++j) {
        const int n=nodes[j],r=rows[j];const auto& s=st.lid_state[r];const auto& c=columns[j];
        const auto& bed=gw.nodeBeds()[beds[j]];const auto& g=gw.state();
        // A finite pressure envelope from actual geometry and available heads,
        // including backwater at native layer ports and existing elastic water.
        // The visible surface head is not the porous boundary sample.
        double max_head=std::max({(ctx.nodes.full_depth[n]+ctx.nodes.sur_depth[n])*metres,
            g.z_bed[bed.cell]+g.hg[bed.cell]-ctx.nodes.invert_elev[n]*metres,port_heads[j]});
        for(std::size_t k=1;k<c.cells.size();++k) {
            const double h=c.cells[k].z+richards::pressure(c.cells[k].material,c.cells[k].theta_s,c.water[k]/c.cells[k].volume);
            if(!std::isfinite(h))return fail("Invalid Richards porous pressure snapshot.");
            max_head=std::max(max_head,h);
        }
        const auto& top=s.cells.front();
        std::vector<double> maximum{std::clamp(ctx.nodes.full_depth[n]-top.bottom,0.,top.top-top.bottom)*top.area*top.porosity*cubic_metres};
        for(std::size_t k=1;k<c.cells.size();++k)
            maximum.push_back(std::max(c.water[k],richards::storage(c.cells[k].material,c.cells[k].theta_s,max_head-c.cells[k].z)*c.cells[k].volume));
        owners.push_back({beds[j],&columns[j],ctx.nodes.invert_elev[n]*metres,bottomCloggingFactor(ctx,r),std::move(maximum)});
    }
    // The lower-level transaction commits only into these private copies. Real
    // node pressure/capacity/forcing views must also qualify before publication.
    auto aquifer=gw;auto surf=surface;
    auto node_beds=ctx.nodes.aquifer2d_bed;node_beds.resize(ctx.n_nodes(),0);
    for(int n:nodes)node_beds[n]=1;
    auto accepted=tier<0?twoD::acceptRichardsBedsAtFiring(aquifer,surf,start,end,owners):
        twoD::acceptRichardsBedsForTier(aquifer,surf,tier,start,end,owners);
    if(!accepted.error.empty())return accepted;
    std::vector<detail::RichardsNodeDraft> drafts;
    try {
        for(std::size_t j=0;j<nodes.size();++j) {
            auto d=detail::draft(ctx,rows[j],columns[j],accepted.trial.reports[j],dt,end,true);
            d.state.richards_mesh_start=start; // preserve the caller's exact accepted endpoint pair
            for(double v:{d.volume,d.depth,d.head,d.full_volume,d.lat_flow,d.state.held_volume,d.state.treated_volume})
                if(!std::isfinite(v))return fail("Nonfinite Richards routing publication draft.");
            const double requested=std::max(0.,ctx.nodes.lat_flow[nodes[j]])*dt*cubic_metres;
            if(std::abs(accepted.trial.reports[j].inflow-requested)>1e-12*std::max(1.,requested))
                return fail("Richards accepted forcing does not match the routed interval.");
            drafts.push_back(std::move(d));
        }
    } catch(const std::exception& e) {return fail(e.what());}
    static_assert(std::is_nothrow_move_assignable_v<twoD::SubsurfaceSolver>);
    static_assert(std::is_nothrow_move_assignable_v<twoD::SurfaceStateData>);
    gw=std::move(aquifer);surface=std::move(surf);
    ctx.nodes.aquifer2d_bed.swap(node_beds); // existing hydraulic checkpoint profile refuses unpaired aquifer owners
    for(auto& d:drafts)detail::publish(ctx,std::move(d));
    return accepted;
}
}
twoD::RichardsBedAcceptance acceptRichardsNodesAtFiring(SimulationContext& ctx,twoD::SubsurfaceSolver& gw,
    twoD::SurfaceStateData& surface,double start,double end,double evaporation,const std::vector<int>& nodes) {
    return acceptNodes(ctx,gw,surface,start,end,evaporation,nodes,-1);
}
twoD::RichardsBedAcceptance acceptRichardsNodesForTier(SimulationContext& ctx,twoD::SubsurfaceSolver& gw,
    twoD::SurfaceStateData& surface,int tier,double start,double end,double evaporation,const std::vector<int>& nodes) {
    if(tier<0){twoD::RichardsBedAcceptance r;r.error="Invalid Richards node groundwater tier.";return r;}
    return acceptNodes(ctx,gw,surface,start,end,evaporation,nodes,tier);
}
}
#endif
