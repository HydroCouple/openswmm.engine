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
 * @file      InputParseUtils.hpp
 * @brief     Shared parsing utilities for input section handlers.
 *
 * @details Consolidates duplicated helper functions (to_double, to_int,
 *          parse_date, parse_time) that were previously defined as static
 *          functions in each handler .cpp file.
 *
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_INPUT_PARSE_UTILS_HPP
#define OPENSWMM_ENGINE_INPUT_PARSE_UTILS_HPP

#include <string_view>
#include <string>
#include <vector>
#include <charconv>
#include "../core/charconv_compat.hpp"
#include "../core/DateTime.hpp"
#include "../core/ErrorCodes.hpp"
#include "../data/NameIndex.hpp"

namespace openswmm::input {

// ============================================================================
// Numeric parsing
// ============================================================================

/**
 * @brief Parse a numeric prefix, retaining a supplied default when conversion fails.
 *
 * Not a whole-token validator; leading whitespace/plus and overflow semantics depend on
 * from_chars_double() backend.
 *
 * @param[in] sv Borrowed character range; trailing nonnumeric bytes are accepted.
 * @param[in] def Initial result returned if the backend leaves its destination unchanged.
 * @return Parsed numeric prefix or default; conversion status and final pointer are intentionally
 *     ignored.
 */
inline double to_double(std::string_view sv, double def = 0.0) noexcept {
    double v = def;
    openswmm::from_chars_double(sv.data(), sv.data() + sv.size(), v);
    return v;
}

/**
 * @brief Parse a complete token while preserving the output on refusal.
 *
 * Does not independently reject NaN/infinity. Whitespace/backend edge cases follow
 * from_chars_double(); use a separate finite/range check when required.
 *
 * @param[in] sv Borrowed token; empty is explicitly accepted as zero. A single leading plus is
 *     stripped before backend conversion.
 * @param[out] out Written only on success; preserved on partial/failed conversion.
 * @return True after complete conversion or the empty-token zero case; false otherwise.
 */
inline bool parse_double_strict(std::string_view sv, double& out) noexcept {
    if (sv.empty()) {
        out = 0.0;
        return true;
    }
    if (sv.front() == '+') { // strtod accepts a leading '+'
        sv.remove_prefix(1);
        if (sv.empty() || sv.front() == '+' || sv.front() == '-')
            return false;
    }
    double v = 0.0;
    const auto [ptr, ec] = openswmm::from_chars_double(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{} || ptr != sv.data() + sv.size())
        return false;
    out = v;
    return true;
}

/**
 * @brief Parse a signed integer prefix using decimal from_chars.
 *
 * Does not trim whitespace or accept a leading plus; not a complete-token validator.
 *
 * @param[in] sv Borrowed token; trailing bytes are ignored after a valid integer prefix.
 * @param[in] def Fallback returned for invalid/out-of-range conversion.
 * @return Parsed integer prefix, or def if from_chars reports an error.
 */
inline int to_int(std::string_view sv, int def = 0) noexcept {
    int v = def;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{})
        return def;
    return v;
}

// ============================================================================
// Date/time parsing
// ============================================================================

/**
 * @brief Parse month/day/year or month-day-year input into SWMM decimal days.
 *
 * Reads numeric prefixes without requiring end-of-token after year. The two failure sentinels are
 * historical behavior, not an optional/error result.
 *
 * @param[in] sv Borrowed numeric date; each separator may independently be slash or hyphen.
 * @return Encoded date relative to 1899-12-30; 0 for syntactic parse failure, or
 *     -datetime::DateDelta for an invalid Gregorian date.
 */
inline double parse_date(std::string_view sv) {
    unsigned m = 0, d = 0, y = 0;
    const char* p = sv.data();
    const char* end = sv.data() + sv.size();

    auto read_uint = [&](unsigned& out) -> bool {
        auto [np, ec] = std::from_chars(p, end, out);
        if (ec != std::errc{})
            return false;
        p = np;
        return true;
    };
    auto read_sep = [&]() -> bool {
        if (p < end && (*p == '/' || *p == '-')) {
            ++p;
            return true;
        }
        return false;
    };

    if (!read_uint(m))
        return 0.0;
    if (!read_sep())
        return 0.0;
    if (!read_uint(d))
        return 0.0;
    if (!read_sep())
        return 0.0;
    if (!read_uint(y))
        return 0.0;

    return datetime::encodeDate(static_cast<int>(y), static_cast<int>(m), static_cast<int>(d));
}

/**
 * @brief Parse a duration as decimal seconds or permissive hour/minute/second fields.
 *
 * Does not enforce clock-field bounds or complete consumption in the colon path. With no colon, a
 * partial numeric-hours prefix is multiplied by 3600. Callers needing strict validation must
 * validate separately.
 *
 * @param[in] sv Borrowed input: complete numeric token means seconds; otherwise reads unsigned
 *     hours/minutes and optional floating seconds.
 * @return Seconds; zero when no leading hours parse. A failed minute parse after a colon returns
 *     the numeric hour value without multiplying it by 3600.
 */
