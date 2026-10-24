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
 * @file      LinkSubtypes.hpp
 * @brief     Relational (normalized) Structure-of-Arrays side-tables for link subtypes.
 *
 * @details Phase 6 of the relational refactor — the link analogue of
 *          NodeSubtypes.hpp. The wide `LinkData` SoA allocates every
 *          conduit/pump/orifice/weir/outlet field for *all* links even though
 *          each field is only valid when `type[i]` matches. This header holds
 *          dense per-subtype side-tables — one per link type — sized to that
 *          type's count and joined back to the base link by a stored `link_idx`
 *          ("foreign key"). A reverse map (`subtype_row`) gives O(1)
 *          base-index → row lookup.
 *
 *          **Authoritative:** the side-tables are the single source of truth for
 *          subtype config. Parse/edit writers populate the rows directly; the
 *          StructureSolver / XSectGroups init source-reads and the C-API read
 *          them. Rows are kept in ascending `link_idx` order so per-row
 *          iteration matches a base-link-ascending scan bit-for-bit.
 *
 *          **Design note — shared fields stay on base LinkData:** the cross
 *          section (`xsect_*`), `has_flap_gate`, and `pump_curve_name` are used
 *          by more than one link type (xsect/has_flap by conduit+orifice+weir;
 *          pump_curve_name by pump+outlet+conduit-IRREGULAR), so they remain on
 *          the base `LinkData` rather than any one subtype table. Only the
 *          unambiguously type-specific fields move here. Structures are split
 *          into Orifice/Weir/Outlet with NAMED columns over the type-overloaded
 *          wide `param1`/`param2` discriminators (stored as double to preserve
 *          bit-for-bit parity with the legacy compute).
 *
 * @see src/engine/data/NodeSubtypes.hpp — the node analogue (same API shape)
 * @see src/engine/data/LinkData.hpp — base LinkData SoA
 * @ingroup engine_data
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_LINK_SUBTYPES_HPP
#define OPENSWMM_ENGINE_LINK_SUBTYPES_HPP

#include <algorithm>
#include <vector>
#include <cstdint>
#include <string>
#include <type_traits>

#include "LinkData.hpp"

namespace openswmm {

// ============================================================================
// ConduitData — side-table for CONDUIT links
// ============================================================================

/**
 * @brief Dense SoA for conduit-only properties (one row per CONDUIT link).
 * @details Cross-section (`xsect_*`) and `has_flap_gate` stay on base LinkData
 *          (shared with orifice/weir). Init-derived hydraulic geometry
 *          (`beta`/`rough_factor`/`q_full`/`q_max`) lives here — it is
 *          config-derived, written once at init.
 */
struct ConduitData {
    std::vector<int> link_idx; ///< Base LinkData index (join key)

    std::vector<double> roughness;
    std::vector<double> length; ///< Authored [CONDUITS] length (ft)
    /// The length the routing uses — legacy conduit_getLength. Equal to
    /// `length` for every section but IRREGULAR, where the authored value
    /// is the main channel's and the routing length is the flood plain's,
    /// `length / Transect.lengthFactor`. Derived; refreshed by
    /// routing::applyConduitLengthening, never serialized.
    std::vector<double> true_length;
    std::vector<double> slope;
    std::vector<double> mod_length;
    std::vector<int> barrels;
    std::vector<double> beta;
    std::vector<double> rough_factor;
    std::vector<double> q_full;
    std::vector<double> q_max;
    std::vector<double> loss_inlet;
    std::vector<double> loss_outlet;
    std::vector<double> loss_avg;
    std::vector<double> seep_rate;
    std::vector<int> culvert_code;

    // Phase 6 Stage B: mutable per-step conduit state moved off wide LinkData.
    std::vector<double> evap_loss_rate;       ///< per-step evaporation loss
    std::vector<double> seep_loss_rate;       ///< per-step seepage loss
    std::vector<uint8_t> normal_flow_limited; ///< per-step flag (reset each step)
    std::vector<uint8_t> inlet_control;       ///< per-step culvert inlet-control flag
    std::vector<int8_t> full_state;           ///< per-step up/down full bitmask

    // G-X4 (2026-09-20): the two-zone aquifer's frozen coupling for this
    // routing step, published by SurfaceRouter2D before routing and read by
    // the two loss laws (DynamicWave and routing::computeConduitLosses).
    // `gw_coupled == 0` on every conduit of every deck without a
    // `[2D_AQUIFER]` + `LINK_SEEPAGE TWO_WAY`, and the laws then take their
    // legacy branch unchanged.
    std::vector<uint8_t> gw_coupled; ///< 1 = signed conductance law applies
    std::vector<double>
        gw_head_rel; ///< water-table elevation MINUS the conduit's mid invert (project length)
    std::vector<double> gw_kc; ///< bed conductivity (project rate units); seeded from seep_rate
    std::vector<double>
        gw_dc; ///< bed thickness / characteristic path (project length); seeded from the full depth
    std::vector<double>
        gw_gain_max; ///< cap on the gaining direction, ONE barrel (project flow units)

