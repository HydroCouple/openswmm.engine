/**
 * @file SpectralPrecond2D.cpp
 * @brief Spectral Laplacian preconditioner — implementation.
 *
 * @see SpectralPrecond2D.hpp
 * @ingroup engine_2d
 */

#include "SpectralPrecond2D.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numeric>
#include <limits>

namespace openswmm::twoD {

// ============================================================================
// CSR helpers
// ============================================================================

static void csr2d_matvec(const Csr2D& A, const double* x, double* y) {
    for (int i = 0; i < A.n; ++i) {
        double s = 0.0;
        for (int p = A.row_ptr[i]; p < A.row_ptr[i + 1]; ++p)
            s += A.values[static_cast<std::size_t>(p)]
               * x[static_cast<std::size_t>(A.col_idx[static_cast<std::size_t>(p)])];
        y[static_cast<std::size_t>(i)] = s;
    }
}

static Csr2D coo_to_csr2d(std::vector<std::vector<std::pair<int,double>>>& coo, int n) {
    Csr2D M;
    M.n = n;
    M.row_ptr.resize(static_cast<std::size_t>(n + 1), 0);

    for (int i = 0; i < n; ++i) {
        auto& row = coo[static_cast<std::size_t>(i)];
        std::sort(row.begin(), row.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });
        std::vector<std::pair<int,double>> merged;
        for (auto& [col, val] : row) {
            if (!merged.empty() && merged.back().first == col)
                merged.back().second += val;
            else
                merged.emplace_back(col, val);
        }
        row = std::move(merged);
        M.row_ptr[static_cast<std::size_t>(i + 1)] =
            M.row_ptr[static_cast<std::size_t>(i)] + static_cast<int>(row.size());
        for (auto& [col, val] : row) {
            M.col_idx.push_back(col);
            M.values.push_back(val);
        }
    }
    return M;
}

// ============================================================================
// Geometric Laplacian
// ============================================================================

Csr2D SpectralPrecond2D::buildGeometricLaplacian(const MeshData& mesh) const {
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
    return coo_to_csr2d(coo, n);
}

// ============================================================================
// Symmetric tridiagonal QL eigensolver (tqli, Numerical Recipes style)
// Eigenvalues returned in ascending order; Z columns are the eigenvectors.
// ============================================================================

bool SpectralPrecond2D::symTridiagQL(std::vector<double>& diag,
                                      std::vector<double>& subdiag,
                                      std::vector<double>& Z, int m) {
    const int max_iter = 30;

    // e[j] = subdiag[j+1] for j=0..m-2; e[m-1]=0
    std::vector<double> e(static_cast<std::size_t>(m), 0.0);
    for (int j = 0; j < m - 1; ++j)
        e[static_cast<std::size_t>(j)] = subdiag[static_cast<std::size_t>(j + 1)];

    for (int l = 0; l < m; ++l) {
        int iter = 0, lm;
        do {
            lm = l;
            while (lm < m - 1) {
                double dd = std::fabs(diag[static_cast<std::size_t>(lm)])
                          + std::fabs(diag[static_cast<std::size_t>(lm + 1)]);
                if (std::fabs(e[static_cast<std::size_t>(lm)]) <= 1e-15 * dd) break;
                ++lm;
            }
            if (lm == l) break;
            if (iter >= max_iter) return false;
            ++iter;

            double g = (diag[static_cast<std::size_t>(l + 1)]
                      - diag[static_cast<std::size_t>(l)])
                     / (2.0 * e[static_cast<std::size_t>(l)]);
            double r = std::sqrt(g*g + 1.0);
            double sign_g = (g >= 0.0) ? 1.0 : -1.0;
            g = diag[static_cast<std::size_t>(lm)]
              - diag[static_cast<std::size_t>(l)]
              + e[static_cast<std::size_t>(l)] / (g + sign_g * r);

            double s = 1.0, c = 1.0, p = 0.0;
            for (int i = lm - 1; i >= l; --i) {
                auto ui  = static_cast<std::size_t>(i);
                auto ui1 = static_cast<std::size_t>(i + 1);
                double f = s * e[ui];
                double b = c * e[ui];
                e[ui1] = (r = std::sqrt(f*f + g*g));
                if (r == 0.0) {
                    diag[ui1] -= p;
                    e[static_cast<std::size_t>(lm)] = 0.0;
                    break;
                }
                s = f / r; c = g / r;
                g = diag[ui1] - p;
                r = (diag[ui] - g) * s + 2.0 * c * b;
                p = s * r;
                diag[ui1] = g + p;
                g = c * r - b;
                for (int k = 0; k < m; ++k) {
                    auto uk = static_cast<std::size_t>(k);
                    double tmp = Z[uk * static_cast<std::size_t>(m) + ui1];
                    Z[uk * static_cast<std::size_t>(m) + ui1]
                        = s * Z[uk * static_cast<std::size_t>(m) + ui] + c * tmp;
                    Z[uk * static_cast<std::size_t>(m) + ui]
                        = c * Z[uk * static_cast<std::size_t>(m) + ui] - s * tmp;
                }
            }
            if (r == 0.0 && (lm - 1) >= l) continue;  // NOLINT
            diag[static_cast<std::size_t>(l)] -= p;
            e[static_cast<std::size_t>(l)] = g;
            e[static_cast<std::size_t>(lm)] = 0.0;
        } while (lm != l);
    }

    // Sort eigenvalues ascending (insertion sort — m is small)
    for (int i = 1; i < m; ++i) {
        double key = diag[static_cast<std::size_t>(i)];
        std::vector<double> col_tmp(static_cast<std::size_t>(m));
        for (int k = 0; k < m; ++k)
            col_tmp[static_cast<std::size_t>(k)] =
                Z[static_cast<std::size_t>(k) * static_cast<std::size_t>(m)
                  + static_cast<std::size_t>(i)];
        int j = i - 1;
        while (j >= 0 && diag[static_cast<std::size_t>(j)] > key) {
            diag[static_cast<std::size_t>(j + 1)] = diag[static_cast<std::size_t>(j)];
            for (int k = 0; k < m; ++k)
                Z[static_cast<std::size_t>(k) * static_cast<std::size_t>(m)
                  + static_cast<std::size_t>(j + 1)]
                = Z[static_cast<std::size_t>(k) * static_cast<std::size_t>(m)
                  + static_cast<std::size_t>(j)];
            --j;
        }
        diag[static_cast<std::size_t>(j + 1)] = key;
        for (int k = 0; k < m; ++k)
            Z[static_cast<std::size_t>(k) * static_cast<std::size_t>(m)
              + static_cast<std::size_t>(j + 1)] = col_tmp[static_cast<std::size_t>(k)];
    }
    return true;
}

