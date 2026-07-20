#
# tests/regression/CMakeLists.txt
#
# Regression tests: compare new openswmm_engine output against legacy
# openswmm_legacy_engine output for the same input models.
#
# Tolerance:
#   - Node depth:  ±0.001 project length units, OR 0.1% relative
#   - Link flow:   ±0.001 project flow units,   OR 0.1% relative
#   - Subcatch runoff: ±0.001 project flow units
#
# Reference models in tests/regression/data/:
#   - Example1.inp  : small drainage, 1 storm event (from EPA SWMM examples)
#   - Example2.inp  : LID-heavy model
#   - Example3.inp  : complex quality routing
#   - LargeCity.inp : 1000+ nodes stress test
#
# See: docs/MASTER_IMPLEMENTATION_PLAN.md Phase 9
#

cmake_minimum_required(VERSION 3.21)
find_package(GTest CONFIG REQUIRED)

add_executable(test_engine_regression
    test_regression_suite.cpp
)

target_link_libraries(test_engine_regression
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        openswmm_engine
        openswmm_legacy_engine
)

target_include_directories(test_engine_regression
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(test_engine_regression PRIVATE /EHsc)
endif()

set_target_properties(test_engine_regression PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Each reference model becomes a parameterized test case
add_test(
    NAME    regression_example1
    COMMAND $<TARGET_FILE:test_engine_regression>
            --gtest_filter=RegressionTest/RegressionTest*
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(regression_example1 PROPERTIES LABELS "regression")

add_custom_command(TARGET test_engine_regression POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:test_engine_regression>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:test_engine_regression>
)

# Stage every runtime shared library the test depends on (project libs +
# vcpkg imported targets like SUNDIALS / HDF5 / sqlite3 / libomp) next to
# the exe. On Windows, the loader fails with STATUS_DLL_NOT_FOUND otherwise.
# On Unix the rpath finds dylibs in lib/ at install time, but ctest runs
# from the build tree where the engine dylibs live in a sibling dir; the
# helper colocates them next to the test exe so $<ORIGIN>/@loader_path
# resolves them.
openswmm_stage_test_runtime_deps(test_engine_regression)

# ============================================================================
# PR 10 — ROM vs brute-force Monte Carlo coverage validation
# ============================================================================

add_executable(test_rom_coverage
    test_rom_coverage.cpp
)

target_link_libraries(test_rom_coverage
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
)

target_include_directories(test_rom_coverage
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/engine
)

if(MSVC)
    target_compile_options(test_rom_coverage PRIVATE /EHsc)
endif()

set_target_properties(test_rom_coverage PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(
    NAME    test_rom_coverage
    COMMAND $<TARGET_FILE:test_rom_coverage>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(test_rom_coverage PROPERTIES LABELS "regression")

add_custom_command(TARGET test_rom_coverage POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:test_rom_coverage>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:test_rom_coverage>
)

openswmm_stage_test_runtime_deps(test_rom_coverage)

# ============================================================================
# SR-5 — soft-rainfall ROM coverage vs Monte Carlo
# ============================================================================
add_executable(test_soft_rain_coverage
    test_soft_rain_coverage.cpp
)

target_link_libraries(test_soft_rain_coverage
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
)

target_include_directories(test_soft_rain_coverage
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/engine
)

if(MSVC)
    target_compile_options(test_soft_rain_coverage PRIVATE /EHsc)
endif()

set_target_properties(test_soft_rain_coverage PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(
    NAME    test_soft_rain_coverage
    COMMAND $<TARGET_FILE:test_soft_rain_coverage>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(test_soft_rain_coverage PROPERTIES LABELS "regression")

add_custom_command(TARGET test_soft_rain_coverage POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:test_soft_rain_coverage>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:test_soft_rain_coverage>
)

openswmm_stage_test_runtime_deps(test_soft_rain_coverage)

# ============================================================================
# CL-1e — correlated soft-rainfall ROM coverage vs correlated Monte Carlo
# ============================================================================
add_executable(test_soft_rain_corr_coverage
    test_soft_rain_corr_coverage.cpp
)

target_link_libraries(test_soft_rain_corr_coverage
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
)

target_include_directories(test_soft_rain_corr_coverage
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/engine
)

# The test calls CorrelatedFieldGenerator directly to materialize the MC
# reference field; the generator header is guarded by OPENSWMM_HAS_2D.
target_compile_definitions(test_soft_rain_corr_coverage PRIVATE OPENSWMM_HAS_2D=1)

if(MSVC)
    target_compile_options(test_soft_rain_corr_coverage PRIVATE /EHsc)
endif()

set_target_properties(test_soft_rain_corr_coverage PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(
    NAME    test_soft_rain_corr_coverage
    COMMAND $<TARGET_FILE:test_soft_rain_corr_coverage>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(test_soft_rain_corr_coverage PROPERTIES LABELS "regression")

add_custom_command(TARGET test_soft_rain_corr_coverage POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:test_soft_rain_corr_coverage>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:test_soft_rain_corr_coverage>
)

openswmm_stage_test_runtime_deps(test_soft_rain_corr_coverage)
