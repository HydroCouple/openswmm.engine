// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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

/** @brief Make the parsed 2D mesh editable without a full initialize().
 *
 *  The mesh-edit/query setters work as soon as the mesh is parsed (OPENED
 *  state) — they do not require the solver to be initialized. Per-edge BC and
 *  conveyance edits additionally need the authored `[2D_BOUNDARY_CONDITIONS]`
 *  / `[2D_EDGE_CONVEYANCE]` rows drained into live storage first; call this
 *  once before editing those so the changes take effect and are written on
 *  save. No-op when already initialized/drained or when no mesh is loaded.
 *  @returns SWMM_OK, or SWMM_ERR_BADPARAM when no 2D mesh is present.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_prepare_for_edit(SWMM_Engine engine);

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

/** @brief Set vertex Z (ground elevation).
 *
 *  Updates `vz[idx]` and recomputes the dependent geometry for every triangle
 *  incident to this vertex: `tri_cz` (centroid Z = mean of vertex Zs) and
 *  `edge_mz` (per-edge midpoint Z) so the solver's bed-elevation references
 *  stay consistent on the next step. XY-derived fields (`tri_area`, `tri_cx`,
 *  `tri_cy`, `edge_length`, `edge_nx`, `edge_ny`, `edge_mx`, `edge_my`) are
 *  unaffected.
 *
 *  When called while the engine is RUNNING, the solver state (`head`,
 *  `depth`) is intentionally **not** rewritten — `head` remains the value
 *  the solver is integrating; the implied `depth = head - bed` therefore changes
 *  by the same amount as bed. This is the expected physical semantics
 *  ("raising the bed under water reduces water depth there").
 *
 *  @param idx Vertex index (0-based).
 *  @param z   New ground elevation (project vertical units).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_vertex_z(SWMM_Engine engine, int idx, double z);

/** @brief Set EVERY vertex Z in one call.
 *
 *  Equivalent to calling ::swmm_2d_set_vertex_z for each vertex in turn — the
 *  dependent geometry (`tri_cz`, `edge_mz`) ends up bitwise identical — but the
 *  scalar setter rescans all triangles on every call, so doing that per vertex
 *  is O(nVertices x nTriangles). This writes all Zs first and recomputes the
 *  dependent geometry in a single pass, i.e. O(nVertices + nTriangles). On a
 *  million-cell mesh that is the difference between minutes and milliseconds,
 *  which is why editors that rewrite a whole mesh should prefer it.
 *
 *  XY-derived fields (`tri_area`, `tri_cx`, `tri_cy`, `edge_length`,
 *  `edge_nx`, `edge_ny`, `edge_mx`, `edge_my`) are unaffected.
 *
 *  When called while the engine is RUNNING, the solver state (`head`,
 *  `depth`) is intentionally **not** rewritten — `head` remains the value
 *  the solver is integrating; the implied `depth = head - bed` therefore changes
 *  by the same amount as bed. This is the expected physical semantics
 *  ("raising the bed under water reduces water depth there").
 *
 *  @param z     Array of `count` new ground elevations (project vertical
 *               units), indexed by vertex.
 *  @param count Must equal ::swmm_2d_vertex_count, else SWMM_ERR_BADPARAM.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_vertex_z_bulk(SWMM_Engine engine,
                                                const double* z, int count);

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

/** @brief Set triangle Manning's roughness coefficient.
 *
 *  Rejected if not strictly positive (SWMM_ERR_BADPARAM). Persists in the
 *  `MANNINGS_N` column of `[2D_TRIANGLES]` on save.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_triangle_mannings(SWMM_Engine engine, int idx,
                                                    double n);

/** @brief Get triangle initial water depth (`[2D_TRIANGLES]` INIT_DEPTH
 *         column, default 0 = dry). Value is in MESH length units — feet on a
 *         US-FLOW_UNITS project, metres on SI or on a mesh file that declared
 *         `;; UNITS: SI (m)` — the same convention as the vertex Z column.
 *         @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_get_init_depth(SWMM_Engine engine,
                                                    int idx, double* d);

/** @brief Set triangle initial water depth (mesh length units, >= 0;
 *         SWMM_ERR_BADPARAM otherwise — see the getter for the unit
 *         convention). Converted to SI and applied to the solver state when
 *         the 2D surface initializes, and persisted in the `INIT_DEPTH` column
 *         of `[2D_TRIANGLES]` on save (written before TAG). @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_triangle_init_depth(SWMM_Engine engine,
                                                    int idx, double d);

/** @brief Get triangle initial velocity (u, v in m/s; `[2D_INITIAL_VELOCITY]`
 *         rows, default 0,0). @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_get_init_velocity(SWMM_Engine engine,
                                                       int idx, double* u,
                                                       double* v);

/** @brief Set triangle initial velocity (u, v in m/s; finite values;
 *         SWMM_ERR_BADPARAM otherwise). Projected onto the explicit
 *         marcher's face normals as (h*u, h*v) when the 2D surface
 *         initializes (t = 0 only — hotstart/reinitialize still zeroes face
 *         momentum), and persisted as sparse `[2D_INITIAL_VELOCITY]` rows on
 *         save. @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_triangle_init_velocity(SWMM_Engine engine,
                                                       int idx, double u,
                                                       double v);

/** @brief Set the descriptive tag of a vertex (the `[2D_VERTICES]` TAG
 *         column). Distinct from the 1D<->2D coupling node. Empty / NULL
 *         clears it. @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_vertex_tag(SWMM_Engine engine, int idx,
                                             const char* tag);

/** @brief Set the descriptive tag of a triangle (the `[2D_TRIANGLES]` TAG
 *         column, e.g. a region / subcatchment id). Empty / NULL clears it.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_triangle_tag(SWMM_Engine engine, int idx,
                                               const char* tag);

/** @brief Get the descriptive tag of a vertex (the `[2D_VERTICES]` TAG
 *         column). Copies up to `buflen-1` bytes into `buf` and always
 *         NUL-terminates; empty string when the vertex has no tag.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_vertex_tag(SWMM_Engine engine, int idx,
                                           char* buf, int buflen);

/** @brief Get the descriptive tag of a triangle (the `[2D_TRIANGLES]` TAG
 *         column). Copies up to `buflen-1` bytes into `buf` and always
 *         NUL-terminates; empty string when the triangle has no tag.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_triangle_tag(SWMM_Engine engine, int idx,
                                             char* buf, int buflen);

/** @brief Get triangle neighbour indices (-1 = boundary edge).
 *  @param n0,n1,n2 Adjacent triangle indices across edges opposite v0,v1,v2.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_get_neighbours(SWMM_Engine engine, int idx,
                                                      int* n0, int* n1, int* n2);

/** @brief Bulk get edge geometry — time-invariant edge length and outward
 *         unit normal components, indexed `[tri*3 + localEdge]`.
 *
 *  Output arrays must be pre-allocated to `triangle_count * 3` doubles each.
 *  Local edge `e` is the edge opposite vertex `e` (matching the neighbour
 *  convention of `swmm_2d_triangle_get_neighbours`). Outward normals point
 *  away from the triangle interior. Provided so client-side velocity
 *  reconstruction from `swmm_2d_get_edge_flux_bulk` can run without
 *  re-deriving geometry from the vertex coordinates.
 *
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_edge_get_geometry_bulk(SWMM_Engine engine,
                                                     double* length,
                                                     double* nx,
                                                     double* ny);

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

/** @brief Set (or clear) the SWMM node a vertex is coupled to, by name.
 *
 *  Stores the node NAME verbatim and resolves it to an index against the
 *  current model (-1 when not found, mirroring the deferred-resolution rule
 *  in SurfaceRouter2D::initialize). An empty / NULL name clears the coupling
 *  (name cleared, index set to -1). Discharge coefficient and exchange area
 *  keep their existing values (defaults 0.65 / 1.0 for a freshly coupled
 *  vertex); only the node association is changed. The `.inp` writer emits the
 *  name in `[2D_VERTEX_NODE_MAP]`, so this is what persists an interactive
 *  coupling edit on save.
 *
 *  @param vertex_idx Vertex index in `[0, vertex_count)`.
 *  @param node_name  SWMM node id, or "" / NULL to clear.
 *  @return SWMM_OK on success; SWMM_ERR_BADINDEX on out-of-range vertex.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_vertex_coupled_node(SWMM_Engine engine,
                                                      int vertex_idx,
                                                      const char* node_name);

/** @brief Append one node→cell coupling row (`[2D_TRIANGLE_NODE_MAP]`
 *         repeated-row form).
 *
 *  A triangle may carry several coupling rows — one per node — so this
 *  APPENDS rather than overwrites (contrast swmm_2d_set_vertex_coupled_node).
 *  The node NAME is stored verbatim and resolved against the current model
 *  (-1 tolerated until re-resolution, mirroring the vertex setter). The
 *  `.inp` writer emits one `[2D_TRIANGLE_NODE_MAP]` row per coupling.
 *
 *  @param tri_idx   Triangle index in `[0, triangle_count)`.
 *  @param node_name SWMM node id (must be non-empty).
 *  @param cd        Discharge coefficient; must be > 0 (engine default 0.65).
 *  @param area      Effective exchange area, m²; must be > 0.
 *  @return SWMM_OK; SWMM_ERR_BADINDEX on bad triangle; SWMM_ERR_BADPARAM on
 *          empty name / non-positive cd or area.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_add_triangle_coupling(SWMM_Engine engine,
                                                    int tri_idx,
                                                    const char* node_name,
                                                    double cd,
                                                    double area);

/** @brief Remove every authored node→cell coupling row (all triangles).
 *
 *  Also clears the legacy per-triangle mirror (coupled node, CD, AREA back
 *  to defaults). Vertex couplings are untouched. Typical use: the GUI's
 *  Remap 1D↔2D re-authoring the full cell-coupling set.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_clear_triangle_couplings(SWMM_Engine engine);

/** @brief Number of authored node→cell coupling rows.
 *  @param count Output row count (>= number of distinct coupled triangles).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_triangle_coupling_rows(SWMM_Engine engine,
                                                     int* count);

/** @brief Read one authored node→cell coupling row by row index.
 *  @param row_idx   Row index in `[0, swmm_2d_triangle_coupling_rows)`.
 *  @param tri_idx   Output triangle index.
 *  @param node_idx  Output SWMM node index (-1 if unresolved).
 *  @param cd        Output discharge coefficient.
 *  @param area      Output exchange area (m²).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_triangle_coupling_row(SWMM_Engine engine,
                                                        int row_idx,
                                                        int* tri_idx,
                                                        int* node_idx,
                                                        double* cd,
                                                        double* area);

/** @brief Get the coupling discharge coefficient of a vertex
 *         (`[2D_VERTEX_NODE_MAP]` CD column; default 0.65).
 *  @param vertex_idx Vertex index in `[0, vertex_count)`.
 *  @param cd Output discharge coefficient.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_vertex_coupling_cd(SWMM_Engine engine,
                                                     int vertex_idx,
                                                     double* cd);

/** @brief Set the coupling discharge coefficient of a vertex
 *         (`[2D_VERTEX_NODE_MAP]` CD column). Persisted by the `.inp` writer.
 *  @param vertex_idx Vertex index in `[0, vertex_count)`.
 *  @param cd Discharge coefficient; must be > 0.
 *  @return SWMM_OK on success; SWMM_ERR_BADPARAM on non-positive cd.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_vertex_coupling_cd(SWMM_Engine engine,
                                                     int vertex_idx,
                                                     double cd);

/** @brief Get the coupling exchange area of a vertex
 *         (`[2D_VERTEX_NODE_MAP]` AREA column, m²; default 1.0).
 *  @param vertex_idx Vertex index in `[0, vertex_count)`.
 *  @param area Output exchange area (m²).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_vertex_coupling_area(SWMM_Engine engine,
                                                       int vertex_idx,
                                                       double* area);

/** @brief Set the coupling exchange area of a vertex
 *         (`[2D_VERTEX_NODE_MAP]` AREA column, m²). Persisted by the `.inp`
 *         writer.
 *  @param vertex_idx Vertex index in `[0, vertex_count)`.
 *  @param area Exchange area in m²; must be > 0.
 *  @return SWMM_OK on success; SWMM_ERR_BADPARAM on non-positive area.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_vertex_coupling_area(SWMM_Engine engine,
                                                       int vertex_idx,
                                                       double area);

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

/** @brief Bulk get normal edge flux at every edge of every triangle.
 *
 *  Output array must be pre-allocated to `triangle_count * 3` doubles,
 *  indexed `[tri*3 + localEdge]`. Sign convention: **positive flux flows
 *  outward through the edge's outward normal** (positive = leaving the cell).
 *  NOTE: the integrator stores edge_flux INFLOW-positive internally (a positive
 *  value raises the cell depth); this accessor (and the HDF5 `Mesh2_edge_flux`
 *  dataset) flip the sign so the *public* convention is outward-positive as
 *  documented here. Units `m^2 s^-1` (depth-integrated normal speed). Combine
 *  with `swmm_2d_edge_get_geometry_bulk` to reconstruct cell-centred velocity
 *  (RT0): for each triangle, solve `(NᵀN) v = Nᵀ q` where rows of `N` are the
 *  outward unit normals and `q[e] = flux[e] / length[e]`; with the
 *  outward-positive sign this yields the physical (down-gradient) velocity.
 *
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_edge_flux_bulk(SWMM_Engine engine,
                                                 double* flux);

/* =========================================================================
 * 2D Edge Conveyance (§11A of docs/2dModelStrategy.md)
 *
 * Per-edge multiplicative factor in [0, 1] that attenuates the
 * diffusion-wave flux across the edge (default 1.0 = unrestricted, 0.0 =
 * wall). Storage is flat 2D `[tri * 3 + edge_local]`.  Interior edges
 * are symmetric: setting the value on one slot mirrors to the partner
 * slot on the neighbour triangle so mass conservation is preserved.
 *
 * Safe to call between routing steps. Calling during a routing step is
 * undefined — the 2D sub-stepper holds a const reference to the mesh.
 * ========================================================================= */

