# :author: Caleb Buahin
# :copyright: Copyright (c) HydroCouple 2026
# :license: MIT
#
# _common.pxd — Shared C declarations for the OpenSWMM Engine C API.
#
# All domain modules (``_solver``, ``_nodes``, ``_links``, etc.) cimport from
# this file to get the opaque handle types and shared function signatures.
#
# cython: language_level=3

cdef extern from "openswmm_engine.h":

    # --- Opaque handles ---
    ctypedef void* SWMM_Engine
    ctypedef void* SWMM_HotStart

    # --- Callback typedefs ---
    ctypedef void (*SWMM_ProgressCallback)(double progress, void* user_data)
    ctypedef void (*SWMM_WarningCallback)(int code, const char* msg, void* user_data)

    # --- Error reporting ---
    cdef int         swmm_get_last_error(SWMM_Engine e)
    cdef const char* swmm_get_last_error_msg(SWMM_Engine e)
    cdef const char* swmm_error_message(int code)

    # --- Engine lifecycle ---
    cdef SWMM_Engine swmm_engine_create()
    cdef int  swmm_engine_open(SWMM_Engine e, const char* inp, const char* rpt, const char* out)
    cdef int  swmm_engine_initialize(SWMM_Engine e)
    cdef int  swmm_engine_start(SWMM_Engine e, int save_results)
    cdef int  swmm_engine_step(SWMM_Engine e, double* elapsed_time)
    cdef int  swmm_engine_end(SWMM_Engine e)
    cdef int  swmm_engine_report(SWMM_Engine e)
    cdef int  swmm_engine_close(SWMM_Engine e)
    cdef void swmm_engine_destroy(SWMM_Engine e)
    cdef int  swmm_engine_get_state(SWMM_Engine e, int* state)

    # --- Timing ---
    cdef int swmm_get_start_time(SWMM_Engine e, double* start)
    cdef int swmm_get_end_time(SWMM_Engine e, double* end)
    cdef int swmm_get_current_time(SWMM_Engine e, double* current)
    cdef int swmm_get_routing_step(SWMM_Engine e, double* dt)

    # --- Callbacks ---
    cdef int swmm_set_progress_callback(SWMM_Engine e, SWMM_ProgressCallback cb, void* ud)
    cdef int swmm_set_warning_callback(SWMM_Engine e, SWMM_WarningCallback cb, void* ud)

cdef extern from "openswmm_model.h":
    cdef SWMM_Engine swmm_engine_new()
    cdef int swmm_validate_model(SWMM_Engine e)
    cdef int swmm_finalize_model(SWMM_Engine e)
    cdef int swmm_model_write(SWMM_Engine e, const char* path)
    cdef int swmm_options_get(SWMM_Engine e, const char* key, char* buf, int buflen)
    cdef int swmm_options_set(SWMM_Engine e, const char* key, const char* value)
    cdef int swmm_options_get_ext(SWMM_Engine e, const char* key, char* buf, int buflen)
    cdef int swmm_options_set_ext(SWMM_Engine e, const char* key, const char* value)
    cdef int swmm_get_crs(SWMM_Engine e, char* buf, int buflen)
    # User flags
    cdef int swmm_userflag_get_bool(SWMM_Engine e, const char* name, int* value)
    cdef int swmm_userflag_get_int(SWMM_Engine e, const char* name, int* value)
    cdef int swmm_userflag_get_real(SWMM_Engine e, const char* name, double* value)
    cdef int swmm_userflag_set_bool(SWMM_Engine e, const char* name, int value)
    cdef int swmm_userflag_set_int(SWMM_Engine e, const char* name, int value)
    cdef int swmm_userflag_set_real(SWMM_Engine e, const char* name, double value)

