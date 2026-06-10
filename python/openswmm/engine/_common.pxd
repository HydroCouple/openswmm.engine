# :author: Caleb Buahin
# :copyright: Copyright (c) 2026 Caleb Buahin
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
    ctypedef void (*SWMM_ProgressCallback)(void* engine, double elapsed_frac, double sim_time, void* user_data)
    ctypedef void (*SWMM_WarningCallback)(SWMM_Engine engine, int code, const char* msg, void* user_data)
    ctypedef void (*SWMM_StepBeginCallback)(SWMM_Engine engine, double sim_time, double dt, void* user_data)
    ctypedef void (*SWMM_StepEndCallback)(SWMM_Engine engine, double sim_time, double dt, void* user_data)

    # --- Error reporting ---
    cdef int         swmm_get_last_error(SWMM_Engine e)
    cdef const char* swmm_get_last_error_msg(SWMM_Engine e)
    cdef const char* swmm_error_message(int code)

    # --- Engine lifecycle ---
    cdef SWMM_Engine swmm_engine_create()
    cdef int  swmm_engine_open(SWMM_Engine e, const char* inp, const char* rpt, const char* out, const char* input_plugin_lib)
    cdef int  swmm_engine_initialize(SWMM_Engine e)
    cdef int  swmm_engine_start(SWMM_Engine e, int save_results)
    # NOTE: step/stride may invoke registered step_begin/step_end callbacks.
    # Those trampolines reacquire the GIL via `noexcept with gil:`, so it is
    # safe to release the GIL around the C call itself (and necessary, so
    # that another thread can advance an independent engine concurrently).
    cdef int  swmm_engine_step(SWMM_Engine e, double* elapsed_time) nogil
    cdef int  swmm_engine_stride(SWMM_Engine e, int n_steps, double* elapsed_time) nogil
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

    # --- Units (typed accessors; new in the latest API expansion) ---
    cdef int swmm_get_flow_units(SWMM_Engine e, int* flow_units)
    cdef int swmm_get_unit_system(SWMM_Engine e, int* unit_system)

    # --- Routing events / steady-state ---
    cdef int swmm_is_between_events(SWMM_Engine e, int* is_between)
    cdef int swmm_get_event_count(SWMM_Engine e, int* count)
    cdef int swmm_get_steady_state_skip(SWMM_Engine e, int* enabled)
    cdef int swmm_set_steady_state_skip(SWMM_Engine e, int enabled)
    # Phase 1b: runoff interface file (legacy "Frunoff"). I/O-bound,
    # so each is declared nogil to allow the GIL to be released around
    # the C call (the wrappers in _solver.pyx wrap them in `with nogil:`).
    cdef int swmm_runoff_iface_open_write(SWMM_Engine e, const char* path) nogil
    cdef int swmm_runoff_iface_open_read(SWMM_Engine e, const char* path) nogil
    cdef int swmm_runoff_iface_save_step(SWMM_Engine e, double dt) nogil
    cdef int swmm_runoff_iface_read_step(SWMM_Engine e, int* has_data) nogil
    cdef int swmm_runoff_iface_close(SWMM_Engine e) nogil

    # --- [EVENTS] section editor (Slice CW, 2026-05-21) ---
    cdef int swmm_events_count(SWMM_Engine e, int* count)
    cdef int swmm_events_get(SWMM_Engine e, int idx, double* start, double* end)
    cdef int swmm_events_set(SWMM_Engine e, int idx, double start, double end)
    cdef int swmm_events_add(SWMM_Engine e, double start, double end, int* out_idx)
    cdef int swmm_events_remove(SWMM_Engine e, int idx)
    cdef int swmm_events_clear(SWMM_Engine e)

    # --- Batch run helpers ---
    cdef int swmm_engine_run(const char* inp, const char* rpt, const char* out,
                             const char* input_plugin_lib)
    cdef int swmm_engine_run_with_callback(
        const char* inp, const char* rpt, const char* out,
        const char* input_plugin_lib,
        SWMM_ProgressCallback callback, void* user_data)

    # --- Callbacks ---
    cdef int swmm_set_progress_callback(SWMM_Engine e, SWMM_ProgressCallback cb, void* ud)
    cdef int swmm_set_warning_callback(SWMM_Engine e, SWMM_WarningCallback cb, void* ud)
    cdef int swmm_set_step_begin_callback(SWMM_Engine e, SWMM_StepBeginCallback cb, void* ud)
    cdef int swmm_set_step_end_callback(SWMM_Engine e, SWMM_StepEndCallback cb, void* ud)

cdef extern from "openswmm_datetime.h":
    # SWMM DateTime is a double: integer days since 1899-12-30 + fractional day.
    cdef int swmm_datetime_encode_date(int year, int month, int day, double* out)
    cdef int swmm_datetime_encode_time(int hour, int minute, int second, double* out)
    cdef int swmm_datetime_decode_date(double value, int* year, int* month, int* day)
    cdef int swmm_datetime_decode_time(double value, int* hour, int* minute, int* second)
    cdef int swmm_datetime_add_seconds(double value, double seconds, double* out)
    cdef int swmm_datetime_time_diff(double value1, double value2, long* out)

