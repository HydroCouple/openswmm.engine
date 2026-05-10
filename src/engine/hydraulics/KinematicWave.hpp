/**
 * @file KinematicWave.hpp
 * @brief Kinematic wave routing solver — batch-oriented design.
 *
 * @details Port of legacy kinwave.c, restructured for data-oriented execution:
 *
 *          1. **Batch geometry** — uses XSectGroups to pre-compute section
 *             factors for all conduits before solving
 *          2. **Per-conduit Newton** — the continuity equation solve is
 *             inherently per-conduit (each converges independently), but
 *             conduits are grouped by shape so the section-factor evaluations
 *             within Newton can also be batched per group
 *          3. **Batch state update** — inlet/outlet areas and flows updated
 *             for all conduits in contiguous sweeps
 *
 *          Weighted finite-difference scheme:
 *            WX = 0.6 (space weighting)
 *            WT = 0.6 (time weighting)
 *
 * @note Legacy reference: src/legacy/engine/kinwave.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_KINEMATIC_WAVE_HPP
#define OPENSWMM_KINEMATIC_WAVE_HPP

#include "XSectBatch.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace kinwave {

// ============================================================================
// Constants (matching legacy)
// ============================================================================

constexpr double WX    = 0.6;     ///< Distance weighting factor
constexpr double WT    = 0.6;     ///< Time weighting factor
constexpr double EPSIL = 0.001;   ///< Newton convergence tolerance

// ============================================================================
// KW solver — operates on entire conduit set
// ============================================================================

/**
 * @brief Kinematic wave solver state.
 *
 * @details Holds SoA working arrays for all conduits. Allocated once at init.
 *          The `execute()` method routes all conduits for one timestep.
 */
class KWSolver {
public:
    /// Initialise for n conduit-type links. Call once after model is built.
    /// Builds topological link order for upstream → downstream processing.
    void init(int n_conduits, const XSectGroups& groups);

    /// Set topological link order (must be called after init, before first execute).
    void setLinkOrder(const std::vector<int>& sorted_links) {
        sorted_links_ = sorted_links;
    }

    /**
     * @brief Route all conduits for one KW timestep.
     *
     * @details
     *   1. Batch-compute inlet section factors from inflows using XSectGroups
     *   2. For each conduit, solve the Newton continuity equation
     *   3. Batch-compute outflows from outlet section factors
     *   4. Update state arrays
     *
     * @param ctx  Simulation context (links/nodes modified in place).
     * @param dt   Timestep (seconds).
     * @returns Average number of Newton iterations across all conduits.
     */
    int execute(SimulationContext& ctx, double dt);

    /// Topological link order (upstream → downstream)
    std::vector<int> sorted_links_;

    /// Per-conduit Newton solve. Returns iteration count.
    int solveConduit(int idx, const XSectParams& xs,
                     double q_full, double a_full, double s_full,
                     double beta, double length, double dt,
                     double loss_rate);

    // Per-conduit SoA state (indexed by conduit-link index)
    // Public so that unit tests can set up / inspect working arrays directly.
    std::vector<double> q1_;    ///< Previous inlet flow (cfs)
    std::vector<double> a1_;    ///< Previous inlet area (ft2)
    std::vector<double> q2_;    ///< Previous outlet flow (cfs)
    std::vector<double> a2_;    ///< Previous outlet area (ft2)

    // Working buffers (reused each timestep)
    std::vector<double> q_in_;      ///< Inflow to each conduit (cfs)
    std::vector<double> a_in_;      ///< Inlet area from inflow (ft2)
    std::vector<double> q_out_;     ///< Computed outflow (cfs)
    std::vector<double> a_out_;     ///< Outlet area from Newton solve (ft2)
    std::vector<double> sf_in_;     ///< Section factor at inlet

private:
    int n_conduits_ = 0;
};

} // namespace kinwave
} // namespace openswmm

#endif // OPENSWMM_KINEMATIC_WAVE_HPP
