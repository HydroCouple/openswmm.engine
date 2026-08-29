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
 * @file FvOptions.hpp
 * @brief Option struct for the explicit finite-volume 1D network solver.
 *
 * @details Mirrors the role SolverOptions2D plays for the 2D marcher: a plain
 *          value type carrying every knob the solver reads, populated from the
 *          `FV_*` keys in `[OPTIONS]` (OptionsHandler.cpp) and stored as an
 *          embedded member of SimulationOptions.
 *
 *          Dependency-free by design (no SimulationContext, no Kokkos) so the
 *          GPU plugin can include it across the plain-C ABI boundary.
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §4.2
 * @ingroup engine_fv
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_OPTIONS_HPP
#define OPENSWMM_ENGINE_FV_OPTIONS_HPP

namespace openswmm::fv {

/// Hard ceiling on LTS tiers. Tier k advances at 2^k·dt₀, so 8 tiers already
/// span a 128× stiffness ratio — beyond that the coarse tier's lag behind a
/// moving front stops being a bounded integration-path difference.
/// (A 14-tier cap was trialled for the MIN_SURFAREA-junction stiffness on
/// macdonald-long-sub and did not help: the stiff set there is every
/// junction, and the scheduler's base-step count is the cost floor. The
/// remedy is the algebraic junction interface, which removes the bound.)
inline constexpr int kMaxLtsTiers = 8;

/// Face flux function. HLLC is the default and HLL exists only as a
/// debugging/comparison baseline — see plan §3.2: the contact wave HLL averages
/// away is exactly the wave that carries an advected scalar, so HLL is not a
/// legitimate production choice once transport is on the same mesh.
enum class RiemannSolver : int {
    HLL  = 0,   ///< baseline / debug only
    HLLC = 1    ///< default
};

/// Slope limiter used by the second-order (MUSCL) reconstruction.
enum class Limiter : int {
    MINMOD   = 0,  ///< most diffusive, most robust (default)
    VANLEER  = 1,
    SUPERBEE = 2   ///< sharpest; can artificially steepen smooth profiles
};

/// Reconstruction used for the advected scalar field.
enum class ScalarScheme : int {
    UPWIND            = 0,  ///< 1st-order upwind — monotone, diffusive
    MUSCL             = 1,  ///< 2nd-order TVD, one-cell stencil (default)
    QUICKEST_ULTIMATE = 2   ///< 3rd-order, two-cell upstream stencil
};

/// Temporal integrator.
enum class TimeIntegration : int {
    EULER = 0,  ///< forward Euler (default)
    RK2   = 1   ///< SSP-RK2 (Heun) — strong-stability-preserving
};

// FV_NODE_COUPLING, FV_NODE_DT and FV_NODE_PICARD were retired 2026-08-29
// (parsed and warned when they ask for the behaviour that no longer exists):
// storage-node coupling is always semi-implicit, the node accuracy bound is
// always armed, and the correction is always a single sweep -- each the
// former default. The SWASHES table that justified FV_NODE_DT was measured
// under the BUCKET junction model described below, three days before that
// model was deleted, and never re-measured
// (plans/FV1D_PERF_PLAN_REVIEW_2026-08-20.md §3.1).

// Junctions are always ALGEBRAIC interfaces, not states (matching legacy
// DYNWAVE, where a junction's own surface area is exactly zero and all
// working area belongs to the conduits — here, the cells): degree-2 nodes
// with no local injection pass fluxes straight through as a direct face, and
// every other junction solves its head from the instantaneous flux balance
// per substep. The earlier BUCKET model (MIN_SURFAREA control volume
// integrated in time, FV_JUNCTION_MODEL option) manufactured a millisecond
// dt bound and a transshipment cap dt ≤ V_node/Q that jammed junction-dense
// channels at the crown; it was removed after the algebraic interface passed
// the open-channel validation suite. See ExplicitFvSolver::solveAlgebraicNode.

/// When pump/orifice/weir/outlet structure equations are re-evaluated.
enum class StructureCoupling : int {
    SUBSTEP      = 0,  ///< every explicit substep (default; physically exact)
    ROUTING_STEP = 1   ///< once per routing step (matches DW-scale control cadence)
};

/// Backend selection for the solver kernels.
enum class Backend : int {
    CPU  = 0,
    AUTO = 1,   ///< default: try plugins above the parallel gate, else CPU
    OMP  = 2,
    CUDA = 3,
    HIP  = 4,
    SYCL = 5
};

/**
 * @brief Knobs for `FLOW_ROUTING FV`.
 *
 * Length-dimensioned members (`cell_length`, `slot_celerity`) are stored in
 * INTERNAL units (feet, ft/s) — OptionsHandler converts from the project's
 * display units on parse, exactly as HEAD_TOLERANCE is handled for DW.
 */
struct FvOptions {
    // -- Mesh (plan §3.2) ---------------------------------------------------