cdef extern from "openswmm_model.h":
    cdef SWMM_Engine swmm_engine_new()
    cdef int swmm_validate_model(SWMM_Engine e)
    cdef int swmm_finalize_model(SWMM_Engine e)
    cdef int swmm_model_write(SWMM_Engine e, const char* path)
    cdef int swmm_model_write_with_plugin(SWMM_Engine e, const char* new_path,
                                           const char* output_plugin_id)
    # Plugins
    cdef int swmm_plugins_count(SWMM_Engine e, int* count)
    cdef int swmm_plugin_get(SWMM_Engine e, int idx,
                              char* path_buf, int path_buf_sz,
                              char* args_buf, int args_buf_sz)
    cdef int swmm_plugin_set(SWMM_Engine e, const char* path_or_id, const char* args)
    cdef int swmm_plugin_remove(SWMM_Engine e, const char* path_or_id)
    # [FILES] section
    cdef int swmm_files_get(SWMM_Engine e, const char* key, char* buf, int buflen)
    cdef int swmm_files_set(SWMM_Engine e, const char* key, const char* value)
    # Title management
    cdef int swmm_title_get_count(SWMM_Engine e, int* count)
    cdef int swmm_title_get_line(SWMM_Engine e, int index, char* buf, int buflen)
    cdef int swmm_title_add_line(SWMM_Engine e, const char* line)
    cdef int swmm_title_set(SWMM_Engine e, const char* text)
    cdef int swmm_title_clear(SWMM_Engine e)
    cdef int swmm_options_get(SWMM_Engine e, const char* key, char* buf, int buflen)
    cdef int swmm_options_set(SWMM_Engine e, const char* key, const char* value)
    cdef int swmm_options_get_ext(SWMM_Engine e, const char* key, char* buf, int buflen)
    cdef int swmm_options_set_ext(SWMM_Engine e, const char* key, const char* value)
    # External-file slots. `role` is the C enum SWMM_FilePathRole; the C++
    # header rejects an implicit int->enum conversion, so declare the enum
    # type here and cast at the call sites (see _model.pyx).
    ctypedef enum SWMM_FilePathRole:
        pass
    cdef int swmm_file_path_get(SWMM_Engine e, SWMM_FilePathRole role, const char* owner,
                                char* absolute_buf, int absolute_buflen,
                                char* original_buf, int original_buflen)
    cdef int swmm_file_path_set(SWMM_Engine e, SWMM_FilePathRole role, const char* owner,
                                const char* new_path)
    cdef int swmm_get_crs(SWMM_Engine e, char* buf, int buflen)
    # Typed time-control accessors (OADate doubles)
    cdef int swmm_options_get_start_date(SWMM_Engine e, double* value)
    cdef int swmm_options_set_start_date(SWMM_Engine e, double value)
    cdef int swmm_options_get_end_date(SWMM_Engine e, double* value)
    cdef int swmm_options_set_end_date(SWMM_Engine e, double value)
    cdef int swmm_options_get_report_start(SWMM_Engine e, double* value)
    cdef int swmm_options_set_report_start(SWMM_Engine e, double value)
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
    cdef int swmm_node_pop_last(SWMM_Engine e, const char* id)
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
    cdef int swmm_node_get_outfall_tidal(SWMM_Engine e, int idx, int* curve_idx)
    cdef int swmm_node_set_outfall_timeseries(SWMM_Engine e, int idx, int ts_idx)
    cdef int swmm_node_get_outfall_timeseries(SWMM_Engine e, int idx, int* ts_idx)
    cdef int swmm_node_get_outfall_param(SWMM_Engine e, int idx, double* param)
    cdef int swmm_node_set_outfall_flap_gate(SWMM_Engine e, int idx, int has_gate)
    cdef int swmm_node_get_outfall_flap_gate(SWMM_Engine e, int idx, int* has_gate)
    # Statistics
    cdef int swmm_node_get_stat_max_depth(SWMM_Engine e, int idx, double* val)
    cdef int swmm_node_get_stat_max_overflow(SWMM_Engine e, int idx, double* val)
    cdef int swmm_node_get_stat_vol_flooded(SWMM_Engine e, int idx, double* val)
    cdef int swmm_node_get_stat_time_flooded(SWMM_Engine e, int idx, double* val)
    # Bulk access
    # Bulk node accessors — pure C memory ops, safe to call without the GIL.
    cdef int swmm_node_get_depths_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_node_get_heads_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_node_get_inflows_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_node_get_overflows_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_node_set_depths_bulk(SWMM_Engine e, const double* buf, int count) nogil
    cdef int swmm_node_set_lat_inflows_bulk(SWMM_Engine e, const double* buf, int count) nogil
    cdef int swmm_node_get_quality_bulk(SWMM_Engine e, int pollutant_idx, double* buf, int count) nogil
    # Phase 3 bulk getters (volumes, outflows, losses, lateral_inflows, ids).
    cdef int swmm_node_get_volumes_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_node_get_outflows_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_node_get_losses_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_node_get_lateral_inflows_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_node_get_ids_bulk(SWMM_Engine e, char* buf, int stride, int count) nogil
    # Outfall route-to
    cdef int swmm_node_set_outfall_route_to(SWMM_Engine e, int idx, int subcatch_idx)
    cdef int swmm_node_get_outfall_route_to(SWMM_Engine e, int idx, int* subcatch_idx)
    # Depth from volume
    cdef int swmm_node_get_depth_from_volume(SWMM_Engine e, int idx, double volume, double* depth)
    # Quality mass flux
    cdef int swmm_node_set_quality_mass_flux(SWMM_Engine e, int node_idx, int pollutant_idx, double mass_rate)
    # Divider
    cdef int swmm_node_set_divider_type(SWMM_Engine e, int idx, int type)
    cdef int swmm_node_get_divider_type(SWMM_Engine e, int idx, int* type)
    # Rename
    cdef int swmm_node_rename(SWMM_Engine e, int idx, const char* newId)
    cdef int swmm_node_get_tag(SWMM_Engine e, int idx, char* buf, int buflen)
    cdef int swmm_node_set_tag(SWMM_Engine e, int idx, const char* tag)

