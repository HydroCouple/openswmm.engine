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

# Windows: stage every runtime DLL the test depends on (project libs +
# vcpkg imported targets like SUNDIALS / HDF5 / sqlite3) next to the exe.
# Without this the loader fails with STATUS_DLL_NOT_FOUND (0xc0000135)
# at test launch, since the engine DLLs are built in src/engine/$<CONFIG>/
# and src/legacy/engine/$<CONFIG>/ rather than alongside the exe, and
# vcpkg's applocal step only stages vcpkg-managed DLLs.
#
# Why three TARGET_RUNTIME_DLLS lookups: the genex walks PRIVATE deps of the
# named target itself, but only INTERFACE deps when recursing through deps-of-
# deps. openswmm_engine links SUNDIALS::cvode and hdf5::hdf5-shared PRIVATELY,
# so $<TARGET_RUNTIME_DLLS:test_engine_regression> yields the two engine DLLs
# but NOT sundials_cvode.dll / hdf5.dll. The Windows loader searches the EXE's
# directory (not the importing DLL's directory) for transitive imports, so we
# must also ask each shared engine target for its own runtime DLLs and stage
# them alongside the test exe.
if(WIN32)
    add_custom_command(TARGET test_engine_regression POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:test_engine_regression>
            $<TARGET_RUNTIME_DLLS:openswmm_engine>
            $<TARGET_RUNTIME_DLLS:openswmm_legacy_engine>
            $<TARGET_FILE_DIR:test_engine_regression>
        COMMAND_EXPAND_LISTS
    )
endif()
