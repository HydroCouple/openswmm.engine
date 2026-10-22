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
 * @file      FilePathPair.hpp
 * @brief     Carrier for an external file reference in a SWMM model.
 *
 * @details Pulled out of SimulationContext.hpp so headers like
 *          `data/GageData.hpp` and `data/TableData.hpp` can use it
 *          without inheriting the full simulation context surface.
 *
 *          See `openswmm.gui/docs/IO_PORTABILITY_PLAN.md` §3.3 for the
 *          two-string design and the IO-3 / IO-4 lifecycle.
 *
 * @ingroup engine_core
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FILE_PATH_PAIR_HPP
#define OPENSWMM_ENGINE_FILE_PATH_PAIR_HPP

#include <cstddef>
#include <string>
#include <utility>

namespace openswmm {

/**
 * @brief Two-string carrier for any external file path that appears in a
 *        SWMM `.inp` file.
 *
 * @details Each slot carries:
 *          - `absolute`: resolved absolute path, ready for `fopen()`.
 *            Filled by `PostParseResolver` after read; empty for
 *            programmatic models until a path-set call runs.
 *          - `original`: verbatim token as it appeared in the source
 *            `.inp` file (relative or absolute, with whichever separators
 *            the author used).  Empty for models that were not loaded
 *            from an `.inp`.
 *
 *          Slice IO-2 keeps every existing caller working through an
 *          implicit `std::string` surface that targets `original`.
 *          Slice IO-3 wires `PostParseResolver` to populate `absolute`;
 *          Slice IO-4 wires the writer to rebase `original` against the
 *          destination directory.
 */
struct FilePathPair {
    std::string absolute;
    std::string original;

    /**
     * @brief Create an empty authored token and empty resolved-path cache.
     */
    FilePathPair() = default;
    /**
     * @brief Copy the authored token and resolved absolute cache from another pair.
     *
     * Both strings become independently owned copies; no path resolution or filesystem access occurs.
     */
    FilePathPair(const FilePathPair&) = default;
    /**
     * @brief Move the authored token and resolved cache from another pair.
     *
     * Source remains valid with unspecified string contents. Does not resolve/check either path.
     */
    FilePathPair(FilePathPair&&) noexcept = default;
    /**
     * @brief Copy both owned path strings from another pair.
     * @return Reference to this pair; old string views may be invalidated.
     */
    FilePathPair& operator=(const FilePathPair&) = default;
    /**
     * @brief Move both owned path strings from another pair.
     * @return Reference to this pair; source remains valid with unspecified string contents.
     */
    FilePathPair& operator=(FilePathPair&&) noexcept = default;

    // Implicit construction from a string token — assigns to `original`
    // and leaves `absolute` empty. Mirrors legacy storage semantics so the
    // mechanical migration touches only struct definitions.
    /**
     * @brief Copy a token into original and leave the resolved cache empty.
     *
     * Does not resolve, normalize, validate or open the path.
     *
     * @param[in] s Borrowed authored path token, copied verbatim including embedded NUL bytes.
     */
    FilePathPair(const std::string& s) : original(s) {} // NOLINT
    /**
     * @brief Move a token into original and leave the resolved cache empty.
     *
     * Does not resolve, normalize, validate or open the path.
     *
     * @param[in] s Token moved into original; source remains valid with unspecified contents.
     */
    FilePathPair(std::string&& s) noexcept : original(std::move(s)) {} // NOLINT
    /**
     * @brief Copy a token into original and leave the resolved cache empty.
     *
     * Does not resolve, normalize, validate or open the path.
     *
     * @param[in] s Borrowed NUL-terminated token; NULL is treated as empty.
     */
    FilePathPair(const char* s) : original(s ? s : "") {} // NOLINT

    // Assignment from string clears the cached absolute resolution because
    // the original token just changed; the next read of `absolute` is the
    // job of PostParseResolver or an explicit caller.
    /**
     * @brief Copy a new authored token and clear cached absolute resolution.
     *
     * Caller must resolve the new token explicitly before relying on absolute.
     *
     * @param[in] s Borrowed authored path token, copied verbatim including embedded NUL bytes.
     * @return Reference to this pair; previous borrowed token/cache views may be invalidated.
     */
    FilePathPair& operator=(const std::string& s) {
        original = s;
        absolute.clear();
        return *this;
    }
    /**
     * @brief Move a new authored token and clear cached absolute resolution.
     *
     * Caller must resolve the new token explicitly before relying on absolute.
     *
     * @param[in] s Token moved into original; source remains valid with unspecified contents.
     * @return Reference to this pair; previous borrowed token/cache views may be invalidated.
     */
    FilePathPair& operator=(std::string&& s) noexcept {
        original = std::move(s);
        absolute.clear();
        return *this;
    }
    /**
     * @brief Copy a new authored token and clear cached absolute resolution.
     *
     * Caller must resolve the new token explicitly before relying on absolute.
     *
     * @param[in] s Borrowed NUL-terminated token; NULL is treated as empty.
     * @return Reference to this pair; previous borrowed token/cache views may be invalidated.
     */
    FilePathPair& operator=(const char* s) {
        original.assign(s ? s : "");
        absolute.clear();
        return *this;
    }

    // Implicit conversion to const std::string& exposes the token to all
    // existing readers (`fprintf`, comparisons, copies, etc.).
    /**
     * @brief Borrow the authored token through implicit string conversion.
     * @return Reference to original, not absolute; valid until token modification or pair destruction.
     */
    operator const std::string&() const noexcept { return original; } // NOLINT

