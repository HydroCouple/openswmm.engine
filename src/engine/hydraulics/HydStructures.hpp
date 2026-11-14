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
 * @file      HydStructures.hpp
 * @brief     Non-conduit link flow: pumps, orifices, weirs, outlets.
 *
 * @details Each structure type is computed in batch over all links of that
 *          type (similar to XSectGroups shape grouping). The flow equations
 *          are pure arithmetic — vectorisable inner loops.
 *
 *          **Batch architecture:**
 *          1. Group links by type at init (like XSectGroups)
 *          2. For each type group: gather head/depth → batch flow calc → scatter
 *
 *          **Pump types:**
 *            Type 1: volume-based curve
 *            Type 2: depth-based curve
 *            Type 3: head-based with speed setting
 *            Type 4: depth-based with dQ/dH
 *            Ideal:  demand-driven
 *
 *          **Orifice:** Q = Cd*A*sqrt(2gH), weir transition at partial fill
 *          **Weir:** Q = Cd*L*H^1.5 (transverse), variants for side/V-notch/trap
 *          **Outlet:** Q = coeff*H^expon or rating curve lookup
 *
 * @note Legacy reference: src/legacy/engine/link.c (pump/orifice/weir/outlet sections)
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_HYD_STRUCTURES_HPP
#define OPENSWMM_HYD_STRUCTURES_HPP

#include "../data/LinkData.hpp"
#include "../data/NodeData.hpp"
#include "../data/TableData.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;
class SWMMEngine;

