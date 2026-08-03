/**
 * @file MeshEigenBasis.cpp
 * @brief Mesh geometric graph-Laplacian eigenbasis — implementation.
 *
 * @see MeshEigenBasis.hpp
 * @ingroup engine_2d
 */

#include "MeshEigenBasis.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace openswmm::twoD {

using openswmm::uncertainty::CsrGraph;
using openswmm::uncertainty::GraphEigenBasis;
using openswmm::uncertainty::coo_to_csr;

// ============================================================================
// Geometric Laplacian
// ============================================================================

CsrGraph MeshEigenBasis::buildGeometricLaplacian(const MeshData& mesh) const {
    int n = mesh.n_triangles();
    std::vector<std::vector<std::pair<int,double>>> coo(
        static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        const int tri_nbrs[3] = {
            mesh.tri_nbr0[static_cast<std::size_t>(i)],
            mesh.tri_nbr1[static_cast<std::size_t>(i)],
            mesh.tri_nbr2[static_cast<std::size_t>(i)]
        };
        for (int e = 0; e < 3; ++e) {
            int j = tri_nbrs[e];
            // Process each shared edge exactly once (i < j)
            if (j < 0 || j <= i) continue;

            double len = mesh.edge_length[static_cast<std::size_t>(i * 3 + e)];
            double dx  = mesh.tri_cx[static_cast<std::size_t>(j)]
                       - mesh.tri_cx[static_cast<std::size_t>(i)];
            double dy  = mesh.tri_cy[static_cast<std::size_t>(j)]
                       - mesh.tri_cy[static_cast<std::size_t>(i)];
            double d   = std::sqrt(dx*dx + dy*dy);
            if (d < 1e-14) continue;

            double w = len / d;  // geometric conductance weight

            // Diagonal
            coo[static_cast<std::size_t>(i)].emplace_back(i,  w);
            coo[static_cast<std::size_t>(j)].emplace_back(j,  w);
            // Off-diagonal (symmetric)
            coo[static_cast<std::size_t>(i)].emplace_back(j, -w);
            coo[static_cast<std::size_t>(j)].emplace_back(i, -w);
        }
    }
    return coo_to_csr(coo, n);
}

// ============================================================================
// Depth-weighted Laplacian
// ============================================================================

CsrGraph MeshEigenBasis::buildDepthWeightedLaplacian(const MeshData& mesh,
                                                     const double* D_cell) const {
    int n = mesh.n_triangles();
    std::vector<std::vector<std::pair<int,double>>> coo(
        static_cast<std::size_t>(n));

    // Floor prevents zero-weight edges when a cell is dry.
    const double D_floor = 1.0e-9;

    for (int i = 0; i < n; ++i) {
        const int tri_nbrs[3] = {
            mesh.tri_nbr0[static_cast<std::size_t>(i)],
            mesh.tri_nbr1[static_cast<std::size_t>(i)],
            mesh.tri_nbr2[static_cast<std::size_t>(i)]
        };
        for (int e = 0; e < 3; ++e) {
            int j = tri_nbrs[e];
            if (j < 0 || j <= i) continue;

            double len = mesh.edge_length[static_cast<std::size_t>(i * 3 + e)];
            double dx  = mesh.tri_cx[static_cast<std::size_t>(j)]
                       - mesh.tri_cx[static_cast<std::size_t>(i)];
            double dy  = mesh.tri_cy[static_cast<std::size_t>(j)]
                       - mesh.tri_cy[static_cast<std::size_t>(i)];
            double d   = std::sqrt(dx*dx + dy*dy);
            if (d < 1e-14) continue;

            double D_i  = std::max(D_cell[static_cast<std::size_t>(i)], D_floor);
            double D_j  = std::max(D_cell[static_cast<std::size_t>(j)], D_floor);
            double D_ij = 2.0 * D_i * D_j / (D_i + D_j);  // harmonic mean
            double w    = D_ij * len / d;

            coo[static_cast<std::size_t>(i)].emplace_back(i,  w);
            coo[static_cast<std::size_t>(j)].emplace_back(j,  w);
            coo[static_cast<std::size_t>(i)].emplace_back(j, -w);
            coo[static_cast<std::size_t>(j)].emplace_back(i, -w);
        }
    }
    return coo_to_csr(coo, n);
}

// ============================================================================
// buildFromLaplacian  (private) — eigensolve + filtering + P/eigenvalues
// ============================================================================

bool MeshEigenBasis::buildFromLaplacian(const CsrGraph& L, int num_modes_req) {
    // GraphEigenBasis over-requests modes (k_req = min(n-1, num_modes_req + 3))
    // so the null-mode filter has spares to discard, runs reorthogonalized
    // Lanczos from a zero-mean ramp start vector — a constant start vector is
    // the Laplacian's null eigenvector and breaks down immediately — solves the
    // tridiagonal system ascending, then keeps the first num_modes_req
    // eigenpairs with λ ≥ null_tol.
    GraphEigenBasis solver;
    solver.null_tol = null_tol;

    if (!solver.build(L, num_modes_req)) {
        // 1 = graph too small, 2 = eigensolver failed (same codes both sides).
        last_error = solver.last_error;
        return false;
    }

    // Commit: update P, eigenvalues, counters
    num_null  = solver.num_null;
    num_kept  = solver.num_kept;
    num_modes = num_modes_req;

    eigenvalues = std::move(solver.eigenvalues);
    P           = std::move(solver.P);
    return true;
}

// ============================================================================
// build()
// ============================================================================

bool MeshEigenBasis::build(const MeshData& mesh, int num_modes_req) {
    last_error     = 0;
    n_triangles    = mesh.n_triangles();
    num_kept       = 0;
    num_null       = 0;
    depth_weighted = false;

    if (n_triangles < 4) { last_error = 1; return false; }

    return buildFromLaplacian(buildGeometricLaplacian(mesh), num_modes_req);
}

// ============================================================================
// buildDepthWeighted()
// ============================================================================

bool MeshEigenBasis::buildDepthWeighted(const MeshData& mesh,
                                        int num_modes_req,
                                        const double* D_cell) {
    if (n_triangles != mesh.n_triangles() || n_triangles < 4) return false;
    if (!D_cell) return false;

    // Preserve existing basis on failure: save current state
    auto P_save         = P;
    auto ev_save        = eigenvalues;
    int  kept_save      = num_kept;
    int  null_save      = num_null;
    int  num_modes_save = num_modes;

    last_error = 0;
    bool ok = buildFromLaplacian(buildDepthWeightedLaplacian(mesh, D_cell),
                                 num_modes_req);
    if (ok) {
        depth_weighted = true;
    } else {
        // Restore previous basis unchanged
        P           = std::move(P_save);
        eigenvalues = std::move(ev_save);
        num_kept    = kept_save;
        num_null    = null_save;
        num_modes   = num_modes_save;
    }
    return ok;
}

} // namespace openswmm::twoD
