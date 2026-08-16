#
# CMakeLists.txt - CMake configuration file for swmm-solver/library
#
# Created: Jul 11, 2019
# Updated: Dec  1, 2021
#
# Author: Michael E. Tryby
#         US EPA ORD/CESER
#


# NOTE (2026-08-16): find_package(OpenMP) moved to the top-level CMakeLists so a
# single SWMM_WITH_OPENMP option governs the whole project and a missing OpenMP
# runtime fails configure instead of silently producing a serial build.


# configure file groups
set(SWMM_PUBLIC_HEADERS
    include/swmm5.h
)

file(GLOB
    SWMM_SOURCES
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.c *.h
)

if(BUILD_DEF)
    # Build library with def file interface for backward compatibility
    set_source_files_properties(${PROJECT_SOURCE_DIR}/bindings/swmm5.def
        PROPERTIES_HEADER_FILE_ONLY TRUE
    )

    add_library(swmm5
        SHARED
            ${SWMM_SOURCES}
            ${PROJECT_SOURCE_DIR}/bindings/swmm5.def
    )

else()

    add_library(swmm5
        SHARED
            ${SWMM_SOURCES}
    )

endif()

# NOTE (2026-08-16): upstream hardcoded /GL /fp:fast /Zi (Release, MSVC) and
# /LTCG here. Those are removed deliberately.
#
#   * /fp:fast contradicts the FP policy this branch now shares with
#     openswmm.engine (/fp:precise, -ffp-contract=off, -fno-fast-math). Matching
#     FP policy across 5.2.4 / 5.3.0 / v6 is what makes benchmark result diffs
#     and timings attributable to engine changes rather than compiler math.
#   * /GL + /LTCG are optimisation choices that belong with the rest of the
#     per-configuration flags.
#
# All of it now comes from CMakePresets.json, so every engine tree is governed
# identically. Do not reintroduce per-target FP flags here.

target_compile_options(swmm5
    PUBLIC
        $<$<C_COMPILER_ID:MSVC>:
            $<$<CONFIG:Release>:/Zi>
        >
)

target_link_libraries(swmm5
    PUBLIC
        $<$<NOT:$<BOOL:$<C_COMPILER_ID:MSVC>>>:m>
)

# NOTE (2026-08-16): the previous link line used
#     $<$<BOOL:OpenMP_C_FOUND>:OpenMP::OpenMP_C>
# which is a latent bug: $<BOOL:...> evaluates the *literal string*
# "OpenMP_C_FOUND" (non-empty and not a false constant => 1), not the variable.
# The genex was therefore always true, so the target unconditionally linked
# OpenMP::OpenMP_C -- and where find_package(OpenMP) failed (macOS without
# libomp) CMake aborted at generate time with "target not found" rather than
# degrading gracefully. Gate on the imported target existing instead.
if(TARGET OpenMP::OpenMP_C)
    target_link_libraries(swmm5 PUBLIC OpenMP::OpenMP_C)
endif()

target_include_directories(swmm5
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${INCLUDE_DIST}>
)

install(TARGETS swmm5 EXPORT swmm5Targets
    RUNTIME    DESTINATION "${TOOL_DIST}"
    LIBRARY    DESTINATION "${TOOL_DIST}"
    ARCHIVE    DESTINATION "${LIBRARY_DIST}"
    FRAMEWORK  DESTINATION "${TOOL_DIST}"
)

# Create target import scripts so other cmake projects can use swmm libraries
install(
    EXPORT
        swmm5Targets
    DESTINATION
        "${CONFIG_DIST}"
    FILE
        swmm5-config.cmake
)

install(
    FILES 
        ${SWMM_PUBLIC_HEADERS} 
    DESTINATION 
        "${INCLUDE_DIST}"
)


# copy swmm5 to build tree for testing
add_custom_command(TARGET swmm5 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:swmm5>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>/$<TARGET_FILE_NAME:swmm5>
)
