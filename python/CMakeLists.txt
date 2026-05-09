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
# 1. Engine library: find_package (CI pre-built) OR add_subdirectory (local dev)
# ============================================================================
# If OPENSWMM_ENGINE_INSTALL_PREFIX is set (via -D or env var), locate the
# pre-built engine via find_package and skip the full C++ source build.
# Otherwise fall back to add_subdirectory so `pip install .` from a fresh
# clone works unchanged.

if(NOT DEFINED OPENSWMM_ENGINE_INSTALL_PREFIX
        AND DEFINED ENV{OPENSWMM_ENGINE_INSTALL_PREFIX})
    set(OPENSWMM_ENGINE_INSTALL_PREFIX "$ENV{OPENSWMM_ENGINE_INSTALL_PREFIX}"
        CACHE PATH "Install prefix of a pre-built OpenSWMM engine")
endif()

if(OPENSWMM_ENGINE_INSTALL_PREFIX)
    # ── Pre-built mode (CI) ────────────────────────────────────────────────
    message(STATUS "openswmm-python: using pre-built engine from ${OPENSWMM_ENGINE_INSTALL_PREFIX}")
    set(OPENSWMM_USE_PREBUILT_ENGINE TRUE CACHE BOOL "" FORCE)

    find_package(OpenSWMMEngine CONFIG REQUIRED
        PATHS "${OPENSWMM_ENGINE_INSTALL_PREFIX}"
        NO_DEFAULT_PATH
    )

    # Bridge OpenSWMMEngine:: → openswmm:: ALIAS names used by all Cython
    # cmake files.  ALIAS of an IMPORTED target requires CMake 3.18+;
    # cmake_minimum_required here is 3.21 so this is safe.
    add_library(openswmm::engine         ALIAS OpenSWMMEngine::openswmm_engine)
    add_library(openswmm::legacy::engine ALIAS OpenSWMMEngine::openswmm_legacy_engine)
    add_library(openswmm::legacy::output ALIAS OpenSWMMEngine::openswmm_legacy_output)
    add_library(openswmm::common         ALIAS OpenSWMMEngine::openswmm_common)
    add_library(openswmm::plugin_sdk     ALIAS OpenSWMMEngine::openswmm_plugin_sdk)

    # LIBRARY_VERSION is set by the parent project in source mode; provide a
    # fallback so python/openswmm/CMakeLists.txt's VERSION property still works.
    if(NOT DEFINED LIBRARY_VERSION)
        set(LIBRARY_VERSION   "${PROJECT_VERSION}")
        set(LIBRARY_SOVERSION "${PROJECT_VERSION_MAJOR}")
    endif()

else()
    # ── Source mode (local dev / fresh clone) ─────────────────────────────
    # Using add_subdirectory keeps all ALIAS targets available and avoids
    # export/install mismatches.
    message(STATUS "openswmm-python: building engine from source")
    set(OPENSWMM_USE_PREBUILT_ENGINE FALSE CACHE BOOL "" FORCE)

    set(OPENSWMM_BUILD_TESTS             OFF CACHE BOOL "" FORCE)
    set(OPENSWMM_BUILD_UNIT_TESTS        OFF CACHE BOOL "" FORCE)
    set(OPENSWMM_BUILD_REGRESSION_TESTS  OFF CACHE BOOL "" FORCE)
    set(OPENSWMM_BUILD_BENCHMARKS        OFF CACHE BOOL "" FORCE)
    set(OPENSWMM_BUILD_PYTHON            OFF CACHE BOOL "" FORCE)

    # Disable WINDOWS_EXPORT_ALL_SYMBOLS globally.  When ON, CMake adds a
    # pre-link step (cmake -E __create_def) that cannot parse /GL (LTCG)
    # object files, causing "unrecognized file format" errors on Windows CI.
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)

    add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${CMAKE_BINARY_DIR}/parent_build"
        EXCLUDE_FROM_ALL
    )
endif()

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

# Refresh cached Cython executable path each configure. This avoids stale
# references to deleted pip-build-env temp directories when re-running
# `pip install .` in the same source tree.
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import shutil; print(shutil.which('cython') or '')"
    OUTPUT_VARIABLE _CYTHON_EXE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(_CYTHON_EXE)
    set(CYTHON_EXECUTABLE "${_CYTHON_EXE}" CACHE FILEPATH "Path to cython executable" FORCE)
endif()

find_package(Cython REQUIRED)

# NumPy include directory (needed by Cython modules that cimport numpy)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import numpy; print(numpy.get_include())"
    OUTPUT_VARIABLE NUMPY_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE NUMPY_RESULT
)
if(NOT NUMPY_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not determine NumPy include directory")
endif()
message(STATUS "NumPy include dir: ${NUMPY_INCLUDE_DIR}")

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
