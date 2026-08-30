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

# ---- Floating-point reproducibility for test TUs ----
# Must match the engine and legacy-engine targets (src/engine/CMakeLists.txt,
# src/legacy/engine/CMakeLists.txt). Linking the engine is NOT enough: the hot
# geometry kernels are header-inline (OPENSWMM_KERNEL_FN in XSectKernels.hpp,
# FvKernels.hpp, ChebSection*.hpp), so every test translation unit compiles its
# OWN copy of them under its own flags. With contraction left enabled here, a
# test comparing the library's instantiation against its own would see the same
# source expression fused differently and differ in the last ulp — which is
# precisely what XSectBitExact and XsectKernels were reporting.
#
# Applied at directory scope so it reaches targets built by add_gtest_unit()
# and the handful created with a bare add_executable() alike.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-fno-fast-math -ffp-contract=off -fno-math-errno)
endif()

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
    # These two are built by add_solver_test() like the rest, but that helper
    # only creates the executable -- ctest registration is this hand-kept list,
    # so a target added there and not here BUILDS AND NEVER RUNS.
    add_test(NAME test_legacy_solver_transect_nc
        COMMAND $<TARGET_FILE:test_solver_transect_nc>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/engine/data)
    add_test(NAME test_legacy_solver_backflow_quality
        COMMAND $<TARGET_FILE:test_solver_backflow_quality>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/engine/data)
    # NOT the data directory: this one reads "./data/gage_scalefactor.inp",
    # so its working directory is the PARENT of the others'.
    add_test(NAME test_legacy_gage_scalefactor
        COMMAND $<TARGET_FILE:test_gage_scalefactor>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/unit/legacy/engine)
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
        test_legacy_solver_transect_nc
        test_legacy_gage_scalefactor
        test_legacy_output
        PROPERTIES LABELS "unit"
    )

    # ---- Load-optimization gates -------------------------------------------
    # Sources live under benchmarks/ but the gates belong here: neither needs
    # Google Benchmark, and tests/benchmarks/ is only added when
    # OPENSWMM_BUILD_BENCHMARKS is ON — a configuration that has
    # enable_testing() off, so add_test() there would silently register
    # nothing. See plans/MODEL_LOAD_INIT_OPTIMIZATION_PLAN_2026-08-12.md §0.4.

    # parity_probe emits the .rpt / .out / written-back .inp that the parity
    # gate diffs; the shipped CLI has no write-back flag.
    add_executable(parity_probe ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/parity_probe.cpp)
    target_link_libraries(parity_probe PRIVATE openswmm_engine)
    target_include_directories(parity_probe PRIVATE ${PROJECT_SOURCE_DIR}/include)
    set_target_properties(parity_probe PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON)
    openswmm_stage_test_runtime_deps(parity_probe)

    # Determinism gate: two runs of the same build must produce identical
    # artifacts. Without this, a `check` failure after an optimization could
    # not be distinguished from run-to-run noise.
    if(UNIX)
        add_test(NAME load_parity_selfcheck
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/scripts/load_parity.sh
                    selfcheck $<TARGET_FILE:parity_probe>)
        set_tests_properties(load_parity_selfcheck PROPERTIES LABELS "unit")
    endif()

    # Generator gate: proves gen_load_bench.py still emits every corpus model.
    # Uses the tiny --smoke sizes so it costs seconds, not gigabytes.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME bench_corpus_generator_smoke
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/scripts/gen_load_bench.py
                    --smoke --force
                    --out-dir ${CMAKE_CURRENT_BINARY_DIR}/corpus_smoke)
        set_tests_properties(bench_corpus_generator_smoke PROPERTIES LABELS "unit")
    endif()
endif()

# ---- Regression tests ----
if(OPENSWMM_BUILD_REGRESSION_TESTS)
    add_subdirectory(regression)
endif()

# Benchmarks are added from root CMakeLists when OPENSWMM_BUILD_BENCHMARKS is ON.
# Do not add_subdirectory(benchmarks) here.
