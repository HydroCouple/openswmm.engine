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
 * @file RadiativeExchange.cpp
 * @brief Phase H3 body — shortwave/longwave radiation.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "RadiativeExchange.hpp"

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

/// T⁴ in Kelvin, written once so the offset cannot drift between terms.
double kelvin4(double t_c) noexcept {
    const double k = t_c + kKelvinOffset;
    return k * k * k * k;
}

}  // namespace

double netShortwave(double incoming_wm2, double albedo,
                    double shade_factor) noexcept {
    // max(0, 1 - fs) mirrors the reference: an over-unity shade factor
    // shades completely rather than turning into negative insolation.
    return (1.0 - albedo) * incoming_wm2 *
           std::max(0.0, 1.0 - shade_factor);
}

double backLongwave(double t_water_c, double emiss_water) noexcept {
    return emiss_water * kStefanBoltzmann * kelvin4(t_water_c);
}

double atmosphericEmissivity(double e_a_kpa, double atm_emiss_coeff,
                             double cloud_factor) noexcept {
    // Brunt (1932). The reference passes PASCALS into the square root
    // (`sqrt(vaporPressureAir * 1000)`); vapour pressure here is kPa, so
    // the ×1000 is the unit conversion, not a fudge. Using kPa understates
    // this term by sqrt(1000).
    const double clear =
        atm_emiss_coeff + 0.0027 * std::sqrt(std::max(0.0, e_a_kpa) * 1000.0);

    // H6a (D-H6a-2). Guarded rather than multiplied unconditionally: the
    // clear-sky result must be the SAME OBJECT CODE H3 was gated on, not a
    // multiply by a value that happens to be 1.0. `cloudLongwaveFactor`
    // returns a literal 1.0 for C <= 0 for the same reason.
    if (cloud_factor == 1.0) return clear;
    return clear * cloud_factor;
}

double atmosphericLongwave(double t_air_c, double humidity_pct,
                           double atm_emiss_coeff, double lw_reflection,
                           double sky_view, double cloud_factor) noexcept {
    const double e_a =
        (humidity_pct / 100.0) * saturationVapourPressure(t_air_c);
    return kStefanBoltzmann * kelvin4(t_air_c) *
           atmosphericEmissivity(e_a, atm_emiss_coeff, cloud_factor) *
           (1.0 - lw_reflection) * sky_view;
}

double landCoverLongwave(double t_air_c, double emiss_landcover,
                         double sky_view) noexcept {
    // The reference carries a separate land-cover TEMPERATURE state; H3
    // uses air temperature, per plan §2.2. Recorded as a known difference:
    // a canopy warmer than the air (the usual daytime case) radiates more
    // than this predicts.
    return emiss_landcover * kStefanBoltzmann * (1.0 - sky_view) *
           kelvin4(t_air_c);
}

double netRadiativeFluxOut(double t_water_c, double t_air_c,
                           double humidity_pct, const RadiativeConfig& cfg,
                           double jin_wm2, double cloud_factor) noexcept {
    // Negative sentinel = "no resolved value, use the configured constant".
    // 0 is a legal Jin (night) and must not read as unset.
    const double jin = (jin_wm2 < 0.0) ? cfg.shortwave_wm2 : jin_wm2;

    const double jsn = netShortwave(jin, cfg.albedo, cfg.shade_factor);
    const double jbr = backLongwave(t_water_c, cfg.emiss_water);
    const double jan = atmosphericLongwave(t_air_c, humidity_pct,
                                           cfg.atm_emiss_coeff,
                                           cfg.lw_reflection, cfg.sky_view,
                                           cloud_factor);
    const double jlc = landCoverLongwave(t_air_c, cfg.emiss_landcover,
                                         cfg.sky_view);
    // Sign flip lives here and nowhere else: three terms warm the water,
    // one cools it, and this module's callers expect positive = OUT.
    return jbr - jsn - jan - jlc;
}

double radiativeFluxOut(const SimulationContext& ctx, double t_w) noexcept {
    if (!ctx.options.heat_transport || !ctx.heat_config.radiative_exchange)
        return 0.0;

    // H6a. `shortwave_now` is resolved once per step by `updateSolarForcing`
    // at every binding prologue; this call is const and only reads it.
    //
    // Under CONSTANT with no cloud these two arguments reproduce H3 exactly:
    // `updateSolarForcing` copies `cfg.shortwave_wm2` into `shortwave_now`
    // unchanged, and `cloudLongwaveFactor(0, k)` is a literal 1.0 which
    // `atmosphericEmissivity` then short-circuits.
    const auto& cc = ctx.heat_config.cloud;
    return netRadiativeFluxOut(
        t_w, airTempCelsius(ctx), ctx.climate_state.humidity,
        ctx.heat_config.radiative, ctx.heat_state.shortwave_now,
        cloudLongwaveFactor(ctx.heat_state.cloud_now, cc.lw_cloud_k));
}

}  // namespace openswmm::transport::heat