cdef extern from "openswmm_links.h":
    # Identity
    cdef int         swmm_link_count(SWMM_Engine e)
    cdef int         swmm_link_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_link_id(SWMM_Engine e, int idx)
    # Creation
    cdef int swmm_link_add(SWMM_Engine e, const char* id, int type)
    cdef int swmm_link_pop_last(SWMM_Engine e, const char* id)
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
    # Engine gaps BN-LINK-01a / -01b — symmetric getters added 2026-05-25.
    cdef int swmm_link_get_initial_flow(SWMM_Engine e, int idx, double* flow)
    cdef int swmm_link_get_max_flow(SWMM_Engine e, int idx, double* flow)
    # Engine gap BN-LINK-02 — orifice TYPE (SIDE=0 / BOTTOM=1) — added 2026-05-25.
    cdef int swmm_link_get_orifice_type(SWMM_Engine e, int idx, int* type_)
    cdef int swmm_link_set_orifice_type(SWMM_Engine e, int idx, int type_)
    # Engine gap BN-LINK-03 — weir TYPE (5 values) — added 2026-05-25.
    cdef int swmm_link_get_weir_type(SWMM_Engine e, int idx, int* type_)
    cdef int swmm_link_set_weir_type(SWMM_Engine e, int idx, int type_)
    # Engine gap BN-LINK-04 — outlet rating type (4 values) + exponent — 2026-05-25.
    cdef int swmm_link_get_outlet_rating_type(SWMM_Engine e, int idx, int* type_)
    cdef int swmm_link_set_outlet_rating_type(SWMM_Engine e, int idx, int type_)
    cdef int swmm_link_get_outlet_expon(SWMM_Engine e, int idx, double* expon)
    cdef int swmm_link_set_outlet_expon(SWMM_Engine e, int idx, double expon)
    # Engine gap BN-LINK-05 — pump startup / shutoff depth — added 2026-05-25.
    cdef int swmm_link_get_pump_startup_depth(SWMM_Engine e, int idx, double* depth)
    cdef int swmm_link_set_pump_startup_depth(SWMM_Engine e, int idx, double depth)
    cdef int swmm_link_get_pump_shutoff_depth(SWMM_Engine e, int idx, double* depth)
    cdef int swmm_link_set_pump_shutoff_depth(SWMM_Engine e, int idx, double depth)
    # Engine gap BN-LINK-06 — orifice open/close rate (1/s) — added 2026-05-25.
    cdef int swmm_link_get_orifice_open_close_rate(SWMM_Engine e, int idx, double* rate)
    cdef int swmm_link_set_orifice_open_close_rate(SWMM_Engine e, int idx, double rate)
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
    # Pump statistics
    cdef int swmm_link_get_stat_pump_cycles(SWMM_Engine e, int idx, int* cycles)
    cdef int swmm_link_get_stat_pump_on_time(SWMM_Engine e, int idx, double* seconds)
    cdef int swmm_link_get_stat_pump_volume(SWMM_Engine e, int idx, double* volume)
    cdef int swmm_link_get_pump_stats_bulk(SWMM_Engine e, int* cycles,
                                            double* on_time, double* volume,
                                            int count) nogil
    # Hydraulic power
    cdef int swmm_link_get_hyd_power(SWMM_Engine e, int idx, double* power)
    # Bulk access
    # Bulk link accessors — pure C memory ops, safe to call without the GIL.
    cdef int swmm_link_get_flows_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_link_get_depths_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_link_set_flows_bulk(SWMM_Engine e, const double* buf, int count) nogil
    cdef int swmm_link_get_quality_bulk(SWMM_Engine e, int pollutant_idx, double* buf, int count) nogil
    # Phase 3 bulk getters — velocities/capacities/volumes/control/target/power, ids.
    cdef int swmm_link_get_velocities_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_link_get_capacities_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_link_get_volumes_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_link_get_control_settings_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_link_get_target_settings_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_link_get_hyd_powers_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_link_get_ids_bulk(SWMM_Engine e, char* buf, int stride, int count) nogil
    # Rename
    cdef int swmm_link_rename(SWMM_Engine e, int idx, const char* newId)
    cdef int swmm_link_get_tag(SWMM_Engine e, int idx, char* buf, int buflen)
    cdef int swmm_link_set_tag(SWMM_Engine e, int idx, const char* tag)

