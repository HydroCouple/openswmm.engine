/**
 * @file LinkSubtypes.hpp
 * @brief Relational (normalized) Structure-of-Arrays side-tables for link subtypes.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_LINK_SUBTYPES_HPP
#define OPENSWMM_ENGINE_LINK_SUBTYPES_HPP

#include <algorithm>
#include <vector>
#include <cstdint>
#include <string>

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
    std::vector<int>    link_idx;       ///< Base LinkData index (join key)

    std::vector<double> roughness;
    std::vector<double> length;
    std::vector<double> slope;
    std::vector<double> mod_length;
    std::vector<int>    barrels;
    std::vector<double> beta;
    std::vector<double> rough_factor;
    std::vector<double> q_full;
    std::vector<double> q_max;
    std::vector<double> loss_inlet;
    std::vector<double> loss_outlet;
    std::vector<double> loss_avg;
    std::vector<double> seep_rate;
    std::vector<int>    culvert_code;

    // Phase 6 Stage B: mutable per-step conduit state moved off wide LinkData.
    std::vector<double>  evap_loss_rate;      ///< per-step evaporation loss
    std::vector<double>  seep_loss_rate;      ///< per-step seepage loss
    std::vector<uint8_t> normal_flow_limited; ///< per-step flag (reset each step)
    std::vector<uint8_t> inlet_control;       ///< per-step culvert inlet-control flag
    std::vector<int8_t>  full_state;          ///< per-step up/down full bitmask

    int count() const noexcept { return static_cast<int>(link_idx.size()); }

    void clear() noexcept {
        link_idx.clear(); roughness.clear(); length.clear(); slope.clear();
        mod_length.clear(); barrels.clear(); beta.clear(); rough_factor.clear();
        q_full.clear(); q_max.clear(); loss_inlet.clear(); loss_outlet.clear();
        loss_avg.clear(); seep_rate.clear(); culvert_code.clear();
        evap_loss_rate.clear(); seep_loss_rate.clear(); normal_flow_limited.clear();
        inlet_control.clear(); full_state.clear();
    }

    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        link_idx.reserve(un); roughness.reserve(un); length.reserve(un); slope.reserve(un);
        mod_length.reserve(un); barrels.reserve(un); beta.reserve(un); rough_factor.reserve(un);
        q_full.reserve(un); q_max.reserve(un); loss_inlet.reserve(un); loss_outlet.reserve(un);
        loss_avg.reserve(un); seep_rate.reserve(un); culvert_code.reserve(un);
        evap_loss_rate.reserve(un); seep_loss_rate.reserve(un); normal_flow_limited.reserve(un);
        inlet_control.reserve(un); full_state.reserve(un);
    }

    /// Insert a default conduit row for base link @p i, keeping link_idx
    /// ascending; returns the inserted row. Defaults match LinkData::resize.
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(link_idx.begin(), link_idx.end(), i) - link_idx.begin());
        link_idx.insert(link_idx.begin() + p, i);
        roughness.insert(roughness.begin() + p, 0.01);
        length.insert(length.begin() + p, 0.0);
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
        return static_cast<int>(p);
    }

    void erase_at(int r) {
        const auto p = static_cast<std::ptrdiff_t>(r);
        link_idx.erase(link_idx.begin() + p);
        roughness.erase(roughness.begin() + p);
        length.erase(length.begin() + p);
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
    std::vector<int>     link_idx;
    std::vector<int>     curve;          ///< Pump curve table index (-1 = ideal)
    std::vector<uint8_t> init_state;     ///< Initial on/off (was vector<bool>)
    std::vector<double>  startup;        ///< Startup depth
    std::vector<double>  shutoff;        ///< Shutoff depth
    std::vector<int>     curve_type;     ///< Pump curve type (1-5, 6=ideal; -1 unset)

    int count() const noexcept { return static_cast<int>(link_idx.size()); }

    void clear() noexcept {
        link_idx.clear(); curve.clear(); init_state.clear();
        startup.clear(); shutoff.clear(); curve_type.clear();
    }

    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        link_idx.reserve(un); curve.reserve(un); init_state.reserve(un);
        startup.reserve(un); shutoff.reserve(un); curve_type.reserve(un);
    }

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
    std::vector<int>    link_idx;
    std::vector<double> orifice_type;    ///< 0 = BOTTOM, 1 = SIDE (legacy param1)
    std::vector<double> cd;              ///< Discharge coefficient
    std::vector<double> orate;           ///< Open/close time (s)

    int count() const noexcept { return static_cast<int>(link_idx.size()); }

    void clear() noexcept {
        link_idx.clear(); orifice_type.clear(); cd.clear(); orate.clear();
    }
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        link_idx.reserve(un); orifice_type.reserve(un); cd.reserve(un); orate.reserve(un);
    }
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(link_idx.begin(), link_idx.end(), i) - link_idx.begin());
        link_idx.insert(link_idx.begin() + p, i);
        orifice_type.insert(orifice_type.begin() + p, 0.0);
        cd.insert(cd.begin() + p, 0.0);
        orate.insert(orate.begin() + p, 0.0);
        return static_cast<int>(p);
    }
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
    std::vector<int>    link_idx;
    std::vector<double> weir_type;       ///< TRANSVERSE/SIDEFLOW/V-NOTCH/TRAPEZOIDAL (legacy param1)
    std::vector<double> cd;              ///< Discharge coefficient
    std::vector<double> end_contractions;///< End contractions (legacy param2)
    std::vector<double> crest_height;

    int count() const noexcept { return static_cast<int>(link_idx.size()); }

    void clear() noexcept {
        link_idx.clear(); weir_type.clear(); cd.clear();
        end_contractions.clear(); crest_height.clear();
    }
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        link_idx.reserve(un); weir_type.reserve(un); cd.reserve(un);
        end_contractions.reserve(un); crest_height.reserve(un);
    }
    int add_default(int i) {
        const auto p = static_cast<std::ptrdiff_t>(
            std::lower_bound(link_idx.begin(), link_idx.end(), i) - link_idx.begin());
        link_idx.insert(link_idx.begin() + p, i);
        weir_type.insert(weir_type.begin() + p, 0.0);
        cd.insert(cd.begin() + p, 0.0);
        end_contractions.insert(end_contractions.begin() + p, 0.0);
        crest_height.insert(crest_height.begin() + p, 0.0);
        return static_cast<int>(p);
    }
    void erase_at(int r) {
        const auto p = static_cast<std::ptrdiff_t>(r);
        link_idx.erase(link_idx.begin() + p);
        weir_type.erase(weir_type.begin() + p);
        cd.erase(cd.begin() + p);
        end_contractions.erase(end_contractions.begin() + p);
        crest_height.erase(crest_height.begin() + p);
    }
};

// ============================================================================
// OutletData — side-table for OUTLET links
//   param1 → outlet_type (rating: 0 FUNC/HEAD,1 FUNC/DEPTH,2 TAB/HEAD,3 TAB/DEPTH)
//   cd → coeff (functional C1), param2 → expon (functional C2)
// ============================================================================

struct OutletData {
    std::vector<int>    link_idx;
    std::vector<double> outlet_type;     ///< Rating type (legacy param1)
    std::vector<double> crest_height;    ///< Offset/crest
    std::vector<double> coeff;           ///< Functional C1 (legacy cd)
    std::vector<double> expon;           ///< Functional C2 exponent (legacy param2)
    std::vector<int>    curve;           ///< TABULAR rating-curve index (legacy pump_curve reuse; -1 = functional)

    int count() const noexcept { return static_cast<int>(link_idx.size()); }

    void clear() noexcept {
        link_idx.clear(); outlet_type.clear(); crest_height.clear();
        coeff.clear(); expon.clear(); curve.clear();
    }
    void reserve(int n) {
        const auto un = static_cast<std::size_t>(n);
        link_idx.reserve(un); outlet_type.reserve(un); crest_height.reserve(un);
        coeff.reserve(un); expon.reserve(un); curve.reserve(un);
    }
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
    PumpData    pumps;
    OrificeData orifices;
    WeirData    weirs;
    OutletData  outlets;

    /** @brief base link index → row in its subtype table (-1 if unset). */
    std::vector<int> subtype_row;

    /**
     * @brief Stage-A scaffold: (re)build all side-tables from the wide LinkData
     *        subtype arrays. TEMPORARY — during the cutover the wide arrays
     *        remain authoritative and this mirrors them so repointed readers see
     *        identical data (parity-safe). Removed at Stage D when the writers
     *        become side-table-authoritative and the wide arrays are deleted.
     */
    void build(const LinkData& L) {
        clear();
        const int n = L.count();
        for (int i = 0; i < n; ++i) {
            const auto u = static_cast<std::size_t>(i);
            switch (L.type[u]) {
                case LinkType::CONDUIT: {
                    const auto r = static_cast<std::size_t>(conduits.add_default(i));
                    conduits.roughness[r]    = L.roughness[u];
                    conduits.length[r]       = L.length[u];
                    conduits.slope[r]        = L.slope[u];
                    conduits.mod_length[r]   = L.mod_length[u];
                    conduits.barrels[r]      = L.barrels[u];
                    conduits.beta[r]         = L.beta[u];
                    conduits.rough_factor[r] = L.rough_factor[u];
                    conduits.q_full[r]       = L.q_full[u];
                    conduits.q_max[r]        = L.q_max[u];
                    conduits.loss_inlet[r]   = L.loss_inlet[u];
                    conduits.loss_outlet[r]  = L.loss_outlet[u];
                    conduits.loss_avg[r]     = L.loss_avg[u];
                    conduits.seep_rate[r]    = L.seep_rate[u];
                    conduits.culvert_code[r] = L.culvert_code[u];
                } break;
                case LinkType::PUMP: {
                    const auto r = static_cast<std::size_t>(pumps.add_default(i));
                    pumps.curve[r]      = L.pump_curve[u];
                    pumps.init_state[r] = L.pump_init_state[u] ? uint8_t{1} : uint8_t{0};
                    pumps.startup[r]    = L.pump_startup[u];
                    pumps.shutoff[r]    = L.pump_shutoff[u];
                    pumps.curve_type[r] = L.pump_curve_type[u];
                } break;
                case LinkType::ORIFICE: {
                    const auto r = static_cast<std::size_t>(orifices.add_default(i));
                    orifices.orifice_type[r] = L.param1[u];
                    orifices.cd[r]           = L.cd[u];
                    orifices.orate[r]        = L.orate[u];
                } break;
                case LinkType::WEIR: {
                    const auto r = static_cast<std::size_t>(weirs.add_default(i));
                    weirs.weir_type[r]        = L.param1[u];
                    weirs.cd[r]               = L.cd[u];
                    weirs.end_contractions[r] = L.param2[u];
                    weirs.crest_height[r]     = L.crest_height[u];
                } break;
                case LinkType::OUTLET: {
                    const auto r = static_cast<std::size_t>(outlets.add_default(i));
                    outlets.outlet_type[r]  = L.param1[u];
                    outlets.crest_height[r] = L.crest_height[u];
                    outlets.coeff[r]        = L.cd[u];
                    outlets.expon[r]        = L.param2[u];
                    outlets.curve[r]        = L.pump_curve[u];   // TABULAR rating curve index
                } break;
            }
        }
        rebuild_index(n);
    }

    /**
     * @brief Stage-A scaffold: (re)build the mirror only if it is stale, i.e.
     *        the reverse-index size no longer matches the base link count.
     * @details Cheap O(1) guard; the O(n) @ref build runs only when needed.
     *          In production @ref build already ran at engine init so this is a
     *          no-op. It exists so unit tests / callers that drive a solver on a
     *          hand-built `SimulationContext` (without the engine init that calls
     *          @ref build) still see a populated mirror before any side-table
     *          read. TEMPORARY — removed together with @ref build at Stage D,
     *          once the side-tables are written authoritatively at parse/edit.
     */
    void ensure_built(const LinkData& L) {
        if (subtype_row.size() != static_cast<std::size_t>(L.count()))
            build(L);
    }

    void clear() noexcept {
        conduits.clear(); pumps.clear(); orifices.clear();
        weirs.clear(); outlets.clear(); subtype_row.clear();
    }

    /// Recompute `subtype_row` from each table's `link_idx` (no row data touched).
    void rebuild_index(int n_links) {
        subtype_row.assign(static_cast<std::size_t>(n_links), -1);
        auto idx = [&](const std::vector<int>& keys) {
            for (int r = 0; r < static_cast<int>(keys.size()); ++r)
                subtype_row[static_cast<std::size_t>(keys[static_cast<std::size_t>(r)])] = r;
        };
        idx(conduits.link_idx); idx(pumps.link_idx); idx(orifices.link_idx);
        idx(weirs.link_idx); idx(outlets.link_idx);
    }

    /// Set link @p i to @p t, creating/removing/moving its subtype row. Returns
    /// the new subtype row. Also sets `links.type[i]`. Idempotent for the same
    /// type. Mirrors NodeSubtypes::set_node_type.
    int set_link_type(LinkData& links, int i, LinkType t) {
        const auto ui = static_cast<std::size_t>(i);
        if (i >= static_cast<int>(subtype_row.size()))
            subtype_row.resize(static_cast<std::size_t>(i) + 1, -1);

        const LinkType old = links.type[ui];
        const int existing = subtype_row[ui];
        if (old == t && existing >= 0)
            return existing;  // already this subtype with a row — idempotent

        bool shifted = false;
        if (existing >= 0) {  // remove the old subtype row (real re-type/convert)
            switch (old) {
                case LinkType::CONDUIT: conduits.erase_at(existing); break;
                case LinkType::PUMP:    pumps.erase_at(existing);    break;
                case LinkType::ORIFICE: orifices.erase_at(existing); break;
                case LinkType::WEIR:    weirs.erase_at(existing);    break;
                case LinkType::OUTLET:  outlets.erase_at(existing);  break;
            }
            shifted = true;
        }

        links.type[ui] = t;

        int row = -1;
        switch (t) {
            case LinkType::CONDUIT: row = conduits.add_default(i);
                                    if (row != conduits.count() - 1) shifted = true; break;
            case LinkType::PUMP:    row = pumps.add_default(i);
                                    if (row != pumps.count() - 1) shifted = true; break;
            case LinkType::ORIFICE: row = orifices.add_default(i);
                                    if (row != orifices.count() - 1) shifted = true; break;
            case LinkType::WEIR:    row = weirs.add_default(i);
                                    if (row != weirs.count() - 1) shifted = true; break;
            case LinkType::OUTLET:  row = outlets.add_default(i);
                                    if (row != outlets.count() - 1) shifted = true; break;
        }

        if (shifted) {
            rebuild_index(links.count());
            return subtype_row[ui];
        }
        subtype_row[ui] = row;  // O(1) ascending-parse / end-insert path
        return row;
    }

    /// Drop link @p i's subtype row and renumber join keys after a base link
    /// erase. Call after `LinkData::erase_at(i)` (@p n_after = new link count).
    void erase_link(int i, int n_after) {
        auto drop = [i](auto& tbl) -> bool {
            for (int r = 0; r < tbl.count(); ++r)
                if (tbl.link_idx[static_cast<std::size_t>(r)] == i) { tbl.erase_at(r); return true; }
            return false;
        };
        drop(conduits) || drop(pumps) || drop(orifices) || drop(weirs) || drop(outlets);

        auto shift = [i](std::vector<int>& keys) { for (auto& k : keys) if (k > i) --k; };
        shift(conduits.link_idx); shift(pumps.link_idx); shift(orifices.link_idx);
        shift(weirs.link_idx); shift(outlets.link_idx);

        rebuild_index(n_after);
    }

    // O(1) row helpers (validate link_idx[r] == i, like NodeSubtypes::*_row).
    int conduit_row(int i) const noexcept { return row_in(i, conduits.link_idx); }
    int pump_row(int i)    const noexcept { return row_in(i, pumps.link_idx); }
    int orifice_row(int i) const noexcept { return row_in(i, orifices.link_idx); }
    int weir_row(int i)    const noexcept { return row_in(i, weirs.link_idx); }
    int outlet_row(int i)  const noexcept { return row_in(i, outlets.link_idx); }

private:
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

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_LINK_SUBTYPES_HPP
