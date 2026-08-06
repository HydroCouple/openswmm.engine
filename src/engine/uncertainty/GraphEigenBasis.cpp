/**
 * @file GraphEigenBasis.cpp
 * @brief Generic graph Laplacian eigenbasis — implementation.
 *
 * @ingroup engine_uncertainty
 */

#include "GraphEigenBasis.hpp"
#include "LhsShuffle.hpp"   // splitmix64 — deterministic cold-start vector

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numeric>
#include <limits>

namespace openswmm::uncertainty {

namespace {

/// Fixed seed for the Lanczos cold-start vector (see lanczos()). Fixed, not
/// derived from time/address/config, so every run of every build produces a
/// bit-identical basis — the engine's determinism guarantees (and the
/// `TwoRunsAreIdentical` regression) depend on it.
constexpr uint64_t kColdStartSeed = UINT64_C(0x5eed10a5ec0d57a7);

/// Relative magnitude of the pseudo-random symmetry-breaking term in the
/// cold-start vector (see lanczos()). Small enough that the smooth ramp +
/// quadratic dominate — preserving fast convergence to the low eigenmodes —
/// but nonzero so no exact graph symmetry can confine the Krylov space.
constexpr double kSymmetryBreakEps = 1.0e-3;

}  // namespace

// ============================================================================
// CSR helpers
// ============================================================================

void csr_matvec(const CsrGraph& A, const double* x, double* y) {
    for (int i = 0; i < A.n; ++i) {
        double s = 0.0;
        for (int p = A.row_ptr[i]; p < A.row_ptr[i + 1]; ++p)
            s += A.values[static_cast<std::size_t>(p)]
               * x[static_cast<std::size_t>(A.col_idx[static_cast<std::size_t>(p)])];
        y[static_cast<std::size_t>(i)] = s;
    }
}

CsrGraph coo_to_csr(std::vector<std::vector<std::pair<int,double>>>& coo, int n) {
    CsrGraph M;
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
// symTridiagQL — symmetric tridiagonal QL eigensolver (Numerical Recipes tqli)
// Eigenvalues returned in ascending order; Z columns are the eigenvectors.
// ============================================================================

bool GraphEigenBasis::symTridiagQL(std::vector<double>& diag,
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
// lanczos — k smallest eigenpairs of symmetric PSD Laplacian L
// ============================================================================

bool GraphEigenBasis::lanczos(const CsrGraph& L, int k_want,
                               std::vector<double>& eigvals_out,
                               std::vector<double>& eigvecs_out,
                               const double* v0) {
    int n = L.n;
    if (n < 2 || k_want < 1) return false;

    // Krylov dimension: [k_want+1, min(n, 3*k_want+15)]
    int m = std::min(n, std::max(k_want + 1, 3 * k_want + 15));

    // Lanczos vectors V: n × (m+1), column-major
    std::vector<double> V(static_cast<std::size_t>(n) *
                          static_cast<std::size_t>(m + 1), 0.0);
    std::vector<double> alpha(static_cast<std::size_t>(m), 0.0);
    std::vector<double> beta (static_cast<std::size_t>(m), 0.0);

    // Starting vector.
    // Warm-start: use v0 (first column of v0_block) when provided.
    // Cold-start: zero-mean deterministic pseudo-random vector (see below).
    if (v0 != nullptr) {
        // Copy v0 and normalize, with fallback to cold-start on zero norm.
        double sq = 0.0;
        for (int i = 0; i < n; ++i)
            sq += v0[static_cast<std::size_t>(i)] * v0[static_cast<std::size_t>(i)];
        if (sq > 1e-30) {
            double inv = 1.0 / std::sqrt(sq);
            for (int i = 0; i < n; ++i)
                V[static_cast<std::size_t>(i)] =
                    v0[static_cast<std::size_t>(i)] * inv;
        } else {
            v0 = nullptr;  // degenerate warm-start → fall through to cold-start
        }
    }
    if (v0 == nullptr) {
        // Cold start: zero-mean SMOOTH ramp + SYMMETRIC quadratic companion,
        // plus a tiny deterministic pseudo-random perturbation.
        //
        // Why not the bare ramp (the original): `r[i] = i − (n−1)/2` is
        // ANTISYMMETRIC under the index reversal `i → n−1−i`. Whenever L
        // commutes with that reversal — ANY left-right symmetric graph, a
        // plain chain/path being the canonical case — every Krylov vector
        // L^j·v0 inherits that antisymmetry, so the whole Krylov space is
        // trapped in the antisymmetric invariant subspace and the SYMMETRIC
        // eigenvectors are unreachable at any Krylov dimension, with any
        // amount of reorthogonalization. On an unweighted path graph the
        // eigenvector of λ_k is antisymmetric for odd k and symmetric for
        // even k, so the bare ramp silently returned λ_1,λ_3,λ_5,λ_7,λ_9
        // while *reporting them as the five smallest* — wrong, since
        // λ_2 < λ_3. (Measured pre-fix on n = 10/15/20; n = 6/30 differed
        // only because round-off leaked symmetric components back in, which
        // is luck, not a mechanism.)
        //
        // Why not a pure random vector: it is generic (reaches every
        // eigenvector) but spectrally ROUGH, so at the fixed Krylov budget
        // used here (m ≈ 3k+15, e.g. 45 steps on a 5000-cell mesh) it
        // converges markedly worse to the SMALLEST eigenvalues, which are
        // exactly the ones this basis retains. Measured on the 50×50
        // structured mesh: a pure-random start returned λ_0 = 6.64e-3 where
        // the smooth ramp reached 1.89e-3 — and since Ritz values bound the
        // true eigenvalues from ABOVE, lower is strictly better converged.
        // Smoothness of the start is load-bearing for convergence here, not
        // incidental.
        //
        // So: keep a smooth base, but make it span both parities.
        //   r[i] = i − (n−1)/2                 antisymmetric, smooth
        //   q[i] = r[i]² − mean(r²)            SYMMETRIC, smooth, zero-mean
        // Both are low-frequency, so convergence to the low modes is
        // preserved; their sum lies in neither invariant subspace, so the
        // symmetric eigenvectors are reachable.
        //
        // The small random term (kSymmetryBreakEps, ~1e-3 relative) is
        // insurance against symmetry groups OTHER than index reversal — a
        // star, a symmetric tree, repeated identical branches — where a
        // purely index-constructed vector could still land in an invariant
        // subspace. It is deliberately tiny so it perturbs convergence
        // negligibly while still giving a generically nonzero component on
        // every eigenvector. splitmix64 with a FIXED seed keeps this
        // bit-exactly reproducible run to run and platform to platform (pure
        // uint64 arithmetic — no libm, no std::random distribution whose
        // implementation varies), which the engine's determinism guarantees
        // and the `TwoRunsAreIdentical` regression require.
        //
        // The mean is removed at the end so the start stays orthogonal to the
        // constant vector — the null eigenvector of an ungrounded (Neumann)
        // Laplacian, which would otherwise cause immediate Lanczos breakdown.
        // That is the property the original ramp was chosen for, preserved.
        const double mid = (n - 1) * 0.5;

        // Mean of r² over i, needed to zero-mean the quadratic companion.
        double mean_r2 = 0.0;
        for (int i = 0; i < n; ++i) {
            const double r = static_cast<double>(i) - mid;
            mean_r2 += r * r;
        }
        mean_r2 /= static_cast<double>(n);

        // Scale the companion so ramp and quadratic contribute comparably
        // (both are O(n) and O(n²) raw, so normalize the quadratic by mid).
        const double q_scale = (mid > 0.0) ? (1.0 / std::max(mid, 1.0)) : 1.0;

        uint64_t rng = kColdStartSeed;
        double sum = 0.0;
        for (int i = 0; i < n; ++i) {
            const double r = static_cast<double>(i) - mid;
            const double q = (r * r - mean_r2) * q_scale;
            const uint64_t rb = splitmix64(rng);
            const double u = static_cast<double>(rb >> 11) *
                             (1.0 / 9007199254740992.0);   // [0,1), 2^-53
            const double eps = kSymmetryBreakEps * (2.0 * u - 1.0) * std::max(mid, 1.0);
            const double v = r + q + eps;
            V[static_cast<std::size_t>(i)] = v;
            sum += v;
        }
        const double mean = sum / static_cast<double>(n);
        double sq = 0.0;
        for (int i = 0; i < n; ++i) {
            V[static_cast<std::size_t>(i)] -= mean;
            sq += V[static_cast<std::size_t>(i)] * V[static_cast<std::size_t>(i)];
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
        std::fill(w.begin(), w.end(), 0.0);
        csr_matvec(L, vj, w.data());

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
// build
// ============================================================================

bool GraphEigenBasis::build(const CsrGraph& L, int num_modes_req,
                             const double* v0_block) {
    last_error = 0;
    n_nodes    = L.n;
    num_kept   = 0;
    num_null   = 0;

    if (n_nodes < 4) { last_error = 1; return false; }

    int k_req = std::min(n_nodes - 1, num_modes_req + 3);
    if (k_req < 1) { last_error = 1; return false; }

    // Warm-start: use a uniform linear combination of all v0_block columns as
    // the Krylov starting vector.  A single column risks lucky breakdown when
    // it happens to be a near-exact eigenvector of L (common when the operator
    // changes slowly), leaving only 1 Ritz pair computable.
    std::vector<double> v0_combo;
    if (v0_block != nullptr) {
        v0_combo.assign(static_cast<std::size_t>(n_nodes), 0.0);
        for (int q = 0; q < num_modes_req; ++q)
            for (int i = 0; i < n_nodes; ++i)
                v0_combo[static_cast<std::size_t>(i)] +=
                    v0_block[static_cast<std::size_t>(q) *
                             static_cast<std::size_t>(n_nodes) +
                             static_cast<std::size_t>(i)];
    }
    const double* v0 = v0_combo.empty() ? nullptr : v0_combo.data();

    std::vector<double> raw_vals, raw_vecs;
    if (!lanczos(L, k_req, raw_vals, raw_vecs, v0)) {
        last_error = 2;
        return false;
    }

    // Discard null modes (λ < null_tol)
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

    num_null = new_null;
    num_kept = new_kept;

    eigenvalues.resize(static_cast<std::size_t>(num_kept));
    P.assign(static_cast<std::size_t>(n_nodes) *
             static_cast<std::size_t>(num_kept), 0.0);

    for (int q = 0; q < num_kept; ++q) {
        eigenvalues[static_cast<std::size_t>(q)] =
            raw_vals[static_cast<std::size_t>(start + q)];
        const double* src = &raw_vecs[static_cast<std::size_t>(start + q) *
                                       static_cast<std::size_t>(n_nodes)];
        double* dst = &P[static_cast<std::size_t>(q) *
                          static_cast<std::size_t>(n_nodes)];
        std::memcpy(dst, src,
                    static_cast<std::size_t>(n_nodes) * sizeof(double));
    }

    // Sign alignment: when a warm-start block is provided, flip the sign of
    // each eigenvector so that P[:,j]^T · v0_block[:,j] >= 0.
    // This prevents spurious sign flips between consecutive solves from
    // inverting modal coordinates in the re-projection step.
    if (v0_block != nullptr) {
        for (int q = 0; q < num_kept; ++q) {
            auto uq = static_cast<std::size_t>(q);
            double* pq = &P[uq * static_cast<std::size_t>(n_nodes)];
            const double* ref = v0_block
                              + uq * static_cast<std::size_t>(n_nodes);
            double dot = 0.0;
            for (int i = 0; i < n_nodes; ++i)
                dot += pq[static_cast<std::size_t>(i)]
                     * ref[static_cast<std::size_t>(i)];
            if (dot < 0.0) {
                for (int i = 0; i < n_nodes; ++i)
                    pq[static_cast<std::size_t>(i)] =
                        -pq[static_cast<std::size_t>(i)];
            }
        }
    }

    return true;
}

} // namespace openswmm::uncertainty