cdef extern from "openswmm_subcatchments.h":
    # Identity
    cdef int         swmm_subcatch_count(SWMM_Engine e)
    cdef int         swmm_subcatch_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_subcatch_id(SWMM_Engine e, int idx)
    # Creation
    cdef int swmm_subcatch_add(SWMM_Engine e, const char* id)
    # Aquifers and snowpacks (model-global named objects)
    cdef int         swmm_aquifer_count(SWMM_Engine e)
    cdef int         swmm_aquifer_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_aquifer_id(SWMM_Engine e, int idx)
    cdef int         swmm_aquifer_add(SWMM_Engine e, const char* id)
    cdef int         swmm_snowpack_count(SWMM_Engine e)
    cdef int         swmm_snowpack_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_snowpack_id(SWMM_Engine e, int idx)
    cdef int         swmm_snowpack_add(SWMM_Engine e, const char* id)
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
    # Bulk subcatchment accessors — pure C memory ops, safe to call without the GIL.
    cdef int swmm_subcatch_get_runoff_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_subcatch_get_quality_bulk(SWMM_Engine e, int pollutant_idx, double* buf, int count) nogil
    # Phase 3 bulk getters — rainfall/evap/infil/snow_depth + ids.
    cdef int swmm_subcatch_get_rainfall_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_subcatch_get_evap_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_subcatch_get_infil_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_subcatch_get_snow_depth_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_subcatch_get_ids_bulk(SWMM_Engine e, char* buf, int stride, int count) nogil
    # Ponded quality
    cdef int swmm_subcatch_get_ponded_quality(SWMM_Engine e, int subcatch_idx, int pollutant_idx, double* mass)
    cdef int swmm_subcatch_set_ponded_quality(SWMM_Engine e, int subcatch_idx, int pollutant_idx, double mass)
    # Rename
    cdef int swmm_subcatch_rename(SWMM_Engine e, int idx, const char* newId)
    cdef int swmm_subcatch_get_tag(SWMM_Engine e, int idx, char* buf, int buflen)
    cdef int swmm_subcatch_set_tag(SWMM_Engine e, int idx, const char* tag)

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
    cdef int swmm_gage_set_scale_factor(SWMM_Engine e, int idx, double factor)
    # Property getters
    cdef int swmm_gage_get_rain_type(SWMM_Engine e, int idx, int* type)
    cdef int swmm_gage_get_data_source(SWMM_Engine e, int idx, int* source)
    cdef int swmm_gage_get_scale_factor(SWMM_Engine e, int idx, double* factor)
    # State
    cdef int swmm_gage_get_rainfall(SWMM_Engine e, int idx, double* rainfall)
    cdef int swmm_gage_set_rainfall(SWMM_Engine e, int idx, double rainfall)
    # Bulk
    cdef int swmm_gage_get_rainfall_bulk(SWMM_Engine e, double* buf, int count) nogil
    # Rename
    cdef int swmm_gage_rename(SWMM_Engine e, int idx, const char* newId)

cdef extern from "openswmm_massbalance.h":
    cdef int swmm_get_runoff_continuity_error(SWMM_Engine e, double* error)
    cdef int swmm_get_routing_continuity_error(SWMM_Engine e, double* error)
    cdef int swmm_get_quality_continuity_error(SWMM_Engine e, int pollutant_idx, double* error)
    cdef int swmm_get_runoff_total(SWMM_Engine e, int component, double* volume)
    cdef int swmm_get_routing_total(SWMM_Engine e, int component, double* volume)
    # Routing diagnostics
    cdef int swmm_get_routing_stats(SWMM_Engine e, double* avg_step, double* min_step,
                                    double* max_step, int* n_steps,
                                    double* pct_non_converged, double* avg_iterations,
                                    double* max_courant)
    cdef int swmm_get_max_courant(SWMM_Engine e, double* max_courant)
    # Quality losses
    cdef int swmm_get_quality_seep_loss(SWMM_Engine e, int pollutant_idx, double* mass)
    cdef int swmm_get_quality_evap_loss(SWMM_Engine e, int pollutant_idx, double* mass)

