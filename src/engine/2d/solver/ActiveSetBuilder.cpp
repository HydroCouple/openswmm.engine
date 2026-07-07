/**
 * @file ActiveSetBuilder.cpp
 * @brief Build/refresh the dry-cell active-set mask (see ActiveSetBuilder.hpp).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "ActiveSetBuilder.hpp"

#include <algorithm>

#include "SurfaceFluxCalculator.hpp"
#include "../mesh/VertexReconstruction.hpp"

namespace openswmm::twoD {

namespace {
inline int tri_nbr_of(const MeshData& mesh, int i, int e) noexcept {
    switch (e) {
        case 0:  return mesh.tri_nbr0[i];
        case 1:  return mesh.tri_nbr1[i];
        default: return mesh.tri_nbr2[i];
    }
}
} // namespace


void seedInactiveState(const MeshData& mesh, SurfaceStateData& state,
                       const SolverOptions2D& opts) {
    // One full (unmasked) pipeline pass at the current state so every frozen
    // cell/vertex carries its exact dry value: dry-cell head = bed elevation,
    // gradients = terrain slope (NOT zero — the limiter averages neighbour
    // gradients, so a zeroed frozen gradient would perturb active cells at the
    // mask edge), edge fluxes = 0 between dry cells. active_set must be OFF /
    // null for these calls to take the full loops.
    reconstructVertexHeads(mesh, state, opts.num_threads);
    computeUnlimitedGradients(mesh, state, opts.num_threads);
    computeLimitedGradients(mesh, state, opts.limiter_epsilon, opts.num_threads);
    computeEdgeFluxes(mesh, state, opts);
}


void rebuildActiveSet(const MeshData& mesh, SurfaceStateData& state,
                      const BoundaryData* boundary,
                      const std::vector<CouplingPoint>* coupling_pts,
                      const SolverOptions2D& opts, ActiveSetData& as,
                      bool live_coupling) {
    const int nt = mesh.n_triangles();
    const int nv = mesh.n_vertices();
    if (static_cast<int>(as.cell_ring.size()) != nt
        || static_cast<int>(as.vert_active.size()) != nv)
        as.resize(nt, nv);

    // Remember the outgoing active flags so departing cells can be cleaned up.
    // (Reuse cell_active as "previously active" until it is rewritten below.)

    // ---- 1. Seed pass: wet or sourced cells are ring 0 --------------------
    std::vector<int> frontier;
    frontier.reserve(256);
    for (int i = 0; i < nt; ++i) {
        const double wet_vol = as.wet_depth_eps * mesh.tri_area[i];
        const bool seed = (state.volume[i] > wet_vol)
                       || (state.rainfall[i] != 0.0)
                       || (state.coupling_flux[i] != 0.0);
        as.cell_ring[i] = seed ? 0 : ActiveSetData::kInactive;
        if (seed) frontier.push_back(i);
    }

    // Cells with a boundary edge that can move water are always seeds: the
    // resolved BC head/flow can inject regardless of the cell's own state.
    // (NORMAL_FLOW only drains existing water, but including it costs a few
    // cells and keeps the boundary integration exact.)
    if (boundary != nullptr) {
        const int ne = boundary->size();  // == nt*3 when sized
        for (int idx = 0; idx < ne; ++idx) {
            if (static_cast<BoundaryType>(boundary->edge_bc_type[idx])
                == BoundaryType::WALL) continue;
            const int c = idx / 3;
            if (as.cell_ring[c] != 0) {
                as.cell_ring[c] = 0;
                frontier.push_back(c);
            }
        }
    }

    // Coupling-point stencils. On the HELD path (live_coupling == false) the
    // exchange is a per-window constant already scattered into coupling_flux —
    // a stencil whose cells all carry zero flux contributes NOTHING this
    // window, so only nonzero-exchange stencils need activation (the wet/flux
    // seeds above already cover them; activating the rest would keep all
    // ~n_nodes stencils hot forever — measured 15.7k active for 740 wet on
    // Bellinge). On the LIVE path the orifice exchange is evaluated INSIDE
    // the RHS against the moving 2D head (coupling_flux stays 0 here), so
    // every coupling stencil must be active unconditionally.
    if (coupling_pts != nullptr && live_coupling) {
        for (const auto& cp : *coupling_pts) {
            if (cp.vertex_idx >= 0) {
                const int v = cp.vertex_idx;
                for (int k = mesh.vert_stencil_ptr[v];
                     k < mesh.vert_stencil_ptr[v + 1]; ++k) {
                    const int c = mesh.vert_stencil_idx[k];
                    if (as.cell_ring[c] != 0) {
                        as.cell_ring[c] = 0;
                        frontier.push_back(c);
                    }
                }
            } else if (cp.cell_idx >= 0 && as.cell_ring[cp.cell_idx] != 0) {
                as.cell_ring[cp.cell_idx] = 0;
                frontier.push_back(cp.cell_idx);
            }
        }
    }
    as.n_seed = static_cast<int>(frontier.size());

    // ---- 2. BFS halo expansion --------------------------------------------
    const int halo = std::max(1, as.halo_rings);
    std::vector<int> next;
    for (int r = 1; r <= halo; ++r) {
        next.clear();
        for (int i : frontier) {
            for (int e = 0; e < 3; ++e) {
                const int nbr = tri_nbr_of(mesh, i, e);
                if (nbr < 0) continue;
                if (as.cell_ring[nbr] == ActiveSetData::kInactive) {
                    as.cell_ring[nbr] = static_cast<uint8_t>(r);
                    next.push_back(nbr);
                }
            }
        }
        frontier.swap(next);
    }

    // ---- 3. Collect flags + compact lists (ascending for cache locality) --
    as.active_cells.clear();
    as.outer_ring_cells.clear();
    std::fill(as.vert_active.begin(), as.vert_active.end(), uint8_t{0});
    for (int i = 0; i < nt; ++i) {
        const bool was_active = (as.cell_active[i] != 0);
        const bool now_active = (as.cell_ring[i] != ActiveSetData::kInactive);
        if (now_active) {
            as.active_cells.push_back(i);
            if (as.cell_ring[i] == halo) as.outer_ring_cells.push_back(i);
            as.vert_active[mesh.tri_v0[i]] = 1;
            as.vert_active[mesh.tri_v1[i]] = 1;
            as.vert_active[mesh.tri_v2[i]] = 1;
        } else if (was_active) {
            // Departing cell: it only deactivates when dry with dry
            // neighbours, so its true fluxes are 0 — zero the slots so stale
            // values cannot linger in diagnostics or boundary integration.
            state.edge_flux[i * 3 + 0] = 0.0;
            state.edge_flux[i * 3 + 1] = 0.0;
            state.edge_flux[i * 3 + 2] = 0.0;
        }
        as.cell_active[i] = now_active ? uint8_t{1} : uint8_t{0};
    }
    as.active_verts.clear();
    for (int v = 0; v < nv; ++v)
        if (as.vert_active[v]) as.active_verts.push_back(v);

    ++as.rebuild_count;
}


bool activeSetBreached(const MeshData& mesh, const SurfaceStateData& state,
                       const ActiveSetData& as) {
    for (int i : as.outer_ring_cells) {
        if (state.volume[i] > as.wet_depth_eps * mesh.tri_area[i]) return true;
    }
    return false;
}

} // namespace openswmm::twoD
