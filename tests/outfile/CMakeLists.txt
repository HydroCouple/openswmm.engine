#
# CMakeLists.txt - CMake configuration file for tests/outfile
#
# Created: July 11, 2019
# Updated: Aug 16, 2026 — Boost.Test -> GoogleTest
#
# Author: Michael E. Tryby
#         US EPA ORD/CESER
#

add_executable(test_output
    test_output.cpp
    )

target_include_directories(test_output
    PUBLIC ../../src/outfile/include
    )

target_link_libraries(test_output
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        swmm-output
    )

set_target_properties(test_output
    PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
