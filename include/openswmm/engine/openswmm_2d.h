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
 * @file      openswmm_2d.h
 * @brief     Optional 2D surface routing module — C API.
 *
 * @details Provides query and control of the optional 2D surface routing
 *          module coupled to the 1D SWMM pipe network. The 2D module is
 *          active when [2D_VERTICES] and [2D_TRIANGLES] sections are present
 *          in the input file and the engine was compiled with OPENSWMM_BUILD_2D.
 *
 *          All functions require the engine to be in SWMM_STATE_RUNNING
 *          unless otherwise noted. Solver-state functions return
 *          SWMM_ERR_LIFECYCLE when the model has a 2D mesh but
 *          swmm_engine_initialize() has not run yet, and SWMM_ERR_BADPARAM
 *          when the model has no active 2D surface (no mesh, or IGNORE_2D).
 *
 *          Units: swmm_engine_initialize() scales the mesh to SI, so solver
 *          state (depths, heads, coordinates, velocities, volumes) is in
 *          metres whatever the project's FLOW_UNITS. Mesh values read before
 *          initialize are in the mesh's authored units.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_2d 2D Surface Routing API
 * @ingroup  engine_api
 */

#ifndef OPENSWMM_2D_H
#define OPENSWMM_2D_H

#include "openswmm_callbacks.h"

#ifdef OPENSWMM_ENGINE_STATIC
#define SWMM_ENGINE_API
#else
#ifdef _WIN32
#ifdef openswmm_engine_EXPORTS
#define SWMM_ENGINE_API __declspec(dllexport)
#else
#define SWMM_ENGINE_API __declspec(dllimport)
#endif
#else
#define SWMM_ENGINE_API __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 2D Module Status
 * ========================================================================= */

/**
 * @brief Check whether the 2D module is active for this simulation.
 *
 *
 *
 *
 *  @returns SWMM_OK or error code.
 *  @note Available before initialization; inactive runtime reports zero.
 *  @ingroup engine_2d
 *
 * @param[out] active Required result slot, 1 for an active surface router or 0 otherwise.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_is_active(SWMM_Engine engine, int* active);

/**
 * @brief Resolve an unoverridden AUTO backend to the built-in CPU marcher for this
 * session. Valid in BUILDING or OPENED state, including models without a mesh.
 * enabled must be 0 or 1. Does not edit BACKEND, serialize a model option or
 * change checkpoint identity. Explicit BACKEND and OPENSWMM_2D_BACKEND requests
 * retain precedence. Reapply after opening a new model.
 *
 * @param[in] enabled Exactly 0 to disable or 1 to enable the session preference.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_auto_backend_cpu(SWMM_Engine engine, int enabled);

/**
 * @brief Make the parsed 2D mesh editable without a full initialize().
 *
 *  The mesh-edit/query setters work as soon as the mesh is parsed (OPENED
 *  state) — they do not require the solver to be initialized. Per-edge BC and
 *  conveyance edits additionally need the authored `[2D_BOUNDARY_CONDITIONS]`
 *  / `[2D_EDGE_CONVEYANCE]` rows drained into live storage first; call this
 *  once before editing those so the changes take effect and are written on
 *  save. No-op when already initialized/drained; a missing mesh is refused.
 *  @returns SWMM_OK, or SWMM_ERR_BADPARAM when no 2D mesh is present.
 *  @ingroup engine_2d
 *
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_prepare_for_edit(SWMM_Engine engine);

/* =========================================================================
 * Mesh Geometry — Query (read-only after initialization)
 * ========================================================================= */

/**
 * @brief Multiplier converting the current mesh XYZ coordinates and lengths to
 * metres (square it for areas). Available on a parsed mesh before initialize.
 * Accounts for project units, external mesh SI declarations and initialization's
 * in-place SI conversion. Does not initialize, normalize or change the mesh.
 *
 * @param[out] scale Required result slot, 1.0 or 0.3048; square for area conversion.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_geometry_scale_to_si(SWMM_Engine engine, double* scale);

/**
 * @brief Get the number of mesh vertices.
 *  @ingroup engine_2d
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_vertex_count(SWMM_Engine engine, int* count);

/**
 * @brief Get the number of mesh triangles.
 *  @ingroup engine_2d
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_count(SWMM_Engine engine, int* count);

/**
 * @brief Get vertex coordinates.
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] x Required coordinate destination in the current mesh length units (metres after
 * initialization).
 * @param[out] y Required coordinate destination in the current mesh length units (metres after
 * initialization).
 * @param[out] z Required coordinate destination in the current mesh length units (metres after
 * initialization).
 * @param[in] idx Zero-based vertex index in [0, vertex_count).
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_vertex_get_xyz(SWMM_Engine engine, int idx, double* x, double* y,
                                           double* z);

/**
 * @brief Bulk get vertex coordinates.
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] x Required vertex_count-element array in the current mesh length units (metres
 * after initialization).
 * @param[out] y Required vertex_count-element array in the current mesh length units (metres
 * after initialization).
 * @param[out] z Required vertex_count-element array in the current mesh length units (metres
 * after initialization).
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_vertex_get_xyz_bulk(SWMM_Engine engine, double* x, double* y,
                                                double* z);

/**
 * @brief Set vertex Z (ground elevation).
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
 *  the solver is integrating; the geometric difference `head - bed` changes, while the stored depth
 *  remains untouched until subsequent solver processing.
 *
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[in] idx Zero-based vertex index in [0, vertex_count).
 * @param[in] z Ground elevation in current mesh length units; no finite/range validation is
 * performed.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_vertex_z(SWMM_Engine engine, int idx, double z);

/**
 * @brief Set EVERY vertex Z in one call.
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
 *  the solver is integrating; the geometric difference `head - bed` changes, while the stored depth
 *  remains untouched until subsequent solver processing.
 *
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[in] z Required readable vertex_count-double elevation array in current mesh length units.
 * @param[in] count Exactly vertex_count; a different count returns SWMM_ERR_BADPARAM.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_vertex_z_bulk(SWMM_Engine engine, const double* z, int count);

/**
 * @brief Get triangle connectivity (3 vertex indices).
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] v0 Required zero-based connectivity index destination.
 * @param[out] v1 Required zero-based connectivity index destination.
 * @param[out] v2 Required zero-based connectivity index destination.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_get_vertices(SWMM_Engine engine, int idx, int* v0, int* v1,
                                                  int* v2);

/* -------------------------------------------------------------------------
 * Mixed triangle / quadrilateral meshes (2D_TRI_QUAD_MESH_PLAN_2026-09-06).
 *
 * A "triangle index" everywhere in this header is a CELL index: cells are
 * numbered triangles first (the [2D_TRIANGLES] rows), then quads (the
 * [2D_QUADS] rows). Every swmm_2d_triangle_* accessor that reads a per-cell
 * scalar (area, centroid, Manning, init depth/velocity, tag, coupling) works
 * on a quad too; only the two 3-slot connectivity getters refuse quads.
 * Local edge k of a cell has endpoints v[(k+1)%nv], v[(k+2)%nv].
 * ------------------------------------------------------------------------- */