    /**
     * @brief Count ConduitData rows from the base-object join-key column.
     * @return link_idx.size() converted to int; other column lengths are not checked.
     */
    int count() const noexcept { return static_cast<int>(link_idx.size()); }

    /**
     * @brief Erase all ConduitData rows while retaining vector capacities.
     *
     * Invalidates element references/iterators and removes owned nested state. Does not clear base
     * objects or repair the enclosing reverse map.
     */
    void clear() noexcept {
        link_idx.clear();
        roughness.clear();
        length.clear();
        true_length.clear();
        slope.clear();
        mod_length.clear();
        barrels.clear();
        beta.clear();
        rough_factor.clear();
        q_full.clear();
        q_max.clear();
        loss_inlet.clear();
        loss_outlet.clear();
        loss_avg.clear();
        seep_rate.clear();
        culvert_code.clear();
        evap_loss_rate.clear();
        seep_loss_rate.clear();
        normal_flow_limited.clear();
        inlet_control.clear();
        full_state.clear();
        gw_coupled.clear();
        gw_head_rel.clear();
        gw_kc.clear(); // G-X4
        gw_dc.clear();
        gw_gain_max.clear();
    }

    /**
     * @brief Request capacity for each ConduitData column without changing row count.
     *
     * May invalidate column references/iterators. Allocation failure can leave different reserved
     * capacities, while existing row values remain.
     *
     * @param[in] n Nonnegative anticipated row count; caller validates before conversion to size_t.
     */
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        link_idx.reserve(un);
        roughness.reserve(un);
        length.reserve(un);
        true_length.reserve(un);
        slope.reserve(un);
        mod_length.reserve(un);
        barrels.reserve(un);
        beta.reserve(un);
        rough_factor.reserve(un);
        q_full.reserve(un);
        q_max.reserve(un);
        loss_inlet.reserve(un);
        loss_outlet.reserve(un);
        loss_avg.reserve(un);
        seep_rate.reserve(un);
        culvert_code.reserve(un);
        evap_loss_rate.reserve(un);
        seep_loss_rate.reserve(un);
        normal_flow_limited.reserve(un);
        inlet_control.reserve(un);
        full_state.reserve(un);
        gw_coupled.reserve(un);
        gw_head_rel.reserve(un);
        gw_kc.reserve(un); // G-X4
        gw_dc.reserve(un);
        gw_gain_max.reserve(un);
    }

