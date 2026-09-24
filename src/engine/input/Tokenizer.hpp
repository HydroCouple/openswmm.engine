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
 * @file Tokenizer.hpp
 * @brief Multi-delimiter tokenizer for SWMM input files.
 *
 * @details Replaces the legacy space-only tokenizer in src/solver/input.c.
 *          Whitespace always delimits; a comma delimits only where it cannot
 *          be part of a name. This allows input files to be formatted as:
 *          - Traditional SWMM (space-delimited)
 *          - Spreadsheet export (comma or tab-delimited CSV)
 *          - Mixed formats within the same file
 *
 *          The comma rule is positional, not dialect-wide, because the two
 *          readings genuinely collide. Legacy splits on `" \t\n\r"` only, so a
 *          comma is an ordinary name character — and real decks depend on it:
 *          `S-2-M20-L109/link2,6-201-9(u)` is ONE subcatchment outlet. Treating
 *          every comma as a delimiter truncated that reference, read the name
 *          fragment after it as the subcatchment's area, and produced a
 *          trailing token the legacy reader then rejected as an undefined
 *          object.
 *
 *          So: a comma separates when it sits at a token boundary — adjacent
 *          to whitespace or at a line edge — or when the line carries no
 *          whitespace at all outside quotes and is therefore pure CSV. A comma
 *          with non-whitespace on both sides, in an otherwise
 *          whitespace-delimited line, belongs to its token. A value that
 *          genuinely contains a comma AND sits in a CSV row must be quoted,
 *          which is the ordinary CSV convention.
 *
 * ### Key differences from legacy SWMM tokenizer
 *
 * | Feature | Legacy (input.c) | New Tokenizer |
 * |---------|-----------------|---------------|
 * | Delimiters | Space, tab, CR/LF | Same, plus comma at a token boundary |
 * | Comment char | `;` (semicolon) | `;` or `;;` |
 * | Quoted strings | Filenames only | Supported anywhere (double-quotes) |
 * | Performance | sscanf-based | std::string_view, no allocations |
 *
 * @see Legacy reference: src/solver/input.c — getToken()
 * @see tests/unit/test_tokenizer.cpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TOKENIZER_HPP
#define OPENSWMM_ENGINE_TOKENIZER_HPP

#include <string>
#include <string_view>
#include <vector>

namespace openswmm::input {

/**
 * @brief Stateless multi-delimiter tokenizer for SWMM input lines.
 *
 * @details All methods are static and stateless. Instantiation is not needed.
 *
 * @ingroup engine_input
 */
class Tokenizer {
public:
    Tokenizer() = delete;

    // -----------------------------------------------------------------------
    // Comment handling
    // -----------------------------------------------------------------------

    /**
     * @brief Strip trailing semicolon comment from a line.
     *
     * @details Removes everything from the first unquoted semicolon (`;`) to
     *          the end of the line. Works correctly when semicolons appear
     *          inside quoted strings.
     *
     * @param line  Input line (may contain comment).
     * @returns     Line with comment stripped (may be empty or whitespace-only).
     *
     * @note Returns a view into `line`; the returned view is valid only as
     *       long as `line` is alive.
     */
    static std::string_view strip_comment(std::string_view line) noexcept;

    // -----------------------------------------------------------------------
    // Tokenization
    // -----------------------------------------------------------------------

    /**
     * @brief Split a SWMM input line into tokens.
     *
     * @details Delimiters: comma (`,`), horizontal tab (`\t`), one-or-more
     *          ASCII spaces. Consecutive delimiters of the same type produce
     *          no empty tokens (e.g., `"A  B"` → `["A", "B"]`, not `["A", "", "B"]`).
     *
     *          Quoted strings (double-quoted `"..."`) are treated as single
     *          tokens with the quotes stripped. Internal whitespace in quoted
     *          strings is preserved.
     *
     *          Comments are stripped before tokenizing (see strip_comment()).
     *
     * @param line  Input line (comment will be stripped internally).
     * @returns     Vector of token strings (empty if line is blank/comment).
     *
     * @note Returns strings by value. Use tokenize_views() for zero-allocation
     *       views if performance is critical.
     */
    static std::vector<std::string> tokenize(std::string_view line);

