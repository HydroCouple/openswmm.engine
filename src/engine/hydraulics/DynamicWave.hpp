// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      DynamicWave.hpp
 * @brief     Dynamic wave routing solver — batch-oriented St. Venant equations.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
#include <string>
#include <vector>

namespace openswmm {

struct SimulationContext;
class Router;

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
    double c_pT = 25.0;     ///< Target pressure celerity (ft/s, internal units)
    double alpha = 3.0;     ///< Surcharge shock parameter (>= 2)
    double r = 0.5;         ///< Decay time scale for P → 1 (seconds)
    double c_pT_sq = 625.0; ///< c_pT^2 (pre-computed)
};

/// Per-conduit DPS state — Structure of Arrays (persistent across timesteps).
/// Each vector is indexed by conduit index [0..n_conduits_).
///
/// The head-first link-node formulation treats `hs` as a diagnostic of the
/// node-head solution (`hs = max(depth_mid − y_full, 0)`) and accumulates
/// `As` from `dAs = T_s · dhs` so the slot storage is path-dependent and
/// invariant under P-only evolution.  `hs_prev_iter` records the last geometry-update head, so each Picard geometry
/// pass accumulates only its increment `dhs = hs_iter - hs_prev_iter`; the
/// anchor is advanced to hs_iter during that pass.
/// `T_s_target = g · A_C / c_pT²` is cached so the per-iter `T_s = T_s_target · P²`
/// reduces to a single multiply.
struct DPSLinkArrays {
    std::vector<double> As;           ///< Accumulated slot area (ft²)
    std::vector<double> hs;           ///< Current surcharge head (ft)
    std::vector<double> hs_prev_iter; ///< hs at start of current timestep (ft)
    std::vector<double> P;            ///< Current Preissmann Number (smoothed)
    std::vector<double> P_hat;        ///< Provisional Preissmann Number (before smoothing)
    std::vector<double> P_hat_0;      ///< Initial P for unpressurized conduit
    std::vector<double> T_s_target;   ///< g · A_C / c_pT² (cached at init)
    std::vector<double> t_s;          ///< Time when element last became surcharged (sec)
    std::vector<uint8_t> surcharged;  ///< Currently surcharged flag

    /**
     * @brief Reset every conduit DPS history array to the requested length.
     *
     * Areas/heads/times/flags reset to zero, all Preissmann-number arrays reset to one, target widths
     * reset to zero. assign discards old history and can invalidate element references; allocation
     * failure may leave partial reset. Native dimensions are ft, ft2 and seconds; n is the real-conduit
     * count, excluding DUMMY.
     *
     * @param[in] n Required element count; must fit vector allocation limits.
     */
    void resize(std::size_t n) {
        As.assign(n, 0.0);
        hs.assign(n, 0.0);
        hs_prev_iter.assign(n, 0.0);
        P.assign(n, 1.0);
        P_hat.assign(n, 1.0);
        P_hat_0.assign(n, 1.0);
        T_s_target.assign(n, 0.0);
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
    std::vector<double> new_surf_area;
    std::vector<double> old_surf_area; ///< Surface area from last non-surcharged state
    std::vector<double> sumdqdh;
    std::vector<double> dYdT;
    std::vector<uint8_t> converged;
    std::vector<uint8_t> is_surcharged; ///< TRUE when node depth > crown elevation

    /**
     * @brief Reset per-node Picard geometry, derivative and convergence state.
     *
     * Every array is assign-zeroed. Surface areas are ft2, sumdqdh ft2/s and dYdT ft/s; flags reset
     * false. Existing values/references are discarded or invalidated; allocation can throw after
     * partial reset.
     *
     * @param[in] n Required element count; must fit vector allocation limits.
     */
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
    EXTRAN = 0, ///< Classic EXTRAN approach — dQ/dH for surcharged nodes
    SLOT = 1,   ///< Preissmann slot — fictitious narrow slot above crown
    DYNAMIC_SLOT =
        2, ///< Dynamic slot — slot width varies with flow conditions (experimental) Sharior, S., Hodges, B.R., & Vasconcelos, J.G. (2023). Generalized, Dynamic, and Transient-Storage Form of the Preissmann Slot. Journal of Hydraulic Engineering, 149(11), 04023046.
    TPA = 3 ///< Two-component pressure approach (experimental, issue #156):
            ///< constant-width slot w = g·A_full/a² above the crown plus a
            ///< per-conduit sub-atmospheric latch below it. Vasconcelos,
            ///< Wright & Roe (2006), J. Hydraul. Eng. 132(6).
};

/**
 * @brief Momentum category for branch-free per-category kernel dispatch.
 *
 * @details Each conduit is classified once per Picard iteration (after geometry
 *          is computed). Per-category kernels have zero shape/type/state branches
 *          in their inner loops, enabling compiler auto-vectorization.
 */
enum class MomentumCategory : uint8_t {
    SKIP_DRY = 0,            ///< DRY/UP_DRY/DN_DRY, aMid<=FUDGE, or is_closed
    MANNING_OPEN = 1,        ///< Standard Manning, open channel (Froude-based sigma)
    MANNING_CLOSED_FS = 2,   ///< Manning, closed conduit, free surface
    MANNING_CLOSED_FULL = 3, ///< Manning, closed conduit, surcharged (fr=0, sig=0)
    FORCE_MAIN_HW = 4,       ///< Force main, Hazen-Williams friction
    FORCE_MAIN_DW = 5,       ///< Force main, Darcy-Weisbach friction
    N_CATEGORIES = 6
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
    /**
     * @brief Allocate and initialize dynamic-wave working state for a resolved network.
     *
     * Builds true-conduit tiles/maps, node adjacency, virtual pairs, momentum/geometry work arrays and
     * optional UF/DPS/TPA state. Solver option fields (surcharge/UF/celerity etc.) must be configured
     * before init. Converts display-unit minimum surface area/celerities to native ft units. Retains
     * groups by address; moving/destroying/rebuilding it requires reinitialization. No context state is
     * changed. Allocation exceptions propagate and can leave partial initialization; serialize
     * lifecycle/execute calls on an instance.
     *
     * @param[in] n_nodes Nonnegative global node count matching ctx node arrays.
     * @param[in] n_links Nonnegative global link count matching ctx link arrays; topology/side-table
     *     indices must already be valid.
     * @param[in] groups Borrowed built cross-section groups whose mutable scratch/profile tables must
     *     remain valid and exclusively available throughout solver use.
     * @param[in] ctx Resolved immutable context supplying native geometry, topology, initial state and
     *     options; not retained as a pointer.
     */
    void init(int n_nodes, int n_links, const XSectGroups& groups, const SimulationContext& ctx);

    /**
     * @brief Set the number of OpenMP threads for parallel loops.
     *
     * @details Called after init() when the thread count is finalized.
     *          Resolution (threadinfo::dwThreads): 0 = auto —
     *          omp_get_max_threads(), the conduits-per-thread gate and the
     *          Apple Silicon P-core clamp; N > 0 = honoured exactly, subject
     *          only to the conduits-per-thread gate, with warnings when N
     *          exceeds the machine / runtime limits. SWMM_DW_THREADS forces
     *          the count outright (warned).
     *
     * @param n         Requested thread count ([OPTIONS] THREADS).
     * @param warnings  Optional sink for human-readable warnings.
     */
    void setNumThreads(int n, std::vector<std::string>* warnings = nullptr);

