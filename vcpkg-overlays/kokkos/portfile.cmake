# Kokkos overlay port — Serial + OpenMP (default) or CUDA (Phase 2 `cuda` feature).
#
# vcpkg ships no kokkos port, so the project's `gpu` feature (kokkos + sundials)
# cannot resolve against the stock registry. This overlay supplies one.
#
# Default build: Serial + OpenMP host backends (on AppleClang OpenMP needs the
# Homebrew libomp wiring below). With the `cuda` feature: Serial + CUDA device
# backend — requires the CUDA toolkit and the target GPU architecture, supplied
# via the OPENSWMM_KOKKOS_CUDA_ARCH env var (a Kokkos_ARCH_* suffix, e.g.
# AMPERE80, HOPPER90). The cuda feature CANNOT build on macOS/Apple Silicon
# (no nvcc); build it on a CUDA host/CI with nvcc_wrapper as the C++ compiler.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO kokkos/kokkos
    REF "${VERSION}"
    SHA512 34825f2d0f202f49fecc24050a4790cb721f3a4cca21381fd0eb0c302bbafe90f997dc96130c2b2479c4344d11dbb062d4b41f2aaab11e49f5bd1da2c9e5d929
    HEAD_REF master
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" KOKKOS_SHARED)

# AppleClang's stock CMake FindOpenMP cannot locate Homebrew libomp on its own.
# Rather than duplicate that search here, route Kokkos' internal
# find_package(OpenMP) through the project's own finder — the SAME
# cmake/FindOpenMP.cmake the engine build uses (src/engine/CMakeLists.txt) — by
# putting the project cmake/ dir on CMAKE_MODULE_PATH for the Kokkos
# sub-configure. This keeps a single source of truth for how macOS locates
# libomp; if that finder changes, both the engine and this overlay follow.
#
# Apple-gated on purpose: the project finder only acts on APPLE and is a no-op
# elsewhere, so injecting it on Linux/Windows would shadow the working stock
# FindOpenMP and break Kokkos' OpenMP detection there. The overlay lives at
# <repo>/vcpkg-overlays/kokkos, so the finder is two levels up in <repo>/cmake.
set(OPENMP_HINTS)
if(VCPKG_TARGET_IS_OSX)
    get_filename_component(OPENSWMM_CMAKE_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../../cmake" ABSOLUTE)
    if(EXISTS "${OPENSWMM_CMAKE_DIR}/FindOpenMP.cmake")
        list(APPEND OPENMP_HINTS "-DCMAKE_MODULE_PATH=${OPENSWMM_CMAKE_DIR}")
    else()
        message(WARNING
            "kokkos overlay: expected the project OpenMP finder at "
            "'${OPENSWMM_CMAKE_DIR}/FindOpenMP.cmake' but it was not found; "
            "falling back to CMake's stock FindOpenMP (may fail to locate "
            "Homebrew libomp under AppleClang).")
    endif()
endif()

# Backend selection. Serial is always on (host fallback space). A device feature
# (`cuda`/`rocm`/`sycl`) swaps the host-parallel OpenMP backend for that device
# backend; with no device feature, the default OpenMP host backend is built.
set(BACKEND_OPTIONS -DKokkos_ENABLE_SERIAL=ON)
if("cuda" IN_LIST FEATURES)
    list(APPEND BACKEND_OPTIONS
        -DKokkos_ENABLE_CUDA=ON
        -DKokkos_ENABLE_CUDA_LAMBDA=ON
        -DKokkos_ENABLE_CUDA_CONSTEXPR=ON)
    if(DEFINED ENV{OPENSWMM_KOKKOS_CUDA_ARCH})
        list(APPEND BACKEND_OPTIONS "-DKokkos_ARCH_$ENV{OPENSWMM_KOKKOS_CUDA_ARCH}=ON")
    else()
        message(WARNING
            "kokkos[cuda]: OPENSWMM_KOKKOS_CUDA_ARCH not set — letting Kokkos "
            "auto-detect the GPU arch (may fail in CI without a visible device).")
    endif()
    set(OPENMP_HINTS)  # no libomp needed for the device build
elseif("rocm" IN_LIST FEATURES)
    # Phase 5 AMD backend. Build with ROCm's hipcc as the C++ compiler.
    list(APPEND BACKEND_OPTIONS -DKokkos_ENABLE_HIP=ON)
    if(DEFINED ENV{OPENSWMM_KOKKOS_HIP_ARCH})
        list(APPEND BACKEND_OPTIONS "-DKokkos_ARCH_$ENV{OPENSWMM_KOKKOS_HIP_ARCH}=ON")
    else()
        message(WARNING
            "kokkos[rocm]: OPENSWMM_KOKKOS_HIP_ARCH not set — letting Kokkos "
            "auto-detect the GPU arch (may fail in CI without a visible device).")
    endif()
    set(OPENMP_HINTS)  # no libomp needed for the device build
elseif("sycl" IN_LIST FEATURES)
    # Phase 5 Intel backend. Build with oneAPI's icpx as the C++ compiler.
    list(APPEND BACKEND_OPTIONS -DKokkos_ENABLE_SYCL=ON)
    if(DEFINED ENV{OPENSWMM_KOKKOS_SYCL_ARCH})
        list(APPEND BACKEND_OPTIONS "-DKokkos_ARCH_$ENV{OPENSWMM_KOKKOS_SYCL_ARCH}=ON")
    else()
        message(WARNING
            "kokkos[sycl]: OPENSWMM_KOKKOS_SYCL_ARCH not set — letting Kokkos "
            "auto-detect the GPU arch (may fail in CI without a visible device).")
    endif()
    set(OPENMP_HINTS)  # no libomp needed for the device build
else()
    list(APPEND BACKEND_OPTIONS -DKokkos_ENABLE_OPENMP=ON)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${BACKEND_OPTIONS}
        -DKokkos_ENABLE_TESTS=OFF
        -DKokkos_ENABLE_EXAMPLES=OFF
        -DKokkos_ENABLE_BENCHMARKS=OFF
        -DBUILD_SHARED_LIBS=${KOKKOS_SHARED}
        -DCMAKE_CXX_STANDARD=20
        ${OPENMP_HINTS}
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME Kokkos CONFIG_PATH lib/cmake/Kokkos)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share")

# Kokkos installs helper scripts (hpcbind, nvcc_wrapper) into bin/. For a host
# build these are unused and trip vcpkg's static-lib bin/ policy, so drop them;
# for the cuda backend, nvcc_wrapper is the compiler consumers need, so keep it.
# The rocm/sycl backends compile via external hipcc/icpx (no Kokkos wrapper to
# keep), so they drop bin/ like the host build.
if("cuda" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES nvcc_wrapper AUTO_CLEAN)
else()
    file(REMOVE_RECURSE
        "${CURRENT_PACKAGES_DIR}/bin"
        "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
