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
 * @file      PressurizedHeadSolver.hpp
 * @brief     Implicit acoustic/slot head update on the pressurized subset
 *        (slot program R2a, Strategy E).
 *
 * @details Above the taper band the Preissmann-slot closure is linear in head
 *          (A = a_crown + t_slot·(h − y_full)), so the acoustic pair — slot
 *          storage T·∂H/∂t against the pressure gradient g·A·∂H/∂x — is a
 *          LINEAR diffusion system in H that can be integrated implicitly
 *          (θ = 1), unconditionally stably. Everything else stays exactly the
 *          explicit Godunov scheme: membership is a pure function of the
 *          instantaneous state (h ≥ y_crown, band entry — no flags, no
 *          memory) — EXCEPT under the TPA closure (issue #156), where
 *          membership IS the regime flag (physical air-pathway history,
 *          cleared on cold start and hotstart restore; see cellPressurized
 *          below — the SLOT-closure path above is untouched). The flag is
 *          fixed within a substep, so the SPD structure is unchanged either
 *          way — and the solve is a FLUX PREDICTOR whose back-substituted
 *          face discharges overwrite `f_mass_`, the single array both the
 *          cell update and the node update read — so conservation, rollback,
 *          hot start and reporting are structurally untouched.
 *
 *          Per implicit face (α = 1/(1 + Δt·γ_f), friction semi-implicit as
 *          everywhere else in the scheme):
 *
 *            Q_f^{n+1} = α·Q*_f − C_f·(H_b − H_a),   C_f = α·Δt·g·Â_f/L_f
 *
 *          and eliminating Q from each pressurized cell's continuity gives
 *
 *            (T_i·Δx_i/Δt)·H_i + Σ_f C_f·(H_i − H_nbr) = RHS_i
 *
 *          — symmetric, diagonally dominant with strictly positive cell
 *          diagonals: SPD per pressurized component. Chains solve by the
 *          Thomas algorithm; components with a degree-≥3 folded junction by
 *          Jacobi-preconditioned CG.
 *
 *          Face taxonomy (re-derived every substep; memoryless under SLOT,
 *          flag-consistent under TPA via cellPressurized/ghostPressurized):
 *            FULL   — both sides pressurized (cell–cell, or cell–node with a
 *                     pressurized ghost). The Casulli-form flux above; at
 *                     steady state it reduces to ΔH/L = −S_f exactly, which
 *                     is what makes full-bore head loss independent of the
 *                     slot width (gate G1).
 *            DELTA  — exactly one side pressurized: the TRANSITION face.
 *                     Stays FULLY explicit — Godunov mass and momentum flux,
 *                     and the full explicit census bound. Bores live here,
 *                     and a filling front advanced at the implicit Δt moves
 *                     one cell per substep regardless of physics (measured:
 *                     arrival 65 s / 345 s at c = 150 / 660 against the
 *                     explicit 115 s). Keeping the front explicit costs dt
 *                     only WHILE a front exists; a fully pressurized network
 *                     has no transition faces and runs advection-bound,
 *                     which is where the R2 runtime win lives. (Giving
 *                     standing fronts their own fine tier is R2b's job.)
 *            (none) — gated, culvert-capped and closed-end faces are never
 *                     implicit: their laws stand and enter the pressurized
 *                     rows as explicit constants.
 *
 *          Pressurized ALGEBRAIC junctions are folded in as unknown rows
 *          (zero storage — today's algebraic convention — plus the face
 *          conductances), because lagging their heads would re-create the
 *          ghost-feedback stiffness this solver exists to delete. Fixed-head
 *          nodes and storage/demoted nodes enter as Dirichlet data at their
 *          current heads. A folded row whose solved head violates the node's
 *          physical ceiling (rim + surcharge depth) or floor (invert) is
 *          demoted to a Dirichlet row at the clamp and the affected
 *          components re-solved once — the flux imbalance then lands in the
 *          node's carry ledger, where settleAlgebraicNode already books
 *          flooding and ponding exactly as before.
 *
 * @see plans/FV_SLOT_STORAGE_PROGRAM_2026-08-24.md §R2
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_PRESSURIZED_HEAD_SOLVER_HPP
#define OPENSWMM_ENGINE_FV_PRESSURIZED_HEAD_SOLVER_HPP

#include <cstdint>
#include <vector>

#include "FvKernels.hpp"
#include "INetworkSolver.hpp"
#include "NetworkMeshData.hpp"
#include "NonlinearPressureStorage.hpp"

namespace openswmm::fv {

/// Non-owning view of the solver internals the implicit pass reads and
/// writes. Same pattern as transport's SpeciesKernelView: the pass stays a
/// separate module without widening ExplicitFvSolver's friend surface.
struct PressurizedView {
    const NetworkMeshData* mesh = nullptr;
    NetworkStateData* state = nullptr;
    const FvOptions* opts = nullptr;
    const FvStepForcing* forcing = nullptr;

    // Face scratch (owned by ExplicitFvSolver).
    double* f_mass = nullptr;
    double* f_sstar = nullptr;
    kernels::FaceState* f_state_l = nullptr;
    kernels::FaceState* f_state_r = nullptr;
    kernels::FaceFlux* f_flux = nullptr;

    // Cell scratch.
    const double* cell_eta = nullptr;
    double* cell_u = nullptr;

    /// Unsteady-friction convective term c·sgn(Vⁿ)·|∂V/∂x|ⁿ per cell
    /// (issue #156), precomputed by ExplicitFvSolver::computeUfGradients from
    /// the old-state snapshot BEFORE solve() runs. Null/empty when
    /// FvOptions::unsteady_friction == 0.
    const std::vector<double>* uf_grad = nullptr;

    const std::vector<int>* active_faces = nullptr;
    const std::vector<char>* cell_active = nullptr;

    // Node machinery (owned by ExplicitFvSolver).
    const std::vector<double>* node_qstruct = nullptr;
    std::vector<double>* node_carry = nullptr;
    const std::vector<std::uint8_t>* node_alg = nullptr;
    const std::vector<double>* node_vfull = nullptr;
    const std::vector<std::uint8_t>* node_lat_div = nullptr;
    const std::vector<double>* cell_qlat = nullptr;
};

class PressurizedHeadSolver {
public:
    /// Face treatment decided by classify(). Values are load-bearing for the
    /// assembly switch; kNone faces are untouched.
    ///   kFull  — both sides pressurized: Casulli form (steady state reduces
    ///            to ΔH/L = −S_f exactly — the head-loss invariance gate).
    ///   kDelta — a transition face (exactly one side pressurized, with a
    ///            pressurized CELL side). CLASSIFICATION ONLY in R2a: the
    ///            face stays fully explicit — Godunov flux and census bound.
    ///            Folding transitions in was built and MEASURED before being
    ///            cut: every memoryless fold (frozen-ghost delta form, free
    ///            cell as a soft unknown, free-side depth thresholds at 0.05
    ///            and 0.5·y_full) re-times an ADVANCING fill — bore arrival
    ///            65/345 s, 60/545 s, 75/1320 s at c = 150/660 against the
    ///            42 s quasi-steady analytic — because any instantaneous
    ///            threshold is swept through transiently while a cell fills.
    ///            A STANDING transition (backwater below an internal jump)
    ///            is only distinguishable by cadence, so releasing its
    ///            census throttle belongs to the LTS composition (R2b),
    ///            which can put the front on its own fine tier.
    ///   kNone  — everything else: gated/culvert faces (their laws stand),
    ///            closed ends, and faces with no pressurized CELL side (a
    ///            pressurized ghost against a free cell is the classic
    ///            manhole bore).
    enum : std::uint8_t { kNone = 0, kFull = 1, kDelta = 2 };

    /**
     * @brief Rebuild pressure membership, implicit face ownership and union-find connectivity.
     *
     * Resets pressure/fold masks and implicit faces. SLOT membership uses closed cells at/above
     * y_crown; nonempty cell_tpa instead supplies history flags. Only kFull faces are implicit; kDelta
     * remains explicit. An eligible algebraic node folds only when every incident face is full, no
     * fixed head applies, and no real pond demotes it. Builds unknown maps and union-find roots;
     * component CSR is built later during solve. Changes internal scratch only, can allocate/throw, and
     * invalidates previous masks/candidate preparation. Calls on one solver instance require
     * serialization.
     *
     * @param[in] v Borrowed fully aligned mesh/state, active face/cell lists, algebraic-node
     *     mask/full-volume arrays and optional forcing. Required pointers are non-null; referenced
     *     topology indices are valid.
     * @return True if at least one active kFull face exists; false if no active pressure cell or no
     *     eligible full face.
     */
    bool classify(const PressurizedView& v);

    /**
     * @brief Predict fully pressurized face discharges with an implicit head system.
     *
     * Prepares friction/conveyance coefficients, solves each pressure component, and demotes
     * out-of-range folded heads to Dirichlet values for at most four passes per solve. Uses Thomas on
     * paths and bounded Jacobi-CG elsewhere; no convergence status is exposed. Applies up to three
     * storage secant corrections by default (process-cached atoi(OPENSWMM_FV_PRESS_SECANT_PASSES)
     * overrides); this Picard correction need not converge for stiff crown transitions. Publishes
     * f_mass, f_flux.mass/sstar, f_sstar and folded node heads; cell A/Q/volume update remains the
     * caller responsibility. Allocations can throw; no rollback guarantee is supplied. Call
     * finalizeCells after the conservative cell/node update.
     *
     * @param[in,out] v Borrowed aligned view matching the last successful classify, with fresh explicit
     *     face states/fluxes, current old-state heads/areas and node carry/source arrays. All required
     *     pointers and ranges remain live for the call; no view pointer is retained.
     * @param[in] dt Positive substep duration in seconds; <=0 or an empty implicit face set is a no-op.
     */
    void solve(const PressurizedView& v, double dt);

    /**
     * @brief Try a guarded nonlinear SLOT-storage pressure predictor.
     *
     * Requires prior classify against the same state. Uses actual old conserved areas, closed SLOT
     * geometry and zero-storage algebraic nodes; pondable or storage folded nodes and
     * nonpositive/nonfinite conductances are rejected. Only after convergence publishes pressure face
     * ledgers and folded heads. Other state remains caller-owned; failed attempts can change internal
     * candidate scratch but do not publish these ledgers. Allocation exceptions propagate.
     *
     * @param[in,out] v Borrowed aligned view matching the last successful classify, with fresh explicit
     *     face states/fluxes, current old-state heads/areas and node carry/source arrays. All required
     *     pointers and ranges remain live for the call; no view pointer is retained.
     * @param[in] dt Finite positive substep duration in seconds.
     * @param[in] controls Nonlinear residual/iteration/workspace controls passed to
     *     NonlinearPressureStorage.
     * @return Nonlinear diagnostics/status; a default non-converged result for an empty set, invalid
     *     dt, TPA state or unsupported folded-node/face configuration.
     */
    NonlinearPressureStorage::Result
    solveCandidate(const PressurizedView& v, double dt,
                   const NonlinearPressureStorage::Controls& controls);
    /**
     * @brief Check the actual post-update endpoint against the last nonlinear candidate equations.
     *
     * Call only after a successful solveCandidate, real limiter/cell/node update, and matching
     * unchanged topology/classification. Reads published state/fluxes and updates diagnostic scratch;
     * never repairs the endpoint. A candidate-free call can vacuously pass, so this is not an
     * independent solver validity test. Allocation errors propagate.
     *
     * @param[in,out] v Borrowed aligned view matching the last successful classify, with fresh explicit
     *     face states/fluxes, current old-state heads/areas and node carry/source arrays. All required
     *     pointers and ranges remain live for the call; no view pointer is retained.
     * @param[in] dt Same positive substep seconds used for candidate preparation/update.
     * @param[in] controls Residual budgets matching the candidate solve: absolute_volume ft3 and
     *     relative_volume dimensionless.
     * @return True if bounded finite heads, original volume/continuity residuals, folded carry and
     *     every implicit face law meet their volume budgets.
     */
    bool verifyCandidateEndpoint(const PressurizedView& v, double dt,
                                 const NonlinearPressureStorage::Controls& controls);

    /**
     * @brief Replace pressure-cell discharge with its solved face through-flow after conservative
     * update.
     *
     * For classified pressurized cells owning an unknown, transforms the two current face mass fluxes
     * into the cell axis, averages them into state.cell_q and refreshes cell_u=q/A (zero velocity at
     * dry area). Leaves area/depth/node state unchanged. Empty implicit set is a no-op. Run after
     * updateCells/updateNodes, using the same classified geometry and current face flux ledger.
     *
     * @param[in,out] v Borrowed aligned view matching the last successful classify, with fresh explicit
     *     face states/fluxes, current old-state heads/areas and node carry/source arrays. All required
     *     pointers and ranges remain live for the call; no view pointer is retained.
     */
    void finalizeCells(const PressurizedView& v);

    /**
     * @brief Borrow the last classification folded-node mask.
     *
     * Consumers skip their explicit algebraic relaxation. Contents/storage can change on the next
     * classify; the reference is valid only while this solver object lives.
     * @return Const node-indexed mask reference; entries with value one own a pressure continuity row.
     */
    const std::vector<std::uint8_t>& foldedNodes() const noexcept { return node_fold_; }

    /**
     * @brief Report whether the last classification contains any fully implicit face.
     * @return True when imp_faces_ is nonempty; this does not certify solve convergence or endpoint
     *     validity.
     */
    bool anyImplicit() const noexcept { return !imp_faces_.empty(); }

    // -- static predicates shared with the census edit ----------------------

    /**
     * @brief Test pressure membership shared by classification and timestep census.
     *
     * When TPA flags exist they must contain every cell; this predicate does not also check the
     * geometry is closed. No flags or history are changed.
     *
     * @param[in] mesh Borrowed immutable aligned FV topology/geometry; index arrays are unchecked.
     * @param[in] state Borrowed current cell depths/regime flags and node heads aligned to mesh.
     * @param[in] cell Valid nonnegative mesh cell index.
     * @return Nonzero cell_tpa flag when that vector is nonempty; otherwise a closed geometry with
     *     depth>=y_crown.
     */
    static bool cellPressurized(const NetworkMeshData& mesh, const NetworkStateData& state,
                                int cell) {
        const auto uc = static_cast<std::size_t>(cell);
        if (!state.cell_tpa.empty())
            return state.cell_tpa[uc] != 0;
        const FvGeometry& g = mesh.geom[static_cast<std::size_t>(mesh.cell_geom[uc])];
        return !g.is_open && state.cell_h[uc] >= g.y_crown;
    }

    /**
     * @brief Classify one face as explicit, full pressure or a pressure/free transition.
     *
     * Any nonzero gate mask, any culvert assignment, or a closed end excludes implicit treatment
     * regardless of current flux direction. Node pressure comes from ghostPressurized. This predicate
     * ignores active timestep masks; classify applies those separately.
     *
     * @param[in] mesh Borrowed immutable aligned FV topology/geometry; index arrays are unchecked.
     * @param[in] state Borrowed current cell depths/regime flags and node heads aligned to mesh.
     * @param[in] f Valid global face index; all incident cells/nodes must be valid or use the
     *     documented -1 boundary sentinel.
     * @return kFull when both sides are pressure and at least one cell is pressure; kDelta for exactly
     *     one pressure side with a pressure cell; kNone otherwise.
     */
    static std::uint8_t faceModeOf(const NetworkMeshData& mesh, const NetworkStateData& state,
                                   int f) {
        const auto uf = static_cast<std::size_t>(f);
        if (mesh.face_gate[uf] != 0)
            return kNone;
        if (mesh.face_culvert[uf] >= 0)
            return kNone;
        const int cl = mesh.face_cl[uf];
        const int cr = mesh.face_cr[uf];
        const int nd = mesh.face_node[uf];
        if (nd < 0 && (cl < 0 || cr < 0))
            return kNone;

        bool pl = false, pr = false, cell_side_press = false;
        if (cl >= 0) {
            pl = cellPressurized(mesh, state, cl);
            cell_side_press = cell_side_press || pl;
        }
        if (cr >= 0) {
            pr = cellPressurized(mesh, state, cr);
            cell_side_press = cell_side_press || pr;
        }
        if (nd >= 0) {
            const bool gp = ghostPressurized(mesh, state, f);
            if (cl < 0)
                pl = gp;
            if (cr < 0)
                pr = gp;
        }
        if (!cell_side_press)
            return kNone;
        return (pl && pr) ? kFull : kDelta;
    }

    /**
     * @brief Test the node-side ghost pressure state in the adjacent cell geometry.
     *
     * Sealed means positive node_sur_depth, allowing sub-atmospheric TPA pressure. No physical head
     * floor or branch mutation is applied.
     *
     * @param[in] mesh Borrowed immutable aligned FV topology/geometry; index arrays are unchecked.
     * @param[in] state Borrowed current cell depths/regime flags and node heads aligned to mesh.
     * @param[in] f Valid face index; a missing node or adjacent cell returns false.
     * @return False for open geometry; otherwise true for a sealed node adjoining a flagged TPA cell,
     *     or node_head-face_zb>=y_crown.
     */
    static bool ghostPressurized(const NetworkMeshData& mesh, const NetworkStateData& state,
                                 int f) {
        const auto uf = static_cast<std::size_t>(f);
        const int nd = mesh.face_node[uf];
        if (nd < 0)
            return false;
        const int other = (mesh.face_cl[uf] >= 0) ? mesh.face_cl[uf] : mesh.face_cr[uf];
        if (other < 0)
            return false;
        const FvGeometry& g =
            mesh.geom[static_cast<std::size_t>(mesh.cell_geom[static_cast<std::size_t>(other)])];
        if (g.is_open)
            return false;
        // TPA (issue #156 Phase 5): a SEALED node against a flagged interior
        // cell carries the pressurized column at sub-atmospheric head — the
        // ghost is pressurized even though head < crown (the same rule
        // faceSide uses for its ghost regime). Without this the sealed
        // sub-crown junction's faces degrade to kDelta, the fold pass skips
        // the node, and the implicit path breaks the column (the P4 pinned
        // known-issue).
        if (!state.cell_tpa.empty() && state.cell_tpa[static_cast<std::size_t>(other)] != 0 &&
            mesh.node_sur_depth[static_cast<std::size_t>(nd)] > 0.0)
            return true;
        const double hg = state.node_head[static_cast<std::size_t>(nd)] - mesh.face_zb[uf];
        return hg >= g.y_crown;
    }

private:
    struct ImpFace {
        int face = -1;
        std::uint8_t mode = kNone;
        // Unknown index of each side (-1 = known/Dirichlet). Left/right in
        // the FACE frame, so the assembly's sign convention is the flux's.
        int unk_l = -1;
        int unk_r = -1;
        // Dirichlet head of a known side (valid where unk < 0).
        double dir_l = 0.0;
        double dir_r = 0.0;
        // Time-n heads of both sides (the DELTA form needs them).
        double eta_l = 0.0;
        double eta_r = 0.0;
        double alpha = 1.0; ///< 1/(1 + Δt·γ_f)
        double cond = 0.0;  ///< C_f = α·Δt·g·Â_f/L_f
        double qstar = 0.0; ///< predictor (FULL) — face-frame discharge
    };

    // classify() products, persistent to avoid per-substep allocation.
    std::vector<std::uint8_t> cell_press_;
    std::vector<std::uint8_t> node_fold_;
    std::vector<int> cell_unk_; ///< cell → unknown index (-1 none)
    std::vector<int> node_unk_; ///< node → unknown index (-1 none)
    std::vector<ImpFace> imp_faces_;

    // Unknown table: entity id (cell c → c; node n → n_cells + n), time-n
    // head, storage coefficient (T·Δx/Δt for cells, 0 for folded junctions —
    // filled with Δt at solve time), and component id.
    std::vector<int> unk_entity_;
    std::vector<double> unk_eta0_;
    std::vector<double> unk_store_;   ///< T·Δx for cells (divided by Δt in solve)
    std::vector<double> unk_rhs_ext_; ///< explicit-face + source constants (cfs)
    std::vector<int> unk_comp_;
    std::vector<double> unk_head_; ///< solution

    // Union-find over unknowns.
    std::vector<int> uf_parent_;
    /**
     * @brief Find an internal pressure unknown root with path compression.
     *
     * @param[in] i Valid union-find unknown index; parent links must terminate in a self root.
     * @return Root index; parent pointers traversed are compressed in place.
     */
    int ufFind(int i);
    /**
     * @brief Merge two pressure unknown sets using the smaller root for deterministic order.
     *
     * Compresses both paths and connects distinct roots; does not allocate or rebuild component CSR.
     *
     * @param[in] a Valid internal unknown index.
     * @param[in] b Valid internal unknown index.
     */
    void ufUnion(int a, int b);

    // Per-component scratch.
    std::vector<int> comp_ptr_, comp_unks_;
    std::vector<double> thom_a_, thom_b_, thom_c_, thom_d_;
    std::vector<int> path_order_, degree_, adj_ptr_, adj_idx_;
    std::vector<double> adj_c_;
    std::vector<double> cg_r_, cg_p_, cg_ap_, cg_z_;

    /**
     * @brief Build deterministic pressure component membership and conductance adjacency.
     *
     * Requires classify union-find/unknown maps and prepared face conductances. Sorts unknowns by root
     * then index, rebuilds component CSR and symmetric positive-conductance adjacency. Components
     * retain Dirichlet rows; solveComponent excludes demoted edges. Allocates scratch and can leave it
     * partially rebuilt on allocation failure.
     */
    void buildComponents();
    /**
     * @brief Refresh old-state storage/source constants and pressure face coefficients.
     *
     * Requires successful matching classify. Resets unknown heads/demotion flags, captures old heads,
     * T*dx/dt storage, explicit face/lateral/node carry sources, capped conveyance area, friction/UF
     * denominator and pressure conductance. TPA uses constant slot storage/friction at full section; no
     * face ledger or node head is published. Allocations can throw; coefficients become stale after
     * state/topology/forcing changes.
     *
     * @param[in,out] v Borrowed aligned view matching the last successful classify, with fresh explicit
     *     face states/fluxes, current old-state heads/areas and node carry/source arrays. All required
     *     pointers and ranges remain live for the call; no view pointer is retained.
     * @param[in] dt Positive finite substep in seconds, required before divisions.
     */
    void prepare(const PressurizedView& v, double dt);
    /**
     * @brief Assemble the current pressure diagonal/RHS about a common head datum.
     *
     * Requires prepared face/unknown constants and current Dirichlet demotions. Rebuilds diag_/rhs_,
     * folds fixed/demoted neighbors into RHS and adds full-face conductances/base fluxes. Units are
     * volume-rate per head on the diagonal and volume-rate on RHS. Allocation can throw; no external
     * state is changed.
     *
     * @param[in] v Unused compatibility view; assembly reads already prepared member arrays.
     * @param[in] dt Unused compatibility timestep; storage already includes the timestep factor.
     */
    void assembleRhs(const PressurizedView& v, double dt);
    /**
     * @brief Solve one prepared pressure component and store its absolute heads.
     *
     * Requires current diag_/rhs_, adjacency and demotion mask. A singleton divides its positive
     * diagonal or retains the old head. Simple active paths use unchecked Thomas elimination; other
     * graphs use Jacobi-CG for at most 200+2*n active iterations, stopping at squared residual
     * max(1e-24*||rhs||^2,1e-28) or nonpositive pAp. Publishes the last iterate without a convergence
     * flag. Positive-definite, finite coefficients and nonzero pivots are caller/assembly
     * preconditions; scratch allocation can throw.
     *
     * @param[in] comp Valid index into current component CSR.
     * @param[in] v Unused compatibility view; system data is held in members.
     * @param[in] dt Unused compatibility timestep; coefficients already contain dt.
     */
    void solveComponent(int comp, const PressurizedView& v, double dt);
    /**
     * @brief Publish pressure face discharges from the current solved/demoted heads.
     *
     * For positive-conductance implicit faces writes alpha*qstar-cond*(Hright-Hleft) in ft3/s into
     * f_mass and f_flux.mass. Also writes that discharge into both sstar copies to preserve the scalar
     * upwind sign; their stored magnitude is not a physical contact speed here. Nonpositive-conductance
     * faces are left unchanged. Does not update momentum flux, cell state or node heads.
     *
     * @param[in,out] v Borrowed aligned view matching the last successful classify, with fresh explicit
     *     face states/fluxes, current old-state heads/areas and node carry/source arrays. All required
     *     pointers and ranges remain live for the call; no view pointer is retained.
     */
    void backSubstitute(const PressurizedView& v);

    /**
     * @brief Declare a reserved component linear-solve interface.
     *
     * This private declaration currently has no definition or call site; the implemented Thomas/CG path
     * resides in solveComponent. It must not be invoked until an implementation establishes its
     * indexing, output and failure contract.
     *
     * @param[in] unks Intended ordered unknown-index subset.
     * @param[in] diag Intended assembled diagonal coefficients.
     * @param[in] rhs Intended assembled right-hand side.
     */
    void linearSolve(const std::vector<int>& unks, const std::vector<double>& diag,
                     const std::vector<double>& rhs);

    // diag/rhs assembled per solve (indexed by unknown).
    std::vector<double> diag_, rhs_;
    std::vector<std::uint8_t> unk_dirichlet_; ///< demoted rows (2nd pass)
    std::vector<double> unk_dirvalue_;
    NonlinearPressureStorage candidate_;
    std::vector<NonlinearPressureStorage::Row> candidate_rows_;
    std::vector<NonlinearPressureStorage::Edge> candidate_edges_;
    std::vector<double> candidate_head_, candidate_residual_, candidate_budget_;
};

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_PRESSURIZED_HEAD_SOLVER_HPP
