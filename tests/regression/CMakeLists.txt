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
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
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

# ---- PR 10: 1D ROM bands vs brute-force network Monte Carlo ----
# Was written 2026-07-08 (commit 586e492b) but never registered here — found
# 2026-08-04 while starting P4 (HSYM_RESIDUALS_PR_CHECKLIST.md). The coverage
# and width-ratio numbers this file measures are the ones quoted throughout
# VALIDATION.md/.memory/current.md/this checklist as PR 10's headline result;
# until now it had never run under ctest and could have silently drifted.
# Drives the public C API + SWMMEngine directly (engine lifecycle, not a
# solver-internal test), so it needs the same link set as the regression
# suite above, not the 2D-only set below.
add_executable(test_rom_coverage test_rom_coverage.cpp)
target_link_libraries(test_rom_coverage
    PRIVATE
        GTest::gtest GTest::gtest_main
        openswmm_engine
)
target_include_directories(test_rom_coverage
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/engine
)
set_target_properties(test_rom_coverage PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME regression_rom_coverage COMMAND $<TARGET_FILE:test_rom_coverage>)
set_tests_properties(regression_rom_coverage PROPERTIES LABELS "regression")
openswmm_stage_test_runtime_deps(test_rom_coverage)

# ---- W3: 2D ROM bands vs brute-force marcher Monte Carlo ----
# Drives ExplicitInertialSolver and SpectralROM directly (operator-level
# validation, no engine lifecycle); needs the 2D module.
if(OPENSWMM_BUILD_2D)
    add_executable(test_2d_rom_marcher_coverage test_2d_rom_marcher_coverage.cpp)
    target_link_libraries(test_2d_rom_marcher_coverage
        PRIVATE
            GTest::gtest GTest::gtest_main
            openswmm_engine
    )
    target_include_directories(test_2d_rom_marcher_coverage
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_2d_rom_marcher_coverage PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    add_test(
        NAME    regression_2d_rom_marcher_coverage
        COMMAND $<TARGET_FILE:test_2d_rom_marcher_coverage>
    )
    set_tests_properties(regression_2d_rom_marcher_coverage PROPERTIES
        LABELS "regression"
        ENVIRONMENT "OPENSWMM_2D_BACKEND=cpu")
endif()
