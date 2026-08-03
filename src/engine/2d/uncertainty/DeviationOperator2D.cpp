/**
 * @file DeviationOperator2D.cpp
 * @brief Reduced deviation operator — assembly and dense propagator.
 *
 * @see DeviationOperator2D.hpp
 * @ingroup engine_2d
 */

#include "DeviationOperator2D.hpp"

#include "uncertainty/GraphEigenBasis.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace openswmm::twoD {

using openswmm::uncertainty::CsrGraph;
using openswmm::uncertainty::coo_to_csr;
using openswmm::uncertainty::csr_matvec;

// ============================================================================
// assemble
// ============================================================================

bool DeviationOperator2D::assemble(const MeshData& mesh,
                                   const MeshEigenBasis& basis,
                                   double D_scale,
                                   const double* h_cell,
                                   const double* cell_u, const double* cell_v,
                                   const double* ground_w) {
    k = 0;
    M.clear();
    if (!basis.is_ready()) return false;
    const int n = mesh.n_triangles();
    if (n < 4 || basis.n_triangles != n) return false;
    if (D_scale < 0.0) return false;

    const bool has_flow = (cell_u != nullptr) && (cell_v != nullptr);
    const bool advect   = has_flow && (c_factor != 0.0);

    // Optional depth weighting: per-cell (h/h̄)^{5/3}, floored so a dry cell
    // cannot zero an edge outright (same floor role as the eigenbasis's
    // depth-weighted build).
    std::vector<double> w_cell;
    if (h_cell != nullptr) {
        w_cell.assign(static_cast<std::size_t>(n), 1.0);
        double h_sum = 0.0;
        for (int t = 0; t < n; ++t)
            h_sum += std::max(h_cell[static_cast<std::size_t>(t)], 0.0);
        const double h_mean = h_sum / static_cast<double>(n);
        if (h_mean > 1.0e-9) {
            const double inv_h53 = 1.0 / std::pow(h_mean, 5.0 / 3.0);
            for (int t = 0; t < n; ++t) {
                const double h_t =
                    std::max(h_cell[static_cast<std::size_t>(t)], 0.0);
                w_cell[static_cast<std::size_t>(t)] =
                    std::max(std::pow(h_t, 5.0 / 3.0) * inv_h53, 1.0e-9);
            }
        }
    }

    // ---- Full-mesh operator L_op, physical FV convention -------------------
    // Diffusion row i: +Σ_j (D_edge·w_geo/A_i), off-diagonal −D_edge·w_geo/A_i.
    // Advection (first-order upwind): outflow faces take the local cell value,
    // inflow faces the neighbour's — dissipative by construction, and the part
    // of the physics a symmetric operator cannot represent.
    std::vector<std::vector<std::pair<int, double>>> coo(
        static_cast<std::size_t>(n));

    const double iso_mean = 0.5 * (alpha_par + alpha_perp);

    for (int i = 0; i < n; ++i) {
        const int tri_nbrs[3] = {
            mesh.tri_nbr0[static_cast<std::size_t>(i)],
            mesh.tri_nbr1[static_cast<std::size_t>(i)],
            mesh.tri_nbr2[static_cast<std::size_t>(i)]
        };
        for (int e = 0; e < 3; ++e) {
            const int j = tri_nbrs[e];
            if (j < 0 || j <= i) continue;  // each interior face once (i < j)

            const auto ui = static_cast<std::size_t>(i);
            const auto uj = static_cast<std::size_t>(j);

            const double len = mesh.edge_length[static_cast<std::size_t>(i * 3 + e)];
            const double dx  = mesh.tri_cx[uj] - mesh.tri_cx[ui];
            const double dy  = mesh.tri_cy[uj] - mesh.tri_cy[ui];
            const double d   = std::sqrt(dx * dx + dy * dy);
            if (d < 1e-14) continue;

            const double A_i = mesh.tri_area[ui];
            const double A_j = mesh.tri_area[uj];
            if (A_i < 1e-30 || A_j < 1e-30) continue;

            // Face flow: mean of the two cell velocities.
            double fu = 0.0, fv = 0.0, speed = 0.0;
            if (has_flow) {
                fu = 0.5 * (cell_u[ui] + cell_u[uj]);
                fv = 0.5 * (cell_v[ui] + cell_v[uj]);
                speed = std::sqrt(fu * fu + fv * fv);
            }

            // Flow-aligned anisotropic conductance factor. cos θ projects the
            // centroid direction onto the flow direction; where the flow is too
            // slow to define a direction, the factor blends to the isotropic
            // mean (vector friction linearizes without a preferred axis at
            // rest — the anisotropy is a property of sustained flow).
            double aniso = iso_mean;
            if (has_flow && speed > u_eps) {
                const double ce = (dx * fu + dy * fv) / (d * speed);  // cos θ
                const double c2 = ce * ce;
                aniso = alpha_par * c2 + alpha_perp * (1.0 - c2);
            } else if (!has_flow) {
                // No velocity supplied at all: pure isotropic operator uses
                // the parallel dial as THE dial (α∥ = α⊥ expected).
                aniso = iso_mean;
            }

            double w_geo = len / d;  // dimensionless geometric conductance
            if (h_cell != nullptr) {
                const double wi = w_cell[ui], wj = w_cell[uj];
                w_geo *= 2.0 * wi * wj / (wi + wj);  // harmonic mean
            }

            const double cond = D_scale * aniso * w_geo;  // m²/s · (len/d)

            // Diffusion, FV: divide each row's flux by that row's cell area.
            coo[ui].emplace_back(i,  cond / A_i);
            coo[ui].emplace_back(j, -cond / A_i);
            coo[uj].emplace_back(j,  cond / A_j);
            coo[uj].emplace_back(i, -cond / A_j);

            // Upwind advection at celerity c⃗ = c_factor·u⃗.
            if (advect && speed > u_eps) {
                const double nx = dx / d, ny = dy / d;   // outward normal of i toward j
                const double un = c_factor * (fu * nx + fv * ny);  // m/s, + = i→j
                const double q  = un * len;                        // m²/s (per unit depth)
                if (un > 0.0) {
                    // Outflow from i carries δh_i; inflow to j receives it.
                    coo[ui].emplace_back(i,  q / A_i);
                    coo[uj].emplace_back(i, -q / A_j);
                } else if (un < 0.0) {
                    // Flow j→i: outflow from j carries δh_j; i receives it.
                    coo[uj].emplace_back(j, -q / A_j);
                    coo[ui].emplace_back(j,  q / A_i);
                }
            }
        }
    }

    // Open-boundary grounding (see the header note): a diagonal-only edge to
    // a zero-deviation ghost, at the mean conductance dial — the boundary face
    // orientation is not carried per cell, and the constant is a calibration
    // dial like the rest.
    if (ground_w != nullptr) {
        for (int i = 0; i < n; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            const double gw = ground_w[ui];
            if (gw <= 0.0) continue;
            const double A_i = mesh.tri_area[ui];
            if (A_i < 1e-30) continue;
            double w = gw;
            if (h_cell != nullptr) w *= w_cell[ui];
            coo[ui].emplace_back(i, D_scale * iso_mean * w / A_i);
        }
    }

    CsrGraph L = coo_to_csr(coo, n);

    // ---- Galerkin projection: M[p][q] = P[:,p]ᵀ · L · P[:,q] ----------------
    k = basis.num_kept;
    const auto nk = static_cast<std::size_t>(k);
    const auto nn = static_cast<std::size_t>(n);
    M.assign(nk * nk, 0.0);

    std::vector<double> y(nn, 0.0);
    for (std::size_t q = 0; q < nk; ++q) {
        const double* Pq = &basis.P[q * nn];
        csr_matvec(L, Pq, y.data());
        for (std::size_t p = 0; p < nk; ++p) {
            const double* Pp = &basis.P[p * nn];
            double dot = 0.0;
            for (std::size_t t = 0; t < nn; ++t)
                dot += Pp[t] * y[t];
            M[p * nk + q] = dot;
        }
    }
    return true;
}

