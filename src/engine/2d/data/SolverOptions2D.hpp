/**
 * @file SolverOptions2D.hpp
 * @brief Configuration options for the 2D surface routing solver.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §2.3
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP
#define OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP

#include <cstdint>
#include <string>

namespace openswmm::twoD {

/**
 * @brief Volume → free-surface closure for a 2D cell.
 *
 * FLAT (default, legacy) reconstructs η = tri_cz + V/A — exact only for a
 * fully wetted cell. On a partially wet (slope/step-spanning) cell it
 * overstates η by up to two-thirds of the cell relief, which is the driver of
 * the water-climbs-uphill artifact (spurious head pushes thin films upslope;
 * lake-at-rest is not a steady state at shorelines).
 *
 * VFR reconstructs η from the exact stage–storage relation of the plane bed
 * through the cell's three vertex elevations (Begnudelli & Sanders 2006/2007
 * volume/free-surface relationships), C¹-regularized by a
 * wetted-area-fraction floor (VFR_MIN_WET_FRAC). Restores the C-property
 * at shorelines. CPU solver only; the Kokkos GPU backends degrade to FLAT
 * with a one-line notice until ported.
 *
 * Parsed from [2D_OPTIONS] CELL_CLOSURE (FLAT|VFR).
 * See plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
 */
enum class CellClosure2D : int8_t {
    FLAT = 0,   ///< Legacy flat-cell closure η = tri_cz + V/A (default).
    VFR  = 1    ///< Planar-bed VFR closure (regularized), CPU solvers only.
};

/**
 * @brief Effective conveyance depth at a shared edge for the diffusive-wave flux.
 *
 * MEAN (default, legacy) uses the upwind cell's MEAN depth V/A — blind to
 * where the waterline sits relative to the edge, so a cell with water pooled
 * in its low corner can discharge across an edge whose bed is entirely above
 * the waterline (uphill creep), and drainage strands water on slopes.
 *
 * VFR_FACE reconstructs the depth at the edge from the upwind free surface
 * and the edge's two endpoint bed elevations (Begnudelli & Sanders 2007,
 * Eq. 14, adapted as the Manning conveyance depth): zero when the upwind
 * surface is below the whole edge (no flow — the wetting gate), the exact
 * partially-submerged mean when the waterline crosses the edge. C¹ in η.
 *
 * Parsed from [2D_OPTIONS] FACE_RECONSTRUCTION (MEAN|VFR_FACE).
 * See plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
 */
enum class FaceDepth2D : int8_t {
    MEAN     = 0,   ///< Legacy: upwind cell-mean depth (default).
    VFR_FACE = 1    ///< B&S Eq. 14 face depth + wetting gate.
};

/**
 * @brief How raingage rainfall is mapped onto the 2D mesh cells.
 *
 * NATURAL_NEIGHBOUR (default) spatially interpolates the located raingages onto
 * every cell centroid — natural-neighbour (Laplace) weights inside the convex
 * hull of the gages, inverse-distance (power 2) extrapolation outside it. The
 * weights are precomputed once in SurfaceRouter2D::initialize() (gage positions
 * are static for a run) and applied each step as a sparse weighted sum.
 *
 * SYSTEM applies one uniform value to all cells: the arithmetic mean of every
 * gage's current rainfall. It is also the automatic fallback when no gage has a
 * map location (no [SYMBOLS] coordinate), since interpolation is then undefined.
 *
 * Parsed from [2D_OPTIONS] RAINFALL_MODE; env OPENSWMM_2D_RAINFALL_MODE
 * (natural|system) overrides at initialize().
 */
enum class RainfallMode : int8_t {
    NATURAL_NEIGHBOUR = 0,  ///< Default: spatial interpolation across all gages.
    SYSTEM            = 1,  ///< Uniform = mean of all gages.
    NONE              = 2   ///< No rain on the mesh. Use when subcatchments
                            ///< already capture the rainfall (runoff → nodes) —
                            ///< rain-on-mesh would double-count the same storm.
};

/**
 * @brief Configuration for the 2D surface routing solver.
 *
 * Populated from [2D_OPTIONS] input section. Defaults are chosen for
 * typical urban drainage surface routing problems.
 */