cdef extern from "openswmm_nodes.h":
    # Identity
    cdef int         swmm_node_count(SWMM_Engine e)
    cdef int         swmm_node_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_node_id(SWMM_Engine e, int idx)
    # Creation
    cdef int swmm_node_add(SWMM_Engine e, const char* id, int type)
    # Geometry setters
    cdef int swmm_node_set_invert_elev(SWMM_Engine e, int idx, double elev)
    cdef int swmm_node_set_max_depth(SWMM_Engine e, int idx, double depth)
    cdef int swmm_node_set_surcharge_depth(SWMM_Engine e, int idx, double depth)
    cdef int swmm_node_set_pond_area(SWMM_Engine e, int idx, double area)
    cdef int swmm_node_set_initial_depth(SWMM_Engine e, int idx, double depth)
    # Geometry getters
    cdef int swmm_node_get_type(SWMM_Engine e, int idx, int* type)
    cdef int swmm_node_get_invert_elev(SWMM_Engine e, int idx, double* elev)
    cdef int swmm_node_get_max_depth(SWMM_Engine e, int idx, double* depth)
    cdef int swmm_node_get_surcharge_depth(SWMM_Engine e, int idx, double* depth)
    cdef int swmm_node_get_ponded_area(SWMM_Engine e, int idx, double* area)
    cdef int swmm_node_get_initial_depth(SWMM_Engine e, int idx, double* depth)
    cdef int swmm_node_get_crown_elev(SWMM_Engine e, int idx, double* elev)
    cdef int swmm_node_get_full_volume(SWMM_Engine e, int idx, double* vol)
    cdef int swmm_node_get_losses(SWMM_Engine e, int idx, double* losses)
    cdef int swmm_node_get_outflow(SWMM_Engine e, int idx, double* outflow)
    cdef int swmm_node_get_degree(SWMM_Engine e, int idx, int* degree)
    # Hydraulic state
    cdef int swmm_node_get_depth(SWMM_Engine e, int idx, double* depth)
    cdef int swmm_node_set_depth(SWMM_Engine e, int idx, double depth)
    cdef int swmm_node_get_head(SWMM_Engine e, int idx, double* head)
    cdef int swmm_node_get_volume(SWMM_Engine e, int idx, double* volume)
    cdef int swmm_node_get_lateral_inflow(SWMM_Engine e, int idx, double* inflow)
    cdef int swmm_node_get_overflow(SWMM_Engine e, int idx, double* overflow)
    cdef int swmm_node_get_inflow(SWMM_Engine e, int idx, double* inflow)
    # Runtime forcing
    cdef int swmm_node_set_lateral_inflow(SWMM_Engine e, int idx, double flow)
    cdef int swmm_node_set_head_boundary(SWMM_Engine e, int idx, double head)
    # Quality
    cdef int swmm_node_get_quality(SWMM_Engine e, int node_idx, int pollutant_idx, double* conc)
    # Storage
    cdef int swmm_node_set_storage_curve(SWMM_Engine e, int idx, int curve_idx)
    cdef int swmm_node_get_storage_curve(SWMM_Engine e, int idx, int* curve_idx)
    cdef int swmm_node_set_storage_functional(SWMM_Engine e, int idx, double a, double b, double c)
    cdef int swmm_node_get_storage_functional(SWMM_Engine e, int idx, double* a, double* b, double* c)
    cdef int swmm_node_set_storage_seep_rate(SWMM_Engine e, int idx, double rate)
    cdef int swmm_node_get_storage_seep_rate(SWMM_Engine e, int idx, double* rate)
    cdef int swmm_node_set_exfil_params(SWMM_Engine e, int idx, double suction, double ksat, double imd)
    cdef int swmm_node_get_exfil_params(SWMM_Engine e, int idx, double* suction, double* ksat, double* imd)
    # Outfall
    cdef int swmm_node_set_outfall_type(SWMM_Engine e, int idx, int type)
    cdef int swmm_node_get_outfall_type(SWMM_Engine e, int idx, int* type)
    cdef int swmm_node_set_outfall_stage(SWMM_Engine e, int idx, double stage)
    cdef int swmm_node_set_outfall_tidal(SWMM_Engine e, int idx, int curve_idx)
    cdef int swmm_node_set_outfall_timeseries(SWMM_Engine e, int idx, int ts_idx)
    cdef int swmm_node_get_outfall_param(SWMM_Engine e, int idx, double* param)
    cdef int swmm_node_set_outfall_flap_gate(SWMM_Engine e, int idx, int has_gate)
    cdef int swmm_node_get_outfall_flap_gate(SWMM_Engine e, int idx, int* has_gate)
    # Statistics
    cdef int swmm_node_get_stat_max_depth(SWMM_Engine e, int idx, double* val)
    cdef int swmm_node_get_stat_max_overflow(SWMM_Engine e, int idx, double* val)
    cdef int swmm_node_get_stat_vol_flooded(SWMM_Engine e, int idx, double* val)
    cdef int swmm_node_get_stat_time_flooded(SWMM_Engine e, int idx, double* val)
    # Bulk access
    cdef int swmm_node_get_depths_bulk(SWMM_Engine e, double* buf, int count)
    cdef int swmm_node_get_heads_bulk(SWMM_Engine e, double* buf, int count)
    cdef int swmm_node_get_inflows_bulk(SWMM_Engine e, double* buf, int count)
    cdef int swmm_node_get_overflows_bulk(SWMM_Engine e, double* buf, int count)
    cdef int swmm_node_set_depths_bulk(SWMM_Engine e, const double* buf, int count)
    cdef int swmm_node_set_lat_inflows_bulk(SWMM_Engine e, const double* buf, int count)
    cdef int swmm_node_get_quality_bulk(SWMM_Engine e, int pollutant_idx, double* buf, int count)

