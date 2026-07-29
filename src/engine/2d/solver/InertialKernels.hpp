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

namespace openswmm::twoD::inertial {

inline constexpr double kGravity = 9.80665;  ///< matches the existing kernels

/// Volume → (η, depth) closure — the SAME semantics as the CVODE/ARKODE
/// reconstructFromVolume: depth = max(V,0)/A; FLAT η = z_c + depth, VFR η from
/// the Begnudelli–Sanders planar-bed relation (ε-regularized).
inline void cellEtaDepth(const MeshData& m, const SolverOptions2D& o,
                         int i, double V, double& eta, double& depth) noexcept {
    const double A = m.tri_area[i];
    const double v = (V > 0.0) ? V : 0.0;
    depth = (A > 1.0e-30) ? v / A : 0.0;
    if (o.cell_closure == CellClosure2D::VFR) {
        double z1 = m.vz[m.tri_v0[i]];
        double z2 = m.vz[m.tri_v1[i]];
        double z3 = m.vz[m.tri_v2[i]];
        vfrSort3(z1, z2, z3);
        eta = vfrEtaFromMeanDepth(z1, z2, z3, depth, o.vfr_min_wet_frac);
    } else {
        eta = m.tri_cz[i] + depth;
    }
}

/// Flow depth at a face: the water surface above the higher of the two
/// interface beds. ≤ 0 means the face is a wall this substep.
inline double faceFlowDepth(double etaL, double etaR, double zface) noexcept {
    return std::max(etaL, etaR) - zface;
}

/// One local-inertial face update over Δt. @p q is the current face discharge,
/// @p qhat the θ-averaged discharge feeding the momentum balance, @p hf the
/// face flow depth (> 0), @p slope (η_R−η_L)·inv_dx_normal, @p n2 the squared
/// face Manning coefficient. Friction is semi-implicit: dividing by
/// (1 + g·Δt·n²·|q|/h^{7/3}) is unconditionally stable — h^{7/3} written as
/// h²·cbrt(h) to avoid pow().
inline double inertialFaceUpdate(double q, double qhat, double hf, double dt,
                                 double slope, double n2) noexcept {
    const double h73 = hf * hf * std::cbrt(hf);
    const double num = qhat - kGravity * hf * dt * slope;
    const double den = 1.0 + kGravity * dt * n2 * std::fabs(q) / h73;
    return num / den;
}

/// Froude-number clamp: |q| ≤ Fr_max · h_f · √(g·h_f). The steep-face guard —
/// the local-inertial scheme carries no advection term, so supercritical
/// acceleration must be capped rather than resolved.
inline double froudeCap(double q, double hf, double fr_max) noexcept {
    const double qcap = fr_max * hf * std::sqrt(kGravity * hf);
    return std::clamp(q, -qcap, qcap);
}

/// Per-cell CFL stable step: dt = α·L_char/(√(g·h) + |u|), with |u| = |q⃗|/h
/// the cell speed from the Perot reconstruction (pass 0 when unavailable —
/// the gravity-wave celerity dominates in the flows this scheme is valid for).
inline double cellCflDt(double alpha, double lchar, double h,
                        double speed) noexcept {
    const double c = std::sqrt(kGravity * h) + speed;
    return (c > 1.0e-12) ? alpha * lchar / c : 1.0e30;
}

/// Positivity scale factor λ for a cell about to export volume: outgoing
/// fluxes (+ explicit sinks) may take at most β·max(V,0) over Δt. Faces leaving
/// the cell are scaled by λ; the identical scaled flux updates both incident
/// cells, so conservation is untouched.
inline double positivityScale(double V, double outflow_m3s, double dt,
                              double beta) noexcept {
    if (outflow_m3s <= 0.0 || dt <= 0.0) return 1.0;
    const double avail = beta * std::max(V, 0.0);
    const double take  = outflow_m3s * dt;
    return (take <= avail) ? 1.0 : avail / take;
}

} // namespace openswmm::twoD::inertial

#endif // OPENSWMM_ENGINE_2D_INERTIAL_KERNELS_HPP