cdef extern from "openswmm_hotstart.h":
    # File I/O — heavy enough to warrant releasing the GIL while
    # the C side reads or writes the (potentially large) hotstart blob.
    cdef int swmm_hotstart_save(SWMM_Engine e, const char* path) nogil
    cdef int swmm_hotstart_open(const char* path, SWMM_HotStart* hs) nogil
    cdef int swmm_hotstart_apply(SWMM_Engine e, SWMM_HotStart hs) nogil
    cdef int swmm_hotstart_close(SWMM_HotStart hs) nogil
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
    # Save schedule ([SAVE HOTSTART] entries on the engine)
    cdef int swmm_hotstart_saves_count(SWMM_Engine e, int* count)
    cdef int swmm_hotstart_saves_get_path(SWMM_Engine e, int idx, char* buf, int buflen)
    cdef int swmm_hotstart_saves_get_datetime(SWMM_Engine e, int idx, double* datetime)
    cdef int swmm_hotstart_saves_set_path(SWMM_Engine e, int idx, const char* path)
    cdef int swmm_hotstart_saves_set_datetime(SWMM_Engine e, int idx, double datetime)
    cdef int swmm_hotstart_saves_add(SWMM_Engine e, const char* path, double datetime)
    cdef int swmm_hotstart_saves_remove(SWMM_Engine e, int idx)
    cdef int swmm_hotstart_saves_clear(SWMM_Engine e)

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
    cdef int         swmm_table_get_type(SWMM_Engine e, int idx, int* type)
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
    cdef int swmm_pattern_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_pattern_id(SWMM_Engine e, int idx)
    cdef int swmm_pattern_get_type(SWMM_Engine e, int idx, int* type)
    cdef int swmm_pattern_get_factor_count(SWMM_Engine e, int idx, int* count)
    cdef int swmm_pattern_get_factor(SWMM_Engine e, int idx, int i, double* v)
    cdef int swmm_pattern_remove(SWMM_Engine e, int idx)
    cdef int swmm_pattern_rename(SWMM_Engine e, int idx, const char* newId)

cdef extern from "openswmm_inflows.h":
    cdef int swmm_ext_inflow_add(SWMM_Engine e, int node_idx, const char* constituent,
                                  const char* ts_name, const char* type,
                                  double m_factor, double s_factor, double baseline,
                                  const char* pattern)
    cdef int swmm_dwf_add(SWMM_Engine e, int node_idx, const char* constituent,
                           double avg_value, const char* pat1, const char* pat2,
                           const char* pat3, const char* pat4)
    cdef int swmm_rdii_add(SWMM_Engine e, int node_idx, const char* uh_name, double area)
    cdef int swmm_rdii_get(SWMM_Engine e, int entry_idx,
                            int* node_idx, char* uh_buf, int buflen,
                            double* area)
    cdef int swmm_ext_inflow_count(SWMM_Engine e)
    cdef int swmm_dwf_count(SWMM_Engine e)
    cdef int swmm_rdii_count(SWMM_Engine e)
    # Unit hydrographs ([HYDROGRAPHS])
    cdef int swmm_hydrograph_add(SWMM_Engine e, const char* uh_name,
                                  int month, int response,
                                  double r, double t, double k,
                                  double dmax, double drecov, double dinit)
    cdef int swmm_hydrograph_get(SWMM_Engine e, int entry_idx,
                                  char* uh_buf, int buflen,
                                  int* month, int* response,
                                  double* r, double* t, double* k,
                                  double* dmax, double* drecov, double* dinit)
    cdef int swmm_hydrograph_count(SWMM_Engine e)
    cdef int swmm_hydrograph_add_gage(SWMM_Engine e,
                                       const char* uh_name,
                                       const char* gage_name)
    cdef int swmm_hydrograph_get_gage(SWMM_Engine e, int entry_idx,
                                       char* uh_buf, int uh_buflen,
                                       char* gage_buf, int gage_buflen)
    cdef int swmm_hydrograph_gage_count(SWMM_Engine e)
    cdef int swmm_hydrograph_group_count(SWMM_Engine e)
    cdef int swmm_hydrograph_group_id(SWMM_Engine e, int idx,
                                       char* buf, int buflen)
    # Exponential IA decay ([RDII_DECAY])
    cdef int swmm_rdii_decay_add(SWMM_Engine e, const char* uh_name,
                                  int response,
                                  double k_dep, double k_0, double k_T,
                                  double T_ref, double theta_rec, double T_freeze)
    cdef int swmm_rdii_decay_get(SWMM_Engine e, int entry_idx,
                                  char* uh_buf, int buflen,
                                  int* response,
                                  double* k_dep, double* k_0, double* k_T,
                                  double* T_ref, double* theta_rec, double* T_freeze)
    cdef int swmm_rdii_decay_count(SWMM_Engine e)
    # Read / remove side (added in the 2026 binding refresh) — entry-index keyed
    cdef int swmm_ext_inflow_get(SWMM_Engine e, int entry_idx, int* node_idx, char* constituent_buf, int constituent_buflen, char* ts_buf, int ts_buflen, char* type_buf, int type_buflen, double* m_factor, double* s_factor, double* baseline, char* pattern_buf, int pattern_buflen)
    cdef int swmm_ext_inflow_remove(SWMM_Engine e, int entry_idx)
    cdef int swmm_dwf_get(SWMM_Engine e, int entry_idx, int* node_idx, char* constituent_buf, int constituent_buflen, double* avg_value, char* pat1_buf, int pat1_buflen, char* pat2_buf, int pat2_buflen, char* pat3_buf, int pat3_buflen, char* pat4_buf, int pat4_buflen)
    cdef int swmm_dwf_remove(SWMM_Engine e, int entry_idx)
    cdef int swmm_rdii_remove(SWMM_Engine e, int entry_idx)
    # Unit-hydrograph editing — (uh_name, month, response) keyed
    cdef int swmm_hydrograph_set_rtk(SWMM_Engine e, const char* uh_name, int month, int response, double r, double t, double k)
    cdef int swmm_hydrograph_set_ia(SWMM_Engine e, const char* uh_name, int month, int response, double dmax, double drecov, double dinit)
    cdef int swmm_hydrograph_remove_entry(SWMM_Engine e, const char* uh_name, int month, int response)
    cdef int swmm_hydrograph_remove_group(SWMM_Engine e, const char* uh_name)
    cdef int swmm_hydrograph_clear_group_months(SWMM_Engine e, const char* uh_name)
    cdef int swmm_hydrograph_group_rename(SWMM_Engine e, int idx, const char* new_id)
    cdef int swmm_hydrograph_set_gage(SWMM_Engine e, const char* uh_name, const char* gage_name)
    # RDII decay editing — (uh_name, response) keyed
    cdef int swmm_rdii_decay_set(SWMM_Engine e, const char* uh_name, int response, double k_dep, double k_0, double k_T, double T_ref, double theta_rec, double T_freeze)
    cdef int swmm_rdii_decay_remove(SWMM_Engine e, const char* uh_name, int response)

