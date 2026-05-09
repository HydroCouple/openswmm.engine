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

/// Per-conduit DPS state — Structure of Arrays (persistent across timesteps).
/// Each vector is indexed by conduit index [0..n_conduits_).
struct DPSLinkArrays {
    std::vector<double>  As;          ///< Accumulated slot area (ft²)
    std::vector<double>  hs;          ///< Current surcharge head (ft)
    std::vector<double>  P;           ///< Current Preissmann Number (smoothed)
    std::vector<double>  P_hat;       ///< Provisional Preissmann Number (before smoothing)
    std::vector<double>  P_hat_0;     ///< Initial P for unpressurized conduit
    std::vector<double>  t_s;         ///< Time when element last became surcharged (sec)
    std::vector<uint8_t> surcharged;  ///< Currently surcharged flag

    void resize(std::size_t n) {
        As.assign(n, 0.0);
        hs.assign(n, 0.0);
        P.assign(n, 1.0);
        P_hat.assign(n, 1.0);
        P_hat_0.assign(n, 1.0);
        t_s.assign(n, 0.0);
        surcharged.assign(n, 0);
    }
};

// ============================================================================
// Per-node extended state for DW iterations
// ============================================================================

/// Per-node extended state for DW iterations — Structure of Arrays.
/// Each vector is indexed by node index [0..n_nodes_).
struct DWNodeArrays {
    std::vector<double>  new_surf_area;
    std::vector<double>  old_surf_area;   ///< Surface area from last non-surcharged state
    std::vector<double>  sumdqdh;
    std::vector<double>  dYdT;
    std::vector<uint8_t> converged;
    std::vector<uint8_t> is_surcharged;   ///< TRUE when node depth > crown elevation

    void resize(std::size_t n) {
        new_surf_area.assign(n, 0.0);
        old_surf_area.assign(n, 0.0);
        sumdqdh.assign(n, 0.0);
        dYdT.assign(n, 0.0);
        converged.assign(n, 0);
        is_surcharged.assign(n, 0);
    }
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

    /// Evaporation rate (ft/s) — set by Router::step() each timestep so that
    /// solveMomentumBatch can recompute dq6 per Picard iteration (Gap #14).
    double evap_rate  = 0.0;

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

    // ------------------------------------------------------------------------
    // Phase A — Conduit-dense "hot tile" of timestep-invariant data.
    //
    // Sized n_conduits_, accessed by ci (0..n_conduits_-1) for dense linear
    // memory access pattern. This replaces sparse `links.X[uj]` /
    // `nodes.X[un]` reads inside the Picard inner loops, where uj/un are
    // sparse-indexed via conduit_idx_ + links.node1/node2.  Each ci-indexed
    // read maps to one contiguous cache line that holds 8+ conduits' data,
    // versus the sparse pattern where each uj read can miss into a new line.
    //
    // All fields below are populated once in init() (or refreshConduitTile
    // when a hot-start changes invariants) and remain constant for the rest
    // of the simulation.  None of these change per Picard iter, per
    // timestep, or per outfall update.
    // ------------------------------------------------------------------------
    std::vector<int>    tile_uj_;            ///< == conduit_idx_, co-located with rest
    std::vector<int>    tile_n1_;            ///< links.node1[uj] for ci
    std::vector<int>    tile_n2_;            ///< links.node2[uj]
    std::vector<double> tile_inv1_elev_;     ///< nodes.invert_elev[node1]
    std::vector<double> tile_inv2_elev_;     ///< nodes.invert_elev[node2]
    std::vector<double> tile_z1_off_;        ///< links.offset1
    std::vector<double> tile_z2_off_;        ///< links.offset2
    std::vector<double> tile_y_full_;        ///< links.xsect_y_full
    std::vector<double> tile_a_full_;        ///< links.xsect_a_full
    std::vector<double> tile_r_full_;        ///< links.xsect_r_full
    std::vector<double> tile_w_max_;         ///< links.xsect_w_max
    std::vector<double> tile_length_;        ///< cached_length_ = max(mod_length, length); used for arithmetic stability
    std::vector<double> tile_inv_length_;    ///< inv_length_
    std::vector<double> tile_links_length_;  ///< raw links.length[uj] — used for volume calculations only
    std::vector<double> tile_beta_;          ///< links.beta
    std::vector<double> tile_q_max_;         ///< links.q_max
    std::vector<double> tile_rough_factor_;  ///< links.rough_factor
    std::vector<double> tile_barrels_d_;     ///< barrels_d_
    std::vector<uint8_t> tile_is_open_;      ///< is_open_
    std::vector<uint8_t> tile_is_force_main_;///< is_force_main_
    std::vector<uint8_t> tile_is_closed_;    ///< links.is_closed
    std::vector<uint8_t> tile_has_losses_;   ///< has_losses_
    std::vector<int>     tile_xsect_batch_shape_; ///< links.xsect_batch_shape
    std::vector<XsectShape> tile_shape_;     ///< links.xsect_shape
    /// Phase C: pre-flag conduits that can possibly enter the Newton path
    /// in STEP C. True iff (offset1 > 0 || offset2 > 0). When false the
    /// flow-classification cascade reduces to two branchless comparisons
    /// (both_full ? SUBCRITICAL : (both_dry ? DRY : SUBCRITICAL/UP_DRY/DN_DRY)),
    /// no Newton solver call. Roughly 80–90 % of conduits in typical CSO
    /// networks have zero offsets and take this fast path.
    std::vector<uint8_t> tile_has_offset_;   ///< (z1_off > 0 || z2_off > 0)
    /// Reverse map uj → ci. Sized n_links_, -1 for non-conduits. Lets the
    /// momentum-kernel functions (processManningLink, processForceMainLink,
    /// applyFlowLimits) read tile_X[ci] directly without changing their
    /// uj-based signatures.
    std::vector<int>     tile_uj_to_ci_;
    /// Per-conduit timestep-invariants used by processManningLink /
    /// processForceMainLink / applyFlowLimits (added in Phase A extension).
    std::vector<int>     tile_culvert_code_;
    std::vector<double>  tile_slope_;
    std::vector<double>  tile_q_limit_;
    std::vector<double>  tile_loss_inlet_;
    std::vector<double>  tile_loss_outlet_;
    std::vector<uint8_t> tile_has_flap_gate_;
    std::vector<int8_t>  tile_direction_;

