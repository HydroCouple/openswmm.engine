/**
 * @file PathResolver.hpp
 * @brief Pure-function path normalisation and relative-path math.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
    Relative            = 0,  ///< Expressible relative to anchor; portable.
    AbsoluteSameVolume  = 1,  ///< Different sub-tree, same root/drive; not portable
                              ///< across folder layouts but still on the same volume.
    AbsoluteCrossVolume = 2,  ///< Different drive / UNC / mount point; not portable
                              ///< across machines without identical mounts.
    Invalid             = 3   ///< Empty or malformed input.
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
 *          variants, `path` is the platform-canonical absolute form
 *          (forward-slash separated for `.inp` portability).
 *
 *          `warning` is non-empty whenever `classification != Relative`,
 *          carrying a short human-readable explanation for the GUI to
 *          surface to the user.
 */
struct RelativeResult {
    std::string path;
    PathClass   classification = PathClass::Invalid;
    int         up_levels      = 0;
    std::string warning;
};

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Compute `target_absolute` expressed relative to `anchor_dir`.
 *
 * @details Uses `std::filesystem::path::lexically_proximate` rather than
 *          `relative` so symlinks are preserved and missing files do not
 *          throw. The output `path` always uses forward-slash separators
 *          regardless of platform.
 *
 *          Classification rules:
 *          - **Invalid**: either input is empty.
 *          - **AbsoluteCrossVolume**: the two paths have different
 *            `root_name()` (different Windows drive letter, different UNC
 *            share). The absolute `target` is returned unchanged (modulo
 *            separator normalisation).
 *          - **AbsoluteSameVolume**: the relative form would require more
 *            than `max_up_levels` `..` segments, OR `anchor_dir` is empty
 *            (no anchor → return target as-is). The absolute `target` is
 *            returned.
 *          - **Relative**: the relative form is well-formed and within
 *            the depth cap.
 *
 *          The depth cap defends against pathological cases like a `.inp`
 *          at `/Users/alice/...` referencing `/etc/swmm/data/` — the
 *          relative form `../../../../../../etc/swmm/data/` is technically
 *          valid but unreadable.
 *
 * @param target_absolute  The path to express relatively. Must be absolute.
 * @param anchor_dir       The directory the relative form is anchored to.
 *                         May be a file path; the parent directory is used.
 * @param max_up_levels    Maximum number of leading `..` segments before
 *                         falling back to absolute. Default 16.
 * @returns RelativeResult with the path + classification + diagnostics.
 */
RelativeResult makeRelative(const std::string& target_absolute,
                            const std::string& anchor_dir,
                            int                max_up_levels = 16);

/**
 * @brief Resolve a possibly-relative token against an anchor directory.
 *
 * @details Mirrors the legacy `addAbsolutePath()` semantics:
 *          - Empty `stored_token` returns empty (no error).
 *          - Absolute `stored_token` (drive letter, leading `/`, UNC) is
 *            returned unchanged, separators normalised to the platform
 *            preferred form.
 *          - Relative `stored_token` is joined onto `anchor_dir` and
 *            normalised. If `anchor_dir` is empty, the token is returned
 *            unchanged (caller's responsibility to provide an anchor when
 *            relative tokens are expected).
 *
 *          Accepts both `/` and `\` separators on input. Returns a path
 *          in the platform's preferred form for direct use with `fopen()`.
 *
 * @param stored_token  The token as it appeared in the `.inp` file.
 * @param anchor_dir    Directory to resolve against (typically the `.inp`'s
 *                      parent directory).
 * @returns Absolute path ready for filesystem operations, or empty.
 */
std::string resolveRelative(const std::string& stored_token,
                            const std::string& anchor_dir);

/**
 * @brief Convert all backslashes to forward-slashes; collapse `./` and
 *        duplicate slashes; drop trailing slash (except for filesystem
 *        roots like `/` or `C:/`).
 *
 * @details Idempotent. Does not touch `..` segments (use makeRelative for
 *          full lexical normalisation). Used by the InpWriter when emitting
 *          path tokens so the `.inp` file is platform-portable.
 *
 * @param path  Any path-like string.
 * @returns Same logical path with normalised separators.
 */
std::string normaliseSeparators(const std::string& path);

/**
 * @brief Parent directory portion of `file_path`, canonicalised for use as
 *        an anchor to `makeRelative` / `resolveRelative`.
 *
 * @details Returns the directory portion in the platform's preferred form,
 *          without trailing separator. If `file_path` has no parent
 *          (it's a single filename), returns empty. If `file_path` is
 *          itself a directory path, returns it unchanged (modulo trailing
 *          slash removal).
 *
 * @param file_path  Path to a file or directory.
 * @returns Parent directory, or empty when none.
 */
std::string parentDir(const std::string& file_path);

// ============================================================================
// Test hooks — exposed for unit tests; not part of the supported API.
// ============================================================================

/**
 * @brief True iff `path` is absolute on the current platform.
 *
 * @details Treats `\\server\share\…` (UNC) and `C:\…` (Windows drive) as
 *          absolute even on POSIX builds — the legacy `.inp` format may
 *          carry such tokens regardless of the platform the engine runs
 *          on.
 */
bool isAbsolutePath(const std::string& path) noexcept;

} // namespace openswmm::io

#endif // OPENSWMM_ENGINE_PATH_RESOLVER_HPP
