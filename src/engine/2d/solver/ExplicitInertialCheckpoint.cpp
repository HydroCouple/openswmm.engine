// SPDX-License-Identifier: Apache-2.0
#include "ExplicitInertialSolver.hpp"
#include "../subsurface/CommonSourceStage.hpp"
#include "../subsurface/SubsurfaceSolver.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/BoundaryData.hpp"
#include "../coupling/NodeCoupling.hpp"
#include "../../data/NodeData.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
using Archive = detail::CompletedSourceArchive;
constexpr uint64_t maxBytes = 64 * 1024 * 1024;
constexpr const char* magicName = "OpenSWMM CPU surface/source boundary";
uint32_t checksum(const std::string& bytes) {
    uint32_t crc = ~uint32_t(0);
    for (unsigned char c : bytes) { crc ^= c; for (int k=0;k<8;++k) crc=(crc>>1)^((crc&1)?0xedb88320u:0u); }
    return ~crc;
}
std::string readFile(const std::string& path) {
    std::error_code ec; const auto size=std::filesystem::file_size(path,ec);
    if(ec || size==0 || size>maxBytes) throw std::runtime_error("CPU checkpoint is missing or exceeds its size limit.");
    std::ifstream in(path,std::ios::binary); std::string bytes(size,'\0'); in.read(bytes.data(),bytes.size());
    if(!in || in.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("CPU checkpoint read failed.");
    return bytes;
}
void publish(const std::string& path,const std::string& bytes) {
    // Refuse to replace an input, result or component-checkpoint file.
    if(std::filesystem::exists(path)) {
        Archive old(readFile(path)); std::string magic; old(magic);
        if(magic!=magicName) throw std::runtime_error("CPU checkpoint destination is an unrelated file.");
    }
    static std::atomic<uint64_t> serial{0};
    const auto part=path+".cpu-source-part-"+std::to_string(++serial);
    FILE* file=std::fopen(part.c_str(),"wbx");
    if(!file) throw std::runtime_error("Cannot create CPU checkpoint publication file.");
    const bool written=std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
    const bool closed=std::fclose(file)==0;
    if(!written || !closed) { std::filesystem::remove(part); throw std::runtime_error("CPU checkpoint write failed."); }
    std::error_code error; std::filesystem::rename(part,path,error);
    if(error) { std::filesystem::remove(part); throw std::runtime_error("Cannot publish CPU checkpoint: "+error.message()); }
}
}

