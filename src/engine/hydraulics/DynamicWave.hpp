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
#include "../core/Constants.hpp"
#include "../core/SimulationOptions.hpp"
#include "../data/NodeData.hpp"
#include "../data/LinkData.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace dynwave {

// ============================================================================
// Constants — imported from global Constants.hpp
// ============================================================================

using constants::OMEGA;
using constants::DEFAULT_HEAD_TOL;
using constants::DEFAULT_MAX_TRIALS;
using constants::MAX_VELOCITY;
using constants::MIN_TIMESTEP;
using constants::EXTRAN_CROWN_CUTOFF;
using constants::SLOT_CROWN_CUTOFF;
using constants::SLOT_WIDTH_FACTOR;
using constants::FUDGE;
using constants::MIN_SURFAREA;

// ============================================================================
// Dynamic Preissmann Slot (DPS) configuration and per-link state
// Sharior, Hodges & Vasconcelos (2023), J. Hydraul. Eng. 149(11)
// ============================================================================

/// DPS configuration parameters (derived from SimulationOptions at init).
struct DPSConfig {
    double c_pT   = 25.0;    ///< Target pressure celerity (ft/s, internal units)
    double alpha   = 3.0;    ///< Surcharge shock parameter (>= 2)
    double r       = 0.5;    ///< Decay time scale for P → 1 (seconds)
    double c_pT_sq = 625.0;  ///< c_pT^2 (pre-computed)
};

/// Per-conduit DPS state (persistent across timesteps).
struct DPSLinkState {
    double As       = 0.0;    ///< Accumulated slot area (ft²)
    double hs       = 0.0;    ///< Current surcharge head (ft)
    double P        = 1.0;    ///< Current Preissmann Number (smoothed)
    double P_hat    = 1.0;    ///< Provisional Preissmann Number (before smoothing)
    double P_hat_0  = 1.0;    ///< Initial P for unpressurized conduit
    double t_s      = 0.0;    ///< Time when element last became surcharged (sec)
    bool   surcharged = false; ///< Currently surcharged flag
};

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
 * @brief Momentum category for branch-free per-category kernel dispatch.
 *
 * @details Each conduit is classified once per Picard iteration (after geometry
 *          is computed). Per-category kernels have zero shape/type/state branches
 *          in their inner loops, enabling compiler auto-vectorization.
 */
enum class MomentumCategory : uint8_t {
    SKIP_DRY          = 0,  ///< DRY/UP_DRY/DN_DRY, aMid<=FUDGE, or is_closed
    MANNING_OPEN      = 1,  ///< Standard Manning, open channel (Froude-based sigma)
    MANNING_CLOSED_FS = 2,  ///< Manning, closed conduit, free surface
    MANNING_CLOSED_FULL = 3,///< Manning, closed conduit, surcharged (fr=0, sig=0)
    FORCE_MAIN_HW     = 4,  ///< Force main, Hazen-Williams friction
    FORCE_MAIN_DW     = 5,  ///< Force main, Darcy-Weisbach friction
    N_CATEGORIES      = 6
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
    void init(int n_nodes, int n_links, const XSectGroups& groups,
              const SimulationContext& ctx);

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

    /// Callback for computing non-conduit link flows inside the Picard loop.
    /// Parameters: (ctx, dt, picard_step) where step=0 is first iteration.
    using NonConduitFlowFunc = std::function<void(SimulationContext&, double, int)>;

    /**
     * @brief Execute one DW routing timestep.
     *
     * @param ctx  Simulation context.
     * @param dt   Timestep (seconds).
     * @param non_conduit_fn  Optional callback to compute pump/orifice/weir/outlet
     *                        flows inside the Picard iteration loop (matching legacy).
     * @returns Number of Picard iterations used.
     */
    int execute(SimulationContext& ctx, double dt,
                NonConduitFlowFunc non_conduit_fn = nullptr);

    /**
     * @brief Compute CFL-based variable timestep.
     * @details Also updates per-node/link CFL-critical counters (matching
     *          legacy stats_updateCriticalTimeCount).
     */
    double getRoutingStep(SimulationContext& ctx,
                          double fixed_step, double courant_factor);

    double head_tol   = DEFAULT_HEAD_TOL;
    int    max_trials = DEFAULT_MAX_TRIALS;
    double omega      = OMEGA;
    SurchargeMethod surcharge_method = SurchargeMethod::EXTRAN;
    NodeContinuity  node_continuity  = NodeContinuity::EXPLICIT;
    bool   anderson_accel = false;       ///< Enable Anderson acceleration

private:
    int n_nodes_ = 0;
    int n_links_ = 0;
    int n_conduits_ = 0;
    int num_threads_ = 1;  ///< OpenMP thread count for parallel loops
    const XSectGroups* groups_ = nullptr;

    // Pre-built conduit index list for skipping non-conduits in inner loops
    std::vector<int> conduit_idx_;

    // Pre-computed per-link invariants (populated once at first execute, reused)
    // Using uint8_t instead of bool to allow .data() pointer access for SIMD/restrict
    std::vector<uint8_t> is_open_;        ///< Shape is open (RECT_OPEN/TRAP/TRI/PARA)
    std::vector<uint8_t> is_force_main_;  ///< Shape is FORCE_MAIN
    std::vector<uint8_t> has_losses_;     ///< Any minor loss coeff != 0
    std::vector<double> barrels_d_;       ///< double(max(barrels, 1))
    std::vector<double> cached_length_;   ///< max(mod_length, length)
    std::vector<double> inv_length_;      ///< 1.0 / cached_length_

