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
 * @file Tokenizer.cpp
 * @brief Implementation of the multi-delimiter SWMM input tokenizer.
 *
 * @see Tokenizer.hpp for interface documentation.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Tokenizer.hpp"

#include <algorithm>
#include "../core/charconv_compat.hpp"

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

bool Tokenizer::is_pure_csv_line(std::string_view line) noexcept {
    bool in_quotes = false;
    for (char c : line) {
        if (c == '"') { in_quotes = !in_quotes; continue; }
        if (!in_quotes && is_space(c)) return false;
    }
    return true;
}

bool Tokenizer::comma_separates(std::string_view line, std::size_t pos,
                                bool pure_csv) noexcept {
    // Nothing else structures the line, so the commas must.
    if (pure_csv) return true;
    // A comma opening or closing the line cannot be interior to a name.
    if (pos == 0 || pos + 1 >= line.size()) return true;
    // Otherwise it separates only where a token has already ended (or is about
    // to begin) — i.e. against whitespace. Flanked by non-whitespace, it is
    // part of the name, which is what legacy does with every comma.
    return is_space(line[pos - 1]) || is_space(line[pos + 1]);
}

std::vector<std::string> Tokenizer::tokenize(std::string_view line) {
    std::string_view stripped = strip_comment(line);
    const bool pure_csv = is_pure_csv_line(stripped);

    std::vector<std::string> tokens;
    // Rows in every .inp section are a handful of columns; without this the
    // vector reallocates two or three times per row, and there are millions of
    // rows in a large model. 8 covers the overwhelming majority in one
    // allocation (longer rows still grow normally).
    tokens.reserve(8);
    std::size_t i = 0;
    const std::size_t n = stripped.size();

    while (i < n) {
        // Skip leading delimiters (spaces/tabs only — commas are explicit separators)
        while (i < n && (stripped[i] == ' ' || stripped[i] == '\t')) {
            ++i;
        }
        if (i >= n) break;

        if (stripped[i] == ',' && comma_separates(stripped, i, pure_csv)) {
            // An empty CSV field, which must be kept or every later column
            // shifts left. It is only empty when this comma directly follows
            // another (",,") or opens the line — a comma reached after
            // whitespace is just the separator that ended the previous token,
            // so "A ,B" is two fields, not three.
            if (i == 0 || stripped[i - 1] == ',') tokens.emplace_back();
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

        // Regular token: read to the next delimiter. A comma only ends the
        // token where it separates; otherwise it is an ordinary name
        // character, as it is in legacy SWMM.
        std::size_t start = i;
        while (i < n && !is_space(stripped[i]) &&
               !(stripped[i] == ',' && comma_separates(stripped, i, pure_csv))) {
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
    std::vector<std::string_view> tokens;
    tokenize_views_into(line, tokens);
    return tokens;
}

void Tokenizer::tokenize_views_into(std::string_view line,
                                    std::vector<std::string_view>& tokens) {
    std::string_view stripped = strip_comment(line);
    const bool pure_csv = is_pure_csv_line(stripped);

    tokens.clear();
    if (tokens.capacity() < 8) tokens.reserve(8);
    std::size_t i = 0;
    const std::size_t n = stripped.size();

    while (i < n) {
        // Skip spaces and tabs
        while (i < n && (stripped[i] == ' ' || stripped[i] == '\t')) ++i;
        if (i >= n) break;

        if (stripped[i] == ',' && comma_separates(stripped, i, pure_csv)) {
            ++i;
            continue;
        }

        // Regular token (quoted tokens not supported — caller must use tokenize())
        std::size_t start = i;
        while (i < n && !is_space(stripped[i]) &&
               !(stripped[i] == ',' && comma_separates(stripped, i, pure_csv))) ++i;
        if (i > start) {
            tokens.push_back(stripped.substr(start, i - start));
        }
        if (i < n && stripped[i] == ',') ++i;
    }
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
    auto [ptr, ec] = openswmm::from_chars_double(sv.data(), sv.data() + sv.size(), val);
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
