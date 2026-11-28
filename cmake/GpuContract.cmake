# Internal C++ plugin boundary: require the same headers, host toolchain and
# build configuration before constructing an object across the shared library.
function(openswmm_gpu_contract target)
    get_filename_component(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    file(GLOB_RECURSE _headers CONFIGURE_DEPENDS
        "${_root}/src/engine/*.hpp" "${_root}/src/engine/*.h"
        "${_root}/include/openswmm/engine/*.h")
    # Generated visibility headers vary while configuring and are not data layouts.
    list(FILTER _headers EXCLUDE REGEX "_export\\.h$")
    list(SORT _headers)
    set(_contract "abi5;${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_SIZEOF_VOID_P};${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_MSVC_RUNTIME_LIBRARY};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_DEBUG};${CMAKE_CXX_FLAGS_RELEASE};${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    foreach(_header IN LISTS _headers)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_header}")
        file(SHA256 "${_header}" _hash)
        string(APPEND _contract ";${_hash}")
    endforeach()
    string(SHA256 _digest "${_contract}")
    target_compile_definitions(${target} PRIVATE "OPENSWMM_GPU_BUILD_CONTRACT=\"${_digest}-$<CONFIG>\"")
endfunction()
