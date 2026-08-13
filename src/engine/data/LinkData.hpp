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
 * @file LinkData.hpp
 * @brief Structure-of-Arrays (SoA) storage for all link types.
 *
 * @details Replaces the global `Link[]` array and `TLink`/`TConduit`/`TPump`/
 *          `TWeir`/`TOrifice`/`TOutlet` structs from src/solver/objects.h.
 *
 * Link types:
 * - CONDUIT   — open/closed channel or pipe
 * - PUMP      — pump (fixed or variable speed)
 * - ORIFICE   — inline or side orifice
 * - WEIR      — transverse, side-flow, V-notch, or trapezoidal weir
 * - OUTLET    — flap gate or rating-curve outlet
 *
 * @see Legacy reference: src/solver/objects.h — TLink, TConduit, TPump, TWeir, TOrifice
 * @see Legacy reference: src/solver/globals.h — Link[], Nlinks[]
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_LINK_DATA_HPP
#define OPENSWMM_ENGINE_LINK_DATA_HPP

#include <vector>
#include <cstdint>
#include <string>
#include <algorithm>

namespace openswmm {

// ============================================================================
// Link type enumerations
// ============================================================================

/**
 * @brief Link type codes.
 * @see Legacy: LinkType in src/solver/enums.h
 */
enum class LinkType : int8_t {
    CONDUIT = 0,
    PUMP    = 1,
    ORIFICE = 2,
    WEIR    = 3,
    OUTLET  = 4
};

/**
 * @brief Conduit cross-section shape code.
 * @see Legacy: XsectType in src/solver/enums.h
 */
enum class XsectShape : int16_t {
    CIRCULAR         = 0,
    FILLED_CIRCULAR  = 1,
    RECT_CLOSED      = 2,
    RECT_OPEN        = 3,
    TRAPEZOIDAL      = 4,
    TRIANGULAR       = 5,
    PARABOLIC        = 6,
    POWER            = 7,
    MODBASKETHANDLE  = 8,
    EGGSHAPED        = 9,
    HORSESHOE        = 10,
    GOTHIC           = 11,
    CATENARY         = 12,
    SEMIELLIPTICAL   = 13,
    BASKETHANDLE     = 14,
    SEMICIRCULAR     = 15,
    RECT_TRIANG      = 16,  ///< Rectangular-triangular bottom
    RECT_ROUND       = 17,  ///< Rectangular-round bottom
    HORIZ_ELLIPSE    = 18,  ///< Horizontal elliptical pipe
    VERT_ELLIPSE     = 19,  ///< Vertical elliptical pipe
    ARCH             = 20,  ///< Arch pipe
    IRREGULAR        = 21,  ///< User-supplied shape curve
    CUSTOM           = 22,  ///< Shape from CURVE_SHAPE table
    FORCE_MAIN       = 23,  ///< Circular force main (Hazen-Williams or D-W)
    STREET_XSECT     = 24,  ///< Street cross-section
    DUMMY            = 25   ///< Dummy (no geometry)
};

/**
 * @brief Link flow state.
 * @see Legacy: FlowClass in src/solver/enums.h
 */
enum class FlowClass : int8_t {
    DRY        = 0,
    UP_DRY     = 1,
    DN_DRY     = 2,
    SUBCRITICAL = 3,
    SUPERCRITICAL = 4,
    UP_CRITICAL = 5,
    DN_CRITICAL = 6
};

// ============================================================================
// LinkData — SoA layout
// ============================================================================

/**
 * @brief Structure-of-Arrays storage for all links.
 *
 * @details All arrays indexed by link index [0, count).
 *          Use SimulationContext::link_names to translate name → index.
 *
 * @ingroup engine_data
 */
struct LinkData {

    // -----------------------------------------------------------------------
    // Static connectivity — set at parse time
    // -----------------------------------------------------------------------

    /** @brief Link type. */
    std::vector<LinkType>   type;

    /**
     * @brief Upstream node index.
     * @see Legacy: Link[i].node1
     */
    std::vector<int>        node1;

    /**
     * @brief Downstream node index.
     * @see Legacy: Link[i].node2
     */
    std::vector<int>        node2;

    /**
     * @brief Link offset at upstream node (project length units above invert).
     * @see Legacy: Link[i].offset1
     */
    std::vector<double>     offset1;

    /**
     * @brief Link offset at downstream node.
     * @see Legacy: Link[i].offset2
     */
    std::vector<double>     offset2;