    /**
     * @brief Insert a default ConduitData row at the sorted base-object key position.
     *
     * Assumes ascending link_idx and aligned columns. Seeds roughness=0.01, barrels=1;
     * geometric/flow/loss fields and coupling state zero. Does not update the enclosing reverse map,
     * reject duplicate keys, enforce lifecycle or initialize solver caches. Allocation failure can
     * leave partially inserted parallel columns.
     *
     * @param[in] i Nonnegative base-object index; caller ensures it exists, is of this subtype and is
     *     absent from this table.
     * @return Inserted dense row index; later dense rows shift up.
     */
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(link_idx.begin(), link_idx.end(), i) - link_idx.begin());
        link_idx.insert(link_idx.begin() + p, i);
        roughness.insert(roughness.begin() + p, 0.01);
        length.insert(length.begin() + p, 0.0);
        true_length.insert(true_length.begin() + p, 0.0);
        slope.insert(slope.begin() + p, 0.0);
        mod_length.insert(mod_length.begin() + p, 0.0);
        barrels.insert(barrels.begin() + p, 1);
        beta.insert(beta.begin() + p, 0.0);
        rough_factor.insert(rough_factor.begin() + p, 0.0);
        q_full.insert(q_full.begin() + p, 0.0);
        q_max.insert(q_max.begin() + p, 0.0);
        loss_inlet.insert(loss_inlet.begin() + p, 0.0);
        loss_outlet.insert(loss_outlet.begin() + p, 0.0);
        loss_avg.insert(loss_avg.begin() + p, 0.0);
        seep_rate.insert(seep_rate.begin() + p, 0.0);
        culvert_code.insert(culvert_code.begin() + p, 0);
        evap_loss_rate.insert(evap_loss_rate.begin() + p, 0.0);
        seep_loss_rate.insert(seep_loss_rate.begin() + p, 0.0);
        normal_flow_limited.insert(normal_flow_limited.begin() + p, uint8_t{0});
        inlet_control.insert(inlet_control.begin() + p, uint8_t{0});
        full_state.insert(full_state.begin() + p, int8_t{0});
        gw_coupled.insert(gw_coupled.begin() + p, uint8_t{0}); // G-X4
        gw_head_rel.insert(gw_head_rel.begin() + p, 0.0);
        gw_kc.insert(gw_kc.begin() + p, 0.0);
        gw_dc.insert(gw_dc.begin() + p, 0.0);
        gw_gain_max.insert(gw_gain_max.begin() + p, 0.0);
        return static_cast<int>(p);
    }

    /**
     * @brief Erase dense ConduitData row data and shift later dense rows down.
     *
     * Erases join/config/per-step columns through full_state, but this implementation leaves
     * gw_coupled/gw_head_rel/gw_kc/gw_dc/gw_gain_max vectors unchanged. Does not erase a base object,
     * renumber stored base keys or rebuild the reverse map; enclosing maintenance handles those steps.
     *
     * @param[in] r Existing dense subtype-row index, NOT the base-object index; caller validates
     *     bounds/alignment.
     */
    void erase_at(int r) {
        const auto p = static_cast<std::ptrdiff_t>(r);
        link_idx.erase(link_idx.begin() + p);
        roughness.erase(roughness.begin() + p);
        length.erase(length.begin() + p);
        true_length.erase(true_length.begin() + p);
        slope.erase(slope.begin() + p);
        mod_length.erase(mod_length.begin() + p);
        barrels.erase(barrels.begin() + p);
        beta.erase(beta.begin() + p);
        rough_factor.erase(rough_factor.begin() + p);
        q_full.erase(q_full.begin() + p);
        q_max.erase(q_max.begin() + p);
        loss_inlet.erase(loss_inlet.begin() + p);
        loss_outlet.erase(loss_outlet.begin() + p);
        loss_avg.erase(loss_avg.begin() + p);
        seep_rate.erase(seep_rate.begin() + p);
        culvert_code.erase(culvert_code.begin() + p);
        evap_loss_rate.erase(evap_loss_rate.begin() + p);
        seep_loss_rate.erase(seep_loss_rate.begin() + p);
        normal_flow_limited.erase(normal_flow_limited.begin() + p);
        inlet_control.erase(inlet_control.begin() + p);
        full_state.erase(full_state.begin() + p);
    }
};

// ============================================================================
// PumpData — side-table for PUMP links
// ============================================================================

struct PumpData {
    std::vector<int> link_idx;
    std::vector<int> curve;          ///< Pump curve table index (-1 = ideal)
    std::vector<uint8_t> init_state; ///< Initial on/off (was vector<bool>)
    std::vector<double> startup;     ///< Startup depth
    std::vector<double> shutoff;     ///< Shutoff depth
    std::vector<int> curve_type;     ///< Pump curve type (1-5, 6=ideal; -1 unset)

    /**
     * @brief Count PumpData rows from the base-object join-key column.
     * @return link_idx.size() converted to int; other column lengths are not checked.
     */
    int count() const noexcept { return static_cast<int>(link_idx.size()); }

    /**
     * @brief Erase all PumpData rows while retaining vector capacities.
     *
     * Invalidates element references/iterators and removes owned nested state. Does not clear base
     * objects or repair the enclosing reverse map.
     */
    void clear() noexcept {
        link_idx.clear();
        curve.clear();
        init_state.clear();
        startup.clear();
        shutoff.clear();
        curve_type.clear();
    }

