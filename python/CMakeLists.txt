# ============================================================================
# CMake entry point for the openswmm Python package (scikit-build-core)
#
# Invoked by `pip install .`, `pip install -e .`, or `python -m build`.
# Responsibilities:
#   1. Resolve the OpenSWMM engine (pre-built CI or from-source local dev).
#   2. Find Python, locate Cython, and discover NumPy include paths.
#   3. Include cmake/CythonHelpers.cmake (shared directives + helpers).
#   4. Delegate extension builds to openswmm/CMakeLists.txt.
# ============================================================================

cmake_minimum_required(VERSION 3.24)

# ============================================================================
# vcpkg manifest directory: the vcpkg.json lives one directory above python/.
# Set VCPKG_MANIFEST_DIR before project() so vcpkg's toolchain file sees it.
# Users who install without vcpkg (e.g. system-lib builds) can ignore this.
# ============================================================================
if(NOT DEFINED CACHE{VCPKG_MANIFEST_DIR})
    set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.."
        CACHE PATH "Directory containing vcpkg.json" FORCE)
endif()

# Auto-load vcpkg toolchain when VCPKG_ROOT is set in the environment.
# Must be done before project() so the toolchain is processed at language
# enable time.  Users can override by passing -DCMAKE_TOOLCHAIN_FILE=...
# on the command line or via CMAKE_ARGS / SKBUILD_CMAKE_ARGS.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE PATH "vcpkg toolchain file (auto-detected from VCPKG_ROOT)")
    message(STATUS "openswmm-python: auto-loaded vcpkg toolchain from $ENV{VCPKG_ROOT}")
endif()

project(
    openswmm-python
    VERSION 6.0.0
    LANGUAGES C CXX
)

# ============================================================================
# 1. Engine library: find_package (CI pre-built) OR add_subdirectory (local)
# ============================================================================
if(NOT DEFINED OPENSWMM_ENGINE_INSTALL_PREFIX
        AND DEFINED ENV{OPENSWMM_ENGINE_INSTALL_PREFIX})
    set(OPENSWMM_ENGINE_INSTALL_PREFIX "$ENV{OPENSWMM_ENGINE_INSTALL_PREFIX}"
        CACHE PATH "Install prefix of a pre-built OpenSWMM engine")
endif()

if(OPENSWMM_ENGINE_INSTALL_PREFIX)
    message(STATUS "openswmm-python: using pre-built engine from ${OPENSWMM_ENGINE_INSTALL_PREFIX}")
    set(OPENSWMM_USE_PREBUILT_ENGINE TRUE CACHE BOOL "" FORCE)

    find_package(OpenSWMMEngine CONFIG REQUIRED
        PATHS "${OPENSWMM_ENGINE_INSTALL_PREFIX}"
        NO_DEFAULT_PATH
    )

    # Provide openswmm:: ALIAS names consumed by all sub-CMakeLists.
    add_library(openswmm::engine         ALIAS OpenSWMMEngine::openswmm_engine)
    add_library(openswmm::legacy::engine ALIAS OpenSWMMEngine::openswmm_legacy_engine)
    add_library(openswmm::legacy::output ALIAS OpenSWMMEngine::openswmm_legacy_output)
    add_library(openswmm::common         ALIAS OpenSWMMEngine::openswmm_common)
    add_library(openswmm::plugin_sdk     ALIAS OpenSWMMEngine::openswmm_plugin_sdk)

    if(NOT DEFINED LIBRARY_VERSION)
        set(LIBRARY_VERSION   "${PROJECT_VERSION}")
        set(LIBRARY_SOVERSION "${PROJECT_VERSION_MAJOR}")
    endif()

else()
    message(STATUS "openswmm-python: building engine from source")
    set(OPENSWMM_USE_PREBUILT_ENGINE FALSE CACHE BOOL "" FORCE)

    set(OPENSWMM_BUILD_TESTS             OFF CACHE BOOL "" FORCE)
    set(OPENSWMM_BUILD_UNIT_TESTS        OFF CACHE BOOL "" FORCE)
    set(OPENSWMM_BUILD_REGRESSION_TESTS  OFF CACHE BOOL "" FORCE)
    set(OPENSWMM_BUILD_BENCHMARKS        OFF CACHE BOOL "" FORCE)
    set(OPENSWMM_BUILD_PYTHON            OFF CACHE BOOL "" FORCE)

    # Disable WINDOWS_EXPORT_ALL_SYMBOLS: the cmake -E __create_def step
    # cannot parse /GL (LTCG) object files on Windows CI.
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)

    add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${CMAKE_BINARY_DIR}/parent_build"
        EXCLUDE_FROM_ALL
    )
