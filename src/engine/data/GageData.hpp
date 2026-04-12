/**
 * @file GageData.hpp
 * @brief Structure-of-Arrays (SoA) storage for rain gages.
 *
 * @details Replaces the global `Gage[]` array and `TGage` struct from
 *          src/solver/objects.h. Rain gages provide precipitation input to
 *          subcatchments.
 *
 * Precipitation sources:
 * - TIMESERIES — reads from an in-file [TIMESERIES] table
 * - FILE       — reads from an external rain file (possibly multi-column CSV)
 *
 * @see Legacy reference: src/solver/objects.h — TGage (line ~90)
 * @see Legacy reference: src/solver/globals.h — Gage[], Ngages
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_GAGE_DATA_HPP
#define OPENSWMM_ENGINE_GAGE_DATA_HPP

#include <vector>
#include <string>
#include <cstdint>

namespace openswmm {

// ============================================================================
// Rain source enumeration
// ============================================================================

/**
 * @brief Precipitation source type for a rain gage.
 * @see Legacy: RainType in src/solver/enums.h
 */
enum class RainSource : int8_t {
    TIMESERIES = 0,  ///< Data from an in-file [TIMESERIES]
    FILE_RAIN  = 1   ///< Data from an external rain file
};

/**
 * @brief Rain data format in an external file.
 * @see Legacy: RainFileType in src/solver/enums.h
 */
enum class RainFileFormat : int8_t {
    UNKNOWN  = -1,
    NWS_15   = 0,   ///< NWS 15-minute data
    NWS_HOURLY = 1, ///< NWS hourly data
    DSI_3240 = 2,   ///< NCDC DSI 3240 hourly
    DSI_3260 = 3,   ///< NCDC DSI 3260 15-minute
    HLY_PRCP = 4,   ///< HLY_PRCP format
    STAN_PRCP = 5,  ///< Standard SWMM rain file
    USER_CSV = 6    ///< User-supplied multi-column CSV (new in 6.0.0, R08)
};

// ============================================================================
// GageData — SoA layout
// ============================================================================

/**
 * @brief Structure-of-Arrays storage for all rain gages.
 *
 * @details The `col_name` field supports the new multi-column CSV format (R08):
 * @code
 * [RAINGAGES]
 * ;; Name   Format   Interval  SCF   Source
 * RG1       VOLUME   0:15      1.0   FILE "rain.csv:EAST_STATION"
 * @endcode
 *
 * @ingroup engine_data
 */
struct GageData {

    // -----------------------------------------------------------------------
    // Static properties — set at parse time
    // -----------------------------------------------------------------------

    /**
     * @brief Rain data type: 0=INTENSITY, 1=VOLUME, 2=CUMULATIVE.
     * @see Legacy: Gage[i].rainType
     */
    std::vector<int>            rain_type;

    /** @brief Precipitation source (TIMESERIES or FILE). */
    std::vector<RainSource>     source;

    /**
     * @brief Time series index (when source == TIMESERIES).
     * @see Legacy: Gage[i].tSeries
     */
    std::vector<int>            ts_index;

    /**
     * @brief External rain file path (when source == FILE_RAIN).
     * @see Legacy: Gage[i].fname
     */
    std::vector<std::string>    file_path;

    /**
     * @brief Timeseries name (for deferred resolution when TS parsed after gages).
     */
    std::vector<std::string>    ts_name;

    /**
     * @brief Column name in the external CSV (when source == FILE_RAIN).
     * @details New in 6.0.0 — supports "FILE path.csv:COLUMN_NAME" syntax (R08).
     *          Empty string means use the first/only data column.
     */
    std::vector<std::string>    col_name;

    /**
     * @brief Rain file format.
     * @see Legacy: Gage[i].fileFormat
     */
    std::vector<RainFileFormat> file_format;

    /**
     * @brief Recording interval in seconds.
     * @see Legacy: Gage[i].rainInterval
     */
    std::vector<int>            interval_sec;

    /**
     * @brief Snow catch deficiency correction factor (1.0 = no correction).
     * @see Legacy: Gage[i].snowFactor
     */
    std::vector<double>         snow_factor;

