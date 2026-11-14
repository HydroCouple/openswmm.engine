//-----------------------------------------------------------------------------
//   datetime.c
//
//   Project:  EPA SWMM5
//   Version:  5.3
//   Date:     11/01/21  (Build 5.2.0)
//   Author:   L. Rossman
//
//   DateTime functions.
//
//   Update History
//   ==============
//   Build 5.1.011:
//   - decodeTime() no longer rounds up.
//   - New getTimeStamp function added.
//-----------------------------------------------------------------------------

/**
 * @file      datetime.c
 * @brief     Legacy SWMM date and time representation and arithmetic.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#define _CRT_SECURE_NO_DEPRECATE

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "datetime.h"

// Macro to convert charcter x to upper case
#define UCHAR(x) (((x) >= 'a' && (x) <= 'z') ? ((x) & ~32) : (x))

//-----------------------------------------------------------------------------
//  Constants
//-----------------------------------------------------------------------------
static const char* MonthTxt[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                 "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
static const int DaysPerMonth[2][12] = // days per month
    {{31, 28, 31, 30, 31, 30,          // normal years
      31, 31, 30, 31, 30, 31},
     {31, 29, 31, 30, 31, 30, // leap years
      31, 31, 30, 31, 30, 31}};
static const int DateDelta = 693594;     // days since 01/01/00
static const double SecsPerDay = 86400.; // seconds per day

//-----------------------------------------------------------------------------
//  Shared variables
//-----------------------------------------------------------------------------
static int DateFormat;

//=============================================================================

/**
 * @brief Divide two integers with legacy zero-divisor handling.
 * @param[in] n Dividend; n / d must be representable as int when d is nonzero.
 * @param[in] d Divisor; zero requests zero quotient and remainder.
 * @param[out] result Non-null distinct caller-owned quotient slot, truncated toward zero.
 * @param[out] remainder Non-null distinct caller-owned remainder slot, n - d * quotient.
 * @note Both output values are zero when d is zero; pointers are not retained.
 */
void divMod(int n, int d, int* result, int* remainder)

//  Input:   n = numerator
//           d = denominator
//  Output:  result = integer part of n/d
//           remainder = remainder of n/d
//  Purpose: finds integer part and remainder of n/d.

{
    if (d == 0) {
        *result = 0;
        *remainder = 0;
    } else {
        *result = n / d;
        *remainder = n - d * (*result);
    }
}

//=============================================================================

/**
 * @brief Apply the Gregorian divisible-by-four/century/400-year leap rule.
 * @param[in] year Calendar year; the arithmetic rule also accepts year zero.
 * @return One for a leap year, zero otherwise.
 */
int isLeapYear(int year)

//  Input:   year = a year
//  Output:  returns 1 if year is a leap year, 0 if not
//  Purpose: determines if year is a leap year.

{
    if ((year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0)))
        return 1;
    else
        return 0;
}

//=============================================================================

/**
 * @brief Match the first three letters of an English month name.
 * @par Implementation arguments
 * - month (in): Borrowed character storage with at least three readable
 * characters; matching is ASCII case-insensitive and later text is ignored.
 * @return Month number 1..12, or zero when no three-letter name matches.
 */
int datetime_findMonth(char* month)

//  Input:   month = month of year as character string
//  Output:  returns: month of year as a number (1-12)
//  Purpose: finds number (1-12) of month.

{
    int i;
    for (i = 0; i < 12; i++) {
        if (UCHAR(month[0]) == MonthTxt[i][0] && UCHAR(month[1]) == MonthTxt[i][1] &&
            UCHAR(month[2]) == MonthTxt[i][2])
            return i + 1;
    }
    return 0;
}

//=============================================================================

/**
 * @brief Encode a Gregorian date as midnight in SWMM calendar days.
 * @par Implementation arguments
 * - year (in): Gregorian year in [1, 9999].
 * - month (in): One-based month in [1, 12].
 * - day (in): Valid one-based day for the month and leap year.
 * @return Integer-valued days since 1899-12-30; -DateDelta for invalid components.
 */
