// SPDX-License-Identifier: Apache-2.0
#include <openswmm/engine/openswmm_coupling.h>
#include <openswmm/engine/openswmm_2d.h>
#include "../../core/SWMMEngine.hpp"
#include <new>
#include <optional>
#include <set>
#include <tuple>

namespace {
using openswmm::SWMMEngine;
using namespace openswmm::twoD;
int ready(SWMMEngine* eng) {
    if(!eng) return SWMM_ERR_BADHANDLE;
    if(eng->context().state!=openswmm::EngineState::RUNNING || !eng->runtimeUpdateAllowed()) return SWMM_ERR_LIFECYCLE;
    return eng->surfaceRouter2D().runtimeCpuAvailable() ? SWMM_OK : SWMM_ERR_PLUGIN;
}
bool domainOk(int d) { return d==0 || d==1; }
RuntimeSources& sources(SWMMEngine& eng,int d) {
    auto& r=eng.surfaceRouter2D(); return d==0 ? r.runtimeSources() : r.subsurface().runtimeSources();
}
int readReady(SWMMEngine* eng,int d) {
    if(!eng) return SWMM_ERR_BADHANDLE;
    if(!domainOk(d)) return SWMM_ERR_BADPARAM;
    auto& r=eng->surfaceRouter2D();
    return eng->runtimeUpdateAllowed() && r.isActive() && (d==0 || r.subsurface().active()) ? SWMM_OK : SWMM_ERR_LIFECYCLE;
}
int species(SWMMEngine& eng,int d) {
    auto& r=eng.surfaceRouter2D(); return d==0 ? r.state().transport.n_species : r.subsurface().transport().n_species;
}
std::string boundaryId(int cell,int edge) { return "boundary/"+std::to_string(cell)+"/"+std::to_string(edge); }
std::string sourceId(const std::string& owner,const char* id) {
    return owner.empty() ? std::string(id) : "provider/"+owner+"/"+id;
}
bool reserved(const char* id) {
    return !id || !*id || std::string(id).rfind("boundary/",0)==0 || std::string(id).rfind("provider/",0)==0;
}
int quality(SWMMEngine& eng,int d,const double* conc,int count) {
    auto& r=eng.surfaceRouter2D();
    const int temp=d==0 ? r.state().transport.temp_row : r.subsurface().transport().temp_row;
    if(count!=species(eng,d) || (count && !conc)) return SWMM_ERR_BADPARAM;
    for(int s=0;s<count;++s) if(!std::isfinite(conc[s]) || (s!=temp && conc[s]<0.0)) return SWMM_ERR_BADPARAM;
    return SWMM_OK;
}
int cellCheck(SWMMEngine& eng,int d,int cell) {
    auto& r=eng.surfaceRouter2D();
    if(!domainOk(d)) return SWMM_ERR_BADPARAM;
    if(cell<0 || cell>=r.mesh().n_cells()) return SWMM_ERR_BADINDEX;
    if(d==1 && (!r.subsurface().active() || r.subsurface().state().zs[cell]<=0.0)) return SWMM_ERR_LIFECYCLE;
    return SWMM_OK;
}
int edgeCheck(SWMMEngine& eng,int d,int cell,int edge) {
    const int rc=cellCheck(eng,d,cell); if(rc) return rc;
    const auto& m=eng.surfaceRouter2D().mesh();
    if(edge<0 || edge>=m.cell_nv[cell]) return SWMM_ERR_BADINDEX;
    const int slot=MeshData::slot(cell,edge);
    return m.cell_neighbour(cell,edge)<0 && m.edge_length[slot]>0.0 && m.edge_dist_c[slot]>0.0 ? SWMM_OK : SWMM_ERR_BADPARAM;
}
void receiptOut(const RuntimeReceipt& r,SWMM_CouplingReceipt& out,double* requested,double* applied) {
    out={r.requested,r.applied,r.requested-r.applied,r.heat_requested,r.heat_applied,r.heat_requested-r.heat_applied,r.last_flow,r.last_dt};
    if(!r.applied_mass.empty()) {
        std::copy(r.requested_mass.begin(),r.requested_mass.end(),requested);
        std::copy(r.applied_mass.begin(),r.applied_mass.end(),applied);
    }
}
int outputCheck(SWMMEngine& eng,int d,SWMM_CouplingReceipt* out,double* req,double* app,int count) {
    return out && count==species(eng,d) && (count==0 || (req && app)) ? SWMM_OK : SWMM_ERR_BADPARAM;
}
}
extern "C" {
int swmm_coupling_apply_frame(SWMM_Engine h,const SWMM_CouplingFrame* f) {
    auto* eng=reinterpret_cast<SWMMEngine*>(h); int rc=ready(eng); if(rc) return rc;
    if(!f || f->struct_size!=sizeof(*f) || f->version!=SWMM_COUPLING_FRAME_VERSION ||
       f->source_count<0 || f->boundary_count<0 || f->forcing_count<0 || f->clear_count<0 ||
       (f->source_count && !f->sources) || (f->boundary_count && !f->boundaries) ||
       (f->forcing_count && !f->forcings) || (f->clear_count && !f->clears)) return SWMM_ERR_BADPARAM;
    try {
        const std::string owner=f->provider ? f->provider : "";
        if(owner.find('/')!=std::string::npos) return SWMM_ERR_BADPARAM;
        auto& router=eng->surfaceRouter2D(); auto& tr=router.state().transport;
        const auto& mesh=router.mesh(); const int nc=mesh.n_cells();
        const double now=eng->context().elapsed_ms/1000.0;
        std::optional<RuntimeSources> next[2];
        const auto editSources=[&](int d)->RuntimeSources& {
            if(!next[d]) next[d]=sources(*eng,d); return *next[d];
        };
        std::optional<BoundaryData> bc;
        std::optional<std::map<int,std::vector<double>>> concentrations;
        std::optional<RuntimeForcings> forcing;
        std::set<std::tuple<int,std::string,int>> source_keys;
        std::set<std::tuple<int,int,int>> edge_keys;
        std::set<std::pair<int,int>> forcing_keys;
        for(int k=0;k<f->source_count;++k) {
            const auto& in=f->sources[k];
            if(in.struct_size!=sizeof(in) || reserved(in.id) || !domainOk(in.domain) ||
               !std::isfinite(in.flow_m3_s) || !std::isfinite(in.heat_w) || !std::isfinite(in.until_seconds) ||
               (in.until_seconds!=0.0 && in.until_seconds<=now)) return SWMM_ERR_BADPARAM;
            if((rc=cellCheck(*eng,in.domain,in.cell)) || (rc=quality(*eng,in.domain,in.concentrations,in.species_count))) return rc;
            const int temp=in.domain==0 ? tr.temp_row : router.subsurface().transport().temp_row;
            const int age=in.domain==0 ? tr.age_row : router.subsurface().transport().age_row;
            if((in.species_count && !in.rates) || (in.heat_w!=0.0 && temp<0)) return SWMM_ERR_BADPARAM;
            RuntimeSource row; row.id=sourceId(owner,in.id); row.owner=owner; row.cell=in.cell;
            row.flow=in.flow_m3_s; row.heat=in.heat_w;
            if(in.until_seconds) row.until=in.until_seconds;
            if(!source_keys.emplace(in.domain,row.id,in.cell).second) return SWMM_ERR_BADPARAM;
            for(int s=0;s<in.species_count;++s) {
                if(!std::isfinite(in.rates[s]) || ((s==temp || s==age) && in.rates[s]!=0.0)) return SWMM_ERR_BADPARAM;
                row.concentrations.push_back(in.concentrations[s]); row.rates.push_back(in.rates[s]);
            }
            editSources(in.domain).replace(std::move(row));
        }
        for(int k=0;k<f->clear_count;++k) {
            const auto& in=f->clears[k];
            if(reserved(in.id) || !domainOk(in.domain) || in.cell< -1) return SWMM_ERR_BADPARAM;
            if(in.cell>=0 && (rc=cellCheck(*eng,in.domain,in.cell))) return rc;
            const auto id=sourceId(owner,in.id);
            // Detect whole-ID conflicts with an ordered lookup, without
            // rescanning every source for each cell-selective clear.
            if(in.cell<0) {
                const auto key=source_keys.lower_bound({in.domain,id,-1});
                if(key!=source_keys.end() && std::get<0>(*key)==in.domain && std::get<1>(*key)==id) return SWMM_ERR_BADPARAM;
            } else if(source_keys.count({in.domain,id,-1})) return SWMM_ERR_BADPARAM;
            if(!source_keys.emplace(in.domain,id,in.cell).second) return SWMM_ERR_BADPARAM;
            if(!editSources(in.domain).clear(id,in.cell)) return SWMM_ERR_BADINDEX;
        }
        for(int k=0;k<f->boundary_count;++k) {
            const auto& in=f->boundaries[k];
            if(in.struct_size!=sizeof(in) || in.kind<0 || in.kind>2 || !std::isfinite(in.value)) return SWMM_ERR_BADPARAM;
            if((rc=edgeCheck(*eng,in.domain,in.cell,in.edge))) return rc;
            if(!edge_keys.emplace(in.domain,in.cell,in.edge).second) return SWMM_ERR_BADPARAM;
            if(in.kind && (rc=quality(*eng,in.domain,in.concentrations,in.species_count))) return rc;
            if(!in.kind && in.species_count!=0) return SWMM_ERR_BADPARAM;
            if(in.domain==0) {
                if(!bc) { bc=router.boundary(); concentrations=tr.runtime_bc_conc; }
                const int slot=MeshData::slot(in.cell,in.edge);
                auto old=bc->runtime_owner.find(slot);
                const std::string previous=old==bc->runtime_owner.end() ? "" : old->second;
                if(bc->runtime_original.count(slot) && previous!=owner) return SWMM_ERR_BADPARAM;
                if(in.kind==0) { bc->clearRuntime(slot); concentrations->erase(slot); continue; }
                bc->saveRuntime(slot); bc->runtime_owner[slot]=owner;
                bc->ensureReceipt(slot,tr.n_species);
                bc->edge_bc_type[slot]=in.kind==1 ? SWMM_2D_BC_SPECIFIED_STAGE : SWMM_2D_BC_SPECIFIED_FLOW;
                if(in.kind==1) {
                    bc->edge_bc_head[slot]=in.value; bc->edge_bc_tseries[slot]=-1; bc->edge_bc_tseries_name[slot].clear();
                } else {
                    bc->edge_bc_flow[slot]=in.value/mesh.edge_length[slot];
                    bc->edge_bc_flow_tseries[slot]=-1; bc->edge_bc_flow_tseries_name[slot].clear();
                    bc->edge_bc_rating_curve[slot]=-1; bc->edge_bc_rating_curve_name[slot].clear();
                }
                auto& tuple=(*concentrations)[slot]; tuple.clear();
                if(in.species_count) tuple.assign(in.concentrations,in.concentrations+in.species_count);
            } else {
                auto& store=editSources(1); const auto id=boundaryId(in.cell,in.edge);
                const auto* old=store.find(id,in.cell);
                if(old && old->enabled && old->owner!=owner) return SWMM_ERR_BADPARAM;
                if(in.kind==0) { store.clear(id,in.cell); continue; }
                RuntimeSource row; row.id=id; row.cell=in.cell; row.owner=owner; row.boundary_edge=in.edge;
                if(in.kind==1) row.boundary_head=in.value; else row.flow=-in.value;
                row.rates.assign(in.species_count,0.0);
                if(in.species_count) row.concentrations.assign(in.concentrations,in.concentrations+in.species_count);
                store.replace(std::move(row));
            }
        }
        for(int k=0;k<f->forcing_count;++k) {
            const auto& in=f->forcings[k];
            if(in.struct_size!=sizeof(in) || in.channel<1 || in.channel>4 || in.mode<0 || in.mode>2 ||
               in.cell< -1 || in.cell>=nc || !std::isfinite(in.rate_m_s) || in.rate_m_s<0.0 ||
               !std::isfinite(in.until_seconds) || (in.until_seconds && in.until_seconds<=now) ||
               ((in.channel>=3) && in.mode==2) || (in.channel==3 && in.cell<0) ||
               ((in.channel==2 || in.channel==3 || in.mode==0) && in.species_count!=0)) return SWMM_ERR_BADPARAM;
            if(!forcing_keys.emplace(in.channel,in.cell).second) return SWMM_ERR_BADPARAM;
            if(in.channel==3) {
                if((rc=cellCheck(*eng,1,in.cell))) return rc;
                const auto& names=router.subsurface().transport().row_names;
                for(const auto& name:tr.row_names) if(std::find(names.begin(),names.end(),name)==names.end()) return SWMM_ERR_BADPARAM;
            }
            if(in.species_count && (rc=quality(*eng,0,in.concentrations,in.species_count))) return rc;
            if(in.channel==4 && in.mode && in.species_count!=tr.n_species) return SWMM_ERR_BADPARAM;
            if(!forcing) forcing=router.state().runtime_forcings;
            const auto conflicts=[&](const RuntimeForcing* old) {
                return old && old->enabled && old->owner!=owner &&
                    (old->channel==in.channel ||
                     (old->channel==4 && in.channel==1 && in.species_count) ||
                     (old->channel==1 && !old->concentrations.empty() && in.channel==4));
            };
            const int related=in.channel==1 ? 4 : in.channel==4 ? 1 : in.channel;
            for(const int channel:{in.channel,related}) {
                if(in.cell>=0) {
                    if(conflicts(forcing->find(channel,in.cell)) || conflicts(forcing->find(channel,-1))) return SWMM_ERR_BADPARAM;
                } else {
                    const auto found=forcing->index.find(channel);
                    if(found!=forcing->index.end()) for(const auto& entry:found->second)
                        if(conflicts(&forcing->rows[entry.second])) return SWMM_ERR_BADPARAM;
                }
                if(related==in.channel) break;
            }
            RuntimeForcing row; row.channel=in.channel; row.cell=in.cell; row.mode=in.mode;
            row.rate=in.rate_m_s; row.owner=owner; row.enabled=in.mode!=0;
            if(in.until_seconds) row.until=in.until_seconds;
            if(in.species_count) row.concentrations.assign(in.concentrations,in.concentrations+in.species_count);
            row.receipt.requested_mass.assign(tr.n_species,0.0); row.receipt.applied_mass.assign(tr.n_species,0.0);
            forcing->replace(std::move(row));
        }
        for(int d=0;d<2;++d) if(next[d]) next[d]->rebuild(nc);
        if(forcing) forcing->rebuild(nc);
        // All validation/allocation precedes the barrier. Preserve cumulative
        // physics belonging to the old prescription, then commit by moves.
        router.flushPendingBatch(eng->context());
        for(int d=0;d<2;++d) if(next[d]) next[d]->syncReceipts(sources(*eng,d));
        if(bc) {
            for(auto& entry:bc->receipts) {
                auto old=router.boundary().receipts.find(entry.first);
                if(old!=router.boundary().receipts.end()) entry.second=old->second;
            }
            bc->edge_bc_cum_flux=router.boundary().edge_bc_cum_flux;
        }
        if(forcing) for(auto& row:forcing->rows)
            if(auto* old=router.state().runtime_forcings.find(row.channel,row.cell)) row.receipt=old->receipt;
        for(int d=0;d<2;++d) if(next[d]) sources(*eng,d)=std::move(*next[d]);
        if(bc) {
            router.boundary()=std::move(*bc); tr.runtime_bc_conc=std::move(*concentrations);
            router.invalidateBoundaryNames(); router.invalidateBoundaryIndex();
        }
        if(forcing) router.state().runtime_forcings=std::move(*forcing);
        if(f->source_count || f->boundary_count || f->forcing_count || f->clear_count) eng->context().runtime_coupling_used=true;
        return SWMM_OK;
    } catch(const std::bad_alloc&) { return SWMM_ERR_NOMEM; }
      catch(...) { return SWMM_ERR_INTERNAL; }
}
int swmm_coupling_set_sources(SWMM_Engine h,const SWMM_CouplingSource* rows,int count) {
    const SWMM_CouplingFrame f{sizeof(f),1,nullptr,count,rows,0,nullptr,0,nullptr,0,nullptr};
    return swmm_coupling_apply_frame(h,&f);
}
int swmm_coupling_set_source(SWMM_Engine h,const char* id,int domain,int cell,double flow,double heat,
    const double* conc,const double* rates,int ns,double until) {
    const SWMM_CouplingSource s{sizeof(s),id,domain,cell,flow,heat,until,ns,conc,rates};
    return swmm_coupling_set_sources(h,&s,1);
}
int swmm_2d_set_runtime_boundary(SWMM_Engine h,int cell,int edge,int kind,double value,const double* conc,int ns) {
    const SWMM_CouplingBoundary b{sizeof(b),0,cell,edge,kind,value,ns,conc};
    const SWMM_CouplingFrame f{sizeof(f),1,nullptr,0,nullptr,1,&b,0,nullptr,0,nullptr};
    return swmm_coupling_apply_frame(h,&f);
}
int swmm_gw2d_set_runtime_boundary(SWMM_Engine h,int cell,int edge,int kind,double value,const double* conc,int ns) {
    const SWMM_CouplingBoundary b{sizeof(b),1,cell,edge,kind,value,ns,conc};
    const SWMM_CouplingFrame f{sizeof(f),1,nullptr,0,nullptr,1,&b,0,nullptr,0,nullptr};
    return swmm_coupling_apply_frame(h,&f);
}
int swmm_gw2d_clear_runtime_boundary(SWMM_Engine h,int cell,int edge) {
    return swmm_gw2d_set_runtime_boundary(h,cell,edge,0,0.0,nullptr,0);
}
int swmm_coupling_clear_source_cell(SWMM_Engine h,int d,const char* id,int cell) {
    const SWMM_CouplingClear c{d,cell,id};
    const SWMM_CouplingFrame f{sizeof(f),1,nullptr,0,nullptr,0,nullptr,0,nullptr,1,&c};
    return swmm_coupling_apply_frame(h,&f);
}
int swmm_coupling_clear_source(SWMM_Engine h,int d,const char* id) {
    return swmm_coupling_clear_source_cell(h,d,id,-1);
}
int swmm_coupling_get_receipt(SWMM_Engine h,int d,const char* id,int cell,SWMM_CouplingReceipt* out,double* req,double* app,int ns) {
    try {
    auto* eng=reinterpret_cast<SWMMEngine*>(h); int rc=readReady(eng,d); if(rc) return rc;
    if(!id || (rc=outputCheck(*eng,d,out,req,app,ns))) return SWMM_ERR_BADPARAM;
    const auto* row=sources(*eng,d).find(id,cell); if(!row) return SWMM_ERR_BADINDEX;
    receiptOut(row->receipt,*out,req,app); return SWMM_OK;
    } catch(const std::bad_alloc&) { return SWMM_ERR_NOMEM; }
      catch(...) { return SWMM_ERR_INTERNAL; }
}
int swmm_coupling_get_receipts(SWMM_Engine h,int d,const char* id,const int* cells,int count,
    SWMM_CouplingReceipt* out,double* req,double* app,int ns) {
    try {
    auto* eng=reinterpret_cast<SWMMEngine*>(h); int rc=readReady(eng,d); if(rc) return rc;
    if(!id || count<0 || ns!=species(*eng,d) || (count && (!cells || !out || (ns && (!req || !app))))) return SWMM_ERR_BADPARAM;
    for(int k=0;k<count;++k) if(!sources(*eng,d).find(id,cells[k])) return SWMM_ERR_BADINDEX;
    for(int k=0;k<count;++k) receiptOut(sources(*eng,d).find(id,cells[k])->receipt,out[k],ns ? req+k*ns : nullptr,ns ? app+k*ns : nullptr);
    return SWMM_OK;
    } catch(const std::bad_alloc&) { return SWMM_ERR_NOMEM; }
      catch(...) { return SWMM_ERR_INTERNAL; }
}
int swmm_coupling_get_boundary_receipt(SWMM_Engine h,int d,int cell,int edge,SWMM_CouplingReceipt* out,double* req,double* app,int ns) {
    try {
    auto* eng=reinterpret_cast<SWMMEngine*>(h); int rc=readReady(eng,d); if(rc) return rc;
    if((rc=edgeCheck(*eng,d,cell,edge)) || (rc=outputCheck(*eng,d,out,req,app,ns))) return rc;
    if(d==0) {
        const auto& receipts=eng->surfaceRouter2D().boundary().receipts;
        const auto found=receipts.find(MeshData::slot(cell,edge));
        if(found!=receipts.end()) { receiptOut(found->second,*out,req,app); return SWMM_OK; }
    } else {
        const auto* row=sources(*eng,1).find(boundaryId(cell,edge),cell);
        if(row) {
            receiptOut(row->receipt,*out,req,app);
            const int temp=eng->surfaceRouter2D().subsurface().transport().temp_row;
            out->requested_heat_j=temp>=0 ? req[temp]*runtimeWaterHeatCapacity : 0.0;
            out->applied_heat_j=temp>=0 ? app[temp]*runtimeWaterHeatCapacity : 0.0;
            out->rejected_heat_j=out->requested_heat_j-out->applied_heat_j;
            return SWMM_OK;
        }
    }
    *out={}; if(ns) { std::fill_n(req,ns,0.0); std::fill_n(app,ns,0.0); } return SWMM_OK;
    } catch(const std::bad_alloc&) { return SWMM_ERR_NOMEM; }
      catch(...) { return SWMM_ERR_INTERNAL; }
}
int swmm_coupling_get_infiltration_receipt(SWMM_Engine h,int cell,SWMM_CouplingReceipt* out,double* req,double* app,int ns) {
    auto* eng=reinterpret_cast<SWMMEngine*>(h); int rc=readReady(eng,0); if(rc) return rc;
    if((rc=cellCheck(*eng,1,cell)) || (rc=outputCheck(*eng,0,out,req,app,ns))) return rc;
    auto* row=eng->surfaceRouter2D().state().runtime_forcings.find(3,cell);
    if(!row) return SWMM_ERR_BADINDEX;
    receiptOut(row->receipt,*out,req,app); return SWMM_OK;
}
int swmm_gw2d_get_runtime_boundary_flow(SWMM_Engine h,int cell,int edge,double* flow) {
    try {
    auto* eng=reinterpret_cast<SWMMEngine*>(h); int rc=readReady(eng,1); if(rc) return rc;
    if(!flow) return SWMM_ERR_BADPARAM; if((rc=edgeCheck(*eng,1,cell,edge))) return rc;
    const auto* row=sources(*eng,1).find(boundaryId(cell,edge),cell);
    *flow=row && row->enabled ? -row->receipt.last_flow : 0.0; return SWMM_OK;
    } catch(const std::bad_alloc&) { return SWMM_ERR_NOMEM; }
      catch(...) { return SWMM_ERR_INTERNAL; }
}
int swmm_2d_get_species_ledger(SWMM_Engine h,int s,int term,double* value) {
    auto* eng=reinterpret_cast<SWMMEngine*>(h); int rc=readReady(eng,0); if(rc) return rc;
    const auto& tr=eng->surfaceRouter2D().state().transport;
    if(!value || s<0 || s>=tr.n_species || term<0 || term>9) return SWMM_ERR_BADPARAM;
    if(term==0) { *value=0.0; for(int c=0;c<tr.n_cells;++c) *value+=tr.cell_mass[tr.idx(s,c)]; }
    else {
        const std::vector<double>* rows[]={&tr.gained_external,&tr.lost_external,&tr.gained_boundary,&tr.lost_boundary,
            &tr.gained_rainfall,&tr.lost_infiltration,&tr.gained_coupling,&tr.lost_coupling,&tr.gained_exfiltration};
        *value=(*rows[term-1])[s];
    }
    return SWMM_OK;
}
int swmm_coupling_get_water_totals(SWMM_Engine h,int d,double* incoming,double* outgoing) {
    auto* eng=reinterpret_cast<SWMMEngine*>(h); int rc=readReady(eng,d); if(rc) return rc;
    if(!incoming || !outgoing) return SWMM_ERR_BADPARAM;
    *incoming=*outgoing=0.0;
    for(const auto& row:sources(*eng,d).rows) { *incoming+=row.receipt.water_in; *outgoing+=row.receipt.water_out; }
    return SWMM_OK;
}
int swmm_coupling_advance_to(SWMM_Engine h,double target,double* actual) {
    auto* eng=reinterpret_cast<SWMMEngine*>(h); return eng ? eng->advanceTo(target,actual) : SWMM_ERR_BADHANDLE;
}
int swmm_coupling_capabilities(SWMM_Engine h,unsigned* flags) {
    auto* eng=reinterpret_cast<SWMMEngine*>(h); if(!eng) return SWMM_ERR_BADHANDLE; if(!flags) return SWMM_ERR_BADPARAM;
    *flags=0; auto& r=eng->surfaceRouter2D();
    if(r.runtimeCpuAvailable()) {
        *flags=1|8|64|128|512;
        if(r.subsurface().active()) *flags|=2|32|256;
        if(r.state().transport.temp_row>=0) *flags|=4;
        if(r.subsurface().active() && r.subsurface().transport().temp_row>=0) *flags|=16;
    }
    return SWMM_OK;
}
}