    /**
     * @brief Split a line and return string_view tokens (zero-allocation).
     *
     * @details Like tokenize(), but returns views into `line`. The views are
     *          only valid as long as `line` is alive.
     *
     * @warning NOT suitable for lines with quoted tokens (views can't escape quotes).
     *          Fall back to tokenize() if quoted tokens are expected.
     *
     * @param line     Input line.
     * @param stripped [out] If provided, receives the comment-stripped line.
     * @returns        Vector of string_view tokens.
     */
    static std::vector<std::string_view> tokenize_views(std::string_view line);

    /**
     * @brief tokenize_views into a caller-owned buffer.
     *
     * @details Hoist the buffer out of a per-row loop and this allocates
     *          nothing at all after the first row — clear() keeps capacity.
     *          That matters in the geometry sections, which are the row-count
     *          leaders in a large model ([VERTICES] alone is three rows per
     *          link).
     *
     *          Like tokenize_views(), quoted tokens are NOT supported; callers
     *          that may see them must fall back to tokenize().
     */
    static void tokenize_views_into(std::string_view line,
                                    std::vector<std::string_view>& out);

    // -----------------------------------------------------------------------
    // Utilities
    // -----------------------------------------------------------------------

    /**
     * @brief Convert a string to uppercase in-place.
     * @param s  String to uppercase.
     */
    static void to_upper_inplace(std::string& s) noexcept;

    /**
     * @brief Convert a string to uppercase (returns new string).
     */
    static std::string to_upper(std::string_view s);

    /**
     * @brief Trim leading and trailing whitespace from a string_view.
     */
    static std::string_view trim(std::string_view s) noexcept;

    /**
     * @brief Returns true if `sv` represents a numeric value.
     * @details Accepts integer and floating-point formats including:
     *          "3.14", "-1.5e-3", "42", "+0.5"
     */
    static bool is_numeric(std::string_view sv) noexcept;

    /**
     * @brief Returns true if `sv` represents a boolean YES/NO/TRUE/FALSE/1/0.
     */
    static bool is_boolean(std::string_view sv) noexcept;

    /**
     * @brief Parse a boolean token.
     * @param sv  Token (YES/NO/TRUE/FALSE/1/0, case-insensitive).
     * @returns   true for YES/TRUE/1, false otherwise.
     */
    static bool parse_boolean(std::string_view sv) noexcept;

private:
    /** @brief Space or tab — always a delimiter, as in legacy SWMM. */
    static bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }

    /**
     * @brief True when the line carries no whitespace OUTSIDE quotes.
     *
     * @details Such a line has no other structure to read, so its commas can
     *          only be column separators — this is the "spreadsheet export"
     *          case. Whitespace inside a quoted token does not count: for
     *          `"a b",c` the quotes supply the structure and the comma still
     *          separates.
     */
    static bool is_pure_csv_line(std::string_view line) noexcept;

    /**
     * @brief True when the comma at @p pos separates tokens rather than
     *        belonging to one.
     *
     * @details Legacy SWMM splits on `" \t\n\r"` only (input.c SEPSTR), so a
     *          comma is an ordinary name character there — and real decks rely
     *          on that: `S-2-M20-L109/link2,6-201-9(u)` is ONE outlet name.
     *          Splitting it truncated the reference, read a name fragment as
     *          the area and invented a trailing token.
     *
     *          This engine also accepts comma-delimited input, so the two
     *          readings are separated by position rather than by dialect: a
     *          comma separates when it sits at a token boundary — against
     *          whitespace or a line edge — or when the whole line is pure CSV.
     *          A comma flanked by non-whitespace on both sides, in a line that
     *          is otherwise whitespace-delimited, belongs to its token.
     *
     * @param line      The comment-stripped line.
     * @param pos       Index of the comma.
     * @param pure_csv  Result of is_pure_csv_line() for this line.
     */
    static bool comma_separates(std::string_view line, std::size_t pos,
                                bool pure_csv) noexcept;
};

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_TOKENIZER_HPP */
