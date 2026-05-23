/**
 * @file VertexReconstruction.cpp
 * @brief Implementation of pseudo-Laplacian vertex stencil construction
 *        and head reconstruction.
 *
 * @see VertexReconstruction.hpp
 * @ingroup engine_2d
 */

#include "VertexReconstruction.hpp"

#include <vector>
#include <cmath>
#include <algorithm>

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


void reconstructVertexHeads(const MeshData& mesh, SurfaceStateData& state) {
    int nv = mesh.n_vertices();

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

} // namespace openswmm::twoD
