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
 * @file      SolverOptions2D.hpp
 * @brief     Configuration options for the 2D surface routing solver.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §2.3
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP
#define OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include "../../core/FilePathPair.hpp"

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
    FLAT = 0, ///< Legacy flat-cell closure η = tri_cz + V/A (default).
    VFR = 1   ///< Planar-bed VFR closure (regularized), CPU solvers only.
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
 * Scope per solver path: BOUNDARY edges honour the mode in
 * SurfaceFluxCalculator::boundaryEdgeFlux. Under the explicit local-inertial
 * marcher, VFR_FACE also governs INTERIOR faces: the face flow depth becomes
 * faceFlowDepthVfr(η_L, η_R, ze_lo, ze_hi) — the Eq. 14 wetted-edge depth of
 * the driving surface over the shared edge's TRUE endpoint beds — so thin
 * crests (embankments/levees/road crowns resolved as lines of high vertices)
 * block until the water genuinely reaches the crest instead of the
 * centroid-diluted zface (≈ ⅓-height early overtopping). MEAN keeps the
 * legacy centroid zface bit-identical on interior faces.
 *
 * Parsed from [2D_OPTIONS] FACE_RECONSTRUCTION (MEAN|VFR_FACE).
 * See plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
 */
enum class FaceDepth2D : int8_t {
    MEAN = 0,    ///< Legacy: upwind cell-mean depth (default).
    VFR_FACE = 1 ///< B&S Eq. 14 face depth + wetting gate.
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
 * (natural|nearest|system|none) overrides at initialize().
 */
enum class RainfallMode : int8_t {
    NATURAL_NEIGHBOUR = 0, ///< Default: spatial interpolation across all gages.
    SYSTEM = 1,            ///< Uniform = mean of all gages.
    NONE = 2,              ///< No rain on the mesh. Use when subcatchments
                           ///< already capture the rainfall (runoff → nodes) —
                           ///< rain-on-mesh would double-count the same storm.
    NEAREST_NEIGHBOUR = 3  ///< Closest located gage; ties use first gage order.
};

/**
 * @brief 1D↔2D exchange law at junction coupling points ([2D_OPTIONS]
 *        COUPLING_LAW; a node's [2D_NODE_COUPLING] LAW overrides it).
 *
 * ORIFICE is the legacy two-way orifice on the map row's CD/AREA (with the
 * rim gate and the AREA→2·AREA ramp). WEIR_ORIFICE is the regime-aware law
 * of coupling/ExchangeLaw.hpp on the node's structure geometry.
 */
enum class CouplingLaw2D : int8_t { ORIFICE = 0, WEIR_ORIFICE = 1 };

/**
 * @brief Where the 2D head driving a junction exchange is read
 *        ([2D_OPTIONS] COUPLING_HEAD).
 *
 * CELL (default) is the coupling cell (the lowest-bed cell around a vertex
 * point). VERTEX is the wet-depth-weighted η over the cells around the mapped
 * vertex. STENCIL_WET_MEAN is the wet-depth-weighted η over the cells whose
 * centroid lies within max(structure size, cell size) of the point — the
 * manhole-area average Rubinato et al. (2017) found best in unsteady runs.
 * Both fall back to the coupling cell when every cell is dry.
 */
enum class CouplingHead2D : int8_t { CELL = 0, VERTEX = 1, STENCIL_WET_MEAN = 2 };

/**
 * @brief Above-rim storage of a 2D-coupled junction under dynamic wave
 *        ([2D_OPTIONS] COUPLING_POND_MODE; plan §4.2).
 *
 * FOOTPRINT (default): the pond spreads over the 2D-cell footprint (the
 * median-dual area of the coupling cells) — the same water the stencil cells
 * hold, so it is stored twice. CHAMBER: the pond is the structure's chamber
 * area only; all ponded water lives on the mesh. TRACKING: the pond area
 * relaxes from the chamber area toward the currently wet footprint over
 * COUPLING_POND_RELAX, conserving the held volume as it changes.
 */
enum class CouplingPondMode2D : int8_t { FOOTPRINT = 0, CHAMBER = 1, TRACKING = 2 };

/**
 * @brief Compute backend for the 2D marcher (mirrors fv::Backend).
 *
 * AUTO (default) lets SurfaceSolverFactory pick: a device plugin (cuda, hip,
 * sycl) above the device mesh-size floor, the OpenMP plugin above its own
 * (higher) floor, else the built-in CPU marcher. The named values request one
 * backend outright and bypass the size floors; a plugin that is absent or has
 * no usable device falls back to CPU with a stderr notice, never a hard fail.
 * The OPENSWMM_2D_BACKEND environment variable, when set, overrides this
 * option (same precedence as OPENSWMM_FV_BACKEND over FV_BACKEND).
 *
 * Parsed from [2D_OPTIONS] BACKEND (AUTO|CPU|OMP|CUDA|HIP|SYCL).
 */
enum class Backend2D : int8_t {
    CPU = 0,  ///< Built-in marcher (OpenMP-threaded on the host), no plugin.
    AUTO = 1, ///< Default: plugins above their size floors, else CPU.
    OMP = 2,  ///< Kokkos OpenMP host plugin.
    CUDA = 3, ///< Kokkos CUDA device plugin (NVIDIA).
    HIP = 4,  ///< Kokkos HIP device plugin (AMD).
    SYCL = 5  ///< Kokkos SYCL device plugin (Intel).
};

/**
 * @brief Momentum closure of the explicit 2D marcher
 *        (2D_FULL_SWE_SHOCK_CAPTURING_PLAN_2026-09-05 §2).
 *
 * Parsed from [2D_OPTIONS] MOMENTUM_EQUATION. One marcher (tiered LTS,
 * active sets, positivity, coupling, transport) with three face laws:
 *  - LOCAL_INERTIAL  de Almeida & Bates face-q update (default; bit-identical
 *                    to the pre-2026-09 marcher).
 *  - FULL_SWE        conservative shallow-water equations with the convective
 *                    term: cell (h, hu, hv), hydrostatic reconstruction,
 *                    rotated HLLC Riemann flux, shock capturing.
 *  - DIFFUSIVE_WAVE  Manning quasi-steady face flux, no inertia (Hunter et al.
 *                    2005 explicit diffusive wave; Δt ∝ Δx² per cell, absorbed
 *                    by the LTS tiers).
 */
enum class Momentum2D : int8_t { LOCAL_INERTIAL = 0, FULL_SWE = 1, DIFFUSIVE_WAVE = 2 };

/**
 * @brief Storage precision of the 2D results file's time-varying datasets
 *        ([2D_OPTIONS] OUTPUT_PRECISION). Mesh geometry always stays float64:
 *        projected coordinates are O(1e6) m and float32 would lose
 *        centimetres there; depth/velocity fields have no such offset.
 */
enum class OutputPrecision2D : int8_t {
    FLOAT32 = 0, ///< default — halves the file, ~7 significant digits
    FLOAT64 = 1  ///< bit-for-bit parity tooling
};

/**
 * @brief Dataset groups the 2D results writer can emit
 *        ([2D_OPTIONS] REPORT_2D_VARIABLES). One bit per group; the tokens
 *        are the names below. Unselected groups are NOT created in the
 *        file, so readers treat every time-varying dataset as optional.
 */
namespace report2d {
enum Var : unsigned {
    DEPTH = 1u << 0,        ///< Mesh2_face_depth, Mesh2_face_head
    VELOCITY = 1u << 1,     ///< Mesh2_face_vx, Mesh2_face_vy
    EDGE_FLUX = 1u << 2,    ///< Mesh2_edge_flux (GUI velocity reconstruction, profile flux)
    NODE_HEAD = 1u << 3,    ///< Mesh2_node_head, Mesh2_node_depth (render reconstruction)
    SPECIES = 1u << 4,      ///< Mesh2_face_species_conc (when transport rows exist)
    RAINFALL = 1u << 5,     ///< Mesh2_face_rainfall, Mesh2_face_rain_cum
    INFILTRATION = 1u << 6, ///< Mesh2_face_infil_rate, Mesh2_face_infil_cum
    COUPLING = 1u << 7,     ///< Mesh2_face_coupling_flux, Mesh2_face_net_source
    GRADIENTS = 1u << 8,    ///< Mesh2_face_grad_hx/hy and the _lim pair (solver diagnostics)
    CONTINUITY = 1u << 9,   ///< Mesh2_face_continuity_err (solver diagnostic)
    ENVELOPES = 1u << 10,   ///< Mesh2_face_max_depth / _max_velocity / _max_continuity_err
    BUILDUP = 1u << 11,     ///< S7: Mesh2_face_buildup (when [2D_COVERAGES] rows resolved)
    GROUNDWATER =
        1u
        << 12, ///< G-O: Mesh2_face_gw_* per-cell fields, groundwater_ledger, groundwater_node_exchange_cum (when a [2D_AQUIFER] resolved)
    GW_DETAILED =
        1u << 13,    ///< G-O: Mesh2_face_gw_theta_sigma [time, layer, face] — m × nFace per step
    HEAT = 1u << 14, ///< Thermal temperatures, soil layers, and signed cumulative heat budgets
    TURBULENCE = 1u << 15, ///< Horizontal eddy viscosity and strain rate diagnostics
    ALL_MASK = (1u << 16) - 1u,
    /// DEFAULT: everything a user renders or plots; solver diagnostics off.
    DEFAULT_MASK = DEPTH | VELOCITY | EDGE_FLUX | NODE_HEAD | SPECIES | RAINFALL | INFILTRATION |
                   ENVELOPES | BUILDUP | GROUNDWATER,
    /// MINIMAL: depth map + render reconstruction + envelopes only.
    MINIMAL_MASK = DEPTH | NODE_HEAD | ENVELOPES,
};
} // namespace report2d

/**
 * @brief Configuration for the 2D surface routing solver.
 *
 * Populated from [2D_OPTIONS] input section. Defaults are chosen for
 * typical urban drainage surface routing problems.
 */
enum class Coriolis2D : int8_t { OFF = 0, LATITUDE = 1, CONSTANT_F = 2 };
enum class Turbulence2D : int8_t { NONE = 0, SMAGORINSKY = 1 };

struct SolverOptions2D {
    // Explicit opt-in. Unauthored fields are omitted by the writer.
    bool coupled_heat = false, heat_model_authored = false;
    int heat_surface = -1, heat_radiative = -1; // AUTO inherits network module
    bool heat_surface_authored = false, heat_radiative_authored = false;
    int bed_heat_model = 0; // NONE, LOCAL_STORAGE, GROUND_COLUMN
    bool bed_heat_authored = false;
    // Derived during initialization: legacy runtime-water conversion stays
    // 4.186 MJ/(m3 K); opt-in coupled heat uses configured water rho*cp.
    double water_heat_capacity = 4186000.0;
    /// [2D_OPTIONS] MOMENTUM_EQUATION — momentum closure (see Momentum2D).
    Momentum2D momentum = Momentum2D::LOCAL_INERTIAL;
    /// Optional horizontal momentum physics; CPU FULL_SWE, global timestep only.
    Coriolis2D coriolis = Coriolis2D::OFF;
    Turbulence2D turbulence = Turbulence2D::NONE;
    double coriolis_latitude = 0.0, coriolis_f = 0.0, smagorinsky_cs = 0.10;
    bool coriolis_latitude_set = false, coriolis_f_set = false;
    /// Derived from authored WALL_DRAG edges; not a global input switch.
    bool wall_drag_enabled = false;
    bool momentumPhysics() const noexcept {
        return wall_drag_enabled || coriolis != Coriolis2D::OFF || turbulence != Turbulence2D::NONE;
    }
    double coriolisParameter() const noexcept {
        return coriolis == Coriolis2D::CONSTANT_F ? coriolis_f
             : coriolis == Coriolis2D::LATITUDE
                 ? 1.4584231711061317e-4 * std::sin(coriolis_latitude * 0.017453292519943295)
                 : 0.0;
    }
    /// [2D_OPTIONS] FRONT_REBUILD AUTO|YES|NO — rebuild the flux-active set
    /// as soon as a wetting front reaches the edge of the active halo instead
    /// of waiting for the fixed rebuild cadence (kRebuildEveryCycles macro
    /// cycles). Without it a dry-bed front can advance at most one cell ring
    /// per cadence — the stall that pins the SWASHES Ritter/Thacker results.
    /// AUTO (default): YES for FULL_SWE / DIFFUSIVE_WAVE, NO for
    /// LOCAL_INERTIAL (which stays bit-identical to its pre-2026-09 results).
    int front_rebuild = -1; ///< -1 AUTO, 0 NO, 1 YES
    /// [2D_OPTIONS] RECONSTRUCTION_ORDER 1|2 — FULL_SWE only: 1 = piecewise
    /// constant (first-order Godunov, forward Euler); 2 = MUSCL on (eta, h, u, v)
    /// with limited Green-Gauss / wet-shoreline gradients + SSP-RK2
    /// (global-dt mode; LTS_TIERS > 1 is reduced to 1 with a warning).
    int reconstruction_order = 1;
    /// Max marcher step (s): caps film-cell CFL steps (and thus the LTS tier
    /// spread) and the co-advance sync-batch span.
    double max_timestep = 10.0;
    double dry_depth = 0.001;        ///< Dry cell threshold (m)
    double limiter_epsilon = 1.0e-6; ///< Slope limiter epsilon
    /// Head-difference regularization (m) for the diffusive-wave flux √|Δη|.
    /// Below this gradient the flux is linearized (C¹) so the transmissivity
    /// stays bounded as the water surface flattens — without it, deep near-level
    /// ponding (e.g. a large design storm draining) makes the flux Jacobian blow
    /// up and the implicit step collapse. Only affects millimeter-scale
    /// gradients, so bulk flow is preserved; raise it for extra robustness on
    /// very deep problems. Default 4 mm; 0 = bare √. Parsed from
    /// [2D_OPTIONS] FLUX_DH_EPS; env OPENSWMM_2D_FLUX_DH_EPS overrides.
    double flux_dh_eps = 0.004; ///< Diffusive-flux gradient floor (m)
    /// [2D_OPTIONS] COUPLING_SYNC (s): 1D↔2D co-advance sync-batch span.
    /// 0 (default) couples every routing step — exchange volumes reach the
    /// 1D with at most one routing step of lag, which keeps fill-and-spill
    /// coupling (weir/culvert ponds) free of batch-delay ringing. > 0
    /// batches the 2D advance over ~SPAN seconds (clamped to
    /// [routing_step, 60]) — the wall-clock lever for large meshes where
    /// per-step advances degenerate to the global-dt tail; expect the
    /// held-exchange error to grow with the span.
    double coupling_sync = 0.0;

