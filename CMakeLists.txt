#
# CMakeLists.txt - CMake configuration file for OpenSWMM Engine
#
# Project:     openswmm.engine
# Description: Open Storm Water Management Model Engine
# Version:     6.0.0-alpha.1
#
# This is the root CMake file for the openswmm.engine project which contains:
#   - openswmm_legacy_engine : Original EPA SWMM 5.x C solver (preserved unmodified)
#   - openswmm_legacy_output : Original EPA SWMM 5.x binary output reader
#   - openswmm_engine        : New refactored C++20 engine with plugin I/O
#   - openswmm_plugin_sdk    : Header-only plugin development kit
#
# Build system: CMake 3.21+ with vcpkg for dependency management
# Test framework: Google Test + Google Benchmark
#
# See docs/MASTER_IMPLEMENTATION_PLAN.md for the full refactoring roadmap.
#

cmake_minimum_required(VERSION 3.21...4.2)

# Prevent in-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there.")
endif()

# Modern project declaration with metadata
project(
    openswmm
    VERSION 6.0.0
    DESCRIPTION "Open Storm Water Management Model Engine"
    HOMEPAGE_URL "https://hydrocouple.org/projects/openswmm"
    LANGUAGES C CXX
)

# Pre-release label (e.g. "alpha.1", "beta.1", "rc.1"). Leave empty for a final release.
set(OPENSWMM_PRERELEASE "alpha.1" CACHE STRING "Pre-release tag (e.g. alpha.1, beta.1, rc.1). Empty for a final release.")

if(OPENSWMM_PRERELEASE)
    set(OPENSWMM_FULL_VERSION "${PROJECT_VERSION}-${OPENSWMM_PRERELEASE}")
else()
    set(OPENSWMM_FULL_VERSION "${PROJECT_VERSION}")
endif()

# Backwards compatibility aliases for downstream consumers
set(OPENSWMMCORE_PRERELEASE    "${OPENSWMM_PRERELEASE}")
set(OPENSWMMCORE_FULL_VERSION  "${OPENSWMM_FULL_VERSION}")

message(STATUS "OpenSWMM Engine version: ${OPENSWMM_FULL_VERSION}")

# Modern build type handling
get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT isMultiConfig)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Essential global settings
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Modern feature options with namespacing
option(OPENSWMM_BUILD_TESTS       "Build all tests (unit + regression). Convenience flag." ON)
option(OPENSWMM_BUILD_UNIT_TESTS  "Build unit tests only" OFF)
option(OPENSWMM_BUILD_REGRESSION_TESTS "Build regression tests only" OFF)
option(OPENSWMM_BUILD_BENCHMARKS  "Build Google Benchmark performance tests" OFF)
option(OPENSWMM_INSTALL           "Install openswmm libraries" ON)
option(OPENSWMM_BUILD_PYTHON      "Build Python bindings" OFF)
option(OPENSWMM_WITH_GEOPACKAGE   "Build GeoPackage I/O library (requires sqlite3)" OFF)
option(OPENSWMM_BUILD_2D          "Build optional 2D surface routing module (requires SUNDIALS)" ON)

# OPENSWMM_BUILD_TESTS is a convenience flag that enables both unit and regression
if(OPENSWMM_BUILD_TESTS)
    set(OPENSWMM_BUILD_UNIT_TESTS ON CACHE BOOL "" FORCE)
    set(OPENSWMM_BUILD_REGRESSION_TESTS ON CACHE BOOL "" FORCE)
endif()

# Include essential modules
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(GenerateExportHeader)

# Build information (improved)
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OPENSWMMCORE_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RESULT
    )
    if(NOT GIT_RESULT EQUAL 0)
        set(OPENSWMMCORE_GIT_HASH "unknown")
    endif()
else()
    set(OPENSWMMCORE_GIT_HASH "unknown")
endif()

# Create interface library for global headers (backwards compatible alias kept)
add_library(openswmmcore INTERFACE)
add_library(OpenSWMMCore::openswmmcore ALIAS openswmmcore)
add_library(openswmm::common ALIAS openswmmcore)

# Configure global version header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/version.h
)

# Set include directories for the common interface
target_include_directories(openswmmcore
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Install generated headers
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/include/version.h
    DESTINATION
        ${CMAKE_INSTALL_INCLUDEDIR}/openswmm
)

# Install all public headers
install(
    DIRECTORY include/openswmm
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# Add testing subdirectories conditionally
if(OPENSWMM_BUILD_UNIT_TESTS OR OPENSWMM_BUILD_REGRESSION_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()

# Add benchmarks subdirectory conditionally
if(OPENSWMM_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
    add_subdirectory(tests/benchmarks)
endif()

# Modern package configuration
if(OPENSWMM_INSTALL OR OPENSWMMCORE_INSTALL)
    configure_package_config_file(
        cmake/OpenSWMMCoreConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/OpenSWMMCoreConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OpenSWMMCore
        PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/OpenSWMMCoreConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion  # ✅ More appropriate than AnyNewerVersion
    )

    install(
        FILES
            ${CMAKE_CURRENT_BINARY_DIR}/OpenSWMMCoreConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/OpenSWMMCoreConfigVersion.cmake
        DESTINATION
            ${CMAKE_INSTALL_LIBDIR}/cmake/OpenSWMMCore
    )

    install(
        EXPORT OpenSWMMCoreTargets
        FILE OpenSWMMCoreTargets.cmake
        NAMESPACE OpenSWMMCore::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OpenSWMMCore
    )

    install(
        TARGETS openswmmcore
        EXPORT OpenSWMMCoreTargets
    )
endif()

# Add subdirectories
add_subdirectory(src)

# Optional Python bindings (Cython extensions compiled via scikit-build).
# Typically invoked indirectly by `pip install ./python`, but can also be
# enabled as part of a top-level CMake build:
#   cmake -DOPENSWMM_BUILD_PYTHON=ON ..
if(OPENSWMM_BUILD_PYTHON)
    # Discover Python and extension-building dependencies without making them
    # hard requirements for a plain C/C++ build. If any of these packages are
    # missing, the Python bindings will be skipped with a warning instead of
    # causing CMake configuration to fail.
    find_package(Python3 QUIET COMPONENTS Interpreter Development.Module)
    find_package(PythonExtensions QUIET)
    find_package(Cython QUIET)

    if(Python3_FOUND AND PythonExtensions_FOUND AND Cython_FOUND)
        add_subdirectory(python/openswmm)
    else()
        message(WARNING
            "OPENSWMM_BUILD_PYTHON is enabled, but required Python build "
            "dependencies (Python3, PythonExtensions, Cython) were not found. "
            "Skipping build of Python bindings."
        )
    endif()
endif()

# Simplified CPack
set(CPACK_PACKAGE_NAME "openswmm-engine")
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "openswmm-engine-${OPENSWMM_FULL_VERSION}")
set(CPACK_PACKAGE_VENDOR "https://hydrocouple.org")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
include(CPack)