    // Per-timestep constant
    double dt_gravity_ = 0.0;            ///< dt * GRAVITY (set once per timestep)

    // Pre-allocated width-capping buffers (avoids thread_local per-call allocation)
    std::vector<double> wcap_d1_, wcap_d2_, wcap_dm_;

    // Variable timestep state (matching legacy VariableStep in dynwave.c)
    mutable double variable_step_ = 0.0;

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

    // Per-link bypass flag (true when both end nodes converged; skip momentum solve)
    std::vector<bool> bypassed_;

    // Per-link surface area contributions to upstream/downstream nodes
    // (matching legacy Link[].surfArea1/surfArea2 from dwflow.c findSurfArea)
    std::vector<double> surf_area1_;   ///< Surface area at upstream node (ft²)
    std::vector<double> surf_area2_;   ///< Surface area at downstream node (ft²)

    // Per-link upstream geometry (for proper weighted hyd. radius)
    std::vector<double> hrad1_;        ///< Hydraulic radius at upstream depth
    std::vector<double> width1_;       ///< Top width at upstream depth
    std::vector<double> width2_;       ///< Top width at downstream depth

    // Per-link head values (persisted from computeLinkGeometry for solveMomentumBatch,
    // may be modified by flow classification for UP_CRITICAL/DN_CRITICAL cases)
    std::vector<double> h1_;           ///< Head at upstream node (possibly modified)
    std::vector<double> h2_;           ///< Head at downstream node (possibly modified)
    std::vector<double> fasnh_;        ///< Fraction between normal & critical depth

    // Anderson acceleration state (per-node, depth-2 mixing)
    std::vector<double> aa_y_prev_;     ///< Node depths at iteration k-1
    std::vector<double> aa_g_prev_;     ///< G(y_{k-1}) — computed depths at k-1
    std::vector<double> aa_r_prev_;     ///< Residual r_{k-1} = G(y_{k-1}) - y_{k-1}
    std::vector<uint8_t> aa_skip_;      ///< Per-node flag: skip AA this iteration

    // Per-conduit momentum category (rebuilt each Picard iteration)
    std::vector<MomentumCategory> category_;
    // Per-category conduit index lists (rebuilt each iteration)
    std::vector<int> cat_indices_[static_cast<int>(MomentumCategory::N_CATEGORIES)];

    // Internal methods
    void initNodeStates(SimulationContext& ctx);
    void findBypassedLinks(const SimulationContext& ctx);
    void computeLinkGeometry(SimulationContext& ctx);
    void solveMomentumBatch(SimulationContext& ctx, double dt, int step);
    void classifyMomentumCategories(SimulationContext& ctx);
    void processDryLinks(SimulationContext& ctx, double dt);
    void processManningLinks(SimulationContext& ctx, double dt, int step,
                             MomentumCategory cat);
    void processForceMainLinks(SimulationContext& ctx, double dt, int step,
                               MomentumCategory cat);
    void applyFlowLimits(SimulationContext& ctx, double dt, int step,
                         std::size_t uj, double& q, double qLast,
                         double barrels_d, bool isFull);
    void updateNodeFlows(SimulationContext& ctx, bool conduits_only = false);
    void computeAASkipFlags(const SimulationContext& ctx);
    bool updateNodeDepths(SimulationContext& ctx, double dt, int step);
    void setNodeDepth(SimulationContext& ctx, int node_idx, double dt, int step);
    double getLinkStep(const SimulationContext& ctx, int link_idx) const;

public:
    /// Access per-node working state (for non-conduit surfarea/dqdh scatter).
    DWNodeState& nodeState(int idx) { return xnode_[static_cast<std::size_t>(idx)]; }

    /// Access per-node AA skip flags (read-only, for testing/diagnostics).
    const std::vector<uint8_t>& aaSkipFlags() const { return aa_skip_; }
private:

    // Preissmann slot helpers (matching legacy dwflow.c)
    double getSlotWidth(double y, double y_full, double w_max, XsectShape shape) const;
    double getSlotArea(double y, double y_full, double a_full, double slot_width) const;
    double getSlotHydRad(double y, double y_full, double r_full) const;
    double getCrownCutoff() const;

    // Dynamic Preissmann Slot (DPS) state and methods
    DPSConfig dps_config_;
    std::vector<DPSLinkState> dps_state_;  ///< Per-conduit DPS state [n_conduits_]
    double sim_time_ = 0.0;               ///< Accumulated simulation time (seconds)

    /// Apply DPS geometry overrides for surcharged conduits (replaces static slot in STEP E).
    void applyDPSGeometry(SimulationContext& ctx);

    /// Update DPS temporal state after Picard convergence (P decay, t_s tracking).
    void updateDPSState(SimulationContext& ctx, double dt);

    /// Spatial smoothing of Preissmann Number across node boundaries.
    void spatialSmoothP(const SimulationContext& ctx);
};

} // namespace dynwave
} // namespace openswmm

#endif // OPENSWMM_DYNAMIC_WAVE_HPP
