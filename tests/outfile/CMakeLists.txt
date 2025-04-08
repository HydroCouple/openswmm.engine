#
# CMakeLists.txt - CMake configuration file for tests/outfile
#
# Created: July 11, 2019
#
# Author: Michael E. Tryby
#         US EPA ORD/CESER
#

if(APPLE)
    set(LIB_ROOT "@loader_path;@rpath;@executable_path;@loader_path/../lib;@rpath/../lib;@executable_path/../lib;@executable_path/$CMAKE_BUILD_TYPE")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN;$ORIGIN/../lib;$ORIGIN/$CMAKE_BUILD_TYPE")
endif()

add_executable(
    test_output
        test_output.cpp
    )
target_include_directories(
    test_output
    PRIVATE 
        ${PROJECT_SOURCE_DIR}/src/outfile
    )
target_link_libraries(
    test_output
    PRIVATE
        Boost::unit_test_framework
        swmm-output
    )

set_target_properties(
    test_output
    PROPERTIES 
        RUNTIME_OUTPUT_DIRECTORY ${TEST_BIN_DIRECTORY}
        INSTALL_RPATH "${LIB_ROOT}"
)