DateTime datetime_encodeDate(int year, int month, int day)

//  Input:   year = a year
//           month = a month (1 to 12)
//           day = a day of month
//  Output:  returns encoded value of year-month-day
//  Purpose: encodes year-month-day to a DateTime value.

{
    int i, j;
    i = isLeapYear(year);
    if ((year >= 1) && (year <= 9999) && (month >= 1) && (month <= 12) && (day >= 1) &&
        (day <= DaysPerMonth[i][month - 1])) {
        for (j = 0; j < month - 1; j++)
            day += DaysPerMonth[i][j];
        i = year - 1;
        i = i * 365 + i / 4 - i / 100 + i / 400 + day - DateDelta;
        return i;
    } else
        return -DateDelta;
}

//=============================================================================

/**
 * @brief Convert nonnegative hour/minute/second components to elapsed days.
 * @par Implementation arguments
 * - hour (in): Nonnegative hours; values above 23 are accepted.
 * - minute (in): Nonnegative minutes; values above 59 are accepted.
 * - second (in): Nonnegative seconds; values above 59 are accepted.
 * @return Total component seconds divided by 86400, or zero if a component
 * is negative. The result may be one day or greater.
 * @pre The total component seconds are representable as int.
 */
DateTime datetime_encodeTime(int hour, int minute, int second)

//  Input:   hour = hour of day (0-24)
//           minute = minute of hour (0-60)
//           second = seconds of minute (0-60)
//  Output:  returns time encoded as fractional part of a day
//  Purpose: encodes hour:minute:second to a DateTime value

{
    int s;
    if ((hour >= 0) && (minute >= 0) && (second >= 0)) {
        s = (hour * 3600 + minute * 60 + second);
        return (double)s / SecsPerDay;
    } else
        return 0.0;
}

//=============================================================================

/**
 * @brief Decode a SWMM whole-day value into Gregorian date components.
 * @par Implementation arguments
 * - date (in): Finite SWMM calendar days; floor(date) is used, including
 * for negative values. Its conversion and sum with DateDelta must fit in int.
 * - year (out): Non-null caller-owned year slot.
 * - month (out): Non-null caller-owned month slot in [1, 12].
 * - day (out): Non-null caller-owned day-of-month slot in [1, 31].
 * @note Dates before 0001-01-01 decode as year zero, month one, day one.
 * All three output slots must be distinct and are not retained.
 */
void datetime_decodeDate(DateTime date, int* year, int* month, int* day)

//  Input:   date = encoded date/time value
//  Output:  year = 4-digit year
//           month = month of year (1-12)
//           day   = day of month
//  Purpose: decodes DateTime value to year-month-day.

{
    int D1, D4, D100, D400;
    int y, m, d, i, k, t;

    D1 = 365;            //365
    D4 = D1 * 4 + 1;     //1461
    D100 = D4 * 25 - 1;  //36524
    D400 = D100 * 4 + 1; //146097

    t = (int)(floor(date)) + DateDelta;
    if (t <= 0) {
        *year = 0;
        *month = 1;
        *day = 1;
    } else {
        t--;
        y = 1;
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
        k = isLeapYear(y);
        m = 1;
        for (;;) {
            i = DaysPerMonth[k][m - 1];
            if (d < i)
                break;
            d -= i;
            m++;
        }
        *year = y;
        *month = m;
        *day = d + 1;
    }
}

//=============================================================================

/**
 * @brief Round a SWMM day fraction to whole-second clock components.
 * @par Implementation arguments
 * - time (in): Finite SWMM days; only time - floor(time) is used.
 * - h (out): Non-null distinct caller-owned hour slot in [0, 23].
 * - m (out): Non-null distinct caller-owned minute slot in [0, 59].
 * - s (out): Non-null distinct caller-owned second slot in [0, 59].
 * @note Rounds with floor(fraction * 86400 + 0.5); a result reaching 86400
 * is clamped to 86399 rather than carried into the next date.
 */
