/**
 * @file Climate.hpp
 * @brief Climate processing — evaporation, temperature, wind.
 *
 * @details Daily climate values are computed once and broadcast to all
 *          subcatchments — no per-subcatchment branching needed.
 *          The Hargreaves ET formula is the most compute-intensive path.
 *
 *          Vectorization: climate state is scalar-per-day (broadcast pattern).
 *          The per-subcatchment evap distribution is a batch multiply.
 *
 * @note Legacy reference: src/legacy/engine/climate.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_CLIMATE_HPP
#define OPENSWMM_CLIMATE_HPP

namespace openswmm {

struct SimulationContext;

namespace climate {

// ============================================================================
// Constants
// ============================================================================

constexpr double MM_PER_INCH = 25.40;

// ============================================================================
// Evaporation method
// ============================================================================

enum class EvapMethod : int {
    CONSTANT    = 0,
    MONTHLY     = 1,
    TIMESERIES  = 2,
    TEMPERATURE = 3,   ///< Hargreaves method
    PAN         = 4
};

// ============================================================================
// 7-day temperature moving average (for Hargreaves ET)
// ============================================================================

struct MovingAvg7 {
    double ta[7]  = {};   ///< Daily average temps (deg F)
    double tr[7]  = {};   ///< Daily temp ranges (deg F)
    int    front  = 0;    ///< Circular buffer write index
    int    count  = 0;    ///< Number of values stored (max 7)

    /// Push a new day's values into the buffer.
    void push(double t_avg, double t_range);

    /// Current moving average of temperature.
    double avg_temp() const;

    /// Current moving average of temperature range.
    double avg_range() const;
};

// ============================================================================
// Daily climate state (scalar — broadcast to all subcatchments)
// ============================================================================

struct ClimateState {
    double temperature  = 70.0;  ///< Air temperature (deg F)
    double temp_range   = 0.0;   ///< Daily temperature range (deg F)
    double evap_rate    = 0.0;   ///< Evaporation rate (ft/sec)
    double wind_speed   = 0.0;   ///< Wind speed (mph)
    double humidity     = 50.0;  ///< Relative humidity (%)

    // Derived values
    double gamma        = 0.0;   ///< Psychrometric constant
    double ea           = 0.0;   ///< Saturation vapor pressure

    // Hargreaves parameters
    double latitude     = 0.0;   ///< Latitude (degrees)

    // Site elevation for psychrometric constant (matching legacy Temp.elev)
    double elev         = 0.0;   ///< Site elevation above sea level (ft)

    // Monthly evaporation table (for MONTHLY method)
    double monthly_evap[12] = {};

    // Monthly adjustment factors
    double adjust_evap[12]   = {1,1,1,1,1,1,1,1,1,1,1,1};
    double adjust_temp[12]   = {0,0,0,0,0,0,0,0,0,0,0,0};
    double adjust_rain[12]   = {1,1,1,1,1,1,1,1,1,1,1,1};
    double adjust_hydcon[12] = {1,1,1,1,1,1,1,1,1,1,1,1}; ///< Infiltration conductivity multipliers

    EvapMethod evap_method = EvapMethod::CONSTANT;

    // Monthly adjustment for infiltration capacity (scales f0/fmin/Ks)
    // @see Legacy: InfilFactor (set from hydcon adjustment pattern)
    double infil_factor = 1.0;

    // 7-day moving average for Hargreaves ET (matching legacy TMovAve Tma)
    MovingAvg7 temp_ma;

    // Recovery factor for infiltration models
    // @see Legacy: Evap.recoveryFactor (set from recovery adjustment pattern)
    double recovery_factor = 1.0;

    // Resolved timeseries/pattern indices (set at init, -1 = none)
    int temp_ts_index     = -1;   ///< Temperature timeseries table index
    int evap_ts_index     = -1;   ///< Evaporation timeseries table index
    int recovery_pat_index = -1;  ///< Recovery pattern index in ctx.patterns

    // Sub-daily sinusoidal temperature interpolation (Gap #9)
    // Valid only when temperature comes from a daily min/max source.
    // @see Legacy: setTemp() / updateTempTimes() in climate.c
    double tmin_daily   = 0.0;  ///< Current day's minimum temperature (deg F)
    double tmax_daily   = 0.0;  ///< Current day's maximum temperature (deg F)
    double prev_tmax    = 0.0;  ///< Previous day's maximum temperature (deg F)
    double hrsr         = 6.0;  ///< Sunrise hour (time of min temp), 0-24
    double hrss         = 15.0; ///< Sunset-3 hour (time of max temp), 0-24
    double hrday        = 10.5; ///< Mid-hour between hrsr and hrss
    double dhrdy        = -9.0; ///< hrsr - hrss (negative)
    double dydif        = 15.0; ///< 24 + hrsr - hrss (hours from max to next min)
    double dtlong       = 0.0;  ///< Longitude correction (hrs); 0 = solar time
    bool   has_minmax   = false;///< True when tmin_daily/tmax_daily are valid
    int    last_temp_doy = -1;  ///< Day-of-year when hrsr/hrss were last computed
};

// ============================================================================
// Functions
// ============================================================================

/**
 * @brief Compute Hargreaves evapotranspiration.
 *
 * @param latitude  Latitude (degrees).
 * @param day_of_year  Day of year (1-365).
 * @param t_avg     Average temperature (deg F).
 * @param t_range   Temperature range (deg F).
 * @returns Evaporation rate (in/day for US, mm/day for SI).
 */
