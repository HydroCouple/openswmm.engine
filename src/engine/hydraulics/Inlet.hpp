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
 * @file      Inlet.hpp
 * @brief     Street inlet capture efficiency — FHWA HEC-22.
 *
 * @details Inlet types: GRATE, CURB, COMBO, SLOTTED, DROP_GRATE, DROP_CURB,
 *          CUSTOM. On-grade capture uses splash-over velocity and frontal /
 *          side flow fractions (HEC-22 Eq. 4-16 … 4-24); on-sag capture uses
 *          the weir ↔ orifice transition (Eq. 4-26 … 4-33).
 *
 *          Two host kinds share one kernel and one statistics block
 *          (plans/INLET_JUNCTION_IMPLEMENTATION_PLAN_2026-09-05.md §2.1):
 *            * conduit-attribute usage — `[INLET_USAGE]`, host = a STREET
 *              conduit, bypass node = that conduit's downstream node;
 *            * inlet junction — `[INLET_JUNCTIONS]`, host = the node itself,
 *              approach flow from the attached street conduit(s).
 *
 *          The pure kernel functions below are transliterations of the legacy
 *          file-scope routines and carry the HEC-22 equation numbers; they take
 *          their state through @ref openswmm::inlet::Geom / @ref openswmm::inlet::Design /
 *          @ref openswmm::inlet::UsageParams instead of legacy's module-level shared
 *          variables, so they are directly unit-testable.
 *
 * @note Legacy reference: src/legacy/engine/inlet.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_INLET_HPP
#define OPENSWMM_INLET_HPP

#include <cstdint>
#include <vector>

namespace openswmm {

struct SimulationContext;
struct XSectParams;
struct Table;

namespace inlet {

/// Legacy `enum InletType` (inlet.c:119-122) — the COMBO slot is the merged
/// GRATE+CURB design the legacy parser encodes as two `[INLETS]` lines.
enum class InletType : int {
    GRATE = 0,
    CURB = 1,
    COMBO = 2,
    SLOTTED = 3,
    DROP_GRATE = 4,
    DROP_CURB = 5,
    CUSTOM = 6
};

/// Legacy `enum GrateType` (inlet.c:124-127).
enum class GrateType : int {
    P_BAR_50 = 0,
    P_BAR_50x100 = 1,
    P_BAR_30 = 2,
    CURVED_VANE = 3,
    TILT_BAR_45 = 4,
    TILT_BAR_30 = 5,
    RETICULINE = 6,
    GENERIC = 7
};

/// Legacy `enum InletPlacementType` (inlet.c:129) — matches the token order of
/// `[INLET_USAGE]` / `[INLET_JUNCTIONS]` and `InletUsageStore::placement`.
enum class Placement : int { AUTOMATIC = 0, ON_GRADE = 1, ON_SAG = 2 };

/// Legacy `enum ThroatAngleType` (inlet.c:131) — `InletStore::curb_throat`.
enum class ThroatAngle : int { HORIZONTAL = 0, INCLINED = 1, VERTICAL = 2 };

/// `InletStore::curve_kind` codes for a CUSTOM design.
enum class CurveKind : int { UNRESOLVED = 0, DIVERSION = 1, RATING = 2 };

/// Legacy MIN_RUNOFF_FLOW (consts.h:203) — cfs.
inline constexpr double MIN_RUNOFF_FLOW = 0.001;
/// Legacy FUDGE (consts.h:272).
inline constexpr double INLET_FUDGE = 0.0001;
/// Legacy BIG (consts.h:118).
inline constexpr double INLET_BIG = 1.0e10;

// ============================================================================
// Kernel inputs — the legacy module-level shared variables, made explicit
// ============================================================================

/**
 * @brief Street / channel geometry seen by one inlet, in INTERNAL units.
 *
 * @details Mirrors the file-scope block of legacy inlet.c (`Sx`, `SL`, `Sw`,
 *          `a`, `W`, `T`, `n`, `Nsides`, `Tcrown`, `Beta`, `Qfactor`, `xsect`)
 *          that `getConduitGeometry()` (inlet.c:1147-1196) fills. `t` (the flow
 *          spread) is an output of `getFlowSpread()` that later kernels read,
 *          exactly as legacy's shared `T` is.
 */
struct Geom {
    double sx = 0.01;       ///< Street cross slope (fraction)
    double sl = 0.0;        ///< Conduit longitudinal slope (fraction)
    double sw = 0.01;       ///< Gutter + cross slope
    double a = 0.0;         ///< Gutter depression (ft)
    double w = 0.0;         ///< Gutter width (ft)
    double t = 0.0;         ///< Top width of flow spread (ft) — set by getFlowSpread
    double n = 0.016;       ///< Manning's roughness
    int nsides = 1;         ///< 1- or 2-sided street
    double t_crown = 100.0; ///< Distance from curb to crown (ft)
    double beta = 0.0;      ///< 1.486·√SL / n
    double qfactor = 0.0;   ///< Izzard's f in Q = f·T^2.67

