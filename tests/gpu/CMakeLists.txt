# ============================================================================
# Phase 1 Kokkos RHS parity test — standalone (Kokkos only, no SUNDIALS)
# ============================================================================
#
# Deliberately self-contained so it can be configured/built on its own against
# just a Kokkos install, independent of the main engine build and of the
# (harder) SUNDIALS-with-Kokkos dependency:
#
#   cmake -S tests/gpu -B build/gpu-parity -DKokkos_ROOT=<kokkos-prefix>
#   cmake --build build/gpu-parity
#   ./build/gpu-parity/test_kokkos_rhs_parity     # exit 0 = parity pass
#
# NOTE: a Release build is required. A vcpkg Debug Kokkos can fail to resolve
# the imported library location (and the debug lib is missing a
# SharedAllocationRecord symbol the release headers reference), so this file
# defaults CMAKE_BUILD_TYPE to Release when the caller leaves it unset.
#
# It compiles the serial reference (SurfaceFluxCalculator.cpp — header-only
# deps, no SUNDIALS) and the Kokkos kernels into one executable and compares
# ydot AND the diagnostic fields (vert_head, gradients) element-wise.

cmake_minimum_required(VERSION 3.20)
project(openswmm_gpu_parity CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Kokkos REQUIRED)

add_executable(test_kokkos_rhs_parity
    test_kokkos_rhs_parity.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/engine/2d/solver/SurfaceFluxCalculator.cpp)

target_link_libraries(test_kokkos_rhs_parity PRIVATE Kokkos::kokkos)

enable_testing()
add_test(NAME kokkos_rhs_parity COMMAND test_kokkos_rhs_parity)