/**
 * @brief Number of cells (triangles + quads) — same value as
 *  ::swmm_2d_triangle_count.
 *  @ingroup engine_2d
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_cell_count(SWMM_Engine engine, int* count);

/**
 * @brief Number of quadrilateral cells (0 for an all-triangle mesh).
 *  @ingroup engine_2d
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_quad_count(SWMM_Engine engine, int* count);

/**
 * @brief Edge-slot stride of the bulk edge arrays
 *  (::swmm_2d_get_edge_flux_bulk, ::swmm_2d_edge_get_geometry_bulk,
 *  ::swmm_2d_get_edge_conveyance_bulk): 3 for an all-triangle mesh (the
 *  historical `[tri*3 + localEdge]` layout, byte-compatible), 4 once the mesh
 *  holds any quad (`[cell*4 + localEdge]`, padding slots of a triangle row
 *  are 0). Size every bulk buffer as `triangle_count * stride`.
 *  @ingroup engine_2d
 *
 * @param[out] stride Required destination, 3 for triangle-only meshes or 4 when any quad exists.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_edge_stride(SWMM_Engine engine, int* stride);

/**
 * @brief Vertices (== edges) of a cell: 3 or 4.
 *  @ingroup engine_2d
 *
 * @param[out] nv Required result slot, 3 or 4.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_cell_vertex_count(SWMM_Engine engine, int idx, int* nv);

/**
 * @brief Cell connectivity for any shape.
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] nv Required result slot, 3 or 4.
 * @param[out] v Required writable int[4]; unused entries are -1.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_cell_get_vertices(SWMM_Engine engine, int idx, int* v, int* nv);

/**
 * @brief Neighbour cell across each local edge, any shape.
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] nv Required result slot, 3 or 4.
 * @param[out] n Required writable int[4]; -1 marks an exterior edge and -2 padding.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_cell_get_neighbours(SWMM_Engine engine, int idx, int* n, int* nv);

/**
 * @brief Get triangle area.
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] area Required area destination in m2; active runtime geometry has been converted
 * to SI.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_get_area(SWMM_Engine engine, int idx, double* area);

/**
 * @brief Get triangle centroid coordinates.
 *  @ingroup engine_2d
 *
 * @param[out] cx Required centroid-coordinate destination in metres SI.
 * @param[out] cy Required centroid-coordinate destination in metres SI.
 * @param[out] cz Required centroid-coordinate destination in metres SI.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_get_centroid(SWMM_Engine engine, int idx, double* cx,
                                                  double* cy, double* cz);

/**
 * @brief Get triangle Manning's roughness.
 *  @ingroup engine_2d
 *
 * @param[out] n Required Manning roughness destination, s/m^(1/3).
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_get_mannings(SWMM_Engine engine, int idx, double* n);

/**
 * @brief Set triangle Manning's roughness coefficient.
 *
 *  Rejected if not strictly positive (SWMM_ERR_BADPARAM). Persists in the
 *  `MANNINGS_N` column of `[2D_TRIANGLES]` on save.
 *  @ingroup engine_2d
 *
 * @param[in] n Strictly positive Manning roughness, s/m^(1/3).
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_triangle_mannings(SWMM_Engine engine, int idx, double n);

/**
 * @brief Get triangle initial water depth (`[2D_TRIANGLES]` INIT_DEPTH
 *         column, default 0 = dry). Value is in MESH length units — feet on a
 *         US-FLOW_UNITS project, metres on SI or on a mesh file that declared
 *         `;; UNITS: SI (m)` — the same convention as the vertex Z column.
 *         @ingroup engine_2d
 *
 * @param[out] d Required initial-depth destination in current mesh length units.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_get_init_depth(SWMM_Engine engine, int idx, double* d);

/**
 * @brief Set triangle initial water depth (mesh length units, >= 0;
 *         SWMM_ERR_BADPARAM otherwise — see the getter for the unit
 *         convention). Converted to SI and applied to the solver state when
 *         the 2D surface initializes, and persisted in the `INIT_DEPTH` column
 *         of `[2D_TRIANGLES]` on save (written before TAG). @ingroup engine_2d
 *
 * @param[in] d Nonnegative initial depth in current mesh length units; only initialization
 * applies it to runtime depth.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_triangle_init_depth(SWMM_Engine engine, int idx, double d);

/**
 * @brief Get triangle initial velocity (u, v in m/s; `[2D_INITIAL_VELOCITY]`
 *         rows, default 0,0). @ingroup engine_2d
 *
 * @param[out] u Required velocity destination in m/s SI.
 * @param[out] v Required velocity destination in m/s SI.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_get_init_velocity(SWMM_Engine engine, int idx, double* u,
                                                       double* v);

/**
 * @brief Set triangle initial velocity (u, v in m/s; finite values;
 *         SWMM_ERR_BADPARAM otherwise). Projected onto the explicit
 *         marcher's face normals as (h*u, h*v) when the 2D surface
 *         initializes (t = 0 only — hotstart/reinitialize still zeroes face
 *         momentum), and persisted as sparse `[2D_INITIAL_VELOCITY]` rows on
 *         save. @ingroup engine_2d
 *
 * @param[in] u Finite initial velocity component in m/s SI.
 * @param[in] v Finite initial velocity component in m/s SI.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_triangle_init_velocity(SWMM_Engine engine, int idx, double u,
                                                       double v);

/**
 * @brief Set the descriptive tag of a vertex (the `[2D_VERTICES]` TAG
 *         column). Distinct from the 1D<->2D coupling node. Empty / NULL
 *         clears it. @ingroup engine_2d
 *
 * @param[in] tag Optional copied null-terminated descriptive tag; NULL or empty clears it.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based vertex index in [0, vertex_count).
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_vertex_tag(SWMM_Engine engine, int idx, const char* tag);

/**
 * @brief Set the descriptive tag of a triangle (the `[2D_TRIANGLES]` TAG
 *         column, e.g. a region / subcatchment id). Empty / NULL clears it.
 *  @ingroup engine_2d
 *
 * @param[in] tag Optional copied null-terminated descriptive tag; NULL or empty clears it.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_triangle_tag(SWMM_Engine engine, int idx, const char* tag);

/**
 * @brief Get the descriptive tag of a vertex (the `[2D_VERTICES]` TAG
 *         column). Copies up to `buflen-1` bytes into `buf` and always
 *         NUL-terminates; empty string when the vertex has no tag.
 *  @ingroup engine_2d
 *
 * @param[out] buf Required writable tag destination, safely truncated with NUL.
 * @param[in] buflen Positive byte capacity including the terminator.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based vertex index in [0, vertex_count).
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_vertex_tag(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Get the descriptive tag of a triangle (the `[2D_TRIANGLES]` TAG
 *         column). Copies up to `buflen-1` bytes into `buf` and always
 *         NUL-terminates; empty string when the triangle has no tag.
 *  @ingroup engine_2d
 *
 * @param[out] buf Required writable tag destination, safely truncated with NUL.
 * @param[in] buflen Positive byte capacity including the terminator.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_triangle_tag(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Get triangle neighbour indices (-1 = boundary edge).
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] n0 Required adjacent-cell destination across the corresponding local edge; -1
 * means exterior.
 * @param[out] n1 Required adjacent-cell destination across the corresponding local edge; -1
 * means exterior.
 * @param[out] n2 Required adjacent-cell destination across the corresponding local edge; -1
 * means exterior.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_get_neighbours(SWMM_Engine engine, int idx, int* n0, int* n1,
                                                    int* n2);

/**
 * @brief Bulk get edge geometry — time-invariant edge length and outward
 *         unit normal components, indexed `[cell*edge_stride + localEdge]`.
 *
 *  Output arrays must be pre-allocated to `cell_count * edge_stride` doubles each.
 *  Local edge `e` is the edge opposite vertex `e` (matching the neighbour
 *  convention of `swmm_2d_triangle_get_neighbours`). Outward normals point
 *  away from the triangle interior. Provided so client-side velocity
 *  reconstruction from `swmm_2d_get_edge_flux_bulk` can run without
 *  re-deriving geometry from the vertex coordinates.
 *
 *  @ingroup engine_2d
 *
 * @param[out] length Required cell_count * edge_stride array of edge lengths in metres.
 * @param[out] nx Required cell_count * edge_stride array of dimensionless outward unit-normal X
 * components.
 * @param[out] ny Required cell_count * edge_stride array of dimensionless outward unit-normal Y
 * components.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_edge_get_geometry_bulk(SWMM_Engine engine, double* length, double* nx,
                                                   double* ny);

/* =========================================================================
 * Coupling Map — Query
 * ========================================================================= */

