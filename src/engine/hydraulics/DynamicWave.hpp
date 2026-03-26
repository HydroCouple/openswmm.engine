/**
 * @file DynamicWave.hpp
 * @brief Dynamic wave routing solver — batch-oriented St. Venant equations.
 *
 * @details Port of legacy dwflow.c + dynwave.c, restructured for batch execution:
 *
 *          **Per Picard iteration:**
 *          1. **Batch geometry** — XSectGroups pre-computes a1[], a2[], aMid[],
 *             rMid[], wMid[] for ALL conduits simultaneously (shape-grouped,
 *             SIMD-vectorisable)
 *          2. **Batch momentum** — friction slope, head gradient, inertial terms
 *             computed as array operations over pre-computed geometry (no xsect
 *             dispatch in the inner loop)
 *          3. **Scatter** — link flows transferred to node inflow/outflow
 *          4. **Node depth update** — per-node (Picard convergence check)
 *
 *          Uses theta-implicit scheme with Preissmann slot for surcharge,
 *          inertial damping based on Froude number, and under-relaxation.
 *
 * @note Legacy reference: src/legacy/engine/dwflow.c, dynwave.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_DYNAMIC_WAVE_HPP
#define OPENSWMM_DYNAMIC_WAVE_HPP

#include "XSectBatch.hpp"
#include "../data/NodeData.hpp"
#include "../data/LinkData.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace dynwave {

// ============================================================================
// Constants (matching legacy)
// ============================================================================

constexpr double OMEGA               = 0.5;       ///< Picard under-relaxation
constexpr double DEFAULT_HEADTOL     = 0.005;     ///< Convergence tolerance (ft)
constexpr int    DEFAULT_MAXTRIALS   = 8;          ///< Max Picard iterations
constexpr double MAXVELOCITY         = 50.0;      ///< Velocity limiter (ft/s)
constexpr double MINTIMESTEP         = 0.001;      ///< Minimum timestep (s)
constexpr double EXTRAN_CROWN_CUTOFF = 0.96;       ///< EXTRAN surcharge fraction
constexpr double SLOT_CROWN_CUTOFF   = 0.985257;   ///< Preissmann slot crown cutoff
constexpr double SLOT_WIDTH_FACTOR   = 0.001;       ///< Slot width = y_full * this factor

// ============================================================================
// Per-node extended state for DW iterations
// ============================================================================

struct DWNodeState {
    double new_surf_area = 0.0;
    double old_surf_area = 0.0;   ///< Surface area from last non-surcharged state
    double sumdqdh       = 0.0;
    double dYdT          = 0.0;
    bool   converged     = false;
    bool   is_surcharged = false;  ///< TRUE when node depth > crown elevation
};

// ============================================================================
// DW solver — batch-oriented
// ============================================================================

/**
 * @brief Surcharge method: EXTRAN (classic) or SLOT (Preissmann).
 * @see Legacy: SurchargeMethod in enums.h
 */
enum class SurchargeMethod : int {
    EXTRAN = 0,  ///< Classic EXTRAN approach — dQ/dH for surcharged nodes
    SLOT   = 1,  ///< Preissmann slot — fictitious narrow slot above crown
    DYNAMIC_SLOT = 2   ///< Dynamic slot — slot width varies with flow conditions (experimental) Sharior, S., Hodges, B.R., & Vasconcelos, J.G. (2023). Generalized, Dynamic, and Transient-Storage Form of the Preissmann Slot. Journal of Hydraulic Engineering, 149(11), 04023046.
};

/**
 * @brief Dynamic wave solver — operates on entire link/node system.
 *
 * @details Working arrays are allocated once at init and reused each timestep.
 *          The solver pre-computes all cross-section geometry via XSectGroups
 *          batch API before entering the momentum arithmetic loop, ensuring
 *          the inner loop is branch-free and vectorisable.
 */
class DWSolver {
public:
    void init(int n_nodes, int n_links, const XSectGroups& groups);

    /**
     * @brief Set the number of OpenMP threads for parallel loops.
     *
     * @details Called after init() when the thread count is finalized.
     *          A threshold is applied: if n_links < 4 * n, threading is
     *          disabled (matching legacy dynwave.c behaviour).
     *
     * @param n  Requested thread count (0 = use omp_get_max_threads()).
     */
    void setNumThreads(int n);

    /**
     * @brief Execute one DW routing timestep.
     *
     * @param ctx  Simulation context.
     * @param dt   Timestep (seconds).
     * @returns Number of Picard iterations used.
     */
    int execute(SimulationContext& ctx, double dt);

    /**
     * @brief Compute CFL-based variable timestep.
     */
    double getRoutingStep(const SimulationContext& ctx,
                          double fixed_step, double courant_factor) const;

    double head_tol   = DEFAULT_HEADTOL;
    int    max_trials = DEFAULT_MAXTRIALS;
    double omega      = OMEGA;
    SurchargeMethod surcharge_method = SurchargeMethod::EXTRAN;

private:
    int n_nodes_ = 0;
    int n_links_ = 0;
    int num_threads_ = 1;  ///< OpenMP thread count for parallel loops
    const XSectGroups* groups_ = nullptr;

    // Per-node working state
    std::vector<DWNodeState> xnode_;

    // Per-link pre-computed geometry (batch-filled by XSectGroups each iteration)
    std::vector<double> area1_;      ///< Area at upstream depth
    std::vector<double> area2_;      ///< Area at downstream depth
    std::vector<double> area_mid_;   ///< Area at average depth
    std::vector<double> hrad_mid_;   ///< Hydraulic radius at average depth
    std::vector<double> width_mid_;  ///< Top width at average depth
    std::vector<double> depth1_;     ///< Upstream flow depth
    std::vector<double> depth2_;     ///< Downstream flow depth
    std::vector<double> depth_mid_;  ///< Average depth

    // Per-link momentum working arrays
    std::vector<double> velocity_;   ///< Current velocity
    std::vector<double> froude_;     ///< Current Froude number
    std::vector<double> sigma_;      ///< Inertial damping factor
    std::vector<double> dqdh_;       ///< dQ/dH for node update
    std::vector<double> new_flow_;   ///< Computed flow this iteration

    // Per-link area from previous iteration (for unsteady term)
    std::vector<double> area_old_;

    // Internal methods
    void initNodeStates(SimulationContext& ctx);
    void computeLinkGeometry(SimulationContext& ctx);
    void solveMomentumBatch(SimulationContext& ctx, double dt, int step);
    void updateNodeFlows(SimulationContext& ctx);
    bool updateNodeDepths(SimulationContext& ctx, double dt, int step);
    void setNodeDepth(SimulationContext& ctx, int node_idx, double dt, int step);
    double getLinkStep(const SimulationContext& ctx, int link_idx) const;

    // Preissmann slot helpers (matching legacy dwflow.c)
    double getSlotWidth(double y, double y_full, double w_max, XsectShape shape) const;
    double getSlotArea(double y, double y_full, double a_full, double slot_width) const;
    double getSlotHydRad(double y, double y_full, double r_full) const;
    double getCrownCutoff() const;
};

} // namespace dynwave
} // namespace openswmm

#endif // OPENSWMM_DYNAMIC_WAVE_HPP