namespace hydstruct {

// ============================================================================
// Constants
// ============================================================================

constexpr double GRAVITY = 32.2;
constexpr double FUDGE = 0.0001;

// ============================================================================
// Flap gates
// ============================================================================

/**
 * @brief Test reverse flow against the link gate and its donor outfall gate.
 *
 * Link gate tests q*stored direction<0, preserving authored orientation. Outfall test uses the
 * inflow/donor end. Does not mutate flow, apply hydraulic caps or validate arbitrary indices.
 *
 * @param[in] ctx Borrowed coherent link direction/gate flags and node/outfall subtype data.
 * @param[in] j Valid global link index.
 * @param[in] n1 Valid current node1 endpoint index when q>0.
 * @param[in] n2 Valid current node2 endpoint index when q<0.
 * @param[in] q Signed flow/direction indicator; only sign is used, positive node1 to node2.
 * @return True for reverse link-gate flow or flow donated by a gated outfall; false for zero/no
 *     applicable gate.
 */
bool flapGateBlocks(const SimulationContext& ctx, int j, int n1, int n2, double q);

// ============================================================================
// Per-type SoA groups (like XSectGroups but for structure types)
// ============================================================================

struct PumpGroup {
    int count = 0;
    std::vector<int> link_idx;   ///< Global link index
    std::vector<int> curve_idx;  ///< Pump curve table index
    std::vector<int> curve_type; ///< Pump type (1-5, 6=ideal)
    std::vector<double> speed;   ///< Current speed setting [0-1]
    std::vector<double> y_on;    ///< Startup depth
    std::vector<double> y_off;   ///< Shutoff depth
    /**
     * @brief Resize PumpGroup columns while preserving existing row prefixes.
     *
     * Sets count then resizes every declared vector; new curve index-1, curve type0, speed1 and
     * thresholds0. Link indices value-initialize on growth. Allocation may leave partial resizing and
     * invalidate borrowed element views.
     *
     * @param[in] n Nonnegative count; negative is unchecked before size_t conversion.
     */
    void resize(int n);
};

struct OrificeGroup {
    int count = 0;
    std::vector<int> link_idx;
    std::vector<int> shape; ///< BOTTOM or SIDE
    // No cached cOrif/cWeir/hCrit: legacy orifice_setSetting recomputes them
    // from the CURRENT setting every step, so computeOrificeFlowK derives
    // them inline per call (see the legacy-exact block there) — an init-time
    // snapshot at full opening could only ever go stale.
    // No cached flap-gate flag: the kernels read ctx.links.has_flap_gate (and
    // flapGateBlocks) directly, so a stale init-time snapshot cannot diverge.
    /// Most-recently-computed surface area (ft²) for scatter into node
    /// new_surf_area (matches legacy Orifice[k].surfArea).
    std::vector<double> surf_area;
    /// Equivalent length (ft) used for surface-area computation on SIDE
    /// orifices. Legacy: max(200, 2·routingStep·sqrt(g·yFull)).
    /// Pre-computed at init; constant per simulation.
    std::vector<double> length_eff;
    /**
     * @brief Resize OrificeGroup columns while preserving existing row prefixes.
     *
     * Sets count then resizes every declared vector; new shape/surface-area/effective-length entries0.
     * Link indices value-initialize on growth. Allocation may leave partial resizing and invalidate
     * borrowed element views.
     *
     * @param[in] n Nonnegative count; negative is unchecked before size_t conversion.
     */
    void resize(int n);
};

struct WeirGroup {
    int count = 0;
    std::vector<int> link_idx;
    std::vector<int> weir_type;   ///< TRANSVERSE/SIDE/VNOTCH/TRAPEZOIDAL/ROADWAY (0..4)
    std::vector<double> c_disch1; ///< Main discharge coefficient
    std::vector<double> c_disch2; ///< End section coefficient (trapezoidal)
    std::vector<double> end_con;  ///< End contraction factor
    std::vector<double> slope;    ///< V-notch slope or trap slope
    std::vector<int> cd_curve;    ///< Optional Cd(head) curve index (legacy Weir.cdCurve), -1 none
    std::vector<double> road_width; ///< ROADWAY weir road width (ft, legacy Weir.roadWidth)
    std::vector<int> road_surface;  ///< ROADWAY weir surface: 0 none, 1 PAVED, 2 GRAVEL
    // No cached flap-gate flag — see the note on OrificeGroup.
    /// Most-recently-computed surface area (ft²), populated by
    /// computeWeirFlows and scattered to node surface-area accumulators
    /// by the non_conduit_fn callback. Matches legacy Weir[k].surfArea.
    std::vector<double> surf_area;
    /// Effective length used for surface-area computation
    /// (legacy Weir[k].length = max(200, 2·routeStep·sqrt(g·yFull))).
    /// Pre-computed at init; constant per simulation.
    std::vector<double> length_eff;
    /// Surcharge YES/NO (legacy Weir.canSurcharge): when NO and h1 >= hcrown,
    /// the head is capped at the weir opening height instead of switching to
    /// the equivalent-orifice formulation (legacy link.c weir_getInflow).
    std::vector<uint8_t> can_surcharge;
    /**
     * @brief Resize WeirGroup columns while preserving existing row prefixes.
     *
     * Sets count then resizes every declared vector; new numerical fields0, curve index-1 and
     * can_surcharge1. Link indices value-initialize on growth. Allocation may leave partial resizing
     * and invalidate borrowed element views.
     *
     * @param[in] n Nonnegative count; negative is unchecked before size_t conversion.
     */
    void resize(int n);
};

struct OutletGroup {
    int count = 0;
    std::vector<int> link_idx;
    std::vector<int> curve_idx;  ///< Rating curve (-1 = power law)
    std::vector<double> q_coeff; ///< Power law coefficient
    std::vector<double> q_expon; ///< Power law exponent
    /**
     * @brief Resize OutletGroup columns while preserving existing row prefixes.
     *
     * Sets count then resizes every declared vector; new curve index-1, coefficient0 and exponent1.
     * Link indices value-initialize on growth. Allocation may leave partial resizing and invalidate
     * borrowed element views.
     *
     * @param[in] n Nonnegative count; negative is unchecked before size_t conversion.
     */
    void resize(int n);
};

// ============================================================================
// Structure flow solver
// ============================================================================

class StructureSolver {
public:
    /**
     * @brief Build per-type structure groups and global sequential-dispatch maps.
     *
     * Copies pump curves/thresholds, raw weir/outlet coefficients and selected geometry, derives
     * effective orifice/weir lengths max(200ft,2*routingStep*sqrt(g*y_full)), and includes DUMMY
     * conduits in the nonconduit index list. Missing/nonpump pump curves select ideal behavior. Does
     * not evaluate flows or validate all authoring; allocation can leave partial caches. Reinitialize
     * after cached authoring/topology edits.
     *
     * @param[in,out] ctx Borrowed resolved model with valid subtype rows/endpoints and geometry; pump
     *     curve_type is written back to its relational store.
     */
    void init(SimulationContext& ctx);

