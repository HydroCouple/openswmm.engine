// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifndef OPENSWMM_MOMENTUM_PHYSICS_2D_HPP
#define OPENSWMM_MOMENTUM_PHYSICS_2D_HPP

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <vector>

namespace openswmm::twoD {
struct MeshData;
struct InertialEdges;
struct BoundaryData;

/** Conservative depth-averaged Smagorinsky face stresses. Least-squares
 * gradients are corrected along the face normal to reproduce the actual
 * centroid-to-centroid velocity difference on nonorthogonal meshes. Every
 * shared face transfers equal/opposite momentum. Physical boundaries have
 * zero viscous traction. Stage bounds and a global energy check qualify
 * each nonlinear explicit mixing substep. Scratch is not checkpoint state.
 */
class MomentumPhysics2D {
public:
    std::vector<double> viscosity, strain, vorticity; ///< per cell: m2/s, |S| and |omega| in 1/s
    /// Distance from each centroid to the nearest mesh boundary edge (m); built on
    /// first use by the depth mixing length, which caps l = min(c h, kappa d).
    std::vector<double> wall_distance;
    static constexpr double kappa = 0.41;
    double last_limit = std::numeric_limits<double>::infinity();
    uint64_t substeps = 0, retries = 0;

    /** Exact tangential quadratic decay on each selected exterior wall face.
     * Palindromic face composition is dissipative for arbitrary corners and
     * second-order at fixed h. Uses actual wetted edge area for VFR, cell h
     * for FLAT. Returns kinetic energy removed divided by water density.
     */
    static double wallDrag(const MeshData& m, const BoundaryData& b,
                           const std::vector<int>& slots, const std::vector<double>& h,
                           const std::vector<double>& eta, std::vector<double>& qx,
                           std::vector<double>& qy, double dry, double seconds, bool vfr);

    static void rotate(std::vector<double>& qx, std::vector<double>& qy, double angle);

    /** Evaluate conservative shared-face dq/dt. Cell viscosity is
     * background + (cs*Delta)^2*|S| (m2/s), or background + l^2*|omega| with
     * l = min(c*h, kappa*d_wall) when a depth_coefficient c > 0 selects the depth
     * mixing length, unless fixed_viscosity >= 0 replaces it. The explicit bound uses the
     * absolute row sums of the actual velocity operator, including gradient
     * reconstruction and cross stresses. Dry or blocked faces are excluded.
     */
    double evaluate(const MeshData& m, const InertialEdges& edges, const std::vector<double>& h,
                    const std::vector<double>& eta, const std::vector<double>& qx,
                    const std::vector<double>& qy, double dry, double cs, std::vector<double>& rx,
                    std::vector<double>& ry, double fixed_viscosity = -1, double background = 0,
                    double depth_coefficient = 0);

    /** SSP-RK2 mixing at fixed water storage. Reevaluate nonlinear viscosity
     * at each stage; retry only momentum, before publishing it. Hydrodynamic
     * sources/receipts are never replayed. Subcycling obeys both stage bounds
     * and checks the discrete energy inequality. No viscosity clipping.
     */
    void advance(const MeshData& m, const InertialEdges& edges, const std::vector<double>& h,
                 const std::vector<double>& eta, std::vector<double>& qx, std::vector<double>& qy,
                 double dry, double cs, double seconds, double background = 0,
                 double depth_coefficient = 0);

    /** Nearest-boundary-edge distance for every cell centroid (all exterior edges). */
    void computeWallDistance(const MeshData& m);

    static double energy(const MeshData& m, const std::vector<double>& h,
                         const std::vector<double>& qx, const std::vector<double>& qy, double dry);

private:
    struct Stencil {
        std::array<int, 5> ids{};
        std::array<double, 5> gx{}, gy{};
        int count = 0;
    };
    std::vector<Stencil> stencils_;
    std::vector<uint8_t> connected_;
    std::vector<double> bound_x_, bound_y_, rx_, ry_, sx_, sy_, tx_, ty_;
};
} // namespace openswmm::twoD
#endif