/**
 * @brief Get the number of vertex-to-node coupling points.
 *  @ingroup engine_2d
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_vertex_coupling_count(SWMM_Engine engine, int* count);

/**
 * @brief Get the number of triangle-to-node coupling points.
 *  @ingroup engine_2d
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_coupling_count(SWMM_Engine engine, int* count);

/**
 * @brief Get vertex coupling: which SWMM node is coupled to this vertex.
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] node_idx Required resolved zero-based node index; -1 means uncoupled or unresolved.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] vertex_idx Zero-based vertex index in [0, vertex_count).
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_vertex_get_coupled_node(SWMM_Engine engine, int vertex_idx,
                                                    int* node_idx);

/**
 * @brief Get triangle coupling: which SWMM node is coupled to this triangle.
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] node_idx Required resolved zero-based node index; -1 means uncoupled or unresolved.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_get_coupled_node(SWMM_Engine engine, int tri_idx,
                                                      int* node_idx);

/**
 * @brief Set (or clear) the SWMM node a vertex is coupled to, by name.
 *
 *  Stores the node NAME verbatim and resolves it to an index against the
 *  current model (-1 when not found, mirroring the deferred-resolution rule
 *  in SurfaceRouter2D::initialize). An empty / NULL name clears the coupling
 *  (name cleared, index set to -1). An assigned nonempty name preserves existing discharge
 * coefficient and exchange
 *  area; clearing the row restores defaults 0.65 / 1.0 and clears their authored flags. The
 * `.inp` writer emits the
 *  name in `[2D_VERTEX_NODE_MAP]`, so this is what persists an interactive
 *  coupling edit on save.
 *
 *
 *
 *
 *
 *  @return SWMM_OK on success; SWMM_ERR_BADINDEX on out-of-range vertex.
 *  @ingroup engine_2d
 *
 * @param[in] node_name Optional copied node name; NULL or empty clears the row and restores
 * default CD/AREA.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] vertex_idx Zero-based vertex index in [0, vertex_count).
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_vertex_coupled_node(SWMM_Engine engine, int vertex_idx,
                                                    const char* node_name);

/**
 * @brief Append one node→cell coupling row (`[2D_TRIANGLE_NODE_MAP]`
 *         repeated-row form).
 *
 *  A triangle may carry several coupling rows — one per node — so this
 *  APPENDS rather than overwrites (contrast swmm_2d_set_vertex_coupled_node).
 *  The node NAME is stored verbatim and resolved against the current model
 *  (-1 tolerated until re-resolution, mirroring the vertex setter). The
 *  `.inp` writer emits one `[2D_TRIANGLE_NODE_MAP]` row per coupling.
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *  @return SWMM_OK; SWMM_ERR_BADINDEX on bad triangle; SWMM_ERR_BADPARAM on
 *          empty name / a non-positive (non-NaN) cd or area.
 *  @ingroup engine_2d
 *
 * @param[in] node_name Required nonempty copied node identifier; an unknown name is deferred.
 * @param[in] cd Positive dimensionless discharge coefficient, or NaN for inherited/unset.
 * @param[in] area Positive area in current mesh length units squared, or NaN for inherited/unset.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_add_triangle_coupling(SWMM_Engine engine, int tri_idx,
                                                  const char* node_name, double cd, double area);

/**
 * @brief Remove every authored node→cell coupling row (all triangles).
 *
 *  Also clears the legacy per-triangle mirror (coupled node, CD, AREA back
 *  to defaults). Vertex couplings are untouched. Typical use: the GUI's
 *  Remap 1D↔2D re-authoring the full cell-coupling set.
 *  @ingroup engine_2d
 *
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_clear_triangle_couplings(SWMM_Engine engine);

/**
 * @brief Number of authored node→cell coupling rows.
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_triangle_coupling_rows(SWMM_Engine engine, int* count);

/**
 * @brief Read one authored node→cell coupling row by row index.
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[in] row_idx Zero-based authored coupling-row index.
 * @param[out] tri_idx Required zero-based coupled-cell destination.
 * @param[out] node_idx Required resolved node-index destination, -1 if unresolved.
 * @param[out] cd Required stored dimensionless discharge-coefficient destination.
 * @param[out] area Required stored exchange-area destination in current mesh length units squared.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_triangle_coupling_row(SWMM_Engine engine, int row_idx, int* tri_idx,
                                                      int* node_idx, double* cd, double* area);

/**
 * @brief Get the coupling discharge coefficient of a vertex
 *         (`[2D_VERTEX_NODE_MAP]` CD column; default 0.65).
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] cd Required destination for the stored dimensionless discharge coefficient.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] vertex_idx Zero-based vertex index in [0, vertex_count).
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_vertex_coupling_cd(SWMM_Engine engine, int vertex_idx, double* cd);

/**
 * @brief Set the coupling discharge coefficient of a vertex
 *         (`[2D_VERTEX_NODE_MAP]` CD column). Persisted by the `.inp` writer.
 *
 *
 *
 *
 *  @return SWMM_OK on success; SWMM_ERR_BADPARAM on non-positive cd.
 *  @ingroup engine_2d
 *
 * @param[in] cd Strictly positive dimensionless discharge coefficient.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] vertex_idx Zero-based vertex index in [0, vertex_count).
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_vertex_coupling_cd(SWMM_Engine engine, int vertex_idx, double cd);

/**
 * @brief Get the coupling exchange area of a vertex
 *         (`[2D_VERTEX_NODE_MAP]` AREA column; default 1.0).
 *
 *  Units follow the MESH: mesh length units squared as authored (ft² for a
 *  US-FLOW_UNITS project unless the mesh file declares `;; UNITS: SI (m)`),
 *  converted to m² in place by swmm_engine_initialize() together with the
 *  vertex coordinates. Before initialize the value is therefore in mesh
 *  (project) units; after it, m².
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] area Required destination for the stored exchange area in current mesh length
 * units squared.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] vertex_idx Zero-based vertex index in [0, vertex_count).
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_vertex_coupling_area(SWMM_Engine engine, int vertex_idx,
                                                     double* area);

/**
 * @brief Set the coupling exchange area of a vertex
 *         (`[2D_VERTEX_NODE_MAP]` AREA column). Persisted by the `.inp`
 *         writer. Same units convention as the getter: mesh length units²
 *         (project units before initialize, m² after).
 *
 *
 *
 *
 *  @return SWMM_OK on success; SWMM_ERR_BADPARAM on non-positive area.
 *  @ingroup engine_2d
 *
 * @param[in] area Strictly positive exchange area in current mesh length units squared.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] vertex_idx Zero-based vertex index in [0, vertex_count).
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_vertex_coupling_area(SWMM_Engine engine, int vertex_idx,
                                                     double area);

/* =========================================================================
 * 2D State — Per-Triangle (read during RUNNING)
 * ========================================================================= */

/**
 * @brief Get water depth at a triangle.
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] depth Required destination for water depth in m.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_depth(SWMM_Engine engine, int idx, double* depth);

/**
 * @brief Get total head at a triangle (z + depth, m).
 *  @ingroup engine_2d
 *
 * @param[out] head Required destination for water-surface head in m.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_head(SWMM_Engine engine, int idx, double* head);

/**
 * @brief Get coupling exchange flux at a triangle (m/s, + = into 2D).
 *  @ingroup engine_2d
 *
 * @param[out] flux Required destination for coupling flux in m/s, positive into 2D.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_coupling_flux(SWMM_Engine engine, int idx, double* flux);

/**
 * @brief Get rainfall intensity at a triangle (m/s).
 *  @ingroup engine_2d
 *
 * @param[out] rainfall Required destination for applied rainfall rate in m/s.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_rainfall(SWMM_Engine engine, int idx, double* rainfall);

/**
 * @brief Inspect how RAINFALL_MODE maps the gages onto one triangle.
 *
 *  Diagnoses a cell that receives no rain: a zero from a dry contributing
 *  gage is a valid reading, not a missing weight.
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] method Required method-code destination: 0 natural neighbour, 1 IDW fallback, 2
 * nearest, or -1 without per-cell weights.
 * @param[out] gage_indices Writable capacity-int array; may be NULL only when capacity is zero.
 * @param[out] weights Writable capacity-double array of matching normalized weights; NULL only
 * for zero capacity.
 * @param[in] capacity Nonnegative array capacity; only min(capacity, *count) rows are copied.
 * @param[out] count Required total contributor count, possibly greater than capacity.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_rainfall_weights(SWMM_Engine engine, int idx, int* method,
                                                 int* gage_indices, double* weights, int capacity,
                                                 int* count);

/**
 * @brief Get net source/sink rate at a triangle (m/s).
 *  @ingroup engine_2d
 *
 * @param[out] net_source Required destination for net source rate in m/s.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_net_source(SWMM_Engine engine, int idx, double* net_source);

/**
 * @brief Bulk get depths for all triangles (m).
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] depths Required caller-owned cell_count-double array receiving depth in m. No
 * capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_depths_bulk(SWMM_Engine engine, double* depths);

/**
 * @brief Bulk get heads for all triangles (m).
 *  @ingroup engine_2d
 *
 * @param[out] heads Required caller-owned cell_count-double array receiving head in m. No
 * capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_heads_bulk(SWMM_Engine engine, double* heads);

/**
 * @brief Bulk cell velocity in physical Cartesian SI m/s, matching Mesh2_face_vx/vy.
 *
 * FULL_SWE uses accepted conserved cell momentum; other closures use the
 * engine's diagnostic reconstruction. Refreshes derived output fields when
 * stale, without advancing hydraulics. Dry cells have zero velocity.
 * Non-FULL_SWE velocities use wetted-edge normal velocities. Paired NaNs
 * mean a wet cell has no determinate velocity; dry cells return zero.
 * Callers must not substitute zero or a mean-depth reconstruction for NaNs.
 * @param[in] engine Borrowed live engine handle. Serialize with stepping/edits.
 * @param[out] vx Required distinct caller-owned cell_count-double X array.
 * @param[out] vy Required distinct caller-owned cell_count-double Y array.
 * No capacity is passed or checked.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null handle;
 * SWMM_ERR_BADPARAM for null arrays or inactive/no-mesh state;
 * SWMM_ERR_LIFECYCLE for a parsed mesh before initialization.
 */
SWMM_ENGINE_API int swmm_2d_get_velocities_bulk(SWMM_Engine engine, double* vx, double* vy);

