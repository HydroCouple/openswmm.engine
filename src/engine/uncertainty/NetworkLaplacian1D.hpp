/**
 * @file NetworkLaplacian1D.hpp
 * @brief Graph Laplacian builder for 1D pipe networks.
 *
 * @details Constructs a CsrGraph Laplacian from conduit-to-node connectivity
 *          suitable for use with GraphEigenBasis.  Outfall nodes are excluded
 *          from the active node set; the returned graph operates on active
 *          nodes only.
 *
 *          Two weighting options:
 *          - Uniform: all conduits contribute weight 1.0 (topological Laplacian).
 *          - Weighted: caller supplies per-conduit conductance values.
 *
 *          The active-node map is returned through output parameters so that
 *          the caller can map between full node indices and the ROM's node indices.
 *
 *          Header-only (no separate .cpp needed).
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_NETWORK_LAPLACIAN_1D_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_NETWORK_LAPLACIAN_1D_HPP

#include "GraphEigenBasis.hpp"
#include <numeric>

namespace openswmm::uncertainty {

/**
 * @brief Builds graph Laplacians for 1D pipe-network uncertainty propagation.
 */
struct NetworkLaplacian1D {

    /**
     * @brief Build a uniform-weight (topological) Laplacian.
     *
     * All conduits contribute conductance weight 1.0.
     *
     * @param n_full          Total node count (including outfalls).
     * @param n_conduits      Number of conduit links.
     * @param conduit_n1      Upstream node index per conduit (length n_conduits).
     * @param conduit_n2      Downstream node index per conduit (length n_conduits).
     * @param is_outfall      Array of length n_full; non-zero = outfall (excluded).
     * @param active_map_out  Output: active_map[i] = full node index for active node i.
     * @param full_to_active_out Output: full_to_active[j] = active index (-1 if outfall).
     * @param ground_outfalls See buildWeighted().
     * @return CsrGraph Laplacian on active nodes.
     */
    static CsrGraph buildUniform(
        int n_full, int n_conduits,
        const int* conduit_n1, const int* conduit_n2,
        const int* is_outfall,
        std::vector<int>& active_map_out,
        std::vector<int>& full_to_active_out,
        bool ground_outfalls = true)
    {
        const double uniform_weight = 1.0;
        std::vector<double> weights(static_cast<std::size_t>(n_conduits),
                                    uniform_weight);
        return buildWeighted(n_full, n_conduits, conduit_n1, conduit_n2,
                             is_outfall, weights.data(),
                             active_map_out, full_to_active_out,
                             ground_outfalls);
    }

    /**
     * @brief Build a conductance-weighted Laplacian.
     *
     * @param n_full          Total node count (including outfalls).
     * @param n_conduits      Number of conduit links.
     * @param conduit_n1      Upstream node index per conduit (length n_conduits).
     * @param conduit_n2      Downstream node index per conduit (length n_conduits).
     * @param is_outfall      Array of length n_full; non-zero = outfall (excluded).
     * @param weights         Per-conduit conductance values (length n_conduits).
     *                        Negative or zero weights are clamped to 1e-10.
     * @param active_map_out  Output: active_map[i] = full node index for active node i.
     * @param full_to_active_out Output: full_to_active[j] = active index (-1 if outfall).
     * @param ground_outfalls When true (default), an interior–outfall conduit
     *        contributes a Dirichlet ground: the interior endpoint's diagonal
     *        gains +w with no off-diagonal (the outfall head is a fixed boundary,
     *        so its coupling term is absorbed into the diagonal). This makes the
     *        operator strictly positive definite for any network with at least
     *        one outfall-adjacent conduit — no null space — and its eigenvectors
     *        are no longer orthogonal to the constant vector, so spatially-uniform
     *        inputs project non-trivially and propagate uncertainty.
     *        When false, interior–outfall conduits are skipped entirely, giving
     *        the pure Neumann (topological) Laplacian with a constant null mode.
     * @return CsrGraph Laplacian on active nodes.
     */
    static CsrGraph buildWeighted(
        int n_full, int n_conduits,
        const int* conduit_n1, const int* conduit_n2,
        const int* is_outfall, const double* weights,
        std::vector<int>& active_map_out,
        std::vector<int>& full_to_active_out,
        bool ground_outfalls = true)
    {
        // Build active-node index maps
        active_map_out.clear();
        full_to_active_out.assign(static_cast<std::size_t>(n_full), -1);
        for (int j = 0; j < n_full; ++j) {
            if (!is_outfall[j]) {
                full_to_active_out[static_cast<std::size_t>(j)] =
                    static_cast<int>(active_map_out.size());
                active_map_out.push_back(j);
            }
        }
        const int n_active = static_cast<int>(active_map_out.size());

        std::vector<std::vector<std::pair<int,double>>> coo(
            static_cast<std::size_t>(n_active));

        constexpr double w_floor = 1.0e-10;

        for (int c = 0; c < n_conduits; ++c) {
            int n1 = conduit_n1[static_cast<std::size_t>(c)];
            int n2 = conduit_n2[static_cast<std::size_t>(c)];
            // Skip conduits with out-of-range endpoints.
            if (n1 < 0 || n1 >= n_full || n2 < 0 || n2 >= n_full) continue;
            int a1 = full_to_active_out[static_cast<std::size_t>(n1)];
            int a2 = full_to_active_out[static_cast<std::size_t>(n2)];

            double w = std::max(weights[static_cast<std::size_t>(c)], w_floor);
            auto ua1 = static_cast<std::size_t>(a1);
            auto ua2 = static_cast<std::size_t>(a2);

            if (a1 >= 0 && a2 >= 0) {
                // Interior–interior: symmetric stencil (Neumann/internal edge).
                //   L[i,i] += w, L[j,j] += w, L[i,j] -= w, L[j,i] -= w
                coo[ua1].emplace_back(a1,  w);
                coo[ua2].emplace_back(a2,  w);
                coo[ua1].emplace_back(a2, -w);
                coo[ua2].emplace_back(a1, -w);
            } else if (a1 >= 0) {
                // Interior–outfall: ground the interior endpoint (Dirichlet).
                // Diagonal only — the fixed outfall head has no state variable,
                // so its off-diagonal coupling is absorbed into the diagonal.
                if (ground_outfalls) coo[ua1].emplace_back(a1, w);
            } else if (a2 >= 0) {
                if (ground_outfalls) coo[ua2].emplace_back(a2, w);
            }
            // else: outfall–outfall conduit — no active endpoint, skip.
        }

        return coo_to_csr(coo, n_active);
    }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_NETWORK_LAPLACIAN_1D_HPP
