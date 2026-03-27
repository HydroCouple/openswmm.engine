#
# src/legacy/output/CMakeLists.txt
#
# Builds the original EPA SWMM 5.x binary output reader as openswmm_legacy_output.
# Source files are the unmodified EPA SWMM 5.x C files moved here from src/output/.
# Public headers live in include/openswmm/legacy/output/ — no redirect headers.
#
# Target:      openswmm_legacy_output
# Output name: openswmm.legacy.output
# Aliases:     openswmm::legacy::output  OpenSWMMCore::Output  openswmmcore_output
#
# IMPORTANT: Do NOT modify any .c or .h file in this directory.
#

# Public headers (canonical location after Phase 1 relocation)
set(SWMM_OUT_PUBLIC_HEADERS
    ${PROJECT_SOURCE_DIR}/include/openswmm/legacy/output/openswmm_output.h
    ${PROJECT_SOURCE_DIR}/include/openswmm/legacy/output/openswmm_output_enums.h
    ${CMAKE_CURRENT_BINARY_DIR}/openswmm_output_export.h
)

add_library(openswmm_legacy_output SHARED
    swmm_output.c
    errormanager.c
)

# Backwards-compatible aliases
add_library(openswmmcore_output         ALIAS openswmm_legacy_output)
add_library(OpenSWMMCore::Output        ALIAS openswmm_legacy_output)
add_library(openswmm::legacy::output    ALIAS openswmm_legacy_output)

set_target_properties(openswmm_legacy_output
    PROPERTIES
        OUTPUT_NAME "openswmm.legacy.output"
        VERSION     ${PROJECT_VERSION}
        SOVERSION   ${PROJECT_VERSION_MAJOR}
)

# Generate export header
# NOTE: File name and macro names must match what openswmm_output.h expects:
#   #include "openswmm_output_export.h"    → EXPORT_FILE_NAME
#   EXPORT_OPENSWMMCORE_OUTPUT_API         → EXPORT_MACRO_NAME
include(GenerateExportHeader)
generate_export_header(openswmm_legacy_output
    BASE_NAME        openswmmcore_output
    EXPORT_MACRO_NAME EXPORT_OPENSWMMCORE_OUTPUT_API
    EXPORT_FILE_NAME openswmm_output_export.h
    STATIC_DEFINE    SHARED_EXPORTS_BUILT_AS_STATIC
)

# Copy generated export header to the canonical include location
file(COPY ${CMAKE_CURRENT_BINARY_DIR}/openswmm_output_export.h
    DESTINATION ${PROJECT_SOURCE_DIR}/include/openswmm/legacy/output
)

# Include directories
target_include_directories(openswmm_legacy_output
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/openswmm/legacy/output>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include/openswmm/legacy/output>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link dependencies
target_link_libraries(openswmm_legacy_output
    PUBLIC
        openswmmcore      # INTERFACE library — exposes include/
)

# RPATH settings
if(APPLE)
    set(LIB_ROOT "@loader_path")
else()
    set(LIB_ROOT "$ORIGIN")
endif()

set_target_properties(openswmm_legacy_output
    PROPERTIES
        MACOSX_RPATH             TRUE
        SKIP_BUILD_RPATH         FALSE
        BUILD_WITH_INSTALL_RPATH FALSE
        INSTALL_RPATH            "${LIB_ROOT};${PACKAGE_RPATH};"
        INSTALL_RPATH_USE_LINK_PATH TRUE
)

# Install public headers
install(
    FILES       ${SWMM_OUT_PUBLIC_HEADERS}
    DESTINATION include/openswmm/legacy/output
)

# Install library
install(
    TARGETS     openswmm_legacy_output
    EXPORT      OpenSWMMCoreTargets
)

# Copy to build tree for testing
add_custom_command(TARGET openswmm_legacy_output POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmm_legacy_output>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmm_legacy_output>
)
