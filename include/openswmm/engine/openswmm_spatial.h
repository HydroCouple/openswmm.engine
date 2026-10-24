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
 * @file      openswmm_spatial.h
 * @brief     OpenSWMM Engine — Spatial Frame C API.
 *
 * @details CRS management, node/link/subcatchment/gage coordinates,
 *          link polyline vertices, and subcatchment polygon vertices.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_SPATIAL_H
#define OPENSWMM_SPATIAL_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * CRS (Coordinate Reference System)
 * ========================================================================= */

/**
 * @brief Set the coordinate reference system string for the model.
 *
 * @details The CRS string is stored as metadata (e.g., an EPSG code or
 *          WKT string). It is not used for coordinate transformations but
 *          is preserved in hot start files for consistency checking.
 *
 * @param[in] engine Engine handle.
 * @param[in] crs Null-terminated CRS string (e.g., "EPSG:4326").
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. crs must be non-null; empty clears it. Copies the
 *     string to both spatial and OPTIONS CRS metadata without validating it or transforming
 *     coordinates.
 */
SWMM_ENGINE_API int swmm_spatial_set_crs(SWMM_Engine engine, const char* crs);

/**
 * @brief Get the coordinate reference system string.
 * @param[in] engine Engine handle.
 * @param[out] buf Caller-allocated buffer to receive the CRS string.
 * @param[in] buflen Size of @p buf in bytes.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. buf must be non-null and buflen positive or
 *     BADPARAM is returned. Copies the spatial-frame CRS (empty allowed), NUL-terminated and
 *     silently truncated to buflen-1 bytes.
 */
SWMM_ENGINE_API int swmm_spatial_get_crs(SWMM_Engine engine, char* buf, int buflen);

/* =========================================================================
 * Node coordinates
 * ========================================================================= */

/**
 * @brief Set the X/Y coordinates for a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[in] x X coordinate in CRS units.
 * @param[in] y Y coordinate in CRS units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant
 *     index check returns SWMM_ERR_BADINDEX. Coordinates are copied without finite/range validation
 *     and do not reproject or recompute other geometry. Marks this node coordinate as explicitly
 *     present, including when x=y=0.
 */
SWMM_ENGINE_API int swmm_spatial_set_node_coord(SWMM_Engine engine, int idx, double x, double y);

/**
 * @brief Read coordinate provenance without confusing an authored origin with missing data.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. No spatial-edit
 *     lifecycle guard is imposed.
 * @param[in] idx Zero-based existing node index.
 * @param[out] present Non-null destination: 1 when coordinate provenance is present, otherwise 0.
 *     An authored (0,0) may be present.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for invalid node, or
 *     SWMM_ERR_BADPARAM for null present.
 */
SWMM_ENGINE_API int swmm_spatial_node_has_coord(SWMM_Engine engine, int idx, int* present);

/**
 * @brief Get the X/Y coordinates for a node.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based node index.
 * @param[out] x Receives the X coordinate.
 * @param[out] y Receives the Y coordinate.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note x may be NULL to suppress that output. y may be NULL to suppress that output. A null engine
 *     returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant index check
 *     returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_spatial_get_node_coord(SWMM_Engine engine, int idx, double* x, double* y);

/**
 * @brief Get X/Y coordinates for all nodes in a single call.
 * @param[in] engine Engine handle.
 * @param[out] x_buf Caller-allocated buffer for X coordinates (count elements).
 * @param[out] y_buf Caller-allocated buffer for Y coordinates (count elements).
 * @param[in] count Number of nodes (should equal swmm_node_count()).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. Both coordinate arrays must be non-null and count
 *     positive or BADPARAM is returned. Copies min(count,node count) elements in CRS units; any
 *     tail remains unchanged.
 */
SWMM_ENGINE_API int swmm_spatial_get_node_coords_bulk(SWMM_Engine engine, double* x_buf,
                                                      double* y_buf, int count);

