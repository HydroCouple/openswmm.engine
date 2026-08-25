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
    # Pin the serial CPU 2D solver so unit assertions are deterministic and
    # bit-exact. With the GPU plugin now built+installed by default, runtime
    # `auto` would otherwise pick the Kokkos-OpenMP backend (results agree only
    # to ~1e-13, not bit-identical). The dedicated `test_engine_2d_omp_default`
    # test below exercises the omp default path explicitly.
    set_tests_properties(${TEST_NAME} PROPERTIES
        LABELS "unit"
        ENVIRONMENT "OPENSWMM_2D_BACKEND=cpu")
endfunction()

# ---- Unit test targets ----
# NOTE: Files are created as stubs initially. Implement when the corresponding
#       engine component is written (see MASTER_IMPLEMENTATION_PLAN.md phases).

add_gtest_unit(test_engine_tokenizer         test_tokenizer.cpp)
add_gtest_unit(test_engine_name_index        test_name_index.cpp)
add_gtest_unit(test_engine_name_case_parity  test_name_case_parity.cpp)
add_gtest_unit(test_engine_options_parser    test_options_parser.cpp)
add_gtest_unit(test_engine_timeseries_file_roundtrip test_timeseries_file_roundtrip.cpp)
add_gtest_unit(test_engine_pathresolver      test_pathresolver.cpp)
add_gtest_unit(test_engine_postparse_resolver test_postparse_resolver.cpp)
add_gtest_unit(test_engine_warning_error_parity test_warning_error_parity.cpp)
add_gtest_unit(test_engine_outfall_stage_data test_outfall_stage_data.cpp)
add_gtest_unit(test_engine_inp_writer_relative_paths test_inp_writer_relative_paths.cpp)
add_gtest_unit(test_engine_inp_writer_saveas_paths test_inp_writer_saveas_paths.cpp)
add_gtest_unit(test_engine_inp_writer_roundtrip test_inp_writer_roundtrip.cpp)
add_gtest_unit(test_engine_plugin_lifecycle      test_plugin_lifecycle.cpp)
add_gtest_unit(test_engine_plugin_filter_invariant test_plugin_filter_invariant.cpp)
# Slice RC.5 — GeoPackage explicit-builtin registration + is_builtin flag.
add_gtest_unit(test_engine_pluginfactory_builtins test_pluginfactory_builtins.cpp)
# Slice QA-01.4 — per-node summary stats read from a SWMM_Output handle.
add_gtest_unit(test_engine_output_node_stats     test_output_node_stats.cpp)
add_gtest_unit(test_engine_model_write_with_plugin test_model_write_with_plugin.cpp)
add_gtest_unit(test_engine_files_section          test_files_section.cpp)
add_gtest_unit(test_engine_iface_routing          test_iface_routing.cpp)
add_gtest_unit(test_engine_files_iface_gaps       test_files_iface_gaps.cpp)
add_gtest_unit(test_engine_section_order_parity   test_section_order_parity.cpp)
add_gtest_unit(test_engine_capi_file_paths        test_capi_file_paths.cpp)
add_gtest_unit(test_engine_events_section         test_events_section.cpp)
add_gtest_unit(test_engine_report_options_api     test_report_options_api.cpp)
# Legacy parity for the .rpt analysis-timing block: the wall clock starts at
# open() (before parsing) as in report_writeLogo(), and elapsed time rolls
# whole days into a "d." prefix as in report_writeSysTime().
add_gtest_unit(test_engine_report_timing          test_report_timing.cpp)
add_gtest_unit(test_engine_ard_transport          test_ard_transport.cpp)
add_gtest_unit(test_engine_process_components     test_process_components.cpp)
add_gtest_unit(test_engine_reactions_config       test_reactions_config.cpp)
add_gtest_unit(test_engine_reaction_expressions   test_reaction_expressions.cpp)
add_gtest_unit(test_engine_reaction_integrators   test_reaction_integrators.cpp)
add_gtest_unit(test_engine_reaction_legacy_binding test_reaction_legacy_binding.cpp)
add_gtest_unit(test_engine_ard_dispersion         test_ard_dispersion.cpp)
add_gtest_unit(test_engine_reaction_ard_binding   test_reaction_ard_binding.cpp)
add_gtest_unit(test_engine_ard_transport_bcs      test_ard_transport_bcs.cpp)
add_gtest_unit(test_engine_ard_e5b                test_ard_e5b.cpp)
add_gtest_unit(test_engine_ard_node_store         test_ard_node_store.cpp)
add_gtest_unit(test_engine_lard_wiring            test_lard_wiring.cpp)
add_gtest_unit(test_engine_lard_transport         test_lard_transport.cpp)
add_gtest_unit(test_engine_lard_age               test_lard_age.cpp)
add_gtest_unit(test_engine_lard_dt_reference      test_lard_dt_reference.cpp)
add_gtest_unit(test_engine_lard_rwpt              test_lard_rwpt.cpp)
add_gtest_unit(test_engine_negative_sources       test_negative_sources.cpp)
add_gtest_unit(test_engine_water_age_api          test_water_age_api.cpp)
add_gtest_unit(test_engine_transport_options_api  test_transport_options_api.cpp)
add_gtest_unit(test_engine_options_malformed      test_options_malformed_values.cpp)
add_gtest_unit(test_engine_water_age              test_water_age.cpp)
add_gtest_unit(test_engine_water_age_watershed    test_water_age_watershed.cpp)
add_gtest_unit(test_engine_water_age_lid          test_water_age_lid.cpp)
add_gtest_unit(test_engine_heat_watershed         test_heat_watershed.cpp)
add_gtest_unit(test_engine_heat_integrator        test_heat_integrator.cpp)
add_gtest_unit(test_engine_heat_lid               test_heat_lid.cpp)
add_gtest_unit(test_engine_transport_dt_reference test_transport_dt_reference.cpp)
add_gtest_unit(test_engine_transport_snow       test_transport_snow.cpp)
add_gtest_unit(test_engine_heat_transport         test_heat_transport.cpp)
add_gtest_unit(test_engine_heat_surface_exchange  test_heat_surface_exchange.cpp)
add_gtest_unit(test_engine_heat_radiative_exchange test_heat_radiative_exchange.cpp)
add_gtest_unit(test_engine_heat_ard_binding       test_heat_ard_binding.cpp)
add_gtest_unit(test_engine_output_quality         test_output_quality.cpp)
# In-place rename for snow packs / aquifers / inlets / streets / LID controls.
# Without these APIs the GUI registries flushed a renamed object as a NEW one,
# duplicating it in the engine and orphaning the original.
add_gtest_unit(test_engine_object_rename          test_object_rename.cpp)
add_gtest_unit(test_engine_options_api_units      test_options_api_units.cpp)
add_gtest_unit(test_engine_climate_api             test_climate_api.cpp)
add_gtest_unit(test_engine_hotstart          test_hotstart.cpp)
add_gtest_unit(test_engine_simd_math         test_simd_math.cpp)
add_gtest_unit(test_engine_mathexpr          test_mathexpr.cpp)
add_gtest_unit(test_engine_user_flags        test_user_flags.cpp)
# User-flags C API surface for the GUI (Phase 0 of
# openswmm.gui/docs/USER_FLAGS_UI_PLAN_2026-06-03.md).
add_gtest_unit(test_engine_user_flags_capi   test_user_flags_capi.cpp)
add_gtest_unit(test_engine_section_registry  test_section_registry.cpp)
add_gtest_unit(test_engine_xsection          test_xsection.cpp)
target_compile_definitions(test_engine_xsection PRIVATE
    "BENCHMARK_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/benchmarks\""
)
# STREET cross-section end-to-end (parse → street transect → routing).
add_gtest_unit(test_engine_street_xsect      test_street_xsect.cpp)
# ctx.transect_tables must keep cached element pointers valid as it grows —
# XSectBatch and Link cache raw double* into its elements at initialize() and
# swmm_link_set_xsect can append afterwards. Guards the deque choice.
add_gtest_unit(test_engine_transect_table_stability test_transect_table_stability.cpp)
# STREET cross-section programmatic builder API (set/get xsect + get_params).
add_gtest_unit(test_engine_street_xsect_api  test_street_xsect_api.cpp)
# Standalone cross-section geometry API + SWMM_XSectShape enum parity guard.
add_gtest_unit(test_engine_xsect_api         test_xsect_api.cpp)
# Portable cross-section kernels (plan §5.1): the public accessors route through
# the moved bodies, and an evaluator over INDEPENDENT copies of the geometry
# tables reproduces the host one at ULP zero — the device-rebinding claim,
# checked on the host across the full shape catalog and depth range.
add_gtest_unit(test_engine_xsect_kernels_parity test_xsect_kernels_parity.cpp)
# Same reason as test_engine_xsect_parity below: XSectLookup.hpp switches its
# normalization and interpolation arithmetic on SWMM_XSECT_FAST_LOOKUP, so a
# test TU built without the engine's setting would compare the header compiled
# in one numerical mode against the library compiled in the other.
if(OPENSWMM_FAST_XSECT_LOOKUP)
    target_compile_definitions(test_engine_xsect_kernels_parity
                               PRIVATE SWMM_XSECT_FAST_LOOKUP)