std::string ExplicitInertialSolver::checkpointBoundary() const {
    if(!initialized_ || !common_stage_ || (!gw_&&!common_stage_->standaloneLocalBed()) || second_order_ || !checkpoint_ready_ ||
       !common_stage_->matches(mesh_,state_,gw_)) return "CPU checkpoint requires an accepted attached first-order source marcher.";
    if(t_last_sync_!=common_stage_->completedEnd() || source_phase_end_!=t_last_sync_)
        return "CPU/source completed clocks differ.";
    const auto& transport=state_->transport;
    const bool temperature_only=joint_checkpoint_ && transport.active() && transport.n_species==1 &&
        transport.n_pollut==0 && transport.n_msx==0 && transport.age_row<0 &&
        transport.temp_row==0 && transport.rowIndex("__TEMPERATURE__")==0;
    const bool standalone_local_bed_quality=common_stage_->standaloneLocalBed() && state_->heat.enabled &&
        state_->heat.bed && !state_->heat.ground_column;
    const bool supported_transport=temperature_only||standalone_local_bed_quality;
    if(!joint_checkpoint_ && (state_->nodes_1d || state_->node_coupling))
        return "CPU checkpoint requires a joint hydraulic owner for live 1D coupling.";
    if(state_->node_row_conc && !supported_transport)
        return "CPU checkpoint live node-row providers are not qualified.";
    if(!exch_head_slope_.empty())return "CPU checkpoint live head-slope providers are not qualified.";
    if(!common_stage_->runtimeBoundariesEnabled() && !bc_cell_.empty())
        return "CPU checkpoint live external boundary providers are not qualified.";
    if(joint_checkpoint_) {
        if(bool(state_->nodes_1d)!=bool(state_->node_coupling) ||
           (transport.active() && !supported_transport) ||
           node_drawn_.size()!=(state_->nodes_1d?std::size_t(state_->nodes_1d->count()):0) ||
           exch_.size()!=(state_->node_coupling?state_->node_coupling->size():0))
            return "Joint CPU checkpoint coupling layout differs.";
        if(state_->node_coupling)for(const auto& cp:*state_->node_coupling)
            if(cp.is_outfall || cp.cell_idx<0 || cp.cell_idx>=mesh_->n_cells() || cp.node_idx<0 ||
               cp.node_idx>=state_->nodes_1d->count() || !std::isfinite(cp.cd) || cp.cd<0 || !std::isfinite(cp.area) || cp.area<=0)
                return "Joint CPU checkpoint requires valid non-outfall exchange points.";
    }
    if(state_->boundary && !common_stage_->runtimeBoundariesEnabled()) {
        if(!state_->boundary->runtime_original.empty()) return "CPU checkpoint boundary runtime providers are not qualified.";
        for(int c=0;c<mesh_->n_cells();++c) for(int k=0;k<mesh_->cell_vertex_count(c);++k)
            if(mesh_->cell_neighbour(c,k)<0 &&
               state_->boundary->edge_bc_type.at(MeshData::slot(c,k))!=int8_t(BoundaryType::WALL))
                return "CPU checkpoint requires wall boundaries.";
    }
    if(common_stage_->runtimeBoundariesEnabled()) {
        if(!state_->boundary)return "CPU boundary checkpoint has no boundary model.";
        std::vector<int> cells,slots,walls,ptr{0};
        for(int c=0;c<mesh_->n_cells();++c) {
            for(int e=0;e<mesh_->cell_vertex_count(c);++e)if(mesh_->cell_neighbour(c,e)<0) {
                const int k=MeshData::slot(c,e);
                if(state_->boundary->edge_bc_type.at(k)==int8_t(BoundaryType::WALL))walls.push_back(k);
                else {cells.push_back(c);slots.push_back(k);}
            }
            ptr.push_back(int(walls.size()));
        }
        const bool unused_walls=wall_slot_.empty() && wall_ptr_.empty() && state_->boundary->runtime_original.empty() &&
            std::all_of(state_->boundary->receipts.begin(),state_->boundary->receipts.end(),[&](const auto& row) {
                return common_stage_->authored_boundary_.edge_bc_type.at(row.first)!=int8_t(BoundaryType::WALL);
            });
        if(cells!=bc_cell_ || slots!=bc_slot_ || (!unused_walls && (walls!=wall_slot_ || ptr!=wall_ptr_)) ||
           bc_q_.size()!=slots.size() || bc_accum_.size()!=slots.size())
            return "CPU checkpoint boundary layout differs from its exterior-edge model.";
        for(const auto* row:{&bc_q_,&bc_accum_})for(double v:*row)if(!std::isfinite(v))
            return "CPU checkpoint boundary momentum or accumulator is nonfinite.";
    }
    return common_stage_->checkpointBoundary();
}

