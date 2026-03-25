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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
 * @param engine      Engine handle.
 * @param idx         Zero-based link index.
 * @param[out] count  Receives the vertex count.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_spatial_get_link_vertex_count(SWMM_Engine engine, int idx, int* count);

/**
 * @brief Get the polyline vertices for a link.
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_SPATIAL_H */