void datetime_decodeTime(DateTime time, int* h, int* m, int* s)

//  Input:   time = decimal fraction of a day
//  Output:  h = hour of day (0-23)
//           m = minute of hour (0-59)
//           s = second of minute (0-59)
//  Purpose: decodes DateTime value to hour:minute:second.

{
    int secs;
    int mins;
    double fracDay = (time - floor(time)) * SecsPerDay;
    secs = (int)(floor(fracDay + 0.5));
    if (secs >= 86400)
        secs = 86399;
    divMod(secs, 60, &mins, s);
    divMod(mins, 60, h, m);
    if (*h > 23)
        *h = 0;
}

//=============================================================================

/**
 * @brief Format the date portion of a SWMM DateTime with the shared DateFormat.
 * @par Implementation arguments
 * - date (in): Finite SWMM days satisfying datetime_decodeDate()'s integer limits.
 * - s (out): Non-null caller-owned buffer of at least DATE_STR_SIZE bytes;
 * receives a null-terminated date string, possibly truncated to fit.
 * @note Reads the shared format without modifying it; no storage is retained.
 */
void datetime_dateToStr(DateTime date, char* s)

//  Input:   date = encoded date/time value
//  Output:  s = formatted date string
//  Purpose: represents DateTime date value as a formatted string.

{
    int y, m, d;
    datetime_decodeDate(date, &y, &m, &d);
    switch (DateFormat) {
    case Y_M_D:
        snprintf(s, DATE_STR_SIZE, "%4d-%3s-%02d", y, MonthTxt[m - 1], d);
        break;

    case M_D_Y:
        //sprintf(dateStr, "%3s-%02d-%4d", MonthTxt[m-1], d, y);
        snprintf(s, DATE_STR_SIZE, "%02d/%02d/%04d", m, d, y);
        break;

    default:
        snprintf(s, DATE_STR_SIZE, "%02d-%3s-%4d", d, MonthTxt[m - 1], y);
    }
}

/**
 * @brief Format the rounded time-of-day portion as HH:MM:SS.
 * @par Implementation arguments
 * - time (in): Finite SWMM calendar/elapsed days; only its day fraction is decoded.
 * - s (out): Non-null caller-owned buffer of at least TIME_STR_SIZE bytes;
 * receives a null-terminated clock string. The pointer is not retained.
 */
void datetime_timeToStr(DateTime time, char* s)

//  Input:   time = decimal fraction of a day
//  Output:  s = time in hr:min:sec format
//  Purpose: represents DateTime time value as a formatted string.

{
    int hr, min, sec;
    datetime_decodeTime(time, &hr, &min, &sec);
    snprintf(s, TIME_STR_SIZE, "%02d:%02d:%02d", hr, min, sec);
}

//=============================================================================

/**
 * @brief Parse a calendar date using the shared date-format setting.
 * @par Implementation arguments
 * - s (in): Non-null borrowed null-terminated text with slash/hyphen-separated
 * fields; accepts numeric months or three-letter English month names.
 * - d (out): Non-null caller-owned date slot, initialized to -DateDelta
 * and replaced with SWMM midnight days only for a valid encoded date.
 * @return One for successful conversion, zero otherwise.
 * @note DateFormat controls field order; input storage is not changed or retained.
 */
