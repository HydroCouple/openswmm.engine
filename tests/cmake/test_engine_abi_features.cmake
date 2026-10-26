# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
# @file test_engine_abi_features.cmake
# @brief Verify optional exported APIs participate in the engine-owned ABI identity.
# @details Configure the actual helper without building the engine or finding optional libraries.
# Every input, generated manifest and compiler log stays in EVIDENCE for review.
cmake_minimum_required(VERSION 3.21)
if(NOT ENGINE_ROOT OR NOT EVIDENCE)
    message(FATAL_ERROR "ENGINE_ROOT and EVIDENCE are required")
endif()
file(MAKE_DIRECTORY "${EVIDENCE}")
function(configure_identity name surface geopackage hdf5 output)
    set(_directory "${EVIDENCE}/${name}")
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -S "${ENGINE_ROOT}/tests/cmake/engine_abi_fixture" -B "${_directory}"
        "-DENGINE_ROOT=${ENGINE_ROOT}"
        "-DOPENSWMM_BUILD_2D=${surface}" "-DOPENSWMM_WITH_GEOPACKAGE=${geopackage}"
        "-DOPENSWMM_WITH_HDF5=${hdf5}" "-DOPENSWMM_2D_HDF5_OUTPUT=${output}"
        RESULT_VARIABLE _status OUTPUT_FILE "${EVIDENCE}/${name}.log"
        ERROR_FILE "${EVIDENCE}/${name}.stderr.log")
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR "ABI fixture ${name} failed; inspect ${EVIDENCE}/${name}.log")
    endif()
    file(READ "${_directory}/engine-abi.json" _manifest)
    string(JSON _tag GET "${_manifest}" tag)
    string(JSON _api GET "${_manifest}" apiHeadersSha256)
    set(${name}_TAG "${_tag}" PARENT_SCOPE)
    set(${name}_API "${_api}" PARENT_SCOPE)
endfunction()
configure_identity(base OFF OFF OFF OFF)
configure_identity(surface ON OFF OFF OFF)
configure_identity(geopackage OFF ON OFF OFF)
configure_identity(hdf5 OFF OFF ON OFF)
configure_identity(surface_hdf5 ON OFF OFF ON)
configure_identity(equivalent TRUE FALSE 0 NO)
configure_identity(inactive_writer OFF OFF OFF ON)
foreach(_variant surface geopackage hdf5 surface_hdf5)
    if("${${_variant}_TAG}" STREQUAL "${base_TAG}")
        message(FATAL_ERROR "${_variant} must change the runtime ABI identity")
    endif()
    if(NOT "${${_variant}_API}" STREQUAL "${base_API}")
        message(FATAL_ERROR "${_variant} unexpectedly changed the source header fingerprint")
    endif()
endforeach()
if("${surface_hdf5_TAG}" STREQUAL "${surface_TAG}")
    message(FATAL_ERROR "The enabled 2D HDF5 writer must affect the ABI identity")
endif()
if(NOT "${equivalent_TAG}" STREQUAL "${surface_TAG}" OR
   NOT "${inactive_writer_TAG}" STREQUAL "${base_TAG}")
    message(FATAL_ERROR "Equivalent effective feature configurations must have identical identities")
endif()
file(WRITE "${EVIDENCE}/verification.json"
    "{\"ok\":true,\"distinctOptionalModules\":true,\"canonicalBooleans\":true,\"inactiveWriterIgnored\":true,\"unchangedHeaderFingerprint\":true}\n")
message(STATUS "Engine ABI feature identity checks passed; evidence: ${EVIDENCE}")
