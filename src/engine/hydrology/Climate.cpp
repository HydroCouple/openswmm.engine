/**
 * @file Climate.cpp
 * @brief Climate processing — numerically identical to legacy climate.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Climate.hpp"
#include "../core/UnitConversion.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace climate {

static constexpr double PI = 3.141592654;

double hargreaves(double latitude, int day_of_year, double t_avg, double t_range) {
    double a = 2.0 * PI / 365.0;

    // Convert to Celsius
    double ta = (t_avg - 32.0) * 5.0 / 9.0;
    double tr = t_range * 5.0 / 9.0;
    if (tr < 0.0) tr = 0.0;

    // Latent heat of vaporization
    double lambda = 2.50 - 0.002361 * ta;
    if (lambda <= 0.0) lambda = 0.01;

    // Relative earth-sun distance
    double dr = 1.0 + 0.033 * std::cos(a * day_of_year);

    // Solar declination
    double delta = 0.4093 * std::sin(a * (284.0 + day_of_year));

    // Latitude in radians
    double phi = latitude * 2.0 * PI / 360.0;

    // Sunset hour angle
    double x = -std::tan(phi) * std::tan(delta);
    x = std::max(-1.0, std::min(1.0, x));
    double omega = std::acos(x);

    // Extraterrestrial radiation (MJ/m2/day)
    double Ra = 37.6 * dr * (omega * std::sin(phi) * std::sin(delta)
                + std::cos(phi) * std::cos(delta) * std::sin(omega));
    if (Ra < 0.0) Ra = 0.0;

    // Hargreaves evapotranspiration (mm/day)
    double e = 0.0023 * Ra / lambda * std::sqrt(tr) * (ta + 17.8);
    if (e < 0.0) e = 0.0;

    return e;  // mm/day
}

void updateDailyClimate(ClimateState& state, int day_of_year, int month) {
    switch (state.evap_method) {
        case EvapMethod::CONSTANT:
            // evap_rate already set
            break;

        case EvapMethod::MONTHLY:
            // Convert from in/day to ft/sec using US EVAPRATE factor
            state.evap_rate = state.monthly_evap[month] / ucf::Ucf[ucf::EVAPRATE][0];
            break;

        case EvapMethod::TEMPERATURE: {
            double e_mm = hargreaves(state.latitude, day_of_year,
                                     state.temperature, state.temp_range);
            // mm/day → ft/sec using SI EVAPRATE factor
            state.evap_rate = e_mm / ucf::Ucf[ucf::EVAPRATE][1];
            break;
        }

        case EvapMethod::TIMESERIES:
        case EvapMethod::PAN:
            // Set externally from timeseries/file
            break;
    }

    // Apply monthly adjustment
    state.evap_rate *= state.adjust_evap[month];

    // Saturation vapor pressure (for snowmelt rain-on-snow)
    double ta = state.temperature;
    state.ea = 8.1175e6 * std::exp(-7701.544 / (ta + 405.0265));
}

// ============================================================================
// Batch distribute evap — VECTORISABLE
// ============================================================================

void batchDistributeEvap(double evap_rate, const double* ponded_depth,
                         double* evap_out, int n, double dt) {
    double inv_dt = (dt > 0.0) ? 1.0 / dt : 0.0;
    for (int i = 0; i < n; ++i) {
        double max_evap = ponded_depth[i] * inv_dt;
        evap_out[i] = std::min(evap_rate, max_evap);
    }
}

} // namespace climate
} // namespace openswmm