cdef extern from "openswmm_links.h":
    # Identity
    cdef int         swmm_link_count(SWMM_Engine e)
    cdef int         swmm_link_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_link_id(SWMM_Engine e, int idx)
    # Creation
    cdef int swmm_link_add(SWMM_Engine e, const char* id, int type)
    # Connectivity
    cdef int swmm_link_set_nodes(SWMM_Engine e, int idx, int from_node, int to_node)
    cdef int swmm_link_get_from_node(SWMM_Engine e, int idx, int* node_idx)
    cdef int swmm_link_get_to_node(SWMM_Engine e, int idx, int* node_idx)
    # Geometry setters
    cdef int swmm_link_set_length(SWMM_Engine e, int idx, double length)
    cdef int swmm_link_set_roughness(SWMM_Engine e, int idx, double n)
    cdef int swmm_link_set_offset_up(SWMM_Engine e, int idx, double offset)
    cdef int swmm_link_set_offset_dn(SWMM_Engine e, int idx, double offset)
    cdef int swmm_link_set_initial_flow(SWMM_Engine e, int idx, double flow)
    cdef int swmm_link_set_max_flow(SWMM_Engine e, int idx, double flow)
    # Cross-section
    cdef int swmm_link_set_xsect(SWMM_Engine e, int idx, int shape,
                                  double g1, double g2, double g3, double g4)
    cdef int swmm_link_get_xsect(SWMM_Engine e, int idx,
                                  int* shape, double* g1, double* g2,
                                  double* g3, double* g4)
    # Geometry getters
    cdef int swmm_link_get_type(SWMM_Engine e, int idx, int* type)
    cdef int swmm_link_get_length(SWMM_Engine e, int idx, double* length)
    cdef int swmm_link_get_roughness(SWMM_Engine e, int idx, double* n)
    cdef int swmm_link_get_slope(SWMM_Engine e, int idx, double* slope)
    cdef int swmm_link_get_offset_up(SWMM_Engine e, int idx, double* offset)
    cdef int swmm_link_get_offset_dn(SWMM_Engine e, int idx, double* offset)
    # Hydraulic state
    cdef int swmm_link_get_flow(SWMM_Engine e, int idx, double* flow)
    cdef int swmm_link_set_flow(SWMM_Engine e, int idx, double flow)
    cdef int swmm_link_get_depth(SWMM_Engine e, int idx, double* depth)
    cdef int swmm_link_get_velocity(SWMM_Engine e, int idx, double* velocity)
    cdef int swmm_link_get_capacity(SWMM_Engine e, int idx, double* capacity)
    cdef int swmm_link_get_volume(SWMM_Engine e, int idx, double* volume)
    # Runtime forcing
    cdef int swmm_link_set_control_setting(SWMM_Engine e, int idx, double setting)
    cdef int swmm_link_get_control_setting(SWMM_Engine e, int idx, double* setting)
    cdef int swmm_link_set_target_setting(SWMM_Engine e, int idx, double setting)
    cdef int swmm_link_get_target_setting(SWMM_Engine e, int idx, double* setting)
    cdef int swmm_link_set_closed(SWMM_Engine e, int idx, int closed)
    cdef int swmm_link_get_closed(SWMM_Engine e, int idx, int* closed)
    # Pump
    cdef int swmm_link_set_pump_curve(SWMM_Engine e, int idx, int curve_idx)
    cdef int swmm_link_get_pump_curve(SWMM_Engine e, int idx, int* curve_idx)
    cdef int swmm_link_set_pump_init_state(SWMM_Engine e, int idx, int on)
    cdef int swmm_link_get_pump_init_state(SWMM_Engine e, int idx, int* on)
    # Weir
    cdef int swmm_link_set_crest_height(SWMM_Engine e, int idx, double h)
    cdef int swmm_link_get_crest_height(SWMM_Engine e, int idx, double* h)
    cdef int swmm_link_set_discharge_coeff(SWMM_Engine e, int idx, double cd)
    cdef int swmm_link_get_discharge_coeff(SWMM_Engine e, int idx, double* cd)
    cdef int swmm_link_set_end_contractions(SWMM_Engine e, int idx, double n)
    cdef int swmm_link_get_end_contractions(SWMM_Engine e, int idx, double* n)
    # Loss coefficients
    cdef int swmm_link_set_loss_coeff(SWMM_Engine e, int idx, double inlet, double outlet, double avg)
    cdef int swmm_link_get_loss_coeff(SWMM_Engine e, int idx, double* inlet, double* outlet, double* avg)
    cdef int swmm_link_set_flap_gate(SWMM_Engine e, int idx, int has_gate)
    cdef int swmm_link_get_flap_gate(SWMM_Engine e, int idx, int* has_gate)
    cdef int swmm_link_set_seep_rate(SWMM_Engine e, int idx, double rate)
    cdef int swmm_link_get_seep_rate(SWMM_Engine e, int idx, double* rate)
    cdef int swmm_link_set_culvert_code(SWMM_Engine e, int idx, int code)
    cdef int swmm_link_get_culvert_code(SWMM_Engine e, int idx, int* code)
    cdef int swmm_link_set_barrels(SWMM_Engine e, int idx, int n)
    cdef int swmm_link_get_barrels(SWMM_Engine e, int idx, int* n)
    # Quality
    cdef int swmm_link_get_quality(SWMM_Engine e, int link_idx, int pollutant_idx, double* conc)
    # Statistics
    cdef int swmm_link_get_stat_max_flow(SWMM_Engine e, int idx, double* val)
    cdef int swmm_link_get_stat_max_velocity(SWMM_Engine e, int idx, double* val)
    cdef int swmm_link_get_stat_max_filling(SWMM_Engine e, int idx, double* val)
    cdef int swmm_link_get_stat_vol_flow(SWMM_Engine e, int idx, double* val)
    cdef int swmm_link_get_stat_surcharge_time(SWMM_Engine e, int idx, double* val)
    # Bulk access
    cdef int swmm_link_get_flows_bulk(SWMM_Engine e, double* buf, int count)
    cdef int swmm_link_get_depths_bulk(SWMM_Engine e, double* buf, int count)
    cdef int swmm_link_set_flows_bulk(SWMM_Engine e, const double* buf, int count)
    cdef int swmm_link_get_quality_bulk(SWMM_Engine e, int pollutant_idx, double* buf, int count)

