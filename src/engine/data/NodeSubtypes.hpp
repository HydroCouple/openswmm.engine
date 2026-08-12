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
 * @file NodeSubtypes.hpp
 * @brief Relational (normalized) Structure-of-Arrays side-tables for node subtypes.
 *
 * @details Part of the relational node refactor (docs/relational/
 *          RELATIONAL_NODE_REFACTOR_PLAN.md). The wide `NodeData` struct
 *          (src/engine/data/NodeData.hpp) currently allocates every storage/
 *          outfall/divider field for *all* nodes, even though those fields are
 *          only valid when `type[i]` matches. This header introduces dense
 *          side-tables — one per subtype — sized to the count of that subtype,
 *          joined back to the base node by a stored `node_idx` ("foreign key").
 *          A reverse map (`subtype_row`) gives O(1) base-index → side-table-row
 *          lookup.
 *
 *          **Phase 4 (authoritative):** the side-tables are the single source of
 *          truth for subtype config. Parse/edit writers populate the rows
 *          directly (no build-from-wide, no `verify_mirror`/`ensure_fresh`
 *          mirror machinery); compute and the C-API read them. Rows are kept in
 *          ascending `node_idx` order so per-row iteration (e.g. the outfall
 *          pass) matches a base-node-ascending scan bit-for-bit. The wide
 *          subtype arrays remain on NodeData until they are deleted at the end
 *          of the cutover (Phase 4 Stage D).
 *
 *          Structural mutations go through `set_node_type` (insert/move a row on
 *          a type change), `erase_node` (drop a row + renumber join keys on a
 *          base-node delete), and `rebuild_index` (re-derive the reverse map).
 *          `subtype_row` lives here, not on NodeData.
 *
 * @see src/engine/data/NodeData.hpp — base NodeData SoA
 * @see src/engine/hydraulics/XSectBatch.hpp — XSectGroups::build (batch pattern)
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_NODE_SUBTYPES_HPP
#define OPENSWMM_ENGINE_NODE_SUBTYPES_HPP

#include <algorithm>
#include <vector>
#include <cstdint>
#include <string>

#include "NodeData.hpp"

namespace openswmm {

// ============================================================================
// StorageData — side-table for STORAGE nodes
// ============================================================================

/**
 * @brief Dense SoA for storage-unit properties (one row per STORAGE node).
 * @details Row r corresponds to base node `node_idx[r]`. Mirrors the
 *          `storage_*` / `exfil_*` fields of NodeData.
 */
struct StorageData {
    /** @brief Base NodeData index this row belongs to (the join key). */
    std::vector<int>         node_idx;

    /** @brief Storage curve index into TableData (-1 = not tabulated). */
    std::vector<int>         curve;
    /** @brief Curve name for deferred resolution. */
    std::vector<std::string> curve_name;
    /** @brief Surface-area relation. Selects how a/b/c below are interpreted. */
    std::vector<StorageShape> shape;
    /** @brief Area coefficient A (legacy a1). FUNCTIONAL: `A·d^B`; geometric shapes:
     *  the linear term of `C + A·d + B·d²`. Derived from p1..p3 for the latter. */
    std::vector<double>      a;
    /** @brief Area coefficient B (legacy a2). FUNCTIONAL: the exponent; geometric
     *  shapes: the quadratic term. */
    std::vector<double>      b;
    /** @brief Area coefficient C (legacy a0). Baseline area at zero depth. */
    std::vector<double>      c;
    /** @brief Raw shape parameter 1 — major axis / base length L (geometric shapes only).
     *  Kept alongside the derived a/b/c so `.inp`/`.gpkg` round-trip losslessly; legacy
     *  discards these, which is why a legacy model cannot rewrite its own shape params. */
    std::vector<double>      p1;
    /** @brief Raw shape parameter 2 — minor axis / base width W. */
    std::vector<double>      p2;
    /** @brief Raw shape parameter 3 — side slope Z (run/rise), or height for PARABOLOID. */
    std::vector<double>      p3;
    /** @brief Seepage rate (project units/day). */
    std::vector<double>      seep_rate;
    /** @brief Fraction of potential evaporation realized (0-1). */
    std::vector<double>      evap_frac;
    /** @brief Evaporation loss this timestep (ft3). */
    std::vector<double>      evap_loss;
    /** @brief Exfiltration loss this timestep (ft3). */
    std::vector<double>      exfil_loss;
    /** @brief Green-Ampt suction head for exfiltration. */
    std::vector<double>      exfil_suction;
    /** @brief Green-Ampt saturated conductivity for exfiltration. */
    std::vector<double>      exfil_ksat;
    /** @brief Green-Ampt initial moisture deficit for exfiltration (0-1). */
    std::vector<double>      exfil_imd;

