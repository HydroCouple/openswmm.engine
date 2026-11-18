// SPDX-License-Identifier: Apache-2.0

/**
 * @file      UebForcing.cpp
 * @brief     Preparation of meteorological forcing for Utah Energy Balance snow models.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "Radiation.hpp"
#include "ClimateFile.hpp"
#include "surface/SolarGeometry.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/DateTime.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/PathResolver.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace openswmm::climate {
namespace {
/** @brief Integrate a piecewise-linear table and return its interval mean.
 * @param[in,out] ctx Borrowed table registry; interpolation mutates the selected cursor.
 * @param index Zero-based table index, validated here.
 * @param date Finite SWMM interval-start days.
 * @param seconds Positive finite seconds such that end is representably after date.
 * @param[out] output Mean in table value units, assigned only after all samples pass.
 * @param coverage True requires the full closed interval inside table bounds; false
 * allows table_lookup_cursor endpoint extension. Input x/y are aligned, finite and
 * strictly increasing by caller contract; empty/mismatched arrays are rejected.
 * @return Empty on success or missing/coverage/nonfinite diagnostic. Endpoint values
 * are checked, not every accumulated arithmetic result. Lookup cursors may change
 * on failure; output otherwise remains unchanged. */
std::string seriesAverage(SimulationContext& ctx, int index, double date, double seconds,
                          double& output, bool coverage = true) {
    if (index < 0 || index >= int(ctx.tables.count()))
        return "Radiation time series was not found.";
    auto& table = ctx.tables[index];
    const double end = date + seconds / 86400.;
    if (table.x.empty() || table.y.size() != table.x.size() ||
        (coverage && (date < table.x.front() || end > table.x.back())))
        return "Time series " + table.id + " does not cover the forcing interval.";
    double a = date, integral = 0;
    auto it = std::upper_bound(table.x.begin(), table.x.end(), a);
    while (a < end) {
        const double b = it == table.x.end() ? end : std::min(end, *it);
        const double va = table_lookup_cursor(table, a), vb = table_lookup_cursor(table, b);
        if (!std::isfinite(va) || !std::isfinite(vb))
            return "Nonfinite forcing in time series " + table.id + ".";
        integral += .5 * (va + vb) * (b - a);
        a = b;
        if (it != table.x.end())
            ++it;
    }
    output = integral / (end - date);
    return {};
}
/** @brief Obtain the day's temperature excursion for radiation transmissivity.
 * @param[in,out] ctx Initialized ordered meteorological/daily/table caches; a selected
 * temperature series may have its cursor advanced. Configured table index/shape
 * must be valid. Preference is hourly MET_FILE, daily cache, climate min/max, then
 * project-unit temperature-series samples plus interpolated day boundaries.
 * @param date Supported finite SWMM day; only floor(date) selects records.
 * @return Nonnegative Celsius/Kelvin difference, or -1 if no finite extrema are
 * available. A present higher-priority cache with no matching day does not fall
 * through to lower-priority sources. Does not advance the climate clock. */
