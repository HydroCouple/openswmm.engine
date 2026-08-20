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
    RAINFALL        = 0,  ///< washoff runoff. H5b retired this row's
                          ///< second job: a LID drain now leaves at its
                          ///< own storage-layer temperature, not the rain's.
    DWF             = 1,
    GW              = 2,
    RDII            = 3,
    EXTERNAL_INFLOW = 4,
    IFACE           = 5,
    INITIAL_STATE   = 6,  ///< water in the network at t = 0
    COUNT_          = 7
};

/// H5a: which subarea of a subcatchment a ponded temperature belongs to.
/// Deliberately a SEPARATE enum from `openswmm::SubArea` (A3's), even though
/// the members coincide today: nothing guarantees the age and heat tracks
/// keep the same subarea decomposition, and a shared enum would make a
/// future divergence a silent index error rather than a compile error.
enum class HeatSubArea : int {
    IMPERV0 = 0,
    IMPERV1 = 1,
    PERV    = 2,
    COUNT_  = 3
};

/**
 * @brief What a dry or absent element reports (plan D-H5c, user 2026-08-19).
 *
 * @details A4 zeroes a LID layer holding no water — "no water, no age" — and
 *          that is right for age and wrong for temperature, because 0 °C is
 *          a real temperature. The three defensible answers serve different
 *          studies, so the deck picks one rather than the engine deciding.
 */
enum class DryTempPolicy : int {
    /// Freeze the last wet value; rewetting mixes against it. Invents no
    /// number and needs no forcing series, which is why it is the default.
    HOLD    = 0,
    /// Track air temperature — the physical answer for an exposed dry
    /// surface. Costs a dependency on the met forcing.
    AIR     = 1,
    /// Fall to `HeatConfigData::kDefaultTemp`. Reproducible, but it invents
    /// a number that then looks like data.
    DEFAULT = 2
};


/**
 * @brief Vertical conduction between LID layers (plan §6.1 D-H5b, H5b).
 *
 * @details **New physics with no in-engine precedent** — every
 *          "conductivity" elsewhere in `src/engine/` is HYDRAULIC. Values
 *          follow HydroCouple's `GWComponent`, a porous soil/gravel column
 *          being the closest analogue to a bioretention or permeable-
 *          pavement stack.
 *
 * @warning The values below are the ones GWComponent's CONSTRUCTOR sets
 *          (`gwmodel.cpp:51-52`), **not** the in-class initializers in
 *          `gwmodel.h:870-871`, which say 2650 kg/m³ and 880 J/kg/°C and are
 *          dead — the member-init list overrides both. The two differ by a
 *          factor of 2.3 in `ρ·cp`. The plan originally recorded the header
 *          pair; that was lesson 69 (a declaration is not a value) landing
 *          on the parameters the decision itself named.
 *          `HTSComponent` uses 1670/1807 for STREAMBED sediment — a third
 *          defensible pair, deliberately not taken here.
 */
