/**
 * @file BoundaryData.hpp
 * @brief Structure-of-Arrays (SoA) storage for 2D mesh boundary conditions.
 *
 * @details Stores per-edge boundary condition type, parameters, and cumulative
 *          flux tracking. Arrays are flat 2D [tri * 3 + edge_local], sized to
 *          n_triangles * 3 (matching edge_flux, edge_length, etc.).
 *
 *          Boundary types:
 *          - WALL: Zero-flux (default). No water crosses the boundary.
 *          - NORMAL_FLOW: Manning outflow based on bed slope at the edge.
 *          - SPECIFIED_STAGE: Prescribed water surface elevation (constant
 *            or time-varying via timeseries).
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_BOUNDARY_DATA_HPP
#define OPENSWMM_ENGINE_2D_BOUNDARY_DATA_HPP

#include <vector>
#include <cstdint>

namespace openswmm::twoD {

/**
 * @brief Boundary condition types for 2D mesh edges.
 *
 * SPECIFIED_FLOW (3) and RATING_CURVE (4) added per GUI plan §V V-E4 / V-E5.
 * All five types are enforced at flux time by the explicit marcher via
 * SurfaceFluxCalculator::boundaryEdgeFlux (and the Kokkos backend's
 * device-side equivalent); time-varying values are resolved each step in
 * SurfaceRouter2D::resolveBoundaryValues.
 */
enum class BoundaryType : int8_t {
    WALL            = 0,   ///< Zero-flux wall (default)
    NORMAL_FLOW     = 1,   ///< Manning outflow using bed slope
    SPECIFIED_STAGE = 2,   ///< Prescribed water surface elevation (constant or TS)
    SPECIFIED_FLOW  = 3,   ///< Prescribed discharge per metre of edge (constant or TS)
    RATING_CURVE    = 4    ///< Stage → flow lookup (curve registry index)
};

/**
 * @brief SoA storage for per-edge boundary conditions.
 *
 * All arrays are flat 2D: indexed as [tri * 3 + edge_local] where
 * edge_local ∈ {0,1,2}. Only meaningful for boundary edges (tri_nbr == -1),
 * but allocated for all edges to avoid indirection in the flux loop.
 */
struct BoundaryData {

    /// Boundary condition type per edge (cast from BoundaryType)
    std::vector<int8_t> edge_bc_type;

    /// Bed slope for NORMAL_FLOW edges (dimensionless, >= 0).
    /// A zero slope zeroes the Manning outflow — the edge behaves as a
    /// Wall (validate_project warns; no auto-compute from bed geometry).
    std::vector<double> edge_bed_slope;

    /// Prescribed total head for SPECIFIED_STAGE edges (m).
    /// For time-varying: updated each step from timeseries.
    std::vector<double> edge_bc_head;

    /// Timeseries index for time-varying SPECIFIED_STAGE.
    /// -1 = constant (use edge_bc_head as-is), -2 = unresolved name,
    /// >= 0 = resolved table index into SimulationContext::tables.
    std::vector<int> edge_bc_tseries;

    /// Timeseries name for deferred resolution (cleared after resolve).
    std::vector<std::string> edge_bc_tseries_name;

    /// Cumulative boundary flux (m³, positive = outflow from domain).
    /// Tracked for mass balance reporting.
    std::vector<double> edge_bc_cum_flux;

    // -----------------------------------------------------------------------
    // V-E4 — SPECIFIED_FLOW slots (per-metre-of-edge discharge).
    // Sign convention: outward-positive (matches edge_bc_cum_flux).
    // -----------------------------------------------------------------------

    /// Prescribed discharge per metre of edge (m³/s/m) for SPECIFIED_FLOW edges.
    /// For time-varying: updated each step from timeseries.
    std::vector<double> edge_bc_flow;

    /// Timeseries index for time-varying SPECIFIED_FLOW.
    /// -1 = constant (use edge_bc_flow as-is), -2 = unresolved name,
    /// >= 0 = resolved table index into SimulationContext::tables.
    std::vector<int> edge_bc_flow_tseries;

    /// Timeseries name for deferred resolution (cleared after resolve).
    std::vector<std::string> edge_bc_flow_tseries_name;

    // -----------------------------------------------------------------------
    // V-E5 — RATING_CURVE slot (stage → flow lookup).
    // -----------------------------------------------------------------------

    /// Curve registry index for RATING_CURVE edges.
    /// -1 = unset, -2 = unresolved name, >= 0 = resolved curve index.
    std::vector<int> edge_bc_rating_curve;

    /// Curve name for deferred resolution (cleared after resolve).
    std::vector<std::string> edge_bc_rating_curve_name;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Resize all arrays to n_edges and initialize to WALL defaults.
     * @param n_edges Total number of edge slots (n_triangles * 3).
     */
    void resize(int n_edges) {
        auto n = static_cast<std::size_t>(n_edges);
        edge_bc_type.assign(n, static_cast<int8_t>(BoundaryType::WALL));
        edge_bed_slope.assign(n, 0.0);
        edge_bc_head.assign(n, 0.0);
        edge_bc_tseries.assign(n, -1);
        edge_bc_tseries_name.resize(n);
        edge_bc_cum_flux.assign(n, 0.0);
        edge_bc_flow.assign(n, 0.0);
        edge_bc_flow_tseries.assign(n, -1);
        edge_bc_flow_tseries_name.resize(n);
        edge_bc_rating_curve.assign(n, -1);
        edge_bc_rating_curve_name.resize(n);
    }

    int size() const noexcept { return static_cast<int>(edge_bc_type.size()); }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_BOUNDARY_DATA_HPP