cdef extern from "openswmm_subcatchments.h":
    # Identity
    cdef int         swmm_subcatch_count(SWMM_Engine e)
    cdef int         swmm_subcatch_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_subcatch_id(SWMM_Engine e, int idx)
    # Creation
    cdef int swmm_subcatch_add(SWMM_Engine e, const char* id)
    # Property setters
    cdef int swmm_subcatch_set_outlet(SWMM_Engine e, int idx, int node_idx)
    cdef int swmm_subcatch_set_area(SWMM_Engine e, int idx, double area)
    cdef int swmm_subcatch_set_width(SWMM_Engine e, int idx, double width)
    cdef int swmm_subcatch_set_slope(SWMM_Engine e, int idx, double slope)
    cdef int swmm_subcatch_set_imperv_pct(SWMM_Engine e, int idx, double pct)
    cdef int swmm_subcatch_set_n_imperv(SWMM_Engine e, int idx, double n)
    cdef int swmm_subcatch_set_n_perv(SWMM_Engine e, int idx, double n)
    cdef int swmm_subcatch_set_ds_imperv(SWMM_Engine e, int idx, double ds)
    cdef int swmm_subcatch_set_ds_perv(SWMM_Engine e, int idx, double ds)
    cdef int swmm_subcatch_set_gage(SWMM_Engine e, int idx, int gage_idx)
    cdef int swmm_subcatch_set_outlet_subcatch(SWMM_Engine e, int idx, int sc_idx)
    # Infiltration setters
    cdef int swmm_subcatch_set_infil_horton(SWMM_Engine e, int idx,
                                             double f0, double fmin,
                                             double decay, double dry_time)
    cdef int swmm_subcatch_set_infil_green_ampt(SWMM_Engine e, int idx,
                                                 double suction, double conductivity,
                                                 double initial_deficit)
    cdef int swmm_subcatch_set_infil_curve_number(SWMM_Engine e, int idx, double cn)
    # Property getters
    cdef int swmm_subcatch_get_area(SWMM_Engine e, int idx, double* area)
    cdef int swmm_subcatch_get_imperv_pct(SWMM_Engine e, int idx, double* pct)
    cdef int swmm_subcatch_get_outlet(SWMM_Engine e, int idx, int* node_idx)
    cdef int swmm_subcatch_get_width(SWMM_Engine e, int idx, double* w)
    cdef int swmm_subcatch_get_slope(SWMM_Engine e, int idx, double* s)
    cdef int swmm_subcatch_get_n_imperv(SWMM_Engine e, int idx, double* n)
    cdef int swmm_subcatch_get_n_perv(SWMM_Engine e, int idx, double* n)
    cdef int swmm_subcatch_get_ds_imperv(SWMM_Engine e, int idx, double* ds)
    cdef int swmm_subcatch_get_ds_perv(SWMM_Engine e, int idx, double* ds)
    cdef int swmm_subcatch_get_gage(SWMM_Engine e, int idx, int* gage_idx)
    cdef int swmm_subcatch_get_outlet_subcatch(SWMM_Engine e, int idx, int* sc_idx)
    # Infiltration getters
    cdef int swmm_subcatch_get_infil_model(SWMM_Engine e, int idx, int* model)
    cdef int swmm_subcatch_get_infil_horton(SWMM_Engine e, int idx,
                                             double* f0, double* fmin,
                                             double* decay, double* dry_time)
    cdef int swmm_subcatch_get_infil_green_ampt(SWMM_Engine e, int idx,
                                                 double* suction, double* conductivity,
                                                 double* deficit)
    cdef int swmm_subcatch_get_infil_curve_number(SWMM_Engine e, int idx, double* cn)
    # Statistics
    cdef int swmm_subcatch_get_stat_precip(SWMM_Engine e, int idx, double* vol)
    cdef int swmm_subcatch_get_stat_runoff_vol(SWMM_Engine e, int idx, double* vol)
    cdef int swmm_subcatch_get_stat_max_runoff(SWMM_Engine e, int idx, double* rate)
    # Coverage
    cdef int swmm_subcatch_set_coverage(SWMM_Engine e, int sc_idx, int lu_idx, double fraction)
    cdef int swmm_subcatch_get_coverage(SWMM_Engine e, int sc_idx, int lu_idx, double* fraction)
    # Hydraulic state
    cdef int swmm_subcatch_get_runoff(SWMM_Engine e, int idx, double* runoff)
    cdef int swmm_subcatch_get_groundwater(SWMM_Engine e, int idx, double* gw)
    cdef int swmm_subcatch_get_rainfall(SWMM_Engine e, int idx, double* rainfall)
    cdef int swmm_subcatch_get_snow_depth(SWMM_Engine e, int idx, double* depth)
    cdef int swmm_subcatch_get_evap(SWMM_Engine e, int idx, double* evap)
    cdef int swmm_subcatch_get_infil(SWMM_Engine e, int idx, double* infil)
    # Runtime forcing
    cdef int swmm_subcatch_set_rainfall(SWMM_Engine e, int idx, double rainfall)
    # Quality
    cdef int swmm_subcatch_get_quality(SWMM_Engine e, int subcatch_idx, int pollutant_idx, double* conc)
    # Bulk access
    cdef int swmm_subcatch_get_runoff_bulk(SWMM_Engine e, double* buf, int count)
    cdef int swmm_subcatch_get_quality_bulk(SWMM_Engine e, int pollutant_idx, double* buf, int count)

