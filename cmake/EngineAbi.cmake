# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Caleb Buahin
# Generate the engine-owned ABI identity from the exact public headers, build ABI and features.
# Header edits trigger reconfiguration; private implementation edits do not change this identity.
function(openswmm_configure_engine_abi)
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    file(GLOB _headers CONFIGURE_DEPENDS "${_root}/include/openswmm/engine/*.h")
    list(FILTER _headers EXCLUDE REGEX "openswmm_(engine_impl|engine_internal|engine_export|abi_tag)[.]h$")
    list(SORT _headers)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_headers})
    set(_api "")
    foreach(_header IN LISTS _headers)
        get_filename_component(_name "${_header}" NAME)
        file(SHA256 "${_header}" _hash)
        string(APPEND _api "${_name}\n${_hash}\n")
    endforeach()
    string(SHA256 OPENSWMM_ABI_API_SHA256 "${_api}")
    # Public headers are an inventory of every supported module. Their text alone cannot
    # distinguish a binary that omits optional exported APIs (notably 2D and GeoPackage).
    # Canonical booleans also make equivalent CMake spellings (ON/TRUE/1) identical.
    foreach(_feature OPENSWMM_BUILD_2D OPENSWMM_WITH_GEOPACKAGE OPENSWMM_WITH_HDF5)
        if(${_feature})
            set(_${_feature} true)
        else()
            set(_${_feature} false)
        endif()
    endforeach()
    if(OPENSWMM_BUILD_2D AND OPENSWMM_2D_HDF5_OUTPUT)
        set(_surface_hdf5 true)
    else()
        set(_surface_hdf5 false)
    endif()
    set(_features "{\"surface2d\":${_OPENSWMM_BUILD_2D},\"geopackage\":${_OPENSWMM_WITH_GEOPACKAGE},\"hdf5Model\":${_OPENSWMM_WITH_HDF5},\"surfaceHdf5\":${_surface_hdf5}}")
    string(REPLACE ";" "+" _arch "${CMAKE_OSX_ARCHITECTURES}")
    set(_build "${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SIZEOF_VOID_P};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${_arch};${_features}")
    string(SHA256 _build_hash "${_build}")
    set(OPENSWMM_ENGINE_ABI_TAG "openswmm-engine-abi/1:${OPENSWMM_ABI_API_SHA256}:${_build_hash}")
    configure_file("${_root}/cmake/openswmm_abi_tag.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/openswmm_abi_tag.h" @ONLY)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/engine-abi.json"
        "{\"schema\":1,\"tag\":\"${OPENSWMM_ENGINE_ABI_TAG}\",\"apiHeadersSha256\":\"${OPENSWMM_ABI_API_SHA256}\",\"buildSha256\":\"${_build_hash}\",\"features\":${_features}}\n")
    set(OPENSWMM_ENGINE_ABI_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/engine-abi.json"
        CACHE INTERNAL "Engine-owned ABI manifest for native/Python consumers" FORCE)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/openswmm_abi_tag.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/openswmm/engine")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/engine-abi.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/openswmm")
endfunction()