    /// Callback for computing non-conduit link flows inside the Picard loop.
    /// Parameters: (ctx, dt, picard_step) where step=0 is first iteration.
    using NonConduitFlowFunc = std::function<void(SimulationContext&, double, int)>;

    /**
     * @brief Advance one dynamic-wave routing interval through the bounded Picard loop.
     *
     * Requires completed init with the same live XSectGroups and topology. Refreshes node tiles,
     * snapshots previous midpoint areas, advances TPA latches once, then performs team
     * geometry/momentum, surface/LID limits, flow gather/structures, outfall boundaries and
     * bounded/optional-Anderson node updates. Updates current link/node hydraulic fields and
     * nonconvergence diagnostics. DPS time/history and virtual diagnostics advance after the loop,
     * including non-converged intervals; no rollback or convergence exception is supplied. Mutable
     * group scratch and this solver cannot serve simultaneous executes. Caller owns clock advancement
     * and run-level mass/report ledgers.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     * @param[in] dt Positive finite routing interval in seconds, consistent with cached dt_gravity_ and
     *     old-state snapshot.
     * @param[in] non_conduit_fn Optional copied synchronous callback(ctx,dt,zero-based-iteration).
     *     Invoked once per Picard iteration on the team single thread after conduit gather; it
     *     computes/scatters structure flow and sensitivity, honors bypass flags and must not throw,
     *     retain mutable borrowed references or reenter this solver.
     * @return Number of Picard iterations performed, at most max_trials; use lastConverged() for actual
     *     convergence.
     */
    int execute(SimulationContext& ctx, double dt, NonConduitFlowFunc non_conduit_fn = nullptr);

    /**
     * @brief Report the final Picard convergence flag from the most recent execute.
     *
     * Iteration count alone does not indicate failure; an exhausted trial budget is non-converged only
     * when this flag is false.
     * @return True only when the final iteration satisfied node convergence, including convergence on
     *     the last allowed trial.
     */
    bool lastConverged() const { return last_converged_; }

    /**
     * @brief Compute the next native routing interval from link/node/virtual-pair constraints.
     *
     * The first variable-step call seeds the minimum interval without counting a critical element.
     * Subsequent calls use current midpoint geometry/Froude and dYdT, with a virtual-pair bound.
     * Requires matching init and current state from execute. Optional OPENSWMM_DT_TRACE emits
     * diagnostics; no finite-input validation or general non-convergence status is supplied.
     *
     * @param[in,out] ctx Matching initialized context; increments the selected critical node/link
     *     counter when a limiting element is identified.
     * @param[in] fixed_step Positive fixed routing interval in seconds; values below MIN_TIMESTEP
     *     bypass variable-step machinery.
     * @param[in] courant_factor Dimensionless positive factor enabling CFL selection; <=0 returns
     *     fixed_step unchanged.
     * @return Routing interval in seconds, floored to the configured minimum then truncated to whole
     *     milliseconds on subsequent variable-step calls.
     */
    double getRoutingStep(SimulationContext& ctx, double fixed_step, double courant_factor);

    /**
     * @brief Record convergence and timestep summaries for conduit reporting.
     *
     * Requires a completed execute with node tile/convergence flags. Both ends must converge, counting
     * outfalls as always converged for this statistic. No-flow/unconstrained sentinel links add no CFL
     * sample. No hydraulic solver state is changed.
     *
     * @param[in,out] ctx Matching initialized context; only conduit reporting counters/step
     *     distributions are updated.
     * @param[in] dt Weight of this completed routing interval in seconds; <=0 is a no-op.
     * @param[in] fixed_step Positive nominal route interval in seconds, used as a sample cap/reference.
     * @param[in] courant_factor Positive CFL multiplier; nonpositive values use one for reporting.
     */
    void accumulateLinkStepStats(SimulationContext& ctx, double dt, double fixed_step,
                                 double courant_factor) const;

    double head_tol = DEFAULT_HEAD_TOL;
    int max_trials = DEFAULT_MAX_TRIALS;
    double omega = OMEGA;
    SurchargeMethod surcharge_method = SurchargeMethod::EXTRAN;
    NodeContinuity node_continuity = NodeContinuity::EXPLICIT;
    bool anderson_accel = false; ///< Enable Anderson acceleration

    /// Unsteady friction (issue #156 Phase 3): 0 = NONE (default, bit-inert),
    /// 1 = VITKOVSKY. Folded into the momentum denominator (so dqdh_ stays
    /// consistent with the surcharge Jacobian) with the same dead-band and
    /// half-momentum clamp as kernels::ufUpdate on the FV side.
    int unsteady_friction = 0;
    double uf_k3 = 0.015; ///< Brunone-type k3; consumed only when active.

    /// TPA acoustic celerity a (PROJECT length units per second, converted to
    /// ft/s at init like FV_SLOT_CELERITY). Sets the constant slot width
    /// w_tpa = g·A_full/a² per conduit (TPA plan §B1). Issue #156.
    double tpa_celerity = 100.0;

    /// Cross-link ∂V/∂x stencil for UF (issue #156). A full link has equal
    /// end areas, so the within-link |v2−v1| estimator is structurally ZERO
    /// in exactly the pressurized cases UF exists for (measured: pure added
    /// inertia, slight ANTI-damping). The gradient therefore comes from the
    /// neighboring conduits' in-place velocities across simple degree-2
    /// conduit junctions. Momentum runs in serial conduit order when UF is
    /// active, preserving the existing serial stencil at every team size.
    /// Built once in init() when unsteady_friction != 0; -1 = no
    /// simple neighbor on that side (falls back one-sided / within-link).
    std::vector<int> uf_nb_up_, uf_nb_dn_;
    std::vector<int8_t> uf_sg_up_, uf_sg_dn_; ///< +1 same sense, -1 opposed

    /// Evaporation rate (ft/s) — set by Router::step() each timestep so that
    /// solveMomentumBatch can recompute dq6 per Picard iteration (Gap #14).
    double evap_rate = 0.0;

private:
    friend class ::openswmm::Router; // explicit internal checkpoint fields only
    int n_nodes_ = 0;
    int n_links_ = 0;
    int n_conduits_ = 0;
    int num_threads_ = 1; ///< OpenMP thread count for parallel loops
    const XSectGroups* groups_ = nullptr;

    /// True if any conduit has a nonzero seepage rate (set once in init()).
    /// With zero evaporation this gates the bit-exact dead-work skip of
    /// recomputeConduitLosses (see losses_all_zero_).
    bool any_conduit_seep_ = false;

    /// One-way latch: true while every stored conduit evap/seep loss rate is
    /// provably zero (initial state, and no pass has ever run with a nonzero
    /// loss possible). While true, recomputeConduitLosses is a structural
    /// no-op and is skipped.
    bool losses_all_zero_ = true;

