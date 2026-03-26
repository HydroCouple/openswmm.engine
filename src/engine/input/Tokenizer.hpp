/**
 * @file Tokenizer.hpp
 * @brief Multi-delimiter tokenizer for SWMM input files.
 *
 * @details Replaces the legacy space-only tokenizer in src/solver/input.c.
 *          The new tokenizer handles comma, tab, and one-or-more spaces as
 *          delimiters. This allows input files to be formatted as:
 *          - Traditional SWMM (space-delimited)
 *          - Spreadsheet export (comma or tab-delimited CSV)
 *          - Mixed formats within the same file
 *
 * ### Key differences from legacy SWMM tokenizer
 *
 * | Feature | Legacy (input.c) | New Tokenizer |
 * |---------|-----------------|---------------|
 * | Delimiters | Space only | Space, tab, comma |
 * | Comment char | `;` (semicolon) | `;` or `;;` |
 * | Quoted strings | Not supported | Supported (double-quotes) |
 * | Performance | sscanf-based | std::string_view, no allocations |
 *
 * @see Legacy reference: src/solver/input.c — getToken()
 * @see tests/unit/test_tokenizer.cpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
    /** @brief Returns true if `c` is a token delimiter (comma, tab, space). */
    static bool is_delimiter(char c) noexcept {
        return c == ',' || c == '\t' || c == ' ';
    }
};

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_TOKENIZER_HPP */