void ExplicitInertialSolver::checkpointState(Archive& a) {
    // Scratch reconstructed before use is excluded; frozen scheduling and
    // prognostic momentum must survive without a rebuild or momentum reset.
    const auto pins=pin_t0_; const bool telemetry_enabled=!telemetry_path_.empty();
    bool saved_telemetry=telemetry_enabled;
    a.fixed(q_,qcx_,qcy_,facc_L_,facc_R_,macc_x_L_,macc_x_R_,macc_y_L_,macc_y_R_,sacc_L_,sacc_R_,
            dw_slope_,cell_active_,tier_,face_tier_,pin_t0_,frontier_,source_ledgers_,cells_by_tier_,edges_by_tier_);
    a(active_cells_,active_faces_,dt0_,t_last_sync_,source_phase_end_,cycles_since_rebuild_,
      accumulators_pending_,front_breach_,h_on_front_,exch_tau_,spill_deficit_,substeps_run_,face_passes_,
      last_steps_,last_dt_,active_frac_min_,active_frac_max_,active_frac_sum_,active_samples_,tier_occupancy_,
      saved_telemetry,telemetry_);
    if(common_stage_->runtimeBoundariesEnabled())a(bc_cell_,bc_slot_,bc_q_,bc_accum_,wall_ptr_,wall_slot_);
    if(joint_checkpoint_) {
        a.fixed(exch_,node_drawn_);
        for(double v:exch_) if(!std::isfinite(v)) throw std::runtime_error("Nonfinite joint CPU exchange.");
        for(double v:node_drawn_) if(!std::isfinite(v) || v<0) throw std::runtime_error("Invalid joint CPU donor withdrawal.");
    }
    if(pin_t0_!=pins || saved_telemetry!=telemetry_enabled) throw std::runtime_error("CPU checkpoint pin/telemetry configuration differs.");
    for(const auto* row:{&q_,&qcx_,&qcy_,&facc_L_,&facc_R_,&macc_x_L_,&macc_x_R_,&macc_y_L_,&macc_y_R_,
        &sacc_L_,&sacc_R_,&dw_slope_,&source_ledgers_})
        for(double v:*row) if(!std::isfinite(v)) throw std::runtime_error("Nonfinite CPU checkpoint state.");
    for(const auto* row:{&facc_L_,&facc_R_,&macc_x_L_,&macc_x_R_,&macc_y_L_,&macc_y_R_,&sacc_L_,&sacc_R_,&source_ledgers_})
        for(double v:*row) if(v!=0) throw std::runtime_error("CPU checkpoint has unsettled transfers.");
    for(double v:{dt0_,t_last_sync_,source_phase_end_,h_on_front_,exch_tau_,spill_deficit_,last_dt_,
                  active_frac_min_,active_frac_max_,active_frac_sum_})
        if(!std::isfinite(v)) throw std::runtime_error("Nonfinite CPU checkpoint clock/statistic.");
    if(t_last_sync_<0 || source_phase_end_!=t_last_sync_ || dt0_<0 || (t_last_sync_>0 && dt0_<=0) ||
       h_on_front_<0 || exch_tau_<0 || spill_deficit_<0 || (!joint_checkpoint_ && spill_deficit_!=0) || last_dt_<0 || accumulators_pending_ ||
       cycles_since_rebuild_<0 || cycles_since_rebuild_>(t_last_sync_==0?1000:4) ||
       substeps_run_<0 || face_passes_<0 || last_steps_<0 || last_steps_>substeps_run_ || active_samples_<0)
        throw std::runtime_error("Invalid CPU checkpoint boundary/schedule.");
    const int n=mesh_->n_cells(),K=int(cells_by_tier_.size());
    std::vector<int> cells,faces; std::vector<std::vector<int>> cell_tiers(K),face_tiers(K);
    for(int i=0;i<n;++i) {
        if(cell_active_[i]>1 || tier_[i]>=K || pin_t0_[i]>1 || (!frontier_.empty() && frontier_[i]>1))
            throw std::runtime_error("Invalid CPU checkpoint cell mask/tier.");
        if(cell_active_[i]) { cells.push_back(i); cell_tiers[tier_[i]].push_back(i); }
        if(active_samples_>0 && pin_t0_[i] && (!cell_active_[i] || tier_[i]!=0))
            throw std::runtime_error("CPU checkpoint lost a pinned source cell.");
    }
    for(int e=0;e<edges_.ne;++e) {
        if(face_tier_[e]>=K) throw std::runtime_error("Invalid CPU checkpoint face tier.");
        if(cell_active_[edges_.cL[e]] && cell_active_[edges_.cR[e]]) {
            const auto tier=std::min(tier_[edges_.cL[e]],tier_[edges_.cR[e]]);
            if(face_tier_[e]!=tier) throw std::runtime_error("CPU checkpoint face/cell tiers differ.");
            faces.push_back(e); face_tiers[tier].push_back(e);
        } else if(active_samples_>0 && q_[e]!=0) throw std::runtime_error("Inactive CPU checkpoint face carries momentum.");
    }
    if(cells!=active_cells_ || faces!=active_faces_ || cell_tiers!=cells_by_tier_ || face_tiers!=edges_by_tier_)
        throw std::runtime_error("CPU checkpoint work lists differ from their masks/tiers.");
    if(active_samples_==0 && (active_frac_min_!=1e30 || active_frac_max_!=-1e30 || active_frac_sum_!=0 || t_last_sync_!=0))
        throw std::runtime_error("Invalid initial CPU checkpoint statistics.");
    if(active_samples_>0 && (active_frac_min_<0 || active_frac_max_>1 || active_frac_min_>active_frac_max_ ||
       active_frac_sum_<0 || active_frac_sum_>double(active_samples_))) throw std::runtime_error("Invalid CPU checkpoint active statistics.");
    for(long v:tier_occupancy_) if(v<0) throw std::runtime_error("Invalid CPU checkpoint tier statistics.");
    if((saved_telemetry && telemetry_.size()!=static_cast<std::size_t>(active_samples_)) || (!saved_telemetry && !telemetry_.empty()))
        throw std::runtime_error("CPU checkpoint telemetry sample count differs.");
    double last=-1;
    for(const auto& [t,count]:telemetry_) {
        if(!std::isfinite(t) || t<last || t<0 || t>t_last_sync_ || count<0 || count>n)
            throw std::runtime_error("Invalid CPU checkpoint telemetry.");
        last=t;
    }
}

