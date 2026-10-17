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
 * @file openswmm_spatial.h
 * @brief OpenSWMM Engine — Spatial Frame C API.
 *
 * @details CRS management, node/link/subcatchment/gage coordinates,
 *          link polyline vertices, and subcatchment polygon vertices.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
 * @param engine  Engine handle.
 * @param crs     Null-terminated CRS string (e.g., "EPSG:4326").
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_set_crs(SWMM_Engine engine, const char* crs);

/**
 * @brief Get the coordinate reference system string.
 * @param engine  Engine handle.
 * @param buf     Caller-allocated buffer to receive the CRS string.
 * @param buflen  Size of @p buf in bytes.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_get_crs(SWMM_Engine engine, char* buf, int buflen);

/* =========================================================================
 * Node coordinates
 * ========================================================================= */

/**
 * @brief Set the X/Y coordinates for a node.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param x       X coordinate in CRS units.
 * @param y       Y coordinate in CRS units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_set_node_coord(SWMM_Engine engine, int idx, double x, double y);

/** Whether a node has authored/API-assigned coordinates. Distinguishes a
 * valid (0,0) from the default for an unlocated node; does not alter geometry.
 * Returns BADINDEX for an invalid node and BADPARAM for a null output. */
SWMM_ENGINE_API int swmm_spatial_node_has_coord(SWMM_Engine engine, int idx, int* present);

/**
 * @brief Get the X/Y coordinates for a node.
 * @param engine  Engine handle.
 * @param idx     Zero-based node index.
 * @param[out] x  Receives the X coordinate.
 * @param[out] y  Receives the Y coordinate.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_get_node_coord(SWMM_Engine engine, int idx, double* x, double* y);

/**
 * @brief Get X/Y coordinates for all nodes in a single call.
 * @param engine      Engine handle.
 * @param[out] x_buf  Caller-allocated buffer for X coordinates (count elements).
 * @param[out] y_buf  Caller-allocated buffer for Y coordinates (count elements).
 * @param count       Number of nodes (should equal swmm_node_count()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_get_node_coords_bulk(SWMM_Engine engine, double* x_buf, double* y_buf, int count);

/**
 * @brief Set X/Y coordinates for all nodes in a single call.
 * @param engine  Engine handle.
 * @param x_buf   Array of X coordinates (count elements).
 * @param y_buf   Array of Y coordinates (count elements).
 * @param count   Number of nodes.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_set_node_coords_bulk(SWMM_Engine engine, const double* x_buf, const double* y_buf, int count);

/* =========================================================================
 * Link coordinates (centroid or midpoint)
 * ========================================================================= */

/**
 * @brief Set the centroid/midpoint X/Y coordinate for a link.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param x       X coordinate in CRS units.
 * @param y       Y coordinate in CRS units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_set_link_coord(SWMM_Engine engine, int idx, double x, double y);

/**
 * @brief Get the centroid/midpoint X/Y coordinate for a link.
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param[out] x  Receives the X coordinate.
 * @param[out] y  Receives the Y coordinate.
 * @returns SWMM_OK on success, or an error code.
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
 * @param engine  Engine handle.
 * @param idx     Zero-based link index.
 * @param x       Array of vertex X coordinates.
 * @param y       Array of vertex Y coordinates.
 * @param count   Number of vertices.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_set_link_vertices(SWMM_Engine engine, int idx, const double* x, const double* y, int count);

/**
 * @brief Get the number of polyline vertices for a link.
 *
 * @details Count includes the upstream node coordinate at index 0,
 *          followed by stored interior vertices (if any), and the
 *          downstream node coordinate as the final index.
 *
 * @param engine      Engine handle.
 * @param idx         Zero-based link index.
 * @param[out] count  Receives the vertex count.
 * @returns SWMM_OK on success, or an error code.
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
 * @param engine     Engine handle.
 * @param idx        Zero-based link index.
 * @param[out] x     Caller-allocated buffer for X coordinates.
 * @param[out] y     Caller-allocated buffer for Y coordinates.
 * @param max_count  Maximum number of vertices to write to the buffers.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_get_link_vertices(SWMM_Engine engine, int idx, double* x, double* y, int max_count);

/* =========================================================================
 * Subcatchment coordinates (centroid)
 * ========================================================================= */

