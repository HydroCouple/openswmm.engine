// SPDX-License-Identifier: Apache-2.0
#include "LidNode.hpp"
#include "LidNodeRichardsState.hpp"
#include <type_traits>
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../hydraulics/Node.hpp"
#include "../input/Tokenizer.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace openswmm::lidnode {
namespace { constexpr double metres = .3048, cubic_metres = .028316846592; }
bool richardsMode(const SimulationContext& ctx, int node) {
    int r = ctx.node_subtypes.storage_row(node);
    return r >= 0 && ctx.node_subtypes.storages.lid_state[r].richards;
}
double exchangeRichardsPorts(SimulationContext& ctx, int link, double flow, double dt) {
    const int source = flow > 0 ? ctx.links.node1[link] : ctx.links.node2[link];
    const int dest = flow > 0 ? ctx.links.node2[link] : ctx.links.node1[link];
    const int sr = ctx.node_subtypes.storage_row(source), dr = ctx.node_subtypes.storage_row(dest);
    const double so = portOffset(ctx, link, source), doff = portOffset(ctx, link, dest);
    const bool source_active = active(ctx, source), dest_active = active(ctx, dest);
    int si = source_active ? portCell(ctx.node_subtypes.storages.lid_state[sr], so) : -1;
    int di = dest_active ? portCell(ctx.node_subtypes.storages.lid_state[dr], doff) : -1;
    const bool source_held = source_active && (richardsMode(ctx, source) ? si > 0 : so >= ctx.nodes.depth[source]);
    const bool dest_held = dest_active && (richardsMode(ctx, dest) ? di > 0 : doff >= ctx.nodes.depth[dest]);
    double volume = std::abs(flow) * dt;
    if (source_active) {
        auto& s = ctx.node_subtypes.storages.lid_state[sr];
        if (source_held) {
            const auto& c = s.cells[si]; double available = 0;
            if (s.richards) {
                const auto p = layers(ctx, ctx.node_subtypes.storages.lid[sr].control)[c.layer - 1].retention;
                double cutoff = (so - .5 * (c.top + c.bottom)) * metres;
                available = s.richards_water[si] + s.port_delta[si] - richards::storage(p, c.porosity, cutoff) * c.geometric_volume;
            } else {
                available = (c.theta - c.wilting_point) * c.geometric_volume + s.port_delta[si];
                if (c.kind == LidNodeLayerKind::Media) volume *= std::clamp(c.theta / c.porosity, 0., 1.);
            }
            volume = std::min(volume, std::max(0., available));
        } else volume = std::min(volume, std::max(0., ctx.nodes.old_volume[source] +
            (ctx.nodes.lat_flow[source] - ctx.nodes.losses[source]) * dt + s.mobile_delta));
    }
    if (dest_held && richardsMode(ctx, dest)) {
        auto& s = ctx.node_subtypes.storages.lid_state[dr]; const auto& c = s.cells[di];
        const auto p = layers(ctx, ctx.node_subtypes.storages.lid[dr].control)[c.layer - 1].retention;
        const double H = ctx.nodes.invert_elev[source] + portDepth(ctx, source, so) - ctx.nodes.invert_elev[dest];
        const double target = (H - .5 * (c.top + c.bottom)) * metres;
        const double capacity = richards::storage(p, c.porosity, target) * c.geometric_volume - s.richards_water[di] - s.port_delta[di];
        volume = std::min(volume, std::max(0., capacity));
    }
    if (source_active) {
        auto& s = ctx.node_subtypes.storages.lid_state[sr];
        if (source_held) {
            s.port_delta[si] -= volume; s.quality_ports.push_back({link, si, -volume});
            ctx.nodes.inflow[source] += volume / dt;
        } else s.mobile_delta -= volume;
    }
    if (dest_active) {
        auto& s = ctx.node_subtypes.storages.lid_state[dr]; double held = 0;
        if (dest_held) {
            const auto& c = s.cells[di];
            held = s.richards ? volume : std::min(volume, std::max(0., (c.porosity - c.theta) * c.geometric_volume - s.port_delta[di]));
            s.port_delta[di] += held; s.quality_ports.push_back({link, di, held});
            ctx.nodes.outflow[dest] += held / dt;
        }
        s.mobile_delta += volume - held;
    }
    return std::copysign(volume / dt, flow);
}
void readRichards(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto t = input::Tokenizer::tokenize(line);
        if (t.empty()) continue;
        int control = ctx.lid_names.find(t[0]);
        if (control < 0 || control >= static_cast<int>(ctx.lid_controls.node_layers.size()) || ctx.lid_controls.node_layers[control].empty()) {
            ctx.deferred_section_rows.emplace_back("LID_RICHARDS", line); continue;
        }
        auto& stack = ctx.lid_controls.node_layers[control];
        std::istringstream in(line); std::string name, tag, trailing;
        in >> name >> tag;
        if (input::Tokenizer::to_upper(tag) == "OPTIONS") {
            richards::Options o; o.enabled = true;
            if (!(in >> o.cells_per_layer >> o.atol >> o.rtol >> o.max_step) || (in >> trailing) || !richards::valid(o) || stack.front().flow.enabled) {
                ctx.errors.push_back("Invalid or duplicate [LID_RICHARDS] OPTIONS: " + line); continue;
            }
            stack.front().flow = o;
        } else {
            std::istringstream layer_stream(tag); int layer = 0;
            richards::Material p;
            if (!(layer_stream >> layer) || (layer_stream >> trailing) || !(in >> p.theta_r >> p.alpha >> p.n >> p.l >> p.specific_storage) ||
                (in >> trailing) || layer < 1 || layer > static_cast<int>(stack.size()) ||
                (stack[layer - 1].kind != LidNodeLayerKind::Media && stack[layer - 1].kind != LidNodeLayerKind::Aggregate) ||
                !richards::valid(p, stack[layer - 1].params[1]) || stack[layer - 1].retention.alpha > 0) {
                ctx.errors.push_back("Invalid or duplicate [LID_RICHARDS] material: " + line); continue;
            }
            stack[layer - 1].retention = p;
        }
    }
}
namespace {
void refresh(LidNodeState& s,const std::vector<LidNodeLayer>& stack,const std::string& name) {
    s.held_volume = 0;
    for (std::size_t i = 0; i < s.cells.size(); ++i) {
        auto& cell = s.cells[i];
        if (!i) { cell.theta = 0; continue; }
        const auto& p = stack[cell.layer - 1].retention;
        const double h = richards::pressure(p, cell.porosity, s.richards_water[i] / cell.geometric_volume);
        if (!std::isfinite(h)) throw std::runtime_error("Richards non-finite pressure at LID node " + name);
        s.richards_pressure[i] = h / metres;
        cell.theta = richards::theta(p, cell.porosity, h);
        s.held_volume += s.richards_water[i];
    }
}
}
void refreshRichardsState(SimulationContext& ctx, int r) {
    auto& st=ctx.node_subtypes.storages;
    refresh(st.lid_state[r],layers(ctx,st.lid[r].control),ctx.node_names.name_of(st.node_idx[r]));
}
namespace detail {
richards::Column column(const SimulationContext& ctx,int r,double dt,double evaporation,bool continuous) {
    const auto& st=ctx.node_subtypes.storages;const auto& s=st.lid_state[r];
    const int node=st.node_idx[r];const auto stack=layers(ctx,st.lid[r].control);
    richards::Column col;col.options=stack.front().flow;
    const double capture=std::max(0.,ctx.nodes.lat_flow[node])*dt;
    for(std::size_t i=0;i<s.cells.size();++i) {
        const auto& c=s.cells[i];richards::Cell cell;
        cell.volume=c.geometric_volume*cubic_metres;cell.area=c.area*metres*metres;
        cell.z=(i?.5*(c.top+c.bottom):c.bottom)*metres;
        cell.dz=(c.top-c.bottom)*metres;cell.theta_s=c.porosity;
        cell.Ks=c.conductivity*metres*std::max(0.,ctx.climate_state.infil_factor);
        cell.wilting=c.wilting_point;cell.material=stack[c.layer-1].retention;
        if(!i)cell.volume*=c.porosity;
        col.cells.push_back(cell);
        col.water.push_back((i?s.richards_water[i]:ctx.nodes.volume[node]+(continuous?0:capture))*cubic_metres);
    }
    if(continuous)col.inflow=std::max(0.,ctx.nodes.lat_flow[node])*cubic_metres;
    const double demand=std::max(0.,evaporation*st.evap_frac[r]*s.cells.front().area)*cubic_metres;
    if(col.water[0]>0)col.cells[0].potential_et=demand;
    else {
        double thickness=0;for(std::size_t i=1;i<col.cells.size();++i)thickness+=col.cells[i].dz;
        for(std::size_t i=1;i<col.cells.size();++i)col.cells[i].potential_et=demand*col.cells[i].dz/thickness;
    }
    const int us=ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    if(st.lid[r].bottom==LidNodeBottom::Current && stack.back().kind==LidNodeLayerKind::Bottom && stack.back().params[0]>0) {
        col.bottom=richards::Bottom::FreeDrainage;
        col.bottom_K=stack.back().params[0]/ucf::Ucf[ucf::RAINFALL][us]*metres*bottomCloggingFactor(ctx,r);
    }
    return col;
}
RichardsNodeDraft draft(const SimulationContext& ctx,int r,const richards::Column& col,
    const richards::Report& report,double dt,double completed,bool mesh) {
    const auto& st=ctx.node_subtypes.storages;RichardsNodeDraft d;d.row=r;d.node=st.node_idx[r];
    d.state=st.lid_state[r];auto& s=d.state;s.richards_report=report;
    s.quality_old_mobile=ctx.nodes.volume[d.node];s.quality_old_water=s.richards_water;
    s.quality_transfers.clear();
    const double capture=mesh?report.inflow/cubic_metres:std::max(0.,ctx.nodes.lat_flow[d.node])*dt;
    s.captured_flow=capture/dt;s.treated_volume+=capture;
    if(!mesh && capture>0)s.quality_transfers.push_back({-2,-1,capture});
    d.lat_flow=ctx.nodes.lat_flow[d.node]-s.captured_flow;d.volume=col.water[0]/cubic_metres;
    for(std::size_t i=1;i<col.water.size();++i)s.richards_water[i]=col.water[i]/cubic_metres;
    for(const auto& t:s.richards_report.transfers)
        s.quality_transfers.push_back({t.from==0?-1:t.from,t.to==0?-1:t.to,t.volume/cubic_metres});
    s.evap_volume=s.richards_report.evaporation/cubic_metres;
    s.richards_bottom_loss=mesh?0:std::max(0.,s.richards_report.bottom_volume)/cubic_metres;
    refresh(s,layers(ctx,st.lid[r].control),ctx.node_names.name_of(d.node));
    s.richards_completed_time=completed;s.richards_mesh_pending_finish=mesh;
    // Richards geometry depends only on the surface cell; accepted porous
    // water does not change these curves, so the current context is read-only.
    const int us=ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    d.full_volume=node::getVolume(ctx.nodes,d.node,ctx.nodes.full_depth[d.node],nullptr,us,&ctx.node_subtypes);
    d.depth=node::getDepth(ctx.nodes,d.node,d.volume,nullptr,us,&ctx.node_subtypes);
    d.head=ctx.nodes.invert_elev[d.node]+d.depth;
    return d;
}
void publish(SimulationContext& ctx,RichardsNodeDraft&& d) noexcept {
    static_assert(std::is_nothrow_swappable_v<LidNodeState>);
    std::swap(ctx.node_subtypes.storages.lid_state[d.row],d.state);
    ctx.nodes.lat_flow[d.node]=d.lat_flow;ctx.nodes.volume[d.node]=d.volume;
    ctx.nodes.full_volume[d.node]=d.full_volume;ctx.nodes.rpt_full_volume[d.node]=d.full_volume;
    ctx.nodes.depth[d.node]=d.depth;ctx.nodes.head[d.node]=d.head;
}
LidNodeState routed(const SimulationContext& ctx,int r) {
    const auto& st=ctx.node_subtypes.storages;auto s=st.lid_state[r];
    if(s.richards_water.size()!=s.cells.size() || s.port_delta.size()!=s.cells.size())
        throw std::runtime_error("Invalid Mesh Richards accepted port layout.");
    for(std::size_t i=0;i<s.cells.size();++i) {
        if(!std::isfinite(s.port_delta[i]))throw std::runtime_error("Nonfinite Mesh Richards accepted port water.");
        if(!i && s.port_delta[i]!=0)throw std::runtime_error("Mesh Richards surface port water belongs to the mobile owner.");
        s.richards_water[i]+=s.port_delta[i];
    }
    refresh(s,layers(ctx,st.lid[r].control),ctx.node_names.name_of(st.node_idx[r]));
    return s;
}
}
void prepareRichardsStep(SimulationContext& ctx,double dt,double evaporation) {
    auto& st=ctx.node_subtypes.storages;
    // Refuse the whole batch before capturing forcing or changing any owner.
    for(int r=0;r<st.count();++r)if(st.lid_state[r].richards && st.lid[r].bottom==LidNodeBottom::MeshAquifer)
        throw std::runtime_error("Mesh Richards nodes require joint aquifer owner preparation.");
    std::vector<richards::Column> columns;std::vector<int> rows;
    columns.reserve(st.count());rows.reserve(st.count());
    for(int r=0;r<st.count();++r)if(st.lid_state[r].richards) {
        rows.push_back(r);columns.push_back(detail::column(ctx,r,dt,evaporation));
    }
    if(columns.empty())return;
    std::vector<richards::Column*> batch;for(auto& c:columns)batch.push_back(&c);
    auto reports=richards::advance(batch,dt);
    for(std::size_t a=0;a<rows.size();++a)if(!reports[a].ok)
        throw std::runtime_error("LID Richards at "+ctx.node_names.name_of(st.node_idx[rows[a]])+": "+reports[a].error);
    std::vector<detail::RichardsNodeDraft> drafts;
    for(std::size_t a=0;a<rows.size();++a)
        drafts.push_back(detail::draft(ctx,rows[a],columns[a],reports[a],dt,ctx.current_time+dt));
    for(auto& d:drafts)detail::publish(ctx,std::move(d));
}
}
