/**
 * @file ActiveSetData.hpp
 * @brief Dry-cell active-set (wet-front) mask for the 2D RHS pipeline.
 *
 * @details Most urban-flood meshes are dry over most of the simulation, with
 *          water confined to a moving front around coupled nodes, outfalls and
 *          boundary inflows. The active set restricts the per-RHS-evaluation
 *          work (reconstruction, gradients, limiter, edge fluxes, assembly) to
 *          the cells that can possibly change state this advance window:
 *
 *            active = wet ∪ sourced ∪ halo(N rings of neighbours)
 *
 *          The CVODE system stays FULL SIZE — inactive components simply get
 *          ydot ≡ 0, which is bit-exact: a dry, source-free cell walled off
 *          from the front has exactly zero RHS in the unmasked pipeline too,
 *          so its finite-difference Jacobian column/row, Krylov components and
 *          Nordsieck history are all exactly zero/frozen. No CVodeReInit is
 *          needed as the front moves; the mask is rebuilt once per advance.
 *
 *          Safety: the halo gives the front room to move within one window;
 *          if any outer-ring cell wets during the advance, the window is
 *          discarded and redone with a doubled halo (SurfaceRouter2D). An
 *          active→inactive edge is treated as a wall (flux 0) so any mask
 *          error is locally conservative, never a leak.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_ACTIVE_SET_DATA_HPP
#define OPENSWMM_ENGINE_2D_ACTIVE_SET_DATA_HPP

#include <cstdint>
#include <vector>

namespace openswmm::twoD {

struct ActiveSetData {
    bool   enabled       = false;  ///< resolved: option ∧ CVODE ∧ DW momentum
    int    halo_rings    = 2;      ///< current halo width (auto-widened on breach)
    double wet_depth_eps = 0.0;    ///< wet threshold (m); cell is a seed if V > eps·A

    /// Ring label per cell: 0 = seed (wet/sourced), 1..halo_rings = halo,
    /// kInactive = frozen. Sized n_triangles.
    static constexpr uint8_t kInactive = 255;
    std::vector<uint8_t> cell_ring;
    /// 1 = active (compute), 0 = frozen. Sized n_triangles.
    std::vector<uint8_t> cell_active;
    /// Vertex touched by ≥1 active cell. Sized n_vertices.
    std::vector<uint8_t> vert_active;

    // Compact ascending index lists rebuilt each advance (cache-friendly
    // iteration; OpenMP schedules over these instead of branch-per-cell).
    std::vector<int> active_cells;
    std::vector<int> active_verts;
    /// Cells on the OUTERMOST halo ring — the breach-check set: one of these
    /// wetting during the advance means the front consumed the whole halo.
    std::vector<int> outer_ring_cells;

    // Diagnostics (cumulative over the run).
    long rebuild_count   = 0;
    long halo_trip_count = 0;   ///< breach-redo occurrences
    int  n_seed          = 0;   ///< wet ∪ sourced cells at the last rebuild

    int n_active() const noexcept { return static_cast<int>(active_cells.size()); }

    /// Allocate the flag arrays (idempotent) and clear the lists.
    void resize(int nt, int nv) {
        cell_ring.assign(static_cast<std::size_t>(nt), kInactive);
        cell_active.assign(static_cast<std::size_t>(nt), 0);
        vert_active.assign(static_cast<std::size_t>(nv), 0);
        active_cells.clear();
        active_verts.clear();
        outer_ring_cells.clear();
    }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_ACTIVE_SET_DATA_HPP
