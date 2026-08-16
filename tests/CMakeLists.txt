#
# CMakeLists.txt - CMake configuration file for swmm-solver/tests
#
# Created: Mar 4, 2020
# Updated: Aug 16, 2026 — Boost.Test -> GoogleTest (vcpkg `tests` feature)
#
# Author: Michael E. Tryby
#         US EPA ORD/CESER
#


# NOTE (2026-08-16): replaces `include(../extern/boost.cmake)`. GoogleTest comes
# from the vcpkg manifest's `tests` feature; extern/boost.cmake is deleted.
find_package(GTest CONFIG REQUIRED)

include(GoogleTest)


add_subdirectory(outfile)


# NOTE (2026-08-16): the test command was previously spelled
#     ${CMAKE_BINARY_DIR}/bin/$<CONFIGURATION>/test_output
# which only resolves correctly under multi-config generators. Under a
# single-config generator (Ninja, used by the Linux/Darwin presets)
# $<CONFIGURATION> still expands to the build type while the binary is written
# to bin/ with no config subdirectory, so ctest looked in a path that does not
# exist. $<TARGET_FILE:...> is generator-agnostic.
add_test(NAME test_output
    COMMAND "$<TARGET_FILE:test_output>"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/outfile/data
    )

# `unit` label mirrors openswmm.engine so `ctest -L unit` selects the same
# class of tests in both engine trees.
set_tests_properties(test_output PROPERTIES LABELS "unit")
