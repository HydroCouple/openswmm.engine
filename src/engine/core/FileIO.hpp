/**
 * @file FileIO.hpp
 * @brief Opening files by UTF-8 path on every platform.
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
 * @brief Interpret a UTF-8 byte string as a filesystem path.
 *
 * On Windows the bytes are decoded as UTF-8 and re-encoded to the native
 * UTF-16. On POSIX they are passed through unchanged — deliberately NOT via
 * `u8string`, because a POSIX path is any byte sequence and need not be valid
 * UTF-8; round-tripping one through a UTF-8 decode would corrupt it.
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
 * @brief Render a path back to UTF-8 bytes — the inverse of `utf8_path()`.
 *
 * `path::string()` / `path::generic_string()` are the wrong tool for this on
 * Windows: they re-encode to the native NARROW form (the ANSI code page), so a
 * value that only passed THROUGH a `path` on its way from one UTF-8 string to
 * another comes out mangled, which is issue #7 with an extra step. Any code
 * that constructs a path with `utf8_path()` and then wants a `std::string`
 * again has to come back through here.
 *
 * @param p         The path.
 * @param generic   Use '/' separators (`generic_u8string`) rather than the
 *                  native ones — needed where the result is an .inp token or
 *                  is concatenated with '/' by hand.
 */
inline std::string path_utf8(const std::filesystem::path& p,
                             bool generic = false) {
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
    requires (!std::same_as<std::remove_cvref_t<S>, std::filesystem::path>)
std::string path_utf8(S&&, bool = false) = delete;

/**
 * @brief `std::fopen` for a path that is ALREADY a `std::filesystem::path`.
 *
 * Prefer this over `fopen_utf8(p.string(), …)` at such call sites: on Windows
 * `path::string()` re-encodes to the native NARROW form (the ANSI code page),
 * which throws away exactly the characters this header exists to preserve.
 *
 * @param p    The path.
 * @param mode An ordinary `fopen` mode string ("r", "wb", "w+b", …), ASCII.
 * @return The stream, or `nullptr` exactly as `std::fopen` would.
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

inline std::FILE* fopen_utf8(const std::string& utf8, const char* mode) {
    return fopen_path(utf8_path(utf8), mode);
}

}  // namespace openswmm::io

#endif  // OPENSWMM_ENGINE_CORE_FILEIO_HPP