// ============================================================================
// Lanczos eigensolver — k smallest eigenpairs of the Laplacian L
// ============================================================================

bool SpectralPrecond2D::lanczos(const Csr2D& L, int k_want,
                                 std::vector<double>& eigvals_out,
                                 std::vector<double>& eigvecs_out) {
    int n = L.n;
    if (n < 2 || k_want < 1) return false;

    // Krylov dimension: [k_want+1, min(n, 3*k_want+15)]
    int m = std::min(n, std::max(k_want + 1, 3 * k_want + 15));

    // Lanczos vectors V: n × (m+1), column-major
    std::vector<double> V(static_cast<std::size_t>(n) *
                          static_cast<std::size_t>(m + 1), 0.0);
    std::vector<double> alpha(static_cast<std::size_t>(m), 0.0);
    std::vector<double> beta (static_cast<std::size_t>(m), 0.0);

    // Starting vector: zero-mean linear ramp, orthogonal to the Laplacian
    // null space (constant vector 1/sqrt(n) would cause immediate breakdown).
    {
        double mean = (n - 1) * 0.5;
        double sq = 0.0;
        for (int i = 0; i < n; ++i) {
            double v = static_cast<double>(i) - mean;
            V[static_cast<std::size_t>(i)] = v;
            sq += v * v;
        }
        if (sq < 1e-30) return false;
        double inv = 1.0 / std::sqrt(sq);
        for (int i = 0; i < n; ++i)
            V[static_cast<std::size_t>(i)] *= inv;
    }

    std::vector<double> w(static_cast<std::size_t>(n), 0.0);
    int m_actual = 0;

    for (int j = 0; j < m; ++j) {
        auto uj = static_cast<std::size_t>(j);
        const double* vj = &V[uj * static_cast<std::size_t>(n)];

        // w = L * v_j
        csr2d_matvec(L, vj, w.data());

        // alpha_j = v_j^T * w
        double a = 0.0;
        for (int i = 0; i < n; ++i)
            a += vj[static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)];
        alpha[uj] = a;

        // w -= alpha_j * v_j
        for (int i = 0; i < n; ++i)
            w[static_cast<std::size_t>(i)] -= a * vj[static_cast<std::size_t>(i)];

        // w -= beta_{j-1} * v_{j-1}
        if (j > 0) {
            double bm1 = beta[uj - 1];
            const double* vjm1 = &V[(uj - 1) * static_cast<std::size_t>(n)];
            for (int i = 0; i < n; ++i)
                w[static_cast<std::size_t>(i)] -= bm1 * vjm1[static_cast<std::size_t>(i)];
        }

        // Full reorthogonalization (two passes for numerical stability)
        for (int pass = 0; pass < 2; ++pass) {
            for (int p = 0; p <= j; ++p) {
                const double* vp = &V[static_cast<std::size_t>(p) *
                                      static_cast<std::size_t>(n)];
                double proj = 0.0;
                for (int i = 0; i < n; ++i)
                    proj += vp[static_cast<std::size_t>(i)] *
                            w[static_cast<std::size_t>(i)];
                for (int i = 0; i < n; ++i)
                    w[static_cast<std::size_t>(i)] -= proj * vp[static_cast<std::size_t>(i)];
            }
        }

        // beta_j = ||w||
        double nrm = 0.0;
        for (int i = 0; i < n; ++i)
            nrm += w[static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)];
        nrm = std::sqrt(nrm);
        beta[uj] = nrm;
        m_actual = j + 1;

        if (nrm < 1e-12) break;  // lucky breakdown

        if (j + 1 < m) {
            double inv_nrm = 1.0 / nrm;
            double* vjp1 = &V[(uj + 1) * static_cast<std::size_t>(n)];
            for (int i = 0; i < n; ++i)
                vjp1[static_cast<std::size_t>(i)] =
                    w[static_cast<std::size_t>(i)] * inv_nrm;
        }
    }

    // Build and solve the m_actual × m_actual symmetric tridiagonal eigenproblem
    std::vector<double> T_diag(static_cast<std::size_t>(m_actual));
    std::vector<double> T_sub(static_cast<std::size_t>(m_actual), 0.0);
    for (int j = 0; j < m_actual; ++j)
        T_diag[static_cast<std::size_t>(j)] = alpha[static_cast<std::size_t>(j)];
    for (int j = 1; j < m_actual; ++j)
        T_sub[static_cast<std::size_t>(j)] = beta[static_cast<std::size_t>(j - 1)];

    std::vector<double> Z(static_cast<std::size_t>(m_actual) *
                          static_cast<std::size_t>(m_actual), 0.0);
    for (int j = 0; j < m_actual; ++j)
        Z[static_cast<std::size_t>(j) * static_cast<std::size_t>(m_actual) +
          static_cast<std::size_t>(j)] = 1.0;

    if (!symTridiagQL(T_diag, T_sub, Z, m_actual)) return false;

    // T_diag now sorted ascending; take first k_want eigenpairs
    int k = std::min(k_want, m_actual);
    eigvals_out.resize(static_cast<std::size_t>(k));
    eigvecs_out.assign(static_cast<std::size_t>(n) *
                       static_cast<std::size_t>(k), 0.0);

    for (int q = 0; q < k; ++q) {
        eigvals_out[static_cast<std::size_t>(q)] = T_diag[static_cast<std::size_t>(q)];
        // Ritz vector y_q = V * Z[:,q]  (Z column q: Z[j*m_actual + q])
        double* yq = &eigvecs_out[static_cast<std::size_t>(q) *
                                   static_cast<std::size_t>(n)];
        for (int j = 0; j < m_actual; ++j) {
            double s_jq = Z[static_cast<std::size_t>(j) *
                            static_cast<std::size_t>(m_actual) +
                            static_cast<std::size_t>(q)];
            const double* vj = &V[static_cast<std::size_t>(j) *
                                   static_cast<std::size_t>(n)];
            for (int i = 0; i < n; ++i)
                yq[static_cast<std::size_t>(i)] +=
                    s_jq * vj[static_cast<std::size_t>(i)];
        }
        // Normalize
        double nrm2 = 0.0;
        for (int i = 0; i < n; ++i)
            nrm2 += yq[static_cast<std::size_t>(i)] * yq[static_cast<std::size_t>(i)];
        if (nrm2 > 1e-30) {
            double inv = 1.0 / std::sqrt(nrm2);
            for (int i = 0; i < n; ++i)
                yq[static_cast<std::size_t>(i)] *= inv;
        }
    }
    return true;
}

