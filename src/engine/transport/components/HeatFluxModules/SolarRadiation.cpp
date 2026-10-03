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
 * @file SolarRadiation.cpp
 * @brief Phase H6a — solar position, clear-sky irradiance, cloud.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SolarRadiation.hpp"

#include <algorithm>
#include <cmath>

#include "../../../core/DateTime.hpp"
#include "../../../core/SimulationContext.hpp"
#include "../../../data/TableData.hpp"

namespace openswmm::transport::heat {

namespace {

constexpr double kPi     = 3.14159265358979323846;
constexpr double kDegRad = kPi / 180.0;
constexpr double kRadDeg = 180.0 / kPi;

/// Feet → metres. `ClimateState::elev` is feet (legacy `Temp.elev`).
constexpr double kFtToM = 0.3048;

}  // namespace

// ===========================================================================
// Solar position — Spencer (1971) Fourier series, NOAA's published form.
//
// Every coefficient below is a term of a truncated Fourier fit and can be
// checked by inspection against the NOAA Solar Calculator documentation.
// That readability IS the reason this is here instead of SPA (header, D-H6a-4).
// ===========================================================================

SolarPosition solarPosition(int day_of_year, double hour_local, double lat_deg,
                            double lon_deg, double tz_hours) noexcept {
    SolarPosition p{};

    // Fractional year, radians. The (hour − 12)/24 term puts the fit at the
    // instant rather than at local midnight; omitting it costs ~0.2° of
    // declination at the equinoxes, where declination moves fastest.
    const double g =
        2.0 * kPi / 365.0 *
        (static_cast<double>(day_of_year) - 1.0 + (hour_local - 12.0) / 24.0);

    const double c1 = std::cos(g),      s1 = std::sin(g);
    const double c2 = std::cos(2.0*g),  s2 = std::sin(2.0*g);
    const double c3 = std::cos(3.0*g),  s3 = std::sin(3.0*g);

    // Equation of time, minutes.
    p.eq_time_min = 229.18 * (0.000075 + 0.001868 * c1 - 0.032077 * s1
                              - 0.014615 * c2 - 0.040849 * s2);

    // Declination, radians → degrees.
    const double decl = 0.006918 - 0.399912 * c1 + 0.070257 * s1
                        - 0.006758 * c2 + 0.000907 * s2
                        - 0.002697 * c3 + 0.001480 * s3;
    p.declination_deg = decl * kRadDeg;

    // Eccentricity / earth-sun distance correction (Spencer 1971), the E0
    // that scales the solar constant. Bird needs it; the position does not.
    p.eccentricity = 1.00011 + 0.034221 * c1 + 0.001280 * s1
                     + 0.000719 * c2 + 0.000077 * s2;

    // True solar time. `4 * longitude` converts degrees to minutes (the sun
    // moves 1° in 4 min); `60 * tz` removes the civil-clock offset. Sign
    // convention: longitude POSITIVE EAST, timezone POSITIVE EAST — so a
    // Utah deck is (-111.8, -7) and the two corrections nearly cancel, which
    // is the arithmetic to check when a result looks shifted by hours.
    const double time_offset_min =
        p.eq_time_min + 4.0 * lon_deg - 60.0 * tz_hours;
    const double tst_min = hour_local * 60.0 + time_offset_min;

    // Hour angle: −180° at solar midnight, 0 at solar noon.
    double ha = tst_min / 4.0 - 180.0;
    // Wrap to [−180, 180]. Without this a time_offset large enough to push
    // tst past a day boundary yields |ha| > 180 and a cos_zenith that is
    // right by accident for zenith but wrong for any azimuth use later.
    while (ha < -180.0) ha += 360.0;
    while (ha >  180.0) ha -= 360.0;
    p.hour_angle_deg = ha;

    const double lat = lat_deg * kDegRad;
    const double cz  = std::sin(lat) * std::sin(decl)
                     + std::cos(lat) * std::cos(decl) * std::cos(ha * kDegRad);
    const double cz_clamped = std::max(-1.0, std::min(1.0, cz));

    p.zenith_deg    = std::acos(cz_clamped) * kRadDeg;
    p.elevation_deg = 90.0 - p.zenith_deg;
    // Below the horizon is 0, not a negative cosine: every downstream term
    // treats this as "no sun", and a negative here would make Bird's air
    // mass and the GHI product silently change sign rather than vanish.
    p.cos_zenith    = std::max(0.0, cz_clamped);

    return p;
}

// ===========================================================================
// Bird & Hulstrom (1981) clear-sky model
// ===========================================================================

double airMass(double zenith_deg) noexcept {
    // Kasten & Young (1989). Undefined at and below the horizon; the caller
    // contract is that 0 means "no path", and every transmittance below is
    // written so that 0 produces darkness rather than a NaN.
    if (zenith_deg >= 90.0) return 0.0;
    const double z = zenith_deg;
    const double denom =
        std::cos(z * kDegRad) + 0.50572 * std::pow(96.07995 - z, -1.6364);
    if (!(denom > 0.0)) return 0.0;
    return 1.0 / denom;
}

double pressureFromElevation(double elevation_m) noexcept {
    // Standard atmosphere. Negative elevations (below sea level) are legal.
    return kStdPressureMb * std::pow(1.0 - 2.25577e-5 * elevation_m, 5.25588);
}

double birdClearSkyGHI(const SolarPosition& pos, double pressure_mb,
                       const SolarConfig& cfg) noexcept {
    const double cz = pos.cos_zenith;
    if (!(cz > 0.0)) return 0.0;

    const double am = airMass(pos.zenith_deg);
    if (!(am > 0.0)) return 0.0;

    // Pressure-corrected air mass.
    const double amp = am * (pressure_mb / kStdPressureMb);

    // --- Rayleigh scattering
    const double t_r = std::exp(-0.0903 * std::pow(amp, 0.84) *
                                (1.0 + amp - std::pow(amp, 1.01)));

    // --- Ozone absorption
    const double xo = cfg.ozone_cm * am;
    const double t_o =
        1.0 - 0.1611 * xo * std::pow(1.0 + 139.48 * xo, -0.3034)
        - 0.002715 * xo / (1.0 + 0.044 * xo + 0.0003 * xo * xo);

    // --- Uniformly mixed gases
    const double t_um = std::exp(-0.0127 * std::pow(amp, 0.26));

    // --- Water vapour
    const double xw = cfg.precip_water_cm * am;
    const double t_w =
        1.0 - 2.4959 * xw /
                  (std::pow(1.0 + 79.034 * xw, 0.6828) + 6.385 * xw);

    // --- Aerosol. The 0.2758/0.35 pair is Bird's broadband weighting of the
    //     two measured optical depths onto one effective depth.
    const double tau_a = 0.2758 * cfg.aod380 + 0.35 * cfg.aod500;
    const double t_a =
        std::exp(-std::pow(tau_a, 0.873) *
                 (1.0 + tau_a - std::pow(tau_a, 0.7088)) *
                 std::pow(am, 0.9108));

    // Aerosol absorptance / scattering split. Ba is the forward-scattering
    // ratio, a fixed 0.85 in Bird.
    constexpr double kBa = 0.85;
    const double t_aa = 1.0 - 0.1 * (1.0 - am + std::pow(am, 1.06)) * (1.0 - t_a);
    const double t_as = (t_aa > 0.0) ? (t_a / t_aa) : 0.0;

    const double i_ext = kSolarConstant * pos.eccentricity;

    // Direct normal, then its horizontal component.
    const double i_dn = 0.9662 * i_ext * t_r * t_o * t_um * t_w * t_a;
    const double i_d  = i_dn * cz;

    // Diffuse on the horizontal.
    const double denom = 1.0 - am + std::pow(am, 1.02);
    const double i_as =
        (denom > 0.0)
            ? (i_ext * cz * 0.79 * t_o * t_um * t_w * t_aa *
               (0.5 * (1.0 - t_r) + kBa * (1.0 - t_as)) / denom)
            : 0.0;

    // Sky-ground multiple reflection. `ground_albedo` is the LAND surface,
    // not the water's `RadiativeConfig::albedo` — see SolarConfig.
    const double rs = 0.0685 + (1.0 - kBa) * (1.0 - t_as);
    const double refl_denom = 1.0 - cfg.ground_albedo * rs;

    const double ghi =
        (refl_denom > 0.0) ? ((i_d + i_as) / refl_denom) : (i_d + i_as);

    return std::max(0.0, ghi);
}

// ===========================================================================
// Cloud
// ===========================================================================

double cloudShortwaveFactor(double cloud_fraction, double k,
                            double n) noexcept {
    if (!(cloud_fraction > 0.0)) return 1.0;
    const double c = std::min(1.0, cloud_fraction);
    // max(0, ...) for the same reason netShortwave guards its shade factor:
    // an over-unity product blocks completely, it does not invert the sign.
    return std::max(0.0, 1.0 - k * std::pow(c, n));
}

double cloudLongwaveFactor(double cloud_fraction, double k_lw) noexcept {
    // A LITERAL 1.0, not `1 + k·0²`. The clear-sky longwave path must be
    // bit-identical to H3's, not merely equal to it within rounding — plan
    // §2.5 makes that a verify criterion, and `1 + k*0*0` is not guaranteed
    // to be exactly 1.0 for every k under every rounding mode.
    if (!(cloud_fraction > 0.0)) return 1.0;
    const double c = std::min(1.0, cloud_fraction);
    return 1.0 + k_lw * c * c;
}

// ===========================================================================
// Per-step resolution
// ===========================================================================

void updateSolarForcing(SimulationContext& ctx) noexcept {
    auto& hs = ctx.heat_state;

    // Unobserved work is not free and not tested (lesson 39): under any
    // configuration that never reads these, do not compute them.
    //
    // `shortwave_now` goes back to the UNRESOLVED sentinel rather than to
    // 0: `radiativeFluxOut` returns early under this same condition, so
    // nothing reads it, and leaving a resolved-looking 0 behind would make
    // a later read silently lose the configured constant.
    if (!ctx.options.heat_transport || !ctx.heat_config.radiative_exchange) {
        hs.shortwave_now = -1.0;
        hs.cloud_now     = 0.0;
        return;
    }

    const auto& rc = ctx.heat_config.radiative;
    const auto& cc = ctx.heat_config.cloud;

    // ---- Cloud fraction first: the shortwave path needs it.
    double cloud = 0.0;
    if (cc.configured) {
        if (cc.use_timeseries) {
            if (cc.ts_index >= 0 &&
                cc.ts_index < static_cast<int>(ctx.tables.count())) {
                cloud = table_tseries_lookup_cursor(ctx.tables[cc.ts_index],
                                                    ctx.current_date);
            }
        } else {
            cloud = cc.fraction;
        }
    }
    // A cloud series is user data and may carry percent, a stray negative,
    // or an interpolation past its end. Clamp rather than refuse: unlike a
    // config fraction (refused at parse time, where the user can still fix
    // it), a series value arrives mid-run where there is no one to ask.
    hs.cloud_now = std::max(0.0, std::min(1.0, cloud));

    // ---- Incoming shortwave.
    double jin = 0.0;
    switch (rc.sw_mode) {
        case ShortwaveMode::CONSTANT:
            jin = rc.shortwave_wm2;
            break;

        case ShortwaveMode::TIMESERIES:
            if (rc.sw_ts_index >= 0 &&
                rc.sw_ts_index < static_cast<int>(ctx.tables.count())) {
                // tseries lookup (not table_lookup_cursor): returns 0 past
                // the end of the series rather than clamping to the last
                // value forever. A solar record that ran out should go dark,
                // not hold yesterday's noon indefinitely.
                jin = table_tseries_lookup_cursor(ctx.tables[rc.sw_ts_index],
                                                  ctx.current_date);
            }
            break;

        case ShortwaveMode::COMPUTED: {
            const auto& sc = ctx.heat_config.solar;
            // The parser refuses COMPUTED without both coordinates, so
            // reaching here without them is a programming error, not a deck
            // error. Fail dark rather than at latitude 0 — an equatorial
            // answer is the exact plausible-but-wrong result plan §2.5
            // trap 1 is about.
            if (!sc.has_latitude || !sc.has_longitude) {
                jin = 0.0;
                break;
            }

            const double doy = static_cast<double>(
                datetime::dayOfYear(ctx.current_date));
            // Fractional local hour from the OADate's time part.
            const double day_frac = ctx.current_date - std::floor(ctx.current_date);
            const double hour_local = day_frac * 24.0;

            const auto pos = solarPosition(static_cast<int>(doy), hour_local,
                                           sc.latitude_deg, sc.longitude_deg,
                                           sc.timezone_hours);

            // Elevation: the deck's explicit value if given, else the
            // climate state's `elev`, which is FEET.
            //
            // Keyed on the FLAG, not on the sign. A `< 0` test was the
            // first spelling and silently discarded every below-sea-level
            // site the parser deliberately admits.
            const double elev_m = sc.has_elevation
                                      ? sc.elevation_m
                                      : ctx.climate_state.elev * kFtToM;

            jin = birdClearSkyGHI(pos, pressureFromElevation(elev_m), sc);
            break;
        }
    }

    // Cloud attenuates every spelling, not only COMPUTED. A measured
    // pyranometer series already contains the clouds that were there, so
    // configuring BOTH double-counts — the parser warns; see HeatComponent.
    jin *= cloudShortwaveFactor(hs.cloud_now, cc.sw_atten_k, cc.sw_atten_n);

    hs.shortwave_now = std::max(0.0, jin);
}

}  // namespace openswmm::transport::heat