/**
 * @brief Set X/Y coordinates for all nodes in a single call.
 * @param[in] engine Engine handle.
 * @param[in] x_buf Array of X coordinates (count elements).
 * @param[in] y_buf Array of Y coordinates (count elements).
 * @param[in] count Number of nodes.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. Both coordinate arrays must be non-null and count
 *     positive or BADPARAM is returned. Copies min(count,node count) elements in CRS units; any
 *     tail remains unchanged. Unlike the scalar node-coordinate setter, this wrapper does not set
 *     node_has_xy provenance flags.
 */
SWMM_ENGINE_API int swmm_spatial_set_node_coords_bulk(SWMM_Engine engine, const double* x_buf,
                                                      const double* y_buf, int count);

/* =========================================================================
 * Link coordinates (centroid or midpoint)
 * ========================================================================= */

/**
 * @brief Set the centroid/midpoint X/Y coordinate for a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] x X coordinate in CRS units.
 * @param[in] y Y coordinate in CRS units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant
 *     index check returns SWMM_ERR_BADINDEX. Coordinates are copied without finite/range validation
 *     and do not reproject or recompute other geometry.
 */
SWMM_ENGINE_API int swmm_spatial_set_link_coord(SWMM_Engine engine, int idx, double x, double y);

/**
 * @brief Get the centroid/midpoint X/Y coordinate for a link.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] x Receives the X coordinate.
 * @param[out] y Receives the Y coordinate.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note x may be NULL to suppress that output. y may be NULL to suppress that output. A null engine
 *     returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant index check
 *     returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_spatial_get_link_coord(SWMM_Engine engine, int idx, double* x, double* y);

/* =========================================================================
 * Link vertices (polyline)
 * ========================================================================= */

/**
 * @brief Set the polyline vertices for a link.
 *
 * @details Vertices define the intermediate points of a link's path between
 *          its upstream and downstream nodes.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[in] x Array of vertex X coordinates.
 * @param[in] y Array of vertex Y coordinates.
 * @param[in] count Number of vertices.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant
 *     index check returns SWMM_ERR_BADINDEX. Arrays are copied; caller retains ownership. count
 *     must be nonnegative and define valid pointer ranges; positive counts require both arrays or
 *     BADPARAM is returned. No polygon validity, closure, orientation or finite-coordinate
 *     validation is performed. Stores interior vertices only; getters add each valid endpoint-node
 *     coordinate. Passing getter output back unchanged would duplicate endpoints as interiors.
 */
SWMM_ENGINE_API int swmm_spatial_set_link_vertices(SWMM_Engine engine, int idx, const double* x,
                                                   const double* y, int count);

/**
 * @brief Get the number of polyline vertices for a link.
 *
 * @details Count includes the upstream node coordinate at index 0,
 *          followed by stored interior vertices (if any), and the
 *          downstream node coordinate as the final index.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] count Receives the vertex count.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note count may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/link/subcatchment/gage/pollutant index check returns SWMM_ERR_BADINDEX. Only
 *     valid endpoint indices are included, so an unconnected link can have fewer than two endpoint
 *     vertices.
 */
SWMM_ENGINE_API int swmm_spatial_get_link_vertex_count(SWMM_Engine engine, int idx, int* count);

/**
 * @brief Get the polyline vertices for a link.
 *
 * @details Returned vertices are ordered as:
 *          1) upstream node coordinate at index 0,
 *          2) interior vertices in stored order,
 *          3) downstream node coordinate as the final vertex.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based link index.
 * @param[out] x Caller-allocated buffer for X coordinates.
 * @param[out] y Caller-allocated buffer for Y coordinates.
 * @param[in] max_count Maximum number of vertices to write to the buffers.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant
 *     index check returns SWMM_ERR_BADINDEX. Both output arrays must be non-null and max_count
 *     positive or BADPARAM is returned. Returns a silently truncated prefix when capacity is short;
 *     query the corresponding count first to allocate complete arrays. No written-count output is
 *     provided.
 */
SWMM_ENGINE_API int swmm_spatial_get_link_vertices(SWMM_Engine engine, int idx, double* x,
                                                   double* y, int max_count);

