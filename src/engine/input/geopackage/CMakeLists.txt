#
# CMakeLists.txt - Optional GeoPackage I/O library for OpenSWMM
#
# Library name:    openswmm_geopackage
# CMake target:    openswmm::geopackage
#
# This library is only built when OPENSWMM_WITH_GEOPACKAGE=ON.
# It depends on sqlite3 (system, Homebrew, or vcpkg).
#
# See STRATEGY.md in this directory for the full design document.
#

cmake_minimum_required(VERSION 3.21)

# Try vcpkg first, then fall back to CMake's built-in FindSQLite3
find_package(unofficial-sqlite3 CONFIG QUIET)
if(NOT unofficial-sqlite3_FOUND)
    find_package(SQLite3 REQUIRED)
endif()

file(GLOB GEOPACKAGE_SOURCES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    *.cpp
)

file(GLOB GEOPACKAGE_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    *.hpp
)

add_library(openswmm_geopackage STATIC ${GEOPACKAGE_SOURCES})
add_library(openswmm::geopackage ALIAS openswmm_geopackage)

set_target_properties(openswmm_geopackage PROPERTIES
    OUTPUT_NAME "openswmm.geopackage"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(openswmm_geopackage
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/openswmm/engine>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/openswmm/plugin_sdk>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/engine
)

target_link_libraries(openswmm_geopackage
    PUBLIC
        openswmm_common
        $<IF:$<TARGET_EXISTS:unofficial::sqlite3::sqlite3>,unofficial::sqlite3::sqlite3,SQLite::SQLite3>
    PRIVATE
        openswmm_engine
)

target_compile_definitions(openswmm_geopackage
    PUBLIC OPENSWMM_HAS_GEOPACKAGE=1
)

# Install
install(
    TARGETS openswmm_geopackage
    EXPORT  OpenSWMMEngineTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
