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
 * @file openswmm_datetime_impl.cpp
 * @brief C API implementation — SWMM DateTime encode/decode utilities.
 *
 * @details Thin wrappers around the inline routines in DateTime.hpp so the
 *          same numerics used inside the engine are reachable from C callers
 *          and language bindings.
 *
 * @see include/openswmm/engine/openswmm_datetime.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "DateTime.hpp"
#include "../../../include/openswmm/engine/openswmm_datetime.h"

extern "C" {

SWMM_ENGINE_API int swmm_datetime_encode_date(int year, int month, int day,
                                              double* out) {
    if (!out) return -1;
    *out = openswmm::datetime::encodeDate(year, month, day);
    /* encodeDate returns -DateDelta as the sentinel for invalid input. */
    return (*out == static_cast<double>(-openswmm::datetime::DateDelta)) ? -1 : 0;
}

SWMM_ENGINE_API int swmm_datetime_encode_time(int hour, int minute, int second,
                                              double* out) {
    if (!out) return -1;
    if (hour < 0 || minute < 0 || second < 0) {
        *out = 0.0;
        return -1;
    }
    *out = openswmm::datetime::encodeTime(hour, minute, second);
    return 0;
}

SWMM_ENGINE_API int swmm_datetime_decode_date(double value,
                                              int* year, int* month, int* day) {
    if (!year && !month && !day) return -1;
    int y = 0, m = 0, d = 0;
    openswmm::datetime::decodeDate(value, y, m, d);
    if (year)  *year  = y;
    if (month) *month = m;
    if (day)   *day   = d;
    return 0;
}

SWMM_ENGINE_API int swmm_datetime_decode_time(double value,
                                              int* hour, int* minute, int* second) {
    if (!hour && !minute && !second) return -1;
    int h = 0, mi = 0, s = 0;
    openswmm::datetime::decodeTime(value, h, mi, s);
    if (hour)   *hour   = h;
    if (minute) *minute = mi;
    if (second) *second = s;
    return 0;
}

SWMM_ENGINE_API int swmm_datetime_add_seconds(double value, double seconds,
                                              double* out) {
    if (!out) return -1;
    *out = openswmm::datetime::addSeconds(value, seconds);
    return 0;
}

SWMM_ENGINE_API int swmm_datetime_time_diff(double value1, double value2,
                                            long* out) {
    if (!out) return -1;
    *out = openswmm::datetime::timeDiff(value1, value2);
    return 0;
}

} /* extern "C" */