    /**
     * @brief Initial flow rate (project flow units).
     * @see Legacy: Link[i].q0
     */
    std::vector<double>     q0;

    /**
     * @brief Maximum allowable flow rate (project flow units, 0 = unlimited).
     * @see Legacy: Link[i].qLimit
     */
    std::vector<double>     q_limit;

    // -----------------------------------------------------------------------
    // Conduit-specific properties (valid when type[i] == CONDUIT)
    // -----------------------------------------------------------------------

    /** @brief Cross-section shape. */
    std::vector<XsectShape> xsect_shape;

    /** @brief Full depth of cross-section (project length units). */
    std::vector<double>     xsect_y_full;

    /** @brief Full-flow area of cross-section (sq project length units). */
    std::vector<double>     xsect_a_full;

    /** @brief Full hydraulic width of cross-section. */
    std::vector<double>     xsect_w_max;

    /**
     * @brief Raw [XSECTIONS] geometry parameters as supplied (display units),
     *        retained purely for lossless serialization.
     * @details The computational fields above (@ref xsect_y_full, @ref
     *          xsect_w_max, @ref xsect_a_full, @ref xsect_y_bot, @ref
     *          xsect_r_bot) are overwritten with derived geometry during
     *          initialization (e.g. for a trapezoid @ref xsect_w_max becomes
     *          the TOP width, discarding the input bottom width and side
     *          slopes), so they cannot reproduce the original Geom1–Geom4.
     *          These mirror the four columns exactly so swmm_model_write and
     *          swmm_link_get_xsect can round-trip them.  @c xsect_geom1 == 0
     *          marks "not populated" (e.g. objects built by readers that do
     *          not set these), in which case writers fall back to the derived
     *          fields.  Set by swmm_link_set_xsect and the [XSECTIONS] parser.
     */
    std::vector<double>     xsect_geom1;
    std::vector<double>     xsect_geom2;
    std::vector<double>     xsect_geom3;
    std::vector<double>     xsect_geom4;

    /**
     * @brief Shape curve index (for IRREGULAR / CUSTOM shapes).
     * @details -1 for standard shapes.
     */
    std::vector<int>        xsect_curve;

    // Phase 6: conduit-config fields (roughness, length, slope, mod_length,
    // barrels, beta, rough_factor, q_full, q_max) moved to ConduitData in
    // LinkSubtypes.hpp — the relational link side-tables are the single store.

    /**
     * @brief Full-pipe hydraulic radius.
     * @see Legacy: Link[j].xsect.rFull
     */
    std::vector<double>     xsect_r_full;

    /**
     * @brief Full-pipe section factor.
     * @see Legacy: Link[j].xsect.sFull
     */
    std::vector<double>     xsect_s_full;

    /**
     * @brief Maximum section factor (at depth of max conveyance).
     * @see Legacy: Link[j].xsect.sMax
     */
    std::vector<double>     xsect_s_max;

    /**
     * @brief Bottom depth for FILLED_CIRCULAR, RECT_TRIANG, RECT_ROUND shapes.
     * @see Legacy: Link[j].xsect.yBot
     */
    std::vector<double>     xsect_y_bot;

    /**
     * @brief Bottom area for FILLED_CIRCULAR, RECT_TRIANG shapes.
     * @see Legacy: Link[j].xsect.aBot
     */
    std::vector<double>     xsect_a_bot;

    /**
     * @brief Multi-purpose shape param: side slope (RECT_TRIANG), C-factor (FORCE_MAIN),
     *        number of open sides (RECT_OPEN), etc.
     * @see Legacy: Link[j].xsect.sBot
     */
    std::vector<double>     xsect_s_bot;

    /**
     * @brief Multi-purpose shape param: wall length (RECT_TRIANG), wetted perimeter (FILLED_CIRCULAR).
     * @see Legacy: Link[j].xsect.rBot
     */
    std::vector<double>     xsect_r_bot;

    /**
     * @brief Depth at maximum width.
     * @see Legacy: Link[j].xsect.ywMax
     */
    std::vector<double>     xsect_yw_max;

    /**
     * @brief Cached batch (XSectBatch) shape code, translated from XsectShape at init.
     * @details Avoids per-timestep translateShape() switch dispatch.
     *          Set once by Routing::init() or PostParseResolver.
     */
    std::vector<int>        xsect_batch_shape;

    /**
     * @brief Current link setting (0-1 for pumps/orifices/weirs, 1.0 default for conduits).
     * @see Legacy: Link[j].setting
     */
    std::vector<double>     setting;