cdef extern from "openswmm_gages.h":
    # Identity
    cdef int         swmm_gage_count(SWMM_Engine e)
    cdef int         swmm_gage_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_gage_id(SWMM_Engine e, int idx)
    # Creation
    cdef int swmm_gage_add(SWMM_Engine e, const char* id)
    # Property setters
    cdef int swmm_gage_set_rain_type(SWMM_Engine e, int idx, int type)
    cdef int swmm_gage_set_rain_interval(SWMM_Engine e, int idx, double seconds)
    cdef int swmm_gage_set_data_source(SWMM_Engine e, int idx, int source)
    cdef int swmm_gage_set_timeseries(SWMM_Engine e, int idx, const char* ts_id)
    cdef int swmm_gage_set_filename(SWMM_Engine e, int idx, const char* path, const char* station_id)
    # Property getters
    cdef int swmm_gage_get_rain_type(SWMM_Engine e, int idx, int* type)
    cdef int swmm_gage_get_data_source(SWMM_Engine e, int idx, int* source)
    # State
    cdef int swmm_gage_get_rainfall(SWMM_Engine e, int idx, double* rainfall)
    cdef int swmm_gage_set_rainfall(SWMM_Engine e, int idx, double rainfall)
    # Bulk
    cdef int swmm_gage_get_rainfall_bulk(SWMM_Engine e, double* buf, int count)

cdef extern from "openswmm_massbalance.h":
    cdef int swmm_get_runoff_continuity_error(SWMM_Engine e, double* error)
    cdef int swmm_get_routing_continuity_error(SWMM_Engine e, double* error)
    cdef int swmm_get_quality_continuity_error(SWMM_Engine e, int pollutant_idx, double* error)
    cdef int swmm_get_runoff_total(SWMM_Engine e, int component, double* volume)
    cdef int swmm_get_routing_total(SWMM_Engine e, int component, double* volume)

cdef extern from "openswmm_hotstart.h":
    cdef int swmm_hotstart_save(SWMM_Engine e, const char* path)
    cdef int swmm_hotstart_open(const char* path, SWMM_HotStart* hs)
    cdef int swmm_hotstart_apply(SWMM_Engine e, SWMM_HotStart hs)
    cdef int swmm_hotstart_close(SWMM_HotStart hs)
    # Modify
    cdef int swmm_hotstart_set_node_depth(SWMM_HotStart hs, const char* node_id, double depth)
    cdef int swmm_hotstart_set_node_head(SWMM_HotStart hs, const char* node_id, double head)
    cdef int swmm_hotstart_set_link_flow(SWMM_HotStart hs, const char* link_id, double flow)
    cdef int swmm_hotstart_set_link_depth(SWMM_HotStart hs, const char* link_id, double depth)
    cdef int swmm_hotstart_set_subcatch_runoff(SWMM_HotStart hs, const char* subcatch_id, double runoff)
    # Metadata
    cdef int swmm_hotstart_get_sim_time(SWMM_HotStart hs, double* sim_time)
    cdef int swmm_hotstart_get_crs(SWMM_HotStart hs, char* buf, int buflen)
    cdef int swmm_hotstart_node_count(SWMM_HotStart hs)
    cdef int swmm_hotstart_link_count(SWMM_HotStart hs)
    # Warnings
    cdef int swmm_hotstart_warning_count(SWMM_HotStart hs)
    cdef const char* swmm_hotstart_warning(SWMM_HotStart hs, int index)

cdef extern from "openswmm_pollutants.h":
    # Identity
    cdef int         swmm_pollutant_count(SWMM_Engine e)
    cdef int         swmm_pollutant_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_pollutant_id(SWMM_Engine e, int idx)
    # Creation
    cdef int swmm_pollutant_add(SWMM_Engine e, const char* id, int units)
    # Property setters
    cdef int swmm_pollutant_set_kdecay(SWMM_Engine e, int idx, double k)
    cdef int swmm_pollutant_set_rain_conc(SWMM_Engine e, int idx, double conc)
    cdef int swmm_pollutant_set_gw_conc(SWMM_Engine e, int idx, double conc)
    cdef int swmm_pollutant_set_init_conc(SWMM_Engine e, int idx, double conc)
    cdef int swmm_pollutant_set_rdii_conc(SWMM_Engine e, int idx, double conc)
    cdef int swmm_pollutant_set_mwt(SWMM_Engine e, int idx, double mwt)
    cdef int swmm_pollutant_set_co_pollutant(SWMM_Engine e, int idx, int co_idx, double frac)
    cdef int swmm_pollutant_set_snow_only(SWMM_Engine e, int idx, int flag)
    # Property getters
    cdef int swmm_pollutant_get_units(SWMM_Engine e, int idx, int* units)
    cdef int swmm_pollutant_get_kdecay(SWMM_Engine e, int idx, double* k)
    cdef int swmm_pollutant_get_rain_conc(SWMM_Engine e, int idx, double* conc)
    cdef int swmm_pollutant_get_gw_conc(SWMM_Engine e, int idx, double* conc)
    cdef int swmm_pollutant_get_init_conc(SWMM_Engine e, int idx, double* conc)
    cdef int swmm_pollutant_get_rdii_conc(SWMM_Engine e, int idx, double* conc)
    cdef int swmm_pollutant_get_mwt(SWMM_Engine e, int idx, double* mwt)
    cdef int swmm_pollutant_get_co_pollutant(SWMM_Engine e, int idx, int* co_idx, double* frac)
    cdef int swmm_pollutant_get_snow_only(SWMM_Engine e, int idx, int* flag)
    # Runtime quality injection
    cdef int swmm_node_set_quality(SWMM_Engine e, int node_idx, int pollut_idx, double conc)
    cdef int swmm_link_set_quality(SWMM_Engine e, int link_idx, int pollut_idx, double conc)