    /**
     * @brief Request capacity for each PumpData column without changing row count.
     *
     * May invalidate column references/iterators. Allocation failure can leave different reserved
     * capacities, while existing row values remain.
     *
     * @param[in] n Nonnegative anticipated row count; caller validates before conversion to size_t.
     */
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        link_idx.reserve(un);
        curve.reserve(un);
        init_state.reserve(un);
        startup.reserve(un);
        shutoff.reserve(un);
        curve_type.reserve(un);
    }

    /**
     * @brief Insert a default PumpData row at the sorted base-object key position.
     *
     * Assumes ascending link_idx and aligned columns. Seeds curve/curve_type=-1, off initial state and
     * zero switching depths. Does not update the enclosing reverse map, reject duplicate keys, enforce
     * lifecycle or initialize solver caches. Allocation failure can leave partially inserted parallel
     * columns.
     *
     * @param[in] i Nonnegative base-object index; caller ensures it exists, is of this subtype and is
     *     absent from this table.
     * @return Inserted dense row index; later dense rows shift up.
     */
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(link_idx.begin(), link_idx.end(), i) - link_idx.begin());
        link_idx.insert(link_idx.begin() + p, i);
        curve.insert(curve.begin() + p, -1);
        init_state.insert(init_state.begin() + p, uint8_t{0});
        startup.insert(startup.begin() + p, 0.0);
        shutoff.insert(shutoff.begin() + p, 0.0);
        curve_type.insert(curve_type.begin() + p, -1);
        return static_cast<int>(p);
    }

    /**
     * @brief Erase dense PumpData row data and shift later dense rows down.
     *
     * Erases every defined side-table column. Does not erase a base object, renumber stored base keys
     * or rebuild the reverse map; enclosing maintenance handles those steps.
     *
     * @param[in] r Existing dense subtype-row index, NOT the base-object index; caller validates
     *     bounds/alignment.
     */
    void erase_at(int r) {
        const auto p = static_cast<std::ptrdiff_t>(r);
        link_idx.erase(link_idx.begin() + p);
        curve.erase(curve.begin() + p);
        init_state.erase(init_state.begin() + p);
        startup.erase(startup.begin() + p);
        shutoff.erase(shutoff.begin() + p);
        curve_type.erase(curve_type.begin() + p);
    }
};

// ============================================================================
// OrificeData — side-table for ORIFICE links  (wide param1 → orifice_type)
// ============================================================================

struct OrificeData {
    std::vector<int> link_idx;
    std::vector<double> orifice_type; ///< 0 = BOTTOM, 1 = SIDE (legacy param1)
    std::vector<double> cd;           ///< Discharge coefficient
    std::vector<double> orate;        ///< Open/close time in HOURS, as parsed
    /**
     * @brief Count OrificeData rows from the base-object join-key column.
     * @return link_idx.size() converted to int; other column lengths are not checked.
     */
    int count() const noexcept { return static_cast<int>(link_idx.size()); }

    /**
     * @brief Erase all OrificeData rows while retaining vector capacities.
     *
     * Invalidates element references/iterators and removes owned nested state. Does not clear base
     * objects or repair the enclosing reverse map.
     */
    void clear() noexcept {
        link_idx.clear();
        orifice_type.clear();
        cd.clear();
        orate.clear();
    }
    /**
     * @brief Request capacity for each OrificeData column without changing row count.
     *
     * May invalidate column references/iterators. Allocation failure can leave different reserved
     * capacities, while existing row values remain.
     *
     * @param[in] n Nonnegative anticipated row count; caller validates before conversion to size_t.
     */
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        link_idx.reserve(un);
        orifice_type.reserve(un);
        cd.reserve(un);
        orate.reserve(un);
    }
    /**
     * @brief Insert a default OrificeData row at the sorted base-object key position.
     *
     * Assumes ascending link_idx and aligned columns. Seeds BOTTOM type and zero discharge/open-close
     * parameters. Does not update the enclosing reverse map, reject duplicate keys, enforce lifecycle
     * or initialize solver caches. Allocation failure can leave partially inserted parallel columns.
     *
     * @param[in] i Nonnegative base-object index; caller ensures it exists, is of this subtype and is
     *     absent from this table.
     * @return Inserted dense row index; later dense rows shift up.
     */
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(link_idx.begin(), link_idx.end(), i) - link_idx.begin());
        link_idx.insert(link_idx.begin() + p, i);
        orifice_type.insert(orifice_type.begin() + p, 0.0);
        cd.insert(cd.begin() + p, 0.0);
        orate.insert(orate.begin() + p, 0.0);
        return static_cast<int>(p);
    }
    /**
     * @brief Erase dense OrificeData row data and shift later dense rows down.
     *
     * Erases every defined side-table column. Does not erase a base object, renumber stored base keys
     * or rebuild the reverse map; enclosing maintenance handles those steps.
     *
     * @param[in] r Existing dense subtype-row index, NOT the base-object index; caller validates
     *     bounds/alignment.
     */
    void erase_at(int r) {
        const auto p = static_cast<std::ptrdiff_t>(r);
        link_idx.erase(link_idx.begin() + p);
        orifice_type.erase(orifice_type.begin() + p);
        cd.erase(cd.begin() + p);
        orate.erase(orate.begin() + p);
    }
};

// ============================================================================
// WeirData — side-table for WEIR links  (param1 → weir_type, param2 → end_con)
// ============================================================================

