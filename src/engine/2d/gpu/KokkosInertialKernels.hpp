/**
 * @file KokkosInertialKernels.hpp
 * @brief Performance-portable RHS + preconditioner for the local-inertial solver.
 *
 * @details Kokkos translation of the explicit-gravity LISFLOOD-FP scheme in
 *          src/engine/2d/solver/ArkodeSurfaceSolver.cpp (the inertial fe/fi
 *          callbacks + block-Jacobi preconditioner). Only the *scalable* split
 *          is ported: gravity transport is EXPLICIT (continuity + surface
 *          gradient) and only the per-edge friction is IMPLICIT — a diagonal
 *          operator, so the preconditioner is exact and there is NO global
 *          solve. The same source compiles to OpenMP / CUDA / HIP / SYCL by
 *          selecting ExecSpace (see KokkosSurfaceKernels.hpp).
 *
 *          Conservation: the continuity divergence is a race-free per-cell CSR
 *          gather (each cell sums its incident edges), and one signed q per edge
 *          telescopes exactly — identical structure to the serial path.
 *
 * @ingroup engine_2d_gpu
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_KOKKOS_INERTIAL_KERNELS_HPP
#define OPENSWMM_ENGINE_2D_GPU_KOKKOS_INERTIAL_KERNELS_HPP

#include "KokkosSurfaceKernels.hpp"   // ExecSpace, DView/CDView/IView/CIView, MeshViews, StateViews

namespace openswmm::twoD::gpu {

/// Device mirror of InertialEdges (unique interior edges + per-cell CSR
/// incidence). Built once at initialize() from the host InertialEdges.
struct InertialEdgeViews {
    int    ne = 0;
    CIView cL, cR;            ///< [ne] incident cells (oriented cL→cR)
    CDView xi, inv_dx, zface; ///< [ne] edge length, 1/Δx, interface bed
    CIView slotL, slotR;      ///< [ne] flat mesh edge slots for q→edge_flux writeback
    CIView cell_ptr;          ///< [n_tri+1] CSR row pointers
    CIView cell_edge;         ///< [nnz] incident edge id
    CDView cell_sign;         ///< [nnz] +1 (cell==cL) / −1 (cell==cR)
};

/// Gravity constant (SI). Matches ArkodeSurfaceSolver.
KOKKOS_INLINE_FUNCTION constexpr double inertialG() { return 9.80665; }

// ---------------------------------------------------------------------------
// Explicit half F_E: cell sources + continuity divergence + surface-gradient
// gravity. y layout = [V(0..nt), q(nt..nt+ne)].
// ---------------------------------------------------------------------------
inline void evaluateInertialFe(const MeshViews& m, const StateViews& s,
                               const InertialEdgeViews& E,
                               DView y, DView ydot, double dry_depth) {
    const int nt = m.n_tri;
    const int ne = E.ne;
    auto tri_cz = m.tri_cz; auto tri_area = m.tri_area;
    auto head = s.head; auto depth = s.depth;
    auto rain = s.rainfall; auto coup = s.coupling_flux; auto evap = s.evap_rate;
    auto cL = E.cL; auto cR = E.cR; auto xi = E.xi; auto inv_dx = E.inv_dx;
    auto zface = E.zface; auto cptr = E.cell_ptr; auto cedge = E.cell_edge;
    auto csign = E.cell_sign;
    const double G = inertialG();

    // 1. Reconstruct head/depth + cell source forcing.
    Kokkos::parallel_for("inert_fe_cells", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            const double A = tri_area(i);
            const double v = y(i) > 0.0 ? y(i) : 0.0;
            const double d = (A > 1.0e-30) ? v / A : 0.0;
            depth(i) = d;
            head(i)  = tri_cz(i) + d;
            ydot(i)  = A * (rain(i) + coup(i) - evapSink(evap(i), d, dry_depth));
        });

    // 2. Continuity divergence added to the cell rows (race-free CSR gather).
    Kokkos::parallel_for("inert_fe_cont", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            double sgx = 0.0;
            for (int k = cptr(i); k < cptr(i + 1); ++k) {
                const int e = cedge(k);
                sgx += csign(k) * y(nt + e) * xi(e);
            }
            ydot(i) -= sgx;
        });

    // 3. Surface-gradient gravity on the edge rows.
    Kokkos::parallel_for("inert_fe_grav", Kokkos::RangePolicy<ExecSpace>(0, ne),
        KOKKOS_LAMBDA(int e) {
            const int l = cL(e), r = cR(e);
            const double hf = Kokkos::fmax(head(l), head(r)) - zface(e);
            ydot(nt + e) = (hf > dry_depth)
                ? -G * hf * (head(r) - head(l)) * inv_dx(e)
                : 0.0;
        });
}

// ---------------------------------------------------------------------------
// Implicit half F_I: per-edge friction only (the stiff, diagonal term). Cell
// rows carry no implicit term (transport is explicit).
// ---------------------------------------------------------------------------
inline void evaluateInertialFi(const MeshViews& m, const StateViews& s,
                               const InertialEdgeViews& E,
                               DView y, DView ydot, double dry_depth) {
    const int nt = m.n_tri;
    const int ne = E.ne;
    auto tri_cz = m.tri_cz; auto tri_area = m.tri_area; auto mn = m.mannings_n;
    auto head = s.head;
    auto cL = E.cL; auto cR = E.cR; auto zface = E.zface;
    const double G = inertialG();

    // Reconstruct head (for h_f) and zero the cell rows.
    Kokkos::parallel_for("inert_fi_cells", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            const double A = tri_area(i);
            const double v = y(i) > 0.0 ? y(i) : 0.0;
            head(i)  = tri_cz(i) + ((A > 1.0e-30) ? v / A : 0.0);
            ydot(i)  = 0.0;
        });

    // Friction on the edge rows; dry interface relaxes residual q to 0.
    Kokkos::parallel_for("inert_fi_fric", Kokkos::RangePolicy<ExecSpace>(0, ne),
        KOKKOS_LAMBDA(int e) {
            const int l = cL(e), r = cR(e);
            const double hf = Kokkos::fmax(head(l), head(r)) - zface(e);
            const double qe = y(nt + e);
            double dq;
            if (hf > dry_depth) {
                const double n    = 0.5 * (mn(l) + mn(r));
                const double hf73 = Kokkos::pow(hf, 7.0 / 3.0);
                dq = -G * n * n * qe * Kokkos::fabs(qe) / hf73;
            } else {
                dq = -qe;
            }
            ydot(nt + e) = dq;
        });
}

// ---------------------------------------------------------------------------
// Block-Jacobi preconditioner. The implicit operator is block-diagonal —
// I on the cells (no implicit term) and (I+γR) on the edges (friction) — so the
// preconditioner is its EXACT inverse: identity on cells, w_e=1/(1+γR_e) on
// edges. No scatter, no global solve.
// ---------------------------------------------------------------------------
inline void precondInertialSetup(const MeshViews& m, const StateViews& s,
                                 const InertialEdgeViews& E,
                                 DView y, DView wq, double gamma, double dry_depth) {
    const int nt = m.n_tri;
    const int ne = E.ne;
    auto tri_cz = m.tri_cz; auto tri_area = m.tri_area; auto mn = m.mannings_n;
    auto head = s.head;
    auto cL = E.cL; auto cR = E.cR; auto zface = E.zface;
    const double G = inertialG();

    Kokkos::parallel_for("inert_pc_head", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) {
            const double A = tri_area(i);
            const double v = y(i) > 0.0 ? y(i) : 0.0;
            head(i) = tri_cz(i) + ((A > 1.0e-30) ? v / A : 0.0);
        });

    Kokkos::parallel_for("inert_pc_wq", Kokkos::RangePolicy<ExecSpace>(0, ne),
        KOKKOS_LAMBDA(int e) {
            const int l = cL(e), r = cR(e);
            const double hf = Kokkos::fmax(head(l), head(r)) - zface(e);
            double R;
            if (hf > dry_depth) {
                const double n    = 0.5 * (mn(l) + mn(r));
                const double hf73 = Kokkos::pow(hf, 7.0 / 3.0);
                R = 2.0 * G * n * n * Kokkos::fabs(y(nt + e)) / hf73;
            } else {
                R = 1.0;
            }
            wq(e) = 1.0 / (1.0 + gamma * R);
        });
}

/// Apply the block-Jacobi preconditioner: z_cells = r_cells (identity),
/// z_edges = w_e · r_edges.
inline void precondInertialSolve(int nt, const InertialEdgeViews& E,
                                 DView r, DView z, DView wq) {
    const int ne = E.ne;
    Kokkos::parallel_for("inert_ps_cells", Kokkos::RangePolicy<ExecSpace>(0, nt),
        KOKKOS_LAMBDA(int i) { z(i) = r(i); });
    Kokkos::parallel_for("inert_ps_edges", Kokkos::RangePolicy<ExecSpace>(0, ne),
        KOKKOS_LAMBDA(int e) { z(nt + e) = wq(e) * r(nt + e); });
}

/// Project the prognostic discharge q back onto the redundant edge_flux slots
/// (inflow-positive to the storing cell), for the downstream diagnostics.
inline void writebackEdgeFlux(int nt, const InertialEdgeViews& E,
                              DView y, DView edge_flux) {
    const int ne = E.ne;
    const std::size_t n3 = edge_flux.extent(0);
    auto xi = E.xi; auto slotL = E.slotL; auto slotR = E.slotR;
    Kokkos::parallel_for("inert_eflux_zero",
        Kokkos::RangePolicy<ExecSpace>(0, static_cast<int>(n3)),
        KOKKOS_LAMBDA(int k) { edge_flux(k) = 0.0; });
    Kokkos::parallel_for("inert_eflux_write", Kokkos::RangePolicy<ExecSpace>(0, ne),
        KOKKOS_LAMBDA(int e) {
            const double f = y(nt + e) * xi(e);
            edge_flux(slotL(e)) = -f;
            edge_flux(slotR(e)) = +f;
        });
}

} // namespace openswmm::twoD::gpu

#endif // OPENSWMM_ENGINE_2D_GPU_KOKKOS_INERTIAL_KERNELS_HPP