    // Common std::string-like introspection.
    /**
     * @brief Test whether the authored token is empty.
     * @return original.empty(); ignores whether absolute has been populated.
     */
    [[nodiscard]] bool empty() const noexcept { return original.empty(); }
    /**
     * @brief Count bytes in the authored token.
     * @return original.size(), excluding the terminating NUL and including any embedded NUL bytes.
     */
    [[nodiscard]] std::size_t size() const noexcept { return original.size(); }
    /**
     * @brief Borrow a NUL-terminated view of the authored token.
     * @return original.c_str(); do not free, and copy before token modification or pair destruction.
     */
    [[nodiscard]] const char* c_str() const noexcept { return original.c_str(); }
    /**
     * @brief Borrow the complete authored token.
     * @return Reference to original; absolute cache is not consulted.
     */
    [[nodiscard]] const std::string& str() const noexcept { return original; }

    // Explicit comparison helpers — template matchers like gtest's EXPECT_EQ
    // prefer direct ops over relying on implicit conversion.
    /**
     * @brief Compare authored tokens for exact-byte equality.
     *
     * No case folding, path normalization, filesystem lookup or resolved-cache comparison is performed.
     *
     * @param[in] a Borrowed pair; only original participates, never absolute.
     * @param[in] b Borrowed pair; only original participates, never absolute.
     * @return True when the authored tokens are equal.
     */
    friend bool operator==(const FilePathPair& a, const FilePathPair& b) {
        return a.original == b.original;
    }
    /**
     * @brief Compare authored tokens for exact-byte inequality.
     *
     * No case folding, path normalization, filesystem lookup or resolved-cache comparison is performed.
     *
     * @param[in] a Borrowed pair; only original participates, never absolute.
     * @param[in] b Borrowed pair; only original participates, never absolute.
     * @return True when the authored tokens are different.
     */
    friend bool operator!=(const FilePathPair& a, const FilePathPair& b) { return !(a == b); }
    /**
     * @brief Compare authored tokens for exact-byte equality.
     *
     * No case folding, path normalization, filesystem lookup or resolved-cache comparison is performed.
     *
     * @param[in] a Borrowed pair; only original participates, never absolute.
     * @param[in] s Borrowed exact-byte token, including embedded NUL bytes.
     * @return True when the authored tokens are equal.
     */
    friend bool operator==(const FilePathPair& a, const std::string& s) { return a.original == s; }
    /**
     * @brief Compare authored tokens for exact-byte equality.
     *
     * No case folding, path normalization, filesystem lookup or resolved-cache comparison is performed.
     *
     * @param[in] s Borrowed exact-byte token, including embedded NUL bytes.
     * @param[in] a Borrowed pair; only original participates, never absolute.
     * @return True when the authored tokens are equal.
     */
    friend bool operator==(const std::string& s, const FilePathPair& a) { return s == a.original; }
    /**
     * @brief Compare authored tokens for exact-byte inequality.
     *
     * No case folding, path normalization, filesystem lookup or resolved-cache comparison is performed.
     *
     * @param[in] a Borrowed pair; only original participates, never absolute.
     * @param[in] s Borrowed exact-byte token, including embedded NUL bytes.
     * @return True when the authored tokens are different.
     */
    friend bool operator!=(const FilePathPair& a, const std::string& s) { return !(a == s); }
    /**
     * @brief Compare authored tokens for exact-byte inequality.
     *
     * No case folding, path normalization, filesystem lookup or resolved-cache comparison is performed.
     *
     * @param[in] s Borrowed exact-byte token, including embedded NUL bytes.
     * @param[in] a Borrowed pair; only original participates, never absolute.
     * @return True when the authored tokens are different.
     */
    friend bool operator!=(const std::string& s, const FilePathPair& a) { return !(s == a); }
    /**
     * @brief Compare authored tokens for exact-byte equality.
     *
     * No case folding, path normalization, filesystem lookup or resolved-cache comparison is performed.
     *
     * @param[in] a Borrowed pair; only original participates, never absolute.
     * @param[in] s Borrowed NUL-terminated token; NULL compares as the empty token.
     * @return True when the authored tokens are equal.
     */
    friend bool operator==(const FilePathPair& a, const char* s) {
        return s ? (a.original == s) : a.original.empty();
    }
    /**
     * @brief Compare authored tokens for exact-byte equality.
     *
     * No case folding, path normalization, filesystem lookup or resolved-cache comparison is performed.
     *
     * @param[in] s Borrowed NUL-terminated token; NULL compares as the empty token.
     * @param[in] a Borrowed pair; only original participates, never absolute.
     * @return True when the authored tokens are equal.
     */
    friend bool operator==(const char* s, const FilePathPair& a) { return a == s; }
    /**
     * @brief Compare authored tokens for exact-byte inequality.
     *
     * No case folding, path normalization, filesystem lookup or resolved-cache comparison is performed.
     *
     * @param[in] a Borrowed pair; only original participates, never absolute.
     * @param[in] s Borrowed NUL-terminated token; NULL compares as the empty token.
     * @return True when the authored tokens are different.
     */
    friend bool operator!=(const FilePathPair& a, const char* s) { return !(a == s); }
    /**
     * @brief Compare authored tokens for exact-byte inequality.
     *
     * No case folding, path normalization, filesystem lookup or resolved-cache comparison is performed.
     *
     * @param[in] s Borrowed NUL-terminated token; NULL compares as the empty token.
     * @param[in] a Borrowed pair; only original participates, never absolute.
     * @return True when the authored tokens are different.
     */
    friend bool operator!=(const char* s, const FilePathPair& a) { return !(a == s); }
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_FILE_PATH_PAIR_HPP
