# Configure the Metal translation units with acpp without changing the engine compiler.
include(ExternalProject)
include("${CMAKE_CURRENT_LIST_DIR}/FetchKokkosMetal.cmake")
find_program(OPENSWMM_ACPP_EXECUTABLE NAMES acpp REQUIRED)
find_program(_openswmm_metal_ninja NAMES ninja REQUIRED)
set(OPENSWMM_METAL_SDK "${CMAKE_OSX_SYSROOT}" CACHE STRING "macOS SDK supported by AdaptiveCpp")
set(_metal_build "${CMAKE_CURRENT_BINARY_DIR}/metal-build")
set(_openswmm_metal_executable "${_metal_build}/openswmm_metal_faces")
ExternalProject_Add(openswmm_metal_worker_build
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/metal"
    BINARY_DIR "${_metal_build}"
    CMAKE_GENERATOR Ninja
    CMAKE_ARGS
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=${_openswmm_metal_ninja}"
        "-DCMAKE_CXX_COMPILER:FILEPATH=${OPENSWMM_ACPP_EXECUTABLE}"
        "-DCMAKE_CXX_FLAGS:STRING=--acpp-targets=generic"
        "-DCMAKE_OSX_SYSROOT:STRING=${OPENSWMM_METAL_SDK}"
        "-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${CMAKE_OSX_DEPLOYMENT_TARGET}"
        -DCMAKE_OSX_ARCHITECTURES:STRING=arm64 -DCMAKE_BUILD_TYPE:STRING=Release
        "-DOPENSWMM_KOKKOS_SOURCE_DIR:PATH=${OPENSWMM_KOKKOS_SOURCE_DIR}"
        "-DOPENSWMM_KOKKOS_GIT_REPOSITORY:STRING=${OPENSWMM_KOKKOS_GIT_REPOSITORY}"
        "-DOPENSWMM_KOKKOS_GIT_TAG:STRING=${OPENSWMM_KOKKOS_GIT_TAG}"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target openswmm_metal_faces --parallel 4
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS "${_openswmm_metal_executable}"
    INSTALL_COMMAND "")
# The engine build stages the plugin in both directories. Keep its worker beside each copy.
add_custom_target(openswmm_metal_worker
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:openswmm_gpu_omp>" "$<TARGET_FILE_DIR:openswmm_engine>"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_openswmm_metal_executable}" "$<TARGET_FILE_DIR:openswmm_gpu_omp>/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_openswmm_metal_executable}" "$<TARGET_FILE_DIR:openswmm_engine>/"
    DEPENDS openswmm_metal_worker_build VERBATIM)
add_dependencies(openswmm_gpu_omp openswmm_metal_worker)
target_compile_definitions(openswmm_gpu_omp PRIVATE OPENSWMM_ENABLE_METAL_FACES)
