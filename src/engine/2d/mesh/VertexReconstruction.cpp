/**
 * @file VertexReconstruction.cpp
 * @brief Implementation of pseudo-Laplacian vertex stencil construction
 *        and head reconstruction.
 *
 * @see VertexReconstruction.hpp
 * @ingroup engine_2d
 */

#include "VertexReconstruction.hpp"
#include "VfrClosure.hpp"

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

    // CSR gather: each vertex reads its stencil's cell heads (read-only) and
    // writes only its own vert_head[b]. schedule(static) ⇒ bit-exact serial.
#pragma omp parallel for schedule(static) num_threads(nthreads)
    for (int b = 0; b < nv; ++b) {
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
    // Delegates to the shared VFR closure (VfrClosure.hpp) with eps = 0 — the
    // EXACT relation, the same math this function carried before the solver
    // adopted the closure. Single source of truth: render and solver cannot
    // drift apart. (NaN mean_depth: !(NaN > 0) → lowest vertex, as before.)
    double z1 = za, z2 = zb, z3 = zc;
    vfrSort3(z1, z2, z3);
    if (!(mean_depth > 0.0)) return z1;
    return vfrEtaFromMeanDepth(z1, z2, z3, mean_depth, 0.0);
}


void reconstructVertexRenderDepths(const MeshData& mesh, SurfaceStateData& state,
                                   double dry_depth,
                                   [[maybe_unused]] int nthreads,
                                   std::vector<double>& eta_scratch) {
    const int nv = mesh.n_vertices();
    const int nt = mesh.n_triangles();

    // Each cell's free surface is a property of the CELL, not of the (vertex,
    // cell) incidence: the per-incidence form re-ran the exact planar-bed
    // closure (sort3 + cube root / Newton) about once per incident vertex,
    // i.e. ~3x per cell per pass, and it was the single largest consumer in
    // the profile. Evaluate it once per wet cell here, then gather. Dry cells
    // are left untouched (the gather re-tests the same predicate) so the
    // closure is never evaluated where the old code would not have evaluated
    // it either.
    eta_scratch.resize(static_cast<std::size_t>(nt));
#pragma omp parallel for schedule(static) num_threads(nthreads)
    for (int t = 0; t < nt; ++t) {
        const double h = state.depth[t];
        if (!(h >= dry_depth)) continue;
        eta_scratch[static_cast<std::size_t>(t)] = cellFreeSurfaceElevation(
            h, mesh.vz[mesh.tri_v0[t]], mesh.vz[mesh.tri_v1[t]],
            mesh.vz[mesh.tri_v2[t]]);
    }

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
            const double eta = eta_scratch[static_cast<std::size_t>(t)];
            if (!std::isfinite(eta)) continue;      // nodata z must not spread
            // Wetted-contact gate: a cell votes at this vertex only if its
            // water surface actually reaches the vertex's corner (η above the
            // vertex bed). Without it a thin film pooled at the BASE of a
            // steep cell stamps its low η onto the cell's HIGH vertex,
            // dragging the reported level there down to the film level (the
            // wall-base "notch" in profile plots). Strict >: at equality the
            // contribution's signed depth is 0 — indistinguishable from the
            // no-data sentinel, so excluding it is harmless. This gate is
            // also why no no-new-minima clamp exists: a mean over gated
            // contributors is already bounded below by min η_k > z_v.
            if (!(eta > mesh.vz[b])) continue;
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
