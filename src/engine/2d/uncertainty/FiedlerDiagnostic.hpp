/**
 * @file FiedlerDiagnostic.hpp
 * @brief Fiedler-vector gradient diagnostic for 2D surface-mesh bottleneck detection.
 *
 * @details The Fiedler vector φ₂ (second Laplacian eigenvector, j=1) identifies
 *          structural transition zones in the 2D mesh — narrow channels, inlet
 *          constrictions, and high-gradient cells — where coupling uncertainty
 *          is most consequential.
 *
 *          For each triangle t the gradient magnitude is:
 *
 *            grad[t] = max_nb  |φ₂[t] − φ₂[nb]| / dist(centroid_t, centroid_nb)
 *
 *          over the (up to 3) face-sharing neighbour triangles.  Boundary faces
 *          (tri_nbr < 0) are skipped; they contribute 0.
 *
 *          The result is sorted into a descending rank order so the top-k
 *          bottleneck cells can be extracted directly as rank[0..k-1].
 *
 * Usage:
 *   FiedlerDiagnostic fd;
 *   fd.basis = &mesh_basis;      // fully built MeshEigenBasis
 *   fd.compute(mesh);                  // mesh with buildMeshTopology() called
 *   // fd.rank[0] = highest-gradient cell
 *   // fd.phi2[t] = Fiedler value at cell t
 *   // fd.grad[t] = gradient magnitude at cell t
 *
 * @note Requires basis->num_kept >= 1 (mode j=0 is the Fiedler mode after null-mode filtering).
 * @ingroup engine_2d
 */

#ifndef OPENSWMM_ENGINE_2D_FIEDLER_DIAGNOSTIC_HPP
#define OPENSWMM_ENGINE_2D_FIEDLER_DIAGNOSTIC_HPP

#ifdef OPENSWMM_HAS_2D

#include "MeshEigenBasis.hpp"
#include "../data/MeshData.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace openswmm::twoD {

// ============================================================================
// FiedlerDiagnostic
// ============================================================================

/**
 * @brief Fiedler-vector gradient diagnostic for 2D mesh bottleneck detection.
 *
 * Lifecycle:
 *   1. Set `basis` to a fully-built MeshEigenBasis (num_kept >= 2).
 *   2. Call compute(mesh) — mesh must have buildMeshTopology() called so
 *      tri_nbr0/1/2 and tri_cx/cy are populated.
 *   3. Read phi2[t], grad[t], rank[i].
 */
struct FiedlerDiagnostic {

    // -------------------------------------------------------------------------
    // Input (set before compute())
    // -------------------------------------------------------------------------

    const MeshEigenBasis* basis = nullptr;  ///< Shared eigenbasis; not owned.

    // -------------------------------------------------------------------------
    // Output (set by compute())
    // -------------------------------------------------------------------------

    /// Fiedler vector value per triangle: phi2[t] = P[1*n_tri + t].
    std::vector<double> phi2;

    /// Gradient magnitude per triangle: max |phi2[t]-phi2[nb]| / dist over neighbours.
    std::vector<double> grad;

    /// Triangle indices sorted by grad descending; rank[0] = highest-gradient cell.
    std::vector<int> rank;

    /// Fiedler eigenvalue (basis->eigenvalues[1]).
    double lambda2 = 0.0;

    // -------------------------------------------------------------------------
    // Methods
    // -------------------------------------------------------------------------

    /// True after a successful compute() call.
    bool is_ready() const noexcept { return !phi2.empty(); }

    /**
     * @brief Compute Fiedler vector, per-cell gradient, and descending rank.
     *
     * @param mesh  2D mesh with buildMeshTopology() already called (requires
     *              tri_nbr0/1/2 and tri_cx/cy).
     * @throws std::runtime_error if basis is not ready or has fewer than 2 modes.
     */
    void compute(const MeshData& mesh) {
        if (!basis || basis->num_kept < 1 || basis->n_triangles < 2)
            throw std::runtime_error(
                "FiedlerDiagnostic::compute: basis needs >= 1 retained mode");

        const int    nt  = basis->n_triangles;
        const auto   unt = static_cast<std::size_t>(nt);

        // Extract Fiedler vector: column j=0 of P (column-major: P[j*nt + t]).
        // MeshEigenBasis already filters out the null (constant) mode, so
        // eigenvalues[0] / P[:,0] is the Fiedler pair — not P[:,1].
        const double* Pf = basis->P.data();  // pointer to column 0
        phi2.assign(Pf, Pf + unt);
        lambda2 = basis->eigenvalues[0];

        // Per-cell gradient: max over face-sharing neighbours of
        //   |phi2[t] - phi2[nb]| / dist(centroid_t, centroid_nb)
        // Boundary faces (tri_nbr < 0) skipped.
        grad.assign(unt, 0.0);
        for (int t = 0; t < nt; ++t) {
            const auto ut = static_cast<std::size_t>(t);
            const double px = mesh.tri_cx[ut];
            const double py = mesh.tri_cy[ut];
            const double pv = phi2[ut];

            const int nbs[3] = {mesh.tri_nbr0[ut],
                                 mesh.tri_nbr1[ut],
                                 mesh.tri_nbr2[ut]};
            for (int nb : nbs) {
                if (nb < 0) continue;
                const auto unb = static_cast<std::size_t>(nb);
                const double dx   = px - mesh.tri_cx[unb];
                const double dy   = py - mesh.tri_cy[unb];
                const double dist = std::sqrt(dx * dx + dy * dy);
                if (dist < 1.0e-12) continue;
                const double g = std::abs(pv - phi2[unb]) / dist;
                if (g > grad[ut]) grad[ut] = g;
            }
        }

        // Rank: descending sort by gradient magnitude.
        rank.resize(unt);
        std::iota(rank.begin(), rank.end(), 0);
        std::sort(rank.begin(), rank.end(), [this](int a, int b) {
            return grad[static_cast<std::size_t>(a)] >
                   grad[static_cast<std::size_t>(b)];
        });
    }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
#endif // OPENSWMM_ENGINE_2D_FIEDLER_DIAGNOSTIC_HPP