    /**
     * @brief Target setting from control rules.
     * @see Legacy: Link[j].targetSetting
     */
    std::vector<double>     target_setting;

    /**
     * @brief Absolute date (decimal days) when target_setting last crossed
     *        an open<->closed boundary. Consulted by the control engine for
     *        LINK_TIMEOPEN / LINK_TIMECLOSED premises. Updated by the
     *        routing layer (SWMMEngine::stepRouting) only on open<->closed
     *        transitions, matching legacy routing.c:295-299.
     * @see Legacy: Link[j].timeLastSet
     */
    std::vector<double>     time_last_set;

    /**
     * @brief Flow direction: +1 = node1→node2, -1 = reversed.
     * @see Legacy: Link[j].direction
     */
    std::vector<int>        direction;

    // -----------------------------------------------------------------------
    // Pump-specific properties (valid when type[i] == PUMP)
    // -----------------------------------------------------------------------

    // Phase 6: pump-config fields (pump_curve, pump_init_state, pump_startup,
    // pump_shutoff, pump_curve_type) moved to PumpData in LinkSubtypes.hpp.
    // pump_curve_name stays on base (shared with outlet/conduit-IRREGULAR).

    /** @brief Pump curve name (for deferred resolution). */
    std::vector<std::string> pump_curve_name;

    // -----------------------------------------------------------------------
    // Conduit loss coefficients
    // -----------------------------------------------------------------------

    // Phase 6: conduit loss/seep/culvert + per-step state (loss_inlet,
    // loss_outlet, loss_avg, seep_rate, evap_loss_rate, seep_loss_rate,
    // culvert_code, normal_flow_limited, inlet_control) moved to ConduitData.
    // has_flap_gate and dqdh stay on base (shared / multi-owner).

    /** @brief Flap gate on this conduit (uint8_t: 0=no, 1=yes). @see Legacy: Link[j].hasFlapGate */
    std::vector<uint8_t>    has_flap_gate;

    /**
     * @brief Derivative dQ/dH for inlet-controlled culvert flow.
     * @see Legacy: Link[j].dqdh
     */
    std::vector<double>     dqdh;

    // -----------------------------------------------------------------------
    // Weir / Orifice / Outlet — shared geometric properties
    // -----------------------------------------------------------------------

    // Phase 6: weir/orifice/outlet config (crest_height, cd, param1, param2,
    // orate) moved to OrificeData/WeirData/OutletData in LinkSubtypes.hpp.

    // -----------------------------------------------------------------------
    // State variables — updated each timestep
    // -----------------------------------------------------------------------

    /**
     * @brief Current flow rate (project flow units, +ve = node1→node2).
     * @see Legacy: Link[i].newFlow
     */
    std::vector<double>     flow;

    /**
     * @brief Current water depth at midpoint (project length units).
     * @see Legacy: Link[i].newDepth
     */
    std::vector<double>     depth;

    /**
     * @brief Current full-flow volume (project volume units).
     * @see Legacy: Link[i].newVolume
     */
    std::vector<double>     volume;

    /**
     * @brief Current froude number (absolute).
     * @see Legacy: Link[i].froude
     */
    std::vector<double>     froude;

    /** @brief Current flow class (DRY, SUBCRITICAL, etc.). */
    std::vector<FlowClass>  flow_class;

    /** @brief True if the link is closed by a control rule (uint8_t: 0=no, 1=yes). */
    std::vector<uint8_t>    is_closed;

    // Phase 6: full_state (per-step up/down full bitmask) moved to ConduitData.

    // -----------------------------------------------------------------------
    // Previous-step state
    // -----------------------------------------------------------------------

    /** @brief Flow at the previous timestep. */
    std::vector<double>     old_flow;

    /** @brief Depth at the previous timestep. */
    std::vector<double>     old_depth;

    /** @brief Volume at the previous timestep. */
    std::vector<double>     old_volume;

    // -----------------------------------------------------------------------
    // Per-link quality state — flat 2D: [link * n_pollutants + pollutant]
    // -----------------------------------------------------------------------

    /**
     * @brief Current quality concentration in each link.
     * @details Size = n_links * n_pollutants.
     * @see Legacy: Link[i].newQual[]
     */
    std::vector<double>     conc;

    /** @brief Previous-step quality in each link. */
    std::vector<double>     conc_old;

    /** @brief Number of pollutants in the quality arrays. */
    int                     conc_n_pollutants = 0;

    // -----------------------------------------------------------------------
    // Per-object INP comment
    // -----------------------------------------------------------------------

