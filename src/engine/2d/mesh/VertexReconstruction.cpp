/**
 * @file VertexReconstruction.cpp
 * @brief Implementation of pseudo-Laplacian vertex stencil construction
 *        and head reconstruction.
 *
 * @see VertexReconstruction.hpp
 * @ingroup engine_2d
 */

#include "VertexReconstruction.hpp"
#include "../data/ActiveSetData.hpp"

#include <vector>
#include <cmath>
#include <algorithm>

#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#else
static inline int omp_get_max_threads() { return 1; }
#endif

namespace openswmm::twoD {

void buildVertexStencils(MeshData& mesh) {
    int nv = mesh.n_vertices();
    int nt = mesh.n_triangles();

    // Step 1: For each vertex, collect all triangles that share it
    std::vector<std::vector<int>> vert_triangles(nv);
    for (int t = 0; t < nt; ++t) {
        vert_triangles[mesh.tri_v0[t]].push_back(t);
        vert_triangles[mesh.tri_v1[t]].push_back(t);
        vert_triangles[mesh.tri_v2[t]].push_back(t);
    }

    // Step 2: Build CSR stencil
    mesh.vert_stencil_ptr.resize(nv + 1);
    mesh.vert_stencil_idx.clear();
    mesh.vert_stencil_wt.clear();

    int ptr = 0;
    for (int b = 0; b < nv; ++b) {
        mesh.vert_stencil_ptr[b] = ptr;
        const auto& tris = vert_triangles[b];
        int M = static_cast<int>(tris.size());

        if (M == 0) {
            // Isolated vertex — no stencil
            continue;
        }

        double xb = mesh.vx[b];
        double yb = mesh.vy[b];

        // Compute moments (Eq. [20]–[21])
        double I_xx = 0.0, I_yy = 0.0, I_xy = 0.0;
        for (int i = 0; i < M; ++i) {
            int t = tris[i];
            double dx = mesh.tri_cx[t] - xb;
            double dy = mesh.tri_cy[t] - yb;
            I_xx += dx * dx;
            I_yy += dy * dy;
            I_xy += dx * dy;
        }

        // Determinant of the moment matrix
        double det = I_xx * I_yy - I_xy * I_xy;

        if (std::abs(det) < 1.0e-30) {
            // Degenerate (collinear stencil) — use uniform weights
            double w = 1.0 / static_cast<double>(M);
            for (int i = 0; i < M; ++i) {
                mesh.vert_stencil_idx.push_back(tris[i]);
                mesh.vert_stencil_wt.push_back(w);
            }
            ptr += M;
            continue;
        }

        double inv_det = 1.0 / det;

        // Lagrange multipliers (Eq. [20])
        // R_x = Σ dx_i, R_y = Σ dy_i (should be ~0 if stencil is symmetric)
        // But we need to enforce linear exactness:
        // λ_x = (I_yy * R_x - I_xy * R_y) / det   where R_x, R_y ensure
        // Σ ω_i * dx_i = 0 and Σ ω_i * dy_i = 0 (constraint for constant reproduction)
        //
        // For the pseudo-Laplacian: ω_i = (1/M) + λ_x*(x_i - x_b) + λ_y*(y_i - y_b)
        // where λ are chosen so that Σ ω_i = 1 and Σ ω_i * r_i = 0

        // We solve the 2x2 system:
        // [I_xx  I_xy] [λ_x]   [0]
        // [I_xy  I_yy] [λ_y] = [0]
        //
        // But with the partition-of-unity constraint already satisfied by 1/M base.
        // The Lagrange multipliers enforce that the linear terms vanish:
        // Σ ω_i (x_i - x_b) = 0  →  Σ (1/M)(x_i-x_b) + λ_x Σ(x_i-x_b)² + λ_y Σ(x_i-x_b)(y_i-y_b) = 0
        // Same for y.

        double Sx = 0.0, Sy = 0.0;
        for (int i = 0; i < M; ++i) {
            Sx += (mesh.tri_cx[tris[i]] - xb);
            Sy += (mesh.tri_cy[tris[i]] - yb);
        }
        double R_x = -Sx / M;
        double R_y = -Sy / M;

        double lambda_x = (I_yy * R_x - I_xy * R_y) * inv_det;
        double lambda_y = (I_xx * R_y - I_xy * R_x) * inv_det;

        // Compute weights and clip negatives (Jawahar & Kamath boundary treatment)
        std::vector<double> weights(M);
        double w_sum = 0.0;
        for (int i = 0; i < M; ++i) {
            double dx = mesh.tri_cx[tris[i]] - xb;
            double dy = mesh.tri_cy[tris[i]] - yb;
            weights[i] = (1.0 / M) + lambda_x * dx + lambda_y * dy;
            if (weights[i] < 0.0) weights[i] = 0.0;  // Clip extraneous weights
            w_sum += weights[i];
        }

        // Renormalize to ensure partition of unity.
        // If all weights were clipped to zero (corner vertex with stencil
        // entirely on one side), fall back to uniform weights.
        if (w_sum > 1.0e-30) {
            for (int i = 0; i < M; ++i) {
                weights[i] /= w_sum;
            }
        } else {
            double w_uniform = 1.0 / static_cast<double>(M);
            for (int i = 0; i < M; ++i) {
                weights[i] = w_uniform;
            }
        }

        for (int i = 0; i < M; ++i) {
            mesh.vert_stencil_idx.push_back(tris[i]);
            mesh.vert_stencil_wt.push_back(weights[i]);
        }
        ptr += M;
    }
    mesh.vert_stencil_ptr[nv] = ptr;
}


void reconstructVertexHeads(const MeshData& mesh, SurfaceStateData& state,
                             [[maybe_unused]] int nthreads) {
    int nv = mesh.n_vertices();

    // Active-set masking: a vertex touched only by frozen cells keeps its
    // seed-pass value — its stencil heads are frozen too, so the gather
    // would reproduce it exactly.
    const ActiveSetData* as = state.active_set;
    const bool masked = (as != nullptr) && as->enabled;

    // CSR gather: each vertex reads its stencil's cell heads (read-only) and
    // writes only its own vert_head[b]. schedule(static) ⇒ bit-exact serial.
#pragma omp parallel for schedule(static) num_threads(nthreads)
    for (int b = 0; b < nv; ++b) {
        if (masked && !as->vert_active[b]) continue;
        int start = mesh.vert_stencil_ptr[b];
        int end   = mesh.vert_stencil_ptr[b + 1];

        double h = 0.0;
        for (int k = start; k < end; ++k) {
            h += mesh.vert_stencil_wt[k] * state.head[mesh.vert_stencil_idx[k]];
        }
        state.vert_head[b] = h;
    }
}


double cellFreeSurfaceElevation(double mean_depth, double za, double zb,
                                double zc) {
    // Sort the three vertex elevations: z1 <= z2 <= z3.
    double z1 = za, z2 = zb, z3 = zc;
    if (z1 > z2) std::swap(z1, z2);
    if (z2 > z3) std::swap(z2, z3);
    if (z1 > z2) std::swap(z1, z2);

    if (!(mean_depth > 0.0)) return z1;

    const double zbar   = (z1 + z2 + z3) / 3.0;
    const double relief = z3 - z1;

    // Effectively flat cell, or fully wet (eta >= z3 <=> h >= z3 - zbar):
    // the flat closure is exact.
    if (relief < 1.0e-9 || mean_depth >= z3 - zbar)
        return zbar + mean_depth;

    // Mean depth when the waterline sits exactly at z2 (branch boundary).
    const double h_at_z2 = (z2 - z1) * (z2 - z1) / (3.0 * relief);

    if (mean_depth <= h_at_z2) {
        // Lower branch (z1 < eta <= z2): closed-form cube root.
        // h(eta) = (eta - z1)^3 / (3 (z2 - z1)(z3 - z1))
        return z1 + std::cbrt(3.0 * mean_depth * (z2 - z1) * relief);
    }

    // Upper branch (z2 < eta < z3): h(eta) = (eta - zbar)
    //   + (z3 - eta)^3 / (3 (z3 - z1)(z3 - z2)).
    // h is strictly increasing in eta (dh/deta = A_wet/A > 0 here), so a
    // safeguarded Newton on the bracket [z2, z3] converges unconditionally.
    const double denom = 3.0 * relief * (z3 - z2);
    double lo = z2, hi = z3;
    double eta = zbar + mean_depth;                 // flat-closure initial guess
    if (eta <= lo || eta >= hi) eta = 0.5 * (lo + hi);
    for (int it = 0; it < 64; ++it) {
        const double dz3 = z3 - eta;
        const double f  = (eta - zbar) + dz3 * dz3 * dz3 / denom - mean_depth;
        if (f > 0.0) hi = eta; else lo = eta;
        const double df = 1.0 - dz3 * dz3 / (relief * (z3 - z2));  // A_wet/A
        double next = (df > 1.0e-12) ? eta - f / df : 0.5 * (lo + hi);
        if (next <= lo || next >= hi) next = 0.5 * (lo + hi);      // safeguard
        if (std::abs(next - eta) < 1.0e-12 * (1.0 + relief)) return next;
        eta = next;
    }
    return eta;
}


void reconstructVertexRenderDepths(const MeshData& mesh, SurfaceStateData& state,
                                   double dry_depth,
                                   [[maybe_unused]] int nthreads) {
    const int nv = mesh.n_vertices();

    // Per-vertex CSR gather over the stencil's cell LIST (topology only — the
    // pseudo-Laplacian weights are a solver concern). Each iteration writes
    // only vert_depth_signed[b]; schedule(static) => bit-exact vs serial.
#pragma omp parallel for schedule(static) num_threads(nthreads)
    for (int b = 0; b < nv; ++b) {
        const int start = mesh.vert_stencil_ptr[b];
        const int end   = mesh.vert_stencil_ptr[b + 1];

        double vsum = 0.0, wsum = 0.0, eta_max = 0.0;
        bool   wet  = false;
        for (int k = start; k < end; ++k) {
            const int    t = mesh.vert_stencil_idx[k];
            const double h = state.depth[t];
            if (!(h >= dry_depth)) continue;        // dry skip (NaN-robust)
            const double eta = cellFreeSurfaceElevation(
                h, mesh.vz[mesh.tri_v0[t]], mesh.vz[mesh.tri_v1[t]],
                mesh.vz[mesh.tri_v2[t]]);
            if (!std::isfinite(eta)) continue;      // nodata z must not spread
            vsum += h * eta;
            wsum += h;
            if (!wet || eta > eta_max) { eta_max = eta; wet = true; }
        }

        if (wsum > 0.0) {
            double eta_v = vsum / wsum;
            // No-new-maxima guard: a depth-weighted mean is already bounded by
            // the incident wet-cell etas; keep the clamp as a hard invariant in
            // case the weighting scheme ever changes.
            if (eta_v > eta_max) eta_v = eta_max;
            const double d = eta_v - mesh.vz[b];    // SIGNED depth (VFR)
            state.vert_depth_signed[b] = std::isfinite(d) ? d : 0.0;
        } else {
            state.vert_depth_signed[b] = 0.0;       // no wet incident cell
        }
    }
}

} // namespace openswmm::twoD
