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
# Build flags:
#   -DOPENSWMM_BUILD_TESTS=ON             Build everything (default)
#   -DOPENSWMM_BUILD_UNIT_TESTS=ON        Build unit tests only
#   -DOPENSWMM_BUILD_REGRESSION_TESTS=ON  Build regression tests only
#
# CTest labels:
#   ctest -L unit          Run unit tests only
#   ctest -L regression    Run regression tests only
#   ctest                  Run all tests
#
# Updated: 2026-03-25 — removed Boost.Test dependency; all tests now use Google Test.
# Updated: 2026-03-25 — reorganised into unit/ subtree (legacy/ moved under unit/).
# Updated: 2026-03-27 — split OPENSWMM_BUILD_TESTS into unit / regression flags.
#

cmake_minimum_required(VERSION 3.21)

# ---- Google Test (required for any test target) ----
find_package(GTest CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Directory with test input data files
set(TEST_BIN_DIRECTORY ${CMAKE_BINARY_DIR}/bin/$<CONFIG>)
set(TEST_DATA_ROOT     ${CMAKE_CURRENT_SOURCE_DIR})

# Stage every shared-library dependency next to a test executable so ctest
# can launch it without DYLD/LD/PATH gymnastics. Generalises the Windows-only
# recipe that previously lived in tests/regression/CMakeLists.txt — it was
# necessary there because the loader searches the EXE's directory, not the
# importing DLL's directory, for transitive imports.
#
# Why three TARGET_RUNTIME_DLLS lookups on Windows: $<TARGET_RUNTIME_DLLS:T>
# walks PRIVATE deps of T itself but only INTERFACE deps when recursing.
# openswmm_engine links SUNDIALS / HDF5 / sqlite3 PRIVATELY, so without the
# extra lookups against each engine target the third-party DLLs are missed.
#
# On Unix the test exes already have $ORIGIN / @loader_path in RPATH (see
# the LIB_ROOT blocks in every test CMakeLists), so colocating dylibs by
# copying the imported targets' $<TARGET_FILE:…> is enough.
function(openswmm_stage_test_runtime_deps TARGET_NAME)
    if(WIN32)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${TARGET_NAME}>
                $<TARGET_RUNTIME_DLLS:openswmm_engine>
                $<TARGET_RUNTIME_DLLS:openswmm_legacy_engine>
                $<TARGET_FILE_DIR:${TARGET_NAME}>
            COMMAND_EXPAND_LISTS
        )
    else()
        # Unix: copy each imported third-party shared lib (when defined)
        # next to the exe. Targets that don't exist in this configuration
        # are silently skipped via $<TARGET_NAME_IF_EXISTS:…>.
        set(_imported_deps
            SUNDIALS::cvode
            SUNDIALS::nvecserial
            SUNDIALS::sunmatrixdense
            SUNDIALS::sunlinsoldense
            hdf5::hdf5-shared
            OpenMP::OpenMP_CXX
            unofficial::sqlite3::sqlite3
            SQLite::SQLite3
        )
        foreach(_dep IN LISTS _imported_deps)
            if(TARGET ${_dep})
                # Wrap each copy in its own command so a missing IMPORTED_LOCATION
                # on one target doesn't break the whole step (e.g. interface
                # libraries that point to OpenMP_CXX_FLAGS only).
                get_target_property(_dep_type ${_dep} TYPE)
                if(_dep_type STREQUAL "SHARED_LIBRARY")
                    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            $<TARGET_FILE:${_dep}>
                            $<TARGET_FILE_DIR:${TARGET_NAME}>
                    )
                endif()
            endif()
        endforeach()

        # Apple Homebrew libomp is exposed via OpenMP::OpenMP_CXX as an
        # INTERFACE library, not a SHARED imported target — its dylib path
        # lives in the OpenMP_omp_LIBRARY cache variable.
        if(APPLE AND OpenMP_omp_LIBRARY AND EXISTS "${OpenMP_omp_LIBRARY}")
            add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${OpenMP_omp_LIBRARY}"
                    $<TARGET_FILE_DIR:${TARGET_NAME}>
            )
        endif()
    endif()
endfunction()

# ---- Unit tests (legacy + new engine) ----
if(OPENSWMM_BUILD_UNIT_TESTS)
    add_subdirectory(unit)

    # Legacy solver tests
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
    add_test(NAME test_legacy_solver_storage_shapes
        COMMAND $<TARGET_FILE:test_solver_storage_shapes>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/engine/data)
    add_test(NAME test_legacy_output
        COMMAND $<TARGET_FILE:test_output>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/output/data)

    # Label all unit tests
    set_tests_properties(
        test_legacy_solver_api
        test_legacy_solver_errors
        test_legacy_solver_hotstart
        test_legacy_solver_shapes
        test_legacy_solver_expanded_api
        test_legacy_solver_storage_shapes
        test_legacy_output
        PROPERTIES LABELS "unit"
    )
endif()

# ---- Regression tests ----
if(OPENSWMM_BUILD_REGRESSION_TESTS)
    add_subdirectory(regression)
endif()

# Benchmarks are added from root CMakeLists when OPENSWMM_BUILD_BENCHMARKS is ON.
# Do not add_subdirectory(benchmarks) here.
