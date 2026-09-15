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
 * @file Inlet.hpp
 * @brief Street inlet capture efficiency — FHWA HEC-22.
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
 *          their state through @ref inlet::Geom / @ref inlet::Design /
 *          @ref inlet::UsageParams instead of legacy's module-level shared
 *          variables, so they are directly unit-testable.
 *
 * @note Legacy reference: src/legacy/engine/inlet.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
    GRATE       = 0,
    CURB        = 1,
    COMBO       = 2,
    SLOTTED     = 3,
    DROP_GRATE  = 4,
    DROP_CURB   = 5,
    CUSTOM      = 6
};

/// Legacy `enum GrateType` (inlet.c:124-127).
enum class GrateType : int {
    P_BAR_50      = 0,
    P_BAR_50x100  = 1,
    P_BAR_30      = 2,
    CURVED_VANE   = 3,
    TILT_BAR_45   = 4,
    TILT_BAR_30   = 5,
    RETICULINE    = 6,
    GENERIC       = 7
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
    double sx      = 0.01;   ///< Street cross slope (fraction)
    double sl      = 0.0;    ///< Conduit longitudinal slope (fraction)
    double sw      = 0.01;   ///< Gutter + cross slope
    double a       = 0.0;    ///< Gutter depression (ft)
    double w       = 0.0;    ///< Gutter width (ft)
    double t       = 0.0;    ///< Top width of flow spread (ft) — set by getFlowSpread
    double n       = 0.016;  ///< Manning's roughness
    int    nsides  = 1;      ///< 1- or 2-sided street
    double t_crown = 100.0;  ///< Distance from curb to crown (ft)
    double beta    = 0.0;    ///< 1.486·√SL / n
    double qfactor = 0.0;    ///< Izzard's f in Q = f·T^2.67

    /// Host conduit cross-section — non-null only for the DROP_* (channel)
    /// designs, whose grate kernel needs A(S)/Y(A)/W(Y) of the channel.
    const XSectParams* xs = nullptr;
};

/// One inlet design in INTERNAL units (legacy `TInletDesign`, inlet.c:66-74).
struct Design {
    int    type            = 0;    ///< inlet::InletType
    int    grate_type      = static_cast<int>(GrateType::GENERIC);
    double grate_length    = 0.0;  ///< ft (0 ⇒ no grate half)
    double grate_width     = 0.0;  ///< ft
    double frac_open_area  = 0.0;  ///< GENERIC grates only
    double splash_veloc    = 0.0;  ///< ft/s, GENERIC grates only
    double curb_length     = 0.0;  ///< ft (0 ⇒ no curb half)
    double curb_height     = 0.0;  ///< ft
    int    curb_throat     = static_cast<int>(ThroatAngle::VERTICAL);
    double slotted_length  = 0.0;  ///< ft (0 ⇒ not a slotted drain)
    double slotted_width   = 0.0;  ///< ft
};

/// The per-placement half of one usage row (legacy `TInlet` fields).
struct UsageParams {
    int    num_inlets  = 1;
    double clog_factor = 1.0;   ///< 1 − %clog/100
    double flow_limit  = 0.0;   ///< cfs per inlet; 0 ⇒ unlimited
};

// ============================================================================
// Pure kernel — 1:1 ports of the legacy static functions
// ============================================================================

/// HEC-22 Eq(4-4) solved for Eo with Ts/w substituted for (T/w)−1 (inlet.c:1248).
double getEo(double sr, double ts, double w);

/// Width of flow spread, HEC-22 Eq(4-2)/(4-6) (inlet.c:1200).
double getFlowSpread(const Geom& g, double Q);

/// Ratio of the flow over width `w` to the total street flow, Eq(4-16) (inlet.c:1518).
double getGutterFlowRatio(const Geom& g, double w);

/// Ratio of the flow area above the grate to that above the depressed gutter,
/// Eq(4-20a) numerator/denominator (inlet.c:1535).
double getGutterAreaRatio(const Geom& g, double grate_width, double area);

/// Splash-over velocity for a standard grate type (HEC-22 Chart 5B, inlet.c:1557).
/// Returns 0 for GENERIC / out-of-range types — those carry a user velocity.
double getSplashOverVelocity(int grate_type, double grate_length);

/// On-grade grate capture, Eq(4-16)/(4-18)/(4-19)/(4-20a)/(4-21) (inlet.c:1387).
/// Reads `g.t` (spread) and may update `g.t` / `g.sx` for a channel drop grate.
double getGrateInletCapture(const Design& d, Geom& g, double Q);

/// On-grade curb-opening capture, Eq(4-22a)/(4-23)/(4-24) (inlet.c:1477).
double getCurbInletCapture(const Geom& g, double Q, double opening_length);

