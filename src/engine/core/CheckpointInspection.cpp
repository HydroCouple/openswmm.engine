// SPDX-License-Identifier: Apache-2.0

/**
 * @file      CheckpointInspection.cpp
 * @brief     Validated checkpoint schemas used to bind compatible fresh sessions.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "CheckpointInspection.hpp"
#include <locale>
#include <sstream>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace openswmm {
bool detail::checkpointUtf8(std::string_view text) noexcept {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80)
            continue;
        unsigned n = 0, code = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            n = 1;
            code = c & 31;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 2;
            code = c & 15;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 3;
            code = c & 7;
            minimum = 0x10000;
        } else
            return false;
        if (n > text.size() - i)
            return false;
        while (n--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (next & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}
namespace {
/**
 * @brief Encode validated UTF-8 bytes as a JSON string literal.
 *
 * Throws logic_error for invalid UTF-8; allocation can throw. Does not normalize Unicode or escape
 * all non-ASCII bytes.
 *
 * @param[in] text Borrowed UTF-8 sequence; embedded controls/NUL are escaped.
 * @return Owned quoted JSON string; quote/backslash escape and controls use lowercase u00xx.
 */
std::string quoted(std::string_view text) {
    if (!detail::checkpointUtf8(text))
        throw std::logic_error("Checkpoint metadata contains invalid UTF-8.");
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += char(c);
    }
    return out + '"';
}
/**
 * @brief Encode a finite double as a locale-independent JSON numeric token.
 *
 * Throws logic_error for nonfinite values/stream failure; allocation can throw.
 *
 * @param[in] value Finite scalar in the metadata field's existing units.
 * @return Owned classic-locale text at max_digits10 precision.
 */
std::string number(double value) {
    if (!std::isfinite(value))
        throw std::logic_error("Checkpoint metadata contains a nonfinite number.");
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(std::numeric_limits<double>::max_digits10);
    out << value;
    if (!out)
        throw std::logic_error("Cannot serialize checkpoint metadata number.");
    return out.str();
}
/**
 * @brief Encode an optional finite metadata number or JSON null.
 *
 * @param[in] value Borrowed optional; present double must be finite.
 * @return Owned numeric token or literal null; number() exceptions propagate.
 */
std::string optional(const std::optional<double>& value) {
    return value ? number(*value) : "null";
}
/**
 * @brief Encode an unsigned64-bit count as a JSON decimal string.
 *
 * @param[in] value Count/byte quantity, preserved without JSON floating-point precision loss.
 * @return Owned quoted decimal digits, not an unquoted JSON number.
 */
std::string count(uint64_t value) {
    return quoted(std::to_string(value));
}
/**
 * @brief Encode one boolean as a JSON literal.
 *
 * @param[in] value Flag to serialize.
 * @return Owned true or false token.
 */
std::string boolean(bool value) {
    return value ? "true" : "false";
}
struct Object {
    std::string text = "{";
    /**
     * @brief Append a comma-delimited JSON member to an unfinished object.
     *
     * Mutates owned text; call before finish. Allocation can leave a partially appended object;
     * duplicate keys are allowed.
     *
     * @param[in] name Non-NULL NUL-terminated valid UTF-8 key; escaped by quoted().
     * @param[in] value Borrowed complete JSON value token, appended verbatim without validation.
     */
    void field(const char* name, const std::string& value) {
        if (text.size() > 1)
            text += ',';
        text += quoted(name) + ':' + value;
    }
    /**
     * @brief Append the object closing brace and move out its owned text.
     * @return Owned JSON object string; this builder is consumed and should not be finished/reused
     *     again.
     */
    std::string finish() {
        text += '}';
        return std::move(text);
    }
};
/**
 * @brief Encode inspected report metadata as a JSON object.
 *
 * No validation of record/date consistency beyond numeric serialization.
 *
 * @param[in] r Borrowed coverage; cadence and present dates must be finite, dates retain OADate
 *     units.
 * @return Owned enabled/records/cadence_seconds/first_date/last_date object; count is a decimal
 *     string.
 */
std::string reports(const CheckpointInspection::Reports& r) {
    Object out;
    out.field("enabled", boolean(r.enabled));
    out.field("records", count(r.records));
    out.field("cadence_seconds", number(r.cadence_seconds));
    out.field("first_date", optional(r.first_date));
    out.field("last_date", optional(r.last_date));
    return out.finish();
}
}
std::string CheckpointInspection::json() const {
    if (schema_version != 1)
        throw std::logic_error("Unsupported checkpoint inspection schema.");
    Object out;
    out.field("schema_version", "1");
    out.field("engine_seconds", number(engine_seconds));
    out.field("engine_date", number(engine_date));
    out.field("start_date", number(start_date));
    out.field("report_start_date", number(report_start_date));
    out.field("next_report_seconds", number(next_report_seconds));
    out.field("checkpoint_bytes", count(checkpoint_bytes));
    out.field("completed_source_seconds", optional(completed_source_seconds));
    std::string driver_json = "null";
    if (driver) {
        Object d;
        d.field("identity", quoted(driver->identity));
        d.field("next_sequence", count(driver->next_sequence));
        d.field("next_update_seconds", number(driver->next_update_seconds));
        driver_json = d.finish();
    }
    out.field("driver", driver_json);
    out.field("binary", reports(binary));
    out.field("mesh", reports(mesh));
    out.field("file_outputs", boolean(file_outputs));
    out.field("relocatable_bundle", boolean(relocatable_bundle));
    out.field("detailed_sources", boolean(detailed_sources));
    out.field("source_frames", count(source_frames));
    out.field("source_prefix_bytes", count(source_prefix_bytes));
    std::string files = "[";
    for (const auto& c : companions) {
        const char* role = nullptr;
        switch (c.role) {
        case Companion::Role::Source:
            role = "source";
            break;
        case Companion::Role::Binary:
            role = "binary";
            break;
        case Companion::Role::Mesh:
            role = "mesh";
            break;
        }
        if (!role)
            throw std::logic_error("Invalid checkpoint companion role.");
        Object file;
        file.field("role", quoted(role));
        file.field("path", quoted(c.path));
        file.field("required_bytes", count(c.required_bytes));
        if (files.size() > 1)
            files += ',';
        files += file.finish();
    }
    out.field("companions", files + ']');
    return out.finish();
}
}
