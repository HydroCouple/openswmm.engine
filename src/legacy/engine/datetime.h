/**
 * @file      datetime.h
 * @brief     Header file for date and time functions.
 * @author    L. Rossman
 * @date      Created: 2021-11-01
 * @date      Last updated: 2024-12-30
 * @version 5.3.0
 * @details
 * The DateTime type is used to store date and time values. It is
 * equivalent to a double floating point type.
 *
 * The integral part of a DateTime value is the number of days that have
 * passed since 12/31/1899. The fractional part of a DateTime value is the
 * fraction of a 24 hour day that has elapsed.
 *
 * Update History
 * ==============
 * Build 5.1.011:
 * - New getTimeStamp function added.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com> (OpenSWMM maintenance and extensions)
 * @copyright Copyright (c) 2026 Caleb Buahin. OpenSWMM contributions only; original EPA SWMM
 *            credits are retained.
 * @license   Apache-2.0 for OpenSWMM contributions; original EPA SWMM provenance is retained.
 */

#ifndef DATETIME_H
#define DATETIME_H

/*!
* \typedef DateTime
* \brief Type for storing date and time values.
*/
typedef double DateTime;

/*!
* \def Y_M_D
* \brief Date format: year, month, day
*/
#define Y_M_D 0

/*!
* \def M_D_Y
* \brief Date format: month, day, year
*/
#define M_D_Y 1

/*!
* \def D_M_Y
* \brief Date format: day, month, year
*/
#define D_M_Y 2

/*!
* \def NO_DATE
* \brief No date value
*/
#define NO_DATE -693594 // 1/1/0001

/*!
* \def DATE_STR_SIZE
* \brief Size of a date string
*/
#define DATE_STR_SIZE 12

/*!
* \def TIME_STR_SIZE
* \brief Size of a time string
*/
#define TIME_STR_SIZE 9

/*!
* \def TIME_STAMP_SIZE
* \brief Size of a time stamp string
*/
#define TIME_STAMP_SIZE 21

/*!
* \brief Encodes a date values from year, month, and day to a DateTime value.
* \param[in] year Year
* \param[in] month Month
* \param[in] day Day
* \return DateTime value
*/
DateTime datetime_encodeDate(int year, int month, int day);

/*!
* \brief Encodes a time value from hour, minute, and second to a DateTime value.
* \param[in] hour Hour
* \param[in] minute Minute
* \param[in] second Second
* \return DateTime value
*/
DateTime datetime_encodeTime(int hour, int minute, int second);

/*!
* \brief Decodes a DateTime value to year, month, and day.
* @par Argument contract
 * - date (in): Date value
* - year (out): Year
* - month (out): Month
* - day (out): Day
*/
void datetime_decodeDate(DateTime date, int* y, int* m, int* d);

/*!
* \brief Decodes a DateTime value to hour, minute, and second.
* \param[in] time Time value
* \param[out] h Hour
* \param[out] m Minute
* \param[out] s Second
*/
void datetime_decodeTime(DateTime time, int* h, int* m, int* s);

/*!
* \brief Finds the month of the year for a date.
* \param[in] date Date value
* \return Month of year
*/
int datetime_monthOfYear(DateTime date);

/*!
* \brief Finds the day of the year for a date.
* \param[in] date Date value
* \return Day of year
*/
int datetime_dayOfYear(DateTime date);

/*!
* \brief Finds the day of the week for a date.
* \param[in] date Date value
* \return Day of week (0 = Sunday, 1 = Monday, ..., 6 = Saturday)
*/
int datetime_dayOfWeek(DateTime date);

/*!
* \brief Finds the hour of the day for a time.
* \param[in] date Time value
* \return Hour of day
*/
int datetime_hourOfDay(DateTime date);

/*!
* \brief Finds the number of days in a month.
* \param[in] year Year
* \param[in] month Month
* \return Number of days in the month
*/
int datetime_daysPerMonth(int year, int month);

/*!
* \brief Converts a DateTime value to a string.
* \param[in] date Date value
* \param[out] s String to store the date
*/
void datetime_dateToStr(DateTime date, char* s);

/*!
* \brief Converts a DateTime value to a string.
* \param[in] time Time value
* \param[out] s String to store the time
*/
void datetime_timeToStr(DateTime time, char* s);

/**
 * @brief Format a calendar date and whole-second clock time in caller storage.
 * @param[in] fmt Date format code accepted by datetime_setDateFormat().
 * @param[in] aDate SWMM calendar days relative to 1899-12-30.
 * @param[in] stampSize Writable byte capacity; below TIME_STAMP_SIZE leaves the buffer untouched.
 * @param[out] timeStamp Caller-owned buffer with stampSize writable bytes when formatting occurs.
 * @note Temporarily selects and restores the global DateFormat. Output is
 * null-terminated and storage is not retained; shared formatting is not thread-safe.
 */
void datetime_getTimeStamp(int fmt, DateTime aDate, int stampSize, char* timeStamp);

/*!
* \brief Finds the month of the year for a date.
* @par Argument contract
 * - month (in): String date
* \return Month of year
*/
int datetime_findMonth(char* s);

/*!
* \brief Converts a string to a DateTime value.
* \param[in] s String date
* \param[out] d DateTime value
* \return 1 if successful, 0 if not
*/
int datetime_strToDate(char* s, DateTime* d);

/*!
* \brief Converts a string to a DateTime value.
* \param[in] s String time
* \param[out] t DateTime value
* \return 1 if successful, 0 if not
*/
int datetime_strToTime(char* s, DateTime* t);

/*!
* \brief Sets the date format.
* \param[in] fmt Date format
*/
void datetime_setDateFormat(int fmt);

/*!
* \brief Adds seconds to a DateTime value.
* \param[in] date1 Date value
* \param[in] seconds Number of seconds to add
* \return New DateTime value
*/
DateTime datetime_addSeconds(DateTime date1, double seconds);

/*!
* \brief Adds days to a DateTime value.
* \param[in] date1 Date value
* \param[in] date2 Number of days to add
* \return New DateTime value
*/
DateTime datetime_addDays(DateTime date1, DateTime date2);

/*!
* \brief Finds the difference in seconds between two DateTime values.
* \param[in] date1 First date value
* \param[in] date2 Second date value
* \return Number of seconds
*/
long datetime_timeDiff(DateTime date1, DateTime date2);

#endif //DATETIME_H