cdef extern from "openswmm_tables.h":
    # Identity
    cdef int         swmm_table_count(SWMM_Engine e)
    cdef int         swmm_table_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_table_id(SWMM_Engine e, int idx)
    # Creation
    cdef int swmm_timeseries_add(SWMM_Engine e, const char* id)
    cdef int swmm_curve_add(SWMM_Engine e, const char* id, int type)
    # Data points
    cdef int swmm_table_add_point(SWMM_Engine e, int idx, double x, double y)
    cdef int swmm_table_get_point_count(SWMM_Engine e, int idx, int* count)
    cdef int swmm_table_get_point(SWMM_Engine e, int idx, int pt_idx, double* x, double* y)
    cdef int swmm_table_clear(SWMM_Engine e, int idx)
    # Lookup
    cdef int swmm_table_lookup(SWMM_Engine e, int idx, double x, double* y)
    # Patterns
    cdef int swmm_pattern_add(SWMM_Engine e, const char* id, int type)
    cdef int swmm_pattern_set_factors(SWMM_Engine e, int idx, const double* factors, int count)
    cdef int swmm_pattern_count(SWMM_Engine e)

cdef extern from "openswmm_inflows.h":
    cdef int swmm_ext_inflow_add(SWMM_Engine e, int node_idx, const char* constituent,
                                  const char* ts_name, const char* type,
                                  double m_factor, double s_factor, double baseline,
                                  const char* pattern)
    cdef int swmm_dwf_add(SWMM_Engine e, int node_idx, const char* constituent,
                           double avg_value, const char* pat1, const char* pat2,
                           const char* pat3, const char* pat4)
    cdef int swmm_rdii_add(SWMM_Engine e, int node_idx, const char* uh_name, double area)
    cdef int swmm_ext_inflow_count(SWMM_Engine e)
    cdef int swmm_dwf_count(SWMM_Engine e)
    cdef int swmm_rdii_count(SWMM_Engine e)

cdef extern from "openswmm_controls.h":
    cdef int swmm_control_add_rule(SWMM_Engine e, const char* rule_text)
    cdef int swmm_control_count(SWMM_Engine e)
    cdef int swmm_control_get_rule(SWMM_Engine e, int idx, char* buf, int buflen)
    cdef int swmm_control_clear_rules(SWMM_Engine e)
    cdef int swmm_control_set_link_setting(SWMM_Engine e, int link_idx, double setting)
    cdef int swmm_control_set_link_status(SWMM_Engine e, int link_idx, int status)

cdef extern from "openswmm_infrastructure.h":
    # Transects
    cdef int swmm_transect_add(SWMM_Engine e, const char* id)
    cdef int swmm_transect_set_roughness(SWMM_Engine e, int idx, double n_left, double n_right, double n_channel)
    cdef int swmm_transect_add_station(SWMM_Engine e, int idx, double station, double elevation)
    cdef int swmm_transect_count(SWMM_Engine e)
    # Streets
    cdef int swmm_street_add(SWMM_Engine e, const char* id)
    cdef int swmm_street_set_params(SWMM_Engine e, int idx,
                                     double t_crown, double h_curb, double sx, double n_road,
                                     double gutter_depres, double gutter_width, int sides,
                                     double back_width, double back_slope, double back_n)
    cdef int swmm_street_count(SWMM_Engine e)
    # Inlets
    cdef int swmm_inlet_add(SWMM_Engine e, const char* id, const char* type)
    cdef int swmm_inlet_set_params(SWMM_Engine e, int idx, double length, double width,
                                    const char* grate_type, double open_area, double splash_veloc)
    cdef int swmm_inlet_count(SWMM_Engine e)
    # LID controls
    cdef int swmm_lid_add(SWMM_Engine e, const char* id, int type)
    cdef int swmm_lid_set_surface(SWMM_Engine e, int idx, double storage, double roughness, double slope)
    cdef int swmm_lid_set_soil(SWMM_Engine e, int idx, double thick, double porosity, double fc, double wp, double ksat, double kslope)
    cdef int swmm_lid_set_storage(SWMM_Engine e, int idx, double thick, double void_frac, double ksat)
    cdef int swmm_lid_set_drain(SWMM_Engine e, int idx, double coeff, double expon, double offset)
    cdef int swmm_lid_count(SWMM_Engine e)
    # LID usage
    cdef int swmm_lid_usage_add(SWMM_Engine e, int subcatch_idx, int lid_idx, int number, double area, double width, double init_sat, double from_imperv)

