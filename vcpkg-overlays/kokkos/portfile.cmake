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

# Kokkos' shared (DLL) build is broken on MSVC: kokkoscore.dll exports no
# symbols, so MSVC produces no import library and the dependent Kokkos DLLs
# fail to link ("LNK1104: cannot open file 'kokkoscore.lib'"). Kokkos upstream
# recommends static linkage on Windows. The only consumer of the Kokkos library
# here is the openswmm_gpu_omp plugin DLL (SUNDIALS' Kokkos N_Vector is
# header-only and links no Kokkos library of its own), so a single static
# Kokkos copy is safe — there is no second runtime to clash with.
if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

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
    if(DEFINED ENV{OPENSWMM_KOKKOS_CUDA_ARCH} AND NOT "$ENV{OPENSWMM_KOKKOS_CUDA_ARCH}" STREQUAL "")
        list(APPEND BACKEND_OPTIONS "-DKokkos_ARCH_$ENV{OPENSWMM_KOKKOS_CUDA_ARCH}=ON")
    elseif(DEFINED ENV{OPENSWMMENGINE_KOKKOS_CUDA_ARCH} AND NOT "$ENV{OPENSWMMENGINE_KOKKOS_CUDA_ARCH}" STREQUAL "")
        list(APPEND BACKEND_OPTIONS "-DKokkos_ARCH_$ENV{OPENSWMMENGINE_KOKKOS_CUDA_ARCH}=ON")
    else()
        # vcpkg sanitizes its cmake subprocess environment, so OPENSWMM_KOKKOS_CUDA_ARCH
        # does not reach this portfile script. Kokkos' GPU auto-detection also fails in
        # vcpkg's sandboxed build (cannot run device executables during cmake configure).
        # Fall back to ADA89 (Ada Lovelace SM 8.9) = NVIDIA RTX 2000 Ada series.
        # To build for a different GPU, set OPENSWMM_KOKKOS_CUDA_ARCH in the environment
        # or add -DKokkos_ARCH_<ARCH>=ON via VCPKG_CMAKE_CONFIGURE_OPTIONS in a triplet.
        list(APPEND BACKEND_OPTIONS "-DKokkos_ARCH_ADA89=ON")
        message(STATUS
            "kokkos[cuda]: OPENSWMM_KOKKOS_CUDA_ARCH not in vcpkg env -- "
            "defaulting to ADA89 (Ada Lovelace SM 8.9, RTX 2000 Ada).")
    endif()
    # On Windows, neither nvcc_wrapper (bash script) nor Clang (separate install)
    # may be available. Kokkos 4.7 offers a built-in path: set
    # Kokkos_ENABLE_COMPILE_AS_CMAKE_LANGUAGE=ON (cmake/kokkos_enable_options.cmake:87).
    # This tells Kokkos to check CMAKE_CUDA_COMPILER (nvcc) instead of
    # CMAKE_CXX_COMPILER (cl.exe) when identifying the compiler, which sets
    # KOKKOS_CXX_COMPILER_ID=NVIDIA and satisfies kokkos_test_cxx_std.cmake:140
    # without requiring nvcc_wrapper or Clang. cmake's native CUDA language then
    # compiles device code with nvcc and host C++ with MSVC; MSVC flags (e.g.
    # -std:c++20 with MSVC colon syntax) are never forwarded to nvcc.
    if(VCPKG_TARGET_IS_WINDOWS)
        set(_kokkos_nvcc "")
        if(DEFINED ENV{CUDA_PATH} AND EXISTS "$ENV{CUDA_PATH}/bin/nvcc.exe")
            set(_kokkos_nvcc "$ENV{CUDA_PATH}/bin/nvcc.exe")
        else()
            foreach(_cuda_ver 13.3 13.2 13.1 13.0 12.8 12.6 12.5 12.4)
                set(_probe
                    "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v${_cuda_ver}/bin/nvcc.exe")
                if(EXISTS "${_probe}")
                    set(_kokkos_nvcc "${_probe}")
                    break()
                endif()
            endforeach()
        endif()
        if(_kokkos_nvcc)
            message(STATUS "kokkos[cuda] Windows: CUDA language mode (MSVC+nvcc), nvcc=${_kokkos_nvcc}")
            list(APPEND BACKEND_OPTIONS
                "-DKokkos_ENABLE_COMPILE_AS_CMAKE_LANGUAGE=ON"
                "-DCMAKE_CUDA_COMPILER=${_kokkos_nvcc}"
                "-DCMAKE_CUDA_STANDARD=20"
                "-DCMAKE_CUDA_STANDARD_REQUIRED=ON")
        else()
            message(FATAL_ERROR
                "kokkos[cuda] on Windows: nvcc.exe not found. "
                "Install the CUDA Toolkit and set CUDA_PATH, or install to the "
                "default path (C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v<ver>).")
        endif()
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
    # MSVC's default `/openmp` advertises only OpenMP 2.0, but Kokkos requires
    # >= 3.0, so stock FindOpenMP rejects it ("Found unsuitable version 2.0").
    # Route the Kokkos host backend through MSVC's LLVM OpenMP runtime
    # (`/openmp:llvm`), which implements the OpenMP 3.x constructs Kokkos uses,
    # and tell FindOpenMP no extra link library is needed — the runtime is
    # supplied by the compiler switch.
    #
    # The catch: even under `/openmp:llvm`, MSVC still defines the `_OPENMP`
    # macro as `200203` (2.0), so FindOpenMP's version probe computes 2.0 and
    # rejects it against Kokkos' >= 3.0 requirement. The macro is the only thing
    # that is stale — the runtime/codegen support the newer constructs — so we
    # pin OpenMP_<lang>_SPEC_DATE to an OpenMP 4.5 date (2015-11). FindOpenMP
    # skips its `_OPENMP` probe when SPEC_DATE is already set, computes 4.5, and
    # accepts the toolchain. This keeps the OpenMP host backend enabled on
    # Windows (OpenMP is the default backend on every platform).
    if(VCPKG_TARGET_IS_WINDOWS)
        list(APPEND OPENMP_HINTS
            "-DOpenMP_C_FLAGS=-openmp:llvm"
            "-DOpenMP_C_LIB_NAMES="
            "-DOpenMP_C_SPEC_DATE=201511"
            "-DOpenMP_CXX_FLAGS=-openmp:llvm"
            "-DOpenMP_CXX_LIB_NAMES="
            "-DOpenMP_CXX_SPEC_DATE=201511")
    endif()