struct ConductionConfig {
    double water_conductivity = 0.606;   ///< W/m/K  (gwmodel.h:885)
    double sed_conductivity   = 2.6;     ///< W/m/K  (gwmodel.h:886)
    double sed_density        = 1970.0;  ///< kg/m³  (gwmodel.cpp:51)
    double sed_specific_heat  = 2758.0;  ///< J/kg/K (gwmodel.cpp:52)
};

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

    /// `[HEAT_FLUXES] DRY_ELEMENT_TEMPERATURE HOLD|AIR|DEFAULT` (D-H5c).
    DryTempPolicy dry_temp_policy = DryTempPolicy::HOLD;

    /// `[HEAT_FLUXES] LAYER_CONDUCTION ON` — vertical conduction between
    /// LID layers (plan §2, phase H5b, D-H5b). Defaults OFF like every other
    /// flux module, so a deck gets pure transport unless it asks for physics.
    bool layer_conduction = false;

    /// Parameters for the module above.
    ConductionConfig conduction;

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

    // ---- H5a watershed rows. Sized by `resizeWatershed`, NOT by `resize`.
    //      Kept a separate call deliberately: A3 widened `WaterAgeState::
    //      resize` with a defaulted third parameter, four existing two-
    //      argument call sites then silently emptied the new arrays at
    //      runtime, and the validator had to remove the default to move the
    //      failure back to the compiler. A second function cannot be called
    //      short, so the hazard is unrepresentable rather than observed.

    /// [subcatch * kNSubArea + subarea], °C — ponded surface temperature.
    std::vector<double> subarea_temp;
    /// [subcatch * kNSubArea + subarea], ft³ — that subarea's stored water at
    /// the END of the previous step. The solver overwrites its depths in
    /// place, so the old volume is otherwise gone by the time this runs.
    std::vector<double> subarea_vol_prev;
    /// [subcatch], °C — temperature the subcatchment's runoff leaves at.
    std::vector<double> subcatch_runoff_temp;
    /// [subcatch], °C·ft³/s — per-step RATE accumulator for arriving run-on,
    /// following the `node_temp_vol_in` convention: donors accumulate
    /// `q · T`, and it is zeroed once consumed.
    std::vector<double> subcatch_runon_temp_vol_in;

    /// [subcatch], ft³/s — the run-on rate whose temperature is KNOWN, i.e.
    /// the flow actually represented in `subcatch_runon_temp_vol_in`.
    ///
    /// @details This is the pair that keeps A3's defect from recurring.
    ///          `subcatches.runon_inflow` has THREE contributors (the
    ///          subcatchment cascade, the LID underdrain return, and the
    ///          outfall return); A3 filled its age numerator from one of
    ///          them and divided by all three, so the arriving age was
    ///          dragged toward zero — measured at 3.834 h under a 4 h rain,
    ///          younger than anything entering the model.
    ///
    ///          Carrying the rate alongside the numerator makes the ratio a
    ///          true mean of whatever was counted, whether that is one
    ///          contributor or three. **H5a counts the cascade and the
    ///          outfall return; the LID underdrain arrives with H5b**,
    ///          because a drain's temperature is a per-layer quantity that
    ///          does not exist until the LID layer species row does. Until
    ///          then the omission biases nothing — it simply averages over
    ///          less water, which is the difference between an incomplete
    ///          answer and a wrong one.
    std::vector<double> subcatch_runon_temp_rate;

    /// [subcatch], °C·ft³ — outfall-return temperature-volume, accumulated on
    /// the ROUTING clock beside `subcatches.outfall_runon_vol` and consumed
    /// on the RUNOFF clock. A volume, not a rate, for exactly the reason its
    /// age counterpart is: the two clocks differ, so the producer cannot
    /// know the interval the consumer will divide by.
    std::vector<double> subcatch_outfall_temp_vol;

    static constexpr int kNSubArea = static_cast<int>(HeatSubArea::COUNT_);

    void resize(int n_nodes, int n_links, double initial_temp) {
        node_temp_vol_in.assign(static_cast<std::size_t>(n_nodes), 0.0);
        node_temp.assign(static_cast<std::size_t>(n_nodes), initial_temp);
        link_temp.assign(static_cast<std::size_t>(n_links), initial_temp);
        legacy_seeded = false;
    }

    void resizeWatershed(int n_subcatch, double initial_temp) {
        const auto n = static_cast<std::size_t>(n_subcatch);
        subarea_temp.assign(n * static_cast<std::size_t>(kNSubArea),
                            initial_temp);
        subarea_vol_prev.assign(n * static_cast<std::size_t>(kNSubArea), 0.0);
        subcatch_runoff_temp.assign(n, initial_temp);
        subcatch_runon_temp_vol_in.assign(n, 0.0);
        subcatch_runon_temp_rate.assign(n, 0.0);
        subcatch_outfall_temp_vol.assign(n, 0.0);
    }

    bool watershedSized(int n_subcatch) const noexcept {
        return subarea_temp.size() == static_cast<std::size_t>(n_subcatch) *
                                          static_cast<std::size_t>(kNSubArea);
    }

    void clear() { *this = HeatState{}; }
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_HEAT_DATA_HPP
