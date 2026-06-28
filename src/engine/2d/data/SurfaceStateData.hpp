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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_STATE_DATA_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_STATE_DATA_HPP

#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace openswmm { struct NodeData; }  // 1D node data (held during a 2D advance)

namespace openswmm::twoD {

struct CouplingPoint;  // fwd decl — 1D↔2D coupling descriptor (NodeCoupling.hpp)

/**
 * @brief SoA storage for 2D surface routing state variables.
 *
 * Per-triangle arrays are indexed [0, n_triangles).
 * Edge arrays are flat 2D: [tri * 3 + edge].
 * Vertex arrays are indexed [0, n_vertices).
 */
struct BoundaryData;  // fwd decl — per-edge boundary conditions (BoundaryData.hpp)

struct SurfaceStateData {

    // -----------------------------------------------------------------------
    // State variables — per triangle [0, n_triangles)
    // -----------------------------------------------------------------------

    /// Non-owning view of the per-edge boundary conditions (owned by
    /// SurfaceRouter2D). nullptr ⇒ all boundary edges are no-flux walls (the
    /// legacy behaviour, and what the unit-test call sites that build a bare
    /// state get). When set, computeEdgeFluxes applies the per-type boundary
    /// flux (NORMAL_FLOW / SPECIFIED_STAGE / SPECIFIED_FLOW / RATING_CURVE).
    /// The pointer is shallow-copied on assignment (it references config, not
    /// per-cell state), which is fine — both copies see the same BC config.
    const BoundaryData* boundary = nullptr;

    /// Live node-coupling view, set by SurfaceRouter2D ONLY when the macro-step
    /// path is active (COUPLING_INTERVAL > 1). When non-null the CVODE RHS
    /// evaluates the 1D↔2D orifice exchange against the CURRENT 2D head (so it
    /// self-limits and integrates stably over a large window) instead of a held
    /// per-window source; the ∫Q dt is carried by CVODE quadrature for
    /// conservative booking. nullptr ⇒ the legacy held-flux path (default).
    /// `nodes_1d` is the 1D node data, frozen for the duration of the advance.
    const NodeData*                   nodes_1d        = nullptr;
    const std::vector<CouplingPoint>* node_coupling   = nullptr;  ///< non-outfall points

    std::vector<double> depth;          ///< Mean wetted depth h̄ = V/A_wet (m) [reconstructed]
    std::vector<double> head;           ///< Free-surface elevation η (m) [reconstructed]
    std::vector<double> volume;         ///< Cell water volume V (m³) — the integrated state

    // Gradient fields (per triangle)
    std::vector<double> grad_hx;        ///< ∂h/∂x (unlimited gradient)
    std::vector<double> grad_hy;        ///< ∂h/∂y (unlimited gradient)
    std::vector<double> grad_hx_lim;    ///< Limited gradient X
    std::vector<double> grad_hy_lim;    ///< Limited gradient Y

    // Reconstructed head at vertices — [0, n_vertices)
    std::vector<double> vert_head;      ///< Head reconstructed at vertices

    // Cell-centred velocity (RT0 reconstruction from edge fluxes) — per triangle
    std::vector<double> face_vx;        ///< Cell velocity X component (m/s)
    std::vector<double> face_vy;        ///< Cell velocity Y component (m/s)

    // Per-cell continuity residual — per triangle (m³/s, ≈0 when conservative)
    std::vector<double> cell_continuity_err;

    // Fluxes — flat 2D: [tri * 3 + edge]
    std::vector<double> edge_flux;      ///< Normal flux through each edge

    // Source/sink terms — per triangle
    std::vector<double> rainfall;       ///< Rainfall intensity (m/s)
    std::vector<double> evap_rate;      ///< Evaporation demand rate (m/s, >= 0)
    std::vector<double> coupling_flux;  ///< Exchange with SWMM node (m/s, + = into 2D)
    std::vector<double> net_source;     ///< Net source/sink per cell (m/s)

    // -----------------------------------------------------------------------
    // Forcing overrides (optional external control)
    // -----------------------------------------------------------------------

    std::vector<int8_t> rainfall_forced;      ///< 0=computed, 1=override, 2=add
    std::vector<int8_t> rainfall_persist;     ///< 0=reset, 1=persist
    std::vector<double> rainfall_force_val;   ///< Forced rainfall value
    std::vector<int8_t> evap_forced;          ///< 0=computed, 1=override, 2=add
    std::vector<int8_t> evap_persist;         ///< 0=reset, 1=persist
    std::vector<double> evap_force_val;       ///< Forced evaporation value
    std::vector<int8_t> coupling_forced;      ///< 0=computed, 1=override, 2=add
    std::vector<int8_t> coupling_persist;     ///< 0=reset, 1=persist
    std::vector<double> coupling_force_val;   ///< Forced coupling value

    // -----------------------------------------------------------------------
    // Previous step state
    // -----------------------------------------------------------------------

    std::vector<double> old_depth;      ///< Mean depth at start of coupling interval
    std::vector<double> old_volume;     ///< Volume at start of coupling interval (m³)

