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
 * @file      SectionParser.hpp
 * @brief     Utility for splitting raw INP section lines into (data, comment) pairs.
 *
 * @details Preserves per-object comments written by the legacy SWMM GUI.
 *          Comment storage format: multiple lines are joined with the literal
 *          two-character token "\\n" (backslash + n) so that each comments
 *          string remains a flat, single-line value suitable for direct storage
 *          in SQLite TEXT columns.  The InpWriter splits on that token when
 *          emitting ';'-prefixed rows.
 *
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once

#include <string>
#include <vector>

namespace openswmm::input {

/**
 * @brief One data row together with its associated object comment.
 *
 * @details `data` is the raw data token string (comment-stripped, ready for
 *          Tokenizer::tokenize).  `comment` is the accumulated object comment:
 *          multiple ';'-prefixed lines immediately above this data row, joined
 *          by the literal two-character sequence "\\n".  Empty string when the
 *          object has no comment.
 */
struct ParsedLine {
    std::string data;    ///< Stripped data line ready for tokenisation
    std::string comment; ///< Object comment joined with literal "\\n"; "" = none
};

/**
 * @brief Pair raw section data rows with preceding object-comment text.
 *
 * Single-semicolon lines append text after the semicolon using a literal backslash-n separator.
 * Double-semicolon headers clear pending comments; trailing unattached comments are discarded.
 * Whitespace-only strings are data unless pretrimmed. Data rows are copied verbatim: this helper
 * does not strip inline comments or quotes.
 *
 * @param[in] raw_lines Borrowed section rows. Empty strings are skipped; semicolon handling
 *     requires the semicolon in column zero, so callers trim structural rows first.
 * @return Independent ParsedLine strings, one per nonempty data row.
 */
inline std::vector<ParsedLine> parse_section(const std::vector<std::string>& raw_lines) {
    std::vector<ParsedLine> result;
    std::string pending;

    for (const auto& raw : raw_lines) {
        if (raw.empty())
            continue;

        if (raw[0] == ';') {
            if (raw.size() >= 2 && raw[1] == ';') {
                // ";;" column-header or dashes line — discard and reset pending
                pending.clear();
            } else {
                // Single-';' object-comment line — append to pending
                if (!pending.empty())
                    pending += "\\n";
                pending += raw.substr(1); // strip leading ';'
            }
            continue;
        }

        // Data line — package with accumulated comment
        result.push_back({raw, std::move(pending)});
        pending.clear();
    }
    return result;
}

} // namespace openswmm::input
