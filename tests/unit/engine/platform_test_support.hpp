/**
 * @file platform_test_support.hpp
 * @brief POSIX shims for the unit tests that MSVC does not provide.
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
inline int  dupFd(int fd)                           { return _dup(fd); }
inline int  dup2Fd(int from, int to)                { return _dup2(from, to); }
inline int  closeFd(int fd)                         { return _close(fd); }
inline int  fileNo(FILE* f)                         { return _fileno(f); }
inline void setEnvVar(const char* n, const char* v) { _putenv_s(n, v); }
inline void unsetEnvVar(const char* n)              { _putenv_s(n, ""); }
#else
inline int  dupFd(int fd)                           { return dup(fd); }
inline int  dup2Fd(int from, int to)                { return dup2(from, to); }
inline int  closeFd(int fd)                         { return close(fd); }
inline int  fileNo(FILE* f)                         { return fileno(f); }
inline void setEnvVar(const char* n, const char* v) { setenv(n, v, 1); }
inline void unsetEnvVar(const char* n)              { unsetenv(n); }
#endif

}  // namespace plattest

#endif  // OPENSWMM_TESTS_PLATFORM_TEST_SUPPORT_HPP
