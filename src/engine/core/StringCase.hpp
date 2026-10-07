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
 * @file StringCase.hpp
 * @brief Case-insensitive string helpers matching legacy SWMM name semantics.
 *
 * @details The legacy engine's hash table (src/legacy/engine/hash.c) compares
 *          and hashes object names case-insensitively via its UCHAR macro,
 *          which uppercases ASCII letters only. Every name lookup in the
 *          refactored engine that must be legacy-parity therefore folds case
 *          with the helpers here — ASCII-only, byte-for-byte the same fold as
 *          UCHAR — never with locale-dependent std::toupper.
 *
 *          CiHash/CiEqual are transparent (heterogeneous) functors so an
 *          unordered_map<std::string, T, CiHash, CiEqual> can be probed with a
 *          std::string_view without allocating a temporary key, while the
 *          stored keys keep their original spelling for faithful round-trip
 *          output.
 *
 * @see Legacy reference: src/legacy/engine/hash.c — UCHAR, samestr(), hash()
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_STRING_CASE_HPP
#define OPENSWMM_ENGINE_STRING_CASE_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace openswmm {

/// ASCII-only uppercase fold; mirrors legacy hash.c UCHAR exactly.
inline constexpr char ci_upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c & ~32) : c;
}

/// Case-insensitive equality (ASCII fold), matching legacy samestr().
inline bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (ci_upper(a[i]) != ci_upper(b[i])) return false;
    return true;
}

/// Transparent case-insensitive hash (FNV-1a over the uppercase fold).
struct CiHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
        std::uint64_t h = 14695981039346656037ull;
        for (char c : s) {
            h ^= static_cast<std::uint64_t>(
                static_cast<unsigned char>(ci_upper(c)));
            h *= 1099511628211ull;
        }
        return static_cast<std::size_t>(h);
    }
};

/// Transparent case-insensitive equality for unordered containers.
struct CiEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return ieq(a, b);
    }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_STRING_CASE_HPP */
