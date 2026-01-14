#
# CMakeLists.txt - CMake configuration file for openswmmcore/output
#
# Created:  July 11, 2019
# Modified: February 26, 2025
#
# Author:   Michael E. Tryby
#           US EPA ORD/CESER
#
#           Caleb Buahin (Modified)
#           US EPA ORD/CESER
#

# configure file groups
set(SWMM_OUT_PUBLIC_HEADERS
    ${PROJECT_SOURCE_DIR}/include/output/openswmm_output.h
    ${PROJECT_SOURCE_DIR}/include/output/openswmm_output_enums.h
    ${PROJECT_SOURCE_DIR}/include/output/openswmm_output_export.h
)


# the binary output file API
add_library(openswmmcore_output
    SHARED
        swmm_output.c
        errormanager.c
)

if (APPLE)
    set_target_properties(
        openswmmcore_output
        PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()


target_link_libraries(
    openswmmcore_output
    PUBLIC
        openswmmcore
)

add_library(OpenSWMMCore::Output ALIAS openswmmcore_output)

target_include_directories(
    openswmmcore_output
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/output>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)


include(GenerateExportHeader)
generate_export_header(openswmmcore_output
    BASE_NAME openswmmcore_output
    EXPORT_MACRO_NAME EXPORT_OPENSWMMCORE_OUTPUT_API
    EXPORT_FILE_NAME openswmm_output_export.h
    STATIC_DEFINE SHARED_EXPORTS_BUILT_AS_STATIC
)

file(COPY ${CMAKE_CURRENT_BINARY_DIR}/openswmm_output_export.h
    DESTINATION ${PROJECT_SOURCE_DIR}/include/output
)

if(APPLE)
    set(LIB_ROOT "@loader_path")
else()
    set(LIB_ROOT "$ORIGIN")
endif()

set_target_properties(openswmmcore_output
    PROPERTIES
        MACOSX_RPATH TRUE
        SKIP_BUILD_RPATH FALSE
        BUILD_WITH_INSTALL_RPATH FALSE
        INSTALL_RPATH "${LIB_ROOT};${PACKAGE_RPATH};"
        INSTALL_RPATH_USE_LINK_PATH TRUE
)

install(
    FILES 
        ${SWMM_OUT_PUBLIC_HEADERS} 
    DESTINATION 
        include
)

install(
    TARGETS
        openswmmcore_output
    EXPORT
        OpenSWMMCoreTargets
)

# copy openswmmcore_output to build tree for testing
if(BUILD_TESTS)
    add_custom_command(TARGET openswmmcore_output POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:openswmmcore_output>
            ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmmcore_output>
        )
endif()