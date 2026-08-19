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
 * @file WaterAgeData.hpp
 * @brief Water-age tracking data (water age plan §1–§2, phase A1a).
 *
 * @details Age is the reserved species `__WATER_AGE__` (registry kind
 *          RESERVED_AGE): zero-order growth d(age)/dt = 1 in every water
 *          parcel, volume-weighted mixing everywhere water mixes. Internal
 *          unit is SECONDS (the config file speaks HOURS — ×3600 at
 *          parse). Per-source initial ages come from the waterage
 *          component's `[WATER_AGE_SOURCES]` (`model.age`, D-UT8): each
 *          QualitySolver loader contributes `age_volume = q · age_source`
 *          to its node, and the transport engine mixes it in exactly like
 *          a pollutant load.
 *
 *          A1a scope: the age species rides the EULERIAN_ARD mesh; the
 *          LEGACY CSTR age mirror is phase A1b (warned until then);
 *          watershed/LID/GW age states are A3/A4.
 *
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_WATER_AGE_DATA_HPP
#define OPENSWMM_ENGINE_DATA_WATER_AGE_DATA_HPP

#include <vector>

namespace openswmm {

/// Source pathways with configurable initial age ([WATER_AGE_SOURCES]).
/// Order is the storage index of WaterAgeConfigData::global_age.
enum class WaterAgeSource : int {
    RAINFALL        = 0,  ///< washoff runoff (and LID drains until A4)
    DWF             = 1,
    GW              = 2,
    RDII            = 3,
    EXTERNAL_INFLOW = 4,
    IFACE           = 5,
    INITIAL_STATE   = 6,  ///< water in the network at t = 0
    COUNT_          = 7
};

/**
 * @brief Parsed `model.age` state (waterage component, phase A1a).
 *
 * @details Node-scope overrides are resolved to node indices at apply (the
 *          rows reference nothing that another component declares, so no
 *          post-apply resolution pass is needed — unlike E5a's transport
 *          rows). TIMESERIES ages and SUBCATCH/EDGE_BC scopes are later
 *          phases and refuse with precise deferral errors.
 */
struct WaterAgeConfigData {
    bool configured = false;

    /// GLOBAL initial age per source, SECONDS (default 0 = fresh water).
    double global_age[static_cast<int>(WaterAgeSource::COUNT_)] = {};

    // NODE-scope overrides (DWF / EXTERNAL_INFLOW), parallel arrays.
    std::vector<int>    node_over_source;  ///< WaterAgeSource as int
    std::vector<int>    node_over_node;    ///< node index
    std::vector<double> node_over_age;     ///< seconds

    /// Age of `source` water entering `node` (seconds).
    double source_age(WaterAgeSource s, int node) const noexcept {
        for (std::size_t i = 0; i < node_over_source.size(); ++i)
            if (node_over_source[i] == static_cast<int>(s) &&
                node_over_node[i] == node)
                return node_over_age[i];
        return global_age[static_cast<int>(s)];
    }
};

/**
 * @brief Runtime age state shared by the engines (phase A1a).
 *
 * @details `node_age_vol_in` is a RATE (age·ft³/s), the age analogue of
 *          nodes.qual_mass_in — loaders add `q · age_source` and the
 *          engine integrates it over its substeps. `node_age`/`link_age`
 *          are the PUBLISHED mean ages (seconds) the API and gates read.
 */
/// A3: subarea index within a subcatchment. Matches the RunoffSolver's
/// three ponded depths (`depth_imperv0/1/perv`, `Runoff.hpp:86-88`) so the
/// age array can be indexed the same way the water is.
enum class SubArea : int {
    IMPERV0 = 0,  ///< impervious, no depression storage
    IMPERV1 = 1,  ///< impervious with depression storage
    PERV    = 2,
    COUNT_  = 3
};

struct WaterAgeState {
    std::vector<double> node_age_vol_in;  ///< [node], age·ft³/s
    std::vector<double> node_age;         ///< [node], seconds
    std::vector<double> link_age;         ///< [link], seconds

    /// A1b: the LEGACY mirror seeds INITIAL_STATE on its first step
    /// (the ARD engine seeds at its own init instead).
    bool legacy_seeded = false;

    // --- A3: watershed age -------------------------------------------
    /// [subcatch*3 + subarea] mean age of the water ponded on each subarea,
    /// SECONDS. Per-subarea rather than per-subcatchment (user decision,
    /// 2026-08-17): impervious water is systematically younger than
    /// pervious, and the three depths already exist separately.
    std::vector<double> subarea_age;