    // -----------------------------------------------------------------------
    // Cumulative statistics
    // -----------------------------------------------------------------------

    std::vector<double> stat_max_depth;     ///< Maximum depth ψ_o seen at each cell (m)
    std::vector<double> stat_max_velocity;  ///< Max cell speed |v| = √(vx²+vy²) (m/s)
    std::vector<double> stat_max_cont_err;  ///< Max |cell_continuity_err| (m³/s)
    std::vector<double> stat_cum_volume;    ///< Cumulative volume through cell (m³)

    /// Cumulative evaporation loss over the whole simulation (m³). Kept here
    /// (not in core's MassBalance2D, owned by another work stream) and folded
    /// into the API-level totals by Api2D.cpp.
    double evap_loss_total = 0.0;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    void resize(int n_triangles, int n_vertices) {
        auto nt = static_cast<std::size_t>(n_triangles);
        auto nv = static_cast<std::size_t>(n_vertices);
        auto n3 = nt * 3;

        depth.assign(nt, 0.0);
        head.assign(nt, 0.0);
        volume.assign(nt, 0.0);
        grad_hx.assign(nt, 0.0);
        grad_hy.assign(nt, 0.0);
        grad_hx_lim.assign(nt, 0.0);
        grad_hy_lim.assign(nt, 0.0);
        vert_head.assign(nv, 0.0);
        face_vx.assign(nt, 0.0);
        face_vy.assign(nt, 0.0);
        cell_continuity_err.assign(nt, 0.0);
        edge_flux.assign(n3, 0.0);
        rainfall.assign(nt, 0.0);
        evap_rate.assign(nt, 0.0);
        coupling_flux.assign(nt, 0.0);
        net_source.assign(nt, 0.0);

        rainfall_forced.assign(nt, 0);
        rainfall_persist.assign(nt, 0);
        rainfall_force_val.assign(nt, 0.0);
        evap_forced.assign(nt, 0);
        evap_persist.assign(nt, 0);
        evap_force_val.assign(nt, 0.0);
        coupling_forced.assign(nt, 0);
        coupling_persist.assign(nt, 0);
        coupling_force_val.assign(nt, 0.0);

        old_depth.assign(nt, 0.0);
        old_volume.assign(nt, 0.0);
        stat_max_depth.assign(nt, 0.0);
        stat_max_velocity.assign(nt, 0.0);
        stat_max_cont_err.assign(nt, 0.0);
        stat_cum_volume.assign(nt, 0.0);
        evap_loss_total = 0.0;
    }

    void save_state() noexcept {
        std::memcpy(old_depth.data(), depth.data(),
                     depth.size() * sizeof(double));
        std::memcpy(old_volume.data(), volume.data(),
                     volume.size() * sizeof(double));
    }

    void reset_state() noexcept {
        std::memcpy(depth.data(), old_depth.data(),
                     old_depth.size() * sizeof(double));
        std::memcpy(volume.data(), old_volume.data(),
                     old_volume.size() * sizeof(double));
    }

    /// Clear RESET forcings after each step
    void clear_reset_forcings() noexcept {
        for (std::size_t i = 0; i < rainfall_forced.size(); ++i) {
            if (rainfall_persist[i] == 0) {
                rainfall_forced[i] = 0;
                rainfall_force_val[i] = 0.0;
            }
            if (evap_persist[i] == 0) {
                evap_forced[i] = 0;
                evap_force_val[i] = 0.0;
            }
            if (coupling_persist[i] == 0) {
                coupling_forced[i] = 0;
                coupling_force_val[i] = 0.0;
            }
        }
    }

    /// Update cumulative statistics / rendering envelopes.
    ///
    /// Aggregates once per model (routing) step at the accepted end-of-step
    /// state. MUST be called after computeFaceVelocity and computeCellContinuity
    /// so face_vx/vy and cell_continuity_err hold the accepted values. All
    /// envelopes fuse into the single per-cell loop already walked for
    /// stat_cum_volume — no extra passes, no sub-step sampling.
    void update_statistics([[maybe_unused]] const std::vector<double>& tri_area,
                           double dt,
                           [[maybe_unused]] int nthreads = 1) noexcept {
        // Each cell updates only its own envelope slots (max/cum into [i]);
        // schedule(static) keeps this bit-identical to serial for any thread
        // count. int loop index for OpenMP canonical-loop form.
        const int n = static_cast<int>(depth.size());
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for schedule(static) num_threads(nthreads)
#endif
        for (int i = 0; i < n; ++i) {
            if (depth[i] > stat_max_depth[i])
                stat_max_depth[i] = depth[i];

            const double speed = std::sqrt(face_vx[i] * face_vx[i]
                                         + face_vy[i] * face_vy[i]);
            if (speed > stat_max_velocity[i])
                stat_max_velocity[i] = speed;

            const double aerr = std::abs(cell_continuity_err[i]);
            if (aerr > stat_max_cont_err[i])
                stat_max_cont_err[i] = aerr;

            // Cell water volume × dt (VFR: V is the integrated state, not h̄·A).
            stat_cum_volume[i] += volume[i] * dt;
        }
    }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_STATE_DATA_HPP
