/**
 * @file Culvert.hpp
 * @brief Culvert inlet control — FHWA HEC-5 equations.
 *
 * @details Three flow regimes:
 *   - Unsubmerged (y <= 0.95*yFull): Q = AD * (h/yFull/K)^(1/M)
 *   - Transition:  linear interpolation
 *   - Submerged (y >= y2):  Q = AD * sqrt((h/yFull - Y + scf) / C)
 *
 *   Batch: classify all culverts by regime, compute flow per regime group.
 *
 * @note Legacy reference: src/legacy/engine/culvert.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_CULVERT_HPP
#define OPENSWMM_CULVERT_HPP

#include <vector>

namespace openswmm {

struct SimulationContext;

namespace culvert {

/// Culvert curve coefficients (K, M, C, Y) per type code.
struct CulvertCoeffs {
    double K = 0.0;
    double M = 0.0;
    double C = 0.0;
    double Y = 0.0;
};

/// Get coefficients for a culvert type code (1-57).
CulvertCoeffs getCoeffs(int culvert_code);

/**
 * @brief Compute culvert inlet-controlled inflow.
 *
 * @param q_proposed  Proposed flow from routing (cfs).
 * @param head        Upstream head above invert (ft).
 * @param y_full      Full depth of culvert (ft).
 * @param a_full      Full area (ft2).
 * @param slope       Conduit slope.
 * @param code        Culvert type code.
 * @param[out] dqdh   Derivative dQ/dH.
 * @returns Inlet-controlled flow (cfs). If < q_proposed, inlet controls.
 */
double getInflow(double q_proposed, double head, double y_full,
                 double a_full, double slope, int code, double& dqdh);

/**
 * @brief Batch compute culvert inlet control for all culvert links.
 *
 * @param link_indices  Indices of culvert links.
 * @param n             Number of culvert links.
 * @param ctx           Simulation context.
 */
void batchComputeInletControl(const int* link_indices, int n,
                               SimulationContext& ctx);

} // namespace culvert
} // namespace openswmm

#endif // OPENSWMM_CULVERT_HPP