endif()
# GUI editor round-trip getters/setters (pollutant units, aquifer ET pattern,
# snowpack surfaces, inlet params/type, LID layers/type).
add_gtest_unit(test_engine_editor_roundtrip_api test_editor_roundtrip_api.cpp)
add_gtest_unit(test_engine_virtual_junction test_virtual_junction.cpp)
add_gtest_unit(test_engine_fv_node_head_consistency test_fv_node_head_consistency.cpp)
add_gtest_unit(test_engine_fv_slot_volume test_fv_slot_volume.cpp)
add_gtest_unit(test_engine_fv_closure_shapes test_fv_closure_shapes.cpp)

add_gtest_unit(test_engine_infiltration     test_infiltration.cpp)
target_compile_definitions(test_engine_infiltration PRIVATE
    "BENCHMARK_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/benchmarks\""
)
add_gtest_unit(test_engine_ode_solver       test_ode_solver.cpp)
target_compile_definitions(test_engine_ode_solver PRIVATE
    "BENCHMARK_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/benchmarks\""
)
add_gtest_unit(test_engine_massbalance      test_massbalance.cpp)
# Runtime-effect coverage for the [OPTIONS] IGNORE_* process flags (closes the
# gap that left five of the six flags as parsed-but-dead no-ops).
add_gtest_unit(test_engine_ignore_flags     test_ignore_flags.cpp)
add_gtest_unit(test_engine_runoff           test_runoff.cpp)
add_gtest_unit(test_engine_snow             test_snow.cpp)
add_gtest_unit(test_engine_gage_scalefactor test_gage_scalefactor.cpp)
add_gtest_unit(test_engine_subcatch_scale_factors test_subcatch_scale_factors.cpp)
add_gtest_unit(test_engine_gage_attributes  test_gage_attributes.cpp)
add_gtest_unit(test_engine_gage_file_roundtrip test_gage_file_roundtrip.cpp)

