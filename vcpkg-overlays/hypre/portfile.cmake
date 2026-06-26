# OpenSWMM overlay of the upstream vcpkg `hypre` port.
#
# Difference from upstream: builds hypre SEQUENTIALLY (HYPRE_ENABLE_MPI=OFF),
# so the port has no `mpi` dependency. OpenSWMM uses BoomerAMG single-process
# from the CVODE psetup/psolve callbacks (MPI_COMM_WORLD resolves to hypre's
# mpistubs no-op communicator), which is all the 2D surface preconditioner
# needs and keeps the portable build free of an MPI stack.
#
# BLAS/LAPACK: on Linux/macOS we link the external vcpkg `lapack` (reference
# LAPACK), which builds cleanly with the platform's gfortran. On Windows that
# pulls `lapack-reference`, whose Fortran dependency (`vcpkg-gfortran` ->
# LLVMFlang) puts a GNU-driver `clang.exe` on PATH; CMake's C-compiler ABI
# probe then picks that clang while the x64-windows triplet injects MSVC-style
# flags (/nologo, /MDd, ...), so the probe fails ("clang: error: no such file or
# directory: '/nologo'") and the whole configure aborts. hypre ships portable C
# implementations of the small BLAS/LAPACK subset BoomerAMG needs, so on Windows
# we use those instead -- no Fortran toolchain, no lapack-reference, no external
# blas/lapack dependency. See vcpkg.json (blas/lapack are gated to !windows).
if(VCPKG_TARGET_IS_WINDOWS)
    set(HYPRE_USE_BUNDLED_BLAS_LAPACK ON)
else()
    set(HYPRE_USE_BUNDLED_BLAS_LAPACK OFF)
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