std::string ExplicitInertialSolver::saveCheckpoint(const std::string& path) const {
    try {
        if(joint_checkpoint_) return "Live coupling requires the joint router checkpoint.";
        std::string bytes; const auto error=checkpointBytes(bytes);
        if(!error.empty()) return error;
        publish(path,bytes); return {};
    } catch(const std::exception& e) { return e.what(); }
}

std::string ExplicitInertialSolver::checkpointBytes(std::string& bytes) const {
    try {
        const auto error=checkpointBoundary(); if(!error.empty()) return error;
        std::string component; const auto state_error=common_stage_->checkpointBytes(component);
        if(!state_error.empty()) return state_error;
        auto trial=*this; Archive state; state(component); trial.checkpointState(state);
        std::string magic=magicName,payload=state.bytes(); uint32_t version=common_stage_->runtimeBoundariesEnabled()?2:1,endian=0x01020304,longBytes=sizeof(long),crc=checksum(payload);
        Archive file; file(magic,version,endian,longBytes,payload,crc);
        if(file.bytes().size()>maxBytes) return "CPU checkpoint exceeds internal qualification size limit.";
        bytes=file.bytes(); return {};
    } catch(const std::exception& e) { return e.what(); }
}

std::string ExplicitInertialSolver::loadCheckpoint(const std::string& path) {
    try { if(joint_checkpoint_) return "Live coupling requires the joint router checkpoint."; return loadCheckpointBytes(readFile(path)); }
    catch(const std::exception& e) { return e.what(); }
}

