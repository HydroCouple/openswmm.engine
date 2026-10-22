// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SolarGeometry.hpp
 * @brief     Solar position and geometry used by surface energy calculations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
namespace openswmm::surface {
struct SolarIntegral {
    double cosine = 0;     // average positive cosine on the inclined plane
    double horizontal = 0; // average positive cosine on horizontal ground
    double hri = 0;        // radiation per horizontal area: cosine / cos(slope)
};
// Integrates the solar-vector dot product exactly between its roots, with
// declination fixed over the interval. Angles are degrees. Aspect is clockwise
// from NORTH, hour angle is zero at solar noon and positive in the afternoon.
// Both the astronomical horizon and the plane's own shadow are respected.
/** @brief Integrate the positive solar dot product over one fixed-declination interval.
 * @param latitude Finite site latitude in degrees north, [-90,90].
 * @param declination Finite solar declination degrees, [-90,90], held fixed for the interval.
 * @param hour_start Finite starting solar hour angle in degrees, zero at noon and
 * positive afternoon; wraps by remainder modulo 360.
 * @param duration_hours Finite interval length in (0,24] hours.
 * @param slope Finite plane inclination degrees in [0,90); vertical planes are rejected.
 * @param aspect Finite plane aspect degrees clockwise from north, periodic modulo 360.
 * @return Mean positive plane cosine and horizontal cosine, each clamped to [0,1],
 * plus horizontal-area irradiance factor hri=cosine/cos(slope), which can exceed one.
 * Astronomical night and plane self-shadow are excluded by root-split integration.
 * @throws std::invalid_argument Nonfinite inputs or rejected angle/duration range.
 * Allocations may throw; no model state or clock is modified. */
SolarIntegral solarIntegral(double latitude, double declination, double hour_start,
                            double duration_hours, double slope, double aspect);
} // namespace openswmm::surface
