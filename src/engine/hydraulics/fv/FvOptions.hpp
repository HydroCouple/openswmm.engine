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
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_FV_OPTIONS_HPP
#define OPENSWMM_ENGINE_FV_OPTIONS_HPP

namespace openswmm::fv {

/// Hard ceiling on LTS tiers. Tier k advances at 2^k·dt₀, so 8 tiers already
/// span a 128× stiffness ratio — beyond that the coarse tier's lag behind a
/// moving front stops being a bounded integration-path difference.
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

/// How a node's continuity equation is coupled to the faces that feed it.
enum class NodeCoupling : int {
    EXPLICIT      = 0,  ///< face fluxes frozen across the node update
    SEMI_IMPLICIT = 1   ///< default: fluxes linearized in the node head
};

/// What a node contributes to the LTS base timestep.
///
/// `dt0` is the minimum over every cell AND every node, and the node's term is
/// `A_s / (sum T*c)` — the correct EXPLICIT stability bound for a control
/// volume of surface area A_s. But the node is not updated explicitly: the
/// semi-implicit correction carries the step in its denominator,
/// `dh = dt*R / (A_s + dt*resist)`, which is unconditionally stable. So the
/// term is not buying stability; it is acting as an accuracy limiter nobody
/// designed as one, and a manhole's plan area is one to two orders of
/// magnitude below a conduit cell's, so it sets `dt0` for the whole network.
///
/// Measured on bump-subcritical: STABILITY gives 3428 substeps/step and an L1
/// depth error of 0.1045; NONE with three node sweeps gives 50 substeps and
/// 0.00159 — 9x faster and 66x more accurate, because marching a first-order
/// tangent 3428 times accumulates more bias than 50 steps of a nearly
/// converged solve. It is NOT a universal win: bump-shock and thacker-planar
/// are ~1.2-1.4x worse without the term, which is why STABILITY remains the
/// default and this exists to make the comparison runnable across the suite.
enum class NodeDtLimit : int {
    STABILITY = 0,  ///< default: the explicit A_s/(sum T*c) bound (today)
    NONE      = 1   ///< nodes do not constrain dt0; accuracy rests on sweeps
};

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

    // -- Coupling -----------------------------------------------------------

    StructureCoupling structure_coupling = StructureCoupling::SUBSTEP;

    /// A junction's storage area is the MIN_SURFAREA floor, which as an
    /// effective length A_s/T is a few feet against a conduit Δx of several
    /// hundred — so under explicit coupling the MANHOLE, not the pipe, sets the
    /// substep for the whole model. Linearizing each coupling face's flux in
    /// the node head removes the node from the explicit stability limit.
    /// Conservation is unaffected: the corrected flux is what BOTH the node and
    /// the cell see.
    NodeCoupling node_coupling = NodeCoupling::SEMI_IMPLICIT;

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

    /// Maximum LTS tier count (tier k advances at 2^k·dt₀). 6 ⇒ 64× spread.
    int lts_max_tiers = 6;

    /// Recompute the global CFL min-reduction every k substeps instead of every
    /// substep, with a safety margin. 1 = every substep (exact).
    int cfl_census_interval = 1;

    /// Whether a node's explicit stability estimate enters the LTS base step.
    NodeDtLimit node_dt_limit = NodeDtLimit::STABILITY;

    /// Picard sweeps on the node head inside the semi-implicit coupling.
    ///
    /// 1 (default) is the original behaviour: ONE correction, with the
    /// characteristic resistance √(g·A·T), the node surface area and the face
    /// fluxes all frozen at the head the substep started from. That is exact
    /// for the linearized problem — the residual after one sweep is
    /// identically zero — so more sweeps only help because the real problem is
    /// not linear: A, T and the storage area all move with the head, and the
    /// Riemann flux is not the straight line the correction extrapolates
    /// along. Freezing them is why the node is stable at a large Δt but not
    /// ACCURATE at one, which is what forces it into a fine LTS tier and sets
    /// the substep count (plan §7B.8).
    ///
    /// > 1 re-evaluates all three at the provisional head each sweep and
    /// re-solves the incident faces' Riemann problems, so the node converges
    /// on its own continuity equation instead of a tangent to it.
    /// Conservation is untouched either way: every sweep leaves the answer in
    /// `f_mass_`, the single array both the cell update and the node update
    /// read.
    int node_picard_sweeps = 1;

    /// Stop sweeping when the head correction falls below this (ft).
    double node_picard_tol = 1.0e-6;

    // -- Transport (plan §3.2 / §6.11) --------------------------------------

    /// Longitudinal dispersion coefficient (ft²/s). 0 disables the parabolic
    /// term entirely (advection only). Treated implicitly per D-FV1 so the
    /// Δx²/(2·D_L) constraint never binds.
    double dispersion = 0.0;
};

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_OPTIONS_HPP