/**
 * @brief Bulk get coupling fluxes for all triangles.
 *  @ingroup engine_2d
 *
 * @param[out] fluxes Required caller-owned cell_count-double array receiving flux in m/s,
 * positive into 2D. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_coupling_fluxes_bulk(SWMM_Engine engine, double* fluxes);

/**
 * @brief Bulk get the rainfall intensity (m/s) applied to every triangle
 *  over the solver's current routing window. Output pre-allocated to
 *  `triangle_count`.
 *  @ingroup engine_2d
 *
 * @param[out] rainfall Required caller-owned cell_count-double array receiving applied rainfall
 * rate in m/s. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_rainfall_bulk(SWMM_Engine engine, double* rainfall);

/**
 * @brief Bulk rainfall at a report instant, in SI m/s.
 *
 * Uses the same gage record conversion and time selection as saved 2D
 * rainfall reports. The solver's applied window mean remains available from
 * swmm_2d_get_rainfall_bulk().
 *
 *
 *
 *
 * @ingroup engine_2d
 *
 * @param[out] rainfall Required caller-owned cell_count-double array receiving report rainfall
 * in m/s. No capacity is passed or checked.
 * @param[in] report_date Finite absolute SWMM DateTime, in decimal days.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_report_rainfall_bulk(SWMM_Engine engine, double report_date,
                                                     double* rainfall);

/**
 * @brief Bulk get the cumulative rainfall VOLUME (m³) booked onto every
 *  triangle since the start of the run — the `Mesh2_face_rain_cum` field;
 *  sums over cells to the 2D mass balance's rainfall inflow. Output
 *  pre-allocated to `triangle_count`.
 *  @ingroup engine_2d
 *
 * @param[out] volumes Required caller-owned cell_count-double array receiving cumulative
 * rainfall volume in m3. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_rain_volume_bulk(SWMM_Engine engine, double* volumes);

/**
 * @brief Bulk get the SIGNED cumulative 1D↔2D coupling exchange VOLUME (m³)
 *  per triangle since the start of the run.
 *
 *  Sign is from the 2D cell's point of view: **positive = the 1D node
 *  delivered water INTO this cell** (spill, submerged-outfall discharge);
 *  **negative = the node abstracted water FROM it** (drain). Output
 *  pre-allocated to `triangle_count`.
 *
 *  Summing the array gives `coupling_1d_to_2d_in − coupling_2d_to_1d_out`
 *  from the 2D mass balance — the identity that ties this per-cell series to
 *  the domain totals, and the natural check for a validator.
 *
 *  Why signed and per cell: the two domain accumulators are unsigned and
 *  cannot answer "what did THIS cell give or take", which is exactly the
 *  question at a coupling point that reverses within a batch — the two
 *  directions cancel in the net and neither is visible in the totals.
 *  `swmm_2d_get_coupling_flux` is a rate that is overwritten every step.
 *  @ingroup engine_2d
 *
 * @param[out] volumes Required caller-owned cell_count-double array receiving signed cumulative
 * coupling volume in m3, positive into 2D. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_coupling_volume_bulk(SWMM_Engine engine, double* volumes);

/**
 * @brief Bulk get normal edge flux at every edge of every triangle.
 *
 *  Output array must be pre-allocated to `cell_count * edge_stride` doubles,
 *  indexed `[cell*edge_stride + localEdge]`. Sign convention: **positive flux flows
 *  outward through the edge's outward normal** (positive = leaving the cell).
 *  NOTE: the integrator stores edge_flux INFLOW-positive internally (a positive
 *  value raises the cell depth); this accessor (and the HDF5 `Mesh2_edge_flux`
 *  dataset) flip the sign so the *public* convention is outward-positive as
 *  documented here. Units `m^3 s^-1` (total flow through the edge). Combine
 *  with `swmm_2d_edge_get_geometry_bulk` to reconstruct cell-centred velocity
 *  (RT0): for each triangle, solve `(NᵀN) v = Nᵀ q` where rows of `N` are the
 *  outward unit normals and `q[e] = flux[e] / length[e]`; with the
 *  outward-positive sign this yields the physical (down-gradient) velocity.
 *
 *  @ingroup engine_2d
 *
 * @param[out] flux Required caller-owned cell_count * edge_stride-double array receiving total
 * edge flow in m3/s, outward positive. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_flux_bulk(SWMM_Engine engine, double* flux);

/* ---- Overland transport results (read-only during RUNNING) -------------- */

/**
 * @brief How many species rows the surface transports (0 when it carries
 *  none). Row order: pollutants, MSX species, `__WATER_AGE__`,
 *  `__TEMPERATURE__` — the same order as `swmm_gw2d_species_name`.
 *  @ingroup engine_2d
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_species_count(SWMM_Engine engine, int* count);

/**
 * @brief One surface species row's name.
 *  @ingroup engine_2d
 *
 * @param[in] species Zero-based active transported species row.
 * @param[out] buf Required writable name destination, safely truncated with NUL.
 * @param[in] buflen Positive byte capacity including the terminator.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_species_name(SWMM_Engine engine, int species, char* buf, int buflen);

/**
 * @brief Per-cell concentration of one surface species: pollutant and MSX
 *  rows in their declared units, `__TEMPERATURE__` in degC and
 *  `__WATER_AGE__` in seconds. A cell at or below the dry depth reports 0
 *  (it keeps its mass). Writes at most @p len values and sets @p written.
 *  @ingroup engine_2d
 *
 * @param[in] species Zero-based active transported species row.
 * @param[out] out Required len-double destination; native species units, age seconds and
 * temperature degrees Celsius.
 * @param[in] len Positive capacity in doubles; partial reads are accepted.
 * @param[out] written Optional count destination, min(len, transport cell count).
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_cell_conc(SWMM_Engine engine, int species, double* out, int len,
                                          int* written);

/* =========================================================================
 * 2D Edge Conveyance (§11A of docs/2dModelStrategy.md)
 *
 * Per-edge multiplicative factor in [0, 1] that attenuates the
 * diffusion-wave flux across the edge (default 1.0 = unrestricted, 0.0 =
 * wall). Public storage is `[cell * edge_stride + edge_local]`.  Interior edges
 * are symmetric: setting the value on one slot mirrors to the partner
 * slot on the neighbour triangle so mass conservation is preserved.
 *
 * Safe to call between routing steps. Calling during a routing step is
 * undefined — the 2D sub-stepper holds a const reference to the mesh.
 * ========================================================================= */

/**
 * @brief Get the per-edge conveyance factor for one edge.
 *
 *
 *
 *
 *
 *
 *  @return SWMM_OK on success; SWMM_ERR_BADINDEX on out-of-range tri/edge.
 *  @ingroup engine_2d
 *
 * @param[out] conveyance Required dimensionless edge-factor destination.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_conveyance(SWMM_Engine engine, int tri, int edge,
                                                double* conveyance);

/**
 * @brief Set the per-edge conveyance factor for one edge.
 *
 *  The value is rejected if outside `[0, 1]` (SWMM_ERR_BADPARAM).  When
 *  the edge is interior (has a neighbour) the value is mirrored to the
 *  partner slot on the neighbour triangle so antisymmetry is preserved.
 *
 *
 *
 *
 *
 *
 *
 *  @return SWMM_OK on success; SWMM_ERR_BADINDEX / SWMM_ERR_BADPARAM on
 *          invalid arguments.
 *  @ingroup engine_2d
 *
 * @param[in] conveyance Finite factor in [0,1]; interior partner edge receives the same value.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_edge_conveyance(SWMM_Engine engine, int tri, int edge,
                                                double conveyance);

/**
 * @brief Bulk get conveyance factor at every edge of every triangle.
 *
 *  Output array must be pre-allocated to `cell_count * edge_stride` doubles,
 *  indexed `[tri*3 + edge_local]`.
 *  @ingroup engine_2d
 *
 * @param[out] conveyance Required caller-owned cell_count * edge_stride-double array receiving
 * dimensionless conveyance in [0,1]. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_conveyance_bulk(SWMM_Engine engine, double* conveyance);

/**
 * @brief Reset every edge's conveyance factor to 1.0 (unrestricted).
 *  @ingroup engine_2d
 *
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_reset_edge_conveyance(SWMM_Engine engine);

/* =========================================================================
 * 2D State — Per-Vertex (reconstructed heads)
 * ========================================================================= */

/**
 * @brief Get reconstructed head at a vertex.
 *  @ingroup engine_2d
 *
 * @param[in] idx Zero-based vertex index in [0, vertex_count).
 * @param[out] head Required destination for reconstructed vertex head in m.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_vertex_get_head(SWMM_Engine engine, int idx, double* head);

/**
 * @brief Bulk get reconstructed heads at all vertices.
 *  @ingroup engine_2d
 *
 * @param[out] heads Required caller-owned vertex_count-double array receiving reconstructed
 * vertex head in m. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_vertex_get_heads_bulk(SWMM_Engine engine, double* heads);

/**
 * @brief Bulk get render-oriented SIGNED vertex depths (m) at all vertices.
 *
 *  The wet-masked, depth-weighted free-surface reconstruction eta_v - z_v:
 *  dry-cell bed elevations never contribute (unlike the vertex heads above,
 *  which are the solver field with dry-cell head = bed elevation). Negative
 *  over the dry side of partially wet cells (sub-cell shoreline intercept),
 *  0 where no incident cell is wet. This is the field GUIs should interpolate
 *  for water-surface rendering and profiles.
 *  @ingroup engine_2d
 *
 * @param[out] depths Required caller-owned vertex_count-double array receiving signed render
 * depth in m. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_vertex_get_render_depths_bulk(SWMM_Engine engine, double* depths);

/* =========================================================================
 * 2D Solver Statistics
 * ========================================================================= */

