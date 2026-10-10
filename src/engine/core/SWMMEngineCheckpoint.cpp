// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "SWMMEngine.hpp"
#include "CompletedSourceArchive.hpp"
#ifdef OPENSWMM_HAS_2D
#include "../2d/subsurface/CommonSourceStage.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace openswmm {
namespace {
using Archive=detail::CompletedSourceArchive;
constexpr uint64_t maxBytes=64*1024*1024;
constexpr const char* magicName="OpenSWMM accepted water-only engine session";
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
uint32_t checksum(const std::string& bytes) {
    uint32_t crc=~uint32_t(0);
    for(unsigned char c:bytes) {crc^=c;for(int k=0;k<8;++k)crc=(crc>>1)^((crc&1)?0xedb88320u:0u);}
    return ~crc;
}
std::string readFile(const std::string& path) {
    std::error_code ec;auto size=std::filesystem::file_size(path,ec);
    require(!ec && size>0 && size<=maxBytes,"Engine checkpoint is missing or exceeds its size limit.");
    std::ifstream in(path,std::ios::binary);std::string bytes(size,'\0');in.read(bytes.data(),bytes.size());
    require(bool(in) && in.peek()==std::char_traits<char>::eof(),"Engine checkpoint read failed.");return bytes;
}
void publish(const std::string& path,const std::string& bytes) {
    if(std::filesystem::exists(path)) {
        Archive old(readFile(path));std::string magic;old(magic);
        require(magic==magicName,"Engine checkpoint destination is an unrelated file.");
    }
    static std::atomic<uint64_t> serial{0};auto part=path+".session-part-"+std::to_string(++serial);
    FILE* f=std::fopen(part.c_str(),"wbx");require(f,"Cannot create engine checkpoint publication file.");
    bool written=std::fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();bool closed=std::fclose(f)==0;
    if(!written || !closed) {std::filesystem::remove(part);throw std::runtime_error("Engine checkpoint write failed.");}
    std::error_code ec;std::filesystem::rename(part,path,ec);
    if(ec) {std::filesystem::remove(part);throw std::runtime_error("Cannot publish engine checkpoint: "+ec.message());}
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
    X(subarea_runoff_rate) X(lid_return_to_perv_cfs) X(lid_drain_runon_cfs) X(stat_precip_vol) X(stat_evap_vol) X(stat_infil_vol) \
    X(stat_imperv_vol) X(stat_perv_vol) X(stat_runoff_vol) X(stat_max_runoff) X(stat_gw_infil_vol) X(stat_gw_upper_evap_vol) \
    X(stat_gw_lower_evap_vol) X(stat_gw_deep_perc_vol) X(stat_gw_flow_vol) X(stat_gw_max_flow) X(stat_gw_sum_theta) \
    X(stat_gw_sum_depth) X(stat_gw_final_theta) X(stat_gw_final_depth) X(stat_gw_steps) X(snow_net_imperv) X(snow_net_perv) \
    X(snow_melt_imperv) X(snow_melt_perv) X(snow_melt_age_imperv) X(snow_melt_age_perv)
#define GAGE_WATER(X) \
    X(rainfall) X(next_rainfall) X(api_rainfall) X(next_rain_date) X(is_raining) X(st_cur) X(st_next) X(st_rain) \
    X(st_init) X(st_used) X(past_rain) X(past_rain_accum) X(past_rain_time) X(cumul_rain_accum)
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
    std::array<double,6> boundary;
    double controls_remaining;
    std::vector<TableCursor> cursors;
    std::vector<controls::ControlEngine::PendingAction> pending;
    std::vector<int> slots;
    int action_count;
    std::array<double,6> control_scratch;
    std::vector<controls::PIDState> pid;
    std::vector<double> orifice_area,weir_area;
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
        std::string marker="engine runoff/rainfall boundary v3";a(marker);
        require(marker=="engine runoff/rainfall boundary v3","Invalid engine hydrology record.");
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
#define F(v) a(v);
        RUNOFF_ENGINE(F)
#undef F
        a.sequence(pid,[&](auto& p){fields(a,p.setpoint,p.e1,p.e2);},true);
        a.fixed(orifice_area,weir_area);finite(orifice_area);finite(weir_area);
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
        require(runoff_interp>=0 && runoff_interp<=1 && std::isfinite(runoff_interp) && !has_snow_,"Invalid engine runoff interpolation.");
        const auto ns=std::size_t(e.ctx_.n_subcatches());
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
                if(sc<0 || std::size_t(sc)>=ns || subcatch.area[sc]<=0 || subcatch.total_lid_area_ft2[sc]<=0)continue;
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
            for(std::size_t i=0;i<ns;++i)stored+=(s.depth_imperv0[i]*s.frac_imperv0[i]+s.depth_imperv1[i]*s.frac_imperv1[i]+
                    s.depth_perv[i]*(1-s.imperv_pct[i]))*s.area[i];
            stored+=lid.storedVolume();
            require(std::isfinite(stored) && std::abs(stored-balance.runoff_final_store)<=1e-10*std::max(1.,std::abs(stored)),
                    "Engine runoff final storage differs from staged water.");
        }
        for(std::size_t i=0;i<cursors.size();++i)require(cursors[i].index>=0 && cursors[i].index<static_cast<int>(std::max(std::size_t(1),e.ctx_.tables.tables[i].size())) &&
                (cursors[i].direction==1 || cursors[i].direction==-1),"Invalid engine table cursor.");
        for(std::size_t i=0;i<exfil.btm_ga.size();++i) {checkGreenAmpt(exfil.btm_ga[i],e.exfil_.state().btm_ga[i]);checkGreenAmpt(exfil.bank_ga[i],e.exfil_.state().bank_ga[i]);}
        std::size_t actions=0;for(const auto& r:e.controls_.rules_)actions+=r.then_actions.size()+r.else_actions.size();
        require(action_count>=0 && action_count<=e.ctx_.n_links() && pending.size()<=actions,"Invalid engine control action count.");
        for(const auto& p:pending)require(p.link_idx>=0 && p.link_idx<e.ctx_.n_links() && p.rule_idx>=0 && p.rule_idx<static_cast<int>(e.controls_.rules_.size()) &&
                p.type>=controls::ActionType::NUMERIC && p.type<=controls::ActionType::PID,"Invalid engine pending control action.");
        require(slots.size()<=e.ctx_.links.setting.size(),"Invalid engine control slot shape.");
        for(int slot:slots)require(slot>=-1 && slot<static_cast<int>(pending.size()),"Invalid engine control slot index.");
        for(const auto* row:{&orifice_area,&weir_area})for(double area:*row)
            require(area>=0,"Negative engine structure surface area.");
    }
    void install(SWMMEngine& e) noexcept {
        static_assert(std::is_nothrow_move_assignable_v<SimulationContext::MassBalance> &&
                      std::is_nothrow_move_assignable_v<exfil::ExfilSoA> && std::is_nothrow_move_assignable_v<AvgAccumulator> &&
                      std::is_nothrow_move_assignable_v<runoff::RunoffSolver> && std::is_nothrow_move_assignable_v<lid::LIDSolver>);
#define COPY(v) e.v=std::move(v);
        ENGINE(COPY)
        RUNOFF_ENGINE(COPY)
#undef COPY
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
        e.exfil_.state()=std::move(exfil);e.avg_=std::move(avg);
        for(std::size_t i=0;i<cursors.size();++i)e.ctx_.tables.tables[i].cursor=cursors[i];
        e.controls_.pending_actions_=std::move(pending);e.controls_.action_slots_=std::move(slots);
        e.controls_.last_action_count_=action_count;
        e.controls_.control_value_=control_scratch[0];e.controls_.set_point_=control_scratch[1];e.controls_.eval_date_=control_scratch[2];
        e.controls_.cur_date_=control_scratch[3];e.controls_.cur_time_=control_scratch[4];e.controls_.elapsed_days_=control_scratch[5];
        e.controls_.pid_states_=std::move(pid);
        e.hydstruct_.orifices_.surf_area=std::move(orifice_area);e.hydstruct_.weirs_.surf_area=std::move(weir_area);
        e.xsp_cache_gen_=~uint64_t(0);e.checkpoint_ready_=true;
    }
};

