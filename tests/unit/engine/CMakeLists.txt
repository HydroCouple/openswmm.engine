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

# Helper function to create a unit test executable
function(add_gtest_unit TEST_NAME SOURCE_FILE)
    add_executable(${TEST_NAME} ${SOURCE_FILE})

    target_link_libraries(${TEST_NAME}
        PRIVATE
            GTest::gtest
            GTest::gtest_main
            GTest::gmock
            openswmm_engine
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
    )

    add_test(
        NAME    ${TEST_NAME}
        COMMAND $<TARGET_FILE:${TEST_NAME}>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
    )

    # Copy to bin directory for convenience
    add_custom_command(TARGET ${TEST_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:${TEST_NAME}>
            ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:${TEST_NAME}>
    )
endfunction()

# ---- Unit test targets ----
# NOTE: Files are created as stubs initially. Implement when the corresponding
#       engine component is written (see MASTER_IMPLEMENTATION_PLAN.md phases).

add_gtest_unit(test_engine_tokenizer         test_tokenizer.cpp)
add_gtest_unit(test_engine_timeseries_cursor test_timeseries_cursor.cpp)
add_gtest_unit(test_engine_options_parser    test_options_parser.cpp)
add_gtest_unit(test_engine_csv_timeseries    test_csv_timeseries.cpp)
add_gtest_unit(test_engine_plugin_lifecycle  test_plugin_lifecycle.cpp)
add_gtest_unit(test_engine_hotstart          test_hotstart.cpp)
add_gtest_unit(test_engine_io_thread         test_io_thread.cpp)
add_gtest_unit(test_engine_timestep_ctrl     test_timestep_controller.cpp)
add_gtest_unit(test_engine_simd_math         test_simd_math.cpp)
add_gtest_unit(test_engine_user_flags        test_user_flags.cpp)
add_gtest_unit(test_engine_section_registry  test_section_registry.cpp)
add_gtest_unit(test_engine_crs               test_crs.cpp)
add_gtest_unit(test_engine_xsection          test_xsection.cpp)
add_gtest_unit(test_engine_infiltration      test_infiltration.cpp)
add_gtest_unit(test_engine_runoff           test_runoff.cpp)
add_gtest_unit(test_engine_snow             test_snow.cpp)
add_gtest_unit(test_engine_climate          test_climate.cpp)
add_gtest_unit(test_engine_groundwater      test_groundwater.cpp)
add_gtest_unit(test_engine_routing          test_routing.cpp)
add_gtest_unit(test_engine_site_drainage    test_site_drainage_model.cpp)