/**
 * @brief Set the centroid X/Y coordinate for a subcatchment.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param x       X coordinate in CRS units.
 * @param y       Y coordinate in CRS units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_set_subcatch_coord(SWMM_Engine engine, int idx, double x, double y);

/**
 * @brief Get the centroid X/Y coordinate for a subcatchment.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param[out] x  Receives the X coordinate.
 * @param[out] y  Receives the Y coordinate.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_get_subcatch_coord(SWMM_Engine engine, int idx, double* x, double* y);

/* =========================================================================
 * Subcatchment polygon
 * ========================================================================= */

/**
 * @brief Set the polygon boundary vertices for a subcatchment.
 * @param engine  Engine handle.
 * @param idx     Zero-based subcatchment index.
 * @param x       Array of polygon vertex X coordinates.
 * @param y       Array of polygon vertex Y coordinates.
 * @param count   Number of polygon vertices.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_set_subcatch_polygon(SWMM_Engine engine, int idx, const double* x, const double* y, int count);

/**
 * @brief Get the number of polygon vertices for a subcatchment.
 * @param engine      Engine handle.
 * @param idx         Zero-based subcatchment index.
 * @param[out] count  Receives the polygon vertex count.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_get_subcatch_polygon_count(SWMM_Engine engine, int idx, int* count);

/**
 * @brief Get the polygon boundary vertices for a subcatchment.
 * @param engine     Engine handle.
 * @param idx        Zero-based subcatchment index.
 * @param[out] x     Caller-allocated buffer for X coordinates.
 * @param[out] y     Caller-allocated buffer for Y coordinates.
 * @param max_count  Maximum number of vertices to write to the buffers.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_get_subcatch_polygon(SWMM_Engine engine, int idx, double* x, double* y, int max_count);

/* =========================================================================
 * Gage coordinates
 * ========================================================================= */

/**
 * @brief Set the X/Y coordinate for a rain gage.
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @param x       X coordinate in CRS units.
 * @param y       Y coordinate in CRS units.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_set_gage_coord(SWMM_Engine engine, int idx, double x, double y);

/**
 * @brief Get the X/Y coordinate for a rain gage.
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @param[out] x  Receives the X coordinate.
 * @param[out] y  Receives the Y coordinate.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_get_gage_coord(SWMM_Engine engine, int idx, double* x, double* y);

/* =========================================================================
 * [MAP] extent and units, and GUI annotation sections
 * (closed C-API gaps 2026-10-11)
 * ========================================================================= */

/** @brief `[MAP] DIMENSIONS`: the map extent (x1, y1) - (x2, y2). */
SWMM_ENGINE_API int swmm_spatial_get_map_dimensions(SWMM_Engine engine,
        double* x1, double* y1, double* x2, double* y2);

/** @brief Set the `[MAP] DIMENSIONS` extent. All four values must be finite. */
SWMM_ENGINE_API int swmm_spatial_set_map_dimensions(SWMM_Engine engine,
        double x1, double y1, double x2, double y2);

/** @brief `[MAP] UNITS` as stored (upper case, e.g. FEET, METERS, DEGREES,
 *         NONE; empty when the deck did not state it). */
SWMM_ENGINE_API int swmm_spatial_get_map_units(SWMM_Engine engine, char* buf,
                                               int buflen);

/** @brief Set `[MAP] UNITS`; stored upper case like the parser does. */
SWMM_ENGINE_API int swmm_spatial_set_map_units(SWMM_Engine engine,
                                               const char* units);

/**
 * @brief Read a GUI annotation section verbatim: `LABELS`, `BACKDROP`,
 *        `PROFILE` or `PROFILES` (case-insensitive).
 *
 * @details The engine never interprets these sections; it keeps their lines
 *          and writes them back unchanged on save. The text is the section's
 *          lines joined by '\n', without the `[SECTION]` header.
 *
 * @param required If not NULL, receives the buffer size (including the
 *                 terminating NUL) the full text needs; @p buf may be NULL
 *                 to query it.
 * @return ::SWMM_ERR_BADPARAM for any other section name.
 */
SWMM_ENGINE_API int swmm_spatial_get_annotation(SWMM_Engine engine,
        const char* section, char* buf, int buflen, int* required);

/**
 * @brief Replace a GUI annotation section's lines with @p text (lines
 *        separated by '\n'); NULL or "" removes the section from the model.
 */
SWMM_ENGINE_API int swmm_spatial_set_annotation(SWMM_Engine engine,
        const char* section, const char* text);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_SPATIAL_H */