endif()

# Keep Kokkos' internal debug instrumentation OFF in BOTH the debug and release
# sub-builds. vcpkg installs a single (release) header set, but under a Debug
# CMAKE_BUILD_TYPE Kokkos turns KOKKOS_ENABLE_DEBUG on, which changes the
# SharedAllocationRecord ABI — so the debug lib loses the out-of-line ctor the
# (release) installed headers reference, and a Debug consumer fails to link
# (undefined SharedAllocationRecord(...,std::string)). Forcing it OFF makes the
# debug lib match the installed headers so Debug builds of the plugin link.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${BACKEND_OPTIONS}
        -DKokkos_ENABLE_DEBUG=OFF
        -DKokkos_ENABLE_DEBUG_BOUNDS_CHECK=OFF
        -DKokkos_ENABLE_DEBUG_DUALVIEW_MODIFY_CHECK=OFF
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

# Kokkos installs helper scripts (hpcbind, nvcc_wrapper, kokkos_launch_compiler)
# into bin/. For a host build these are unused and trip vcpkg's static-lib bin/
# policy, so drop them. For CUDA on Linux: nvcc_wrapper is the compiler consumers
# need, so keep it. For CUDA on Windows: we use cmake's native CUDA language mode
# (KOKKOS_COMPILER_IS_KOKKOS_LAUNCH_COMPILER=ON), so consumers use cmake's CUDA
# language directly instead of kokkos_launch_compiler -- drop bin/ entirely.
# The rocm/sycl backends drop bin/ like the host build.
if("cuda" IN_LIST FEATURES AND NOT VCPKG_TARGET_IS_WINDOWS)
    vcpkg_copy_tools(TOOL_NAMES nvcc_wrapper AUTO_CLEAN)
else()
    file(REMOVE_RECURSE
        "${CURRENT_PACKAGES_DIR}/bin"
        "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
