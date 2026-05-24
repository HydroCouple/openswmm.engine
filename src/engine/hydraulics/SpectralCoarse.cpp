/**
 * @file SpectralCoarse.cpp
 * @brief Two-level spectral coarse correction — implementation.
 *
 * All arithmetic is standard C++ with no external dependencies beyond the
 * standard library. Thread safety: all state is instance-scoped; no global
 * mutable variables.
 *
 * @ingroup new_engine
 */

#include "SpectralCoarse.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <cassert>
#include <limits>

namespace openswmm {
namespace dynwave {

// ============================================================================
// CSR helpers
// ============================================================================

void csr_matvec(const CsrMatrix& A, const double* x, double* y) {
    for (int i = 0; i < A.n; ++i) {
        double s = 0.0;
        for (int p = A.row_ptr[static_cast<std::size_t>(i)];
                 p < A.row_ptr[static_cast<std::size_t>(i + 1)]; ++p) {
            s += A.values[static_cast<std::size_t>(p)]
               * x[static_cast<std::size_t>(A.col_idx[static_cast<std::size_t>(p)])];
        }
        y[static_cast<std::size_t>(i)] = s;
    }
}

void csr_add_entry(std::vector<std::vector<std::pair<int,double>>>& coo,
                   int i, int j, double v) {
    coo[static_cast<std::size_t>(i)].emplace_back(j, v);
    if (i != j)
        coo[static_cast<std::size_t>(j)].emplace_back(i, v);
}

CsrMatrix coo_to_csr(std::vector<std::vector<std::pair<int,double>>>& coo, int n) {
    CsrMatrix M;
    M.n = n;
    M.row_ptr.resize(static_cast<std::size_t>(n + 1), 0);

    // Sort each row and sum duplicates
    for (int i = 0; i < n; ++i) {
        auto& row = coo[static_cast<std::size_t>(i)];
        std::sort(row.begin(), row.end(),
                  [](const auto& a, const auto& b){ return a.first < b.first; });
        // Merge duplicates
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
// Symmetric tridiagonal QL eigensolver (based on Numerical Recipes tqli)
// ============================================================================

bool SpectralCoarse::symTridiagQL(std::vector<double>& diag,
                                  std::vector<double>& subdiag,
                                  std::vector<double>& Z,
                                  int m) {
    // diag[0..m-1] = diagonal (a)
    // subdiag[0..m-1]; subdiag[0] unused; subdiag[j] = beta_j for j=1..m-1
    // Z[0..m*m-1] column-major eigenvector accumulator (initialized to I)

    const int max_iter = 30;
    // Shift subdiag to 0-indexed: e[j] = subdiag[j+1] for j=0..m-2; e[m-1]=0
    std::vector<double> e(static_cast<std::size_t>(m), 0.0);
    for (int j = 0; j < m - 1; ++j)
        e[static_cast<std::size_t>(j)] = subdiag[static_cast<std::size_t>(j + 1)];

    for (int l = 0; l < m; ++l) {
        int iter = 0;
        int lm;
        do {
            // Find small subdiagonal element
            lm = l;
            while (lm < m - 1) {
                double dd = std::fabs(diag[static_cast<std::size_t>(lm)])
                          + std::fabs(diag[static_cast<std::size_t>(lm + 1)]);
                if (std::fabs(e[static_cast<std::size_t>(lm)]) <= 1e-15 * dd) break;
                ++lm;
            }
            if (lm == l) break;  // converged

            if (iter >= max_iter) return false;  // non-convergence

            ++iter;
            // Wilkinson shift
            double g = (diag[static_cast<std::size_t>(l + 1)]
                      - diag[static_cast<std::size_t>(l)])
                     / (2.0 * e[static_cast<std::size_t>(l)]);
            double r = std::sqrt(g * g + 1.0);
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
                e[ui1] = (r = std::sqrt(f * f + g * g));
                if (r == 0.0) {
                    diag[ui1] -= p;
                    e[static_cast<std::size_t>(lm)] = 0.0;
                    break;
                }
                s = f / r;
                c = g / r;
                g = diag[ui1] - p;
                r = (diag[ui] - g) * s + 2.0 * c * b;
                p = s * r;
                diag[ui1] = g + p;
                g = c * r - b;

                // Accumulate rotations into Z (column-major: Z[k*m + i])
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
        int j = i - 1;
        // Also move corresponding column of Z
        std::vector<double> col_tmp(static_cast<std::size_t>(m));
        for (int k = 0; k < m; ++k)
            col_tmp[static_cast<std::size_t>(k)] =
                Z[static_cast<std::size_t>(k) * static_cast<std::size_t>(m)
                  + static_cast<std::size_t>(i)];
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
// LU factorization and solve (partial pivoting)
// ============================================================================

bool SpectralCoarse::luFactor(std::vector<double>& A, std::vector<int>& piv, int n) {
    // A is n×n column-major. piv length n.
    const double tol = 1e-14;
    for (int k = 0; k < n; ++k) {
        // Find pivot row in column k (from row k..n-1)
        int pivot_row = k;
        double max_val = std::fabs(A[static_cast<std::size_t>(k * n + k)]);
        for (int i = k + 1; i < n; ++i) {
            double v = std::fabs(A[static_cast<std::size_t>(i + k * n)]);
            if (v > max_val) { max_val = v; pivot_row = i; }
        }
        piv[static_cast<std::size_t>(k)] = pivot_row;
        if (max_val < tol) return false;  // singular

        // Swap rows k and pivot_row in all columns
        if (pivot_row != k) {
            for (int j = 0; j < n; ++j) {
                std::swap(A[static_cast<std::size_t>(k + j * n)],
                          A[static_cast<std::size_t>(pivot_row + j * n)]);
            }
        }
        // Eliminate below pivot
        double inv_pivot = 1.0 / A[static_cast<std::size_t>(k + k * n)];
        for (int i = k + 1; i < n; ++i) {
            A[static_cast<std::size_t>(i + k * n)] *= inv_pivot;  // multiplier stored in L
            for (int j = k + 1; j < n; ++j)
                A[static_cast<std::size_t>(i + j * n)] -=
                    A[static_cast<std::size_t>(i + k * n)] *
                    A[static_cast<std::size_t>(k + j * n)];
        }
    }
    return true;
}

void SpectralCoarse::luSolve(const std::vector<double>& A,
                             const std::vector<int>& piv,
                             std::vector<double>& b, int n) {
    // Forward substitution with row swaps (L part, diagonal = 1)
    for (int k = 0; k < n; ++k) {
        if (piv[static_cast<std::size_t>(k)] != k)
            std::swap(b[static_cast<std::size_t>(k)],
                      b[static_cast<std::size_t>(piv[static_cast<std::size_t>(k)])]);
        for (int i = k + 1; i < n; ++i)
            b[static_cast<std::size_t>(i)] -=
                A[static_cast<std::size_t>(i + k * n)] * b[static_cast<std::size_t>(k)];
    }
    // Back substitution (U part)
    for (int k = n - 1; k >= 0; --k) {
        b[static_cast<std::size_t>(k)] /= A[static_cast<std::size_t>(k + k * n)];
        for (int i = 0; i < k; ++i)
            b[static_cast<std::size_t>(i)] -=
                A[static_cast<std::size_t>(i + k * n)] * b[static_cast<std::size_t>(k)];
    }
}

// ============================================================================
// Laplacian construction
// ============================================================================

CsrMatrix SpectralCoarse::buildTopologicalLaplacian(int n_active, int n_conduits,
                                                     const int* conduit_n1,
                                                     const int* conduit_n2) const {
    // COO accumulation: row i holds (col, val) pairs
    std::vector<std::vector<std::pair<int,double>>> coo(
        static_cast<std::size_t>(n_active));

    for (int k = 0; k < n_conduits; ++k) {
        int fn1 = conduit_n1[static_cast<std::size_t>(k)];
        int fn2 = conduit_n2[static_cast<std::size_t>(k)];

        // Skip if either endpoint is an outfall (not in active subspace)
        int a1 = (fn1 >= 0 && fn1 < N) ? full_to_active[static_cast<std::size_t>(fn1)] : -1;
        int a2 = (fn2 >= 0 && fn2 < N) ? full_to_active[static_cast<std::size_t>(fn2)] : -1;

        if (a1 < 0 && a2 < 0) continue;  // both endpoints excluded

        // Add diagonal +1 for each active endpoint
        if (a1 >= 0) coo[static_cast<std::size_t>(a1)].emplace_back(a1, 1.0);
        if (a2 >= 0) coo[static_cast<std::size_t>(a2)].emplace_back(a2, 1.0);

        // Add off-diagonal -1 only if both active
        if (a1 >= 0 && a2 >= 0) {
            coo[static_cast<std::size_t>(a1)].emplace_back(a2, -1.0);
            coo[static_cast<std::size_t>(a2)].emplace_back(a1, -1.0);
        }
    }

    return coo_to_csr(coo, n_active);
}

// ============================================================================
// Lanczos eigensolver
// ============================================================================

bool SpectralCoarse::lanczos(const CsrMatrix& L, int k_want,
                              std::vector<double>& eigvals_out,
                              std::vector<double>& eigvecs_out) {
    int n = L.n;
    if (n < 2 || k_want < 1) return false;

    // Number of Lanczos steps: clamp to [k_want+1, min(n, 3*k_want+15)]
    int m = std::min(n, std::max(k_want + 1, 3 * k_want + 15));

    // V: n × (m+1) Lanczos vectors, column-major
    std::vector<double> V(static_cast<std::size_t>(n) *
                          static_cast<std::size_t>(m + 1), 0.0);
    std::vector<double> alpha(static_cast<std::size_t>(m), 0.0);
    std::vector<double> beta(static_cast<std::size_t>(m), 0.0);  // beta[j] = ||w_j||

    // Deterministic starting vector: linearly varying with zero mean.
    // Using 1/sqrt(n) (the constant vector) would be the null eigenvector of L,
    // causing immediate Lanczos breakdown with only the null mode discovered.
    // A zero-mean linear ramp is orthogonal to the null space of any connected
    // graph Laplacian, ensuring the first Krylov step explores the non-null space.
    {
        double mean = (n - 1) * 0.5;
        double sq = 0.0;
        for (int i = 0; i < n; ++i) {
            double v = static_cast<double>(i) - mean;
            V[static_cast<std::size_t>(i)] = v;
            sq += v * v;
        }
        if (sq < 1e-30) {
            // Degenerate (n == 1): shouldn't reach here, but be safe
            V[0] = 1.0;
        } else {
            double inv = 1.0 / std::sqrt(sq);
            for (int i = 0; i < n; ++i)
                V[static_cast<std::size_t>(i)] *= inv;
        }
    }

    int m_actual = 0;  // actual number of steps taken (may terminate early)

    std::vector<double> w(static_cast<std::size_t>(n), 0.0);

    for (int j = 0; j < m; ++j) {
        auto uj = static_cast<std::size_t>(j);

        // w = L * v_j
        const double* vj = &V[uj * static_cast<std::size_t>(n)];
        csr_matvec(L, vj, w.data());

        // alpha_j = v_j^T * w
        double a = 0.0;
        for (int i = 0; i < n; ++i)
            a += vj[static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)];
        alpha[uj] = a;

        // w -= alpha_j * v_j  +  beta_{j-1} * v_{j-1}
        for (int i = 0; i < n; ++i)
            w[static_cast<std::size_t>(i)] -=
                a * vj[static_cast<std::size_t>(i)];
        if (j > 0) {
            const double* vjm1 = &V[(uj - 1) * static_cast<std::size_t>(n)];
            double bm1 = beta[uj - 1];
            for (int i = 0; i < n; ++i)
                w[static_cast<std::size_t>(i)] -=
                    bm1 * vjm1[static_cast<std::size_t>(i)];
        }

        // Full reorthogonalization (two passes for stability)
        for (int pass = 0; pass < 2; ++pass) {
            for (int p = 0; p <= j; ++p) {
                const double* vp = &V[static_cast<std::size_t>(p) *
                                      static_cast<std::size_t>(n)];
                double proj = 0.0;
                for (int i = 0; i < n; ++i)
                    proj += vp[static_cast<std::size_t>(i)] *
                            w[static_cast<std::size_t>(i)];
                for (int i = 0; i < n; ++i)
                    w[static_cast<std::size_t>(i)] -=
                        proj * vp[static_cast<std::size_t>(i)];
            }
        }

        // beta_j = ||w||
        double nrm = 0.0;
        for (int i = 0; i < n; ++i)
            nrm += w[static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)];
        nrm = std::sqrt(nrm);
        beta[uj] = nrm;
        m_actual = j + 1;

        if (nrm < 1e-12) break;  // lucky breakdown — invariant subspace found

        if (j + 1 < m) {
            // v_{j+1} = w / beta_j
            double inv_nrm = 1.0 / nrm;
            double* vjp1 = &V[(uj + 1) * static_cast<std::size_t>(n)];
            for (int i = 0; i < n; ++i)
                vjp1[static_cast<std::size_t>(i)] =
                    w[static_cast<std::size_t>(i)] * inv_nrm;
        }
    }

    // Build the m_actual × m_actual tridiagonal eigenproblem
    // T: diag = alpha[0..m_actual-1], subdiag[j] = beta[j-1] for j=1..m_actual-1
    std::vector<double> T_diag(static_cast<std::size_t>(m_actual));
    std::vector<double> T_sub(static_cast<std::size_t>(m_actual), 0.0);
    for (int j = 0; j < m_actual; ++j)
        T_diag[static_cast<std::size_t>(j)] = alpha[static_cast<std::size_t>(j)];
    for (int j = 1; j < m_actual; ++j)
        T_sub[static_cast<std::size_t>(j)] = beta[static_cast<std::size_t>(j - 1)];

    // Z = identity (m_actual × m_actual, column-major)
    std::vector<double> Z(static_cast<std::size_t>(m_actual) *
                          static_cast<std::size_t>(m_actual), 0.0);
    for (int j = 0; j < m_actual; ++j)
        Z[static_cast<std::size_t>(j) * static_cast<std::size_t>(m_actual) +
          static_cast<std::size_t>(j)] = 1.0;

    if (!symTridiagQL(T_diag, T_sub, Z, m_actual)) return false;

    // T_diag is now sorted ascending. Take first k_want eigenpairs.
    int k = std::min(k_want, m_actual);
    eigvals_out.resize(static_cast<std::size_t>(k));
    eigvecs_out.assign(static_cast<std::size_t>(n) * static_cast<std::size_t>(k), 0.0);

    for (int q = 0; q < k; ++q) {
        eigvals_out[static_cast<std::size_t>(q)] = T_diag[static_cast<std::size_t>(q)];
        // Ritz vector y_q = V * s_q  where s_q = Z[:, q] (column of Z)
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
        // Normalize (Ritz vectors may not be exactly unit-norm due to FP)
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
// SpectralCoarse::init
// ============================================================================

bool SpectralCoarse::init(int N_full, int n_conduits,
                           const int* is_outfall,
                           const int* conduit_n1,
                           const int* conduit_n2,
                           int num_modes_req) {
    enabled   = 0;
    last_error = 0;
    N = N_full;
    num_modes = num_modes_req;

    // Build active node index maps
    active_map.clear();
    full_to_active.assign(static_cast<std::size_t>(N), -1);
    for (int i = 0; i < N; ++i) {
        if (!is_outfall[static_cast<std::size_t>(i)]) {
            full_to_active[static_cast<std::size_t>(i)] =
                static_cast<int>(active_map.size());
            active_map.push_back(i);
        }
    }
    n_active = static_cast<int>(active_map.size());

    if (n_active < 2) { last_error = 1; return false; }

    // Build unweighted topological Laplacian
    CsrMatrix L = buildTopologicalLaplacian(n_active, n_conduits,
                                            conduit_n1, conduit_n2);

    // Request num_modes + 3 eigenpairs (buffer for null modes)
    int k_req = std::min(n_active - 1, num_modes + 3);
    if (k_req < 1) { last_error = 1; return false; }

    std::vector<double> raw_vals, raw_vecs;
    if (!lanczos(L, k_req, raw_vals, raw_vecs)) {
        last_error = 2;
        return false;
    }

    // Discard null modes (eigenvalue < null_tol), retain up to num_modes nontrivial
    num_null = 0;
    int start = 0;
    for (int q = 0; q < static_cast<int>(raw_vals.size()); ++q) {
        if (raw_vals[static_cast<std::size_t>(q)] < null_tol) {
            ++num_null;
            start = q + 1;
        } else {
            break;
        }
    }

    int available = static_cast<int>(raw_vals.size()) - start;
    num_kept = std::min(available, num_modes);

    if (num_kept < 1) { last_error = 4; return false; }

    // Store retained eigenvalues and eigenvectors in P (n_active × num_kept, col-major)
    eigenvalues.resize(static_cast<std::size_t>(num_kept));
    P.assign(static_cast<std::size_t>(n_active) *
             static_cast<std::size_t>(num_kept), 0.0);

    for (int q = 0; q < num_kept; ++q) {
        eigenvalues[static_cast<std::size_t>(q)] =
            raw_vals[static_cast<std::size_t>(start + q)];
        // raw_vecs: n_active × k_req column-major; column (start+q) → P column q
        const double* src = &raw_vecs[static_cast<std::size_t>(start + q) *
                                       static_cast<std::size_t>(n_active)];
        double* dst = &P[static_cast<std::size_t>(q) *
                          static_cast<std::size_t>(n_active)];
        for (int i = 0; i < n_active; ++i)
            dst[static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(i)];
    }

    // Eigengap: ratio of first two retained eigenvalues (quality indicator)
    if (num_kept >= 2 && eigenvalues[0] > 1e-12)
        eigengap = eigenvalues[1] / eigenvalues[0];
    else if (num_kept >= 2)
        eigengap = eigenvalues[1];  // first is near zero
    else
        eigengap = eigenvalues[0];

    // Pre-allocate working vectors and coarse operator space
    work_coarse.resize(static_cast<std::size_t>(num_kept), 0.0);
    work_fine.resize(static_cast<std::size_t>(n_active), 0.0);
    A_coarse.assign(static_cast<std::size_t>(num_kept) *
                    static_cast<std::size_t>(num_kept), 0.0);
    A_coarse_lu.assign(static_cast<std::size_t>(num_kept) *
                       static_cast<std::size_t>(num_kept), 0.0);
    A_coarse_piv.assign(static_cast<std::size_t>(num_kept), 0);
    W_work.assign(static_cast<std::size_t>(n_active) *
                  static_cast<std::size_t>(num_kept), 0.0);

    enabled = 1;
    return true;
}

// ============================================================================
// SpectralCoarse::assembleCoarseOperator
// ============================================================================

bool SpectralCoarse::assembleCoarseOperator(const double* active_diag,
                                             int n_conduits,
                                             const int* conduit_n1,
                                             const int* conduit_n2,
                                             const double* conduit_off) {
    if (!enabled) return false;

    // Build A_fine as a CSR matrix (assembled from diagonal + off-diagonal contributions)
    // We compute W = A_fine * P column by column: for each column q of P,
    //   W[:,q] = A_fine * P[:,q]
    // A_fine[i][i] = active_diag[i]
    // A_fine[i][j] = -conduit_off[k]  for conduit k connecting active nodes a1,a2
    //                                  (contribution to both A[a1][a2] and A[a2][a1])

    auto an = static_cast<std::size_t>(n_active);
    auto kn = static_cast<std::size_t>(num_kept);

    // W = A_fine * P: compute column-by-column
    for (std::size_t q = 0; q < kn; ++q) {
        const double* Pq = &P[q * an];
        double* Wq = &W_work[q * an];

        // Diagonal contribution: W[:,q] = diag * P[:,q]
        for (int i = 0; i < n_active; ++i)
            Wq[static_cast<std::size_t>(i)] =
                active_diag[static_cast<std::size_t>(i)] *
                Pq[static_cast<std::size_t>(i)];

        // Off-diagonal contribution from conduits
        for (int k = 0; k < n_conduits; ++k) {
            int fn1 = conduit_n1[static_cast<std::size_t>(k)];
            int fn2 = conduit_n2[static_cast<std::size_t>(k)];
            int a1 = (fn1 >= 0 && fn1 < N) ?
                     full_to_active[static_cast<std::size_t>(fn1)] : -1;
            int a2 = (fn2 >= 0 && fn2 < N) ?
                     full_to_active[static_cast<std::size_t>(fn2)] : -1;
            if (a1 < 0 || a2 < 0) continue;  // boundary conduit — skip

            double c = conduit_off[static_cast<std::size_t>(k)];
            // A_fine off-diagonal: A[a1][a2] = A[a2][a1] = -c
            // So (A_fine * P)[a1][q] += -c * P[a2][q]  and vice versa
            Wq[static_cast<std::size_t>(a1)] -= c * Pq[static_cast<std::size_t>(a2)];
            Wq[static_cast<std::size_t>(a2)] -= c * Pq[static_cast<std::size_t>(a1)];
        }
    }

    // A_coarse = P^T * W  (num_kept × num_kept, column-major)
    for (std::size_t q = 0; q < kn; ++q) {
        const double* Wq = &W_work[q * an];
        for (std::size_t p = 0; p < kn; ++p) {
            const double* Pp = &P[p * an];
            double dot = 0.0;
            for (int i = 0; i < n_active; ++i)
                dot += Pp[static_cast<std::size_t>(i)] *
                       Wq[static_cast<std::size_t>(i)];
            // A_coarse[p][q] (column-major: A_coarse[p + q*kn])
            A_coarse[p + q * kn] = dot;
        }
    }

    // Factor A_coarse
    A_coarse_lu = A_coarse;
    if (!luFactor(A_coarse_lu, A_coarse_piv, num_kept)) {
        last_error = 3;
        return false;
    }
    return true;
}

// ============================================================================
// SpectralCoarse::applyCorrection
// ============================================================================

double SpectralCoarse::applyCorrection(const double* depth_resid,
                                        double* correction_out,
                                        double head_tol) {
    ++corrections_attempted;

    auto an = static_cast<std::size_t>(n_active);
    auto kn = static_cast<std::size_t>(num_kept);

    // r_coarse = P^T * r_fine  (restriction)
    std::fill(work_coarse.begin(), work_coarse.end(), 0.0);
    for (std::size_t q = 0; q < kn; ++q) {
        const double* Pq = &P[q * an];
        double dot = 0.0;
        for (int i = 0; i < n_active; ++i)
            dot += Pq[static_cast<std::size_t>(i)] *
                   depth_resid[static_cast<std::size_t>(i)];
        work_coarse[q] = dot;
    }

    // Solve A_coarse * e_coarse = r_coarse
    luSolve(A_coarse_lu, A_coarse_piv, work_coarse, num_kept);
    // work_coarse now holds e_coarse

    // Prolongate: delta_fine = P * e_coarse
    std::fill(work_fine.begin(), work_fine.end(), 0.0);
    for (std::size_t q = 0; q < kn; ++q) {
        const double* Pq = &P[q * an];
        double eq = work_coarse[q];
        for (int i = 0; i < n_active; ++i)
            work_fine[static_cast<std::size_t>(i)] +=
                eq * Pq[static_cast<std::size_t>(i)];
    }

    // Per-node backtracking: at EVERY node the predicted residual must not increase.
    // r_trial[i] = r[i] - omega*delta[i] (first-order: delta is in the A_fine eigenspace).
    // The + head_tol slack permits small corrections at already-converged nodes
    // (where |r[i]| ≈ 0) without false rejections.
    // Global max-norm is insufficient: a smooth correction can improve the worst node
    // while corrupting many near-converged nodes, passing the global check while
    // diverging the solution over multiple corrections.
    const double omegas[3] = {1.0, 0.5, 0.25};
    const double tol_factor = 1.0 + accept_tol;

    for (double omega : omegas) {
        bool ok = true;
        for (int i = 0; i < n_active; ++i) {
            double ri      = depth_resid[static_cast<std::size_t>(i)];
            double r_trial = ri - omega * work_fine[static_cast<std::size_t>(i)];
            double limit   = std::fabs(ri) * tol_factor + head_tol;
            if (std::fabs(r_trial) > limit) { ok = false; break; }
        }
        if (ok) {
            for (int i = 0; i < n_active; ++i)
                correction_out[static_cast<std::size_t>(i)] =
                    omega * work_fine[static_cast<std::size_t>(i)];
            ++corrections_accepted;
            return omega;
        }
    }

    // Reject
    std::fill(correction_out, correction_out + n_active, 0.0);
    ++corrections_rejected;
    return 0.0;
}

// ============================================================================
// SpectralCoarse::reset
// ============================================================================

void SpectralCoarse::reset() {
    corrections_attempted = 0;
    corrections_accepted  = 0;
    corrections_rejected  = 0;
}

} // namespace dynwave
} // namespace openswmm
