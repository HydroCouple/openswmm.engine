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

#include <limits>
#include <vector>

#include "BedZoneData.hpp"      // SedimentConfig — H6b's bed zone configuration
#include "HeatOverrideData.hpp" // HeatElement / HeatAttr / HeatScope — PE

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
 * @brief Where incoming shortwave `Jin` comes from (plan §2.5, phase H6a).
 *
 * @details The three spellings of `[RADIATIVE_FLUXES] SHORTWAVE` are
 *          **mutually exclusive by parse error, not by precedence**
 *          (D-H6a-3). A ladder would let a deck configuring two sources run
 *          plausibly while silently discarding one; the parser refuses
 *          instead, the way it already refuses an out-of-range fraction
 *          rather than clamping it.
 */
enum class ShortwaveMode : int {
    CONSTANT   = 0,  ///< `SHORTWAVE GLOBAL <W/m²>` — the H3 spelling.
    TIMESERIES = 1,  ///< `SHORTWAVE GLOBAL TIMESERIES <name>` — measured.
    COMPUTED   = 2   ///< `SHORTWAVE GLOBAL COMPUTED` — position + clear-sky.
};

/**
 * @brief `[SOLAR_RADIATION]` — site geometry and Bird atmosphere (H6a).
 *
 * @warning `latitude`/`longitude` have **no usable default** and the
 *          COMPUTED branch refuses without them. This is deliberate and is
 *          stricter than D-H5c's dry-element policy, which does default:
 *          a dry-element convention has a defensible default, an unstated
 *          latitude does not. `ClimateState::latitude` cannot stand in —
 *          it is the `[TEMPERATURE]` SNOWMELT field, defaults to 0, and is
 *          written only by decks carrying that line, so borrowing it would
 *          silently model equatorial noon (plan §2.5 trap 1).
 *
 * @note `ClimateState::dtlong` cannot stand in for `longitude` either: it
 *       is a solar-time correction in MINUTES carrying a sentinel (0 means
 *       "true solar time", which is not "longitude 0"). Plan §2.5 trap 2.
 */
struct SolarConfig {
    double latitude_deg   = 0.0;    ///< +N. REQUIRED under COMPUTED.
    double longitude_deg  = 0.0;    ///< +E. REQUIRED under COMPUTED.
    double timezone_hours = 0.0;    ///< Offset from UTC, +E (e.g. MST = -7).
    bool   has_latitude   = false;  ///< Set by the parser, checked at close.
    bool   has_longitude  = false;
    /// Not required — 0 (UTC) is a legal answer — but an OMITTED timezone
    /// shifts the whole diurnal curve by up to 12 h, which dwarfs every
    /// other error in this module. Tracked so the parser can WARN.
    bool   has_timezone   = false;

    /// Site elevation, metres, for the Bird pressure term. Absent means
    /// "take the climate state's `elev`", which is the usual case.
    ///
    /// A `< 0` sentinel was the first spelling and was wrong: the parser
    /// deliberately admits elevations below sea level (the Dead Sea, the
    /// Salton Sea), and every one of them would have been silently
    /// discarded in favour of the climate value. An explicit flag cannot
    /// collide with a legal value.
    double elevation_m    = 0.0;
    bool   has_elevation  = false;

    // ---- Bird & Hulstrom (1981) atmosphere. Defaults are the paper's
    //      standard atmosphere.
    double aod380        = 0.30;   ///< Aerosol optical depth at 380 nm
    double aod500        = 0.20;   ///< Aerosol optical depth at 500 nm
    double precip_water_cm = 1.42; ///< Precipitable water vapour, cm
    double ozone_cm        = 0.34; ///< Ozone column, cm (NTP)
    double ground_albedo   = 0.20; ///< Surface albedo for the sky-ground
                                   ///< multiple-reflection term. NOT
                                   ///< `RadiativeConfig::albedo`, which is
                                   ///< the WATER's reflectance — two
                                   ///< different surfaces, deliberately two
                                   ///< different fields.
};

