/**
 * @file      FileIO.hpp
 * @brief     Opening files by UTF-8 path on every platform.
 *
 * @details Paths reach the engine as UTF-8 `std::string` — that is what the
 *          GUI hands over (`QString::toUtf8`), what the C API documents, and
 *          what a UTF-8 `.inp` carries in `[FILES]`.
 *
 *          On POSIX a path is an opaque byte string, so UTF-8 bytes go
 *          straight through. On Windows they do not: the narrow `char*` file
 *          APIs — `std::fopen`, and the `const char*` `fstream` constructors —
 *          interpret their argument in the process's ACTIVE ANSI CODE PAGE
 *          (CP936/GBK on a Chinese system, CP1252 on a Western one), never as
 *          UTF-8. A path such as
 *
 *              C:/…/模拟情景8/模拟情景[8].inp
 *
 *          is therefore mangled before it reaches the filesystem and the open
 *          fails with a plain "no such file" — issue #7.
 *
 *          `std::filesystem::path` is the fix because on Windows it stores
 *          `wchar_t` natively and the standard `fstream`/`fopen`-equivalent
 *          overloads that take a `path` use the wide APIs. The trap is that
 *          `path(const std::string&)` assumes the NATIVE NARROW encoding, so
 *          simply switching to `fs::path` fixes nothing; the input has to be
 *          declared as UTF-8, which is what `utf8_path()` below does.
 *
 * @ingroup engine_core
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_CORE_FILEIO_HPP
#define OPENSWMM_ENGINE_CORE_FILEIO_HPP

#include <concepts>
#include <cstdio>
#include <filesystem>
#include <string>
#include <type_traits>

namespace openswmm::io {

/**
 * @brief Convert borrowed path bytes into an owned native filesystem path.
 *
 * Windows conversion uses char8_t UTF-8 construction. Allocation/encoding exceptions propagate;
 * POSIX input may contain non-UTF-8 bytes.
 *
 * @param[in] utf8 Path bytes: interpreted as UTF-8 on Windows, passed through unchanged on POSIX.
 * @return Owned filesystem path; no file lookup, normalization or open is performed.
 */
inline std::filesystem::path utf8_path(const std::string& utf8) {
#ifdef _WIN32
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
#else
    return std::filesystem::path(utf8);
#endif
}

/**
 * @brief Passing an `fs::path` to `utf8_path()` is always a mistake — refuse it
 *        at compile time, on EVERY platform.
 *
 * The value is already a path; re-encoding it is at best a no-op and at worst
 * the very corruption this header exists to prevent. The reason this needs a
 * deleted overload rather than a code review is that the mistake COMPILES ON
 * POSIX: `fs::path::string_type` is `std::string` there, so `path` converts
 * implicitly to the `const std::string&` parameter. On Windows `string_type`
 * is `std::wstring`, no such conversion exists, and it is a hard error — so a
 * Linux/macOS build and a local `-fsyntax-only` check both pass and only the
 * MSVC job fails. That cost two Windows CI cycles (b3b1590d → b814bcf8, then
 * again at 92e63366) before this guard existed.
 *
 * Constrained to match ONLY an actual path: a bare overload taking
 * `const fs::path&` would make `utf8_path("literal")` and `utf8_path(char_ptr)`
 * ambiguous, since converting `const char*` to `std::string` and to `path` are
 * equally good user-defined conversions. A template that matches `path`
 * exactly wins against the conversion to `std::string` and loses for
 * everything else, which is precisely the rule we want.
 */
template <class P>
    requires std::same_as<std::remove_cvref_t<P>, std::filesystem::path>
std::filesystem::path utf8_path(P&&) = delete;

/**
 * @brief Copy a native path into the engine byte-string representation.
 *
 * No filesystem lookup. Allocation/encoding exceptions propagate; use utf8_path() explicitly before
 * passing byte strings here.
 *
 * @param[in] p Borrowed native filesystem path, already constructed using the intended encoding.
 * @param[in] generic True selects generic slash separators, false native formatting.
 * @return Owned UTF-8 bytes on Windows; native byte-preserving string on POSIX.
 */
inline std::string path_utf8(const std::filesystem::path& p, bool generic = false) {
#ifdef _WIN32
    const std::u8string s = generic ? p.generic_u8string() : p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
#else
    // Mirror of utf8_path's POSIX branch: bytes through unchanged, never via
    // u8string, because a POSIX path need not be valid UTF-8.
    return generic ? p.generic_string() : p.string();
#endif
}

/**
 * @brief The mirror guard: `path_utf8()` takes a path, never a string.
 *
 * A `std::string` or `const char*` argument would silently construct the
 * `fs::path` parameter from the NATIVE NARROW encoding — the ANSI code page on
 * Windows — which is issue #7 itself, arrived at through the very helper meant
 * to prevent it, and again visible only on MSVC. Convert explicitly with
 * `utf8_path()` first if that is genuinely what was meant.
 */
template <class S>
    requires(!std::same_as<std::remove_cvref_t<S>, std::filesystem::path>)
std::string path_utf8(S&&, bool = false) = delete;

/**
 * @brief Open a borrowed native path using platform C stdio.
 *
 * Windows widens the ASCII mode and invokes _wfopen; POSIX calls fopen. Mode may truncate/create
 * the destination. C-runtime errors use errno; allocations for the Windows mode string may throw.
 *
 * @param[in] p Borrowed native filesystem path.
 * @param[in] mode Non-null NUL-terminated ASCII fopen mode; must be valid for the platform C
 *     runtime.
 * @return Owned FILE stream on success or nullptr for C-runtime open failure; caller closes with
 *     fclose().
 */
inline std::FILE* fopen_path(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    // _wfopen is the only fopen that takes a wide path. The mode is ASCII by
    // construction, so widening it character-by-character is exact.
    const std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
    return ::_wfopen(p.c_str(), wmode.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}

/**
 * @brief Convert engine path bytes and open a C stdio stream.
 *
 * Path conversion/allocation exceptions propagate. Opening can create/truncate according to mode.
 *
 * @param[in] utf8 Borrowed UTF-8 Windows path or native POSIX path bytes.
 * @param[in] mode Non-null NUL-terminated ASCII stdio mode, passed to fopen_path().
 * @return Owned FILE stream, or nullptr on open failure; caller must fclose() once.
 */
inline std::FILE* fopen_utf8(const std::string& utf8, const char* mode) {
    return fopen_path(utf8_path(utf8), mode);
}

} // namespace openswmm::io

#endif // OPENSWMM_ENGINE_CORE_FILEIO_HPP
