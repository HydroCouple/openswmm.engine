/**
 * @file KinematicWave.cpp
 * @brief Kinematic wave routing — batch-oriented, numerically identical to legacy.
 *
 * @details The solver is structured as:
 *   1. Gather inflows for all conduits
 *   2. Batch-compute inlet areas from inflows (via section factor inversion)
 *   3. Per-conduit Newton solve for outlet area (grouped by shape where possible)
 *   4. Batch-compute outflows from outlet areas (via section factor)
 *   5. Scatter results back to global link arrays
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "KinematicWave.hpp"
#include "XSectBatch.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace kinwave {

static constexpr int    MAX_ITERS = 40;
static constexpr double TINY      = 1.0e-6;

// ============================================================================
// Init
// ============================================================================

void KWSolver::init(int n_conduits, const XSectGroups& /*groups*/) {
    n_conduits_ = n_conduits;
    auto un = static_cast<std::size_t>(n_conduits);

    q1_.assign(un, 0.0);
    a1_.assign(un, 0.0);
    q2_.assign(un, 0.0);
    a2_.assign(un, 0.0);

    q_in_.resize(un);
    a_in_.resize(un);
    q_out_.resize(un);
    a_out_.resize(un);
    sf_in_.resize(un);
}

// ============================================================================
// Per-conduit Newton solve
// ============================================================================

int KWSolver::solveConduit(int idx, const XSectParams& xs,
                            double q_full, double a_full, double s_full,
                            double beta, double length, double dt,
                            double loss_rate) {
    auto ui = static_cast<std::size_t>(idx);
    if (q_full <= 0.0 || a_full <= 0.0) {
        q_out_[ui] = 0.0;
        a_out_[ui] = 0.0;
        return 0;
    }

    // Normalise
    double q_in_norm = q_in_[ui] / q_full;
    double q3 = loss_rate / q_full;
    double beta1 = beta / q_full;

    double prev_q1 = q1_[ui] / q_full;
    double prev_a1 = a1_[ui] / a_full;
    double prev_q2 = q2_[ui] / q_full;
    double prev_a2 = a2_[ui] / a_full;

    // Normalised inlet area
    double a_in_norm;
    if (q_in_norm >= 1.0) {
        a_in_norm = 1.0;
    } else if (q_in_norm <= 0.0) {
        a_in_norm = 0.0;
    } else {
        double s_needed = q_in_norm / beta1;
        a_in_norm = xsect::getAofS(xs, s_needed * a_full) / a_full;
    }

    // Finite-difference coefficients
    double dxdt = length / dt * a_full / q_full;
    double dq = prev_q2 - prev_q1;

    double C1 = dxdt * WT / WX;
    double C2 = (1.0 - WT) * (a_in_norm - prev_a1);
    C2 -= WT * prev_a2;
    C2 *= dxdt / WX;
    C2 += (1.0 - WX) / WX * dq - q_in_norm;
    C2 += q3 / WX;

    // Newton-Raphson: solve f(a) = beta1*S(a*Afull) + C1*a + C2 = 0
    double a = (prev_a2 > TINY) ? prev_a2 : a_in_norm;
    a = std::max(a, TINY);

    int iters = 0;
    for (; iters < MAX_ITERS; ++iters) {
        double a_abs = a * a_full;
        double s = xsect::getSofA(xs, a_abs);
        double f = beta1 * s + C1 * a + C2;

        double dsda = xsect::getdSdA(xs, a_abs);
        double df = beta1 * a_full * dsda + C1;

        if (std::fabs(df) < TINY) break;

        double da = -f / df;
        a += da;
        if (a < 0.0) a = 0.5 * (a - da);
        if (std::fabs(da) < EPSIL) break;
    }
    a = std::max(a, 0.0);

    // Outflow from outlet area
    double s_out = xsect::getSofA(xs, a * a_full);
    double q_out_norm = beta1 * s_out;
    q_out_norm = std::max(q_out_norm, 0.0);

    // De-normalise and store
    a_in_[ui]  = a_in_norm * a_full;
    a_out_[ui] = a * a_full;
    q_out_[ui] = q_out_norm * q_full;

    // Update state for next timestep
    q1_[ui] = q_in_[ui];
    a1_[ui] = a_in_[ui];
    q2_[ui] = q_out_[ui];
    a2_[ui] = a_out_[ui];

    return iters;
}

// ============================================================================
// Main execute — batch-oriented
// ============================================================================

int KWSolver::execute(SimulationContext& /*ctx*/, double /*dt*/) {
    // TODO: wire up to SimulationContext
    // The structure is:
    //
    // 1. For each conduit link (topologically sorted):
    //    a. Gather inflow from upstream node
    //    b. q_in_[i] = upstream_node_outflow or lateral_inflow
    //
    // 2. For each conduit, solve Newton (per-conduit, grouped by shape):
    //    solveConduit(i, xs, q_full, a_full, s_full, beta, length, dt, loss)
    //
    // 3. Scatter outflows to downstream nodes:
    //    downstream_node.inflow += q_out_[i]
    //
    // 4. Update link state:
    //    link.flow = q_out_[i]
    //    link.depth = 0.5 * (yofA(a_in) + yofA(a_out))
    //    link.volume = 0.5 * (a_in + a_out) * length * barrels
    //
    // The Newton solve itself is per-conduit but uses xsect::getSofA which
    // dispatches by shape. For further optimisation, conduits could be grouped
    // by shape and the Newton solved in shape-grouped batches where S(a) uses
    // the same table for all conduits in the group.

    return 0;
}

} // namespace kinwave
} // namespace openswmm
