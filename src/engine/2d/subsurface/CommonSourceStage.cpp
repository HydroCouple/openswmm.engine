// SPDX-License-Identifier: Apache-2.0
#include "CommonSourceStage.hpp"
#include "SubsurfaceSolver.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../solver/SurfaceFluxCalculator.hpp"
#include "../solver/InertialKernels.hpp"
#include "../../core/SimulationContext.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace openswmm::twoD {
namespace {
constexpr double ft2=.3048*.3048,ft3=ft2*.3048;
bool closeArea(double a,double b) { return std::abs(a-b)<=1e-8*std::max(1.0,std::max(a,b)); }
using Key=std::tuple<SurfaceDonorKind,std::string,int>;
Key key(const SurfaceIntakeRequest& r) { return {r.kind,r.source,r.unit}; }
double bottomTotal(const runoff::SourceWaterDriver& s,int sc) {
    double v=0;
    for(const auto& [t,u]:s.lids().usageOrder()) {
        const auto& g=s.lids().group(t);
        if(g.subcatch_idx[u]==sc) v+=g.wb_infil[u]*g.area[u]*ft3;
    }
    return v;
}
}

std::string CommonSourceStage::initialize(const MeshData& mesh,SurfaceStateData& surface,
    const SolverOptions2D& options,SubsurfaceSolver& gw,runoff::SourceWaterDriver& sources,const SurfaceOwnershipPreview& preview) {
    if(mesh_) return "Common source stage already initialized.";
    if(!gw.active() || !preview.valid() || preview.cancelled || preview.mesh_weather_area.size()!=mesh.tri_area.size())
        return "Common source stage requires a valid reviewed mesh/receiver snapshot.";
    if(surface.transport.n_species>0 || gw.transport().active()) return "Common source stage quality/age/heat/MSX is not qualified.";
    if(surface.runtime_sources.active() || surface.runtime_forcings.active() || gw.runtimeSources().active())
        return "Common source stage runtime coupling providers are not qualified.";
    if(surface.volume.size()!=mesh.tri_area.size() || gw.state().n_cells!=mesh.n_cells())
        return "Common source stage mesh/receiver sizes differ.";
    for(const auto& g:sources.clocks().groups()) if(g.completed_end!=0 || !g.pending.empty())
        return "Common source stage must attach at the initial source clock.";
    const auto& c=sources.context(); const auto& r=sources.runoff().soa();
    std::vector<Donor> donors;std::vector<bool> owned(mesh.tri_area.size(),false);
    std::vector<double> coverage(mesh.tri_area.size(),0);
    std::vector<bool> reviewed(c.n_subcatches(),false);
    for(const auto& o:preview.objects) {
        if(!o.reviewed) continue;
        const int sc=o.subcatch;
        if(sc<0 || sc>=c.n_subcatches() || sources.clocks().groupForSubcatch(sc)<0 || o.lumped || o.outside_area>1e-8)
            return "Common source stage requires fully inside, non-lumped sources in its clock groups.";
        if(reviewed[sc]) return "Duplicate reviewed common source.";
        reviewed[sc]=true;
        Donor d;d.source=sc;d.area=r.area[sc]*(1-r.imperv_pct[sc])*ft2;
        double weather=0;
        for(const auto& share:preview.shares) if(share.subcatch==sc) {
            if(share.cell<0 || share.cell>=mesh.n_cells() || share.weather_area<0 || share.pervious_area<0)
                return "Invalid common source contact.";
            owned[share.cell]=true;coverage[share.cell]+=share.weather_area;weather+=share.weather_area;
            if(share.pervious_area>0) d.contacts.emplace_back(share.cell,share.pervious_area);
        }
        double area=0;for(const auto& x:d.contacts) area+=x.second;
        if(!closeArea(area,d.area) || !closeArea(weather,(r.area[sc]+c.subcatches.total_lid_area_ft2[sc])*ft2))
            return "Reviewed source shares do not match private physical water areas.";
        if(d.area>0) donors.push_back(d);
        for(std::size_t id=0;id<sources.lids().usageOrder().size();++id) {
            const auto [t,u]=sources.lids().usageOrder()[id];const auto& g=sources.lids().group(t);
            if(g.subcatch_idx[u]!=sc || g.stor_ksat[u]<=0) continue;
            Donor l;l.source=sc;l.type=t;l.unit=u;l.identity=int(id);l.area=g.area[u]*ft2;
            for(const auto& share:preview.shares) if(share.subcatch==sc && share.lid_area>0)
                l.contacts.emplace_back(share.cell,share.lid_area*g.area[u]/c.subcatches.total_lid_area_ft2[sc]);
            double total=0;for(const auto& x:l.contacts) total+=x.second;
            if(!closeArea(total,l.area)) return "Reviewed LID bottom shares do not match its unit footprint.";
            donors.push_back(std::move(l));
        }
    }
    for(const auto& g:sources.clocks().groups()) for(int sc:g.subcatches) if(!reviewed[sc])
        return "Every member of a common source clock group needs reviewed inside shares.";
    std::vector<std::vector<std::pair<int,double>>> et_contacts;
    std::vector<double> atmospheric_area(mesh.tri_area.size(),0);
    for(const auto& b:sources.atmosphere()) {
        std::vector<std::pair<int,double>> contacts;double total=0;
        for(const auto& sh:preview.shares) if(sh.subcatch==b.source) {
            double area=b.kind==runoff::SourceEtKind::PERVIOUS ? sh.pervious_area : sh.impervious_area;
            if(b.kind==runoff::SourceEtKind::LID)
                area=sh.lid_area*b.area/c.subcatches.total_lid_area_ft2[b.source];
            if(!std::isfinite(area)||area<0) return "Invalid common atmospheric contact area.";
            if(area>0) { contacts.emplace_back(sh.cell,area);total+=area;atmospheric_area[sh.cell]+=area; }
        }
        if(!closeArea(total,b.area*ft2)) return "Reviewed atmospheric shares do not match private component footprints.";
        et_contacts.push_back(std::move(contacts));
    }
    std::vector<int> cells;
    for(int i=0;i<mesh.n_cells();++i) if(owned[i]) {
        if(static_cast<GwClosure>(gw.state().closure[i])==GwClosure::SIGMA)
            return "Common source SIGMA table/column conservation must be qualified before attachment.";
        const double w=preview.mesh_weather_area[i];
        if(!std::isfinite(w) || w<0 || !closeArea(coverage[i]+w,mesh.tri_area[i]))
            return "Reviewed mesh/source weather areas do not close.";
        if(!closeArea(atmospheric_area[i],coverage[i])) return "Reviewed atmospheric components do not close the cell's owned weather area.";
        double contact=w;for(const auto& d:donors) for(const auto& x:d.contacts) if(x.first==i) contact+=x.second;
        if(contact>mesh.tri_area[i]*(1+1e-8)) return "Common source contact areas overbook the cell.";
        cells.push_back(i);
    }
    if(cells.empty()) return "Common source stage has no reviewed receiving cells.";
    mesh_=&mesh;surface_=&surface;options_=&options;gw_=&gw;sources_=&sources;donors_=std::move(donors);
    cells_=std::move(cells);owned_=std::move(owned);weather_=preview.mesh_weather_area;
    et_contacts_=std::move(et_contacts);
    return {};
}

