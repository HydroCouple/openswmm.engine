/**
 * @file InertialKernels.hpp
 * @brief Single-source flat kernels for the explicit local-inertial marcher.
 *
 * @details The numerical core of ExplicitInertialSolver (2026-07-29 2D
 *          reimplementation plan): the de Almeida & Bates (2013) local-inertial
 *          face update on unstructured unique faces, the FLAT/VFR volume→η
 *          closure, the Froude clamp, and the CFL step bound. Everything here
 *          is a plain inline function over scalars/raw pointers — no owning
 *          containers, no allocation, no exceptions — so the same bodies can be
 *          annotated for Kokkos in the deferred parallel phase.
 *
 *          Scheme summary (face e between cells L,R; q = unit-width discharge
 *          normal to the face, positive cL→cR; SI, g = 9.80665):
 *
 *            h_f  = max(η_L, η_R) − max(z_L, z_R)          flow depth at face
 *            S_f  = (η_R − η_L) · inv_dx_normal            surface slope
 *            q̂    = θ·q + (1−θ)·½(q⃗_L + q⃗_R)·n̂            lateral θ-average
 *            q*   = (q̂ − g·h_f·Δt·S_f) / (1 + g·Δt·n_f²·|q|/h_f^{7/3})
 *            q^{n+1} = clamp(q*, ±Fr_max·h_f·√(g·h_f))
 *
 *          The friction denominator is the exact semi-implicit form used by
 *          the ARKODE inertial path (unconditionally stable in friction);
 *          θ = 1 recovers Bates et al. (2010). Well-balancedness: at rest
 *          η_L = η_R ⇒ S_f = 0 and q stays exactly 0 for any bathymetry; a dry
 *          neighbour standing higher gives h_f ≤ 0 ⇒ wall (no uphill creep).
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_INERTIAL_KERNELS_HPP
#define OPENSWMM_ENGINE_2D_INERTIAL_KERNELS_HPP

#include <algorithm>
#include <cmath>

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../mesh/VfrClosure.hpp"

// Portable kernel-function marker (P5 Kokkos port): host builds get plain
// `inline`; the GPU plugin defines OPENSWMM_KERNEL_FN to
// KOKKOS_INLINE_FUNCTION *before* including this header so the identical
// scalar bodies compile for the device (CUDA/HIP/SYCL) as well. The math uses
// std:: functions deliberately — CUDA/HIP provide device overloads and Kokkos
// enables relaxed-constexpr; keeping one spelling preserves single-source
// bit-identity with the serial marcher.
#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::twoD::inertial {

inline constexpr double kGravity = 9.80665;  ///< matches the existing kernels

/// Free-surface difference below which a face slope is numerical zero (m).
/// Without it, the local-inertial q integrates even 1-ulp closure round-trip
/// noise up to its Manning equilibrium (√-amplification: Δη ~ 1e-16 sustains
/// q ~ 1e-6 m²/s), so lake-at-rest would drift measurably. 1e-12 m is far
/// below any physical head; with the slope zeroed the friction denominator
/// decays q geometrically and rest states are exact.
inline constexpr double kEtaDeadband = 1.0e-12;

/// Scalar core of the V → (η, depth) closure (device-callable; the MeshData
/// wrapper below loads the geometry and forwards — identical ops and order).
OPENSWMM_KERNEL_FN void etaDepthScalar(double area, double cz,
                                       double z1, double z2, double z3,
                                       bool vfr, double vfr_min_wet_frac,
                                       double V, double& eta,
                                       double& depth) noexcept {
    const double v = (V > 0.0) ? V : 0.0;
    depth = (area > 1.0e-30) ? v / area : 0.0;
    if (vfr) {
        vfrSort3(z1, z2, z3);
        eta = vfrEtaFromMeanDepth(z1, z2, z3, depth, vfr_min_wet_frac);
    } else {
        eta = cz + depth;
    }
}

/// Scalar core of the η → V inverse (device-callable).
OPENSWMM_KERNEL_FN double volumeFromEtaScalar(double area, double cz,
                                              double z1, double z2, double z3,
                                              bool vfr, double vfr_min_wet_frac,
                                              double eta) noexcept {
    if (vfr) {
        vfrSort3(z1, z2, z3);
        return area * vfrMeanDepthFromEta(z1, z2, z3, eta, vfr_min_wet_frac);
    }
    const double d = eta - cz;
    return (d > 0.0) ? area * d : 0.0;
}

/// Volume → (η, depth) closure — the SAME semantics as the CVODE/ARKODE
/// reconstructFromVolume: depth = max(V,0)/A; FLAT η = z_c + depth, VFR η from
/// the Begnudelli–Sanders planar-bed relation (ε-regularized).
inline void cellEtaDepth(const MeshData& m, const SolverOptions2D& o,
                         int i, double V, double& eta, double& depth) noexcept {
    etaDepthScalar(m.tri_area[i], m.tri_cz[i],
                   m.vz[m.tri_v0[i]], m.vz[m.tri_v1[i]], m.vz[m.tri_v2[i]],
                   o.cell_closure == CellClosure2D::VFR, o.vfr_min_wet_frac,
                   V, eta, depth);
}

/// Exact inverse of the closure: cell volume holding free surface η — FLAT
/// A·max(0, η − z_c); VFR the regularized B&S forward relation (round-trips
/// with cellEtaDepth). Closure-consistent seeding for tests/hotstart.
inline double cellVolumeFromEta(const MeshData& m, const SolverOptions2D& o,
                                int i, double eta) noexcept {
    return volumeFromEtaScalar(m.tri_area[i], m.tri_cz[i],
                               m.vz[m.tri_v0[i]], m.vz[m.tri_v1[i]],
                               m.vz[m.tri_v2[i]],
                               o.cell_closure == CellClosure2D::VFR,
                               o.vfr_min_wet_frac, eta);
}

/// Flow depth at a face: the water surface above the higher of the two
/// interface beds. ≤ 0 means the face is a wall this substep.
OPENSWMM_KERNEL_FN double faceFlowDepth(double etaL, double etaR, double zface) noexcept {
    return std::max(etaL, etaR) - zface;
}

/// One local-inertial face update over Δt. @p q is the current face discharge,
/// @p qhat the θ-averaged discharge feeding the momentum balance, @p hf the
/// face flow depth (> 0), @p slope (η_R−η_L)·inv_dx_normal, @p n2 the squared
/// face Manning coefficient. Friction is semi-implicit: dividing by
/// (1 + g·Δt·n²·|q|/h^{7/3}) is unconditionally stable — h^{7/3} written as
/// h²·cbrt(h) to avoid pow().
OPENSWMM_KERNEL_FN double inertialFaceUpdate(double q, double qhat, double hf, double dt,
                                 double slope, double n2) noexcept {
    const double h73 = hf * hf * std::cbrt(hf);
    const double num = qhat - kGravity * hf * dt * slope;
    const double den = 1.0 + kGravity * dt * n2 * std::fabs(q) / h73;
    return num / den;
}

/// Froude-number clamp: |q| ≤ Fr_max · h_f · √(g·h_f). The steep-face guard —
/// the local-inertial scheme carries no advection term, so supercritical
/// acceleration must be capped rather than resolved.
OPENSWMM_KERNEL_FN double froudeCap(double q, double hf, double fr_max) noexcept {
    const double qcap = fr_max * hf * std::sqrt(kGravity * hf);
    return std::clamp(q, -qcap, qcap);
}

/// Per-cell CFL stable step: dt = α·L_char/(√(g·h) + |u|), with |u| = |q⃗|/h
/// the cell speed from the Perot reconstruction (pass 0 when unavailable —
/// the gravity-wave celerity dominates in the flows this scheme is valid for).
OPENSWMM_KERNEL_FN double cellCflDt(double alpha, double lchar, double h,
                        double speed) noexcept {
    const double c = std::sqrt(kGravity * h) + speed;
    return (c > 1.0e-12) ? alpha * lchar / c : 1.0e30;
}

/// Positivity scale factor λ for a cell about to export volume: outgoing
/// fluxes (+ explicit sinks) may take at most β·max(V,0) over Δt. Faces leaving
/// the cell are scaled by λ; the identical scaled flux updates both incident
/// cells, so conservation is untouched.
OPENSWMM_KERNEL_FN double positivityScale(double V, double outflow_m3s, double dt,
                              double beta) noexcept {
    if (outflow_m3s <= 0.0 || dt <= 0.0) return 1.0;
    const double avail = beta * std::max(V, 0.0);
    const double take  = outflow_m3s * dt;
    return (take <= avail) ? 1.0 : avail / take;
}

} // namespace openswmm::twoD::inertial

#endif // OPENSWMM_ENGINE_2D_INERTIAL_KERNELS_HPP