    /// Host conduit cross-section — non-null only for the DROP_* (channel)
    /// designs, whose grate kernel needs A(S)/Y(A)/W(Y) of the channel.
    const XSectParams* xs = nullptr;
};

/// One inlet design in INTERNAL units (legacy `TInletDesign`, inlet.c:66-74).
struct Design {
    int type = 0; ///< inlet::InletType
    int grate_type = static_cast<int>(GrateType::GENERIC);
    double grate_length = 0.0;   ///< ft (0 ⇒ no grate half)
    double grate_width = 0.0;    ///< ft
    double frac_open_area = 0.0; ///< GENERIC grates only
    double splash_veloc = 0.0;   ///< ft/s, GENERIC grates only
    double curb_length = 0.0;    ///< ft (0 ⇒ no curb half)
    double curb_height = 0.0;    ///< ft
    int curb_throat = static_cast<int>(ThroatAngle::VERTICAL);
    double slotted_length = 0.0; ///< ft (0 ⇒ not a slotted drain)
    double slotted_width = 0.0;  ///< ft
};

/// The per-placement half of one usage row (legacy `TInlet` fields).
struct UsageParams {
    int num_inlets = 1;
    double clog_factor = 1.0; ///< 1 − %clog/100
    double flow_limit = 0.0;  ///< cfs per inlet; 0 ⇒ unlimited
};

// ============================================================================
// Pure kernel — 1:1 ports of the legacy static functions
// ============================================================================

/**
 * @brief Compute the depressed-gutter flow ratio using HEC-22 Eq4-4.
 *
 * No finite/domain checks or clamping; caller supplies a physically valid nondegenerate geometry.
 *
 * @param[in] sr Positive gutter-to-street cross-slope ratio, dimensionless.
 * @param[in] ts Positive spread beyond gutter ft; no zero-denominator guard.
 * @param[in] w Positive gutter width ft.
 * @return Dimensionless ratio1/(1+sr/((1+sr/(ts/w))^2.67-1)).
 */
double getEo(double sr, double ts, double w);

/**
 * @brief Compute approach flow spread with crown limiting and depressed-gutter iteration.
 *
 * Undepressed street uses (Q/qfactor)^0.375. Depressed gutter uses a maximum10-update fixed-point
 * solve with0.01ft difference tolerance; last estimate is returned without a convergence flag. Does
 * not assign g.t because geometry is const.
 *
 * @param[in] g Borrowed native street/channel geometry with positive physical
 *     slopes/roughness/dimensions needed by the chosen branch.
 * @param[in] Q Positive finite approach discharge cfs; low-level formula does not validate it.
 * @return Top width/spread ft, capped above by g.t_crown.
 */
double getFlowSpread(const Geom& g, double Q);

/**
 * @brief Compute the fraction of street flow above a selected gutter width.
 *
 * No flow calculation, spread refresh or finite/range checks.
 *
 * @param[in] g Borrowed native street/channel geometry with positive physical
 *     slopes/roughness/dimensions needed by the chosen branch.
 * @param[in] w Positive width ft; g.t is already computed flow spread.
 * @return 1 when spread<=w, otherwise depressed-gutter Eo or1-(1-w/t)^2.67.
 */
double getGutterFlowRatio(const Geom& g, double w);

/**
 * @brief Compute the grate-width correction to depressed-gutter intercepted flow.
 *
 * Uses precomputed spread and gutter slopes; no generic denominator/finite guard.
 *
 * @param[in] g Borrowed native street/channel geometry with positive physical
 *     slopes/roughness/dimensions needed by the chosen branch.
 * @param[in] grate_width Positive effective grate width ft.
 * @param[in] area Positive total flow cross-section area ft2; denominator after subtracting
 *     outside-gutter area must remain positive.
 * @return Dimensionless grate/gutter flow-area ratio, with1 for full-width coverage.
 */
double getGutterAreaRatio(const Geom& g, double grate_width, double area);

/**
 * @brief Evaluate the standard grate splash-over velocity polynomial.
 *
 * Polynomial is not clamped to nonnegative values; generic designs use their separate authored
 * splash velocity.
 *
 * @param[in] grate_type Standard fitted type0..6; GENERIC or out-of-range selects0.
 * @param[in] grate_length Physical grate length ft.
 * @return Fitted velocity ft/s, or0 for an unsupported curve.
 */
double getSplashOverVelocity(int grate_type, double grate_length);

/**
 * @brief Compute one on-grade grate's frontal and side capture.
 *
 * Reads existing spread for street geometry; rectangular/trapezoidal DROP_GRATE may recompute
 * area/depth/width and adjust sx. Uses standard or authored splash velocity and depressed-gutter
 * corrections. This low-level result is not globally clipped to0..Q; outer usage routines apply
 * selected upper caps.
 *
 * @param[in] d Borrowed validated inlet design in native ft/ft per second; coefficients/types must
 *     match the selected physical design.
 * @param[in,out] g Borrowed native geometry; capture routines may update spread t and channel
 *     cross-slope sx. Borrowed xs must remain live for the call.
 * @param[in] Q Positive finite approach discharge cfs; low-level formula does not validate it.
 * @return Native cfs capture from HEC-22 efficiency formulas.
 */
double getGrateInletCapture(const Design& d, Geom& g, double Q);

/**
 * @brief Compute on-grade curb-opening capture from equivalent cross slope.
 *
 * Depressed gutters modify equivalent slope through Eo; full-capture length uses HEC-22 Eq4-22a.
 * Requires positive slope/roughness and valid spread; no independent geometry/finite checks.
 *
 * @param[in] g Borrowed native street/channel geometry with positive physical
 *     slopes/roughness/dimensions needed by the chosen branch.
 * @param[in] Q Positive finite approach discharge cfs; low-level formula does not validate it.
 * @param[in] opening_length Nonnegative curb/slotted opening length ft.
 * @return Approach Q times capture efficiency clamped0..1, cfs.
 */
double getCurbInletCapture(const Geom& g, double Q, double opening_length);

/**
 * @brief Compute a single on-grade inlet or combination sweeper/grate capture.
 *
 * DROP_CURB uses sag capacity and DROP_GRATE grate capacity, each upper-limited to Q. SLOTTED uses
 * curb capture. Street combinations compute positive curb length ahead of grate, then grate on
 * remaining Q, refreshing spread as needed. No universal lower clamp or donor inventory booking.
 *
 * @param[in] d Borrowed validated inlet design in native ft/ft per second; coefficients/types must
 *     match the selected physical design.
 * @param[in,out] g Borrowed native geometry; capture routines may update spread t and channel
 *     cross-slope sx. Borrowed xs must remain live for the call.
 * @param[in] Q Positive finite approach discharge cfs; low-level formula does not validate it.
 * @param[in] depth Nonnegative finite local water depth ft unless explicitly stated otherwise.
 * @return Native cfs capture before usage clog/replicate/flow-limit scaling.
 */
double getOnGradeInletCapture(const Design& d, Geom& g, double Q, double depth);

/**
 * @brief Compute series inlet capture across the current street sides.
 *
 * Requires positive g.nsides. Divides approach per side, evaluates replicates on remaining bypass,
 * applies clog/per-inlet limit and bypass upper cap, then multiplies sides. Stops when remaining
 * per-side flow is below MIN_RUNOFF_FLOW; geometry spread can change during evaluation.
 *
 * @param[in] d Borrowed validated inlet design in native ft/ft per second; coefficients/types must
 *     match the selected physical design.
 * @param[in] u Borrowed usage with nonnegative replicate count, clog factor0..1 and per-inlet
 *     flow_limit cfs (<=0 means unlimited).
 * @param[in,out] g Borrowed native geometry; capture routines may update spread t and channel
 *     cross-slope sx. Borrowed xs must remain live for the call.
 * @param[in] q Finite approach cfs; q<MIN_RUNOFF_FLOW or zero replicates returns0.
 * @param[in] depth Nonnegative finite local water depth ft unless explicitly stated otherwise.
 * @return Total native cfs captured across g.nsides.
 */
double getOnGradeCapturedFlow(const Design& d, const UsageParams& u, Geom& g, double q,
                              double depth);

/**
 * @brief Compute the active grate weir or orifice contribution for one sag inlet.
 *
 * DROP_GRATE uses all-four-side perimeter; gutter grate adjusts wetted width and mean depth.
 * Transition is depth<=1.79*open_area/perimeter; outputs cfs. Caller supplies positive
 * nondegenerate geometry and valid root/power domains; no full output reset or pointer validation.
 *
 * @param[in] d Borrowed validated inlet design in native ft/ft per second; coefficients/types must
 *     match the selected physical design.
 * @param[in] g Borrowed native street/channel geometry with positive physical
 *     slopes/roughness/dimensions needed by the chosen branch.
 * @param[in] depth Nonnegative finite local water depth ft unless explicitly stated otherwise.
 * @param[in,out] weir_flow Non-NULL scalar initialized by caller, typically0; assigned only on weir
 *     branch, otherwise preserved.
 * @param[in,out] orifice_flow Non-NULL scalar initialized by caller, typically0; assigned only on
 *     orifice branch, otherwise preserved.
 */
void findOnSagGrateFlows(const Design& d, const Geom& g, double depth, double* weir_flow,
                         double* orifice_flow);

/**
 * @brief Compute sag curb-opening weir/orifice contributions with transition blending.
 *
 * Uses uniform/long-opening or depressed-gutter weir, then blends toward orifice at1.4*curb_height.
 * Caller supplies nondegenerate transition depths and positive root domains; no automatic output
 * clearing, unit conversion or pointer checks.
 *
 * @param[in] d Borrowed validated inlet design in native ft/ft per second; coefficients/types must
 *     match the selected physical design.
 * @param[in] g Borrowed native street/channel geometry with positive physical
 *     slopes/roughness/dimensions needed by the chosen branch.
 * @param[in] depth Nonnegative finite local water depth ft unless explicitly stated otherwise.
 * @param[in] opening_length Curb length ft; <=0 leaves both outputs unchanged, DROP_CURB multiplies
 *     it by4.
 * @param[in,out] weir_flow Non-NULL cfs destination initialized by caller; untouched on
 *     pure-orifice/no-opening branch.
 * @param[in,out] orifice_flow Non-NULL cfs destination initialized by caller; untouched on
 *     pure-weir/no-opening branch.
 */
void findOnSagCurbFlows(const Design& d, const Geom& g, double depth, double opening_length,
                        double* weir_flow, double* orifice_flow);

/**
 * @brief Compute sag curb-opening orifice flow using throat-adjusted head.
 *
 * No finite/domain guard or source-flow cap.
 *
 * @param[in] flow_depth Water depth ft; must leave nonnegative head after throat correction.
 * @param[in] opening_height Positive curb-opening height ft.
 * @param[in] opening_length Nonnegative effective opening length ft.
 * @param[in] throat_angle HORIZONTAL subtracts height/2, INCLINED0.7071*height/2; other codes use
 *     full depth.
 * @return 0.67*height*length*sqrt(64.32*effective_head), cfs.
 */
double getCurbOrificeFlow(double flow_depth, double opening_height, double opening_length,
                          int throat_angle);

/**
 * @brief Compute one sag slotted-drain weir or orifice capacity.
 *
 * Uses2.48*length*depth^1.5 below threshold,0.8*length*width*sqrt(64.32*depth) above. No clog,
 * replicate or approach-flow cap here.
 *
 * @param[in] d Borrowed validated inlet design in native ft/ft per second; coefficients/types must
 *     match the selected physical design.
 * @param[in] depth Nonnegative finite local water depth ft unless explicitly stated otherwise.
 * @return Native cfs, switching at depth=2.587*slotted_width.
 */
double getOnSagSlottedFlow(const Design& d, double depth);

/**
 * @brief Combine one sag inlet's grate, sweeper and curb-orifice contributions.
 *
 * Positive slotted length takes precedence. Otherwise sums grate/sweeper weir+orifice and only
 * curb-orifice overlap when grate is in orifice mode; excludes overlapping curb-weir contribution
 * to match legacy. No donor cap or validation.
 *
 * @param[in] d Borrowed validated inlet design in native ft/ft per second; coefficients/types must
 *     match the selected physical design.
 * @param[in] g Borrowed native street/channel geometry with positive physical
 *     slopes/roughness/dimensions needed by the chosen branch.
 * @param[in] depth Nonnegative finite local water depth ft unless explicitly stated otherwise.
 * @return Native cfs capacity before usage scaling.
 */
double getOnSagInletCapture(const Design& d, const Geom& g, double depth);

/**
 * @brief Scale sag capacity using usage factors and legacy carried-over street sides.
 *
 * Zero replicates returns0. Does not cap by current approach/disposable volume; caller applies
 * routing-model limits. Previous-side ordering deliberately preserves legacy shared-state behavior.
 *
 * @param[in] d Borrowed validated inlet design in native ft/ft per second; coefficients/types must
 *     match the selected physical design.
 * @param[in] u Borrowed usage with nonnegative replicate count, clog factor0..1 and per-inlet
 *     flow_limit cfs (<=0 means unlimited).
 * @param[in] g Borrowed native street/channel geometry with positive physical
 *     slopes/roughness/dimensions needed by the chosen branch.
 * @param[in] depth Finite signed depth ft; absolute value is used for sag capacity.
 * @param[in] nsides_prev Nonnegative side count from previously processed inlet, not necessarily
 *     g.nsides; product with num_inlets must fit int.
 * @return Sag capacity cfs after clog/per-inlet limit, multiplied by nsides_prev*num_inlets.
 */
double getOnSagCapturedFlow(const Design& d, const UsageParams& u, const Geom& g, double depth,
                            int nsides_prev);

/**
 * @brief Compute custom inlet capture from authored diversion or rating curves.
 *
 * DIVERSION loops replicates on bypassed per-side flow, applying clog, per-inlet limit and bypass
 * upper cap. RATING multiplies depth-curve output by replicates/clog/sides without applying q or
 * flow_limit caps. No name binding, finite check or inventory booking.
 *
 * @param[in] u Borrowed usage with nonnegative replicate count, clog factor0..1 and per-inlet
 *     flow_limit cfs (<=0 means unlimited).
 * @param[in] g Borrowed native street/channel geometry with positive physical
 *     slopes/roughness/dimensions needed by the chosen branch.
 * @param[in] curve Borrowed resolved table with valid shape/sample units; table_lookupEx
 *     extrapolation rules apply.
 * @param[in] curve_kind DIVERSION means captured versus approach flow; RATING means captured versus
 *     depth; other codes return0.
 * @param[in] q Nonnegative native approach flow cfs.
 * @param[in] depth Nonnegative finite local water depth ft unless explicitly stated otherwise.
 * @param[in] ucf_flow Positive project-display flow units per cfs, for curve x/y conversion.
 * @param[in] ucf_len Positive project-display length units per ft, for rating curve depth.
 * @return Native cfs capture across current positive g.nsides.
 */
double getCustomCapturedFlow(const UsageParams& u, const Geom& g, const Table& curve,
                             int curve_kind, double q, double depth, double ucf_flow,
                             double ucf_len);

/**
 * @brief Compute the usage's effective open inlet area for backflow weighting.
 *
 * Adds grate openings and curb length exceeding grate length; positive slotted length replaces that
 * sum with slot area. Does not multiply street sides or validate dimensions.
 *
 * @param[in] d Borrowed validated inlet design in native ft/ft per second; coefficients/types must
 *     match the selected physical design.
 * @param[in] u Borrowed usage with nonnegative replicate count, clog factor0..1 and per-inlet
 *     flow_limit cfs (<=0 means unlimited).
 * @return Native ft2 open area times replicate count and clog factor.
 */
double getInletArea(const Design& d, const UsageParams& u);

// ============================================================================
// Runtime SoA
// ============================================================================

struct InletSoA {
    int count = 0;