    // -----------------------------------------------------------------------
    // State variables — updated each timestep
    // -----------------------------------------------------------------------

    /**
     * @brief Current rainfall rate (project length/time units — inches or mm/hr).
     * @see Legacy: Gage[i].rainfall
     */
    std::vector<double>         rainfall;

    /**
     * @brief Rainfall rate at the next recorded interval (for interpolation).
     * @see Legacy: Gage[i].nextRainfall
     */
    std::vector<double>         next_rainfall;

    /**
     * @brief Current API (antecedent precipitation index) rainfall.
     * @see Legacy: Gage[i].apiRainfall
     */
    std::vector<double>         api_rainfall;

    /**
     * @brief Simulation time of the next recorded value (decimal days).
     * @see Legacy: Gage[i].nextRainDate
     */
    std::vector<double>         next_rain_date;

    /**
     * @brief Current state flag (0 = no rain, 1 = raining).
     * @see Legacy: Gage[i].currentHour / isUsed logic
     */
    std::vector<bool>           is_raining;

    // -----------------------------------------------------------------------
    // Past-rain history (for control rules — GAGE_RAIN_PAST)
    // -----------------------------------------------------------------------

    static constexpr int MAXPASTRAIN = 48; ///< Max past hours tracked per gage

    /** @brief Flat 2D: [gage * MAXPASTRAIN + hour]. Hourly rain totals. */
    std::vector<double>         past_rain;

    /** @brief Per-gage accumulator for the current partial hour. */
    std::vector<double>         past_rain_accum;

    /** @brief Per-gage time (seconds) of last past-rain shift. */
    std::vector<double>         past_rain_time;

    // -----------------------------------------------------------------------
    // Capacity management
    // -----------------------------------------------------------------------

    int count() const noexcept { return static_cast<int>(source.size()); }

    void resize(int n) {
        const auto un = static_cast<std::size_t>(n);

        rain_type.assign(un, 0);
        source.assign(un, RainSource::TIMESERIES);
        ts_index.assign(un, -1);
        ts_name.assign(un, std::string{});
        file_path.assign(un, std::string{});
        col_name.assign(un, std::string{});
        file_format.assign(un, RainFileFormat::UNKNOWN);
        interval_sec.assign(un, 3600);
        snow_factor.assign(un, 1.0);

        rainfall.assign(un, 0.0);
        next_rainfall.assign(un, 0.0);
        api_rainfall.assign(un, -1.0);  // -1.0 means no API override
        next_rain_date.assign(un, 0.0);
        is_raining.assign(un, false);

        past_rain.assign(un * MAXPASTRAIN, 0.0);
        past_rain_accum.assign(un, 0.0);
        past_rain_time.assign(un, 0.0);
    }

    /**
     * @brief Release excess vector capacity accumulated during parsing.
     */
    void shrink_to_fit() {
        rain_type.shrink_to_fit();
        source.shrink_to_fit();
        ts_index.shrink_to_fit();
        ts_name.shrink_to_fit();
        file_path.shrink_to_fit();
        col_name.shrink_to_fit();
        file_format.shrink_to_fit();
        interval_sec.shrink_to_fit();
        snow_factor.shrink_to_fit();

        rainfall.shrink_to_fit();
        next_rainfall.shrink_to_fit();
        api_rainfall.shrink_to_fit();
        next_rain_date.shrink_to_fit();
        is_raining.shrink_to_fit();

        past_rain.shrink_to_fit();
        past_rain_accum.shrink_to_fit();
        past_rain_time.shrink_to_fit();
    }

    void reset_state() noexcept {
        std::fill(rainfall.begin(),      rainfall.end(),      0.0);
        std::fill(next_rainfall.begin(), next_rainfall.end(), 0.0);
        std::fill(api_rainfall.begin(),  api_rainfall.end(),  -1.0); // -1 = no override
        std::fill(is_raining.begin(),    is_raining.end(),    false);
        std::fill(past_rain.begin(),     past_rain.end(),     0.0);
        std::fill(past_rain_accum.begin(), past_rain_accum.end(), 0.0);
        std::fill(past_rain_time.begin(),  past_rain_time.end(),  0.0);
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_GAGE_DATA_HPP */