/**
 * @brief Get the maximum depth across all triangles.
 *  @ingroup engine_2d
 *
 * @param[out] max_depth Required destination for maximum cell depth in m.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_max_depth(SWMM_Engine engine, double* max_depth);

/**
 * @brief Get total 2D surface volume (sum of depth * area).
 *  @ingroup engine_2d
 *
 * @param[out] volume Required destination for current surface storage in m3.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_total_volume(SWMM_Engine engine, double* volume);

/**
 * @brief Get total exchange flow rate (sum of all coupling flows, m³/s).
 *  Positive = net flow from 2D into 1D network.
 *  @ingroup engine_2d
 *
 * @param[out] flow Required destination for signed total coupling flow in m3/s, positive into 2D.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_total_exchange_flow(SWMM_Engine engine, double* flow);

/**
 * @brief Get number of explicit-marcher sub-steps taken in the last advance.
 *  (Renamed from swmm_2d_get_cvode_steps with the D2 CVODE/ARKODE
 *  retirement.)
 *  @ingroup engine_2d
 *
 * @param[out] steps Required destination for substep count from the last advance.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_solver_steps(SWMM_Engine engine, long* steps);

/**
 * @brief Get the explicit marcher's last sub-step size (s).
 *  (Renamed from swmm_2d_get_cvode_last_step with the D2 CVODE/ARKODE
 *  retirement.)
 *  @ingroup engine_2d
 *
 * @param[out] h_last Required destination for last marcher substep in seconds.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_solver_last_step(SWMM_Engine engine, double* h_last);

/** @brief Cumulative explicit-marcher run statistics plus the backend and
 *  closure this run is on — the numbers the .rpt "2D Solver Statistics"
 *  block prints, readable DURING the run (after swmm_engine_start).
 *  @ingroup engine_2d */
typedef struct SWMM_2DRunStats {
    char backend[64];       /**< Solver label chosen at initialize, e.g.
                                    "cpu (explicit marcher)", "omp (Kokkos OpenMP …)". */
    int momentum;           /**< 0 LOCAL_INERTIAL, 1 FULL_SWE, 2 DIFFUSIVE_WAVE. */
    int lts_tiers;          /**< Configured [2D_OPTIONS] LTS_TIERS. */
    long steps;             /**< Cumulative internal (marcher) substeps. */
    long face_evals;        /**< Cumulative face-kernel evaluations. */
    double last_step;       /**< Last accepted internal step (s). */
    double active_frac_min; /**< Active-cell fraction over rebuild samples; -1 = not populated. */
    double active_frac_mean;
    double active_frac_max;
    int n_tiers;        /**< Populated entries of tier_cells (0 = no LTS telemetry). */
    long tier_cells[8]; /**< Cumulative rebuild-sampled cells per LTS tier. */
} SWMM_2DRunStats;

/**
 * @brief Read the cumulative marcher statistics and backend of the 2D run.
 *  Counters are zero (and n_tiers 0) before the first advance and again once
 *  the solver has been finalised at swmm_engine_end; backend stays filled.
 *  @ingroup engine_2d
 *
 * @param[out] stats Required caller-owned SWMM_2DRunStats; replaced on success, no struct_size
 * field required.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_run_stats(SWMM_Engine engine, SWMM_2DRunStats* stats);

/**
 * @brief Get per-triangle max depth statistics (cumulative, m).
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] max_depths Required caller-owned cell_count-double array receiving cumulative
 * maximum depth in m. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_stat_max_depths(SWMM_Engine engine, double* max_depths);

/**
 * @brief Get per-triangle max velocity-magnitude statistics (cumulative, m/s).
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] max_velocities Required caller-owned cell_count-double array receiving cumulative
 * maximum speed in m/s. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_stat_max_velocities(SWMM_Engine engine, double* max_velocities);

/**
 * @brief Get per-triangle max |continuity residual| statistics (cumulative, m3/s).
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] max_errs Required caller-owned cell_count-double array receiving cumulative
 * maximum absolute continuity error in m3/s. No capacity is passed or checked.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_stat_max_continuity_err(SWMM_Engine engine, double* max_errs);

/**
 * @brief Get the global 2D surface continuity error (fraction).
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] err Required destination for signed relative water-balance residual, dimensionless.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_continuity_error(SWMM_Engine engine, double* err);

/**
 * @brief Get the global 2D mass-balance terms (all m3). Any pointer may be NULL.
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] init_storage Optional destination for init storage in m3.
 * @param[out] final_storage Optional destination for final storage in m3.
 * @param[out] rainfall_in Optional destination for rainfall in in m3.
 * @param[out] coupling_1d_to_2d_in Optional destination for coupling 1d to 2d in in m3.
 * @param[out] coupling_2d_to_1d_out Optional destination for coupling 2d to 1d out in m3.
 * @param[out] outfall_in Optional destination for outfall in in m3.
 * @param[out] outfall_out Optional destination for outfall out in m3.
 * @param[out] boundary_in Optional destination for boundary in in m3.
 * @param[out] boundary_out Optional destination for boundary out in m3.
 * @param[out] evap_out Optional destination for evap out in m3.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_mass_balance(SWMM_Engine engine, double* init_storage,
                                             double* final_storage, double* rainfall_in,
                                             double* coupling_1d_to_2d_in,
                                             double* coupling_2d_to_1d_out, double* outfall_in,
                                             double* outfall_out, double* boundary_in,
                                             double* boundary_out, double* evap_out);

/* =========================================================================
 * 2D Forcing — Override rainfall or coupling for external control
 * ========================================================================= */

/**
 * @brief Force rainfall on a specific triangle.
 *
 *
 *
 *
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[in] value Finite rate in m/s; coupling is positive into 2D, evaporation is an
 * extraction demand.
 * @param[in] mode SWMM_FORCING_NONE to clear, SWMM_FORCING_OVERRIDE to replace, or
 * SWMM_FORCING_ADD to add.
 * @param[in] persist SWMM_FORCING_RESET for one routing window or SWMM_FORCING_PERSIST until
 * cleared.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_force_rainfall(SWMM_Engine engine, int idx, double value, int mode,
                                           int persist);

/**
 * @brief Force rainfall on all triangles (uniform).
 *  @ingroup engine_2d
 *
 * @param[in] value Finite rate in m/s; coupling is positive into 2D, evaporation is an
 * extraction demand.
 * @param[in] mode SWMM_FORCING_NONE to clear, SWMM_FORCING_OVERRIDE to replace, or
 * SWMM_FORCING_ADD to add.
 * @param[in] persist SWMM_FORCING_RESET for one routing window or SWMM_FORCING_PERSIST until
 * cleared.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_force_rainfall_uniform(SWMM_Engine engine, double value, int mode,
                                                   int persist);

/**
 * @brief Force evaporation on a specific triangle.
 *
 *  The rate is a demand: wet cells lose depth at this rate, shut off
 *  smoothly as a cell dries (depths never go negative). Default rate is 0
 *  until forced. Negative values are treated as zero (no condensation).
 *
 *
 *
 *
 *
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[in] value Finite rate in m/s; coupling is positive into 2D, evaporation is an
 * extraction demand.
 * @param[in] mode SWMM_FORCING_NONE to clear, SWMM_FORCING_OVERRIDE to replace, or
 * SWMM_FORCING_ADD to add.
 * @param[in] persist SWMM_FORCING_RESET for one routing window or SWMM_FORCING_PERSIST until
 * cleared.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_force_evap(SWMM_Engine engine, int idx, double value, int mode,
                                       int persist);

/**
 * @brief Force evaporation on all triangles (uniform).
 *  @ingroup engine_2d
 *
 * @param[in] value Finite rate in m/s; coupling is positive into 2D, evaporation is an
 * extraction demand.
 * @param[in] mode SWMM_FORCING_NONE to clear, SWMM_FORCING_OVERRIDE to replace, or
 * SWMM_FORCING_ADD to add.
 * @param[in] persist SWMM_FORCING_RESET for one routing window or SWMM_FORCING_PERSIST until
 * cleared.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_force_evap_uniform(SWMM_Engine engine, double value, int mode,
                                               int persist);

/**
 * @brief Force coupling flux on a specific triangle (override computed exchange).
 *
 *
 *  @ingroup engine_2d
 *
 * @param[in] value Finite rate in m/s; coupling is positive into 2D, evaporation is an
 * extraction demand.
 * @param[in] mode SWMM_FORCING_NONE to clear, SWMM_FORCING_OVERRIDE to replace, or
 * SWMM_FORCING_ADD to add.
 * @param[in] persist SWMM_FORCING_RESET for one routing window or SWMM_FORCING_PERSIST until
 * cleared.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] idx Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_force_coupling_flux(SWMM_Engine engine, int idx, double value, int mode,
                                                int persist);

/**
 * @brief Clear all 2D forcings.
 *  @ingroup engine_2d
 *
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_force_clear_all(SWMM_Engine engine);

/* =========================================================================
 * 2D Solver Options — Query/Modify (valid after INITIALIZED)
 * ========================================================================= */

