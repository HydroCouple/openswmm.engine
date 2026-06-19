# ============================================================================
# openswmm-gpu-omp — companion wheel CMake entry point (scikit-build-core)
#
# Builds ONLY the Kokkos-OpenMP 2D surface-solver plugin
# (libopenswmm_gpu_omp) from the engine tree and installs it INTO this wheel's
# own import package (openswmm_gpu_omp/). The plugin is a standalone shared
# library — it is NOT linked against the engine; the base `openswmm` package's
# C++ core dlopen()s it at runtime once the discovery shim
# (openswmm/__init__.py) has added this directory to OPENSWMM_GPU_PLUGIN_PATH.
#
# This reuses the engine's real plugin CMake (src/engine/2d/gpu/CMakeLists.txt,
# omp backend) — the same standalone build pattern validated by
# tests/regression_testing/p1b_omp_plugin — so there is no bespoke build logic
# to drift from the engine's.
# ============================================================================
cmake_minimum_required(VERSION 3.24)

# Engine repo root (this package lives at packages/gpu-omp/): vcpkg.json,
# vcpkg-configuration.json, the overlay ports, and the plugin sources.
get_filename_component(OPENSWMM_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.."
                      ABSOLUTE)

# ----------------------------------------------------------------------------
# vcpkg: reuse the engine manifest + overlay ports (single source of truth for
# Kokkos / SUNDIALS[kokkos] versions and the pinned registry baseline), but pull
# ONLY the `gpu` feature — no default features (no full 2D/HDF5; the plugin needs
# neither). Must be set before project() so the toolchain reads them.
# ----------------------------------------------------------------------------
if(NOT DEFINED CACHE{VCPKG_MANIFEST_DIR})
    set(VCPKG_MANIFEST_DIR "${OPENSWMM_ENGINE_ROOT}"
        CACHE PATH "Directory containing the engine vcpkg.json" FORCE)
endif()
if(NOT DEFINED CACHE{VCPKG_MANIFEST_NO_DEFAULT_FEATURES})
    set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON CACHE BOOL
        "Companion pulls only the gpu feature, not the engine defaults" FORCE)
endif()
if(NOT DEFINED CACHE{VCPKG_MANIFEST_FEATURES})
    # gpu   → Kokkos + SUNDIALS[kokkos] for the OpenMP plugin.
    # hypre → BoomerAMG so the plugin's PRECONDITIONER=AMG path (the Kokkos
    #         KokkosAmgPreconditioner) is compiled; OPENSWMM_WITH_HYPRE defaults
    #         ON, so this must be listed or the plugin build fails at
    #         find_package(HYPRE REQUIRED).
    set(VCPKG_MANIFEST_FEATURES "gpu;hypre" CACHE STRING
        "Kokkos + SUNDIALS[kokkos] + hypre BoomerAMG for the OpenMP plugin" FORCE)
endif()

# Auto-load the vcpkg toolchain from VCPKG_ROOT (mirrors python/CMakeLists.txt).
# A non-vcpkg build can instead point CMAKE_PREFIX_PATH at an installed
# Kokkos + SUNDIALS[kokkos] prefix.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE PATH "vcpkg toolchain file (auto-detected from VCPKG_ROOT)")
endif()

project(openswmm-gpu-omp VERSION 6.0.0 LANGUAGES CXX)

# Use the engine's custom OpenMP finder so omp links on Apple clang (probes the
# Homebrew libomp prefixes the macOS wheel build installs into). Kokkos' config
# does find_dependency(OpenMP); placing the engine cmake/ dir on the module path
# routes that to the same finder.
list(APPEND CMAKE_MODULE_PATH "${OPENSWMM_ENGINE_ROOT}/cmake")
if(NOT TARGET OpenMP::OpenMP_CXX)
    include("${OPENSWMM_ENGINE_ROOT}/cmake/FindOpenMP.cmake")
endif()

# Build the omp plugin from the engine's real plugin CMake. Standalone: there is
# no openswmm_engine target here, so the plugin's co-locate POST_BUILD step is
# skipped (guarded) and its OPENSWMM_INSTALL block stays off — we add our own
# install rule below.
set(OPENSWMM_BUILD_GPU_PLUGIN ON  CACHE BOOL   "" FORCE)
set(OPENSWMM_GPU_BACKEND      omp CACHE STRING "" FORCE)
add_subdirectory("${OPENSWMM_ENGINE_ROOT}/src/engine/2d/gpu"
                 "${CMAKE_BINARY_DIR}/gpu_build")

# Ship the plugin inside this wheel's import package (openswmm_gpu_omp/), NOT in
# the base openswmm/ package — so the two wheels never write into the same dir
# and the discovery shim can resolve this package's location by import.
install(TARGETS openswmm_gpu_omp
    LIBRARY DESTINATION openswmm_gpu_omp
    RUNTIME DESTINATION openswmm_gpu_omp)
