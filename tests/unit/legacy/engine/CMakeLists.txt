#
# tests/legacy/engine/CMakeLists.txt
#
# Google Test unit tests for the openswmm_legacy_engine (legacy SWMM solver API).
# Converted from Boost.Test to Google Test 2026-03-25.
# Moved from tests/solver/ to tests/legacy/engine/ 2026-03-25.
#

cmake_minimum_required(VERSION 3.21)

find_package(GTest CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(LIB_ROOT "@loader_path;@rpath;@executable_path;@loader_path/../lib;@rpath/../lib;@executable_path/../lib;@executable_path/$CMAKE_BUILD_TYPE")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib;$ORIGIN/$CMAKE_BUILD_TYPE")
endif()

# Helper function to create a solver test executable
function(add_solver_test TEST_NAME SOURCE_FILE)
    add_executable(${TEST_NAME} ${SOURCE_FILE})

    target_include_directories(${TEST_NAME}
        PRIVATE
            ${PROJECT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/include/openswmm/legacy/engine
            ${PROJECT_SOURCE_DIR}/include/openswmm/legacy/output
    )

    target_link_libraries(${TEST_NAME}
        PRIVATE
            GTest::gtest
            GTest::gtest_main
            openswmm_legacy_engine
            openswmm_legacy_output
    )

    if(MSVC)
        target_compile_options(${TEST_NAME} PRIVATE /EHsc)
    endif()

    set_target_properties(${TEST_NAME}
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
            INSTALL_RPATH "${LIB_ROOT}"
    )
endfunction()

add_solver_test(test_solver_api          test_solver_api.cpp)
add_solver_test(test_solver_errors       test_solver_errors.cpp)
add_solver_test(test_solver_hotstart     test_solver_hotstart.cpp)
add_solver_test(test_solver_shapes       test_solver_shapes.cpp)
add_solver_test(test_solver_expanded_api test_solver_expanded_api.cpp)