    /** @brief Number of storage rows. */
    int count() const noexcept { return static_cast<int>(node_idx.size()); }

    /** @brief Drop all rows (capacity retained). */
    void clear() noexcept {
        node_idx.clear(); curve.clear(); curve_name.clear();
        shape.clear();
        a.clear(); b.clear(); c.clear();
        p1.clear(); p2.clear(); p3.clear();
        seep_rate.clear(); evap_frac.clear(); evap_loss.clear(); exfil_loss.clear();
        exfil_suction.clear(); exfil_ksat.clear(); exfil_imd.clear();
    }

    /** @brief Reserve capacity for `n` rows. */
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        node_idx.reserve(un); curve.reserve(un); curve_name.reserve(un);
        shape.reserve(un);
        a.reserve(un); b.reserve(un); c.reserve(un);
        p1.reserve(un); p2.reserve(un); p3.reserve(un);
        seep_rate.reserve(un); evap_frac.reserve(un);
        evap_loss.reserve(un); exfil_loss.reserve(un);
        exfil_suction.reserve(un); exfil_ksat.reserve(un); exfil_imd.reserve(un);
    }

    /** @brief Insert a default storage row for base node `i`, keeping `node_idx`
     *  ascending; returns the inserted row index. Defaults match NodeData::resize.
     *  Ascending parse inserts at the end (O(1)); edits may insert in the middle. */
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(node_idx.begin(), node_idx.end(), i) - node_idx.begin());
        node_idx.insert(node_idx.begin() + p, i);
        curve.insert(curve.begin() + p, -1);
        curve_name.insert(curve_name.begin() + p, std::string{});
        // FUNCTIONAL is the historical default for a curve-less storage row: it is what
        // `curve < 0` has always meant, so existing callers see no behaviour change.
        shape.insert(shape.begin() + p, StorageShape::FUNCTIONAL);
        a.insert(a.begin() + p, 0.0);
        b.insert(b.begin() + p, 0.0);
        c.insert(c.begin() + p, 0.0);
        p1.insert(p1.begin() + p, 0.0);
        p2.insert(p2.begin() + p, 0.0);
        p3.insert(p3.begin() + p, 0.0);
        seep_rate.insert(seep_rate.begin() + p, 0.0);
        evap_frac.insert(evap_frac.begin() + p, 0.0);
        evap_loss.insert(evap_loss.begin() + p, 0.0);
        exfil_loss.insert(exfil_loss.begin() + p, 0.0);
        exfil_suction.insert(exfil_suction.begin() + p, 0.0);
        exfil_ksat.insert(exfil_ksat.begin() + p, 0.0);
        exfil_imd.insert(exfil_imd.begin() + p, 0.0);
        return static_cast<int>(p);
    }

    /** @brief Erase storage row `r` from every column. */
    void erase_at(int r) {
        const auto p = static_cast<std::ptrdiff_t>(r);
        node_idx.erase(node_idx.begin() + p);
        curve.erase(curve.begin() + p);
        curve_name.erase(curve_name.begin() + p);
        shape.erase(shape.begin() + p);
        a.erase(a.begin() + p);
        b.erase(b.begin() + p);
        c.erase(c.begin() + p);
        p1.erase(p1.begin() + p);
        p2.erase(p2.begin() + p);
        p3.erase(p3.begin() + p);
        seep_rate.erase(seep_rate.begin() + p);
        evap_frac.erase(evap_frac.begin() + p);
        evap_loss.erase(evap_loss.begin() + p);
        exfil_loss.erase(exfil_loss.begin() + p);
        exfil_suction.erase(exfil_suction.begin() + p);
        exfil_ksat.erase(exfil_ksat.begin() + p);
        exfil_imd.erase(exfil_imd.begin() + p);
    }
};

// ============================================================================
// OutfallData — side-table for OUTFALL nodes
// ============================================================================

/**
 * @brief Dense SoA for outfall boundary-condition properties (one row per OUTFALL).
 * @details Mirrors the `outfall_*` fields of NodeData.
 */
struct OutfallData {
    /** @brief Base NodeData index this row belongs to (the join key). */
    std::vector<int>         node_idx;