    double coupling_cd = 0.65; ///< Default discharge coefficient
    /// [2D_OPTIONS] COUPLING_LAW ORIFICE|WEIR_ORIFICE (default ORIFICE).
    CouplingLaw2D coupling_law = CouplingLaw2D::ORIFICE;
    /// [2D_OPTIONS] COUPLING_HEAD CELL|VERTEX|STENCIL_WET_MEAN (default CELL).
    CouplingHead2D coupling_head = CouplingHead2D::CELL;
    /// [2D_OPTIONS] COUPLING_COEFFS — WEIR_ORIFICE coefficient preset
    /// (NodeCoupling2DCoeffs: DEFAULT|RUBINATO2017|RUBINATO2017_LOWER|
    /// RUBINATO2017_UPPER); a node's COEFFS / CW / CSW / COI / COO override.
    int coupling_coeffs = 0;
    /// [2D_OPTIONS] COUPLING_POND_MODE FOOTPRINT|CHAMBER|TRACKING.
    CouplingPondMode2D coupling_pond_mode = CouplingPondMode2D::FOOTPRINT;
    /// [2D_OPTIONS] COUPLING_POND_RELAX — TRACKING relaxation time (s);
    /// 0 = five routing steps.
    double coupling_pond_relax = 0.0;
    /// [2D_OPTIONS] COUPLING_RIM_BAND — height (m) above the rim over which
    /// the pond area ramps in (C¹). < 0 = the mode default: 0 under FOOTPRINT
    /// (the legacy step), 0.05 under CHAMBER / TRACKING.
    double coupling_rim_band = -1.0;
    /// [2D_OPTIONS] COUPLING_EQUALIZE YES|NO — cap each junction exchange so
    /// one coupling window cannot carry the 1D and 2D heads past each other.
    /// -1 = per law (on under WEIR_ORIFICE, off under ORIFICE).
    int8_t coupling_equalize = -1;
    bool report_2d = true; ///< Write 2D results to output