    /**
     * @brief Object comment from the INP file (';'-prefixed lines immediately
     *        above this link's data row), joined by literal "\\n".
     *        Empty string means no comment.
     */
    std::vector<std::string> comments;

    /**
     * @brief Per-object tag from the INP `[TAGS]` section.
     *
     * @details Free-form string label. Index-keyed so `swmm_link_rename`
     *          keeps the tag attached.
     */
    std::vector<std::string> tags;

    // -----------------------------------------------------------------------
    // Report flag — per-object output filter
    // -----------------------------------------------------------------------

    /** @brief Whether this link is included in report/output (0=no, 1=yes).
     *  @see Legacy: Link[j].rptFlag */
    std::vector<char>       rpt_flag;

    // -----------------------------------------------------------------------
    // Cumulative statistics
    // -----------------------------------------------------------------------

    /**
     * @brief Total volume conveyed by this link (project volume units).
     * @see Legacy: LinkStats[i].volFlow
     */
    std::vector<double>     stat_vol_flow;

    /**
     * @brief Maximum reported flow rate (project flow units).
     * @see Legacy: LinkStats[i].maxFlow
     */
    std::vector<double>     stat_max_flow;

    /**
     * @brief Maximum reported flow velocity (project length/time units).
     * @see Legacy: LinkStats[i].maxVeloc
     */
    std::vector<double>     stat_max_veloc;

    /**
     * @brief Maximum reported filling ratio (depth / full depth).
     * @see Legacy: LinkStats[i].maxFroude
     */
    std::vector<double>     stat_max_filling;

    /**
     * @brief Total duration of surcharge (seconds).
     * @see Legacy: LinkStats[i].timeSurcharged
     */
    std::vector<double>     stat_time_surcharged;

    /**
     * @brief Per-link flow classification step counts.
     * @details Flat 2D: [link * N_FLOW_CLASSES + class], where N_FLOW_CLASSES = 7.
     *          Indexed by FlowClass enum (DRY=0..DN_CRITICAL=6).
     * @see Legacy: LinkStats[i].timeInFlowClass[]
     */
    static constexpr int N_FLOW_CLASSES = 7;
    std::vector<long> stat_flow_class;
    std::vector<long> stat_norm_ltd;      ///< Count of steps with normal flow limiting
    std::vector<long> stat_inlet_ctrl;    ///< Count of steps with inlet control

    /// Date/time when maximum flow occurred (OADate (days since 12/30/1899)).
    /// @see Legacy: LinkStats[i].maxFlowDate
    std::vector<double>     stat_max_flow_date;

    /// Time of upstream surcharge (seconds).
    /// @see Legacy: LinkStats[i].timeFullUpstream
    std::vector<double>     stat_time_full_upstream;

    /// Time of downstream surcharge (seconds).
    /// @see Legacy: LinkStats[i].timeFullDnstream
    std::vector<double>     stat_time_full_dnstream;

    /// Time both ends surcharged (seconds).
    /// @see Legacy: LinkStats[i].timeFullFlow
    std::vector<double>     stat_time_full_both;

    /// Time above full normal flow (seconds).
    /// @see Legacy: LinkStats[i].timeCapacityLimited
    std::vector<double>     stat_time_capacity_limited;

    /**
     * @brief Cumulative pollutant loads transported through each link.
     * @details Flat 2D: [link * n_pollutants + p].
     *          Resized by resize_loads() after pollutant count is known.
     * @see Legacy: Link[i].totalLoad[p]
     */
    std::vector<double>     stat_total_load;
    int                     stat_n_pollutants = 0;

    // -----------------------------------------------------------------------
    // Pump utilization statistics (valid when type[i] == PUMP)
    // -----------------------------------------------------------------------

    /** @brief Number of pump on→off + off→on transitions per pump. */
    std::vector<int>        stat_pump_cycles;

    /** @brief Time pump was running (seconds). */
    std::vector<double>     stat_pump_on_time;

    /** @brief Total volume pumped (ft³). */
    std::vector<double>     stat_pump_volume;

    /**
     * @brief Total pump energy consumed (kWh).
     * @see Legacy PumpStats[].energy, computed as link_getPower(j) * dt / 3600
     *      where link_getPower = |dh| * |q| / 8.814 * KWperHP.
     */
    std::vector<double>     stat_pump_energy;

    /** @brief Previous pump state for cycle detection (true = on). */
    std::vector<bool>       stat_pump_was_on;

