/**
 * @file InputParseUtils.hpp
 * @brief Shared parsing utilities for input section handlers.
 *
 * @details Consolidates duplicated helper functions (to_double, to_int,
 *          parse_date, parse_time) that were previously defined as static
 *          functions in each handler .cpp file.
 *
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
 * @brief Parse a double from a string_view, returning a default on failure.
 */
inline double to_double(std::string_view sv, double def = 0.0) noexcept {
    double v = def;
    openswmm::from_chars_double(sv.data(), sv.data() + sv.size(), v);
    return v;
}

/**
 * @brief Parse an int from a string_view, returning a default on failure.
 */
inline int to_int(std::string_view sv, int def = 0) noexcept {
    int v = def;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{}) return def;
    return v;
}

// ============================================================================
// Date/time parsing
// ============================================================================

/**
 * @brief Parse a date string in MM/DD/YYYY format to a DateTime (decimal days).
 */
inline double parse_date(std::string_view sv) {
    unsigned m = 0, d = 0, y = 0;
    const char* p = sv.data();
    const char* end = sv.data() + sv.size();

    auto read_uint = [&](unsigned& out) -> bool {
        auto [np, ec] = std::from_chars(p, end, out);
        if (ec != std::errc{}) return false;
        p = np;
        return true;
    };

    if (!read_uint(m)) return 0.0;
    if (p < end && *p == '/') ++p; else return 0.0;
    if (!read_uint(d)) return 0.0;
    if (p < end && *p == '/') ++p; else return 0.0;
    if (!read_uint(y)) return 0.0;

    return datetime::encodeDate(static_cast<int>(y),
                                static_cast<int>(m),
                                static_cast<int>(d));
}

/**
 * @brief Parse a time string to seconds.
 *
 * @details Accepts:
 *   - HH:MM:SS → hours*3600 + min*60 + sec
 *   - HH:MM    → hours*3600 + min*60
 *   - N        → N (plain seconds, floating point)
 */
inline double parse_time_seconds(std::string_view sv) {
    // Try plain number first
    double val = 0.0;
    auto [ptr, ec] = openswmm::from_chars_double(sv.data(), sv.data() + sv.size(), val);
    if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
        return val;  // plain number = seconds
    }

    // Try HH:MM:SS or HH:MM
    unsigned h = 0, m = 0;
    double s = 0.0;
    const char* p = sv.data();
    const char* end = sv.data() + sv.size();

    auto read_uint = [&](unsigned& out) -> bool {
        auto [np, nec] = std::from_chars(p, end, out);
        if (nec != std::errc{}) return false;
        p = np;
        return true;
    };

    if (!read_uint(h)) return 0.0;
    if (p < end && *p == ':') {
        ++p;
        if (!read_uint(m)) return static_cast<double>(h);
        if (p < end && *p == ':') {
            ++p;
            openswmm::from_chars_double(p, end, s);
        }
    }
    return h * 3600.0 + m * 60.0 + s;
}

/**
 * @brief Parse a time string HH:MM:SS to a fractional day (DateTime).
 *
 * @details For use with datetime::encodeTime. Returns fractional day [0, 1).
 */
inline double parse_time_day_fraction(std::string_view sv) {
    unsigned h = 0, m = 0, s = 0;
    const char* p = sv.data();
    const char* end = sv.data() + sv.size();

    auto read_uint = [&](unsigned& out) -> bool {
        auto [np, ec] = std::from_chars(p, end, out);
        if (ec != std::errc{}) return false;
        p = np;
        return true;
    };

    if (!read_uint(h)) return 0.0;
    if (p < end && *p == ':') ++p;
    read_uint(m);
    if (p < end && *p == ':') ++p;
    read_uint(s);

    return datetime::encodeTime(static_cast<int>(h),
                                static_cast<int>(m),
                                static_cast<int>(s));
}

/**
 * @brief Parse a combined date + time pair to a DateTime.
 *
 * @param date_sv  Date string in MM/DD/YYYY format.
 * @param time_sv  Time string in HH:MM:SS format.
 * @returns        DateTime value (decimal days).
 */
inline double parse_datetime(std::string_view date_sv, std::string_view time_sv) {
    return parse_date(date_sv) + parse_time_day_fraction(time_sv);
}

// ============================================================================
// Object registration
// ============================================================================

/**
 * @brief Register a new object definition, rejecting duplicates like legacy.
 *
 * @details Legacy input.c addObject() emits ERR_DUP_NAME (207) when an object
 *          class that defines one object per line sees a second definition of
 *          the same name — and its hash table is case-insensitive, so names
 *          differing only in case collide too. Multi-line object classes
 *          (timeseries, curves, patterns, snowpacks, LID controls, inlets,
 *          unit hydrographs) legitimately repeat their name across lines and
 *          must keep using find-then-add instead of this helper.
 *
 * @param reg     Name registry for the object class.
 * @param name    Name from the definition line (stored spelling).
 * @param errors  ctx.errors sink for the ERR_DUP_NAME message.
 * @returns       New index, or -1 if the name was already registered (the
 *                error has been pushed; caller should skip the line).
 */
inline int add_unique(NameIndex& reg, const std::string& name,
                      std::vector<std::string>& errors) {
    const int idx = reg.try_add(name);
    if (idx < 0) {
        const std::string* canon = reg.canonical(name);
        if (canon != nullptr && *canon != name) {
            errors.push_back(format_error(ERR_DUP_NAME, name,
                "(matches existing ID '" + *canon +
                "'; names are case-insensitive)"));
        } else {
            errors.push_back(format_error(ERR_DUP_NAME, name));
        }
    }
    return idx;
}

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_INPUT_PARSE_UTILS_HPP */