    // Pre-built conduit index list for skipping non-conduits in inner loops
    std::vector<int> conduit_idx_;

    // Pre-computed per-link invariants (populated once at first execute, reused)
    // Using uint8_t instead of bool to allow .data() pointer access for SIMD/restrict
    std::vector<uint8_t> is_open_;       ///< Shape is open (RECT_OPEN/TRAP/TRI/PARA)
    std::vector<uint8_t> is_force_main_; ///< Shape is FORCE_MAIN
    std::vector<uint8_t> has_losses_;    ///< Any minor loss coeff != 0
    std::vector<double> barrels_d_;      ///< double(max(barrels, 1))
    std::vector<double> cached_length_;  ///< max(mod_length, length)
    std::vector<double> inv_length_;     ///< 1.0 / cached_length_

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
    std::vector<int> tile_uj_; ///< == conduit_idx_, co-located with rest
    /// ConduitData row of tile entry ci. NOT the tile index: conduit_idx_
    /// skips DUMMY-shaped conduits, so every ConduitData array (length,
    /// seep_rate, evap_loss_rate, inlet_control, …) must be indexed by this,
    /// never by ci — the two only coincide on a deck with no DUMMY conduit.
    std::vector<int> tile_crow_;
    std::vector<int> tile_n1_;           ///< links.node1[uj] for ci
    std::vector<int> tile_n2_;           ///< links.node2[uj]
    std::vector<double> tile_inv1_elev_; ///< nodes.invert_elev[node1]
    std::vector<double> tile_inv2_elev_; ///< nodes.invert_elev[node2]
    std::vector<double> tile_z1_off_;    ///< links.offset1
    std::vector<double> tile_z2_off_;    ///< links.offset2
    std::vector<double> tile_y_full_;    ///< links.xsect_y_full
    std::vector<double> tile_a_full_;    ///< links.xsect_a_full
    std::vector<double> tile_r_full_;    ///< links.xsect_r_full
    std::vector<double> tile_w_max_;     ///< links.xsect_w_max
    std::vector<double>
        tile_length_; ///< cached_length_ = max(mod_length, length); used for arithmetic stability
    std::vector<double> tile_inv_length_; ///< inv_length_
    std::vector<double>
        tile_links_length_; ///< routing length (legacy link_getLength) — volume and the dq6 divisor
    std::vector<double> tile_beta_;           ///< ConduitData.beta
    std::vector<double> tile_q_max_;          ///< ConduitData.q_max
    std::vector<double> tile_rough_factor_;   ///< ConduitData.rough_factor
    std::vector<double> tile_barrels_d_;      ///< barrels_d_
    std::vector<uint8_t> tile_is_open_;       ///< is_open_
    std::vector<uint8_t> tile_is_force_main_; ///< is_force_main_
    std::vector<uint8_t> tile_is_closed_;     ///< links.is_closed
    std::vector<uint8_t> tile_has_losses_;    ///< has_losses_
    std::vector<int> tile_xsect_batch_shape_; ///< links.xsect_batch_shape
    std::vector<XsectShape> tile_shape_;      ///< links.xsect_shape
    /// Phase C: pre-flag conduits that can possibly enter the Newton path
    /// in STEP C. True iff (offset1 > 0 || offset2 > 0). When false the
    /// flow-classification cascade reduces to two branchless comparisons
    /// (both_full ? SUBCRITICAL : (both_dry ? DRY : SUBCRITICAL/UP_DRY/DN_DRY)),
    /// no Newton solver call. Roughly 80–90 % of conduits in typical CSO
    /// networks have zero offsets and take this fast path.
    std::vector<uint8_t> tile_has_offset_; ///< (z1_off > 0 || z2_off > 0)
    /// Reverse map uj → ci. Sized n_links_, -1 for non-conduits. Lets the
    /// momentum-kernel functions (processManningLink, processForceMainLink,
    /// applyFlowLimits) read tile_X[ci] directly without changing their
    /// uj-based signatures.
    std::vector<int> tile_uj_to_ci_;
    /// Per-conduit timestep-invariants used by processManningLink /
    /// processForceMainLink / applyFlowLimits (added in Phase A extension).
    std::vector<int> tile_culvert_code_;
    std::vector<double> tile_slope_;
    std::vector<double> tile_q_limit_;
    std::vector<double> tile_loss_inlet_;
    std::vector<double> tile_loss_outlet_;
    std::vector<double> tile_loss_avg_;  ///< ConduitData.loss_avg
    std::vector<double> tile_roughness_; ///< ConduitData.roughness (force-main detection)
    std::vector<double>
        tile_fm_sbot_; ///< Force-main sBot rough factor (legacy link.c:1127-1131, incl. lengthFactor)
    std::vector<double>
        tile_fm_rbot_; ///< Force-main C / roughness height (xsect rBot) for the D-W friction factor
    std::vector<uint8_t> tile_has_flap_gate_;
    std::vector<int8_t> tile_direction_;

    /**
     * @brief Refresh dense per-conduit invariant copies and the global-link reverse map.
     *
     * Requires init conduit indices and cached length/barrel/open flags. Reallocates/copies the hot
     * tile and reverse map; no context values change. Use after legitimate invariant restoration before
     * execution, not concurrently with the Picard team. Does not rebuild conduit membership or node
     * topology itself.
     *
     * @param[in] ctx Resolved matching context with valid true-conduit side rows, endpoint nodes and
     *     native geometry/loss/friction fields.
     */
    void refreshConduitTile(const SimulationContext& ctx);

    // Per-timestep constants
    double dt_gravity_ = 0.0; ///< dt * GRAVITY (set once per timestep)

    bool last_converged_ = false; ///< final Picard converged flag of last execute()

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
    std::vector<double> area1_;     ///< Area at upstream depth
    std::vector<double> area2_;     ///< Area at downstream depth
    std::vector<double> area_mid_;  ///< Area at average depth
    std::vector<double> hrad_mid_;  ///< Hydraulic radius at average depth
    std::vector<double> width_mid_; ///< Top width at average depth
    std::vector<double> depth1_;    ///< Upstream flow depth
    std::vector<double> depth2_;    ///< Downstream flow depth
    std::vector<double> depth_mid_; ///< Average depth

    // Per-link momentum working arrays
    std::vector<double> velocity_; ///< Current velocity
    std::vector<double> froude_;   ///< Current Froude number
    std::vector<double> sigma_;    ///< Inertial damping factor
    std::vector<double> dqdh_;     ///< dQ/dH for node update
    std::vector<double> new_flow_; ///< Computed flow this iteration
    /// Per-BARREL flow from the conduit's last solve — legacy Conduit.q1,
    /// read back as qLast. Rebuilding it as links.flow / barrels is not
    /// exact once barrels > 1 ((q*6)/6 != q in the last bit).
    std::vector<double> q1_;

    // Per-link area from previous iteration (for unsteady term)
    std::vector<double> area_old_;

    // Per-link bypass flag (true when both end nodes converged; skip momentum solve)
    // uint8_t instead of bool: avoids std::vector<bool> bit-packing overhead
    std::vector<uint8_t> bypassed_;

