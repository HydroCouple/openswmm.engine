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

# Configure based on whether OpenMP was found.
#
# We intentionally do NOT append ${OpenMP_C_FLAGS} / ${OpenMP_CXX_FLAGS} to
# CMAKE_C_FLAGS / CMAKE_CXX_FLAGS here. On macOS arm64 with Apple Clang, the
# stock find_package(OpenMP) probes fail and CMake's FindOpenMP caches the
# *_FLAGS variables as the literal sentinel string "NOTFOUND". The Apple
# fallback in cmake/FindOpenMP.cmake then sets OpenMP_FOUND=TRUE without
# overwriting those cached sentinels, so the append would inject the bare word
# "NOTFOUND" into the compile line and clang would try to open it as a file.
#
# Both the stock CMake FindOpenMP and our Apple fallback create the
# OpenMP::OpenMP_C / OpenMP::OpenMP_CXX imported targets with the correct
# INTERFACE compile options and link libraries, so the target_link_libraries
# call below is sufficient — no global flag manipulation needed.
if(OpenMP_FOUND OR OpenMP_C_FOUND)
    message(STATUS "OpenMP found and will be used for legacy engine")
else()
    message(WARNING "OpenMP not found - building legacy engine without parallel support")
    add_compile_definitions(NO_OPENMP)
endif()

# Legacy SWMM version (independent of the OpenSWMM Engine version)
set(LEGACY_SWMM_VERSION_MAJOR 5)
set(LEGACY_SWMM_VERSION_MINOR 3)
set(LEGACY_SWMM_VERSION_PATCH 0)
set(LEGACY_SWMM_VERSION "${LEGACY_SWMM_VERSION_MAJOR}.${LEGACY_SWMM_VERSION_MINOR}.${LEGACY_SWMM_VERSION_PATCH}")
set(LEGACY_SWMM_VERSION_FULL "${LEGACY_SWMM_VERSION} (Build ${LEGACY_SWMM_VERSION})")

# OpenSWMM wrapper pre-release tag for the legacy engine integration
# (e.g. "beta.1", "rc.1", or "" for a stable release).
set(OPENSWMM_LEGACY_PRERELEASE "beta.1" CACHE STRING
    "Pre-release tag for the legacy SWMM engine wrapper (e.g. beta.1, rc.1). Empty for a stable release.")

if(OPENSWMM_LEGACY_PRERELEASE)
    set(OPENSWMM_LEGACY_FULL_VERSION "${LEGACY_SWMM_VERSION}-${OPENSWMM_LEGACY_PRERELEASE}")
else()
    set(OPENSWMM_LEGACY_FULL_VERSION "${LEGACY_SWMM_VERSION}")
endif()

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

# Backwards-compatible aliases
add_library(openswmm::legacy::engine     ALIAS openswmm_legacy_engine)

set_target_properties(openswmm_legacy_engine
    PROPERTIES
        OUTPUT_NAME "openswmm.legacy.engine"
        VERSION     ${PROJECT_VERSION}
        SOVERSION   ${PROJECT_VERSION_MAJOR}
        # Tell generate_export_header that symbols default to hidden so it
        # emits __attribute__((visibility("default"))) on the export macro.
        # Without this, if -fvisibility=hidden is in CMAKE_C_FLAGS the API
        # symbols are hidden and the CLI / worker fail to link.
        C_VISIBILITY_PRESET   hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN TRUE
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
#
# generate_export_header uses the cached COMPILER_HAS_HIDDEN_VISIBILITY result
# to decide whether to emit __attribute__((visibility("default"))) on the export
# macro.  On macOS/ARM the check_compiler_flag test may fail or return empty
# because unsupported flags like -ffloat-store are present in CMAKE_C_FLAGS and
# cause a compiler warning that is treated as a test failure.  Force the variable
# TRUE here so the generated header always includes the visibility attribute on
# non-Windows platforms where the compiler is known to support it.
if(NOT WIN32)
    set(COMPILER_HAS_HIDDEN_VISIBILITY TRUE CACHE BOOL
        "Compiler supports -fvisibility=hidden (forced for legacy engine)" FORCE)
endif()
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

# Include directories
#   PUBLIC:  consumers find openswmm_solver.h via include/openswmm/legacy/engine/
#            and the source files themselves (internal headers) via CMAKE_CURRENT_SOURCE_DIR
target_include_directories(openswmm_legacy_engine
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/openswmm/legacy/engine>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include/openswmm/legacy/engine>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        # OpenMP include dirs come from the OpenMP::OpenMP_C imported target
        # via target_link_libraries below — do not interpolate
        # ${OpenMP_C_INCLUDE_DIRS} here. CMake's stock FindOpenMP can leave
        # that variable as the literal sentinel "NOTFOUND" on macOS arm64
        # when its probes fail, which would be added as a literal include
        # path and break the build (same class of bug as ${OpenMP_C_FLAGS}).
)

# Link dependencies
# OpenMP::OpenMP_C is gated on OpenMP_FOUND so the "no OpenMP" fallback
# (lines 33-36 above, which defines NO_OPENMP) actually produces a buildable
# target instead of failing on a missing imported target.
target_link_libraries(openswmm_legacy_engine
    PUBLIC
        openswmm_common   # INTERFACE library — exposes include/
    PRIVATE
        $<$<NOT:$<BOOL:$<C_COMPILER_ID:MSVC>>>:m>
        $<$<BOOL:${OpenMP_FOUND}>:OpenMP::OpenMP_C>
)

# RPATH — wheel layout: bundled deps in openswmm/.dylibs/, two levels above
# openswmm/legacy/engine/.  Standalone install: system finds libomp via its
# default search path.  Build-tree rpath is computed automatically by CMake.
if(APPLE)
    set(LIB_ROOT "@loader_path/../../.dylibs")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN/../../.dylibs")
else()
    set(LIB_ROOT "")
endif()

if(NOT WIN32)
    set_target_properties(openswmm_legacy_engine PROPERTIES
        MACOSX_RPATH  TRUE
        INSTALL_RPATH "${LIB_ROOT}"
    )
endif()

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