    /**
     * @brief Evaluate grouped pumps, orifices, weirs and outlets in type order.
     *
     * Pumps run first, then orifices/weirs, then outlets. Only orifices add node surface area; weirs
     * retain a group diagnostic area but do not scatter it. Does not add link flows to node
     * inflow/outflow accumulators or reset all dqdh fields before each type; sequential DW routing uses
     * computeNonConduitFlowOne instead.
     *
     * @param[in,out] ctx Borrowed resolved native node/link/subtype/curve stores; links
     *     flow/depth/class/dqdh and selected table cursors may change. Caller serializes evaluation.
     * @param[in] dt Positive routing seconds used for pump donor limits.
     * @param[in,out] node_new_surf_area Optional writable n_nodes ft2 accumulator. Pump evaluation
     *     reads donor area; orifices add eligible endpoint area; NULL skips those effects.
     */
    void computeAllFlows(SimulationContext& ctx, double dt, double* node_new_surf_area = nullptr);

    /**
     * @brief Apply pump startup/shutoff hysteresis once before step iterations.
     *
     * For each valid upstream node resets target to current setting, then turns on/off at strict
     * positive startup/shutoff thresholds. Does not immediately write setting or compute flow;
     * computePumpFlowK adopts target. Repeating after depths change within Picard iterations changes
     * the intended hysteresis timing.
     *
     * @param[in,out] ctx Borrowed current start-of-step node depths ft and pump setting/target columns.
     */
    void updatePumpTargetSettings(SimulationContext& ctx);

    /// @brief Get pre-built list of all non-conduit link indices.
    const std::vector<int>& nonConduitIndices() const noexcept { return nc_indices_; }

    /**
     * @brief Evaluate one sequential nonconduit or DUMMY link using current node accumulators.
     *
     * DUMMY conduit passes upstream inflow+overflow/divider split, subject to setting and flow limit.
     * Other valid mapped types reset dqdh then dispatch one type-group row. Caller must scatter
     * resulting link flow/derivative to node accumulators before evaluating a later structure,
     * preserving shared-wet-well ordering. No generic rollback or finite/input validation.
     *
     * @param[in,out] ctx Borrowed resolved native node/link/subtype/curve stores; links
     *     flow/depth/class/dqdh and selected table cursors may change. Caller serializes evaluation.
     * @param[in] dt Positive routing seconds for pump/storage donor limits.
     * @param[in,out] node_new_surf_area Optional writable n_nodes ft2 accumulator. Pump evaluation
     *     reads donor area; orifices add eligible endpoint area; NULL skips those effects.
     * @param[in] link_idx Valid global link index selected from nonConduitIndices; ordinary conduits
     *     must not be passed.
     */
    void computeNonConduitFlowOne(SimulationContext& ctx, double dt, double* node_new_surf_area,
                                  int link_idx);

    /**
     * @brief Scatter a bypassed orifice's retained area using current flow classification.
     *
     * Uses the previous computed orifice area; half at each end except storage/critical-end
     * suppression. Does not reevaluate opening, flow or depth; other structure types contribute no held
     * area.
     *
     * @param[in] ctx Borrowed native endpoints/node types/link class; no hydraulic values are mutated.
     * @param[in,out] node_new_surf_area Optional writable n_nodes ft2 accumulator; NULL no-op.
     * @param[in] link_idx Valid global link index; nonorifices, absent mapped rows or negative
     *     endpoints skip.
     */
    void scatterHeldSurfArea(SimulationContext& ctx, double* node_new_surf_area, int link_idx);

private:
    friend class openswmm::SWMMEngine;
    PumpGroup pumps_;
    OrificeGroup orifices_;
    WeirGroup weirs_;
    OutletGroup outlets_;
    std::vector<int> nc_indices_; ///< All non-conduit link indices
    std::vector<int> nc_group_k_; ///< link index → row in its type group (-1 = none)