    /// Target Δx. 0 ⇒ no length target, and each conduit gets `min_cells`.
    /// > 0 ⇒ each conduit is cut into max(min_cells, ceil(L/cell_length))
    /// cells. Internal units (ft).
    double cell_length = 0.0;

    /// Floor on cells per conduit. A FLOOR, not a FINE-mode detail: it applies
    /// with or without a `cell_length` target.
    ///
    /// Four, not one. A conduit meshed as a single cell has no interior
    /// gradient of its own and presents an artificial bed step of half its fall
    /// at every manhole (§8.3), so it under-conveys and backs water up.
    /// Measured on Example1 — mean absolute peak-flow deviation from DYNWAVE,
    /// and wall-clock relative to one cell:
    ///
    ///     cells      1       2       4       8
    ///     deviation  37.1 %  25.7 %  15.3 %   7.6 %
    ///     worst     -75.8 % -58.1 % -43.2 % -22.6 %
    ///     time       1.0x    1.4x    2.2x    5.4x
    ///
    /// Four is the knee: it more than halves the one-cell error for about twice
    /// the cost, and convergence past it is slower than its price. One cell was
    /// the default and should not have been — it is a known-wrong setting, not
    /// a cheap-but-approximate one. Raise this (or set cell_length) where peak
    /// flows or in-conduit profiles matter.
    int min_cells = 4;

    // -- Scheme -------------------------------------------------------------

    double        cfl              = 0.5;
    RiemannSolver riemann          = RiemannSolver::HLLC;
    int           order            = 1;                    ///< 1 | 2 (MUSCL-Hancock)
    Limiter       limiter          = Limiter::MINMOD;
    ScalarScheme  scalar_scheme    = ScalarScheme::MUSCL;
    TimeIntegration time_integration = TimeIntegration::EULER;

    /// Pressurized wave speed (ft/s internal). Sets the Preissmann slot top
    /// width through T_slot = g·A_full/c², so it is a direct accuracy/cost dial:
    /// physical acoustic speeds would crush the global CFL step. Default 100
    /// ft/s is the same order DW's SLOT surcharge method produces.
    double slot_celerity = 100.0;

    /// Integrate the acoustic/slot pair implicitly on the pressurized subset
    /// (slot program R2a, Strategy E). Above the taper band the closure is
    /// linear in head, so cells at/above band entry solve an SPD head system
    /// per substep (Thomas on chains, Jacobi-CG past folded junctions) whose
    /// back-substituted face discharges overwrite `f_mass_` — a pure flux
    /// predictor: conservation, rollback and hot start are untouched, and a
    /// run that never pressurizes is bit-identical with the option on.
    ///
    /// With it, pressurized cells are advection-bound in the step census —
    /// FV_SLOT_CELERITY leaves the dt law entirely, making the slot width a
    /// pure accuracy parameter (a narrow slot no longer costs runtime).
    /// Default OFF while the R2 gates land; the R4 default flip is a
    /// separate, deliberate commit.
    bool pressurized_implicit = false;

    // -- Coupling -----------------------------------------------------------

    StructureCoupling structure_coupling = StructureCoupling::SUBSTEP;

    // -- Execution ----------------------------------------------------------

    Backend backend = Backend::AUTO;

    /// Small-problem gate: below this cell count AUTO stays on the CPU because
    /// per-kernel launch overhead dominates. Carried over from the 2D marcher's
    /// measured recalibration (plan §2 guardrails).
    long min_parallel_cells = 20000;

    // -- Performance levers (plan §5.2) -------------------------------------

    /// Dry/inactive work-list compaction. Results-transparent by contract: a
    /// compacted run must reproduce the non-compacted run bit-for-bit on the
    /// same backend (§6.10).
    bool compaction = true;

    /// Local time stepping — stiff cells (short Δx, pressurized) substep at
    /// their own dt while the rest advance at the macro step. When tiering
    /// finds nothing to separate the solver falls through to the global-dt
    /// path bit-for-bit, so this is on by default (plan §3.3).
    bool lts = true;