    /// [subcatch*3 + subarea] the previous step's stored volume (ft³) —
    /// the mixing denominator. Kept here rather than recomputed because
    /// the RunoffSolver overwrites its depths in place.
    std::vector<double> subarea_vol_prev;

    /// [subcatch] age of the water LEAVING as runoff, SECONDS — the
    /// volume-weighted mean of the contributing subareas. This is what the
    /// wet-weather loader delivers to the outlet node, and what run-on
    /// carries to a downstream subcatchment.
    std::vector<double> subcatch_runoff_age;

    /// [subcatch] age·ft³/s arriving as RUN-ON from upstream subcatchments,
    /// the watershed analogue of node_age_vol_in. Without this, run-on
    /// water arrived with no age at all — the flow path adds q_runon while
    /// the quality path never did.
    std::vector<double> subcatch_runon_age_vol_in;

    /// A4: [node] age·ft³/s arriving through LID underdrains, accumulated in
    /// A6b beside the drain volume and consumed by the wet-weather loader.
    /// Separate from `node_age_vol_in` because the drain volume has its own
    /// per-runoff-step lifecycle (`nodes.lid_drain_qual_vol`), and the age
    /// has to be zeroed and consumed on exactly that cadence or it would be
    /// counted against a volume from a different step.
    std::vector<double> node_lid_drain_age_vol_in;

    /// A4: [subcatch] age·ft³ (LID drain) and age·ft³ (outfall) waiting to be
    /// handed to `subcatch_runon_age_vol_in` when `assembleRunon` converts the
    /// matching volumes into run-on rates.
    ///
    /// A3 filled `subcatch_runon_age_vol_in` from the subcatchment cascade
    /// ALONE, then divided it by `runon_inflow` — a denominator that also
    /// carries LID drain water and outfall return flow. A numerator missing
    /// terms the denominator has does not merely lose precision: it produced
    /// a run-on age BELOW every source age in the model (3.834 h of arriving
    /// water under a 4 h rain, with nothing younger than 4 h anywhere). The
    /// flow path knew about all three contributors; the age path knew about
    /// one.
    std::vector<double> subcatch_lid_drain_age_cfs;
    std::vector<double> subcatch_outfall_age_vol;

    /// A2a: set by HotStartManager::apply when a V3 file restored ages —
    /// both engines then seed from node_age/link_age instead of
    /// INITIAL_STATE (the ARD engine consumes and clears it at init).
    bool hotstart_loaded = false;

    /// @note `n_subcatch` is deliberately NOT defaulted. A default would
    ///       let a two-argument call compile and silently empty the
    ///       watershed arrays at runtime — and every one of the five call
    ///       sites is guarded by a size mismatch, so the wipe would be
    ///       repaired by the next `routeSubcatchmentAge` and never show up
    ///       in any assertion. Requiring the argument moves the trap from
    ///       runtime to the compiler, which is the only observer that can
    ///       actually see it.
    void resize(int n_nodes, int n_links, int n_subcatch) {
        node_age_vol_in.assign(static_cast<std::size_t>(n_nodes), 0.0);
        node_age.assign(static_cast<std::size_t>(n_nodes), 0.0);
        link_age.assign(static_cast<std::size_t>(n_links), 0.0);
        node_lid_drain_age_vol_in.assign(static_cast<std::size_t>(n_nodes), 0.0);
        const auto ns3 = static_cast<std::size_t>(n_subcatch) *
                         static_cast<std::size_t>(SubArea::COUNT_);
        subarea_age.assign(ns3, 0.0);
        subarea_vol_prev.assign(ns3, 0.0);
        subcatch_runoff_age.assign(static_cast<std::size_t>(n_subcatch), 0.0);
        subcatch_runon_age_vol_in.assign(
            static_cast<std::size_t>(n_subcatch), 0.0);
        subcatch_lid_drain_age_cfs.assign(
            static_cast<std::size_t>(n_subcatch), 0.0);
        subcatch_outfall_age_vol.assign(
            static_cast<std::size_t>(n_subcatch), 0.0);
        legacy_seeded   = false;
        hotstart_loaded = false;
    }
    void clear() { *this = WaterAgeState{}; }
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_WATER_AGE_DATA_HPP
