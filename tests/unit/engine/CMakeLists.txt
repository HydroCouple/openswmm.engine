#
# tests/unit/engine/CMakeLists.txt
#
# Google Test unit tests for the openswmm_engine library.
#
# Each test file compiles to its own executable registered with CTest.
# Tests are fast (< 1 second each) and do not require external data files.
#
# Adding a new test file:
#   1. Create tests/unit/engine/test_<component>.cpp
#   2. Add it to the list below
#   3. Run cmake --build and ctest
#
# Moved from tests/unit/ 2026-03-25.
# See: docs/MASTER_IMPLEMENTATION_PLAN.md Phase 9
#

cmake_minimum_required(VERSION 3.21)

find_package(GTest CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Platform-specific RPATH so the test executable can find shared libraries
# at runtime (mirrors the legacy test configuration).
if(APPLE)
    set(LIB_ROOT "@loader_path;@rpath;@executable_path;@loader_path/../lib;@rpath/../lib;@executable_path/../lib;@executable_path/$CMAKE_BUILD_TYPE")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib;$ORIGIN/$CMAKE_BUILD_TYPE")
endif()

# Helper function to create a unit test executable
function(add_gtest_unit TEST_NAME SOURCE_FILE)
    add_executable(${TEST_NAME} ${SOURCE_FILE})

    target_link_libraries(${TEST_NAME}
        PRIVATE
            GTest::gtest
            GTest::gtest_main
            GTest::gmock
            # On MSVC, link against the internal static library so tests can
            # call non-exported C++ symbols without per-test source tracking.
            $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
            openswmm_legacy_engine
    )

    target_include_directories(${TEST_NAME}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )

    if(MSVC)
        target_compile_options(${TEST_NAME} PRIVATE /EHsc)
    endif()

    set_target_properties(${TEST_NAME} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
        INSTALL_RPATH "${LIB_ROOT}"
    )

    openswmm_stage_test_runtime_deps(${TEST_NAME})

    add_test(
        NAME    ${TEST_NAME}
        COMMAND $<TARGET_FILE:${TEST_NAME}>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
    )
    set_tests_properties(${TEST_NAME} PROPERTIES LABELS "unit")
endfunction()

# ---- Unit test targets ----
# NOTE: Files are created as stubs initially. Implement when the corresponding
#       engine component is written (see MASTER_IMPLEMENTATION_PLAN.md phases).

add_gtest_unit(test_engine_tokenizer         test_tokenizer.cpp)
add_gtest_unit(test_engine_timeseries_cursor test_timeseries_cursor.cpp)
add_gtest_unit(test_engine_options_parser    test_options_parser.cpp)
add_gtest_unit(test_engine_csv_timeseries    test_csv_timeseries.cpp)
add_gtest_unit(test_engine_plugin_lifecycle      test_plugin_lifecycle.cpp)
add_gtest_unit(test_engine_plugin_filter_invariant test_plugin_filter_invariant.cpp)
# Slice RC.5 — GeoPackage explicit-builtin registration + is_builtin flag.
add_gtest_unit(test_engine_pluginfactory_builtins test_pluginfactory_builtins.cpp)
# Slice QA-01.4 — per-node summary stats read from a SWMM_Output handle.
add_gtest_unit(test_engine_output_node_stats     test_output_node_stats.cpp)
add_gtest_unit(test_engine_model_write_with_plugin test_model_write_with_plugin.cpp)
add_gtest_unit(test_engine_files_section          test_files_section.cpp)
add_gtest_unit(test_engine_events_section         test_events_section.cpp)
add_gtest_unit(test_engine_report_options_api     test_report_options_api.cpp)
add_gtest_unit(test_engine_hotstart          test_hotstart.cpp)
add_gtest_unit(test_engine_io_thread         test_io_thread.cpp)
add_gtest_unit(test_engine_timestep_ctrl     test_timestep_controller.cpp)
add_gtest_unit(test_engine_simd_math         test_simd_math.cpp)
add_gtest_unit(test_engine_mathexpr          test_mathexpr.cpp)
add_gtest_unit(test_engine_user_flags        test_user_flags.cpp)
add_gtest_unit(test_engine_section_registry  test_section_registry.cpp)
add_gtest_unit(test_engine_crs               test_crs.cpp)
add_gtest_unit(test_engine_xsection          test_xsection.cpp)
# STREET cross-section end-to-end (parse → street transect → routing).
add_gtest_unit(test_engine_street_xsect      test_street_xsect.cpp)
# STREET cross-section programmatic builder API (set/get xsect + get_params).
add_gtest_unit(test_engine_street_xsect_api  test_street_xsect_api.cpp)


# Golden cross-section parity harness — diffs the new xsect:: implementation
# against the legacy xsect.c. The legacy dylib hides its internal xsect_*
# symbols, so we compile xsect.c (+ findroot.c + stub globals) directly into the
# test executable. The stub globals satisfy the linker for the IRREGULAR/CUSTOM/
# STREET code paths the harness does not exercise.
add_executable(test_engine_xsect_parity
    test_xsect_parity.cpp
    ${PROJECT_SOURCE_DIR}/src/legacy/engine/xsect.c
    ${PROJECT_SOURCE_DIR}/src/legacy/engine/findroot.c
    legacy_xsect_stubs.c
)
target_link_libraries(test_engine_xsect_parity
    PRIVATE
        GTest::gtest GTest::gtest_main GTest::gmock
        $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
)
target_include_directories(test_engine_xsect_parity
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/engine
        ${PROJECT_SOURCE_DIR}/src/legacy/engine
        ${PROJECT_BINARY_DIR}/src/legacy/engine   # generated legacy_version.h
)
if(MSVC)
    target_compile_options(test_engine_xsect_parity PRIVATE /EHsc)
endif()
set_target_properties(test_engine_xsect_parity PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
    INSTALL_RPATH "${LIB_ROOT}"
)
openswmm_stage_test_runtime_deps(test_engine_xsect_parity)
add_test(
    NAME    test_engine_xsect_parity
    COMMAND $<TARGET_FILE:test_engine_xsect_parity>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
)
set_tests_properties(test_engine_xsect_parity PROPERTIES LABELS "unit")


add_gtest_unit(test_engine_infiltration     test_infiltration.cpp)
add_gtest_unit(test_engine_runoff           test_runoff.cpp)
add_gtest_unit(test_engine_snow             test_snow.cpp)
add_gtest_unit(test_engine_gage_scalefactor test_gage_scalefactor.cpp)
add_gtest_unit(test_engine_climate          test_climate.cpp)
add_gtest_unit(test_engine_lid              test_lid.cpp)
add_gtest_unit(test_engine_groundwater      test_groundwater.cpp)
add_gtest_unit(test_engine_routing          test_routing.cpp)
add_gtest_unit(test_engine_quality_routing  test_quality_routing.cpp)
add_gtest_unit(test_engine_treatment        test_treatment.cpp)
add_gtest_unit(test_engine_rdii             test_rdii.cpp)
add_gtest_unit(test_engine_hydrograph_mutation test_hydrograph_mutation_api.cpp)
add_gtest_unit(test_engine_pattern_mutation  test_pattern_mutation_api.cpp)
add_gtest_unit(test_engine_transect_mutation test_transect_mutation_api.cpp)
add_gtest_unit(test_engine_transect_inp_parser test_transect_inp_parser.cpp)
add_gtest_unit(test_engine_da4_api          test_da4_engine_api.cpp)
add_gtest_unit(test_engine_controls         test_controls.cpp)
add_gtest_unit(test_engine_control_rule_validate test_control_rule_validate_api.cpp)
add_gtest_unit(test_engine_controls_legacy_parity test_controls_legacy_parity.cpp)
add_gtest_unit(test_engine_gap_fixes       test_gap_fixes.cpp)
add_gtest_unit(test_engine_links_pump_stats_bulk test_links_pump_stats_bulk.cpp)
add_gtest_unit(test_engine_nodes_bulk_phase3 test_nodes_bulk_phase3.cpp)
add_gtest_unit(test_engine_links_bulk_phase3 test_links_bulk_phase3.cpp)
add_gtest_unit(test_engine_subcatchments_bulk_phase3 test_subcatchments_bulk_phase3.cpp)
add_gtest_unit(test_engine_statistics_bulk_phase3 test_statistics_bulk_phase3.cpp)
add_gtest_unit(test_engine_runoff_interface_capi test_runoff_interface_capi.cpp)
add_gtest_unit(test_engine_report_section  test_report_section.cpp)
add_gtest_unit(test_engine_concurrent       test_concurrent_engines.cpp)
add_gtest_unit(test_engine_object_deletion test_object_deletion.cpp)
add_gtest_unit(test_engine_type_conversion test_type_conversion.cpp)
add_gtest_unit(test_engine_tags           test_tags.cpp)
# Engine gaps BN-LINK-01a / -01b — symmetric getters for initial-flow /
# max-flow (added 2026-05-25 for Slice SB of the GUI plan).
add_gtest_unit(test_engine_link_flow_roundtrip test_link_flow_roundtrip.cpp)

# End-to-end model-builder parity: build site_drainage_model.inp via the
# programmatic API, run it, write it back out, and confirm result parity
# against a fresh run of the reference .inp.
add_gtest_unit(test_engine_site_drainage_builder test_site_drainage_builder.cpp)

# 2D surface routing tests — geometry, gradients, flux, parsing.
# Non-CVODE portions can be built without SUNDIALS by compiling the needed
# sources directly; the HDF5 output plugin test is gated on OPENSWMM_BUILD_2D
# because it needs the real Default2DOutputPlugin + libhdf5.
set(TEST_2D_SOURCES
    test_2d_surface_routing.cpp
    ${PROJECT_SOURCE_DIR}/src/engine/2d/mesh/MeshBuilder.cpp
    ${PROJECT_SOURCE_DIR}/src/engine/2d/mesh/VertexReconstruction.cpp
    ${PROJECT_SOURCE_DIR}/src/engine/2d/solver/SurfaceFluxCalculator.cpp
    ${PROJECT_SOURCE_DIR}/src/engine/2d/input/SectionHandlers2D.cpp
)
if(OPENSWMM_BUILD_2D)
    list(APPEND TEST_2D_SOURCES
        ${PROJECT_SOURCE_DIR}/src/engine/2d/output/Default2DOutputPlugin.cpp)
endif()
add_executable(test_engine_2d_surface ${TEST_2D_SOURCES})
target_link_libraries(test_engine_2d_surface
    PRIVATE
        GTest::gtest GTest::gtest_main GTest::gmock
        $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
        openswmm_legacy_engine
)
if(OPENSWMM_BUILD_2D)
    # HDF5 is required by Default2DOutputPlugin. The engine target resolves it
    # at configure time — prefer the imported target it set up (visible globally),
    # falling back to FindHDF5 variables for system installs.
    if(TARGET hdf5::hdf5-shared)
        target_link_libraries(test_engine_2d_surface PRIVATE hdf5::hdf5-shared)
    elseif(TARGET hdf5::hdf5-static)
        target_link_libraries(test_engine_2d_surface PRIVATE hdf5::hdf5-static)
    else()
        if(NOT HDF5_FOUND)
            find_package(HDF5 REQUIRED COMPONENTS C)
        endif()
        target_link_libraries(test_engine_2d_surface PRIVATE ${HDF5_C_LIBRARIES})
        target_include_directories(test_engine_2d_surface PRIVATE ${HDF5_INCLUDE_DIRS})
    endif()
endif()
target_include_directories(test_engine_2d_surface
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/engine
)
if(MSVC)
    target_compile_options(test_engine_2d_surface PRIVATE /EHsc)
endif()
set_target_properties(test_engine_2d_surface PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
    INSTALL_RPATH "${LIB_ROOT}"
)
openswmm_stage_test_runtime_deps(test_engine_2d_surface)
add_test(
    NAME    test_engine_2d_surface
    COMMAND $<TARGET_FILE:test_engine_2d_surface>
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
)
set_tests_properties(test_engine_2d_surface PROPERTIES LABELS "unit")

# GeoPackage tests (only if the library is built)
if(OPENSWMM_WITH_GEOPACKAGE)
    add_executable(test_engine_geopackage test_geopackage.cpp)
    target_link_libraries(test_engine_geopackage
        PRIVATE
            GTest::gtest GTest::gtest_main GTest::gmock
            $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
            openswmm_geopackage
            openswmm_legacy_engine
    )
    target_include_directories(test_engine_geopackage
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_engine_geopackage PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
        INSTALL_RPATH "${LIB_ROOT}"
    )
    openswmm_stage_test_runtime_deps(test_engine_geopackage)
    add_test(
        NAME    test_engine_geopackage
        COMMAND $<TARGET_FILE:test_engine_geopackage>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
    )
    set_tests_properties(test_engine_geopackage PROPERTIES LABELS "unit")
endif()
