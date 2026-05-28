#
# CMakeLists.txt - CMake configuration file for tests/solver
#
# Created: 2026
#

add_executable(test_gage_scalefactor
    test_gage_scalefactor.cpp
    )
target_include_directories(test_gage_scalefactor
    PUBLIC ../../src/solver/include
    )
target_link_libraries(test_gage_scalefactor
    ${Boost_LIBRARIES}
    swmm5
    )

set_target_properties(test_gage_scalefactor
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