    // --- host / topology -----------------------------------------------------
    std::vector<int> link_idx;        ///< Host conduit link index (−1 for a node host)
    std::vector<int> host_node;       ///< Host inlet-junction node (−1 for a conduit host)
    std::vector<int> node_idx;        ///< Capture (receiving) node index
    std::vector<int> bypass_node;     ///< Node the uncaptured flow stays at
    std::vector<int> up_link;         ///< Approach conduit (node host: node2 == host)
    std::vector<int> up_link2;        ///< Second inbound conduit at a sag node host (−1)
    std::vector<uint8_t> two_inbound; ///< 1 when both attached conduits flow toward the host

    // --- design --------------------------------------------------------------
    std::vector<int> inlet_type;
    std::vector<int> grate_type;
    std::vector<double> grate_length;   ///< ft
    std::vector<double> grate_width;    ///< ft
    std::vector<double> open_area;      ///< GENERIC open-area fraction
    std::vector<double> splash_veloc;   ///< ft/s (GENERIC)
    std::vector<double> curb_length;    ///< ft
    std::vector<double> curb_height;    ///< ft
    std::vector<int> curb_throat;       ///< 0=HORIZONTAL 1=INCLINED 2=VERTICAL
    std::vector<double> slotted_length; ///< ft
    std::vector<double> slotted_width;  ///< ft
    std::vector<int> curve_index;       ///< CUSTOM: index into ctx.tables
    std::vector<int> curve_kind;        ///< CUSTOM: inlet::CurveKind