    // ---- Results-file size controls (plans/2D_OUTPUT_FLOAT32_AND_VARIABLE_SELECTION_PLAN) ----
    /// [2D_OPTIONS] OUTPUT_PRECISION FLOAT32|FLOAT64 — storage type of the
    /// time-varying datasets and envelopes (geometry stays float64).
    OutputPrecision2D output_precision = OutputPrecision2D::FLOAT32;
    /// [2D_OPTIONS] OUTPUT_COMPRESSION 0..9 — zlib level on chunked datasets;
    /// the byte-shuffle filter precedes it whenever level > 0. 0 = none.
    int output_compression = 4;
    /// [2D_OPTIONS] REPORT_2D_VARIABLES — bitmask of report2d::Var groups
    /// (tokens, or presets DEFAULT | MINIMAL | ALL).
    unsigned report_2d_vars = report2d::DEFAULT_MASK;
    /// [2D_OPTIONS] REPORT_2D_SPECIES — species row names to write; empty =
    /// every row the transport layout carries.
    std::vector<std::string> report_2d_species;
    /// [2D_OPTIONS] REPORT_2D_STEP — 2D-only report interval (seconds);
    /// 0 = follow [OPTIONS] REPORT_STEP. Validated at start to be a positive
    /// multiple of REPORT_STEP.
    double report_2d_step = 0.0;

