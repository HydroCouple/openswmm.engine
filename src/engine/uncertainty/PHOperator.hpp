/**
 * @file PHOperator.hpp
 * @brief Port-Hamiltonian deviation operator core (PORT_HAMILTONIAN phase 1, card S-3).
 *
 * @details Convention:   Q dψ/dt = −L ψ + B u,   L = R − J + (one-way terms).
 *
 *          TWO CONTRACTS (PORT_HAMILTONIAN §2 item 4, the F-1 verdict):
 *
 *          1. Two-way edges and diagonals (R ⪰ 0 symmetric, J = −Jᵀ skew) are
 *             ℓ2-stable: with a Q-orthonormal basis (PᵀQP = I) Galerkin keeps
 *             the form, J_r = PᵀJP is skew and R_r = PᵀRP ⪰ 0, so the quadratic
 *             energy ½ψᵀQψ cannot grow. These may be REDUCED (`reduce`).
 *          2. One-way edges (weir, free outlet, supercritical pipe, pump) are
 *             NOT ℓ2-stable in deviation form: δQ = d·z_up between two stores
 *             has sym(L) = d·[[1,−½],[−½,0]] with eigenvalue d(1−√2)/2 < 0.
 *             What closes is ℓ1 contraction of Q·ψ (L column-dominant
 *             M-matrix: off-diagonals ≤ 0, column sums ≥ 0) plus order
 *             preservation. That property is destroyed by projection (the
 *             1e33 / 1e41 failure class), so one-way edges stay FULL ORDER:
 *             `reduce` refuses them and `backwardEulerStep` handles them.
 *             Note the ℓ1 property also needs the two-way part to be a pure
 *             R (j = 0): a J edge has column sums ±j, which is not
 *             column-dominant. J is a transient-transport device for the
 *             reduced (ℓ2) channel only.
 *
 *          Edge forms (L contributions):
 *            two-way (a,b,w,j):  w·[[1,−1],[−1,1]] − j·[[0,1],[−1,0]]
 *            one-way (from→to,d): +d at (from,from), −d at (to,from) ONLY
 *                                 (no downstream diagonal; to = −1 means the
 *                                 flow leaves the system: diagonal only).
 *            Rdiag[i] ≥ 0 on the diagonal (grounding, kinematic diagonal).
 *
 *          The dense solve is a test-grade reference (n ≲ 2000), not a
 *          production path; production uses the elimination in
 *          RomAdditiveChannel.hpp.
 *
 *          Pure header, no engine dependencies (RomDiagTrust.hpp pattern).
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_PH_OPERATOR_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_PH_OPERATOR_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace openswmm::uncertainty {

struct PHOperator {
    struct TwoWay { int a = 0, b = 0; double w = 0.0; double j = 0.0; };
    struct OneWay { int from = 0, to = 0; double d = 0.0; };

    int n = 0;
    std::vector<double> Q;       ///< diagonal storage, > 0, size n
    std::vector<double> Rdiag;   ///< diagonal dissipation >= 0, size n
    std::vector<TwoWay> two_way;
    std::vector<OneWay> one_way;
    /// Nodes that carry ports (outfall / source columns of B). Informational:
    /// assembly does not read it.
    std::vector<int> port_nodes;

    mutable std::string last_error;

    bool hasOneWay() const { return !one_way.empty(); }

    /// Dense L, n×n row-major, L = R − J + one-way terms. Returns false on bad
    /// dimensions or indices.
    bool assembleFull(std::vector<double>& L) const {
        if (!validate()) return false;
        const auto un = static_cast<std::size_t>(n);
        L.assign(un * un, 0.0);
        for (int i = 0; i < n && !Rdiag.empty(); ++i)
            L[idx(i, i)] += Rdiag[static_cast<std::size_t>(i)];
        for (const auto& e : two_way) {
            if (e.a == e.b) continue;
            L[idx(e.a, e.a)] += e.w;  L[idx(e.b, e.b)] += e.w;
            L[idx(e.a, e.b)] -= e.w;  L[idx(e.b, e.a)] -= e.w;
            L[idx(e.a, e.b)] -= e.j;  L[idx(e.b, e.a)] += e.j;
        }
        for (const auto& e : one_way) {
            L[idx(e.from, e.from)] += e.d;
            if (e.to >= 0) L[idx(e.to, e.from)] -= e.d;
        }
        return true;
    }

    /// l1 stability contract (file header, contract 2): L is a column-dominant
    /// M-matrix -- off-diagonals <= tol, column sums >= -tol. `L` is dense n x n
    /// row-major (n = sqrt(size)). Test-grade O(n^2).
    static bool checkColumnDominant(const std::vector<double>& L, double tol = 1.0e-12) {
        const auto un = static_cast<std::size_t>(std::llround(std::sqrt(static_cast<double>(L.size()))));
        if (un * un != L.size()) return false;
        for (std::size_t c = 0; c < un; ++c) {
            double sum = 0.0;
            for (std::size_t r = 0; r < un; ++r) {
                if (r != c && L[r * un + c] > tol) return false;
                sum += L[r * un + c];
            }
            if (sum < -tol * (1.0 + std::fabs(L[c * un + c]))) return false;
        }
        return true;
    }

    /// Same contract evaluated on the edge lists in O(edges) (no dense matrix),
    /// for the per-step Debug assertion in AdditiveSystem::build. Requires
    /// j == 0 on every two-way edge.
    bool columnDominantSparse(double tol = 1.0e-12) const {
        if (!validate()) return false;
        const auto un = static_cast<std::size_t>(n);
        std::vector<double> colsum(un, 0.0), diagmag(un, 0.0);
        for (std::size_t i = 0; i < un && !Rdiag.empty(); ++i) { colsum[i] += Rdiag[i]; diagmag[i] += std::fabs(Rdiag[i]); }
        for (const auto& e : two_way) {
            if (e.a == e.b) continue;
            if (e.j != 0.0 || e.w < -tol) return false;
            // columns a and b: +w on the diagonal, -w off it: sum 0.
        }
        for (const auto& e : one_way) {
            if (e.d < -tol) return false;
            colsum[static_cast<std::size_t>(e.from)] += e.d;
            diagmag[static_cast<std::size_t>(e.from)] += std::fabs(e.d);
            if (e.to >= 0 && e.to != e.from) colsum[static_cast<std::size_t>(e.from)] -= e.d;
        }
        for (std::size_t c = 0; c < un; ++c)
            if (colsum[c] < -tol * (1.0 + diagmag[c])) return false;
        return true;
    }

    /**
     * @brief Galerkin M = PᵀLP for the two-way + diagonal part, P k×n row-major
     *        (`P[j*n+i]`), assumed Q-orthonormal (not checked).
     *
     * Refuses (returns false, sets last_error) when any one-way edge exists:
     * one-way edges are ℓ1-stable, not ℓ2; projecting them is the 1e33/1e41
     * failure class (see file header).
     */
    bool reduce(const double* P, int k, std::vector<double>& M_out) const {
        if (!validate()) return false;
        if (hasOneWay()) {
            last_error = "PHOperator::reduce: one-way edges are l1-stable, not l2; "
                         "they must stay full order";
            return false;
        }
        if (!P || k <= 0) { last_error = "PHOperator::reduce: bad basis"; return false; }
        const auto uk = static_cast<std::size_t>(k);
        const auto un = static_cast<std::size_t>(n);
        M_out.assign(uk * uk, 0.0);
        for (const auto& e : two_way) {
            if (e.a == e.b) continue;
            const auto ua = static_cast<std::size_t>(e.a), ub = static_cast<std::size_t>(e.b);
            for (std::size_t p = 0; p < uk; ++p) {
                const double Ppa = P[p * un + ua], Ppb = P[p * un + ub];
                double* Mp = &M_out[p * uk];
                for (std::size_t q = 0; q < uk; ++q) {
                    const double Pqa = P[q * un + ua], Pqb = P[q * un + ub];
                    // R part (same expression order as RomDirectionalOperator),
                    // then −J part: −j·(Ppa·Pqb − Ppb·Pqa).
                    Mp[q] += e.w * (Ppa * Pqa + Ppb * Pqb - Ppa * Pqb - Ppb * Pqa);
                    Mp[q] -= e.j * (Ppa * Pqb - Ppb * Pqa);
                }
            }
        }
        if (!Rdiag.empty()) {
            for (std::size_t i = 0; i < un; ++i) {
                const double r = Rdiag[i];
                if (r == 0.0) continue;
                for (std::size_t p = 0; p < uk; ++p) {
                    const double rp = r * P[p * un + i];
                    double* Mp = &M_out[p * uk];
                    for (std::size_t q = 0; q < uk; ++q) Mp[q] += rp * P[q * un + i];
                }
            }
        }
        return true;
    }

    /**
     * @brief One full-order backward-Euler step: (Q/dt + L) ψ_new = Q ψ_old/dt + rhs.
     * @param rhs  B·u (size n) or nullptr for zero. psi is in/out.
     * Dense LU with partial pivoting: TEST-GRADE reference, n ≲ 2000.
     */
    bool backwardEulerStep(double dt, const double* rhs, double* psi) const {
        if (!(dt > 0.0) || !psi) { last_error = "backwardEulerStep: bad dt/psi"; return false; }
        std::vector<double> A;
        if (!assembleFull(A)) return false;
        const auto un = static_cast<std::size_t>(n);
        std::vector<double> b(un);
        for (std::size_t i = 0; i < un; ++i) {
            A[i * un + i] += Q[i] / dt;
            b[i] = Q[i] * psi[i] / dt + (rhs ? rhs[i] : 0.0);
        }
        for (std::size_t c = 0; c < un; ++c) {
            std::size_t piv = c;
            for (std::size_t r = c + 1; r < un; ++r)
                if (std::fabs(A[r * un + c]) > std::fabs(A[piv * un + c])) piv = r;
            if (A[piv * un + c] == 0.0) { last_error = "backwardEulerStep: singular"; return false; }
            if (piv != c) {
                for (std::size_t j = 0; j < un; ++j) std::swap(A[c * un + j], A[piv * un + j]);
                std::swap(b[c], b[piv]);
            }
            for (std::size_t r = c + 1; r < un; ++r) {
                const double f = A[r * un + c] / A[c * un + c];
                if (f == 0.0) continue;
                for (std::size_t j = c; j < un; ++j) A[r * un + j] -= f * A[c * un + j];
                b[r] -= f * b[c];
            }
        }
        for (std::size_t ri = un; ri-- > 0;) {
            double s = b[ri];
            for (std::size_t j = ri + 1; j < un; ++j) s -= A[ri * un + j] * psi[j];
            psi[ri] = s / A[ri * un + ri];
        }
        return true;
    }