    // --- usage ---------------------------------------------------------------
    std::vector<int> num_inlets;
    std::vector<double> clog_factor;
    std::vector<double> flow_limit;    ///< cfs per inlet (0 = unlimited)
    std::vector<double> local_depress; ///< ft
    std::vector<double> local_width;   ///< ft
    std::vector<int> placement;        ///< as authored (inlet::Placement)
    std::vector<uint8_t> is_sag;       ///< resolved placement: 1 = ON_SAG

    // --- street / conduit geometry (internal units) --------------------------
    std::vector<double> sx;                ///< Cross slope (fraction)
    std::vector<double> gutter_depression; ///< ft
    std::vector<double> gutter_width;      ///< ft
    std::vector<double> road_roughness;    ///< Manning's n
    std::vector<double> t_crown;           ///< ft
    std::vector<int> n_sides;
    std::vector<double> slope;       ///< Host/approach conduit slope
    std::vector<double> flow_factor; ///< Izzard f = (0.56/n)·√SL·Sx^1.67

    // --- per-step results ----------------------------------------------------
    std::vector<double> q_approach;     ///< Approach flow seen this step (cfs)
    std::vector<double> flow_capture;   ///< Captured flow rate (cfs)
    std::vector<double> backflow;       ///< Backflow from the capture node (cfs)
    std::vector<double> backflow_ratio; ///< inlet backflow / capture-node overflow