// ============================================================================
// Dense helpers (file-local)
// ============================================================================

namespace {

// C = A·B, all n×n row-major.
void matmul(const std::vector<double>& A, const std::vector<double>& B,
            std::vector<double>& C, int n) {
    const auto un = static_cast<std::size_t>(n);
    C.assign(un * un, 0.0);
    for (std::size_t i = 0; i < un; ++i) {
        const double* Ai = &A[i * un];
        double* Ci = &C[i * un];
        for (std::size_t p = 0; p < un; ++p) {
            const double a = Ai[p];
            if (a == 0.0) continue;
            const double* Bp = &B[p * un];
            for (std::size_t j = 0; j < un; ++j)
                Ci[j] += a * Bp[j];
        }
    }
}

// Solve D·X = N in place (X returned in N). Dense LU, partial pivoting.
// n is small (ROM modes + 1); this is not a performance path.
bool solveInPlace(std::vector<double>& D, std::vector<double>& N, int n) {
    const auto un = static_cast<std::size_t>(n);
    for (int col = 0; col < n; ++col) {
        // Pivot.
        int piv = col;
        double best = std::fabs(D[static_cast<std::size_t>(col) * un + col]);
        for (int r = col + 1; r < n; ++r) {
            const double v = std::fabs(D[static_cast<std::size_t>(r) * un + col]);
            if (v > best) { best = v; piv = r; }
        }
        if (best < 1e-300) return false;  // singular — cannot happen for Padé D
        if (piv != col) {
            for (int c = 0; c < n; ++c) {
                std::swap(D[static_cast<std::size_t>(piv) * un + c],
                          D[static_cast<std::size_t>(col) * un + c]);
                std::swap(N[static_cast<std::size_t>(piv) * un + c],
                          N[static_cast<std::size_t>(col) * un + c]);
            }
        }
        const double inv = 1.0 / D[static_cast<std::size_t>(col) * un + col];
        for (int r = 0; r < n; ++r) {
            if (r == col) continue;
            const double f = D[static_cast<std::size_t>(r) * un + col] * inv;
            if (f == 0.0) continue;
            for (int c = 0; c < n; ++c) {
                D[static_cast<std::size_t>(r) * un + c] -=
                    f * D[static_cast<std::size_t>(col) * un + c];
                N[static_cast<std::size_t>(r) * un + c] -=
                    f * N[static_cast<std::size_t>(col) * un + c];
            }
        }
        for (int c = 0; c < n; ++c)
            N[static_cast<std::size_t>(col) * un + c] *= inv;
        // Normalize the pivot row of D too, to keep later eliminations exact.
        for (int c = 0; c < n; ++c)
            D[static_cast<std::size_t>(col) * un + c] *= inv;
    }
    return true;
}

} // namespace