// ============================================================================
// buildDepthWeightedLaplacian  (private)
// ============================================================================

Csr2D SpectralPrecond2D::buildDepthWeightedLaplacian(const MeshData& mesh,
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
    return coo_to_csr2d(coo, n);
}

// ============================================================================
// buildFromLaplacian  (private) — Lanczos + filtering + P/eigenvalues
// ============================================================================

bool SpectralPrecond2D::buildFromLaplacian(Csr2D L, int num_modes_req) {
    int k_req = std::min(n_triangles - 1, num_modes_req + 3);
    if (k_req < 1) { last_error = 1; return false; }

    std::vector<double> raw_vals, raw_vecs;
    if (!lanczos(L, k_req, raw_vals, raw_vecs)) {
        last_error = 2;
        return false;
    }

    // Discard null modes (lambda < 1e-8)
    const double null_tol = 1e-8;
    int start    = 0;
    int new_null = 0;
    for (int q = 0; q < static_cast<int>(raw_vals.size()); ++q) {
        if (raw_vals[static_cast<std::size_t>(q)] < null_tol) {
            ++new_null;
            start = q + 1;
        } else {
            break;
        }
    }

    int available = static_cast<int>(raw_vals.size()) - start;
    int new_kept  = std::min(available, num_modes_req);
    if (new_kept < 1) { last_error = 2; return false; }

    // Commit: update P, eigenvalues, counters
    num_null = new_null;
    num_kept = new_kept;
    num_modes = num_modes_req;

    eigenvalues.resize(static_cast<std::size_t>(num_kept));
    P.assign(static_cast<std::size_t>(n_triangles) *
             static_cast<std::size_t>(num_kept), 0.0);

    for (int q = 0; q < num_kept; ++q) {
        eigenvalues[static_cast<std::size_t>(q)] =
            raw_vals[static_cast<std::size_t>(start + q)];
        const double* src = &raw_vecs[static_cast<std::size_t>(start + q) *
                                       static_cast<std::size_t>(n_triangles)];
        double* dst = &P[static_cast<std::size_t>(q) *
                          static_cast<std::size_t>(n_triangles)];
        std::memcpy(dst, src,
                    static_cast<std::size_t>(n_triangles) * sizeof(double));
    }

    c_buf_.resize(static_cast<std::size_t>(num_kept), 0.0);
    return true;
}