double dailyRange(SimulationContext& ctx, double date) {
    double low = std::numeric_limits<double>::infinity(), high = -low;
    const double day = std::floor(date);
    const auto accumulate = [&](double t) {
        low = std::min(low, t);
        high = std::max(high, t);
    };
    if (!ctx.snowpacks.meteorology.empty()) {
        const auto& records = ctx.snowpacks.meteorology.records();
        auto it = std::lower_bound(records.begin(), records.end(), day,
                                   [](const HourlyMetRecord& r, double d) { return r.date < d; });
        for (; it != records.end() && it->date < day + 1; ++it)
            accumulate(it->values[0]);
    } else if (!ctx.snowpacks.daily_weather.empty()) {
        const auto& records = ctx.snowpacks.daily_weather;
        const auto i = std::lower_bound(records.begin(), records.end(), day,
                                        [](auto& row, double d) { return row.date < d; });
        if (i != records.end() && i->date == day)
            return std::max(0., (i->tmax - i->tmin) * 5 / 9);
    } else if (ctx.climate_state.has_minmax) {
        return std::max(0., (ctx.climate_state.tmax_daily - ctx.climate_state.tmin_daily) * 5 / 9);
    } else if (ctx.climate_state.temp_ts_index >= 0) {
        auto& table = ctx.tables[ctx.climate_state.temp_ts_index];
        // The stored series uses project temperature units, before the native
        // climate boundary converts it to Fahrenheit.
        const double scale =
            ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)) == 0 ? 5. / 9 : 1.;
        if (!table.x.empty()) {
            accumulate(table_lookup_cursor(table, day) * scale);
            accumulate(table_lookup_cursor(table, day + 1) * scale);
            const auto start = std::lower_bound(table.x.begin(), table.x.end(), day);
            for (auto it = start; it != table.x.end() && *it < day + 1; ++it)
                accumulate(table.y[it - table.x.begin()] * scale);
        }
    }
    return std::isfinite(low) && std::isfinite(high) ? high - low : -1;
}
/** @brief Integrate the native three-part sinusoidal daily temperature curve.
 * @param s Climate state with consistent sunrise/sunset and temperature-curve fields;
 * all temperature fields use the same units, normally Fahrenheit.
 * @param a Start local hour in [0,24].
 * @param b End local hour with a<b<=24.
 * @return Mean in the state temperature units, integrating each overlapping segment.
 * Near-zero curve-period denominators use legacy finite fallbacks; input validity,
 * interval length and finite output are not checked here. */
double meanDailyTemperature(const ClimateState& s, double a, double b) {
    const double pi = 3.14159265358979323846;
    double sum = 0;
    const auto integrate = [&](double lo, double hi, double base, double amplitude, double phase,
                               double frequency) {
        lo = std::max(a, lo);
        hi = std::min(b, hi);
        if (hi > lo)
            sum += base * (hi - lo) +
                   amplitude *
                       (std::cos(phase + frequency * lo) - std::cos(phase + frequency * hi)) /
                       frequency;
    };
    const double night = pi / (std::abs(s.dydif) > 1e-10 ? s.dydif : 1.);
    const double daylight = pi / (std::abs(s.dhrdy) > 1e-10 ? s.dhrdy : -1.);
    integrate(0, s.hrsr, s.tmin_daily, .5 * (s.prev_tmax - s.tmin_daily), night * s.hrsr, -night);
    integrate(s.hrsr, s.hrss, .5 * (s.tmin_daily + s.tmax_daily),
              .5 * (s.tmax_daily - s.tmin_daily), daylight * s.hrday, -daylight);
    integrate(s.hrss, 24, s.tmax_daily, -.5 * (s.tmax_daily - s.tmin_daily), -night * s.hrss,
              night);
    return sum / (b - a);
}
/** @brief Evaluate the pinned UEB Lowe saturation-vapour polynomial.
 * @param temperature Air/dewpoint temperature in degrees Celsius within the model's
 * physical calibration range; no range validation/clamp is applied.
 * @return Saturation pressure Pa; extreme/nonfinite inputs can overflow or propagate. */
