# ============================================================================
# OpenSWMM GPU surface-solver plugin
# ============================================================================
#
# Built only when -DOPENSWMM_BUILD_GPU_PLUGIN=ON. Standalone shared library:
# deliberately NOT linked into openswmm_engine. The core discovers it on a
# search path, dlopen()s it at runtime, and queries it via the C ABI in
# src/engine/2d/solver/GpuPluginAbi.h (docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md §4).
#
# Backend selector (OPENSWMM_GPU_BACKEND):
#   stub  (default) — no Kokkos; reports "no device"; host compiler only.
#   omp             — Phase 1 Kokkos/OpenMP CVODE solver (CvodeKokkosSurfaceSolver).
#   cuda            — Phase 2 Kokkos/CUDA  device backend, NVIDIA (same solver sources).
#   hip             — Phase 5 Kokkos/HIP   device backend, AMD   (same solver sources).
#   sycl            — Phase 5 Kokkos/SYCL  device backend, Intel (same solver sources).
#
# The omp/cuda/hip/sycl backends all share CvodeKokkosSurfaceSolver.cpp; they
# differ only in the Kokkos execution space (set via OPENSWMM_GPU_EXECSPACE_*
# below) and the per-vendor plugin probe — this is the Phase 5 payoff described
# in docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md §6.1. All require SUNDIALS built WITH
# the Kokkos N_Vector
# (ENABLE_KOKKOS=ON; the `kokkos` feature of the sundials overlay port):
#   * omp  — Kokkos built with Kokkos_ENABLE_OPENMP=ON
#   * cuda — Kokkos built with Kokkos_ENABLE_CUDA=ON + Kokkos_ARCH_<gpu>, compiled
#            via nvcc_wrapper (no nvcc on macOS — build on a CUDA host/CI).
#   * hip  — Kokkos built with Kokkos_ENABLE_HIP=ON  + Kokkos_ARCH_<gpu>, compiled
#            via hipcc (build on a ROCm host/CI).
#   * sycl — Kokkos built with Kokkos_ENABLE_SYCL=ON + Kokkos_ARCH_<gpu>, compiled
#            via icpx/oneAPI (build on an Intel-GPU host/CI).
# See docs/2D_GPU_PHASE1_VERIFICATION.md, docs/2D_GPU_PHASE2_VERIFICATION.md, and
# docs/2D_GPU_PHASE5_VERIFICATION.md.

# Backend chosen when the plugin is enabled (OPENSWMM_BUILD_GPU_PLUGIN=ON, which
# is itself opt-in so the base build stays Kokkos-free). `omp` is the usual
# choice — the Kokkos-OpenMP plugin runtime `auto` selects where installed
# (docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md §4.2); device hosts pass
# -DOPENSWMM_GPU_BACKEND=cuda|hip|sycl; `stub` builds the Kokkos-free placeholder.
set(OPENSWMM_GPU_BACKEND "omp" CACHE STRING
    "GPU plugin backend: stub | omp | cuda | hip | sycl")
set_property(CACHE OPENSWMM_GPU_BACKEND PROPERTY STRINGS stub omp cuda hip sycl)

