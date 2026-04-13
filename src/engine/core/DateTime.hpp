/**
 * @file DateTime.hpp
 * @brief DateTime utility functions — numerically identical to legacy datetime.c.
 *
 * @details Uses the OLE Automation Date (OADate) / Delphi TDateTime convention:
 *          a double where the integer part is the number of days since
 *          December 30, 1899 and the fractional part represents time-of-day.
 *
 *          DateDelta = 693594 days from 01/01/0000 to 12/31/1899 converts
 *          between the proleptic Gregorian calendar and the OADate epoch.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_DATETIME_HPP
#define OPENSWMM_ENGINE_DATETIME_HPP

#include <cmath>

namespace openswmm {
namespace datetime {

// ============================================================================
// Constants — matching legacy datetime.c exactly
// ============================================================================

static constexpr int    DateDelta  = 693594;  ///< Days from 01/01/0000 to 12/31/1899
static constexpr double SecsPerDay = 86400.0; ///< Seconds per day
static constexpr double OneSecond  = 1.1574074e-5; ///< 1 second as fractional days

// ============================================================================
// Date/time type — same as legacy DateTime (double)
// ============================================================================

using DateTime = double;

// ============================================================================
// Core functions — numerically identical to legacy datetime.c
// ============================================================================

/**
 * @brief Integer divmod — matching legacy divMod().
 */
inline void divMod(int n, int d, int* result, int* remainder) {
    if (d == 0) { *result = 0; *remainder = 0; }
    else { *result = n / d; *remainder = n - d * (*result); }
}

/**
 * @brief Check if year is a leap year.
 */
inline bool isLeapYear(int year) {
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

/**
 * @brief Encode year-month-day to DateTime.
 * @see Legacy: datetime_encodeDate() in datetime.c
 */
inline DateTime encodeDate(int year, int month, int day) {
    static constexpr int DaysPerMonth[2][12] = {
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
        {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
    int i = isLeapYear(year) ? 1 : 0;
    if (year >= 1 && year <= 9999 && month >= 1 && month <= 12
        && day >= 1 && day <= DaysPerMonth[i][month - 1]) {
        for (int j = 0; j < month - 1; ++j) day += DaysPerMonth[i][j];
        int y = year - 1;
        return y * 365 + y / 4 - y / 100 + y / 400 + day - DateDelta;
    }
    return -DateDelta;
}

/**
 * @brief Encode hour:minute:second to fractional day.
 * @see Legacy: datetime_encodeTime() in datetime.c
 */
inline DateTime encodeTime(int hour, int minute, int second) {
    if (hour >= 0 && minute >= 0 && second >= 0) {
        int s = hour * 3600 + minute * 60 + second;
        return static_cast<double>(s) / SecsPerDay;
    }
    return 0.0;
}

/**
 * @brief Decode DateTime to year-month-day.
 * @see Legacy: datetime_decodeDate() in datetime.c
 */
inline void decodeDate(DateTime date, int& year, int& month, int& day) {
    static constexpr int DaysPerMonth[2][12] = {
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
        {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
    constexpr int D1 = 365, D4 = 1461, D100 = 36524, D400 = 146097;

    int t = static_cast<int>(std::floor(date)) + DateDelta;
    if (t <= 0) { year = 0; month = 1; day = 1; return; }

    t--;
    int y = 1, i, d;
    while (t >= D400) { t -= D400; y += 400; }
    divMod(t, D100, &i, &d);
    if (i == 4) { i--; d += D100; }
    y += i * 100;
    divMod(d, D4, &i, &d);
    y += i * 4;
    divMod(d, D1, &i, &d);
    if (i == 4) { i--; d += D1; }
    y += i;

    int k = isLeapYear(y) ? 1 : 0;
    int m = 1;
    for (;;) {
        i = DaysPerMonth[k][m - 1];
        if (d < i) break;
        d -= i;
        m++;
    }
    year = y; month = m; day = d + 1;
}

/**
 * @brief Decode DateTime to hour:minute:second.
 * @see Legacy: datetime_decodeTime() in datetime.c
 *
 * @details Uses floor(fracDay + 0.5) for rounding, matching legacy exactly.
 *          This integer decomposition is the key to deterministic date arithmetic.
 */
inline void decodeTime(DateTime time, int& h, int& m, int& s) {
    double fracDay = (time - std::floor(time)) * SecsPerDay;
    int secs = static_cast<int>(std::floor(fracDay + 0.5));
    if (secs >= 86400) secs = 86399;
    divMod(secs, 60, &m, &s);  // mins, secs
    int mins = m;
    divMod(mins, 60, &h, &m);  // hours, mins
    if (h > 23) h = 0;
}

/**
 * @brief Add seconds to a DateTime — numerically identical to legacy.
 * @see Legacy: datetime_addSeconds() in datetime.c
 *
 * @details Decomposes time to integer H:M:S, adds seconds, recomposes.
 *          This decompose-recompose cycle is deterministic and produces
 *          the exact same floating-point result as the legacy engine.
 */
inline DateTime addSeconds(DateTime date, double seconds) {
    double d = std::floor(date);
    int h, m, s;
    decodeTime(date, h, m, s);
    return d + (3600.0 * h + 60.0 * m + s + seconds) / SecsPerDay;
}

/**
 * @brief Compute difference in seconds between two DateTimes.
 * @see Legacy: datetime_timeDiff() in datetime.c
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
 * @brief Get month of year (1..12) from DateTime.
 */
inline int monthOfYear(DateTime date) {
    int y, m, d;
    decodeDate(date, y, m, d);
    return m;
}

/**
 * @brief Get day of year (1..365/366) from DateTime.
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
