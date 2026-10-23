// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GroundwaterDispersion.hpp
 * @brief     Least-squares gradients for groundwater dispersion.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GROUNDWATER_DISPERSION_HPP
#define OPENSWMM_GROUNDWATER_DISPERSION_HPP
#include <algorithm>
#include <array>
#include <cmath>
namespace openswmm::twoD::dispersion {
// Inverse-distance least squares; the rank-one limit retains only the
// resolved direction instead of inventing an unobserved transverse slope.
struct GradientFit {
    double xx = 0, xy = 0, yy = 0, bx = 0, by = 0;
    /**
     * @brief Accumulate one inverse-distance least-squares sample into this fit.
     * @param[in] x Finite neighbor-minus-center X displacement in m.
     * @param[in] y Finite neighbor-minus-center Y displacement in m.
     * @param[in] difference Finite neighbor-minus-center scalar difference in native
     * concentration/temperature units.
     * @post Adds weighted moment/RHS terms; a nonpositive/NaN squared distance is skipped. Does
     * not validate other nonfinite inputs or allocate. Requires exclusive fit access while
     * accumulating.
     */
    void add(double x, double y, double difference) {
        const double d = x * x + y * y;
        if (!(d > 0))
            return;
        xx += x * x / d;
        xy += x * y / d;
        yy += y * y / d;
        bx += x * difference / d;
        by += y * difference / d;
    }
    /**
     * @brief Solve the accumulated planar gradient with a rank-one fallback.
     * @return Value-owned X/Y gradient in scalar units per m. Zero for nonpositive trace; full
     * 2x2 inverse for det > 1e-12*trace^2, otherwise only the resolved rank-one direction.
     * @note Read-only allocation-free arithmetic; no nonfinite/error status or explicit
     * conditioning diagnostics. Fit must be stable during evaluation.
     */
    std::array<double, 2> value() const {
        const double trace = xx + yy, det = xx * yy - xy * xy;
        if (!(trace > 0))
            return {0, 0};
        if (det > 1e-12 * trace * trace)
            return {(yy * bx - xy * by) / det, (xx * by - xy * bx) / det};
        return {bx / trace, by / trace};
    }
};
struct Tensor {
    double xx = 0, xy = 0, yy = 0;
    /**
     * @brief Multiply a two-component gradient by this symmetric tensor.
     * @param[in] g Borrowed X/Y gradient array; not retained.
     * @return Value-owned tensor-gradient product; units are tensor coefficient times g units.
     * No allocation, validation or mutation.
     */
    std::array<double, 2> apply(const std::array<double, 2>& g) const {
        return {xx * g[0] + xy * g[1], xy * g[0] + yy * g[1]};
    }
};
/**
 * @brief Form the velocity-aligned mechanical-plus-molecular dispersion tensor.
 * @param[in] vx Finite porewater X velocity, m/s.
 * @param[in] vy Finite porewater Y velocity, m/s.
 * @param[in] longitudinal Finite nonnegative longitudinal dispersivity, m.
 * @param[in] transverse Finite nonnegative transverse dispersivity, m.
 * @param[in] molecular Finite nonnegative molecular diffusion coefficient, m2/s.
 * @return Value-owned symmetric dispersion tensor, m2/s; isotropic molecular tensor when speed
 * is not positive. No allocation/input validation or mutation.
 */
inline Tensor tensor(double vx, double vy, double longitudinal, double transverse,
                     double molecular) {
    const double speed = std::hypot(vx, vy), base = molecular + transverse * speed;
    if (!(speed > 0))
        return {molecular, 0, molecular};
    const double anisotropy = (longitudinal - transverse) / speed;
    return {base + anisotropy * vx * vx, anisotropy * vx * vy, base + anisotropy * vy * vy};
}
// Harmonic normal resistance with a deferred tangential/nonorthogonal
// correction. Exact for a linear field and constant tensor on skewed cells.
// a/b include the water cross-section per unit edge length (theta*h).
/**
 * @brief Evaluate a harmonic-normal dispersive exchange with deferred nonorthogonal correction.
 * @param[in] a Left dispersion tensor multiplied by left water cross-section per edge width
 * (theta*thickness), m3/s.
 * @param[in] b Corresponding right weighted tensor, m3/s.
 * @param[in] nx Finite X component of the unit left-to-right face normal.
 * @param[in] ny Finite Y component of the same normal.
 * @param[in] dl Positive left centroid-to-face normal distance, m.
 * @param[in] dr Positive right centroid-to-face normal distance, m.
 * @param[in] armL Value-owned left centroid-to-face XY displacement, m.
 * @param[in] armR Value-owned right centroid-to-face XY displacement, m.
 * @param[in] gradL Value-owned left concentration gradient, native concentration per m.
 * @param[in] gradR Corresponding right gradient.
 * @param[in] left Finite left dissolved concentration in native species units.
 * @param[in] right Finite right dissolved concentration in the same units.
 * @param[in] width Finite nonnegative shared edge length, m.
 * @return Signed species inventory rate in native concentration*m3/s, positive left-to-right;
 * zero if either normal tensor coefficient or normal distance is not positive.
 * @note Pure arithmetic; no positivity/donor-budget limiter is applied here, no arrays retained
 * or state mutated. Caller supplies finite coherent geometry/tensors and scales/limits the
 * result for its accepted timestep.
 */
inline double faceFlux(Tensor a, Tensor b, double nx, double ny, double dl, double dr,
                       std::array<double, 2> armL, std::array<double, 2> armR,
                       std::array<double, 2> gradL, std::array<double, 2> gradR, double left,
                       double right, double width) {
    const auto ag = a.apply(gradL), bg = b.apply(gradR);
    const double an = nx * (a.xx * nx + a.xy * ny) + ny * (a.xy * nx + a.yy * ny);
    const double bn = nx * (b.xx * nx + b.xy * ny) + ny * (b.xy * nx + b.yy * ny);
    if (!(an > 0 && bn > 0 && dl > 0 && dr > 0))
        return 0;
    const double crossL =
        gradL[0] * armL[0] + gradL[1] * armL[1] - dl * (nx * ag[0] + ny * ag[1]) / an;
    const double crossR =
        gradR[0] * armR[0] + gradR[1] * armR[1] + dr * (nx * bg[0] + ny * bg[1]) / bn;
    return width * (left - right + crossL - crossR) / (dl / an + dr / bn);
}
}
#endif