cdef extern from "openswmm_quality.h":
    # Landuse
    cdef int         swmm_landuse_count(SWMM_Engine e)
    cdef int         swmm_landuse_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_landuse_id(SWMM_Engine e, int idx)
    cdef int swmm_landuse_add(SWMM_Engine e, const char* id)
    cdef int swmm_landuse_set_sweep_interval(SWMM_Engine e, int idx, double days)
    cdef int swmm_landuse_get_sweep_interval(SWMM_Engine e, int idx, double* days)
    cdef int swmm_landuse_set_sweep_removal(SWMM_Engine e, int idx, double frac)
    cdef int swmm_landuse_get_sweep_removal(SWMM_Engine e, int idx, double* frac)
    # Buildup
    cdef int swmm_buildup_set(SWMM_Engine e, int lu_idx, int pollut_idx,
                               int func_type, double c1, double c2, double c3, int normalizer)
    cdef int swmm_buildup_get(SWMM_Engine e, int lu_idx, int pollut_idx,
                               int* func_type, double* c1, double* c2, double* c3, int* normalizer)
    # Washoff
    cdef int swmm_washoff_set(SWMM_Engine e, int lu_idx, int pollut_idx,
                               int func_type, double coeff, double expon,
                               double sweep_effic, double bmp_effic)
    cdef int swmm_washoff_get(SWMM_Engine e, int lu_idx, int pollut_idx,
                               int* func_type, double* coeff, double* expon,
                               double* sweep_effic, double* bmp_effic)
    # Treatment
    cdef int swmm_treatment_set(SWMM_Engine e, int node_idx, int pollut_idx, const char* expression)
    cdef int swmm_treatment_get(SWMM_Engine e, int node_idx, int pollut_idx, char* buf, int buflen)
    cdef int swmm_treatment_clear(SWMM_Engine e, int node_idx, int pollut_idx)

cdef extern from "openswmm_statistics.h":
    # Node
    cdef int swmm_stat_node_max_depth(SWMM_Engine e, int idx, double* val)
    cdef int swmm_stat_node_max_overflow(SWMM_Engine e, int idx, double* val)
    cdef int swmm_stat_node_vol_flooded(SWMM_Engine e, int idx, double* val)
    cdef int swmm_stat_node_time_flooded(SWMM_Engine e, int idx, double* val)
    # Link
    cdef int swmm_stat_link_max_flow(SWMM_Engine e, int idx, double* val)
    cdef int swmm_stat_link_max_velocity(SWMM_Engine e, int idx, double* val)
    cdef int swmm_stat_link_max_filling(SWMM_Engine e, int idx, double* val)
    cdef int swmm_stat_link_vol_flow(SWMM_Engine e, int idx, double* val)
    cdef int swmm_stat_link_surcharge_time(SWMM_Engine e, int idx, double* val)
    # Subcatchment
    cdef int swmm_stat_subcatch_precip(SWMM_Engine e, int idx, double* val)
    cdef int swmm_stat_subcatch_runoff_vol(SWMM_Engine e, int idx, double* val)
    cdef int swmm_stat_subcatch_max_runoff(SWMM_Engine e, int idx, double* val)
    # Bulk
    cdef int swmm_stat_node_max_depth_bulk(SWMM_Engine e, double* buf, int count)
    cdef int swmm_stat_link_max_flow_bulk(SWMM_Engine e, double* buf, int count)
    cdef int swmm_stat_subcatch_runoff_vol_bulk(SWMM_Engine e, double* buf, int count)

cdef extern from "openswmm_spatial.h":
    # CRS
    cdef int swmm_spatial_set_crs(SWMM_Engine e, const char* crs)
    cdef int swmm_spatial_get_crs(SWMM_Engine e, char* buf, int buflen)
    # Node coordinates
    cdef int swmm_spatial_set_node_coord(SWMM_Engine e, int idx, double x, double y)
    cdef int swmm_spatial_get_node_coord(SWMM_Engine e, int idx, double* x, double* y)
    cdef int swmm_spatial_get_node_coords_bulk(SWMM_Engine e, double* x_buf, double* y_buf, int count)
    cdef int swmm_spatial_set_node_coords_bulk(SWMM_Engine e, const double* x_buf, const double* y_buf, int count)
    # Link coordinates
    cdef int swmm_spatial_set_link_coord(SWMM_Engine e, int idx, double x, double y)
    cdef int swmm_spatial_get_link_coord(SWMM_Engine e, int idx, double* x, double* y)
    # Link vertices
    cdef int swmm_spatial_set_link_vertices(SWMM_Engine e, int idx, const double* x, const double* y, int count)
    cdef int swmm_spatial_get_link_vertex_count(SWMM_Engine e, int idx, int* count)
    cdef int swmm_spatial_get_link_vertices(SWMM_Engine e, int idx, double* x, double* y, int max_count)
    # Subcatchment coordinates
    cdef int swmm_spatial_set_subcatch_coord(SWMM_Engine e, int idx, double x, double y)
    cdef int swmm_spatial_get_subcatch_coord(SWMM_Engine e, int idx, double* x, double* y)
    # Subcatchment polygon
    cdef int swmm_spatial_set_subcatch_polygon(SWMM_Engine e, int idx, const double* x, const double* y, int count)
    cdef int swmm_spatial_get_subcatch_polygon_count(SWMM_Engine e, int idx, int* count)
    cdef int swmm_spatial_get_subcatch_polygon(SWMM_Engine e, int idx, double* x, double* y, int max_count)
    # Gage coordinates
    cdef int swmm_spatial_set_gage_coord(SWMM_Engine e, int idx, double x, double y)
    cdef int swmm_spatial_get_gage_coord(SWMM_Engine e, int idx, double* x, double* y)

