#
# CMakeLists.txt - CMake configuration file for swmm-solver/library
#
# Created: Jul 11, 2019
# Updated: Feb 26, 2025
#
# Author: Michael E. Tryby
#         US EPA ORD/CESER
#         
#         Caleb Buahin (Modified)
#         US EPA ORD/CESER 
#

find_package(OpenMP)

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

target_compile_options(swmm5
    PUBLIC
        $<$<C_COMPILER_ID:MSVC>:
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:Release>:/fp:fast>
            $<$<CONFIG:Release>:/Zi>
        >
)

target_link_options(swmm5
    PUBLIC
        $<$<C_COMPILER_ID:MSVC>:
            $<$<CONFIG:Release>:/LTCG:incremental>
        >
)

# CONFIGURE BUILD_NUMBER, BUILD_ID, BUILD_COMMIT, BUILD_BRANCH, BUILD_DATE
string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC")
set(BUILD_DATE "${BUILD_TIMESTAMP}")

execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get the branch name
execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_BRANCH_NAME
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

string(
    UUID BUILD_ID 
    NAMESPACE "00000000-0000-0000-0000-000000000000" 
    NAME "https://www.epa.gov/water-research/storm-water-management-model-swmm" 
    TYPE MD5
)
set(BUILD_ID "${BUILD_ID}")
set(BUILD_BRANCH "${GIT_BRANCH_NAME}")
set(BUILD_COMMIT "${GIT_COMMIT_HASH}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/version.h.in 
    ${CMAKE_CURRENT_SOURCE_DIR}/include/version.h
)


# Define preprocessor macros for different platforms
if(WIN32)
    add_compile_definitions(OS_WINDOWS)
elseif(APPLE)
    add_compile_definitions(OS_MAC)
elseif(UNIX)
    add_compile_definitions(OS_UNIX)
endif()

include(GenerateExportHeader)
generate_export_header(swmm5
    BASE_NAME swmm5
    EXPORT_MACRO_NAME EXPORT_SWMM_SOLVER_API
    EXPORT_FILE_NAME swmm5_export.h
    STATIC_DEFINE SHARED_EXPORTS_BUILT_AS_STATIC
)

file(COPY ${CMAKE_CURRENT_BINARY_DIR}/swmm5_export.h
    DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(swmm5
    PUBLIC
        $<$<NOT:$<BOOL:$<C_COMPILER_ID:MSVC>>>:m>
        $<$<BOOL:OpenMP_C_FOUND>:OpenMP::OpenMP_C>
)

target_include_directories(swmm5
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${INCLUDE_DIST}>
)


if(APPLE)
    set(LIB_ROOT "@loader_path")
else()
    set(LIB_ROOT "$ORIGIN")
endif()

set_target_properties(swmm5
    PROPERTIES
        MACOSX_RPATH TRUE
        SKIP_BUILD_RPATH FALSE
        BUILD_WITH_INSTALL_RPATH FALSE
        INSTALL_RPATH "${LIB_ROOT};${PACKAGE_RPATH};"
        INSTALL_RPATH_USE_LINK_PATH TRUE
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
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:swmm5>
)