private:
    std::size_t idx(int r, int c) const {
        return static_cast<std::size_t>(r) * static_cast<std::size_t>(n) + static_cast<std::size_t>(c);
    }
    bool validate() const {
        if (n <= 0 || Q.size() != static_cast<std::size_t>(n) ||
            (!Rdiag.empty() && Rdiag.size() != static_cast<std::size_t>(n))) {
            last_error = "PHOperator: bad dimensions";
            return false;
        }
        for (const auto& e : two_way)
            if (e.a < 0 || e.a >= n || e.b < 0 || e.b >= n) { last_error = "PHOperator: two-way index"; return false; }
        for (const auto& e : one_way)
            if (e.from < 0 || e.from >= n || e.to >= n) { last_error = "PHOperator: one-way index"; return false; }
        return true;
    }
};

/// (M + Mᵀ)/2, k×k row-major.
inline std::vector<double> symPart(const std::vector<double>& M, int k) {
    const auto uk = static_cast<std::size_t>(k);
    std::vector<double> S(uk * uk);
    for (std::size_t p = 0; p < uk; ++p)
        for (std::size_t q = 0; q < uk; ++q) S[p * uk + q] = 0.5 * (M[p * uk + q] + M[q * uk + p]);
    return S;
}

/// (M − Mᵀ)/2, k×k row-major.
inline std::vector<double> skewPart(const std::vector<double>& M, int k) {
    const auto uk = static_cast<std::size_t>(k);
    std::vector<double> S(uk * uk);
    for (std::size_t p = 0; p < uk; ++p)
        for (std::size_t q = 0; q < uk; ++q) S[p * uk + q] = 0.5 * (M[p * uk + q] - M[q * uk + p]);
    return S;
}