/** @brief Get the per-edge conveyance factor for one edge.
 *  @param tri  Triangle index in `[0, triangle_count)`.
 *  @param edge Local edge index in `{0, 1, 2}`.
 *  @param conveyance Output value in `[0, 1]`.
 *  @return SWMM_OK on success; SWMM_ERR_BADINDEX on out-of-range tri/edge.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_edge_conveyance(SWMM_Engine engine,
                                                  int tri, int edge,
                                                  double* conveyance);

/** @brief Set the per-edge conveyance factor for one edge.
 *
 *  The value is rejected if outside `[0, 1]` (SWMM_ERR_BADPARAM).  When
 *  the edge is interior (has a neighbour) the value is mirrored to the
 *  partner slot on the neighbour triangle so antisymmetry is preserved.
 *
 *  @param tri  Triangle index in `[0, triangle_count)`.
 *  @param edge Local edge index in `{0, 1, 2}`.
 *  @param conveyance New value in `[0, 1]`.
 *  @return SWMM_OK on success; SWMM_ERR_BADINDEX / SWMM_ERR_BADPARAM on
 *          invalid arguments.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_edge_conveyance(SWMM_Engine engine,
                                                  int tri, int edge,
                                                  double conveyance);

/** @brief Bulk get conveyance factor at every edge of every triangle.
 *
 *  Output array must be pre-allocated to `triangle_count * 3` doubles,
 *  indexed `[tri*3 + edge_local]`.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_edge_conveyance_bulk(SWMM_Engine engine,
                                                       double* conveyance);

/** @brief Reset every edge's conveyance factor to 1.0 (unrestricted).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_reset_edge_conveyance(SWMM_Engine engine);

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

/** @brief Bulk get render-oriented SIGNED vertex depths (m) at all vertices.
 *
 *  The wet-masked, depth-weighted free-surface reconstruction eta_v - z_v:
 *  dry-cell bed elevations never contribute (unlike the vertex heads above,
 *  which are the solver field with dry-cell head = bed elevation). Negative
 *  over the dry side of partially wet cells (sub-cell shoreline intercept),
 *  0 where no incident cell is wet. This is the field GUIs should interpolate
 *  for water-surface rendering and profiles.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_vertex_get_render_depths_bulk(SWMM_Engine engine,
                                                            double* depths);

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

/** @brief Get number of explicit-marcher sub-steps taken in the last advance.
 *  (Renamed from swmm_2d_get_cvode_steps with the D2 CVODE/ARKODE
 *  retirement.)
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_solver_steps(SWMM_Engine engine, long* steps);

/** @brief Get the explicit marcher's last sub-step size (s).
 *  (Renamed from swmm_2d_get_cvode_last_step with the D2 CVODE/ARKODE
 *  retirement.)
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_solver_last_step(SWMM_Engine engine,
                                                   double* h_last);

/** @brief Get per-triangle max depth statistics (cumulative).
 *  @param max_depths Output array (pre-allocated to triangle_count).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_stat_max_depths(SWMM_Engine engine,
                                                  double* max_depths);

/** @brief Get per-triangle max velocity-magnitude statistics (cumulative, m/s).
 *  @param max_velocities Output array (pre-allocated to triangle_count).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_stat_max_velocities(SWMM_Engine engine,
                                                     double* max_velocities);

/** @brief Get per-triangle max |continuity residual| statistics (cumulative, m3/s).
 *  @param max_errs Output array (pre-allocated to triangle_count).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_stat_max_continuity_err(SWMM_Engine engine,
                                                        double* max_errs);

/** @brief Get the global 2D surface continuity error (fraction).
 *  @param err Output: (total_in - total_out) / total_in.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_continuity_error(SWMM_Engine engine, double* err);

/** @brief Get the global 2D mass-balance terms (all m3). Any pointer may be NULL.
 *  @param outfall_out Cumulative 2D→pipe withdrawal at submerged outfalls (m3).
 *  @param evap_out Cumulative evaporation loss from the 2D surface (m3).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_mass_balance(SWMM_Engine engine,
                                             double* init_storage,
                                             double* final_storage,
                                             double* rainfall_in,
                                             double* coupling_1d_to_2d_in,
                                             double* coupling_2d_to_1d_out,
                                             double* outfall_in,
                                             double* outfall_out,
                                             double* boundary_in,
                                             double* boundary_out,
                                             double* evap_out);

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

/** @brief Force evaporation on a specific triangle.
 *
 *  The rate is a demand: wet cells lose depth at this rate, shut off
 *  smoothly as a cell dries (depths never go negative). Default rate is 0
 *  until forced. Negative values are treated as zero (no condensation).
 *
 *  @param idx Triangle index.
 *  @param value Evaporation rate (m/s — same SI convention as rainfall).
 *  @param mode SWMM_FORCING_OVERRIDE or SWMM_FORCING_ADD.
 *  @param persist SWMM_FORCING_RESET or SWMM_FORCING_PERSIST.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_force_evap(SWMM_Engine engine, int idx,
                                         double value, int mode,
                                         int persist);

/** @brief Force evaporation on all triangles (uniform).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_force_evap_uniform(SWMM_Engine engine,
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





/* =========================================================================
 * 2D Boundary Conditions
 * ========================================================================= */

