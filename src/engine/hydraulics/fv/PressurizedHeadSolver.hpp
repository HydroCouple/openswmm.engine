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
 * @file PressurizedHeadSolver.hpp
 * @brief Implicit acoustic/slot head update on the pressurized subset
 *        (slot program R2a, Strategy E).
 *
 * @details Above the taper band the Preissmann-slot closure is linear in head
 *          (A = a_crown + t_slot·(h − y_full)), so the acoustic pair — slot
 *          storage T·∂H/∂t against the pressure gradient g·A·∂H/∂x — is a
 *          LINEAR diffusion system in H that can be integrated implicitly
 *          (θ = 1), unconditionally stably. Everything else stays exactly the
 *          explicit Godunov scheme: membership is a pure function of the
 *          instantaneous state (h ≥ y_crown, band entry — no flags, no
 *          memory), and the solve is a FLUX PREDICTOR whose back-substituted
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
 *          Face taxonomy (memoryless, re-derived every substep):
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_PRESSURIZED_HEAD_SOLVER_HPP
#define OPENSWMM_ENGINE_FV_PRESSURIZED_HEAD_SOLVER_HPP

#include <cstdint>
#include <vector>

#include "FvKernels.hpp"
#include "INetworkSolver.hpp"
#include "NetworkMeshData.hpp"

namespace openswmm::fv {

/// Non-owning view of the solver internals the implicit pass reads and
/// writes. Same pattern as transport's SpeciesKernelView: the pass stays a
/// separate module without widening ExplicitFvSolver's friend surface.
struct PressurizedView {
    const NetworkMeshData*  mesh    = nullptr;
    NetworkStateData*       state   = nullptr;
    const FvOptions*        opts    = nullptr;
    const FvStepForcing*    forcing = nullptr;

    // Face scratch (owned by ExplicitFvSolver).
    double*              f_mass    = nullptr;
    double*              f_sstar   = nullptr;
    kernels::FaceState*  f_state_l = nullptr;
    kernels::FaceState*  f_state_r = nullptr;
    kernels::FaceFlux*   f_flux    = nullptr;

    // Cell scratch.
    const double* cell_eta = nullptr;
    double*       cell_u   = nullptr;

    /// Unsteady-friction convective term c·sgn(Vⁿ)·|∂V/∂x|ⁿ per cell
    /// (issue #156), precomputed by ExplicitFvSolver::computeUfGradients from
    /// the old-state snapshot BEFORE solve() runs. Null/empty when
    /// FvOptions::unsteady_friction == 0.
    const std::vector<double>* uf_grad = nullptr;

    const std::vector<int>*  active_faces = nullptr;
    const std::vector<char>* cell_active  = nullptr;

    // Node machinery (owned by ExplicitFvSolver).
    const std::vector<double>*       node_qstruct = nullptr;
    std::vector<double>*             node_carry   = nullptr;
    const std::vector<std::uint8_t>* node_alg     = nullptr;
    const std::vector<double>*       node_vfull   = nullptr;
    const std::vector<std::uint8_t>* node_lat_div = nullptr;
    const std::vector<double>*       cell_qlat    = nullptr;
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

    /// Re-derive the pressurized subset from the instantaneous state:
    /// pressurized cells, implicit faces with their mode, folded junction
    /// rows and the component partition. Returns true when at least one
    /// implicit face exists (i.e. solve() has work).
    bool classify(const PressurizedView& v);

    /// Assemble and solve the SPD head system per component, then overwrite
    /// the implicit faces' entries in f_mass (and the species-facing copies
    /// f_flux.mass / f_sstar) with the back-substituted discharges, and
    /// write the folded junctions' heads (ceiling/floor clamped, with a
    /// one-shot Dirichlet re-solve for violating rows).
    void solve(const PressurizedView& v, double dt);

    /// Post-update pass: a pressurized cell's momentum state is slaved to
    /// the solved face discharges (Q_i = mean of its two face fluxes in the
    /// cell frame) so no explicit acoustic residue survives in the
    /// collocated Q. Call AFTER updateCells/updateNodes.
    void finalizeCells(const PressurizedView& v);

    /// Folded-node mask from the last classify() — relaxNodeFluxes must skip
    /// these (their continuity is a row of the implicit system).
    const std::vector<std::uint8_t>& foldedNodes() const noexcept {
        return node_fold_;
    }

    bool anyImplicit() const noexcept { return !imp_faces_.empty(); }

    // -- static predicates shared with the census edit ----------------------

    /// Is this cell pressurized (band entry and above) under the R2 rule?
    /// Under the TPA closure (issue #156: state.cell_tpa non-empty) membership
    /// IS the regime flag — a latched sub-atmospheric cell sits below y_crown
    /// yet is exactly the stiff-acoustic case this solver exists for (plan
    /// §2.2(1)). The flag is fixed within a substep, so the SPD structure and
    /// the census edit stay consistent.
    static bool cellPressurized(const NetworkMeshData& mesh,
                                const NetworkStateData& state, int cell) {
        const auto uc = static_cast<std::size_t>(cell);
        if (!state.cell_tpa.empty()) return state.cell_tpa[uc] != 0;
        const FvGeometry& g =
            mesh.geom[static_cast<std::size_t>(mesh.cell_geom[uc])];
        return !g.is_open && state.cell_h[uc] >= g.y_crown;
    }

