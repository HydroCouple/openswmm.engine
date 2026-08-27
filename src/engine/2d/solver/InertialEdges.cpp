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
    ze_lo.clear(); ze_hi.clear();
    slotL.clear(); slotR.clear();
    nx.clear(); ny.clear(); mx.clear(); my.clear();
    inv_dx_normal.clear(); n2_face.clear(); cell_lchar.clear();

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
            {
                // Endpoint bed elevations of the shared edge (MeshBuilder
                // convention: edge e is opposite vertex e). Same rule as
                // SurfaceFluxCalculator's edgeEndpointZ — both incident cells
                // see identical (z_lo, z_hi), so the VFR face depth is
                // antisymmetric and the FV flux mass-conservative.
                const int v[3] = {mesh.tri_v0[t], mesh.tri_v1[t], mesh.tri_v2[t]};
                const double za = mesh.vz[v[(e + 1) % 3]];
                const double zb = mesh.vz[v[(e + 2) % 3]];
                ze_lo.push_back(std::min(za, zb));
                ze_hi.push_back(std::max(za, zb));
            }
            slotL.push_back(t * 3 + e);

            // Find nb's local edge facing t (the mirror slot) and record it.
            int e2 = 0;
            for (; e2 < 3; ++e2) if (nbr_of(mesh, nb, e2) == t) break;
            slotR.push_back(nb * 3 + e2);

            slot_edge[t][e]  = eid;
            if (e2 < 3) slot_edge[nb][e2] = eid;

            // Marcher extension. The mesh stores the OUTWARD normal per cell
            // slot; cL's slot normal already points cL→cR.
            const int sl = t * 3 + e;
            nx.push_back(mesh.edge_nx[sl]);
            ny.push_back(mesh.edge_ny[sl]);
            mx.push_back(mesh.edge_mx[sl]);
            my.push_back(mesh.edge_my[sl]);
            {
                const double dxc = mesh.tri_cx[nb] - mesh.tri_cx[t];
                const double dyc = mesh.tri_cy[nb] - mesh.tri_cy[t];
                const double chord = std::hypot(dxc, dyc);
                double dn = std::fabs(dxc * mesh.edge_nx[sl] +
                                      dyc * mesh.edge_ny[sl]);
                dn = std::max(dn, 0.3 * chord);   // near-degenerate floor
                inv_dx_normal.push_back(dn > 1.0e-12 ? 1.0 / dn : 0.0);
            }
            {
                const double nf = 0.5 * (mesh.mannings_n[t] + mesh.mannings_n[nb]);
                n2_face.push_back(nf * nf);
            }
        }
    }
    ne = static_cast<int>(cL.size());

    // Per-cell characteristic length for the CFL bound, derived from the
    // ACTUAL discrete wave operator instead of a geometric proxy. The face
    // update couples cells through g·h·ξ_f/(A·dn_f); the worst (odd–even)
    // mode has eigenvalue λ = 2·(g·h/A)·Σ_f ξ_f/dn_f, and the explicit
    // update is linearly stable for dt ≤ 2/√λ = √(2A/Σ ξ/dn)/√(g·h). With
    //   L_char = √(2A / Σ_f ξ_f·inv_dx_normal_f)
    // CFL_NUMBER is a TRUE Courant fraction (1.0 = linear stability limit):
    // a raster square recovers the classic c·dt/Δx ≤ 1/√2, and a union-jack
    // right-triangle pair gets 0.408·Δx — the old 2A/ξ_max (= 0.707·Δx
    // there) overstated the allowable dt by √3, which is why frictionless
    // basins seiched at nominal CFL ≥ 0.6 (SWASHES lake-at-rest sweep,
    // 2026-08-03: predicted critical nominal 0.577, observed 0.5 flat /
    // 0.6 unstable). Cells with no interior faces keep the altitude proxy
    // (they carry no flux until a neighbour opens).
    cell_lchar.assign(static_cast<std::size_t>(nt), 0.0);
    {
        std::vector<double> S(static_cast<std::size_t>(nt), 0.0);
        for (int e = 0; e < ne; ++e) {
            const double s = xi[e] * inv_dx_normal[e];
            S[cL[e]] += s;
            S[cR[e]] += s;
        }
        for (int t = 0; t < nt; ++t) {
            if (S[t] > 1.0e-30) {
                cell_lchar[t] = std::sqrt(2.0 * mesh.tri_area[t] / S[t]);
            } else {
                double xi_max = 0.0;
                for (int e = 0; e < 3; ++e)
                    xi_max = std::max(xi_max, mesh.edge_length[t * 3 + e]);
                cell_lchar[t] =
                    (xi_max > 0.0) ? 2.0 * mesh.tri_area[t] / xi_max : 0.0;
            }
        }
    }

    // 2. Per-cell CSR incidence with orientation signs.
    cell_ptr.assign(static_cast<std::size_t>(nt) + 1, 0);
    for (int t = 0; t < nt; ++t) {
        int c = 0;
        for (int e = 0; e < 3; ++e) if (slot_edge[t][e] >= 0) ++c;
        cell_ptr[t + 1] = cell_ptr[t] + c;
    }
    const int total = cell_ptr[nt];
    cell_edge.assign(static_cast<std::size_t>(total), 0);
    cell_sign.assign(static_cast<std::size_t>(total), 0);
    cell_arm_x.assign(static_cast<std::size_t>(total), 0.0);
    cell_arm_y.assign(static_cast<std::size_t>(total), 0.0);

    std::vector<int> fill(static_cast<std::size_t>(nt), 0);
    for (int t = 0; t < nt; ++t) {
        for (int e = 0; e < 3; ++e) {
            const int eid = slot_edge[t][e];
            if (eid < 0) continue;
            const int pos = cell_ptr[t] + fill[t]++;
            cell_edge[pos] = eid;
            cell_sign[pos] = (cL[eid] == t) ? 1 : -1;
            cell_arm_x[pos] = mx[eid] - mesh.tri_cx[t];
            cell_arm_y[pos] = my[eid] - mesh.tri_cy[t];
        }
    }
}

} // namespace openswmm::twoD