double hargreaves(double latitude, int day_of_year, double t_avg, double t_range);

/**
 * @brief Update daily climate state.
 *
 * @param state      [in/out] Climate state.
 * @param day_of_year Day of year.
 * @param month      Month (0-11).
 */
void updateDailyClimate(ClimateState& state, int day_of_year, int month);

/**
 * @brief Update sunrise/sunset parameters for sub-daily temperature interpolation.
 *
 * @details Matches legacy updateTempTimes() in climate.c.
 *          Computes hrsr, hrss, hrday, dhrdy, dydif from latitude and day-of-year.
 *          Must be called once per day before calling getSubdailyTemp().
 *
 * @param state      [in/out] Climate state (uses latitude, dtlong; writes hrsr etc.)
 * @param doy        Day of year (1-365).
 */
void updateTempTimes(ClimateState& state, int doy);

/**
 * @brief Compute sub-daily temperature using a three-zone sinusoidal model.
 *
 * @details Matches legacy setTemp() in climate.c.
 *          Zones: (1) before sunrise — sinusoid from prev_tmax toward tmin;
 *                 (2) sunrise to sunset-3 — sinusoid peaking at tmax;
 *                 (3) after sunset-3 — sinusoid decaying from tmax.
 *          Requires updateTempTimes() to have been called for the current day.
 *
 * @param state      Climate state (uses tmin_daily, tmax_daily, prev_tmax, hrsr,
 *                   hrss, hrday, dhrdy, dydif).
 * @param hour       Hour of day (0.0–24.0).
 * @returns          Air temperature (deg F).
 *
 * @see Legacy: setTemp() in climate.c
 */
double getSubdailyTemp(const ClimateState& state, double hour);

/**
 * @brief Batch distribute evaporation to all subcatchments.
 *
 * @details evap_out[i] = min(evap_rate, ponded_depth[i] / dt)
 *          This is a vectorisable clamp operation.
 *
 * @param evap_rate    Scalar evap rate (ft/sec).
 * @param ponded_depth [in] Per-subcatchment ponded depth (ft).
 * @param evap_out     [out] Per-subcatchment actual evap rate (ft/sec).
 * @param n            Number of subcatchments.
 * @param dt           Timestep (seconds).
 */
void batchDistributeEvap(double evap_rate, const double* ponded_depth,
                         double* evap_out, int n, double dt);

} // namespace climate
} // namespace openswmm

#endif // OPENSWMM_CLIMATE_HPP