    // ------------------------------------------------------------------------
    // B2 threading — CSR node→incident-conduit adjacency for the parallel
    // node-centric flow gather (see gatherConduitNodeFlows). Built once in
    // init() from static topology.
    //
    // PROOF OF BIT-EXACTNESS vs the serial per-link scatter: legacy
    // findLinkFlows (dynwave.c:385-388) scatters conduits one link at a time
    // in ascending link index; each link updates node1's accumulators, then
    // node2's. For a given NODE, the projection of that global order is
    // simply its incident conduit links in ascending link index (node1-end
    // entry before node2-end entry when both ends touch the same node). CSR
    // entries are stored per node in exactly that order, so the per-node
    // gather performs the identical FP accumulation sequence on each
    // accumulator (inflow, outflow, sumdqdh, new_surf_area) — bit-exact at
    // any thread count, since each node is owned by exactly one thread.
    // ------------------------------------------------------------------------
    std::vector<int> csr_row_;               ///< size n_nodes_+1 (prefix offsets)
    std::vector<int32_t> csr_link_;          ///< incident conduit link index j
    std::vector<uint8_t> csr_is_n2_;         ///< 0 = node1 end, 1 = node2 end
    std::vector<uint8_t> csr_other_outfall_; ///< other-end node is an OUTFALL

    // ------------------------------------------------------------------------
    // Virtual junctions — zero-storage, momentum-transmitting pair nodes
    // (see plans/VIRTUAL_JUNCTION_IMPLEMENTATION_PLAN.md). vjunc_ is empty for
    // models without [VIRTUAL_JUNCTIONS]; every hot-loop hook below is gated
    // on that emptiness so VJ-free models compile down to the original paths.
    // ------------------------------------------------------------------------
    struct VJuncPair {
        int node = -1;                  ///< virtual junction node index
        int link_a = -1, link_b = -1;   ///< the two attached conduits (any orientation)
        int up_link = -1, dn_link = -1; ///< through orientation (node2(up)==node, node1(dn)==node)
        uint8_t through = 0; ///< 1 when a through orientation exists (not a sag/peak pair)
        double lambda = 0.0; ///< (L_a + L_b) / 2 — interface control-volume span

        // Per-Picard-iteration cache (filled by vjPrepareIteration):
        double sigma_j = 1.0;  ///< shared junction sigma from through-flow Froude
        double a_up_mid = 0.0; ///< up-link mid area (cross-junction upwind state)
        double r_up_mid = 0.0; ///< up-link mid hyd. radius (upwind state)
        double dq4j = 0.0;     ///< cross-junction convective correction (FULL mode)
        uint8_t active = 0;    ///< pair coupling live this iteration (wet through pair)
        int8_t dirn = 0;       ///< coherent through-flow: +1 up→dn, −1 dn→up, 0 opposing/still

        // Momentum-residual diagnostic accumulators (whole run):
        double resid_max = 0.0;
        double resid_sum = 0.0;
        long long resid_n = 0;
    };
    std::vector<VJuncPair> vjunc_;
    /// Link j → vjunc_ row of the pair at each end (-1 = none). A chain link
    /// belongs to TWO pairs (dn side of its node1's pair, up side of its
    /// node2's pair) — a single map drops one side and biases the coupling.
    std::vector<int32_t> vj_pair_n1_;
    std::vector<int32_t> vj_pair_n2_;

    /// Wetting floor for a virtual junction that receives lateral inflow
    /// (plans/VJ_LATERAL_INFLOW_PLAN_2026-09-04.md): the pair's natural
    /// half-link free-surface area evaluated at the seed depth
    /// kVjWetSeedFrac · y_full, so an imposed lateral can wet a dry pair
    /// without dividing dV by a vanishing area — and without the fixed
    /// MIN_SURFAREA storage the feature exists to remove. max(natural, floor)
    /// is continuous at the seed depth and inert above it. Zero for every
    /// other node; read only when is_virtual && lat_flow != 0.
    static constexpr double kVjWetSeedFrac = 0.02;
    std::vector<double> vj_wet_floor_;

    /**
     * @brief Build virtual-pair topology, orientation maps and lateral wetting-area floors.
     *
     * Clears pair diagnostics/maps, discovers pair attachments and through orientation, caches
     * half-link interface span and a seed-depth natural wetting floor. Sag/peak pairs keep zero-storage
     * continuity but no directional momentum coupling. Assumes validated topology; does not repair
     * malformed virtual junctions. Allocations may throw; context is read-only.
     *
     * @param[in] ctx Resolved context after conduit CSR/tile initialization, with validated two-conduit
     *     virtual topology and optional inlet/lateral flags.
     */
    void buildVirtualJunctionPairs(const SimulationContext& ctx);
    /**
     * @brief Prepare coherent through-pair momentum caches for the current Picard geometry.
     *
     * Serial call behind the team single/barrier. Resets active/direction/correction, excludes inlet
     * junctions and dry/non-through pairs, computes shared Froude damping/upwind state and optional
     * FULL cross-junction convective correction. Uses per-barrel conveyance flow/area. Changes pair
     * scratch only; the existing FULL correction has documented numerical limitations and is not
     * repaired here.
     *
     * @param[in] ctx Matching context with current per-link flow; prepared pre-slot-override end/mid
     *     geometry is read.
     * @param[in] dt Positive native timestep in seconds.
     */
    void vjPrepareIteration(const SimulationContext& ctx, double dt);
    /**
     * @brief Accumulate post-step virtual-pair momentum mismatch diagnostics.
     *
     * For wet through pairs computes per-barrel Q^2/A mismatch plus mean-area hydrostatic head term in
     * ft4/s2. Updates absolute maximum/sum/count in pair state and report mirrors; no hydraulic state
     * changes. Rebuilds report vectors if pair count differs, so allocation can throw.
     *
     * @param[in,out] ctx Matching current link/node state; ctx.vj_diag is initialized/refreshed and
     *     receives cumulative report values.
     */
    void vjAccumulateResiduals(SimulationContext& ctx);

    // Node-dense tile of the per-node invariants setNodeDepth touches every
    // Picard iteration. setNodeDepth reads ~20 SoA arrays per node; the seven
    // step-invariant ones below otherwise cost seven separate cache streams
    // (legacy's AoS TNode record pays 1-2 lines per node for the same reads).
    // Rebuilt once per routing step in execute() — amortised over the Picard
    // iterations and automatically correct even if the editing API mutates
    // node geometry mid-run. y_crown pre-evaluates crownElev − invertElev
    // with the identical operands the per-call subtraction used, so the
    // value is bit-identical (legacy setNodeDepth recomputes it per call).
    struct NodeTile {
        double full_depth;
        double y_crown; ///< crown_elev − invert_elev
        double invert_elev;
        double ponded_area;
        double sur_depth;
        double full_volume;
        double rpt_full_volume; ///< legacy Node.fullVolume convention (NodeData::rpt_full_volume)
        int32_t degree;
        uint8_t is_storage;
        uint8_t is_outfall;
        uint8_t is_virtual; ///< zero-storage virtual junction (see vjunc_)
    };
    std::vector<NodeTile> node_tile_;
    // Unit system for node volume/surf-area table dispatch, hoisted from the
    // per-call ucf::getUnitSystem(options.flow_units) (options are fixed
    // during a run).
    int unit_sys_ = 0;