/** Boundary condition type constants.
 *
 *  WALL / NORMAL_FLOW / SPECIFIED_STAGE were the original three; the
 *  SPECIFIED_FLOW (3) and RATING_CURVE (4) values were added per GUI plan
 *  §V V-E4 / V-E5. All five types are enforced at flux time by the
 *  explicit marcher (SurfaceFluxCalculator::boundaryEdgeFlux); timeseries
 *  and rating-curve driving values are re-resolved every step. Note a
 *  NORMAL_FLOW edge with zero bed slope produces zero flux (behaves as a
 *  Wall).
 */
#define SWMM_2D_BC_WALL            0
#define SWMM_2D_BC_NORMAL_FLOW     1
#define SWMM_2D_BC_SPECIFIED_STAGE 2
#define SWMM_2D_BC_SPECIFIED_FLOW  3   /**< V-E4. */
#define SWMM_2D_BC_RATING_CURVE    4   /**< V-E5. */

/** @brief Get the number of boundary edges (edges with no neighbour).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_boundary_edge_count(SWMM_Engine engine, int* count);

/** @brief Get boundary condition type for an edge.
 *  @param tri_idx Triangle index (0-based).
 *  @param edge    Local edge index (0, 1, or 2).
 *  @param bc_type Output: one of the SWMM_2D_BC_* constants.
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

/** @brief Set the timeseries NAME to drive a SPECIFIED_STAGE edge.
 *
 *  V-E2. Stores the name verbatim; the engine resolves it into a table
 *  index from `SimulationContext::tables` on the next forcing-step
 *  lookup (until then `edge_bc_tseries[idx]` is -2). Empty name clears
 *  the slot (back to constant `edge_bc_head`).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_tseries_name(SWMM_Engine engine,
                                                       int tri_idx,
                                                       int edge,
                                                       const char* name);

/** @brief Get the timeseries NAME driving a SPECIFIED_STAGE edge.
 *
 *  SVBC A8 — the setter's mirror, so a caller can verify what the engine
 *  holds after an edit push. Works in the OPENED state (mesh guard, like
 *  the setter). Truncating copy into \p buf, always NUL-terminated.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_tseries_name(SWMM_Engine engine,
                                                       int tri_idx,
                                                       int edge,
                                                       char* buf, int buflen);

/** @brief Get prescribed flow per metre of edge (m³/s/m) for a
 *         SPECIFIED_FLOW edge. V-E4.
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_flow(SWMM_Engine engine,
                                               int tri_idx, int edge,
                                               double* flow);

/** @brief Set prescribed flow per metre of edge (m³/s/m). V-E4. */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_flow(SWMM_Engine engine,
                                               int tri_idx, int edge,
                                               double flow);

