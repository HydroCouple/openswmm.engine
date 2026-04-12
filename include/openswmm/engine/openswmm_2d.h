/**
 * @file openswmm_2d.h
 * @brief Optional 2D surface routing module — C API.
 *
 * @details Provides query and control of the optional 2D surface routing
 *          module coupled to the 1D SWMM pipe network. The 2D module is
 *          active when [2D_VERTICES] and [2D_TRIANGLES] sections are present
 *          in the input file and the engine was compiled with OPENSWMM_BUILD_2D.
 *
 *          All functions require the engine to be in SWMM_STATE_RUNNING
 *          unless otherwise noted. Functions return SWMM_ERR_BADPARAM if
 *          the 2D module is not active.
 *
 * @defgroup engine_2d 2D Surface Routing API
 * @ingroup  engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_2D_H
#define OPENSWMM_2D_H

#include "openswmm_callbacks.h"

#ifdef OPENSWMM_ENGINE_STATIC
#  define SWMM_ENGINE_API
#else
#  ifdef _WIN32
#    ifdef openswmm_engine_EXPORTS
#      define SWMM_ENGINE_API __declspec(dllexport)
#    else
#      define SWMM_ENGINE_API __declspec(dllimport)
#    endif
#  else
#    define SWMM_ENGINE_API __attribute__((visibility("default")))
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 2D Module Status
 * ========================================================================= */

/** @brief Check whether the 2D module is active for this simulation.
 *  @param engine Engine handle.
 *  @param active Output: 1 if 2D is active, 0 otherwise.
 *  @returns SWMM_OK or error code.
 *  @note Valid after SWMM_STATE_INITIALIZED.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_is_active(SWMM_Engine engine, int* active);

/* =========================================================================
 * Mesh Geometry — Query (read-only after initialization)
 * ========================================================================= */

/** @brief Get the number of mesh vertices.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_vertex_count(SWMM_Engine engine, int* count);

/** @brief Get the number of mesh triangles.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_count(SWMM_Engine engine, int* count);

/** @brief Get vertex coordinates.
 *  @param idx Vertex index (0-based).
 *  @param x,y,z Output coordinates.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_vertex_get_xyz(SWMM_Engine engine, int idx,
                                             double* x, double* y, double* z);

/** @brief Bulk get vertex coordinates.
 *  @param x,y,z Output arrays (must be pre-allocated to vertex_count).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_vertex_get_xyz_bulk(SWMM_Engine engine,
                                                  double* x, double* y, double* z);

/** @brief Get triangle connectivity (3 vertex indices).
 *  @param idx Triangle index (0-based).
 *  @param v0,v1,v2 Output vertex indices.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_get_vertices(SWMM_Engine engine, int idx,
                                                    int* v0, int* v1, int* v2);

/** @brief Get triangle area.
 *  @param idx Triangle index.
 *  @param area Output area (m² or ft²).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_get_area(SWMM_Engine engine, int idx,
                                                double* area);

/** @brief Get triangle centroid coordinates.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_get_centroid(SWMM_Engine engine, int idx,
                                                    double* cx, double* cy,
                                                    double* cz);

/** @brief Get triangle Manning's roughness.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_get_mannings(SWMM_Engine engine, int idx,
                                                    double* n);

/** @brief Get triangle neighbour indices (-1 = boundary edge).
 *  @param n0,n1,n2 Adjacent triangle indices across edges opposite v0,v1,v2.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_get_neighbours(SWMM_Engine engine, int idx,
                                                      int* n0, int* n1, int* n2);

/* =========================================================================
 * Coupling Map — Query
 * ========================================================================= */

/** @brief Get the number of vertex-to-node coupling points.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_vertex_coupling_count(SWMM_Engine engine, int* count);

/** @brief Get the number of triangle-to-node coupling points.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_coupling_count(SWMM_Engine engine, int* count);

/** @brief Get vertex coupling: which SWMM node is coupled to this vertex.
 *  @param vertex_idx Vertex index.
 *  @param node_idx Output: SWMM node index, or -1 if uncoupled.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_vertex_get_coupled_node(SWMM_Engine engine,
                                                      int vertex_idx,
                                                      int* node_idx);

/** @brief Get triangle coupling: which SWMM node is coupled to this triangle.
 *  @param tri_idx Triangle index.
 *  @param node_idx Output: SWMM node index, or -1 if uncoupled.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_get_coupled_node(SWMM_Engine engine,
                                                        int tri_idx,
                                                        int* node_idx);

/* =========================================================================
 * 2D State — Per-Triangle (read during RUNNING)
 * ========================================================================= */

/** @brief Get water depth at a triangle.
 *  @param idx Triangle index.
 *  @param depth Output depth (m or ft).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_depth(SWMM_Engine engine, int idx, double* depth);

/** @brief Get total head at a triangle (z + depth).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_head(SWMM_Engine engine, int idx, double* head);

/** @brief Get coupling exchange flux at a triangle (m/s, + = into 2D).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_coupling_flux(SWMM_Engine engine, int idx,
                                                double* flux);

/** @brief Get rainfall intensity at a triangle (m/s).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_rainfall(SWMM_Engine engine, int idx,
                                           double* rainfall);

/** @brief Get net source/sink rate at a triangle (m/s).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_net_source(SWMM_Engine engine, int idx,
                                             double* net_source);

/** @brief Bulk get depths for all triangles.
 *  @param depths Output array (pre-allocated to triangle_count).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_depths_bulk(SWMM_Engine engine, double* depths);

/** @brief Bulk get heads for all triangles.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_heads_bulk(SWMM_Engine engine, double* heads);

/** @brief Bulk get coupling fluxes for all triangles.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_coupling_fluxes_bulk(SWMM_Engine engine,
                                                       double* fluxes);

/* =========================================================================
 * 2D State — Per-Vertex (reconstructed heads)
 * ========================================================================= */