struct SolverOptions2D {
    /// Max marcher step (s): caps film-cell CFL steps (and thus the LTS tier
    /// spread) and the co-advance sync-batch span.
    double max_timestep      = 10.0;
    double dry_depth         = 0.001;   ///< Dry cell threshold (m)
    double limiter_epsilon   = 1.0e-6;  ///< Slope limiter epsilon
    /// Head-difference regularization (m) for the diffusive-wave flux √|Δη|.
    /// Below this gradient the flux is linearized (C¹) so the transmissivity
    /// stays bounded as the water surface flattens — without it, deep near-level
    /// ponding (e.g. a large design storm draining) makes the flux Jacobian blow
    /// up and the implicit step collapse. Only affects millimeter-scale
    /// gradients, so bulk flow is preserved; raise it for extra robustness on
    /// very deep problems. Default 4 mm; 0 = bare √. Parsed from
    /// [2D_OPTIONS] FLUX_DH_EPS; env OPENSWMM_2D_FLUX_DH_EPS overrides.
    double flux_dh_eps       = 0.004;   ///< Diffusive-flux gradient floor (m)
    /// [2D_OPTIONS] COUPLING_SYNC (s): 1D↔2D co-advance sync-batch span.
    /// 0 (default) couples every routing step — exchange volumes reach the
    /// 1D with at most one routing step of lag, which keeps fill-and-spill
    /// coupling (weir/culvert ponds) free of batch-delay ringing. > 0
    /// batches the 2D advance over ~SPAN seconds (clamped to
    /// [routing_step, 60]) — the wall-clock lever for large meshes where
    /// per-step advances degenerate to the global-dt tail; expect the
    /// held-exchange error to grow with the span.
    double coupling_sync     = 0.0;

    double coupling_cd       = 0.65;    ///< Default discharge coefficient
    bool   report_2d         = true;    ///< Write 2D results to output

    // Rainfall→mesh mapping. Default NATURAL_NEIGHBOUR (spatial interpolation
    // across all located gages); SYSTEM applies the uniform all-gage mean.
    // Parsed from [2D_OPTIONS] RAINFALL_MODE; env OPENSWMM_2D_RAINFALL_MODE
    // (natural|system) overrides.
    RainfallMode       rainfall_mode   = RainfallMode::NATURAL_NEIGHBOUR;

    // Volume → free-surface cell closure. Default FLAT (legacy η = tri_cz + V/A):
    // fast and the right choice for typical deep-water urban flooding, where a
    // partially wet cell is the exception. VFR (planar-bed volume/free-surface,
    // fully implemented on all backends) restores the C-property at shorelines
    // and removes the water-climbs-uphill artifact, but resolves the shoreline
    // wetting/drying FLAT freezes out — ~3–8× more marcher substeps — so it is
    // OPT-IN (best on shallow water / gentle slopes). Parsed from [2D_OPTIONS]
    // CELL_CLOSURE (FLAT|VFR). See plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
    CellClosure2D      cell_closure    = CellClosure2D::FLAT;

    // Effective conveyance depth at shared edges. Default MEAN (legacy upwind
    // cell-mean depth). VFR_FACE (B&S Eq. 14 face depth + wetting gate) pairs
    // with CELL_CLOSURE=VFR to complete the artifact fix; opt-in for the same
    // reason. Parsed from [2D_OPTIONS] FACE_RECONSTRUCTION (MEAN|VFR_FACE).
    FaceDepth2D        face_reconstruction = FaceDepth2D::MEAN;

    /// Wetted-area-fraction floor ε of the regularized VFR closure: below wet
    /// fraction ε the η(V) relation continues linearly (slope 1/(εA)), bounding
    /// dη/dV for the implicit solvers' Newton/Jacobian path. Exact elsewhere.
    /// Only used when CELL_CLOSURE = VFR. Parsed from [2D_OPTIONS]
    /// VFR_MIN_WET_FRAC; valid range (0, 0.5].
    double             vfr_min_wet_frac = 0.01;