inline double parse_time_seconds(std::string_view sv) {
    // Try plain number first
    double val = 0.0;
    auto [ptr, ec] = openswmm::from_chars_double(sv.data(), sv.data() + sv.size(), val);
    if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
        return val; // plain number = seconds
    }

    // Try HH:MM:SS or HH:MM
    unsigned h = 0, m = 0;
    double s = 0.0;
    const char* p = sv.data();
    const char* end = sv.data() + sv.size();

    auto read_uint = [&](unsigned& out) -> bool {
        auto [np, nec] = std::from_chars(p, end, out);
        if (nec != std::errc{})
            return false;
        p = np;
        return true;
    };

    if (!read_uint(h))
        return 0.0;
    if (p < end && *p == ':') {
        ++p;
        if (!read_uint(m))
            return static_cast<double>(h);
        if (p < end && *p == ':') {
            ++p;
            openswmm::from_chars_double(p, end, s);
        }
    }
    return h * 3600.0 + m * 60.0 + s;
}

/**
 * @brief Parse decimal hours or permissive clock fields into elapsed days.
 *
 * Clock path tolerates missing minute/second fields and trailing text; no [0,1) clamp. Parsed clock
 * components must satisfy encodeTime() integer representability requirements.
 *
 * @param[in] sv Borrowed token: complete decimal number is hours/24; otherwise unsigned
 *     HH[:MM[:SS]] fields are parsed.
 * @return Elapsed-day value, possibly >=1 or negative for decimal-hour inputs; zero when no leading
 *     integer hour parses.
 */
inline double parse_time_day_fraction(std::string_view sv) {
    const char* p = sv.data();
    const char* end = sv.data() + sv.size();

    // A time token is EITHER decimal hours (a bare number such as "0.167" or
    // "10") OR a clock string "HH:MM[:SS]" — matching legacy datetime_strToTime
    // ("accepts time as hr:min:sec or as decimal hours"). Try a full decimal
    // parse first: if the WHOLE token is a number it is decimal hours (/24);
    // otherwise fall back to integer HH:MM:SS. This helper (and parse_datetime
    // through it) previously handled only HH:MM:SS, so a bare "0.167" on a dated
    // [TIMESERIES] row was truncated to 0 — corrupting the series and colliding
    // with the prior day's "24" row (dx == 0 -> spurious ERR 173).
    double dec_hours = 0.0;
    auto [dp, dec] = openswmm::from_chars_double(p, end, dec_hours);
    if (dec == std::errc{} && dp == end)
        return dec_hours / 24.0;

    unsigned h = 0, m = 0, s = 0;
    auto read_uint = [&](unsigned& out) -> bool {
        auto [np, ec] = std::from_chars(p, end, out);
        if (ec != std::errc{})
            return false;
        p = np;
        return true;
    };

    if (!read_uint(h))
        return 0.0;
    if (p < end && *p == ':')
        ++p;
    read_uint(m);
    if (p < end && *p == ':')
        ++p;
    read_uint(s);

    return datetime::encodeTime(static_cast<int>(h), static_cast<int>(m), static_cast<int>(s));
}

/**
 * @brief Combine the permissive date and decimal-hour/clock parsers.
 *
 * @param[in] date_sv Borrowed date accepted by parse_date(), including its historical failure
 *     behavior.
 * @param[in] time_sv Borrowed decimal hours or clock token accepted by parse_time_day_fraction().
 * @return Sum of both parser results in SWMM decimal days; no independent combined validity check.
 */
inline double parse_datetime(std::string_view date_sv, std::string_view time_sv) {
    return parse_date(date_sv) + parse_time_day_fraction(time_sv);
}

// ============================================================================
// Object registration
// ============================================================================

/**
 * @brief Register a one-definition object name or append a duplicate diagnostic.
 *
 * Use only for one-definition object classes; repeated-row timeseries/curves/patterns require
 * find-then-add instead. Exceptions from allocation propagate.
 *
 * @param[in,out] reg Mutable case-insensitive registry; new names retain their spelling.
 * @param[in] name Copied authored ID; collisions differing only by ASCII case are duplicates.
 * @param[in,out] errors Caller-owned sink receiving formatted ERR_DUP_NAME for a collision.
 * @return New object index, or -1 after recording duplicate-name error.
 */
inline int add_unique(NameIndex& reg, const std::string& name, std::vector<std::string>& errors) {
    const int idx = reg.try_add(name);
    if (idx < 0) {
        const std::string* canon = reg.canonical(name);
        if (canon != nullptr && *canon != name) {
            errors.push_back(
                format_error(ERR_DUP_NAME, name,
                             "(matches existing ID '" + *canon + "'; names are case-insensitive)"));
        } else {
            errors.push_back(format_error(ERR_DUP_NAME, name));
        }
    }
    return idx;
}

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_INPUT_PARSE_UTILS_HPP */