struct WeirData {
    std::vector<int> link_idx;
    std::vector<double>
        weir_type; ///< TRANSVERSE/SIDEFLOW/V-NOTCH/TRAPEZOIDAL/ROADWAY = 0..4 (legacy param1)
    std::vector<double> cd;               ///< Discharge coefficient
    std::vector<double> end_contractions; ///< End contractions (legacy param2)
    std::vector<double> crest_height;
    std::vector<double> cd2; ///< End-section discharge coeff (legacy cDisch2)
    std::vector<uint8_t>
        can_surcharge; ///< Surcharge YES/NO (legacy Weir.canSurcharge, default YES)
    std::vector<double>
        road_width; ///< ROADWAY weir: road width across the flow (legacy Weir.roadWidth; authored length units until the resolver, then ft)
    std::vector<int8_t>
        road_surface; ///< ROADWAY weir: 0 none, 1 PAVED, 2 GRAVEL (legacy Weir.roadSurface)
    std::vector<int>
        cd_curve; ///< Discharge-coefficient curve index in ctx.tables, -1 none (legacy Weir.cdCurve)

    /**
     * @brief Count WeirData rows from the base-object join-key column.
     * @return link_idx.size() converted to int; other column lengths are not checked.
     */
    int count() const noexcept { return static_cast<int>(link_idx.size()); }

    /**
     * @brief Erase all WeirData rows while retaining vector capacities.
     *
     * Invalidates element references/iterators and removes owned nested state. Does not clear base
     * objects or repair the enclosing reverse map.
     */
    void clear() noexcept {
        link_idx.clear();
        weir_type.clear();
        cd.clear();
        end_contractions.clear();
        crest_height.clear();
        cd2.clear();
        can_surcharge.clear();
        road_width.clear();
        road_surface.clear();
        cd_curve.clear();
    }
    /**
     * @brief Request capacity for each WeirData column without changing row count.
     *
     * May invalidate column references/iterators. Allocation failure can leave different reserved
     * capacities, while existing row values remain.
     *
     * @param[in] n Nonnegative anticipated row count; caller validates before conversion to size_t.
     */
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        link_idx.reserve(un);
        weir_type.reserve(un);
        cd.reserve(un);
        end_contractions.reserve(un);
        crest_height.reserve(un);
        cd2.reserve(un);
        can_surcharge.reserve(un);
        road_width.reserve(un);
        road_surface.reserve(un);
        cd_curve.reserve(un);
    }
    /**
     * @brief Insert a default WeirData row at the sorted base-object key position.
     *
     * Assumes ascending link_idx and aligned columns. Seeds TRANSVERSE type, surcharge enabled,
     * curve=-1 and zero geometric/coefficient values. Does not update the enclosing reverse map, reject
     * duplicate keys, enforce lifecycle or initialize solver caches. Allocation failure can leave
     * partially inserted parallel columns.
     *
     * @param[in] i Nonnegative base-object index; caller ensures it exists, is of this subtype and is
     *     absent from this table.
     * @return Inserted dense row index; later dense rows shift up.
     */
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(link_idx.begin(), link_idx.end(), i) - link_idx.begin());
        link_idx.insert(link_idx.begin() + p, i);
        weir_type.insert(weir_type.begin() + p, 0.0);
        cd.insert(cd.begin() + p, 0.0);
        end_contractions.insert(end_contractions.begin() + p, 0.0);
        crest_height.insert(crest_height.begin() + p, 0.0);
        cd2.insert(cd2.begin() + p, 0.0);
        can_surcharge.insert(can_surcharge.begin() + p, uint8_t{1});
        road_width.insert(road_width.begin() + p, 0.0);
        road_surface.insert(road_surface.begin() + p, int8_t{0});
        cd_curve.insert(cd_curve.begin() + p, -1);
        return static_cast<int>(p);
    }
    /**
     * @brief Erase dense WeirData row data and shift later dense rows down.
     *
     * Erases every defined side-table column. Does not erase a base object, renumber stored base keys
     * or rebuild the reverse map; enclosing maintenance handles those steps.
     *
     * @param[in] r Existing dense subtype-row index, NOT the base-object index; caller validates
     *     bounds/alignment.
     */
    void erase_at(int r) {
        const auto p = static_cast<std::ptrdiff_t>(r);
        link_idx.erase(link_idx.begin() + p);
        weir_type.erase(weir_type.begin() + p);
        cd.erase(cd.begin() + p);
        end_contractions.erase(end_contractions.begin() + p);
        crest_height.erase(crest_height.begin() + p);
        cd2.erase(cd2.begin() + p);
        can_surcharge.erase(can_surcharge.begin() + p);
        road_width.erase(road_width.begin() + p);
        road_surface.erase(road_surface.begin() + p);
        cd_curve.erase(cd_curve.begin() + p);
    }
};