    // -----------------------------------------------------------------------
    // E2 (2026-09-07) — process enables. Every default reproduces the
    // behaviour before the keys existed, so a deck without them is
    // bit-identical. Consumed in SurfaceRouter2D::initialize() (infiltration
    // rows, transport row layout via transport::surface2DEnables) and the
    // per-step forcing refresh (evaporation).
    // -----------------------------------------------------------------------
    /// [2D_OPTIONS] INFILTRATION YES|NO. -1 = AUTO (unset): infiltration is
    /// on iff any [2D_INFILTRATION*] row resolved (the pre-E2 rule). 0 = NO:
    /// rows are kept in the model but Infil2D is deactivated for the run.
    /// 1 = YES: explicit; warns when no row resolves.
    int8_t infiltration = -1;
    /// [2D_OPTIONS] INFIL_STEP (seconds). The canonical home of the
    /// [2D_INFILTRATION_OPTIONS] INFIL_STEP value; the old section is still
    /// read. 0 = unset (the section value, else the project WET_STEP).
    /// When both are present this one wins.
    double infil_step = 0.0;
    /// [2D_OPTIONS] INFIL_DEFAULT_METHOD token (NONE | HORTON | MOD_HORTON |
    /// GREEN_AMPT | MOD_GREEN_AMPT | CURVE_NUMBER | CONSTANT). Empty = unset:
    /// the '*' row of [2D_INFILTRATION_DEFAULTS] governs. NONE drops the '*'
    /// row for the run; a method must match the '*' row (its parameters live
    /// there) — a mismatch is an initialize error, a missing row a warning.
    std::string infil_default_method;
    /// [2D_OPTIONS] INFIL_DESTINATION token (LOST | SUBCATCH_AQUIFER |
    /// AQUIFER_2D). Empty = LOST. Applied at initialize to every
    /// [2D_INFILTRATION*] row that did not spell its own DEST column.
    /// SUBCATCH_AQUIFER routes to the containing subcatchment's legacy
    /// aquifer (U3); AQUIFER_2D is authoring-only until the G1 kernel.
    std::string infil_destination;
    /// [2D_OPTIONS] EVAPORATION NO | YES | CLIMATE. 1 = YES (default, the
    /// pre-E2 sink: per-cell forcing only — swmm_2d_force_evap*). 0 = NO:
    /// the evaporation sink is zero for the run (forcing ignored). 2 =
    /// CLIMATE: unforced cells evaporate at the project [EVAPORATION] rate
    /// (climate_state.evap_rate, ft/s → m/s); forcing still overrides/adds.
    int8_t evaporation = 1;
    /// [2D_OPTIONS] TRANSPORT_POLLUTANTS | TRANSPORT_MSX | TRANSPORT_AGE |
    /// TRANSPORT_TEMPERATURE YES|NO — the 2D column of the Domain × Species
    /// matrix (transport::resolve). NO drops that class's rows from the
    /// surface transport state; the 1D side is unaffected.
    bool transport_pollutants = true;
    bool transport_msx = true;
    bool transport_age = true;
    bool transport_temperature = true;

