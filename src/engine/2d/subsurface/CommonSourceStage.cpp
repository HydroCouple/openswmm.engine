// SPDX-License-Identifier: Apache-2.0
#include "CommonSourceStage.hpp"
#include "SubsurfaceSolver.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../solver/SurfaceFluxCalculator.hpp"
#include "../solver/InertialKernels.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/CompletedSourceArchive.hpp"
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
        if(g.subcatch_idx[u]==sc) v+=g.wb_spatial_infil[u]*g.area[u]*ft3;
    }
    return v;
}
std::string qualityProfile(const runoff::SourceWaterDriver& sources,
                          const SurfaceStateData& surface,const SubsurfaceSolver& gw) {
    const auto& names=sources.pollutantNames();const int np=int(names.size());
    const auto& a=surface.transport;const auto& b=gw.transport();
    if(np==0) return a.n_species>0 || b.active() ? "Common source stage quality/age/heat/MSX is not qualified." : "";
    if(a.n_species!=np || a.n_pollut!=np || a.n_msx!=0 || a.age_row>=0 || a.temp_row>=0 || a.row_names!=names ||
       b.n_species!=np || b.n_pollut!=np || b.n_msx!=0 || b.age_row>=0 || b.temp_row>=0 || b.row_names!=names ||
       a.n_cells!=gw.state().n_cells || b.n_cells!=gw.state().n_cells ||
       a.cell_mass.size()!=std::size_t(np*a.n_cells) || a.gained_rainfall.size()!=std::size_t(np) ||
       a.lost_infiltration.size()!=std::size_t(np) || a.gained_exfiltration.size()!=std::size_t(np) || (!a.rain_conc.empty() && a.rain_conc.size()!=std::size_t(np)) ||
       b.xacc_from_surface.size()!=std::size_t(np*b.n_cells) || b.xacc_to_surface.size()!=std::size_t(np*b.n_cells))
        return "Common source pollutant layout must match the source pollutant IDs; age/heat/MSX are not qualified.";
    for(double concentration:a.rain_conc) if(!std::isfinite(concentration) || concentration<0)
        return "Common source mesh rainfall concentration is invalid.";
    return {};
}
}

