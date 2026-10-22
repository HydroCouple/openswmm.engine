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
 * @file      Radiation.hpp
 * @brief Solar geometry, atmospheric radiation and interval weather forcing.
 *
 * @details Solar position uses the Spencer/NOAA approximation, not NREL SPA.
 * Angles are degrees; longitude and timezone are positive east. The position
 * result is shared by Bird-Hulstrom clear-sky irradiance and the interval solar
 * geometry used for UEB forcing. Atmospheric and radiation outputs use SI units.
 *
 * Shortwave cloud attenuation follows 1-k*C^n; the independent longwave factor
 * follows 1+k_lw*C^2. Constant, time-series and computed heat radiation are
 * supported alongside the snow forcing resolver's meteorological file inputs.
 * These functions supply incoming forcing; net surface fluxes live in the heat
 * components. Serialization, configuration validation and runtime scheduling
 * remain caller responsibilities.
 *
 * @see SolarPosition, resolveRadiation(), updateSolarForcing()
 * @ingroup engine_transport
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_HYDROLOGY_RADIATION_HPP
#define OPENSWMM_ENGINE_HYDROLOGY_RADIATION_HPP
#include <string>
#include "surface/UebSnow.hpp"

namespace openswmm {
struct SimulationContext;
struct SolarConfig;
struct CloudConfig;
}

namespace openswmm::climate {

enum class RadiationOrigin { Computed, Timeseries, MetFile, LegacyHeat };
struct RadiationState {
    double shortwave = 0, longwave = 0; // W/m², horizontal
    double cosine = 0, horizontal = 0, transmittance = 0, cloud = 0;
    double temperature = 0, humidity = .5, wind = 0; // °C, fraction, m/s
    double pressure = 101325;
    double declination = 0, hour_angle = 0, latitude = 0;
    double date = 0, seconds = 0;
    RadiationOrigin shortwave_source = RadiationOrigin::Computed;
    RadiationOrigin longwave_source = RadiationOrigin::Computed;
    bool shared_heat_forcing = false;
};

/** @brief Resolve mean weather/radiation for one interval without advancing climate time.
 * @param[in,out] ctx Initialized compatible model with sorted, finite, aligned tables,
 * valid date/unit configuration and caches prepared by initializeRadiation(). Table
 * lookup cursors can advance, although the climate clock and output state do not.
 * @param date Finite absolute SWMM interval-start date in days within supported calendar range.
 * @param seconds Finite duration in (0,86400] seconds.
 * @param[out] output Assigned only on success: horizontal shortwave/longwave W/m2,
 * air temperature C, humidity fraction, wind m/s, pressure Pa, averaged positive
 * horizontal cosine, degree geometry and input-origin markers.
 * @return Empty on success, otherwise a configuration/coverage/value diagnostic.
 * MET_FILE overrides weather and both radiation channels; otherwise authored series
 * and legacy heat forcing can replace computed radiation. Weather prescriptions
 * apply after native/file conversion. Series use piecewise-linear interval means;
 * solar geometry fixes start-instant declination over the interval. Temperature,
 * humidity, wind and radiation are checked, but pressure/geometry are not all
 * individually validated here. Failure retains output but may change lookup cursors.
 * Exceptions from calendar/geometry/allocation helpers are not caught here. */
std::string resolveRadiation(SimulationContext& ctx, double date, double seconds,
                             RadiationState& output);
/** @brief Validate snow parameters and prepare site and weather input caches.
 * @param[in,out] ctx Compatible initialized model/options with resolved input path,
 * climate/heat/snow configuration and time-series tables. A snow SITE takes precedence
 * over legacy coordinates/elevation and can append one conflict warning. Resolves
 * MET_FILE relative to the input, loads/qualifies coverage, or builds a daily legacy
 * climate cache when needed. Daily gaps retain the preceding temperature/wind values
 * with initial defaults 70 F/zero wind. Does not advance the simulation clock.
 * @return Empty on accepted setup, otherwise diagnostic. Changes made before failure
 * (including site overrides, paths and weather caches) remain; not transactional.
 * Requires caller serialization with forcing resolution and table/cache mutation.
 * Archive/path/allocation exceptions can propagate beyond normal diagnostic returns. */
std::string initializeRadiation(SimulationContext& ctx);
/** @brief Merge resolved interval weather and slope-adjusted radiation into a UEB forcing.
 * @param[in,out] ctx Compatible initialized weather configuration; successful return
 * also stores horizontal radiation in climate_state.radiation. Lookup cursors may
 * advance even on failure; no simulation clock advancement occurs.
 * @param date Absolute SWMM start date in days, as for resolveRadiation().
 * @param seconds Finite interval seconds in (0,86400].
 * @param slope Plane inclination in degrees; follows solarIntegral() admissible range.
 * @param aspect Plane aspect in degrees clockwise from north.
 * @param[in,out] forcing Existing forcing copied on success, preserving precipitation
 * and unrelated fields. Replaces weather, duration, cosine, longwave and shortwave;
 * division-temperature mode also replaces snow_fraction with zero or one. Shortwave
 * uses horizontal-area slope ratio unless flat or the astronomical denominator is zero.
 * @param[out] pressure Station pressure Pa, assigned only on success.
 * @return Empty on success, otherwise resolver/standard geometry-exception text.
 * Outputs remain unchanged for normal failures before publication; exceptions from
 * resolveRadiation occur outside the local geometry catch and can propagate. */
std::string resolveUebForcing(SimulationContext& ctx, double date, double seconds, double slope,
                              double aspect, surface::ueb::Forcing& forcing, double& pressure);

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
    double declination_deg = 0.0; ///< Solar declination, +N
    double eq_time_min = 0.0;     ///< Equation of time, minutes
    double hour_angle_deg = 0.0;  ///< Solar hour angle, 0 at solar noon
    double zenith_deg = 90.0;     ///< Solar zenith angle
    double cos_zenith = 0.0;      ///< Clamped at 0 below the horizon
    double elevation_deg = 0.0;   ///< 90 − zenith (may be negative)
    /// Earth–sun distance correction `E0 = (r0/r)²`, dimensionless.
    double eccentricity = 1.0;
};

