/**
 * @file SurfaceFluxCalculator.cpp
 * @brief Implementation of gradient computation, slope limiting, and edge fluxes.
 *
 * @see SurfaceFluxCalculator.hpp
 * @ingroup engine_2d
 */

#include "SurfaceFluxCalculator.hpp"
#include "InertialKernels.hpp"
#include "../data/BoundaryData.hpp"

#include <cmath>
#include <algorithm>
#include <cstdlib>

#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#else
static inline int omp_get_max_threads() { return 1; }
#endif

namespace openswmm::twoD {

namespace {

inline int tri_nbr(const MeshData& mesh, int t, int e) {
    return (e >= 0 && e < mesh.cell_vertex_count(t)) ? mesh.cell_neighbour(t, e) : -1;
}

inline double sq(double x) noexcept { return x * x; }

// Endpoint bed elevations of local edge e of cell t, sorted z_lo <= z_hi.
// MeshData convention: edge k = (v[(k+1)%nv], v[(k+2)%nv]) (for a triangle:
// edge e is opposite vertex e). For an interior edge both incident cells see
// the same two vertices, so both compute identical (z_lo, z_hi) — the face
// depth below stays antisymmetric and the FV flux mass-conservative.
inline void edgeEndpointZ(const MeshData& mesh, int t, int e,
                          double& z_lo, double& z_hi) noexcept {
    int va, vb;
    mesh.cell_edge_vertices(t, e, va, vb);
    const double za = mesh.vz[va];
    const double zb = mesh.vz[vb];
    z_lo = (za < zb) ? za : zb;
    z_hi = (za < zb) ? zb : za;
}

// Effective conveyance depth at an edge from the upwind free surface η and the
// edge's endpoint bed elevations — Begnudelli & Sanders (2007) Eq. 14, adapted
// to the diffusive wave (it replaces the upwind CELL-MEAN depth in the Manning
// conveyance). Piecewise C¹ in η:
//   η ≤ z_lo          : 0                        (wetting gate: bed above water)
//   z_lo < η ≤ z_hi   : (η − z_lo)² / (2(z_hi − z_lo))   (partially submerged)
//   η > z_hi          : η − (z_lo + z_hi)/2      (fully submerged: mean depth)
// The quadratic branch matches value AND slope at both joins, so the flux
// stays C¹ for the implicit (Newton/FD-Jacobian) solvers — no new Hermite
// bands. A cell with water pooled below the whole shared edge conveys nothing
// across it (kills the uphill-creep / slope-stranding artifacts, per the
// paper's sloping-bed and roughened-bed tests).
// The implementation now lives in InertialKernels.hpp
// (inertial::faceDepthFromEta) — one source shared by this boundary path, the
// GPU boundary kernels, and the VFR interior-face path.
using inertial::faceDepthFromEta;

// Head-difference regularization for the diffusive-wave flux. The collapsed
// Manning flux carries √|Δη|, whose derivative ∂F/∂Δη ∝ 1/√|Δη| → ∞ as the
// water surface flattens — so in deep, near-level ponding (post-storm drainage)
// the flux Jacobian / transmissivity blow up and CVODE's step collapses. Below
// a small head ε we replace √x by a C¹ quadratic with FINITE slope at 0, which
// bounds the transmissivity (the flux becomes linear in Δη there) while keeping
// the C-property (F → 0 as Δη → 0). The bound feeds the implicit corrector,
// the FD Jacobian, AND the diagonal preconditioner (all read the stored flux),
// so the whole stiff-at-flat-water pathway is regularized in one place.
// Value comes from SolverOptions2D::flux_dh_eps (default 4 mm, parseable from
// [2D_OPTIONS] FLUX_DH_EPS); the env var OPENSWMM_2D_FLUX_DH_EPS override is
// folded into the options once per SurfaceRouter2D::initialize() — per-run,
// not process-lifetime. 0 restores the bare √.
inline double regSqrt(double x, double eps) noexcept {
    if (eps <= 0.0 || x >= eps) return std::sqrt(x);
    const double inv = 1.0 / std::sqrt(eps);
    return (1.5 * inv) * x - (0.5 * inv / eps) * x * x;
}

// Boundary-edge flux: inflow-positive contribution to cell i across boundary
// edge idx (outward discharge is negative — it leaves the cell). Returns 0 for
// a WALL or when no boundary data is attached (state.boundary == nullptr), which
// reproduces the legacy "all boundaries are walls" behaviour. The serial path
// and the Kokkos kernel implement the identical per-type math so every backend
// agrees. h_bc / per-metre flow values are resolved on the host each step
// (SurfaceRouter2D::resolveBoundaryValues); a RATING_CURVE is resolved there
// into edge_bc_flow, so it is handled identically to SPECIFIED_FLOW here.
inline double boundaryEdgeFlux(const MeshData& mesh, const SurfaceStateData& state,
                               const SolverOptions2D& opts,
                               double dh_eps, int i, int idx) noexcept {
    const BoundaryData* b = state.boundary;
    if (!b) return 0.0;
    const double L = mesh.edge_length[idx];
    const double n = mesh.mannings_n[i];
    const double depth = state.depth[i];
    const bool vfr_face =
        (opts.face_reconstruction == FaceDepth2D::VFR_FACE);
    switch (static_cast<BoundaryType>(b->edge_bc_type[idx])) {
        case BoundaryType::WALL:
            return 0.0;
        case BoundaryType::NORMAL_FLOW: {
            // Manning normal-flow outlet: per-metre outflow q = (1/n)·h^(5/3)·√S.
            const double S = b->edge_bed_slope[idx];
            if (S <= 0.0 || depth <= 0.0 || n <= 0.0) return 0.0;
            // §VFR_FACE: convey with the depth AT the boundary edge (Eq. 14
            // from the cell's free surface) instead of the cell mean, so a
            // cell whose water pools away from the outlet edge does not leak.
            double h_out = depth;
            if (vfr_face) {
                double z_lo, z_hi;
                edgeEndpointZ(mesh, i, MeshData::slot_local(idx), z_lo, z_hi);
                h_out = faceDepthFromEta(state.head[i], z_lo, z_hi);
                if (h_out <= 0.0) return 0.0;
            }
            const double h53 = h_out * std::cbrt(h_out * h_out);
            return -(h53 * std::sqrt(S) / n) * L;
        }
        case BoundaryType::SPECIFIED_FLOW:
        case BoundaryType::RATING_CURVE:
            // edge_bc_flow holds outward discharge per metre of edge (m³/s/m).
            return -b->edge_bc_flow[idx] * L;
        case BoundaryType::SPECIFIED_STAGE: {
            // Collapsed-Manning flux toward the prescribed stage h_bc, mirroring
            // the interior operator with the ghost at h_bc and the centroid→edge
            // distance Δx = 2A/(3L) (triangle centroid is 1/3 of the height up).
            // NOTE: the explicit marchers (CPU + Kokkos) no longer call this
            // branch — they integrate stage boundaries with the interior
            // local-inertial momentum law (prognostic bc_q_ vs a ghost at
            // η_bc), because this diffusive-wave conductance saturated the
            // equilibrium clamp into a Dirichlet cell and left every BC-driven
            // steady case one head-jump above its prescribed stage.
            if (n <= 0.0) return 0.0;
            const double h_bc = b->edge_bc_head[idx];
            const double dh   = state.head[i] - h_bc;
            const double A    = mesh.tri_area[i];
            // Triangle expression retained verbatim (bit-identity); a quad
            // uses the precomputed centroid→edge normal distance.
            const double dx_b = (mesh.cell_vertex_count(i) == 3)
                ? ((L > 1.0e-12) ? (2.0 * A) / (3.0 * L) : 0.0)
                : mesh.edge_dist_c[idx];
            if (dx_b <= 1.0e-12) return 0.0;
            double h_up;
            if (vfr_face) {
                // §VFR_FACE: upwind depth AT the boundary edge from whichever
                // side is higher (cell surface on outflow, prescribed stage on
                // inflow) — mirrors the interior Eq. 14 treatment.
                double z_lo, z_hi;
                edgeEndpointZ(mesh, i, MeshData::slot_local(idx), z_lo, z_hi);
                h_up = faceDepthFromEta((dh > 0.0) ? state.head[i] : h_bc,
                                        z_lo, z_hi);
            } else {
                h_up = (dh > 0.0) ? depth
                                  : std::max(h_bc - mesh.tri_cz[i], 0.0);
            }
            if (h_up <= 0.0) return 0.0;
            const double h53     = h_up * std::cbrt(h_up * h_up);
            const double sign_dh = (dh > 0.0) ? 1.0 : (dh < 0.0 ? -1.0 : 0.0);
            return -h53 * sign_dh * regSqrt(std::abs(dh), dh_eps) * L
                   / (n * std::sqrt(dx_b));
        }
    }
    return 0.0;
}

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

