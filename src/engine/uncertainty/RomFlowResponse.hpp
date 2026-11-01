/**
 * @file RomFlowResponse.hpp
 * @brief PR H17 — the 1D ROM's Manning sensitivity field, phi = dh/d(ln n),
 *        from the solver's own flows.
 *
 * Manning flow scales as 1/n. Raise every roughness by d(ln n) = eps and each
 * conduit loses eps*|Q| of capacity; heads shift by eps*phi until every node
 * again passes its outflow. Linearizing a conduit about the deterministic
 * state, with conveyance at its UPSTREAM end (as the kinematic wave does),
 *
 *     |Q| = K(y_up) * sqrt(|dh| / L) / n
 *     dQ  = Ju*dphi_up + Jd*dphi_dn - eps*|Q|
 *     Ju  = |Q| * (1/(2|dh|) + K'(y_up)/K(y_up)),   Jd = -|Q| / (2|dh|).
 *
 * A free weir, orifice, outlet or pump passes a flow set by its upstream head
 * alone and does not depend on n: dQ = d*dphi_up with d = |dQ/dH|.
 *
 * Requiring each node's total outflow to be unchanged gives, node by node,
 *
 *     phi_i = (sum_out |Q_c| - sum_out Jd_c*phi_dn) / (sum_out Ju_c + sum_out d_s),
 *
 * which depends only on what lies DOWNSTREAM of i. It is solved in one sweep
 * from the outfalls upstream (exact on a dendritic network; a few
 * Gauss-Seidel passes settle flow cycles). Outfalls are phi = 0. A node with
 * no outflow at all -- still water, or the tip of a filling front where the
 * water is accumulating -- is phi = 0: there is no steady state there for
 * roughness to shift, and front timing belongs to the phase channel (H11).
 *
 * The textbook limits come out with no constant put in: a full pipe gives
 * phi_up = phi_dn + 2*|dh| (friction loss ~ n^2); a steep free-surface reach
 * gives phi = 1/(K'/K) = 0.6*y for a wide section (Q ~ y^(5/3)/n), with no
 * relay from downstream; mild reaches carry part of the downstream response
 * (backwater). A node that can spill over a weir has the weir's d in its
 * denominator: the overflow absorbs part of the rise.
 *
 * Dependency-free (same pattern as RomDiagTrust.hpp / RomThreshold.hpp).
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace openswmm::uncertainty {

/// One flowing link, oriented by its deterministic flow (up -> dn).
/// Node indices are in the solver's node space; -1 marks a fixed end
/// (outfall or inactive node, phi = 0).
struct FlowEdge {
    int up = -1, dn = -1;
    int link = -1;     ///< engine link index (diagnostics and per-member storage ratings)
    bool conduit = true;
    double q  = 0.0;   ///< |Q| (conduits; source term)
    double ju = 0.0;   ///< conduit dQ/dh_up
    double jd = 0.0;   ///< conduit dQ/dh_dn (<= 0)
    double d  = 0.0;   ///< structure |dQ/dH| on the upstream head
};

/// Fill phi (size n) from the oriented edge list. Returns the number of
/// Gauss-Seidel passes used (1 on a dendritic network).
///
/// `pinned` (optional, size n): nodes whose head cannot move -- a junction
/// overflowing at its rim, where surplus water spills -- get phi = 0, like an
/// outfall, and relay nothing upstream.
inline int solveFlowResponse(int n, const std::vector<FlowEdge>& edges,
                             std::vector<double>& phi, int max_passes = 50,
                             const std::vector<char>* pinned = nullptr) {
    const auto un = static_cast<std::size_t>(std::max(n, 0));
    phi.assign(un, 0.0);
    if (n <= 0) return 0;

    // Outgoing edges per node (CSR).
    std::vector<int> start(un + 1, 0);
    for (const auto& e : edges)
        if (e.up >= 0 && e.up < n) ++start[static_cast<std::size_t>(e.up) + 1];
    for (std::size_t i = 0; i < un; ++i) start[i + 1] += start[i];
    std::vector<int> out(static_cast<std::size_t>(start[un]));
    {
        std::vector<int> pos(start.begin(), start.end() - 1);
        for (int k = 0; k < static_cast<int>(edges.size()); ++k) {
            const int u = edges[static_cast<std::size_t>(k)].up;
            if (u >= 0 && u < n) out[static_cast<std::size_t>(pos[static_cast<std::size_t>(u)]++)] = k;
        }
    }
    auto update = [&](int i) -> double {
        if (pinned && static_cast<std::size_t>(i) < pinned->size() && (*pinned)[static_cast<std::size_t>(i)])
            return 0.0;
        double num = 0.0, den = 0.0;
        for (int p = start[static_cast<std::size_t>(i)]; p < start[static_cast<std::size_t>(i) + 1]; ++p) {
            const auto& e = edges[static_cast<std::size_t>(out[static_cast<std::size_t>(p)])];
            if (e.conduit) {
                const double pd = (e.dn >= 0 && e.dn < n) ? phi[static_cast<std::size_t>(e.dn)] : 0.0;
                num += e.q - e.jd * pd;
                den += e.ju;
            } else {
                den += e.d;
            }
        }
        return (den > 0.0) ? num / den : 0.0;
    };

    // Topological order, outfalls first: node i is ready once every node its
    // outgoing edges reach is done (Kahn on the reversed flow graph).
    std::vector<int> pending(un, 0);
    std::vector<std::vector<int>> feeders(un);
    for (const auto& e : edges) {
        if (e.up < 0 || e.up >= n || e.dn < 0 || e.dn >= n) continue;
        ++pending[static_cast<std::size_t>(e.up)];
        feeders[static_cast<std::size_t>(e.dn)].push_back(e.up);
    }
    std::vector<int> queue;
    queue.reserve(un);
    for (int i = 0; i < n; ++i) if (pending[static_cast<std::size_t>(i)] == 0) queue.push_back(i);
    std::vector<char> done(un, 0);
    for (std::size_t h = 0; h < queue.size(); ++h) {
        const int i = queue[h];
        phi[static_cast<std::size_t>(i)] = update(i);
        done[static_cast<std::size_t>(i)] = 1;
        for (int f : feeders[static_cast<std::size_t>(i)])
            if (--pending[static_cast<std::size_t>(f)] == 0) queue.push_back(f);
    }
    if (queue.size() == un) return 1;

    // Flow cycles: Gauss-Seidel on the nodes the sweep could not order.
    int passes = 1;
    for (; passes < max_passes; ++passes) {
        double change = 0.0, scale = 0.0;
        for (int i = 0; i < n; ++i) {
            if (done[static_cast<std::size_t>(i)]) continue;
            const double v = update(i);
            change = std::max(change, std::fabs(v - phi[static_cast<std::size_t>(i)]));
            scale = std::max(scale, std::fabs(v));
            phi[static_cast<std::size_t>(i)] = v;
        }
        if (change <= 1.0e-10 * (scale + 1.0e-12)) break;
    }
    for (double& v : phi) if (!std::isfinite(v)) v = 0.0;
    return passes;
}

}  // namespace openswmm::uncertainty
