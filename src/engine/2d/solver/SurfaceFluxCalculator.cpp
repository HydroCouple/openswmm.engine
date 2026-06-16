/**
 * @file SurfaceFluxCalculator.cpp
 * @brief Implementation of gradient computation, slope limiting, and edge fluxes.
 *
 * @see SurfaceFluxCalculator.hpp
 * @ingroup engine_2d
 */

#include "SurfaceFluxCalculator.hpp"

#include <cmath>
#include <algorithm>

#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#else
static inline int omp_get_max_threads() { return 1; }
#endif

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


void computeUnlimitedGradients(const MeshData& mesh, SurfaceStateData& state,
                                [[maybe_unused]] int nthreads) {
    int nt = mesh.n_triangles();

    // Each cell writes only its own grad_hx[i]/grad_hy[i] (it reads neighbour
    // heads, never writes them), so schedule(static) is bit-identical to the
    // serial loop for any thread count.
#pragma omp parallel for schedule(static) num_threads(nthreads)
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
                              double epsilon, [[maybe_unused]] int nthreads) {
    int nt = mesh.n_triangles();
    double eps2 = epsilon * epsilon;

    // Each cell writes only its own grad_*_lim[i] from its own and its
    // neighbours' (read-only) unlimited gradients; the per-iteration q[]/
    // gx_nbr[]/gy_nbr[] arrays are declared inside the body and thus private.
#pragma omp parallel for schedule(static) num_threads(nthreads)
    for (int i = 0; i < nt; ++i) {
        // Regularised squared L2 norms of the unlimited gradients of this
        // cell (q0) and its three neighbours (q1..q3). Adding eps² inside
        // each q_k makes every weight strictly positive and gives uniform
        // 1/4 weights as all |∇h| → 0, avoiding a degenerate-division branch.
        double q0 = sq(state.grad_hx[i]) + sq(state.grad_hy[i]) + eps2;

        double q[3];
        double gx_nbr[3], gy_nbr[3];

        for (int e = 0; e < 3; ++e) {
            int nbr = tri_nbr(mesh, i, e);
            if (nbr >= 0) {
                q[e] = sq(state.grad_hx[nbr]) + sq(state.grad_hy[nbr]) + eps2;
                gx_nbr[e] = state.grad_hx[nbr];
                gy_nbr[e] = state.grad_hy[nbr];
            } else {
                // Boundary: mirror the cell's own gradient.
                q[e] = q0;
                gx_nbr[e] = state.grad_hx[i];
                gy_nbr[e] = state.grad_hy[i];
            }
        }

        // Canonical Jawahar-Kamath (JK 2000) weights for 4 contributing
        // gradients: w_k = (∏_{j≠k} q_j) / Σ_k (∏_{j≠k} q_j).
        //
        // The numerator for each weight is the product of the other three
        // regularised norms — so a value with a large |∇h| (an outlier)
        // appears in three numerators with itself absent and in zero
        // numerators with itself present, damping its own weight as 1/q_k.
        // Uniform inputs → all numerators equal → uniform 1/4 weights.
        // Sum of numerators is positive by construction (each q_j ≥ eps²)
        // so no normalisation pass is required.
        double n0 = q[0] * q[1] * q[2];   // skip self
        double n1 = q0   * q[1] * q[2];   // skip nbr 0
        double n2 = q0   * q[0] * q[2];   // skip nbr 1
        double n3 = q0   * q[0] * q[1];   // skip nbr 2

        double denom = n0 + n1 + n2 + n3;
        double w0 = n0 / denom;
        double w1 = n1 / denom;
        double w2 = n2 / denom;
        double w3 = n3 / denom;

        state.grad_hx_lim[i] = w0 * state.grad_hx[i]
                              + w1 * gx_nbr[0] + w2 * gx_nbr[1] + w3 * gx_nbr[2];
        state.grad_hy_lim[i] = w0 * state.grad_hy[i]
                              + w1 * gy_nbr[0] + w2 * gy_nbr[1] + w3 * gy_nbr[2];
    }
}