/**
 * @brief Get the dry depth threshold (m).
 *  @ingroup engine_2d
 *
 * @param[out] dry_depth Required destination for dry-depth threshold in m.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_dry_depth(SWMM_Engine engine, double* dry_depth);

/**
 * @brief Set the dry depth threshold (m).
 *  @ingroup engine_2d
 *
 * @param[in] dry_depth Dry-depth threshold in m; this low-level setter does not validate range
 * or finiteness.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
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
#define SWMM_2D_BC_WALL 0
#define SWMM_2D_BC_NORMAL_FLOW 1
#define SWMM_2D_BC_SPECIFIED_STAGE 2
#define SWMM_2D_BC_SPECIFIED_FLOW 3 /**< V-E4. */
#define SWMM_2D_BC_RATING_CURVE 4   /**< V-E5. */

/**
 * @brief Get the number of boundary edges (edges with no neighbour).
 *  @ingroup engine_2d
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_boundary_edge_count(SWMM_Engine engine, int* count);

/**
 * @brief Get boundary condition type for an edge.
 *
 *
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[out] bc_type Required destination for SWMM_2D_BC_* code.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_type(SWMM_Engine engine, int tri_idx, int edge,
                                             int* bc_type);

/** Restore an edge's pre-runtime boundary prescription, including its time series.
 * Valid between running steps; cumulative flux accounting is preserved. */
/**
 * @brief Runtime inflow concentrations in the species getter's units/order. Outflow
 * uses donor concentrations. clear_edge_bc restores authored hydraulics/quality.
 *
 * @param[in] values Borrowed species_count-double inflow concentration array; NULL only for
 * zero count. Every value finite; nonsigned species nonnegative.
 * @param[in] species_count Exactly the active transported species count; order/units match
 * swmm_2d_species_name().
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] cell Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @note Boundary storage must be prepared. Edits target exterior unowned edges
 * in OPENED/BUILDING/INITIALIZED/RUNNING. Active edits additionally require an
 * allowed between-step update and the CPU runtime; they synchronize pending work
 * and save the authored prescription for later restoration.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_concentrations(SWMM_Engine engine, int cell, int edge,
                                                       const double* values, int species_count);
/**
 * @brief Restore the authored boundary hydraulics and quality for one edge.
 *
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] cell Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @note Boundary storage must be prepared. Edits target exterior unowned edges
 * in OPENED/BUILDING/INITIALIZED/RUNNING. Active edits additionally require an
 * allowed between-step update and the CPU runtime; they synchronize pending work
 * and save the authored prescription for later restoration.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_clear_edge_bc(SWMM_Engine engine, int cell, int edge);

/* -------------------------------------------------------------------------
 * Authored [2D_INITIAL_QUALITY] and [2D_BOUNDARY_QUALITY] rows
 * (closed C-API gaps 2026-10-11)
 *
 * The rows the deck spells, resolved against the mesh, tags and species at
 * swmm_engine_initialize() and written back on save. Editable while BUILDING
 * or OPENED (SWMM_ERR_LIFECYCLE afterwards); concentrations are in species
 * units, finite and >= 0, as the parser requires. Cell indices are 0-based
 * here (the deck's CELL rows are 1-based). A set with the same key as an
 * existing row replaces it.
 * ------------------------------------------------------------------------- */

#define SWMM_2D_QUAL_SCOPE_GLOBAL 0 /**< the `*` row */
#define SWMM_2D_QUAL_SCOPE_TAG 1    /**< every cell carrying a tag */
#define SWMM_2D_QUAL_SCOPE_CELL 2   /**< one cell */

/**
 * @brief Read the authored initial water-quality row count.
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_initial_quality_count(SWMM_Engine engine, int* count);

/**
 * @brief Read one [2D_INITIAL_QUALITY] row. Any out-pointer may be NULL.
 *
 * @param[out] scope Optional SWMM_2D_QUAL_SCOPE_* destination.
 * @param[out] tag Optional tag destination; positive capacity safely truncates with NUL.
 * @param[in] tag_len Tag capacity in bytes; nonpositive skips copying.
 * @param[out] cell Optional zero-based cell index; -1 for a non-CELL row.
 * @param[out] species Optional species-name destination, safely truncated when capacity is
 * positive.
 * @param[in] species_len Species buffer byte capacity; nonpositive skips copying.
 * @param[out] conc Optional authored concentration in native species units.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] index Zero-based authored row index; removing a row shifts later indices down.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_initial_quality(SWMM_Engine engine, int index, int* scope,
                                                char* tag, int tag_len, int* cell, char* species,
                                                int species_len, double* conc);

/**
 * @brief Add or replace a row keyed by (scope, tag | cell, species).
 *
 * @param[in] scope SWMM_2D_QUAL_SCOPE_GLOBAL, TAG or CELL.
 * @param[in] tag Required nonempty copied tag for TAG scope; ignored otherwise and may be NULL.
 * @param[in] cell Zero-based cell for CELL scope; nonnegative, checked against a present mesh,
 * otherwise deferred.
 * @param[in] species Required nonempty copied species name; resolution deferred to initialize.
 * @param[in] conc Finite nonnegative concentration in native species units.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Configuration edits require BUILDING/OPENED; later states return SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_initial_quality(SWMM_Engine engine, int scope, const char* tag,
                                                int cell, const char* species, double conc);

/**
 * @brief Erase one authored initial water-quality row.
 *
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] index Zero-based authored row index; removing a row shifts later indices down.
 * @note Configuration edits require BUILDING/OPENED; later states return SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_remove_initial_quality(SWMM_Engine engine, int index);

/* Authored [2D_INITIAL_BED_QUALITY] rows seed finite LOCAL_STORAGE bed
 * porewater. Scope constants and lifecycle match [2D_INITIAL_QUALITY]. */