    // --- legacy statistics (inlet.c:78-87, updateInletStats :985) ------------
    std::vector<int> flow_periods;
    std::vector<int> capture_periods;
    std::vector<int> backflow_periods;
    std::vector<double> peak_flow;         ///< Peak approach flow (cfs)
    std::vector<double> peak_flow_capture; ///< Capture efficiency at peak flow (%)
    std::vector<double> avg_flow_capture;  ///< Σ capture efficiency over capture periods
    std::vector<double> bypass_freq;       ///< # capture periods that also bypassed

    // --- volume statistics (Gap #68 → InletUsageStore) -----------------------
    std::vector<double> stat_capture_vol;  ///< ft³
    std::vector<double> stat_bypass_vol;   ///< ft³
    std::vector<double> stat_backflow_vol; ///< ft³
    std::vector<double> stat_peak_flow;    ///< Peak captured flow (cfs)

    /**
     * @brief Reset every inlet runtime/design/statistic column to n rows.
     *
     * Assigns defaults/zero statistics and-1 topology references, invalidating element views as vectors
     * reallocate. Allocation can leave partial reset; does not resize InletSolver node scratch itself.
     *
     * @param[in] n Nonnegative count; negative is unchecked before size_t conversion.
     */
    void resize(int n);
};

class InletSolver {
public:
    /**
     * @brief Resolve inlet deployments into native geometry, topology, placement and backflow weights.
     *
     * Resets SoA/statistics, converts lengths/flow limits once to ft/cfs and slopes percent to
     * fractions, finds node-host approach links or conduit downstream bypass, resolves AUTOMATIC sag
     * placement and seeds carried-over side count. Backflow weights deliberately use the legacy global
     * inlet bucket. No-op after resetting count when no usages; no complete geometry/reference
     * validation. Allocation can leave partial solver state; rerun after authoring edits, serialize all
     * solver use.
     *
     * @param[in] ctx Borrowed resolved model with coherent inlet usage/design/street/link/node/curve
     *     registries; authoring lengths/flows stay in project units.
     */
    void init(SimulationContext& ctx);

