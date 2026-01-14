# Description: CMakeLists.txt file for the openswmm python package
# Created by: Caleb Buahin (EPA/ORD/CESER/WID)
# Created on: 2024-11-19

# ============================================================================
# Project Configuration
# ============================================================================

cmake_minimum_required(VERSION 3.11)

project(
    openswmm
    VERSION 5.3.0
    LANGUAGES C CXX
)

# ============================================================================
# Parent C/C++ Libraries
# ============================================================================

# Set the build directory path for parent project
set(PARENT_BUILD_DIR "${CMAKE_BINARY_DIR}/parent_build")

# Add parent directory to CMAKE_PREFIX_PATH to find the package
list(APPEND CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}")

# First, configure the parent project to generate package files
execute_process(
    COMMAND ${CMAKE_COMMAND} -S ${CMAKE_CURRENT_SOURCE_DIR}/..
            -B ${PARENT_BUILD_DIR}
            -G ${CMAKE_GENERATOR}
    RESULT_VARIABLE result
)

if(result)
    message(FATAL_ERROR "CMake configuration of parent project failed: ${result}")
endif()

# Build the parent project first
execute_process(
    COMMAND ${CMAKE_COMMAND} --build ${PARENT_BUILD_DIR}
    RESULT_VARIABLE result
)

if(result)
    message(FATAL_ERROR "CMake build of parent project failed: ${result}")
endif()

# Install the parent project to generate the targets file
execute_process(
    COMMAND ${CMAKE_COMMAND} --install ${PARENT_BUILD_DIR} --prefix ${PARENT_BUILD_DIR}/install
    RESULT_VARIABLE result
)

if(result)
    message(FATAL_ERROR "CMake install of parent project failed: ${result}")
endif()

# Update the search path to point to the install directory
list(REMOVE_ITEM CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}")
list(APPEND CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}/install")

# Now find the package in the install directory
find_package(OpenSWMMCore REQUIRED PATHS ${PARENT_BUILD_DIR}/install NO_DEFAULT_PATH)


# ============================================================================
# CMake Policies and Settings
# ============================================================================

cmake_policy(SET CMP0148 OLD)
set(CMAKE_MACOSX_RPATH ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================================
# Find Required Packages
# ============================================================================

find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
find_package(PythonExtensions REQUIRED)
find_package(Cython REQUIRED)

# ============================================================================
# Scikit-Build Configuration
# ============================================================================

if(SKBUILD)
    message(STATUS "Building with scikit-build")

    # Set module suffix for different platforms
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(CMAKE_SHARED_MODULE_SUFFIX ".${SKBUILD_SOABI}.pyd")
    else()
        set(CMAKE_SHARED_MODULE_SUFFIX ".${SKBUILD_SOABI}${CMAKE_SHARED_MODULE_SUFFIX}")
    endif()
endif()

# ============================================================================
# System Libraries Installation
# ============================================================================

# Install runtime libraries (vcruntime, msvcp, vcomp on Windows)
set(CMAKE_INSTALL_OPENMP_LIBRARIES TRUE)
include(InstallRequiredSystemLibraries)

# ============================================================================
# Parent C/C++ Libraries
# ============================================================================

# Add the main openswmm package (delegates to openswmm/CMakeLists.txt)
add_subdirectory(openswmm)