/** @brief Compute Spencer/NOAA sun geometry at a local civil-clock instant.
 * @param day_of_year Calendar ordinal in [1,366]; the fit uses a 365-day denominator.
 * @param hour_local Finite decimal local hour, normally [0,24).
 * @param lat_deg Latitude degrees north in [-90,90].
 * @param lon_deg Finite longitude degrees east, normally [-180,180].
 * @param tz_hours Finite civil offset hours east of UTC; no daylight-saving lookup.
 * @return Degree-valued declination/hour-angle/zenith/elevation, equation of time in
 * minutes, nonnegative zenith cosine and dimensionless distance correction. Hour
 * angle wraps to [-180,180]. Inputs are not validated; nonfinite hour/longitude can
 * make wrapping nonterminating. This approximation is not a high-precision SPA solver. */
SolarPosition solarPosition(int day_of_year, double hour_local, double lat_deg, double lon_deg,
                            double tz_hours) noexcept;

/** @brief Compute Kasten-Young relative optical air mass.
 * @param zenith_deg Finite solar zenith in degrees, normally [0,180].
 * @return Dimensionless path factor, or zero for zenith>=90 or a nonpositive/NaN
 * denominator. Does not normalize angles or validate negative zenith inputs. */
double airMass(double zenith_deg) noexcept;

/** @brief Estimate standard-atmosphere station pressure from elevation.
 * @param elevation_m Finite metres above sea level; negative elevations allowed.
 * Keep 1-2.25577e-5*elevation_m nonnegative for the power-law domain.
 * @return Pressure in millibars; no range clamp, finite check or error diagnostic. */