# USER_CSV rain-file loading (documented in the manual, previously never read —
# such a gage contributed zero rainfall silently) plus the guard that the
# resolved-series API cannot drift from what updateAllGages applies.
add_gtest_unit(test_engine_gage_rain_series test_gage_rain_series.cpp)
# Parse-once multi-column series files (CSV/TSV/PCSWMM TSF) — wide rows,
# AM/PM datetimes, delimiter sniff, single-read cache. See
# plans/MULTICOLUMN_SERIES_SINGLE_READ_2026-08-17.md.
add_gtest_unit(test_engine_multicolumn_series_file test_multicolumn_series_file.cpp)
# B1 regression: set_station_id / set_filename must not corrupt a USER_CSV
# gage's file format or column binding; new file-column accessors.
add_gtest_unit(test_engine_gage_format_preservation test_gage_format_preservation.cpp)
add_gtest_unit(test_engine_climate          test_climate.cpp)
add_gtest_unit(test_engine_lid              test_lid.cpp)
target_compile_definitions(test_engine_lid PRIVATE
    "BENCHMARK_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/benchmarks\""
)
add_gtest_unit(test_engine_groundwater      test_groundwater.cpp)
target_compile_definitions(test_engine_groundwater PRIVATE
    "BENCHMARK_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/benchmarks\""
)
add_gtest_unit(test_engine_routing          test_routing.cpp)
target_compile_definitions(test_engine_routing PRIVATE
    "BENCHMARK_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/benchmarks\""
)
add_gtest_unit(test_engine_quality_routing  test_quality_routing.cpp)
target_compile_definitions(test_engine_quality_routing PRIVATE
    "BENCHMARK_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/benchmarks\""
)
add_gtest_unit(test_engine_exfiltration      test_exfiltration.cpp)
target_compile_definitions(test_engine_exfiltration PRIVATE
    "BENCHMARK_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/benchmarks\""
)
# Presence guard: fails (does not skip) if any manufactured benchmark
# reference.csv is missing or truncated, so the data-driven benchmark tests'
# GTEST_SKIP()-on-missing fallback cannot silently drop coverage in CI.
add_gtest_unit(test_engine_benchmark_corpus  test_benchmark_corpus.cpp)
target_compile_definitions(test_engine_benchmark_corpus PRIVATE
    "BENCHMARK_DATA_DIR=\"${PROJECT_SOURCE_DIR}/tests/benchmarks\""
)
add_gtest_unit(test_engine_treatment        test_treatment.cpp)
add_gtest_unit(test_engine_rdii             test_rdii.cpp)
add_gtest_unit(test_engine_hydrograph_mutation test_hydrograph_mutation_api.cpp)
add_gtest_unit(test_engine_pattern_mutation  test_pattern_mutation_api.cpp)
add_gtest_unit(test_engine_transect_mutation test_transect_mutation_api.cpp)
add_gtest_unit(test_engine_transect_inp_parser test_transect_inp_parser.cpp)
add_gtest_unit(test_engine_da4_api          test_da4_engine_api.cpp)
add_gtest_unit(test_engine_controls         test_controls.cpp)
add_gtest_unit(test_engine_control_rule_validate test_control_rule_validate_api.cpp)
add_gtest_unit(test_engine_control_rule_runtime_add test_control_rule_runtime_add.cpp)
add_gtest_unit(test_engine_controls_legacy_parity test_controls_legacy_parity.cpp)
add_gtest_unit(test_engine_gap_fixes       test_gap_fixes.cpp)
add_gtest_unit(test_engine_storage_shapes  test_storage_shapes.cpp)
add_gtest_unit(test_engine_storage_shape_roundtrip test_storage_shape_roundtrip.cpp)
add_gtest_unit(test_engine_links_pump_stats_bulk test_links_pump_stats_bulk.cpp)
add_gtest_unit(test_engine_pump_ideal_curve test_pump_ideal_curve.cpp)
add_gtest_unit(test_engine_nodes_bulk_phase3 test_nodes_bulk_phase3.cpp)
add_gtest_unit(test_engine_links_bulk_phase3 test_links_bulk_phase3.cpp)
add_gtest_unit(test_engine_subcatchments_bulk_phase3 test_subcatchments_bulk_phase3.cpp)
add_gtest_unit(test_engine_subcatch_editor_api test_subcatch_editor_api.cpp)
add_gtest_unit(test_engine_statistics_bulk_phase3 test_statistics_bulk_phase3.cpp)
add_gtest_unit(test_engine_runoff_interface_capi test_runoff_interface_capi.cpp)
add_gtest_unit(test_engine_report_section  test_report_section.cpp)
add_gtest_unit(test_engine_concurrent       test_concurrent_engines.cpp)
add_gtest_unit(test_engine_object_deletion test_object_deletion.cpp)
add_gtest_unit(test_engine_object_deletion_ext test_object_deletion_ext.cpp)
add_gtest_unit(test_engine_quality_roundtrip test_quality_roundtrip.cpp)
add_gtest_unit(test_engine_type_conversion test_type_conversion.cpp)
add_gtest_unit(test_engine_tags           test_tags.cpp)
# Engine gaps BN-LINK-01a / -01b — symmetric getters for initial-flow /
# max-flow (added 2026-05-25 for Slice SB of the GUI plan).
add_gtest_unit(test_engine_link_flow_roundtrip test_link_flow_roundtrip.cpp)