    // -----------------------------------------------------------------------
    // U5 (2026-09-07), rewired 2026-09-07 once the G1 two-zone kernel landed.
    // These are the PROCESS ENABLES for the integrated 2D subsurface; the
    // parameters themselves live in [2D_AQUIFER_OPTIONS] / [2D_AQUIFER] /
    // [2D_AQUIFER_NODE]. Same shape as INFILTRATION / INFIL_STEP above: a
    // tri-state enable whose AUTO reproduces the pre-existing behaviour, and
    // an alias that folds into the authoritative struct at open.
    // -----------------------------------------------------------------------
    /// [2D_OPTIONS] GROUNDWATER YES|NO. -1 = AUTO (unset): the kernel runs iff
    /// any [2D_AQUIFER*] row was authored — exactly what it did before this
    /// key existed, so a deck without it is unchanged. NO keeps the rows (they
    /// still save) and runs without the subsurface; YES with no rows is a deck
    /// that expects groundwater and has none, and says so.
    int8_t groundwater = -1;
    /// [2D_OPTIONS] GW_ET NONE | CAPILLARY_RISE | BOUNDARY_ET | BOTH — an
    /// ALIAS of the [2D_AQUIFER_OPTIONS] key of the same name, which is the
    /// single source of truth (GwOptions::gw_et is what the kernel reads and
    /// what the writer emits). Empty = not spelled here; SWMMEngine::open
    /// folds a spelled value into GwOptions and clears this, so the value is
    /// never stored — or written — in two places.
    std::string gw_et;