    /**
     * @brief Count of flow direction reversals per link.
     * @details Incremented when the sign of (newFlow - oldFlow) reverses and
     *          the magnitude exceeds 0.001 (matching legacy
     *          LinkStats[i].flowTurns).
     * @see Legacy: stats_updateLinkStats()
     */
    std::vector<long>       stat_flow_turns;

    /**
     * @brief Sign of previous flow change for flow-turn detection.
     * @details +1 or -1 indicating the sign of (newFlow - oldFlow) at the
     *          previous routing step (matching legacy LinkStats[i].flowTurnSign).
     * @see Legacy: stats_updateLinkStats()
     */
    std::vector<int>        stat_flow_turn_sign;

    /**
     * @brief CFL time-step critical count per link.
     * @details Incremented when the link's CFL condition produces the smallest
     *          adaptive timestep (matching legacy LinkStats[i].timeCourantCritical).
     * @see Legacy: stats_updateCriticalTimeCount()
     */
    std::vector<double>     stat_time_courant_critical;

    // -----------------------------------------------------------------------
    // Capacity management
    // -----------------------------------------------------------------------

    int count() const noexcept { return static_cast<int>(type.size()); }

    void resize(int n) {
        const auto un = static_cast<std::size_t>(n);

        type.assign(un, LinkType::CONDUIT);
        node1.assign(un, -1);
        node2.assign(un, -1);
        offset1.assign(un, 0.0);
        offset2.assign(un, 0.0);
        q0.assign(un, 0.0);
        q_limit.assign(un, 0.0);

        xsect_shape.assign(un, XsectShape::CIRCULAR);
        xsect_y_full.assign(un, 0.0);
        xsect_a_full.assign(un, 0.0);
        xsect_w_max.assign(un, 0.0);
        xsect_geom1.assign(un, 0.0);
        xsect_geom2.assign(un, 0.0);
        xsect_geom3.assign(un, 0.0);
        xsect_geom4.assign(un, 0.0);
        xsect_curve.assign(un, -1);
        xsect_r_full.assign(un, 0.0);
        xsect_s_full.assign(un, 0.0);
        xsect_s_max.assign(un, 0.0);
        xsect_y_bot.assign(un, 0.0);
        xsect_a_bot.assign(un, 0.0);
        xsect_s_bot.assign(un, 0.0);
        xsect_r_bot.assign(un, 0.0);
        xsect_yw_max.assign(un, 0.0);
        xsect_batch_shape.assign(un, 0);
        setting.assign(un, 1.0);
        target_setting.assign(un, 1.0);
        time_last_set.assign(un, 0.0);
        direction.assign(un, 1);

        has_flap_gate.assign(un, 0);
        dqdh.assign(un, 0.0);
        pump_curve_name.resize(un);

        flow.assign(un, 0.0);
        depth.assign(un, 0.0);
        volume.assign(un, 0.0);
        froude.assign(un, 0.0);
        flow_class.assign(un, FlowClass::DRY);
        is_closed.assign(un, 0);
        old_flow.assign(un, 0.0);
        old_depth.assign(un, 0.0);
        old_volume.assign(un, 0.0);

        comments.assign(un, std::string{});
        tags.assign(un, std::string{});

        rpt_flag.assign(un, 0);

        stat_vol_flow.assign(un, 0.0);
        stat_max_flow.assign(un, 0.0);
        stat_max_veloc.assign(un, 0.0);
        stat_max_filling.assign(un, 0.0);
        stat_time_surcharged.assign(un, 0.0);
        stat_flow_class.assign(un * N_FLOW_CLASSES, 0L);
        stat_norm_ltd.assign(un, 0L);
        stat_inlet_ctrl.assign(un, 0L);
        stat_max_flow_date.assign(un, 0.0);
        stat_time_full_upstream.assign(un, 0.0);
        stat_time_full_dnstream.assign(un, 0.0);
        stat_time_full_both.assign(un, 0.0);
        stat_time_capacity_limited.assign(un, 0.0);
        stat_pump_cycles.assign(un, 0);
        stat_pump_on_time.assign(un, 0.0);
        stat_pump_volume.assign(un, 0.0);
        stat_pump_energy.assign(un, 0.0);
        stat_pump_was_on.assign(un, false);
        stat_flow_turns.assign(un, 0L);
        stat_flow_turn_sign.assign(un, 0);
        stat_time_courant_critical.assign(un, 0.0);
    }