/// Capture of a single on-grade inlet — type dispatch + COMBO sweeper (inlet.c:1325).
double getOnGradeInletCapture(const Design& d, Geom& g, double Q, double depth);

/// Total capture of an on-grade usage: `num_inlets` in series, both sides (inlet.c:1267).
double getOnGradeCapturedFlow(const Design& d, const UsageParams& u, Geom& g,
                              double q, double depth);

/// On-sag grate weir/orifice flows, Eq(4-26)/(4-27) (inlet.c:1644).
void findOnSagGrateFlows(const Design& d, const Geom& g, double depth,
                         double* weir_flow, double* orifice_flow);

/// On-sag curb weir/orifice flows with the transition band, Eq(4-28)/(4-30)
/// (inlet.c:1703). A DROP_CURB opening acts on all four sides (`L × 4`).
void findOnSagCurbFlows(const Design& d, const Geom& g, double depth,
                        double opening_length,
                        double* weir_flow, double* orifice_flow);

/// Curb-opening orifice flow with the throat-angle head, Eq(4-31a) (inlet.c:1764).
double getCurbOrificeFlow(double flow_depth, double opening_height,
                          double opening_length, int throat_angle);

/// On-sag slotted drain flow, Eq(4-32)/(4-33) (inlet.c:1785).
double getOnSagSlottedFlow(const Design& d, double depth);

/// Capture of a single on-sag inlet (inlet.c:1610).
double getOnSagInletCapture(const Design& d, const Geom& g, double depth);

/// Total capture of an on-sag usage: single inlet × `nsides · num_inlets` (inlet.c:1574).
double getOnSagCapturedFlow(const Design& d, const UsageParams& u,
                            const Geom& g, double depth);

/**
 * @brief Capture of a CUSTOM inlet from its diversion / rating curve (inlet.c:1902).
 *
 * @param curve      Resolved capture curve (`InletStore::curve_index`).
 * @param curve_kind inlet::CurveKind — DIVERSION (capture vs approach flow) or
 *                   RATING (capture vs depth).
 * @param ucf_flow   internal → display flow factor (curve x/y are user units).
 * @param ucf_len    internal → display length factor.
 */
double getCustomCapturedFlow(const UsageParams& u, const Geom& g,
                             const Table& curve, int curve_kind,
                             double q, double depth,
                             double ucf_flow, double ucf_len);

/// Unclogged open area of one usage (ft²) — legacy getInletArea (inlet.c:1870).
double getInletArea(const Design& d, const UsageParams& u);

// ============================================================================
// Runtime SoA
// ============================================================================

struct InletSoA {
    int count = 0;

    // --- host / topology -----------------------------------------------------
    std::vector<int>    link_idx;       ///< Host conduit link index (−1 for a node host)
    std::vector<int>    host_node;      ///< Host inlet-junction node (−1 for a conduit host)
    std::vector<int>    node_idx;       ///< Capture (receiving) node index
    std::vector<int>    bypass_node;    ///< Node the uncaptured flow stays at
    std::vector<int>    up_link;        ///< Approach conduit (node host: node2 == host)
    std::vector<int>    up_link2;       ///< Second inbound conduit at a sag node host (−1)
    std::vector<uint8_t> two_inbound;   ///< 1 when both attached conduits flow toward the host

    // --- design --------------------------------------------------------------
    std::vector<int>    inlet_type;
    std::vector<int>    grate_type;
    std::vector<double> grate_length;   ///< ft
    std::vector<double> grate_width;    ///< ft
    std::vector<double> open_area;      ///< GENERIC open-area fraction
    std::vector<double> splash_veloc;   ///< ft/s (GENERIC)
    std::vector<double> curb_length;    ///< ft
    std::vector<double> curb_height;    ///< ft
    std::vector<int>    curb_throat;    ///< 0=HORIZONTAL 1=INCLINED 2=VERTICAL
    std::vector<double> slotted_length; ///< ft
    std::vector<double> slotted_width;  ///< ft
    std::vector<int>    curve_index;    ///< CUSTOM: index into ctx.tables
    std::vector<int>    curve_kind;     ///< CUSTOM: inlet::CurveKind

    // --- usage ---------------------------------------------------------------
    std::vector<int>    num_inlets;
    std::vector<double> clog_factor;
    std::vector<double> flow_limit;     ///< cfs per inlet (0 = unlimited)
    std::vector<double> local_depress;  ///< ft
    std::vector<double> local_width;    ///< ft
    std::vector<int>    placement;      ///< as authored (inlet::Placement)
    std::vector<uint8_t> is_sag;        ///< resolved placement: 1 = ON_SAG

