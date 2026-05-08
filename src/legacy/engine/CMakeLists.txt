#
# src/legacy/engine/CMakeLists.txt
#
# Builds the original EPA SWMM 5.x solver as openswmm_legacy_engine.
# Source files are the unmodified EPA SWMM 5.x C files moved here from src/solver/.
# Public headers live in include/openswmm/legacy/engine/ — no redirect headers.
#
# Target:      openswmm_legacy_engine
# Output name: openswmm.legacy.engine
# Aliases:     openswmm::legacy::engine
#
# IMPORTANT: Do NOT modify any .c or .h file in this directory.
#            The legacy code must remain byte-for-byte identical to EPA SWMM 5.x
#            to serve as the regression baseline (R27, R43).
#

# Try to find OpenMP (optional)
find_package(OpenMP)

# On Apple, if not found, try custom finder
if(APPLE AND NOT OpenMP_FOUND)
    message(STATUS "OpenMP not found via find_package, trying custom finder...")
    include(${PROJECT_SOURCE_DIR}/cmake/FindOpenMP.cmake)
endif()

# Configure based on whether OpenMP was found
if(OpenMP_FOUND OR OpenMP_C_FOUND)
    message(STATUS "OpenMP found and will be used for legacy engine")
    if(APPLE)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    endif()
else()
    message(WARNING "OpenMP not found - building legacy engine without parallel support")
    add_compile_definitions(NO_OPENMP)
    set(OPENSWMM_LEGACY_NO_OPENMP TRUE)
endif()

# Legacy SWMM version (independent of the OpenSWMM Engine version)
set(LEGACY_SWMM_VERSION_MAJOR 5)
set(LEGACY_SWMM_VERSION_MINOR 3)
set(LEGACY_SWMM_VERSION_PATCH 0)
set(LEGACY_SWMM_VERSION "${LEGACY_SWMM_VERSION_MAJOR}.${LEGACY_SWMM_VERSION_MINOR}.${LEGACY_SWMM_VERSION_PATCH}")
set(LEGACY_SWMM_VERSION_FULL "${LEGACY_SWMM_VERSION} (Build ${LEGACY_SWMM_VERSION})")

configure_file(
    ${PROJECT_SOURCE_DIR}/include/openswmm/legacy/engine/legacy_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/legacy_version.h
)

# Public header (canonical location after Phase 1 relocation)
set(SWMM_PUBLIC_HEADERS
    ${PROJECT_SOURCE_DIR}/include/openswmm/legacy/engine/openswmm_solver.h
)

# All .c and .h files in this directory
file(GLOB SWMM_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.c *.h)

add_library(openswmm_legacy_engine SHARED ${SWMM_SOURCES})

if(OPENSWMM_LEGACY_NO_OPENMP)
    # project.c and swmm5.c both provide a fallback omp_get_max_threads().
    # Rename one translation unit's local fallback to keep the legacy sources untouched.
    set_source_files_properties(project.c PROPERTIES
        COMPILE_DEFINITIONS "omp_get_max_threads=project_omp_get_max_threads"
    )
endif()

# Backwards-compatible aliases
add_library(openswmm::legacy::engine     ALIAS openswmm_legacy_engine)

set_target_properties(openswmm_legacy_engine
    PROPERTIES
        OUTPUT_NAME "openswmm.legacy.engine"
        VERSION     ${PROJECT_VERSION}
        SOVERSION   ${PROJECT_VERSION_MAJOR}
)

# Platform definitions (required by legacy SWMM source)
if(WIN32)
    target_compile_definitions(openswmm_legacy_engine PRIVATE OS_WINDOWS)
elseif(APPLE)
    target_compile_definitions(openswmm_legacy_engine PRIVATE OS_MAC)
elseif(UNIX)
    target_compile_definitions(openswmm_legacy_engine PRIVATE OS_UNIX)
endif()

# Generate export header
# NOTE: Macro name must stay EXPORT_OPENSWMMCORE_SOLVER_API — it is used
#       directly in the unmodified legacy C source files (swmm5.c, swmm5_stats.c).
#   #include "openswmm_legacy_solver_export.h"   → EXPORT_FILE_NAME
#   EXPORT_OPENSWMMCORE_SOLVER_API                → EXPORT_MACRO_NAME (preserved)
include(GenerateExportHeader)
generate_export_header(openswmm_legacy_engine
    BASE_NAME        openswmm_legacy_solver
    EXPORT_MACRO_NAME EXPORT_OPENSWMMCORE_SOLVER_API
    EXPORT_FILE_NAME openswmm_legacy_solver_export.h
    STATIC_DEFINE    SHARED_EXPORTS_BUILT_AS_STATIC
)

# Copy generated export header to the canonical include location
file(COPY ${CMAKE_CURRENT_BINARY_DIR}/openswmm_legacy_solver_export.h
    DESTINATION ${PROJECT_SOURCE_DIR}/include/openswmm/legacy/engine
)

# Build the private include list incrementally so OpenMP remains optional.
set(OPENSWMM_LEGACY_ENGINE_PRIVATE_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(OpenMP_FOUND OR TARGET OpenMP::OpenMP_C)
    list(APPEND OPENSWMM_LEGACY_ENGINE_PRIVATE_INCLUDE_DIRS ${OpenMP_C_INCLUDE_DIRS})
endif()

# Include directories
#   PUBLIC:  consumers find openswmm_solver.h via include/openswmm/legacy/engine/
#            and the source files themselves (internal headers) via CMAKE_CURRENT_SOURCE_DIR
target_include_directories(openswmm_legacy_engine
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/openswmm/legacy/engine>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include/openswmm/legacy/engine>
    PRIVATE
        ${OPENSWMM_LEGACY_ENGINE_PRIVATE_INCLUDE_DIRS}
)

# Link dependencies
target_link_libraries(openswmm_legacy_engine
    PUBLIC
        openswmm_common   # INTERFACE library — exposes include/
    PRIVATE
        $<$<NOT:$<BOOL:$<C_COMPILER_ID:MSVC>>>:m>
)

if(TARGET OpenMP::OpenMP_C)
    target_link_libraries(openswmm_legacy_engine PRIVATE OpenMP::OpenMP_C)
endif()

# RPATH settings
if(APPLE)
    set(LIB_ROOT "@loader_path/../lib;@rpath")
else()
    set(LIB_ROOT "$ORIGIN/../lib;$ORIGIN")
endif()

set_target_properties(openswmm_legacy_engine
    PROPERTIES
        INSTALL_RPATH          "${LIB_ROOT}"
        BUILD_RPATH            "${LIB_ROOT}"
        BUILD_WITH_INSTALL_RPATH TRUE
)

# Install public headers
install(
    FILES       ${SWMM_PUBLIC_HEADERS}
    DESTINATION include/openswmm/legacy/engine
)

# Install library
install(
    TARGETS     openswmm_legacy_engine
    EXPORT      OpenSWMMEngineTargets
)

# Copy to build tree for testing
add_custom_command(TARGET openswmm_legacy_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmm_legacy_engine>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmm_legacy_engine>
)
