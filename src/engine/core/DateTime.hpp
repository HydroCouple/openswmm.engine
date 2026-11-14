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
 * @file      DateTime.hpp
 * @brief     DateTime utility functions — numerically identical to legacy datetime.c.
 *
 * @details Uses the OLE Automation Date (OADate) / Delphi TDateTime convention:
 *          a double where the integer part is the number of days since
 *          December 30, 1899 and the fractional part represents time-of-day.
 *
 *          DateDelta = 693594 is the Gregorian ordinal of 1899-12-30
 *          when 0001-01-01 is ordinal day 1. Subtracting it converts
 *          calendar ordinals to the SWMM epoch.
 *
 *          Replicates the exact date/time encoding, decoding, and arithmetic
 *          from the legacy SWMM datetime.c including the same integer H:M:S
 *          decomposition and the same rounding behavior.
 *
 *          This ensures that operations like datetime_addSeconds() produce
 *          bit-identical results to the legacy engine, which is critical for
 *          deterministic rain gage interval boundary alignment.
 *
 * @note    This is NOT an astronomical Julian Date. SWMM uses the OADate
 *          convention throughout (matching Delphi's TDateTime type).
 *
 * @see Legacy reference: src/legacy/engine/datetime.c
 * @ingroup engine_core
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATETIME_HPP
#define OPENSWMM_ENGINE_DATETIME_HPP

#include <cmath>

namespace openswmm {
namespace datetime {

// ============================================================================
// Constants — matching legacy datetime.c exactly
// ============================================================================

static constexpr int DateDelta = 693594;          ///< Gregorian ordinal of 1899-12-30
static constexpr double SecsPerDay = 86400.0;     ///< Seconds per day
static constexpr double OneSecond = 1.1574074e-5; ///< 1 second as fractional days

// ============================================================================
// Date/time type — same as legacy DateTime (double)
// ============================================================================

using DateTime = double;

// ============================================================================
// Core functions — numerically identical to legacy datetime.c
// ============================================================================

/**
 * @brief Divide integers with the legacy zero-divisor convention.
 * @param[in] n Dividend; n / d must be representable as int when d is nonzero.
 * @param[in] d Divisor; zero requests zero quotient and remainder.
 * @param[out] result Non-null writable quotient, truncated toward zero.
 * @param[out] remainder Non-null writable n - d * quotient; zero when d is zero.
 * @pre The two output pointers refer to distinct integers.
 * @note Neither pointer is retained.
 */
inline void divMod(int n, int d, int* result, int* remainder) {
    if (d == 0) {
        *result = 0;
        *remainder = 0;
    } else {
        *result = n / d;
        *remainder = n - d * (*result);
    }
}

/**
 * @brief Test the proleptic Gregorian leap-year rule.
 * @param[in] year Calendar year to test; the arithmetic rule also accepts year zero.
 * @return True for multiples of 4 except centuries not divisible by 400.
 */
inline bool isLeapYear(int year) {
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

/**
 * @brief Encode a Gregorian calendar date as midnight in SWMM days.
 * @param[in] year Year in [1, 9999].
 * @param[in] month Month in [1, 12].
 * @param[in] day One-based day valid for the specified month and leap year.
 * @return Integer days relative to 1899-12-30, or -DateDelta for invalid input.
 * @see Legacy datetime_encodeDate() in datetime.c.
 */
inline DateTime encodeDate(int year, int month, int day) {
    static constexpr int DaysPerMonth[2][12] = {{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
                                                {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
    int i = isLeapYear(year) ? 1 : 0;
    if (year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 &&
        day <= DaysPerMonth[i][month - 1]) {
        for (int j = 0; j < month - 1; ++j)
            day += DaysPerMonth[i][j];
        int y = year - 1;
        return y * 365 + y / 4 - y / 100 + y / 400 + day - DateDelta;
    }
    return -DateDelta;
}

/**
 * @brief Convert nonnegative time components to an elapsed-day fraction.
 * @param[in] hour Nonnegative hours; values above 23 are accepted.
 * @param[in] minute Nonnegative minutes; values above 59 are accepted.
 * @param[in] second Nonnegative seconds; values above 59 are accepted.
 * @return (3600 * hour + 60 * minute + second) / 86400, or zero if any
 * component is negative. The result can exceed one day.
 * @pre The component sum must be representable as int.
 * @see Legacy datetime_encodeTime() in datetime.c.
 */
inline DateTime encodeTime(int hour, int minute, int second) {
    if (hour >= 0 && minute >= 0 && second >= 0) {
        int s = hour * 3600 + minute * 60 + second;
        return static_cast<double>(s) / SecsPerDay;
    }
    return 0.0;
}

/**
 * @brief Decode the whole-day part of a SWMM DateTime.
 * @param[in] date Finite SWMM days relative to 1899-12-30; fractional days
 * are discarded with floor(), including for dates before the epoch.
 * @param[out] year Decoded Gregorian year; zero for dates before 0001-01-01.
 * @param[out] month One-based month; 1 for dates before 0001-01-01.
 * @param[out] day One-based day of month; 1 for dates before 0001-01-01.
 * @pre floor(date) and floor(date) + DateDelta must be representable as int.
 * @see Legacy datetime_decodeDate() in datetime.c.
 */
inline void decodeDate(DateTime date, int& year, int& month, int& day) {
    static constexpr int DaysPerMonth[2][12] = {{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
                                                {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
    constexpr int D1 = 365, D4 = 1461, D100 = 36524, D400 = 146097;

    int t = static_cast<int>(std::floor(date)) + DateDelta;
    if (t <= 0) {
        year = 0;
        month = 1;
        day = 1;
        return;
    }

    t--;
    int y = 1, i, d;
    while (t >= D400) {
        t -= D400;
        y += 400;
    }
    divMod(t, D100, &i, &d);
    if (i == 4) {
        i--;
        d += D100;
    }
    y += i * 100;
    divMod(d, D4, &i, &d);
    y += i * 4;
    divMod(d, D1, &i, &d);
    if (i == 4) {
        i--;
        d += D1;
    }
    y += i;

    int k = isLeapYear(y) ? 1 : 0;
    int m = 1;
    for (;;) {
        i = DaysPerMonth[k][m - 1];
        if (d < i)
            break;
        d -= i;
        m++;
    }
    year = y;
    month = m;
    day = d + 1;
}

/**
 * @brief Decode a SWMM day fraction to the nearest whole second.
 * @param[in] time Finite SWMM DateTime; only time - floor(time) is used.
 * @param[out] h Hour in [0, 23].
 * @param[out] m Minute in [0, 59].
 * @param[out] s Second in [0, 59].
 * @details Rounds fractional-day seconds with floor(seconds + 0.5).
 * A value rounding to 86400 is clamped to 86399 rather than carrying into
 * the next date. This matches legacy datetime_decodeTime().
 */
inline void decodeTime(DateTime time, int& h, int& m, int& s) {
    double fracDay = (time - std::floor(time)) * SecsPerDay;
    int secs = static_cast<int>(std::floor(fracDay + 0.5));
    if (secs >= 86400)
        secs = 86399;
    divMod(secs, 60, &m, &s); // mins, secs
    int mins = m;
    divMod(mins, 60, &h, &m); // hours, mins
    if (h > 23)
        h = 0;
}

/**
 * @brief Add an offset after rounding the input time to whole seconds.
 * @param[in] date Finite SWMM DateTime in days relative to 1899-12-30.
 * @param[in] seconds Finite seconds to add; negative and fractional offsets
 * are accepted, and the result may cross any number of day boundaries.
 * @return SWMM DateTime containing the rounded input time plus the offset.
 * @details Decodes the input using decodeTime(), then recomposes the day
 * and time. The decompose/recompose sequence matches legacy arithmetic;
 * subsecond precision already present in date is not retained.
 * @see Legacy datetime_addSeconds() in datetime.c.
 */
inline DateTime addSeconds(DateTime date, double seconds) {
    double d = std::floor(date);
    int h, m, s;
    decodeTime(date, h, m, s);
    return d + (3600.0 * h + 60.0 * m + s + seconds) / SecsPerDay;
}

/**
 * @brief Subtract two SWMM DateTimes using whole-second decoded times.
 * @param[in] date1 Finite minuend in SWMM days; need not be later than date2.
 * @param[in] date2 Finite subtrahend in SWMM days.
 * @return Signed date1 - date2 in seconds after independently rounding each
 * time-of-day with decodeTime(). Negative values indicate date1 < date2.
 * @pre The whole-day difference in seconds and final result fit in long.
 * @see Legacy datetime_timeDiff() in datetime.c.
 */
inline long timeDiff(DateTime date1, DateTime date2) {
    double d1 = std::floor(date1);
    double d2 = std::floor(date2);
    int h, m, s;
    decodeTime(date1, h, m, s);
    long s1 = 3600L * h + 60L * m + s;
    decodeTime(date2, h, m, s);
    long s2 = 3600L * h + 60L * m + s;
    long secs = static_cast<long>(std::floor((d1 - d2) * SecsPerDay + 0.5));
    secs += (s1 - s2);
    return secs;
}

/**
 * @brief Extract the month from a SWMM DateTime.
 * @param[in] date Finite SWMM DateTime for a valid Gregorian date encoded
 * by encodeDate(); the time-of-day does not affect the result.
 * @return One-based Gregorian month in [1, 12].
 * @pre The date satisfies decodeDate()'s representability requirements.
 */
inline int monthOfYear(DateTime date) {
    int y, m, d;
    decodeDate(date, y, m, d);
    return m;
}

/**
 * @brief Extract the ordinal day from a SWMM DateTime.
 * @param[in] date Finite SWMM DateTime for a valid Gregorian date encoded
 * by encodeDate(); the time-of-day does not affect the result.
 * @return One-based day in [1, 365] or [1, 366] for leap years.
 * @pre The date satisfies decodeDate()'s representability requirements.
 */
inline int dayOfYear(DateTime date) {
    int y, m, d;
    decodeDate(date, y, m, d);
    DateTime startOfYear = encodeDate(y, 1, 1);
    return static_cast<int>(std::floor(date - startOfYear)) + 1;
}

} // namespace datetime
} // namespace openswmm

#endif // OPENSWMM_ENGINE_DATETIME_HPP