/** @brief Get reconstructed head at a vertex.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_vertex_get_head(SWMM_Engine engine, int idx,
                                              double* head);

/** @brief Bulk get reconstructed heads at all vertices.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_vertex_get_heads_bulk(SWMM_Engine engine,
                                                    double* heads);

/* =========================================================================
 * 2D Solver Statistics
 * ========================================================================= */

/** @brief Get the maximum depth across all triangles.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_max_depth(SWMM_Engine engine, double* max_depth);

/** @brief Get total 2D surface volume (sum of depth * area).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_total_volume(SWMM_Engine engine, double* volume);

/** @brief Get total exchange flow rate (sum of all coupling flows, m³/s).
 *  Positive = net flow from 2D into 1D network.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_total_exchange_flow(SWMM_Engine engine,
                                                      double* flow);

/** @brief Get number of CVODE internal steps taken in the last advance.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_cvode_steps(SWMM_Engine engine, long* steps);

/** @brief Get CVODE last internal step size.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_cvode_last_step(SWMM_Engine engine,
                                                  double* h_last);

/** @brief Get per-triangle max depth statistics (cumulative).
 *  @param max_depths Output array (pre-allocated to triangle_count).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_stat_max_depths(SWMM_Engine engine,
                                                  double* max_depths);

/* =========================================================================
 * 2D Forcing — Override rainfall or coupling for external control
 * ========================================================================= */

/** @brief Force rainfall on a specific triangle.
 *  @param idx Triangle index.
 *  @param value Rainfall rate (m/s).
 *  @param mode SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 *  @param persist SWMM_FORCING_RESET or SWMM_FORCING_PERSIST.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_force_rainfall(SWMM_Engine engine, int idx,
                                             double value, int mode,
                                             int persist);

/** @brief Force rainfall on all triangles (uniform).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_force_rainfall_uniform(SWMM_Engine engine,
                                                     double value, int mode,
                                                     int persist);

/** @brief Force coupling flux on a specific triangle (override computed exchange).
 *  @param value Flux rate (m/s, + = into 2D).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_force_coupling_flux(SWMM_Engine engine, int idx,
                                                  double value, int mode,
                                                  int persist);

/** @brief Clear all 2D forcings.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_force_clear_all(SWMM_Engine engine);

/* =========================================================================
 * 2D Solver Options — Query/Modify (valid after INITIALIZED)
 * ========================================================================= */

/** @brief Get the dry depth threshold (m).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_dry_depth(SWMM_Engine engine, double* dry_depth);

/** @brief Set the dry depth threshold (m).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_dry_depth(SWMM_Engine engine, double dry_depth);

/** @brief Get CVODE relative tolerance.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_rel_tolerance(SWMM_Engine engine, double* rtol);

/** @brief Set CVODE relative tolerance.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_rel_tolerance(SWMM_Engine engine, double rtol);

/** @brief Get CVODE absolute tolerance.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_abs_tolerance(SWMM_Engine engine, double* atol);

/** @brief Set CVODE absolute tolerance.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_abs_tolerance(SWMM_Engine engine, double atol);

/* =========================================================================
 * 2D Boundary Conditions
 * ========================================================================= */

/** Boundary condition type constants. */
#define SWMM_2D_BC_WALL            0
#define SWMM_2D_BC_NORMAL_FLOW     1
#define SWMM_2D_BC_SPECIFIED_STAGE 2

/** @brief Get the number of boundary edges (edges with no neighbour).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_boundary_edge_count(SWMM_Engine engine, int* count);

/** @brief Get boundary condition type for an edge.
 *  @param tri_idx Triangle index (0-based).
 *  @param edge    Local edge index (0, 1, or 2).
 *  @param bc_type Output: SWMM_2D_BC_WALL, SWMM_2D_BC_NORMAL_FLOW, or SWMM_2D_BC_SPECIFIED_STAGE.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_type(SWMM_Engine engine,
                                               int tri_idx, int edge,
                                               int* bc_type);

/** @brief Set boundary condition type for an edge.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_type(SWMM_Engine engine,
                                               int tri_idx, int edge,
                                               int bc_type);

/** @brief Get specified stage boundary head for an edge.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_head(SWMM_Engine engine,
                                               int tri_idx, int edge,
                                               double* head);

/** @brief Set specified stage boundary head for an edge.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_head(SWMM_Engine engine,
                                               int tri_idx, int edge,
                                               double head);

/** @brief Get normal flow boundary slope for an edge.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_slope(SWMM_Engine engine,
                                                int tri_idx, int edge,
                                                double* slope);

/** @brief Set normal flow boundary slope for an edge.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_slope(SWMM_Engine engine,
                                                int tri_idx, int edge,
                                                double slope);

/** @brief Get cumulative boundary flux at an edge (m³, + = outflow).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_cum_flux(SWMM_Engine engine,
                                                    int tri_idx, int edge,
                                                    double* cum_flux);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_2D_H */