    /**
     * @brief Evaluate capture/backflow and add inlet exchanges to node lateral flows.
     *
     * Runs before hydraulic routing after external lateral assembly. Processes reverse authored order
     * with carried-over street sides, computes approach/design capacity and previous capture-node
     * overflow backflow, then applies selected non-DW/node-host water limits. Subtracts net capture at
     * bypass and adds capture at receiving node, accumulating ft3/flow statistics. Performance counters
     * require current_date>report_start. Scratch can resize if node count grew; no full topology
     * rebind, generic rollback or finite validation.
     *
     * @param[in,out] ctx Borrowed live hydraulic state and aligned init mappings; modifies
     *     nodes.lat_flow and reads previous link flows/overflow.
     * @param[in] dt Positive finite routing interval seconds for donor-volume caps and cumulative
     *     volumes.
     */
    void computeAll(SimulationContext& ctx, double dt);

    /**
     * @brief Add pollutant inflow volume and mass-rate contributions for net inlet exchange.
     *
     * Call after hydraulic routing before quality routing. Positive net capture uses bypass old
     * concentration at capture receiver; negative net uses capture old concentration at bypass
     * receiver. Skips absent/invalid endpoints and no-inlet/no-pollutant cases. Does not subtract
     * sender mass itself, convert concentration units or multiply mass-rate by dt here.
     *
     * @param[in,out] ctx Borrowed live quality arrays with valid node-sized qual_vol_in and
     *     pollutant-strided old concentrations/mass-rate accumulators.
     * @param[in] dt Routing interval seconds; qNet*dt adds ft3 while qNet*old concentration adds
     *     mass-rate in existing quality conventions.
     */
    void adjustQualInflows(SimulationContext& ctx, double dt);

