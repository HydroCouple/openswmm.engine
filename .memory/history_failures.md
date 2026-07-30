---
project: SWMM6_2
type: failure_log
---

<!-- Newest first. Write lesson: as a transferable warning, not a project-specific note. -->

## macOS 26 Beta — AGL.framework Removed from SDK | 2026-05-23
**context:** Linking QPropertyModel against Qt 6.7.3 on macOS 26 beta
**approach:** Used default macOS 26.5 SDK as sysroot
**result:** `ld: framework 'AGL' not found` — Qt 6.7.3 links -framework AGL which was removed from the 26 beta SDK
**lesson:** On macOS beta SDKs, legacy frameworks (AGL, OpenAL) may be missing. Set CMAKE_OSX_SYSROOT to the previous stable SDK (MacOSX15.4.sdk) as a sysroot to restore them.

## CMake 4.x — install(IMPORTED_RUNTIME_ARTIFACTS) on Static Libs | 2026-05-23
**context:** openswmm.engine CMakeLists.txt called install(IMPORTED_RUNTIME_ARTIFACTS SQLite::SQLite3)
**approach:** Ran cmake --preset Darwin with CMake 4.3.2
**result:** Hard error — CMake 4.x no longer silently ignores this call on static library targets
**lesson:** Always guard install(IMPORTED_RUNTIME_ARTIFACTS) with a target TYPE check. Use a local sibling checkout instead of FetchContent when you need to patch upstream source — FetchContent re-downloads on clean builds.

## macOS 26 Beta — Empty CLT C++ Headers Directory | 2026-05-23
**context:** vcpkg building proj/gdal/sundials on macOS 26 beta with Command Line Tools
**approach:** Used default CLT compiler without explicit SDK include path
**result:** `fatal error: 'memory' file not found` — CLT ships /usr/include/c++/v1/ empty; headers only in SDK
**lesson:** On macOS beta, run `clang++ -v /dev/null` to verify c++ include paths resolve to non-empty dirs. Fix: add -I$(xcrun --show-sdk-path)/usr/include/c++/v1 to compiler flags.
