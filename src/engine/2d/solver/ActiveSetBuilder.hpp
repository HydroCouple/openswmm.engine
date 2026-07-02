/**
 * @file ActiveSetBuilder.hpp
 * @brief Build/refresh the dry-cell active-set mask (see ActiveSetData.hpp).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_ACTIVE_SET_BUILDER_HPP
#define OPENSWMM_ENGINE_2D_ACTIVE_SET_BUILDER_HPP

#include <vector>

#include "../data/ActiveSetData.hpp"
#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../data/BoundaryData.hpp"
#include "../coupling/NodeCoupling.hpp"

namespace openswmm::twoD {

/// One-time seed: run the FULL vertex-head, gradient, limiter and edge-flux
/// passes so every cell/vertex holds its frozen-correct dry value (dry-cell
/// head = bed, terrain gradients, zero fluxes) before masking starts — the
/// limiter reads inactive neighbours' gradients and the vertex reconstruction
/// reads inactive stencil heads, so those must be exact, not initial zeros.
/// Call from SurfaceRouter2D::initialize() and after every reinitialize().
void seedInactiveState(const MeshData& mesh, SurfaceStateData& state,
                       const SolverOptions2D& opts);

/// Rebuild the mask from the CURRENT wet set + sources. O(nt + nv), once per
/// advance window (never per RHS evaluation). Seeds = wet cells (V > eps·A)
/// ∪ nonzero rainfall/coupling sources (runtime forcings must already be
/// folded into those arrays) ∪ cells with a non-WALL boundary edge ∪ the
/// coupling-point stencils (force-activated: on the live path the exchange
/// is scattered inside the RHS, so coupling_flux alone cannot reveal them).
/// Cells that leave the active set get their edge-flux slots zeroed so stale
/// values cannot linger in diagnostics or boundary integration.
void rebuildActiveSet(const MeshData& mesh, SurfaceStateData& state,
                      const BoundaryData* boundary,
                      const std::vector<CouplingPoint>* coupling_pts,
                      const SolverOptions2D& opts, ActiveSetData& as);

/// Post-advance safety check: true if any OUTER-ring cell got wet
/// (volume > eps·A) — the front crossed the whole halo within one window and
/// the advance must be discarded and redone with a wider halo.
bool activeSetBreached(const MeshData& mesh, const SurfaceStateData& state,
                       const ActiveSetData& as);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_ACTIVE_SET_BUILDER_HPP