// ============================================================================
// OutletData — side-table for OUTLET links
//   param1 → outlet_type (rating: 0 FUNC/HEAD,1 FUNC/DEPTH,2 TAB/HEAD,3 TAB/DEPTH)
//   cd → coeff (functional C1), param2 → expon (functional C2)
// ============================================================================

struct OutletData {
    std::vector<int> link_idx;
    std::vector<double> outlet_type;  ///< Rating type (legacy param1)
    std::vector<double> crest_height; ///< Offset/crest
    std::vector<double> coeff;        ///< Functional C1 (legacy cd)
    std::vector<double> expon;        ///< Functional C2 exponent (legacy param2)
    std::vector<int>
        curve; ///< TABULAR rating-curve index (legacy pump_curve reuse; -1 = functional)

    /**
     * @brief Count OutletData rows from the base-object join-key column.
     * @return link_idx.size() converted to int; other column lengths are not checked.
     */
    int count() const noexcept { return static_cast<int>(link_idx.size()); }

    /**
     * @brief Erase all OutletData rows while retaining vector capacities.
     *
     * Invalidates element references/iterators and removes owned nested state. Does not clear base
     * objects or repair the enclosing reverse map.
     */
    void clear() noexcept {
        link_idx.clear();
        outlet_type.clear();
        crest_height.clear();
        coeff.clear();
        expon.clear();
        curve.clear();
    }
    /**
     * @brief Request capacity for each OutletData column without changing row count.
     *
     * May invalidate column references/iterators. Allocation failure can leave different reserved
     * capacities, while existing row values remain.
     *
     * @param[in] n Nonnegative anticipated row count; caller validates before conversion to size_t.
     */
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        link_idx.reserve(un);
        outlet_type.reserve(un);
        crest_height.reserve(un);
        coeff.reserve(un);
        expon.reserve(un);
        curve.reserve(un);
    }
    /**
     * @brief Insert a default OutletData row at the sorted base-object key position.
     *
     * Assumes ascending link_idx and aligned columns. Seeds functional/head type, curve=-1 and zero
     * crest/coefficient/exponent. Does not update the enclosing reverse map, reject duplicate keys,
     * enforce lifecycle or initialize solver caches. Allocation failure can leave partially inserted
     * parallel columns.
     *
     * @param[in] i Nonnegative base-object index; caller ensures it exists, is of this subtype and is
     *     absent from this table.
     * @return Inserted dense row index; later dense rows shift up.
     */
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(link_idx.begin(), link_idx.end(), i) - link_idx.begin());
        link_idx.insert(link_idx.begin() + p, i);
        outlet_type.insert(outlet_type.begin() + p, 0.0);
        crest_height.insert(crest_height.begin() + p, 0.0);
        coeff.insert(coeff.begin() + p, 0.0);
        expon.insert(expon.begin() + p, 0.0);
        curve.insert(curve.begin() + p, -1);
        return static_cast<int>(p);
    }
    /**
     * @brief Erase dense OutletData row data and shift later dense rows down.
     *
     * Erases every defined side-table column. Does not erase a base object, renumber stored base keys
     * or rebuild the reverse map; enclosing maintenance handles those steps.
     *
     * @param[in] r Existing dense subtype-row index, NOT the base-object index; caller validates
     *     bounds/alignment.
     */
    void erase_at(int r) {
        const auto p = static_cast<std::ptrdiff_t>(r);
        link_idx.erase(link_idx.begin() + p);
        outlet_type.erase(outlet_type.begin() + p);
        crest_height.erase(crest_height.begin() + p);
        coeff.erase(coeff.begin() + p);
        expon.erase(expon.begin() + p);
        curve.erase(curve.begin() + p);
    }
};

// ============================================================================
// LinkSubtypes — container + reverse map + incremental maintenance
// ============================================================================

/**
 * @brief Owns the five link subtype side-tables plus the reverse index map.
 * @details `subtype_row[i]` is the row of link `i` within whichever side-table
 *          matches `links.type[i]` (every link type has exactly one). Together
 *          with each table's `link_idx`, this is O(1) in both directions.
 */
struct LinkSubtypes {
    ConduitData conduits;
    PumpData pumps;
    OrificeData orifices;
    WeirData weirs;
    OutletData outlets;

    /** @brief base link index → row in its subtype table (-1 if unset). */
    std::vector<int> subtype_row;