// ============================================================================
// build()
// ============================================================================

bool SpectralPrecond2D::build(const MeshData& mesh, int num_modes_req) {
    last_error    = 0;
    n_triangles   = mesh.n_triangles();
    num_kept      = 0;
    num_null      = 0;
    depth_weighted = false;

    if (n_triangles < 4) { last_error = 1; return false; }

    return buildFromLaplacian(buildGeometricLaplacian(mesh), num_modes_req);
}

// ============================================================================
// buildDepthWeighted()
// ============================================================================

bool SpectralPrecond2D::buildDepthWeighted(const MeshData& mesh,
                                           int num_modes_req,
                                           const double* D_cell) {
    if (n_triangles != mesh.n_triangles() || n_triangles < 4) return false;
    if (!D_cell) return false;

    // Preserve existing basis on failure: save current state
    auto P_save          = P;
    auto ev_save         = eigenvalues;
    int  kept_save       = num_kept;
    int  null_save       = num_null;
    int  num_modes_save  = num_modes;

    last_error = 0;
    bool ok = buildFromLaplacian(buildDepthWeightedLaplacian(mesh, D_cell),
                                 num_modes_req);
    if (ok) {
        depth_weighted = true;
    } else {
        // Restore previous basis unchanged
        P            = std::move(P_save);
        eigenvalues  = std::move(ev_save);
        num_kept     = kept_save;
        num_null     = null_save;
        num_modes    = num_modes_save;
        c_buf_.resize(static_cast<std::size_t>(num_kept), 0.0);
    }
    return ok;
}

// ============================================================================
// apply()
// ============================================================================

void SpectralPrecond2D::apply(const double* r, double* z, double gamma) const {
    int n = n_triangles;
    int k = num_kept;

    // c = P^T * r
    for (int ki = 0; ki < k; ++ki) {
        const double* col = P.data() + ki * n;
        double dot = 0.0;
        for (int i = 0; i < n; ++i) dot += col[i] * r[i];
        c_buf_[static_cast<std::size_t>(ki)] = dot;
    }

    // z = r
    std::memcpy(z, r, static_cast<std::size_t>(n) * sizeof(double));

    // z += P * diag(1/(1+gamma*lambda_ki) - 1) * c
    for (int ki = 0; ki < k; ++ki) {
        double lam   = eigenvalues[static_cast<std::size_t>(ki)];
        double scale = 1.0 / (1.0 + gamma * lam) - 1.0;  // negative for lam>0, gamma>0
        double coeff = scale * c_buf_[static_cast<std::size_t>(ki)];
        const double* col = P.data() + ki * n;
        for (int i = 0; i < n; ++i) z[i] += col[i] * coeff;
    }
}

} // namespace openswmm::twoD
