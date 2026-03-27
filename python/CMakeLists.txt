# ============================================================================
# CMake entry point for the openswmm Python package (scikit-build)
#
# When building via `pip install .` or `python -m build`, scikit-build
# invokes this file.  It:
#   1. Adds the parent C/C++ project as a subdirectory so all library
#      targets and their ALIAS names are available directly.
#   2. Finds Python, Cython, and PythonExtensions.
#   3. Delegates to openswmm/CMakeLists.txt for the Cython extensions.
# ============================================================================

cmake_minimum_required(VERSION 3.21)

project(
    openswmm-python
    VERSION 6.0.0
    LANGUAGES C CXX
)

# ============================================================================
# 1. Build the parent C/C++ project as a subdirectory
# ============================================================================
# Using add_subdirectory (instead of a separate configure/build/install +
# find_package) keeps all ALIAS targets available (openswmm::engine,
# openswmm::legacy::engine, etc.) and avoids export/install mismatches.
set(OPENSWMM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OPENSWMM_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(OPENSWMM_BUILD_REGRESSION_TESTS OFF CACHE BOOL "" FORCE)
set(OPENSWMM_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(OPENSWMM_BUILD_PYTHON OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_BINARY_DIR}/parent_build"
    EXCLUDE_FROM_ALL
)

# ============================================================================
# 2. CMake policies & settings
# ============================================================================
cmake_policy(SET CMP0148 OLD)   # FindPythonInterp / FindPythonLibs compat
set(CMAKE_MACOSX_RPATH ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================================
# 3. Find Python + Cython toolchain
# ============================================================================
find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(PythonExtensions REQUIRED)
find_package(Cython REQUIRED)

# scikit-build module suffix
if(SKBUILD)
    message(STATUS "Building with scikit-build (SOABI=${SKBUILD_SOABI})")
    if(WIN32)
        set(CMAKE_SHARED_MODULE_SUFFIX ".${SKBUILD_SOABI}.pyd")
    else()
        set(CMAKE_SHARED_MODULE_SUFFIX ".${SKBUILD_SOABI}${CMAKE_SHARED_MODULE_SUFFIX}")
    endif()
endif()

# ============================================================================
# 4. System runtime libraries (vcruntime, OpenMP DLLs on Windows)
# ============================================================================
set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
include(InstallRequiredSystemLibraries)

# ============================================================================
# 5. Cython extensions
# ============================================================================
add_subdirectory(openswmm)