    // --- street / conduit geometry (internal units) --------------------------
    std::vector<double> sx;                 ///< Cross slope (fraction)
    std::vector<double> gutter_depression;  ///< ft
    std::vector<double> gutter_width;       ///< ft
    std::vector<double> road_roughness;     ///< Manning's n
    std::vector<double> t_crown;            ///< ft
    std::vector<int>    n_sides;
    std::vector<double> slope;              ///< Host/approach conduit slope
    std::vector<double> flow_factor;        ///< Izzard f = (0.56/n)·√SL·Sx^1.67

    // --- per-step results ----------------------------------------------------
    std::vector<double> q_approach;     ///< Approach flow seen this step (cfs)
    std::vector<double> flow_capture;   ///< Captured flow rate (cfs)
    std::vector<double> backflow;       ///< Backflow from the capture node (cfs)
    std::vector<double> backflow_ratio; ///< inlet backflow / capture-node overflow

    // --- legacy statistics (inlet.c:78-87, updateInletStats :985) ------------
    std::vector<int>    flow_periods;
    std::vector<int>    capture_periods;
    std::vector<int>    backflow_periods;
    std::vector<double> peak_flow;          ///< Peak approach flow (cfs)
    std::vector<double> peak_flow_capture;  ///< Capture efficiency at peak flow (%)
    std::vector<double> avg_flow_capture;   ///< Σ capture efficiency over capture periods
    std::vector<double> bypass_freq;        ///< # capture periods that also bypassed

    // --- volume statistics (Gap #68 → InletUsageStore) -----------------------
    std::vector<double> stat_capture_vol;   ///< ft³
    std::vector<double> stat_bypass_vol;    ///< ft³
    std::vector<double> stat_backflow_vol;  ///< ft³
    std::vector<double> stat_peak_flow;     ///< Peak captured flow (cfs)

    void resize(int n);
};

class InletSolver {
public:
    /// Resolve designs, street geometry, host topology and placement.
    void init(SimulationContext& ctx);

    /**
     * @brief Compute capture + backflow and fold them into `nodes.lat_flow`.
     *
     * @details Runs at the END of `SWMMEngine::assembleLateralInflows`, i.e.
     *          BEFORE the router sees `lat_flow` — matching legacy
     *          `inlet_findCapturedFlows` at routing.c:244 (plan G6).
     *
     * @param ctx  Simulation context.
     * @param dt   Routing timestep (seconds).
     */
    void computeAll(SimulationContext& ctx, double dt);

    /**
     * @brief Adjust quality inflows at the bypass and capture nodes.
     *
     * @details Called after hydraulic routing, before quality routing; moves
     *          pollutant mass by the net flow direction. Matches legacy
     *          `inlet_adjustQualInflows()`. Covers both host kinds.
     */
    void adjustQualInflows(SimulationContext& ctx, double dt);

    /**
     * @brief Take the backflow returned to the street back out of the
     *        system flooding totals.
     *
     * @details A capture node's overflow is handed back to the street through
     *          its inlets as backflow (computeAll), so it never leaves the
     *          system — booking it as flooding counts the same water once as a
     *          loss and once as water put back on the corridor. Matches legacy
     *          `inlet_adjustQualOutflows()` (inlet.c:706), called from
     *          routing.c:260 directly after `removeSystemOutflows()`.
     *
     * @param ctx  Simulation context.
     * @param dt   Routing timestep (seconds).
     */
    void adjustFloodingTotals(SimulationContext& ctx, double dt) const;

    /// Copy accumulated statistics into `ctx.inlet_usages` (volumes, Gap #68)
    /// and `ctx.inlet_diag` (the legacy per-inlet performance block the report
    /// needs). Called by SWMMEngine::report() before the summary tables.
    void gatherStats(SimulationContext& ctx) const;

    const InletSoA& soa() const noexcept { return soa_; }
    bool hasInlets() const noexcept { return soa_.count > 0; }

private:
    InletSoA soa_;

    /// Scratch, sized to the node count in init(): legacy `InletFlow[]` and the
    /// non-DW `Node[].inflow` pre-pass (inlet.c:572-579). Members so computeAll
    /// allocates nothing (it is called from a noexcept assembly step).
    std::vector<double> node_inlet_flow_;
    std::vector<double> node_inflow_;

    /// Assemble the kernel geometry block for row `ii` (legacy getConduitGeometry).
    Geom geomOf(const SimulationContext& ctx, int ii,
                const XSectParams* xs) const;
    /// Assemble the design block for row `ii`.
    Design designOf(int ii) const;
    /// Assemble the usage block for row `ii`.
    UsageParams usageOf(int ii) const;

    /// Legacy updateInletStats (inlet.c:985) for row `ii` at approach flow `q`.
    void updateStats(int ii, double q);

    /// Pre-compute backflow_ratio[] from the capture-node topology
    /// (legacy getBackflowRatios, with the inlet.c:1836 node-index bug fixed).
    void computeBackflowRatios();
};

} // namespace inlet
} // namespace openswmm

#endif // OPENSWMM_INLET_HPP