    // -----------------------------------------------------------------------
    // Explicit local-inertial marcher (INTEGRATOR EXPLICIT — the only 2D
    // integrator since the D2 CVODE/ARKODE retirement) options. Defaults per
    // the 2026-07-29 reimplementation plan.
    // -----------------------------------------------------------------------
    /// Face-update θ weighting (de Almeida & Bates 2013): 1 = pure Bates 2010
    /// (no numerical diffusion), <1 blends the Perot-reconstructed neighbour
    /// discharge to damp thin-film checkerboarding on steep faces.
    double theta        = 0.8;    ///< [2D_OPTIONS] THETA, (0, 1]
    double cfl_number   = 0.7;    ///< [2D_OPTIONS] CFL_NUMBER — α in dt = α·L/√(gh)
    /// Flux-activation depth (m): cells below it are source-only (lazy rain
    /// accumulation, no face flux). Hysteresis band ±1 mm around it.
    double h_move       = 0.003;  ///< [2D_OPTIONS] H_MOVE (m)
    int    lts_tiers    = 4;      ///< [2D_OPTIONS] LTS_TIERS, 1..8 (1 = global dt)
    double froude_max   = 1.5;    ///< [2D_OPTIONS] FROUDE_MAX face |u| clamp
    /// Positivity/exchange availability fraction β: max share of a cell's
    /// volume that outgoing fluxes (or a coupling drain) may take per own-step.
    double exchange_beta  = 0.8;
    /// Optional EMA sub-relaxation of per-substep coupling exchange (1 = off).
    double exchange_relax = 1.0;
    /// [2D_OPTIONS] COUPLING_AREA AUTO: derive exchange area at coupling-point
    /// resolve from the largest connected conduit (clamp(1.25·A_conduit,
    /// 0.05, 2.0) m²) for rows that did not author an explicit area.
    bool   coupling_area_auto = false;

    /// Path from [2D_MESH_FILE] FILE token. Empty = mesh is inline in main .inp.
    std::string mesh_file;

    /// HDF5 output file path from [2D_OPTIONS] OUTPUT_FILE token. Empty =
    /// no 2D output is written. Resolved relative to the parent .inp directory
    /// by the section handler.
    std::string output_file;

    // -----------------------------------------------------------------------
    // Unit-system coupling factors — NOT parsed from input. Computed once in
    // SurfaceRouter2D::initialize() from the project FLOW_UNITS.
    //
    // The 2D solver runs internally in SI (metres, m², m³, m³/s, g=9.80665).
    // The 1D SWMM engine ALWAYS computes internally in FEET (g=32.2, PHI=1.486)
    // — for EVERY project, US or SI: its reader converts metric inputs to feet
    // on load and only converts back at the display/output boundary. So these
    // coupling factors are ALWAYS the feet⇄metres conversion, independent of
    // FLOW_UNITS. SurfaceRouter2D::initialize() overwrites the 1.0 defaults
    // with the real ft⇄m factors; the defaults only stand when 2D is inactive
    // (no coupling occurs). The MESH scaling factor is separate and IS driven
    // by FLOW_UNITS (the mesh is authored in project units) — see initialize().
    // -----------------------------------------------------------------------
    double len_1d_to_2d  = 1.0;  ///< 1D length → 2D length (ft→m, 0.3048)
    double len_2d_to_1d  = 1.0;  ///< 2D length → 1D length (m→ft, 3.2808)
    double vol_1d_to_2d  = 1.0;  ///< 1D volume → 2D volume (ft³→m³, 0.02832)
    double flow_1d_to_2d = 1.0;  ///< 1D flow → 2D flow (ft³/s→m³/s, 0.02832)
    double flow_2d_to_1d = 1.0;  ///< 2D flow → 1D flow (m³/s→ft³/s, 35.315)

    /*! Runtime-only: resolved OpenMP thread count for the embarrassingly-
     *  parallel 2D per-cell / per-vertex loops (RHS pipeline, Jacobi
     *  preconditioner, post-step diagnostics). Set in
     *  SurfaceRouter2D::initialize() from SimulationOptions::num_threads (the
     *  global THREADS option) using the same min(N,max) + size-gate
     *  DWSolver::setNumThreads applies. 1 = serial. The parallelised loops use
     *  schedule(static) and write only their own cell/vertex slot, so any
     *  thread count is bit-identical to serial. Never parsed/persisted. */
    int num_threads = 1;