    // Rainfall→mesh mapping. Default NATURAL_NEIGHBOUR (spatial interpolation
    // across all located gages); SYSTEM applies the uniform all-gage mean.
    // Parsed from [2D_OPTIONS] RAINFALL_MODE; env OPENSWMM_2D_RAINFALL_MODE
    // (natural|nearest|system|none) overrides.
    RainfallMode rainfall_mode = RainfallMode::NATURAL_NEIGHBOUR;

    // Volume → free-surface cell closure. Default FLAT (legacy η = tri_cz + V/A):
    // fast and the right choice for typical deep-water urban flooding, where a
    // partially wet cell is the exception. VFR (planar-bed volume/free-surface,
    // fully implemented on all backends) restores the C-property at shorelines
    // and removes the water-climbs-uphill artifact, but resolves the shoreline
    // wetting/drying FLAT freezes out — ~3–8× more marcher substeps — so it is
    // OPT-IN (best on shallow water / gentle slopes). Parsed from [2D_OPTIONS]
    // CELL_CLOSURE (FLAT|VFR). See plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
    CellClosure2D cell_closure = CellClosure2D::FLAT;

    // Effective conveyance depth at shared edges. Default MEAN (legacy upwind
    // cell-mean depth). VFR_FACE (B&S Eq. 14 face depth + wetting gate) pairs
    // with CELL_CLOSURE=VFR to complete the artifact fix; opt-in for the same
    // reason. Parsed from [2D_OPTIONS] FACE_RECONSTRUCTION (MEAN|VFR_FACE).
    FaceDepth2D face_reconstruction = FaceDepth2D::MEAN;

    /// Wetted-area-fraction floor ε of the regularized VFR closure: below wet
    /// fraction ε the η(V) relation continues linearly (slope 1/(εA)), bounding
    /// dη/dV for the implicit solvers' Newton/Jacobian path. Exact elsewhere.
    /// Only used when CELL_CLOSURE = VFR. Parsed from [2D_OPTIONS]
    /// VFR_MIN_WET_FRAC; valid range (0, 0.5].
    double vfr_min_wet_frac = 0.01;

