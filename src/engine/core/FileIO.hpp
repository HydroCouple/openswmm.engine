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

#include <cstdio>
#include <filesystem>
#include <string>

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