    // Per-link surface area contributions to upstream/downstream nodes
    // (matching legacy Link[].surfArea1/surfArea2 from dwflow.c findSurfArea)
    std::vector<double> surf_area1_; ///< Surface area at upstream node (ft²)
    std::vector<double> surf_area2_; ///< Surface area at downstream node (ft²)

    // Per-link upstream geometry (for proper weighted hyd. radius)
    std::vector<double> hrad1_;  ///< Hydraulic radius at upstream depth
    std::vector<double> width1_; ///< Top width at upstream depth
    std::vector<double> width2_; ///< Top width at downstream depth

    // Per-link head values (persisted from computeLinkGeometry for solveMomentumBatch,
    // may be modified by flow classification for UP_CRITICAL/DN_CRITICAL cases)
    std::vector<double> h1_;    ///< Head at upstream node (possibly modified)
    std::vector<double> h2_;    ///< Head at downstream node (possibly modified)
    std::vector<double> fasnh_; ///< Fraction between normal & critical depth

    // Anderson acceleration state (per-node, depth-2 mixing)
    std::vector<double> aa_y_prev_; ///< Node depths at iteration k-1
    std::vector<double> aa_g_prev_; ///< G(y_{k-1}) — computed depths at k-1
    std::vector<double> aa_r_prev_; ///< Residual r_{k-1} = G(y_{k-1}) - y_{k-1}
    std::vector<uint8_t> aa_skip_;  ///< Per-node flag: skip AA this iteration

    // Per-conduit momentum category (rebuilt each Picard iteration).
    // solveMomentumBatch dispatches on category_[uj] inline — no auxiliary
    // per-category index list is needed.
    std::vector<MomentumCategory> category_;

    // Internal methods
    /**
     * @brief Reset one Picard iteration node accumulators and lateral/loss baseline.
     *
     * Collectively team-callable orphaned omp-for-nowait: each worker owns disjoint nodes and consumers
     * require a later barrier. Clears convergence/sensitivity, evaluates storage/pond or coupled-pond
     * surface area, resets inflow/outflow, adds signed lateral flow and existing node losses. Does not
     * apply the ordinary MIN_SURFAREA floor; setNodeDepth does that. Requires execute to refresh
     * node_tile_ first.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     */
    void initNodeStates(SimulationContext& ctx);
    /**
     * @brief Mark links whose two endpoint nodes converged for holding next iteration.
     *
     * Collective orphaned omp-for with closing barrier. Missing negative endpoints are not bypassed. A
     * serial virtual-pair fixup forces both pair conduits live whenever their bypass flags disagree.
     * Includes structures as well as conduits; the structure callback must honor these flags. Mutates
     * bypass scratch only.
     *
     * @param[in] ctx Matching resolved context and initialized solver maps/geometry/accumulators; no
     *     context values are changed.
     */
    void findBypassedLinks(const SimulationContext& ctx);
    /**
     * @brief Build current per-conduit end/mid geometry and legacy flow classification.
     *
     * Collectively called by every worker in the persistent team. Packs the current bypass mask,
     * computes port-aware heads/depths, evaluates grouped widths, patches normal/critical/dry end
     * classifications, and evaluates grouped area/radius triples with barriers between
     * producer/consumer phases. Bypassed conduit geometry is held. DPS/TPA overrides run serially after
     * group evaluation; static SLOT/EXTRAN overrides are deferred to momentumKernels. Writes solver
     * geometry and link flow classes; requires exclusive mutable XSectGroups scratch and prebuilt
     * native tiles/profile tables.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     */
    void computeLinkGeometry(SimulationContext& ctx);
    /**
     * @brief Recompute one live conduit evaporation and signed seepage for this Picard iterate.
     *
     * Own-element writes are safe within the conduit team loop. Dry/closed/empty categories explicitly
     * zero both loss rates. Uses mean old/current depth, true routing length and faithful section
     * width; positive physical losses share a current-volume/dt cap while aquifer gain passes through
     * its own cap. Host seepage forcing applies last and can override the physical cap. Stores
     * per-barrel ft3/s in the mapped ConduitData row. Caller skips the call while the global zero-loss
     * latch proves no work, and bypassed conduits keep previous rates.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     * @param[in] dt Positive finite routing interval in seconds, consistent with cached dt_gravity_ and
     *     old-state snapshot.
     * @param[in] ci Dense true-conduit tile index in [0,n_conduits_), distinct from both global link
     *     and ConduitData row indices.
     */
    void recomputeConduitLossOne(SimulationContext& ctx, double dt, int ci);
    /**
     * @brief Run the fused conduit Picard tail and publish each solved flow/sensitivity.
     *
     * Collective persistent-team entry. Serially prepares virtual-pair caches, then each live conduit
     * recomputes losses, applies static crown/slot geometry, classifies dry/Manning/force-main momentum
     * and commits links.flow/dqdh. Bypassed links retain both flow and derivative. Active
     * unsteady-friction cross-link reads require serial conduit order under omp-single; other
     * configurations use disjoint omp-for ownership. No node flow scatter is performed here.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     * @param[in] dt Positive finite routing interval in seconds, consistent with cached dt_gravity_ and
     *     old-state snapshot.
     * @param[in] step Zero-based Picard iteration; >0 applies under-relaxation paths.
     */
    void momentumKernels(SimulationContext& ctx, double dt, int step);