    /// Implicit eligibility and mode of one face, derived from the
    /// instantaneous state alone — the ONE predicate classify() and the
    /// census edit share, so the dt bound and the solve can never disagree
    /// about which faces are covered. Gated and culvert-capped faces are
    /// never implicit (their laws stand, explicitly); a pressurized ghost
    /// against a FREE cell is the filling bore and stays explicit too.
    static std::uint8_t faceModeOf(const NetworkMeshData& mesh,
                                   const NetworkStateData& state, int f) {
        const auto uf = static_cast<std::size_t>(f);
        if (mesh.face_gate[uf] != 0) return kNone;
        if (mesh.face_culvert[uf] >= 0) return kNone;
        const int cl = mesh.face_cl[uf];
        const int cr = mesh.face_cr[uf];
        const int nd = mesh.face_node[uf];
        if (nd < 0 && (cl < 0 || cr < 0)) return kNone;

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
            if (cl < 0) pl = gp;
            if (cr < 0) pr = gp;
        }
        if (!cell_side_press) return kNone;
        return (pl && pr) ? kFull : kDelta;
    }

    /// Is the node-side ghost of face @p f pressurized?
    static bool ghostPressurized(const NetworkMeshData& mesh,
                                 const NetworkStateData& state, int f) {
        const auto uf = static_cast<std::size_t>(f);
        const int nd = mesh.face_node[uf];
        if (nd < 0) return false;
        const int other = (mesh.face_cl[uf] >= 0) ? mesh.face_cl[uf]
                                                  : mesh.face_cr[uf];
        if (other < 0) return false;
        const FvGeometry& g = mesh.geom[static_cast<std::size_t>(
            mesh.cell_geom[static_cast<std::size_t>(other)])];
        if (g.is_open) return false;
        const double hg =
            state.node_head[static_cast<std::size_t>(nd)] - mesh.face_zb[uf];
        return hg >= g.y_crown;
    }

private:
    struct ImpFace {
        int          face  = -1;
        std::uint8_t mode  = kNone;
        // Unknown index of each side (-1 = known/Dirichlet). Left/right in
        // the FACE frame, so the assembly's sign convention is the flux's.
        int  unk_l = -1;
        int  unk_r = -1;
        // Dirichlet head of a known side (valid where unk < 0).
        double dir_l = 0.0;
        double dir_r = 0.0;
        // Time-n heads of both sides (the DELTA form needs them).
        double eta_l = 0.0;
        double eta_r = 0.0;
        double alpha = 1.0;  ///< 1/(1 + Δt·γ_f)
        double cond  = 0.0;  ///< C_f = α·Δt·g·Â_f/L_f
        double qstar = 0.0;  ///< predictor (FULL) — face-frame discharge
    };

    // classify() products, persistent to avoid per-substep allocation.
    std::vector<std::uint8_t> cell_press_;
    std::vector<std::uint8_t> node_fold_;
    std::vector<int>          cell_unk_;   ///< cell → unknown index (-1 none)
    std::vector<int>          node_unk_;   ///< node → unknown index (-1 none)
    std::vector<ImpFace>      imp_faces_;

    // Unknown table: entity id (cell c → c; node n → n_cells + n), time-n
    // head, storage coefficient (T·Δx/Δt for cells, 0 for folded junctions —
    // filled with Δt at solve time), and component id.
    std::vector<int>    unk_entity_;
    std::vector<double> unk_eta0_;
    std::vector<double> unk_store_;  ///< T·Δx for cells (divided by Δt in solve)
    std::vector<double> unk_rhs_ext_;///< explicit-face + source constants (cfs)
    std::vector<int>    unk_comp_;
    std::vector<double> unk_head_;   ///< solution

    // Union-find over unknowns.
    std::vector<int> uf_parent_;
    int ufFind(int i);
    void ufUnion(int a, int b);

    // Per-component scratch.
    std::vector<int>    comp_ptr_, comp_unks_;
    std::vector<double> thom_a_, thom_b_, thom_c_, thom_d_;
    std::vector<int>    path_order_, degree_, adj_ptr_, adj_idx_;
    std::vector<double> adj_c_;
    std::vector<double> cg_r_, cg_p_, cg_ap_, cg_z_;

    void buildComponents();
    void assembleRhs(const PressurizedView& v, double dt);
    void solveComponent(int comp, const PressurizedView& v, double dt);
    void backSubstitute(const PressurizedView& v);

    /// Solve one component given the per-unknown diag/rhs and the edge list;
    /// writes unk_head_. Thomas when the component is a simple path, CG
    /// otherwise.
    void linearSolve(const std::vector<int>& unks,
                     const std::vector<double>& diag,
                     const std::vector<double>& rhs);

    // diag/rhs assembled per solve (indexed by unknown).
    std::vector<double> diag_, rhs_;
    std::vector<std::uint8_t> unk_dirichlet_;  ///< demoted rows (2nd pass)
    std::vector<double>       unk_dirvalue_;
};

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_PRESSURIZED_HEAD_SOLVER_HPP
