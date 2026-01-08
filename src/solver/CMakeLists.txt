#
# CMakeLists.txt - CMake configuration file for openswmmcore/library
#
# Created: Jul 11, 2019
# Updated: Feb 26, 2025
#
# Original Creator : Michael E. Tryby
#                   US EPA ORD/CESER
#         
#         Caleb Buahin (Modified)
#         US EPA ORD/CESER 
#


find_package(OpenMP REQUIRED)

# if not found on Apple, try to find via local find in cmake/FindOpenMP.cmake

if(APPLE)
    if(NOT OpenMP_FOUND)
        include(${PROJECT_SOURCE_DIR}/cmake/FindOpenMP.cmake)
    endif()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
else()
    find_package(OpenMP REQUIRED)
endif()


# configure file groups
set(SWMM_PUBLIC_HEADERS
    include/openswmmcore_export.h
    include/openswmmcore.h
)

file(GLOB
    SWMM_SOURCES
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.c *.h
)


add_library(openswmmcore_solver
    SHARED
        ${SWMM_SOURCES}
)

# specify namespace alias



if (APPLE)
    set_target_properties(
        openswmmcore_solver
        PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()


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
generate_export_header(openswmmcore_solver
    BASE_NAME openswmmcore
    EXPORT_MACRO_NAME EXPORT_OPENSWMMCORE_API
    EXPORT_FILE_NAME openswmmcore_export.h
    STATIC_DEFINE SHARED_EXPORTS_BUILT_AS_STATIC
)

file(COPY ${CMAKE_CURRENT_BINARY_DIR}/openswmmcore_export.h
    DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    openswmmcore_solver
        PRIVATE
    $<$<NOT:$<BOOL:$<C_COMPILER_ID:MSVC>>>:m>
    OpenMP::OpenMP_C
)

# Set public include directories for both build and install time
target_include_directories(
    openswmmcore
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE 
            ${OpenMP_C_INCLUDE_DIRS}
)

if(APPLE)
    set(LIB_ROOT "@loader_path/../lib;@rpath")
else()
    set(LIB_ROOT "$ORIGIN/../lib;$ORIGIN")
endif()

set_target_properties(openswmmcore
    PROPERTIES
        INSTALL_RPATH "${LIB_ROOT}"
        BUILD_RPATH "${LIB_ROOT}"
        BUILD_WITH_INSTALL_RPATH TRUE
)

install(
    FILES 
        ${SWMM_PUBLIC_HEADERS} 
    DESTINATION 
        include
)

install(
    TARGETS
        openswmmcore
    EXPORT
        openswmmcoreTargets
)

# copy openswmmcore to build tree for testing
add_custom_command(TARGET openswmmcore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmmcore>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmmcore>
)