/** @brief Set the timeseries NAME to drive a SPECIFIED_FLOW edge.
 *  V-E4 — same resolution contract as `swmm_2d_set_edge_bc_tseries_name`. */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_flow_tseries_name(SWMM_Engine engine,
                                                            int tri_idx,
                                                            int edge,
                                                            const char* name);

/** @brief Get the timeseries NAME driving a SPECIFIED_FLOW edge.
 *  SVBC A8 — same contract as `swmm_2d_get_edge_bc_tseries_name`. */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_flow_tseries_name(SWMM_Engine engine,
                                                            int tri_idx,
                                                            int edge,
                                                            char* buf,
                                                            int buflen);

/** @brief Set the curve NAME to drive a RATING_CURVE edge.
 *
 *  V-E5. Stage → flow lookup is resolved against the existing
 *  `swmm_curve_*` registry on the next forcing-step lookup. Empty name
 *  clears the slot. */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_rating_curve_name(SWMM_Engine engine,
                                                            int tri_idx,
                                                            int edge,
                                                            const char* name);

/** @brief Get the curve NAME driving a RATING_CURVE edge.
 *  SVBC A8 — same contract as `swmm_2d_get_edge_bc_tseries_name`. */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_rating_curve_name(SWMM_Engine engine,
                                                            int tri_idx,
                                                            int edge,
                                                            char* buf,
                                                            int buflen);

/** @brief Get cumulative boundary flux at an edge (m³, + = outflow).
 *  @ingroup engine_2d */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_cum_flux(SWMM_Engine engine,
                                                    int tri_idx, int edge,
                                                    double* cum_flux);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_2D_H */
