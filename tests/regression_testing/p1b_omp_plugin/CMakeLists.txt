# Standalone driver for Phase 1 Gate P1-B: build the REAL openswmm_gpu_omp
# plugin from src/engine/2d/gpu/CMakeLists.txt (omp backend) against an
# installed Kokkos + SUNDIALS[kokkos], isolated from the engine build's custom
# OpenMP finder / IPO / SUNDIALS-config coupling. This exercises the actual
# plugin CMake + sources under review, nothing bespoke.
#
#   cmake -S tests/regression_testing/p1b_omp_plugin -B build/p1b-omp \
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<vcpkg-installed-prefix> \
#         <OpenMP hints>
#   cmake --build build/p1b-omp --target openswmm_gpu_omp

cmake_minimum_required(VERSION 3.20)
project(p1b_omp_plugin CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENSWMM_BUILD_GPU_PLUGIN ON CACHE BOOL "" FORCE)
set(OPENSWMM_GPU_BACKEND omp CACHE STRING "" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/engine/2d/gpu"
    "${CMAKE_CURRENT_BINARY_DIR}/gpu_build")