    /*! When true, the inline `.inp` or referenced `.2dm` declared
     *  `;; UNITS: SI (m)` (or an equivalent metric keyword). The mesh on
     *  disk is already in SI metres, so SurfaceRouter2D::initialize
     *  SKIPS the FLOW_UNITS-based mesh scaling (vx/vy/vz and the
     *  coupling areas).  The 1D⇄2D coupling factors (len_1d_to_2d,
     *  vol_1d_to_2d, flow_*) are unaffected — they are always the
     *  feet⇄metres conversion (the 1D side is always feet), not the mesh
     *  scaling. */
    bool mesh_units_si = false;

    /*! Runtime-only: true after SurfaceRouter2D::initialize() applied the
     *  FLOW_UNITS ft→m in-place mesh scaling (vx/vy/vz, coupling areas).
     *  Lets serialization (InpWriter, GeoPackage) un-scale back to the
     *  authored units, and makes a repeated initialize() idempotent
     *  against double-scaling. Never parsed from input, never persisted. */
    bool mesh_scaled_to_si = false;

    /*! Runtime-only: true after SurfaceRouter2D::initialize() drained the
     *  pending [2D_BOUNDARY_CONDITIONS] / [2D_EDGE_CONVEYANCE] rows into
     *  BoundaryData / MeshData::edge_conveyance. Serialization collectors
     *  (Serialize2D.hpp) switch to the drained arrays once this is set —
     *  they are the live state that post-initialize API mutations edit;
     *  the retained pending rows would be stale. Never parsed/persisted. */
    bool pending_rows_drained = false;

    // ------------------------------------------------------------------------
    // Surface uncertainty ROM ([2D_ROM] / [UNCERTAINTY] LAYER 2D)
    // ------------------------------------------------------------------------
    // The ROM is a read-only observer: it runs an M-member deviation-form
    // ensemble alongside the deterministic marcher and never writes mesh or
    // state. With enable_rom = false nothing is allocated and the deterministic
    // path is untouched — the invariant the bit-identical on/off test guards.

    bool enable_rom      = false;  ///< Master switch for the 2D ROM.
    int  rom_members     = 50;     ///< Ensemble size M.
    int  rom_modes       = 20;     ///< Eigenmodes k retained from MeshEigenBasis.

    double rom_mannings_pert = 0.20; ///< Manning's n half-range: n ∈ [1±p]·base.
    double rom_rainfall_pert = 0.20; ///< Rainfall half-range: r ∈ [1±p]·base.
    double rom_cd_pert       = 0.00; ///< Coupling Cd half-range; 0 = disabled.

    /*! Deviation decay coefficient K_eff [m^(4/3)/s]. 0 (default) = AUTO:
     *  SurfaceRouter2D derives it each advance from mesh Manning/slope and the
     *  current depth field. A positive value pins it, which is what the unit
     *  tests and the MC-calibration harness use to hold the operator fixed. */
    double rom_k_eff = 0.0;

    /*! Correlation lengths (m) for spatially-varying parameter perturbation.
     *  0 = scalar (one multiplier per member, uniform over the mesh); > 0 draws
     *  a correlated random field at that length scale instead. */
    double rom_mannings_corr_len = 0.0;
    double rom_rainfall_corr_len = 0.0;

    /*! Drop mode j from the advance when its ensemble energy and rainfall
     *  forcing are both below this — the modes that carry no spread cost
     *  nothing to skip. */
    double rom_mode_drop_threshold = 1.0e-10;

    /*! Re-seed when the wet-cell count has moved by more than this fraction of
     *  the mesh since the last seed, no more often than every
     *  rom_wet_reseed_min_interval seconds. This exists for BASIS COVERAGE, not
     *  for drift: the depth-weighted eigenmodes are fitted to the wet domain, so
     *  a domain that has grown substantially is no longer well represented by
     *  them. The deviation form removed any need to re-seed for drift — the
     *  median tracks the deterministic run by construction. Re-seeding does zero
     *  the accumulated deviations, so keep the fraction high enough that it fires
     *  on genuine domain growth only. 0 disables. */
    double rom_wet_reseed_fraction     = 0.25;
    double rom_wet_reseed_min_interval = 600.0;  ///< Seconds; 0 = no throttle.

    /*! Fit a log-normal upper tail to the wet sub-population for q95 instead of
     *  taking the sort-based order statistic. Helps when M is small enough that
     *  the empirical 95th percentile is noisy. q05/q50 stay sort-based. */
    bool rom_parametric_tails = false;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP
