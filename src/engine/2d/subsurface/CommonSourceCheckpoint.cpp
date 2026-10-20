// SPDX-License-Identifier: Apache-2.0
#include "CommonSourceStage.hpp"
#include "SubsurfaceSolver.hpp"
#include "../heat/ThermalCheckpoint.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../../core/SimulationContext.hpp"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
using Archive=detail::CompletedSourceArchive;
static_assert(std::is_nothrow_move_assignable_v<runoff::SourceWaterDriver>);
static_assert(std::is_nothrow_move_assignable_v<SubsurfaceSolver>);
static_assert(std::is_nothrow_move_assignable_v<SurfaceStateData>);
static_assert(std::is_nothrow_move_assignable_v<CommonSourceStage>);
constexpr uint32_t checkpointVersion=1;
constexpr uint32_t thermalCheckpointOffset=5;
constexpr uint64_t maxCheckpointBytes=64*1024*1024;
uint32_t checksum(const std::string& bytes) {
    uint32_t crc=~uint32_t(0);
    for(unsigned char c:bytes){crc^=c;for(int k=0;k<8;++k)crc=(crc>>1)^((crc&1)?0xedb88320u:0u);}
    return ~crc;
}
void intake(Archive& a,SurfaceIntakeReceipt& r) {
    auto& q=r.request;a(q.kind,q.source,q.unit,q.cell,q.area,q.pond,q.water_ceiling,q.candidate,
                       r.start,r.end,r.volume,r.mass);
}
void returned(Archive& a,SurfaceReturnReceipt& r) {
    a(r.kind);intake(a,r.intake);a(r.available_at,r.volume,r.mass);
}
bool validTransportMass(const std::vector<double>& mass,int temp_row) {
    for(std::size_t s=0;s<mass.size();++s)
        if(!std::isfinite(mass[s]) || (int(s)!=temp_row && mass[s]<0))return false;
    return true;
}
void meshState(Archive& a,SurfaceStateData& s) {
    a.fixed(s.depth,s.head,s.volume,s.grad_hx,s.grad_hy,s.grad_hx_lim,s.grad_hy_lim,s.vert_head,
            s.vert_depth_signed,s.face_vx,s.face_vy,s.cell_continuity_err,s.edge_flux,s.rainfall,s.evap_rate,
            s.infil_rate,s.infil_applied,s.coupling_flux,s.coupling_applied,s.net_source,s.rainfall_forced,
            s.rainfall_persist,s.rainfall_force_val,s.evap_forced,s.evap_persist,s.evap_force_val,
            s.coupling_forced,s.coupling_persist,s.coupling_force_val,s.coupling_native,s.old_depth,
            s.old_volume,s.stat_max_depth,s.stat_max_velocity,s.stat_max_cont_err,s.stat_cum_volume);
    a(s.forcing_dirty,s.forcing_ever_set,s.evap_loss_total);
    if(s.snow.active()) {
        auto bytes=a.reading()?std::string{}:s.snow.saveCheckpoint();a(bytes);a.fixed(s.snow_catch);
        if(a.reading())if(auto error=s.snow.restoreCheckpoint(bytes);!error.empty())throw std::runtime_error(error);
    }
    auto& t=s.transport;
    if(s.heat.enabled) {
        std::string profile=a.reading()?std::string{}:"accepted-boundary mesh thermal state v2";a(profile);
        if(a.reading() && profile!="accepted-boundary mesh thermal state v1" &&
           profile!="accepted-boundary mesh thermal state v2")
            throw std::runtime_error("Unsupported accepted-boundary mesh thermal checkpoint profile.");
        checkpoint::meshHeat(a,s.heat);
        if(s.heat.reported_temperature.size()!=s.depth.size())
            throw std::runtime_error("Thermal mesh checkpoint cell shape differs from its surface.");
        if(profile=="accepted-boundary mesh thermal state v2") {
            std::string receipt_profile=a.reading()?std::string{}:"finite mesh bed species receipt ledger v1";a(receipt_profile);
            if(a.reading() && receipt_profile!="finite mesh bed species receipt ledger v1")
                throw std::runtime_error("Unsupported mesh bed species receipt checkpoint profile.");
            a.fixed(t.gained_bed);
        } else if(a.reading() && s.heat.bed && !t.gained_bed.empty()) {
            throw std::runtime_error("Legacy mesh thermal checkpoint has no finite-bed species receipts; restart cannot preserve the bed mass budget.");
        }
    }
    a.fixed(t.cell_mass,t.lost_infiltration,t.lost_boundary,t.gained_external,t.lost_external,t.lost_coupling,
            t.exch_mass,t.exch_spill,t.gained_rainfall,t.gained_boundary,t.gained_coupling,
            t.gained_exfiltration,t.gained_washoff,t.cell_runoff_vol,t.coupling_src);
    { // Validate mesh state on save and restore.
        for(const auto* values:{&s.depth,&s.head,&s.volume,&s.grad_hx,&s.grad_hy,&s.grad_hx_lim,
            &s.grad_hy_lim,&s.vert_head,&s.vert_depth_signed,&s.face_vx,&s.face_vy,&s.cell_continuity_err,
            &s.edge_flux,&s.rainfall,&s.evap_rate,&s.infil_rate,&s.infil_applied,&s.coupling_flux,
            &s.coupling_applied,&s.net_source,&s.rainfall_force_val,&s.evap_force_val,&s.coupling_force_val,
            &s.coupling_native,&s.old_depth,&s.old_volume,&s.stat_max_depth,&s.stat_max_velocity,
            &s.stat_max_cont_err,&s.stat_cum_volume})
            for(double v:*values)if(!std::isfinite(v))throw std::runtime_error("Nonfinite s checkpoint state.");
        for(const auto* values:{&t.cell_mass,&t.lost_infiltration,&t.lost_boundary,&t.gained_external,
            &t.lost_external,&t.lost_coupling,&t.exch_mass,&t.exch_spill,&t.gained_rainfall,
            &t.gained_boundary,&t.gained_coupling,&t.gained_exfiltration,&t.gained_washoff,&t.cell_runoff_vol,
            &t.coupling_src,&t.gained_bed})
            for(double v:*values)if(!std::isfinite(v))throw std::runtime_error("Nonfinite t checkpoint state.");
        for(double v:s.volume)if(!std::isfinite(v)||v<0)throw std::runtime_error("Invalid checkpoint mesh water.");
        if(t.n_cells<0||t.n_species<0||t.cell_mass.size()!=std::size_t(t.n_cells)*std::size_t(t.n_species))
            throw std::runtime_error("Invalid checkpoint mesh transport shape.");
        if(s.heat.enabled && s.heat.bed && t.n_species>0 && t.gained_bed.size()!=std::size_t(t.n_species))
            throw std::runtime_error("Invalid checkpoint mesh bed species receipt shape.");
        for(std::size_t k=0;k<t.cell_mass.size();++k) {
            const int row=t.n_cells>0?int(k/std::size_t(t.n_cells)):-1;
            if(!std::isfinite(t.cell_mass[k])||(row!=t.temp_row&&t.cell_mass[k]<0))
                throw std::runtime_error("Invalid checkpoint mesh mass.");
        }
    }
}
// Publish only a complete file. An existing target survives validation/write failures.
void publish(const std::string& path,const std::string& bytes) {
    static std::atomic<uint64_t> serial{0};
    const auto part=path+".source-part-"+std::to_string(++serial);
    FILE* file=std::fopen(part.c_str(),"wbx");
    if(!file)throw std::runtime_error("Cannot create source checkpoint publication file.");
    const bool written=std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
    const bool closed=std::fclose(file)==0;
    if(!written||!closed){std::filesystem::remove(part);throw std::runtime_error("Source checkpoint write failed.");}
    std::error_code error;std::filesystem::rename(part,path,error);
    if(error){std::filesystem::remove(part);throw std::runtime_error("Cannot publish source checkpoint: "+error.message());}
}
}
std::string CommonSourceStage::checkpointModel() const {
    Archive a;auto source=sources_->waterTrial();auto gw=*gw_;auto m=*mesh_;auto o=*options_;auto t=surface_->transport;
    source.checkpoint(a,true);gw.checkpoint(a,true);
    auto mesh_thermal=checkpoint::meshHeatModelIdentity(surface_->heat);a(mesh_thermal);
    a(m.vx,m.vy,m.vz,m.vtag,m.cell_nv,m.cell_v,m.cell_nbr,m.tri_area,m.tri_cx,m.tri_cy,m.tri_cz,m.edge_length,
      m.edge_nx,m.edge_ny,m.edge_mx,m.edge_my,m.edge_mz,m.edge_dist_c,m.quad_vfr_z,m.quad_vfr_a,
      m.edge_conveyance,m.mannings_n,m.tri_init_depth,m.tri_init_u,m.tri_init_v,m.tri_tag,m.vert_stencil_ptr,
      m.vert_stencil_idx,m.vert_stencil_wt,m.vert_coupled_node,m.tri_coupled_node,m.vert_coupling_cd,
      m.vert_coupling_area,m.vert_coupling_area_set,m.tri_coupling_cd,m.tri_coupling_area,
      m.vert_coupled_node_name,m.tri_coupled_node_name);
    a(o.momentum,o.front_rebuild,o.reconstruction_order,o.max_timestep,o.dry_depth,o.limiter_epsilon,
      o.flux_dh_eps,o.coupling_sync,o.coupling_cd,o.report_2d,o.output_compression,o.report_2d_vars,
      o.report_2d_step,o.infiltration,o.infil_step,o.evaporation,o.transport_pollutants,o.transport_msx,
      o.transport_age,o.transport_temperature,o.groundwater,o.rainfall_mode,o.cell_closure,
      o.face_reconstruction,o.vfr_min_wet_frac,o.theta,o.cfl_number,o.h_move,o.lts_tiers,o.froude_max,
      o.advection,o.exchange_beta,o.dispersion,o.exchange_relax,o.coupling_area_auto,o.coupling_in_flooding,
      o.backend,o.len_1d_to_2d,o.len_2d_to_1d,o.vol_1d_to_2d,o.flow_1d_to_2d,o.flow_2d_to_1d,o.num_threads,
      o.requested_threads,o.mesh_units_si,o.mesh_scaled_to_si,o.mesh_to_si_factor,o.pending_rows_drained,
      o.bc_flow_to_si_applied);
    a(o.infil_default_method,o.infil_destination,o.gw_et);
    // Exchange law options join the identity only when set, so a legacy
    // model's checkpoint identity is unchanged.
    if(o.coupling_law!=CouplingLaw2D::ORIFICE||o.coupling_head!=CouplingHead2D::CELL||o.coupling_coeffs!=0)
        a(o.coupling_law,o.coupling_head,o.coupling_coeffs);
    if(o.coupling_pond_mode!=CouplingPondMode2D::FOOTPRINT||o.coupling_pond_relax!=0||o.coupling_rim_band>=0||o.coupling_equalize>=0)
        a(o.coupling_pond_mode,o.coupling_pond_relax,o.coupling_rim_band,o.coupling_equalize);
    a(t.n_species,t.n_cells,t.n_pollut,t.n_msx,t.age_row,t.temp_row,t.row_names,t.rain_conc,t.bc_conc);
    a.sequence(t.bc_quality_rows,[&](auto& b){a(b.slot,b.species,b.conc);});
    auto owned=owned_;auto weather=weather_;auto cells=cells_;auto donors=donors_;auto et=et_contacts_;
    a(owned,weather,cells,et);
    if(surface_->snow.active()){auto identity=surface_->snow.configurationIdentity();a(identity);}
    a.sequence(donors,[&](auto& d){a(d.source,d.type,d.unit,d.identity,d.area,d.contacts);});
    if(runtimeWaterEnabled()) {auto identity=runtime_water_identity_;a(identity);}
    if(runtimeCatchmentForcingEnabled()) {
        std::string profile="reviewed catchment provider forcing v1";auto contacts=forcing_contacts_;a(profile,contacts);
    }
    if(runtimeBoundariesEnabled()) {auto model=RuntimeWaterCheckpoint::boundaryModel(authored_boundary_,&sources_->context().tables);a(model);}
    auto standalone_local_bed=standalone_local_bed_;a(standalone_local_bed);
    if(mesh_only_ground_quality_) {std::string profile="mesh-only shared ground quality v1";a(profile);}
    return a.bytes();
}
std::string CommonSourceStage::checkpointBytes(std::string& bytes) const {
    try {
        const auto error=checkpointBoundary();if(!error.empty())return error;
        if(checkpoint_model_!=checkpointModel())return "Source checkpoint model changed after initial attachment.";
        auto source=sources_->waterTrial();auto gw=*gw_;auto mesh=*surface_;Archive state;
        double end=completedEnd();auto intervals=intervals_;auto rain=mesh_rain_,evap=mesh_evap_,back=mesh_return_;
        auto receipts=exchange_.receipts_;auto et=et_receipts_;auto returns=returns_;
        state(end,intervals,rain,evap,back);
        state.sequence(receipts,[&](auto& r){intake(state,r);});
        state.sequence(et,[&](auto& r){state(r.owner,r.source,r.type,r.unit,r.cell,r.start,r.end,r.area,r.potential,r.evaporation,r.soil_demand,r.unused);});
        state.sequence(returns,[&](auto& r){returned(state,r);});
        source.checkpoint(state,false);
        // A standalone local bed has no aquifer runtime state to persist. The
        // inactive solver's default vectors are intentionally not a valid
        // completed-source aquifer checkpoint, so leave that block out of the
        // profile rather than round-tripping synthetic groundwater state.
        if(!standalone_local_bed_)gw.checkpoint(state,false);
        meshState(state,mesh);
        if(runtimeWaterEnabled()) {
            // Source result capture precedes the engine's expiry pass. Its
            // immutable completed snapshot has no still-active expired rows.
            mesh.runtime_sources.expire(end);gw.runtimeSources().expire(end);mesh.runtime_forcings.expire(end);
            std::string providers;auto invalid=RuntimeWaterCheckpoint::capture(runtimeWaterBinding(end),
                mesh.runtime_sources,gw.runtimeSources(),mesh.runtime_forcings,providers,runtimeBoundariesEnabled()?mesh.boundary:nullptr);
            if(!invalid.empty())return invalid;state(providers);
        }
        std::string magic="OpenSWMM source boundary";uint32_t version=runtimeCatchmentForcingEnabled()?(runtimeBoundariesEnabled()?5:4):runtimeBoundariesEnabled()?3:runtimeWaterEnabled()?2:checkpointVersion;
        if(surface_->heat.enabled||gw_->heat().active())version+=thermalCheckpointOffset;
        uint32_t endian=0x01020304;
        uint32_t longBytes=sizeof(long),doubleBytes=sizeof(double);auto model=checkpoint_model_,payload=state.bytes();
        auto crc=checksum(payload);Archive file;file(magic,version,endian,longBytes,doubleBytes,model,payload,crc);
        if(file.bytes().size()>maxCheckpointBytes)return "Source checkpoint exceeds internal qualification size limit.";
        bytes=file.bytes();return {};
    } catch(const std::exception& e){return e.what();}
}
std::string CommonSourceStage::saveCheckpoint(const std::string& path) const {
    try {
        std::string bytes;const auto error=checkpointBytes(bytes);if(!error.empty())return error;
        publish(path,bytes);return {};
    } catch(const std::exception& e){return e.what();}
}
std::string CommonSourceStage::loadCheckpoint(const std::string& path) {
    try {
        std::error_code ec;const auto size=std::filesystem::file_size(path,ec);
        if(ec||size==0||size>maxCheckpointBytes)return "Source checkpoint file is missing or exceeds its size limit.";
        std::ifstream in(path,std::ios::binary);std::string bytes(size,'\0');in.read(bytes.data(),bytes.size());
        if(!in||in.peek()!=std::char_traits<char>::eof())return "Source checkpoint read failed.";
        return loadCheckpointBytes(std::move(bytes));
    } catch(const std::exception& e){return e.what();}
}
std::string CommonSourceStage::inspectCheckpointBytes(std::string bytes,const std::function<std::string(const CommonSourceStage&)>& inspect) const {
    auto trial=*this;return trial.loadCheckpointBytes(std::move(bytes),&inspect);
}
std::string CommonSourceStage::loadCheckpointBytes(std::string bytes,const std::function<std::string(const CommonSourceStage&)>* inspect) {
    try {
        const auto error=checkpointBoundary();if(!error.empty())return error;
        if(!inspect && (completedEnd()!=0||intervals_!=0))return "Source checkpoint restore requires a fresh attached stage.";
        if(checkpoint_model_!=checkpointModel())return "Source checkpoint target model changed after attachment.";
        if(bytes.empty()||bytes.size()>maxCheckpointBytes)return "Source checkpoint exceeds its size limit.";
        Archive file(std::move(bytes));std::string magic,model,payload;uint32_t version=0,endian=0,longBytes=0,doubleBytes=0,crc=0;
        file(magic,version,endian,longBytes,doubleBytes,model,payload,crc);file.finish();
        uint32_t expected_version=runtimeCatchmentForcingEnabled()?(runtimeBoundariesEnabled()?5u:4u):runtimeBoundariesEnabled()?3u:runtimeWaterEnabled()?2u:checkpointVersion;
        if(surface_->heat.enabled||gw_->heat().active())expected_version+=thermalCheckpointOffset;
        if(magic!="OpenSWMM source boundary"||version!=expected_version||endian!=0x01020304||longBytes!=sizeof(long)||doubleBytes!=sizeof(double))
            return "Unsupported source checkpoint version or platform.";
        if(model!=checkpoint_model_)return "Source checkpoint model, forcing, species or reviewed ownership differs.";
        if(crc!=checksum(payload))return "Source checkpoint checksum differs.";
        // Every parse and semantic check targets private copies. Failed loads change nothing.
        auto source=sources_->waterTrial();auto gw=*gw_;auto mesh=*surface_;auto trial=*this;
        BoundaryData boundary;if(runtimeBoundariesEnabled()) {boundary=*surface_->boundary;mesh.boundary=&boundary;}
        Archive state(std::move(payload));
        state(trial.exchange_.completed_end_,trial.intervals_,trial.mesh_rain_,trial.mesh_evap_,trial.mesh_return_);
        state.sequence(trial.exchange_.receipts_,[&](auto& r){intake(state,r);});
        state.sequence(trial.et_receipts_,[&](auto& r){state(r.owner,r.source,r.type,r.unit,r.cell,r.start,r.end,r.area,r.potential,r.evaporation,r.soil_demand,r.unused);});
        state.sequence(trial.returns_,[&](auto& r){returned(state,r);});
        source.checkpoint(state,false);
        if(!standalone_local_bed_)gw.checkpoint(state,false);
        meshState(state,mesh);
        if(runtimeWaterEnabled()) {
            std::string providers;state(providers);
            const auto invalid=RuntimeWaterCheckpoint::restore(runtimeWaterBinding(trial.completedEnd()),
                std::move(providers),mesh.runtime_sources,gw.runtimeSources(),mesh.runtime_forcings,runtimeBoundariesEnabled()?&boundary:nullptr);
            if(!invalid.empty())return invalid;
        }
        state.finish();
        if(mesh.heat.enabled)mesh.heat.aquifer_ground=mesh.heat.ground_column?&gw.heat():nullptr;
        trial.sources_=&source;trial.gw_=&gw;trial.surface_=&mesh;
        const auto invalid=trial.checkpointBoundary();if(!invalid.empty())return invalid;
        const double end=trial.completedEnd();
        const int ntransport=gw.transport().active()?gw.transport().n_species:0;
        const int temp_row=gw.transport().temp_row;
        if(!std::isfinite(end)||end<0||trial.intervals_<0||(end>0&&trial.intervals_==0))return "Invalid source checkpoint completed boundary.";
        for(double v:{trial.mesh_rain_,trial.mesh_evap_,trial.mesh_return_})if(!std::isfinite(v)||v<0)return "Invalid source checkpoint ledger.";
        const auto validIntake=[&](const auto& r){
            const auto& q=r.request;
            if(!trial.ownsCell(q.cell)||!std::isfinite(r.start)||!std::isfinite(r.end)||r.start<0||r.end<=r.start||r.end>end||
               !std::isfinite(r.volume)||r.volume<0||int(r.mass.size())!=ntransport||int(q.kind)<0||int(q.kind)>2)return false;
            for(double v:{q.area,q.pond,q.water_ceiling,q.candidate})if(!std::isfinite(v)||v<0)return false;
            if(!validTransportMass(r.mass,temp_row))return false;
            for(double m:r.mass)if(r.volume==0&&m!=0)return false;
            if(r.volume>std::min(q.candidate,q.water_ceiling)+1e-10*std::max(1.,r.volume))return false;
            if(q.kind==SurfaceDonorKind::MESH)return q.source=="cell:"+std::to_string(q.cell)&&q.unit==-1;
            for(const auto& d:trial.donors_)if(source.context().subcatch_names.name_of(d.source)==q.source&&d.identity==q.unit&&
                (d.type<0)==(q.kind==SurfaceDonorKind::NON_LID))for(const auto& c:d.contacts)if(c.first==q.cell)return true;
            return false;
        };
        for(const auto& r:trial.exchange_.receipts_)if(!validIntake(r))return "Invalid checkpoint last intake.";
        if(!trial.standalone_local_bed_)for(int cell:trial.cells_)for(const auto& r:gw.pendingSourceReceipts(cell))if(!validIntake(r))return "Invalid checkpoint pending donor.";
        const auto validReturn=[&](const auto& r){
            if(!trial.ownsCell(r.intake.request.cell)||!std::isfinite(r.available_at)||r.available_at<0||r.available_at>end||
               !std::isfinite(r.volume)||r.volume<0||int(r.mass.size())!=ntransport||int(r.kind)<0||int(r.kind)>2)return false;
            if(!validTransportMass(r.mass,temp_row))return false;
            for(double m:r.mass)if(r.volume==0&&m!=0)return false;
            if(r.kind==SurfaceReturnKind::NUMERICAL_REFUND) {
                if(!validIntake(r.intake)||r.available_at<r.intake.end||r.volume>r.intake.volume*(1+1e-10))return false;
                for(int p=0;p<ntransport;++p)if(std::abs(r.mass[p]-(r.intake.volume>0?r.intake.mass[p]*r.volume/r.intake.volume:0))>
                    1e-10*std::max(1.,r.mass[p]))return false;
            }
            return true;
        };
        for(const auto& r:trial.returns_)if(!validReturn(r))return "Invalid checkpoint last return.";
        if(!trial.standalone_local_bed_)for(const auto& r:gw.sourceReturns())if(!validReturn(r))return "Invalid checkpoint pending return.";
        for(const auto& r:trial.et_receipts_) {
            if(int(r.owner)<0||int(r.owner)>3||!std::isfinite(r.start)||!std::isfinite(r.end)||r.start<0||r.end<=r.start||r.end>end||
                (r.cell!=-1&&!trial.ownsCell(r.cell)))return "Invalid checkpoint ET interval.";
            bool identity=r.owner==SurfaceEtOwner::MESH&&trial.ownsCell(r.cell)&&r.source==-1&&r.type==-1&&r.unit==-1;
            if(r.owner!=SurfaceEtOwner::MESH)for(const auto& b:source.atmosphere()) {
                const auto owner=b.kind==runoff::SourceEtKind::PERVIOUS?SurfaceEtOwner::PERVIOUS:
                    b.kind==runoff::SourceEtKind::IMPERVIOUS?SurfaceEtOwner::IMPERVIOUS:SurfaceEtOwner::LID;
                if(r.owner==owner&&r.source==b.source&&r.type==b.type&&r.unit==b.unit)identity=true;
            }
            if(!identity)return "Invalid checkpoint ET owner.";
            for(double v:{r.area,r.potential,r.evaporation,r.soil_demand,r.unused})if(!std::isfinite(v)||v<0)return "Invalid checkpoint ET volume.";
            if(std::abs(r.potential-r.evaporation-r.soil_demand-r.unused)>1e-10*std::max(1.,r.potential))return "Checkpoint ET budget differs.";
        }
        // There is no active exchange transaction at an accepted boundary.
        trial.exchange_.start_=trial.exchange_.end_=end;trial.exchange_.receiver_=nullptr;trial.exchange_.awards_.clear();
        if(inspect)return (*inspect)(trial);
        if(runtimeBoundariesEnabled()) {
            *const_cast<BoundaryData*>(surface_->boundary)=std::move(boundary);mesh.boundary=surface_->boundary;
        }
        trial.sources_=sources_;trial.gw_=gw_;trial.surface_=surface_;
        *sources_=std::move(source);*gw_=std::move(gw);*surface_=std::move(mesh);
        if(surface_->heat.enabled)surface_->heat.aquifer_ground=surface_->heat.ground_column?&gw_->heat():nullptr;
        *this=std::move(trial);
        return {};
    } catch(const std::exception& e){return e.what();}
}
}