set(_OPENSWMM_GPU_COMMON_PROPS
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

if(OPENSWMM_GPU_BACKEND STREQUAL "stub")

    add_library(openswmm_gpu_stub SHARED GpuPluginStub.cpp)
    target_include_directories(openswmm_gpu_stub
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../solver)
    set_target_properties(openswmm_gpu_stub PROPERTIES
        OUTPUT_NAME "openswmm_gpu_stub" ${_OPENSWMM_GPU_COMMON_PROPS})
    message(STATUS "OpenSWMM Engine: GPU plugin backend = stub (no Kokkos)")

elseif(OPENSWMM_GPU_BACKEND STREQUAL "omp")

    find_package(Kokkos REQUIRED)
    find_package(SUNDIALS REQUIRED COMPONENTS cvode nveckokkos)

    add_library(openswmm_gpu_omp SHARED
        GpuPluginOmp.cpp
        CvodeKokkosSurfaceSolver.cpp)
    target_include_directories(openswmm_gpu_omp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../solver
        ${CMAKE_CURRENT_SOURCE_DIR}/../data)
    # SUNDIALS' Kokkos N_Vector (<nvector/nvector_kokkos.hpp>) is header-only:
    # SUNDIALS 7.x exports no SUNDIALS::nveckokkos *link* target (it is absent
    # from the config's alias list, unlike the compiled vectors). The
    # `nveckokkos` COMPONENT above still validates the header was installed; the
    # header itself is reachable via SUNDIALS::cvode's transitive include dirs
    # and is implemented against Kokkos::kokkos, both already linked here.
    target_link_libraries(openswmm_gpu_omp PRIVATE
        Kokkos::kokkos
        SUNDIALS::cvode)
    target_compile_definitions(openswmm_gpu_omp PRIVATE OPENSWMM_GPU_EXECSPACE_OPENMP)
    set_target_properties(openswmm_gpu_omp PROPERTIES
        OUTPUT_NAME "openswmm_gpu_omp" ${_OPENSWMM_GPU_COMMON_PROPS})
    message(STATUS "OpenSWMM Engine: GPU plugin backend = omp (Kokkos OpenMP, Phase 1)")

elseif(OPENSWMM_GPU_BACKEND STREQUAL "cuda")

    # Phase 2 device backend. Same sources as the omp plugin — only the
    # execution space changes (OPENSWMM_GPU_EXECSPACE_CUDA -> Kokkos::Cuda) and
    # the probe reports an NVIDIA device. Requires a CUDA-enabled Kokkos
    # (Kokkos_ENABLE_CUDA=ON, built for the target Kokkos_ARCH_*) and a SUNDIALS
    # with the Kokkos N_Vector. Kokkos drives CUDA compilation of the device
    # lambdas; configure the plugin with Kokkos' nvcc_wrapper as CMAKE_CXX_COMPILER
    # (or a CUDA-clang toolchain) — see docs/2D_GPU_PHASE2_VERIFICATION.md.
    # NOT buildable on macOS/Apple Silicon (no nvcc); built on a CUDA host/CI.
    find_package(Kokkos REQUIRED)
    find_package(SUNDIALS REQUIRED COMPONENTS cvode nveckokkos)

    if(NOT Kokkos_ENABLE_CUDA)
        message(FATAL_ERROR
            "OPENSWMM_GPU_BACKEND=cuda requires a Kokkos built with "
            "Kokkos_ENABLE_CUDA=ON (found devices: ${Kokkos_DEVICES})")
    endif()

    add_library(openswmm_gpu_cuda SHARED
        GpuPluginCuda.cpp
        CvodeKokkosSurfaceSolver.cpp)
    target_include_directories(openswmm_gpu_cuda PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../solver
        ${CMAKE_CURRENT_SOURCE_DIR}/../data)
    # Header-only Kokkos N_Vector: same rationale as the omp branch — link
    # SUNDIALS::cvode (carries the SUNDIALS includes) + Kokkos::kokkos, not a
    # nonexistent SUNDIALS::nveckokkos target.
    target_link_libraries(openswmm_gpu_cuda PRIVATE
        Kokkos::kokkos
        SUNDIALS::cvode)
    target_compile_definitions(openswmm_gpu_cuda PRIVATE OPENSWMM_GPU_EXECSPACE_CUDA)
    set_target_properties(openswmm_gpu_cuda PROPERTIES
        OUTPUT_NAME "openswmm_gpu_cuda" ${_OPENSWMM_GPU_COMMON_PROPS})
    message(STATUS "OpenSWMM Engine: GPU plugin backend = cuda (Kokkos CUDA, Phase 2)")

elseif(OPENSWMM_GPU_BACKEND STREQUAL "hip")

    # Phase 5 device backend (AMD). Same solver sources as the cuda branch — only
    # the execution space changes (OPENSWMM_GPU_EXECSPACE_HIP -> Kokkos::HIP) and
    # the probe (GpuPluginHip.cpp) reports an AMD device. Requires a HIP-enabled
    # Kokkos (Kokkos_ENABLE_HIP=ON, built for the target Kokkos_ARCH_*) and a
    # SUNDIALS with the Kokkos N_Vector. Configure the plugin with ROCm's hipcc
    # as CMAKE_CXX_COMPILER. NOT buildable on macOS; built on a ROCm host/CI.
    find_package(Kokkos REQUIRED)
    find_package(SUNDIALS REQUIRED COMPONENTS cvode nveckokkos)

    if(NOT Kokkos_ENABLE_HIP)
        message(FATAL_ERROR
            "OPENSWMM_GPU_BACKEND=hip requires a Kokkos built with "
            "Kokkos_ENABLE_HIP=ON (found devices: ${Kokkos_DEVICES})")
    endif()

    add_library(openswmm_gpu_hip SHARED
        GpuPluginHip.cpp
        CvodeKokkosSurfaceSolver.cpp)
    target_include_directories(openswmm_gpu_hip PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../solver
        ${CMAKE_CURRENT_SOURCE_DIR}/../data)
    # Header-only Kokkos N_Vector: same rationale as the cuda branch — link
    # SUNDIALS::cvode (carries the SUNDIALS includes) + Kokkos::kokkos.
    target_link_libraries(openswmm_gpu_hip PRIVATE
        Kokkos::kokkos
        SUNDIALS::cvode)
    target_compile_definitions(openswmm_gpu_hip PRIVATE OPENSWMM_GPU_EXECSPACE_HIP)
    set_target_properties(openswmm_gpu_hip PROPERTIES
        OUTPUT_NAME "openswmm_gpu_hip" ${_OPENSWMM_GPU_COMMON_PROPS})
    message(STATUS "OpenSWMM Engine: GPU plugin backend = hip (Kokkos HIP, Phase 5)")

elseif(OPENSWMM_GPU_BACKEND STREQUAL "sycl")

    # Phase 5 device backend (Intel). Same solver sources as the cuda branch —
    # only the execution space changes (OPENSWMM_GPU_EXECSPACE_SYCL ->
    # Kokkos::SYCL) and the probe (GpuPluginSycl.cpp) reports an Intel device.
    # Requires a SYCL-enabled Kokkos (Kokkos_ENABLE_SYCL=ON, built for the target
    # Kokkos_ARCH_*) and a SUNDIALS with the Kokkos N_Vector. Configure the plugin
    # with oneAPI's icpx as CMAKE_CXX_COMPILER. NOT buildable on macOS; built on
    # an Intel-GPU host/CI.
    find_package(Kokkos REQUIRED)
    find_package(SUNDIALS REQUIRED COMPONENTS cvode nveckokkos)

    if(NOT Kokkos_ENABLE_SYCL)
        message(FATAL_ERROR
            "OPENSWMM_GPU_BACKEND=sycl requires a Kokkos built with "
            "Kokkos_ENABLE_SYCL=ON (found devices: ${Kokkos_DEVICES})")
    endif()

    add_library(openswmm_gpu_sycl SHARED
        GpuPluginSycl.cpp
        CvodeKokkosSurfaceSolver.cpp)
    target_include_directories(openswmm_gpu_sycl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../solver
        ${CMAKE_CURRENT_SOURCE_DIR}/../data)
    # Header-only Kokkos N_Vector: same rationale as the cuda branch — link
    # SUNDIALS::cvode (carries the SUNDIALS includes) + Kokkos::kokkos.
    target_link_libraries(openswmm_gpu_sycl PRIVATE
        Kokkos::kokkos
        SUNDIALS::cvode)
    target_compile_definitions(openswmm_gpu_sycl PRIVATE OPENSWMM_GPU_EXECSPACE_SYCL)
    set_target_properties(openswmm_gpu_sycl PROPERTIES
        OUTPUT_NAME "openswmm_gpu_sycl" ${_OPENSWMM_GPU_COMMON_PROPS})
    message(STATUS "OpenSWMM Engine: GPU plugin backend = sycl (Kokkos SYCL, Phase 5)")

else()
    message(FATAL_ERROR
        "OPENSWMM_GPU_BACKEND='${OPENSWMM_GPU_BACKEND}' not recognised "
        "(expected 'stub', 'omp', 'cuda', 'hip', or 'sycl')")
endif()