/**
 * @brief `[CLOUD_COVER]` — one fraction driving two modules (H6a, D-H6a-2).
 *
 * @details Cloud lives here rather than in `RadiativeConfig` because the
 *          same `C` feeds BOTH shortwave attenuation and the longwave
 *          emissivity correction. Two modules reading a value from two
 *          copies is free to drift; D-H5e is the nearest precedent in kind.
 *
 * @warning `lw_cloud_k` reaches into the H3-validated Brunt path. The
 *          factor MUST reduce to exactly 1.0 at `C = 0` — see
 *          `cloudLongwaveFactor`, which returns a literal 1.0 on the
 *          `!configured` path rather than evaluating `1 + k·0²`.
 */
struct CloudConfig {
    bool   configured = false;  ///< No `[CLOUD_COVER]` section → clear sky.
    bool   use_timeseries = false;
    int    ts_index   = -1;     ///< Index into `ctx.tables`, -1 = none.
    double fraction   = 0.0;    ///< C ∈ [0,1], constant spelling.

    double sw_atten_k = 0.75;   ///< Kasten–Czeplak k
    double sw_atten_n = 3.4;    ///< Kasten–Czeplak n
    double lw_cloud_k = 0.17;   ///< Bolz k_lw
};

/**
 * @brief `[RADIATIVE_FLUXES]` parameters (heat plan §2.2, phase H3).
 *
 * @details Defaults are RHEComponent's (`rhemodel.cpp:43-47`) except where
 *          noted. GLOBAL scope only in H3; per-element ranges are RHE's
 *          `[RADIATIVE_FLUXES]` semantics and refuse until a later phase.
 *          H6a keeps GLOBAL scope deliberately — plan §7 records why (the
 *          per-step solar position is computed once, not per element).
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

    /**
     * @brief Land-cover radiating temperature, °C. NaN ⇒ use air temperature.
     *
     * @details PE2. `RadiativeExchange` computed the land-cover longwave
     *          term from AIR temperature and its own header recorded that as
     *          a departure from the reference, which carries a per-element
     *          `landCoverTemperature`. The consequence is a daytime
     *          understatement: a sunlit canopy or a concrete wall runs well
     *          above air temperature and radiates accordingly.
     *
     *          The NaN sentinel — not a plausible number — is what keeps
     *          every pre-PE model bit-identical and makes "the deck set
     *          this" distinguishable from "the deck did not", the
     *          `configured_source[]` distinction one struct over. A default
     *          of, say, 20.0 would be indistinguishable from a deliberate
     *          20 °C and would change every existing answer.
     */
    double landcover_temp = std::numeric_limits<double>::quiet_NaN();

    /// H6a. CONSTANT keeps `shortwave_wm2` load-bearing and is the default,
    /// so an H3-era deck is unaffected.
    ShortwaveMode sw_mode = ShortwaveMode::CONSTANT;
    /// Index into `ctx.tables` under TIMESERIES; -1 otherwise.
    int sw_ts_index = -1;
};

/**
 * @brief Dense per-element attribute storage (PE2, D-PE2).
 *
 * @details Sized ONLY when at least one override row targets the family.
 *          `radiativeFor`/`sedimentFor` return the global when the vector is
 *          empty, so a model without overrides allocates nothing and passes
 *          the same object it passed before PE — byte-identity is structural
 *          rather than tested.
 *
 * @warning Do not size these unconditionally "for uniformity". That single
 *          change would move every heat deck in the corpus, and it would do
 *          it by writing globals into a vector that then reads back as
 *          per-element configuration.
 */
struct HeatOverrideData {
    std::vector<HeatOverrideRow> rows;   ///< as parsed; the serializer's input

    std::vector<RadiativeConfig> rad_link;   ///< [link], empty ⇒ use global
    std::vector<RadiativeConfig> rad_node;   ///< [node], empty ⇒ use global
    std::vector<SedimentConfig>  sed_link;   ///< [link], empty ⇒ use global

    bool resolved = false;

    bool empty() const noexcept { return rows.empty(); }
    void clear() { *this = HeatOverrideData{}; }
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

    /// `[HEAT_FLUXES] SEDIMENT_EXCHANGE ON` — the bed / hyporheic transient
    /// storage zone (plan §2.3, phase H6b). Defaults OFF like every other
    /// flux module. **Unlike the others it is not a surface flux**: it adds a
    /// second state variable and is integrated as a coupled pair, which is
    /// why its physics lives in `BedExchange.hpp` rather than as another
    /// term in `netFluxOut`. See that header for why the earlier prediction
    /// that it would be one term was wrong.
    bool sediment_exchange = false;