    /** @brief Boundary condition type (FREE/NORMAL/FIXED/TIDAL/TIMESERIES). */
    std::vector<OutfallType> bc_type;
    /** @brief Fixed stage, or tidal/timeseries table index (per bc_type).
     *  For TIDAL/TIMESERIES an unresolved reference is -1 (never 0, which is a
     *  valid table index). */
    std::vector<double>      param;
    /** @brief Stage-data table name for TIDAL/TIMESERIES, held for deferred
     *  name→index resolution in PostParseResolver (empty otherwise). */
    std::vector<std::string> param_name;
    /** @brief Flap gate present (0/1). */
    std::vector<uint8_t>     has_flap_gate;
    /** @brief Subcatchment index to route discharge to (-1 = none). */
    std::vector<int>         route_to;
    /** @brief Cached connected-conduit index (-1 = none). */
    std::vector<int>         link_idx;
    /** @brief Conduit offset at the outfall end. */
    std::vector<double>      link_offset;
    /** @brief Cached 2D surface head at the coupling point (sentinel -1e30). */
    std::vector<double>      head_2d;
    /** @brief Cached wet/dry ramp factor [0,1] for the 2D tailwater override
     *  (0 = dry → free discharge, 1 = wet → full tailwater); default 0. */
    std::vector<double>      ramp_2d;

    /** @brief Number of outfall rows. */
    int count() const noexcept { return static_cast<int>(node_idx.size()); }

    /** @brief Drop all rows (capacity retained). */
    void clear() noexcept {
        node_idx.clear(); bc_type.clear(); param.clear(); param_name.clear();
        has_flap_gate.clear(); route_to.clear();
        link_idx.clear(); link_offset.clear(); head_2d.clear();
        ramp_2d.clear();
    }

    /** @brief Reserve capacity for `n` rows. */
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        node_idx.reserve(un); bc_type.reserve(un); param.reserve(un);
        param_name.reserve(un);
        has_flap_gate.reserve(un); route_to.reserve(un);
        link_idx.reserve(un); link_offset.reserve(un); head_2d.reserve(un);
        ramp_2d.reserve(un);
    }

    /** @brief Insert a default outfall row for base node `i`, keeping `node_idx`
     *  ascending; returns the inserted row index. Defaults match NodeData::resize. */
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(node_idx.begin(), node_idx.end(), i) - node_idx.begin());
        node_idx.insert(node_idx.begin() + p, i);
        bc_type.insert(bc_type.begin() + p, OutfallType::FREE);
        param.insert(param.begin() + p, 0.0);
        param_name.insert(param_name.begin() + p, std::string{});
        has_flap_gate.insert(has_flap_gate.begin() + p, uint8_t{0});
        route_to.insert(route_to.begin() + p, -1);
        link_idx.insert(link_idx.begin() + p, -1);
        link_offset.insert(link_offset.begin() + p, 0.0);
        head_2d.insert(head_2d.begin() + p, -1.0e30);
        ramp_2d.insert(ramp_2d.begin() + p, 0.0);
        return static_cast<int>(p);
    }

    /** @brief Erase outfall row `r` from every column. */
    void erase_at(int r) {
        const auto p = static_cast<std::ptrdiff_t>(r);
        node_idx.erase(node_idx.begin() + p);
        bc_type.erase(bc_type.begin() + p);
        param.erase(param.begin() + p);
        param_name.erase(param_name.begin() + p);
        has_flap_gate.erase(has_flap_gate.begin() + p);
        route_to.erase(route_to.begin() + p);
        link_idx.erase(link_idx.begin() + p);
        link_offset.erase(link_offset.begin() + p);
        head_2d.erase(head_2d.begin() + p);
        ramp_2d.erase(ramp_2d.begin() + p);
    }
};

// ============================================================================
// DividerData — side-table for DIVIDER nodes
// ============================================================================

/**
 * @brief Dense SoA for flow-divider properties (one row per DIVIDER node).
 * @details Mirrors the `divider_*` fields of NodeData.
 */
struct DividerData {
    /** @brief Base NodeData index this row belongs to (the join key). */
    std::vector<int>         node_idx;

    /** @brief Diversion method (CUTOFF/OVERFLOW/TABULAR/WEIR). */
    std::vector<DividerType> method;
    /** @brief Cutoff flow for CUTOFF dividers. */
    std::vector<double>      cutoff;
    /** @brief Weir discharge coefficient for WEIR dividers. */
    std::vector<double>      cd;
    /** @brief Weir max depth for WEIR dividers. */
    std::vector<double>      max_depth;
    /** @brief Diversion curve index for TABULAR dividers (-1 = none). */
    std::vector<int>         curve;
    /** @brief Diversion link index (-1 = not set). */
    std::vector<int>         link;
    /** @brief Diversion link name (deferred resolution). */
    std::vector<std::string> link_name;
    /** @brief Diversion curve name (deferred resolution, TABULAR only). */
    std::vector<std::string> curve_name;