    /// Populate the tile arrays from links/nodes SoA. Called once after
    /// conduit_idx_ is built in init(), and exposed publicly for hotstart
    /// refresh (currently unused but cheap to call again).
    void refreshConduitTile(const SimulationContext& ctx);

    // Per-timestep constants
    double dt_gravity_ = 0.0;            ///< dt * GRAVITY (set once per timestep)

    /// Effective minimum nodal surface area (ft²) used as a floor for the
    /// dy = dV/surf_area Picard update.  Legacy `MinSurfArea` is the user
    /// override from the INP `[OPTIONS]` `MIN_SURFAREA` line, falling back
    /// to `DEFAULT_SURFAREA = 12.566 ft²` (4·π) only when the user did not
    /// set it.  The new engine had been hard-coding `constants::MIN_SURFAREA`
    /// regardless — under-counting the floor by up to 4× on models that
    /// override it (Rich_BC_CSO sets it to 50 ft²), causing aggressive
    /// dy oscillation and the 99.74 % vs 73.27 % non-convergence gap.
    /// Set in `init()` from `ctx.options.min_surf_area`.
    double min_surf_area_ = constants::MIN_SURFAREA;

    // Pre-allocated width-capping buffers (avoids thread_local per-call allocation)
    std::vector<double> wcap_d1_, wcap_d2_, wcap_dm_;

    // Variable timestep state (matching legacy VariableStep in dynwave.c)
    mutable double variable_step_ = 0.0;

    // Per-node working state (SoA)
    DWNodeArrays xnode_;

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
    // uint8_t instead of bool: avoids std::vector<bool> bit-packing overhead
    std::vector<uint8_t> bypassed_;

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

    // Per-conduit momentum category (rebuilt each Picard iteration).
    // solveMomentumBatch dispatches on category_[uj] inline — no auxiliary
    // per-category index list is needed.
    std::vector<MomentumCategory> category_;

    // Internal methods
    void initNodeStates(SimulationContext& ctx);
    void findBypassedLinks(const SimulationContext& ctx);
    void computeLinkGeometry(SimulationContext& ctx);
    void solveMomentumBatch(SimulationContext& ctx, double dt, int step);
    void classifyMomentumCategories(SimulationContext& ctx);

    /// Per-element momentum kernels. Called inside the single OpenMP
    /// parallel-for over all conduits in solveMomentumBatch, matching
    /// legacy dynwave.c::findLinkFlows (one fork per Picard iteration,
    /// per-element category dispatch inside).
    void processDryLink(SimulationContext& ctx, double dt, std::size_t uj);
    void processManningLink(SimulationContext& ctx, double dt, int step,
                            std::size_t uj, MomentumCategory cat);
    void processForceMainLink(SimulationContext& ctx, double dt, int step,
                              std::size_t uj, MomentumCategory cat);
    void applyFlowLimits(SimulationContext& ctx, double dt, int step,
                         std::size_t uj, double& q, double qLast,
                         double barrels_d, bool isFull);
    void updateNodeFlows(SimulationContext& ctx, bool conduits_only = false);
    void computeAASkipFlags(const SimulationContext& ctx);
    bool updateNodeDepths(SimulationContext& ctx, double dt, int step);
    void setNodeDepth(SimulationContext& ctx, int node_idx, double dt, int step);
    double getLinkStep(const SimulationContext& ctx, int link_idx) const;

public:
    /// Direct write access to the per-node is_surcharged flag (for tests/non-conduit scatter).
    uint8_t& nodeSurchargedFlag(int idx) { return xnode_.is_surcharged[static_cast<std::size_t>(idx)]; }

    /// Mutable pointer to the per-node new_surf_area array (for HydStructures scatter).
    double* nodeNewSurfAreaDataMut() { return xnode_.new_surf_area.data(); }

    /// Mutable reference to the per-node sumdqdh accumulator at index n.
    double& nodeSumDqdh(int n) { return xnode_.sumdqdh[static_cast<std::size_t>(n)]; }

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
    DPSLinkArrays dps_;  ///< Per-conduit DPS state (SoA) [n_conduits_]
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
