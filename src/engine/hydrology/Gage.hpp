/**
 * @file Gage.hpp
 * @brief Rain gage processing — rainfall interpolation, type conversion.
 *
 * @details Processes rainfall from timeseries or files for each gage,
 *          converts between INTENSITY/VOLUME/CUMULATIVE rain types,
 *          separates rainfall from snowfall based on temperature,
 *          and tracks past n-hour rainfall totals.
 *
 * @note Legacy reference: src/legacy/engine/gage.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GAGE_HPP
#define OPENSWMM_GAGE_HPP

#include "../data/SubcatchData.hpp"
#include <vector>

namespace openswmm {

struct SimulationContext;

namespace gage {

// ============================================================================
// Constants
// ============================================================================

constexpr double ONE_SECOND    = 1.1574074e-5;  ///< One second in days
constexpr int    MAXPASTRAIN   = 48;             ///< Max past hours tracked

// ============================================================================
// Rain type codes
// ============================================================================

enum class RainType : int {
    INTENSITY  = 0,
    VOLUME     = 1,
    CUMULATIVE = 2
};

// ============================================================================
// Per-gage state
// ============================================================================

struct GageState {
    double rainfall      = 0.0;   ///< Current rainfall intensity (project units/sec)
    double snowfall      = 0.0;   ///< Current snowfall intensity (project units/sec)
    double total_precip  = 0.0;   ///< rainfall + snowfall
    double rain_accum    = 0.0;   ///< Cumulative rain accumulator (for CUMULATIVE type)
    double api_rainfall  = -1.0;  ///< API-overridden rainfall (-1 = no override)
    double snow_factor   = 1.0;   ///< Snow catch factor
    double units_factor  = 1.0;   ///< Unit conversion factor
    double adjust_factor = 1.0;   ///< Monthly/seasonal adjustment factor
    double rain_interval = 0.0;   ///< Rain recording interval (seconds)
    RainType rain_type   = RainType::INTENSITY;

    double past_rain[MAXPASTRAIN] = {}; ///< Past hourly rainfall totals
    double past_rain_accum = 0.0;       ///< Accumulator for current hour
    double past_rain_time  = 0.0;       ///< Time of last past-rain update
};

// ============================================================================
// Functions
// ============================================================================

/**
 * @brief Convert raw rainfall value based on rain type.
 *
 * @param raw_value      Raw value from timeseries.
 * @param state          Gage state (for accumulators, factors).
 * @returns Rainfall intensity (project units/sec).
 */
double convertRainfall(double raw_value, GageState& state);

/**
 * @brief Separate rainfall from snowfall based on temperature.
 *
 * @param state       [in/out] Gage state. Sets rainfall/snowfall/total_precip.
 * @param intensity   Total precipitation intensity (project units/sec).
 * @param temperature Current air temperature (project units).
 * @param snow_temp   Snow temperature threshold (project units).
 */
void separatePrecip(GageState& state, double intensity,
                    double temperature, double snow_temp);

/**
 * @brief Update past n-hour rainfall accumulation.
 *
 * @param state      [in/out] Gage state.
 * @param current_time  Current simulation time (seconds from start).
 */
void updatePastRain(GageState& state, double current_time);

/**
 * @brief Get past n-hour rainfall total.
 *
 * @param state  Gage state.
 * @param hours  Number of past hours (1 to MAXPASTRAIN).
 * @returns Cumulative rainfall over past n hours (project depth units).
 */
double getPastRain(const GageState& state, int hours);

/**
 * @brief Process all gages for one timestep.
 *
 * @param ctx  Simulation context (gages + timeseries accessed).
 * @param current_time  Current simulation time (seconds).
 */
void updateAllGages(SimulationContext& ctx, double current_time);

} // namespace gage
} // namespace openswmm

#endif // OPENSWMM_GAGE_HPP