# End-to-end model-builder parity: build site_drainage_model.inp via the
# programmatic API, run it, write it back out, and confirm result parity
# against a fresh run of the reference .inp.
add_gtest_unit(test_engine_site_drainage_builder test_site_drainage_builder.cpp)

# ---- Serialize tests that share the site_drainage_model.out fixture ----
# These targets all run site_drainage_model.inp with site_drainage_model.out as
# the output path (swmm_engine_open truncates/recreates it), while
# test_engine_output_node_stats opens that same file read-only as its fixture.
# All share data/ as their WORKING_DIRECTORY, so under `ctest -jN` a writer
# truncating the file races the reader's open → intermittent
# "Failed to open fixture 'site_drainage_model.out'" failures. A shared
# RESOURCE_LOCK makes CTest never schedule these concurrently; they still run in
# parallel with the rest of the suite.
set_property(TEST
    test_engine_output_node_stats
    test_engine_links_bulk_phase3
    test_engine_links_pump_stats_bulk
    test_engine_nodes_bulk_phase3
    test_engine_subcatchments_bulk_phase3
    test_engine_subcatch_editor_api
    test_engine_statistics_bulk_phase3
    test_engine_runoff_interface_capi
    test_engine_site_drainage_builder
    APPEND PROPERTY RESOURCE_LOCK site_drainage_model_out)

# ---- Same defect, second fixture: REMOVED AT THE ROOT, not locked ----
# `test_engine_heat_watershed` and `test_engine_heat_lid` both WROTE
# `_h5b.inp` / `_h5b.heat` into the shared data/ working directory and then
# opened them. Under `ctest -jN` one suite's config landed between the other's
# write and its open: measured 8 failures in 8 concurrent runs, every one
# `SURFACE_EXCHANGE did not parse` — the gate's SETUP catching the WRONG FILE
# rather than a wrong number. Both passed alone.
#
# A RESOURCE_LOCK fixed it by serialising the pair. The names are now unique
# instead (`_h5lid.*` in the LID suite), which is strictly better for an
# ACCIDENTAL collision: it costs no parallelism, and it protects against the
# next test to pick the same name rather than only the pair someone noticed.
#
# The site_drainage lock above stays, because that case is different in kind —
# a real committed fixture that nine tests genuinely share. You cannot rename
# your way out of that one.
#
# A sweep at the time found a SECOND, unlocked instance nobody had hit:
# `_out.inp`, shared by test_object_deletion_ext and test_quality_roundtrip.
# Also renamed. The check below is what stops a third.

