#
# tests/unit/output/CMakeLists.txt
#
# Unit tests for the new openswmm_engine output layer.
# Add add_gtest_unit() calls here as output unit tests are created.
#

cmake_minimum_required(VERSION 3.21)

find_package(GTest CONFIG REQUIRED)
add_executable(test_flowtrace test_flowtrace.cpp)
target_link_libraries(test_flowtrace PRIVATE openswmm_engine GTest::gtest_main)
target_compile_features(test_flowtrace PRIVATE cxx_std_20)
add_test(NAME test_flowtrace COMMAND test_flowtrace)
set_tests_properties(test_flowtrace PROPERTIES LABELS "unit;output;trace")
target_compile_definitions(test_flowtrace PRIVATE TRACE_TEST_OUTPUT="${CMAKE_SOURCE_DIR}/plans/artifacts/flow_trace_2026-10-01")