    /** @brief Number of divider rows. */
    int count() const noexcept { return static_cast<int>(node_idx.size()); }

    /** @brief Drop all rows (capacity retained). */
    void clear() noexcept {
        node_idx.clear(); method.clear(); cutoff.clear();
        cd.clear(); max_depth.clear(); curve.clear(); link.clear();
        link_name.clear(); curve_name.clear();
    }

    /** @brief Reserve capacity for `n` rows. */
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        node_idx.reserve(un); method.reserve(un); cutoff.reserve(un);
        cd.reserve(un); max_depth.reserve(un); curve.reserve(un); link.reserve(un);
        link_name.reserve(un); curve_name.reserve(un);
    }

    /** @brief Insert a default divider row for base node `i`, keeping `node_idx`
     *  ascending; returns the inserted row index. Defaults match NodeData::resize. */
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(node_idx.begin(), node_idx.end(), i) - node_idx.begin());
        node_idx.insert(node_idx.begin() + p, i);
        method.insert(method.begin() + p, DividerType::CUTOFF);
        cutoff.insert(cutoff.begin() + p, 0.0);
        cd.insert(cd.begin() + p, 0.0);
        max_depth.insert(max_depth.begin() + p, 0.0);
        curve.insert(curve.begin() + p, -1);
        link.insert(link.begin() + p, -1);
        link_name.insert(link_name.begin() + p, std::string{});
        curve_name.insert(curve_name.begin() + p, std::string{});
        return static_cast<int>(p);
    }

    /** @brief Erase divider row `r` from every column. */
    void erase_at(int r) {
        const auto p = static_cast<std::ptrdiff_t>(r);
        node_idx.erase(node_idx.begin() + p);
        method.erase(method.begin() + p);
        cutoff.erase(cutoff.begin() + p);
        cd.erase(cd.begin() + p);
        max_depth.erase(max_depth.begin() + p);
        curve.erase(curve.begin() + p);
        link.erase(link.begin() + p);
        link_name.erase(link_name.begin() + p);
        curve_name.erase(curve_name.begin() + p);
    }
};

// ============================================================================
// NodeSubtypes — container + reverse map + build/verify
// ============================================================================

/**
 * @brief Owns the three node subtype side-tables plus the reverse index map.
 *
 * @details `subtype_row[i]` is the row of node `i` within whichever side-table
 *          matches `nodes.type[i]` (i.e. an index into storages/outfalls/
 *          dividers), or -1 for JUNCTION nodes. Together with each side-table's
 *          `node_idx`, this provides O(1) lookup in both directions.
 */
struct NodeSubtypes {
    StorageData      storages;
    OutfallData      outfalls;
    DividerData      dividers;

    /** @brief base node index → row in its subtype table (-1 for junctions). */
    std::vector<int> subtype_row;

    /** @brief Drop all rows and the reverse map. */
    void clear() noexcept {
        storages.clear();
        outfalls.clear();
        dividers.clear();
        subtype_row.clear();
    }

    /**
     * @brief Recompute the reverse map (`subtype_row`) from the side-table rows.
     *
     * @details Phase 4: the side-tables are the authoritative store (no longer
     *          built from the wide arrays), so this only re-derives the O(1)
     *          base→row index from each table's `node_idx`. Sizes `subtype_row`
     *          to @p n_nodes (junctions and untyped nodes stay -1). Used after a
     *          structural edit (insert/erase/convert) and as the end-of-parse /
     *          hydraulics-init consistency pass. Does not touch row data.
     */
    void rebuild_index(int n_nodes) {
        subtype_row.assign(static_cast<std::size_t>(n_nodes), -1);
        for (int r = 0; r < storages.count(); ++r)
            subtype_row[static_cast<std::size_t>(storages.node_idx[static_cast<std::size_t>(r)])] = r;
        for (int r = 0; r < outfalls.count(); ++r)
            subtype_row[static_cast<std::size_t>(outfalls.node_idx[static_cast<std::size_t>(r)])] = r;
        for (int r = 0; r < dividers.count(); ++r)
            subtype_row[static_cast<std::size_t>(dividers.node_idx[static_cast<std::size_t>(r)])] = r;
    }