/* =========================================================================
 * Subcatchment coordinates (centroid)
 * ========================================================================= */

/**
 * @brief Set the centroid X/Y coordinate for a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] x X coordinate in CRS units.
 * @param[in] y Y coordinate in CRS units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant
 *     index check returns SWMM_ERR_BADINDEX. Coordinates are copied without finite/range validation
 *     and do not reproject or recompute other geometry.
 */
SWMM_ENGINE_API int swmm_spatial_set_subcatch_coord(SWMM_Engine engine, int idx, double x,
                                                    double y);

/**
 * @brief Get the centroid X/Y coordinate for a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] x Receives the X coordinate.
 * @param[out] y Receives the Y coordinate.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note x may be NULL to suppress that output. y may be NULL to suppress that output. A null engine
 *     returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant index check
 *     returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_spatial_get_subcatch_coord(SWMM_Engine engine, int idx, double* x,
                                                    double* y);

/* =========================================================================
 * Subcatchment polygon
 * ========================================================================= */

/**
 * @brief Set the polygon boundary vertices for a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[in] x Array of polygon vertex X coordinates.
 * @param[in] y Array of polygon vertex Y coordinates.
 * @param[in] count Number of polygon vertices.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant
 *     index check returns SWMM_ERR_BADINDEX. Arrays are copied; caller retains ownership. count
 *     must be nonnegative and define valid pointer ranges; positive counts require both arrays or
 *     BADPARAM is returned. No polygon validity, closure, orientation or finite-coordinate
 *     validation is performed.
 */
SWMM_ENGINE_API int swmm_spatial_set_subcatch_polygon(SWMM_Engine engine, int idx, const double* x,
                                                      const double* y, int count);

/**
 * @brief Get the number of polygon vertices for a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] count Receives the polygon vertex count.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note count may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed node/link/subcatchment/gage/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_spatial_get_subcatch_polygon_count(SWMM_Engine engine, int idx,
                                                            int* count);

/**
 * @brief Get the polygon boundary vertices for a subcatchment.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based subcatchment index.
 * @param[out] x Caller-allocated buffer for X coordinates.
 * @param[out] y Caller-allocated buffer for Y coordinates.
 * @param[in] max_count Maximum number of vertices to write to the buffers.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant
 *     index check returns SWMM_ERR_BADINDEX. Both output arrays must be non-null and max_count
 *     positive or BADPARAM is returned. Returns a silently truncated prefix when capacity is short;
 *     query the corresponding count first to allocate complete arrays. No written-count output is
 *     provided.
 */
SWMM_ENGINE_API int swmm_spatial_get_subcatch_polygon(SWMM_Engine engine, int idx, double* x,
                                                      double* y, int max_count);

/* =========================================================================
 * Gage coordinates
 * ========================================================================= */

/**
 * @brief Set the X/Y coordinate for a rain gage.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based gage index.
 * @param[in] x X coordinate in CRS units.
 * @param[in] y Y coordinate in CRS units.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant
 *     index check returns SWMM_ERR_BADINDEX. Coordinates are copied without finite/range validation
 *     and do not reproject or recompute other geometry.
 */
SWMM_ENGINE_API int swmm_spatial_set_gage_coord(SWMM_Engine engine, int idx, double x, double y);

/**
 * @brief Get the X/Y coordinate for a rain gage.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based gage index.
 * @param[out] x Receives the X coordinate.
 * @param[out] y Receives the Y coordinate.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note x may be NULL to suppress that output. y may be NULL to suppress that output. A null engine
 *     returns SWMM_ERR_BADHANDLE. A failed node/link/subcatchment/gage/pollutant index check
 *     returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_spatial_get_gage_coord(SWMM_Engine engine, int idx, double* x, double* y);

/* =========================================================================
 * [MAP] extent and units, and GUI annotation sections
 * (closed C-API gaps 2026-10-11)
 * ========================================================================= */