cdef extern from "openswmm_controls.h":
    cdef int swmm_control_add_rule(SWMM_Engine e, const char* rule_text)
    cdef int swmm_control_validate_rule(SWMM_Engine e, const char* rule_text, char* err_buf, int err_buf_len, int* line_out)
    cdef int swmm_control_count(SWMM_Engine e)
    cdef int swmm_control_get_rule(SWMM_Engine e, int idx, char* buf, int buflen)
    cdef int swmm_control_get_id(SWMM_Engine e, int idx, char* buf, int buflen)
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
    cdef int swmm_street_get_params(SWMM_Engine e, int idx,
                                     double* t_crown, double* h_curb, double* sx, double* n_road,
                                     double* gutter_depres, double* gutter_width, int* sides,
                                     double* back_width, double* back_slope, double* back_n)
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
    # Identity lookups (read side)
    cdef int         swmm_transect_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_transect_id(SWMM_Engine e, int idx)
    cdef int         swmm_street_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_street_id(SWMM_Engine e, int idx)
    cdef int         swmm_inlet_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_inlet_id(SWMM_Engine e, int idx)
    cdef int         swmm_lid_index(SWMM_Engine e, const char* id)
    cdef const char* swmm_lid_id(SWMM_Engine e, int idx)
    # Transect detail getters / mutation
    cdef int swmm_transect_get_roughness(SWMM_Engine e, int idx, double* n_left, double* n_right, double* n_channel)
    cdef int swmm_transect_set_bank_stations(SWMM_Engine e, int idx, double left, double right)
    cdef int swmm_transect_get_bank_stations(SWMM_Engine e, int idx, double* left, double* right)
    cdef int swmm_transect_set_encroachment_stations(SWMM_Engine e, int idx, double left, double right)
    cdef int swmm_transect_get_encroachment_stations(SWMM_Engine e, int idx, double* left, double* right)
    cdef int swmm_transect_set_modifiers(SWMM_Engine e, int idx, double n_factor, double x_factor, double y_factor)
    cdef int swmm_transect_get_modifiers(SWMM_Engine e, int idx, double* n_factor, double* x_factor, double* y_factor)
    cdef int swmm_transect_set_comments(SWMM_Engine e, int idx, const char* text)
    cdef int swmm_transect_get_comments(SWMM_Engine e, int idx, char* buf, int buflen)
    cdef int swmm_transect_get_station_count(SWMM_Engine e, int idx)
    cdef int swmm_transect_get_station(SWMM_Engine e, int idx, int station_idx, double* station, double* elevation)
    cdef int swmm_transect_clear_stations(SWMM_Engine e, int idx)
    cdef int swmm_transect_rename(SWMM_Engine e, int idx, const char* new_id)
    cdef int swmm_transect_remove(SWMM_Engine e, int idx)

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
    cdef int swmm_stat_node_max_depth_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_stat_link_max_flow_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_stat_subcatch_runoff_vol_bulk(SWMM_Engine e, double* buf, int count) nogil
    # Phase 3 statistics bulk getters — flooding + max-runoff.
    cdef int swmm_stat_node_max_overflow_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_stat_node_vol_flooded_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_stat_node_time_flooded_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_stat_subcatch_max_runoff_bulk(SWMM_Engine e, double* buf, int count) nogil
    # Phase 4e link-stat bulks — completes the per-link statistics surface.
    cdef int swmm_stat_link_max_velocity_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_stat_link_max_filling_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_stat_link_vol_flow_bulk(SWMM_Engine e, double* buf, int count) nogil
    cdef int swmm_stat_link_surcharge_time_bulk(SWMM_Engine e, double* buf, int count) nogil

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
    # Per-period results — disk-backed read into caller's float buffer.
    # nogil: pure C I/O, no Python objects touched.
    cdef int swmm_output_get_subcatch_result(SWMM_Output handle, int period, int var, float* values) nogil
    cdef int swmm_output_get_node_result(SWMM_Output handle, int period, int var, float* values) nogil
    cdef int swmm_output_get_link_result(SWMM_Output handle, int period, int var, float* values) nogil
    cdef int swmm_output_get_system_result(SWMM_Output handle, int period, int var, float* value) nogil
    # Time series — potentially large reads from the .out file.
    cdef int swmm_output_get_subcatch_series(SWMM_Output handle, int subcatch_idx, int var,
                                              int start_period, int end_period, float* values) nogil
    cdef int swmm_output_get_node_series(SWMM_Output handle, int node_idx, int var,
                                          int start_period, int end_period, float* values) nogil
    cdef int swmm_output_get_link_series(SWMM_Output handle, int link_idx, int var,
                                          int start_period, int end_period, float* values) nogil
    cdef int swmm_output_get_system_series(SWMM_Output handle, int var,
                                            int start_period, int end_period, float* values) nogil
    # Per-object attribute
    cdef int swmm_output_get_subcatch_attribute(SWMM_Output handle, int subcatch_idx, int period,
                                                 float* values, int* count) nogil
    cdef int swmm_output_get_node_attribute(SWMM_Output handle, int node_idx, int period,
                                             float* values, int* count) nogil
    cdef int swmm_output_get_link_attribute(SWMM_Output handle, int link_idx, int period,
                                             float* values, int* count) nogil
    # Time
    cdef int swmm_output_get_period_time(SWMM_Output handle, int period, double* time)
    # Error
    cdef int swmm_output_get_error_code(SWMM_Output handle)
    # Post-run node statistics aggregated from the .out file
    # (added 2026-05 to the engine; bound here in the Phase 5 drift sweep).
    cdef int swmm_output_get_node_stat_max_depth(SWMM_Output handle,
                                                  int node_idx, double* value) nogil
    cdef int swmm_output_get_node_stat_max_overflow(SWMM_Output handle,
                                                     int node_idx, double* value) nogil
    cdef int swmm_output_get_node_stat_vol_flooded(SWMM_Output handle,
                                                    int node_idx, double* value) nogil
    cdef int swmm_output_get_node_stat_time_flooded(SWMM_Output handle,
                                                     int node_idx, double* value) nogil