void computeEdgeFluxes(const MeshData& mesh, SurfaceStateData& state,
                        const SolverOptions2D& opts) {
    int nt = mesh.n_triangles();

    // Parallelise the OUTER per-cell loop only — the inner e=0..2 writes the
    // cell's own edge_flux[i*3+e] slots (interior edges are stored redundantly
    // per incident cell, so there is no cross-cell scatter). schedule(static)
    // keeps results bit-identical to serial.
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
    for (int i = 0; i < nt; ++i) {
        for (int e = 0; e < 3; ++e) {
            int idx = i * 3 + e;
            int nbr = tri_nbr(mesh, i, e);

            if (nbr < 0) {
                // Boundary edge: zero-flux (wall) condition.
                state.edge_flux[idx] = 0.0;
                continue;
            }

            // Hydrostatic upwinding by total head — standard FV-SWE choice.
            double h_L = state.head[i];
            double h_R = state.head[nbr];
            int upstream = (h_L >= h_R) ? i : nbr;
            double depth_up = state.depth[upstream];

            if (depth_up <= 0.0) {
                state.edge_flux[idx] = 0.0;
                continue;
            }

            // Unified well-balanced flux for the Manning diffusive wave.
            //
            // Continuity:  dh/dt = R − ∇·q  with  q = −K·h·∇H  and
            //   K(h, |∇H|) = h^(2/3) / (n · √|∇H|).
            // The FV inflow contribution to cell i across edge e is
            //   F_e = −(q · n_e) · L_e.
            // Substituting the FD estimate ∇H · n_e ≈ −(h_L − h_R) / Δx
            // (Δx = centroid-to-centroid distance, L = this cell,
            // R = neighbour) collapses the K · Δh subexpression to
            //   K · Δh = h_up^(2/3) · sign(Δh) · √(|Δh| · Δx) / n_up,
            // which removes the removable 1/√|∇H| singularity at flat
            // regions, vanishes correctly as Δh → 0 (C-property), and
            // gives the dimensionally consistent F_e units of m³/s.
            //
            // Sign convention: state.edge_flux holds the INFLOW
            // contribution to cell i across edge e — a positive value
            // increases h_i.  assembleRHS adds it as +flux_sum / A.
            double dx_x = mesh.tri_cx[i] - mesh.tri_cx[nbr];
            double dx_y = mesh.tri_cy[i] - mesh.tri_cy[nbr];
            double dx   = std::sqrt(dx_x * dx_x + dx_y * dx_y);
            if (dx < 1.0e-12) {
                state.edge_flux[idx] = 0.0;
                continue;
            }

            double dh      = h_L - h_R;
            double abs_dh  = std::abs(dh);
            double sign_dh = (dh > 0.0) ? 1.0 : (dh < 0.0 ? -1.0 : 0.0);

            // h_up^(5/3) = depth · depth^(2/3)
            double h53 = depth_up * std::cbrt(depth_up * depth_up);

            double n_up = mesh.mannings_n[upstream];
            double xi   = mesh.edge_length[idx];

            double F_e = -h53 * sign_dh * std::sqrt(abs_dh) * xi
                         / (n_up * std::sqrt(dx));

            // Cubic Hermite wet/dry shutoff on the source-side depth.
            // s(t) = 3t² − 2t³ for t = depth_up/dry_depth ∈ [0, 1]: C¹ at
            // both endpoints, vanishing flux as the source-side cell
            // approaches dry. Applied at the flux level so the wet/dry
            // transition behaviour is contained in this one place.
            if (depth_up < opts.dry_depth) {
                double t = depth_up / opts.dry_depth;
                F_e *= t * t * (3.0 - 2.0 * t);
            }

            // §11A — per-edge conveyance factor in [0, 1] (Q4: LAST, after
            // the wet/dry shutoff).  Default 1.0 (no-op).  Mass-conservation
            // argument: for an interior edge shared by cells A and B, the
            // two slots [A*3+e_A] and [B*3+e_B] compute the SAME |F_e| (up
            // to sign) from the antisymmetric centroid Δh / Δx; multiplying
            // both by the SAME factor (enforced by the partner-mirror in
            // SurfaceRouter2D::initialize) preserves antisymmetry → no
            // spurious source/sink.  c == 0 → F_e == 0, identical to the
            // boundary early-return — an interior edge with conveyance 0
            // is a wall in everything but its storage location.
            F_e *= mesh.edge_conveyance[idx];

            state.edge_flux[idx] = F_e;
        }
    }
}