    /**
     * @brief Zero a dry/closed conduit flow while preserving legacy dry-state bookkeeping.
     *
     * Requires prepared end/mid geometry and tile. Stores zero new_flow/q1/Froude, computes aggregate
     * dqdh from the pre-average midpoint area, replaces midpoint area by the two-end mean, and
     * publishes crown-capped depth and native routing-length volume. Does not clear every held conduit
     * flag or scatter to nodes. Called by the worker owning this conduit.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     * @param[in] dt Unused uniform-kernel argument; cached dt_gravity_ supplies the timestep-dependent
     *     sensitivity.
     * @param[in] uj Valid global index of a true initialized conduit, with a nonnegative reverse tile
     *     mapping; unchecked.
     */
    void processDryLink(SimulationContext& ctx, double dt, std::size_t uj);
    /**
     * @brief Update one wet Manning conduit momentum and apply its physical/control limits.
     *
     * Uses per-barrel conveyance areas capped at a_full for velocity/friction/inertial/minor-loss
     * terms, while slot area remains in storage. Evaluates legacy damping/upwind weights, Manning
     * friction, head/inertial/loss terms and optional UF, then updates aggregate dqdh/Froude and
     * delegates final flow/volume flags to applyFlowLimits. Default friction exponent preserves legacy
     * 1.33333; opt-in SWMM_FAST_MANNING_POW uses its fast exponent path. Requires positive wet
     * area/radius/length. UF in-place neighbor reads require the serial conduit path.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     * @param[in] dt Positive finite routing interval in seconds, consistent with cached dt_gravity_ and
     *     old-state snapshot.
     * @param[in] step Zero-based Picard iteration; >0 applies under-relaxation paths.
     * @param[in] uj Valid global index of a true initialized conduit, with a nonnegative reverse tile
     *     mapping; unchecked.
     * @param[in] cat MANNING_OPEN, MANNING_CLOSED_FS or MANNING_CLOSED_FULL, matching prepared
     *     geometry.
     */
    void processManningLink(SimulationContext& ctx, double dt, int step, std::size_t uj,
                            MomentumCategory cat);
    /**
     * @brief Emit optional native-hex per-link momentum diagnostics.
     *
     * Process-static configuration reads SWMM_TRACE_LINK/RSTEP/SKIP/LSTEP once, opens a native narrow
     * filename and writes at most 128 selected rows before closing. Disabled tracing is inert after
     * initialization. Only the selected conduit worker may mutate the stream; multiple solver/context
     * instances sharing this diagnostic must be externally serialized. File errors are ignored and no
     * hydraulic state changes.
     *
     * @param[in] ctx Matching resolved context and initialized solver maps/geometry/accumulators; no
     *     context values are changed.
     * @param[in] uj Valid global index of a true initialized conduit, with a nonnegative reverse tile
     *     mapping; unchecked.
     * @param[in] qLast Previous Picard per-barrel flow in ft3/s.
     * @param[in] v Conveyance velocity in ft/s.
     * @param[in] sig Dimensionless inertial damping coefficient.
     * @param[in] rho Dimensionless upstream weighting coefficient.
     * @param[in] aWtd Weighted per-barrel conveyance area in ft2.
     * @param[in] rWtd Weighted hydraulic radius in ft.
     * @param[in] dq1 Dimensionless steady-friction denominator increment.
     * @param[in] dq2 Per-barrel head-gradient discharge term in ft3/s.
     * @param[in] dq3 Per-barrel local inertial discharge term in ft3/s.
     * @param[in] dq4 Per-barrel convective discharge term in ft3/s.
     * @param[in] dq5 Dimensionless minor-loss denominator increment.
     * @param[in] dq6 Per-barrel evaporation/seepage momentum discharge term in ft3/s.
     * @param[in] qOld Accepted previous-step per-barrel flow in ft3/s.
     * @param[in] q Raw candidate per-barrel flow before final limits, in ft3/s.
     * @param[in] aMidConv Slot-excluded midpoint per-barrel conveyance area in ft2.
     */
    void traceLinkTerms(const SimulationContext& ctx, std::size_t uj, double qLast, double v,
                        double sig, double rho, double aWtd, double rWtd, double dq1, double dq2,
                        double dq3, double dq4, double dq5, double dq6, double qOld, double q,
                        double aMidConv);
    /**
     * @brief Update one full force-main momentum with the selected native friction law.
     *
     * Requires full wet native geometry and positive length/area/radius. Uses slot-excluded conveyance,
     * selected Hazen-Williams/Darcy-Weisbach friction, head and local/evap-seep loss terms, optional
     * local UF acceleration and aggregate flow/head derivative. Shares trace and applyFlowLimits
     * publication with Manning. Does not itself scatter to nodes; called by the owning conduit worker.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     * @param[in] dt Positive finite routing interval in seconds, consistent with cached dt_gravity_ and
     *     old-state snapshot.
     * @param[in] step Zero-based Picard iteration; >0 applies under-relaxation paths.
     * @param[in] uj Valid global index of a true initialized conduit, with a nonnegative reverse tile
     *     mapping; unchecked.
     * @param[in] cat FORCE_MAIN_HW or FORCE_MAIN_DW, selected from the global equation option.
     */
    void processForceMainLink(SimulationContext& ctx, double dt, int step, std::size_t uj,
                              MomentumCategory cat);
    /**
     * @brief Limit and publish one candidate conduit discharge and its depth/volume flags.
     *
     * Resets inlet/normal-limit flags, applies positive-flow inlet or normal-flow limits, then
     * under-relaxation/sign-flip guard, authored flow cap, link/outfall flap gates and dry-donor port
     * guard. Stores per-barrel q1, aggregate new_flow, crown-capped displayed depth, endpoint-mean area
     * times true length/barrels as volume and full-state bits. Culvert control can replace dqdh. Node
     * ledgers and links.flow commit are performed later by the caller.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     * @param[in] dt Uniform post-processing timestep in seconds; current implementation does not
     *     otherwise consume it.
     * @param[in] step Zero-based Picard iteration; >0 applies under-relaxation paths.
     * @param[in] uj Valid global index of a true initialized conduit, with a nonnegative reverse tile
     *     mapping; unchecked.
     * @param[in,out] q Per-barrel candidate discharge in ft3/s, modified in place by all
     *     limit/relaxation checks.
     * @param[in] qLast Previous Picard per-barrel discharge in ft3/s.
     * @param[in] barrels_d Positive double barrel multiplier used to publish aggregate new_flow and
     *     native volume.
     * @param[in] isFull True when both prepared endpoint depths reach full depth; excludes the
     *     part-full culvert inlet branch.
     */
    void applyFlowLimits(SimulationContext& ctx, double dt, int step, std::size_t uj, double& q,
                         double qLast, double barrels_d, bool isFull);
    /**
     * @brief Scatter all current link flows and conduit properties into node accumulators serially.
     *
     * Standalone/no-callback path after initNodeStates and momentum preparation. Reapplies
     * surface-outfall availability bounds, commits links.flow, then accumulates signed flow, dqdh,
     * per-barrel conduit loss times barrels and endpoint surface areas in ascending link order. Aquifer
     * gains add inflow; losses skip outfall endpoints and split between other ends. Requires all
     * referenced true conduits to have valid tile mappings; no general topology validation. Do not also
     * gather the same conduits or scatter callback structures a second time.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     */
    void updateNodeFlows(SimulationContext& ctx);
    /**
     * @brief Build node-to-true-conduit incidence in deterministic serial-scatter order.
     *
     * Rebuilds CSR, end-side and opposite-outfall flags. Each node list preserves ascending link order,
     * including node1 before node2 for self-incidence; parallel gathers can reproduce serial addition
     * order. Allocates scratch and must run outside the Picard team. Does not mutate ctx.
     *
     * @param[in] ctx Resolved matching link/node topology; conduit_idx_ must be ascending and endpoint
     *     indices valid or negative absent sentinels.
     */
    void buildConduitNodeCSR(const SimulationContext& ctx);
    /**
     * @brief Accumulate conduit contributions once per node in deterministic CSR order.
     *
     * Collectively called orphaned omp-for after momentum commits and initNodeStates, before the serial
     * structure callback. Each worker owns a node and gathers current aggregate new_flow/dqdh, signed
     * barrel-scaled conduit gains/losses and surface areas in the serial link/end order. Does not
     * recompute or commit link flow and does not include structures. Closing barrier orders
     * accumulators for later callback/depth consumers.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     */
    void gatherConduitNodeFlows(SimulationContext& ctx);
    /**
     * @brief Mark nodes where the current Picard operator is unsuitable for Anderson mixing.
     *
     * Serial team-single phase. No-op when acceleration is disabled; otherwise clears/rebuilds mask for
     * explicit EXTRAN surcharge, active DPS, static-slot crown band, changed TPA latches,
     * crown-crossing weirs/orifices and every pump endpoint. Reads current xnode/link geometry; writes
     * aa_skip_ only. LID nodes receive an additional exclusion during updateNodeDepthsTeam.
     *
     * @param[in] ctx Matching resolved context and initialized solver maps/geometry/accumulators; no
     *     context values are changed.
     */
    void computeAASkipFlags(const SimulationContext& ctx);
    /**
     * @brief Commit node Picard candidates, optional bounded mixing and a shared convergence tally.
     *
     * Collective orphaned omp-for-nowait, atomic integer combine and final barrier. Skips outfalls
     * while leaving their individual convergence flags false. Calls setNodeDepth, optionally accepts
     * guarded depth-two convex Anderson mixing through the canonical commit, records raw history, and
     * requires both raw Picard residual and accepted movement <=head_tol. All workers see the completed
     * tally on return; no floating reduction changes summation order.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     * @param[in] dt Positive finite routing interval in seconds, consistent with cached dt_gravity_ and
     *     old-state snapshot.
     * @param[in] step Zero-based Picard iteration; >0 applies under-relaxation paths.
     * @param[in,out] unconv_shared Shared integer tally; caller must zero it before the team entry
     *     behind a barrier. Each worker atomically adds its disjoint nonconverged count.
     */
    void updateNodeDepthsTeam(SimulationContext& ctx, double dt, int step, int& unconv_shared);
    /**
     * @brief Solve and commit one current node-continuity update.
     *
     * Requires current inflow/outflow, old net inflow/depth/volume, accumulated natural area and dqdh.
     * Selects unfloored virtual continuity, ordinary explicit/EXTRAN or unified semi-implicit update,
     * relaxation and surcharge transitions, then commits physical bounds/volume/overflow/head/dYdT.
     * Active LID nodes instead solve accepted volume including conduit-storage shares and port water,
     * with Richards-specific handling. Reads neighboring topology but owns this node state; callers
     * exclude unsafe simultaneous external writes.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     * @param[in] node_idx Valid non-outfall node index with a current execute node tile.
     * @param[in] dt Positive finite routing interval in seconds, consistent with cached dt_gravity_ and
     *     old-state snapshot.
     * @param[in] step Zero-based Picard iteration; >0 applies under-relaxation paths.
     */
    void setNodeDepth(SimulationContext& ctx, int node_idx, double dt, int step);
    /**
     * @brief Commit one ordinary or virtual accepted depth and its consistent diagnostics.
     *
     * Sealed virtual nodes retain zero volume/overflow with no upper depth cap. Inlet virtual nodes can
     * flood but still book zero storage. Ordinary nodes apply rim/surcharge/ponding rules, derive
     * legacy booked rather than artificial mixed-store volume, clear/recompute overflow, and publish
     * depth/head plus dYdT from the same candidate. May be called twice for a raw Picard then accepted
     * Anderson candidate; state is recomputed to avoid stale diagnostics. LID nodes use their own
     * volume path in setNodeDepth.
     *
     * @param[in,out] ctx Matching initialized native-unit context with fixed validated topology/side
     *     tables, current and previous routing state and prepared external forcing. No context pointer
     *     is retained.
     * @param[in] node_idx Valid initialized non-LID node index with current invariant tile.
     * @param[in] y_new Accepted candidate native depth in ft; floored at zero before publication.
     * @param[in] dV Trapezoidal accepted node volume-change estimate in ft3, used for flooding/ponded
     *     bookkeeping.
     * @param[in] dt Positive seconds required for overflow divisions; dYdT is refreshed only when dt>0.
     */
    void commitNodeDepthState(SimulationContext& ctx, int node_idx, double y_new, double dV,
                              double dt);
    /**
     * @brief Compute one conduit native CFL interval before the caller Courant multiplier.
     *
     * Uses current midpoint geometry and per-barrel flow/volume. DPS surcharge uses physical conveyance
     * velocity plus pressure celerity c_pT/P; ordinary links use
     * volume/Q*modLength/trueLength*Fr/(1+Fr) in legacy arithmetic order. Requires initialization and
     * current geometry; does not mutate statistics or solver state.
     *
     * @param[in] ctx Matching resolved context and initialized solver maps/geometry/accumulators; no
     *     context values are changed.
     * @param[in] link_idx Valid global link index, unchecked; non-conduits return the unconstrained
     *     sentinel.
     * @return Seconds, or 1e10 for non-conduit/dry/still/low-Froude state.
     */
    double getLinkStep(const SimulationContext& ctx, int link_idx) const;

public:
    /**
     * @brief Check whether the node head-sensitivity working array is nonempty.
     *
     * This is an allocation-state guard for shared structure callbacks, not a full
     * initialization-validity or convergence check.
     * @return True when sumdqdh has elements; false before init and also for a zero-node
     *     initialization.
     */
    bool isInitialized() const noexcept { return !xnode_.sumdqdh.empty(); }