    /**
     * @brief Set node @p i to @p t, creating/removing/moving its subtype row so
     *        the side-table stays the single source of truth. Returns the new
     *        subtype row (or -1 for JUNCTION). Also sets `nodes.type[i]`.
     *
     * @details Idempotent: re-setting the same subtype type returns the existing
     *          row (parse duplicate lines, no-op). On a real type change the old
     *          row is erased and a fresh default row inserted (keeping `node_idx`
     *          ascending). Fresh ascending parse appends at the end in O(1); any
     *          mid-table insert or erase triggers an O(n) `rebuild_index`. New
     *          rows carry NodeData::resize defaults (so a converted node starts
     *          clean, matching the legacy clear-then-apply-defaults path).
     */
    int set_node_type(NodeData& nodes, int i, NodeType t) {
        const auto ui = static_cast<std::size_t>(i);
        if (i >= static_cast<int>(subtype_row.size()))
            subtype_row.resize(static_cast<std::size_t>(i) + 1, -1);

        const NodeType old = nodes.type[ui];
        const int existing = subtype_row[ui];
        if (old == t && existing >= 0)
            return existing;  // already this subtype with a row — idempotent.

        bool shifted = false;
        if (existing >= 0) {  // remove the old subtype row (real re-type/convert).
            switch (old) {
                case NodeType::STORAGE: storages.erase_at(existing); break;
                case NodeType::OUTFALL: outfalls.erase_at(existing); break;
                case NodeType::DIVIDER: dividers.erase_at(existing); break;
                default: break;
            }
            shifted = true;
        }

        nodes.type[ui] = t;

        int row = -1;
        switch (t) {
            case NodeType::STORAGE: row = storages.add_default(i);
                                    if (row != storages.count() - 1) shifted = true; break;
            case NodeType::OUTFALL: row = outfalls.add_default(i);
                                    if (row != outfalls.count() - 1) shifted = true; break;
            case NodeType::DIVIDER: row = dividers.add_default(i);
                                    if (row != dividers.count() - 1) shifted = true; break;
            case NodeType::JUNCTION:
            default: break;  // no subtype row
        }

        if (shifted) {
            rebuild_index(nodes.count());
            return (t == NodeType::JUNCTION) ? -1 : subtype_row[ui];
        }
        subtype_row[ui] = row;  // O(1) ascending-parse / end-insert path
        return row;
    }

    /**
     * @brief Drop node @p i's subtype row and renumber the join keys after a base
     *        node erase. Call after `NodeData::erase_at(i)` (so @p n_after is the
     *        new node count). Every `node_idx > i` shifts down by one.
     */
    void erase_node(int i, int n_after) {
        // Erase the row whose node_idx == i (in whichever table holds it).
        auto drop = [i](auto& tbl) -> bool {
            for (int r = 0; r < tbl.count(); ++r)
                if (tbl.node_idx[static_cast<std::size_t>(r)] == i) { tbl.erase_at(r); return true; }
            return false;
        };
        drop(storages) || drop(outfalls) || drop(dividers);

        auto shift = [i](std::vector<int>& keys) {
            for (auto& k : keys) if (k > i) --k;
        };
        shift(storages.node_idx);
        shift(outfalls.node_idx);
        shift(dividers.node_idx);

        rebuild_index(n_after);
    }

    /** @brief Storage side-table row for base node @p i, or -1 if @p i is not a
     *  storage node (or the side-table is unbuilt). O(1). */
    int storage_row(int i) const noexcept {
        if (i >= 0 && i < static_cast<int>(subtype_row.size())) {
            const int r = subtype_row[static_cast<std::size_t>(i)];
            if (r >= 0 && r < storages.count() &&
                storages.node_idx[static_cast<std::size_t>(r)] == i)
                return r;
        }
        return -1;
    }

    /** @brief Outfall side-table row for base node @p i, or -1 if not an outfall
     *  (or the side-table is unbuilt). O(1). */
    int outfall_row(int i) const noexcept {
        if (i >= 0 && i < static_cast<int>(subtype_row.size())) {
            const int r = subtype_row[static_cast<std::size_t>(i)];
            if (r >= 0 && r < outfalls.count() &&
                outfalls.node_idx[static_cast<std::size_t>(r)] == i)
                return r;
        }
        return -1;
    }

    /** @brief Divider side-table row for base node @p i, or -1 if not a divider
     *  (or the side-table is unbuilt). O(1). */
    int divider_row(int i) const noexcept {
        if (i >= 0 && i < static_cast<int>(subtype_row.size())) {
            const int r = subtype_row[static_cast<std::size_t>(i)];
            if (r >= 0 && r < dividers.count() &&
                dividers.node_idx[static_cast<std::size_t>(r)] == i)
                return r;
        }
        return -1;
    }
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_NODE_SUBTYPES_HPP
