/**
 * @file openswmm_datetime.h
 * @brief OpenSWMM Engine — SWMM DateTime conversion utilities (C API).
 *
 * @details SWMM's native DateTime is a `double`. The integer part is the
 *          number of days since 1899-12-30 (the OLE Automation / Delphi
 *          TDateTime epoch) and the fractional part is the time-of-day
 *          fraction (0.0 = midnight, 0.5 = noon). This is the same
 *          representation produced and consumed by every other date or
 *          time value crossing the C API boundary — for example
 *          @ref swmm_options_get_start_date, @ref swmm_get_current_time,
 *          @ref swmm_output_get_period_time and the `sim_time` argument
 *          passed to engine callbacks.
 *
 *          This header exposes the encode/decode primitives used inside
 *          the engine so language bindings (Python, .NET, etc.) can
 *          convert losslessly between the SWMM DateTime double and their
 *          native calendar/time types without re-implementing the epoch
 *          arithmetic.
 *
 *          The functions are pure (no engine handle required) and
 *          numerically identical to the legacy `datetime.c` routines.
 *
 * @note  This is NOT an astronomical Julian Date. The epoch is
 *        1899-12-30, matching Microsoft's OLE Automation date and
 *        Delphi's TDateTime — the format SWMM has always used.
 *
 * @section datetime_api_usage Usage
 *
 * @code{.c}
 * #include <openswmm/engine/openswmm_datetime.h>
 *
 * // Build a SWMM DateTime for 2024-06-15 13:30:00
 * double d, t, sim_t;
 * swmm_datetime_encode_date(2024, 6, 15, &d);
 * swmm_datetime_encode_time(13, 30, 0, &t);
 * sim_t = d + t;
 *
 * // Decode a value returned by the engine
 * int y, m, day, h, mi, s;
 * swmm_datetime_decode_date(sim_t, &y, &m, &day);
 * swmm_datetime_decode_time(sim_t, &h, &mi, &s);
 * @endcode
 *
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_DATETIME_H
#define OPENSWMM_ENGINE_DATETIME_H

#include "openswmm_engine_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Epoch constant
 * ========================================================================= */

/**
 * @brief Days from 0001-01-01 to 1899-12-30 (the SWMM DateTime epoch).
 *
 * @details Exposed so bindings can validate against the same reference value
 *          the engine uses internally. A SWMM DateTime of 0.0 corresponds to
 *          1899-12-30T00:00:00.
 */
#define SWMM_DATETIME_DATE_DELTA 693594

/* =========================================================================
 * Encode (calendar parts -> SWMM DateTime double)
 * ========================================================================= */

/**
 * @brief Encode a calendar date as a SWMM DateTime (date portion only).
 *
 * @param year   Year in the range 1..9999.
 * @param month  Month in the range 1..12.
 * @param day    Day of month, valid for the given year/month.
 * @param out    [out] SWMM DateTime double (integer days since 1899-12-30).
 *
 * @returns 0 on success; -1 if @p out is NULL or the date is out of range.
 *          On out-of-range input, @p *out is set to the sentinel
 *          `-SWMM_DATETIME_DATE_DELTA` (matching legacy `datetime_encodeDate`).
 */
SWMM_ENGINE_API int swmm_datetime_encode_date(int year, int month, int day,
                                              double* out);

/**
 * @brief Encode a wall-clock time as the fractional-day part of a SWMM DateTime.
 *
 * @param hour    Hour [0..].
 * @param minute  Minute [0..].
 * @param second  Second [0..].
 * @param out     [out] Fractional-day value in [0.0, 1.0).
 *
 * @returns 0 on success; -1 if @p out is NULL or any input is negative.
 *          On invalid input @p *out is set to 0.0 (matching legacy
 *          `datetime_encodeTime`).
 */
SWMM_ENGINE_API int swmm_datetime_encode_time(int hour, int minute, int second,
                                              double* out);

/* =========================================================================
 * Decode (SWMM DateTime double -> calendar parts)
 * ========================================================================= */

/**
 * @brief Decode the date portion of a SWMM DateTime.
 *
 * @param value  SWMM DateTime double.
 * @param year   [out] Year (>= 0). May be NULL.
 * @param month  [out] Month (1..12). May be NULL.
 * @param day    [out] Day of month (1..31). May be NULL.
 *
 * @returns 0 on success; -1 if all output pointers are NULL.
 */
SWMM_ENGINE_API int swmm_datetime_decode_date(double value,
                                              int* year, int* month, int* day);

/**
 * @brief Decode the time-of-day portion of a SWMM DateTime.
 *
 * @param value   SWMM DateTime double.
 * @param hour    [out] Hour (0..23). May be NULL.
 * @param minute  [out] Minute (0..59). May be NULL.
 * @param second  [out] Second (0..59). May be NULL.
 *
 * @returns 0 on success; -1 if all output pointers are NULL.
 *
 * @note Uses the same integer second decomposition as the legacy engine
 *       (`floor(fracDay * 86400 + 0.5)`), so round-tripping through
 *       encode/decode is bit-identical to a legacy SWMM run.
 */
SWMM_ENGINE_API int swmm_datetime_decode_time(double value,
                                              int* hour, int* minute, int* second);

/* =========================================================================
 * DateTime arithmetic
 * ========================================================================= */

/**
 * @brief Add a (possibly fractional) number of seconds to a SWMM DateTime.
 *
 * @param value     Input SWMM DateTime double.
 * @param seconds   Seconds to add (may be negative).
 * @param out       [out] Resulting SWMM DateTime double.
 *
 * @returns 0 on success; -1 if @p out is NULL.
 *
 * @note Mirrors legacy `datetime_addSeconds`: decomposes the input to
 *       integer H:M:S, adds the offset, and recomposes — producing
 *       bit-identical results to a legacy SWMM run.
 */
SWMM_ENGINE_API int swmm_datetime_add_seconds(double value, double seconds,
                                              double* out);

/**
 * @brief Compute the difference, in whole seconds, between two SWMM DateTimes.
 *
 * @param value1   Later SWMM DateTime.
 * @param value2   Earlier SWMM DateTime.
 * @param out      [out] `value1 - value2` rounded to the nearest second.
 *
 * @returns 0 on success; -1 if @p out is NULL.
 */
SWMM_ENGINE_API int swmm_datetime_time_diff(double value1, double value2,
                                            long* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_ENGINE_DATETIME_H */