/**
 * @brief Read the authored finite-bed initial-quality row count.
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_initial_bed_quality_count(SWMM_Engine engine, int* count);
/**
 * @brief Read one authored finite-bed initial-quality row.
 *
 * @param[out] scope Optional SWMM_2D_QUAL_SCOPE_* destination.
 * @param[out] tag Optional tag destination; positive capacity safely truncates with NUL.
 * @param[in] tag_len Tag capacity in bytes; nonpositive skips copying.
 * @param[out] cell Optional zero-based cell index; -1 for a non-CELL row.
 * @param[out] species Optional species-name destination, safely truncated when capacity is
 * positive.
 * @param[in] species_len Species buffer byte capacity; nonpositive skips copying.
 * @param[out] conc Optional authored concentration in native species units.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] index Zero-based authored row index; removing a row shifts later indices down.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_initial_bed_quality(SWMM_Engine engine, int index, int* scope,
                                                    char* tag, int tag_len, int* cell,
                                                    char* species, int species_len, double* conc);
/**
 * @brief Upsert a finite-bed initial-quality row by scope and species.
 *
 * @param[in] scope SWMM_2D_QUAL_SCOPE_GLOBAL, TAG or CELL.
 * @param[in] tag Required nonempty copied tag for TAG scope; ignored otherwise and may be NULL.
 * @param[in] cell Zero-based cell for CELL scope; nonnegative, checked against a present mesh,
 * otherwise deferred.
 * @param[in] species Required nonempty copied species name; resolution deferred to initialize.
 * @param[in] conc Finite nonnegative concentration in native species units.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Configuration edits require BUILDING/OPENED; later states return SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_initial_bed_quality(SWMM_Engine engine, int scope, const char* tag,
                                                    int cell, const char* species, double conc);
/**
 * @brief Erase one authored finite-bed initial-quality row.
 *
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] index Zero-based authored row index; removing a row shifts later indices down.
 * @note Configuration edits require BUILDING/OPENED; later states return SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_remove_initial_bed_quality(SWMM_Engine engine, int index);

/**
 * @brief Read the authored boundary-quality row count.
 *
 * @param[out] count Required destination for the current count.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_boundary_quality_count(SWMM_Engine engine, int* count);

/**
 * @brief Read one [2D_BOUNDARY_QUALITY] row (inflow concentration through a
 *         boundary edge). Any out-pointer may be NULL.
 *
 * @param[out] cell Optional zero-based boundary-cell destination.
 * @param[out] edge Optional zero-based local-edge destination.
 * @param[out] species Optional species-name destination, safely truncated when capacity is
 * positive.
 * @param[in] species_len Species-name byte capacity; nonpositive skips the string.
 * @param[out] conc Optional native-species concentration destination.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] index Zero-based authored row index; removing a row shifts later indices down.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_boundary_quality(SWMM_Engine engine, int index, int* cell,
                                                 int* edge, char* species, int species_len,
                                                 double* conc);

/**
 * @brief Add or replace the row keyed by (cell, edge, species).
 *
 * @param[in] species Required nonempty copied species name; resolved at initialize.
 * @param[in] conc Finite nonnegative concentration in native species units.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] cell Zero-based cell index in [0, cell_count); includes triangles and quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Configuration edits require BUILDING/OPENED; later states return SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_boundary_quality(SWMM_Engine engine, int cell, int edge,
                                                 const char* species, double conc);

/**
 * @brief Erase one authored boundary-quality row.
 *
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] index Zero-based authored row index; removing a row shifts later indices down.
 * @note Configuration edits require BUILDING/OPENED; later states return SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_remove_boundary_quality(SWMM_Engine engine, int index);

/**
 * @brief Set boundary condition type for an edge.
 *  @ingroup engine_2d
 *
 * @param[in] bc_type New SWMM_2D_BC_* code.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @note Boundary storage must be prepared. Edits target exterior unowned edges
 * in OPENED/BUILDING/INITIALIZED/RUNNING. Active edits additionally require an
 * allowed between-step update and the CPU runtime; they synchronize pending work
 * and save the authored prescription for later restoration.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_type(SWMM_Engine engine, int tri_idx, int edge,
                                             int bc_type);

/**
 * @brief Get specified stage boundary head for an edge.
 *
 *  Units follow the mesh vertical datum: mesh length units as authored
 *  (feet for a US project without an SI mesh header) before
 *  swmm_engine_initialize(), metres after it.
 *  @ingroup engine_2d
 *
 * @param[out] head Required destination for stage elevation in current boundary/mesh length
 * units, metres after initialization.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_head(SWMM_Engine engine, int tri_idx, int edge,
                                             double* head);

/**
 * @brief Set specified stage boundary head for an edge (same units
 *         convention as the getter).
 *  @ingroup engine_2d
 *
 * @param[in] head Finite new stage elevation in current boundary/mesh length units, metres
 * after initialization.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @note Boundary storage must be prepared. Edits target exterior unowned edges
 * in OPENED/BUILDING/INITIALIZED/RUNNING. Active edits additionally require an
 * allowed between-step update and the CPU runtime; they synchronize pending work
 * and save the authored prescription for later restoration.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_head(SWMM_Engine engine, int tri_idx, int edge,
                                             double head);

/** Author a boundary group token (empty clears), preserving the hydraulic prescription.
 * OPENED/BUILDING only; nonempty names require an exterior edge and cannot
 * contain whitespace or INP delimiters. Groups are organizational metadata.
 */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_group(SWMM_Engine engine, int tri_idx, int edge, const char* group);

/** Tangential WALL traction tau/rho=-Cw|ut|ut. Finite Cw>=0, zero free-slip.
 * Getter uses a valid prepared mesh slot. Setter is authoring-only (OPENED/BUILDING);
 * positive values require an exterior WALL edge. CPU FULL_SWE, LTS_TIERS 1,
 * uncoupled hydraulics required when running. No density or 1/2 in Cw convention.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_wall_drag(SWMM_Engine engine, int tri_idx, int edge, double* cw);
SWMM_ENGINE_API int swmm_2d_set_edge_wall_drag(SWMM_Engine engine, int tri_idx, int edge, double cw);

/**
 * @brief Get normal flow boundary slope for an edge.
 *  @ingroup engine_2d
 *
 * @param[out] slope Required destination for dimensionless nonnegative bed slope.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_slope(SWMM_Engine engine, int tri_idx, int edge,
                                              double* slope);

/**
 * @brief Set normal flow boundary slope for an edge.
 *  @ingroup engine_2d
 *
 * @param[in] slope Finite new dimensionless nonnegative bed slope.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @note Boundary storage must be prepared. Edits target exterior unowned edges
 * in OPENED/BUILDING/INITIALIZED/RUNNING. Active edits additionally require an
 * allowed between-step update and the CPU runtime; they synchronize pending work
 * and save the authored prescription for later restoration.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_slope(SWMM_Engine engine, int tri_idx, int edge,
                                              double slope);

/**
 * @brief Set the timeseries NAME to drive a SPECIFIED_STAGE edge.
 *
 *  V-E2. Stores the name verbatim; the engine resolves it into a table
 *  index from `SimulationContext::tables` on the next forcing-step
 *  lookup (until then `edge_bc_tseries[idx]` is -2). Empty name clears
 *  the slot (back to constant `edge_bc_head`).
 *  @ingroup engine_2d
 *
 * @param[in] name Optional copied table name; NULL or empty clears the binding. Must resolve
 * immediately during an active run; pre-run lookup is deferred.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @note Boundary storage must be prepared. Edits target exterior unowned edges
 * in OPENED/BUILDING/INITIALIZED/RUNNING. Active edits additionally require an
 * allowed between-step update and the CPU runtime; they synchronize pending work
 * and save the authored prescription for later restoration.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_tseries_name(SWMM_Engine engine, int tri_idx, int edge,
                                                     const char* name);

/**
 * @brief Get the timeseries NAME driving a SPECIFIED_STAGE edge.
 *
 *  SVBC A8 — the setter's mirror, so a caller can verify what the engine
 *  holds after an edit push. Works in the OPENED state (mesh guard, like
 *  the setter). Truncating copy into \p buf, always NUL-terminated.
 *  @ingroup engine_2d
 *
 * @param[out] buf Required writable table-name destination, safely truncated with NUL.
 * @param[in] buflen Positive byte capacity including the terminator.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_tseries_name(SWMM_Engine engine, int tri_idx, int edge,
                                                     char* buf, int buflen);

/**
 * @brief Get prescribed flow per metre of edge for a SPECIFIED_FLOW edge.
 *         V-E4.
 *
 *  Units: project display flow units per metre (the `[2D_BOUNDARY_CONDITIONS]`
 *  file contract, e.g. CFS/m) before swmm_engine_initialize(); m³/s/m after
 *  it (initialize scales the constants in place; the writers scale back).
 *  @ingroup engine_2d
 *
 * @param[out] flow Required destination for signed total discharge per edge width, current
 * boundary length units squared per second.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_flow(SWMM_Engine engine, int tri_idx, int edge,
                                             double* flow);

/**
 * @brief Set prescribed flow per metre of edge (same units convention as
 *         the getter: display flow units/m before initialize, m³/s/m after).
 *         V-E4.
 *
 * @param[in] flow Finite new signed total discharge per edge width, current boundary length
 * units squared per second.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @note Boundary storage must be prepared. Edits target exterior unowned edges
 * in OPENED/BUILDING/INITIALIZED/RUNNING. Active edits additionally require an
 * allowed between-step update and the CPU runtime; they synchronize pending work
 * and save the authored prescription for later restoration.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_flow(SWMM_Engine engine, int tri_idx, int edge,
                                             double flow);

/**
 * @brief Set the timeseries NAME to drive a SPECIFIED_FLOW edge.
 *  V-E4 — same resolution contract as `swmm_2d_set_edge_bc_tseries_name`.
 *
 * @param[in] name Optional copied table name; NULL or empty clears the binding. Must resolve
 * immediately during an active run; pre-run lookup is deferred.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @note Boundary storage must be prepared. Edits target exterior unowned edges
 * in OPENED/BUILDING/INITIALIZED/RUNNING. Active edits additionally require an
 * allowed between-step update and the CPU runtime; they synchronize pending work
 * and save the authored prescription for later restoration.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_flow_tseries_name(SWMM_Engine engine, int tri_idx, int edge,
                                                          const char* name);

/**
 * @brief Get the timeseries NAME driving a SPECIFIED_FLOW edge.
 *  SVBC A8 — same contract as `swmm_2d_get_edge_bc_tseries_name`.
 *
 * @param[out] buf Required writable table-name destination, safely truncated with NUL.
 * @param[in] buflen Positive byte capacity including the terminator.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_flow_tseries_name(SWMM_Engine engine, int tri_idx, int edge,
                                                          char* buf, int buflen);

/**
 * @brief Set the curve NAME to drive a RATING_CURVE edge.
 *
 *  V-E5. Stage → flow lookup is resolved against the existing
 *  `swmm_curve_*` registry on the next forcing-step lookup. Empty name
 *  clears the slot.
 *
 * @param[in] name Optional copied table name; NULL or empty clears the binding. Must resolve
 * immediately during an active run; pre-run lookup is deferred.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @note Boundary storage must be prepared. Edits target exterior unowned edges
 * in OPENED/BUILDING/INITIALIZED/RUNNING. Active edits additionally require an
 * allowed between-step update and the CPU runtime; they synchronize pending work
 * and save the authored prescription for later restoration.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_set_edge_bc_rating_curve_name(SWMM_Engine engine, int tri_idx, int edge,
                                                          const char* name);

/**
 * @brief Get the curve NAME driving a RATING_CURVE edge.
 *  SVBC A8 — same contract as `swmm_2d_get_edge_bc_tseries_name`.
 *
 * @param[out] buf Required writable table-name destination, safely truncated with NUL.
 * @param[in] buflen Positive byte capacity including the terminator.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires a parsed mesh; no initialized hydraulic state is needed.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_rating_curve_name(SWMM_Engine engine, int tri_idx, int edge,
                                                          char* buf, int buflen);

/**
 * @brief Get cumulative boundary flux at an edge (m³, + = outflow).
 *  @ingroup engine_2d
 *
 * @param[out] cum_flux Required destination for cumulative boundary volume in m3, positive outward.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @param[in] tri_idx Zero-based cell index in [0, cell_count); includes triangles and
 * quadrilaterals.
 * @param[in] edge Zero-based local edge in [0, cell_vertex_count); exterior edge required for
 * boundary edits.
 * @note Requires an active surface router. A parsed mesh before initialization
 * returns SWMM_ERR_LIFECYCLE; other inactive/no-mesh cases return SWMM_ERR_BADPARAM.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_2d_get_edge_bc_cum_flux(SWMM_Engine engine, int tri_idx, int edge,
                                                 double* cum_flux);

/* =========================================================================
 * Results-file variable selection ([2D_OPTIONS] REPORT_2D_VARIABLES)
 * =========================================================================
 * Engine-independent helpers so a host can build a checklist without
 * re-implementing the token vocabulary. Tokens are listed in bit order; the
 * presets DEFAULT / MINIMAL / ALL map to masks. Use swmm_options_get_ext /
 * swmm_options_set_ext with key "REPORT_2D_VARIABLES" to read/write a model.
 */

