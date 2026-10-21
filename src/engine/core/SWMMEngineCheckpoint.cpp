// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "SWMMEngine.hpp"
#include "HotStartManager.hpp"
#include "../hydrology/surface/NativeSnow.hpp"
#include "../hydrology/surface/SnowCheckpoint.hpp"
#include "CompletedSourceArchive.hpp"
#include "ReactionCheckpoint.hpp"
#include "CheckpointInspection.hpp"
#include "DateTime.hpp"
#include "FileIO.hpp"
#include "../io/CheckpointFile.hpp"
#include "../io/CheckpointDirectory.hpp"
#include <openswmm/engine/openswmm_checkpoint.h>
#include "../plugins/DefaultOutputPlugin.hpp"
#include "../plugins/DefaultStateIOPlugin.hpp"
#include "../plugins/DefaultReportPlugin.hpp"
#include "../data/HeatData.hpp"
#include "../data/BedZoneData.hpp"
#ifdef OPENSWMM_HAS_2D
#include "../2d/subsurface/CommonSourceStage.hpp"
#include "../2d/subsurface/EngineSourceResultHistory.hpp"
#include "../2d/subsurface/EngineSourceResultStream.hpp"
#include "../2d/solver/ExplicitInertialSolver.hpp"
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
#include "../2d/output/Default2DOutputPlugin.hpp"
#include "../io/Hdf5Mutex.hpp"
#endif
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace openswmm {
namespace {
using Archive=detail::CompletedSourceArchive;
constexpr uint64_t maxBytes=64*1024*1024;
constexpr const char* magicName="OpenSWMM accepted water-only engine session";
constexpr const char* fileMagic="OpenSWMM file-backed water engine session";
constexpr const char* streamMagic="OpenSWMM engine session with source stream";
void require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
template<class T> void finite(const T& v) {
    if constexpr(std::is_floating_point_v<T>) require(std::isfinite(v),"Nonfinite engine checkpoint field.");
}

template<class T> void finite(const std::vector<T>& v) { for(const auto& x:v)finite(x); }
template<class T,std::size_t N> void finite(const std::array<T,N>& v) { for(const auto& x:v)finite(x); }
template<class T> void field(Archive& a,T& v) { a(v);finite(v); }
template<class T,std::size_t N> void field(Archive& a,T (&v)[N]) { for(auto& x:v)field(a,x); }
template<class... T> void fields(Archive& a,T&... v) { (field(a,v),...); }
template<class T> void stateField(Archive& a,T& v) { field(a,v); }
template<class T> void stateField(Archive& a,std::vector<T>& v) { a.fixed(v);finite(v); }
void radiativeConfig(Archive& a,RadiativeConfig& r) {
    a(r.shortwave_wm2,r.albedo,r.shade_factor,r.sky_view,r.emiss_water,r.emiss_landcover,
      r.atm_emiss_coeff,r.lw_reflection,r.landcover_temp,r.sw_mode,r.sw_ts_index);
}
void sedimentConfig(Archive& a,SedimentConfig& s) {
    a(s.mesh_ground,s.ground_source_configured,s.mesh_porosity,s.thermal_diffusivity,s.solute_diffusivity,
      s.bed_thickness,s.ground_depth,s.ground_temp,s.has_ground_temp,s.ground_ts_index,
      s.hyporheic_velocity,s.sed_density,s.sed_specific_heat,s.initial_temp,s.has_initial_temp);
}
void heatConfig(Archive& a,HeatConfigData& h) {
    a(h.configured,h.radiative_exchange,h.surface_exchange,h.dry_temp_policy,h.layer_conduction,
      h.conduction.water_conductivity,h.conduction.sed_conductivity,h.conduction.sed_density,
      h.conduction.sed_specific_heat,h.sediment_exchange);
    radiativeConfig(a,h.radiative);sedimentConfig(a,h.sediment);
    a.sequence(h.overrides.rows,[&](auto& row){a(row.attr,row.scope,row.name,row.value);});
    a.sequence(h.overrides.rad_link,[&](auto& row){radiativeConfig(a,row);},true);
    a.sequence(h.overrides.rad_node,[&](auto& row){radiativeConfig(a,row);},true);
    a.sequence(h.overrides.sed_link,[&](auto& row){sedimentConfig(a,row);},true);
    a.sequence(h.overrides.rad_cell,[&](auto& row){radiativeConfig(a,row);},true);
    a.sequence(h.overrides.sed_cell,[&](auto& row){sedimentConfig(a,row);},true);
    a(h.overrides.resolved);
    auto& solar=h.solar;
    a(solar.latitude_deg,solar.longitude_deg,solar.timezone_hours,solar.has_latitude,solar.has_longitude,
      solar.has_timezone,solar.elevation_m,solar.has_elevation,solar.aod380,solar.aod500,
      solar.precip_water_cm,solar.ozone_cm,solar.ground_albedo);
    auto& cloud=h.cloud;
    a(cloud.configured,cloud.use_timeseries,cloud.ts_index,cloud.fraction,cloud.sw_atten_k,
      cloud.sw_atten_n,cloud.lw_cloud_k);
    for(double& value:h.global_temp)a(value);
    for(bool& value:h.configured_source)a(value);
    a(h.node_over_source,h.node_over_node,h.node_over_temp);
}
void heatState(Archive& a,HeatState& h) {
    a(h.legacy_seeded,h.shortwave_now,h.cloud_now);
    a(h.mesh_contact_link,h.node_temp_vol_in,h.node_lid_drain_temp_vol_in,h.node_temp,h.link_temp,
      h.subarea_temp,h.subarea_vol_prev,h.subcatch_runoff_temp,h.subcatch_runon_temp_vol_in,
      h.subcatch_runon_temp_rate,h.subcatch_outfall_temp_vol,h.subcatch_lid_drain_temp_cfs);
}
void bedZoneState(Archive& a,BedZoneState& b) {
    a(b.n_species,b.seeded,b.cell_n_species,b.cells_seeded);
    a(b.link_temp,b.link_conc,b.cell_temp,b.cell_conc);
}
uint32_t checksum(const std::string& bytes) {
    uint32_t crc=~uint32_t(0);
    for(unsigned char c:bytes) {crc^=c;for(int k=0;k<8;++k)crc=(crc>>1)^((crc&1)?0xedb88320u:0u);}
    return ~crc;
}
std::string readFile(const std::string& path) {
    std::error_code ec;auto size=std::filesystem::file_size(io::utf8_path(path),ec);
    require(!ec && size>0 && size<=maxBytes,"Engine checkpoint is missing or exceeds its size limit.");
    std::ifstream in(io::utf8_path(path),std::ios::binary);std::string bytes(size,'\0');in.read(bytes.data(),bytes.size());
    require(bool(in) && in.peek()==std::char_traits<char>::eof(),"Engine checkpoint read failed.");return bytes;
}
void publish(const std::string& path,const std::string& bytes,bool exclusive=false) {
    if(exclusive)require(!std::filesystem::exists(std::filesystem::symlink_status(io::utf8_path(path))),"Checkpoint destination already exists.");
    else if(std::filesystem::exists(io::utf8_path(path))) {
        Archive old(readFile(path));std::string magic;old(magic);
        Archive incoming(bytes);std::string next;incoming(next);
        require(magic==next && (magic==magicName || magic==streamMagic || magic==fileMagic),"Engine checkpoint destination is an unrelated file.");
    }
    static std::atomic<uint64_t> serial{0};auto part=path+".session-part-"+std::to_string(++serial);
    FILE* f=io::fopen_path(io::utf8_path(part),"wbx");require(f,"Cannot create engine checkpoint publication file.");
    bool written=std::fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();bool closed=std::fclose(f)==0;
    if(!written || !closed) {std::filesystem::remove(io::utf8_path(part));throw std::runtime_error("Engine checkpoint write failed.");}
    if(exclusive) {
        try {io::publishCheckpointNoReplace(io::utf8_path(part),io::utf8_path(path));}
        catch(...){std::error_code ignored;std::filesystem::remove(io::utf8_path(part),ignored);throw;}
        return;
    }
    std::error_code ec;std::filesystem::rename(io::utf8_path(part),io::utf8_path(path),ec);
    if(ec) {std::filesystem::remove(io::utf8_path(part));throw std::runtime_error("Cannot publish engine checkpoint: "+ec.message());}
}
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
struct FileCheckpointEnvelope {
    uint32_t version=1;
    std::string physical,binary,mesh,source;
    explicit FileCheckpointEnvelope(std::string bytes) {
        Archive in(std::move(bytes));std::string magic;uint32_t crc;
        in(magic,version,crc,physical,binary,mesh,source);in.finish();
        Archive data;data(physical,binary,mesh,source);
        require(magic==fileMagic && (version==1||version==2) && crc==checksum(data.bytes()),
            "File-backed checkpoint checksum or version differs.");
    }
    std::string binding() {Archive a;a(physical,binary,mesh);return a.bytes();}
    std::string encode() {
        Archive data;data(physical,binary,mesh,source);auto crc=checksum(data.bytes());
        std::string magic=fileMagic;Archive out;out(magic,version,crc,physical,binary,mesh,source);
        require(out.bytes().size()<=maxBytes,"File checkpoint exceeds its model-state limit.");return out.bytes();
    }
};
struct FileOutputContract {
    uint32_t version=1;
    int periods=0;
    double cadence=0,due=-1;
    hsize_t steps=0;
    bool groundwater=false;
    io::CheckpointFile file;
    FileOutputContract(std::string bytes,bool mesh) {
        require(bytes.size()<32768,"File output contract exceeds its limit.");
        Archive in(std::move(bytes));std::string descriptor;
        if(mesh)in(version,cadence,steps,due,groundwater,descriptor);else in(version,periods,descriptor);
        in.finish();require(version==1&&periods>=0&&std::isfinite(cadence)&&std::isfinite(due),"Invalid file output contract.");
        file=io::CheckpointFile::decode(std::move(descriptor));
    }
    std::string encode(bool mesh) {
        Archive out;auto descriptor=file.encode();
        if(mesh)out(version,cadence,steps,due,groundwater,descriptor);else out(version,periods,descriptor);
        return out.bytes();
    }
};
struct StreamContract {
    std::string path,cursor;
    uint64_t anchor=0,whole=0;
    explicit StreamContract(std::string bytes) {
        require(bytes.size()<32768,"Source stream contract exceeds its limit.");
        Archive in(std::move(bytes));in(path,cursor,anchor,whole);in.finish();
        require(!path.empty()&&path.size()<8192&&path.find('\0')==std::string::npos,"Invalid source stream path.");
    }
    std::string encode() {Archive out;out(path,cursor,anchor,whole);return out.bytes();}
};
// Bundle v2 contains only fixed relative member names. Resolve after computing
// the saved manifest binding, which includes the original relative descriptors.
void resolveBundle(FileCheckpointEnvelope& saved,const std::string& checkpoint) {
    if(saved.version!=2)return;
    const auto path=io::utf8_path(checkpoint);io::checkpointFileSize(path);
    const auto root=std::filesystem::canonical(std::filesystem::absolute(path).parent_path());
    FileOutputContract binary(saved.binary,false),mesh(saved.mesh,true);StreamContract source(saved.source);
    require(binary.file.path==io::utf8_path("binary.out")&&mesh.file.path==io::utf8_path("mesh.h5")&&source.path=="source.history",
        "Bundle references must use their fixed relative member names.");
    binary.file.path=root/"binary.out";mesh.file.path=root/"mesh.h5";source.path=io::path_utf8(root/"source.history");
    io::checkpointFileSize(binary.file.path);io::checkpointFileSize(mesh.file.path);io::checkpointFileSize(root/"source.history");
    saved.binary=binary.encode(false);saved.mesh=mesh.encode(true);saved.source=source.encode();
}
#endif
#ifdef OPENSWMM_HAS_2D
using WaterDriver=twoD::RuntimeWaterDriverCheckpoint;
constexpr uint64_t maxDriverBytes=8*1024*1024;
void driverSnapshotOk(const WaterDriver::Snapshot& s,double time) {
    require(std::isfinite(s.next_update_seconds) && (s.next_update_seconds==-1 || s.next_update_seconds>=time),
            "Water driver next update precedes the accepted boundary.");
    require(s.state.size()<=maxDriverBytes,"Water driver state exceeds its size limit.");
}
std::string captureDriver(const WaterDriver& driver,double time,const std::string& hydraulic) {
    auto state=driver.capture(time);driverSnapshotOk(state,time);
    require(state.next_update_seconds==driver.nextUpdateSeconds(),"Water driver captured schedule differs from its live cursor.");
    auto trial=driver.prepareRestore(state,time);require(bool(trial),"Water driver returned no prepared restore.");
    Archive payload;payload(state.next_sequence,state.next_update_seconds,state.state);
    auto identity=driver.identity(),bytes=payload.bytes();auto crc=checksum(bytes),water_crc=checksum(hydraulic);
    uint32_t version=1,endian=0x01020304;std::string magic="OpenSWMM external water driver";
    Archive out;out(magic,version,endian,identity,time,water_crc,bytes,crc);return out.bytes();
}
std::unique_ptr<WaterDriver::PreparedRestore> prepareDriver(const WaterDriver& driver,double time,
    const std::string& hydraulic,std::string bytes,CheckpointInspection::Driver* inspection=nullptr) {
    require(bytes.size()<=maxDriverBytes+8192,"Water driver archive exceeds its size limit.");
    Archive in(std::move(bytes));std::string magic,identity,payload;uint32_t version,endian,water_crc,crc;double saved;
    in(magic,version,endian,identity,saved,water_crc,payload,crc);in.finish();
    require(magic=="OpenSWMM external water driver" && version==1 && endian==0x01020304 &&
            identity==driver.identity() && saved==time && water_crc==checksum(hydraulic) && crc==checksum(payload),
            "Water driver checkpoint identity/time/prescription binding differs.");
    WaterDriver::Snapshot state;Archive p(std::move(payload));p(state.next_sequence,state.next_update_seconds,state.state);p.finish();
    driverSnapshotOk(state,time);
    auto prepared=driver.prepareRestore(state,time);require(bool(prepared),"Water driver returned no prepared restore.");
    if(inspection)*inspection={identity,state.next_sequence,state.next_update_seconds};
    return prepared;
}
#endif
// Report dates are the exact calendar grid that the output validators check,
// including REPORT_START and the writer's one-millisecond date convention.
CheckpointInspection::Reports inspectReports(std::string bytes,bool mesh,
    const SimulationContext& ctx,double next_report_ms) {
    Archive in(std::move(bytes));uint32_t version;uint64_t records=0;
    double cadence=0,due=0;bool groundwater=false;std::string image_or_descriptor;
    if(mesh)in(version,cadence,records,due,groundwater,image_or_descriptor);
    else {int count;in(version,count,image_or_descriptor);require(count>=0,"Invalid inspected report count.");records=count;}
    in.finish();require(version==1,"Unsupported inspected report version.");
    const double step=ctx.options.report_step,step_ms=1000*step,grid=std::round(next_report_ms/step_ms);
    require(std::isfinite(grid)&&grid>=1&&grid<=std::numeric_limits<int>::max()&&grid*step_ms==next_report_ms,
        "Invalid inspected report grid.");
    const auto completed=static_cast<int>(grid)-1;
    const double ratio=mesh&&cadence>0?cadence*86400/step:1;
    require(std::isfinite(ratio)&&ratio>=1&&ratio<=std::numeric_limits<int>::max(),"Invalid inspected report cadence.");
    const auto stride=static_cast<int>(std::round(ratio));
    CheckpointInspection::Reports result;result.enabled=true;result.records=records;result.cadence_seconds=stride*step;
    if(!records)return result;
    auto date=[&](int report){return datetime::addSeconds(ctx.options.start_date,(report*step_ms+1)/1000);};
    int first=1,upper=completed+1;
    while(first<upper){const int middle=first+(upper-first)/2;if(date(middle)<ctx.options.report_start)first=middle+1;else upper=middle;}
    require(first<=completed&&records<=uint64_t(1+(completed-first)/stride),"Inspected report coverage exceeds its grid.");
    result.first_date=date(first);result.last_date=date(first+static_cast<int>(records-1)*stride);return result;
}
// Explicit fields: no padding, file handles, plugins or compiled expressions.
#define BALANCE(X) \
    X(runoff_rainfall) X(runoff_runon) X(runoff_evap) X(runoff_infil) X(runoff_runoff) X(runoff_lid_drain) \
    X(runoff_snowremov) X(runoff_init_store) X(runoff_final_store) X(runoff_init_snow) X(runoff_final_snow) X(routing_dry_weather) \
    X(routing_wet_weather) X(routing_gw_inflow) X(routing_link_gw_inflow) X(routing_rdii) X(routing_external) X(routing_flooding) \
    X(routing_coupling_out) X(routing_outflow) X(routing_evap_loss) X(routing_seep_loss) X(routing_init_storage) X(routing_final_storage) \
    X(routing_forcing_inflow) X(routing_forcing_qual_inflow) X(gw_infil) X(gw_upper_evap) X(gw_lower_evap) X(gw_lower_perc) \
    X(gw_lateral_flow) X(gw_init_storage) X(gw_final_storage) X(gw_infil_2d_recharge) X(step_flooding) X(step_outflow) \
    X(step_dw_inflow) X(step_gw_inflow) X(step_rdii_inflow) X(step_ext_inflow) X(last_step_flow_error) X(step_ext_withdrawal) \
    X(qual_init_buildup) X(qual_final_buildup) X(qual_surface_buildup) X(qual_wet_deposition) X(qual_sweeping) X(qual_bmp_removal) \
    X(qual_infil_loss) X(qual_runoff_load) X(qual_routing_wet) X(qual_routing_outflow) X(qual_routing_flood) X(qual_routing_init) \
    X(qual_routing_final) X(qual_routing_reacted) X(qual_routing_ii_in) X(qual_routing_dw_in) X(qual_routing_gw_in) X(qual_routing_ex_in) \
    X(qual_routing_seep) X(qual_routing_evap) X(qual_routing_final_dry) X(routing_report)
#define STATS(X) \
    X(min_step) X(max_step) X(sum_step) X(n_steps) X(steady_pct) X(steady_time) \
    X(report_steps) X(report_time) X(max_outfall_flow) X(step_counts) X(step_intervals) X(n_non_converged) \
    X(sum_iterations) X(max_courant) X(slot_peak_share) X(slot_time_above_s) X(fv_nsteps) X(fv_nflux) \
    X(fv_avg_h) X(fv_last_h) X(fv_min_h) X(fv_active_min) X(fv_active_mean) X(fv_active_max) \
    X(fv_tier_cells) X(fv_n_tiers) X(fv_dt_argmin_pressurized) X(fv_dt_argmin_band) X(fv_dt_argmin_free) X(fv_dt_argmin_node) \
    X(fv_macro_cycles) X(fv_macro_rejected) X(lstep_mode)
#define CLIMATE(X) \
    X(temperature) X(temp_range) X(evap_rate) X(wind_speed) X(humidity) X(temperature_src) \
    X(file_tmin) X(file_tmax) X(wind_speed_src) X(gamma) X(ea) X(latitude) \
    X(elev) X(snow_divt) X(monthly_evap) X(evaprate_ucf) X(adjust_evap) X(next_evap_date) \
    X(next_evap_rate) X(evap_ts_pos) X(adjust_temp) X(adjust_rain) X(adjust_hydcon) X(evap_method) \
    X(infil_factor) X(recovery_factor) X(temp_ts_index) X(evap_ts_index) X(humidity_ts_index) X(recovery_pat_index) \
    X(tmin_daily) X(tmax_daily) X(prev_tmax) X(hrsr) X(hrss) X(hrday) \
    X(dhrdy) X(dydif) X(dtlong) X(has_minmax) X(last_temp_doy)
#define OPTIONS(X) \
    X(start_date) X(end_date) X(total_duration_ms) X(report_start) X(routing_step) X(routing_step_authored) \
    X(min_routing_step) X(dry_step) X(wet_step) X(report_step) X(dry_days) X(variable_step) \
    X(lengthening_step) X(sweep_start) X(sweep_end) X(flow_units) X(routing_model) X(quality_solver) \
    X(quality_step) X(max_segments_per_link) X(lard_rwpt) X(rwpt_seed) X(water_age) X(outfall_backflow_zero) \
    X(heat_transport) X(humidity) X(humidity_type) X(humidity_var) X(humidity_ts_name) X(water_density) \
    X(water_specific_heat) X(wind_func_coeff_a) X(wind_func_coeff_b) X(pressure_ratio) X(infiltration) X(runoff_model) \
    X(max_trials) X(surcharge_method) X(tpa_celerity) X(dps_target_celerity) X(dps_alpha) X(dps_decay_time) \
    X(unsteady_friction) X(report_signed_heads) X(uf_k3) X(node_continuity) X(virtual_junction_momentum) X(anderson_accel) \
    X(normal_flow_ltd) X(force_main_eqn) X(inertial_damping) X(link_offsets) X(min_slope) X(min_surf_area) \
    X(head_tol) X(sys_flow_tol) X(lat_flow_tol) X(skip_steady_state) X(write_absolute_paths) X(quality_routing) \
    X(allow_ponding) X(ignore_rainfall) X(ignore_snow_melt) X(ignore_groundwater) X(ignore_rdii) X(ignore_routing) \
    X(ignore_quality) X(ignore_2d) X(rule_step) X(num_threads) X(crs) X(evap_type) \
    X(evap_values) X(evap_ts_name) X(evap_recovery_pat) X(evap_dry_only) X(pan_coeff) X(temp_source) \
    X(temp_ts_name) X(temp_file_start) X(temp_units) X(wind_type) X(wind_speed) X(snow_divt) \
    X(snow_ati_wt) X(snow_nrg_ratio) X(snow_lat) X(snow_dtlong) X(snow_elev) X(snow_min_melt) \
    X(snow_max_melt) X(adc_imperv) X(adc_perv) X(rpt_disabled) X(rpt_subcatchments) X(rpt_nodes) \
    X(rpt_links) X(rpt_input) X(rpt_continuity) X(rpt_flowstats) X(rpt_controls) X(rpt_averages) \
    X(rpt_link_steps) X(rpt_subcatch_names) X(rpt_node_names) X(rpt_link_names)
#define ENGINE(X) \
    X(old_runoff_time_) X(new_runoff_time_) X(old_runoff_ms_) X(new_runoff_ms_) X(prev_runoff_step_sec_) X(new_rule_time_ms_) \
    X(next_event_) X(between_events_) X(last_melt_doy_) X(prev_lid_infil_vol_) X(prev_lid_evap_vol_) X(has_runoff_) \
    X(has_snow_) X(last_step_steady_) X(routing_prev_rates_) X(lid_drain_q_interp_)
// Only water runtime fields are decoded; authored data stays in the fresh model.
#define SUBCATCH_WATER(X) \
    X(runoff) X(rainfall) X(evap_loss) X(infil_loss) X(ponded_depth) X(old_ponded_depth) X(washoff_vout1) \
    X(gw_flow) X(snow_depth) X(lid_drain_flow) X(old_runoff) X(old_gw_flow) X(old_snow_depth) X(old_lid_drain_flow) \
    X(runon_inflow) X(old_runon_inflow) X(runon_rate) X(outfall_runon_vol) X(gw_sw_head) X(gw_node_avail_flow) X(gw_max_infil_vol) \
    X(subarea_runoff_rate) X(lid_return_to_perv_cfs) X(lid_drain_runon_cfs) X(stat_precip_vol) X(stat_runon_vol) X(stat_gw_time) X(stat_evap_vol) X(stat_infil_vol) \
    X(stat_imperv_vol) X(stat_perv_vol) X(stat_runoff_vol) X(stat_max_runoff) X(stat_gw_infil_vol) X(stat_gw_upper_evap_vol) \
    X(stat_gw_lower_evap_vol) X(stat_gw_deep_perc_vol) X(stat_gw_flow_vol) X(stat_gw_max_flow) X(stat_gw_sum_theta) \
    X(stat_gw_sum_depth) X(stat_gw_final_theta) X(stat_gw_final_depth) X(stat_gw_steps) X(snow_net_imperv) X(snow_net_perv) \
    X(snow_melt_imperv) X(snow_melt_perv) X(snow_melt_age_imperv) X(snow_melt_age_perv)
#define GAGE_WATER(X) \
    X(rainfall) X(next_rainfall) X(api_rainfall) X(next_rain_date) X(is_raining) X(st_cur) X(st_next) X(st_rain) \
    X(st_init) X(st_used) X(past_rain) X(past_rain_accum) X(past_rain_interval) X(cumul_rain_accum)
#define LID_USAGE_WATER(X) \
    X(wb_inflow) X(wb_evap) X(wb_infil) X(wb_surf_flow) X(wb_drain_flow) X(wb_init_vol) X(wb_final_vol)
#define RUNOFF_ENGINE(X) \
    X(wet_q_interp_) X(gw_q_interp_) X(gw_q_node_) X(lid_vlidin_vol_) X(lid_qsurf_cfs_) X(old_gw_rate_) X(lid_drain_q_units_)
void climateState(Archive& a,climate::ClimateState& s) {
#define F(v) field(a,s.v);
    CLIMATE(F)
#undef F
    fields(a,s.temp_ma.ta,s.temp_ma.tr,s.temp_ma.front,s.temp_ma.count);
}
std::string climateModel(climate::ClimateState s) {
    Archive a;
    fields(a,s.latitude,s.elev,s.snow_divt,s.monthly_evap,s.evaprate_ucf,s.adjust_evap,s.adjust_temp,s.adjust_rain,s.adjust_hydcon,
           s.evap_method,s.temp_ts_index,s.evap_ts_index,s.humidity_ts_index,s.recovery_pat_index,s.dtlong);
    return a.bytes();
}
void greenAmpt(Archive& a,GreenAmptState& g) { fields(a,g.S,g.Ks,g.IMDmax,g.IMD,g.F,g.Fu,g.Fumax,g.Lu,g.T,g.saturated); }
void checkGreenAmpt(const GreenAmptState& g,const GreenAmptState& model) {
    require(g.S==model.S && g.Ks==model.Ks && g.IMDmax==model.IMDmax && g.Fumax==model.Fumax && g.Lu==model.Lu,
            "Engine exfiltration material differs.");
    require(g.IMD>=0 && g.IMD<=g.IMDmax && g.F>=0 && g.Fu>=0,"Invalid engine exfiltration inventory.");
}
}

std::string SWMMEngine::queueHotstartCheckpoint(std::string checkpoint) {
    try {
        require(ctx_.state==EngineState::INITIALIZED && !checkpoint.empty() &&
                pending_hotstart_checkpoint_.empty(),
                "Queue a nonempty native engine hotstart after initialize and before start.");
        pending_hotstart_checkpoint_=std::move(checkpoint);
        return {};
    } catch(const std::exception& e) {return e.what();}
}

std::string SWMMEngine::saveNativeHotstart(const std::string& path) const {
    std::string snapshot;
    const auto error=checkpointBytes(snapshot,false);
    if(!error.empty())return error;
    std::unique_ptr<HotStartFile> saved(HotStartManager::save_with_engine_checkpoint(
        ctx_,&runoff_,&groundwater_,path,snapshot));
    return saved?std::string{}:HotStartManager::last_io_error();
}

struct SWMMEngine::RestartState {
    SimulationContext::MassBalance balance;
    SimulationContext::RoutingStepStats stats;
    SimulationContext::NegativeSourceStats negsrc;
    std::array<SimulationContext::MaxStats,4*SimulationContext::MAX_STATS> tops;
    climate::ClimateState climate;
    exfil::ExfilSoA exfil;
    AvgAccumulator avg;
    runoff::RunoffSolver runoff;
    lid::LIDSolver lid;
    SubcatchData subcatch;
    GageData gages;
    LidUsageStore lid_usage;
    double runoff_interp;
    std::unique_ptr<SimulationContext> snow_context;
    snow::SnowSolver snow;
    HeatState heat;
    BedZoneState bed;
    bool heat_profile=false;
    bool rk2_profile=false;
    bool reaction_profile=false;
    ReactionData reaction;
    std::string ard_bytes,lard_bytes;
    bool ard_attempted=false;
    std::unique_ptr<transport::ArdEngine> prepared_ard;
    std::unique_ptr<lard::LagrangianSolver> prepared_lard;
    std::vector<double> snow_rain,snow_snow;
    std::array<double,6> boundary;
    double controls_remaining;
    std::vector<TableCursor> cursors;
    std::vector<controls::ControlEngine::PendingAction> pending;
    std::vector<int> slots;
    int action_count;
    std::array<double,6> control_scratch;
    std::vector<controls::PIDState> pid;
    std::vector<double> orifice_area,weir_area;
#ifdef OPENSWMM_HAS_2D
    bool spatial=false;std::array<double,6> source_totals{};
#endif
#define DECL(v) decltype(SWMMEngine::v) v;
    ENGINE(DECL)
    RUNOFF_ENGINE(DECL)
#undef DECL
    explicit RestartState(const SWMMEngine& e) : balance(e.ctx_.mass_balance),stats(e.ctx_.routing_stats),negsrc(e.ctx_.negsrc),
        climate(e.ctx_.climate_state),exfil(e.exfil_.state()),avg(e.avg_),runoff(e.runoff_),lid(e.lid_.waterTrial()),
        subcatch(e.ctx_.subcatches),gages(e.ctx_.gages),lid_usage(e.ctx_.lid_usage),runoff_interp(e.ctx_.runoff_interp_f),
        boundary{e.ctx_.current_time,e.ctx_.current_date,e.ctx_.elapsed_ms,e.ctx_.old_elapsed_ms,e.ctx_.next_report_ms,e.ctx_.climate_state.evap_rate},
        controls_remaining(e.ctx_.dt_controls_remaining),pending(e.controls_.pending_actions_),
        slots(e.controls_.action_slots_),action_count(e.controls_.last_action_count_),
        control_scratch{e.controls_.control_value_,e.controls_.set_point_,e.controls_.eval_date_,e.controls_.cur_date_,e.controls_.cur_time_,e.controls_.elapsed_days_},
        pid(e.controls_.pid_states_),orifice_area(e.hydstruct_.orifices_.surf_area),weir_area(e.hydstruct_.weirs_.surf_area) {
#define COPY(v) v=e.v;
        ENGINE(COPY)
        RUNOFF_ENGINE(COPY)
#undef COPY
        heat_profile=e.ctx_.options.heat_transport;
#ifdef OPENSWMM_HAS_2D
        rk2_profile=e.surface_router_.standaloneRk2Checkpointing();
#endif
        reaction_profile=e.ctx_.reactions.configured;
        if(reaction_profile) {
            reaction=e.ctx_.reactions;ard_attempted=e.ard_init_attempted_;
            ard_bytes=e.ard_.checkpointState();lard_bytes=e.lard_.checkpointState();
        }
        if(heat_profile) {heat=e.ctx_.heat_state;bed=e.ctx_.bed_state;}
#ifdef OPENSWMM_HAS_2D
        spatial=bool(e.source_delivery_);source_totals=e.source_totals_;
#endif
        if(std::any_of(e.ctx_.subcatches.snowpack.begin(),e.ctx_.subcatches.snowpack.end(),[](int p){return p>=0;})) {
            snow_context=std::make_unique<SimulationContext>(e.ctx_);snow=e.snow_;
            snow_rain=e.snow_rain_;snow_snow=e.snow_snow_;
        }
        int at=0;for(const auto* row:{e.ctx_.max_courant_crit,e.ctx_.max_flow_turns,e.ctx_.max_non_converged,e.ctx_.max_mass_bal_errs})
            for(int i=0;i<SimulationContext::MAX_STATS;++i)tops[at++]=row[i];
        for(const auto& t:e.ctx_.tables.tables)cursors.push_back(t.cursor);
    }
    void archive(Archive& a) {
        fields(a,boundary,controls_remaining);
#define F(v) field(a,v);
        ENGINE(F)
#undef F
#define F(v) stateField(a,balance.v);
        BALANCE(F)
#undef F
#define F(v) field(a,stats.v);
        STATS(F)
#undef F
        fields(a,negsrc.clamp_events,negsrc.shortfall_mass,negsrc.age_clamp_events,negsrc.first_node,negsrc.first_clamp_recorded,negsrc.api_warned);
        for(auto& x:tops)fields(a,x.obj_type,x.index,x.value);
        climateState(a,climate);
        if(rk2_profile) {
            std::string profile="managed RK2 native radiation state v1";a(profile);
            require(profile=="managed RK2 native radiation state v1","Unsupported managed RK2 radiation record.");
            auto& r=climate.radiation;
            a(r.shortwave,r.longwave,r.cosine,r.horizontal,r.transmittance,r.cloud,r.temperature,r.humidity,r.wind,r.pressure,
              r.declination,r.hour_angle,r.latitude,r.date,r.seconds,r.shortwave_source,r.longwave_source,r.shared_heat_forcing);
        }
        a.sequence(cursors,[&](auto& c){a(c.index,c.direction);},true);
        a.sequence(exfil.btm_ga,[&](auto& g){greenAmpt(a,g);},true);
        a.sequence(exfil.bank_ga,[&](auto& g){greenAmpt(a,g);},true);
        fields(a,avg.n_steps);
        a.fixed(avg.node_depth,avg.node_head,avg.node_volume,avg.node_lat_inflow,avg.node_total_inflow,avg.node_overflow,
                avg.link_flow,avg.link_depth,avg.link_velocity,avg.link_volume,avg.link_capacity);
        for(const auto* row:{&avg.node_depth,&avg.node_head,&avg.node_volume,&avg.node_lat_inflow,&avg.node_total_inflow,&avg.node_overflow,
                            &avg.link_flow,&avg.link_depth,&avg.link_velocity,&avg.link_volume,&avg.link_capacity})finite(*row);
        a.sequence(pending,[&](auto& p){fields(a,p.link_idx,p.value,p.priority,p.rule_idx,p.type);});
        fields(a,slots,action_count,control_scratch);
        std::string marker="engine runoff/rainfall boundary v4";a(marker);
        require(marker=="engine runoff/rainfall boundary v4","Invalid engine hydrology record.");
        std::string water,units;
        if(!a.reading()) {Archive r,l;runoff.checkpoint(r,false);lid.checkpoint(l,false,true);water=r.bytes();units=l.bytes();}
        a(water,units);
        if(a.reading()) {Archive r(std::move(water)),l(std::move(units));runoff.checkpoint(r,false);r.finish();lid.checkpoint(l,false,true);l.finish();}
#define F(v) stateField(a,subcatch.v);
        SUBCATCH_WATER(F)
#undef F
#define F(v) stateField(a,gages.v);
        GAGE_WATER(F)
#undef F
#define F(v) stateField(a,lid_usage.v);
        LID_USAGE_WATER(F)
#undef F
        field(a,runoff_interp);
        if(snow_context) {
            auto bytes=a.reading()?std::string{}:surface::saveSnowCheckpoint(*snow_context,snow.state());a(bytes);
            if(a.reading()) {
                auto error=surface::restoreSnowCheckpoint(*snow_context,snow.state(),bytes);
                require(error.empty(),error.c_str());
            }
            a(snow_rain,snow_snow);
        }
#define F(v) a(v);
        RUNOFF_ENGINE(F)
#undef F
        if(heat_profile) {
            // Thermal sessions may materialize control and structure state
            // after the target's initial boundary. Preserve its accepted
            // runtime shape and validate it against the compiled model below.
            a.sequence(pid,[&](auto& p){fields(a,p.setpoint,p.e1,p.e2);});
            a(orifice_area,weir_area);
        } else {
            // Preserve the established water-only checkpoint layout.
            a.sequence(pid,[&](auto& p){fields(a,p.setpoint,p.e1,p.e2);},true);
            a.fixed(orifice_area,weir_area);
        }
        finite(orifice_area);finite(weir_area);
        if(heat_profile) {
            bool thermal=true;a(thermal);require(thermal,"Engine checkpoint thermal profile differs.");
            heatState(a,heat);bedZoneState(a,bed);
        }
        if(reaction_profile) {
            detail::reactionState(a,reaction);
            a(ard_attempted,ard_bytes,lard_bytes);
            a(subcatch.sweep_last_swept,subcatch.sweep_now,subcatch.sweep_last_date);
        }
#ifdef OPENSWMM_HAS_2D
        if (spatial) fields(a,source_totals);
#endif
    }
    void validate(const SWMMEngine& e) const {
        const double time=boundary[0],elapsed=boundary[2],duration=e.ctx_.options.totalDurationMs();
        require(time>=0 && elapsed>=0 && elapsed<=duration && std::abs(1000*time-elapsed)<=1.e-7*std::max(1.,elapsed),"Engine checkpoint routing clocks differ.");
        require(old_runoff_time_>=0 && new_runoff_time_>=old_runoff_time_ && old_runoff_ms_>=0 && new_runoff_ms_>=old_runoff_ms_ &&
                new_runoff_ms_>=elapsed && new_runoff_ms_<=duration && prev_runoff_step_sec_>=0 &&
                std::abs(1000*new_runoff_time_-new_runoff_ms_)<=1.e-7*std::max(1.,new_runoff_ms_),"Invalid engine runoff interpolation clocks.");
        require(next_event_>=0 && next_event_<=static_cast<int>(e.ctx_.events.size()),"Invalid engine event position.");
        require(new_rule_time_ms_>=0 && new_rule_time_ms_<=elapsed+1 && controls_remaining>=0,"Invalid engine rule scheduling clock.");
        require(climate.evap_rate==boundary[5] && climate.evap_rate>=0 && climate.temp_ma.front>=0 && climate.temp_ma.front<7 &&
                climate.temp_ma.count>=0 && climate.temp_ma.count<=7,"Invalid engine climate checkpoint.");
        require(climateModel(climate)==climateModel(e.ctx_.climate_state),"Engine climate parameters differ.");
        require(heat_profile==e.ctx_.options.heat_transport,"Engine thermal checkpoint profile differs.");
        if(rk2_profile) {
            const auto& r=climate.radiation;
            for(double value:{r.shortwave,r.longwave,r.cosine,r.horizontal,r.transmittance,r.cloud,r.temperature,r.humidity,r.wind,r.pressure,
                              r.declination,r.hour_angle,r.latitude,r.date,r.seconds})require(std::isfinite(value),"Nonfinite managed RK2 radiation state.");
            require(int(r.shortwave_source)>=0&&int(r.shortwave_source)<=3&&int(r.longwave_source)>=0&&int(r.longwave_source)<=3,
                    "Invalid managed RK2 radiation provenance.");
        }
        if(reaction_profile) {
            detail::validateReactionState(reaction,e.ctx_);
            require(subcatch.sweep_last_swept.size()==e.ctx_.subcatches.sweep_last_swept.size(),"Reaction sweeping-clock shape differs.");
            finite(subcatch.sweep_last_swept);finite(subcatch.sweep_last_date);
        }
        if(heat_profile) {
            const auto same_size=[&](const auto& saved,std::size_t model,const char* message) {
                require(saved.size()==model || (boundary[0]==0 && saved.empty()),message);
            };
            require(heat.mesh_contact_link==e.ctx_.heat_state.mesh_contact_link,
                    "Engine thermal checkpoint mesh-contact ownership differs.");
            const auto nn=std::size_t(e.ctx_.n_nodes()),nl=std::size_t(e.ctx_.n_links()),ns=std::size_t(e.ctx_.n_subcatches());
            same_size(heat.node_temp_vol_in,nn,"Engine node heat receipt shape differs.");
            same_size(heat.node_lid_drain_temp_vol_in,nn,"Engine LID heat receipt shape differs.");
            same_size(heat.node_temp,nn,"Engine node temperature shape differs.");
            same_size(heat.link_temp,nl,"Engine link temperature shape differs.");
            same_size(heat.subarea_temp,ns*std::size_t(HeatState::kNSubArea),"Engine subarea temperature shape differs.");
            same_size(heat.subarea_vol_prev,ns*std::size_t(HeatState::kNSubArea),"Engine subarea heat-volume shape differs.");
            same_size(heat.subcatch_runoff_temp,ns,"Engine runoff temperature shape differs.");
            same_size(heat.subcatch_runon_temp_vol_in,ns,"Engine runon heat receipt shape differs.");
            same_size(heat.subcatch_runon_temp_rate,ns,"Engine runon heat-rate shape differs.");
            same_size(heat.subcatch_outfall_temp_vol,ns,"Engine outfall heat receipt shape differs.");
            same_size(heat.subcatch_lid_drain_temp_cfs,ns,"Engine LID heat receipt shape differs.");
            require(std::isfinite(heat.shortwave_now) && std::isfinite(heat.cloud_now) &&
                    heat.cloud_now>=0 && heat.cloud_now<=1,"Invalid engine thermal forcing state.");
            for(const auto* row:{&heat.node_temp_vol_in,&heat.node_lid_drain_temp_vol_in,&heat.node_temp,&heat.link_temp,
                    &heat.subarea_temp,&heat.subarea_vol_prev,&heat.subcatch_runoff_temp,&heat.subcatch_runon_temp_vol_in,
                    &heat.subcatch_runon_temp_rate,&heat.subcatch_outfall_temp_vol,&heat.subcatch_lid_drain_temp_cfs})
                finite(*row);
            for(auto owned:heat.mesh_contact_link)require(owned<=1,"Invalid engine thermal contact ownership flag.");
            const auto& expected_bed=e.ctx_.bed_state;
            require(bed.n_species==expected_bed.n_species && bed.cell_n_species==expected_bed.cell_n_species &&
                    (bed.link_temp.size()==expected_bed.link_temp.size() ||
                     (expected_bed.link_temp.empty() && bed.link_temp.size()==std::size_t(e.ctx_.n_links()))) &&
                    (bed.cell_temp.size()==expected_bed.cell_temp.size() || expected_bed.cell_temp.empty()) &&
                    bed.link_conc.size()==bed.link_temp.size()*std::size_t(bed.n_species) &&
                    bed.cell_conc.size()==bed.cell_temp.size()*std::size_t(bed.cell_n_species),
                    "Engine bed checkpoint array shape differs.");
            for(const auto* row:{&bed.link_temp,&bed.link_conc,&bed.cell_temp,&bed.cell_conc})finite(*row);
            for(double mass:bed.link_conc)require(mass>=0,"Negative engine bed species inventory.");
            for(double mass:bed.cell_conc)require(mass>=0,"Negative engine bed-cell species inventory.");
        }
        require(avg.n_steps>=0 && stats.n_steps>=0 && stats.report_steps>=0 && stats.n_non_converged>=0 &&
                stats.sum_step>=0 && stats.report_time>=0 && stats.steady_time>=0 && stats.sum_iterations>=0 &&
                (stats.n_steps==0 || stats.sum_iterations>0) && stats.lstep_mode==e.ctx_.routing_stats.lstep_mode,
                "Invalid engine routing statistics.");
        for(long count:stats.step_counts)require(count>=0,"Invalid engine step histogram.");
        require(negsrc.clamp_events>=0 && negsrc.age_clamp_events>=0 && negsrc.shortfall_mass>=0,"Invalid engine source diagnostic.");
        require(balance.routing_init_storage>=0 && balance.routing_final_storage>=0,"Negative engine continuity storage.");
        require(balance.routing_init_storage==e.ctx_.mass_balance.routing_init_storage,"Engine initial continuity storage differs.");
        require(lid_drain_q_interp_.empty() || lid_drain_q_interp_.size()==std::size_t(e.ctx_.n_nodes()),"Invalid engine LID-drain interpolation shape.");
        for(double q:lid_drain_q_interp_)require(q>=0,"Negative engine LID drain interpolation.");
        runoff.validateCheckpoint(e.runoff_);lid.validateCheckpoint(e.lid_,std::size_t(e.ctx_.n_subcatches()));
        require(runoff_interp>=0 && runoff_interp<=1 && std::isfinite(runoff_interp),"Invalid engine runoff interpolation.");
        const auto ns=std::size_t(e.ctx_.n_subcatches());
        if(snow_context) {
            for(const auto* row:{&snow_rain,&snow_snow}) {
                require(row->empty()||row->size()==ns,"Invalid snow forcing buffer shape.");
                for(double v:*row)require(std::isfinite(v)&&v>=0,"Invalid snow forcing buffer.");
            }
            const auto& state=snow.state();
            for(std::size_t sc=0;sc<ns;++sc) {
                if(!state.enabled[sc])continue;
                double water=0;for(int k=0;k<3;++k){const auto i=sc*3+k;water+=state.wsnow[i]*state.fArea[i];}
                require(std::abs(water-subcatch.snow_depth[sc])<=1e-10,"Snow and subcatchment checkpoint inventories differ.");
            }
        }
        for(const auto* row:{&wet_q_interp_,&gw_q_interp_,&lid_vlidin_vol_,&lid_qsurf_cfs_,&old_gw_rate_}) {
            require(row->empty() || row->size()==ns,"Invalid engine runoff buffer shape.");
            for(double v:*row)require(std::isfinite(v) && v>=0,"Invalid engine runoff interpolation water.");
        }
        require(gw_q_node_.empty() || gw_q_node_.size()==ns,"Invalid engine groundwater receiver shape.");
        for(int node:gw_q_node_)require(node==-1,"Unqualified engine groundwater receiver.");
        for(const auto* row:{&gw_q_interp_,&old_gw_rate_})for(double v:*row)require(v==0,"Unqualified engine groundwater flow.");
        std::size_t max_units=0;for(int n:e.ctx_.lid_usage.number)max_units+=std::size_t(n);
        require(lid_drain_q_units_.size()<=max_units,"Invalid engine LID drain unit count.");
        for(const auto& [node,q]:lid_drain_q_units_)require(node>=0 && node<e.ctx_.n_nodes() && std::isfinite(q) && q>=0,
                "Invalid engine LID drain receiver/flow.");
        for(const auto* row:{&subcatch.runoff,&subcatch.rainfall,&subcatch.evap_loss,&subcatch.infil_loss,&subcatch.ponded_depth,
                &subcatch.old_ponded_depth,&subcatch.old_runoff,&subcatch.lid_drain_flow,&subcatch.old_lid_drain_flow,
                &subcatch.runon_inflow,&subcatch.old_runon_inflow,&subcatch.runon_rate,&subcatch.outfall_runon_vol,
                &subcatch.lid_return_to_perv_cfs,&subcatch.lid_drain_runon_cfs,&subcatch.stat_precip_vol,&subcatch.stat_evap_vol,
                &subcatch.stat_infil_vol,&subcatch.stat_imperv_vol,&subcatch.stat_perv_vol,&subcatch.stat_runoff_vol,&subcatch.stat_max_runoff})
            for(double v:*row)require(v>=0,"Negative engine subcatchment water.");
        for(const auto* row:{&gages.rainfall,&gages.next_rainfall,&gages.st_rain,&gages.past_rain,&gages.past_rain_accum,&gages.cumul_rain_accum})
            for(double v:*row)require(v>=0,"Negative engine rainfall history.");
        for(double v:gages.past_rain_interval)
            require(v>=0 && v<=3600 && std::floor(v)==v,"Invalid rainfall partial-hour duration.");
        for(std::size_t i=0;i<gages.st_cur.size();++i) {
            auto count=e.ctx_.tables.tables[gages.ts_index[i]].size();
            require(gages.st_cur[i]>=-2 && (gages.st_cur[i]<0 || std::size_t(gages.st_cur[i])<count) &&
                    gages.st_next[i]>=-1 && (gages.st_next[i]<0 || std::size_t(gages.st_next[i])<count) &&
                    gages.st_init[i]<=1 && gages.st_used[i]<=1 && gages.api_rainfall[i]==-1 &&
                    (!gages.st_init[i] || bool(gages.st_used[i])==gage::gageIsUsed(e.ctx_,int(i))) &&
                    (gages.st_init[i] || (gages.st_cur[i]==-2 && gages.st_next[i]==-1 && gages.st_used[i]==0)) &&
                    (gages.st_next[i]<0 || gages.st_next[i]>gages.st_cur[i]),
                    "Invalid engine rainfall cursor/override.");
        }
        if(time>0 && ns>0) {
            require(wet_q_interp_.size()==ns && gw_q_interp_.size()==ns && old_gw_rate_.size()==ns && gw_q_node_.size()==ns,
                    "Missing engine runoff interpolation state.");
            for(std::size_t i=0;i<ns;++i) {
                const int out=subcatch.outlet_node[i];
                const double q=subcatch.area[i]>0 && out>=0 && out<e.ctx_.n_nodes() ?
                    (1-runoff_interp)*subcatch.old_runoff[i]+runoff_interp*subcatch.runoff[i] : 0;
                require(wet_q_interp_[i]==q,"Engine runoff interpolation differs from staged water.");
            }
            std::vector<std::pair<int,double>> drains;std::vector<double> totals(e.ctx_.n_nodes(),0);
            for(const auto& [type,u]:lid.usageOrder()) {
                const auto& g=lid.group(type);const int sc=g.subcatch_idx[u];
                if(sc<0 || std::size_t(sc)>=ns || e.ownsSpatialSource(sc) || subcatch.area[sc]<=0 || subcatch.total_lid_area_ft2[sc]<=0)continue;
                int node=g.drain_node[u];const int dsc=g.drain_subcatch[u];
                if(node<0 && (dsc<0 || dsc==sc))node=subcatch.outlet_node[sc];
                if(node<0 || node>=e.ctx_.n_nodes())continue;
                const double q=(1-runoff_interp)*(g.old_drain_flow[u]*g.area[u])+runoff_interp*(g.drain_flow[u]*g.area[u]);
                drains.emplace_back(node,q);totals[node]+=q;
            }
            require(drains==lid_drain_q_units_ && totals==lid_drain_q_interp_,"Engine ordered LID drain interpolation differs.");
        }
        require(balance.runoff_init_store==e.ctx_.mass_balance.runoff_init_store,"Engine initial runoff storage differs.");
        if(time>0) {
            const auto& s=runoff.soa();double stored=0;
            for(std::size_t i=0;i<ns;++i)if(!e.ownsSpatialSource(int(i)))stored+=(s.depth_imperv0[i]*s.frac_imperv0[i]+s.depth_imperv1[i]*s.frac_imperv1[i]+
                    s.depth_perv[i]*(1-s.imperv_pct[i]))*s.area[i];
            stored+=lid.storedVolume(e.ordinarySourceOwners());
            require(std::isfinite(stored) && (spatialProfile(e) || std::abs(stored-balance.runoff_final_store)<=1e-10*std::max(1.,std::abs(stored))),
                    "Engine runoff final storage differs from staged water.");
        }
        for(std::size_t i=0;i<cursors.size();++i)require(cursors[i].index>=0 && cursors[i].index<static_cast<int>(std::max(std::size_t(1),e.ctx_.tables.tables[i].size())) &&
                (cursors[i].direction==1 || cursors[i].direction==-1),"Invalid engine table cursor.");
        for(std::size_t i=0;i<exfil.btm_ga.size();++i) {checkGreenAmpt(exfil.btm_ga[i],e.exfil_.state().btm_ga[i]);checkGreenAmpt(exfil.bank_ga[i],e.exfil_.state().bank_ga[i]);}
        std::size_t actions=0;for(const auto& r:e.controls_.rules_)actions+=r.then_actions.size()+r.else_actions.size();
        require(action_count>=0 && action_count<=e.ctx_.n_links() && pending.size()<=actions,"Invalid engine control action count.");
        if(heat_profile) {
            std::size_t expected_pid_count=0;
            for(const auto& rule:e.controls_.rules_)for(const auto* list:{&rule.then_actions,&rule.else_actions})
                for(const auto& action:*list)if(action.type==controls::ActionType::PID) {
                    require(action.pid_idx>=0,"Invalid compiled PID action index.");
                    expected_pid_count=std::max(expected_pid_count,std::size_t(action.pid_idx)+1);
                }
            require(pid.size()==expected_pid_count,"Engine PID state shape differs from compiled actions.");
            for(std::size_t i=0;i<pid.size();++i)
                require(pid[i].action_idx==static_cast<int>(i) && std::isfinite(pid[i].kp) &&
                        std::isfinite(pid[i].ki) && std::isfinite(pid[i].kd) &&
                        std::isfinite(pid[i].setpoint) && std::isfinite(pid[i].e1) &&
                        std::isfinite(pid[i].e2),"Invalid engine PID state.");
        }
        for(const auto& p:pending)require(p.link_idx>=0 && p.link_idx<e.ctx_.n_links() && p.rule_idx>=0 && p.rule_idx<static_cast<int>(e.controls_.rules_.size()) &&
                p.type>=controls::ActionType::NUMERIC && p.type<=controls::ActionType::PID,"Invalid engine pending control action.");
        require(slots.size()<=e.ctx_.links.setting.size(),"Invalid engine control slot shape.");
        for(int slot:slots)require(slot>=-1 && slot<static_cast<int>(pending.size()),"Invalid engine control slot index.");
        for(const auto* row:{&orifice_area,&weir_area})for(double area:*row)
            require(area>=0,"Negative engine structure surface area.");
        if(heat_profile)require(orifice_area.size()==std::size_t(e.hydstruct_.orifices_.count) &&
                weir_area.size()==std::size_t(e.hydstruct_.weirs_.count),
                "Engine structure surface-area shape differs.");
    }
    static bool spatialProfile(const SWMMEngine& e) {
#ifdef OPENSWMM_HAS_2D
        return bool(e.source_delivery_);
#else
        return false;
#endif
    }
#ifdef OPENSWMM_HAS_2D
    void validateSource(const SWMMEngine& e,const runoff::SourceWaterDriver& source,
                        const runoff::SourceWaterDeliveryBridge& bridge) const {
        require(bridge.receiverEnd()==boundary[0],"Engine/source receiving clocks differ.");
        if(source.acceptsOutfallReturns())require(source.outfallReturnEnd()==boundary[0],
                                                "Engine/source outfall receipt clocks differ.");
        require(source_totals==e.sourceTotals(source,bridge),"Engine/source completed ledgers differ.");
        for(double v:source_totals)require(std::isfinite(v) && v>=0,"Invalid engine completed-source ledger.");
        const auto& s=runoff.soa();double stored=e.spatialSourceStorage(source)+lid.storedVolume(e.ordinarySourceOwners());
        for(int i=0;i<e.ctx_.n_subcatches();++i)if(!e.ownsSpatialSource(i))
            stored+=(s.depth_imperv0[i]*s.frac_imperv0[i]+s.depth_imperv1[i]*s.frac_imperv1[i]+
                     s.depth_perv[i]*(1-s.imperv_pct[i]))*s.area[i];
        require(std::isfinite(stored) && std::abs(stored-balance.runoff_final_store)<=1e-10*std::max(1.,stored),
                "Engine/source final runoff storage differs.");
        if(boundary[0]>0)require(std::abs(bridge.lastStart()*1000-boundary[3])<=1e-7*std::max(1.,boundary[3]),
                                "Engine/source last receiving interval differs.");
        for(int i=0;i<e.ctx_.n_subcatches();++i)if(e.ownsSpatialSource(i)) {
            require(subcatch.runoff[i]==0 && subcatch.old_runoff[i]==0 && subcatch.runon_inflow[i]==0 &&
                subcatch.runon_rate[i]==0 && subcatch.outfall_runon_vol[i]==0 && subcatch.lid_return_to_perv_cfs[i]==0 &&
                subcatch.lid_drain_runon_cfs[i]==0 && subcatch.lid_drain_flow[i]==0 && subcatch.old_lid_drain_flow[i]==0,
                "Owned catchment has duplicate ordinary transfers.");
        }
    }
#endif
    void install(SWMMEngine& e) noexcept {
        static_assert(std::is_nothrow_move_assignable_v<SimulationContext::MassBalance> &&
                      std::is_nothrow_move_assignable_v<exfil::ExfilSoA> && std::is_nothrow_move_assignable_v<AvgAccumulator> &&
                      std::is_nothrow_move_assignable_v<runoff::RunoffSolver> && std::is_nothrow_move_assignable_v<lid::LIDSolver>);
#define COPY(v) e.v=std::move(v);
        ENGINE(COPY)
        RUNOFF_ENGINE(COPY)
#undef COPY
        if(snow_context) {
            e.snow_=std::move(snow);e.ctx_.snowpacks.runtime=std::move(snow_context->snowpacks.runtime);
            e.snow_rain_=std::move(snow_rain);e.snow_snow_=std::move(snow_snow);
        }
        e.runoff_=std::move(runoff);e.lid_=std::move(lid);e.ctx_.runoff_interp_f=runoff_interp;
#define COPY(v) e.ctx_.subcatches.v=std::move(subcatch.v);
        SUBCATCH_WATER(COPY)
#undef COPY
#define COPY(v) e.ctx_.gages.v=std::move(gages.v);
        GAGE_WATER(COPY)
#undef COPY
#define COPY(v) e.ctx_.lid_usage.v=std::move(lid_usage.v);
        LID_USAGE_WATER(COPY)
#undef COPY
        e.ctx_.mass_balance=std::move(balance);e.ctx_.routing_stats=stats;e.ctx_.negsrc=negsrc;
        int at=0;for(auto* row:{e.ctx_.max_courant_crit,e.ctx_.max_flow_turns,e.ctx_.max_non_converged,e.ctx_.max_mass_bal_errs})
            for(int i=0;i<SimulationContext::MAX_STATS;++i)row[i]=tops[at++];
        e.ctx_.climate_state=climate;e.ctx_.dt_controls_remaining=controls_remaining;
        if(heat_profile) {e.ctx_.heat_state=std::move(heat);e.ctx_.bed_state=std::move(bed);}
        if(reaction_profile) {
            e.ctx_.reactions=std::move(reaction);
            if(prepared_ard)e.ard_=std::move(*prepared_ard);
            if(prepared_lard)e.lard_=std::move(*prepared_lard);
            e.ard_init_attempted_=ard_attempted;
            e.ctx_.subcatches.sweep_last_swept=std::move(subcatch.sweep_last_swept);
            e.ctx_.subcatches.sweep_now=std::move(subcatch.sweep_now);
            e.ctx_.subcatches.sweep_last_date=std::move(subcatch.sweep_last_date);
        }
        e.exfil_.state()=std::move(exfil);e.avg_=std::move(avg);
        for(std::size_t i=0;i<cursors.size();++i)e.ctx_.tables.tables[i].cursor=cursors[i];
        e.controls_.pending_actions_=std::move(pending);e.controls_.action_slots_=std::move(slots);
        e.controls_.last_action_count_=action_count;
        e.controls_.control_value_=control_scratch[0];e.controls_.set_point_=control_scratch[1];e.controls_.eval_date_=control_scratch[2];
        e.controls_.cur_date_=control_scratch[3];e.controls_.cur_time_=control_scratch[4];e.controls_.elapsed_days_=control_scratch[5];
        e.controls_.pid_states_=std::move(pid);
        e.hydstruct_.orifices_.surf_area=std::move(orifice_area);e.hydstruct_.weirs_.surf_area=std::move(weir_area);
#ifdef OPENSWMM_HAS_2D
        if (spatial) e.source_totals_=source_totals;
#endif
        e.xsp_cache_gen_=~uint64_t(0);e.checkpoint_ready_=true;
    }
};

void SWMMEngine::checkCheckpointProfile(bool attaching_surface) const {
    bool advancing_catchments=false;
#ifdef OPENSWMM_HAS_2D
    advancing_catchments=step_active_ && source_stage_ && source_stage_->runtimeCatchmentForcingEnabled();
#endif
    bool advancing_rk2=false;
#ifdef OPENSWMM_HAS_2D
    advancing_rk2=surface_router_.standaloneRk2Checkpointing();
#endif
    require(exchange_target_<0 || advancing_catchments || advancing_rk2,"Engine session checkpoint does not yet support runtime advance/coupling targets.");
    require(ctx_.state==EngineState::RUNNING && ctx_.error_code==0 && do_routing_ &&
            save_results_==(checkpoint_output_?1:0),
            "Engine checkpoint requires a running qualified session with its prepared output mode.");
    if(checkpoint_output_)require(binary_output_plugin_ && binary_output_plugin_->checkpoint_fresh_ &&
            binary_output_plugin_->out_file_ && ctx_.plugin_specs.empty() && plugins_.output_plugins().size()==
#ifdef OPENSWMM_HAS_2D
            (checkpoint_surface_output_?2u:1u) &&
#else
            1u &&
#endif
            std::find(plugins_.output_plugins().begin(),plugins_.output_plugins().end(),binary_output_plugin_)!=plugins_.output_plugins().end() && plugins_.input_plugins().size()==1 &&
            plugins_.state_io_plugins().size()==1 && dynamic_cast<DefaultStateIOPlugin*>(plugins_.state_io_plugins().front()) &&
            plugins_.report_plugins().size()==(rpt_path_.empty()?0u:1u) &&
            (rpt_path_.empty() || dynamic_cast<DefaultReportPlugin*>(plugins_.report_plugins().front())),
            "Binary output checkpoint supports only the built-in binary writer.");
    require(ctx_.rdii_assigns.count()==0 &&
            ctx_.files.rainfall_mode==FileMode::NONE && ctx_.files.runoff_mode==FileMode::NONE &&
            ctx_.files.rdii_mode==FileMode::NONE && ctx_.files.inflows_path.empty() &&
            ctx_.files.outflows_path.empty() &&
            !runoff_iface_file_ && detail::checkpointReactionComponents(ctx_),
            "Engine checkpoint does not recover RDII, interface files, providers or process components.");
#ifdef OPENSWMM_HAS_2D
    require(!ctx_.runtime_coupling_used || surface_router_.runtimeWaterCheckpointing(),
            "Engine checkpoint runtime providers require their initial mesh water profile.");
    if(runtime_water_driver_) {
        require(surface_router_.runtimeWaterCheckpointing() && surface_router_.common_stage_ &&
                runtime_water_driver_->identity()==surface_router_.common_stage_->runtimeWaterIdentity() &&
                !runtime_water_driver_->identity().empty() && runtime_water_driver_->identity().size()<=4096,
                "Water driver requires its matching immutable mesh water identity.");
        const auto next=runtime_water_driver_->nextUpdateSeconds();
        require(std::isfinite(next) && (next==-1 || next>=ctx_.current_time),"Water driver cursor is behind the engine.");
    }
    if(surface_router_.isActive()) {
        if(surface_router_.options().reconstruction_order==2)require(ctx_.snowpacks.config.met_file.empty(),
            "Managed RK2 requires in-memory native weather; file-based radiation is not captured.");
        require(ctx_.events.empty(),"Surface session checkpoint does not yet recover skipped routing events.");
        require(surface_router_.options_.output_file.empty() || checkpoint_surface_output_,
                "Surface session checkpoint requires its prepared mesh output mode.");
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
        if(checkpoint_surface_output_)require(surface_output_plugin_ &&
                surface_output_plugin_->checkpoint_fresh_ && ctx_.plugin_specs.empty() &&
                plugins_.output_plugins().size()==(checkpoint_output_?2u:1u) &&
                std::find(plugins_.output_plugins().begin(),plugins_.output_plugins().end(),surface_output_plugin_)!=plugins_.output_plugins().end() &&
                plugins_.input_plugins().size()==1 && plugins_.state_io_plugins().size()==1 &&
                dynamic_cast<DefaultStateIOPlugin*>(plugins_.state_io_plugins().front()) &&
                plugins_.report_plugins().size()==(rpt_path_.empty()?0u:1u) &&
                (rpt_path_.empty() || dynamic_cast<DefaultReportPlugin*>(plugins_.report_plugins().front())),
                "Mesh output checkpoint supports only the built-in mesh writer.");
#else
        require(!checkpoint_surface_output_,"Mesh output checkpoint support is unavailable.");
#endif
        require(attaching_surface || surface_router_.standaloneRk2Checkpointing() || (surface_router_.joint_hydraulic_==&router_ &&
                surface_router_.common_stage_ && (surface_router_.common_stage_->meshOnly() ||
                 (source_delivery_ && source_stage_==surface_router_.common_stage_))),
                "Surface session checkpoint requires its explicitly attached common stage and engine-owned hydraulic router.");
    } else require(!attaching_surface,"Surface checkpoint attachment requires an active surface router.");
#else
    (void)attaching_surface;
    require(!ctx_.runtime_coupling_used,"Engine checkpoint runtime providers are not qualified.");
#endif
    require(!callbacks_.on_progress && !callbacks_.on_warning && !callbacks_.on_step_begin && !callbacks_.on_step_end,
            "Engine checkpoint does not recover external callbacks.");
    require(ctx_.options.temp_source<=1 && ctx_.options.wind_type!=2 &&
            (ctx_.options.evap_type==0 || ctx_.options.evap_type==1) && ctx_.options.temp_file.empty() && !ctx_.options.rpt_controls,
            "Engine checkpoint requires in-memory climate sources and no control-report continuation.");
    for(const auto& t:ctx_.tables.tables)require(!t.is_file_based && !t.file_handle,"Engine checkpoint cannot recover file-backed tables.");
    const auto& f=ctx_.forcing;
    for(const auto* row:{&f.node_lat_inflow_mode,&f.node_head_boundary_mode,&f.node_quality_mode,&f.node_temperature_mode,&f.node_age_mode,
                        &f.link_flow_mode,&f.link_setting_mode,&f.link_seepage_mode,&f.link_quality_mode,&f.subcatch_rainfall_mode,
                        &f.subcatch_evap_mode,&f.subcatch_snowfall_mode,&f.gage_rainfall_mode})
        for(auto mode:*row)require(mode==ForcingMode::NONE,"Engine checkpoint cannot recover runtime forcing.");
    require(f.climate_temperature_mode==ForcingMode::NONE && f.climate_wind_mode==ForcingMode::NONE && f.climate_evap_mode==ForcingMode::NONE,
            "Engine checkpoint cannot recover runtime climate forcing.");
    require(ctx_.n_subcatches()==0 || ctx_.options.ignore_groundwater,
            "Runoff session checkpoint currently requires IGNORE_GROUNDWATER.");
    for(int aquifer:ctx_.subcatches.gw_aquifer)require(aquifer<0,"Engine checkpoint does not recover assigned groundwater.");
    for(const auto& path:ctx_.lid_usage.rpt_file)require(path.empty(),"Engine checkpoint cannot append detailed LID reports.");
    require(gage_states_.size()==std::size_t(ctx_.n_gages()),"Engine rainfall state shape differs.");
    for(std::size_t i=0;i<ctx_.gages.source.size();++i)require(ctx_.gages.source[i]==RainSource::TIMESERIES &&
            ctx_.gages.ts_index[i]>=0 && ctx_.gages.ts_index[i]<ctx_.n_tables() && ctx_.gages.file_path[i].empty() &&
            ctx_.gages.rain_series[i].empty() && ctx_.gages.api_rainfall[i]==-1,
            "Engine checkpoint supports inline rainfall without API overrides.");
    for(const auto* row:{&gw_frac_perv_,&gw_perv_evap_,&gw_2d_recharge_vol_,&gw_infil_with_2d_,&gw_infil_rate_})
        require(row->empty(),"Engine checkpoint has unqualified hydrology interpolation state.");
    require(lid_drain_q_interp_.empty() || lid_drain_q_interp_.size()==std::size_t(ctx_.n_nodes()),"Engine checkpoint LID-drain shape differs.");
    require(router_.structures_==&hydstruct_,"Engine checkpoint structure solver ownership differs.");
    if(ctx_.reactions.configured) {
        bool mesh_reactions=false;
#ifdef OPENSWMM_HAS_2D
        mesh_reactions=surface_router_.isActive()&&ctx_.n_nodes()==0&&ctx_.n_links()==0&&ctx_.n_subcatches()==0&&
            surface_router_.state().transport.n_msx>0&&
            (twoD::CommonSourceStage::supportsMeshGroundQuality(ctx_,surface_router_.state(),surface_router_.subsurface())||
             (surface_router_.options().reconstruction_order==2&&(attaching_surface||surface_router_.standaloneRk2Checkpointing())));
        require(!surface_router_.isActive()||mesh_reactions,
            "Reaction mesh checkpoints require the matched mesh-only ground quality profile without live network or catchment owners.");
#endif
        require(ctx_.ard_config.detailed_output_path.empty() && (mesh_reactions||
            (!ctx_.options.water_age&&!ctx_.options.heat_transport&&ctx_.reactions.subsurface_laws.empty())),
            "Reaction checkpoints require the qualified 1D profile or mesh-only ground-quality profile; other heat, age, subsurface and ARD detailed-output combinations remain guarded.");
    }
    if(ctx_.routing_stats.sum_iterations>0)for(const auto& lid:ctx_.node_subtypes.storages.lid_state)
        require(lid.quality_old_water.size()==lid.cells.size(),"Engine routed LID previous-water shape differs.");
    router_.checkCheckpointProfile(ctx_);
}
std::string SWMMEngine::checkpointModel() const {
    Archive a;auto o=ctx_.options;
#define F(v) field(a,o.v);
    OPTIONS(F)
#undef F
    if(o.heat_transport) {
        auto heat=ctx_.heat_config;heatConfig(a,heat);
        auto mesh_contact_link=ctx_.heat_state.mesh_contact_link;a(mesh_contact_link);
    }
    detail::reactionModel(a,ctx_);
    auto cm=climateModel(ctx_.climate_state);a(cm);
    auto ext=ctx_.ext_inflows;auto dwf=ctx_.dwf_inflows;auto pat=ctx_.patterns;auto rules=ctx_.control_rules.rule_text;auto events=ctx_.events;
    a(ext.node_idx,ext.node_name,ext.constituent,ext.ts_name,ext.inflow_type,ext.m_factor,ext.s_factor,ext.baseline,ext.pattern_name,
      dwf.node_idx,dwf.node_name,dwf.constituent,dwf.avg_value,dwf.pat1,dwf.pat2,dwf.pat3,dwf.pat4,pat.names,pat.types,pat.factors,rules);
    a.sequence(events,[&](auto& event){fields(a,event.start,event.end);});
    auto ei=inflow_.ext_inflows_;auto di=inflow_.dwf_inflows_;auto patterns=inflow_.patterns_;
    a(ei.count,ei.node_idx,ei.ts_idx,ei.base_pat_idx,ei.baseline,ei.scale_factor,ei.conv_factor,ei.kind,ei.pollut_idx,
      di.count,di.node_idx,di.avg_value,di.pat_monthly,di.pat_daily,di.pat_hourly,di.pat_weekend,di.is_flow,di.pollut_idx,di.msx_idx);
    a.sequence(patterns,[&](auto& p){fields(a,p.type,p.factors);});
    auto count=ctx_.n_tables();a(count);
    for(auto t:ctx_.tables.tables)a(t.id,t.type,t.x,t.y,t.n_relative,t.rel_anchor,t.num_cols,t.column_ids);
    auto cr=controls_.rules_;auto named=controls_.named_vars_;
    a.sequence(cr,[&](auto& r){
        a(r.name,r.priority);
        a.sequence(r.premises,[&](auto& p){a(p.logic,p.is_expression,p.expr_idx,p.lhs_var,p.lhs_idx,p.lhs_param,p.op,p.rhs_is_variable,p.rhs_var,p.rhs_idx,p.rhs_param,p.rhs_value);});
        for(auto* list:{&r.then_actions,&r.else_actions})a.sequence(*list,[&](auto& v){
            a(v.link_idx,v.rule_idx,v.type,v.curve_idx,v.tseries_idx,v.pid_idx,v.value);
        });
    });
    a.sequence(named,[&](auto& n){a(n.name,n.var,n.idx,n.param);});
    auto pid=controls_.pid_states_;
    a.sequence(pid,[&](auto& p){fields(a,p.kp,p.ki,p.kd,p.action_idx);});
    auto pumps=ctx_.link_subtypes.pumps;auto orifices=ctx_.link_subtypes.orifices;
    auto weirs=ctx_.link_subtypes.weirs;auto outlets=ctx_.link_subtypes.outlets;
    a(pumps.link_idx,pumps.curve,pumps.init_state,pumps.startup,pumps.shutoff,pumps.curve_type,
      orifices.link_idx,orifices.orifice_type,orifices.cd,orifices.orate,
      weirs.link_idx,weirs.weir_type,weirs.cd,weirs.end_contractions,weirs.crest_height,weirs.cd2,weirs.can_surcharge,
      weirs.road_width,weirs.road_surface,weirs.cd_curve,
      outlets.link_idx,outlets.outlet_type,outlets.crest_height,outlets.coeff,outlets.expon,outlets.curve);
    // Frozen compiled groups/iteration order; only the held surface areas are mutable.
    auto h=hydstruct_;auto& p=h.pumps_;auto& ori=h.orifices_;auto& w=h.weirs_;auto& q=h.outlets_;
    a(h.nc_indices_,h.nc_group_k_,p.count,p.link_idx,p.curve_idx,p.curve_type,p.speed,p.y_on,p.y_off,
      ori.count,ori.link_idx,ori.shape,ori.length_eff,
      w.count,w.link_idx,w.weir_type,w.c_disch1,w.c_disch2,w.end_con,w.slope,w.cd_curve,w.road_width,w.road_surface,w.length_eff,w.can_surcharge,
      q.count,q.link_idx,q.curve_idx,q.q_coeff,q.q_expon);
    auto x=exfil_.state();a(x.count,x.node_idx,x.btm_area,x.bank_min_depth,x.bank_max_depth,x.bank_max_area);
    for(auto* row:{&x.btm_ga,&x.bank_ga})a.sequence(*row,[&](auto& g){a(g.S,g.Ks,g.IMDmax,g.Fumax,g.Lu);});
    for(auto* row:{ctx_.adjust_temp,ctx_.adjust_evap,ctx_.adjust_rain,ctx_.adjust_hydcon})for(int i=0;i<12;++i){auto v=row[i];a(v);}
    auto sub=ctx_.subcatches;auto g=ctx_.gages;auto lc=ctx_.lid_controls;auto lu=ctx_.lid_usage;
    auto sn=ctx_.subcatch_names.names();auto gn=ctx_.gage_names.names();a(sn,gn);
    a(sub.outlet_node,sub.outlet_subcatch,sub.outlet_name,sub.gage_name,sub.gage,sub.area,sub.width,sub.slope,sub.curb_length,
      sub.rain_scale_factor,sub.snow_scale_factor,sub.frac_imperv,sub.frac_imperv_no_store,sub.pct_zero,sub.n_imperv,sub.n_perv,
      sub.ds_imperv,sub.ds_perv,sub.subarea_routing,sub.pct_routed,sub.infil_model,sub.infil_p1,sub.infil_p2,sub.infil_p3,sub.infil_p4,
      sub.infil_p5,sub.snowpack,sub.snowpack_name,sub.gw_aquifer,sub.gw_node,sub.total_lid_area_ft2,sub.comments,sub.tags,sub.rpt_flag);
    auto adj=ctx_.subcatch_infil_pattern,np=ctx_.subcatch_n_perv_pattern,ds=ctx_.subcatch_d_store_pattern;auto has=ctx_.has_subcatch_adj_patterns;a(adj,np,ds,has);
    a(g.rain_type,g.source,g.ts_index,g.ts_name,g.interval_sec,g.snow_factor,g.scale_factor,g.co_gage_index,g.comments);
    if(std::any_of(ctx_.subcatches.snowpack.begin(),ctx_.subcatches.snowpack.end(),[](int p){return p>=0;})) {
        auto snow=surface::snowModelIdentity(ctx_,snow_.state());a(snow);
    }
    for(int i=0;i<ctx_.n_gages();++i){auto used=gage::gageIsUsed(ctx_,i);a(used);}
    a(lc.names,lc.lid_type,lc.surface,lc.soil,lc.pavement,lc.storage,lc.drain,lc.drainmat,lc.removals,
      lu.subcatch_index,lu.lid_index,lu.number,lu.area,lu.width,lu.init_sat,lu.from_imperv,lu.to_perv,lu.drain_to,lu.from_perv);
    auto runoff=runoff_;auto lid=lid_.waterTrial();runoff.checkpoint(a,true);lid.checkpoint(a,true);
    auto hydraulic=router_.checkpointModel(ctx_);a(hydraulic);
#ifdef OPENSWMM_HAS_2D
    if(source_delivery_) {auto bridge=*source_delivery_;bridge.checkpoint(a,true,source_stage_->sources());}
    if(surface_router_.isActive()&&surface_router_.options().reconstruction_order==2&&ctx_.options.heat_transport) {
        std::string profile="managed RK2 shared radiation model v1";auto identity=surface::snowModelIdentity(ctx_,snow_.state());a(profile,identity);
    }
    if(runtime_water_driver_) {auto identity=runtime_water_driver_->identity();a(identity);}
#endif
    require(a.bytes().size()<=maxBytes,"Engine checkpoint model exceeds its size limit.");return a.bytes();
}
void SWMMEngine::checkCheckpointBoundary() const {
    require(checkpoint_enabled_ && checkpoint_ready_,"Engine checkpoint requires a completed accepted session boundary.");
    checkCheckpointProfile();require(checkpoint_model_==checkpointModel(),"Engine checkpoint model changed after attachment.");
    RestartState state(*this);Archive out;state.archive(out);state.validate(*this);
#ifdef OPENSWMM_HAS_2D
    if(surface_router_.isActive()) {auto error=surface_router_.sourceBoundary(ctx_);require(error.empty(),error.c_str());}
    if(source_delivery_) {
        auto bridge=*source_delivery_;Archive out;bridge.checkpoint(out,false,source_stage_->sources());
        state.validateSource(*this,source_stage_->sources(),bridge);
        auto error=bridge.validateReceiver(ctx_);require(error.empty(),error.c_str());
    }
#endif
}
std::string SWMMEngine::enableCheckpointing() {
    return enableCheckpointingImpl(
#ifdef OPENSWMM_HAS_2D
        nullptr
#endif
    );
}
std::string SWMMEngine::prepareOutputCheckpointing() {
    try {
        require(ctx_.state==EngineState::INITIALIZED && !step_active_ && !runtime_pipeline_ &&
                !checkpoint_enabled_ && !checkpoint_output_ && ctx_.current_time==0 && binary_output_plugin_,
                "Prepare binary output checkpointing after initialize and before start(1).");
        require(ctx_.plugin_specs.empty() && plugins_.output_plugins().size()==1 &&
                plugins_.output_plugins().front()==binary_output_plugin_ && plugins_.input_plugins().size()==1 &&
                plugins_.state_io_plugins().size()==1 && dynamic_cast<DefaultStateIOPlugin*>(plugins_.state_io_plugins().front()) &&
                plugins_.report_plugins().size()==(rpt_path_.empty()?0u:1u) &&
                (rpt_path_.empty() || dynamic_cast<DefaultReportPlugin*>(plugins_.report_plugins().front())),
                "Binary output checkpoint supports only the built-in binary writer.");
#ifdef OPENSWMM_HAS_2D
        require(surface_router_.options_.output_file.empty(),"Surface output continuation is not yet qualified.");
#endif
        std::error_code ec;const auto status=std::filesystem::symlink_status(io::utf8_path(out_path_),ec);
        require((!ec || ec==std::errc::no_such_file_or_directory) && !std::filesystem::exists(status),
                "Binary output checkpoint requires a new output destination.");
        checkpoint_output_=true;binary_output_plugin_->checkpoint_fresh_=true;return {};
    } catch(const std::exception& e) {return e.what();}
}
#ifdef OPENSWMM_HAS_2D
std::string SWMMEngine::prepareSurfaceOutputCheckpointing() {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    try {
        require(ctx_.state==EngineState::INITIALIZED && !step_active_ && !runtime_pipeline_ &&
                !checkpoint_enabled_ && !checkpoint_output_ && !checkpoint_surface_output_ && ctx_.current_time==0 &&
                surface_output_plugin_ && !binary_output_plugin_,
                "Prepare mesh output checkpointing after initialize and before start(0), without a binary writer.");
        require(ctx_.plugin_specs.empty() && plugins_.output_plugins().size()==1 &&
                plugins_.output_plugins().front()==surface_output_plugin_ && plugins_.input_plugins().size()==1 &&
                plugins_.state_io_plugins().size()==1 && dynamic_cast<DefaultStateIOPlugin*>(plugins_.state_io_plugins().front()) &&
                plugins_.report_plugins().size()==(rpt_path_.empty()?0u:1u) &&
                (rpt_path_.empty() || dynamic_cast<DefaultReportPlugin*>(plugins_.report_plugins().front())),
                "Mesh checkpoint requires the built-in mesh writer alone.");
        std::error_code ec;const auto status=std::filesystem::symlink_status(io::utf8_path(surface_output_plugin_->h5_path_),ec);
        require((!ec || ec==std::errc::no_such_file_or_directory) && !std::filesystem::exists(status),
                "Mesh output checkpoint requires a new output destination.");
        checkpoint_surface_output_=true;surface_output_plugin_->checkpoint_fresh_=true;return {};
    } catch(const std::exception& e) {return e.what();}
#else
    return "Mesh output checkpoint support is unavailable in this build.";
#endif
}
std::string SWMMEngine::prepareCombinedOutputCheckpointing() {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    try {
        require(ctx_.state==EngineState::INITIALIZED && !step_active_ && !runtime_pipeline_ &&
                !checkpoint_enabled_ && !checkpoint_output_ && !checkpoint_surface_output_ && ctx_.current_time==0 &&
                surface_router_.isActive() && surface_output_plugin_ && binary_output_plugin_,
                "Prepare combined output checkpointing after initialize and before start(1).");
        const auto& outputs=plugins_.output_plugins();
        require(ctx_.plugin_specs.empty() && outputs.size()==2 &&
                std::find(outputs.begin(),outputs.end(),binary_output_plugin_)!=outputs.end() &&
                std::find(outputs.begin(),outputs.end(),surface_output_plugin_)!=outputs.end() &&
                plugins_.input_plugins().size()==1 && plugins_.state_io_plugins().size()==1 &&
                dynamic_cast<DefaultStateIOPlugin*>(plugins_.state_io_plugins().front()) &&
                plugins_.report_plugins().size()==(rpt_path_.empty()?0u:1u) &&
                (rpt_path_.empty() || dynamic_cast<DefaultReportPlugin*>(plugins_.report_plugins().front())),
                "Combined checkpoints require the two built-in output writers.");
        const auto binary=io::utf8_path(out_path_),mesh=io::utf8_path(surface_output_plugin_->h5_path_);
        require(std::filesystem::weakly_canonical(binary)!=std::filesystem::weakly_canonical(mesh),
                "Combined checkpoint destinations must differ.");
        for(const auto& path:{binary,mesh}) {
            std::error_code ec;const auto status=std::filesystem::symlink_status(path,ec);
            require((!ec || ec==std::errc::no_such_file_or_directory) && !std::filesystem::exists(status),
                    "Combined checkpoint output requires two new destinations.");
        }
        checkpoint_output_=checkpoint_surface_output_=true;
        binary_output_plugin_->checkpoint_fresh_=surface_output_plugin_->checkpoint_fresh_=true;return {};
    } catch(const std::exception& e) {return e.what();}
#else
    return "Combined output checkpoint support is unavailable in this build.";
#endif
}
std::string SWMMEngine::enableCheckpointing(twoD::CommonSourceStage& stage) {return enableCheckpointingImpl(&stage);}
std::string SWMMEngine::enableSpatialSourceCheckpointing(twoD::CommonSourceStage& stage) {return enableCheckpointingImpl(&stage,true);}
std::string SWMMEngine::attachRuntimeWaterDriverCheckpoint(twoD::RuntimeWaterDriverCheckpoint& driver) {
    try {
        require(!step_active_ && !runtime_pipeline_ && !runtime_water_driver_ && !engine_source_history_ && !engine_source_stream_ &&
                ctx_.current_time==0 && checkpoint_enabled_ && surface_router_.runtimeWaterCheckpointing(),
                "Attach a water driver at the initial mesh water checkpoint boundary.");
        checkCheckpointBoundary();
        const auto identity=driver.identity();
        require(!identity.empty() && identity.size()<=4096 && identity==surface_router_.common_stage_->runtimeWaterIdentity(),
                "Water driver immutable replay identity differs from the mesh water profile.");
        std::string hydraulic;const auto error=surface_router_.checkpointBytes(ctx_,hydraulic);require(error.empty(),error.c_str());
        captureDriver(driver,0,hydraulic);
        Archive extra;auto id=identity;extra(id);auto model=checkpoint_model_+extra.bytes();
        checkpoint_model_=std::move(model);runtime_water_driver_=&driver;return {};
    } catch(const std::exception& e) {return e.what();}
}
#endif
#ifdef OPENSWMM_HAS_2D
void SWMMEngine::flushRuntimeCouplingBoundary(bool changing_prescription) {
    // The qualification recorder has only accepted-step frames. Until explicit
    // barrier events are supported, reject before closing work or changing its
    // checkpoint anchor between those frames.
    if (engine_source_history_ && !step_active_ &&
        (changing_prescription || surface_router_.pending_dt_>0))
        throw std::invalid_argument("Engine source history does not yet support completed coupling barriers.");
    if(engine_source_stream_) {
        require(!engine_source_stream_->pending_capture_,"Settle the missing stream capture before changing the coupling boundary.");
        engine_source_stream_->preflight();
    }
    const bool pending=surface_router_.pending_dt_>0;
    surface_router_.flushPendingBatch(ctx_);
    if(source_delivery_) {
        const auto error=source_delivery_->capture(source_stage_->sources(),ctx_.current_time);
        if(!error.empty())throw std::runtime_error(error);
        const auto totals=sourceTotals(source_stage_->sources(),*source_delivery_);
        auto& mb=ctx_.mass_balance;
        double* fields[]{&mb.runoff_rainfall,&mb.runoff_evap,&mb.runoff_infil,&mb.runoff_runon,&mb.runoff_runoff,&mb.runoff_lid_drain};
        for(int k=0;k<6;++k)*fields[k]+=totals[k]-source_totals_[k];
        source_totals_=totals;
        computeFinalStorage();
    }
    if(engine_source_stream_ && !step_active_ && (pending || changing_prescription)) {
        const auto error=engine_source_stream_->capture(*this,twoD::EngineSourceResultStream::Kind::CouplingBarrier,ctx_.current_time);
        if(!error.empty())throw std::runtime_error(error);
    }
}
void SWMMEngine::recordRuntimeCouplingPrescription() {
    if(engine_source_stream_) {
        const auto error=engine_source_stream_->capture(*this,twoD::EngineSourceResultStream::Kind::PrescriptionUpdate,ctx_.current_time);
        if(!error.empty())throw std::runtime_error(error);
    }
}
#endif
std::string SWMMEngine::enableCheckpointingImpl(
#ifdef OPENSWMM_HAS_2D
    twoD::CommonSourceStage* stage, bool own_sources
#endif
) {
    const bool owned=router_.checkpoint_owned_session_;
    const bool aquifer_owned=router_.checkpoint_aquifer_session_;
    const bool fresh_router=router_.checkpoint_context_==nullptr;
    try {
        require(!step_active_ && !runtime_pipeline_ && exchange_target_<0 && !checkpoint_enabled_ && ctx_.current_time==0,
                "Enable engine checkpointing at start's initial session boundary.");
        router_.checkpoint_owned_session_=true;
#ifdef OPENSWMM_HAS_2D
        const bool rk2_attachment=!stage&&surface_router_.isActive()&&surface_router_.options_.reconstruction_order==2;
        router_.checkpoint_aquifer_session_=stage && !surface_router_.subsurface_.nodeBeds().empty();
        require(!stage || own_sources || stage->meshOnly(),"Private catchment ownership requires explicit spatial-source attachment.");
        std::unique_ptr<runoff::SourceWaterDeliveryBridge> bridge;
        if(own_sources) {
            require(stage && stage->matches(&surface_router_.mesh_,&surface_router_.state_,&surface_router_.subsurface_),
                    "Spatial sources require the matching attached surface state.");
            const auto& source=stage->sources();
            require(!source.clocks().groups().empty(),"Spatial source attachment requires selected catchment graphs.");
            bridge=std::make_unique<runoff::SourceWaterDeliveryBridge>();
            auto error=bridge->initialize(source);require(error.empty(),error.c_str());
            error=bridge->validateReceiver(ctx_);require(error.empty(),error.c_str());
            for(int sc:ctx_.node_subtypes.outfalls.route_to)
                require(!bridge->ownsSubcatch(sc) || source.acceptsOutfallReturns(),
                        "Outfall returns into owned catchments need an explicit conservative source receipt.");
            for(const auto& point:surface_router_.coupling_points_)if(point.is_outfall) {
                const int row=ctx_.node_subtypes.outfall_row(point.node_idx);
                require(row<0 || !bridge->ownsSubcatch(ctx_.node_subtypes.outfalls.route_to[row]),
                        "Owned source return cannot also receive a mapped outfall's surface discharge.");
            }
            std::vector<int> seeds;for(int sc=0;sc<ctx_.n_subcatches();++sc)if(bridge->ownsSubcatch(sc))seeds.push_back(sc);
            runoff::SourceWaterDriver expected;
            error=expected.initialize(ctx_,seeds,source.runoff().reviewedAreas(),&lid_,source.acceptsOutfallReturns());require(error.empty(),error.c_str());
            error=expected.configureSpatialCoverage(source.spatialFractions());require(error.empty(),error.c_str());
            auto authored=source.waterTrial();Archive a,b;authored.checkpoint(a,true);expected.checkpoint(b,true);
            require(a.bytes()==b.bytes(),"Spatial source material, forcing or compiled catchment model differs from the live engine.");
            Archive initial,matching;authored.checkpoint(initial,false);expected.checkpoint(matching,false);
            require(initial.bytes()==matching.bytes(),"Spatial source initial water differs from the live engine.");
        }
        checkCheckpointProfile(stage!=nullptr||rk2_attachment);
#else
        checkCheckpointProfile();
#endif
        auto model=checkpointModel();
#ifdef OPENSWMM_HAS_2D
        if(bridge) {Archive extra;bridge->checkpoint(extra,true,stage->sources());model+=extra.bytes();}
#endif
        RestartState state(*this);Archive out;state.archive(out);state.validate(*this);
        router_.enableCheckpointing(ctx_);
#ifdef OPENSWMM_HAS_2D
        if(stage)surface_router_.setJointSourceStage(ctx_,*stage,router_);
        else if(rk2_attachment) {
            // The hydraulic router owns its accepted calendar and any disjoint
            // water-only network stores; the mesh has no shared exchanges.
            surface_router_.joint_hydraulic_=&router_;
            try {surface_router_.attachStandaloneRk2Checkpoint(ctx_);}
            catch(...) {surface_router_.joint_hydraulic_=nullptr;throw;}
        }
#endif
#ifdef OPENSWMM_HAS_2D
        if(bridge) {
            source_stage_=stage;source_delivery_=std::move(bridge);
            if(stage->sources().snow().active()) {
                double initial=0;
                for(int sc=0;sc<ctx_.n_subcatches();++sc) {
                    snow_.state().enabled[sc]=!ownsSpatialSource(sc);
                    if(!ownsSpatialSource(sc)) {
                        const double area=ctx_.subcatches.area[sc]/ucf::UCF(ucf::LANDAREA,ctx_.options)-ctx_.subcatches.total_lid_area_ft2[sc];
                        initial+=surface::nativeSnowVolume(snow_.state(),sc,std::max(0.,area));
                    }
                }
                for(const auto& b:stage->sources().snow().ledgers())initial+=b.initial;
                ctx_.mass_balance.runoff_init_snow=ctx_.mass_balance.runoff_final_snow=initial;
            }

            double ordinary=0;const auto& s=runoff_.soa();
            for(int i=0;i<ctx_.n_subcatches();++i)if(!ownsSpatialSource(i))
                ordinary+=(s.depth_imperv0[i]*s.frac_imperv0[i]+s.depth_imperv1[i]*s.frac_imperv1[i]+s.depth_perv[i]*(1-s.imperv_pct[i]))*s.area[i];
            ordinary+=lid_.storedVolume(ordinarySourceOwners());
            ctx_.mass_balance.runoff_init_store=ctx_.mass_balance.runoff_final_store=ordinary+spatialSourceStorage(stage->sources());
        }
#endif
        // A new manual attachment must not inherit a previous managed-session
        // lifetime/status. The managed builder holds its local owner and
        // reinstalls it immediately after this attachment succeeds.
        checkpoint_session_resources_.reset();
        checkpoint_model_=std::move(model);checkpoint_enabled_=checkpoint_ready_=true;return {};
    } catch(const std::exception& e) {
        router_.checkpoint_owned_session_=owned;
        router_.checkpoint_aquifer_session_=aquifer_owned;
        if(fresh_router) {
            router_.checkpoint_context_=nullptr;router_.checkpoint_model_.clear();
            router_.checkpoint_time_=router_.checkpoint_step_end_=0;
            router_.checkpoint_ready_=router_.checkpoint_prepared_=router_.checkpoint_step_finished_=false;
        }
        return e.what();
    }
}
std::string SWMMEngine::checkpointBytes(std::string& result,bool include_outputs) const {
    try {
        checkCheckpointBoundary();
        RestartState state(*this);Archive payload;state.archive(payload);std::string hydraulic;
        // New report-statistics schema; keep profile/output offsets distinct from old files.
        uint32_t version=103;std::string error;
#ifdef OPENSWMM_HAS_2D
        if(surface_router_.isActive()) {version=source_delivery_?105:104;error=surface_router_.checkpointBytes(ctx_,hydraulic);}
        else
#endif
        error=router_.checkpointBytes(ctx_,hydraulic);
#ifdef OPENSWMM_HAS_2D
        if(surface_router_.runtimeWaterCheckpointing())version+=20;
        if(runtime_water_driver_)version+=40;
#endif
        if(ctx_.options.heat_transport)version+=80;
        if(ctx_.reactions.configured)version+=1000;
        require(error.empty(),error.c_str());payload(hydraulic);
#ifdef OPENSWMM_HAS_2D
        if(source_delivery_) {auto bridge=*source_delivery_;Archive a;bridge.checkpoint(a,false,source_stage_->sources());auto bytes=a.bytes();payload(bytes);}
        if(runtime_water_driver_) {auto driver=captureDriver(*runtime_water_driver_,ctx_.current_time,hydraulic);payload(driver);}
#endif
        if(checkpoint_output_ && include_outputs) {
            require(io_thread_.flush()==0,"Cannot checkpoint failed output writes.");
            auto output=binary_output_plugin_->checkpointBytes(ctx_,ctx_.next_report_ms);payload(output);version+=3;
        }
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
        if(checkpoint_surface_output_ && include_outputs) {
            require(io_thread_.flush()==0,"Cannot checkpoint failed mesh writes.");
            SimulationSnapshot snap;fillSurfaceSnapshot(snap);
            auto output=surface_output_plugin_->checkpointBytes(ctx_,ctx_.next_report_ms,snap);payload(output);version+=6;
        }
#endif
        auto bytes=payload.bytes(),model=checkpoint_model_;auto crc=checksum(bytes);std::string magic=magicName;
        Archive out;out(magic,version,model,crc,bytes);require(out.bytes().size()<=maxBytes,"Engine checkpoint exceeds its size limit.");
        result=out.bytes();return {};
    } catch(const std::exception& e) {return e.what();}
}
std::string SWMMEngine::saveCheckpoint(const std::string& path) const {
    return saveCheckpointImpl(path,false);
}
std::string SWMMEngine::saveCheckpointNew(const std::string& path) const {
    return saveCheckpointImpl(path,true);
}
unsigned long SWMMEngine::checkpointSessionStatus() const noexcept {
    unsigned long flags=checkpoint_enabled_?SWMM_CHECKPOINT_SESSION_ATTACHED:0;
    if(checkpoint_enabled_&&checkpoint_session_resources_)flags|=SWMM_CHECKPOINT_SESSION_MANAGED;
    if(checkpoint_enabled_&&checkpoint_ready_&&!step_active_&&!runtime_pipeline_&&exchange_target_<0&&
        ctx_.state==EngineState::RUNNING&&ctx_.error_code==0&&do_routing_) {
        flags|=SWMM_CHECKPOINT_SESSION_BOUNDARY;
        if(ctx_.current_time==0)flags|=SWMM_CHECKPOINT_SESSION_FRESH;
    }
    return flags;
}
std::string SWMMEngine::saveCheckpointImpl(const std::string& path,bool exclusive) const {
    try {
        if(exclusive) {
            require(!path.empty()&&path.find('\0')==std::string::npos,"Invalid checkpoint destination.");
            require(io::checkpointNoReplaceAvailable(),"Exclusive checkpoint publication unavailable in this build.");
            require(!std::filesystem::exists(std::filesystem::symlink_status(io::utf8_path(path))),"Checkpoint destination already exists.");
        }
        require(!step_active_ && !runtime_pipeline_ && exchange_target_<0,"Cannot save an active engine pipeline.");
        #ifdef OPENSWMM_HAS_2D
        if(checkpoint_file_outputs_)return saveFileOutputCheckpoint(path,exclusive);
        #endif
        std::string bytes;const auto error=checkpointBytes(bytes);require(error.empty(),error.c_str());
#ifdef OPENSWMM_HAS_2D
        require(!engine_source_history_ || engine_source_history_->boundary_==bytes,"Engine source history is not at the accepted checkpoint.");
        if(engine_source_stream_) {
            using Stream=twoD::EngineSourceResultStream;
            std::string physical;const auto snapshot=checkpointBytes(physical,false);require(snapshot.empty(),snapshot.c_str());
            const auto hash=Stream::fingerprint(physical);
            if(engine_source_stream_->latest_.kind==Stream::Kind::SourceInterval || engine_source_stream_->latest_.boundary_fingerprint!=hash) {
                const bool missing_step=engine_source_stream_->engine_end_<ctx_.current_time;
                const auto captured=engine_source_stream_->capture(*this,engine_source_stream_->pending_capture_?engine_source_stream_->pending_kind_:(missing_step?Stream::Kind::EngineStep:Stream::Kind::Checkpoint),
                    engine_source_stream_->pending_capture_?engine_source_stream_->pending_start_:(missing_step?engine_source_stream_->engine_end_:ctx_.current_time),hash);
                require(captured.empty(),captured.c_str());
            }
            auto contract=engine_source_stream_->checkpointContract(hash,Stream::fingerprint(bytes));Archive data;data(bytes,contract);auto crc=checksum(data.bytes());
            std::string magic=streamMagic;uint32_t version=1;Archive wrapped;wrapped(magic,version,crc,bytes,contract);bytes=wrapped.bytes();
            require(bytes.size()<=maxBytes,"Stream-bound engine checkpoint exceeds its size limit.");
        }
#endif
        publish(path,bytes,exclusive);return {};
    } catch(const std::exception& e) {return e.what();}
}
#ifdef OPENSWMM_HAS_2D
void SWMMEngine::detachEngineSourceHistory() noexcept {
    if(auto* cpu=dynamic_cast<twoD::ExplicitInertialSolver*>(surface_router_.solver_.get())) {
        cpu->engine_source_history_=nullptr;cpu->engine_source_stream_=nullptr;
    }
    if(engine_source_stream_)engine_source_stream_->engine_=nullptr;
    engine_source_stream_=nullptr;engine_source_history_=nullptr;checkpoint_file_outputs_=false;
}
std::string SWMMEngine::setEngineSourceResultHistory(twoD::EngineSourceResultHistory& history,bool detailed_source_intervals) {
    try {
        require(!step_active_ && !runtime_pipeline_ && !engine_source_history_ && !engine_source_stream_ && ctx_.current_time==0 && history.tables().empty(),
                "Attach an empty engine source recorder at the initial accepted boundary.");
        auto* cpu=dynamic_cast<twoD::ExplicitInertialSolver*>(surface_router_.solver_.get());
        require(cpu && !cpu->source_history_ && !cpu->engine_source_history_ && !cpu->engine_source_stream_,"Engine source history requires its unbound CPU marcher.");
        auto trial=history;trial.detailed_=detailed_source_intervals;
        const auto error=trial.append(*this,0);require(error.empty(),error.c_str());
        history=std::move(trial);engine_source_history_=&history;
        if(detailed_source_intervals)cpu->engine_source_history_=&history;
        return {};
    } catch(const std::exception& e) {return e.what();}
}
std::string SWMMEngine::resumeEngineSourceResultHistory(twoD::EngineSourceResultHistory& history) {
    try {
        require(!step_active_ && !runtime_pipeline_ && !engine_source_history_ && !engine_source_stream_,"Resume engine source history after restoring the engine checkpoint.");
        auto* cpu=dynamic_cast<twoD::ExplicitInertialSolver*>(surface_router_.solver_.get());
        require(cpu && !cpu->source_history_ && !cpu->engine_source_history_ && !cpu->engine_source_stream_,"Engine source history requires its unbound CPU marcher.");
        const auto error=history.resume(*this);require(error.empty(),error.c_str());engine_source_history_=&history;
        if(history.detailed_)cpu->engine_source_history_=&history;
        return {};
    } catch(const std::exception& e) {return e.what();}
}
std::string SWMMEngine::setEngineSourceResultStream(twoD::EngineSourceResultStream& stream,const std::string& path,bool detailed) {
    try {
        require(!step_active_ && !runtime_pipeline_ && !engine_source_history_ && !engine_source_stream_ && ctx_.current_time==0,
            "Attach a stream at the initial accepted boundary.");
        auto* cpu=dynamic_cast<twoD::ExplicitInertialSolver*>(surface_router_.solver_.get());
        require(cpu && !cpu->source_history_ && !cpu->engine_source_history_ && !cpu->engine_source_stream_,"Stream requires its unbound CPU marcher.");
        const auto error=stream.initialize(*this,path,detailed);require(error.empty(),error.c_str());
        engine_source_stream_=&stream;cpu->engine_source_stream_=&stream;return {};
    } catch(const std::exception& e){return e.what();}
}

std::string SWMMEngine::enableFileOutputCheckpointing() {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    try {
        require(!step_active_ && !runtime_pipeline_ && ctx_.current_time==0 && engine_source_stream_ && checkpoint_output_ && checkpoint_surface_output_ && !checkpoint_file_outputs_,
            "Select file companions at the initial combined-output source-stream boundary.");
        checkCheckpointBoundary();checkpoint_file_outputs_=true;return {};
    }catch(const std::exception& e){return e.what();}
#else
    return "File-backed combined output checkpoints unavailable in this build.";
#endif
}
std::string SWMMEngine::saveFileOutputCheckpoint(const std::string& path,bool exclusive) const {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    try {
        require(checkpoint_file_outputs_ && engine_source_stream_ && checkpoint_output_ && checkpoint_surface_output_,"File checkpoint profile differs.");
        require(io_thread_.flush()==0,"Cannot checkpoint failed output writes.");
        std::string physical;const auto error=checkpointBytes(physical,false);require(error.empty(),error.c_str());using Stream=twoD::EngineSourceResultStream;
        const auto hash=Stream::fingerprint(physical);
        if(engine_source_stream_->pending_capture_ || engine_source_stream_->latest_.kind==Stream::Kind::SourceInterval || engine_source_stream_->latest_.boundary_fingerprint!=hash) {
            const bool missing=engine_source_stream_->engine_end_<ctx_.current_time;
            const auto captured=engine_source_stream_->capture(*this,engine_source_stream_->pending_capture_?engine_source_stream_->pending_kind_:(missing?Stream::Kind::EngineStep:Stream::Kind::Checkpoint),
                engine_source_stream_->pending_capture_?engine_source_stream_->pending_start_:(missing?engine_source_stream_->engine_end_:ctx_.current_time),hash);require(captured.empty(),captured.c_str());
        }
        std::shared_ptr<io::OwnedCheckpointFile> binary_copy,mesh_copy;
        auto binary=binary_output_plugin_->captureFileCheckpoint(ctx_,ctx_.next_report_ms,io::utf8_path(path+".binary"),binary_copy);
        SimulationSnapshot snap;fillSurfaceSnapshot(snap);auto mesh=surface_output_plugin_->captureFileCheckpoint(ctx_,ctx_.next_report_ms,snap,io::utf8_path(path+".mesh"),mesh_copy);
        Archive binding;binding(physical,binary,mesh);auto source=engine_source_stream_->checkpointContract(hash,Stream::fingerprint(binding.bytes()));
        Archive data;data(physical,binary,mesh,source);auto crc=checksum(data.bytes());std::string magic=fileMagic;uint32_t version=1;Archive out;out(magic,version,crc,physical,binary,mesh,source);
        require(out.bytes().size()<=maxBytes,"File-backed physical checkpoint exceeds its model-state limit.");publish(path,out.bytes(),exclusive);binary_copy->retain();mesh_copy->retain();return {};
    }catch(const std::exception& e){return e.what();}
#else
    return "File-backed combined output checkpoints unavailable in this build.";
#endif
}

#endif
std::string SWMMEngine::loadCheckpoint(const std::string& path) {
    return loadCheckpointPath(path,nullptr);
}
std::string SWMMEngine::inspectCheckpoint(const std::string& path,CheckpointInspection& inspection) {
    CheckpointInspection candidate;const auto error=loadCheckpointPath(path,&candidate);
    if(error.empty())inspection=std::move(candidate);
    return error;
}
std::string SWMMEngine::loadCheckpointPath(const std::string& path,CheckpointInspection* inspection) {
    try {
        auto bytes=readFile(path);Archive peek(bytes);std::string magic;peek(magic);
        const auto size=bytes.size();
        auto inspected=[&](std::string error,bool bundle=false) {
            if(error.empty()&&inspection){inspection->checkpoint_bytes=size;inspection->relocatable_bundle=bundle;}
            return error;
        };
        if(magic==fileMagic) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
            require(checkpoint_file_outputs_,"Select the file-backed output profile before restore.");
            FileCheckpointEnvelope saved(std::move(bytes));
            const auto binding=twoD::EngineSourceResultStream::fingerprint(saved.binding());
            resolveBundle(saved,path);
            if(saved.version==2) {
                StreamContract source(saved.source);
                const auto cursor=twoD::EngineSourceResultStream::decodeCursor(source.cursor);
                require(io::checkpointFileSize(io::utf8_path(source.path))==cursor.bytes,"Bundle source prefix size changed.");
            }
            return inspected(loadCheckpointBytes(std::move(saved.physical),saved.source,saved.binary,saved.mesh,binding,inspection),saved.version==2);
#else
            return "File-backed output checkpoints unavailable in this build.";
#endif
        }
        if(magic!=streamMagic)return inspected(loadCheckpointBytes(std::move(bytes),{},{},{},0,inspection));
#ifdef OPENSWMM_HAS_2D
        Archive in(std::move(bytes));uint32_t version,crc;std::string physical,contract;in(magic,version,crc,physical,contract);in.finish();
        Archive payload;payload(physical,contract);require(version==1 && crc==checksum(payload.bytes()),"Stream-bound checkpoint checksum or version differs.");
        return inspected(loadCheckpointBytes(std::move(physical),contract,{},{},0,inspection));
#else
        return "Source stream checkpoint support unavailable in this build.";
#endif
    }catch(const std::exception& e){return e.what();}
}
std::string SWMMEngine::packageCheckpointBundle(const std::string& checkpoint,const std::string& directory) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    try {
        require(!checkpoint.empty()&&checkpoint.find('\0')==std::string::npos&&
            !directory.empty()&&directory.find('\0')==std::string::npos,"Invalid checkpoint bundle path.");
        FileCheckpointEnvelope saved(readFile(checkpoint));using Stream=twoD::EngineSourceResultStream;
        const auto binding=Stream::fingerprint(saved.binding());resolveBundle(saved,checkpoint);
        FileOutputContract binary(saved.binary,false),mesh(saved.mesh,true);StreamContract source(saved.source);
        require(source.whole==binding&&source.anchor==Stream::fingerprint(saved.physical),"Bundle physical/source binding differs.");
        Archive physical(saved.physical);std::string magic,model,state;uint32_t version,crc;
        physical(magic,version,model,crc,state);physical.finish();
        require(magic==magicName&&crc==checksum(state),"Bundle physical envelope checksum differs.");
        const auto cursor=Stream::decodeCursor(source.cursor);
        const auto size=io::checkpointFileSize(io::utf8_path(source.path));
        require(saved.version==2?size==cursor.bytes:size>=cursor.bytes,"Bundle source prefix size differs.");
        auto validate_source=[&](const std::string& path) {
            uint64_t anchor=0;Stream::Kind kind=Stream::Kind::SourceInterval;
            const auto error=Stream::visit(path,cursor,[&](const auto& frame){anchor=frame.boundary_fingerprint;kind=frame.kind;});
            require(error.empty(),error.c_str());
            require(kind!=Stream::Kind::SourceInterval&&anchor==source.anchor,"Bundle source prefix does not match its physical state.");
        };
        validate_source(source.path);binary.file.verify();mesh.file.verify();
        io::CheckpointDirectory bundle(std::filesystem::absolute(io::utf8_path(directory)));
        auto copy=[&](const std::filesystem::path& path,uint64_t bytes,const char* name) {
            const auto target=bundle.staging()/name;
            auto owned=io::copyCheckpointFile(path,bytes,target);
            std::filesystem::rename(owned->file.path,target);owned->file.path=target;owned->retain();
            return owned->file;
        };
        const auto binary_copy=copy(binary.file.path,binary.file.bytes,"binary.out");
        const auto mesh_copy=copy(mesh.file.path,mesh.file.bytes,"mesh.h5");
        require(binary_copy.fingerprint==binary.file.fingerprint&&mesh_copy.fingerprint==mesh.file.fingerprint,
            "Bundle companion changed while copying.");
        const auto source_copy=copy(io::utf8_path(source.path),cursor.bytes,"source.history");
        validate_source(io::path_utf8(source_copy.path));
        binary.file.path=io::utf8_path("binary.out");mesh.file.path=io::utf8_path("mesh.h5");
        saved.binary=binary.encode(false);saved.mesh=mesh.encode(true);
        source.path="source.history";source.whole=Stream::fingerprint(saved.binding());saved.source=source.encode();saved.version=2;
        const auto bytes=saved.encode(),manifest=io::path_utf8(bundle.staging()/"session.chk");
        publish(manifest,bytes);require(readFile(manifest)==bytes,"Cannot verify staged bundle manifest.");bundle.publish();return {};
    }catch(const std::exception& e){return e.what();}
