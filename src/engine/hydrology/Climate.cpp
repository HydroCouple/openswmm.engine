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
#include "../core/Constants.hpp"
#include "../core/UnitConversion.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace climate {

using constants::PI;

// ============================================================================
// MovingAvg7 — 7-day circular buffer (matching legacy TMovAve)
// ============================================================================

void MovingAvg7::push(double t_avg, double t_range) {
    ta[front] = t_avg;
    tr[front] = t_range;
    front = (front + 1) % 7;
    if (count < 7) ++count;
}

double MovingAvg7::avg_temp() const {
    if (count == 0) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < count; ++i) sum += ta[i];
    return sum / count;
}

double MovingAvg7::avg_range() const {
    if (count == 0) return 0.0;
    double sum = 0.0;
    for (int i = 0; i < count; ++i) sum += tr[i];
    return sum / count;
}

// ============================================================================
// Hargreaves ET
// ============================================================================

double hargreaves(double latitude, int day_of_year, double t_avg, double t_range) {
    double a = 2.0 * PI / 365.0;

    // Convert to Celsius
    double ta = (t_avg - 32.0) * 5.0 / 9.0;
    double tr = t_range * 5.0 / 9.0;
    tr = std::max(tr, 0.0);

    // Latent heat of vaporization
    double lambda = 2.50 - 0.002361 * ta;
    lambda = std::max(lambda, 0.01);

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
    Ra = std::max(Ra, 0.0);

    // Hargreaves evapotranspiration (mm/day)
    double e = 0.0023 * Ra / lambda * std::sqrt(tr) * (ta + 17.8);
    e = std::max(e, 0.0);

    return e;  // mm/day
}

void updateDailyClimate(ClimateState& state, int day_of_year, int month) {
    // NOTE: Temperature source (timeseries/file) sets state.temperature
    // before this function is called. The monthly adjustment is applied
    // here and reflected in evap/gamma/ea calculations below.
    // When no source is active, temperature keeps its previous value.

    switch (state.evap_method) {
        case EvapMethod::CONSTANT:
            // evap_rate already set
            break;

        case EvapMethod::MONTHLY:
            // Convert from in/day to ft/sec using US EVAPRATE factor
            state.evap_rate = state.monthly_evap[month] / ucf::Ucf[ucf::EVAPRATE][0];
            break;

        case EvapMethod::TEMPERATURE: {
            // Push today's values into 7-day moving average
            state.temp_ma.push(state.temperature, state.temp_range);
            // Use smoothed values for Hargreaves (matching legacy Tma)
            double e_mm = hargreaves(state.latitude, day_of_year,
                                     state.temp_ma.avg_temp(),
                                     state.temp_ma.avg_range());
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

    // Psychrometric constant — matching legacy climate.c:
    //   z = Temp.elev / 1000 (elevation in thousands of ft)
    //   pa = 29.9 - 1.02*z + 0.0032*z^2.4  (atmospheric pressure, in-Hg)
    //   gamma = 0.000359 * pa
    // If elevation <= 0, default to sea-level pressure of 29.9.
    {
        double z = state.elev / 1000.0;
        double pa = (z <= 0.0) ? 29.9 : (29.9 - 1.02 * z + 0.0032 * std::pow(z, 2.4));
        state.gamma = 0.000359 * pa;
    }
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

// ============================================================================
// Sub-daily temperature interpolation — Gap #9
// Matches legacy updateTempTimes() + setTemp() in climate.c
// ============================================================================

void updateTempTimes(ClimateState& state, int doy) {
    // Solar declination (radians) — matches legacy climate.c
    double decl = 0.40928 * std::cos(0.017202 * (172.0 - doy));

    // Hour-angle argument: -tan(decl) * tan(latitude)
    double lat_rad  = state.latitude * PI / 180.0;
    double tan_lat  = std::tan(lat_rad);
    double arg      = -std::tan(decl) * tan_lat;

    double hrsr, hrss;
    if (arg >= 1.0) {
        // Polar night — sun never rises; use noon for both
        hrsr = 12.0;
        hrss = 12.0;
    } else if (arg <= -1.0) {
        // Midnight sun — sun never sets
        hrsr = 0.0;
        hrss = 24.0;
    } else {
        double hrang = 3.8197 * std::acos(arg);  // half-day length in hours
        hrsr = 12.0 - hrang + state.dtlong;
        hrss = 12.0 + hrang + state.dtlong - 3.0; // max temp ~3 h before actual sunset
    }

    state.hrsr  = hrsr;
    state.hrss  = hrss;
    state.hrday = (hrsr + hrss) / 2.0;
    state.dhrdy = hrsr - hrss;                     // negative (~-9 to -12)
    state.dydif = 24.0 + hrsr - hrss;              // sunset-to-next-sunrise span
}

double getSubdailyTemp(const ClimateState& state, double hour) {
    double tmin  = state.tmin_daily;
    double tmax  = state.tmax_daily;
    double tave  = (tmin + tmax) / 2.0;
    double trng  = (tmax - tmin) / 2.0;
    // Trng1: overnight range using previous day's max → current min
    double trng1 = state.prev_tmax - tmin;

    double hrsr  = state.hrsr;
    double hrss  = state.hrss;
    double hrday = state.hrday;
    double dhrdy = state.dhrdy;  // negative
    double dydif = state.dydif;

    double ta;
    if (hour < hrsr) {
        // Zone 1: before sunrise — sinusoidal rise toward tmin
        double denom = (std::fabs(dydif) > 1e-10) ? dydif : 1.0;
        ta = tmin + (trng1 / 2.0) * std::sin(PI / denom * (hrsr - hour));
    } else if (hour <= hrss) {
        // Zone 2: sunrise to sunset-3 — sinusoid peaking at tmax (at hrss)
        double denom = (std::fabs(dhrdy) > 1e-10) ? dhrdy : -1.0;
        ta = tave + trng * std::sin(PI / denom * (hrday - hour));
    } else {
        // Zone 3: after sunset-3 — sinusoidal decay from tmax toward next sunrise
        double denom = (std::fabs(dydif) > 1e-10) ? dydif : 1.0;
        ta = tmax - trng * std::sin(PI / denom * (hour - hrss));
    }
    return ta;
}

} // namespace climate
} // namespace openswmm
