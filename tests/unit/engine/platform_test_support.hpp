/**
 * @file      platform_test_support.hpp
 * @brief     POSIX shims for the unit tests that MSVC does not provide.
 *
 * @details A handful of gates drive the engine through environment variables
 *          (OPENSWMM_2D_BACKEND) or capture stderr to prove a silent fallback
 *          did not happen. Both need POSIX calls that MSVC either spells with
 *          a leading underscore (`_dup`, `_dup2`, `_close`, `_fileno`) or does
 *          not ship at all (`setenv` / `unsetenv`, for which `_putenv_s` with
 *          an empty value is the documented removal form).
 *
 *          Inline functions rather than `#define close _close`: a macro on a
 *          name that common would rewrite `swmm_engine_close` and every
 *          `stream.close()` in the including translation unit, which is how
 *          this kind of shim usually goes wrong.
 *
 * @ingroup engine_tests
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_TESTS_PLATFORM_TEST_SUPPORT_HPP
#define OPENSWMM_TESTS_PLATFORM_TEST_SUPPORT_HPP

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace plattest {

#ifdef _WIN32
/**
 * @brief Duplicate a native file descriptor for fixture stream restoration.
 *
 * Uses the platform dup/_dup spelling; duplicate refers to the same underlying open-file
 * description/position.
 *
 * @param[in] fd Open descriptor to duplicate; caller retains the original.
 * @return New owned descriptor to close, or negative on native failure.
 */
inline int dupFd(int fd) {
    return _dup(fd);
}
/**
 * @brief Redirect one native descriptor to the source open-file description.
 *
 * Ownership of from remains with caller; destination remains caller-managed and shares file
 * position on success.
 *
 * @param[in] from Borrowed open source descriptor.
 * @param[in] to Destination descriptor number, replaced/closed as required by the native API.
 * @return Native dup2 result on POSIX or _dup2 result on Windows; negative denotes failure.
 */
inline int dup2Fd(int from, int to) {
    return _dup2(from, to);
}
/**
 * @brief Close a fixture-owned native descriptor.
 *
 * Does not clear caller variables or retry errors; descriptor state after a close error is
 * platform-dependent.
 *
 * @param[in] fd Owned descriptor being released.
 * @return Native close/_close status, zero on success and negative on error.
 */
inline int closeFd(int fd) {
    return _close(fd);
}
/**
 * @brief Borrow the native descriptor underlying a C stream.
 *
 * Does not duplicate or transfer ownership; do not independently close it while the stream remains
 * active.
 *
 * @param[in] f Non-null live FILE stream.
 * @return Borrowed descriptor from fileno/_fileno, possibly negative on error.
 */
inline int fileNo(FILE* f) {
    return _fileno(f);
}
/**
 * @brief Set a process environment value through the platform runtime.
 *
 * Overwrites existing values and ignores native status. Mutates process-wide state; tests must
 * serialize environment changes and restore any previous value themselves. Strings are copied by
 * the runtime, not retained by this helper.
 *
 * @param[in] n Non-null valid environment variable name.
 * @param[in] v Non-null value string; an empty value removes the variable on Windows but stores an
 *     empty value on POSIX.
 */
inline void setEnvVar(const char* n, const char* v) {
    _putenv_s(n, v);
}
/**
 * @brief Remove a variable from the process environment.
 *
 * Uses unsetenv on POSIX or empty _putenv_s value on Windows; ignores status. Tests serialize
 * global environment mutations and restore prior state when required.
 *
 * @param[in] n Non-null valid variable name.
 */
inline void unsetEnvVar(const char* n) {
    _putenv_s(n, "");
}
#else
/**
 * @brief Duplicate a native file descriptor for fixture stream restoration.
 *
 * Uses the platform dup/_dup spelling; duplicate refers to the same underlying open-file
 * description/position.
 *
 * @param[in] fd Open descriptor to duplicate; caller retains the original.
 * @return New owned descriptor to close, or negative on native failure.
 */
inline int dupFd(int fd) {
    return dup(fd);
}
/**
 * @brief Redirect one native descriptor to the source open-file description.
 *
 * Ownership of from remains with caller; destination remains caller-managed and shares file
 * position on success.
 *
 * @param[in] from Borrowed open source descriptor.
 * @param[in] to Destination descriptor number, replaced/closed as required by the native API.
 * @return Native dup2 result on POSIX or _dup2 result on Windows; negative denotes failure.
 */
inline int dup2Fd(int from, int to) {
    return dup2(from, to);
}
/**
 * @brief Close a fixture-owned native descriptor.
 *
 * Does not clear caller variables or retry errors; descriptor state after a close error is
 * platform-dependent.
 *
 * @param[in] fd Owned descriptor being released.
 * @return Native close/_close status, zero on success and negative on error.
 */
inline int closeFd(int fd) {
    return close(fd);
}
/**
 * @brief Borrow the native descriptor underlying a C stream.
 *
 * Does not duplicate or transfer ownership; do not independently close it while the stream remains
 * active.
 *
 * @param[in] f Non-null live FILE stream.
 * @return Borrowed descriptor from fileno/_fileno, possibly negative on error.
 */
inline int fileNo(FILE* f) {
    return fileno(f);
}
/**
 * @brief Set a process environment value through the platform runtime.
 *
 * Overwrites existing values and ignores native status. Mutates process-wide state; tests must
 * serialize environment changes and restore any previous value themselves. Strings are copied by
 * the runtime, not retained by this helper.
 *
 * @param[in] n Non-null valid environment variable name.
 * @param[in] v Non-null value string; an empty value removes the variable on Windows but stores an
 *     empty value on POSIX.
 */
inline void setEnvVar(const char* n, const char* v) {
    setenv(n, v, 1);
}
/**
 * @brief Remove a variable from the process environment.
 *
 * Uses unsetenv on POSIX or empty _putenv_s value on Windows; ignores status. Tests serialize
 * global environment mutations and restore prior state when required.
 *
 * @param[in] n Non-null valid variable name.
 */
inline void unsetEnvVar(const char* n) {
    unsetenv(n);
}
#endif

} // namespace plattest

#endif // OPENSWMM_TESTS_PLATFORM_TEST_SUPPORT_HPP