/**
 * @brief Read the stored map display extent.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. No spatial-edit
 *     lifecycle guard is imposed.
 * @param[out] x1 Optional destination for the first X coordinate in map/CRS units.
 * @param[out] y1 Optional destination for the first Y coordinate in map/CRS units.
 * @param[out] x2 Optional destination for the second X coordinate in map/CRS units.
 * @param[out] y2 Optional destination for the second Y coordinate in map/CRS units.
 * @return SWMM_OK, including suppressed outputs; SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_spatial_get_map_dimensions(SWMM_Engine engine, double* x1, double* y1,
                                                    double* x2, double* y2);

/**
 * @brief Replace the stored map display extent.
 *
 * Does not require minimum/maximum ordering, change object coordinates or compute a new bounding
 * box.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. No spatial-edit
 *     lifecycle guard is imposed.
 * @param[in] x1 Finite first X coordinate in map/CRS units.
 * @param[in] y1 Finite first Y coordinate in map/CRS units.
 * @param[in] x2 Finite second X coordinate in map/CRS units.
 * @param[in] y2 Finite second Y coordinate in map/CRS units.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for any nonfinite
 *     coordinate.
 */
SWMM_ENGINE_API int swmm_spatial_set_map_dimensions(SWMM_Engine engine, double x1, double y1,
                                                    double x2, double y2);

/**
 * @brief Copy the stored map-unit label.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. No spatial-edit
 *     lifecycle guard is imposed.
 * @param[out] buf Non-null caller-owned text destination, empty when unspecified; NUL-terminated
 *     and silently truncated to buflen-1 bytes.
 * @param[in] buflen Positive capacity including NUL, in bytes.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid buffer.
 */
SWMM_ENGINE_API int swmm_spatial_get_map_units(SWMM_Engine engine, char* buf, int buflen);

/**
 * @brief Copy and uppercase the map-unit label.
 *
 * Does not restrict the label vocabulary, rescale geometry or reproject coordinates.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. No spatial-edit
 *     lifecycle guard is imposed.
 * @param[in] units Non-null NUL-terminated label (such as FEET, METERS, DEGREES or NONE); empty is
 *     allowed.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null units.
 */
SWMM_ENGINE_API int swmm_spatial_set_map_units(SWMM_Engine engine, const char* units);

/**
 * @brief Join stored annotation-section lines into text without a section header.
 *
 * Does not interpret annotation content; stored lines are joined with LF. Both buf and required may
 * be NULL for a valid section.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. No spatial-edit
 *     lifecycle guard is imposed.
 * @param[in] section Non-null case-insensitive LABELS, BACKDROP, PROFILE or PROFILES token.
 * @param[out] buf Optional caller-owned text destination. NULL queries required size regardless of
 *     buflen; non-null output is NUL-terminated and silently truncated.
 * @param[in] buflen Destination capacity in bytes including NUL; must be positive when buf is
 *     non-null.
 * @param[out] required Optional full byte count INCLUDING NUL, written before short/nonpositive
 *     buffer handling. Empty/absent section requires 1 byte.
 * @return SWMM_OK including truncation/size query; SWMM_ERR_BADHANDLE for null engine or
 *     SWMM_ERR_BADPARAM for unknown/null section or a non-null buffer with buflen<=0.
 */
SWMM_ENGINE_API int swmm_spatial_get_annotation(SWMM_Engine engine, const char* section, char* buf,
                                                int buflen, int* required);

/**
 * @brief Replace the passthrough lines of a supported annotation section.
 *
 * Internal blank lines are retained; a trailing LF does not create a final additional empty line.
 * Stores text for later serialization without interpreting drawing/profile syntax.
 *
 * @param[in] engine Non-null live engine; caller provides exclusive access. No spatial-edit
 *     lifecycle guard is imposed.
 * @param[in] section Non-null case-insensitive LABELS, BACKDROP, PROFILE or PROFILES token.
 * @param[in] text Borrowed NUL-terminated content to copy; NULL/empty removes the section. LF
 *     separates lines; CR bytes are discarded.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null/unsupported
 *     section.
 */
SWMM_ENGINE_API int swmm_spatial_set_annotation(SWMM_Engine engine, const char* section,
                                                const char* text);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_SPATIAL_H */
