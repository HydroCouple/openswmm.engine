#
# tests/legacy/output/CMakeLists.txt
#
# Google Test unit tests for the openswmm_legacy_output (legacy SWMM binary output reader).
# Original: July 11, 2019 - Michael E. Tryby, US EPA ORD/CESER
# Converted from Boost.Test to Google Test: 2026-03-25.
# Moved from tests/outfile/ to tests/legacy/output/ 2026-03-25.
#

cmake_minimum_required(VERSION 3.21)

find_package(GTest CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(LIB_ROOT "@loader_path;@rpath;@executable_path;@loader_path/../lib;@rpath/../lib;@executable_path/../lib;@executable_path/$CMAKE_BUILD_TYPE")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib;$ORIGIN/$CMAKE_BUILD_TYPE")
endif()

add_executable(
    test_output
        test_output.cpp
    )
target_include_directories(
    test_output
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include/openswmm/legacy/output
    )
target_link_libraries(
    test_output
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        openswmm_legacy_output
    )

if(MSVC)
    target_compile_options(test_output PRIVATE /EHsc)
endif()

set_target_properties(
    test_output
    PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
        INSTALL_RPATH "${LIB_ROOT}"
)
