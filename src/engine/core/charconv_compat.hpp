/**
 * @file charconv_compat.hpp
 * @brief Portable from_chars for floating-point types.
 *
 * @details Apple libc++ does not support std::from_chars for floating-point
 *          types.  This header provides a drop-in wrapper that uses strtod
 *          as a fallback when the standard library lacks the feature.
 *
 *          Every numeric token in every `.inp` section goes through here, so
 *          the fallback is on the hottest path in the whole load sequence —
 *          on a 500k-element model with geometry, parsing is ~80% of open().
 *          See plans/MODEL_LOAD_INIT_OPTIMIZATION_PLAN_2026-08-12.md item 2.1.
 *
 * @ingroup engine_core
 */

#ifndef OPENSWMM_CHARCONV_COMPAT_HPP
#define OPENSWMM_CHARCONV_COMPAT_HPP

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <string>

#if !defined(__cpp_lib_to_chars) || __cpp_lib_to_chars < 201611L
// The fallback path is only compiled where float from_chars is missing, which
// in practice means Apple libc++. strtod_l lives in <xlocale.h> there and in
// <locale.h> on glibc; both provide newlocale/freelocale from POSIX.2008.
#  define OPENSWMM_CHARCONV_FALLBACK 1
#  include <locale.h>
#  if defined(__APPLE__)
#    include <xlocale.h>
#  endif
#endif

namespace openswmm {

#ifdef OPENSWMM_CHARCONV_FALLBACK
namespace detail {

/**
 * @brief Process-wide "C" locale handle for strtod_l.
 *
 * @details Plain strtod consults the current global locale on every call to
 *          find the decimal separator. Pinning the C locale removes that work
 *          and, more importantly, makes the conversion locale-independent —
 *          which is what std::from_chars guarantees on the platforms that take
 *          the other branch. Without it a host running under a comma-decimal
 *          locale would parse "0.013" as 0.
 *
 *          Constructed once on first use. If newlocale() fails the handle is
 *          null and the call sites fall back to plain strtod.
 */
inline locale_t c_locale() noexcept {
    static locale_t loc = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    return loc;
}

}  // namespace detail
#endif

/**
 * @brief Locale-independent parse of a double from a character range.
 *
 * On toolchains that support std::from_chars for doubles this forwards
 * directly; otherwise it converts via strtod_l against a cached C locale,
 * using a stack buffer for the null termination strtod requires.
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

    // strtod needs a null-terminated string. Numeric tokens in a .inp are
    // short; a stack buffer keeps the common case free of any std::string
    // construction at all. Anything longer than the buffer cannot be a
    // meaningful double anyway, but is still handled correctly via the heap
    // path rather than being truncated.
    const std::size_t n = static_cast<std::size_t>(last - first);
    char        stack_buf[64];
    std::string heap_buf;
    const char* cstr = nullptr;
    if (n < sizeof(stack_buf)) {
        std::memcpy(stack_buf, first, n);
        stack_buf[n] = '\0';
        cstr = stack_buf;
    } else {
        heap_buf.assign(first, last);
        cstr = heap_buf.c_str();
    }

    char*  end = nullptr;
    double v   = 0.0;
    if (const locale_t loc = detail::c_locale(); loc != nullptr) {
        v = strtod_l(cstr, &end, loc);
    } else {
        v = std::strtod(cstr, &end);
    }

    if (end == cstr) {
        return {first, std::errc::invalid_argument};
    }
    value = v;
    return {first + (end - cstr), std::errc{}};
#endif
}

} // namespace openswmm

#endif // OPENSWMM_CHARCONV_COMPAT_HPP