double saturatedVapourPressure(double temperature) {
    // Lowe polynomial used by the pinned UEB reference, pressure in Pa.
    return 100 *
           (6.107799961 +
            temperature *
                (.4436518521 +
                 temperature *
                     (.01428945805 +
                      temperature *
                          (.0002650648471 +
                           temperature * (.000003031240396 +
                                          temperature * (.00000002034080948 +
                                                         temperature * .00000000006136820929))))));
}
}
std::string initializeRadiation(SimulationContext& ctx) {
    auto& cfg = ctx.snowpacks.config;
    if (const auto error = cfg.parameters.validate(); !error.empty())
        return error;
    auto& state = ctx.climate_state;
    if (cfg.site) {
        const auto& site = *cfg.site;
        const auto& solar = ctx.heat_config.solar;
        const bool conflict =
            (solar.has_latitude && solar.latitude_deg != site.latitude) ||
            (solar.has_longitude && solar.longitude_deg != site.longitude) ||
            (solar.has_timezone && solar.timezone_hours != site.timezone) ||
            (solar.has_elevation && solar.elevation_m != site.elevation) ||
            (ctx.options.snow_lat != 0 && ctx.options.snow_lat != site.latitude) ||
            (ctx.options.snow_elev != 0 && state.elev * .3048 != site.elevation);
        const std::string warning =
            "[TEMPERATURE] SITE overrides conflicting legacy snow/solar site values.";
        if (conflict &&
            std::find(ctx.warnings.begin(), ctx.warnings.end(), warning) == ctx.warnings.end())
            ctx.warnings.push_back(warning);
        state.latitude = site.latitude;
        state.elev = site.elevation / .3048;
        state.dtlong = site.timezone - site.longitude / 15;
        const double z = state.elev / 1000.;
        const double pressure_inhg = z <= 0 ? 29.9 : 29.9 - 1.02 * z + .0032 * std::pow(z, 2.4);
        state.gamma = .000359 * pressure_inhg;
    }
    if (!cfg.met_file.empty()) {
        cfg.met_file.absolute =
            io::resolveRelative(cfg.met_file.original, io::parentDir(ctx.inp_file_path));
        if (const auto error = ctx.snowpacks.meteorology.load(cfg.met_file.absolute);
            !error.empty())
            return error;
        if (ctx.options.end_date > ctx.options.start_date) {
            std::array<double, 5> values;
            const auto error = ctx.snowpacks.meteorology.average(
                ctx.options.start_date, (ctx.options.end_date - ctx.options.start_date) * 86400.,
                values);
            if (!error.empty())
                return error;
        }
    } else
        ctx.snowpacks.meteorology = {};
    ctx.snowpacks.daily_weather.clear();
    const bool needed = cfg.model == surface::SnowKind::Ueb || !cfg.packs.empty() ||
                        ctx.snowpacks.mesh_config.assigned() || cfg.shortwave.authored ||
                        cfg.longwave.authored;
    if (needed && ctx.snowpacks.meteorology.empty() && !ctx.options.temp_file.empty() &&
        (ctx.options.temp_source == 2 || ctx.options.wind_type == 1)) {
        ClimateFileReader reader;
        const auto path =
            io::resolveRelative(ctx.options.temp_file.original, io::parentDir(ctx.inp_file_path));
        if (!reader.open(path, ctx.options.temp_file_start,
                         ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)),
                         ctx.options.temp_units))
            return "Snow weather could not read the configured daily climate file.";
        reader.setDayOffset(ctx.options.temp_file_start > 0
                                ? std::floor(ctx.options.temp_file_start) -
                                      std::floor(ctx.options.start_date)
                                : 0);
        double low = 70, high = 70, wind = 0, previous = 70;
        for (double day = std::floor(ctx.options.start_date); day < ctx.options.end_date;
             day += 1) {
            DailyClimateRecord rec;
            if (reader.getRecord(day, rec)) {
                if (std::isfinite(rec.tmin))
                    low = rec.tmin;
                if (std::isfinite(rec.tmax))
                    high = rec.tmax;
                if (std::isfinite(rec.wind))
                    wind = rec.wind;
            }
            if (ctx.snowpacks.daily_weather.empty())
                previous = high;
            ctx.snowpacks.daily_weather.push_back({day, low, high, previous, wind});
            previous = high;
        }
    }
    for (const auto& source : {cfg.shortwave, cfg.longwave}) {
        using Mode = surface::RadiationSource::Mode;
        if (source.mode == Mode::File && ctx.snowpacks.meteorology.empty())
            return "Radiation FILE source requires MET_FILE.";
        if (source.mode != Mode::Timeseries || !ctx.snowpacks.meteorology.empty())
            continue;
        const int index = ctx.find_timeseries(source.series);
        if (index < 0)
            return "Radiation time series not found: " + source.series;
        const auto& table = ctx.tables[index];
        if (table.x.empty() || table.x.front() > ctx.options.start_date ||
            table.x.back() < ctx.options.end_date)
            return "Radiation time series does not cover the simulation: " + source.series;
        for (const double value : table.y)
            if (!std::isfinite(value) || value < 0)
                return "Radiation time series must contain finite nonnegative W/m²: " +
                       source.series;
    }
    return {};
}
std::string resolveRadiation(SimulationContext& ctx, double date, double seconds,
                             RadiationState& output) {
    if (!std::isfinite(date) || !std::isfinite(seconds) || seconds <= 0 || seconds > 86400)
        return "Invalid radiation interval.";
    const auto& cfg = ctx.snowpacks.config;
    const auto& climate = ctx.climate_state;
    const auto& solar = ctx.heat_config.solar;
    RadiationState next;
    next.date = date;
    next.seconds = seconds;
    next.temperature = (climate.temperature - 32) * 5 / 9;
    next.humidity = climate.humidity * .01;
    next.wind = climate.wind_speed * .44704;
    // Legacy dtlong is a solar-time correction in hours; when longitude is
    // absent use it directly, with no artificial equatorial/UTC relocation.
    double longitude = solar.has_longitude ? solar.longitude_deg : -climate.dtlong * 15;
    double timezone = solar.has_longitude ? solar.timezone_hours : 0;
    double elevation = solar.has_elevation ? solar.elevation_m : climate.elev * .3048;
    next.latitude = solar.has_latitude ? solar.latitude_deg : climate.latitude;
    if (cfg.site) {
        next.latitude = cfg.site->latitude;
        longitude = cfg.site->longitude;
        timezone = cfg.site->timezone;
        elevation = cfg.site->elevation;
    }
    next.pressure = pressureFromElevation(elevation) * 100;
    const auto pos = solarPosition(datetime::dayOfYear(date), (date - std::floor(date)) * 24,
                                   next.latitude, longitude, timezone);
    next.declination = pos.declination_deg;
    next.hour_angle = pos.hour_angle_deg;
    const auto integral = surface::solarIntegral(next.latitude, next.declination, next.hour_angle,
                                                 seconds / 3600, 0, 0);
    next.cosine = next.horizontal = integral.horizontal;
    int year, month, day;
    datetime::decodeDate(date, year, month, day);
    double range = dailyRange(ctx, date);
    const auto monthly = cfg.monthly_range[std::clamp(month - 1, 0, 11)];
    if (range < 0)
        range = monthly.value_or(0.);
    const double mean_range = monthly.value_or(range);
    const double b = .036 * std::exp(-.154 * mean_range);
    next.transmittance = cfg.bc_a * (1 - std::exp(-b * std::pow(range, cfg.bc_c)));
    next.cloud = 1 - next.transmittance / cfg.bc_a;
    const auto& cloud = ctx.heat_config.cloud;
    if (cloud.configured) {
        next.cloud = cloud.fraction;
        if (cloud.use_timeseries) {
            const auto error = seriesAverage(ctx, cloud.ts_index, date, seconds, next.cloud);
            if (!error.empty())
                return error;
        }
        if (!std::isfinite(next.cloud) || next.cloud < 0 || next.cloud > 1)
            return "Cloud forcing must be a fraction in [0,1].";
    }
    next.shortwave = next.transmittance * kSolarConstant * pos.eccentricity * integral.horizontal;
    if (!cfg.shortwave.authored && ctx.snowpacks.meteorology.empty() &&
        ctx.options.heat_transport && ctx.heat_config.radiative_exchange) {
        const auto& legacy = ctx.heat_config.radiative;
        if (legacy.sw_mode == ShortwaveMode::TIMESERIES) {
            const auto error =
                seriesAverage(ctx, legacy.sw_ts_index, date, seconds, next.shortwave);
            if (!error.empty())
                return error;
        } else if (legacy.sw_mode == ShortwaveMode::CONSTANT)
            next.shortwave = legacy.shortwave_wm2;
        else
            next.shortwave = birdClearSkyGHI(pos, next.pressure / 100, solar);
        next.shortwave *= cloudShortwaveFactor(cloud.configured ? next.cloud : 0, cloud.sw_atten_k,
                                               cloud.sw_atten_n);
        next.shortwave_source = RadiationOrigin::LegacyHeat;
    }
    for (int channel = 0; channel < 2; ++channel) {
        const auto& source = channel ? cfg.longwave : cfg.shortwave;
        if (source.mode == surface::RadiationSource::Mode::File &&
            ctx.snowpacks.meteorology.empty())
            return "Radiation FILE source requires MET_FILE.";
        if (source.mode != surface::RadiationSource::Mode::Timeseries ||
            !ctx.snowpacks.meteorology.empty())
            continue;
        double value;
        const auto error =
            seriesAverage(ctx, ctx.find_timeseries(source.series), date, seconds, value);
        if (!error.empty())
            return error;
        if (channel) {
            next.longwave = value;
            next.longwave_source = RadiationOrigin::Timeseries;
        } else {
            next.shortwave = value;
            next.shortwave_source = RadiationOrigin::Timeseries;
        }
    }
    if (!ctx.snowpacks.meteorology.empty()) {
        std::array<double, 5> values;
        const auto error = ctx.snowpacks.meteorology.average(date, seconds, values);
        if (!error.empty())
            return error;
        next.temperature = values[0];
        next.humidity = values[1] * .01;
        next.wind = values[2];
        next.shortwave = values[3];
        next.longwave = values[4];
        next.shortwave_source = next.longwave_source = RadiationOrigin::MetFile;
    }
    if (ctx.snowpacks.meteorology.empty()) {
        const int mon = datetime::monthOfYear(date) - 1;
        const bool si = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)) == 1;
        double temperature = climate.temperature_src, wind = climate.wind_speed_src;
        if (climate.temp_ts_index >= 0) {
            if (auto error =
                    seriesAverage(ctx, climate.temp_ts_index, date, seconds, temperature, false);
                !error.empty())
                return error;
            if (si)
                temperature = temperature * 1.8 + 32;
            temperature += climate.adjust_temp[mon];
        } else if (ctx.options.temp_source == 2 && !ctx.snowpacks.daily_weather.empty()) {
            double sum = 0, span = 0;
            const double end = date + seconds / 86400.;
            for (double a = date; a < end;) {
                const double day = std::floor(a), b = std::min(end, day + 1);
                const auto& records = ctx.snowpacks.daily_weather;
                const auto row = std::lower_bound(records.begin(), records.end(), day,
                                                  [](auto& r, double d) { return r.date < d; });
                if (row == records.end() || row->date != day)
                    return "Daily climate cache does not cover the snow interval.";
                auto daily = climate;
                const int month = datetime::monthOfYear(a) - 1;
                daily.tmin_daily = row->tmin + climate.adjust_temp[month];
                daily.tmax_daily = row->tmax + climate.adjust_temp[month];
                daily.prev_tmax = row->previous_max + climate.adjust_temp[month];
                updateTempTimes(daily, datetime::dayOfYear(a));
                sum += meanDailyTemperature(daily, (a - day) * 24, (b - day) * 24) * (b - a);
                span += b - a;
                a = b;
            }
            temperature = sum / span;
        } else if (ctx.forcing.climate_temperature_mode == ForcingMode::NONE)
            temperature = climate.temperature;
        if (ctx.options.wind_type == 0)
            wind = ctx.options.wind_speed[mon] / ucf::UCF(ucf::WINDSPEED, ctx.options);
        else if (!ctx.snowpacks.daily_weather.empty()) {
            const auto& rows = ctx.snowpacks.daily_weather;
            auto row = std::lower_bound(rows.begin(), rows.end(), std::floor(date),
                                        [](auto& r, double d) { return r.date < d; });
            if (row != rows.end())
                wind = row->wind;
        }
        next.temperature = (ctx.forcing.effective_temperature(temperature) - 32) * 5 / 9;
        next.wind = ctx.forcing.effective_wind(wind) * .44704;
        double humidity = ctx.options.humidity[mon];
        if (climate.humidity_ts_index >= 0)
            if (auto error =
                    seriesAverage(ctx, climate.humidity_ts_index, date, seconds, humidity, false);
                !error.empty())
                return error;
        if (ctx.options.humidity_var == 1) {
            const double dew = si ? humidity : (humidity - 32) * 5 / 9;
            humidity =
                100 * saturatedVapourPressure(dew) / saturatedVapourPressure(next.temperature);
        }
        next.humidity = std::clamp(humidity * .01, 0., 1.);
    } else {
        next.temperature =
            (ctx.forcing.effective_temperature(next.temperature * 1.8 + 32) - 32) * 5 / 9;
        next.wind = ctx.forcing.effective_wind(next.wind / .44704) * .44704;
    }
    if (next.longwave_source == RadiationOrigin::Computed) {
        const double kelvin = next.temperature + 273.15;
        const double vapour =
            std::max(0., saturatedVapourPressure(next.temperature) * next.humidity);
        const double emissivity = 1.08 * (1 - std::exp(-std::pow(vapour / 100, kelvin / 2016)));
        next.longwave =
            (next.cloud + (1 - next.cloud) * emissivity) * (2.0747e-7 / 3.6) * std::pow(kelvin, 4);
    }
    if (!std::isfinite(next.shortwave) || next.shortwave < 0 || !std::isfinite(next.longwave) ||
        next.longwave < 0 || !std::isfinite(next.temperature) || next.temperature <= -100 ||
        next.temperature > 70 || !std::isfinite(next.humidity) || next.humidity < 0 ||
        next.humidity > 1 || !std::isfinite(next.wind) || next.wind < 0)
        return "Invalid resolved UEB meteorological forcing.";
    output = next;
    return {};
}
std::string resolveUebForcing(SimulationContext& ctx, double date, double seconds, double slope,
                              double aspect, surface::ueb::Forcing& forcing, double& pressure) {
    RadiationState radiation;
    const auto error = resolveRadiation(ctx, date, seconds, radiation);
    if (!error.empty())
        return error;
    try {
        const auto geometry =
            surface::solarIntegral(radiation.latitude, radiation.declination, radiation.hour_angle,
                                   seconds / 3600, slope, aspect);
        auto next = forcing;
        next.seconds = seconds;
        next.temperature = radiation.temperature;
        next.humidity = radiation.humidity;
        next.wind = radiation.wind;
        next.longwave = radiation.longwave;
        next.cosine = geometry.cosine;
        if (ctx.snowpacks.config.division_temperature)
            next.snow_fraction =
                next.temperature <= (ctx.climate_state.snow_divt - 32) * 5 / 9 ? 1 : 0;
        // Preserve measured horizontal radiation at night as the reference
        // does. A zero astronomical denominator cannot define a slope ratio.
        next.shortwave = slope == 0 || geometry.horizontal <= 0
                             ? radiation.shortwave
                             : radiation.shortwave * geometry.hri / geometry.horizontal;
        forcing = next;
        pressure = radiation.pressure;
        ctx.climate_state.radiation = radiation;
        return {};
    } catch (const std::exception& e) {
        return e.what();
    }
}
} // namespace openswmm::climate
