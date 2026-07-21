/**
 * @file FiedlerDiagnostic1D.hpp
 * @brief Fiedler-vector gradient diagnostic for 1D pipe-network bottleneck detection.
 *
 * @details The Fiedler vector φ₂ (second Laplacian eigenvector, j=0 after
 *          null-mode filtering in GraphEigenBasis) identifies structural transition
 *          zones in a 1D pipe network — pump stations, inline storage, inlet
 *          constrictions — where routing uncertainty is most consequential.
 *
 *          For each active node i the gradient magnitude is:
 *
 *            grad[i] = max_k  |φ₂[i] − φ₂[j]| / len[k]
 *
 *          over all conduits k adjacent to node i (endpoints n1[k]/n2[k] are
 *          active-node indices, len[k] is conduit length in metres).
 *          Pass an empty `lengths` vector to use unit lengths.
 *
 *          The result is sorted into a descending rank order so the top-k
 *          bottleneck nodes can be extracted directly as rank[0..k-1].
 *
 * Usage:
 *   FiedlerDiagnostic1D fd;
 *   fd.basis = &graph_eigen_basis;   // fully built GraphEigenBasis
 *   fd.compute(n1, n2, lengths);     // conduit connectivity (active-node indices)
 *   // fd.rank[0]     = highest-gradient node
 *   // fd.phi2[i]     = Fiedler value at active node i
 *   // fd.grad[i]     = gradient magnitude at active node i
 *   // fd.gradAtFullNode(full_idx, full_to_active) = gradient via SWMM node index
 *
 * @note Requires basis->num_kept >= 1 (mode j=0 is the Fiedler mode after
 *       null-mode filtering by GraphEigenBasis::build()).
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_FIEDLER_DIAGNOSTIC_1D_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_FIEDLER_DIAGNOSTIC_1D_HPP

#include "GraphEigenBasis.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace openswmm::uncertainty {

// ============================================================================
// FiedlerDiagnostic1D
// ============================================================================

/**
 * @brief Fiedler-vector gradient diagnostic for 1D pipe-network bottleneck detection.
 *
 * Lifecycle:
 *   1. Set `basis` to a fully-built GraphEigenBasis (num_kept >= 1).
 *   2. Call compute(n1, n2, lengths) with active-node conduit connectivity.
 *   3. Read phi2[i], grad[i], rank[k].
 *   4. Use gradAtFullNode(full_idx, full_to_active) to look up by SWMM node index.
 */
struct FiedlerDiagnostic1D {

    // -------------------------------------------------------------------------
    // Input (set before compute())
    // -------------------------------------------------------------------------

    const GraphEigenBasis* basis = nullptr;  ///< Shared eigenbasis; not owned.

    // -------------------------------------------------------------------------
    // Output (set by compute())
    // -------------------------------------------------------------------------

    /// Fiedler vector value per active node: phi2[i] = P[0 * n_nodes + i].
    std::vector<double> phi2;

    /// Gradient magnitude per active node: max |phi2[i]-phi2[j]|/len over adjacent conduits.
    std::vector<double> grad;

    /// Active-node indices sorted by grad descending; rank[0] = highest-gradient node.
    std::vector<int> rank;

    /// Fiedler eigenvalue (basis->eigenvalues[0]).
    double lambda2 = 0.0;

    // -------------------------------------------------------------------------
    // Methods
    // -------------------------------------------------------------------------

    /// True after a successful compute() call.
    bool is_ready() const noexcept { return !phi2.empty(); }

    /**
     * @brief Compute Fiedler vector, per-node gradient, and descending rank.
     *
     * @param n1       Active-node index of conduit start, length n_conduits.
     * @param n2       Active-node index of conduit end,   length n_conduits.
     * @param lengths  Conduit length in metres, length n_conduits.
     *                 Pass empty vector to use unit lengths (1.0 for every conduit).
     * @throws std::runtime_error  if basis is not ready or has fewer than 1 mode.
     * @throws std::invalid_argument if lengths is non-empty and != n1.size().
     */
    void compute(const std::vector<int>&    n1,
                 const std::vector<int>&    n2,
                 const std::vector<double>& lengths) {
        if (!basis || !basis->is_ready() || basis->num_kept < 1)
            throw std::runtime_error(
                "FiedlerDiagnostic1D::compute: basis needs >= 1 retained mode");
        if (!lengths.empty() && lengths.size() != n1.size())
            throw std::invalid_argument(
                "FiedlerDiagnostic1D::compute: lengths size must match n1 size");

        const int    nn  = basis->n_nodes;
        const auto   unn = static_cast<std::size_t>(nn);

        // Extract Fiedler vector: column j=0 of P (column-major: P[0*n_nodes + i]).
        // GraphEigenBasis already filters out the null (constant) mode, so
        // eigenvalues[0] / P[:,0] is the Fiedler pair.
        phi2.assign(basis->P.data(), basis->P.data() + unn);
        lambda2 = basis->eigenvalues[0];

        // Per-node gradient: max over adjacent conduits of |phi2[n1]-phi2[n2]|/len.
        grad.assign(unn, 0.0);
        const std::size_t nc = n1.size();
        for (std::size_t k = 0; k < nc; ++k) {
            const auto   ua  = static_cast<std::size_t>(n1[k]);
            const auto   ub  = static_cast<std::size_t>(n2[k]);
            const double len = lengths.empty() ? 1.0 : lengths[k];
            if (len < 1.0e-12) continue;
            const double g = std::abs(phi2[ua] - phi2[ub]) / len;
            if (g > grad[ua]) grad[ua] = g;
            if (g > grad[ub]) grad[ub] = g;
        }

        // Rank: descending sort by gradient magnitude.
        rank.resize(unn);
        std::iota(rank.begin(), rank.end(), 0);
        std::sort(rank.begin(), rank.end(), [this](int a, int b) {
            return grad[static_cast<std::size_t>(a)] >
                   grad[static_cast<std::size_t>(b)];
        });
    }

    /**
     * @brief Gradient at an active-node index.
     *
     * Bounds-unchecked; caller must ensure 0 <= active_idx < (int)grad.size().
     */
    double gradAtActiveNode(int active_idx) const noexcept {
        return grad[static_cast<std::size_t>(active_idx)];
    }

    /**
     * @brief Gradient at a full (SWMM) node index.
     *
     * Looks up the active-node index via `full_to_active`, then returns the
     * pre-computed gradient.  Returns 0.0 for outfalls (full_to_active[i] == -1)
     * and for out-of-range indices.
     *
     * Typical call:
     * @code
     *   double g = fd.gradAtFullNode(cp.node_idx, rom1d.full_to_active);
     * @endcode
     *
     * @param full_idx       Full SWMM node index (0-based).
     * @param full_to_active full→active map; -1 for outfalls / inactive nodes.
     * @return Gradient magnitude in [1/m], or 0.0 if node not in active set.
     */
    double gradAtFullNode(int full_idx,
                          const std::vector<int>& full_to_active) const noexcept {
        if (full_idx < 0 ||
            full_idx >= static_cast<int>(full_to_active.size()))
            return 0.0;
        const int ai = full_to_active[static_cast<std::size_t>(full_idx)];
        if (ai < 0 || ai >= static_cast<int>(grad.size()))
            return 0.0;
        return grad[static_cast<std::size_t>(ai)];
    }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_FIEDLER_DIAGNOSTIC_1D_HPP
