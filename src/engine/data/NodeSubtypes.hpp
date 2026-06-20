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
 *          **Phase 1 (shadow):** the side-tables are *built from* the existing
 *          wide arrays at init and not yet read by the solver. They mirror the
 *          wide arrays exactly (verifiable via verify_mirror()), so behaviour is
 *          unchanged. Later phases switch compute and IO to read the side-tables
 *          and then remove the wide subtype arrays from NodeData.
 *
 *          Design note: building once at init from the resolved wide arrays
 *          (mirroring xsect_batch::XSectGroups::build and StructureSolver::init)
 *          keeps NodeData's resize/grow_to/erase_at/shrink_to_fit untouched in
 *          this phase — a single build site instead of threading population
 *          through every parser. `subtype_row` lives here, not on NodeData, for
 *          the same reason.
 *
 * @see src/engine/data/NodeData.hpp — base NodeData SoA
 * @see src/engine/hydraulics/XSectBatch.hpp — XSectGroups::build (batch pattern)
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_NODE_SUBTYPES_HPP
#define OPENSWMM_ENGINE_NODE_SUBTYPES_HPP

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

    /** @brief Storage curve index into TableData (-1 = functional A·d^B + C). */
    std::vector<int>         curve;
    /** @brief Curve name for deferred resolution. */
    std::vector<std::string> curve_name;
    /** @brief Functional area parameter A. */
    std::vector<double>      a;
    /** @brief Functional area parameter B. */
    std::vector<double>      b;
    /** @brief Functional area parameter C (baseline area). */
    std::vector<double>      c;
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
        a.clear(); b.clear(); c.clear();
        seep_rate.clear(); evap_frac.clear(); evap_loss.clear(); exfil_loss.clear();
        exfil_suction.clear(); exfil_ksat.clear(); exfil_imd.clear();
    }

    /** @brief Reserve capacity for `n` rows. */
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        node_idx.reserve(un); curve.reserve(un); curve_name.reserve(un);
        a.reserve(un); b.reserve(un); c.reserve(un);
        seep_rate.reserve(un); evap_frac.reserve(un);
        evap_loss.reserve(un); exfil_loss.reserve(un);
        exfil_suction.reserve(un); exfil_ksat.reserve(un); exfil_imd.reserve(un);
    }

    /** @brief Append one storage row copied from base node `i` of `nodes`. */
    void push_from(const NodeData& nodes, int i) {
        const auto ui = static_cast<std::size_t>(i);
        node_idx.push_back(i);
        curve.push_back(nodes.storage_curve[ui]);
        curve_name.push_back(nodes.storage_curve_name[ui]);
        a.push_back(nodes.storage_a[ui]);
        b.push_back(nodes.storage_b[ui]);
        c.push_back(nodes.storage_c[ui]);
        seep_rate.push_back(nodes.storage_seep_rate[ui]);
        evap_frac.push_back(nodes.storage_evap_frac[ui]);
        evap_loss.push_back(nodes.storage_evap_loss[ui]);
        exfil_loss.push_back(nodes.storage_exfil_loss[ui]);
        exfil_suction.push_back(nodes.exfil_suction[ui]);
        exfil_ksat.push_back(nodes.exfil_ksat[ui]);
        exfil_imd.push_back(nodes.exfil_imd[ui]);
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
    /** @brief Fixed stage, or tidal/timeseries table index (per bc_type). */
    std::vector<double>      param;
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

    /** @brief Number of outfall rows. */
    int count() const noexcept { return static_cast<int>(node_idx.size()); }

    /** @brief Drop all rows (capacity retained). */
    void clear() noexcept {
        node_idx.clear(); bc_type.clear(); param.clear();
        has_flap_gate.clear(); route_to.clear();
        link_idx.clear(); link_offset.clear(); head_2d.clear();
    }

    /** @brief Reserve capacity for `n` rows. */
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        node_idx.reserve(un); bc_type.reserve(un); param.reserve(un);
        has_flap_gate.reserve(un); route_to.reserve(un);
        link_idx.reserve(un); link_offset.reserve(un); head_2d.reserve(un);
    }

    /** @brief Append one outfall row copied from base node `i` of `nodes`. */
    void push_from(const NodeData& nodes, int i) {
        const auto ui = static_cast<std::size_t>(i);
        node_idx.push_back(i);
        bc_type.push_back(nodes.outfall_type[ui]);
        param.push_back(nodes.outfall_param[ui]);
        has_flap_gate.push_back(nodes.outfall_has_flap_gate[ui]);
        route_to.push_back(nodes.outfall_route_to[ui]);
        link_idx.push_back(nodes.outfall_link_idx[ui]);
        link_offset.push_back(nodes.outfall_link_offset[ui]);
        head_2d.push_back(nodes.outfall_2d_head[ui]);
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

    /** @brief Append one divider row copied from base node `i` of `nodes`. */
    void push_from(const NodeData& nodes, int i) {
        const auto ui = static_cast<std::size_t>(i);
        node_idx.push_back(i);
        method.push_back(nodes.divider_type[ui]);
        cutoff.push_back(nodes.divider_cutoff[ui]);
        cd.push_back(nodes.divider_cd[ui]);
        max_depth.push_back(nodes.divider_max_depth[ui]);
        curve.push_back(nodes.divider_curve[ui]);
        link.push_back(nodes.divider_link[ui]);
        link_name.push_back(nodes.divider_link_name[ui]);
        curve_name.push_back(nodes.divider_curve_name[ui]);
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
     * @brief (Re)build all side-tables from the resolved wide NodeData arrays.
     *
     * @details Single build site (called at hydraulics init, after PostParse
     *          resolution). Idempotent: clears first, then one pass over nodes.
     *          Junctions contribute no subtype row (subtype_row = -1).
     */
    void build(const NodeData& nodes) {
        clear();
        const int n = nodes.count();
        subtype_row.assign(static_cast<std::size_t>(n), -1);

        // Pre-count for tight allocation (the memory win this refactor targets).
        int n_storage = 0, n_outfall = 0, n_divider = 0;
        for (int i = 0; i < n; ++i) {
            switch (nodes.type[static_cast<std::size_t>(i)]) {
                case NodeType::STORAGE: ++n_storage; break;
                case NodeType::OUTFALL: ++n_outfall; break;
                case NodeType::DIVIDER: ++n_divider; break;
                default: break;
            }
        }
        storages.reserve(n_storage);
        outfalls.reserve(n_outfall);
        dividers.reserve(n_divider);

        for (int i = 0; i < n; ++i) {
            switch (nodes.type[static_cast<std::size_t>(i)]) {
                case NodeType::STORAGE:
                    subtype_row[static_cast<std::size_t>(i)] = storages.count();
                    storages.push_from(nodes, i);
                    break;
                case NodeType::OUTFALL:
                    subtype_row[static_cast<std::size_t>(i)] = outfalls.count();
                    outfalls.push_from(nodes, i);
                    break;
                case NodeType::DIVIDER:
                    subtype_row[static_cast<std::size_t>(i)] = dividers.count();
                    dividers.push_from(nodes, i);
                    break;
                case NodeType::JUNCTION:
                default:
                    break;  // subtype_row stays -1
            }
        }
    }

    /**
     * @brief Debug self-check: every side-table row equals the matching wide
     *        NodeData field, and the reverse map is consistent.
     * @return true if the side-tables exactly mirror NodeData.
     * @note Phase-1 shadow guard. Intended for use inside assert() in debug
     *       builds; not called on the release hot path.
     */
    bool verify_mirror(const NodeData& nodes) const {
        const int n = nodes.count();
        if (static_cast<int>(subtype_row.size()) != n) return false;

        for (int i = 0; i < n; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            const int r = subtype_row[ui];
            switch (nodes.type[ui]) {
                case NodeType::STORAGE: {
                    if (r < 0 || r >= storages.count()) return false;
                    const auto ur = static_cast<std::size_t>(r);
                    if (storages.node_idx[ur] != i) return false;
                    if (storages.curve[ur]      != nodes.storage_curve[ui]) return false;
                    if (storages.curve_name[ur] != nodes.storage_curve_name[ui]) return false;
                    if (storages.a[ur] != nodes.storage_a[ui]) return false;
                    if (storages.b[ur] != nodes.storage_b[ui]) return false;
                    if (storages.c[ur] != nodes.storage_c[ui]) return false;
                    if (storages.seep_rate[ur]     != nodes.storage_seep_rate[ui]) return false;
                    if (storages.evap_frac[ur]     != nodes.storage_evap_frac[ui]) return false;
                    if (storages.evap_loss[ur]     != nodes.storage_evap_loss[ui]) return false;
                    if (storages.exfil_loss[ur]    != nodes.storage_exfil_loss[ui]) return false;
                    if (storages.exfil_suction[ur] != nodes.exfil_suction[ui]) return false;
                    if (storages.exfil_ksat[ur]    != nodes.exfil_ksat[ui]) return false;
                    if (storages.exfil_imd[ur]     != nodes.exfil_imd[ui]) return false;
                    break;
                }
                case NodeType::OUTFALL: {
                    if (r < 0 || r >= outfalls.count()) return false;
                    const auto ur = static_cast<std::size_t>(r);
                    if (outfalls.node_idx[ur] != i) return false;
                    if (outfalls.bc_type[ur]       != nodes.outfall_type[ui]) return false;
                    if (outfalls.param[ur]         != nodes.outfall_param[ui]) return false;
                    if (outfalls.has_flap_gate[ur] != nodes.outfall_has_flap_gate[ui]) return false;
                    if (outfalls.route_to[ur]      != nodes.outfall_route_to[ui]) return false;
                    if (outfalls.link_idx[ur]      != nodes.outfall_link_idx[ui]) return false;
                    if (outfalls.link_offset[ur]   != nodes.outfall_link_offset[ui]) return false;
                    if (outfalls.head_2d[ur]       != nodes.outfall_2d_head[ui]) return false;
                    break;
                }
                case NodeType::DIVIDER: {
                    if (r < 0 || r >= dividers.count()) return false;
                    const auto ur = static_cast<std::size_t>(r);
                    if (dividers.node_idx[ur] != i) return false;
                    if (dividers.method[ur]    != nodes.divider_type[ui]) return false;
                    if (dividers.cutoff[ur]    != nodes.divider_cutoff[ui]) return false;
                    if (dividers.cd[ur]        != nodes.divider_cd[ui]) return false;
                    if (dividers.max_depth[ur] != nodes.divider_max_depth[ui]) return false;
                    if (dividers.curve[ur]     != nodes.divider_curve[ui]) return false;
                    if (dividers.link[ur]      != nodes.divider_link[ui]) return false;
                    if (dividers.link_name[ur] != nodes.divider_link_name[ui]) return false;
                    if (dividers.curve_name[ur] != nodes.divider_curve_name[ui]) return false;
                    break;
                }
                case NodeType::JUNCTION:
                default:
                    if (r != -1) return false;
                    break;
            }
        }
        return true;
    }
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_NODE_SUBTYPES_HPP