    /**
     * @brief Replace all subtype rows with defaults derived from current base link types.
     *
     * Clears all existing subtype configuration/state and recreates sorted default rows, then rebuilds
     * reverse map. Primarily supports hand-built fixtures; does not infer custom geometry/parameters
     * from other stores.
     *
     * @param[in] L Borrowed aligned LinkData with supported type for each existing row.
     */
    void build_default_rows(const LinkData& L) {
        clear();
        const int n = L.count();
        for (int i = 0; i < n; ++i) {
            switch (L.type[static_cast<std::size_t>(i)]) {
            case LinkType::CONDUIT:
                conduits.add_default(i);
                break;
            case LinkType::PUMP:
                pumps.add_default(i);
                break;
            case LinkType::ORIFICE:
                orifices.add_default(i);
                break;
            case LinkType::WEIR:
                weirs.add_default(i);
                break;
            case LinkType::OUTLET:
                outlets.add_default(i);
                break;
            }
        }
        rebuild_index(n);
    }

    /**
     * @brief Erase all owned link subtype tables and reverse-map entries.
     *
     * Does not edit base LinkData rows; all borrowed subtype references/row indices must be
     * re-resolved.
     */
    void clear() noexcept {
        conduits.clear();
        pumps.clear();
        orifices.clear();
        weirs.clear();
        outlets.clear();
        subtype_row.clear();
    }

    /**
     * @brief Rebuild base link indices to dense subtype-row lookup.
     *
     * Replaces reverse map with -1 then writes each side-table row index. Does not validate
     * duplicate/type-consistency or change table data; duplicate keys are overwritten by later visited
     * tables. Invalid join keys cause unchecked indexing. Allocation exceptions propagate.
     *
     * @param[in] n_links Nonnegative total base-object count; every stored join key must be in
     *     [0,count).
     */
    void rebuild_index(int n_links) {
        subtype_row.assign(static_cast<std::size_t>(n_links), -1);
        auto idx = [&](const std::vector<int>& keys) {
            for (int r = 0; r < static_cast<int>(keys.size()); ++r)
                subtype_row[static_cast<std::size_t>(keys[static_cast<std::size_t>(r)])] = r;
        };
        idx(conduits.link_idx);
        idx(pumps.link_idx);
        idx(orifices.link_idx);
        idx(weirs.link_idx);
        idx(outlets.link_idx);
    }

    /**
     * @brief Change one base link type and maintain its dense side-table row.
     *
     * Erases old row and inserts target defaults, preserving sorted join keys. End insertion updates
     * one reverse entry; shifted rows trigger full rebuild. Does not perform higher-level conversion
     * cleanup, topology validation, source rewiring or solver-cache refresh. Allocation failure is not
     * transactional.
     *
     * @param[in,out] links Caller-owned aligned base LinkData store; this function changes its type
     *     field.
     * @param[in] i Existing nonnegative base-object index; no bounds check on the base store.
     * @param[in] t Supported internal LinkType; caller validates enum and editable lifecycle.
     * @return Dense target row; unrecognized types can return -1. Same-type existing row returns
     *     unchanged.
     */
    int set_link_type(LinkData& links, int i, LinkType t) {
        const auto ui = static_cast<std::size_t>(i);
        if (i >= static_cast<int>(subtype_row.size()))
            subtype_row.resize(static_cast<std::size_t>(i) + 1, -1);

        const LinkType old = links.type[ui];
        const int existing = subtype_row[ui];
        if (old == t && existing >= 0)
            return existing; // already this subtype with a row — idempotent

        bool shifted = false;
        if (existing >= 0) { // remove the old subtype row (real re-type/convert)
            switch (old) {
            case LinkType::CONDUIT:
                conduits.erase_at(existing);
                break;
            case LinkType::PUMP:
                pumps.erase_at(existing);
                break;
            case LinkType::ORIFICE:
                orifices.erase_at(existing);
                break;
            case LinkType::WEIR:
                weirs.erase_at(existing);
                break;
            case LinkType::OUTLET:
                outlets.erase_at(existing);
                break;
            }
            shifted = true;
        }

        links.type[ui] = t;

        int row = -1;
        switch (t) {
        case LinkType::CONDUIT:
            row = conduits.add_default(i);
            if (row != conduits.count() - 1)
                shifted = true;
            break;
        case LinkType::PUMP:
            row = pumps.add_default(i);
            if (row != pumps.count() - 1)
                shifted = true;
            break;
        case LinkType::ORIFICE:
            row = orifices.add_default(i);
            if (row != orifices.count() - 1)
                shifted = true;
            break;
        case LinkType::WEIR:
            row = weirs.add_default(i);
            if (row != weirs.count() - 1)
                shifted = true;
            break;
        case LinkType::OUTLET:
            row = outlets.add_default(i);
            if (row != outlets.count() - 1)
                shifted = true;
            break;
        }

        if (shifted) {
            rebuild_index(links.count());
            return subtype_row[ui];
        }
        subtype_row[ui] = row; // O(1) ascending-parse / end-insert path
        return row;
    }

