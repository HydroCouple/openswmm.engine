#
# CMakeLists.txt - CMake configuration file for swmm-solver
#
# Created: July 11, 2019
# Modified: Aug 16, 2022
#
# Author: Michael E. Tryby
#         US EPA ORD/CESER
#


# NOTE (2026-08-16): raised 3.13 -> 3.21. CMakePresets.json schema v3 (added by
# this branch, mirroring openswmm.engine) requires CMake >= 3.21, and 3.13 is
# deprecated under CMake 4.x.
cmake_minimum_required (VERSION 3.21)

if("${CMAKE_BINARY_DIR}" STREQUAL "${CMAKE_SOURCE_DIR}")
    message(FATAL_ERROR "In-source builds are disabled.")
endif()


project(swmm-solver
    VERSION 5.2.4
    LANGUAGES C CXX
)

# NOTE (2026-08-16): upstream appended ${PROJECT_SOURCE_DIR}/cmake to
# CMAKE_MODULE_PATH here, but that directory did not exist in stock v5.2.4 so
# the line was a no-op. It must STAY off the module path now that the directory
# holds cmake/FindOpenMP.cmake (copied verbatim from openswmm.engine): on the
# module path that file shadows CMake's builtin FindOpenMP module, and since
# its body is Apple-only, Linux/Windows would "find" nothing and silently link
# no OpenMP runtime. It is include()d by full path, APPLE-only, instead.

# Sets the position independent code property for all targets
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Sets default install prefix when cmakecache is initialized for first time
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/install CACHE PATH "..." FORCE)
endif()


# Define install locations (will be prepended by install prefix)
set(TOOL_DIST "bin")
set(INCLUDE_DIST "include")
set(LIBRARY_DIST "lib")
set(CONFIG_DIST  "cmake")


# Define build options
option(BUILD_TESTS "Builds component tests (requires GoogleTest)" OFF)
option(BUILD_DEF   "Builds library with def file interface" OFF)

# OpenMP is REQUIRED by default rather than best-effort. The solver parallelises
# dynamic-wave routing (src/solver/dynwave.c) and sizes its thread pool from
# omp_get_max_threads() (src/solver/project.c), so a silently OpenMP-less build
# still runs but is serial -- which would invalidate any performance comparison
# against 5.3.0 / v6. Failing configure is preferable to shipping a quietly
# serial binary. Set -DSWMM_WITH_OPENMP=OFF to opt out deliberately.
option(SWMM_WITH_OPENMP "Build with OpenMP support (REQUIRED when ON)" ON)

if(SWMM_WITH_OPENMP)
    # macOS/AppleClang ships no discoverable OpenMP runtime. Same strategy as
    # openswmm.engine (src/engine/CMakeLists.txt): include the Homebrew shim —
    # cmake/FindOpenMP.cmake, copied verbatim from that repo — by FULL PATH on
    # APPLE only, then run the builtin find_package(OpenMP). The file must
    # never be reachable via CMAKE_MODULE_PATH (see the note at the top of
    # this file); by full path it cannot shadow the builtin module elsewhere.
    if(APPLE)
        include(${PROJECT_SOURCE_DIR}/cmake/FindOpenMP.cmake)
        # Not REQUIRED on APPLE: the shim may be the only source of the
        # imported targets, and the builtin probe can legitimately fail even
        # though the shim succeeded. The TARGET assertion below is the gate.
        find_package(OpenMP)
    else()
        find_package(OpenMP REQUIRED)
    endif()

    # Belt-and-braces: prove the imported target actually exists. A find module
    # that "succeeds" without publishing the target is exactly how this project
    # previously ended up linking no OpenMP runtime at all.
    if(NOT TARGET OpenMP::OpenMP_C)
        message(FATAL_ERROR
            "find_package(OpenMP) succeeded but OpenMP::OpenMP_C was not defined. "
            "Refusing to produce a silently serial build. Configure with "
            "-DSWMM_WITH_OPENMP=OFF to build serially on purpose.")
    endif()

    message(STATUS "OpenMP C found: version ${OpenMP_C_VERSION}, flags ${OpenMP_C_FLAGS}")
else()
    message(WARNING
        "SWMM_WITH_OPENMP=OFF: building a serial solver. Timings from this "
        "build are NOT comparable with OpenMP-enabled 5.3.0 / v6 builds.")
endif()


# Add project subdirectories
add_subdirectory(src/outfile)
add_subdirectory(src/solver)
add_subdirectory(src/run)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()


# Create install rules for vcruntime.dll, msvcp.dll, vcomp.dll etc.
set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
include(InstallRequiredSystemLibraries)


# Configure CPack driven installer package
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_VENDOR "US_EPA")
set(CPACK_ARCHIVE_FILE_NAME "swmm")

include(CPack)