std::string CommonSourceStage::initialize(const MeshData& mesh,SurfaceStateData& surface,
    const SolverOptions2D& options,SubsurfaceSolver& gw,runoff::SourceWaterDriver& sources,const SurfaceOwnershipPreview& preview,bool mesh_only,
    const std::string& runtime_water_identity) {
    if(mesh_) return "Common source stage already initialized.";
    if(!gw.active() || !preview.valid() || preview.cancelled || preview.mesh_weather_area.size()!=mesh.tri_area.size())
        return "Common source stage requires a valid reviewed mesh/receiver snapshot.";
    const auto quality_error=qualityProfile(sources,surface,gw);if(!quality_error.empty())return quality_error;
    if(!runtime_water_identity.empty() && (!mesh_only || !sources.pollutantNames().empty() ||
       runtime_water_identity.size()>=64*1024*1024))
        return "Runtime water source stage requires a water-only mesh-owned footprint and bounded replay identity.";
    if(surface.runtime_sources.active() || surface.runtime_forcings.active() || gw.runtimeSources().active())
        return "Common source stage runtime coupling providers are not qualified.";
    if(surface.volume.size()!=mesh.tri_area.size() || gw.state().n_cells!=mesh.n_cells())
        return "Common source stage mesh/receiver sizes differ.";
    for(const auto& g:sources.clocks().groups()) if(g.completed_end!=0 || !g.pending.empty())
        return "Common source stage must attach at the initial source clock.";
    const auto& c=sources.context(); const auto& r=sources.runoff().soa();
    if(mesh_only && (!sources.clocks().groups().empty() || !preview.objects.empty() || !preview.shares.empty()))
        return "Mesh-only source stage cannot own private subcatchment sources.";
    std::vector<Donor> donors;std::vector<bool> owned(mesh.tri_area.size(),mesh_only);
    std::vector<double> coverage(mesh.tri_area.size(),0);
    std::vector<bool> resolved(c.n_subcatches(),false);
    std::vector<double> fractions(c.n_subcatches(),0);
    for(const auto& o:preview.objects) {
        const int sc=o.subcatch;
        if(sc<0 || sc>=c.n_subcatches())return "Invalid reviewed common source.";
        if(sources.clocks().groupForSubcatch(sc)<0) {
            if(o.reviewed)return "Reviewed common source is outside its clock groups.";
            continue;
        }
        if(o.lumped)return "Common source stage lumped groundwater is not qualified.";
        if(resolved[sc])return "Duplicate common source footprint.";
        resolved[sc]=true;
        const double full=(r.area[sc]+c.subcatches.total_lid_area_ft2[sc])*ft2;
        if(!std::isfinite(full)||full<=0)return "Common source has no physical footprint.";
        Donor d;d.source=sc;
        double weather=0,pervious=0,impervious=0,lid_area=0;
        for(const auto& share:preview.shares)if(share.subcatch==sc) {
            if(share.cell<0 || share.cell>=mesh.n_cells())return "Invalid common source contact.";
            for(double area:{share.weather_area,share.pervious_area,share.impervious_area,share.lid_area})
                if(!std::isfinite(area)||area<0)return "Invalid common source contact area.";
            if(!closeArea(share.weather_area,share.pervious_area+share.impervious_area+share.lid_area))
                return "Reviewed component areas do not close their weather footprint.";
            if(share.weather_area>0){owned[share.cell]=true;coverage[share.cell]+=share.weather_area;}
            weather+=share.weather_area;pervious+=share.pervious_area;
            impervious+=share.impervious_area;lid_area+=share.lid_area;
            if(share.pervious_area>0)d.contacts.emplace_back(share.cell,share.pervious_area);
        }
        if(weather>0 && !o.reviewed)return "Inside source shares require reviewed uniform ownership.";
        if(!std::isfinite(o.inside_area)||o.inside_area<0||!closeArea(weather,o.inside_area)||
           !std::isfinite(o.outside_area)||o.outside_area<0||!closeArea(weather+o.outside_area,full)||
           (o.declared_area>0&&!closeArea(o.declared_area,full)))
            return "Reviewed inside/outside shares do not match private physical water areas.";
        fractions[sc]=o.outside_area==0 ? 1.0 : weather==0 ? 0.0 : std::clamp(weather/full,0.0,1.0);
        if(!closeArea(pervious,r.area[sc]*(1-r.imperv_pct[sc])*ft2*fractions[sc])||
           !closeArea(impervious,r.area[sc]*r.imperv_pct[sc]*ft2*fractions[sc])||
           !closeArea(lid_area,c.subcatches.total_lid_area_ft2[sc]*ft2*fractions[sc]))
            return "Reviewed components do not match uniform inside/outside footprints.";
        d.area=pervious;if(d.area>0)donors.push_back(d);
        for(std::size_t id=0;id<sources.lids().usageOrder().size();++id) {
            const auto [t,u]=sources.lids().usageOrder()[id];const auto& g=sources.lids().group(t);
            if(g.subcatch_idx[u]!=sc || !lid::hasNativeBottom(g.type,g.stor_thick[u],g.stor_ksat[u]) || fractions[sc]==0)continue;
            Donor l;l.source=sc;l.type=t;l.unit=u;l.identity=int(id);l.area=g.area[u]*ft2*fractions[sc];
            for(const auto& share:preview.shares)if(share.subcatch==sc && share.lid_area>0)
                l.contacts.emplace_back(share.cell,share.lid_area*g.area[u]/c.subcatches.total_lid_area_ft2[sc]);
            double total=0;for(const auto& x:l.contacts)total+=x.second;
            if(!closeArea(total,l.area))return "Reviewed LID bottom shares do not match its inside footprint.";
            if(l.area>0)donors.push_back(std::move(l));
        }
    }
    for(const auto& g:sources.clocks().groups())for(int sc:g.subcatches)if(!resolved[sc])
        return "Every member of a common source clock group needs a resolved inside/outside footprint.";
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
        if(!closeArea(total,b.area*ft2*fractions[b.source])) return "Reviewed atmospheric shares do not match private component footprints.";
        et_contacts.push_back(std::move(contacts));
    }
    std::vector<int> cells;
    for(int i=0;i<mesh.n_cells();++i) if(owned[i]) {
        const double w=preview.mesh_weather_area[i];
        if(!std::isfinite(w) || w<0 || !closeArea(coverage[i]+w,mesh.tri_area[i]))
            return "Reviewed mesh/source weather areas do not close.";
        if(!closeArea(atmospheric_area[i],coverage[i])) return "Reviewed atmospheric components do not close the cell's owned weather area.";
        double contact=w;for(const auto& d:donors) for(const auto& x:d.contacts) if(x.first==i) contact+=x.second;
        if(contact>mesh.tri_area[i]*(1+1e-8)) return "Common source contact areas overbook the cell.";
        cells.push_back(i);
    }
    if(cells.empty()) return "Common source stage has no reviewed receiving cells.";
    if(gw.sourceReceiptOwnershipBound())return "Aquifer source receipt ownership already bound.";
    // Both bindings are initial-state operations. Validate receiver queues
    // before changing the source partition so an invalid attachment is retryable.
    for(int i:cells) {
        if(gw.state().xacc_from_surface[i]!=0 || gw.state().xacc_to_surface[i]!=0)
            return "Common source ownership must bind before pending aquifer exchange.";
        for(int p=0;p<gw.transport().n_species;++p)
            if(gw.transport().xacc_from_surface[gw.transport().idx(p,i)]!=0 || gw.transport().xacc_to_surface[gw.transport().idx(p,i)]!=0)
                return "Common source ownership must bind before pending aquifer mass exchange.";
    }
    const auto partition_error=sources.configureSpatialCoverage(fractions);
    if(!partition_error.empty())return partition_error;
    const auto receipt_error=gw.configureSourceReceipts(cells);if(!receipt_error.empty())return receipt_error;
    mesh_=&mesh;surface_=&surface;options_=&options;gw_=&gw;sources_=&sources;donors_=std::move(donors);
    cells_=std::move(cells);owned_=std::move(owned);weather_=preview.mesh_weather_area;
    et_contacts_=std::move(et_contacts);
    runtime_water_identity_=runtime_water_identity;
    checkpoint_model_=checkpointModel();
    return {};
}