        const int nv_i = mesh.cell_vertex_count(i);
        for (int e = 0; e < nv_i; ++e) {
            int idx = MeshData::slot(i, e);
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

        double q[kMaxCellVerts];
        double gx_nbr[kMaxCellVerts], gy_nbr[kMaxCellVerts];
        const int nv_i = mesh.cell_vertex_count(i);

        for (int e = 0; e < nv_i; ++e) {
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

        if (nv_i == 4) {
            // Quad: the same JK product weights over 5 contributing
            // gradients (self + 4 neighbours), w_k = ∏_{j≠k} q_j / Σ.
            double qq[5] = {q0, q[0], q[1], q[2], q[3]};
            double gx[5] = {state.grad_hx[i], gx_nbr[0], gx_nbr[1], gx_nbr[2], gx_nbr[3]};
            double gy[5] = {state.grad_hy[i], gy_nbr[0], gy_nbr[1], gy_nbr[2], gy_nbr[3]};
            double num[5], den = 0.0;
            for (int k = 0; k < 5; ++k) {
                double pr = 1.0;
                for (int j = 0; j < 5; ++j) if (j != k) pr *= qq[j];
                num[k] = pr; den += pr;
            }
            double sx = 0.0, sy = 0.0;
            for (int k = 0; k < 5; ++k) { sx += num[k] / den * gx[k]; sy += num[k] / den * gy[k]; }
            state.grad_hx_lim[i] = sx;
            state.grad_hy_lim[i] = sy;
            continue;
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
        for (int e = 0; e < mesh.cell_vertex_count(i); ++e) {
            flux_sum += state.edge_flux[MeshData::slot(i, e)];
        }

        // Source volume rate (m³/s): same source terms assembleRHS uses. The
        // evaporation and infiltration sinks are evaluated at the accepted
        // end-of-step depth (first-order, consistent with the diagnostic
        // character above). Omitting the infiltration term here would leave
        // cell_continuity_err wrong on every infiltrating cell (plan §5.5.2,
        // site 4) — a silently wrong diagnostic, not a crash.
        double source = (state.rainfall[i] + state.coupling_flux[i]
                         - evapSink(state.evap_rate[i], state.depth[i],
                                    opts.dry_depth)
                         - infilSink(state.infil_rate[i], state.depth[i],
                                     opts.dry_depth)) * area;

        // Storage change rate (m³/s) — volume is the conserved state.
        double storage_rate =
            (state.volume[i] - state.old_volume[i]) * inv_dt;

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
        for (int e = 0; e < mesh.cell_vertex_count(i); ++e) {
            int idx = MeshData::slot(i, e);
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

double computeBoundaryEdgeFlux(const MeshData& mesh,
                               const SurfaceStateData& state,
                               const SolverOptions2D& opts,
                               double dh_eps, int i, int idx) noexcept {
    return boundaryEdgeFlux(mesh, state, opts, dh_eps, i, idx);
}

} // namespace openswmm::twoD
