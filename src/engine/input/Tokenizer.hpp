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
 * @file      Tokenizer.hpp
 * @brief     Multi-delimiter tokenizer for SWMM input files.
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
 * | Quoted strings | Filenames only | Whole tokens or embedded key="value" (double-quotes) |
 * | Performance | sscanf-based | Reusable view vector avoids per-token copies |
 *
 * @see Legacy reference: src/solver/input.c — getToken()
 * @see tests/unit/test_tokenizer.cpp
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
    /**
     * @brief Disallow tokenizer instances; all operations are static.
     */
    Tokenizer() = delete;

    // -----------------------------------------------------------------------
    // Comment handling
    // -----------------------------------------------------------------------

    /**
     * @brief Borrow the line prefix before its first unquoted semicolon.
     *
     * Double quotes toggle quote state; backslash/CSV doubled-quote escaping is not interpreted. No
     * allocation; returned storage must outlive the view.
     *
     * @param[in] line Borrowed bytes; caller retains storage for the duration of this call.
     * @return View into line, unchanged if no unquoted semicolon exists.
     */
    static std::string_view strip_comment(std::string_view line) noexcept;

    // -----------------------------------------------------------------------
    // Tokenization
    // -----------------------------------------------------------------------

    /**
     * @brief Split a line into owned tokens using whitespace and positional comma rules.
     *
     * Strips unquoted semicolon comments. Space/tab delimit; commas delimit at boundaries or in a line
     * without unquoted whitespace. Leading/consecutive CSV commas can create empty tokens; a final
     * comma does not create a final empty field. A leading quoted token loses its enclosing quotes;
     * embedded quotes remain. Unclosed quotes are tolerated through end of line. No escape decoding or
     * CR/LF normalization is performed here.
     *
     * @param[in] line Borrowed bytes; caller retains storage for the duration of this call.
     * @return Independent owned strings; empty for a blank/comment-only line.
     */
    static std::vector<std::string> tokenize(std::string_view line);

    /**
     * @brief Split an unquoted line into borrowed nonempty token views.
     *
     * Comment/comma rules mirror tokenize(), but quoted tokens are unsupported and empty CSV columns
     * are discarded. Allocates the result vector; token bytes are not copied.
     *
     * @param[in] line Borrowed bytes; caller retains storage for the duration of this call.
     * @return Vector owning the views, whose referenced bytes remain owned by line.
     */
    static std::vector<std::string_view> tokenize_views(std::string_view line);

    /**
     * @brief Replace a reusable vector with borrowed nonempty token views.
     *
     * Quoted tokens are unsupported; empty CSV columns are discarded. Reserves at least eight entries,
     * so first use and larger rows can allocate. Retained views are invalid once input storage changes
     * or dies.
     *
     * @param[in] line Borrowed bytes; caller retains storage for the duration of this call.
     * @param[out] out Caller-owned vector cleared first; its capacity is reused and grown when needed.
     *     Views borrow line storage, which must not alias storage invalidated by clearing out.
     */
    static void tokenize_views_into(std::string_view line, std::vector<std::string_view>& out);

    // -----------------------------------------------------------------------
    // Utilities
    // -----------------------------------------------------------------------

    /**
     * @brief Uppercase bytes in place using the current C locale.
     *
     * Preserves byte count; does not perform Unicode case mapping. Unlike engine name equality helpers
     * this uses the C locale, not a fixed ASCII-only fold.
     *
     * @param[in,out] s Mutable string whose bytes are converted via unsigned-char-safe std::toupper.
     */
    static void to_upper_inplace(std::string& s) noexcept;

    /**
     * @brief Copy bytes and uppercase the copy using the current C locale.
     *
     * @param[in] s Borrowed source bytes, including embedded NUL bytes.
     * @return Independent uppercase string; input is preserved.
     */
    static std::string to_upper(std::string_view s);

    /**
     * @brief Borrow the substring without leading/trailing C-locale whitespace.
     *
     * Uses unsigned-char-safe std::isspace; does not remove interior whitespace.
     *
     * @param[in] s Borrowed input bytes; may be empty.
     * @return View into s, possibly empty; caller must preserve underlying storage.
     */
    static std::string_view trim(std::string_view s) noexcept;

    /**
     * @brief Test whether a trimmed token parses completely as a double.
     *
     * Special values and leading-plus acceptance follow the selected numeric backend. This is not a
     * finiteness predicate.
     *
     * @param[in] sv Borrowed token; leading/trailing C-locale whitespace is trimmed.
     * @return True for successful complete conversion by from_chars_double(); false for empty, invalid,
     *     partial or reported overflow.
     */
    static bool is_numeric(std::string_view sv) noexcept;

    /**
     * @brief Recognize an untrimmed boolean token.
     *
     * @param[in] sv Borrowed token; locale-uppercase comparison accepts YES/NO/TRUE/FALSE/1/0.
     * @return True for those complete tokens; false for whitespace-padded, unknown or >=8-byte tokens.
     */
    static bool is_boolean(std::string_view sv) noexcept;

    /**
     * @brief Interpret an untrimmed affirmative boolean token.
     *
     * Use is_boolean() first when an invalid token must be distinguished from false.
     *
     * @param[in] sv Borrowed token compared after C-locale byte uppercasing.
     * @return True for YES/TRUE/1; false for NO/FALSE/0 and all invalid tokens.
     */
    static bool parse_boolean(std::string_view sv) noexcept;

private:
    /**
     * @brief Recognize tokenizer structural whitespace.
     *
     * @param[in] c Single byte to classify.
     * @return True only for ASCII space or horizontal tab; CR/LF are not included.
     */
    static bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }

    /**
     * @brief Test whether a line has no spaces/tabs outside double quotes.
     *
     * Does not strip comments itself; callers pass the comment-stripped line. Quotes toggle state
     * without escape interpretation.
     *
     * @param[in] line Borrowed bytes; caller retains storage for the duration of this call.
     * @return True for no unquoted space/tab, including an empty line.
     */
    static bool is_pure_csv_line(std::string_view line) noexcept;

    /**
     * @brief Determine whether a particular comma is a field separator.
     *
     * @param[in] line Borrowed comment-stripped line.
     * @param[in] pos Index of an existing comma within line; caller guarantees validity.
     * @param[in] pure_csv Previously computed is_pure_csv_line(line).
     * @return True in pure CSV mode or at a line edge or beside space/tab; false for interior name
     *     commas in whitespace-delimited rows.
     */
    static bool comma_separates(std::string_view line, std::size_t pos, bool pure_csv) noexcept;
};

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_TOKENIZER_HPP */