int datetime_strToDate(char* s, DateTime* d) {
    int yr = 0, mon = 0, day = 0, n;
    char month[4];
    char sep1, sep2;
    *d = -DateDelta;
    if (strchr(s, '-') || strchr(s, '/')) {
        switch (DateFormat) {
        case Y_M_D:
            n = sscanf(s, "%d%c%d%c%d", &yr, &sep1, &mon, &sep2, &day);
            if (n < 3) {
                mon = 0;
                n = sscanf(s, "%d%c%3s%c%d", &yr, &sep1, month, &sep2, &day);
                if (n < 3)
                    return 0;
            }
            break;

        case D_M_Y:
            n = sscanf(s, "%d%c%d%c%d", &day, &sep1, &mon, &sep2, &yr);
            if (n < 3) {
                mon = 0;
                n = sscanf(s, "%d%c%3s%c%d", &day, &sep1, month, &sep2, &yr);
                if (n < 3)
                    return 0;
            }
            break;

        default: // M_D_Y
            n = sscanf(s, "%d%c%d%c%d", &mon, &sep1, &day, &sep2, &yr);
            if (n < 3) {
                mon = 0;
                n = sscanf(s, "%3s%c%d%c%d", month, &sep1, &day, &sep2, &yr);
                if (n < 3)
                    return 0;
            }
        }
        if (mon == 0)
            mon = datetime_findMonth(month);
        *d = datetime_encodeDate(yr, mon, day);
    }
    if (*d == -DateDelta)
        return 0;
    else
        return 1;
}

//=============================================================================

/**
 * @brief Parse decimal hours or nonnegative colon-separated clock components.
 * @par Implementation arguments
 * - s (in): Non-null borrowed null-terminated time text; not modified or retained.
 * - t (out): Non-null caller-owned result in elapsed days. Failed colon
 * parsing leaves zero; successful decimal hours are divided by 24.
 * @return One when a complete strtod conversion succeeds, including signed
 * decimal hours, or when parsed colon components are nonnegative; zero otherwise.
 * @note Clock components are not capped at 23/59/59, and trailing colon-text
 * validation is permissive. This routine does not validate finite numeric range.
 */
int datetime_strToTime(char* s, DateTime* t)

//  Input:   s = time as string
//  Output:  t = encoded time,
//           returns 1 if conversion successful, 0 if not
//  Purpose: converts a string time to a DateTime value.
//  Note:    accepts time as hr:min:sec or as decimal hours.

{
    int n, hr, min = 0, sec = 0;
    char* endptr;

    // Attempt to read time as decimal hours
    *t = strtod(s, &endptr);
    if (*endptr == 0) {
        *t /= 24.0;
        return 1;
    }

    // Read time in hr:min:sec format
    *t = 0.0;
    n = sscanf(s, "%d:%d:%d", &hr, &min, &sec);
    if (n == 0)
        return 0;
    *t = datetime_encodeTime(hr, min, sec);
    if ((hr >= 0) && (min >= 0) && (sec >= 0))
        return 1;
    else
        return 0;
}

//=============================================================================

/**
 * @brief Select the shared calendar text field order.
 * @par Implementation arguments
 * - fmt (in): Y_M_D, D_M_Y or M_D_Y; out-of-range codes leave the setting unchanged.
 * @note Affects later date parsing/formatting and must not race other shared-format calls.
 */
void datetime_setDateFormat(int fmt)

//  Input:   fmt = date format code
//  Output:  none
//  Purpose: sets date format

{
    if (fmt >= Y_M_D && fmt <= M_D_Y)
        DateFormat = fmt;
}

//=============================================================================

/**
 * @brief Add seconds after rounding the input time to the legacy whole-second clock.
 * @par Implementation arguments
 * - date1 (in): Finite SWMM calendar days since 1899-12-30.
 * - seconds (in): Finite signed or fractional seconds to add.
 * @return Recombined SWMM days; offsets can cross day boundaries.
 * Subsecond precision already in date1 is discarded by datetime_decodeTime().
 */
DateTime datetime_addSeconds(DateTime date1, double seconds)

//  Input:   date1 = an encoded date/time value
//           seconds = number of seconds to add to date1
//  Output:  returns updated value of date1
//  Purpose: adds a given number of seconds to a date/time.