    /**
     * @brief Evaluate every cached pump row using current targets and donor conditions.
     *
     * Calls computePumpFlowK in group order without scattering node flow accumulators; caller supplies
     * desired inter-pump sequencing. No hysteresis recomputation here.
     *
     * @param[in,out] ctx Borrowed resolved native node/link/subtype/curve stores; links
     *     flow/depth/class/dqdh and selected table cursors may change. Caller serializes evaluation.
     * @param[in] dt Positive routing seconds.
     * @param[in] node_new_surf_area Optional readable n_nodes ft2 donor areas; NULL uses MIN_SURFAREA
     *     fallback for eligible nonstorage pumps.
     */
    void computePumpFlows(SimulationContext& ctx, double dt, const double* node_new_surf_area);

    // Per-element bodies of the batch loops (k = row in the type group).
    /**
     * @brief Evaluate one pump curve/ideal inflow and apply its donor-water limiter.
     *
     * Copies target_setting to setting. Types1/2 use interval curves,3/5 head curves with type5
     * affinity scaling,4 depth curve,6 native node inflow+overflow. Converts curve units to cfs then
     * setting-scales; records applicable derivatives/classes and caps selected donor paths using old
     * volume/inflow. Does not reset every dqdh on early exits or scatter node flows; sequential caller
     * performs those steps.
     *
     * @param[in,out] ctx Borrowed resolved native node/link/subtype/curve stores; links
     *     flow/depth/class/dqdh and selected table cursors may change. Caller serializes evaluation.
     * @param[in] dt Positive routing seconds for old-volume/depth donor limits.
     * @param[in] node_new_surf_area Optional readable n_nodes ft2 area; exact supplied values
     *     including0 used by type2/3/4 nonstorage limiter.
     * @param[in] k Valid zero-based pump-group row, not global link index.
     */
    void computePumpFlowK(SimulationContext& ctx, double dt, const double* node_new_surf_area,
                          int k);
    /**
     * @brief Evaluate one setting-adjusted bottom/side orifice and endpoint area.
     *
     * Derives coefficients from current opening, uses native ft/cfs, dry/flap/critical classification,
     * ARMCO gate loss and Villemonte submergence. Exact closed setting keeps prior depth/class/area and
     * scatters held area; dry/gated exits use FUDGE*effective-length area. Writes flow/dqdh/depth and
     * cached area; does not book node flows or clamp general invalid geometry. Optional environment
     * trace writes share static file state, so trace use also requires serialization.
     *
     * @param[in,out] ctx Borrowed resolved native node/link/subtype/curve stores; links
     *     flow/depth/class/dqdh and selected table cursors may change. Caller serializes evaluation.
     * @param[in,out] node_new_surf_area Optional writable n_nodes ft2 accumulator. Pump evaluation
     *     reads donor area; orifices add eligible endpoint area; NULL skips those effects.
     * @param[in] k Valid zero-based orifice-group row, not global link index.
     */
    void computeOrificeFlowK(SimulationContext& ctx, double* node_new_surf_area, int k);
    /**
     * @brief Evaluate one weir with raw authored coefficients and current setting.
     *
     * Applies design-crest direction, partial-opening geometry, optional Cd curve, ARMCO/Villemonte
     * corrections and optional surcharge-orifice law; roadway uses its HDS tables before the ordinary
     * flap branch. Evaluates raw coefficient formula in project display dimensions then converts to
     * cfs, preserving operation order. Writes flow/depth/dqdh and cached area; closed setting retains
     * prior depth/class/area. Does not scatter any weir area to nodes or provide a validity status.
     *
     * @param[in,out] ctx Borrowed resolved native node/link/subtype/curve stores; links
     *     flow/depth/class/dqdh and selected table cursors may change. Caller serializes evaluation.
     * @param[in] node_new_surf_area Compatibility parameter currently unused; NULL permitted.
     * @param[in] k Valid zero-based weir-group row.
     */
    void computeWeirFlowK(SimulationContext& ctx, double* node_new_surf_area, int k);
    /**
     * @brief Evaluate one functional/tabular outlet with crest-based head or depth rating.
     *
     * Uses authored display-unit head/flow rating conventions, current setting multiplier and flap
     * suppression, returning signed cfs. Crest stays tied to declared node1 invert even under reverse
     * flow. Dry/gated result resets flow/depth/class/derivative; exact closed setting keeps previous
     * depth. Ordinary open outlet dqdh is0. No generic donor cap or unit/geometry validation.
     *
     * @param[in,out] ctx Borrowed resolved native node/link/subtype/curve stores; links
     *     flow/depth/class/dqdh and selected table cursors may change. Caller serializes evaluation.
     * @param[in] k Valid zero-based outlet-group row with coherent relational outlet configuration.
     */
    void computeOutletFlowK(SimulationContext& ctx, int k);
    /**
     * @brief Add half an orifice's cached area to each eligible endpoint.
     *
     * Suppresses node1 contribution for UP_CRITICAL/storage and node2 for DN_CRITICAL/storage. Does not
     * recompute orifice area, clear accumulators or check index consistency.
     *
     * @param[in] ctx Borrowed node types/link flow class, no context fields mutated.
     * @param[in,out] node_new_surf_area Optional writable n_nodes ft2 accumulator; NULL no-op.
     * @param[in] uk Valid ORIFICE GROUP row.
     * @param[in] uj Corresponding valid GLOBAL link slot.
     * @param[in] un1 Valid node1 global slot.
     * @param[in] un2 Valid node2 global slot.
     */
    void scatterOrificeSurfArea(SimulationContext& ctx, double* node_new_surf_area, std::size_t uk,
                                std::size_t uj, std::size_t un1, std::size_t un2);

