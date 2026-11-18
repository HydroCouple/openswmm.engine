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
 * @file      openswmm_datetime.h
 * @brief     OpenSWMM Engine — SWMM DateTime conversion utilities (C API).
 *
 * @details SWMM's native DateTime is a `double`. The integer part is the
 *          number of days since 1899-12-30 (the OLE Automation / Delphi
 *          TDateTime epoch) and the fractional part is the time-of-day
 *          fraction (0.0 = midnight, 0.5 = noon). This is the same
 *          representation produced and consumed by every other date or
 *          time value crossing the C API boundary — for example
 *          @ref swmm_options_get_start_date and @ref swmm_output_get_period_time.
 *          Elapsed-time APIs and callback arguments use their separately
 *          documented time units rather than this calendar encoding.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
 * @brief Ordinal-date offset used to encode the SWMM DateTime epoch.
 *
 * @details Exposed so bindings can validate against the same reference value
 *          the engine uses internally: ordinal day 1 for 0001-01-01 is
 *          converted to SWMM days by subtracting this offset. A value of
 *          0.0 corresponds to
 *          1899-12-30T00:00:00.
 */
#define SWMM_DATETIME_DATE_DELTA 693594

/* =========================================================================
 * Encode (calendar parts -> SWMM DateTime double)
 * ========================================================================= */

/**
 * @brief Encode a calendar date as a SWMM DateTime (date portion only).
 *
 * @param[in] year Year in the range 1..9999.
 * @param[in] month Month in the range 1..12.
 * @param[in] day Day of month, valid for the given year/month.
 * @param[out] out SWMM DateTime double (integer days since 1899-12-30).
 *
 * @returns 0 on success; -1 if @p out is NULL or the date is out of range.
 *          On out-of-range input, @p *out is set to the sentinel
 *          `-SWMM_DATETIME_DATE_DELTA` (matching legacy `datetime_encodeDate`).
 */
SWMM_ENGINE_API int swmm_datetime_encode_date(int year, int month, int day, double* out);

/**
 * @brief Encode a wall-clock time as the fractional-day part of a SWMM DateTime.
 *
 * @param[in] hour Nonnegative hours; values above 23 are accepted.
 * @param[in] minute Nonnegative minutes; values above 59 are accepted.
 * @param[in] second Nonnegative seconds; values above 59 are accepted.
 * @param[out] out Elapsed days; can be >= 1.0 for components totaling a day or more.
 *
 * @returns 0 on success; -1 if @p out is NULL or any input is negative.
 *          The component sum in seconds must fit in int. On invalid input
 *          @p *out is set to 0.0 (matching legacy
 *          `datetime_encodeTime`).
 */
SWMM_ENGINE_API int swmm_datetime_encode_time(int hour, int minute, int second, double* out);

/* =========================================================================
 * Decode (SWMM DateTime double -> calendar parts)
 * ========================================================================= */

/**
 * @brief Decode the date portion of a SWMM DateTime.
 *
 * @param[in] value Finite SWMM days; floor(value) and its sum with
 *               SWMM_DATETIME_DATE_DELTA must fit in int.
 * @param[out] year Year (>= 0). May be NULL.
 * @param[out] month Month (1..12). May be NULL.
 * @param[out] day Day of month (1..31). May be NULL.
 *
 * @returns 0 on success; -1 if all output pointers are NULL.
 */
SWMM_ENGINE_API int swmm_datetime_decode_date(double value, int* year, int* month, int* day);

/**
 * @brief Decode the time-of-day portion of a SWMM DateTime.
 *
 * @param[in] value Finite SWMM days; the whole-day portion is ignored.
 * @param[out] hour Hour (0..23). May be NULL.
 * @param[out] minute Minute (0..59). May be NULL.
 * @param[out] second Second (0..59). May be NULL.
 *
 * @returns 0 on success; -1 if all output pointers are NULL.
 *
 * @note Uses the same integer second decomposition as the legacy engine
 *       (`floor(fracDay * 86400 + 0.5)`), with results clamped to
 *       23:59:59 when rounding would reach the next
 *       day. Subsecond input precision is discarded, matching legacy SWMM.
 */
SWMM_ENGINE_API int swmm_datetime_decode_time(double value, int* hour, int* minute, int* second);

/* =========================================================================
 * DateTime arithmetic
 * ========================================================================= */

/**
 * @brief Add a (possibly fractional) number of seconds to a SWMM DateTime.
 *
 * @param[in] value Finite input SWMM DateTime in days.
 * @param[in] seconds Finite seconds to add (may be negative or fractional).
 * @param[out] out Caller-owned slot for resulting SWMM days; not retained.
 *
 * @returns 0 on success; -1 if @p out is NULL.
 *
 * @note Mirrors legacy `datetime_addSeconds`: decomposes the input to
 *       integer H:M:S, adds the offset, and recomposes — producing
 *       bit-identical results to a legacy SWMM run.
 */
SWMM_ENGINE_API int swmm_datetime_add_seconds(double value, double seconds, double* out);

/**
 * @brief Compute the difference, in whole seconds, between two SWMM DateTimes.
 *
 * @param[in] value1 Finite SWMM minuend; may be earlier than value2.
 * @param[in] value2 Finite SWMM subtrahend.
 * @param[out] out Signed seconds after rounding each input time independently.
 *
 * @returns 0 on success; -1 if @p out is NULL.
 * @pre The whole-day difference in seconds and final result fit in long.
 * @note Output storage is borrowed for this call and is not retained.
 */
SWMM_ENGINE_API int swmm_datetime_time_diff(double value1, double value2, long* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_ENGINE_DATETIME_H */