{
    double d = floor(date1);
    int h, m, s;
    datetime_decodeTime(date1, &h, &m, &s);
    return d + (3600.0 * h + 60.0 * m + s + seconds) / SecsPerDay;
}

//=============================================================================

/**
 * @brief Add two day values after rounding each fractional part to whole seconds.
 * @par Implementation arguments
 * - date1 (in): Finite SWMM calendar date/time in days.
 * - date2 (in): Finite signed elapsed-day offset; floor supplies the integer
 * part and datetime_decodeTime() rounds its positive day fraction.
 * @return Sum of integer-day parts and encoded component seconds.
 * @note This preserves legacy decompose/recompose rounding rather than
 * performing a direct floating-point addition.
 */
DateTime datetime_addDays(DateTime date1, DateTime date2)

//  Input:   date1 = an encoded date/time value
//           date2 = decimal days to be added to date1
//  Output:  returns date1 + date2
//  Purpose: adds a given number of decimal days to a date/time.

{
    double d1 = floor(date1);
    double d2 = floor(date2);
    int h1, m1, s1;
    int h2, m2, s2;
    datetime_decodeTime(date1, &h1, &m1, &s1);
    datetime_decodeTime(date2, &h2, &m2, &s2);
    return d1 + d2 + datetime_encodeTime(h1 + h2, m1 + m2, s1 + s2);
}

//=============================================================================

/**
 * @brief Subtract two dates using separately rounded whole-second clock values.
 * @par Implementation arguments
 * - date1 (in): Finite minuend in SWMM calendar days.
 * - date2 (in): Finite subtrahend in SWMM calendar days.
 * @return Signed difference in seconds; negative when date1 is earlier.
 * @pre The whole-day difference times 86400 fits in int, because the legacy
 * implementation casts that intermediate to int, and the final result fits in long.
 */
long datetime_timeDiff(DateTime date1, DateTime date2)

//  Input:   date1 = an encoded date/time value
//           date2 = an encoded date/time value
//  Output:  returns date1 - date2 in seconds
//  Purpose: finds number of seconds between two dates.

{
    double d1 = floor(date1);
    double d2 = floor(date2);
    int h, m, s;
    long s1, s2, secs;
    datetime_decodeTime(date1, &h, &m, &s);
    s1 = 3600 * h + 60 * m + s;
    datetime_decodeTime(date2, &h, &m, &s);
    s2 = 3600 * h + 60 * m + s;
    secs = (int)(floor((d1 - d2) * SecsPerDay + 0.5));
    secs += (s1 - s2);
    return secs;
}

//=============================================================================

/**
 * @brief Extract the month from a SWMM DateTime.
 * @par Implementation arguments
 * - date (in): Finite SWMM calendar days for a date in [0001-01-01, 9999-12-31].
 * @return One-based Gregorian month in [1, 12].
 * @note Uses legacy floor/whole-second decoding conventions; no state is changed.
 */
int datetime_monthOfYear(DateTime date)

//  Input:   date = an encoded date/time value
//  Output:  returns index of month of year (1..12)
//  Purpose: finds month of year (Jan = 1 ...) for a given date.

{
    int year, month, day;
    datetime_decodeDate(date, &year, &month, &day);
    return month;
}

//=============================================================================

/**
 * @brief Extract the day of year from a SWMM DateTime.
 * @par Implementation arguments
 * - date (in): Finite SWMM calendar days for a date in [0001-01-01, 9999-12-31].
 * @return One-based day in [1, 365] or [1, 366] for leap years.
 * @note Uses legacy floor/whole-second decoding conventions; no state is changed.
 */
int datetime_dayOfYear(DateTime date)

//  Input:   date = an encoded date/time value
//  Output:  returns day of year (1..365)
//  Purpose: finds day of year (Jan 1 = 1) for a given date.

