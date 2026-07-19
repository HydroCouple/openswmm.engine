/**
 * @file VfrClosure.hpp
 * @brief Volume–free-surface (VFR) closure for a planar-bed triangular cell.
 *
 * @details Exact stage–storage relations for a cell whose bed is the plane
 *          through its three vertex elevations, after Begnudelli & Sanders
 *          (2006, 2007): the "volume/free-surface relationships" (VFRs) that
 *          make wetting and drying conservative and free of the flat-closure
 *          bias η = z̄ + V/A, which overstates the free surface of a partially
 *          wet cell by up to two-thirds of the cell relief and drives the
 *          water-climbs-uphill artifact (see plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md).
 *
 *          With sorted vertex elevations z1 ≤ z2 ≤ z3, z̄ = (z1+z2+z3)/3, and
 *          h̄ = V/A the cell-mean depth:
 *
 *            z1 < η ≤ z2 : h̄(η) = (η−z1)³ / (3(z2−z1)(z3−z1))
 *            z2 < η ≤ z3 : h̄(η) = (η−z̄) + (z3−η)³ / (3(z3−z1)(z3−z2))
 *            η ≥ z3      : h̄(η) = η − z̄                       (fully wet, flat)
 *
 *          The inverse η(h̄) is exact (closed-form cube root on the lower
 *          branch, safeguarded Newton on the upper). For use inside the
 *          implicit (CVODE/ARKODE) solvers, where dη/dV = 1/A_wet must stay
 *          bounded as the cell dries, the inverse takes a wet-area-fraction
 *          floor ε: the exact relation is continued below wet fraction ε by
 *          its tangent line (slope 1/(εA)), keeping the closure C¹ and
 *          monotone with η(V=0) slightly above z1. ε = 0 gives the exact
 *          (unregularized) relation, used by the render path.
 *
 *          Header-only, dependency-free (<cmath> only): shared by the CPU
 *          solvers, the flux calculator, the vertex render reconstruction and
 *          (eventually) the Kokkos kernels — the single source of truth for
 *          the closure.
 *
 * @see plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_VFR_CLOSURE_HPP
#define OPENSWMM_ENGINE_2D_VFR_CLOSURE_HPP

#include <cmath>

namespace openswmm::twoD {

/// Relief below which a cell is treated as flat (metres). Matches the guard
/// in the pre-existing render closure (cellFreeSurfaceElevation).
inline constexpr double kVfrFlatRelief = 1.0e-9;

/// Sort three vertex elevations in place so z1 <= z2 <= z3.
inline void vfrSort3(double& z1, double& z2, double& z3) noexcept {
    if (z1 > z2) { const double t = z1; z1 = z2; z2 = t; }
    if (z2 > z3) { const double t = z2; z2 = z3; z3 = t; }
    if (z1 > z2) { const double t = z1; z1 = z2; z2 = t; }
}

/// Wetted-area fraction A_wet/A of the planar-bed cell at stage @p eta.
/// Piecewise C0, monotone from 0 (η ≤ z1) to 1 (η ≥ z3). This is dh̄/dη.
/// Inputs must be sorted (z1 <= z2 <= z3).
inline double vfrWetFraction(double z1, double z2, double z3,
                             double eta) noexcept {
    const double relief = z3 - z1;
    if (relief < kVfrFlatRelief) return (eta > z1) ? 1.0 : 0.0;
    if (eta <= z1) return 0.0;
    if (eta >= z3) return 1.0;
    if (eta <= z2) {
        const double d21 = z2 - z1;
        if (d21 < kVfrFlatRelief) return 0.0;   // measure-zero band z1==z2
        const double t = eta - z1;
        return t * t / (d21 * relief);
    }
    // z2 < eta < z3 (z3 > z2 strictly here)
    const double d32 = z3 - z2;
    const double t = z3 - eta;
    return 1.0 - t * t / (relief * d32);
}

/// EXACT cell-mean depth h̄(η) of the planar-bed cell (per unit area; V = A·h̄).
/// Returns 0 for η ≤ z1. Inputs must be sorted.
inline double vfrMeanDepthFromEtaExact(double z1, double z2, double z3,
                                       double eta) noexcept {
    const double zbar   = (z1 + z2 + z3) / 3.0;
    const double relief = z3 - z1;
    if (relief < kVfrFlatRelief)
        return (eta > zbar) ? (eta - zbar) : 0.0;
    if (eta <= z1) return 0.0;
    if (eta >= z3) return eta - zbar;
    if (eta <= z2) {
        const double d21 = z2 - z1;
        if (d21 < kVfrFlatRelief) return 0.0;   // measure-zero band z1==z2
        const double t = eta - z1;
        return t * t * t / (3.0 * d21 * relief);
    }
    const double d32 = z3 - z2;
    const double t   = z3 - eta;
    return (eta - zbar) + t * t * t / (3.0 * relief * d32);
}

/// Stage η_s at which the wetted-area fraction equals @p eps (0 < eps < 1).
/// Inputs must be sorted and non-flat (z3 − z1 ≥ kVfrFlatRelief).
inline double vfrStageAtWetFraction(double z1, double z2, double z3,
                                    double eps) noexcept {
    const double relief = z3 - z1;
    const double d21    = z2 - z1;
    // Lower branch reaches wet fraction (z2−z1)/(z3−z1) at η = z2.
    if (eps <= d21 / relief)
        return z1 + std::sqrt(eps * d21 * relief);
    const double d32 = z3 - z2;
    return z3 - std::sqrt((1.0 - eps) * relief * d32);
}

/// Regularized cell-mean depth h̄(η): exact for η ≥ η_s (wet fraction ≥ eps),
/// tangent-line continuation below (slope eps), clamped at 0. eps == 0 gives
/// the exact relation. The EXACT inverse of vfrEtaFromMeanDepth for the same
/// eps, so head ↔ volume seeding round-trips (hotstart, reinitialize).
/// Inputs must be sorted.
inline double vfrMeanDepthFromEta(double z1, double z2, double z3,
                                  double eta, double eps) noexcept {
    const double relief = z3 - z1;
    if (eps <= 0.0 || relief < kVfrFlatRelief)
        return vfrMeanDepthFromEtaExact(z1, z2, z3, eta);
    const double eta_s = vfrStageAtWetFraction(z1, z2, z3, eps);
    if (eta >= eta_s)
        return vfrMeanDepthFromEtaExact(z1, z2, z3, eta);
    const double h_s = vfrMeanDepthFromEtaExact(z1, z2, z3, eta_s);
    const double h   = h_s - eps * (eta_s - eta);
    return (h > 0.0) ? h : 0.0;
}

/// Regularized free-surface elevation η(h̄) — the solver closure.
///
/// Exact VFR inverse while the wet fraction is ≥ eps; below that, the C¹
/// tangent-line tail η = η_s − (h̄_s − h̄)/eps, which bounds dη/dh̄ by 1/eps
/// (i.e. dη/dV ≤ 1/(εA)) and lands at η(0) = η_s − h̄_s/eps ∈ (z1, η_s).
/// eps == 0 gives the exact relation with η(h̄ ≤ 0) = z1 (render use).
/// Fully wet (h̄ ≥ z3 − z̄) reduces to the flat closure η = z̄ + h̄ exactly.
/// Inputs must be sorted.
inline double vfrEtaFromMeanDepth(double z1, double z2, double z3,
                                  double mean_depth, double eps) noexcept {
    const double zbar   = (z1 + z2 + z3) / 3.0;
    const double relief = z3 - z1;

    // Flat (or degenerate) cell: the flat closure is exact.
    if (relief < kVfrFlatRelief)
        return zbar + ((mean_depth > 0.0) ? mean_depth : 0.0);

    // Fully wet: flat closure is exact. Checked FIRST — it is the dominant case
    // in deep water (e.g. flooded urban meshes) and skips the ε-tail's sqrt/cubic
    // switch-point evaluation below, which every cell would otherwise pay each
    // RHS. The fully-wet threshold z3−z̄ always exceeds the ε-tail depth h_s, so
    // reordering is exact (the two branches never overlap).
    if (mean_depth >= z3 - zbar) return zbar + mean_depth;

    // Regularized tail: below the switch depth h_s the closure is linear.
    double eta_s = z1, h_s = 0.0;
    if (eps > 0.0) {
        eta_s = vfrStageAtWetFraction(z1, z2, z3, eps);
        h_s   = vfrMeanDepthFromEtaExact(z1, z2, z3, eta_s);
        const double h = (mean_depth > 0.0) ? mean_depth : 0.0;
        if (h <= h_s) return eta_s - (h_s - h) / eps;
    } else if (!(mean_depth > 0.0)) {
        return z1;                                // exact relation, dry limit
    }

    // Lower branch (z1 < η ≤ z2): closed-form cube root.
    const double d21     = z2 - z1;
    const double h_at_z2 = d21 * d21 / (3.0 * relief);
    if (mean_depth <= h_at_z2)
        return z1 + std::cbrt(3.0 * mean_depth * d21 * relief);

    // z2 == z3 (to rounding): the upper branch is an empty interval — the
    // fully-wet check above and h_at_z2 meet at R/3 — but 1-ulp slivers must
    // not reach the 1/(relief·d32) divisions below.
    if (z3 - z2 < kVfrFlatRelief) return zbar + mean_depth;

    // Upper branch (z2 < η < z3): safeguarded Newton on the bracket [z2, z3].
    // h̄ is strictly increasing (dh̄/dη = A_wet/A > 0 here) so this converges
    // unconditionally. Mirrors the pre-existing render-path iteration.
    const double d32   = z3 - z2;
    const double denom = 3.0 * relief * d32;
    double lo = z2, hi = z3;
    double eta = zbar + mean_depth;                 // flat-closure initial guess
    if (eta <= lo || eta >= hi) eta = 0.5 * (lo + hi);
    for (int it = 0; it < 64; ++it) {
        const double dz3 = z3 - eta;
        const double f   = (eta - zbar) + dz3 * dz3 * dz3 / denom - mean_depth;
        if (f > 0.0) hi = eta; else lo = eta;
        const double df = 1.0 - dz3 * dz3 / (relief * d32);   // A_wet/A
        double next = (df > 1.0e-12) ? eta - f / df : 0.5 * (lo + hi);
        if (next <= lo || next >= hi) next = 0.5 * (lo + hi); // safeguard
        if (std::abs(next - eta) < 1.0e-12 * (1.0 + relief)) return next;
        eta = next;
    }
    return eta;
}

/// Dry-cell free surface of the regularized closure: η(V = 0). This is the
/// value a dry cell's head must be seeded with under CELL_CLOSURE = VFR so
/// that head → volume seeding (vfrMeanDepthFromEta) returns exactly 0.
/// Inputs must be sorted.
inline double vfrDryEta(double z1, double z2, double z3, double eps) noexcept {
    return vfrEtaFromMeanDepth(z1, z2, z3, 0.0, eps);
}

/// dη/dh̄ of the regularized closure at stage @p eta: 1/max(w(η), eps).
/// Divide by the cell area A to get dη/dV (the preconditioner chain-rule
/// factor; equals 1/A for a fully wet cell, matching the flat closure).
/// Inputs must be sorted.
inline double vfrDEtaDMeanDepth(double z1, double z2, double z3,
                                double eta, double eps) noexcept {
    double w = vfrWetFraction(z1, z2, z3, eta);
    if (w < eps) w = eps;
    if (w < 1.0e-12) w = 1.0e-12;   // eps == 0 safety (exact relation, dry cell)
    return 1.0 / w;
}

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_VFR_CLOSURE_HPP