// ============================================================================
// expm — scaling-and-squaring with a [6/6] Padé approximant
// ============================================================================

void DeviationOperator2D::expm(std::vector<double>& A, int n) {
    const auto un = static_cast<std::size_t>(n);
    assert(A.size() == un * un);

    // ‖A‖∞ (max absolute row sum) → scaling exponent s so ‖A/2^s‖ ≤ 1/2.
    double norm = 0.0;
    for (std::size_t i = 0; i < un; ++i) {
        double rs = 0.0;
        for (std::size_t j = 0; j < un; ++j) rs += std::fabs(A[i * un + j]);
        norm = std::max(norm, rs);
    }
    int s = 0;
    if (norm > 0.5) {
        s = static_cast<int>(std::ceil(std::log2(norm / 0.5)));
        const double f = std::ldexp(1.0, -s);  // 2^{-s}
        for (auto& a : A) a *= f;
    }

    // [6/6] Padé: N = Σ c_m A^m, D = Σ (−1)^m c_m A^m,
    // c_m = (12−m)!·6! / (12!·m!·(6−m)!).
    static const double c[7] = {
        1.0, 1.0 / 2.0, 5.0 / 44.0, 1.0 / 66.0,
        1.0 / 792.0, 1.0 / 15840.0, 1.0 / 665280.0
    };

    std::vector<double> Apow(un * un, 0.0);  // A^m, starts at identity
    for (std::size_t i = 0; i < un; ++i) Apow[i * un + i] = 1.0;
    std::vector<double> Nmat(un * un, 0.0), Dmat(un * un, 0.0), tmp;
    for (std::size_t i = 0; i < un; ++i) {
        Nmat[i * un + i] = c[0];
        Dmat[i * un + i] = c[0];
    }
    double sign = 1.0;
    for (int m = 1; m <= 6; ++m) {
        matmul(Apow, A, tmp, n);
        Apow.swap(tmp);
        sign = -sign;
        for (std::size_t idx = 0; idx < un * un; ++idx) {
            Nmat[idx] += c[m] * Apow[idx];
            Dmat[idx] += sign * c[m] * Apow[idx];
        }
    }

    const bool ok = solveInPlace(Dmat, Nmat, n);
    assert(ok && "Padé denominator singular — matrix not properly scaled");
    (void)ok;

    // Undo the scaling: square s times.
    for (int r = 0; r < s; ++r) {
        matmul(Nmat, Nmat, tmp, n);
        Nmat.swap(tmp);
    }
    A.swap(Nmat);
}

// ============================================================================
// propagate — δa ← exp(−s·Δt·M)·δa + φ₁(−s·Δt·M)·Δt·g
// ============================================================================

void DeviationOperator2D::propagate(const std::vector<double>& M, int k,
                                    double s, double dt,
                                    double* delta_a, const double* g) {
    assert(k > 0);
    const auto uk = static_cast<std::size_t>(k);
    assert(M.size() == uk * uk);

    // Augmented (k+1)×(k+1): B = [[−s·Δt·M, Δt·g],[0, 0]];
    // exp(B) = [[E, φ₁(−s·Δt·M)·Δt·g],[0, 1]].
    const int n = k + 1;
    const auto un = static_cast<std::size_t>(n);
    std::vector<double> B(un * un, 0.0);
    const double f = -s * dt;
    for (std::size_t i = 0; i < uk; ++i) {
        for (std::size_t j = 0; j < uk; ++j)
            B[i * un + j] = f * M[i * uk + j];
        B[i * un + uk] = dt * g[i];
    }

    expm(B, n);

    std::vector<double> out(uk, 0.0);
    for (std::size_t i = 0; i < uk; ++i) {
        double v = B[i * un + uk];              // forcing integral column
        for (std::size_t j = 0; j < uk; ++j)
            v += B[i * un + j] * delta_a[j];    // E·δa
        out[i] = v;
    }
    for (std::size_t i = 0; i < uk; ++i) delta_a[i] = out[i];
}

} // namespace openswmm::twoD
