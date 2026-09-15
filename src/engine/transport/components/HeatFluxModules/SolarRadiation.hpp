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
 * @file SolarRadiation.hpp
 * @brief Phase H6a — where incoming shortwave `Jin` comes from
 *        (heat plan §2.5, D-H6a).
 *
 * @details H3 took `Jin` as a static constant. This module adds the two
 *          other spellings of plan §2.5: an interpolated timeseries, and a
 *          computed clear-sky value from solar position — plus the cloud
 *          parameterization that modulates BOTH shortwave and longwave.
 *
 *          Nothing here is a flux family. This module produces a number in
 *          W/m² and hands it to H3's existing `netShortwave`; it adds no
 *          term to `netFluxOut`, touches no sign convention, and introduces
 *          no element state. That is why H6a is separable from H6b.
 *
 * @par Solar position: NOAA/Spencer, NOT NREL SPA — and why (D-H6a-4, as
 *      amended 2026-08-30)
 *      The plan first specified NREL SPA for its published test vectors.
 *      Implementing it faithfully needs ~260 rows of periodic-term
 *      constants (truncated VSOP87 L/B/R plus the IAU 1980 nutation
 *      series). Transcribing those without the source document in reach
 *      means the TABLES and the TEST VECTOR would both be recalled — and a
 *      consistently misremembered pair gates green on wrong physics. That
 *      is the H3 Brunt-in-kPa failure (a plausible wrong number that only a
 *      true reference catches) at 260× the surface area, with the reference
 *      itself compromised.
 *
 *      So the position solver here is the Spencer (1971) / NOAA formulation:
 *      ~40 lines, no constant tables, **verifiable by inspection**, and
 *      independently cross-checkable against two things already in this
 *      engine — `Climate.cpp:178`'s declination and its sunrise/sunset
 *      hours. Stated accuracy is ~0.1° in declination and ~0.5 min in the
 *      equation of time, against SPA's ±0.0003°.
 *
 *      **That error is not the binding one.** A 0.1° zenith error moves
 *      clear-sky GHI by well under 0.1%; the cloud fraction multiplying it
 *      is a whole-number guess. Spending 400 lines of unverifiable
 *      constants to refine the small term under the large one is the wrong
 *      trade.
 *
 * @par The SPA swap point
 *      `solarPosition()` is the ONLY function that knows how a position is
 *      obtained. Everything downstream consumes `SolarPosition`. Landing
 *      SPA later is a new implementation of that one function plus a
 *      `SolarAlgorithm` selector — no caller changes. Do it with NREL's
 *      published C source open, so the tables can be DIFFED rather than
 *      recalled, and gate it against the report's worked example.
 *
 * @par Clear-sky: Bird & Hulstrom (1981)
 *      Broadband direct + diffuse on a horizontal surface. Five
 *      transmittance terms (Rayleigh, ozone, mixed gases, water vapour,
 *      aerosol) and a sky-ground multiple-reflection correction. Defaults
 *      are the paper's standard atmosphere.
 *
 * @warning The Bird coefficients below are transcribed from the published
 *          formulation. The validation handoff instructs the checking agent
 *          to re-derive them from the paper before trusting any gate that
 *          uses them — see `H6A_VALIDATION_HANDOFF_2026-08-30.md` §2.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §2.5, §6 H6a, §6.4
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_SOLAR_RADIATION_HPP
#define OPENSWMM_ENGINE_TRANSPORT_SOLAR_RADIATION_HPP

namespace openswmm {
struct SimulationContext;
struct SolarConfig;
struct CloudConfig;
}

