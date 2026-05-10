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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
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

    /**
     * @brief Gap #31: Cumulative rainfall accumulator (project rain units).
     * @details Tracks the previous raw cumulative value so the delta can be
     *          computed each timestep.  Initialised to 0.  Matches legacy
     *          Gage[i].rainAccum.
     */
    std::vector<double>         cumul_rain_accum;

    /**
     * @brief Object comment from the INP file (';'-prefixed lines immediately
     *        above this gage's data row), joined by literal "\\n".
     *        Empty string means no comment.
     */
    std::vector<std::string>    comments;

    /**
     * @brief Gap #53: Co-gage index — index of the primary gage sharing the
     *        same timeseries, or -1 if this gage reads independently.
     * @details When two gages use the same TIMESERIES source and the same
     *          ts_index, the later gage copies its rainfall from the earlier
     *          one (the primary).  Matches legacy Gage[i].coGage.
     */
    std::vector<int>            co_gage_index;

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
        cumul_rain_accum.assign(un, 0.0);
        co_gage_index.assign(un, -1);
        comments.assign(un, std::string{});
    }

    /**
     * @brief Erase the rain gage at index `idx` from every parallel array.
     *
     * @details Removes element `idx` from every SoA vector. The flat-2D
     *          past_rain array ([gage * MAXPASTRAIN + hour]) has its full
     *          stride for `idx` removed. Spatial arrays are erased separately.
     */
    void erase_at(int idx) {
        const auto ui = static_cast<std::size_t>(idx);
        auto e = [&](auto& v) { if (ui < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx)); };

        e(rain_type); e(source); e(ts_index); e(ts_name);
        e(file_path); e(col_name); e(file_format); e(interval_sec); e(snow_factor);
        e(rainfall); e(next_rainfall); e(api_rainfall); e(next_rain_date); e(is_raining);
        e(past_rain_accum); e(past_rain_time); e(cumul_rain_accum); e(co_gage_index);
        e(comments);

        // Flat 2D past_rain: [gage * MAXPASTRAIN + hour]
        const auto base = ui * static_cast<std::size_t>(MAXPASTRAIN);
        const auto end  = base + static_cast<std::size_t>(MAXPASTRAIN);
        if (end <= past_rain.size())
            past_rain.erase(past_rain.begin() + static_cast<std::ptrdiff_t>(base),
                            past_rain.begin() + static_cast<std::ptrdiff_t>(end));
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
        cumul_rain_accum.shrink_to_fit();
        co_gage_index.shrink_to_fit();
        comments.shrink_to_fit();
    }

    void reset_state() noexcept {
        std::fill(rainfall.begin(),      rainfall.end(),      0.0);
        std::fill(next_rainfall.begin(), next_rainfall.end(), 0.0);
        std::fill(api_rainfall.begin(),  api_rainfall.end(),  -1.0); // -1 = no override
        std::fill(is_raining.begin(),    is_raining.end(),    false);
        std::fill(past_rain.begin(),     past_rain.end(),     0.0);
        std::fill(past_rain_accum.begin(), past_rain_accum.end(), 0.0);
        std::fill(past_rain_time.begin(),  past_rain_time.end(),  0.0);
        std::fill(cumul_rain_accum.begin(), cumul_rain_accum.end(), 0.0);
        // co_gage_index is static (set at parse time), not reset between runs
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_GAGE_DATA_HPP */