    // -----------------------------------------------------------------------
    // Explicit local-inertial marcher (INTEGRATOR EXPLICIT — the only 2D
    // integrator since the D2 CVODE/ARKODE retirement) options. Defaults per
    // the 2026-07-29 reimplementation plan.
    // -----------------------------------------------------------------------
    /// Face-update θ weighting (de Almeida & Bates 2013): 1 = pure Bates 2010
    /// (no numerical diffusion), <1 blends the Perot-reconstructed neighbour
    /// discharge to damp thin-film checkerboarding on steep faces.
    double theta = 0.8;      ///< [2D_OPTIONS] THETA, (0, 1]
    /// Seconds over which THETA retains momentum. Zero preserves the legacy
    /// per-firing blend; positive values make its relaxation rate independent
    /// of face-step splitting and coupling-window tails.
    double theta_reference_step = 0.0; ///< [2D_OPTIONS] THETA_REFERENCE_STEP, s >= 0
    double cfl_number = 0.7; ///< [2D_OPTIONS] CFL_NUMBER — α in
                             ///< dt = α·L_char/(√(gh)+|u|). L_char is
                             ///< derived from the discrete wave operator
                             ///< (InertialEdges), so α is a TRUE Courant
                             ///< fraction: 1.0 = linear stability limit,
                             ///< 0.7 default = 30% margin on any mesh.
    /// Flux-activation depth (m): cells below it are source-only (lazy rain
    /// accumulation, no face flux). Hysteresis band ±1 mm around it.
    double h_move = 0.003;   ///< [2D_OPTIONS] H_MOVE (m) — flux-active
                             ///< cell threshold; the marcher's on/off
                             ///< hysteresis band is min(1 mm, h_move/2),
                             ///< so thin-depth models (H_MOVE ≪ 1 mm)
                             ///< activate near h_move as requested.
    int lts_tiers = 4;       ///< [2D_OPTIONS] LTS_TIERS, 1..8 (1 = global dt)
    double froude_max = 1.5; ///< [2D_OPTIONS] FROUDE_MAX face |u| clamp
    /// Convective momentum flux ∂(u·q)/∂n at interior faces, in
    /// Stelling–Duinmeijer staggered upwind form on the Perot cell vectors.
    /// This is the term the pure local-inertial formulation drops, and
    /// without it the scheme has no velocity head: a transcritical reach
    /// holds a FLAT free surface upstream of a control (measured on the
    /// SWASHES bump: η = 0.59 m against an analytic backwater of 1.02 m) and
    /// bores land on the wrong Rankine–Hugoniot states. Vanishes identically
    /// at rest and in uniform flow, so lake-at-rest exactness is untouched.
    /// OPT-IN while the 2D validation ladder is re-graded against it: the
    /// default reproduces the established local-inertial results exactly.
    bool advection = false; ///< [2D_OPTIONS] ADVECTION YES|NO
    /// Positivity/exchange availability fraction β: max share of a cell's
    /// volume that outgoing fluxes (or a coupling drain) may take per own-step.
    double exchange_beta = 0.8;

    /// Overland transport S2 — `[2D_OPTIONS] DISPERSION <m²/s>`: isotropic
    /// species dispersion coefficient (D-2DT7). 0 (default) means the
    /// dispersive face term is never entered, so pre-S2 answers are
    /// bit-identical by construction rather than by a zero coefficient
    /// multiplying through. Refused negative at parse: anti-diffusion is not
    /// a modelling case.
    double dispersion = 0.0;
    /// Unused: no input sets it and no solver path reads it. Kept only
    /// because the common-source checkpoint fingerprint hashes it (removing
    /// it would refuse existing native checkpoints).
    double exchange_relax = 1.0;
    /// [2D_OPTIONS] COUPLING_AREA AUTO: derive exchange area at coupling-point
    /// resolve from the largest connected conduit (clamp(1.25·A_conduit,
    /// 0.05, 2.0) m²) for every junction coupling point, authored or not.
    bool coupling_area_auto = false;

    /// [2D_OPTIONS] COUPLING_IN_FLOODING: book the 1D→2D spill into the 1D
    /// report's "Flooding Loss" row instead of its own "2D Coupling Outflow"
    /// row. Default NO — the split row is the accurate reporting, since a
    /// coupling transfer is not flooding.
    ///
    /// YES exists for one reason: comparing a run against a build from before
    /// the split, where the spill was inside Flooding Loss. It changes the
    /// report only; the continuity error and every routed volume are
    /// identical either way.
    bool coupling_in_flooding = false;

    /// [2D_OPTIONS] BACKEND: which marcher implementation runs the mesh. See
    /// Backend2D. Read once, at SurfaceRouter2D::initialize() (the solver is
    /// constructed there), so a mid-run edit takes effect on the next open.
    Backend2D backend = Backend2D::AUTO;

