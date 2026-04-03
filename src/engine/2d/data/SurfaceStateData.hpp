/**
 * @file SurfaceStateData.hpp
 * @brief Structure-of-Arrays (SoA) storage for 2D surface routing state.
 *
 * @details Stores per-triangle overland flow depth, total head, gradient
 *          fields, edge fluxes, source/sink terms, and cumulative statistics.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §2.2
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_STATE_DATA_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_STATE_DATA_HPP

#include <vector>
#include <cstring>
#include <algorithm>

namespace openswmm::twoD {

/**
 * @brief SoA storage for 2D surface routing state variables.
 *
 * Per-triangle arrays are indexed [0, n_triangles).
 * Edge arrays are flat 2D: [tri * 3 + edge].
 * Vertex arrays are indexed [0, n_vertices).
 */
struct SurfaceStateData {

    // -----------------------------------------------------------------------
    // State variables — per triangle [0, n_triangles)
    // -----------------------------------------------------------------------

    std::vector<double> depth;          ///< Overland flow depth ψ_o (m)
    std::vector<double> head;           ///< Total head h_o = z_s + ψ_o (m)

    // Gradient fields (per triangle)
    std::vector<double> grad_hx;        ///< ∂h/∂x (unlimited gradient)
    std::vector<double> grad_hy;        ///< ∂h/∂y (unlimited gradient)
    std::vector<double> grad_hx_lim;    ///< Limited gradient X
    std::vector<double> grad_hy_lim;    ///< Limited gradient Y

    // Reconstructed head at vertices — [0, n_vertices)
    std::vector<double> vert_head;      ///< Head reconstructed at vertices

    // Fluxes — flat 2D: [tri * 3 + edge]
    std::vector<double> edge_flux;      ///< Normal flux through each edge

    // Source/sink terms — per triangle
    std::vector<double> rainfall;       ///< Rainfall intensity (m/s)
    std::vector<double> coupling_flux;  ///< Exchange with SWMM node (m/s, + = into 2D)
    std::vector<double> net_source;     ///< Net source/sink per cell (m/s)

    // -----------------------------------------------------------------------
    // Forcing overrides (optional external control)
    // -----------------------------------------------------------------------

    std::vector<int8_t> rainfall_forced;      ///< 0=computed, 1=override, 2=add
    std::vector<int8_t> rainfall_persist;     ///< 0=reset, 1=persist
    std::vector<double> rainfall_force_val;   ///< Forced rainfall value
    std::vector<int8_t> coupling_forced;      ///< 0=computed, 1=override, 2=add
    std::vector<int8_t> coupling_persist;     ///< 0=reset, 1=persist
    std::vector<double> coupling_force_val;   ///< Forced coupling value

    // -----------------------------------------------------------------------
    // Previous step state
    // -----------------------------------------------------------------------

    std::vector<double> old_depth;      ///< Depth at start of coupling interval

    // -----------------------------------------------------------------------
    // Cumulative statistics
    // -----------------------------------------------------------------------

    std::vector<double> stat_max_depth;     ///< Maximum depth seen at each cell
    std::vector<double> stat_cum_volume;    ///< Cumulative volume through cell

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void resize(int n_triangles, int n_vertices) {
        auto nt = static_cast<std::size_t>(n_triangles);
        auto nv = static_cast<std::size_t>(n_vertices);
        auto n3 = nt * 3;

        depth.assign(nt, 0.0);
        head.assign(nt, 0.0);
        grad_hx.assign(nt, 0.0);
        grad_hy.assign(nt, 0.0);
        grad_hx_lim.assign(nt, 0.0);
        grad_hy_lim.assign(nt, 0.0);
        vert_head.assign(nv, 0.0);
        edge_flux.assign(n3, 0.0);
        rainfall.assign(nt, 0.0);
        coupling_flux.assign(nt, 0.0);
        net_source.assign(nt, 0.0);

        rainfall_forced.assign(nt, 0);
        rainfall_persist.assign(nt, 0);
        rainfall_force_val.assign(nt, 0.0);
        coupling_forced.assign(nt, 0);
        coupling_persist.assign(nt, 0);
        coupling_force_val.assign(nt, 0.0);

        old_depth.assign(nt, 0.0);
        stat_max_depth.assign(nt, 0.0);
        stat_cum_volume.assign(nt, 0.0);
    }

    void save_state() noexcept {
        std::memcpy(old_depth.data(), depth.data(),
                     depth.size() * sizeof(double));
    }

    void reset_state() noexcept {
        std::memcpy(depth.data(), old_depth.data(),
                     old_depth.size() * sizeof(double));
    }

    /// Clear RESET forcings after each step
    void clear_reset_forcings() noexcept {
        for (std::size_t i = 0; i < rainfall_forced.size(); ++i) {
            if (rainfall_persist[i] == 0) {
                rainfall_forced[i] = 0;
                rainfall_force_val[i] = 0.0;
            }
            if (coupling_persist[i] == 0) {
                coupling_forced[i] = 0;
                coupling_force_val[i] = 0.0;
            }
        }
    }

    /// Update cumulative statistics
    void update_statistics(const std::vector<double>& tri_area,
                           double dt) noexcept {
        for (std::size_t i = 0; i < depth.size(); ++i) {
            if (depth[i] > stat_max_depth[i])
                stat_max_depth[i] = depth[i];
            stat_cum_volume[i] += depth[i] * tri_area[i] * dt;
        }
    }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_STATE_DATA_HPP
