# ============================================================================
# CMake entry point for the openswmm Python package (scikit-build)
#
# When building via `pip install .` or `python -m build`, scikit-build
# invokes this file.  It:
#   1. Configures + builds the parent C/C++ project (if not already built).
#   2. Installs it to a local prefix so find_package() works.
#   3. Finds Python, Cython, and PythonExtensions.
#   4. Delegates to openswmm/CMakeLists.txt for the Cython extensions.
# ============================================================================

cmake_minimum_required(VERSION 3.21)

project(
    openswmm-python
    VERSION 6.0.0
    LANGUAGES C CXX
)

# ============================================================================
# 1. Build the parent C/C++ project
# ============================================================================
set(PARENT_BUILD_DIR "${CMAKE_BINARY_DIR}/parent_build")
set(PARENT_INSTALL_DIR "${PARENT_BUILD_DIR}/install")

# Configure
execute_process(
    COMMAND ${CMAKE_COMMAND}
        -S "${CMAKE_CURRENT_SOURCE_DIR}/.."
        -B "${PARENT_BUILD_DIR}"
        -G "${CMAKE_GENERATOR}"
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DOPENSWMM_BUILD_TESTS=OFF
        -DOPENSWMM_BUILD_BENCHMARKS=OFF
        -DOPENSWMM_BUILD_PYTHON=OFF
    RESULT_VARIABLE _cfg_result
)
if(_cfg_result)
    message(FATAL_ERROR "Parent project configure failed (${_cfg_result})")
endif()

# Build
execute_process(
    COMMAND ${CMAKE_COMMAND} --build "${PARENT_BUILD_DIR}"
    RESULT_VARIABLE _bld_result
)
if(_bld_result)
    message(FATAL_ERROR "Parent project build failed (${_bld_result})")
endif()

# Install to local prefix
execute_process(
    COMMAND ${CMAKE_COMMAND}
        --install "${PARENT_BUILD_DIR}"
        --prefix "${PARENT_INSTALL_DIR}"
    RESULT_VARIABLE _inst_result
)
if(_inst_result)
    message(FATAL_ERROR "Parent project install failed (${_inst_result})")
endif()

# ============================================================================
# 2. Locate the parent package
# ============================================================================
list(APPEND CMAKE_PREFIX_PATH "${PARENT_INSTALL_DIR}")
find_package(OpenSWMMCore REQUIRED PATHS "${PARENT_INSTALL_DIR}" NO_DEFAULT_PATH)

# ============================================================================
# 3. CMake policies & settings
# ============================================================================
cmake_policy(SET CMP0148 OLD)   # FindPythonInterp / FindPythonLibs compat
set(CMAKE_MACOSX_RPATH ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================================
# 4. Find Python + Cython toolchain
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
# 5. System runtime libraries (vcruntime, OpenMP DLLs on Windows)
# ============================================================================
set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
include(InstallRequiredSystemLibraries)

# ============================================================================
# 6. Cython extensions
# ============================================================================
add_subdirectory(openswmm)
