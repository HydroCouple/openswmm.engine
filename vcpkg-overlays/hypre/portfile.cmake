# OpenSWMM overlay of the upstream vcpkg `hypre` port.
#
# Difference from upstream: builds hypre SEQUENTIALLY (HYPRE_ENABLE_MPI=OFF),
# so the port has no `mpi` dependency. OpenSWMM uses BoomerAMG single-process
# from the CVODE psetup/psolve callbacks (MPI_COMM_WORLD resolves to hypre's
# mpistubs no-op communicator), which is all the 2D surface preconditioner
# needs and keeps the portable build free of an MPI stack.
#
# BLAS/LAPACK: hypre needs only a tiny dense BLAS/LAPACK subset for BoomerAMG,
# and it ships portable C implementations of exactly that subset. We use those
# bundled C sources on every platform EXCEPT macOS, so the build carries no
# external blas/lapack dependency and no Fortran toolchain (`lapack-reference`):
#   * Windows — `lapack-reference`'s Fortran dep (`vcpkg-gfortran` -> LLVMFlang)
#     puts a GNU-driver `clang.exe` on PATH; CMake's C-compiler ABI probe then
#     picks that clang while the x64-windows triplet injects MSVC-style flags
#     (/nologo, /MDd, ...), so the probe fails ("clang: error: no such file or
#     directory: '/nologo'") and configure aborts.
#   * Linux — the manylinux/musllinux wheel containers expose a gfortran driver
#     (identified as GNU 14.x) but an incomplete Fortran runtime, so
#     `lapack-reference`'s configure fails its Fortran ABI probe ("building
#     lapack-reference:*-linux failed with: BUILD_FAILED"), taking the whole
#     wheel build down before any wheel is produced.
# macOS keeps the external vcpkg `lapack` (reference LAPACK): its runners carry a
# complete gfortran and that path builds cleanly. See vcpkg.json (blas/lapack are
# gated to `osx`).
if(VCPKG_TARGET_IS_OSX)
    set(HYPRE_USE_BUNDLED_BLAS_LAPACK OFF)
else()
    set(HYPRE_USE_BUNDLED_BLAS_LAPACK ON)
endif()

if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO hypre-space/hypre
    REF "v${VERSION}"
    SHA512 c1b09a31781ce4e1a411c486424cf7a4df1275d53445ed83d0e4e210dcc87e9c09e17e26cc5ee736aebbd70618674cd3b7dba6736f8e725ba1c3d981869ada24
    HEAD_REF master
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" HYPRE_SHARED)

# NOTE (2026-06-27): HYPRE_WITH_OPENMP was tried and REVERTED. Threaded
# BoomerAMG regressed even the single-thread path (1M-cell CVODE-DW: 12.5 s →
# 16.7 s serial; 100k slower at every thread count) — its setup/coarsening does
# not parallelize and the OpenMP-instrumented code carries net overhead on CPU.
# The 2D AMG preconditioner is single-threaded by design; thread the SUNDIALS
# vector ops instead (the sundials overlay's `openmp` feature). The real parallel
# AMG path is GPU (the CUDA plugin with a device-built hypre). See
# examples/imex_scaling/RESULTS_phase2.md.

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/src"
    DISABLE_PARALLEL_CONFIGURE # See 'Autogenerate csr_spgemm_device_numer$ files'
    OPTIONS
        -DHYPRE_SHARED=${HYPRE_SHARED}
        -DHYPRE_WITH_MPI=OFF            # sequential (HYPRE_SEQUENTIAL) — no MPI dependency
        # ON on Windows → use hypre's bundled C BLAS/LAPACK (no external lapack).
        -DHYPRE_ENABLE_HYPRE_BLAS=${HYPRE_USE_BUNDLED_BLAS_LAPACK}
        -DHYPRE_ENABLE_HYPRE_LAPACK=${HYPRE_USE_BUNDLED_BLAS_LAPACK}
    OPTIONS_RELEASE
        -DHYPRE_BUILD_TYPE=Release
        "-DHYPRE_INSTALL_PREFIX=${CURRENT_PACKAGES_DIR}"
    OPTIONS_DEBUG
        -DHYPRE_BUILD_TYPE=Debug
        "-DHYPRE_INSTALL_PREFIX=${CURRENT_PACKAGES_DIR}/debug"
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/HYPRE)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

# Handle copyright
file(INSTALL "${SOURCE_PATH}/COPYRIGHT" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