# 2D surface routing tests — geometry, gradients, flux, parsing.
# The 2D engine sources (MeshBuilder, VertexReconstruction, SurfaceFluxCalculator,
# SectionHandlers2D, Default2DOutputPlugin) are NOT compiled into the test here:
# the target links the engine library, which exports them. Compiling them into
# the exe AS WELL produced duplicate definitions across the exe and the engine
# dylib — an ODR violation whose UB surfaced as heap corruption in the CVODE
# solver path once 2D struct sizes changed. The test calls the engine's copies.
set(TEST_2D_SOURCES
    test_2d_surface_routing.cpp
)
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
set_tests_properties(test_engine_2d_surface PROPERTIES
    LABELS "unit"
    ENVIRONMENT "OPENSWMM_2D_BACKEND=cpu")

# Exercise the acceleration path end-to-end under BOTH cell closures. The
# Kokkos marcher kernels implement FLAT and VFR alike, so the factory must
# discover and load the omp plugin in both cases (the default-on policy) — the
# old closure-refusal guard retired with the CVODE-family solvers, which were
# VFR-only. Numeric parity is byte-exact by construction (disjoint writes,
# exact-min reductions); the bench harness verifies .out byte-identity
# serial-vs-omp on the Bellinge slice.
if(OPENSWMM_BUILD_2D AND OPENSWMM_BUILD_GPU_PLUGIN)
    file(COPY ${PROJECT_SOURCE_DIR}/examples/2d_complete_example.inp
         DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
    # VFR variant of the same fixture, generated at configure time.
    file(READ ${PROJECT_SOURCE_DIR}/examples/2d_complete_example.inp
         _omp_vfr_inp)
    string(REPLACE "[2D_OPTIONS]" "[2D_OPTIONS]\nCELL_CLOSURE           VFR"
           _omp_vfr_inp "${_omp_vfr_inp}")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/2d_complete_example_vfr.inp
         "${_omp_vfr_inp}")

    add_test(
        NAME    test_engine_2d_omp_default
        COMMAND $<TARGET_FILE:openswmm> 2d_complete_example.inp 2d_omp_default.rpt
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    add_test(
        NAME    test_engine_2d_omp_vfr
        COMMAND $<TARGET_FILE:openswmm> 2d_complete_example_vfr.inp 2d_omp_vfr.rpt
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    # Point plugin discovery straight at the freshly built omp plugin. Without
    # this the engine only searches its own module dir; on Windows multi-config
    # builds the CLI and the plugin land in separate per-config trees (e.g.
    # bin/Debug vs src/engine/2d/gpu/Debug), so the omp plugin is never found,
    # the run silently falls back to the serial CPU solver, and the
    # PASS_REGULAR_EXPRESSION below never matches. OPENSWMM_GPU_PLUGIN_PATH is
    # the highest-priority search dir, so this makes discovery deterministic on
    # every platform/generator. Guarded so non-omp GPU backends still configure.
    set(_omp_test_env "OPENSWMM_2D_BACKEND=omp")
    if(TARGET openswmm_gpu_omp)
        list(APPEND _omp_test_env
             "OPENSWMM_GPU_PLUGIN_PATH=$<TARGET_FILE_DIR:openswmm_gpu_omp>")
    endif()
    # test_engine_2d_omp_default carries no INTEGRATOR line, so it also gates
    # the marcher-is-the-default contract; the separate P5
    # test_engine_2d_omp_explicit became an exact duplicate of it once EXPLICIT
    # became the default and was dropped.
    set_tests_properties(test_engine_2d_omp_default PROPERTIES
        LABELS "unit"
        ENVIRONMENT "${_omp_test_env}"
        PASS_REGULAR_EXPRESSION "using GPU backend: omp")
    set_tests_properties(test_engine_2d_omp_vfr PROPERTIES
        LABELS "unit"
        ENVIRONMENT "${_omp_test_env}"
        PASS_REGULAR_EXPRESSION "using GPU backend: omp")
endif()

# InpWriter [2D_*] emission — end-to-end through the public C API (open a
# 2D .inp → swmm_model_write → reopen → compare via swmm_2d_* accessors,
# plus the GeoPackage write→reopen end-to-end). Needs the full 2D module.
if(OPENSWMM_BUILD_2D)
    add_gtest_unit(test_engine_inp_writer_2d test_inp_writer_2d.cpp)
    # Repeated-row [2D_TRIANGLE_NODE_MAP] (MESH-REMAP-01) — several nodes may
    # couple to ONE cell (weir/orifice endpoints). Guards the parser's append
    # semantics (was last-line-wins), the InpWriter row emission, and the new
    # swmm_2d_add/clear/rows/get_triangle_coupling_row C API the GUI's
    # Remap 1D↔2D pushes through.
    add_gtest_unit(test_engine_2d_triangle_coupling_rows
                   test_2d_triangle_coupling_rows.cpp)
    # 1D↔2D coupling unit-system equivalence — runs the same physical coupled
    # model in CMS and CFS and asserts matching 2D depths/continuity. Guards the
    # always-feet⇄metres coupling factors (the 1D side is always feet-internal).
    add_gtest_unit(test_engine_2d_coupling_units test_2d_coupling_units.cpp)
    # [2D_TRIANGLES] INIT_DEPTH unit-system regression — the column shares the
    # mesh's vertical datum (project display units), so initialize() must scale
    # it by mesh_to_si like vz. It was consumed raw as metres before the fix.
    add_gtest_unit(test_engine_2d_init_depth_units test_2d_init_depth_units.cpp)
    # Two-way 1D↔2D OUTFALL coupling regression — a coupled outfall's pipe
    # discharge must reach the 2D mesh (it read nodes.outflow≈0 before the fix)
    # and both the 2D and 1D continuity ledgers must close.
    add_gtest_unit(test_engine_2d_outfall_coupling test_2d_outfall_coupling.cpp)
    # Two-way 1D↔2D JUNCTION coupling regression — a coupled junction must
    # surcharge above its crown to spill onto the 2D mesh and recapture ponded
    # surface water through its inlet (both blocked when ponded_area was zeroed).
    add_gtest_unit(test_engine_2d_junction_coupling test_2d_junction_coupling.cpp)
    # Cross-domain volume conservation of 1D↔2D coupling under VARIABLE_STEP —
    # guards the per-window-volume carry that keeps the staggered exchange
    # mass-conservative when the routing dt varies between produce and consume.
    add_gtest_unit(test_engine_2d_coupling_conservation test_2d_coupling_conservation.cpp)
    # Decoupled 1D/2D timesteps (2026-07 plan): per-step accumulated exchange
    # across the marcher's multi-step sync batches, queue-spread delivery, and
    # the withdrawal budget. The macro-window cases retired with the implicit
    # solvers.
    add_gtest_unit(test_engine_2d_decoupled_stepping test_2d_decoupled_stepping.cpp)
    # Rainfall→mesh interpolation (natural-neighbour / IDW) + RAINFALL_MODE
    # option round-trip. Pure geometry over the engine-exported interpolator.
    add_gtest_unit(test_engine_2d_rainfall_interp test_2d_rainfall_interp.cpp)
    # VFR (volume–free-surface) closure + VFR_FACE edge gate: closure math
    # (round-trip / monotone / C¹ / dry anchor), lake-at-rest C-property at a
    # shoreline (zero fluxes under VFR, spurious under FLAT — the uphill-creep
    # driver), Eq. 14 wetting gate, and option round-trip. See
    # plans/2d/2D_VFR_SOLVER_CLOSURE_PLAN.md.
    add_gtest_unit(test_engine_2d_vfr_closure test_2d_vfr_closure.cpp)
    # Explicit local-inertial marcher analytic gates:
    # exact lake-at-rest (FLAT + VFR), dry-neighbour wall / C-property,
    # closed-basin conservation + positivity through a dam-break, Manning
    # steady-slope rain/outflow ledger + normal depth, Froude clamp.
    add_gtest_unit(test_engine_2d_inertial_marcher test_2d_inertial_marcher.cpp)
    # Tiered LTS gates: exact conservation at every tier count through a
    # multiscale dam-break, K-tier ≈ global-dt equivalence, and the
    # fine-patch-in-coarse-watershed stability case.
    add_gtest_unit(test_engine_2d_lts_equivalence test_2d_lts_equivalence.cpp)
    # Windowless co-advance coupling gates: cross-domain conservation through
    # the live in-marcher exchange + batch queue delivery, COUPLING_AREA AUTO
    # rederivation, conductance sign + FD parity.
    add_gtest_unit(test_engine_2d_windowless_coupling
                   test_2d_windowless_coupling.cpp)
    # Track I — per-cell 2D infiltration (plan §5.5, D-I1…D-I6). Unit gates
    # over Infil2D itself: the SI⇄ft/project-unit adapter (anchored on
    # hand-computed SI values, not on a re-derivation of the adapter), the
    # D-I3 override>tag>'*'>none resolution order, and D-I4 validation.
    add_gtest_unit(test_engine_2d_infil test_2d_infil.cpp)
    # Track I end-to-end: mass-balance ledger closure, the per-cell continuity
    # residual (the silent call site 4), lazy/inactive-cell equivalence,
    # section round-trip and the C API surface.
    add_gtest_unit(test_engine_2d_infil_integration test_2d_infil_integration.cpp)
endif()

# ---------------------------------------------------------------------------
# Explicit finite-volume 1D network solver (FLOW_ROUTING FV).
# See plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §6.
# ---------------------------------------------------------------------------
# Cross-section closure: A(h) monotone and C⁰ through the crown, dA/dh == T
# (the tapered slot mouth), A↔h round-trip incl. the taper band and deep slot,
# I₁ as the antiderivative of A with an analytic above-crown extension, and the
# FV_SLOT_CELERITY design value actually delivered.
add_gtest_unit(test_engine_fv_closure test_fv_solver_closure.cpp)
# Analytic scheme gates: exact lake-at-rest over a slope break AND while
# pressurized, Ritter (dry) and Stoker (wet) dam break vs. the closed-form
# solutions incl. shock SPEED, and mass conservation to machine precision over
# 10⁵ substeps.
add_gtest_unit(test_engine_fv_analytic test_fv_solver_analytic.cpp)
# Network gates: virtual-junction equivalence (single conduit vs. split), node
# coupling and continuity, mixed-flow filling bore vs. Rankine–Hugoniot,
# compaction results-transparency, and the scalar-transport contracts.
add_gtest_unit(test_engine_fv_network test_fv_solver_network.cpp)
# Local time stepping (§6.12): fall-through to global stepping bit-for-bit on an
# untiered mesh, tier-interface conservation exact at every tier count, a
# wetting front crossing tier boundaries without loss, and the substep-count
# relief a 100x graded reach actually delivers.
add_gtest_unit(test_engine_fv_lts test_fv_lts.cpp)
# Engine-level gates (§6.7): FLOW_ROUTING FV end-to-end on the reference
# drainage model — routing continuity vs DYNWAVE, routed-volume agreement,
# mesh-refinement CONVERGENCE toward the DW hydrograph (the consistency gate),
# conservation independent of resolution, and the FV_* .inp round-trip.
add_gtest_unit(test_engine_fv_integration test_fv_engine_integration.cpp)
target_compile_definitions(test_engine_fv_integration PRIVATE
    OPENSWMM_FV_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/regression/data"
    OPENSWMM_FV_UNIT_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    OPENSWMM_FV_TEST_OUT_DIR="${PROJECT_SOURCE_DIR}/tests/output")

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
# Mirror the engine's fast-lookup build variant into the test TU so the
# BitExact batch/golden checks adapt (ULP==0 when bit-exact, tolerance when
# the default fast lookup is on). The scalar lookup_exact checks stay ULP==0
# either way (XSection.cpp's scalar accessor always uses the exact core).
if(OPENSWMM_FAST_XSECT_LOOKUP)
    target_compile_definitions(test_engine_xsect_parity PRIVATE SWMM_XSECT_FAST_LOOKUP)
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

    # Part E — 2D mesh persistence round-trip (data-level: the fixture wires
    # ctx.twod_io at locally-owned 2D structs, so this runs in both 2D and
    # non-2D builds).
    add_executable(test_engine_geopackage_mesh2d test_geopackage_mesh2d.cpp)
    target_link_libraries(test_engine_geopackage_mesh2d
        PRIVATE
            GTest::gtest GTest::gtest_main GTest::gmock
            $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
            openswmm_geopackage
            openswmm_legacy_engine
    )
    target_include_directories(test_engine_geopackage_mesh2d
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_engine_geopackage_mesh2d PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
        INSTALL_RPATH "${LIB_ROOT}"
    )
    openswmm_stage_test_runtime_deps(test_engine_geopackage_mesh2d)
    add_test(
        NAME    test_engine_geopackage_mesh2d
        COMMAND $<TARGET_FILE:test_engine_geopackage_mesh2d>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
    )
    set_tests_properties(test_engine_geopackage_mesh2d PROPERTIES LABELS "unit")

    # Slice IO-6 — external-file format parsers/materialisers.
    add_executable(test_engine_geopackage_formats
        test_geopackage_formats.cpp)
    target_link_libraries(test_engine_geopackage_formats
        PRIVATE
            GTest::gtest GTest::gtest_main
            $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
            openswmm_geopackage
    )
    target_include_directories(test_engine_geopackage_formats
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_engine_geopackage_formats PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
        INSTALL_RPATH "${LIB_ROOT}"
    )
    openswmm_stage_test_runtime_deps(test_engine_geopackage_formats)
    add_test(
        NAME    test_engine_geopackage_formats
        COMMAND $<TARGET_FILE:test_engine_geopackage_formats>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
    )
    set_tests_properties(test_engine_geopackage_formats PROPERTIES LABELS "unit")

    # Slice IO-5 — structured external-file content schema + FK integrity.
    add_executable(test_engine_geopackage_schema_external_content
        test_geopackage_schema_external_content.cpp)
    target_link_libraries(test_engine_geopackage_schema_external_content
        PRIVATE
            GTest::gtest GTest::gtest_main
            $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
            openswmm_geopackage
    )
    target_include_directories(test_engine_geopackage_schema_external_content
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_engine_geopackage_schema_external_content
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
            INSTALL_RPATH "${LIB_ROOT}"
    )
    openswmm_stage_test_runtime_deps(
        test_engine_geopackage_schema_external_content)
    add_test(
        NAME    test_engine_geopackage_schema_external_content
        COMMAND $<TARGET_FILE:test_engine_geopackage_schema_external_content>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
    )
    set_tests_properties(test_engine_geopackage_schema_external_content
        PROPERTIES LABELS "unit")

    # Slice IO-7 — external-content fan-out from SimulationContext slots
    # into the Part D tables.
    add_executable(test_engine_geopackage_external_content_writer
        test_geopackage_external_content_writer.cpp)
    target_link_libraries(test_engine_geopackage_external_content_writer
        PRIVATE
            GTest::gtest GTest::gtest_main
            $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
            openswmm_geopackage
    )
    target_include_directories(test_engine_geopackage_external_content_writer
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_engine_geopackage_external_content_writer
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
            INSTALL_RPATH "${LIB_ROOT}"
    )
    openswmm_stage_test_runtime_deps(
        test_engine_geopackage_external_content_writer)
    add_test(
        NAME    test_engine_geopackage_external_content_writer
        COMMAND $<TARGET_FILE:test_engine_geopackage_external_content_writer>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
    )
    set_tests_properties(test_engine_geopackage_external_content_writer
        PROPERTIES LABELS "unit")

    # Slice IO-8 — Part D content → ctx slot hydration + scratch materialise.
    add_executable(test_engine_geopackage_external_content_reader
        test_geopackage_external_content_reader.cpp)
    target_link_libraries(test_engine_geopackage_external_content_reader
        PRIVATE
            GTest::gtest GTest::gtest_main
            $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
            openswmm_geopackage
    )
    target_include_directories(test_engine_geopackage_external_content_reader
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${PROJECT_SOURCE_DIR}/src/engine
    )
    set_target_properties(test_engine_geopackage_external_content_reader
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
            INSTALL_RPATH "${LIB_ROOT}"
    )
    openswmm_stage_test_runtime_deps(
        test_engine_geopackage_external_content_reader)
    add_test(
        NAME    test_engine_geopackage_external_content_reader
        COMMAND $<TARGET_FILE:test_engine_geopackage_external_content_reader>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
    )
    set_tests_properties(test_engine_geopackage_external_content_reader
        PROPERTIES LABELS "unit")
endif()


# ---- No two test sources may share a scratch-fixture name ----
# Every test here runs with data/ as its WORKING_DIRECTORY, so two files that
# write the same "_name.ext" are one test wearing two names: under -jN they
# race, and the failure surfaces as a CONTENT error (a parse failure, a wrong
# value) rather than as a file conflict. That disguise is why the first
# instance was written off as a flake and the second went unnoticed entirely.
#
# Fails at CONFIGURE time, before anything runs.
# CONFIGURE_DEPENDS so ADDING a test file re-runs this check. Without it
# the guard only sees the files that existed at the last configure — and
# a new file is exactly when a name gets reused.
file(GLOB _osw_test_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
set(_osw_seen_fixtures "")
set(_osw_seen_owners "")
set(_osw_fixture_clashes "")
foreach(_src IN LISTS _osw_test_sources)
    get_filename_component(_srcname "${_src}" NAME)
    file(READ "${_src}" _content)
    string(REGEX MATCHALL "\"_[A-Za-z0-9_]+\\.[A-Za-z0-9]+\"" _lits "${_content}")
    list(REMOVE_DUPLICATES _lits)
    foreach(_lit IN LISTS _lits)
        list(FIND _osw_seen_fixtures "${_lit}" _idx)
        if(_idx EQUAL -1)
            list(APPEND _osw_seen_fixtures "${_lit}")
            list(APPEND _osw_seen_owners "${_srcname}")
        else()
            list(GET _osw_seen_owners ${_idx} _first)
            list(APPEND _osw_fixture_clashes
                 "  ${_lit} written by both ${_first} and ${_srcname}")
        endif()
    endforeach()
endforeach()
if(_osw_fixture_clashes)
    string(REPLACE ";" "\n" _osw_clash_text "${_osw_fixture_clashes}")
    message(FATAL_ERROR
        "Two or more unit tests share a scratch-fixture name, and they all run "
        "with data/ as their working directory:\n${_osw_clash_text}\n"
        "Rename one of them. A RESOURCE_LOCK also works but costs parallelism "
        "and only protects the pair you noticed. See the note above this check.")
endif()