/// Smallest eigenvalue of a SYMMETRIC k×k matrix (cyclic Jacobi, k ≲ 64).
/// The input is symmetrized first, so passing a non-symmetric M returns the
/// minimum eigenvalue of its symmetric part.
inline double minEigSym(const std::vector<double>& M, int k) {
    if (k <= 0) return 0.0;
    const auto uk = static_cast<std::size_t>(k);
    std::vector<double> A = symPart(M, k);
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0, tot = 0.0;
        for (std::size_t p = 0; p < uk; ++p)
            for (std::size_t q = 0; q < uk; ++q) {
                tot += A[p * uk + q] * A[p * uk + q];
                if (p != q) off += A[p * uk + q] * A[p * uk + q];
            }
        if (off <= 1.0e-30 * tot || off == 0.0) break;
        for (std::size_t p = 0; p + 1 < uk; ++p)
            for (std::size_t q = p + 1; q < uk; ++q) {
                const double apq = A[p * uk + q];
                if (apq == 0.0) continue;
                const double theta = (A[q * uk + q] - A[p * uk + p]) / (2.0 * apq);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                 (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (std::size_t r = 0; r < uk; ++r) {
                    if (r == p || r == q) continue;
                    const double arp = A[r * uk + p], arq = A[r * uk + q];
                    A[r * uk + p] = A[p * uk + r] = c * arp - s * arq;
                    A[r * uk + q] = A[q * uk + r] = s * arp + c * arq;
                }
                A[p * uk + p] -= t * apq;
                A[q * uk + q] += t * apq;
                A[p * uk + q] = A[q * uk + p] = 0.0;
            }
    }
    double m = A[0];
    for (std::size_t i = 1; i < uk; ++i) m = std::min(m, A[i * uk + i]);
    return m;
}

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_PH_OPERATOR_HPP