    /**
     * @brief Grow all arrays to hold at least `n` links, preserving existing data.
     */
    void grow_to(int n) {
        if (n <= count()) return;
        const auto un = static_cast<std::size_t>(n);
        auto g = [&](auto& vec, auto def) { vec.resize(un, def); };
        g(type, LinkType::CONDUIT); g(node1, -1); g(node2, -1);
        g(offset1, 0.0); g(offset2, 0.0); g(q0, 0.0); g(q_limit, 0.0);
        g(xsect_shape, XsectShape::CIRCULAR);
        g(xsect_y_full, 0.0); g(xsect_a_full, 0.0); g(xsect_w_max, 0.0);
        g(xsect_geom1, 0.0); g(xsect_geom2, 0.0); g(xsect_geom3, 0.0); g(xsect_geom4, 0.0);
        g(xsect_curve, -1);
        g(xsect_r_full, 0.0); g(xsect_s_full, 0.0); g(xsect_s_max, 0.0);
        g(xsect_y_bot, 0.0); g(xsect_a_bot, 0.0);
        g(xsect_s_bot, 0.0); g(xsect_r_bot, 0.0);
        g(xsect_yw_max, 0.0); g(xsect_batch_shape, 0);
        g(setting, 1.0); g(target_setting, 1.0); g(time_last_set, 0.0);
        g(direction, 1);
        g(has_flap_gate, uint8_t{0}); g(dqdh, 0.0);
        pump_curve_name.resize(un);
        g(flow, 0.0); g(depth, 0.0); g(volume, 0.0);
        g(froude, 0.0); g(flow_class, FlowClass::DRY); g(is_closed, uint8_t{0});
        g(old_flow, 0.0); g(old_depth, 0.0); g(old_volume, 0.0);
        comments.resize(un, std::string{});
        tags.resize(un, std::string{});

        g(rpt_flag, static_cast<char>(0));
        g(stat_vol_flow, 0.0); g(stat_max_flow, 0.0);
        g(stat_max_veloc, 0.0); g(stat_max_filling, 0.0);
        g(stat_time_surcharged, 0.0); g(stat_max_flow_date, 0.0);
        g(stat_time_full_upstream, 0.0); g(stat_time_full_dnstream, 0.0);
        g(stat_time_full_both, 0.0); g(stat_time_capacity_limited, 0.0);
        g(stat_pump_cycles, 0); g(stat_pump_on_time, 0.0);
        g(stat_pump_volume, 0.0); g(stat_pump_energy, 0.0);
        g(stat_pump_was_on, false);
        g(stat_flow_turns, 0L); g(stat_flow_turn_sign, 0);
        g(stat_time_courant_critical, 0.0);
        g(stat_norm_ltd, 0L); g(stat_inlet_ctrl, 0L);
        // stat_flow_class is flat 2D [n * N_FLOW_CLASSES]
        stat_flow_class.resize(un * N_FLOW_CLASSES, 0L);
        // Note: conc, conc_old handled by resize_quality()
        // Note: stat_total_load handled by resize_loads()
    }

    /**
     * @brief Reserve capacity for `n` links without changing count().
     *
     * @details Parsing grows these arrays one row at a time via grow_to(),
     *          so each of the ~65 parallel vectors reallocates and copies
     *          O(log n) times over a section — the dominant memory traffic in
     *          handler dispatch on a large model.
     *
     *          This reserves capacity only. It deliberately does NOT resize:
     *          count() is the vector size, and PostParseResolver compares it
     *          against the final name count and calls resize() when they
     *          differ — a call with destructive assign semantics. Growing the
     *          SIZE speculatively here would trip that path and wipe parsed
     *          data. Capacity is invisible to all of it.
     *
     *          Over-reserving is harmless, so callers pass the section's row
     *          count as an upper bound.
     */
    void reserve_to(int n) {
        if (n <= 0) return;
        const auto un = static_cast<std::size_t>(n);
        if (type.capacity() >= un) return;
        auto r = [&](auto& vec) { vec.reserve(un); };
        r(type); r(node1); r(node2); r(offset1);
        r(offset2); r(q0); r(q_limit); r(xsect_shape);
        r(xsect_y_full); r(xsect_a_full); r(xsect_w_max); r(xsect_geom1);
        r(xsect_geom2); r(xsect_geom3); r(xsect_geom4); r(xsect_curve);
        r(xsect_r_full); r(xsect_s_full); r(xsect_s_max); r(xsect_y_bot);
        r(xsect_a_bot); r(xsect_s_bot); r(xsect_r_bot); r(xsect_yw_max);
        r(xsect_batch_shape); r(setting); r(target_setting); r(time_last_set);
        r(direction); r(has_flap_gate); r(dqdh); r(flow);
        r(depth); r(volume); r(froude); r(flow_class);
        r(is_closed); r(old_flow); r(old_depth); r(old_volume);
        r(rpt_flag); r(stat_vol_flow); r(stat_max_flow); r(stat_max_veloc);
        r(stat_max_filling); r(stat_time_surcharged); r(stat_max_flow_date); r(stat_time_full_upstream);
        r(stat_time_full_dnstream); r(stat_time_full_both); r(stat_time_capacity_limited); r(stat_pump_cycles);
        r(stat_pump_on_time); r(stat_pump_volume); r(stat_pump_energy); r(stat_pump_was_on);
        r(stat_flow_turns); r(stat_flow_turn_sign); r(stat_time_courant_critical); r(stat_norm_ltd);
        r(stat_inlet_ctrl); r(pump_curve_name); r(comments); r(tags);
        r(stat_flow_class);
    }