void assembleRHS(const MeshData& mesh, const SurfaceStateData& state,
                  const SolverOptions2D& opts, double* ydot) {
    int nt = mesh.n_triangles();

    // Per-cell gather: each cell sums its own 3 edge fluxes and writes ydot[i].
    // No cross-cell writes → schedule(static) is bit-identical to serial.
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
    for (int i = 0; i < nt; ++i) {
        double inv_area = (mesh.tri_area[i] > 1.0e-30)
                              ? 1.0 / mesh.tri_area[i] : 0.0;

        // Sum edge fluxes
        double flux_sum = 0.0;
        for (int e = 0; e < 3; ++e) {
            flux_sum += state.edge_flux[i * 3 + e];
        }

        // RHS: dψ/dt = (1/A) Σ F_j + sources − evaporation sink. The sink is
        // depth-limited (Hermite ramp below dry_depth, see evapSink) so it
        // can never drive a depth negative.
        ydot[i] = flux_sum * inv_area + state.rainfall[i] + state.coupling_flux[i]
                  - evapSink(state.evap_rate[i], state.depth[i], opts.dry_depth);
    }
}


void computeCellContinuity(const MeshData& mesh, SurfaceStateData& state,
                            const SolverOptions2D& opts, double dt) {
    int nt = mesh.n_triangles();
    if (dt <= 0.0) {
        std::fill(state.cell_continuity_err.begin(),
                  state.cell_continuity_err.end(), 0.0);
        return;
    }
    double inv_dt = 1.0 / dt;

    // Per-cell diagnostic: each cell writes only its own cell_continuity_err[i].
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
    for (int i = 0; i < nt; ++i) {
        double area = mesh.tri_area[i];

        // Net inflow (m³/s): edge_flux is inflow-positive volumetric flux.
        double flux_sum = 0.0;
        for (int e = 0; e < 3; ++e) {
            flux_sum += state.edge_flux[i * 3 + e];
        }

        // Source volume rate (m³/s): same source terms assembleRHS uses. The
        // evaporation sink is evaluated at the accepted end-of-step depth
        // (first-order, consistent with the diagnostic character above).
        double source = (state.rainfall[i] + state.coupling_flux[i]
                         - evapSink(state.evap_rate[i], state.depth[i],
                                    opts.dry_depth)) * area;

        // Storage change rate (m³/s).
        double storage_rate =
            (state.depth[i] - state.old_depth[i]) * area * inv_dt;

        state.cell_continuity_err[i] = storage_rate - (flux_sum + source);
    }
}


void computeFaceVelocity(const MeshData& mesh, SurfaceStateData& state,
                          const SolverOptions2D& opts) {
    int nt = mesh.n_triangles();
    constexpr double kQMax = 10.0;  // clamp |b_e| against wet/dry-front spikes

    // Parallelise the OUTER per-cell loop only; each cell solves its own 2×2
    // normal-equations system and writes only face_vx[i]/face_vy[i]. The inner
    // accumulators (a00..b1) are loop-private. schedule(static) ⇒ bit-exact.
#pragma omp parallel for schedule(static) num_threads(opts.num_threads)
    for (int i = 0; i < nt; ++i) {
        double depth = state.depth[i];
        if (depth < opts.dry_depth) {
            state.face_vx[i] = 0.0;
            state.face_vy[i] = 0.0;
            continue;
        }

        // Normal equations for N·q ≈ b: NᵀN (2×2 SPD) and Nᵀb.
        double a00 = 0.0, a01 = 0.0, a11 = 0.0;
        double b0  = 0.0, b1  = 0.0;
        for (int e = 0; e < 3; ++e) {
            int idx = i * 3 + e;
            double nx  = mesh.edge_nx[idx];
            double ny  = mesh.edge_ny[idx];
            double len = mesh.edge_length[idx];
            if (len <= 1.0e-12) continue;
            double b = state.edge_flux[idx] / len;  // m²/s normal speed
            if (b >  kQMax) b =  kQMax;
            if (b < -kQMax) b = -kQMax;
            a00 += nx * nx;
            a01 += nx * ny;
            a11 += ny * ny;
            b0  += nx * b;
            b1  += ny * b;
        }

        double det = a00 * a11 - a01 * a01;
        if (std::abs(det) < 1.0e-12) {
            state.face_vx[i] = 0.0;
            state.face_vy[i] = 0.0;
            continue;
        }
        double inv_det = 1.0 / det;
        // Specific-discharge vector (m²/s).
        double qx = ( a11 * b0 - a01 * b1) * inv_det;
        double qy = (-a01 * b0 + a00 * b1) * inv_det;

        // Velocity (m/s) = specific discharge / depth.
        double inv_depth = 1.0 / depth;
        state.face_vx[i] = qx * inv_depth;
        state.face_vy[i] = qy * inv_depth;
    }
}

} // namespace openswmm::twoD
