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

CsrGraph MeshEigenBasis::buildGeometricLaplacian(
    const MeshData& mesh, const double* ground_w) const {
    int n = mesh.n_triangles();
    std::vector<std::vector<std::pair<int,double>>> coo(
        static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        for (int e = 0; e < mesh.cell_vertex_count(static_cast<int>(i)); ++e) {
            int j = mesh.cell_neighbour(static_cast<int>(i), e);
            // Process each shared edge exactly once (i < j)
            if (j < 0 || j <= i) continue;

            double len = mesh.edge_length[static_cast<std::size_t>(openswmm::twoD::MeshData::slot(static_cast<int>(i), e))];
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
    // Open-boundary grounding: diagonal-only edge to a zero-deviation ghost.
    if (ground_w != nullptr)
        for (int i = 0; i < n; ++i)
            if (ground_w[static_cast<std::size_t>(i)] > 0.0)
                coo[static_cast<std::size_t>(i)].emplace_back(
                    i, ground_w[static_cast<std::size_t>(i)]);
    return coo_to_csr(coo, n);
}

// ============================================================================
// Depth-weighted Laplacian
// ============================================================================

CsrGraph MeshEigenBasis::buildDepthWeightedLaplacian(
    const MeshData& mesh, const double* D_cell,
    const double* ground_w) const {
    int n = mesh.n_triangles();
    std::vector<std::vector<std::pair<int,double>>> coo(
        static_cast<std::size_t>(n));

    // Floor prevents zero-weight edges when a cell is dry.
    const double D_floor = 1.0e-9;

    for (int i = 0; i < n; ++i) {
        for (int e = 0; e < mesh.cell_vertex_count(static_cast<int>(i)); ++e) {
            int j = mesh.cell_neighbour(static_cast<int>(i), e);
            if (j < 0 || j <= i) continue;

            double len = mesh.edge_length[static_cast<std::size_t>(openswmm::twoD::MeshData::slot(static_cast<int>(i), e))];
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
    // Grounding, weighted by the cell's own diffusivity (same convention as
    // its interior edges).
    if (ground_w != nullptr)
        for (int i = 0; i < n; ++i)
            if (ground_w[static_cast<std::size_t>(i)] > 0.0) {
                const double D_i =
                    std::max(D_cell[static_cast<std::size_t>(i)], D_floor);
                coo[static_cast<std::size_t>(i)].emplace_back(
                    i, D_i * ground_w[static_cast<std::size_t>(i)]);
            }
    return coo_to_csr(coo, n);
}

// ============================================================================
// buildFromLaplacian  (private) — eigensolve + filtering + P/eigenvalues
// ============================================================================

bool MeshEigenBasis::buildFromLaplacian(const CsrGraph& L, int num_modes_req,
                                        const double* area_weights) {
    std::vector<double> rs;
    double rho = 1.0;
    CsrGraph Ls;
    if (area_weights != nullptr) {
        const int n = L.n;
        rs.resize(static_cast<std::size_t>(n));
        double sum_a = 0.0, sum_d = 0.0;
        for (int i = 0; i < n; ++i) {
            const double a = area_weights[i];
            if (!(a > 0.0)) { last_error = 1; return false; }
            rs[static_cast<std::size_t>(i)] = 1.0 / std::sqrt(a);
            sum_a += a;
            for (int p = L.row_ptr[static_cast<std::size_t>(i)];
                 p < L.row_ptr[static_cast<std::size_t>(i) + 1]; ++p)
                if (L.col_idx[static_cast<std::size_t>(p)] == i)
                    sum_d += L.values[static_cast<std::size_t>(p)];
        }
        rho = (sum_d > 0.0 && sum_a > 0.0) ? sum_d / sum_a : 1.0;
        Ls = L;
        for (int i = 0; i < n; ++i)
            for (int p = Ls.row_ptr[static_cast<std::size_t>(i)];
                 p < Ls.row_ptr[static_cast<std::size_t>(i) + 1]; ++p) {
                const auto up = static_cast<std::size_t>(p);
                Ls.values[up] *= rs[static_cast<std::size_t>(i)] *
                    rs[static_cast<std::size_t>(Ls.col_idx[up])] / rho;
            }
    }
    const CsrGraph& Lsolve = (area_weights != nullptr) ? Ls : L;
    // GraphEigenBasis over-requests modes (k_req = min(n-1, num_modes_req + 3))
    // so the null-mode filter has spares to discard, runs reorthogonalized
    // Lanczos from a zero-mean ramp start vector — a constant start vector is
    // the Laplacian's null eigenvector and breaks down immediately — solves the
    // tridiagonal system ascending, then keeps the first num_modes_req
    // eigenpairs with λ ≥ null_tol.
    GraphEigenBasis solver;
    solver.null_tol = null_tol;

    if (!solver.build(Lsolve, num_modes_req)) {
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
    if (area_weights != nullptr) {
        const auto un = static_cast<std::size_t>(L.n);
        for (std::size_t j = 0; j < static_cast<std::size_t>(num_kept); ++j) {
            for (std::size_t i = 0; i < un; ++i) P[j * un + i] *= rs[i];
            eigenvalues[j] *= rho;
        }
        area.assign(area_weights, area_weights + un);
        inv_sqrt_area = std::move(rs);
        area_weighted = true;
    } else {
        area.clear();
        inv_sqrt_area.clear();
        area_weighted = false;
    }
    return true;
}

// ============================================================================
// build()
// ============================================================================

bool MeshEigenBasis::build(const MeshData& mesh, int num_modes_req,
                           const double* ground_w,
                           const double* area_weights) {
    last_error     = 0;
    n_triangles    = mesh.n_triangles();
    num_kept       = 0;
    num_null       = 0;
    depth_weighted = false;

    if (n_triangles < 4) { last_error = 1; return false; }

    return buildFromLaplacian(buildGeometricLaplacian(mesh, ground_w),
                              num_modes_req, area_weights);
}

void MeshEigenBasis::projectArea(const double* field, double* out) const {
    const auto un = static_cast<std::size_t>(n_triangles);
    for (std::size_t j = 0; j < static_cast<std::size_t>(num_kept); ++j) {
        double s = 0.0;
        for (std::size_t i = 0; i < un; ++i)
            s += P[j * un + i] * (area_weighted ? area[i] : 1.0) * field[i];
        out[j] = s;
    }
}

// ============================================================================
// buildDepthWeighted()
// ============================================================================

bool MeshEigenBasis::buildDepthWeighted(const MeshData& mesh,
                                        int num_modes_req,
                                        const double* D_cell,
                                        const double* ground_w) {
    if (n_triangles != mesh.n_triangles() || n_triangles < 4) return false;
    if (!D_cell) return false;

    // Preserve existing basis on failure: save current state
    auto P_save         = P;
    auto ev_save        = eigenvalues;
    int  kept_save      = num_kept;
    int  null_save      = num_null;
    int  num_modes_save = num_modes;
    auto area_save = area; auto isa_save = inv_sqrt_area; bool aw_save = area_weighted;

    last_error = 0;
    bool ok = buildFromLaplacian(
        buildDepthWeightedLaplacian(mesh, D_cell, ground_w), num_modes_req);
    if (ok) {
        depth_weighted = true;
    } else {
        // Restore previous basis unchanged
        P           = std::move(P_save);
        eigenvalues = std::move(ev_save);
        num_kept    = kept_save;
        num_null    = null_save;
        num_modes   = num_modes_save;
        area = std::move(area_save); inv_sqrt_area = std::move(isa_save); area_weighted = aw_save;
    }
    return ok;
}

} // namespace openswmm::twoD
