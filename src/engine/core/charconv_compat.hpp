/**
 * @file charconv_compat.hpp
 * @brief Portable from_chars for floating-point types.
 *
 * @details Apple libc++ does not support std::from_chars for floating-point
 *          types.  This header provides a drop-in wrapper that uses std::strtod
 *          as a fallback when the standard library lacks the feature.
 *
 * @ingroup engine_core
 */

#ifndef OPENSWMM_CHARCONV_COMPAT_HPP
#define OPENSWMM_CHARCONV_COMPAT_HPP

#include <charconv>
#include <cstdlib>
#include <system_error>
#include <string>

namespace openswmm {

/**
 * @brief Locale-independent parse of a double from a character range.
 *
 * On toolchains that support std::from_chars for doubles this forwards
 * directly; otherwise it falls back to std::strtod via a null-terminated
 * temporary copy.
 */
inline std::from_chars_result from_chars_double(const char* first,
                                                const char* last,
                                                double& value) noexcept {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    return std::from_chars(first, last, value);
#else
    if (first == last) {
        return {first, std::errc::invalid_argument};
    }
    // std::strtod needs a null-terminated string
    std::string tmp(first, last);
    char* end = nullptr;
    double v = std::strtod(tmp.c_str(), &end);
    if (end == tmp.c_str()) {
        return {first, std::errc::invalid_argument};
    }
    value = v;
    return {first + (end - tmp.c_str()), std::errc{}};
#endif
}

} // namespace openswmm

#endif // OPENSWMM_CHARCONV_COMPAT_HPP
