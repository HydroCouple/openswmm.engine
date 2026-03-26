/**
 * @file Tokenizer.cpp
 * @brief Implementation of the multi-delimiter SWMM input tokenizer.
 *
 * @see Tokenizer.hpp for interface documentation.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Tokenizer.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace openswmm::input {

// ============================================================================
// strip_comment
// ============================================================================

std::string_view Tokenizer::strip_comment(std::string_view line) noexcept {
    bool in_quote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            in_quote = !in_quote;
        } else if (c == ';' && !in_quote) {
            return line.substr(0, i);
        }
    }
    return line;
}

// ============================================================================
// tokenize
// ============================================================================

std::vector<std::string> Tokenizer::tokenize(std::string_view line) {
    std::string_view stripped = strip_comment(line);

    std::vector<std::string> tokens;
    std::size_t i = 0;
    const std::size_t n = stripped.size();

    while (i < n) {
        // Skip leading delimiters (spaces/tabs only — commas are explicit separators)
        while (i < n && (stripped[i] == ' ' || stripped[i] == '\t')) {
            ++i;
        }
        if (i >= n) break;

        if (stripped[i] == ',') {
            // Comma separator: produces an empty token if nothing was before it
            // but we treat consecutive commas as empty tokens (CSV semantics)
            tokens.emplace_back();
            ++i;
            continue;
        }

        if (stripped[i] == '"') {
            // Quoted string: collect until closing quote
            ++i;  // skip opening quote
            std::string tok;
            while (i < n && stripped[i] != '"') {
                tok += stripped[i++];
            }
            if (i < n) ++i;  // skip closing quote
            tokens.push_back(std::move(tok));

            // After a quoted token, skip trailing whitespace then handle optional comma
            while (i < n && (stripped[i] == ' ' || stripped[i] == '\t')) ++i;
            if (i < n && stripped[i] == ',') ++i;
            continue;
        }

        // Regular token: read until next delimiter
        std::size_t start = i;
        while (i < n && !is_delimiter(stripped[i])) {
            ++i;
        }
        tokens.emplace_back(stripped.substr(start, i - start));

        // If ended on a comma, skip it (and keep going — next iteration handles
        // the space-skip at the start of the next token)
        if (i < n && stripped[i] == ',') {
            ++i;
        }
    }

    return tokens;
}

// ============================================================================
// tokenize_views
// ============================================================================

std::vector<std::string_view> Tokenizer::tokenize_views(std::string_view line) {
    std::string_view stripped = strip_comment(line);

    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    const std::size_t n = stripped.size();

    while (i < n) {
        // Skip spaces and tabs
        while (i < n && (stripped[i] == ' ' || stripped[i] == '\t')) ++i;
        if (i >= n) break;

        if (stripped[i] == ',') {
            ++i;
            continue;
        }

        // Regular token (quoted tokens not supported — caller must use tokenize())
        std::size_t start = i;
        while (i < n && !is_delimiter(stripped[i])) ++i;
        if (i > start) {
            tokens.push_back(stripped.substr(start, i - start));
        }
        if (i < n && stripped[i] == ',') ++i;
    }

    return tokens;
}

// ============================================================================
// String utilities
// ============================================================================

void Tokenizer::to_upper_inplace(std::string& s) noexcept {
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
}

std::string Tokenizer::to_upper(std::string_view s) {
    std::string result(s);
    to_upper_inplace(result);
    return result;
}

std::string_view Tokenizer::trim(std::string_view s) noexcept {
    std::size_t lo = 0;
    while (lo < s.size() && std::isspace(static_cast<unsigned char>(s[lo]))) ++lo;
    std::size_t hi = s.size();
    while (hi > lo && std::isspace(static_cast<unsigned char>(s[hi - 1]))) --hi;
    return s.substr(lo, hi - lo);
}

// ============================================================================
// Numeric / boolean classification
// ============================================================================

bool Tokenizer::is_numeric(std::string_view sv) noexcept {
    sv = trim(sv);
    if (sv.empty()) return false;

    // Use std::from_chars to check validity (no allocations, locale-independent)
    double val;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
    return ec == std::errc{} && ptr == sv.data() + sv.size();
}

bool Tokenizer::is_boolean(std::string_view sv) noexcept {
    // Normalize to uppercase for comparison
    char buf[8];
    if (sv.size() >= sizeof(buf)) return false;
    for (std::size_t i = 0; i < sv.size(); ++i) {
        buf[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(sv[i])));
    }
    std::string_view upper(buf, sv.size());
    return upper == "YES"   || upper == "NO"    ||
           upper == "TRUE"  || upper == "FALSE" ||
           upper == "1"     || upper == "0";
}

bool Tokenizer::parse_boolean(std::string_view sv) noexcept {
    char buf[8];
    if (sv.size() >= sizeof(buf)) return false;
    for (std::size_t i = 0; i < sv.size(); ++i) {
        buf[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(sv[i])));
    }
    std::string_view upper(buf, sv.size());
    return upper == "YES" || upper == "TRUE" || upper == "1";
}

} /* namespace openswmm::input */
