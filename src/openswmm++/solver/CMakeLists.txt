#
# CMakeLists.txt - CMake configuration file for openswmmcore/solver
#
# Created: Jul 11, 2019
# Updated: Feb 26, 2025
#
# Original Creator : Michael E. Tryby
#                    US EPA ORD/CESER
#         
#                    Caleb Buahin (Modified)
#                    US EPA ORD/CESER 
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
    message(STATUS "OpenMP found and will be used")
    if(APPLE)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    endif()
else()
    message(WARNING "OpenMP not found - building without parallel support")
    message(STATUS "To enable OpenMP on macOS, run: brew install libomp")
    # Optionally set a define to disable OpenMP in code
    add_compile_definitions(NO_OPENMP)
endif()

# configure file groups
set(
    SWMM_PUBLIC_HEADERS
        ${PROJECT_SOURCE_DIR}/include/solver/openswmmcore_solver_export.h
        ${PROJECT_SOURCE_DIR}/include/solver/openswmm_solver.h
)

file(
    GLOB
    SWMM_SOURCES
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.c *.h
)


add_library(
    openswmmcore_solver
    SHARED
        ${SWMM_SOURCES}
)

target_link_libraries(
    openswmmcore_solver
    PUBLIC
        openswmmcore
)

add_library(OpenSWMMCore::Solver ALIAS openswmmcore_solver)

# specify namespace alias
if (APPLE)
    set_target_properties(
        openswmmcore_solver
        PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()


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
    BASE_NAME openswmmcore_solver
    EXPORT_MACRO_NAME EXPORT_OPENSWMMCORE_SOLVER_API
    EXPORT_FILE_NAME openswmmcore_solver_export.h
    STATIC_DEFINE SHARED_EXPORTS_BUILT_AS_STATIC
)

file(COPY ${CMAKE_CURRENT_BINARY_DIR}/openswmmcore_solver_export.h
    DESTINATION ${PROJECT_SOURCE_DIR}/include/solver
)

target_link_libraries(
    openswmmcore_solver
    PRIVATE
        $<$<NOT:$<BOOL:$<C_COMPILER_ID:MSVC>>>:m>
        OpenMP::OpenMP_C
)

# Set public include directories for both build and install time
target_include_directories(
    openswmmcore_solver
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/solver>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:include>
        PRIVATE 
            ${OpenMP_C_INCLUDE_DIRS}
)

if(APPLE)
    set(LIB_ROOT "@loader_path/../lib;@rpath")
else()
    set(LIB_ROOT "$ORIGIN/../lib;$ORIGIN")
endif()

set_target_properties(openswmmcore_solver
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
        openswmmcore_solver
    EXPORT
        OpenSWMMCoreTargets
)

# copy openswmmcore_solver to build tree for testing
add_custom_command(TARGET openswmmcore_solver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmmcore_solver>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmmcore_solver>
)
