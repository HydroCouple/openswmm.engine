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
 * @file FilePathPair.hpp
 * @brief Carrier for an external file reference in a SWMM model.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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

    FilePathPair() = default;
    FilePathPair(const FilePathPair&) = default;
    FilePathPair(FilePathPair&&) noexcept = default;
    FilePathPair& operator=(const FilePathPair&) = default;
    FilePathPair& operator=(FilePathPair&&) noexcept = default;

    // Implicit construction from a string token — assigns to `original`
    // and leaves `absolute` empty. Mirrors legacy storage semantics so the
    // mechanical migration touches only struct definitions.
    FilePathPair(const std::string& s) : original(s) {}                  // NOLINT
    FilePathPair(std::string&& s) noexcept : original(std::move(s)) {}    // NOLINT
    FilePathPair(const char* s) : original(s ? s : "") {}                // NOLINT

    // Assignment from string clears the cached absolute resolution because
    // the original token just changed; the next read of `absolute` is the
    // job of PostParseResolver or an explicit caller.
    FilePathPair& operator=(const std::string& s) {
        original = s;
        absolute.clear();
        return *this;
    }
    FilePathPair& operator=(std::string&& s) noexcept {
        original = std::move(s);
        absolute.clear();
        return *this;
    }
    FilePathPair& operator=(const char* s) {
        original.assign(s ? s : "");
        absolute.clear();
        return *this;
    }

    // Implicit conversion to const std::string& exposes the token to all
    // existing readers (`fprintf`, comparisons, copies, etc.).
    operator const std::string&() const noexcept { return original; }    // NOLINT

    // Common std::string-like introspection.
    [[nodiscard]] bool        empty() const noexcept { return original.empty(); }
    [[nodiscard]] std::size_t size()  const noexcept { return original.size();  }
    [[nodiscard]] const char* c_str() const noexcept { return original.c_str(); }
    [[nodiscard]] const std::string& str() const noexcept { return original; }

    // Explicit comparison helpers — template matchers like gtest's EXPECT_EQ
    // prefer direct ops over relying on implicit conversion.
    friend bool operator==(const FilePathPair& a, const FilePathPair& b) {
        return a.original == b.original;
    }
    friend bool operator!=(const FilePathPair& a, const FilePathPair& b) {
        return !(a == b);
    }
    friend bool operator==(const FilePathPair& a, const std::string& s) {
        return a.original == s;
    }
    friend bool operator==(const std::string& s, const FilePathPair& a) {
        return s == a.original;
    }
    friend bool operator!=(const FilePathPair& a, const std::string& s) {
        return !(a == s);
    }
    friend bool operator!=(const std::string& s, const FilePathPair& a) {
        return !(s == a);
    }
    friend bool operator==(const FilePathPair& a, const char* s) {
        return s ? (a.original == s) : a.original.empty();
    }
    friend bool operator==(const char* s, const FilePathPair& a) { return a == s; }
    friend bool operator!=(const FilePathPair& a, const char* s) { return !(a == s); }
    friend bool operator!=(const char* s, const FilePathPair& a) { return !(a == s); }
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_FILE_PATH_PAIR_HPP