std::string CommonSourceStage::advance(double start,double end) {
    if(!mesh_ || !std::isfinite(end) || end<=start || start!=completedEnd()) return "Invalid common source interval.";
    const double dt=end-start;
    for(const auto& g:sources_->clocks().groups()) if(g.completed_end!=start || !g.pending.empty())
        return "Common source clocks do not match the accepted mesh source interval.";
    if(surface_->transport.n_species>0 || gw_->transport().active()) return "Common source transport changed to an unsupported profile.";
    if(surface_->runtime_sources.active() || surface_->runtime_forcings.active() || gw_->runtimeSources().active())
        return "Common source runtime provider profile changed to an unsupported configuration.";
    struct MeshTrial { int cell;double rain,evap,out,incoming,candidate; };
    std::vector<MeshTrial> mesh_trials;
    std::vector<SurfaceIntakeRequest> requests;
    std::map<Key,int> donor_index;
    // Mesh requests are water-limited after its competing evaporation/outflow.
    for(int i:cells_) {
        if(!std::isfinite(surface_->volume[i]) || surface_->volume[i]<0 ||
           !std::isfinite(surface_->rainfall[i]) || !std::isfinite(surface_->evap_rate[i]) ||
           !std::isfinite(surface_->coupling_flux[i]) || !std::isfinite(surface_->depth[i]))
            return "Common source mesh water/forcing is invalid.";
        if(gw_->state().xacc_to_surface[i]!=0) return "Common source stage needs qualified source refunds/Dunne ownership before surface returns.";
        const double A=mesh_->tri_area[i],w=weather_[i];
        const double rain=std::max(0.0,surface_->rainfall[i])*w*dt;
        const double coupling=surface_->coupling_flux[i]*A*dt;
        const double incoming=rain+std::max(coupling,0.0);
        const double available=std::max(0.0,surface_->volume[i]+incoming);
        const double evap=evapSink(surface_->evap_rate[i],surface_->depth[i],options_->dry_depth)*w*dt;
        const double out=std::max(-coupling,0.0);
        const double cap=gw_->sourceInfiltrationCapacity(i,std::max(0.0,surface_->depth[i]),dt)*w*dt;
        const double sum=cap+evap+out,scale=sum>available && sum>0 ? available/sum : 1;
        mesh_trials.push_back({i,rain,evap*scale,out*scale,incoming,cap*scale});
        if(w>0) requests.push_back({SurfaceDonorKind::MESH,"cell:"+std::to_string(i),-1,i,w,
            std::max(0.0,surface_->depth[i]),cap*scale,cap*scale});
    }
    auto proposal=sources_->waterTrial();
    const auto findDonor=[&](int sc,int t=-1,int u=-1)->int {
        for(std::size_t j=0;j<donors_.size();++j) if(donors_[j].source==sc && donors_[j].type==t && donors_[j].unit==u) return int(j);
        return -1;
    };
    std::vector<double> pond(donors_.size(),1e30);
    const auto capacity=[&](int j,double p) {
        pond[j]=std::min(pond[j],p);double total=0;
        for(const auto& [cell,area]:donors_[j].contacts) total+=gw_->sourceInfiltrationCapacity(cell,p,dt)*area;
        return total/donors_[j].area/.3048;
    };
    runoff::RunoffSolver::InfiltrationBoundary candidate=[&](int sc,double p,double,double& rate) {
        const int j=findDonor(sc);if(j<0) return false;rate=capacity(j,p*.3048);return true;
    };
    runoff::SourceWaterDriver::BottomCeiling bottom=[&](int t,int u,double,double) {
        const auto& g=proposal.lids(true).group(t);const int j=findDonor(g.subcatch_idx[u],t,u);
        return j<0 ? 1e10 : capacity(j,std::max(0.0,g.stor_depth[u])*.3048);
    };
    for(std::size_t group=0;group<proposal.clocks().groups().size();++group) {
        auto error=proposal.stage(int(group),end,end,&candidate,&bottom);
        if(!error.empty()) return error;
        error=proposal.commit();if(!error.empty()) return error;
    }
    const auto donorVolume=[&](const runoff::SourceWaterDriver& s,int j) {
        const auto& d=donors_[j];
        if(d.type>=0) {
            const auto& a=s.lids().group(d.type);const auto& b=sources_->lids().group(d.type);
            return (a.wb_infil[d.unit]-b.wb_infil[d.unit])*a.area[d.unit]*ft3;
        }
        return (s.ledgers()[d.source].infiltration-sources_->ledgers()[d.source].infiltration)*ft3-
            (bottomTotal(s,d.source)-bottomTotal(*sources_,d.source));
    };
    for(std::size_t j=0;j<donors_.size();++j) {
        const auto& d=donors_[j];const double volume=std::max(0.0,donorVolume(proposal,int(j)));
        const double p=pond[j]==1e30 ? 0 : pond[j];double total=0;
        for(const auto& [cell,area]:d.contacts) total+=gw_->sourceInfiltrationCapacity(cell,p,dt)*area;
        const auto kind=d.type<0 ? SurfaceDonorKind::NON_LID : SurfaceDonorKind::LID_BOTTOM;
        const auto name=sources_->context().subcatch_names.name_of(d.source);
        donor_index[{kind,name,d.identity}]=int(j);
        for(const auto& [cell,area]:d.contacts) {
            const double weight=total>0 ? gw_->sourceInfiltrationCapacity(cell,p,dt)*area/total : 0;
            requests.push_back({kind,name,d.identity,cell,area,p,volume,volume*weight});
        }
    }
    auto error=exchange_.plan(*gw_,start,end,end,requests);if(!error.empty()) return error;
    std::vector<double> awarded(donors_.size(),0);
    std::map<int,double> mesh_awarded;
    for(const auto& a:exchange_.awards()) {
        if(a.request.kind==SurfaceDonorKind::MESH) mesh_awarded[a.request.cell]+=a.maximum;
        else awarded[donor_index.at(key(a.request))]+=a.maximum;
    }
    auto bounded=sources_->waterTrial();
    const auto rate=[&](int j) { return std::nextafter(awarded[j]/donors_[j].area/.3048/dt,0.0); };
    runoff::RunoffSolver::InfiltrationBoundary limit=[&](int sc,double,double,double& q) {
        const int j=findDonor(sc);if(j<0) return false;q=rate(j);return true;
    };
    runoff::SourceWaterDriver::BottomCeiling bottom_limit=[&](int t,int u,double,double) {
        const int j=findDonor(bounded.lids(true).group(t).subcatch_idx[u],t,u);return j<0 ? 1e10 : rate(j);
    };
    for(std::size_t group=0;group<bounded.clocks().groups().size();++group) {
        error=bounded.stage(int(group),end,end,&limit,&bottom_limit);
        if(error.empty()) error=bounded.commit();
        if(!error.empty()) { exchange_.cancel();return error; }
    }
    std::vector<SurfaceIntakeActual> actual;std::vector<double> consumed(donors_.size(),0),processed(donors_.size(),0);
    for(const auto& a:exchange_.awards()) {
        double volume=a.maximum;
        if(a.request.kind!=SurfaceDonorKind::MESH) {
            const int j=donor_index.at(key(a.request));const double taken=std::max(0.0,donorVolume(bounded,j));
            if(taken>awarded[j]+1e-12) { exchange_.cancel();return "Bounded source exceeded its shared award."; }
            // Spread actual withdrawal over its original awards. Denied cell
            // capacity never migrates; only FP residue lands on the last share.
            processed[j]+=a.maximum;
            volume=awarded[j]>0 ? std::min({a.maximum,std::max(0.0,taken-consumed[j]),
                processed[j]>=awarded[j] ? std::max(0.0,taken-consumed[j]) : taken*a.maximum/awarded[j]}) : 0;
            consumed[j]+=volume;
        }
        actual.push_back({volume,{}});
    }
    for(std::size_t j=0;j<donors_.size();++j) if(std::abs(consumed[j]-donorVolume(bounded,int(j)))>1e-12) {
        exchange_.cancel();return "Common source actual receipts do not match donor withdrawal.";
    }
    std::vector<SurfaceEtReceipt> et_receipts;
    struct EtBudget { double potential=0,evaporation=0,soil=0; };
    std::vector<EtBudget> et_budget(mesh_->n_cells());
    const auto addEt=[&](SurfaceEtReceipt r,bool eligible) {
        const double remainder=std::max(0.0,r.potential-r.evaporation);
        r.soil_demand=eligible ? remainder : 0;r.unused=eligible ? 0 : remainder;
        auto& b=et_budget[r.cell];b.potential+=r.potential;b.evaporation+=r.evaporation;b.soil+=r.soil_demand;
        et_receipts.push_back(r);
    };
    for(const auto& m:mesh_trials) addEt({SurfaceEtOwner::MESH,-1,-1,-1,m.cell,start,end,weather_[m.cell],
        std::max(0.0,surface_->evap_rate[m.cell])*weather_[m.cell]*dt,m.evap},true);
    for(std::size_t j=0;j<bounded.atmosphere().size();++j) {
        const auto& a=bounded.atmosphere()[j];const auto& b=sources_->atmosphere()[j];
        const auto owner=a.kind==runoff::SourceEtKind::PERVIOUS ? SurfaceEtOwner::PERVIOUS :
            a.kind==runoff::SourceEtKind::IMPERVIOUS ? SurfaceEtOwner::IMPERVIOUS : SurfaceEtOwner::LID;
        for(const auto& [cell,area]:et_contacts_[j]) {
            const double fraction=area/(a.area*ft2);
            addEt({owner,a.source,a.type,a.unit,cell,start,end,area,
                (a.potential-b.potential)*ft3*fraction,(a.evaporation-b.evaporation)*ft3*fraction},a.soil_eligible);
        }
    }
    // Validate all atmospheric volumes before either receiver or owner changes.
    for(int i:cells_) {
        const auto& b=et_budget[i];
        if(!std::isfinite(b.potential)||!std::isfinite(b.evaporation)||!std::isfinite(b.soil)||
           b.potential<0||b.evaporation<0||b.soil<0||b.evaporation+b.soil>b.potential+1e-12*std::max(1.0,b.potential)) {
            exchange_.cancel();return "Common source atmospheric area budget does not close.";
        }
    }
    // Revalidation and receiver booking happen before any managed source debit.
    error=exchange_.commit(*gw_,actual);if(!error.empty()) { exchange_.cancel();return error; }
    for(const auto& m:mesh_trials) {
        const double infil=mesh_awarded[m.cell],A=mesh_->tri_area[m.cell];
        surface_->volume[m.cell]=std::max(0.0,surface_->volume[m.cell]+m.incoming-m.evap-m.out-infil);
        surface_->infil_applied[m.cell]+=infil/A;
        surface_->coupling_applied[m.cell]+=m.incoming-m.rain-m.out;
        surface_->evap_loss_total+=m.evap;
        const auto& b=et_budget[m.cell];gw_->bookAreaEt(m.cell,dt,b.potential,b.evaporation,b.soil);
        inertial::cellEtaDepth(*mesh_,*options_,m.cell,surface_->volume[m.cell],surface_->head[m.cell],surface_->depth[m.cell]);
        mesh_rain_+=m.rain;mesh_evap_+=m.evap;
    }
    *sources_=std::move(bounded);et_receipts_=std::move(et_receipts);++intervals_;return {};
}
}
