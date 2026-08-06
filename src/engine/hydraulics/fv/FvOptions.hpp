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

    /// Target Δx. 0 ⇒ COARSE mode: one cell per conduit, element count equal
    /// to DW's. > 0 ⇒ FINE mode: each conduit is cut into
    /// max(min_cells, ceil(L/cell_length)) cells. Internal units (ft).
    double cell_length = 0.0;

    /// Floor on cells per conduit in FINE mode. Guards a short pipe against
    /// degenerating to a single cell wedged between two boundary faces.
    int min_cells = 1;

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

    // -- Transport (plan §3.2 / §6.11) --------------------------------------

    /// Longitudinal dispersion coefficient (ft²/s). 0 disables the parabolic
    /// term entirely (advection only). Treated implicitly per D-FV1 so the
    /// Δx²/(2·D_L) constraint never binds.
    double dispersion = 0.0;
};

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_OPTIONS_HPP