cdef extern from "openswmm_edit.h":

    # Reference type enum
    cdef enum SWMM_RefType:
        SWMM_REF_NODE        = 0
        SWMM_REF_LINK        = 1
        SWMM_REF_SUBCATCH    = 2
        SWMM_REF_GAGE        = 3
        SWMM_REF_TABLE       = 4
        SWMM_REF_TRANSECT    = 5
        SWMM_REF_INLET_USAGE = 6

    # Impact entry
    cdef struct SWMM_ImpactEntry:
        int         obj_type
        int         obj_idx
        const char* field
        int         cascaded

    # Impact report (heap-allocated entries)
    cdef struct SWMM_ImpactReport:
        SWMM_ImpactEntry* entries
        int               n_entries

    # Conversion result (heap-allocated string arrays)
    cdef struct SWMM_ConversionResult:
        int          new_type
        const char** cleared_fields
        int          n_cleared
        const char** warnings
        int          n_warnings

    # Memory release
    cdef void swmm_impact_report_free(SWMM_ImpactReport* report)
    cdef void swmm_conversion_result_free(SWMM_ConversionResult* result)

    # Non-destructive impact analysis
    cdef int swmm_node_analyze_impact    (SWMM_Engine e, int idx, SWMM_ImpactReport* out)
    cdef int swmm_link_analyze_impact    (SWMM_Engine e, int idx, SWMM_ImpactReport* out)
    cdef int swmm_subcatch_analyze_impact(SWMM_Engine e, int idx, SWMM_ImpactReport* out)
    cdef int swmm_gage_analyze_impact    (SWMM_Engine e, int idx, SWMM_ImpactReport* out)
    cdef int swmm_table_analyze_impact   (SWMM_Engine e, int idx, SWMM_ImpactReport* out)
    cdef int swmm_transect_analyze_impact(SWMM_Engine e, int idx, SWMM_ImpactReport* out)

    # Deletion (cascade + renumber)
    cdef int swmm_node_delete    (SWMM_Engine e, int idx, SWMM_ImpactReport* out)
    cdef int swmm_link_delete    (SWMM_Engine e, int idx, SWMM_ImpactReport* out)
    cdef int swmm_subcatch_delete(SWMM_Engine e, int idx, SWMM_ImpactReport* out)
    cdef int swmm_gage_delete    (SWMM_Engine e, int idx, SWMM_ImpactReport* out)
    cdef int swmm_table_delete   (SWMM_Engine e, int idx, SWMM_ImpactReport* out)
    cdef int swmm_transect_delete(SWMM_Engine e, int idx, SWMM_ImpactReport* out)

    # In-place type conversion
    cdef int swmm_node_convert(SWMM_Engine e, int idx, int new_type,
                                SWMM_ConversionResult* out)
    cdef int swmm_link_convert(SWMM_Engine e, int idx, int new_type,
                                SWMM_ConversionResult* out)


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
    cdef int swmm_climate_get_evap_rate(SWMM_Engine e, double* value)
    # Gage forcing
    cdef int swmm_forcing_gage_rainfall(SWMM_Engine e, int idx, double value, int mode, int persist)
    # Clear
    cdef int swmm_forcing_clear(SWMM_Engine e, int type, int idx)
    cdef int swmm_forcing_clear_all(SWMM_Engine e)