    /**
     * @brief Evaluate each orifice and add eligible endpoint surface-area contributions.
     *
     * Invokes per-row opening/weir/orifice/submergence/gate relations; does not reset caller node areas
     * or scatter flow accumulators.
     *
     * @param[in,out] ctx Borrowed resolved native node/link/subtype/curve stores; links
     *     flow/depth/class/dqdh and selected table cursors may change. Caller serializes evaluation.
     * @param[in,out] node_new_surf_area Optional writable n_nodes ft2 accumulator. Pump evaluation
     *     reads donor area; orifices add eligible endpoint area; NULL skips those effects.
     */
    void computeOrificeFlows(SimulationContext& ctx, double* node_new_surf_area = nullptr);

    /**
     * @brief Evaluate every weir row without adding node surface area.
     *
     * Invokes current-setting/free/surcharge/roadway relations. Cached weir surf_area is a diagnostic,
     * not an endpoint area contribution. Caller owns node flow accumulation.
     *
     * @param[in,out] ctx Borrowed resolved native node/link/subtype/curve stores; links
     *     flow/depth/class/dqdh and selected table cursors may change. Caller serializes evaluation.
     * @param[in] node_new_surf_area Retained compatibility parameter, ignored by the weir path; NULL
     *     permitted.
     */
    void computeWeirFlows(SimulationContext& ctx, double* node_new_surf_area = nullptr);

    /**
     * @brief Evaluate every cached functional/tabular outlet row.
     *
     * Writes native signed cfs/depth/class and zero derivative through computeOutletFlowK; does not
     * scatter node inflow/outflow accumulators.
     *
     * @param[in,out] ctx Borrowed resolved native node/link/subtype/curve stores; links
     *     flow/depth/class/dqdh and selected table cursors may change. Caller serializes evaluation.
     */
    void computeOutletFlows(SimulationContext& ctx);
};

} // namespace hydstruct
} // namespace openswmm

#endif // OPENSWMM_HYD_STRUCTURES_HPP
