/**
 * @file SpatialFrame.hpp
 * @brief Spatial frame — CRS specification and coordinate data for nodes/links.
 *
 * @details The spatial frame encapsulates the coordinate reference system (CRS)
 *          and georeferenced coordinates of nodes, link centroids, and
 *          subcatchment centroids. This is a new addition in version 6.0.0
 *          that prepares the engine for 2D coupling.
 *
 * ### 2D Coupling Readiness (R30)
 *
 * The SpatialFrame is designed to serve as the interface point for future
 * 2D component coupling. A 2D SWMM component will register ICouplingPoint
 * objects that map 1D nodes/links to cells in the 2D mesh.
 *
 * @see src/engine/core/SimulationOptions.hpp — CRS stored here too
 * @see Legacy reference: src/solver/objects.h — TNode.x, TNode.y coordinates
 * @see docs/MASTER_IMPLEMENTATION_PLAN.md AD-9 (CRS), R30 (2D coupling)
 *
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_SPATIAL_FRAME_HPP
#define OPENSWMM_ENGINE_SPATIAL_FRAME_HPP

#include <string>
#include <vector>
#include <cstdint>

namespace openswmm {

// ============================================================================
// Forward declarations for 2D coupling (R30)
// ============================================================================

/**
 * @brief Abstract coupling point between a 1D SWMM object and a 2D mesh cell.
 *
 * @details This will be implemented by the 2D SWMM component to register
 *          exchange points. The 1D engine does not depend on the 2D
 *          implementation — coupling is always optional.
 *
 * @ingroup engine_core
 */
class ICouplingPoint {
public:
    virtual ~ICouplingPoint() = default;

    /** @brief Index of the 1D SWMM object (node, link, or subcatch index). */
    virtual int object_index() const noexcept = 0;

    /** @brief Type: "NODE", "LINK", or "SUBCATCH" */
    virtual const char* object_type() const noexcept = 0;

    /** @brief Exchange 1D state to the 2D component at this point. */
    virtual void exchange_to_2d(double value) = 0;

    /** @brief Receive 2D state from the 2D component at this point. */
    virtual double receive_from_2d() const = 0;
};

// ============================================================================
// SpatialFrame
// ============================================================================

/**
 * @brief Spatial frame containing CRS and georeferenced coordinates.
 *
 * @details Coordinates are stored in the CRS specified by the `crs` field.
 *          If `crs` is empty, coordinates are in model units (as in legacy SWMM).
 *
 * Arrays are indexed identically to the corresponding SoA data stores
 * (NodeData, LinkData, SubcatchData). Index i in `node_x` corresponds
 * to node i in NodeData.
 *
 * @ingroup engine_core
 */
struct SpatialFrame {
    // -----------------------------------------------------------------------
    // Coordinate reference system
    // -----------------------------------------------------------------------

    /**
     * @brief CRS specification string.
     *
     * @details EPSG code (e.g., "EPSG:4326") or PROJ string
     *          (e.g., "+proj=utm +zone=33 +datum=WGS84").
     *          Empty string means no CRS is specified (legacy behavior).
     *
     * Set from the CRS key in [OPTIONS]:
     * @code
     * [OPTIONS]
     * CRS  EPSG:4326
     * @endcode
     */
    std::string crs;

    /**
     * @brief True if the CRS represents geographic coordinates (lon/lat).
     * @details Used for distance calculations and 2D coupling.
     */
    bool is_geographic = false;

    // -----------------------------------------------------------------------
    // Map extents (from [MAP] DIMENSIONS)
    // -----------------------------------------------------------------------

    /** @brief Map extent minimum X (left). */
    double map_x1 = 0.0;

    /** @brief Map extent minimum Y (bottom). */
    double map_y1 = 0.0;

    /** @brief Map extent maximum X (right). */
    double map_x2 = 0.0;

    /** @brief Map extent maximum Y (top). */
    double map_y2 = 0.0;

    /**
     * @brief Map units string from [MAP] UNITS keyword.
     * @details Typically NONE, FEET, METERS, or DEGREES.
     */
    std::string map_units;

    // -----------------------------------------------------------------------
    // Node coordinates (indexed parallel to NodeData)
    // -----------------------------------------------------------------------

    /** @brief Node X coordinates (easting or longitude). */
    std::vector<double> node_x;

    /** @brief Node Y coordinates (northing or latitude). */
    std::vector<double> node_y;

    // -----------------------------------------------------------------------
    // Link coordinates (one point per link — nominal centroid or from-node)
    // -----------------------------------------------------------------------

    /** @brief Link centroid X coordinates. */
    std::vector<double> link_x;

    /** @brief Link centroid Y coordinates. */
    std::vector<double> link_y;

    // -----------------------------------------------------------------------
    // Subcatchment centroid coordinates
    // -----------------------------------------------------------------------

    /** @brief Subcatchment centroid X. */
    std::vector<double> subcatch_x;

    /** @brief Subcatchment centroid Y. */
    std::vector<double> subcatch_y;

    // -----------------------------------------------------------------------
    // Link vertices (polyline points between from-node and to-node)
    // -----------------------------------------------------------------------

    /** @brief Per-link X vertices. link_vertices_x[link_idx] is a vector of X coords. */
    std::vector<std::vector<double>> link_vertices_x;

    /** @brief Per-link Y vertices. link_vertices_y[link_idx] is a vector of Y coords. */
    std::vector<std::vector<double>> link_vertices_y;

    // -----------------------------------------------------------------------
    // Subcatchment polygon vertices
    // -----------------------------------------------------------------------

    /** @brief Per-subcatchment polygon X vertices. */
    std::vector<std::vector<double>> subcatch_polygon_x;

    /** @brief Per-subcatchment polygon Y vertices. */
    std::vector<std::vector<double>> subcatch_polygon_y;

    // -----------------------------------------------------------------------
    // Gage coordinates
    // -----------------------------------------------------------------------

    /** @brief Gage X coordinates. */
    std::vector<double> gage_x;

    /** @brief Gage Y coordinates. */
    std::vector<double> gage_y;

    // -----------------------------------------------------------------------
    // 2D coupling interface (R30)
    // -----------------------------------------------------------------------

    /**
     * @brief Registered 2D coupling points.
     *
     * @details 2D components register ICouplingPoint objects here to establish
     *          exchange at specific 1D objects. The 1D engine calls
     *          exchange_to_2d() and receive_from_2d() at each timestep
     *          if any coupling points are registered.
     *
     * Not owned — the 2D component manages the lifetime of these objects.
     */
    std::vector<ICouplingPoint*> coupling_points;

    /**
     * @brief Register a 2D coupling point.
     * @param point  Non-owning pointer to a coupling point.
     */
    void register_coupling_point(ICouplingPoint* point) {
        if (point) coupling_points.push_back(point);
    }

    /**
     * @brief Returns true if any 2D coupling points are registered.
     */
    bool has_2d_coupling() const noexcept {
        return !coupling_points.empty();
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_SPATIAL_FRAME_HPP */
