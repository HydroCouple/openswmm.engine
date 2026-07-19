/**
 * @file KokkosSurfaceKernels.hpp
 * @brief Performance-portable RHS pipeline for the 2D surface router (Phase 1).
 *
 * @details Faithful Kokkos translation of the serial finite-volume RHS in
 *          src/engine/2d/solver/SurfaceFluxCalculator.cpp and the Jacobi
 *          preconditioner in CvodeSurfaceSolver.cpp. The math is byte-for-byte
 *          the same as the CPU reference; only the loop structure changes from
 *          serial `for` to `Kokkos::parallel_for`, so the same source compiles
 *          to OpenMP (Phase 1), CUDA / HIP / SYCL (Phase 2+) by selecting the
 *          Kokkos execution space at build time.
 *
 *          IMPORTANT — this layout has NO cross-cell scatter. Each stage writes
 *          only the index it owns:
 *            - per-cell stages write cell i's slot,
 *            - the edge-flux stage writes cell i's own three edge slots
 *              [i*3+e] (edge flux is stored redundantly per incident cell),
 *            - assembly gathers cell i's own three edge slots.
 *          Gradient/limiter stages only *read* neighbours. So the pipeline is
 *          embarrassingly parallel and needs no atomics — the §2.4 scatter
 *          concern in docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md does not arise for
 *          this data structure.
 *
 *          NOTE on stages: the vertex-head reconstruction and the
 *          gradient/limiter stages are recomputed each RHS call (as in the CPU
 *          path) to keep the diagnostic state fields current, even though the
 *          collapsed-FD edge flux does not consume them. Omitting them would
 *          still give an identical ydot, but we mirror the reference exactly.
 *
 * @ingroup engine_2d_gpu
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_KOKKOS_SURFACE_KERNELS_HPP
#define OPENSWMM_ENGINE_2D_GPU_KOKKOS_SURFACE_KERNELS_HPP

#include <Kokkos_Core.hpp>

namespace openswmm::twoD::gpu {

// ---------------------------------------------------------------------------
// Execution space, selected at build time by the plugin backend:
//   OPENSWMM_GPU_EXECSPACE_CUDA   -> Kokkos::Cuda   (Phase 2 device backend, NVIDIA)
//   OPENSWMM_GPU_EXECSPACE_HIP    -> Kokkos::HIP    (Phase 5 device backend, AMD)
//   OPENSWMM_GPU_EXECSPACE_SYCL   -> Kokkos::SYCL   (Phase 5 device backend, Intel)
//   OPENSWMM_GPU_EXECSPACE_OPENMP -> Kokkos::OpenMP (Phase 1 host backend)
// When none is defined (e.g. the standalone Kokkos parity test) we fall back
// to Kokkos' default space. Selecting explicitly matters once Kokkos is built
// with several spaces enabled: the *device* space wins DefaultExecutionSpace, so
// an OpenMP plugin linked against a CUDA-enabled Kokkos would silently run on
// the GPU. The macro pins each plugin to the space its CMake target intends.
//
// HIP and SYCL are top-level Kokkos:: names as of Kokkos 4.x (the project pins
// 4.7.04 via vcpkg-overlays/kokkos); on older Kokkos they live under
// Kokkos::Experimental::{HIP,SYCL}. Per strategy §6.1 these two typedef lines
// are the *entire* solver-side change needed to add the AMD and Intel backends;
// the kernels below use only Kokkos:: math and KOKKOS_LAMBDA, so they compile
// to every backend unchanged.
//
// All buffers live in ExecSpace's memory_space. Under OpenMP that is host
// memory, so the host<->device deep_copies in the solver are no-ops; under CUDA,
// HIP, or SYCL they become real device transfers, confined to advance()
// boundaries — the RHS, preconditioner and vector ops all run device-resident.
// ---------------------------------------------------------------------------
#if defined(OPENSWMM_GPU_EXECSPACE_CUDA)
using ExecSpace = Kokkos::Cuda;
#elif defined(OPENSWMM_GPU_EXECSPACE_HIP)
using ExecSpace = Kokkos::HIP;
#elif defined(OPENSWMM_GPU_EXECSPACE_SYCL)
using ExecSpace = Kokkos::SYCL;
#elif defined(OPENSWMM_GPU_EXECSPACE_OPENMP)
using ExecSpace = Kokkos::OpenMP;
#else
using ExecSpace = Kokkos::DefaultExecutionSpace;
#endif
using MemSpace  = ExecSpace::memory_space;

using DView  = Kokkos::View<double*, MemSpace>;        ///< mutable double array
using CDView = Kokkos::View<const double*, MemSpace>;  ///< const double array
using IView  = Kokkos::View<int*, MemSpace>;           ///< mutable int array
using CIView = Kokkos::View<const int*, MemSpace>;     ///< const int array

/// Immutable mesh geometry/topology mirrored to device once at initialize().
struct MeshViews {
    int    n_tri  = 0;
    int    n_vert = 0;

    CDView tri_cz, tri_area, tri_cx, tri_cy, mannings_n;   ///< [n_tri]
    CIView tri_nbr0, tri_nbr1, tri_nbr2;                   ///< [n_tri]
    CDView edge_length, edge_nx, edge_ny, edge_conveyance; ///< [n_tri*3]

    // VFR closure inputs, pre-sorted on the host at initialize.
    CDView tri_z1, tri_z2, tri_z3;      ///< [n_tri] sorted vertex elevations
    CDView edge_z_lo, edge_z_hi;        ///< [n_tri*3] sorted shared-edge endpoints

    // Vertex reconstruction stencil (CSR).
    CIView vert_ptr;   ///< [n_vert+1]
    CIView vert_idx;   ///< [nnz]
    CDView vert_wt;    ///< [nnz]
};

/// Mutable per-step state mirrored on device.
struct StateViews {
    DView head, depth;                              ///< [n_tri]
    DView grad_hx, grad_hy, grad_hx_lim, grad_hy_lim; ///< [n_tri]
    DView vert_head;                                ///< [n_vert]
    DView edge_flux;                                ///< [n_tri*3]
    DView rainfall, coupling_flux, evap_rate;       ///< [n_tri] sources
    DView precond_diag;                             ///< [n_tri] Jacobi diag

    // Per-edge boundary conditions [n_tri*3]; extent 0 ⇒ all boundary edges are
    // no-flux walls (the default, and what the standalone parity test gets).
    // Mirror of SurfaceRouter2D's BoundaryData; edge_bc_head / edge_bc_flow are
    // resolved on the host each step (timeseries / rating) and re-uploaded.
    IView edge_bc_type;     ///< BoundaryType per edge (0=WALL..4=RATING_CURVE)
    DView edge_bed_slope;   ///< NORMAL_FLOW bed slope (static)
    DView edge_bc_head;     ///< SPECIFIED_STAGE prescribed head (per step)
    DView edge_bc_flow;     ///< SPECIFIED_FLOW / RATING_CURVE per-metre flow (per step)
};

// ---------------------------------------------------------------------------
// Device-side scalar helpers (mirror SurfaceFluxCalculator.{hpp,cpp}).
// ---------------------------------------------------------------------------

/// Depth-limited evaporation sink (mirrors evapSink()).
KOKKOS_INLINE_FUNCTION
double evapSink(double rate, double depth, double dry_depth) {
    if (rate <= 0.0 || depth <= 0.0) return 0.0;
    if (depth >= dry_depth) return rate;
    const double t = depth / dry_depth;
    return rate * t * t * (3.0 - 2.0 * t);
}

KOKKOS_INLINE_FUNCTION double sq(double x) { return x * x; }

KOKKOS_INLINE_FUNCTION int nbr_of(int e, int n0, int n1, int n2) {
    return (e == 0) ? n0 : (e == 1) ? n1 : n2;
}

/// Head-difference regularization for the diffusive-wave flux (mirrors
/// SurfaceFluxCalculator::regSqrt). Below a small head ε the bare √x — whose
/// derivative ∂F/∂Δη ∝ 1/√|Δη| → ∞ as the surface flattens — is replaced by a
/// C¹ quadratic with FINITE slope at 0, bounding the transmissivity in deep,
/// near-level ponding while keeping the C-property (F → 0 as Δη → 0). ε ≤ 0
/// restores the bare √.
KOKKOS_INLINE_FUNCTION double regSqrt(double x, double eps) {
    if (eps <= 0.0 || x >= eps) return Kokkos::sqrt(x);
    const double inv = 1.0 / Kokkos::sqrt(eps);
    return (1.5 * inv) * x - (0.5 * inv / eps) * x * x;
}

// Boundary types (mirror BoundaryData.hpp BoundaryType); plain ints so the
// device kernel needs no host enum header.
enum : int { BC_WALL = 0, BC_NORMAL_FLOW = 1, BC_SPECIFIED_STAGE = 2,
             BC_SPECIFIED_FLOW = 3, BC_RATING_CURVE = 4 };

// Boundary-edge flux (inflow-positive contribution to the owning cell; outward
// discharge is negative). Byte-for-byte mirror of SurfaceFluxCalculator's
// boundaryEdgeFlux so every backend agrees. h_bc / q_flow are resolved on the
// host (timeseries / rating) and uploaded; a RATING_CURVE arrives as a per-metre
// flow in q_flow, handled identically to SPECIFIED_FLOW.
KOKKOS_INLINE_FUNCTION double faceDepthFromEta(double eta, double z_lo, double z_hi);

KOKKOS_INLINE_FUNCTION
double boundaryEdgeFlux(int bc, double slope, double h_bc, double q_flow,
                        double depth, double head, double tri_cz, double area,
                        double L, double n, double dh_eps,
                        double edge_z_lo, double edge_z_hi) {
    switch (bc) {
        case BC_NORMAL_FLOW: {
            if (slope <= 0.0 || depth <= 0.0 || n <= 0.0) return 0.0;
            // §VFR_FACE: convey with the depth AT the boundary edge from the
            // cell's free surface, so a cell whose water pools away from the
            // outlet edge does not leak.
            const double h_out = faceDepthFromEta(head, edge_z_lo, edge_z_hi);
            if (h_out <= 0.0) return 0.0;
            const double h53 = h_out * Kokkos::cbrt(h_out * h_out);
            return -(h53 * Kokkos::sqrt(slope) / n) * L;
        }
        case BC_SPECIFIED_FLOW:
        case BC_RATING_CURVE:
            return -q_flow * L;
        case BC_SPECIFIED_STAGE: {
            if (n <= 0.0) return 0.0;
            const double dh   = head - h_bc;
            const double dx_b = (L > 1.0e-12) ? (2.0 * area) / (3.0 * L) : 0.0;
            if (dx_b <= 1.0e-12) return 0.0;
            // §VFR_FACE: upwind depth AT the boundary edge from whichever side
            // is higher (cell surface on outflow, prescribed stage on inflow).
            const double h_up = faceDepthFromEta((dh > 0.0) ? head : h_bc,
                                                 edge_z_lo, edge_z_hi);
            if (h_up <= 0.0) return 0.0;
            const double h53 = h_up * Kokkos::cbrt(h_up * h_up);
            const double sgn = (dh > 0.0) ? 1.0 : (dh < 0.0 ? -1.0 : 0.0);
            return -h53 * sgn * regSqrt(Kokkos::fabs(dh), dh_eps) * L
                   / (n * Kokkos::sqrt(dx_b));
        }
        default: return 0.0;  // WALL
    }
}

// ---------------------------------------------------------------------------
// VFR closure — device mirror of src/engine/2d/mesh/VfrClosure.hpp (the single
// source of truth for the CPU solvers). Duplicated here as KOKKOS_INLINE
// device functions using Kokkos:: math, the same pattern as regSqrt /
// boundaryEdgeFlux above (the header's std:: math is not portable to CUDA/HIP
// device code). Inputs (z1,z2,z3) are pre-SORTED on the host at initialize
// (MeshViews::tri_z1..3 / edge_z_lo/hi), so no device sort is needed.
// See plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
// ---------------------------------------------------------------------------

KOKKOS_INLINE_FUNCTION double kVfrFlatReliefDev() { return 1.0e-9; }

/// Wetted-area fraction A_wet/A at stage eta (== dh̄/dη). Sorted inputs.
KOKKOS_INLINE_FUNCTION
double vfrWetFraction(double z1, double z2, double z3, double eta) {
    const double relief = z3 - z1;
    if (relief < kVfrFlatReliefDev()) return (eta > z1) ? 1.0 : 0.0;
    if (eta <= z1) return 0.0;
    if (eta >= z3) return 1.0;
    if (eta <= z2) {
        const double d21 = z2 - z1;
        if (d21 < kVfrFlatReliefDev()) return 0.0;
        const double t = eta - z1;
        return t * t / (d21 * relief);
    }
    const double d32 = z3 - z2;
    const double t = z3 - eta;
    return 1.0 - t * t / (relief * d32);
}

/// Exact cell-mean depth h̄(η). Sorted inputs.
KOKKOS_INLINE_FUNCTION
double vfrMeanDepthFromEtaExact(double z1, double z2, double z3, double eta) {
    const double zbar   = (z1 + z2 + z3) / 3.0;
    const double relief = z3 - z1;
    if (relief < kVfrFlatReliefDev()) return (eta > zbar) ? (eta - zbar) : 0.0;
    if (eta <= z1) return 0.0;
    if (eta >= z3) return eta - zbar;
    if (eta <= z2) {
        const double d21 = z2 - z1;
        if (d21 < kVfrFlatReliefDev()) return 0.0;
        const double t = eta - z1;
        return t * t * t / (3.0 * d21 * relief);
    }
    const double d32 = z3 - z2;
    const double t   = z3 - eta;
    return (eta - zbar) + t * t * t / (3.0 * relief * d32);
}

/// Stage at which the wet fraction equals eps (0<eps<1). Sorted, non-flat.
KOKKOS_INLINE_FUNCTION
double vfrStageAtWetFraction(double z1, double z2, double z3, double eps) {
    const double relief = z3 - z1;
    const double d21    = z2 - z1;
    if (eps <= d21 / relief)
        return z1 + Kokkos::sqrt(eps * d21 * relief);
    const double d32 = z3 - z2;
    return z3 - Kokkos::sqrt((1.0 - eps) * relief * d32);
}

/// Regularized free-surface elevation η(h̄) — the solver closure. Sorted inputs.
KOKKOS_INLINE_FUNCTION
double vfrEtaFromMeanDepth(double z1, double z2, double z3,
                           double mean_depth, double eps) {
    const double zbar   = (z1 + z2 + z3) / 3.0;
    const double relief = z3 - z1;
    if (relief < kVfrFlatReliefDev())
        return zbar + ((mean_depth > 0.0) ? mean_depth : 0.0);

    // Fully wet checked first — dominant case in deep water; skips the ε-tail
    // sqrt/cubic switch-point evaluation (mirrors the host VfrClosure.hpp).
    if (mean_depth >= z3 - zbar) return zbar + mean_depth;

    double eta_s = z1, h_s = 0.0;
    if (eps > 0.0) {
        eta_s = vfrStageAtWetFraction(z1, z2, z3, eps);
        h_s   = vfrMeanDepthFromEtaExact(z1, z2, z3, eta_s);
        const double h = (mean_depth > 0.0) ? mean_depth : 0.0;
        if (h <= h_s) return eta_s - (h_s - h) / eps;
    } else if (!(mean_depth > 0.0)) {
        return z1;
    }

    const double d21     = z2 - z1;
    const double h_at_z2 = d21 * d21 / (3.0 * relief);
    if (mean_depth <= h_at_z2)
        return z1 + Kokkos::cbrt(3.0 * mean_depth * d21 * relief);

    if (z3 - z2 < kVfrFlatReliefDev()) return zbar + mean_depth;

    const double d32   = z3 - z2;
    const double denom = 3.0 * relief * d32;
    double lo = z2, hi = z3;
    double eta = zbar + mean_depth;
    if (eta <= lo || eta >= hi) eta = 0.5 * (lo + hi);
    for (int it = 0; it < 64; ++it) {
        const double dz3 = z3 - eta;
        const double f   = (eta - zbar) + dz3 * dz3 * dz3 / denom - mean_depth;
        if (f > 0.0) hi = eta; else lo = eta;
        const double df = 1.0 - dz3 * dz3 / (relief * d32);
        double next = (df > 1.0e-12) ? eta - f / df : 0.5 * (lo + hi);
        if (next <= lo || next >= hi) next = 0.5 * (lo + hi);
        if (Kokkos::fabs(next - eta) < 1.0e-12 * (1.0 + relief)) return next;
        eta = next;
    }
    return eta;
}

/// dη/dh̄ = 1/max(w(η), eps). Sorted inputs. Divide by area A for dη/dV.
KOKKOS_INLINE_FUNCTION
double vfrDEtaDMeanDepth(double z1, double z2, double z3,
                         double eta, double eps) {
    double w = vfrWetFraction(z1, z2, z3, eta);
    if (w < eps) w = eps;
    if (w < 1.0e-12) w = 1.0e-12;
    return 1.0 / w;
}

/// B&S Eq. 14 face depth + wetting gate. Sorted endpoint elevations.
KOKKOS_INLINE_FUNCTION
double faceDepthFromEta(double eta, double z_lo, double z_hi) {
    if (eta <= z_lo) return 0.0;
    const double dz = z_hi - z_lo;
    if (dz < 1.0e-9) return eta - z_lo;
    if (eta <= z_hi) {
        const double t = eta - z_lo;
        return t * t / (2.0 * dz);
    }
    return eta - 0.5 * (z_lo + z_hi);
}

// ---------------------------------------------------------------------------
// RHS pipeline. Evaluates ydot = f(y) for the VOLUME formulation: y is the cell
// water volume V; the free surface η = tri_cz + V/A and mean depth h̄ = V/A are
// reconstructed per cell and drive the flux pipeline. Mirrors
// CvodeSurfaceSolver::rhs_fn step-for-step.
// ---------------------------------------------------------------------------
inline void evaluateRhs(const MeshViews& m, const StateViews& s,
                        DView y, DView ydot,
                        double dry_depth, double limiter_eps, double dh_eps,
                        double vfr_eps) {
    const int nt = m.n_tri;
    const int nv = m.n_vert;

    // Local copies of the View handles so the device lambdas capture only
    // Views (never the host-side structs).
    auto tri_cz = m.tri_cz; auto tri_area = m.tri_area;
    auto tri_cx = m.tri_cx; auto tri_cy = m.tri_cy; auto mn = m.mannings_n;
    auto nb0 = m.tri_nbr0; auto nb1 = m.tri_nbr1; auto nb2 = m.tri_nbr2;
    auto e_len = m.edge_length; auto e_nx = m.edge_nx; auto e_ny = m.edge_ny;
    auto e_conv = m.edge_conveyance;
    auto v_ptr = m.vert_ptr; auto v_idx = m.vert_idx; auto v_wt = m.vert_wt;
    auto tz1 = m.tri_z1; auto tz2 = m.tri_z2; auto tz3 = m.tri_z3;
    auto ez_lo = m.edge_z_lo; auto ez_hi = m.edge_z_hi;

    auto head = s.head; auto depth = s.depth;
    auto gx = s.grad_hx; auto gy = s.grad_hy;
    auto gxl = s.grad_hx_lim; auto gyl = s.grad_hy_lim;
    auto vhead = s.vert_head; auto eflux = s.edge_flux;
    auto rain = s.rainfall; auto coup = s.coupling_flux; auto evap = s.evap_rate;
    auto bc_type = s.edge_bc_type; auto bc_slope = s.edge_bed_slope;
    auto bc_head = s.edge_bc_head; auto bc_flow = s.edge_bc_flow;
    const bool has_bc = (bc_type.extent(0) > 0);

    // 1. Unpack y -> head, depth (VOLUME formulation): depth stays the mean
    //    depth h̄ = V/A; the free surface η is the VFR planar-bed closure
    //    (mirrors reconstructFromVolume under CELL_CLOSURE=VFR).
    Kokkos::parallel_for("rhs_unpack", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            const double A = tri_area(i);
            const double v = y(i) > 0.0 ? y(i) : 0.0;
            const double d = (A > 1.0e-30) ? v / A : 0.0;
            depth(i) = d;
            head(i)  = vfrEtaFromMeanDepth(tz1(i), tz2(i), tz3(i), d, vfr_eps);
        });

    // 2. Reconstruct head at vertices (pseudo-Laplacian, CSR gather).
    Kokkos::parallel_for("rhs_vertex_heads", Kokkos::RangePolicy<ExecSpace>(0, nv),
        KOKKOS_LAMBDA(int b) {
            const int start = v_ptr(b);
            const int end   = v_ptr(b + 1);
            double h = 0.0;
            for (int k = start; k < end; ++k) h += v_wt(k) * head(v_idx(k));
            vhead(b) = h;
        });

    // 3. Unlimited gradients (Green-Gauss).
    Kokkos::parallel_for("rhs_grad_unlimited", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            const double inv_area = (tri_area(i) > 1.0e-30) ? 1.0 / tri_area(i) : 0.0;
            const int n0 = nb0(i), n1 = nb1(i), n2 = nb2(i);
            double ax = 0.0, ay = 0.0;
            for (int e = 0; e < 3; ++e) {
                const int idx = i * 3 + e;
                const int nbr = nbr_of(e, n0, n1, n2);
                const double h_edge = (nbr >= 0) ? 0.5 * (head(i) + head(nbr))
                                                 : head(i);
                ax += h_edge * e_nx(idx) * e_len(idx);
                ay += h_edge * e_ny(idx) * e_len(idx);
            }
            gx(i) = ax * inv_area;
            gy(i) = ay * inv_area;
        });

    // 4. Jawahar-Kamath limited gradients (reads stage-3 neighbour gradients).
    Kokkos::parallel_for("rhs_grad_limited", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            const double eps2 = limiter_eps * limiter_eps;
            const double q0 = sq(gx(i)) + sq(gy(i)) + eps2;
            const int n0 = nb0(i), n1 = nb1(i), n2 = nb2(i);
            double q[3], gxn[3], gyn[3];
            for (int e = 0; e < 3; ++e) {
                const int nbr = nbr_of(e, n0, n1, n2);
                if (nbr >= 0) {
                    q[e]   = sq(gx(nbr)) + sq(gy(nbr)) + eps2;
                    gxn[e] = gx(nbr);
                    gyn[e] = gy(nbr);
                } else {
                    q[e]   = q0;
                    gxn[e] = gx(i);
                    gyn[e] = gy(i);
                }
            }
            const double m0 = q[0] * q[1] * q[2];
            const double m1 = q0   * q[1] * q[2];
            const double m2 = q0   * q[0] * q[2];
            const double m3 = q0   * q[0] * q[1];
            const double denom = m0 + m1 + m2 + m3;
            const double w0 = m0 / denom, w1 = m1 / denom,
                         w2 = m2 / denom, w3 = m3 / denom;
            gxl(i) = w0 * gx(i) + w1 * gxn[0] + w2 * gxn[1] + w3 * gxn[2];
            gyl(i) = w0 * gy(i) + w1 * gyn[0] + w2 * gyn[1] + w3 * gyn[2];
        });

    // 5. Edge fluxes (collapsed-FD Manning diffusive wave, hydrostatic upwind).
    Kokkos::parallel_for("rhs_edge_flux", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            const int n0 = nb0(i), n1 = nb1(i), n2 = nb2(i);
            for (int e = 0; e < 3; ++e) {
                const int idx = i * 3 + e;
                const int nbr = nbr_of(e, n0, n1, n2);
                if (nbr < 0) {
                    // Domain boundary: no-flux wall, or the configured BC.
                    eflux(idx) = has_bc
                        ? boundaryEdgeFlux(bc_type(idx), bc_slope(idx),
                              bc_head(idx), bc_flow(idx), depth(i), head(i),
                              tri_cz(i), tri_area(i), e_len(idx), mn(i), dh_eps,
                              ez_lo(idx), ez_hi(idx))
                        : 0.0;
                    continue;
                }

                const double h_L = head(i), h_R = head(nbr);
                const int up = (h_L >= h_R) ? i : nbr;
                // §VFR_FACE: conveyance depth AT the shared edge (B&S Eq. 14)
                // from the upwind free surface and the edge's endpoint beds —
                // zero when the upwind surface is below the whole edge (wetting
                // gate). Both incident cells see the same (z_lo,z_hi,η_up), so
                // the redundant per-cell slots stay antisymmetric.
                const double eta_up   = (up == i) ? h_L : h_R;
                const double depth_up = faceDepthFromEta(eta_up, ez_lo(idx),
                                                         ez_hi(idx));
                if (depth_up <= 0.0) { eflux(idx) = 0.0; continue; }

                const double dxx = tri_cx(i) - tri_cx(nbr);
                const double dxy = tri_cy(i) - tri_cy(nbr);
                const double dx  = Kokkos::sqrt(dxx * dxx + dxy * dxy);
                if (dx < 1.0e-12) { eflux(idx) = 0.0; continue; }

                const double dh      = h_L - h_R;
                const double abs_dh  = Kokkos::fabs(dh);
                const double sign_dh = (dh > 0.0) ? 1.0 : (dh < 0.0 ? -1.0 : 0.0);
                const double h53     = depth_up * Kokkos::cbrt(depth_up * depth_up);
                const double n_up    = mn(up);
                const double xi      = e_len(idx);

                double F_e = -h53 * sign_dh * regSqrt(abs_dh, dh_eps) * xi
                             / (n_up * Kokkos::sqrt(dx));

                // No explicit wet/dry shutoff: under the flat volume closure the
                // source-side depth (= V/A) vanishes smoothly as the cell empties,
                // so h^(5/3) → 0 and the flux shuts off C¹-smoothly with no 1 mm
                // Hermite band (the depth_up ≤ 0 guard above zeroes a dry source).
                // The √|Δη| is C¹-regularized (regSqrt) so the transmissivity stays
                // bounded as the surface flattens (deep-water stiffness).
                F_e *= e_conv(idx);
                eflux(idx) = F_e;
            }
        });

    // 6. Assemble RHS (VOLUME): dV/dt = ΣF + A·(rain + coupling − evapSink). No
    //    1/A — V is the conserved state, so interior fluxes telescope exactly.
    Kokkos::parallel_for("rhs_assemble", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            const double area = tri_area(i);
            double fs = 0.0;
            for (int e = 0; e < 3; ++e) fs += eflux(i * 3 + e);
            ydot(i) = fs + area * (rain(i) + coup(i)
                                   - evapSink(evap(i), depth(i), dry_depth));
        });
}

// ---------------------------------------------------------------------------
// Jacobi preconditioner (mirrors psetup_fn / psolve_fn).
// ---------------------------------------------------------------------------

/// Build the per-cell diagonal D from the most recent edge fluxes/heads.
///
/// The secant transmissivity T = |F|/|Δη| is floored at the flux's own
/// head-difference regularization scale (FLUX_DH_EPS), NOT a bare 1e-9. Under
/// the VFR closure a lake-at-rest shoreline restores the C-property: incident
/// heads EQUALIZE, so Δη → 0 while |F| carries a small regularized residual.
/// A 1e-9 floor then makes T (and the Jacobi diagonal D) blow up; the psolve
/// z = r/(1−γD) collapses the Newton correction toward 0, CVODE's convergence
/// heuristic passes spuriously, and it accepts non-conservative steps (observed:
/// mass created, −400% continuity on a small JACOBI mesh). Flooring at the same
/// Δη scale where the flux is linearized keeps D bounded and the step honest.
/// FLAT never equalizes heads (that is the artifact), so it never hit this.
/// The dη/dV chain-rule factor the serial psetup carries is intentionally
/// omitted here — preconditioner-only, and it compounds the same spike.
inline void precondSetup(const MeshViews& m, const StateViews& s, double dh_eps) {
    const int nt = m.n_tri;
    auto nb0 = m.tri_nbr0; auto nb1 = m.tri_nbr1; auto nb2 = m.tri_nbr2;
    auto tri_area = m.tri_area;
    auto head = s.head; auto eflux = s.edge_flux; auto D = s.precond_diag;
    const double dh_floor = Kokkos::fmax(dh_eps, 1.0e-9);
    Kokkos::parallel_for("prec_setup", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            const int n0 = nb0(i), n1 = nb1(i), n2 = nb2(i);
            double T_sum = 0.0;
            for (int e = 0; e < 3; ++e) {
                const int nbr = nbr_of(e, n0, n1, n2);
                if (nbr < 0) continue;
                const double dh = Kokkos::fabs(head(i) - head(nbr));
                const double F  = Kokkos::fabs(eflux(i * 3 + e));
                T_sum += F / Kokkos::fmax(dh, dh_floor);
            }
            const double inv_area = (tri_area(i) > 1.0e-30) ? 1.0 / tri_area(i) : 0.0;
            D(i) = -T_sum * inv_area;
        });
}

/// Apply (I − γD)^{-1} element-wise: z = r / (1 − γ·D).
inline void precondSolve(const StateViews& s, DView r, DView z, double gamma) {
    const int nt = static_cast<int>(s.precond_diag.extent(0));
    auto D = s.precond_diag;
    Kokkos::parallel_for("prec_solve", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            constexpr double m_floor = 1.0e-12;
            double mval = 1.0 - gamma * D(i);
            if (Kokkos::fabs(mval) < m_floor) mval = Kokkos::copysign(m_floor, mval);
            z(i) = r(i) / mval;
        });
}

} // namespace openswmm::twoD::gpu

#endif // OPENSWMM_ENGINE_2D_GPU_KOKKOS_SURFACE_KERNELS_HPP
