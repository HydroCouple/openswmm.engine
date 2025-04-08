#
# CMakeLists.txt - CMake configuration file for tests/solver
#
# Created: Feb 02, 2024
# Updated: Feb 02, 2024
#
#

if(APPLE)
    set(LIB_ROOT "@loader_path;@rpath;@executable_path;@loader_path/../lib;@rpath/../lib;@executable_path/../lib;@executable_path/$CMAKE_BUILD_TYPE")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib;$ORIGIN/$CMAKE_BUILD_TYPE")
endif()

# Test solver api
add_executable(
    test_solver_api
        test_solver_api.cpp
)
     
target_include_directories(
    test_solver_api
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/tests 
        ${PROJECT_SOURCE_DIR}/src/solver/include
        ${PROJECT_SOURCE_DIR}/src/outfile/include
    )

target_link_libraries(
    test_solver_api
    PRIVATE
        swmm-output 
        swmm5
        Boost::unit_test_framework
 )

set_target_properties(
    test_solver_api
    PROPERTIES 
        RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
        INSTALL_RPATH "${LIB_ROOT}"
)



# Test solver errors
add_executable(
    test_solver_errors
        test_solver_errors.cpp 
)
     
target_include_directories(
    test_solver_errors
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/tests 
        ${PROJECT_SOURCE_DIR}/src/solver/include
        ${PROJECT_SOURCE_DIR}/src/outfile/include
    )

target_link_libraries(
    test_solver_errors
    PRIVATE
        swmm-output 
        swmm5
        Boost::unit_test_framework
 )

set_target_properties(
    test_solver_errors
    PROPERTIES 
        RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
        INSTALL_RPATH "${LIB_ROOT}"
)

# Test solver hotstart
add_executable(
    test_solver_hotstart
        test_solver_hotstart.cpp
)
     
target_include_directories(
    test_solver_hotstart
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/tests 
        ${PROJECT_SOURCE_DIR}/src/solver/include
        ${PROJECT_SOURCE_DIR}/src/outfile/include
    )

target_link_libraries(
    test_solver_hotstart
    PRIVATE
        swmm-output 
        swmm5
        Boost::unit_test_framework
 )

set_target_properties(
    test_solver_hotstart
    PROPERTIES 
        RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
        INSTALL_RPATH "${LIB_ROOT}"
)


# Test solver shapes
add_executable(
    test_solver_shapes
        test_solver_shapes.cpp
)
     
target_include_directories(
    test_solver_shapes
    PUBLIC 
        ${PROJECT_SOURCE_DIR}/tests 
        ${PROJECT_SOURCE_DIR}/src/solver/include
        ${PROJECT_SOURCE_DIR}/src/outfile/include
    )

target_link_libraries(
    test_solver_shapes
    PRIVATE
        swmm-output 
        swmm5
        Boost::unit_test_framework
 )

set_target_properties( 
    test_solver_shapes
    PROPERTIES 
        RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
        INSTALL_RPATH "${LIB_ROOT}"
)