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
    formats/*.cpp     # Slice IO-6 — external-file format parsers/materialisers
)

file(GLOB GEOPACKAGE_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    *.hpp
    formats/*.hpp
)

add_library(openswmm_geopackage STATIC ${GEOPACKAGE_SOURCES})
add_library(openswmm::geopackage ALIAS openswmm_geopackage)

set_target_properties(openswmm_geopackage PROPERTIES
    OUTPUT_NAME "openswmm.geopackage"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    # Slice RC.2 (see §R.3 of openswmm.gui's GUI_IMPLEMENTATION_PLAN.md) —
    # default-hide GeoPackage symbols so the historically-leaked
    # `openswmm_plugin_info` C symbol no longer appears as a dlsym hit
    # on the engine's SHARED library. The PluginFactory registers
    # GeoPackagePluginInfo explicitly through register_builtin_infos
    # instead; this hardening removes the duplicate-discovery side
    # effect and any third-party consumer that was resolving the
    # symbol via dlsym should switch to discover_plugins_by_id().
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET   hidden
    VISIBILITY_INLINES_HIDDEN ON
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
)
# Slice RC.1 follow-up — geopackage no longer links openswmm_engine.
# Engine now PRIVATE-links geopackage (absorbing the STATIC archive
# into the SHARED), which made the reverse link both redundant and a
# CMake target cycle (SHARED ↔ STATIC is not allowed). Engine headers
# remain reachable via the PUBLIC include paths above (lines 54-57).

target_compile_definitions(openswmm_geopackage
    PUBLIC OPENSWMM_HAS_GEOPACKAGE=1
    # openswmm_geopackage is a STATIC library whose archive is absorbed into
    # the engine SHARED (engine PRIVATE-links geopackage). Its source files
    # define the swmm_gpkg_* C API decorated with SWMM_ENGINE_API (from
    # openswmm_engine.h). On MSVC, CMake auto-defines openswmm_engine_EXPORTS
    # only for the engine target itself, not for sources in static archives
    # that get linked into it — so without intervention, SWMM_ENGINE_API
    # expands to __declspec(dllimport) when these TUs compile, and the
    # compiler rejects defining a function as dllimport (C2491).
    #
    # We mirror the engine's own _EXPORTS flag on this target so that
    # SWMM_ENGINE_API expands to __declspec(dllexport) here. Result: the
    # swmm_gpkg_* symbols land in the engine DLL's export table (and thus
    # openswmm.engine.lib), so the Cython _geopackage extension — and any
    # other external consumer — can dllimport them at link time.
    #
    # The previous workaround (defining OPENSWMM_ENGINE_STATIC instead)
    # made SWMM_ENGINE_API empty, which silenced the compiler but left the
    # symbols out of the export table. That caused 27 LNK2019 errors when
    # building the Windows Python wheel — see Cython _geopackage link step.
    #
    # On non-MSVC, openswmm_engine_EXPORTS is harmless: SWMM_ENGINE_API
    # expands to __attribute__((visibility("default"))) on all platforms
    # under that branch, matching what the engine's own TUs already get.
    PRIVATE openswmm_engine_EXPORTS
)

# Install
install(
    TARGETS openswmm_geopackage
    EXPORT  OpenSWMMEngineTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