    /// `[SEDIMENT_EXCHANGE]` — parameters for the module above. Deliberately
    /// a SEPARATE struct from `ConductionConfig`: that one describes a
    /// bioretention soil column (GWComponent, 1970/2758), this one describes
    /// streambed sediment (HTSComponent, 1670/1807). Sharing them would make
    /// one number stand for two materials.
    SedimentConfig sediment;

    /// PE2 — per-element overrides for `radiative` and `sediment` above.
    /// Empty on every model that does not use them.
    HeatOverrideData overrides;

    /// `[SOLAR_RADIATION]` — only consulted under `ShortwaveMode::COMPUTED`
    /// (plan §2.5, phase H6a).
    SolarConfig solar;

    /// `[CLOUD_COVER]` — consulted by BOTH the shortwave and longwave paths
    /// (plan §2.5, D-H6a-2).
    CloudConfig cloud;

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
    /// [node], °C·ft³/s arriving through LID underdrains, accumulated in the
    /// runoff step beside `nodes.lid_drain_qual_vol` and consumed by the
    /// wet-weather loader on that same per-runoff-step cadence — the twin of
    /// `WaterAgeState::node_lid_drain_age_vol_in`. Until this existed the
    /// drain-to-node loader handed the drain the RAINFALL temperature, so
    /// H5b's storage-layer temperature reached run-on receivers but never a
    /// node (LID fix round, 2026-08-30).
    std::vector<double> node_lid_drain_temp_vol_in;
    std::vector<double> node_temp;         ///< [node], °C
    std::vector<double> link_temp;         ///< [link], °C

    /// The LEGACY mirror seeds INITIAL_STATE on its first step.
    bool legacy_seeded = false;

    // ---- H6a per-step solar forcing (plan §2.5). RESOLVED ONCE PER STEP by
    //      `updateSolarForcing`, then read const by every flux call.
    //
    //      This is state, not config, which is why it lives here: `Jin`
    //      under TIMESERIES or COMPUTED changes every step, while
    //      `RadiativeConfig::shortwave_wm2` is what the deck wrote and must
    //      not be overwritten (a hot-started or re-opened model would
    //      otherwise resume from a stale interpolation rather than from its
    //      own configuration).
    //
    //      It is also what keeps the SPA cost argument honest: shortwave is
    //      GLOBAL scope, so the position is computed once per step and every
    //      element reads the same cached number. A per-element solve is
    //      plan §7 work and would not use this field.

    /// Incoming shortwave at the current step, W/m², cloud already applied.
    ///
    /// **NEGATIVE means "not yet resolved this run"**, and that is
    /// load-bearing, not decorative. `radiativeFluxOut` passes this
    /// straight into `netRadiativeFluxOut`'s `jin_wm2`, whose documented
    /// sentinel for "use the configured constant" is a negative value. A
    /// 0.0 default would make that sentinel unreachable from the
    /// production path, so any call landing before the step's
    /// `updateSolarForcing` would silently drop the shortwave term to zero
    /// instead of falling back on `RadiativeConfig::shortwave_wm2`.
    /// `updateSolarForcing` never writes a negative (it ends in
    /// `max(0.0, ...)`), so the sentinel cannot be confused with a
    /// resolved night-time 0.
    double shortwave_now = -1.0;
    /// Cloud fraction at the current step, C ∈ [0,1]. 0 = clear.
    double cloud_now = 0.0;

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
        node_lid_drain_temp_vol_in.assign(static_cast<std::size_t>(n_nodes), 0.0);
        node_temp.assign(static_cast<std::size_t>(n_nodes), initial_temp);
        link_temp.assign(static_cast<std::size_t>(n_links), initial_temp);
        legacy_seeded = false;
        // H6a. `clear()` resets these via whole-struct assignment, but
        // `resize()` is what runs at INITIALIZE — so a re-initialize on an
        // already-run context would otherwise leave the previous run's
        // forcing readable, and `swmm_heat_get_current_shortwave` documents
        // itself as unresolved before the first step.
        shortwave_now = -1.0;
        cloud_now     = 0.0;
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