RuntimeWaterCheckpointBinding CommonSourceStage::runtimeWaterBinding(double time) const {
    RuntimeWaterCheckpointBinding b;b.identity=checkpoint_model_;b.completed_time=time;
    for(double zs:gw_->state().zs)b.groundwater_cells.push_back(zs>0);
    return b;
}
std::string CommonSourceStage::validateRuntimeWater(const RuntimeSources& surface,const RuntimeSources& groundwater,
    const RuntimeForcings& forcing,double time) const {
    if(!runtimeWaterEnabled())return "Common source runtime providers require their initial water profile.";
    return RuntimeWaterCheckpoint::validate(runtimeWaterBinding(time),surface,groundwater,forcing);
}

std::string CommonSourceStage::checkpointBoundary() const {
    if(!mesh_||!sources_||!gw_||!surface_||exchange_.planned_)return "Checkpoint requires an attached accepted source boundary.";
    for(const auto& group:sources_->clocks().groups())
        if(!group.pending.empty()||group.completed_end!=completedEnd())return "Checkpoint source clocks do not match the accepted boundary.";
    const auto error=qualityProfile(*sources_,*surface_,*gw_);if(!error.empty())return error;
    if(!surface_->transport.runtime_bc_conc.empty())return "Checkpoint runtime boundary quality is not qualified.";
    if(runtimeWaterEnabled()) {
        auto surface=surface_->runtime_sources,groundwater=gw_->runtimeSources();auto forcing=surface_->runtime_forcings;
        surface.expire(completedEnd());groundwater.expire(completedEnd());forcing.expire(completedEnd());
        const auto invalid=validateRuntimeWater(surface,groundwater,forcing,completedEnd());if(!invalid.empty())return invalid;
    } else if(!surface_->runtime_sources.rows.empty()||!surface_->runtime_forcings.rows.empty()||!gw_->runtimeSources().rows.empty())
        return "Checkpoint runtime provider history is not qualified.";
    for(int cell:cells_) {
        if(gw_->sourceCompletedTime(cell)>completedEnd())return "Checkpoint aquifer firing exceeds the accepted source horizon.";
        for(const auto& receipt:gw_->pendingSourceReceipts(cell))if(receipt.end>completedEnd())
            return "Checkpoint intake exceeds the accepted source horizon.";
    }
    // Check required pairing and private balances on copies, on save as well as load.
    try {
        detail::CompletedSourceArchive a;sources_->checkpoint(a,false);
        auto source=sources_->waterTrial();detail::CompletedSourceArchive b(a.bytes());source.checkpoint(b,false);b.finish();
        auto gw=*gw_;detail::CompletedSourceArchive c;gw.checkpoint(c,false);
        detail::CompletedSourceArchive d(c.bytes());gw.checkpoint(d,false);d.finish();
    } catch(const std::exception& e){return e.what();}
    return {};
}