void SWMMEngine::checkCheckpointProfile(bool attaching_surface) const {
    require(exchange_target_<0,"Engine session checkpoint does not yet support runtime advance/coupling targets.");
    require(ctx_.state==EngineState::RUNNING && ctx_.error_code==0 && do_routing_ && save_results_==0,
            "Engine checkpoint requires a running water-only start(0) hydraulic session.");
    require(ctx_.rdii_assigns.count()==0 && !ctx_.files.has_any() &&
            !runoff_iface_file_ && !ctx_.runtime_coupling_used && ctx_.process_component_specs.empty(),
            "Engine checkpoint does not recover RDII, interface files, providers or process components.");
#ifdef OPENSWMM_HAS_2D
    if(surface_router_.isActive()) {
        require(ctx_.events.empty(),"Surface session checkpoint does not yet recover skipped routing events.");
        require(surface_router_.options_.output_file.empty(),"Surface session checkpoint cannot append surface output.");
        require(attaching_surface || (surface_router_.joint_hydraulic_==&router_ &&
                surface_router_.common_stage_ && surface_router_.common_stage_->meshOnly()),
                "Surface session checkpoint requires its mesh-only common stage and engine-owned hydraulic router.");
    } else require(!attaching_surface,"Surface checkpoint attachment requires an active surface router.");
#else
    (void)attaching_surface;
#endif
    require(!callbacks_.on_progress && !callbacks_.on_warning && !callbacks_.on_step_begin && !callbacks_.on_step_end,
            "Engine checkpoint does not recover external callbacks.");
    require(ctx_.options.temp_source==0 && ctx_.options.wind_type!=2 && ctx_.options.humidity_type!=2 &&
            (ctx_.options.evap_type==0 || ctx_.options.evap_type==1) && ctx_.options.temp_file.empty() && !ctx_.options.rpt_controls,
            "Engine checkpoint supports in-memory constant/monthly climate and no control-report continuation.");
    for(const auto& t:ctx_.tables.tables)require(!t.is_file_based && !t.file_handle,"Engine checkpoint cannot recover file-backed tables.");
    const auto& f=ctx_.forcing;
    for(const auto* row:{&f.node_lat_inflow_mode,&f.node_head_boundary_mode,&f.node_quality_mode,&f.node_temperature_mode,&f.node_age_mode,
                        &f.link_flow_mode,&f.link_setting_mode,&f.link_seepage_mode,&f.link_quality_mode,&f.subcatch_rainfall_mode,
                        &f.subcatch_evap_mode,&f.subcatch_snowfall_mode,&f.gage_rainfall_mode})
        for(auto mode:*row)require(mode==ForcingMode::NONE,"Engine checkpoint cannot recover runtime forcing.");
    require(f.climate_temperature_mode==ForcingMode::NONE && f.climate_wind_mode==ForcingMode::NONE && f.climate_evap_mode==ForcingMode::NONE,
            "Engine checkpoint cannot recover runtime climate forcing.");
    require(ctx_.n_subcatches()==0 || (ctx_.options.ignore_snow_melt && ctx_.options.ignore_groundwater),
            "Runoff session checkpoint currently requires IGNORE_SNOWMELT and IGNORE_GROUNDWATER.");
    for(int pack:ctx_.subcatches.snowpack)require(pack<0,"Engine checkpoint does not recover assigned snowpacks.");
    for(int aquifer:ctx_.subcatches.gw_aquifer)require(aquifer<0,"Engine checkpoint does not recover assigned groundwater.");
    for(const auto& path:ctx_.lid_usage.rpt_file)require(path.empty(),"Engine checkpoint cannot append detailed LID reports.");
    require(gage_states_.size()==std::size_t(ctx_.n_gages()),"Engine rainfall state shape differs.");
    for(std::size_t i=0;i<ctx_.gages.source.size();++i)require(ctx_.gages.source[i]==RainSource::TIMESERIES &&
            ctx_.gages.ts_index[i]>=0 && ctx_.gages.ts_index[i]<ctx_.n_tables() && ctx_.gages.file_path[i].empty() &&
            ctx_.gages.rain_series[i].empty() && ctx_.gages.api_rainfall[i]==-1,
            "Engine checkpoint supports inline rainfall without API overrides.");
    for(const auto* row:{&gw_frac_perv_,&gw_perv_evap_,&gw_2d_recharge_vol_,&gw_infil_with_2d_,&gw_infil_rate_,&snow_rain_,&snow_snow_})
        require(row->empty(),"Engine checkpoint has unqualified hydrology interpolation state.");
    require(lid_drain_q_interp_.empty() || lid_drain_q_interp_.size()==std::size_t(ctx_.n_nodes()),"Engine checkpoint LID-drain shape differs.");
    require(router_.structures_==&hydstruct_,"Engine checkpoint structure solver ownership differs.");
    if(ctx_.routing_stats.sum_iterations>0)for(const auto& lid:ctx_.node_subtypes.storages.lid_state)
        require(lid.quality_old_water.size()==lid.cells.size(),"Engine routed LID previous-water shape differs.");
    router_.checkCheckpointProfile(ctx_);
}
std::string SWMMEngine::checkpointModel() const {
    Archive a;auto o=ctx_.options;
#define F(v) field(a,o.v);
    OPTIONS(F)
#undef F
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
        a.sequence(r.premises,[&](auto& p){a(p.logic,p.is_expression,p.expr_idx,p.lhs_var,p.lhs_idx,p.lhs_param,p.op,p.rhs_is_variable,p.rhs_var,p.rhs_idx,p.rhs_value);});
        for(auto* list:{&r.then_actions,&r.else_actions})a.sequence(*list,[&](auto& v){
            a(v.link_idx,v.rule_idx,v.type,v.curve_idx,v.tseries_idx,v.pid_idx,v.value);
        });
    });
    a.sequence(named,[&](auto& n){a(n.name,n.var,n.idx);});
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
    for(int i=0;i<ctx_.n_gages();++i){auto used=gage::gageIsUsed(ctx_,i);a(used);}
    a(lc.names,lc.lid_type,lc.surface,lc.soil,lc.pavement,lc.storage,lc.drain,lc.drainmat,lc.removals,
      lu.subcatch_index,lu.lid_index,lu.number,lu.area,lu.width,lu.init_sat,lu.from_imperv,lu.to_perv,lu.drain_to,lu.from_perv);
    auto runoff=runoff_;auto lid=lid_.waterTrial();runoff.checkpoint(a,true);lid.checkpoint(a,true);
    auto hydraulic=router_.checkpointModel(ctx_);a(hydraulic);
    require(a.bytes().size()<=maxBytes,"Engine checkpoint model exceeds its size limit.");return a.bytes();
}
void SWMMEngine::checkCheckpointBoundary() const {
    require(checkpoint_enabled_ && checkpoint_ready_,"Engine checkpoint requires a completed accepted session boundary.");
    checkCheckpointProfile();require(checkpoint_model_==checkpointModel(),"Engine checkpoint model changed after attachment.");
    RestartState state(*this);Archive out;state.archive(out);state.validate(*this);
#ifdef OPENSWMM_HAS_2D
    if(surface_router_.isActive()) {auto error=surface_router_.sourceBoundary(ctx_);require(error.empty(),error.c_str());}
#endif
}
std::string SWMMEngine::enableCheckpointing() {
    return enableCheckpointingImpl(
#ifdef OPENSWMM_HAS_2D
        nullptr
#endif
    );
}
#ifdef OPENSWMM_HAS_2D
std::string SWMMEngine::enableCheckpointing(twoD::CommonSourceStage& stage) {return enableCheckpointingImpl(&stage);}
#endif
std::string SWMMEngine::enableCheckpointingImpl(
#ifdef OPENSWMM_HAS_2D
    twoD::CommonSourceStage* stage
#endif
) {
    const bool owned=router_.checkpoint_owned_session_;
    const bool fresh_router=router_.checkpoint_context_==nullptr;
    try {
        require(!step_active_ && !runtime_pipeline_ && exchange_target_<0 && !checkpoint_enabled_ && ctx_.current_time==0,
                "Enable engine checkpointing at start(0)'s initial session boundary.");
        router_.checkpoint_owned_session_=true;
#ifdef OPENSWMM_HAS_2D
        require(!stage || stage->meshOnly(),"Engine source checkpoint does not yet own private subcatchment donors/deliveries.");
        checkCheckpointProfile(stage!=nullptr);
#else
        checkCheckpointProfile();
#endif
        auto model=checkpointModel();RestartState state(*this);Archive out;state.archive(out);state.validate(*this);
        router_.enableCheckpointing(ctx_);
#ifdef OPENSWMM_HAS_2D
        if(stage)surface_router_.setJointSourceStage(ctx_,*stage,router_);
#endif
        checkpoint_model_=std::move(model);checkpoint_enabled_=checkpoint_ready_=true;return {};
    } catch(const std::exception& e) {
        router_.checkpoint_owned_session_=owned;
        if(fresh_router) {
            router_.checkpoint_context_=nullptr;router_.checkpoint_model_.clear();
            router_.checkpoint_time_=router_.checkpoint_step_end_=0;
            router_.checkpoint_ready_=router_.checkpoint_prepared_=router_.checkpoint_step_finished_=false;
        }
        return e.what();
    }
}
std::string SWMMEngine::saveCheckpoint(const std::string& path) const {
    try {
        require(!step_active_ && !runtime_pipeline_ && exchange_target_<0,"Cannot save an active engine pipeline.");checkCheckpointBoundary();
        RestartState state(*this);Archive payload;state.archive(payload);std::string hydraulic;
        uint32_t version=3;std::string error;
#ifdef OPENSWMM_HAS_2D
        if(surface_router_.isActive()) {version=4;error=surface_router_.checkpointBytes(ctx_,hydraulic);}
        else
#endif
        error=router_.checkpointBytes(ctx_,hydraulic);
        require(error.empty(),error.c_str());payload(hydraulic);
        auto bytes=payload.bytes(),model=checkpoint_model_;auto crc=checksum(bytes);std::string magic=magicName;
        Archive out;out(magic,version,model,crc,bytes);require(out.bytes().size()<=maxBytes,"Engine checkpoint exceeds its size limit.");
        publish(path,out.bytes());return {};
    } catch(const std::exception& e) {return e.what();}
}
std::string SWMMEngine::loadCheckpoint(const std::string& path) {
    try {
        require(!step_active_ && !runtime_pipeline_ && exchange_target_<0 && ctx_.current_time==0,"Engine restore requires an identically initialized fresh session.");
        checkCheckpointBoundary();Archive in(readFile(path));std::string magic,model,bytes;uint32_t version=0,crc=0;
        in(magic,version,model,crc,bytes);in.finish();
        uint32_t expected_version=3;
#ifdef OPENSWMM_HAS_2D
        if(surface_router_.isActive())expected_version=4;
#endif
        require(magic==magicName && version==expected_version && model==checkpoint_model_ && crc==checksum(bytes),"Engine checkpoint format/model/checksum differs.");
        RestartState state(*this);Archive payload(std::move(bytes));state.archive(payload);std::string hydraulic;payload(hydraulic);payload.finish();state.validate(*this);
#ifdef OPENSWMM_HAS_2D
        if(surface_router_.isActive()) {
            auto error=surface_router_.loadCheckpointBytes(ctx_,std::move(hydraulic),&state.boundary,&state.gages.rainfall,
                &state.balance.routing_final_storage,state.stats.sum_iterations>0);
            require(error.empty(),error.c_str());state.install(*this);return {};
        }
#endif
        auto routing=router_.prepareCheckpointBytes(ctx_,std::move(hydraulic),&state.boundary);
        if(state.boundary[0]>0)Router::validateSessionStorage(routing,ctx_,state.balance.routing_final_storage,state.stats.sum_iterations>0);
        // All throwing parse/model/shape/ledger checks precede installation.
        router_.installCheckpoint(ctx_,std::move(routing));state.install(*this);return {};
    } catch(const std::exception& e) {return e.what();}
}
} // namespace openswmm
