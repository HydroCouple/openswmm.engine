/**
 * @file DiffusiveConductance.hpp
 * @brief Diffusive conductance K(ψ_o) for Manning-based overland flow.
 *
 * @details Implements Eq. [3] from Kumar et al. (2009):
 *          K(ψ_o) = ψ_o^{2/3} / (n · |∂h/∂s|^{1/2})
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §3.2
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_DIFFUSIVE_CONDUCTANCE_HPP
#define OPENSWMM_ENGINE_2D_DIFFUSIVE_CONDUCTANCE_HPP

#include <cmath>
#include <algorithm>

namespace openswmm::twoD {

/**
 * @brief Compute diffusive conductance K(ψ_o) for Manning-based overland flow.
 *
 * @param depth       Overland flow depth ψ_o (m). Must be >= 0.
 * @param mannings_n  Manning's roughness coefficient (s/m^{1/3}).
 * @param grad_h_mag  Magnitude of the head gradient |∇h| (dimensionless).
 * @param dry_depth   Threshold below which conductance is zero (m).
 * @return Diffusive conductance K (m/s). Zero if depth < dry_depth.
 */
inline double diffusiveConductance(double depth, double mannings_n,
                                    double grad_h_mag, double dry_depth) noexcept {
    if (depth < dry_depth) return 0.0;
    double denom = mannings_n * std::sqrt(std::max(grad_h_mag, 1.0e-12));
    return std::cbrt(depth * depth) / denom;  // depth^(2/3)
}

/**
 * @brief Compute diffusive conductance with smoothed transition near dry threshold.
 *
 * Uses a cubic Hermite ramp over [0, dry_depth] instead of a hard cutoff.
 * This improves CVODE convergence by avoiding discontinuities in the RHS.
 *
 * @param depth       Overland flow depth ψ_o (m).
 * @param mannings_n  Manning's roughness coefficient.
 * @param grad_h_mag  Magnitude of head gradient.
 * @param dry_depth   Transition depth (m).
 * @return Smoothed diffusive conductance K (m/s).
 */
inline double diffusiveConductanceSmooth(double depth, double mannings_n,
                                          double grad_h_mag, double dry_depth) noexcept {
    if (depth <= 0.0) return 0.0;

    double denom = mannings_n * std::sqrt(std::max(grad_h_mag, 1.0e-12));
    double K_raw = std::cbrt(depth * depth) / denom;

    if (depth >= dry_depth) return K_raw;

    // Smooth ramp: cubic Hermite s(t) = 3t² - 2t³ where t = depth/dry_depth
    double t = depth / dry_depth;
    double s = t * t * (3.0 - 2.0 * t);
    return s * K_raw;
}

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_DIFFUSIVE_CONDUCTANCE_HPP