    /**
     * @brief Credit current capture-node overflow returned to inlet corridors out of flooding ledgers.
     *
     * Call after system outflow removal. Visits each marked capture node once and subtracts its entire
     * positive current overflow, independent of legacy per-inlet backflow ratios. Step flooding rate
     * and cumulative flooding may become slightly negative; no clamp. Does not modify hydraulic
     * overflow or inlet capture columns.
     *
     * @param[in,out] ctx Borrowed aligned current overflow/concentration and mass-balance arrays;
     *     updates global water/pollutant flood totals.
     * @param[in] dt Routing seconds; water volume q*dt ft3 and pollutant ledger q*concentration*dt.
     */
    void adjustFloodingTotals(SimulationContext& ctx, double dt) const;

    /**
     * @brief Publish retained inlet statistics to model usage and reporting stores.
     *
     * Copies ft3 captured/bypass/backflow totals, peak cfs and legacy period/efficiency sums; does not
     * normalize average sums or reset runtime statistics. Allocation can leave partial publication;
     * call before report generation.
     *
     * @param[in,out] ctx Borrowed model whose inlet_usages stat vectors and inlet_diag are
     *     resized/overwritten for min(runtime count,usage count).
     */
    void gatherStats(SimulationContext& ctx) const;

    /**
     * @brief Borrow read-only resolved inlet/runtime/statistic columns.
     * @return Owned SoA reference; element views invalidate on init/resize/destruction.
     */
    const InletSoA& soa() const noexcept { return soa_; }
    /**
     * @brief Check whether any runtime usage rows are allocated.
     * @return True when soa.count>0; does not establish valid topology or active capture.
     */
    bool hasInlets() const noexcept { return soa_.count > 0; }

private:
    InletSoA soa_;