    /**
     * @brief Erase the link at index `idx` from every parallel array.
     *
     * @details Removes the element at `idx` from every SoA vector. Flat-2D
     *          arrays (stat_flow_class, conc, conc_old, stat_total_load) have
     *          their full stride for `idx` removed. Spatial arrays are erased
     *          separately by ObjectDeleter.
     */
    void erase_at(int idx) {
        const auto ui = static_cast<std::size_t>(idx);
        auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx)); };

        e(type); e(node1); e(node2); e(offset1); e(offset2); e(q0); e(q_limit);

        e(xsect_shape); e(xsect_y_full); e(xsect_a_full); e(xsect_w_max); e(xsect_curve);
        e(xsect_r_full); e(xsect_s_full); e(xsect_s_max);
        e(xsect_y_bot); e(xsect_a_bot); e(xsect_s_bot); e(xsect_r_bot); e(xsect_yw_max);
        e(xsect_batch_shape); e(setting); e(target_setting); e(direction);

        e(pump_curve_name);

        e(has_flap_gate); e(dqdh);

        e(flow); e(depth); e(volume); e(froude); e(flow_class); e(is_closed);
        e(old_flow); e(old_depth); e(old_volume);
        e(comments); e(tags); e(rpt_flag);

        e(stat_vol_flow); e(stat_max_flow); e(stat_max_veloc); e(stat_max_filling);
        e(stat_time_surcharged); e(stat_norm_ltd); e(stat_inlet_ctrl);
        e(stat_max_flow_date); e(stat_time_full_upstream); e(stat_time_full_dnstream);
        e(stat_time_full_both); e(stat_time_capacity_limited);
        e(stat_pump_cycles); e(stat_pump_on_time); e(stat_pump_volume); e(stat_pump_energy);
        e(stat_pump_was_on); e(stat_flow_turns); e(stat_flow_turn_sign);
        e(stat_time_courant_critical);

        // Flat 2D: stat_flow_class [link * N_FLOW_CLASSES + class]
        {
            const auto base = ui * static_cast<std::size_t>(N_FLOW_CLASSES);
            const auto end  = base + static_cast<std::size_t>(N_FLOW_CLASSES);
            if (end <= stat_flow_class.size())
                stat_flow_class.erase(stat_flow_class.begin() + static_cast<std::ptrdiff_t>(base),
                                      stat_flow_class.begin() + static_cast<std::ptrdiff_t>(end));
        }

        // Flat 2D quality arrays: [link * np + p]
        if (conc_n_pollutants > 0) {
            const auto np = static_cast<std::size_t>(conc_n_pollutants);
            const auto base = ui * np;
            auto erase2d = [&](auto& v) {
                if (base + np <= v.size())
                    v.erase(v.begin() + static_cast<std::ptrdiff_t>(base),
                            v.begin() + static_cast<std::ptrdiff_t>(base + np));
            };
            erase2d(conc); erase2d(conc_old);
        }

        // Flat 2D stat load: [link * np + p]
        if (stat_n_pollutants > 0) {
            const auto np = static_cast<std::size_t>(stat_n_pollutants);
            const auto base = ui * np;
            if (base + np <= stat_total_load.size())
                stat_total_load.erase(
                    stat_total_load.begin() + static_cast<std::ptrdiff_t>(base),
                    stat_total_load.begin() + static_cast<std::ptrdiff_t>(base + np));
        }
    }

    /**
     * @brief Resize pollutant load arrays after pollutant count is known.
     */
    void resize_loads(int n_pollutants) {
        stat_n_pollutants = n_pollutants;
        if (n_pollutants > 0) {
            auto total = static_cast<std::size_t>(count()) *
                         static_cast<std::size_t>(n_pollutants);
            stat_total_load.assign(total, 0.0);
        }
    }

    /**
     * @brief Resize per-link quality arrays after pollutant count is known.
     */
    void resize_quality(int n_pollutants) {
        conc_n_pollutants = n_pollutants;
        if (n_pollutants > 0) {
            auto total = static_cast<std::size_t>(count()) *
                         static_cast<std::size_t>(n_pollutants);
            conc.assign(total, 0.0);
            conc_old.assign(total, 0.0);
        }
    }

    /**
     * @brief Release excess vector capacity accumulated during parsing.
     */
    void shrink_to_fit() {
        type.shrink_to_fit();
        node1.shrink_to_fit();
        node2.shrink_to_fit();
        offset1.shrink_to_fit();
        offset2.shrink_to_fit();
        q0.shrink_to_fit();
        q_limit.shrink_to_fit();

        xsect_shape.shrink_to_fit();
        xsect_y_full.shrink_to_fit();
        xsect_a_full.shrink_to_fit();
        xsect_w_max.shrink_to_fit();
        xsect_curve.shrink_to_fit();
        xsect_r_full.shrink_to_fit();
        xsect_s_full.shrink_to_fit();
        xsect_s_max.shrink_to_fit();
        xsect_y_bot.shrink_to_fit();
        xsect_a_bot.shrink_to_fit();
        xsect_s_bot.shrink_to_fit();
        xsect_r_bot.shrink_to_fit();
        xsect_yw_max.shrink_to_fit();
        xsect_batch_shape.shrink_to_fit();
        setting.shrink_to_fit();
        target_setting.shrink_to_fit();
        direction.shrink_to_fit();

        has_flap_gate.shrink_to_fit();
        dqdh.shrink_to_fit();
        pump_curve_name.shrink_to_fit();

        flow.shrink_to_fit();
        depth.shrink_to_fit();
        volume.shrink_to_fit();
        froude.shrink_to_fit();
        flow_class.shrink_to_fit();
        is_closed.shrink_to_fit();
        old_flow.shrink_to_fit();
        old_depth.shrink_to_fit();
        old_volume.shrink_to_fit();
        conc.shrink_to_fit();
        conc_old.shrink_to_fit();

        comments.shrink_to_fit();
        tags.shrink_to_fit();

        rpt_flag.shrink_to_fit();

        stat_vol_flow.shrink_to_fit();
        stat_max_flow.shrink_to_fit();
        stat_max_veloc.shrink_to_fit();
        stat_max_filling.shrink_to_fit();
        stat_time_surcharged.shrink_to_fit();
        stat_flow_class.shrink_to_fit();
        stat_norm_ltd.shrink_to_fit();
        stat_inlet_ctrl.shrink_to_fit();
        stat_max_flow_date.shrink_to_fit();
        stat_time_full_upstream.shrink_to_fit();
        stat_time_full_dnstream.shrink_to_fit();
        stat_time_full_both.shrink_to_fit();
        stat_time_capacity_limited.shrink_to_fit();
        stat_pump_cycles.shrink_to_fit();
        stat_pump_on_time.shrink_to_fit();
        stat_pump_volume.shrink_to_fit();
        stat_pump_energy.shrink_to_fit();
        stat_pump_was_on.shrink_to_fit();
        stat_total_load.shrink_to_fit();
    }

    void save_state() noexcept {
        std::copy(flow.begin(),   flow.end(),   old_flow.begin());
        std::copy(depth.begin(),  depth.end(),  old_depth.begin());
        std::copy(volume.begin(), volume.end(), old_volume.begin());
        std::copy(conc.begin(),   conc.end(),   conc_old.begin());
    }

    void reset_state() noexcept {
        std::fill(flow.begin(),  flow.end(),  0.0);
        std::fill(depth.begin(), depth.end(), 0.0);
        std::fill(volume.begin(), volume.end(), 0.0);
        std::fill(froude.begin(), froude.end(), 0.0);
        std::fill(flow_class.begin(), flow_class.end(), FlowClass::DRY);
        std::fill(old_flow.begin(),  old_flow.end(),  0.0);
        std::fill(old_depth.begin(), old_depth.end(), 0.0);
        std::fill(old_volume.begin(), old_volume.end(), 0.0);
        std::fill(conc.begin(), conc.end(), 0.0);
        std::fill(conc_old.begin(), conc_old.end(), 0.0);
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_LINK_DATA_HPP */
