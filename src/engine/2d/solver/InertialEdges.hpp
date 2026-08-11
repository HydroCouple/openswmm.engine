/**
 * @file InertialEdges.hpp
 * @brief Unique interior-edge structure for the local-inertial momentum DOFs.
 *
 * @details Phase 2 of docs/IMEX_LOCAL_INERTIAL_IMPLEMENTATION_PLAN.md. The
 *          diffusive-wave path stores edges redundantly per cell
 *          (MeshData edge arrays are flat [tri*3+edge]); the local-inertial
 *          scheme instead carries ONE prognostic discharge q per shared
 *          interior edge (the conservation invariant: a single antisymmetric
 *          flux per edge). This builds that canonical unique-edge enumeration
 *          plus a per-cell CSR incidence so the continuity divergence
 *          dV_i/dt = −Σ_e sign_i(e)·q_e·ξ_e is a race-free per-cell gather
 *          (bit-identical under OpenMP, like the rest of the 2D pipeline).
 *
 *          Orientation: each interior edge is stored once with cL = min(t,nbr),
 *          cR = max(t,nbr). q_e > 0 means flow cL→cR. So q_e is OUTFLOW from cL
 *          (sign +1) and INFLOW to cR (sign −1).
 *
 *          Boundary edges (nbr < 0) carry NO q DOF here. Non-Wall boundary
 *          types are applied as explicit mass sources/sinks on the owning
 *          cell via SurfaceFluxCalculator::boundaryEdgeFlux (plan §3:
 *          prescribed boundaries stay explicit — no boundary momentum DOF).
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_INERTIAL_EDGES_HPP
#define OPENSWMM_ENGINE_2D_INERTIAL_EDGES_HPP

#include <vector>

namespace openswmm::twoD {

struct MeshData;

/**
 * @brief Canonical unique interior-edge layout + per-cell incidence for the
 *        local-inertial scheme.
 */
struct InertialEdges {
    int ne = 0;                       ///< number of interior (q-carrying) edges

    // Per-edge arrays (size ne), oriented cL→cR (cL = min(t,nbr), cR = max).
    std::vector<int>    cL, cR;       ///< incident cell indices
    std::vector<double> xi;           ///< edge length ξ (m)
    std::vector<double> inv_dx;       ///< 1 / centroid-to-centroid distance (1/m)
    std::vector<double> zface;        ///< max(tri_cz[cL], tri_cz[cR]) interface bed (m) — MEAN face mode
    /// Shared edge's TRUE endpoint bed elevations, sorted ze_lo ≤ ze_hi (m).
    /// Used by FACE_RECONSTRUCTION = VFR_FACE to evaluate the B&S Eq. 14
    /// wetted-edge depth so thin crests block at their real elevation instead
    /// of the centroid-diluted zface (same endpoint rule as the boundary
    /// path's edgeEndpointZ — both incident cells see the identical pair).
    std::vector<double> ze_lo, ze_hi;
    std::vector<int>    slotL, slotR; ///< flat mesh edge slots [tri*3+e] for writeback

    // Explicit-marcher extension (ExplicitInertialSolver). Precomputed here so
    // the per-substep kernels stay pure arithmetic.
    std::vector<double> nx, ny;       ///< unit normal, oriented cL→cR
    std::vector<double> mx, my;       ///< edge midpoint (m)
    /// 1 / face-normal projected centroid distance: |(c⃗_R−c⃗_L)·n̂|, floored at
    /// 0.3·|c⃗_R−c⃗_L| against near-degenerate pairs. The projection is the
    /// correct gradient arm under cell-size disparity; the raw centroid chord
    /// (inv_dx above) overestimates slopes on non-orthogonal triangle pairs.
    std::vector<double> inv_dx_normal;
    std::vector<double> n2_face;      ///< (½(n_L+n_R))² Manning coefficient
    /// Per-CELL characteristic length L_char = 2A/ξ_max (smallest altitude, m)
    /// for the CFL step bound dt = α·L_char/√(g·h).
    std::vector<double> cell_lchar;

    // Per-cell CSR incidence for the conservative continuity gather. For cell i,
    // the incident edges are cell_edge[cell_ptr[i] .. cell_ptr[i+1]) with sign
    // cell_sign (+1 if i==cL, −1 if i==cR). Then
    //   dV_i/dt (flux part) = − Σ cell_sign · q[edge] · ξ[edge].
    std::vector<int>    cell_ptr;     ///< [n_triangles + 1] CSR row pointers
    std::vector<int>    cell_edge;    ///< incident edge id
    std::vector<double> cell_sign;    ///< +1 (i==cL) / −1 (i==cR)

    /// Build the structure from mesh topology. O(n_triangles).
    void build(const MeshData& mesh);

    bool empty() const noexcept { return ne == 0; }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_INERTIAL_EDGES_HPP
