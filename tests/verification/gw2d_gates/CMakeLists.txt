# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
#
# G1 — the two-zone groundwater plan's numerical gates.
#
# A plain executable, not a gtest: these are the conservation identities of a
# kernel, they must be runnable before anything else in the tree builds, and
# their output is a table a reviewer can read. `ctest -R gw2d_gates -V` prints
# it. See gw2d_gates.cpp's header for the gate order and what each failure
# means.

add_executable(gw2d_gates gw2d_gates.cpp)

target_link_libraries(gw2d_gates
    PRIVATE
        $<IF:$<BOOL:${MSVC}>,openswmm_engine_internal,openswmm_engine>
)

target_include_directories(gw2d_gates
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src/engine
)

target_compile_definitions(gw2d_gates PRIVATE OPENSWMM_HAS_2D)

set_target_properties(gw2d_gates PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

if(MSVC)
    target_compile_options(gw2d_gates PRIVATE /EHsc)
endif()

add_test(NAME gw2d_gates COMMAND $<TARGET_FILE:gw2d_gates>)
