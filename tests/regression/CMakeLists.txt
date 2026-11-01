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

# Each reference model becomes a parameterized test case. The models are
# located relative to the working directory set here, so the whole suite goes
# silently empty if that directory is wrong. Deliberately NO --gtest_filter:
# an empty parameter list leaves the suite uninstantiated, and gtest's
# GoogleTestVerification.UninstantiatedParameterizedTestSuite check then fails
# the run. Filtering that check out is what previously let this test report
# "Passed" while executing zero test cases.
add_test(
    NAME    regression_suite
    COMMAND $<TARGET_FILE:test_engine_regression>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
)
set_tests_properties(regression_suite PROPERTIES LABELS "regression")

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

# ---- PR P4: 1D ROM compatibility matrix (NODE_CONTINUITY x Anderson) ----
# Structural invariants (non-intrusiveness, deviation-form exactness,
# monotonicity, cross-cell band consistency) across the four solver-mode
# cells {EXPLICIT,SEMI_IMPLICIT} x {Anderson off,on}. Does not assert band
# *magnitude* against MC (see test_rom_coverage_bellinge.cpp for that).
add_executable(test_rom_solver_mode_compat test_rom_solver_mode_compat.cpp)
target_link_libraries(test_rom_solver_mode_compat
    PRIVATE
        GTest::gtest GTest::gtest_main
        openswmm_engine
)
target_include_directories(test_rom_solver_mode_compat
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/engine
)
set_target_properties(test_rom_solver_mode_compat PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME regression_rom_solver_mode_compat COMMAND $<TARGET_FILE:test_rom_solver_mode_compat>)
set_tests_properties(regression_rom_solver_mode_compat PROPERTIES LABELS "regression")
openswmm_stage_test_runtime_deps(test_rom_solver_mode_compat)

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

    # ---- W4: 2D validation breadth -- correlated-field marcher MC + a second
    # surface (thalweg channel). A MEASUREMENT target (C1/C2 verdicts printed,
    # no band-magnitude asserts). ~150 marcher runs ≈ 12-15 min Debug, so it
    # carries the "slow" label like the Bellinge gate; W4_CELLS/W4_MEMBERS
    # narrow it for iteration (see the test header).
    add_executable(test_2d_rom_marcher_coverage_corr test_2d_rom_marcher_coverage_corr.cpp)
    target_link_libraries(test_2d_rom_marcher_coverage_corr
        PRIVATE
            GTest::gtest GTest::gtest_main
            openswmm_engine
    )
    target_include_directories(test_2d_rom_marcher_coverage_corr
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_2d_rom_marcher_coverage_corr PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    add_test(
        NAME    regression_2d_rom_marcher_coverage_corr
        COMMAND $<TARGET_FILE:test_2d_rom_marcher_coverage_corr>
    )
    set_tests_properties(regression_2d_rom_marcher_coverage_corr PROPERTIES
        LABELS "regression;slow"
        TIMEOUT 2400
        ENVIRONMENT "OPENSWMM_2D_BACKEND=cpu")

    # ---- PR 10 variant: Bellinge model coverage (fast saturation) ----
    # Real 1020-node network with K1d ≈ 0.123 (540× larger than Phase-9 chain).
    # Dominant mode τ_0 ≈ 100 s (vs 15 h), so 1-hour window reaches ~99% saturation.
    # Model + rainfall paths come from OPENSWMM_BELLINGE_INP / _RAIN (defaults
    # under $HOME); the test skips when they are absent. See the test header.
    add_executable(test_rom_coverage_bellinge test_rom_coverage_bellinge.cpp)
    target_link_libraries(test_rom_coverage_bellinge
        PRIVATE
            GTest::gtest GTest::gtest_main
            openswmm_engine
    )
    target_include_directories(test_rom_coverage_bellinge
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_rom_coverage_bellinge PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    add_test(NAME regression_rom_coverage_bellinge COMMAND $<TARGET_FILE:test_rom_coverage_bellinge>)
    # Full 24 h window is ~5 min (65,589 engine steps). Labelled "slow" so
    # `ctest -LE slow` skips it; skips itself if the external data is absent.
    set_tests_properties(regression_rom_coverage_bellinge PROPERTIES
        LABELS "regression;slow"
        TIMEOUT 1500)
    openswmm_stage_test_runtime_deps(test_rom_coverage_bellinge)
    # ---- CL-2a: correlation-length profile of the reduced spatial basis ----
    # Found dormant in the 2026-08-18 sweep: on disk since the mechanical port,
    # never registered. Drives the ROM's setSoftForcing() directly (no
    # [SOFT_RAINGAGES] parser dependency), so unlike its two SR-5 siblings it
    # runs on this base as-is.
    # SpdeSpatialBasis.cpp is in the engine library since SP3 (not recompiled
    # here: ODR).
    add_executable(test_corr_len_profile test_corr_len_profile.cpp)
    target_link_libraries(test_corr_len_profile
        PRIVATE
            GTest::gtest GTest::gtest_main
            openswmm_engine
    )
    target_include_directories(test_corr_len_profile
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_corr_len_profile PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    add_test(NAME regression_corr_len_profile COMMAND $<TARGET_FILE:test_corr_len_profile>)
    set_tests_properties(regression_corr_len_profile PROPERTIES
        LABELS "regression"
        ENVIRONMENT "OPENSWMM_2D_BACKEND=cpu")
    openswmm_stage_test_runtime_deps(test_corr_len_profile)

    # ---- SP1: soft-rainfall ROM bands vs brute-force MC (SR-5) --------------
    # Was a ready-made acceptance gate for the [SOFT_RAINGAGES] re-port; the
    # section now parses and reaches the 1D ROM (SP1), so it runs for real.
    add_executable(test_soft_rain_coverage test_soft_rain_coverage.cpp)
    target_link_libraries(test_soft_rain_coverage
        PRIVATE
            GTest::gtest GTest::gtest_main
            openswmm_engine
    )
    target_include_directories(test_soft_rain_coverage
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_soft_rain_coverage PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    add_test(NAME regression_soft_rain_coverage COMMAND $<TARGET_FILE:test_soft_rain_coverage>)
    # SR-7 (H19): steady rain on steady flow, the regime where the old dh/dt-driven
    # rain channel loses its band (docs/uncertainty/ADDITIVE_CHANNEL.md §6).
    add_test(NAME regression_soft_rain_steady_coverage COMMAND $<TARGET_FILE:test_soft_rain_coverage>)
    set_tests_properties(regression_soft_rain_steady_coverage PROPERTIES LABELS "regression"
        ENVIRONMENT "SR5_STORAGE_AREA=50;SR5_SUBCATCH_AREA=0.5;SR5_SUBCATCH_WIDTH=500;SR5_PIPE_DIAM=3;SR5_CELL=SR-7 soft rain, steady flow")
    set_tests_properties(regression_soft_rain_coverage PROPERTIES LABELS "regression")
    openswmm_stage_test_runtime_deps(test_soft_rain_coverage)

    # ---- Basin behind a weir: per-member storage levels vs brute-force MC ----
    # docs/uncertainty/ADDITIVE_CHANNEL.md §7. Two scenarios (main run spills /
    # stops just below the crest), 2 x 21 MC runs + 4 ROM runs, under a minute.
    add_executable(test_storage_threshold_coverage test_storage_threshold_coverage.cpp)
    target_link_libraries(test_storage_threshold_coverage PRIVATE GTest::gtest GTest::gtest_main openswmm_engine)
    target_include_directories(test_storage_threshold_coverage PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/engine)
    set_target_properties(test_storage_threshold_coverage PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    add_test(NAME regression_storage_threshold_coverage COMMAND $<TARGET_FILE:test_storage_threshold_coverage>)
    set_tests_properties(regression_storage_threshold_coverage PROPERTIES LABELS "regression")
    openswmm_stage_test_runtime_deps(test_storage_threshold_coverage)

    # ---- SP3: correlated soft-rain ROM vs correlated MC (CL-1e / SR-5b) ------
    # Needs CorrelatedFieldGenerator (2D-only compiled) for its MC reference, so
    # it stays inside this 2D block.
    add_executable(test_soft_rain_corr_coverage test_soft_rain_corr_coverage.cpp)
    target_link_libraries(test_soft_rain_corr_coverage
        PRIVATE
            GTest::gtest GTest::gtest_main
            openswmm_engine
    )
    target_include_directories(test_soft_rain_corr_coverage
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_soft_rain_corr_coverage PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    add_test(NAME regression_soft_rain_corr_coverage COMMAND $<TARGET_FILE:test_soft_rain_corr_coverage>)
    set_tests_properties(regression_soft_rain_corr_coverage PROPERTIES LABELS "regression")
    openswmm_stage_test_runtime_deps(test_soft_rain_corr_coverage)
endif()

# ---- H17: 1D ROM vs MC on a network with structures (EPA Example 8) ----
add_executable(test_rom_coverage_structures test_rom_coverage_structures.cpp)
target_link_libraries(test_rom_coverage_structures PRIVATE GTest::gtest GTest::gtest_main openswmm_engine)
target_include_directories(test_rom_coverage_structures PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src/engine)
target_compile_definitions(test_rom_coverage_structures PRIVATE
    REGRESSION_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
set_target_properties(test_rom_coverage_structures PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
add_test(NAME regression_rom_coverage_structures COMMAND $<TARGET_FILE:test_rom_coverage_structures>)
set_tests_properties(regression_rom_coverage_structures PROPERTIES LABELS "regression")
openswmm_stage_test_runtime_deps(test_rom_coverage_structures)