    /**
     * @brief Borrow mutable per-node surcharge diagnostic state.
     *
     * Caller must serialize access with the Picard team. Changes affect diagnostics/AA logic and must
     * preserve solver invariants.
     *
     * @param[in] idx Valid nonnegative initialized node index, unchecked.
     * @return Reference to the flag; valid until reinitialization/reallocation or destruction.
     */
    uint8_t& nodeSurchargedFlag(int idx) {
        return xnode_.is_surcharged[static_cast<std::size_t>(idx)];
    }

    /**
     * @brief Borrow writable per-node Picard surface-area accumulation storage.
     *
     * No ownership transfers. Pointer/contents can be invalidated by init or destruction; coordinate
     * writes with the solver scatter/gather phase. The C++ empty-vector data() value is not guaranteed
     * to be null.
     * @return data() pointer to n_nodes entries in ft2; an empty vector pointer must not be
     *     dereferenced.
     */
    double* nodeNewSurfAreaDataMut() { return xnode_.new_surf_area.data(); }

    /**
     * @brief Check whether a link holds its previous Picard flow/geometry this iteration.
     *
     * A shared structure callback uses this to hold a converged link. Uninitialized storage returns
     * false; read only after the corresponding Picard barrier.
     *
     * @param[in] j Global link index; negative or out-of-range values return false after size
     *     conversion.
     * @return True for a nonzero initialized bypass flag, otherwise false.
     */
    bool isBypassed(int j) const {
        const auto uj = static_cast<std::size_t>(j);
        return uj < bypassed_.size() && bypassed_[uj] != 0;
    }

    /**
     * @brief Borrow mutable node flow/head sensitivity accumulation.
     *
     * Meaningful inside the implicit continuity/Picard scatter. No ownership transfers;
     * reinit/destruction invalidates it. Serialize external writes with the owning node/team phase.
     *
     * @param[in] n Valid nonnegative initialized node index, unchecked.
     * @return Reference to the current sumdqdh entry in ft2/s.
     */
    double& nodeSumDqdh(int n) { return xnode_.sumdqdh[static_cast<std::size_t>(n)]; }