std::string CommonSourceStage::advance(double start,double end) {
    if(!mesh_ || !std::isfinite(end) || end<=start || start!=completedEnd()) return "Invalid common source interval.";
    const double dt=end-start;
    for(const auto& g:sources_->clocks().groups()) if(g.completed_end!=start || !g.pending.empty())
        return "Common source clocks do not match the accepted mesh source interval.";
    const auto quality_error=qualityProfile(*sources_,*surface_,*gw_);if(!quality_error.empty())return quality_error;
    const int np=int(sources_->pollutantNames().size());
    if(runtimeWaterEnabled()) {
        const auto invalid=validateRuntimeWater(surface_->runtime_sources,gw_->runtimeSources(),surface_->runtime_forcings,start);
        if(!invalid.empty())return invalid;
        if(end>std::min({surface_->runtime_sources.nextTime(start),gw_->runtimeSources().nextTime(start),
            surface_->runtime_forcings.nextTime(start)}))return "Common source interval crosses provider expiry.";
    } else if(surface_->runtime_sources.active() || surface_->runtime_forcings.active() || gw_->runtimeSources().active())
        return "Common source runtime provider profile changed to an unsupported configuration.";
    std::vector<SurfaceReturnReceipt> ready_returns;
    std::vector<runoff::SourceSurfaceReturn> source_returns;
    std::vector<double> mesh_return(mesh_->n_cells(),0);
    std::vector<std::vector<double>> mesh_return_mass(mesh_->n_cells(),std::vector<double>(np,0));
    const auto credit=[&](const SurfaceReturnReceipt& r,int source,int type,int unit,int pool,double fraction) {
        if(fraction<=0)return;
        if(source<0) {
            const int cell=r.intake.request.cell;mesh_return[cell]+=r.volume*fraction;
            for(int p=0;p<np;++p)mesh_return_mass[cell][p]+=r.mass[p]*fraction;
        } else {
            runoff::SourceSurfaceReturn q;q.source=source;q.type=type;q.unit=unit;q.pool=pool;
            q.volume=r.volume*fraction/ft3;q.mass.resize(np);
            for(int p=0;p<np;++p)q.mass[p]=r.mass[p]*fraction;
            source_returns.push_back(std::move(q));
        }
    };
    for(const auto& r:gw_->sourceReturns())if(r.available_at<=start) {
        const int cell=r.intake.request.cell;
        if(!ownsCell(cell) || !std::isfinite(r.volume) || r.volume<0 || int(r.mass.size())!=np)
            return "Invalid paired common source return.";
        for(double mass:r.mass)if(!std::isfinite(mass) || mass<0 || (r.volume==0 && mass!=0))
            return "Invalid common source return mass.";
        if(r.kind==SurfaceReturnKind::NUMERICAL_REFUND) {
            const auto& original=r.intake.request;
            if(original.kind==SurfaceDonorKind::MESH)credit(r,-1,-1,-1,2,1);
            else {
                auto donor=std::find_if(donors_.begin(),donors_.end(),[&](const auto& d) {
                    return sources_->context().subcatch_names.name_of(d.source)==original.source &&
                        d.identity==original.unit && (d.type<0)==(original.kind==SurfaceDonorKind::NON_LID);
                });
                if(donor==donors_.end())return "Source refund has no originating donor.";
                credit(r,donor->source,donor->type,donor->unit,2,1);
            }
        } else {
            struct Owner {int source,type,unit,pool;double area;};
            std::vector<Owner> owners;if(weather_[cell]>0)owners.push_back({-1,-1,-1,2,weather_[cell]});
            const auto& atmosphere=sources_->atmosphere();const auto& runoff=sources_->runoff().soa();
            for(std::size_t j=0;j<atmosphere.size();++j)for(const auto& [i,area]:et_contacts_[j])if(i==cell) {
                const auto& b=atmosphere[j];
                if(b.kind==runoff::SourceEtKind::IMPERVIOUS) {
                    const double impervious=runoff.imperv_pct[b.source];
                    for(int pool=0;pool<2;++pool) {
                        const double fraction=pool==0 ? runoff.frac_imperv0[b.source] : runoff.frac_imperv1[b.source];
                        if(fraction>0)owners.push_back({b.source,-1,-1,pool,area*fraction/impervious});
                    }
                } else owners.push_back({b.source,b.type,b.unit,2,area});
            }
            double total=0;for(const auto& o:owners)total+=o.area;
            if(!closeArea(total,mesh_->tri_area[cell]))return "Surface emergence has incomplete local ownership.";
            double remaining=1;
            for(std::size_t j=0;j<owners.size();++j) {
                const auto& o=owners[j];const double fraction=j+1==owners.size() ? remaining : std::min(remaining,o.area/total);
                remaining-=fraction;credit(r,o.source,o.type,o.unit,o.pool,fraction);
            }
        }
        ready_returns.push_back(r);
    }
    struct MeshTrial { int cell;double rain,evap,out,incoming,available,requested_infil,et_rate;std::vector<double> mass,rain_mass; };
    std::vector<MeshTrial> mesh_trials;
    std::vector<int> mesh_index(mesh_->n_cells(),-1);
    std::vector<SurfaceIntakeRequest> requests;
    std::map<Key,int> donor_index;
    // Mesh requests are water-limited after its competing evaporation/outflow.
    for(int i:cells_) {
        if(!std::isfinite(surface_->volume[i]) || surface_->volume[i]<0 ||
           !std::isfinite(surface_->rainfall[i]) || !std::isfinite(surface_->evap_rate[i]) ||
           !std::isfinite(surface_->coupling_flux[i]) || !std::isfinite(surface_->depth[i]))
            return "Common source mesh water/forcing is invalid.";
        if(gw_->state().xacc_to_surface[i]!=0) return "Unpaired generic aquifer return in common source cell.";
        if(np>0) {
            if(surface_->coupling_flux[i]!=0) return "Common LID pollutant pairing has no qualified mesh coupling donor.";
            for(int j=0;j<np;++j) if(gw_->transport().xacc_to_surface[gw_->transport().idx(j,i)]!=0)
                return "Common LID pollutant pairing needs qualified paired source returns.";
        }
        const double A=mesh_->tri_area[i],w=weather_[i];
        const double rain=std::max(0.0,surface_->runtime_forcings.rate(1,i,surface_->rainfall[i]))*w*dt;
        const double coupling=surface_->coupling_flux[i]*A*dt;
        const double incoming=rain+std::max(coupling,0.0)+mesh_return[i];
        const double available=std::max(0.0,surface_->volume[i]+incoming);
        const double et_rate=surface_->runtime_forcings.rate(2,i,surface_->evap_rate[i]);
        const double evap=evapSink(et_rate,surface_->depth[i],options_->dry_depth)*w*dt;
        const double out=std::max(-coupling,0.0);
        const auto* prescribed=surface_->runtime_forcings.get(3,i);
        const double cap=(prescribed ? std::max(0.0,infilSink(prescribed->rate,surface_->depth[i],options_->dry_depth)) :
            gw_->sourceInfiltrationCapacity(i,std::max(0.0,surface_->depth[i]),dt))*w*dt;
        const double sum=cap+evap+out,scale=sum>available && sum>0 ? available/sum : 1;
        MeshTrial trial{i,rain,evap*scale,out*scale,incoming,available,cap,et_rate,std::vector<double>(np,0),std::vector<double>(np,0)};
        const auto& tr=surface_->transport;
        for(int p=0;p<np;++p) {
            const double opening=tr.cell_mass[tr.idx(p,i)];
            trial.rain_mass[p]=rain*(tr.rain_conc.empty() ? 0 : tr.rain_conc[p]);
            trial.mass[p]=opening+trial.rain_mass[p]+mesh_return_mass[i][p];
            if(!std::isfinite(opening) || opening<0 || !std::isfinite(trial.mass[p]))
                return "Common source mesh pollutant mixture is invalid.";
        }
        mesh_index[i]=int(mesh_trials.size());mesh_trials.push_back(std::move(trial));
        if(w>0) requests.push_back({SurfaceDonorKind::MESH,"cell:"+std::to_string(i),-1,i,w,
            std::max(0.0,surface_->depth[i]),cap*scale,cap*scale});
    }
    auto credited=sources_->waterTrial();
    const auto return_error=credited.creditSurfaceReturns(start,source_returns);if(!return_error.empty())return return_error;
    auto proposal=credited.waterTrial();
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
        return j<0 ? 1e10 : capacity(j,std::max(0.0,g.type==lid::LIDType::VEG_SWALE ? g.surf_depth[u] : g.stor_depth[u])*.3048);
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
            return (a.wb_spatial_infil[d.unit]-b.wb_spatial_infil[d.unit])*a.area[d.unit]*ft3;
        }
        return (s.ledgers()[d.source].spatial_infiltration-sources_->ledgers()[d.source].spatial_infiltration)*ft3-
            (bottomTotal(s,d.source)-bottomTotal(*sources_,d.source));
    };
    const auto donorMass=[&](const runoff::SourceWaterDriver& s,int j,int pollutant) {
        const auto& d=donors_[j];double mass=0;
        for(const auto& b:s.pollutants()) if(b.source==d.source && b.type==d.type && b.unit==d.unit)
            mass+=b.spatial_infiltration[pollutant];
        for(const auto& b:sources_->pollutants()) if(b.source==d.source && b.type==d.type && b.unit==d.unit)
            mass-=b.spatial_infiltration[pollutant];
        return mass;
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
    SurfaceExchange::Capacity capacity_override=[&](const SurfaceIntakeRequest& r,double span) {
        if(r.kind==SurfaceDonorKind::MESH)if(const auto* row=surface_->runtime_forcings.get(3,r.cell))
            return std::max(0.0,infilSink(row->rate,surface_->depth[r.cell],options_->dry_depth))*r.area*span;
        return gw_->sourceInfiltrationCapacity(r.cell,r.pond,span)*r.area*span;
    };
    const auto* prescribed_capacity=runtimeWaterEnabled()?&capacity_override:nullptr;
    auto error=exchange_.plan(*gw_,start,end,end,requests,prescribed_capacity);if(!error.empty()) return error;
    std::vector<double> awarded(donors_.size(),0);
    std::map<int,double> mesh_awarded;
    for(const auto& a:exchange_.awards()) {
        if(a.request.kind==SurfaceDonorKind::MESH) mesh_awarded[a.request.cell]+=a.maximum;
        else awarded[donor_index.at(key(a.request))]+=a.maximum;
    }
    auto bounded=credited.waterTrial();
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
    std::vector<std::vector<double>> consumed_mass(donors_.size(),std::vector<double>(np,0));
    std::vector<std::vector<double>> mesh_mass(mesh_trials.size(),std::vector<double>(np,0));
    for(const auto& a:exchange_.awards()) {
        double volume=a.maximum;
        std::vector<double> mass(np,0);
        if(a.request.kind==SurfaceDonorKind::MESH) {
            const int j=mesh_index[a.request.cell];const auto& m=mesh_trials[j];
            // Match ordinary mesh source transport: advective sinks sample the
            // post-arrival mixture before evaporation; evaporation retains solute.
            for(int p=0;p<np;++p) {
                mass[p]=m.available>0 ? m.mass[p]*(volume/m.available) : 0;
                mesh_mass[j][p]+=mass[p];
            }
        } else {
            const int j=donor_index.at(key(a.request));const double taken=std::max(0.0,donorVolume(bounded,j));
            if(taken>awarded[j]+1e-12) { exchange_.cancel();return "Bounded source exceeded its shared award."; }
            // Spread actual withdrawal over its original awards. Denied cell
            // capacity never migrates; only FP residue lands on the last share.
            processed[j]+=a.maximum;
            volume=awarded[j]>0 ? std::min({a.maximum,std::max(0.0,taken-consumed[j]),
                processed[j]>=awarded[j] ? std::max(0.0,taken-consumed[j]) : taken*a.maximum/awarded[j]}) : 0;
            consumed[j]+=volume;
            for(int p=0;p<np;++p) {
                const double transferred=donorMass(bounded,j,p);
                mass[p]=taken>0 ? transferred*(volume/taken) : 0;
                consumed_mass[j][p]+=mass[p];
            }
        }
        actual.push_back({volume,std::move(mass)});
    }
    for(std::size_t j=0;j<donors_.size();++j) for(int p=0;p<np;++p) {
        const double mass=donorMass(bounded,int(j),p);
        if(!std::isfinite(mass) || mass<0 || std::abs(consumed_mass[j][p]-mass)>1e-12*std::max(1.0,mass)) {
            exchange_.cancel();return "Common source pollutant receipts do not match the actual donor-pool withdrawal.";
        }
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
        if(r.cell>=0){auto& b=et_budget[r.cell];b.potential+=r.potential;b.evaporation+=r.evaporation;b.soil+=r.soil_demand;}
        et_receipts.push_back(r);
    };
    for(const auto& m:mesh_trials) addEt({SurfaceEtOwner::MESH,-1,-1,-1,m.cell,start,end,weather_[m.cell],
        std::max(0.0,m.et_rate)*weather_[m.cell]*dt,m.evap},true);
    for(std::size_t j=0;j<bounded.atmosphere().size();++j) {
        const auto& a=bounded.atmosphere()[j];const auto& b=sources_->atmosphere()[j];
        const auto owner=a.kind==runoff::SourceEtKind::PERVIOUS ? SurfaceEtOwner::PERVIOUS :
            a.kind==runoff::SourceEtKind::IMPERVIOUS ? SurfaceEtOwner::IMPERVIOUS : SurfaceEtOwner::LID;
        for(const auto& [cell,area]:et_contacts_[j]) {
            const double fraction=area/(a.area*ft2);
            addEt({owner,a.source,a.type,a.unit,cell,start,end,area,
                (a.potential-b.potential)*ft3*fraction,(a.evaporation-b.evaporation)*ft3*fraction},a.soil_eligible);
        }
        const double outside_area=a.area*ft2*(1-bounded.spatialFractions()[a.source]);
        if(outside_area>0){const double fraction=outside_area/(a.area*ft2);
            addEt({owner,a.source,a.type,a.unit,-1,start,end,outside_area,
                (a.potential-b.potential)*ft3*fraction,(a.evaporation-b.evaporation)*ft3*fraction},false);
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
    error=exchange_.commit(*gw_,actual,prescribed_capacity);if(!error.empty()) { exchange_.cancel();return error; }
    for(const auto& m:mesh_trials) {
        const double infil=mesh_awarded[m.cell],A=mesh_->tri_area[m.cell];
        if(auto* row=const_cast<RuntimeForcing*>(surface_->runtime_forcings.get(3,m.cell))) {
            auto& r=row->receipt;r.requested+=m.requested_infil;r.applied+=infil;r.water_out+=infil;
            r.last_flow=infil/dt;r.last_dt=dt;
        }
        surface_->volume[m.cell]=std::max(0.0,surface_->volume[m.cell]+m.incoming-m.evap-m.out-infil);
        surface_->infil_applied[m.cell]+=infil/A;
        surface_->coupling_applied[m.cell]+=m.incoming-m.rain-m.out;
        mesh_return_+=mesh_return[m.cell];
        surface_->evap_loss_total+=m.evap;
        const auto& b=et_budget[m.cell];gw_->bookAreaEt(m.cell,dt,b.potential,b.evaporation,b.soil);
        inertial::cellEtaDepth(*mesh_,*options_,m.cell,surface_->volume[m.cell],surface_->head[m.cell],surface_->depth[m.cell]);
        mesh_rain_+=m.rain;mesh_evap_+=m.evap;
        auto& tr=surface_->transport;
        for(int p=0;p<np;++p) {
            const double taken=mesh_mass[mesh_index[m.cell]][p];
            tr.cell_mass[tr.idx(p,m.cell)]=m.mass[p]-taken;
            tr.gained_rainfall[p]+=m.rain_mass[p];tr.lost_infiltration[p]+=taken;
            tr.gained_exfiltration[p]+=mesh_return_mass[m.cell][p];
        }
    }
    *sources_=std::move(bounded);et_receipts_=std::move(et_receipts);returns_=std::move(ready_returns);
    gw_->acknowledgeSourceReturns(start);++intervals_;return {};
}
}