    /// Per-session AUTO policy, set before initialization through the C API.
    /// Not an authored option or checkpoint identity. Explicit model/environment
    /// requests retain precedence; the GUI uses this to avoid plugin discovery.
    bool auto_backend_cpu = false;

    /// Path from [2D_MESH_FILE] FILE token. Empty = mesh is inline in main .inp.
    /// `.absolute` is filled by resolve_external_file_slots() against the source
    /// .inp directory, which is what lets the writer re-anchor a RELATIVE token
    /// when saving to a different folder (without it, a Save-As left the .2dm
    /// reference pointing at the old directory).
    openswmm::FilePathPair mesh_file;

    /// HDF5 output file path from [2D_OPTIONS] OUTPUT_FILE token. Empty =
    /// no 2D output is written. Resolved relative to the parent .inp directory
    /// at the point of use (SWMMEngine::open) and re-anchored on save from
    /// `.absolute`, same as mesh_file.
    openswmm::FilePathPair output_file;

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
    double len_1d_to_2d = 1.0;  ///< 1D length → 2D length (ft→m, 0.3048)
    double len_2d_to_1d = 1.0;  ///< 2D length → 1D length (m→ft, 3.2808)
    double vol_1d_to_2d = 1.0;  ///< 1D volume → 2D volume (ft³→m³, exact 0.3048³)
    double flow_1d_to_2d = 1.0; ///< 1D flow → 2D flow (ft³/s→m³/s, exact 0.3048³)
    double flow_2d_to_1d = 1.0; ///< 2D flow → 1D flow (m³/s→ft³/s, 35.315)

    /*! Runtime-only: resolved OpenMP thread count for the embarrassingly-
     *  parallel 2D per-cell / per-vertex loops (RHS pipeline, Jacobi
     *  preconditioner, post-step diagnostics). Set in
     *  SurfaceRouter2D::initialize() from SimulationOptions::num_threads (the
     *  global THREADS option) using the same min(N,max) + size-gate
     *  DWSolver::setNumThreads applies. 1 = serial. The parallelised loops use
     *  schedule(static) and write only their own cell/vertex slot, so any
     *  thread count is bit-identical to serial. Never parsed/persisted. */
    int num_threads = 1;

    /*! Runtime-only: the RAW [OPTIONS] THREADS value (0 = auto) copied in
     *  SurfaceRouter2D::initialize(), handed to the Kokkos OpenMP plugin as
     *  OpenSwmmGpuProbe::requested_threads (ABI v4) so the 2D host backend
     *  follows THREADS like the 1D solvers. Never parsed/persisted. */
    int requested_threads = 0;

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

    /*! Runtime-only: the linear factor SurfaceRouter2D::initialize() applied
     *  to the authored mesh coordinates to reach the solver's SI metres —
     *  0.3048 for US FLOW_UNITS, 1.0 for SI projects and for meshes that
     *  declared `;; UNITS: SI (m)`. Equivalently: metres per model-CRS linear
     *  unit. Georeferenced output (Default2DOutputPlugin) writes this into the
     *  `/crs` variable so a consumer can return the stored metric coordinates
     *  to the model CRS's own unit exactly. Never parsed/persisted. */
    double mesh_to_si_factor = 1.0;

    /*! Runtime-only: true after SurfaceRouter2D::initialize() drained the
     *  pending [2D_BOUNDARY_CONDITIONS] / [2D_EDGE_CONVEYANCE] rows into
     *  BoundaryData / MeshData::edge_conveyance. Serialization collectors
     *  (Serialize2D.hpp) switch to the drained arrays once this is set —
     *  they are the live state that post-initialize API mutations edit;
     *  the retained pending rows would be stale. Never parsed/persisted. */
    bool pending_rows_drained = false;

    /*! Runtime-only: the display-flow-units → m³/s factor initialize() has
     *  applied IN PLACE to the constant SPECIFIED_FLOW values in BoundaryData
     *  (1.0 until it runs, and for CMS projects). The [2D_BOUNDARY_CONDITIONS]
     *  file contract is display flow units per metre — there is no SI header
     *  for flows, unlike lengths — so the writers divide edge_bc_flow by this
     *  before emitting. Never parsed/persisted. */
    double bc_flow_to_si_applied = 1.0;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SOLVER_OPTIONS_HPP