# --- Shared helpers ---
cdef inline void _check(int code) except *:
    """Raise the right ``EngineError`` subclass for a non-zero ``code``.

    ``code == 0`` is a no-op. Dispatch on the integer code is delegated to
    :func:`openswmm.engine._exceptions.raise_for_code` so the mapping is
    expressed once, in pure Python, and stays in lockstep with the
    :class:`openswmm.engine.ErrorCode` enum.
    """
    cdef const char* msg
    cdef str py_msg
    if code == 0:
        return
    # Resolve the engine's own message text. Import is intentionally local so
    # the cdef inline can be used from any module without circular-import risk.
    from openswmm.engine._exceptions import raise_for_code
    msg = swmm_error_message(code)
    py_msg = msg.decode('utf-8') if msg != NULL else ""
    raise_for_code(code, py_msg)


# Function-pointer typedefs for the polymorphic ``_resolve_index`` helper —
# every domain has a matching ``swmm_*_count(handle)`` and
# ``swmm_*_index(handle, id)`` pair, so we can express the resolver once.
ctypedef int (*swmm_index_fn_t)(SWMM_Engine e, const char* id)
ctypedef int (*swmm_count_fn_t)(SWMM_Engine e)


cdef inline int _resolve_index(
    SWMM_Engine handle,
    object key,
    swmm_index_fn_t index_fn,
    swmm_count_fn_t count_fn,
    str obj_name,
) except -1:
    """Resolve a string id or integer index to a validated integer index.

    :param handle: Engine handle.
    :param key: ``int`` index or ``str`` object id.
    :param index_fn: C lookup function for the domain (e.g.
        ``swmm_node_index``).
    :param count_fn: C count function for the domain (e.g.
        ``swmm_node_count``).
    :param obj_name: Human-readable domain name used in error messages
        (e.g. ``"Node"``).
    :returns: A validated integer index in ``[0, count)``.
    :raises KeyError: ``key`` is a string and no object with that id exists.
    :raises IndexError: ``key`` is an int out of range.
    :raises TypeError: ``key`` is neither ``int`` nor ``str``.
    """
    cdef int i
    cdef int n
    cdef bytes b
    if isinstance(key, str):
        b = (<str>key).encode('utf-8')
        i = index_fn(handle, b)
        if i < 0:
            raise KeyError(f"{obj_name} '{key}' not found")
        return i
    # ``bool`` is a subclass of ``int`` — reject it explicitly so
    # ``solver.nodes[True]`` doesn't silently mean index 1.
    if isinstance(key, bool):
        raise TypeError(
            f"{obj_name} key must be int or str, got bool"
        )
    if isinstance(key, int):
        i = <int>key
        n = count_fn(handle)
        if i < 0 or i >= n:
            raise IndexError(
                f"{obj_name} index {i} out of range [0, {n})"
            )
        return i
    # numpy integers — duck-typed; importing numpy here would force a
    # hard dependency on numpy in every Cython module, so we test for
    # the ``__index__`` protocol instead.
    if hasattr(key, "__index__"):
        i = <int>key.__index__()
        n = count_fn(handle)
        if i < 0 or i >= n:
            raise IndexError(
                f"{obj_name} index {i} out of range [0, {n})"
            )
        return i
    raise TypeError(
        f"{obj_name} key must be int or str, got {type(key).__name__}"
    )
