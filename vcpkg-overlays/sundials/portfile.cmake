# SUNDIALS overlay — stock vcpkg port plus an optional `kokkos` feature.
#
# The stock vcpkg sundials port builds no device N_Vector, so the openswmm_gpu_omp
# plugin (which needs SUNDIALS::nveckokkos) cannot link against it. This overlay
# adds a `kokkos` feature that turns on ENABLE_KOKKOS against the kokkos overlay
# port. Everything else mirrors the upstream port verbatim.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO LLNL/sundials
    REF "v${VERSION}"
    SHA512 b6d15f68f25c5326bd42abb5e3652cc98e83d2eb31b213c9144b46c5b93fd123be5972e9d36217fdd09a0002dee3f78e530c21eda85f3b4d1d8d93b007546ea0
    HEAD_REF master
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" SUN_BUILD_STATIC)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" SUN_BUILD_SHARED)

if(VCPKG_TARGET_IS_ANDROID)
    set(POSIX_TIMERS "-DSUNDIALS_POSIX_TIMERS=TRUE")
endif()

# Optional Kokkos N_Vector (SUNDIALS::nveckokkos).
set(KOKKOS_OPTIONS)
if("kokkos" IN_LIST FEATURES)
    # SundialsKokkos.cmake does: find_package(Kokkos REQUIRED HINTS "${Kokkos_DIR}"
    # NO_DEFAULT_PATH) — so neither Kokkos_ROOT nor CMAKE_PREFIX_PATH is consulted;
    # Kokkos_DIR must point straight at the installed KokkosConfig.cmake directory.
    # The kokkos overlay installs that config via vcpkg_cmake_config_fixup(
    # PACKAGE_NAME Kokkos ...) -> share/Kokkos (capital K). Case-insensitive
    # macOS/Windows tolerate "share/kokkos", but case-sensitive Linux does not,
    # so spell it exactly as installed.
    set(KOKKOS_OPTIONS
        -DENABLE_KOKKOS=ON
        "-DKokkos_DIR=${CURRENT_INSTALLED_DIR}/share/Kokkos"
        -DCMAKE_CXX_STANDARD=20)
    # KokkosConfig.cmake calls find_dependency(CUDAToolkit REQUIRED) when Kokkos
    # was built with the CUDA backend. vcpkg sandboxes the cmake subprocess
    # environment so nvcc is not on PATH — provide CUDAToolkit_ROOT explicitly.
    if(VCPKG_TARGET_IS_WINDOWS)
        set(_cuda_root "")
        if(DEFINED ENV{CUDA_PATH} AND EXISTS "$ENV{CUDA_PATH}/bin/nvcc.exe")
            set(_cuda_root "$ENV{CUDA_PATH}")
        else()
            foreach(_cuda_ver 13.3 13.2 13.1 13.0 12.8 12.6 12.5 12.4)
                set(_probe
                    "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v${_cuda_ver}")
                if(EXISTS "${_probe}/bin/nvcc.exe")
                    set(_cuda_root "${_probe}")
                    break()
                endif()
            endforeach()
        endif()
        if(_cuda_root)
            message(STATUS "sundials[kokkos] Windows: CUDAToolkit_ROOT=${_cuda_root}")
            list(APPEND KOKKOS_OPTIONS "-DCUDAToolkit_ROOT=${_cuda_root}")
        else()
            message(WARNING
                "sundials[kokkos] Windows: nvcc.exe not found; "
                "KokkosConfig.cmake will fail to locate CUDAToolkit. "
                "Install CUDA Toolkit or set CUDA_PATH.")
        endif()
    endif()
    # Kokkos's package config does find_dependency(OpenMP REQUIRED); on AppleClang
    # CMake cannot locate Homebrew libomp unaided, so wire it explicitly.
    if(VCPKG_TARGET_IS_OSX)
        foreach(_libomp_prefix /opt/homebrew/opt/libomp /usr/local/opt/libomp)
            if(EXISTS "${_libomp_prefix}/lib/libomp.dylib")
                list(APPEND KOKKOS_OPTIONS
                    "-DOpenMP_CXX_FLAGS=-Xpreprocessor -fopenmp -I${_libomp_prefix}/include"
                    "-DOpenMP_CXX_LIB_NAMES=omp"
                    "-DOpenMP_omp_LIBRARY=${_libomp_prefix}/lib/libomp.dylib")
                break()
            endif()
        endforeach()
    endif()
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${POSIX_TIMERS}
        ${KOKKOS_OPTIONS}
        -D_BUILD_EXAMPLES=OFF
        -DEXAMPLES_ENABLE_CXX=OFF
        -DEXAMPLES_INSTALL=OFF
        -DSUNDIALS_TEST_UNITTESTS=OFF
        -DBUILD_STATIC_LIBS=${SUN_BUILD_STATIC}
        -DBUILD_SHARED_LIBS=${SUN_BUILD_SHARED}
)

vcpkg_cmake_install()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(REMOVE "${CURRENT_PACKAGES_DIR}/LICENSE")
file(REMOVE "${CURRENT_PACKAGES_DIR}/debug/LICENSE")

vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/${PORT}")