    /**
     * @brief Drop a deleted base link's subtype row and renumber later join keys.
     *
     * Drops only the first matching subtype row across conduits/pumps/orifices/weirs/outlets,
     * decrements surviving keys above i and rebuilds the map. Does not perform the base erase or repair
     * unrelated model references.
     *
     * @param[in] i Erased pre-deletion base-object index; call AFTER LinkData::erase_at(i).
     * @param[in] n_after Nonnegative base count after erasure; surviving join keys must fit this count.
     */
    void erase_link(int i, int n_after) {
        auto drop = [i](auto& tbl) -> bool {
            for (int r = 0; r < tbl.count(); ++r)
                if (tbl.link_idx[static_cast<std::size_t>(r)] == i) {
                    tbl.erase_at(r);
                    return true;
                }
            return false;
        };
        drop(conduits) || drop(pumps) || drop(orifices) || drop(weirs) || drop(outlets);

        auto shift = [i](std::vector<int>& keys) {
            for (auto& k : keys)
                if (k > i)
                    --k;
        };
        shift(conduits.link_idx);
        shift(pumps.link_idx);
        shift(orifices.link_idx);
        shift(weirs.link_idx);
        shift(outlets.link_idx);

        rebuild_index(n_after);
    }

    // O(1) row helpers (validate link_idx[r] == i, like NodeSubtypes::*_row).
    /**
     * @brief Resolve a base link key into the conduit side table.
     *
     * O(1), no allocation. Does not independently consult the base object type; correctness depends on
     * coherent table maintenance.
     *
     * @param[in] i Base-object index; invalid/unbuilt/mismatched join keys yield -1.
     * @return Dense conduit row when reverse-map slot is in range and that table's stored key equals i;
     *     otherwise -1.
     */
    int conduit_row(int i) const noexcept { return row_in(i, conduits.link_idx); }
    /**
     * @brief Resolve a base link key into the pump side table.
     *
     * O(1), no allocation. Does not independently consult the base object type; correctness depends on
     * coherent table maintenance.
     *
     * @param[in] i Base-object index; invalid/unbuilt/mismatched join keys yield -1.
     * @return Dense pump row when reverse-map slot is in range and that table's stored key equals i;
     *     otherwise -1.
     */
    int pump_row(int i) const noexcept { return row_in(i, pumps.link_idx); }
    /**
     * @brief Resolve a base link key into the orifice side table.
     *
     * O(1), no allocation. Does not independently consult the base object type; correctness depends on
     * coherent table maintenance.
     *
     * @param[in] i Base-object index; invalid/unbuilt/mismatched join keys yield -1.
     * @return Dense orifice row when reverse-map slot is in range and that table's stored key equals i;
     *     otherwise -1.
     */
    int orifice_row(int i) const noexcept { return row_in(i, orifices.link_idx); }
    /**
     * @brief Resolve a base link key into the weir side table.
     *
     * O(1), no allocation. Does not independently consult the base object type; correctness depends on
     * coherent table maintenance.
     *
     * @param[in] i Base-object index; invalid/unbuilt/mismatched join keys yield -1.
     * @return Dense weir row when reverse-map slot is in range and that table's stored key equals i;
     *     otherwise -1.
     */
    int weir_row(int i) const noexcept { return row_in(i, weirs.link_idx); }
    /**
     * @brief Resolve a base link key into the outlet side table.
     *
     * O(1), no allocation. Does not independently consult the base object type; correctness depends on
     * coherent table maintenance.
     *
     * @param[in] i Base-object index; invalid/unbuilt/mismatched join keys yield -1.
     * @return Dense outlet row when reverse-map slot is in range and that table's stored key equals i;
     *     otherwise -1.
     */
    int outlet_row(int i) const noexcept { return row_in(i, outlets.link_idx); }

private:
    /**
     * @brief Validate one reverse-map slot against a selected subtype key vector.
     *
     * @param[in] i Base link index; invalid/out-of-range yields -1.
     * @param[in] keys Borrowed ordered base-link keys for the selected side table.
     * @return Reverse-map row when keys[row]==i and all bounds hold; otherwise -1.
     */
    int row_in(int i, const std::vector<int>& keys) const noexcept {
        if (i >= 0 && i < static_cast<int>(subtype_row.size())) {
            const int r = subtype_row[static_cast<std::size_t>(i)];
            if (r >= 0 && r < static_cast<int>(keys.size()) &&
                keys[static_cast<std::size_t>(r)] == i)
                return r;
        }
        return -1;
    }
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_LINK_SUBTYPES_HPP
