# _2d.pxd — Cython declarations for the 2D surface routing C API.
#
# These declarations allow Cython modules to call the C functions in
# openswmm_2d.h without an intermediate Python layer.

cdef extern from "openswmm_2d.h":
    # Status
    int swmm_2d_is_active(void* engine, int* active)

    # Mesh geometry
    int swmm_2d_vertex_count(void* engine, int* count)
    int swmm_2d_triangle_count(void* engine, int* count)
    int swmm_2d_vertex_get_xyz(void* engine, int idx,
                                double* x, double* y, double* z)
    int swmm_2d_vertex_get_xyz_bulk(void* engine,
                                     double* x, double* y, double* z) nogil
    int swmm_2d_set_vertex_z(void* engine, int idx, double z)
    int swmm_2d_prepare_for_edit(void* engine)
    int swmm_2d_triangle_get_vertices(void* engine, int idx,
                                       int* v0, int* v1, int* v2)
    int swmm_2d_triangle_get_area(void* engine, int idx, double* area)
    int swmm_2d_triangle_get_centroid(void* engine, int idx,
                                       double* cx, double* cy, double* cz)
    int swmm_2d_triangle_get_mannings(void* engine, int idx, double* n)
    int swmm_2d_set_triangle_mannings(void* engine, int idx, double n)
    int swmm_2d_set_triangle_tag(void* engine, int idx, const char* tag)
    int swmm_2d_set_vertex_tag(void* engine, int idx, const char* tag)
    int swmm_2d_get_triangle_tag(void* engine, int idx, char* buf, int buflen)
    int swmm_2d_get_vertex_tag(void* engine, int idx, char* buf, int buflen)
    int swmm_2d_triangle_get_neighbours(void* engine, int idx,
                                         int* n0, int* n1, int* n2)
    int swmm_2d_edge_get_geometry_bulk(void* engine,
                                        double* length, double* nx, double* ny) nogil

    # Coupling
    int swmm_2d_vertex_coupling_count(void* engine, int* count)
    int swmm_2d_triangle_coupling_count(void* engine, int* count)
    int swmm_2d_vertex_get_coupled_node(void* engine, int vidx, int* nidx)
    int swmm_2d_set_vertex_coupled_node(void* engine, int vidx, const char* node_name)
    int swmm_2d_get_vertex_coupling_cd(void* engine, int vidx, double* cd)
    int swmm_2d_set_vertex_coupling_cd(void* engine, int vidx, double cd)
    int swmm_2d_get_vertex_coupling_area(void* engine, int vidx, double* area)
    int swmm_2d_set_vertex_coupling_area(void* engine, int vidx, double area)
    int swmm_2d_triangle_get_coupled_node(void* engine, int tidx, int* nidx)
    int swmm_2d_add_triangle_coupling(void* engine, int tidx,
                                       const char* node_name,
                                       double cd, double area)
    int swmm_2d_clear_triangle_couplings(void* engine)
    int swmm_2d_triangle_coupling_rows(void* engine, int* count)
    int swmm_2d_get_triangle_coupling_row(void* engine, int row_idx,
                                           int* tri_idx, int* node_idx,
                                           double* cd, double* area)

    # State — per triangle
    int swmm_2d_get_depth(void* engine, int idx, double* depth)
    int swmm_2d_get_head(void* engine, int idx, double* head)
    int swmm_2d_get_coupling_flux(void* engine, int idx, double* flux)
    int swmm_2d_get_rainfall(void* engine, int idx, double* rainfall)
    int swmm_2d_get_net_source(void* engine, int idx, double* net_source)
    int swmm_2d_get_depths_bulk(void* engine, double* depths) nogil
    int swmm_2d_get_heads_bulk(void* engine, double* heads) nogil
    int swmm_2d_get_coupling_fluxes_bulk(void* engine, double* fluxes) nogil
    int swmm_2d_get_edge_flux_bulk(void* engine, double* flux) nogil

    # State — per vertex
    int swmm_2d_vertex_get_head(void* engine, int idx, double* head)
    int swmm_2d_vertex_get_heads_bulk(void* engine, double* heads) nogil
    int swmm_2d_vertex_get_render_depths_bulk(void* engine, double* depths) nogil

    # Statistics
    int swmm_2d_get_max_depth(void* engine, double* max_depth)
    int swmm_2d_get_total_volume(void* engine, double* volume)
    int swmm_2d_get_total_exchange_flow(void* engine, double* flow)
    int swmm_2d_get_solver_steps(void* engine, long* steps)
    int swmm_2d_get_solver_last_step(void* engine, double* h_last)
    int swmm_2d_get_stat_max_depths(void* engine, double* max_depths)
    int swmm_2d_get_stat_max_velocities(void* engine, double* max_velocities)
    int swmm_2d_get_stat_max_continuity_err(void* engine, double* max_errs)
    int swmm_2d_get_continuity_error(void* engine, double* err)
    int swmm_2d_get_mass_balance(void* engine,
                                 double* init_storage,
                                 double* final_storage,
                                 double* rainfall_in,
                                 double* coupling_1d_to_2d_in,
                                 double* coupling_2d_to_1d_out,
                                 double* outfall_in,
                                 double* outfall_out,
                                 double* boundary_in,
                                 double* boundary_out,
                                 double* evap_out)

    # Forcing
    int swmm_2d_force_rainfall(void* engine, int idx,
                                double value, int mode, int persist)
    int swmm_2d_force_rainfall_uniform(void* engine,
                                        double value, int mode, int persist)
    int swmm_2d_force_evap(void* engine, int idx,
                            double value, int mode, int persist)
    int swmm_2d_force_evap_uniform(void* engine,
                                    double value, int mode, int persist)
    int swmm_2d_force_coupling_flux(void* engine, int idx,
                                     double value, int mode, int persist)
    int swmm_2d_force_clear_all(void* engine)

    # Options
    int swmm_2d_get_dry_depth(void* engine, double* dry_depth)
    int swmm_2d_set_dry_depth(void* engine, double dry_depth)

    # Boundary edges
    int swmm_2d_boundary_edge_count(void* engine, int* count)
    int swmm_2d_get_edge_bc_type(void* engine, int tri_idx, int edge, int* bc_type)
    int swmm_2d_set_edge_bc_type(void* engine, int tri_idx, int edge, int bc_type)
    int swmm_2d_get_edge_bc_head(void* engine, int tri_idx, int edge, double* head)
    int swmm_2d_set_edge_bc_head(void* engine, int tri_idx, int edge, double head)
    int swmm_2d_get_edge_bc_slope(void* engine, int tri_idx, int edge, double* slope)
    int swmm_2d_set_edge_bc_slope(void* engine, int tri_idx, int edge, double slope)
    int swmm_2d_get_edge_bc_cum_flux(void* engine, int tri_idx, int edge, double* cum_flux)

    # SPECIFIED_FLOW (V-E4) / RATING_CURVE (V-E5) edge BC: constant flow per
    # metre of edge, plus timeseries/rating-curve name drivers. Empty name
    # clears the slot. Signatures verified against openswmm_2d.h L460-492.
    int swmm_2d_get_edge_bc_flow(void* engine, int tri_idx, int edge, double* flow)
    int swmm_2d_set_edge_bc_flow(void* engine, int tri_idx, int edge, double flow)
    int swmm_2d_set_edge_bc_tseries_name(void* engine, int tri_idx, int edge, const char* name)
    int swmm_2d_set_edge_bc_flow_tseries_name(void* engine, int tri_idx, int edge, const char* name)
    int swmm_2d_set_edge_bc_rating_curve_name(void* engine, int tri_idx, int edge, const char* name)

    # Edge conveyance factor (§11A) — per-edge [0,1] multiplier on the
    # diffusion-wave flux; default 1.0; setter mirrors to the partner slot.
    int swmm_2d_get_edge_conveyance(void* engine, int tri, int edge, double* conveyance)
    int swmm_2d_set_edge_conveyance(void* engine, int tri, int edge, double conveyance)
    int swmm_2d_get_edge_conveyance_bulk(void* engine, double* conveyance) nogil
    int swmm_2d_reset_edge_conveyance(void* engine)
