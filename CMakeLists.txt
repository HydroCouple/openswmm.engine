#
# CMakeLists.txt - CMake configuration file for OpenSWMMCore
#
# Created:  July 11, 2019
# Last Modified:  January 07, 2026
#
# Original Creators: Michael E. Tryby
#                    US EPA ORD/CESER
#
#                    Caleb Buahin (Modified)
#                    US EPA ORD/CESER 
#


cmake_minimum_required(VERSION 3.21...4.2)

# Prevent in-source builds (Keep)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there.")
endif()

# Modern project declaration with metadata
project(
    OpenSWMMCore
    VERSION 5.3.0
    DESCRIPTION "OpenSWMM Core - Storm Water Management Model"
    HOMEPAGE_URL "https://hydrocouple.org/projects/openswmmcore"
    LANGUAGES C CXX
)

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
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Modern feature options with namespacing
option(OPENSWMMCORE_BUILD_TESTS "Build tests" OFF)
option(OPENSWMMCORE_INSTALL "Install OpenSWMMCore" ON)

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


# Add subdirectories
add_subdirectory(src)

if(OPENSWMMCORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Modern package configuration
if(OPENSWMMCORE_INSTALL)
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
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OpenSWMMCore
    )

    install(
        EXPORT OpenSWMMCoreTargets
        FILE OpenSWMMCoreTargets.cmake
        NAMESPACE OpenSWMMCore::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OpenSWMMCore
    )
endif()

# Simplified CPack (remove unnecessary variables)
set(CPACK_PACKAGE_VENDOR "https://hydrocouple.org")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
include(CPack)