double pressureFromElevation(double elevation_m) noexcept;

/** @brief Estimate clear-sky global horizontal irradiance with Bird-Hulstrom terms.
 * @param pos Compatible solarPosition result with nonnegative cosine and valid
 * zenith/eccentricity. The cosine and zenith must describe the same instant.
 * @param pressure_mb Finite nonnegative station pressure in millibars.
 * @param cfg Physical aerosol optical depths, precipitable water/ozone centimetres
 * and land ground-albedo fraction. Caller validates finite model ranges.
 * @return Nonnegative irradiance W/m2, zero without positive cosine/air mass.
 * A nonpositive reflection denominator omits that correction. No diagnostics or
 * complete input-domain validation are supplied by this noexcept numerical helper. */
double birdClearSkyGHI(const SolarPosition& pos, double pressure_mb,
                       const SolarConfig& cfg) noexcept;

/** @brief Compute the cloud attenuation multiplier max(0,1-k*C^n).
 * @param cloud_fraction Cloud fraction; nonpositive or NaN returns literal one;
 * positive values above one are capped at one.
 * @param k Finite dimensionless attenuation coefficient, normally nonnegative.
 * @param n Finite exponent compatible with the positive cloud fraction.
 * @return Dimensionless factor; lower-bounded at zero but not capped at one when
 * negative k is supplied. No other coefficient validation occurs. */
double cloudShortwaveFactor(double cloud_fraction, double k, double n) noexcept;

/** @brief Compute the cloud atmospheric-emissivity multiplier 1+k_lw*C^2.
 * @param cloud_fraction Cloud fraction; nonpositive or NaN returns literal one;
 * positive values above one are capped at one.
 * @param k_lw Finite dimensionless enhancement coefficient, normally nonnegative.
 * @return Dimensionless multiplier without an output clamp. Literal clear-sky one
 * avoids evaluating coefficient arithmetic for a cloud-free input. */
double cloudLongwaveFactor(double cloud_fraction, double k_lw) noexcept;

/** @brief Update shared heat shortwave/cloud forcing with mesh radiation disabled.
 * @param[in,out] ctx Initialized heat/climate/snow state at current_date; serializes
 * table cursors and shared output state with other forcing readers/writers.
 * @param seconds Interval seconds forwarded to the snow resolver when authored
 * radiation/MET_FILE is present; default one. Otherwise legacy heat uses an instant.
 * Equivalent to the three-argument overload with mesh_enabled=false; same failure
 * and mutation behavior. Call before flux evaluation on each consuming clock. */
void updateSolarForcing(SimulationContext& ctx, double seconds = 1);
/** @brief Resolve shared incoming heat shortwave and cloud forcing at current_date.
 * @param[in,out] ctx Compatible initialized heat/climate/snow state and resolved tables.
 * Writes heat_state.shortwave_now/cloud_now and radiation.shared_heat_forcing; the
 * snow path stores a full resolved radiation record. Table cursors may advance.
 * @param seconds Positive interval seconds (at most one day) for the authored snow/
 * MET_FILE path; unused by ordinary constant/series/computed legacy heat lookup.
 * @param mesh_enabled Enables work for mesh heat even if legacy radiative exchange
 * is off; heat_transport must still be enabled.
 * @throws std::runtime_error Authored radiation/weather resolver diagnostic.
 * Disabled work writes shortwave=-1 and cloud=0, retaining the prior radiation record.
 * Legacy series lookup is instantaneous and can return zero beyond coverage; cloud
 * is clamped to [0,1], then attenuates all shortwave modes. Missing computed-site
 * coordinates yield zero. Repeated calls are not free of table-cursor mutations. */
void updateSolarForcing(SimulationContext& ctx, double seconds, bool mesh_enabled);

} // namespace openswmm::climate

#endif // OPENSWMM_ENGINE_HYDROLOGY_RADIATION_HPP
