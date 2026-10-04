# Standalone Kokkos explicit-marcher and SWE-kernel alignment tests.
# Requires Kokkos (OpenMP/Serial or a device execution space), no engine DLL.
#
# cmake -S tests/gpu -B build/kokkos-cpu-alignment -DKokkos_ROOT=<prefix>
# cmake --build build/kokkos-cpu-alignment
# ctest --test-dir build/kokkos-cpu-alignment --output-on-failure
#
# Release is the default to match the installed Kokkos library configuration.
# The historical RHS parity test is built only if its retired header exists.

cmake_minimum_required(VERSION 3.20)
project(openswmm_gpu_parity CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Kokkos REQUIRED)

enable_testing()
# Historical RHS test targets a retired integrator/header. Do not make the
# current explicit marcher test depend on that removed implementation.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine/2d/gpu/KokkosSurfaceKernels.hpp")
    add_executable(test_kokkos_rhs_parity test_kokkos_rhs_parity.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine/2d/solver/SurfaceFluxCalculator.cpp)
    target_link_libraries(test_kokkos_rhs_parity PRIVATE Kokkos::kokkos)
    add_test(NAME kokkos_rhs_parity COMMAND test_kokkos_rhs_parity)
endif()

# Corrected marcher and experimental SWE building blocks. This target is
# standalone and can run with host Kokkos before a device build is available.
set(_engine ${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine)
add_executable(test_kokkos_explicit_alignment
    test_kokkos_explicit_alignment.cpp
    ${_engine}/2d/gpu/ExplicitKokkosSurfaceSolver.cpp
    ${_engine}/2d/solver/ExplicitInertialSolver.cpp
    ${_engine}/2d/solver/InertialEdges.cpp
    ${_engine}/2d/solver/SurfaceFluxCalculator.cpp
    ${_engine}/2d/mesh/MeshBuilder.cpp
    ${_engine}/2d/coupling/NodeCoupling.cpp
    ${_engine}/2d/subsurface/SubsurfaceSolver.cpp
    ${_engine}/2d/subsurface/SigmaColumn.cpp
    ${_engine}/2d/subsurface/SoilCharacteristic.cpp
    ${_engine}/2d/subsurface/SubsurfaceData.cpp)
target_include_directories(test_kokkos_explicit_alignment PRIVATE
    ${_engine} ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include/openswmm/engine)
target_link_libraries(test_kokkos_explicit_alignment PRIVATE Kokkos::kokkos)
if(APPLE)
    target_compile_definitions(test_kokkos_explicit_alignment PRIVATE SWMM_OS_MACOS)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_kokkos_explicit_alignment PRIVATE -fno-fast-math -ffp-contract=off)
endif()
add_test(NAME kokkos_explicit_alignment COMMAND test_kokkos_explicit_alignment)
set_tests_properties(kokkos_explicit_alignment PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=4;OMP_PROC_BIND=false")

add_test(NAME kokkos_explicit_alignment_one_thread COMMAND test_kokkos_explicit_alignment)
set_tests_properties(kokkos_explicit_alignment_one_thread PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=1;OMP_PROC_BIND=false")
