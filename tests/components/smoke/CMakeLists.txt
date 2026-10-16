# SPDX-License-Identifier: Apache-2.0
#
# D2 (program plan §C.1): the component-discovery test doubles.
#
# Six REAL shared libraries from one source (smoke_component.cpp), each with
# its own component id so a single discovery pass sees every variant without
# the duplicate-id rule masking one behind another. Where each lands is the
# point: the three stamped ones sit in the three places §B.2.3 says the engine
# must look.
#
#   hc_smoke_next        stamped      next to the host (engine) library /components
#   hc_smoke_installed   stamped      the installed lib/hydrocouple/components layout
#   hc_smoke_env         stamped      a directory named by HYDROCOUPLE_COMPONENT_PATH
#   hc_smoke_explicit    stamped      a directory given to swmm_component_search_path_add
#   hc_smoke_misstamped  MIS-stamped  the explicit directory — must be REFUSED
#   hc_smoke_legacy      unstamped    the explicit directory — must load as "unstamped"

set(SMOKE_ENV_DIR      ${CMAKE_BINARY_DIR}/test_components/env      PARENT_SCOPE)
set(SMOKE_EXPLICIT_DIR ${CMAKE_BINARY_DIR}/test_components/explicit PARENT_SCOPE)
set(_env_dir      ${CMAKE_BINARY_DIR}/test_components/env)
set(_explicit_dir ${CMAKE_BINARY_DIR}/test_components/explicit)

# "Next to the host library" is wherever the engine library is actually
# LOADED from: its own build directory on Unix, but on Windows the test
# staging copies the engine DLL next to each test executable
# (openswmm_stage_test_runtime_deps), so the loaded copy lives there.
if(WIN32)
    set(_next_dir ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/components)
    set(_installed_dir ${CMAKE_BINARY_DIR}/bin/lib/hydrocouple/components)
else()
    set(_next_dir $<TARGET_FILE_DIR:openswmm_engine>/components)
    set(_installed_dir $<TARGET_FILE_DIR:openswmm_engine>/hydrocouple/components)
endif()

function(_hc_smoke NAME ID MODE OUTDIR)
    add_library(${NAME} SHARED ${CMAKE_CURRENT_SOURCE_DIR}/smoke_component.cpp)
    target_link_libraries(${NAME} PRIVATE HydroCouple::HydroCouple)
    target_compile_definitions(${NAME} PRIVATE
        SMOKE_COMPONENT_ID="${ID}" SMOKE_MODE=${MODE})
    set_target_properties(${NAME} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        LIBRARY_OUTPUT_DIRECTORY ${OUTDIR}
        RUNTIME_OUTPUT_DIRECTORY ${OUTDIR})
endfunction()

_hc_smoke(hc_smoke_next       org.hydrocouple.openswmm.test.smoke.next       0 ${_next_dir})
_hc_smoke(hc_smoke_installed  org.hydrocouple.openswmm.test.smoke.installed  0 ${_installed_dir})
_hc_smoke(hc_smoke_env        org.hydrocouple.openswmm.test.smoke.env        0 ${_env_dir})
_hc_smoke(hc_smoke_explicit   org.hydrocouple.openswmm.test.smoke.explicit   0 ${_explicit_dir})
_hc_smoke(hc_smoke_misstamped org.hydrocouple.openswmm.test.smoke.misstamped 1 ${_explicit_dir})
_hc_smoke(hc_smoke_legacy     org.hydrocouple.openswmm.test.smoke.legacy     2 ${_explicit_dir})

add_custom_target(hc_smoke_components DEPENDS
    hc_smoke_next hc_smoke_installed hc_smoke_env hc_smoke_explicit hc_smoke_misstamped hc_smoke_legacy)