/**
 * @brief Number of selectable dataset groups.
 *
 *
 * @return Number of known group tokens.
 */
SWMM_ENGINE_API int swmm_2d_output_variable_count(void);
/**
 * @brief Token of group @p i (0-based, bit i), or "" when out of range.
 *
 * @param[in] i Zero-based dataset-group token index, in bit order.
 * @return Borrowed immutable token string for the process lifetime, or an empty string for an
 * invalid index. Do not free it.
 */
SWMM_ENGINE_API const char* swmm_2d_output_variable_name(int i);
/**
 * @brief Bitmask for a preset name ("DEFAULT", "MINIMAL", "ALL") or a token list;
 *  returns 0 for an unparseable string.
 *
 * @param[in] text Optional null-terminated preset/token list; NULL or unparseable text yields zero.
 * @return Parsed bitmask, or zero for NULL/unparseable input (also a valid empty selection).
 */
SWMM_ENGINE_API unsigned swmm_2d_output_variable_mask(const char* text);
/**
 * @brief Preset name or token list for @p mask (static buffer, valid until the next call).
 *
 * @param[in] mask Dataset-group bitmask to format as a preset or token list.
 * @return Borrowed thread-local string; valid until this function is called again on the same
 * thread. Copy before retaining; do not free it.
 */
SWMM_ENGINE_API const char* swmm_2d_output_variable_text(unsigned mask);

/* =========================================================================
 * Per-node 1D↔2D coupling type ([2D_NODE_COUPLING])
 * =========================================================================
 * A node attribute of the 1D model, held with or without a mesh: whether a
 * node may couple to the 2D surface and through what structure. Map rows
 * ([2D_VERTEX_NODE_MAP] / [2D_TRIANGLE_NODE_MAP]) naming a NONE node are
 * ignored at initialize. A node without an authored row takes its class
 * default: junction or divider → MANHOLE, storage → OPEN, outfall → OUTFALL,
 * inlet or virtual junction → NONE. OUTFALL is legal only on outfalls (an
 * outfall is OUTFALL or NONE); OPEN only on storage nodes. Setters work in
 * the BUILDING and OPENED states.
 */

/** Coupling structure types. */
typedef enum SWMM_NodeCoupling2DType {
    SWMM_2D_COUPLING_NONE = 0, /**< Not coupleable. */
    SWMM_2D_COUPLING_MANHOLE = 1,
    SWMM_2D_COUPLING_GRATE = 2,
    SWMM_2D_COUPLING_CURB = 3,
    SWMM_2D_COUPLING_OPEN = 4,   /**< Open-top storage under the mesh. */
    SWMM_2D_COUPLING_OUTFALL = 5 /**< Tailwater + discharge delivery. */
} SWMM_NodeCoupling2DType;

/**
 * @brief Effective coupling type of a node.
 *
 *
 *
 *
 *  @ingroup engine_2d
 *
 * @param[in] node_idx Zero-based existing node index; a surface mesh is not required.
 * @param[out] type Required SWMM_NodeCoupling2DType destination.
 * @param[out] authored Optional 0/1 row-provenance destination.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a readable model state; a CREATED/CLOSED engine returns SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_node_get_2d_coupling_type(SWMM_Engine engine, int node_idx, int* type,
                                                   int* authored);

/**
 * @brief Class default coupling type of a node (what it has without a row).
 *  @ingroup engine_2d
 *
 * @param[in] node_idx Zero-based existing node index; a surface mesh is not required.
 * @param[out] type Required SWMM_NodeCoupling2DType destination.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a readable model state; a CREATED/CLOSED engine returns SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_node_get_2d_coupling_default_type(SWMM_Engine engine, int node_idx,
                                                           int* type);

/**
 * @brief Author a node's coupling type. Parameters that do not apply to the
 *         new type are dropped.
 *  @return SWMM_ERR_BADPARAM for a type illegal for the node's class.
 *  @ingroup engine_2d
 *
 * @param[in] node_idx Zero-based existing node index; a surface mesh is not required.
 * @param[in] type SWMM_NodeCoupling2DType legal for this node class.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Configuration edits require BUILDING/OPENED; later states return SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_node_set_2d_coupling_type(SWMM_Engine engine, int node_idx, int type);

/**
 * @brief Remove a node's [2D_NODE_COUPLING] row (back to the class default).
 *  @ingroup engine_2d
 *
 * @param[in] node_idx Zero-based existing node index; a surface mesh is not required.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Configuration edits require BUILDING/OPENED; later states return SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_node_reset_2d_coupling(SWMM_Engine engine, int node_idx);

/**
 * @brief Read an authored coupling parameter as its section token, e.g.
 *         key "D" → "1.2", key "CREST" → "MESH_BED". Empty when not authored.
 *  Keys: D, PERIMETER, AREA, CHAMBER_AREA, LID_OPEN_FRACTION, CREST, COEFFS,
 *  CW, CSW, COI, COO, LAW, SPREAD, TAILWATER_RELAX. Lengths and areas are in
 *  project units, TAILWATER_RELAX in seconds.
 *
 *
 *  @return SWMM_ERR_BADPARAM for an unknown key.
 *  @ingroup engine_2d
 *
 * @param[in] node_idx Zero-based existing node index; a surface mesh is not required.
 * @param[in] key Required null-terminated coupling section key from the list above.
 * @param[out] value Required writable token buffer, safely truncated with NUL.
 * @param[in] value_len Positive byte capacity including NUL.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Requires a readable model state; a CREATED/CLOSED engine returns SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_node_get_2d_coupling_param(SWMM_Engine engine, int node_idx,
                                                    const char* key, char* value, int value_len);

/**
 * @brief Author a coupling parameter from its section token; NULL or "" clears
 *         it. A node without a row gets one at its class default type.
 *  @return SWMM_ERR_BADPARAM for an unknown key, a key that does not apply to
 *          the node's type, or an invalid value.
 *  @ingroup engine_2d
 *
 * @param[in] node_idx Zero-based existing node index; a surface mesh is not required.
 * @param[in] key Required null-terminated coupling section key from the list above.
 * @param[in] value Copied section token in project units; NULL or empty clears this parameter.
 * @param[in] engine Borrowed live engine handle; all caller storage remains caller-owned.
 * Serialize calls with stepping and other engine edits.
 * @note Configuration edits require BUILDING/OPENED; later states return SWMM_ERR_LIFECYCLE.
 * @return SWMM_OK on success; SWMM_ERR_BADHANDLE for a null engine;
 * SWMM_ERR_BADINDEX for an invalid integer index; SWMM_ERR_BADPARAM for invalid
 * required storage or values. Lifecycle restrictions and additional errors are described above.
 */
SWMM_ENGINE_API int swmm_node_set_2d_coupling_param(SWMM_Engine engine, int node_idx,
                                                    const char* key, const char* value);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_2D_H */