endif()

# ============================================================================
# 2. Global settings
# ============================================================================
set(CMAKE_MACOSX_RPATH          ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_INSTALL_OPENMP_LIBRARIES  TRUE)
include(InstallRequiredSystemLibraries)

# ============================================================================
# 2a. OpenMP — use the same custom FindOpenMP.cmake as the parent project.
#
# In source mode the parent's src/engine/CMakeLists.txt already includes it,
# so the OpenMP::OpenMP_C/CXX targets exist when we reach here.
# In pre-built mode the parent is skipped, so we must ensure the targets exist
# ourselves before any Cython extension is linked.
# ============================================================================
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
if(NOT TARGET OpenMP::OpenMP_CXX)
    include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/FindOpenMP.cmake")
endif()

# ============================================================================
# 3. Python (standard CMake FindPython — scikit-build-core populates hints)
# ============================================================================
find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
message(STATUS "Python executable : ${Python_EXECUTABLE}")
message(STATUS "Python version    : ${Python_VERSION}")

# ============================================================================
# 4. Cython driver — invoked as `python -m cython` so the cython that runs
#    is ALWAYS the one installed in the active interpreter.  Calling a
#    `cython` binary discovered on PATH is a footgun: the binary may shebang
#    a different interpreter (e.g. a system / conda Python), in which case
#    `cimport numpy` resolves to that interpreter's numpy/*.cython-3?.pxd
#    instead of the venv's, producing ABI mismatches at compile time
#    (e.g. _PyArray_Descr.subarray on numpy 2.x).
# ============================================================================
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import Cython, sys; sys.stdout.write(Cython.__version__)"
    OUTPUT_VARIABLE _cython_version
    RESULT_VARIABLE _cython_check_rc
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _cython_check_rc EQUAL 0)
    message(FATAL_ERROR
        "Cython is not importable from the active Python (${Python_EXECUTABLE}).\n"
        "Install it with:  ${Python_EXECUTABLE} -m pip install 'cython>=3.0.12'")
endif()
# OPENSWMM_CYTHON_EXECUTABLE is treated as a list so add_custom_command's
# COMMAND expansion runs `<python> -m cython ...`.
set(OPENSWMM_CYTHON_EXECUTABLE "${Python_EXECUTABLE};-m;cython"
    CACHE STRING "Cython driver (python -m cython) for the active interpreter" FORCE)
message(STATUS "Cython driver     : ${Python_EXECUTABLE} -m cython (Cython ${_cython_version})")

# ============================================================================
# 5. NumPy include directory + pxd patch
#
# numpy.get_include() returns the C headers used by the .cxx compiler.
# Separately, numpy ships Cython declaration files (numpy/__init__.cython-30.pxd
# etc.) that have a known incompatibility with numpy 2.x — see
# cmake/PatchNumpyPxd.cmake for the full story.  We materialise patched
# copies into the build tree and prepend them to Cython's --include-dir so
# our extensions compile against numpy 2.x without modifying the user's
# numpy install.
# ============================================================================
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import numpy; print(numpy.get_include())"
    OUTPUT_VARIABLE NUMPY_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _numpy_rc
)
if(NOT _numpy_rc EQUAL 0)
    message(FATAL_ERROR "Could not determine NumPy include directory")
endif()
message(STATUS "NumPy include dir : ${NUMPY_INCLUDE_DIR}")
set(NUMPY_INCLUDE_DIR "${NUMPY_INCLUDE_DIR}" CACHE PATH "" FORCE)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchNumpyPxd.cmake")
openswmm_patch_numpy_pxd()
set(OPENSWMM_NUMPY_PATCHED_INCLUDE_DIR
    "${OPENSWMM_NUMPY_PATCHED_INCLUDE_DIR}" CACHE PATH "" FORCE)

# ============================================================================
# 6. Cython helper functions (shared directives + add_cython_extension)
# ============================================================================
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CythonHelpers.cmake")

# ============================================================================
# 7. Cython extensions
# ============================================================================
add_subdirectory(openswmm)
