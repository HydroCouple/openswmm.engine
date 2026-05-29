/**
 * @file SectionParser.hpp
 * @brief Utility for splitting raw INP section lines into (data, comment) pairs.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
    std::string data;     ///< Stripped data line ready for tokenisation
    std::string comment;  ///< Object comment joined with literal "\\n"; "" = none
};

/**
 * @brief Split raw section lines into (data, comment) pairs.
 *
 * @details Rules applied in order:
 *   - Lines starting with ";;" are column-header or dashes markers — discarded,
 *     and any accumulated pending comment is reset.
 *   - Lines starting with ";" (but not ";;") are object-comment lines.  The
 *     leading semicolon is stripped and the text is appended to the pending
 *     comment, separated from any prior line by the literal "\\n" token.
 *   - All other non-empty lines are data lines.  They receive the pending
 *     comment (which is then cleared) and are appended to the result.
 *   - A trailing comment block with no subsequent data line is silently
 *     discarded (there is no object to attach it to).
 *
 * @param raw_lines  Lines as accumulated by InputReader (may contain ";;" and
 *                   ";" lines verbatim, plus stripped data lines).
 * @returns          Vector of ParsedLine, one entry per data line.
 */
inline std::vector<ParsedLine>
parse_section(const std::vector<std::string>& raw_lines)
{
    std::vector<ParsedLine> result;
    std::string pending;

    for (const auto& raw : raw_lines) {
        if (raw.empty()) continue;

        if (raw[0] == ';') {
            if (raw.size() >= 2 && raw[1] == ';') {
                // ";;" column-header or dashes line — discard and reset pending
                pending.clear();
            } else {
                // Single-';' object-comment line — append to pending
                if (!pending.empty()) pending += "\\n";
                pending += raw.substr(1);  // strip leading ';'
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
