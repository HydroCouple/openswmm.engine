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
                                     double* x, double* y, double* z)
    int swmm_2d_triangle_get_vertices(void* engine, int idx,
                                       int* v0, int* v1, int* v2)
    int swmm_2d_triangle_get_area(void* engine, int idx, double* area)
    int swmm_2d_triangle_get_centroid(void* engine, int idx,
                                       double* cx, double* cy, double* cz)
    int swmm_2d_triangle_get_mannings(void* engine, int idx, double* n)
    int swmm_2d_triangle_get_neighbours(void* engine, int idx,
                                         int* n0, int* n1, int* n2)

    # Coupling
    int swmm_2d_vertex_coupling_count(void* engine, int* count)
    int swmm_2d_triangle_coupling_count(void* engine, int* count)
    int swmm_2d_vertex_get_coupled_node(void* engine, int vidx, int* nidx)
    int swmm_2d_triangle_get_coupled_node(void* engine, int tidx, int* nidx)

    # State — per triangle
    int swmm_2d_get_depth(void* engine, int idx, double* depth)
    int swmm_2d_get_head(void* engine, int idx, double* head)
    int swmm_2d_get_coupling_flux(void* engine, int idx, double* flux)
    int swmm_2d_get_rainfall(void* engine, int idx, double* rainfall)
    int swmm_2d_get_net_source(void* engine, int idx, double* net_source)
    int swmm_2d_get_depths_bulk(void* engine, double* depths)
    int swmm_2d_get_heads_bulk(void* engine, double* heads)
    int swmm_2d_get_coupling_fluxes_bulk(void* engine, double* fluxes)

    # State — per vertex
    int swmm_2d_vertex_get_head(void* engine, int idx, double* head)
    int swmm_2d_vertex_get_heads_bulk(void* engine, double* heads)

    # Statistics
    int swmm_2d_get_max_depth(void* engine, double* max_depth)
    int swmm_2d_get_total_volume(void* engine, double* volume)
    int swmm_2d_get_total_exchange_flow(void* engine, double* flow)
    int swmm_2d_get_cvode_steps(void* engine, long* steps)
    int swmm_2d_get_cvode_last_step(void* engine, double* h_last)
    int swmm_2d_get_stat_max_depths(void* engine, double* max_depths)

    # Forcing
    int swmm_2d_force_rainfall(void* engine, int idx,
                                double value, int mode, int persist)
    int swmm_2d_force_rainfall_uniform(void* engine,
                                        double value, int mode, int persist)
    int swmm_2d_force_coupling_flux(void* engine, int idx,
                                     double value, int mode, int persist)
    int swmm_2d_force_clear_all(void* engine)

    # Options
    int swmm_2d_get_dry_depth(void* engine, double* dry_depth)
    int swmm_2d_set_dry_depth(void* engine, double dry_depth)
    int swmm_2d_get_rel_tolerance(void* engine, double* rtol)
    int swmm_2d_set_rel_tolerance(void* engine, double rtol)
    int swmm_2d_get_abs_tolerance(void* engine, double* atol)
    int swmm_2d_set_abs_tolerance(void* engine, double atol)