    /**
     * @brief Apply the private continuity update directly for a test fixture.
     *
     * Requires execute to have built the node invariant tile, and current solver accumulators. Commits
     * depth/head/volume/overflow/dYdT through the production node path, including special LID handling.
     * Does not independently check convergence or prepare boundary/link flows.
     *
     * @param[in,out] ctx Matching initialized context with valid current/old node state and prepared
     *     flow/surface-area inputs.
     * @param[in] node_idx Valid non-outfall node index.
     * @param[in] dt Positive timestep in seconds.
     * @param[in] step Zero-based Picard iteration; >0 enables under-relaxation paths.
     */
    void setNodeDepthForTest(SimulationContext& ctx, int node_idx, double dt, int step) {
        setNodeDepth(ctx, node_idx, dt, step);
    }

    /// Access per-node AA skip flags (read-only, for testing/diagnostics).
    const std::vector<uint8_t>& aaSkipFlags() const { return aa_skip_; }

    /// Read-only access to DPS per-conduit state arrays (for tests/diagnostics).
    const DPSLinkArrays& dpsState() const { return dps_; }
    /// Read-only access to DPS configuration (for tests/diagnostics).
    const DPSConfig& dpsConfig() const { return dps_config_; }
    /// Mutable access to DPS state for tests that need to seed slot conditions.
    DPSLinkArrays& dpsStateMut() { return dps_; }

private:
    // Preissmann slot helpers (matching legacy dwflow.c)
    /**
     * @brief Evaluate the static Sjoberg slot width for one closed shape.
     *
     * Above normalized depth 1.78 returns 0.01*w_max; otherwise w_max*0.5423*exp(-(y/y_full)^2.4). DPS
     * and TPA use their own geometry methods and return zero here. No finite checks or slot storage
     * integration are performed.
     *
     * @param[in] y Finite depth in ft.
     * @param[in] y_full Full depth in ft; <=0 returns zero.
     * @param[in] w_max Maximum section width in ft.
     * @param[in] shape Storage XsectShape identifier used to exclude open profiles.
     * @return Width in ft: zero unless surcharge_method is SLOT and y/y_full reaches the slot cutoff.
     */
    double getSlotWidth(double y, double y_full, double w_max, XsectShape shape) const;
    /**
     * @brief Select the normalized crown cutoff for the configured surcharge method.
     * @return SLOT_CROWN_CUTOFF for SLOT/DYNAMIC_SLOT/TPA, otherwise EXTRAN_CROWN_CUTOFF.
     */
    double getCrownCutoff() const;

    // Dynamic Preissmann Slot (DPS) state and methods
    DPSConfig dps_config_;
    DPSLinkArrays dps_;     ///< Per-conduit DPS state (SoA) [n_conduits_]
    double sim_time_ = 0.0; ///< Accumulated simulation time (seconds)

    /**
     * @brief Update path-dependent DPS slot area and override closed-conduit geometry.
     *
     * Requires current end/mid depths, initialized DPS arrays and group-computed geometry. For each
     * closed valid section accumulates Ts*(hs_iter-hs_prev_iter) into nonnegative As, advances both hs
     * and its anchor, then supplies slot-inclusive area, width, endpoint surface contributions and full
     * friction radius. Internal DPS history changes each Picard call; ctx itself is only read. Run
     * serially behind the team geometry barrier.
     *
     * @param[in] ctx Matching resolved context; all solver conduit/node maps and geometry/history
     *     arrays are initialized.
     */
    void applyDPSGeometry(SimulationContext& ctx);

    // -- TPA (SurchargeMethod::TPA, issue #156 Phase 5) ----------------------
    /// Constant slot width g·A_full/a² per conduit (ft), from tpa_celerity.
    std::vector<double> tpa_w_;
    /// Sub-atmospheric latch per conduit: while set, the conduit keeps
    /// full-pipe geometry (signed slot line) even when node heads drop below
    /// the crown — the latch's only job is preventing the free-surface
    /// tables from reinterpreting a sealed head as a depth (TPA plan §B2).
    std::vector<uint8_t> tpa_latch_;
    /// Latch transitions this routing step (for the AA skip walk, §B3).
    std::vector<uint8_t> tpa_latch_changed_;

    /**
     * @brief Advance sub-atmospheric regime history once before the Picard loop.
     *
     * Reads last-committed endpoint/mid depths. Unlatched closed conduits engage only when both ends
     * reach full depth; latched ones clear on a midpoint 30-ft column-separation bound or an
     * unsubmerged vented end. Virtual junctions or positive surcharge depth seal an end. Records
     * latch-change flags for Anderson guards; ctx is read-only and membership stays fixed within this
     * step.
     *
     * @param[in] ctx Matching resolved context; all solver conduit/node maps and geometry/history
     *     arrays are initialized.
     */
    void updateTpaLatch(SimulationContext& ctx);

    /**
     * @brief Apply the signed TPA slot-line geometry to engaged closed conduits.
     *
     * Requires initialized tpa widths/latches and current depths. Engages when latched or midpoint
     * reaches SLOT_CROWN_CUTOFF*y_full. Overrides end/mid area with
     * max(a_full+w*(y-y_full),0.05*a_full), width w, full hydraulic radius and endpoint slot surface
     * contributions. ctx is read-only; only working geometry changes. Run serially after batch
     * geometry.
     *
     * @param[in] ctx Matching resolved context; all solver conduit/node maps and geometry/history
     *     arrays are initialized.
     */
    void applyTpaGeometry(SimulationContext& ctx);

    /**
     * @brief Advance DPS onset/decay history after the completed Picard loop.
     *
     * Tracks surcharge when midpoint exceeds full depth and accumulated slot area is positive. Sets
     * onset time, exponentially decays P_hat toward one using configured r, or resets area/head/anchor
     * when depressurized. Then spatially smooths P. Called after execute even when the trial budget
     * ended without convergence; no convergence test is performed here.
     *
     * @param[in] ctx Matching context supplying current closed/open geometry; ctx is not modified.
     * @param[in] dt Positive routing interval in seconds, added to accumulated sim_time_.
     */
    void updateDPSState(SimulationContext& ctx, double dt);

    /**
     * @brief Smooth provisional Preissmann numbers across the link-node graph.
     *
     * Reads current topology; averages closed-conduit P_hat at each incident node, averages the two
     * endpoint means for each conduit and floors resulting P at one. Missing endpoints use the conduit
     * own provisional value. Uses thread-local scratch vectors and may allocate; serialize updates to
     * this solver DPS state.
     *
     * @param[in] ctx Matching resolved context; all solver conduit/node maps and geometry/history
     *     arrays are initialized.
     */
    void spatialSmoothP(const SimulationContext& ctx);
};

} // namespace dynwave
} // namespace openswmm

#endif // OPENSWMM_DYNAMIC_WAVE_HPP
