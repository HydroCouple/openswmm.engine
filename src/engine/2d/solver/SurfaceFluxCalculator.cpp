/**
 * @file SurfaceFluxCalculator.cpp
 * @brief Implementation of gradient computation, slope limiting, and edge fluxes.
 *
 * @see SurfaceFluxCalculator.hpp
 * @ingroup engine_2d
 */

#include "SurfaceFluxCalculator.hpp"
#include "DiffusiveConductance.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm::twoD {

namespace {

inline int tri_nbr(const MeshData& mesh, int t, int e) {
    switch (e) {
        case 0: return mesh.tri_nbr0[t];
        case 1: return mesh.tri_nbr1[t];
        case 2: return mesh.tri_nbr2[t];
        default: return -1;
    }
}

inline double sq(double x) noexcept { return x * x; }

} // anonymous namespace


void computeUnlimitedGradients(const MeshData& mesh, SurfaceStateData& state) {
    int nt = mesh.n_triangles();

    for (int i = 0; i < nt; ++i) {
        double inv_area = (mesh.tri_area[i] > 1.0e-30)
                              ? 1.0 / mesh.tri_area[i] : 0.0;
        double gx = 0.0, gy = 0.0;

        for (int e = 0; e < 3; ++e) {
            int idx = i * 3 + e;
            int nbr = tri_nbr(mesh, i, e);

            // Head at edge midpoint: average of this cell and neighbour
            double h_edge;
            if (nbr >= 0) {
                h_edge = 0.5 * (state.head[i] + state.head[nbr]);
            } else {
                // Boundary: use this cell's head (zero-gradient extrapolation)
                h_edge = state.head[i];
            }

            // Green-Gauss: ∇h ≈ (1/A) Σ h_edge * n * ξ
            gx += h_edge * mesh.edge_nx[idx] * mesh.edge_length[idx];
            gy += h_edge * mesh.edge_ny[idx] * mesh.edge_length[idx];
        }

        state.grad_hx[i] = gx * inv_area;
        state.grad_hy[i] = gy * inv_area;
    }
}


void computeLimitedGradients(const MeshData& mesh, SurfaceStateData& state,
                              double epsilon) {
    int nt = mesh.n_triangles();
    double eps2 = epsilon * epsilon;

    for (int i = 0; i < nt; ++i) {
        // Squared L2 norms of unlimited gradients
        double g1 = sq(state.grad_hx[i]) + sq(state.grad_hy[i]);

        // Collect neighbour gradients (use cell's own if boundary)
        double g_nbr[3];
        double gx_nbr[3], gy_nbr[3];

        for (int e = 0; e < 3; ++e) {
            int nbr = tri_nbr(mesh, i, e);
            if (nbr >= 0) {
                g_nbr[e] = sq(state.grad_hx[nbr]) + sq(state.grad_hy[nbr]);
                gx_nbr[e] = state.grad_hx[nbr];
                gy_nbr[e] = state.grad_hy[nbr];
            } else {
                g_nbr[e] = g1;
                gx_nbr[e] = state.grad_hx[i];
                gy_nbr[e] = state.grad_hy[i];
            }
        }

        // Jawahar-Kamath weights (Eq. [23]–[24])
        // Using 3 neighbours: weighted blend of the cell and 3 neighbour gradients
        // Simplified to: w_k = (product of other g's + eps²) / denom
        double g2 = g_nbr[0], g3 = g_nbr[1], g4 = g_nbr[2];

        double denom = sq(g1) + g2 * g3 + g3 * g4 + g4 * g2 + 4.0 * eps2;
        if (denom < 1.0e-30) {
            // All gradients zero — limited gradient is zero
            state.grad_hx_lim[i] = 0.0;
            state.grad_hy_lim[i] = 0.0;
            continue;
        }

        // Weight for the cell's own gradient
        double w1 = (g2 * g3 + eps2) / denom;
        // Weights for each neighbour's gradient contribution
        double w2 = (g3 * g4 + eps2) / denom;
        double w3 = (g4 * g1 + eps2) / denom;
        double w4 = (g1 * g2 + eps2) / denom;

        // Normalize
        double w_sum = w1 + w2 + w3 + w4;
        if (w_sum > 1.0e-30) {
            w1 /= w_sum; w2 /= w_sum; w3 /= w_sum; w4 /= w_sum;
        }

        state.grad_hx_lim[i] = w1 * state.grad_hx[i]
                              + w2 * gx_nbr[0] + w3 * gx_nbr[1] + w4 * gx_nbr[2];
        state.grad_hy_lim[i] = w1 * state.grad_hy[i]
                              + w2 * gy_nbr[0] + w3 * gy_nbr[1] + w4 * gy_nbr[2];
    }
}


void computeEdgeFluxes(const MeshData& mesh, SurfaceStateData& state,
                        const SolverOptions2D& opts) {
    int nt = mesh.n_triangles();

    for (int i = 0; i < nt; ++i) {
        for (int e = 0; e < 3; ++e) {
            int idx = i * 3 + e;
            int nbr = tri_nbr(mesh, i, e);

            if (nbr < 0) {
                // Boundary edge: zero-flux (wall) condition
                state.edge_flux[idx] = 0.0;
                continue;
            }

            // Determine upstream cell based on head comparison
            double h_L = state.head[i];
            double h_R = state.head[nbr];
            int upstream = (h_L >= h_R) ? i : nbr;

            // Reconstruct head at edge using upstream cell's limited gradient
            // h_edge = h_centre + r · ∇h_lim
            // where r = (edge_midpoint - cell_centroid)
            double rx = mesh.edge_mx[idx] - mesh.tri_cx[upstream];
            double ry = mesh.edge_my[idx] - mesh.tri_cy[upstream];
            double h_edge = state.head[upstream]
                          + rx * state.grad_hx_lim[upstream]
                          + ry * state.grad_hy_lim[upstream];

            // C-property: reconstruct depth from total head - bed elevation
            double z_edge = mesh.edge_mz[idx];
            double depth_edge = std::max(h_edge - z_edge, 0.0);

            // Gradient magnitude at the edge (for conductance)
            double grad_mag = std::sqrt(
                sq(state.grad_hx_lim[upstream]) + sq(state.grad_hy_lim[upstream]));

            // Diffusive conductance
            double K = diffusiveConductanceSmooth(
                depth_edge, mesh.mannings_n[upstream], grad_mag, opts.dry_depth);

            // Head gradient projected onto the edge normal direction
            double dh_dn = (h_L - h_R);  // Simple difference for normal gradient

            // Normal flux through edge: F = depth * K * (dh/dn) * edge_length
            // Sign convention: positive flux = flow in outward normal direction
            double xi = mesh.edge_length[idx];
            state.edge_flux[idx] = depth_edge * K * dh_dn * xi;
        }
    }
}


void assembleRHS(const MeshData& mesh, const SurfaceStateData& state,
                  double* ydot) {
    int nt = mesh.n_triangles();

    for (int i = 0; i < nt; ++i) {
        double inv_area = (mesh.tri_area[i] > 1.0e-30)
                              ? 1.0 / mesh.tri_area[i] : 0.0;

        // Sum edge fluxes
        double flux_sum = 0.0;
        for (int e = 0; e < 3; ++e) {
            flux_sum += state.edge_flux[i * 3 + e];
        }

        // RHS: dψ/dt = (1/A) Σ F_j + sources
        ydot[i] = flux_sum * inv_area + state.rainfall[i] + state.coupling_flux[i];
    }
}

} // namespace openswmm::twoD
