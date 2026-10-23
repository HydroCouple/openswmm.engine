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
 * @file      RadiativeExchange.cpp
 * @brief     Phase H3 body — shortwave/longwave radiation.
 *
 * @details D-H5e moved the node and link BINDING out of this file into
 *          HeatFluxes.cpp. This file had its own hand-inlined explicit
 *          conversion and its own copy of the element traversal, which is
 *          how it came to carry H2's divergence through a different
 *          spelling and then to relax separately from it.
 *
 * @details Line-for-line provenance is `RHEComponent/src/element.cpp`:
 *          `computeNetSWSolarRadiation` (:106), `computeBackLWRadiation`
 *          (:113), `computeAtmosphericLWRadiation` (:119),
 *          `computeLandCoverLWRadiation` (:131).
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "RadiativeExchange.hpp"

#include "HeatOverrides.hpp"

#include <algorithm>
#include <cmath>

#include "SolarRadiation.hpp"
#include "SurfaceExchange.hpp"
#include "../../../core/SimulationContext.hpp"

namespace openswmm::transport::heat {

namespace {

// `kSqFtToSqM`, `kCuFtToCuM` and `airTempCelsius` were duplicated here and
// in SurfaceExchange.cpp. H5a exported them from SurfaceExchange.hpp (which
// this file already includes) because a third copy would have been needed
// for the watershed binding; the local copies are removed rather than left
// to shadow, since unqualified lookup would find both and neither the
// ambiguity nor a future divergence is something a test would catch.

/**
 * @brief T⁴ in Kelvin, written once so the offset cannot drift between terms.
 *
 * @details Compute fourth power of an absolute temperature converted from Celsius.
 * @param[in] t_c Qualified finite temperature degC (normally>=absolute zero).
 * @return (t_c+273.15)^4 K4; no physical range/overflow repair. Pure/allocation-free.
 */
double kelvin4(double t_c) noexcept {
    const double k = t_c + kKelvinOffset;
    return k * k * k * k;
}

} // namespace

/**
 * @brief Compute absorbed incident shortwave after albedo and shade.
 * @param[in] incoming_wm2 Qualified nonnegative incident solar W/m2.
 * @param[in] albedo Dimensionless reflected fraction, normally[0,1], not clamped.
 * @param[in] shade_factor Dimensionless shade fraction; values>1 fully shade via max(0,1-shade).
 * @return (1-albedo)*incoming*max(0,1-shade) W/m2 positive into water. Negative shade can
 * amplify forcing; no finite/range validation. Pure/no allocation/physical state or clock
 * change.
 */
double netShortwave(double incoming_wm2, double albedo, double shade_factor) noexcept {
    // max(0, 1 - fs) mirrors the reference: an over-unity shade factor
    // shades completely rather than turning into negative insolation.
    return (1.0 - albedo) * incoming_wm2 * std::max(0.0, 1.0 - shade_factor);
}

/**
 * @brief Compute emitted water longwave flux from the Stefan-Boltzmann law.
 * @param[in] t_water_c Qualified water degC, converted to K.
 * @param[in] emiss_water Dimensionless water emissivity, normally[0,1].
 * @return emissivity*sigma*(T+273.15)^4 W/m2 positive out of water. No finite/range checks or
 * canonical energy/clock mutation; pure/allocation-free.
 */
double backLongwave(double t_water_c, double emiss_water) noexcept {
    return emiss_water * kStefanBoltzmann * kelvin4(t_water_c);
}

/**
 * @brief Compute Brunt atmospheric emissivity with optional held cloud enhancement.
 * @param[in] e_a_kpa Air vapor pressure kPa, negative floored0 before sqrt and converted to Pa.
 * @param[in] atm_emiss_coeff Dimensionless clear-sky intercept.
 * @param[in] cloud_factor Dimensionless Bolz cloud multiplier, normally>=1; exactly1 bypasses
 * multiply.
 * @return (coefficient+.0027*sqrt(max(0,e_a)*1000))*cloud_factor, dimensionless, not clamped
 * to[0,1].
 * @note Qualified finite input required; pure/no allocation/physics or clock mutation.
 */
double atmosphericEmissivity(double e_a_kpa, double atm_emiss_coeff, double cloud_factor) noexcept {
    // Brunt (1932). The reference passes PASCALS into the square root
    // (`sqrt(vaporPressureAir * 1000)`); vapour pressure here is kPa, so
    // the ×1000 is the unit conversion, not a fudge. Using kPa understates
    // this term by sqrt(1000).
    const double clear = atm_emiss_coeff + 0.0027 * std::sqrt(std::max(0.0, e_a_kpa) * 1000.0);

    // H6a (D-H6a-2). Guarded rather than multiplied unconditionally: the
    // clear-sky result must be the SAME OBJECT CODE H3 was gated on, not a
    // multiply by a value that happens to be 1.0. `cloudLongwaveFactor`
    // returns a literal 1.0 for C <= 0 for the same reason.
    if (cloud_factor == 1.0)
        return clear;
    return clear * cloud_factor;
}

/**
 * @brief Compute absorbed visible-sky atmospheric longwave into water.
 * @param[in] t_air_c Qualified air degC.
 * @param[in] humidity_pct Relative humidity percent, normally[0,100].
 * @param[in] atm_emiss_coeff Dimensionless Brunt intercept.
 * @param[in] lw_reflection Longwave reflected fraction, normally[0,1].
 * @param[in] sky_view Visible-sky hemisphere fraction, normally[0,1].
 * @param[in] cloud_factor Held dimensionless cloud emissivity multiplier, default1 for clear
 * sky.
 * @return sigma*TairK^4*emissivity*(1-reflection)*sky_view W/m2 into water. No independent
 * weather/range/finite validation, owner/clock mutation or allocation.
 */
double atmosphericLongwave(double t_air_c, double humidity_pct, double atm_emiss_coeff,
                           double lw_reflection, double sky_view, double cloud_factor) noexcept {
    const double e_a = (humidity_pct / 100.0) * saturationVapourPressure(t_air_c);
    return kStefanBoltzmann * kelvin4(t_air_c) *
           atmosphericEmissivity(e_a, atm_emiss_coeff, cloud_factor) * (1.0 - lw_reflection) *
           sky_view;
}

/**
 * @brief Compute visible land-cover longwave contribution into water.
 * @param[in] t_air_c Radiating land-cover temperature degC; parameter historical name reflects
 * fallback air temperature, caller may supply configured distinct land-cover temperature.
 * @param[in] emiss_landcover Dimensionless emissivity, normally[0,1].
 * @param[in] sky_view Visible-sky fraction, so land cover uses complement1-sky_view.
 * @return emissivity*sigma*(1-sky_view)*Tk^4 W/m2 into water. Pure/no allocation/state change;
 * physical/finite input qualification belongs to caller.
 */
double landCoverLongwave(double t_air_c, double emiss_landcover, double sky_view) noexcept {
    // The reference carries a separate land-cover TEMPERATURE state; H3
    // uses air temperature, per plan §2.2. Recorded as a known difference:
    // a canopy warmer than the air (the usual daytime case) radiates more
    // than this predicts.
    return emiss_landcover * kStefanBoltzmann * (1.0 - sky_view) * kelvin4(t_air_c);
}

/**
 * @brief Combine water emission with absorbed shortwave, sky longwave and land-cover longwave.
 * @param[in] t_water_c Qualified water degC.
 * @param[in] t_air_c Held air degC.
 * @param[in] humidity_pct Held relative humidity percent.
 * @param[in] cfg Stable element radiative parameters; NaN landcover_temp selects air fallback,
 * otherwise configured degC is used directly. Fractions/emissivities must already qualify.
 * @param[in] jin_wm2 Held incident W/m2; negative sentinel uses cfg.shortwave_wm2, zero is valid
 * night.
 * @param[in] cloud_factor Held dimensionless atmospheric enhancement, used for estimated sky
 * flux.
 * @param[in] incoming_longwave Nonnegative measured/shared sky W/m2 before reflection/sky-view;
 * negative sentinel estimates sky from weather. Measured branch bypasses Brunt/cloud estimate.
 * @return Water back radiation minus absorbed shortwave/sky/landcover, signed W/m2 positive
 * cooling.
 * @note Keeps all absorbed solar in water in this evaluator; bed/penetration sharing belongs to
 * its physical owner. No finite-result/physical bound repair, allocation or state/calendar
 * mutation.
 */
double netRadiativeFluxOut(double t_water_c, double t_air_c, double humidity_pct,
                           const RadiativeConfig& cfg, double jin_wm2, double cloud_factor,
                           double incoming_longwave) noexcept {
    // Negative sentinel = "no resolved value, use the configured constant".
    // 0 is a legal Jin (night) and must not read as unset.
    const double jin = (jin_wm2 < 0.0) ? cfg.shortwave_wm2 : jin_wm2;

    const double jsn = netShortwave(jin, cfg.albedo, cfg.shade_factor);
    const double jbr = backLongwave(t_water_c, cfg.emiss_water);
    const double jan = incoming_longwave >= 0
                           ? incoming_longwave * (1 - cfg.lw_reflection) * cfg.sky_view
                           : atmosphericLongwave(t_air_c, humidity_pct, cfg.atm_emiss_coeff,
                                                 cfg.lw_reflection, cfg.sky_view, cloud_factor);
    // PE2: the land cover radiates at ITS OWN temperature when the deck
    // states one; NaN is the sentinel for "use air temperature", which is
    // what this term did unconditionally before PE and what it still does
    // for every model that does not set the key. (The check found the field
    // parsed, validated and serialized but never read — this line is where
    // it is read.)
    const double t_lc_c = std::isnan(cfg.landcover_temp) ? t_air_c : cfg.landcover_temp;
    const double jlc = landCoverLongwave(t_lc_c, cfg.emiss_landcover, cfg.sky_view);
    // Sign flip lives here and nowhere else: three terms warm the water,
    // one cools it, and this module's callers expect positive = OUT.
    return jbr - jsn - jan - jlc;
}

/**
 * @brief Evaluate element radiative flux using the configured global family enable.
 * @param[in] ctx Stable held heat/weather/override context, queried without refresh.
 * @param[in] elem Borrowed native element/global identity for local parameter/forcing selection.
 * @param[in] t_w Qualified water degC.
 * @return Explicit-enable overload result with ctx.heat_config.radiative_exchange; positive W/m2
 * out.
 * @note Heat transport gate, override/forcing precedence, finite-value assumptions and noexcept
 * provider restrictions of delegated overload apply; no physical/calendar mutation.
 */
double radiativeFluxOut(const SimulationContext& ctx, const HeatElement& elem,
                        double t_w) noexcept {
    return radiativeFluxOut(ctx, elem, t_w, ctx.heat_config.radiative_exchange);
}
/**
 * @brief Evaluate held element radiation with an explicit domain enable and no global toggle
 * mutation.
 * @param[in] ctx Stable heat/climate config, prepared solar/cloud/shared longwave and
 * override/provider bindings. Accessors must remain live and nonthrowing through this noexcept
 * query.
 * @param[in] elem Element/global identity selecting cached radiative block and local
 * weather/shortwave.
 * @param[in] t_w Qualified water degC.
 * @param[in] enabled Domain-selected radiation enable, independent of global family flag.
 * @return Zero unless heat_transport and enabled; otherwise signed outward W/m2 from
 * local/global parameters, element air/humidity/shortwave forcing, cloud enhancement and
 * optional shared longwave.
 * @note Element shortwave overrides global held Jin before shade/albedo; local air degF converts
 * to Celsius. radiativeFor only reads already-resolved dense overrides with global fallback. No
 * weather-step refresh, water/energy transfer or clock advance; no finite validation. Unexpected
 * throwing cache/provider/allocation under noexcept terminates; stable serial cache binding
 * required.
 */
double radiativeFluxOut(const SimulationContext& ctx, const HeatElement& elem, double t_w,
                        bool enabled) noexcept {
    if (!ctx.options.heat_transport || !enabled)
        return 0.0;

    // H6a. `shortwave_now` is resolved once per step by `updateSolarForcing`
    // at every binding prologue; this call is const and only reads it.
    //
    // Under CONSTANT with no cloud these two arguments reproduce H3 exactly:
    // `updateSolarForcing` copies `cfg.shortwave_wm2` into `shortwave_now`
    // unchanged, and `cloudLongwaveFactor(0, k)` is a literal 1.0 which
    // `atmosphericEmissivity` then short-circuits.
    const auto& cc = ctx.heat_config.cloud;
    // PE2: the per-element radiative block (shading, sky view, emissivities,
    // land-cover temperature). Returns the GLOBAL object when this model has
    // no overrides, so the call below is unchanged for every pre-PE deck.
    const RadiativeConfig& rc = radiativeFor(ctx, elem);
    // PE4: per-element climate, resolved at the flux call (see SurfaceExchange).
    const double t_air_c =
        (ctx.forcing.elementAirTempF(elem, ctx.climate_state.temperature) - 32.0) * 5.0 / 9.0;
    const double rh = ctx.forcing.elementHumidity(elem, ctx.climate_state.humidity);
    // PE4: a coupled shade model can push Jin per element outright; the deck
    // states one incident resource and SHADE_FACTOR modulates it. Precedence
    // is stated once, here: API element forcing > deck shade on global Jin.
    const double jin = ctx.forcing.elementShortwave(elem, ctx.heat_state.shortwave_now);
    return netRadiativeFluxOut(
        t_w, t_air_c, rh, rc, jin, cloudLongwaveFactor(ctx.heat_state.cloud_now, cc.lw_cloud_k),
        ctx.climate_state.radiation.shared_heat_forcing ? ctx.climate_state.radiation.longwave
                                                        : -1.0);
}

} // namespace openswmm::transport::heat