    /// Scratch, sized to the node count in init(): legacy `InletFlow[]` and the
    /// non-DW `Node[].inflow` pre-pass (inlet.c:572-579). Members so computeAll
    /// allocates nothing (it is called from a noexcept assembly step).
    std::vector<double> node_inlet_flow_;
    std::vector<double> node_inflow_;

    /// Legacy inlet.c's file-scope `Nsides`, which OUTLIVES each inlet and each
    /// routing step (a static, zero at the first evaluation). getOnSagCaptured-
    /// Flow reads it for `totalInlets = Nsides * numInlets` BEFORE
    /// getConduitGeometry refreshes it, so an on-sag inlet is scaled by the
    /// street sides of the PREVIOUSLY processed inlet — a HalfStreet sag inlet
    /// captures double when the inlet before it sat on a FullStreet
    /// (example7-inlets' Street5). Reproduced deliberately; see computeAll.
    int nsides_prev_ = 0;

    /// Per-node flag for legacy's `Node[].inlet == CAPTURE` (inlet.c:512),
    /// set in init() for every valid usage's capture node. Sized to the node
    /// count; empty when the model has no inlets.
    std::vector<uint8_t> is_capture_node_;

    /**
     * @brief Assemble a copied native geometry record for one runtime inlet.
     *
     * Adds local depression when depression*width>0, substitutes local width, then
     * beta=1.486*sqrt(slope)/n for positive slope/n. No allocation/validation or section ownership
     * transfer.
     *
     * @param[in] ctx Borrowed context, currently unused.
     * @param[in] ii Valid runtime row with aligned columns, unchecked.
     * @param[in] xs Optional borrowed channel section pointer copied into returned view; must outlive
     *     subsequent kernel use.
     * @return Value geometry with local depression override and computed gutter slope/beta; spread
     *     starts at its default until capture sets it.
     */
    Geom geomOf(const SimulationContext& ctx, int ii, const XSectParams* xs) const;
    /**
     * @brief Copy one native inlet design from resolved runtime columns.
     *
     * @param[in] ii Valid zero-based runtime row with aligned design columns.
     * @return Owned scalar Design, with ft dimensions/ft per second splash velocity; no validation.
     */
    Design designOf(int ii) const;
    /**
     * @brief Copy replicate, clog and native flow-limit settings from one runtime row.
     *
     * @param[in] ii Valid zero-based row with aligned usage columns.
     * @return Value copy; flow_limit cfs, clog dimensionless.
     */
    UsageParams usageOf(int ii) const;

    /**
     * @brief Accumulate one reported inlet performance sample.
     *
     * Returns for near-zero approach without backflow. Updates flow/capture/backflow period counts,
     * capped capture-efficiency sum, bypass frequency and peak approach/percent capture; does not add
     * volume statistics or check report window itself.
     *
     * @param[in] ii Valid runtime row with current capture/backflow already stored.
     * @param[in] q Nonnegative approach flow cfs; caller ensures meaningful q for a positive net
     *     capture ratio.
     */
    void updateStats(int ii, double q);

    /**
     * @brief Compute legacy global-bucket backflow shares across all valid inlet usages.
     *
     * Walks reverse authored order, summing effective open areas and custom replicate counts into ONE
     * model-wide bucket, reproducing the legacy capture-node indexing quirk. Shares standard area
     * proportionally by fraction of standard usages, custom counts by remaining fraction. Does not
     * group by individual capture node; node-index-negative usages are ignored. No-inlet/no-valid-row
     * returns preserve prior weights.
     */
    void computeBackflowRatios();
};

} // namespace inlet
} // namespace openswmm

#endif // OPENSWMM_INLET_HPP