namespace openswmm::transport::heat {

/// Solar constant, W/m² (Bird & Hulstrom 1981; WMO 1982).
inline constexpr double kSolarConstant = 1367.0;

/// Sea-level standard pressure, millibars — Bird's pressure reference.
inline constexpr double kStdPressureMb = 1013.25;

/**
 * @brief Sun geometry at one instant. The SPA swap point's return type.
 *
 * @details Angles in DEGREES. `cos_zenith` is carried alongside `zenith_deg`
 *          because every downstream consumer wants the cosine and rounding
 *          a degree value back through `cos` twice is how a night-time
 *          `cos_zenith` acquires a small positive value.
 */
struct SolarPosition {
    double declination_deg = 0.0;  ///< Solar declination, +N
    double eq_time_min     = 0.0;  ///< Equation of time, minutes
    double hour_angle_deg  = 0.0;  ///< Solar hour angle, 0 at solar noon
    double zenith_deg      = 90.0; ///< Solar zenith angle
    double cos_zenith      = 0.0;  ///< Clamped at 0 below the horizon
    double elevation_deg   = 0.0;  ///< 90 − zenith (may be negative)
    /// Earth–sun distance correction `E0 = (r0/r)²`, dimensionless.
    double eccentricity    = 1.0;
};

/**
 * @brief Sun position by the Spencer (1971) / NOAA formulation.
 *
 * @param day_of_year 1–366.
 * @param hour_local  Local clock hour, decimal (13.5 = 13:30).
 * @param lat_deg     Latitude, +N.
 * @param lon_deg     Longitude, +E.
 * @param tz_hours    Offset from UTC, +E (MST = −7).
 *
 * @note This is the function SPA replaces. Nothing else in this module
 *       knows how a position is obtained.
 */
SolarPosition solarPosition(int day_of_year, double hour_local, double lat_deg,
                            double lon_deg, double tz_hours) noexcept;

/// Kasten–Young relative optical air mass. Returns 0 below the horizon,
/// which is what makes every Bird transmittance collapse to a dark result
/// rather than to a domain error.
double airMass(double zenith_deg) noexcept;

/// Station pressure [mb] from elevation [m], standard atmosphere.
double pressureFromElevation(double elevation_m) noexcept;

/**
 * @brief Bird & Hulstrom clear-sky global horizontal irradiance [W/m²].
 *
 * @param pos           Sun geometry from `solarPosition`.
 * @param pressure_mb   Station pressure.
 * @param cfg           Aerosol / water / ozone / ground albedo.
 * @returns 0 when the sun is at or below the horizon.
 */
double birdClearSkyGHI(const SolarPosition& pos, double pressure_mb,
                       const SolarConfig& cfg) noexcept;

/**
 * @brief Cloud attenuation of shortwave, `1 − k·C^n` (Kasten–Czeplak).
 *
 * @details Clamped at 0: an over-unity `k·C^n` blocks completely rather
 *          than turning into negative insolation — the same guard
 *          `netShortwave` applies to an over-unity shade factor.
 */
double cloudShortwaveFactor(double cloud_fraction, double k, double n) noexcept;

/**
 * @brief Cloud enhancement of atmospheric emissivity, `1 + k_lw·C²` (Bolz).
 *
 * @warning This multiplies into H3's RHE-gated Brunt term. It returns a
 *          LITERAL 1.0 when `cloud_fraction <= 0`, rather than evaluating
 *          `1 + k·0²`, so the clear-sky path is bit-identical to H3 rather
 *          than merely equal to it. Plan §2.5 makes that a verify criterion.
 */
double cloudLongwaveFactor(double cloud_fraction, double k_lw) noexcept;

/**
 * @brief Resolve `Jin` and `C` for this step into `ctx.heat_state`.
 *
 * @details Call at the PROLOGUE of every binding that will go on to call
 *          `netFluxOut`. There are four (`applyHeatFluxes`, the ARD
 *          binding, the watershed binding, the LID binding) and they run on
 *          two different clocks, so a single call site upstream of all of
 *          them does not exist. The function is idempotent within a step
 *          and costs ~40 flops, which is why repeating it is cheaper than
 *          inventing a scheduler to avoid repeating it.
 *
 * @note No-op unless HEAT_TRANSPORT is on and RADIATIVE_EXCHANGE is
 *       enabled — under any other configuration `shortwave_now` is never
 *       read, and computing it would be the kind of unobserved work
 *       lesson 39 is about.
 */
void updateSolarForcing(SimulationContext& ctx) noexcept;

}  // namespace openswmm::transport::heat

#endif  // OPENSWMM_ENGINE_TRANSPORT_SOLAR_RADIATION_HPP