cdef extern from "openswmm_output.h":
    ctypedef void* SWMM_Output
    # Lifecycle
    cdef SWMM_Output swmm_output_open(const char* path)
    cdef void swmm_output_close(SWMM_Output handle)
    # Metadata
    cdef int swmm_output_get_version(SWMM_Output handle)
    cdef int swmm_output_get_flow_units(SWMM_Output handle)
    cdef int swmm_output_get_subcatch_count(SWMM_Output handle)
    cdef int swmm_output_get_node_count(SWMM_Output handle)
    cdef int swmm_output_get_link_count(SWMM_Output handle)
    cdef int swmm_output_get_pollut_count(SWMM_Output handle)
    cdef int swmm_output_get_period_count(SWMM_Output handle)
    cdef int swmm_output_get_start_date(SWMM_Output handle, double* start_date)
    cdef int swmm_output_get_report_step(SWMM_Output handle)
    # Object IDs
    cdef const char* swmm_output_get_subcatch_id(SWMM_Output handle, int index)
    cdef const char* swmm_output_get_node_id(SWMM_Output handle, int index)
    cdef const char* swmm_output_get_link_id(SWMM_Output handle, int index)
    # Per-period results
    cdef int swmm_output_get_subcatch_result(SWMM_Output handle, int period, int var, float* values)
    cdef int swmm_output_get_node_result(SWMM_Output handle, int period, int var, float* values)
    cdef int swmm_output_get_link_result(SWMM_Output handle, int period, int var, float* values)
    cdef int swmm_output_get_system_result(SWMM_Output handle, int period, int var, float* value)
    # Time series
    cdef int swmm_output_get_subcatch_series(SWMM_Output handle, int subcatch_idx, int var,
                                              int start_period, int end_period, float* values)
    cdef int swmm_output_get_node_series(SWMM_Output handle, int node_idx, int var,
                                          int start_period, int end_period, float* values)
    cdef int swmm_output_get_link_series(SWMM_Output handle, int link_idx, int var,
                                          int start_period, int end_period, float* values)
    cdef int swmm_output_get_system_series(SWMM_Output handle, int var,
                                            int start_period, int end_period, float* values)
    # Per-object attribute
    cdef int swmm_output_get_subcatch_attribute(SWMM_Output handle, int subcatch_idx, int period,
                                                 float* values, int* count)
    cdef int swmm_output_get_node_attribute(SWMM_Output handle, int node_idx, int period,
                                             float* values, int* count)
    cdef int swmm_output_get_link_attribute(SWMM_Output handle, int link_idx, int period,
                                             float* values, int* count)
    # Time
    cdef int swmm_output_get_period_time(SWMM_Output handle, int period, double* time)
    # Error
    cdef int swmm_output_get_error_code(SWMM_Output handle)


cdef extern from "openswmm_forcing.h":
    # Node forcing
    cdef int swmm_forcing_node_lat_inflow(SWMM_Engine e, int idx, double value, int mode, int persist)
    cdef int swmm_forcing_node_head_boundary(SWMM_Engine e, int idx, double value, int mode, int persist)
    cdef int swmm_forcing_node_quality(SWMM_Engine e, int node_idx, int pollutant_idx,
                                        double mass_rate, int mode, int persist)
    # Link forcing
    cdef int swmm_forcing_link_flow(SWMM_Engine e, int idx, double value, int mode, int persist)
    cdef int swmm_forcing_link_setting(SWMM_Engine e, int idx, double value, int mode, int persist)
    # Subcatchment forcing
    cdef int swmm_forcing_subcatch_rainfall(SWMM_Engine e, int idx, double value, int mode, int persist)
    cdef int swmm_forcing_subcatch_evap(SWMM_Engine e, int idx, double value, int mode, int persist)
    # Gage forcing
    cdef int swmm_forcing_gage_rainfall(SWMM_Engine e, int idx, double value, int mode, int persist)
    # Clear
    cdef int swmm_forcing_clear(SWMM_Engine e, int type, int idx)
    cdef int swmm_forcing_clear_all(SWMM_Engine e)


# --- Shared helper ---
cdef inline void _check(int code) except *:
    """Raise EngineError if code != 0."""
    cdef const char* msg
    if code != 0:
        msg = swmm_error_message(code)
        raise RuntimeError(
            msg.decode('utf-8') if msg != NULL else f"SWMM error {code}"
        )