std::string ExplicitInertialSolver::loadCheckpointBytes(std::string bytes,const std::array<double,10>* router_boundary,const std::vector<double>* pending_outfalls,
    const std::function<void(const runoff::SourceWaterDriver&)>* validate_source,
    const std::function<void()>* before_install,const std::vector<int>* boundary_slots,double* inspect_source_end) {
    try {
        const auto error=checkpointBoundary(); if(!error.empty()) return error;
        if(t_last_sync_!=0 || common_stage_->intervals()!=0 || source_history_ || engine_source_history_)
            return "CPU checkpoint restore requires a fresh marcher without a history recorder.";
        if(bytes.empty() || bytes.size()>maxBytes) return "CPU checkpoint exceeds its size limit.";
        Archive file(std::move(bytes)); std::string magic,payload;
        uint32_t version=0,endian=0,longBytes=0,crc=0; file(magic,version,endian,longBytes,payload,crc); file.finish();
        if(magic!=magicName || version!=(common_stage_->runtimeBoundariesEnabled()?2u:1u) || endian!=0x01020304 || longBytes!=sizeof(long))
            return "Unsupported CPU checkpoint version/platform.";
        if(crc!=checksum(payload)) return "CPU checkpoint checksum differs.";
        auto* live_gw=gw_?gw_:common_stage_->gw_;
        if(!live_gw)return "CPU checkpoint has no matching groundwater state owner.";
        auto source=common_stage_->sources_->waterTrial(); auto gw=*live_gw; auto surface=*state_; auto stage=*common_stage_; auto trial=*this;
        BoundaryData boundary;if(common_stage_->runtimeBoundariesEnabled()) {boundary=*state_->boundary;surface.boundary=&boundary;}
        stage.sources_=&source; stage.gw_=&gw; stage.surface_=&surface;
        trial.gw_=gw_?&gw:nullptr; trial.state_=&surface; trial.common_stage_=&stage;
        Archive state(std::move(payload)); std::string component; state(component); trial.checkpointState(state); state.finish();
        const auto component_error=stage.loadCheckpointBytes(std::move(component)); if(!component_error.empty()) return component_error;
        const auto invalid=trial.checkpointBoundary(); if(!invalid.empty()) return invalid;
        if(boundary_slots && trial.bc_slot_!=*boundary_slots)return "Router/CPU checkpoint boundary indices differ.";
        if(router_boundary) {
            if(trial.t_last_sync_!=(*router_boundary)[0]) return "Router/CPU checkpoint clocks differ.";
            double volume=0; for(double v:surface.volume) volume+=v;
            const std::array<double,4> ledger{stage.meshRain(),stage.meshEvaporation(),stage.meshReturn(),volume};
            for(int k=0;k<4;++k) if(std::abs(ledger[k]-(*router_boundary)[k+1])>1e-12*std::max(1.0,std::abs(ledger[k])))
                return "Router/source checkpoint ledgers differ.";
            double incoming=0,outgoing=0;
            for(const auto& row:surface.runtime_sources.rows) {incoming+=row.receipt.water_in;outgoing+=row.receipt.water_out;}
            if(!std::isfinite(incoming) || !std::isfinite(outgoing) ||
               std::abs(incoming-(*router_boundary)[5])>1e-12*std::max(1.,incoming) ||
               std::abs(outgoing-(*router_boundary)[6])>1e-12*std::max(1.,outgoing))
                return "Router/provider checkpoint water receipts differ.";
            double boundary_in=0,boundary_out=0;
            if(surface.boundary)for(const auto& [slot,r]:surface.boundary->receipts) {boundary_in+=r.water_in;boundary_out+=r.water_out;}
            if(!std::isfinite(boundary_in) || !std::isfinite(boundary_out) ||
               std::abs(boundary_in-(*router_boundary)[8])>1e-12*std::max(1.,boundary_in) ||
               std::abs(boundary_out-(*router_boundary)[9])>1e-12*std::max(1.,boundary_out))
                return "Router/boundary checkpoint water receipts differ.";
            if((*router_boundary)[7]==0 && (!surface.runtime_sources.rows.empty() || !gw.runtimeSources().rows.empty() ||
                !surface.runtime_forcings.rows.empty() || (surface.boundary && (!surface.boundary->runtime_original.empty() ||
                    std::any_of(surface.boundary->receipts.begin(),surface.boundary->receipts.end(),[&](const auto& row) {
                        return stage.authored_boundary_.edge_bc_type.at(row.first)==int8_t(BoundaryType::WALL);
                    })))))return "Router checkpoint lost its provider-use history.";
            for(double v:surface.infil_applied) if(v!=0) return "Router checkpoint has undrained infiltration.";
            if(pending_outfalls && pending_outfalls->size()!=surface.coupling_applied.size()) return "Router outfall receipt shape differs.";
            for(std::size_t c=0;c<surface.coupling_applied.size();++c)
                if(surface.coupling_applied[c]!=(pending_outfalls?(*pending_outfalls)[c]:0))
                    return "Router checkpoint undrained coupling differs from its outfall receipts.";
        }
        if (validate_source) (*validate_source)(source);
        if (inspect_source_end) { *inspect_source_end=stage.completedEnd();return {}; }
        if (before_install) (*before_install)();
        // All parsing, model, shape, schedule and component checks preceded
        // installation. Moves cannot throw; restored pointers retain ownership.
        static_assert(std::is_nothrow_move_assignable_v<ExplicitInertialSolver>);
        static_assert(std::is_nothrow_move_assignable_v<CommonSourceStage>);
        static_assert(std::is_nothrow_move_assignable_v<runoff::SourceWaterDriver>);
        static_assert(std::is_nothrow_move_assignable_v<SubsurfaceSolver>);
        static_assert(std::is_nothrow_move_assignable_v<SurfaceStateData>);
        if(common_stage_->runtimeBoundariesEnabled()) {
            *const_cast<BoundaryData*>(state_->boundary)=std::move(boundary);surface.boundary=state_->boundary;
        }
        stage.sources_=common_stage_->sources_; stage.gw_=live_gw; stage.surface_=state_;
        trial.gw_=gw_; trial.state_=state_; trial.common_stage_=common_stage_;
        *stage.sources_=std::move(source); *live_gw=std::move(gw); *state_=std::move(surface);
        if(state_->heat.enabled)
            state_->heat.aquifer_ground=state_->heat.ground_column?&live_gw->heat():nullptr;
        *common_stage_=std::move(stage); *this=std::move(trial); return {};
    } catch(const std::exception& e) { return e.what(); }
}
}