    /// Bound the explicit step by the ALGEBRAIC JUNCTION feedback limit.
    /// Opt-in: correct but costly (see the wall-time note below).
    ///
    /// An algebraic junction has no volume state, so it carries no storage
    /// bound and was exempted from the step census on both paths. But its head
    /// is solved from the instantaneous flux balance and then handed to the
    /// incident cells as a ghost, so the neighbours integrate against a
    /// boundary state that can travel a long way inside one step. That feedback
    /// is explicit and has its own limit (algebraicNodeStableDt):
    ///
    ///     tau = min_i(0.5*dx_i*T_i) / sum_j(T_j*c_j)
    ///
    /// `min` in the numerator, not `sum`: every incident conduit pushes flux
    /// (the sum), but the solved head must resolve on the TIGHTEST incident
    /// storage. Summing -- what legacy DW does for its own node continuity --
    /// makes the bound LOOSER at exactly the junctions that need it (a 12 ft
    /// barrel meeting a 3 ft one gives a 2123 ft effective length against a
    /// 250 ft cell). Gated on the junction being pressurized, so open-channel
    /// networks pay nothing.
    ///
    /// Relative to the cell bound this is 1/(2n) * (T_min/T_max) -- purely
    /// geometric: 4x for two equal barrels, 32x across a 16:1 area step. Those
    /// are the factors the EPA QA decks were measured to need, and they are
    /// CELERITY-INVARIANT (T ~ 1/c^2 cancels), which is why lowering
    /// FV_SLOT_CELERITY suppresses the symptom without fixing the bound.
    ///
    /// The constraint is applied to the junction's INCIDENT CELLS in
    /// assignTiers, not to dt_node: a plain junction integrates no state and is
    /// pinned to its finest incident cell, so routing it through dt_node
    /// discards the tier while still dragging dt0 down. On the cells it is an
    /// ordinary local CFL number and tiering localises it normally.
    ///
    /// Measured at stock FV_CFL 0.5 with LTS on (zigzag = total variation /
    /// range of the worst link, against dynamic wave):
    ///   test5  37.95 -> 4.97  (DW 5.08), peak Q 1.06x -> 1.01x,  8.5 s ->  121 s
    ///   test2  52.01 -> 2.99  (DW 6.87), peak Q 11.9x -> 0.99x,  1.0 s -> 9.7 s
    /// It also closes an accuracy hole unrelated to oscillation: on the culvert
    /// fixture the un-tiered path floods 0.885 acre-ft against DW's 0.003, and
    /// 0.001 with this on.
    ///
    /// Default OFF because of that 10-14x wall cost on pressurized networks.
    /// The cost is the bound doing its job -- a stiff junction genuinely needs
    /// a small step -- but it is the user's call whether to pay it. It does NOT
    /// disable tiering (an earlier revision did): LTS still localises the cost,
    /// and turning LTS off instead costs 376 s / 38 s on the same decks.
    bool node_feedback_dt = false;

    /// Maximum LTS tier count (tier k advances at 2^k·dt₀). 6 ⇒ 64× spread.
    int lts_max_tiers = 6;

    /// Recompute the global CFL min-reduction every k substeps instead of every
    /// substep. 1 = every substep (exact).
    ///
    /// INERT BEFORE 2026-08-20. The accepted-substep tail wrote the step it had
    /// just taken into the census cache and forced the countdown to zero, so
    /// the pre-step census ran every substep for every value of k. The step
    /// taken and the Courant bound are now separate quantities (`dt_cache_` vs
    /// `dt_census_`) and the countdown survives; a retry — the post-step census
    /// proving the bound inadmissible — still resets it. The default of 1 is
    /// bit-identical across the change.
    ///
    /// NOT YET SWEPT. k > 1 throttles only the PRE-step census; the post-step
    /// acceptance census still runs every substep and is the safety net that
    /// makes skipping the pre-step one defensible at all. The ceiling on this
    /// option is therefore ~2× on census cost, not k×. Measure before raising
    /// the default (Phase 1, FV1D_PERF_PLAN_REVISED_2026-08-20.md).
    int cfl_census_interval = 1;

    // -- Transport (plan §3.2 / §6.11) --------------------------------------

    /// Longitudinal dispersion coefficient (ft²/s). 0 disables the parabolic
    /// term entirely (advection only). Treated implicitly per D-FV1 so the
    /// Δx²/(2·D_L) constraint never binds.
    double dispersion = 0.0;
};

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_OPTIONS_HPP
