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
 * @file      PathResolver.hpp
 * @brief     Pure-function path normalisation and relative-path math.
 *
 * @details Foundation of the IO portability work (see
 *          openswmm.gui/docs/IO_PORTABILITY_PLAN.md §3.1). Three jobs:
 *
 *          1. Compute `target` expressed *relative to* an anchor directory,
 *             classifying the result so callers can fall back to absolute
 *             when relative is impossible (cross-volume, UNC, beyond depth
 *             cap).
 *          2. Resolve a stored token (relative or absolute) against an
 *             anchor directory, mirroring the legacy `addAbsolutePath()`
 *             semantics in `src/legacy/engine/swmm5.c:2820`.
 *          3. Normalise separators to forward-slash on write while
 *             accepting both `/` and `\` on read — the SWMM `.inp` text
 *             format is platform-neutral, the in-memory representation
 *             is platform-native.
 *
 *          All functions are pure, allocation-only, and thread-safe.
 *          They use `std::filesystem::path` for parsing but never throw
 *          on missing files (no `canonical()` calls).
 *
 * @ingroup engine_core
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_PATH_RESOLVER_HPP
#define OPENSWMM_ENGINE_PATH_RESOLVER_HPP

#include <string>

namespace openswmm::io {

// ============================================================================
// PathClass — outcome of a makeRelative() attempt
// ============================================================================

/**
 * @brief Classification of a makeRelative() result.
 *
 * @details Callers use this to decide whether the produced string is usable
 *          as a portable token in a `.inp` file. Only `Relative` is portable;
 *          the absolute variants are returned when the relative form would
 *          be invalid or worse than the absolute one.
 */
enum class PathClass : int {
    Relative = 0,            ///< Expressible relative to anchor; portable.
    AbsoluteSameVolume = 1,  ///< Different sub-tree, same root/drive; not portable
                             ///< across folder layouts but still on the same volume.
    AbsoluteCrossVolume = 2, ///< Different drive / UNC / mount point; not portable
                             ///< across machines without identical mounts.
    Invalid = 3              ///< Empty or malformed input.
};

// ============================================================================
// RelativeResult — full outcome of makeRelative()
// ============================================================================

/**
 * @brief Outcome of computing `target` relative to `anchor_dir`.
 *
 * @details `path` is always forward-slash-separated, has no trailing slash,
 *          and contains no redundant `./` segments. For `Relative`,
 *          `up_levels` counts the leading `..` segments. For the absolute
 *          variants, `path` is the lexically formatted absolute token
 *          (forward-slash separated for `.inp` portability).
 *
 *          `warning` is non-empty whenever `classification != Relative`,
 *          carrying a short human-readable explanation for the GUI to
 *          surface to the user.
 */
struct RelativeResult {
    std::string path;
    PathClass classification = PathClass::Invalid;
    int up_levels = 0;
    std::string warning;
};

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Compute a lexical relative token or return a classified absolute fallback.
 *
 * Pure lexical operation: does not resolve symlinks, check existence or detect POSIX mount
 * boundaries. Recognizes drive/UNC roots across hosts and compares the entire root
 * case-insensitively using C-locale lowercase. Empty anchor returns normalized target with
 * AbsoluteSameVolume for recognized absolute syntax, otherwise Invalid. Relative output uses
 * forward slashes and preserves authored relative geometry. Allocation/encoding exceptions
 * propagate.
 *
 * @param[in] target_absolute Borrowed target text expected absolute; an empty target is Invalid.
 *     Nonempty relative text is not independently rejected when an anchor exists.
 * @param[in] anchor_dir Borrowed directory token. It is always treated as a directory; no file-type
 *     detection or automatic parent extraction occurs.
 * @param[in] max_up_levels Maximum permitted leading parent segments, default 16; negative values
 *     force absolute fallback even for zero parent segments.
 * @return Owned path/classification/warning result; Relative sets up_levels, fallback results leave
 *     it zero.
 */
RelativeResult makeRelative(const std::string& target_absolute, const std::string& anchor_dir,
                            int max_up_levels = 16);

/**
 * @brief Join a stored token to a directory anchor using lexical normalization.
 *
 * No file access, symlink resolution or existence check. The result is absolute only if input
 * syntax or a suitable absolute anchor makes it so. Windows encoding/allocation errors can throw.
 *
 * @param[in] stored_token Borrowed path bytes; empty returns empty. Leading slash, alphabetic
 *     drive-colon or UNC syntax is considered already absolute.
 * @param[in] anchor_dir Borrowed directory anchor; empty leaves a normalized relative token rather
 *     than making it absolute.
 * @return Owned generic slash-form path bytes. Relative tokens are lexically normalized after
 *     joining; already absolute tokens only receive separator/path formatting.
 */
std::string resolveRelative(const std::string& stored_token, const std::string& anchor_dir);

/**
 * @brief Normalize separator and dot spelling without resolving parent segments.
 *
 * Text cleanup only: does not consult files, resolve parent segments or promise native canonical
 * form. Drive/UNC root-shaped strings receive special trailing-slash preservation.
 *
 * @param[in] path Borrowed path-like byte string; empty stays empty.
 * @return Owned string with backslashes changed to slash, repeated slash runs collapsed except
 *     leading UNC marker, and redundant slash-dot forms/trailing separators trimmed.
 */
std::string normaliseSeparators(const std::string& path);

/**
 * @brief Extract a directory token using normalized separator text.
 *
 * Does not inspect filesystem object type: an actual directory without a trailing separator is
 * treated like a filename. Recognized drive/root prefixes are preserved.
 *
 * @param[in] file_path Borrowed token. A trailing slash/backslash explicitly marks a directory;
 *     otherwise the final component is treated as a filename.
 * @return Owned normalized directory token, or empty for no parent/empty input.
 */
std::string parentDir(const std::string& file_path);

// ============================================================================
// Test hooks — exposed for unit tests; not part of the supported API.
// ============================================================================

/**
 * @brief Recognize platform-neutral absolute-looking path syntax.
 *
 * Drive-relative C:folder is deliberately classified as absolute-looking here, unlike native
 * Windows filesystem semantics. No filesystem lookup or validity checking.
 *
 * @param[in] path Borrowed byte string; no separator normalization is performed.
 * @return True for leading slash, alphabetic drive-colon prefix or two leading slash/backslash
 *     separators; false for empty/relative syntax.
 */
bool isAbsolutePath(const std::string& path) noexcept;

} // namespace openswmm::io

#endif // OPENSWMM_ENGINE_PATH_RESOLVER_HPP
