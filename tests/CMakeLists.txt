#
# tests/CMakeLists.txt - Test suite for openswmm.engine
#
# Test framework: Google Test (all tests)
#                 Google Benchmark (performance)
#
# Directory layout:
#   unit/
#     legacy/
#       engine/   — tests for openswmm_legacy_engine  (legacy SWMM solver API)
#       output/   — tests for openswmm_legacy_output  (legacy SWMM binary output)
#     engine/     — unit tests for the new openswmm_engine library
#     output/     — unit tests for the new openswmm_engine output layer
#   regression/ — full-simulation regression tests (new engine vs. legacy)
#   benchmarks/ — performance benchmarks (added from root when OPENSWMM_BUILD_BENCHMARKS is ON)
#
# Updated: 2026-03-25 — removed Boost.Test dependency; all tests now use Google Test.
# Updated: 2026-03-25 — reorganised into unit/ subtree (legacy/ moved under unit/).
#

cmake_minimum_required(VERSION 3.21)

# ---- Google Test (required when OPENSWMM_BUILD_TESTS is ON) ----
find_package(GTest CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Directory with test input data files
set(TEST_BIN_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
set(TEST_DATA_ROOT     ${CMAKE_CURRENT_SOURCE_DIR})

# ---- All tests (legacy + unit) ----
add_subdirectory(unit)

add_test(NAME test_legacy_solver_api
    COMMAND $<TARGET_FILE:test_solver_api>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/engine/data)
add_test(NAME test_legacy_solver_errors
    COMMAND $<TARGET_FILE:test_solver_errors>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/engine/data)
add_test(NAME test_legacy_solver_hotstart
    COMMAND $<TARGET_FILE:test_solver_hotstart>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/engine/data)
add_test(NAME test_legacy_solver_shapes
    COMMAND $<TARGET_FILE:test_solver_shapes>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/engine/data)
add_test(NAME test_legacy_solver_expanded_api
    COMMAND $<TARGET_FILE:test_solver_expanded_api>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/engine/data)
add_test(NAME test_legacy_output
    COMMAND $<TARGET_FILE:test_output>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/output/data)

# ---- Regression tests ----
add_subdirectory(regression)

# Benchmarks are added from root CMakeLists when OPENSWMM_BUILD_BENCHMARKS is ON.
# Do not add_subdirectory(benchmarks) here.
