#
# CMakeLists.txt - Plugin SDK (header-only INTERFACE library)
#
# Library name: openswmm_plugin_sdk
# CMake target: openswmm::plugin_sdk
#
# This is a HEADER-ONLY library. Plugin developers link against this target
# to access the IPluginComponentInfo, IOutputPlugin, IReportPlugin, and
# SimulationSnapshot interfaces without needing to link against the engine.
#
# Usage in plugin CMakeLists.txt:
#   find_package(openswmm CONFIG REQUIRED)
#   target_link_libraries(my_plugin PRIVATE openswmm::plugin_sdk)
#
# See: include/openswmm/plugin_sdk/
# See: docs/MASTER_IMPLEMENTATION_PLAN.md Phase 4
#

cmake_minimum_required(VERSION 3.21)

add_library(openswmm_plugin_sdk INTERFACE)
add_library(openswmm::plugin_sdk ALIAS openswmm_plugin_sdk)

target_compile_features(openswmm_plugin_sdk INTERFACE cxx_std_20)

target_include_directories(openswmm_plugin_sdk
    INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/openswmm/plugin_sdk>
        $<INSTALL_INTERFACE:include>
)

# The plugin SDK depends on the common interface for version.h
target_link_libraries(openswmm_plugin_sdk INTERFACE openswmm_common)

install(
    TARGETS openswmm_plugin_sdk
    EXPORT  OpenSWMMEngineTargets
)

install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/include/openswmm/plugin_sdk
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/openswmm
    FILES_MATCHING PATTERN "*.hpp"
)