#else
    return "Checkpoint bundle packaging unavailable in this build.";
#endif
}
std::string SWMMEngine::loadCheckpointBytes(std::string checkpoint,const std::string& stream_contract,
    const std::string& binary_file,const std::string& mesh_file,uint64_t whole_binding,CheckpointInspection* inspection) {
    try {
        CheckpointInspection summary;
        require(!step_active_ && !runtime_pipeline_ && exchange_target_<0 && ctx_.current_time==0,"Engine restore requires an identically initialized fresh session.");
#ifdef OPENSWMM_HAS_2D
        require(!engine_source_history_,"Restore the engine checkpoint before attaching its source history.");
#endif
        checkCheckpointBoundary();
#ifdef OPENSWMM_HAS_2D
        require(checkpoint_file_outputs_==(!binary_file.empty()&&!mesh_file.empty()),"Checkpoint file-output profile differs.");
        require(bool(engine_source_stream_)==!stream_contract.empty(),"Stream checkpoints require their fresh stream destination; old checkpoints require no stream.");
        std::unique_ptr<twoD::EngineSourceResultStream::PreparedRestore> prepared_stream;
        if(engine_source_stream_)prepared_stream=engine_source_stream_->prepareRestore(stream_contract,!binary_file.empty()?whole_binding:twoD::EngineSourceResultStream::fingerprint(checkpoint));
#else
        require(stream_contract.empty()&&binary_file.empty()&&mesh_file.empty(),"Source stream/file checkpoint support unavailable.");
#endif
        Archive in(std::move(checkpoint));std::string magic,model,bytes;uint32_t version=0,crc=0;
        in(magic,version,model,crc,bytes);in.finish();
        const bool files=!binary_file.empty();
        uint32_t expected_version=103;
#ifdef OPENSWMM_HAS_2D
        if(surface_router_.isActive())expected_version=source_delivery_?105:104;
        if(surface_router_.runtimeWaterCheckpointing())expected_version+=20;
        if(runtime_water_driver_)expected_version+=40;
#endif
        if(checkpoint_output_&&!files)expected_version+=3;
#ifdef OPENSWMM_HAS_2D
        if(checkpoint_surface_output_&&!files)expected_version+=6;
#endif
        if(ctx_.options.heat_transport)expected_version+=80;
        if(ctx_.reactions.configured)expected_version+=1000;
        require(magic==magicName && version==expected_version && model==checkpoint_model_ && crc==checksum(bytes),"Engine checkpoint format/model/checksum differs.");
        RestartState state(*this);Archive payload(std::move(bytes));state.archive(payload);std::string hydraulic;payload(hydraulic);
#ifdef OPENSWMM_HAS_2D
        std::string delivery;if(source_delivery_)payload(delivery);
        std::string driver;if(runtime_water_driver_)payload(driver);
#endif
        std::string output;if(checkpoint_output_&&!files)payload(output);
        std::string mesh_output;
#ifdef OPENSWMM_HAS_2D
        if(checkpoint_surface_output_&&!files)payload(mesh_output);
#endif
        payload.finish();state.validate(*this);
        if(state.reaction_profile) {
            auto staging_context=ctx_;
            state.prepared_ard=std::make_unique<transport::ArdEngine>();
            state.prepared_lard=std::make_unique<lard::LagrangianSolver>();
            state.prepared_ard->restoreCheckpointState(state.ard_bytes,staging_context);
            state.prepared_lard->restoreCheckpointState(state.lard_bytes,staging_context);
        }
        if(inspection) {
            summary.engine_seconds=state.boundary[0];summary.engine_date=state.boundary[1];
            summary.start_date=ctx_.options.start_date;summary.report_start_date=ctx_.options.report_start;
            summary.next_report_seconds=state.boundary[4]/1000;
            summary.file_outputs=!binary_file.empty();
            if(checkpoint_output_)summary.binary=inspectReports(binary_file.empty()?output:binary_file,false,ctx_,state.boundary[4]);
#ifdef OPENSWMM_HAS_2D
            if(checkpoint_surface_output_)summary.mesh=inspectReports(mesh_file.empty()?mesh_output:mesh_file,true,ctx_,state.boundary[4]);
            if(prepared_stream) {
                summary.detailed_sources=engine_source_stream_->detailed();
                summary.source_prefix_bytes=prepared_stream->cursor.bytes;summary.source_frames=prepared_stream->cursor.frames;
                Archive a(stream_contract);std::string path,cursor;uint64_t anchor,whole;a(path,cursor,anchor,whole);a.finish();
                summary.companions.push_back({CheckpointInspection::Companion::Role::Source,path,summary.source_prefix_bytes});
            }
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
            if(summary.file_outputs)for(const auto& item:std::array<std::pair<std::string,bool>,2>{{{binary_file,false},{mesh_file,true}}}) {
                FileOutputContract contract(item.first,item.second);
                summary.companions.push_back({item.second?CheckpointInspection::Companion::Role::Mesh:CheckpointInspection::Companion::Role::Binary,
                    io::path_utf8(contract.file.path),contract.file.bytes});
            }
#endif
#endif
        }
#ifdef OPENSWMM_HAS_2D
        std::unique_ptr<WaterDriver::PreparedRestore> prepared_driver;
        if(runtime_water_driver_) {
            if(inspection)summary.driver.emplace();
            prepared_driver=prepareDriver(*runtime_water_driver_,state.boundary[0],hydraulic,std::move(driver),inspection?&*summary.driver:nullptr);
        }
#endif
        std::shared_ptr<DefaultOutputPlugin::PreparedCheckpoint> prepared_output;
        std::function<void()> publish_output;
        if(checkpoint_output_) {
            require(io_thread_.flush()==0,"Cannot restore failed output writes.");
            prepared_output=files?binary_output_plugin_->prepareFileCheckpoint(ctx_,state.boundary[4],binary_file):
                binary_output_plugin_->prepareCheckpoint(ctx_,state.boundary[4],std::move(output));
            publish_output=[&]{binary_output_plugin_->installCheckpoint(*prepared_output);};
        }
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
        std::shared_ptr<twoD::Default2DOutputPlugin::PreparedCheckpoint> prepared_mesh;
        if(checkpoint_surface_output_) {
            require(io_thread_.flush()==0,"Cannot restore failed mesh writes.");
            SimulationSnapshot snap;fillSurfaceSnapshot(snap);snap.sim_time=state.boundary[0];
            twoD::SourceResultHistory initial;
            if(source_stage_) {const auto error=initial.append(*source_stage_);require(error.empty(),error.c_str());}
            prepared_mesh=files?surface_output_plugin_->prepareFileCheckpoint(ctx_,state.boundary[4],snap,mesh_file):
                surface_output_plugin_->prepareCheckpoint(ctx_,state.boundary[4],snap,source_stage_?&initial:nullptr,std::move(mesh_output));
            publish_output=[&]{
                if(!prepared_output) {surface_output_plugin_->installCheckpoint(*prepared_mesh);return;}
                std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
                io::CheckpointOutputPublication publication({binary_output_plugin_->checkpointPublication(*prepared_output),
                    surface_output_plugin_->checkpointPublication(*prepared_mesh)});
                publication.publish();
                binary_output_plugin_->adoptCheckpoint(*prepared_output);
                surface_output_plugin_->adoptCheckpoint(*prepared_mesh);
            };
        }
#endif
#ifdef OPENSWMM_HAS_2D
        if(prepared_stream&&!inspection) {
            std::vector<io::CheckpointOutputPublication::Output> outputs;
            if(prepared_output)outputs.push_back(binary_output_plugin_->checkpointPublication(*prepared_output));
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
            if(prepared_mesh)outputs.push_back(surface_output_plugin_->checkpointPublication(*prepared_mesh));
#endif
            outputs.push_back(prepared_stream->publication);
            publish_output=[&,outputs=std::move(outputs)] {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
                std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
#endif
                io::CheckpointOutputPublication publication(outputs);publication.publish();
                if(prepared_output)binary_output_plugin_->adoptCheckpoint(*prepared_output);
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
                if(prepared_mesh)surface_output_plugin_->adoptCheckpoint(*prepared_mesh);
#endif
                engine_source_stream_->adopt(*prepared_stream);
            };
        }
        if(surface_router_.isActive()) {
            std::function<void()> before_install;
            if(publish_output || prepared_driver)before_install=[&] {
                if(publish_output)publish_output();
                if(prepared_driver)prepared_driver->install();
            };
            std::optional<runoff::SourceWaterDeliveryBridge> bridge;
            std::function<void(const runoff::SourceWaterDriver&)> validate_source;
            if(source_delivery_) {
                bridge=*source_delivery_;
                validate_source=[&](const auto& source) {
                    Archive a(std::move(delivery));bridge->checkpoint(a,false,source);a.finish();
                    state.validateSource(*this,source,*bridge);
                };
            }
            auto error=surface_router_.loadCheckpointBytes(ctx_,std::move(hydraulic),&state.boundary,&state.gages.rainfall,
                &state.balance.routing_final_storage,state.stats.sum_iterations>0,bridge?&validate_source:nullptr,
                before_install&&!inspection?&before_install:nullptr,inspection?&summary.completed_source_seconds.emplace():nullptr,&state.climate,&state.heat);
            require(error.empty(),error.c_str());
            if(inspection) {
                if(prepared_stream)require(*summary.completed_source_seconds==prepared_stream->latest.source_end,"Inspected source clocks differ.");
                *inspection=std::move(summary);return {};
            }
            if(bridge) {static_assert(std::is_nothrow_move_assignable_v<runoff::SourceWaterDeliveryBridge>);*source_delivery_=std::move(*bridge);}
            state.install(*this);return {};
        }
#endif
        auto routing=router_.prepareCheckpointBytes(ctx_,std::move(hydraulic),&state.boundary);
        if(state.boundary[0]>0)Router::validateSessionStorage(routing,ctx_,state.balance.routing_final_storage,state.stats.sum_iterations>0);
        if(inspection){*inspection=std::move(summary);return {};}
        // All throwing parse/model/shape/ledger checks precede installation.
        if(prepared_output)publish_output();
        router_.installCheckpoint(ctx_,std::move(routing));state.install(*this);return {};
    } catch(const std::exception& e) {return e.what();}
}
} // namespace openswmm
