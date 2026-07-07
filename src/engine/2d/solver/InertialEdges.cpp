/**
 * @file InertialEdges.cpp
 * @brief Implementation of the unique interior-edge builder.
 *
 * @see InertialEdges.hpp
 * @ingroup engine_2d
 */

#include "InertialEdges.hpp"
#include "../data/MeshData.hpp"

#include <array>
#include <algorithm>
#include <cmath>

namespace openswmm::twoD {

namespace {
inline int nbr_of(const MeshData& m, int t, int e) {
    return (e == 0) ? m.tri_nbr0[t] : (e == 1) ? m.tri_nbr1[t] : m.tri_nbr2[t];
}
} // namespace

void InertialEdges::build(const MeshData& mesh) {
    const int nt = mesh.n_triangles();

    cL.clear(); cR.clear(); xi.clear(); inv_dx.clear(); zface.clear();
    slotL.clear(); slotR.clear();

    // slot_edge[t][e] = unique-edge id incident to (t, local edge e), or −1 for
    // a boundary edge. Filled for BOTH sides of every interior edge.
    std::vector<std::array<int, 3>> slot_edge(
        static_cast<std::size_t>(nt), std::array<int, 3>{-1, -1, -1});

    // 1. Enumerate unique interior edges. Count each once (nbr > t), and stamp
    //    the matching local edge on the neighbour so the CSR pass sees both.
    for (int t = 0; t < nt; ++t) {
        for (int e = 0; e < 3; ++e) {
            const int nb = nbr_of(mesh, t, e);
            if (nb < 0 || nb < t) continue;   // boundary, or already counted

            const int eid = static_cast<int>(cL.size());
            cL.push_back(t);
            cR.push_back(nb);
            xi.push_back(mesh.edge_length[t * 3 + e]);
            const double ddx = std::hypot(mesh.tri_cx[t] - mesh.tri_cx[nb],
                                          mesh.tri_cy[t] - mesh.tri_cy[nb]);
            inv_dx.push_back(ddx > 1.0e-12 ? 1.0 / ddx : 0.0);
            zface.push_back(std::max(mesh.tri_cz[t], mesh.tri_cz[nb]));
            slotL.push_back(t * 3 + e);

            // Find nb's local edge facing t (the mirror slot) and record it.
            int e2 = 0;
            for (; e2 < 3; ++e2) if (nbr_of(mesh, nb, e2) == t) break;
            slotR.push_back(nb * 3 + e2);

            slot_edge[t][e]  = eid;
            if (e2 < 3) slot_edge[nb][e2] = eid;
        }
    }
    ne = static_cast<int>(cL.size());

    // 2. Per-cell CSR incidence with orientation signs.
    cell_ptr.assign(static_cast<std::size_t>(nt) + 1, 0);
    for (int t = 0; t < nt; ++t) {
        int c = 0;
        for (int e = 0; e < 3; ++e) if (slot_edge[t][e] >= 0) ++c;
        cell_ptr[t + 1] = cell_ptr[t] + c;
    }
    const int total = cell_ptr[nt];
    cell_edge.assign(static_cast<std::size_t>(total), 0);
    cell_sign.assign(static_cast<std::size_t>(total), 0.0);

    std::vector<int> fill(static_cast<std::size_t>(nt), 0);
    for (int t = 0; t < nt; ++t) {
        for (int e = 0; e < 3; ++e) {
            const int eid = slot_edge[t][e];
            if (eid < 0) continue;
            const int pos = cell_ptr[t] + fill[t]++;
            cell_edge[pos] = eid;
            cell_sign[pos] = (cL[eid] == t) ? 1.0 : -1.0;
        }
    }
}

} // namespace openswmm::twoD