{
    int year, month, day;
    DateTime startOfYear;
    datetime_decodeDate(date, &year, &month, &day);
    startOfYear = datetime_encodeDate(year, 1, 1);
    return (int)(floor(date - startOfYear)) + 1;
}

//=============================================================================

/**
 * @brief Extract the day of week from a SWMM DateTime.
 * @par Implementation arguments
 * - date (in): Finite SWMM calendar days for a date in [0001-01-01, 9999-12-31].
 * @return Sunday = 1 through Saturday = 7 for supported Gregorian dates.
 * @note Uses legacy floor/whole-second decoding conventions; no state is changed.
 */
int datetime_dayOfWeek(DateTime date)

//  Input:   date = an encoded date/time value
//  Output:  returns index of day of week (1..7)
//  Purpose: finds day of week (Sun = 1, ... Sat = 7) for a given date.

{
    int t = (int)(floor(date)) + DateDelta;
    return (t % 7) + 1;
}

//=============================================================================

/**
 * @brief Extract the hour from a SWMM DateTime.
 * @par Implementation arguments
 * - date (in): Finite SWMM calendar days for a date in [0001-01-01, 9999-12-31].
 * @return Whole-second decoded hour in [0, 23].
 * @note Uses legacy floor/whole-second decoding conventions; no state is changed.
 */
int datetime_hourOfDay(DateTime date)

//  Input:   date = an encoded date/time value
//  Output:  returns hour of day (0..23)
//  Purpose: finds hour of day (0 = 12 AM, ..., 23 = 11 PM) for a given date.

{
    int hour, min, sec;
    datetime_decodeTime(date, &hour, &min, &sec);
    return hour;
}

//=============================================================================

/**
 * @brief Find a Gregorian month's length using the leap-year rule.
 * @par Implementation arguments
 * - year (in): Calendar year to classify for leap-year behavior.
 * - month (in): One-based month in [1, 12].
 * @return Month length in days, or zero for an out-of-range month.
 */
int datetime_daysPerMonth(int year, int month)

//  Input:   year = year in which month falls
//           month = month of year (1..12)
//  Output:  returns number of days in the month
//  Purpose: finds number of days in a given month of a specified year.

{
    if (month < 1 || month > 12)
        return 0;
    return DaysPerMonth[isLeapYear(year)][month - 1];
}

//=============================================================================

/**
 * @brief Format SWMM calendar days as a date and whole-second time stamp.
 * @par Implementation arguments
 * - fmt (in): Date-format code accepted by datetime_setDateFormat().
 * - aDate (in): SWMM DateTime in days relative to 1899-12-30.
 * - stampSize (in): Writable capacity in bytes; values below TIME_STAMP_SIZE
 * leave the destination unchanged.
 * - timeStamp (out): Non-null caller-owned character buffer of stampSize
 * bytes when formatting occurs; receives a null-terminated date/time string.
 * @note Temporarily changes and then restores the global DateFormat. The
 * shared date formatter is not safe for concurrent calls without coordination.
 */
void datetime_getTimeStamp(int fmt, DateTime aDate, int stampSize, char* timeStamp)

//  Input:   fmt = desired date format code
//           aDate = a date/time value in decimal days
//           stampSize = the number of bytes allocated for the time stamp
//  Output:  returns a time stamp string (e.g., Year-Month-Day Hr:Min:Sec)
//  Purpose: Expresses a decimal day date by a time stamp.
{
    char dateStr[DATE_STR_SIZE];
    char timeStr[TIME_STR_SIZE];
    int oldDateFormat = DateFormat;

    if (stampSize < TIME_STAMP_SIZE)
        return;
    datetime_setDateFormat(fmt);
    datetime_dateToStr(aDate, dateStr);
    DateFormat = oldDateFormat;
    datetime_timeToStr(aDate, timeStr);
    snprintf(timeStamp, stampSize, "%s %s", dateStr, timeStr);
}
