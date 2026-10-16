// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace openswmm::surface {
struct SolarIntegral {
    double cosine = 0;      // average positive cosine on the inclined plane
    double horizontal = 0;  // average positive cosine on horizontal ground
    double hri = 0;         // radiation per horizontal area: cosine / cos(slope)
};
// Integrates the solar-vector dot product exactly between its roots, with
// declination fixed over the interval. Angles are degrees. Aspect is clockwise
// from NORTH, hour angle is zero at solar noon and positive in the afternoon.
// Both the astronomical horizon and the plane's own shadow are respected.
SolarIntegral solarIntegral(double latitude, double declination, double hour_start,
                            double duration_hours, double slope, double aspect);
} // namespace openswmm::surface
