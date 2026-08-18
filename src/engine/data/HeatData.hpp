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
 * @file HeatData.hpp
 * @brief Heat-transport data (heat plan §1, §3; phase H1).
 *
 * @details Temperature is the reserved species `__TEMPERATURE__` (registry
 *          kind RESERVED_TEMPERATURE), advected and mixed by whichever
 *          quality engine is active. H1 delivers TRANSPORT ONLY — the
 *          surface, radiative and sediment flux modules of plan §2 arrive
 *          with H2–H4, so nothing here adds or removes energy; temperature
 *          is carried and mixed exactly as a conservative tracer.
 *
 *          Per-source inlet temperatures come from the heat component's
 *          `[HEAT_SOURCES]` (`model.heat`, D-UT8), mirroring
 *          `[WATER_AGE_SOURCES]` row-for-row. Each QualitySolver loader
 *          contributes `q · T_source` to its node, the same seam the age
 *          channel uses (master plan §4.3 / D-UT10).
 *
 * @par Why this carries temperature-volume and not Joules
 *      Plan §3 describes the channel as enthalpy `ρw cp V T_source`. At H1
 *      there are no energy fluxes, so ρw and cp appear on BOTH sides of
 *      every mixing operation and cancel identically — carrying them would
 *      ship two constants that no H1 gate could observe being wrong
 *      (lesson 39: unobserved is not tested). `node_temp_vol_in` is
 *      therefore `q · T` (°C·ft³/s), the exact analogue of
 *      `node_age_vol_in`. **H2 introduces the constants together with the
 *      W/m² fluxes that make them load-bearing and observable**, and
 *      rescales this accumulator to J/s at that point — a rename in the
 *      same loader sites, which is the churn D-UT10 already accepted as
 *      cheap.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §1, §3, §6 H1
 * @see data/WaterAgeData.hpp — the shape this mirrors
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_HEAT_DATA_HPP
#define OPENSWMM_ENGINE_DATA_HEAT_DATA_HPP

#include <vector>

namespace openswmm {

/// Source pathways with a configurable inlet temperature ([HEAT_SOURCES]).
/// Order is the storage index of HeatConfigData::global_temp, and matches
/// WaterAgeSource one-for-one so the two tables read the same way.
enum class HeatSource : int {
    RAINFALL        = 0,  ///< washoff runoff (and LID drains until H5)
    DWF             = 1,
    GW              = 2,
    RDII            = 3,
    EXTERNAL_INFLOW = 4,
    IFACE           = 5,
    INITIAL_STATE   = 6,  ///< water in the network at t = 0
    COUNT_          = 7
};

/**
 * @brief Parsed `model.heat` state (heat component, phase H1).
 *
 * @details Mirrors WaterAgeConfigData, with one difference that matters:
 *          the default is **not** zero. An unset source temperature means
 *          "not configured", and 0 °C is a perfectly ordinary temperature,
 *          so a defaulted-to-zero table would silently chill every model.
 *          `kDefaultTemp` is the documented default inlet temperature and
 *          `configured_source[]` records which rows the user actually set,
 *          so a gate (and a user) can tell a deliberate 0 °C from a
 *          default.
 */
/**
 * @brief `[RADIATIVE_FLUXES]` parameters (heat plan §2.2, phase H3).
 *
 * @details Defaults are RHEComponent's (`rhemodel.cpp:43-47`) except where
 *          noted. GLOBAL scope only in H3; per-element ranges are RHE's
 *          `[RADIATIVE_FLUXES]` semantics and refuse until a later phase.
 */
struct RadiativeConfig {
    double shortwave_wm2   = 0.0;   ///< Incoming solar Jin, W/m² (0 = night)
    double albedo          = 0.0;   ///< Rs, reference default
    double shade_factor    = 0.0;   ///< fs, 0 = unshaded
    double sky_view        = 1.0;   ///< fsky, 1 = open sky
    double emiss_water     = 0.97;  ///< εw
    double emiss_landcover = 0.97;  ///< εlc
    double atm_emiss_coeff = 0.5;   ///< Brunt Aa
    double lw_reflection   = 0.03;  ///< RL
};

struct HeatConfigData {
    bool configured = false;

    /// `[HEAT_FLUXES] RADIATIVE_EXCHANGE ON` (plan §2.2, phase H3).
    /// Defaults OFF for the same reason SurfaceExchange does.
    bool radiative_exchange = false;

    /// Parameters for the module above.
    RadiativeConfig radiative;

    /**
     * @brief `[HEAT_FLUXES] SURFACE_EXCHANGE ON` — latent + sensible
     *        exchange at the free surface (plan §2.1, phase H2).
     *
     * @details Defaults OFF so H1's pure-transport behaviour is what a deck
     *          gets unless it asks for physics, and so every existing
     *          `.out` stays byte-identical. Each flux module of plan §2 is
     *          independently toggleable; radiative and sediment exchange
     *          arrive with H3/H4 and their keys refuse until then.
     */
    bool surface_exchange = false;

    /// Default inlet temperature when a source has no row (°C).
    static constexpr double kDefaultTemp = 20.0;

    /// GLOBAL inlet temperature per source, °C.
    double global_temp[static_cast<int>(HeatSource::COUNT_)] = {
        kDefaultTemp, kDefaultTemp, kDefaultTemp, kDefaultTemp,
        kDefaultTemp, kDefaultTemp, kDefaultTemp};

    /// Whether the user set this source explicitly (vs. taking the default).
    bool configured_source[static_cast<int>(HeatSource::COUNT_)] = {};

    // NODE-scope overrides (DWF / EXTERNAL_INFLOW), parallel arrays.
    std::vector<int>    node_over_source;  ///< HeatSource as int
    std::vector<int>    node_over_node;    ///< node index
    std::vector<double> node_over_temp;    ///< °C

    /// Temperature of `source` water entering `node` (°C).
    double source_temp(HeatSource s, int node) const noexcept {
        for (std::size_t i = 0; i < node_over_source.size(); ++i)
            if (node_over_source[i] == static_cast<int>(s) &&
                node_over_node[i] == node)
                return node_over_temp[i];
        return global_temp[static_cast<int>(s)];
    }
};

/**
 * @brief Runtime heat state shared by the engines (phase H1).
 *
 * @details `node_temp_vol_in` is a RATE (°C·ft³/s) — loaders add `q · T`
 *          and the engine integrates it over its substeps, exactly as
 *          `node_age_vol_in` does for age. `node_temp`/`link_temp` are the
 *          PUBLISHED temperatures (°C) the reports and gates read.
 */
struct HeatState {
    std::vector<double> node_temp_vol_in;  ///< [node], °C·ft³/s
    std::vector<double> node_temp;         ///< [node], °C
    std::vector<double> link_temp;         ///< [link], °C

    /// The LEGACY mirror seeds INITIAL_STATE on its first step.
    bool legacy_seeded = false;

    void resize(int n_nodes, int n_links, double initial_temp) {
        node_temp_vol_in.assign(static_cast<std::size_t>(n_nodes), 0.0);
        node_temp.assign(static_cast<std::size_t>(n_nodes), initial_temp);
        link_temp.assign(static_cast<std::size_t>(n_links), initial_temp);
        legacy_seeded = false;
    }
    void clear() { *this = HeatState{}; }
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_HEAT_DATA_HPP
