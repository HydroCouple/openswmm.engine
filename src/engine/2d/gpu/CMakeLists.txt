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
#   omp             — Kokkos/OpenMP host-parallel marcher.
#   cuda            — Kokkos/CUDA  device backend, NVIDIA (same solver sources).
#   hip             — Kokkos/HIP   device backend, AMD   (same solver sources).
#   sycl            — Kokkos/SYCL  device backend, Intel (same solver sources).
#
# All backends share ExplicitKokkosSurfaceSolver.cpp (the Kokkos port of the
# explicit LTS marcher — the only 2D integrator since the D2 retirement,
# 2026-07-29); they differ only in the Kokkos execution space (set via
# OPENSWMM_GPU_EXECSPACE_* below) and the per-vendor plugin probe. No SUNDIALS,
# no hypre:
#   * omp  — Kokkos built with Kokkos_ENABLE_OPENMP=ON
#   * cuda — Kokkos built with Kokkos_ENABLE_CUDA=ON + Kokkos_ARCH_<gpu>, compiled
#            via nvcc_wrapper (no nvcc on macOS — build on a CUDA host/CI).
#   * hip  — Kokkos built with Kokkos_ENABLE_HIP=ON  + Kokkos_ARCH_<gpu>, compiled
#            via hipcc (build on a ROCm host/CI).
#   * sycl — Kokkos built with Kokkos_ENABLE_SYCL=ON + Kokkos_ARCH_<gpu>, compiled
#            via icpx/oneAPI (build on an Intel-GPU host/CI).

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
    target_compile_definitions(openswmm_gpu_stub PRIVATE OPENSWMM_GPU_PLUGIN_BUILD)
    set_target_properties(openswmm_gpu_stub PROPERTIES
        OUTPUT_NAME "openswmm_gpu_stub" ${_OPENSWMM_GPU_COMMON_PROPS})
    message(STATUS "OpenSWMM Engine: GPU plugin backend = stub (no Kokkos)")

elseif(OPENSWMM_GPU_BACKEND STREQUAL "omp")

    find_package(Kokkos REQUIRED)

    add_library(openswmm_gpu_omp SHARED
        GpuPluginOmp.cpp
        ExplicitKokkosSurfaceSolver.cpp
        ../solver/InertialEdges.cpp)
    target_include_directories(openswmm_gpu_omp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../solver
        ${CMAKE_CURRENT_SOURCE_DIR}/../data)
    target_link_libraries(openswmm_gpu_omp PRIVATE Kokkos::kokkos)
    target_compile_definitions(openswmm_gpu_omp PRIVATE
        OPENSWMM_GPU_EXECSPACE_OPENMP OPENSWMM_GPU_PLUGIN_BUILD)
    # MSVC's legacy `/openmp` (OpenMP 2.0/2.5) lacks omp_get_level(), which
    # Kokkos' OpenMP backend (<OpenMP/Kokkos_OpenMP_Instance.hpp>) calls — it
    # fails to compile with C3861. `/openmp:llvm` provides OpenMP 3.0+ runtime
    # routines. Added as a PRIVATE option so it follows (and overrides, with a
    # benign D9025) any `/openmp` injected via Kokkos' interface flags.
    if(MSVC)
        target_compile_options(openswmm_gpu_omp PRIVATE /openmp:llvm)
    endif()
    set_target_properties(openswmm_gpu_omp PROPERTIES
        OUTPUT_NAME "openswmm_gpu_omp" ${_OPENSWMM_GPU_COMMON_PROPS})
    message(STATUS "OpenSWMM Engine: GPU plugin backend = omp (Kokkos OpenMP, Phase 1)")

elseif(OPENSWMM_GPU_BACKEND STREQUAL "cuda")

    # Phase 2 device backend. Same sources as the omp plugin — only the
    # execution space changes (OPENSWMM_GPU_EXECSPACE_CUDA -> Kokkos::Cuda) and
    # the probe reports an NVIDIA device. Requires a CUDA-enabled Kokkos
    # (Kokkos_ENABLE_CUDA=ON, built for the target Kokkos_ARCH_*) 
    #
    # Compiler strategy by platform:
    #   Linux/macOS — configure the plugin with Kokkos' nvcc_wrapper as
    #                 CMAKE_CXX_COMPILER (or a CUDA-clang toolchain).
    #   Windows     — nvcc_wrapper is a bash script and unavailable on Windows.
    #                 Use CMake's native CUDA language instead: CMake invokes nvcc
    #                 with cl.exe as the host compiler, then MSVC for the remaining
    #                 C++ translation units. Source files containing Kokkos device
    #                 lambdas are tagged LANGUAGE CUDA so nvcc compiles them.
    #
    # NOT buildable on macOS/Apple Silicon (no nvcc); built on a CUDA host/CI.
    find_package(Kokkos REQUIRED)

    if(NOT Kokkos_ENABLE_CUDA)
        message(FATAL_ERROR
            "OPENSWMM_GPU_BACKEND=cuda requires a Kokkos built with "
            "Kokkos_ENABLE_CUDA=ON (found devices: ${Kokkos_DEVICES})")
    endif()

    # On Windows, enable CMake's CUDA language so nvcc (with cl.exe as host) can
    # compile device-lambda sources that nvcc_wrapper would handle on Linux.
    if(WIN32)
        enable_language(CUDA)
        # Propagate the C++20 standard to nvcc; nvcc does not inherit CXX_STANDARD.
        set(CMAKE_CUDA_STANDARD 20)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)
        # Separable compilation is required when device code spans multiple TUs
        # (Kokkos parallel kernels across GpuPluginCuda + ExplicitKokkosSurfaceSolver).
        set(CMAKE_CUDA_SEPARABLE_COMPILATION ON)
        # CUDA 13.x CCCL headers require the MSVC standard-conforming preprocessor.
        string(APPEND CMAKE_CUDA_FLAGS " -Xcompiler=/Zc:preprocessor")
    endif()

    add_library(openswmm_gpu_cuda SHARED
        GpuPluginCuda.cpp
        ExplicitKokkosSurfaceSolver.cpp
        ../solver/InertialEdges.cpp)

    # On Windows, mark the device-lambda sources so CMake routes them through nvcc.
    # On Linux, nvcc_wrapper as CMAKE_CXX_COMPILER handles all .cpp files uniformly.
    if(WIN32)
        set_source_files_properties(
            GpuPluginCuda.cpp
            ExplicitKokkosSurfaceSolver.cpp
            PROPERTIES LANGUAGE CUDA)
        set_target_properties(openswmm_gpu_cuda PROPERTIES
            CUDA_STANDARD 20
            CUDA_STANDARD_REQUIRED ON
            CUDA_SEPARABLE_COMPILATION ON)
    endif()

    target_include_directories(openswmm_gpu_cuda PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../solver
        ${CMAKE_CURRENT_SOURCE_DIR}/../data)
    target_link_libraries(openswmm_gpu_cuda PRIVATE Kokkos::kokkos)
    target_compile_definitions(openswmm_gpu_cuda PRIVATE
        OPENSWMM_GPU_EXECSPACE_CUDA OPENSWMM_GPU_PLUGIN_BUILD)
    set_target_properties(openswmm_gpu_cuda PROPERTIES
        OUTPUT_NAME "openswmm_gpu_cuda" ${_OPENSWMM_GPU_COMMON_PROPS})
    message(STATUS "OpenSWMM Engine: GPU plugin backend = cuda (Kokkos CUDA, Phase 2)")

elseif(OPENSWMM_GPU_BACKEND STREQUAL "hip")

    # Phase 5 device backend (AMD). Same solver sources as the cuda branch — only
    # the execution space changes (OPENSWMM_GPU_EXECSPACE_HIP -> Kokkos::HIP) and
    # the probe (GpuPluginHip.cpp) reports an AMD device. Requires a HIP-enabled
    # Kokkos (Kokkos_ENABLE_HIP=ON, built for the target Kokkos_ARCH_*). Configure the plugin with ROCm's hipcc
    # as CMAKE_CXX_COMPILER. NOT buildable on macOS; built on a ROCm host/CI.
    find_package(Kokkos REQUIRED)

    if(NOT Kokkos_ENABLE_HIP)
        message(FATAL_ERROR
            "OPENSWMM_GPU_BACKEND=hip requires a Kokkos built with "
            "Kokkos_ENABLE_HIP=ON (found devices: ${Kokkos_DEVICES})")
    endif()

    add_library(openswmm_gpu_hip SHARED
        GpuPluginHip.cpp
        ExplicitKokkosSurfaceSolver.cpp
        ../solver/InertialEdges.cpp)
    target_include_directories(openswmm_gpu_hip PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../solver
        ${CMAKE_CURRENT_SOURCE_DIR}/../data)
    target_link_libraries(openswmm_gpu_hip PRIVATE Kokkos::kokkos)
    target_compile_definitions(openswmm_gpu_hip PRIVATE
        OPENSWMM_GPU_EXECSPACE_HIP OPENSWMM_GPU_PLUGIN_BUILD)
    set_target_properties(openswmm_gpu_hip PROPERTIES
        OUTPUT_NAME "openswmm_gpu_hip" ${_OPENSWMM_GPU_COMMON_PROPS})
    message(STATUS "OpenSWMM Engine: GPU plugin backend = hip (Kokkos HIP, Phase 5)")

elseif(OPENSWMM_GPU_BACKEND STREQUAL "sycl")

    # Phase 5 device backend (Intel). Same solver sources as the cuda branch —
    # only the execution space changes (OPENSWMM_GPU_EXECSPACE_SYCL ->
    # Kokkos::SYCL) and the probe (GpuPluginSycl.cpp) reports an Intel device.
    # Requires a SYCL-enabled Kokkos (Kokkos_ENABLE_SYCL=ON, built for the target
    # Kokkos_ARCH_*). Configure the plugin
    # with oneAPI's icpx as CMAKE_CXX_COMPILER. NOT buildable on macOS; built on
    # an Intel-GPU host/CI.
    find_package(Kokkos REQUIRED)

    if(NOT Kokkos_ENABLE_SYCL)
        message(FATAL_ERROR
            "OPENSWMM_GPU_BACKEND=sycl requires a Kokkos built with "
            "Kokkos_ENABLE_SYCL=ON (found devices: ${Kokkos_DEVICES})")
    endif()

    add_library(openswmm_gpu_sycl SHARED
        GpuPluginSycl.cpp
        ExplicitKokkosSurfaceSolver.cpp
        ../solver/InertialEdges.cpp)
    target_include_directories(openswmm_gpu_sycl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../solver
        ${CMAKE_CURRENT_SOURCE_DIR}/../data)
    target_link_libraries(openswmm_gpu_sycl PRIVATE Kokkos::kokkos)
    target_compile_definitions(openswmm_gpu_sycl PRIVATE
        OPENSWMM_GPU_EXECSPACE_SYCL OPENSWMM_GPU_PLUGIN_BUILD)
    set_target_properties(openswmm_gpu_sycl PROPERTIES
        OUTPUT_NAME "openswmm_gpu_sycl" ${_OPENSWMM_GPU_COMMON_PROPS})
    message(STATUS "OpenSWMM Engine: GPU plugin backend = sycl (Kokkos SYCL, Phase 5)")

else()
    message(FATAL_ERROR
        "OPENSWMM_GPU_BACKEND='${OPENSWMM_GPU_BACKEND}' not recognised "
        "(expected 'stub', 'omp', 'cuda', 'hip', or 'sycl')")
endif()

# ---------------------------------------------------------------------------
# Co-locate the built plugin with the engine and install it next to the engine
# library, so the runtime factory (SurfaceSolverFactory::search_dirs — engine
# dir, then <engine dir>/gpu) discovers it and `auto` selects it by default.
# ---------------------------------------------------------------------------
set(_OPENSWMM_GPU_TARGET "openswmm_gpu_${OPENSWMM_GPU_BACKEND}")

# Build tree: copy the plugin next to the engine's *built* library, which is the
# directory the CLI and test executables actually load the engine from (their
# build RPATH points at the engine target's dir, not the bin/ staging copy). The
# factory resolves its search root via dladdr on the loaded engine, so the plugin
# must sit beside it for `auto` to discover it with no env var.
#
# Guarded on the engine target existing: when this CMakeLists is built standalone
# (the Python companion wheel and the Phase-1 p1b_omp_plugin wrapper both do
# add_subdirectory() on it without the engine), there is no openswmm_engine to
# co-locate beside, so the copy is simply skipped.
if(TARGET openswmm_engine)
    add_custom_command(TARGET ${_OPENSWMM_GPU_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${_OPENSWMM_GPU_TARGET}>"
            "$<TARGET_FILE_DIR:openswmm_engine>/$<TARGET_FILE_NAME:${_OPENSWMM_GPU_TARGET}>"
        VERBATIM)
endif()
# Note: Debug builds of this plugin link cleanly because the kokkos overlay port
# forces Kokkos_ENABLE_DEBUG=OFF, keeping the debug Kokkos lib ABI-compatible
# with the (release) installed Kokkos headers vcpkg ships. See
# vcpkg-overlays/kokkos/portfile.cmake.

if(OPENSWMM_INSTALL)
    # Installed layout: place the plugin where the engine library lands (lib/ on
    # Unix, bin/ on Windows) so the factory's engine-dir search finds it. The
    # Python companion wheel overrides this to its own import-package dir.
    if(WIN32)
        set(_OPENSWMM_GPU_INSTALL_DEST_DEFAULT "${CMAKE_INSTALL_BINDIR}")
    else()
        set(_OPENSWMM_GPU_INSTALL_DEST_DEFAULT "${CMAKE_INSTALL_LIBDIR}")
    endif()
    set(OPENSWMM_GPU_PLUGIN_INSTALL_DESTINATION
        "${_OPENSWMM_GPU_INSTALL_DEST_DEFAULT}" CACHE STRING
        "Install destination (relative to prefix) for the GPU plugin shared library")
    install(TARGETS ${_OPENSWMM_GPU_TARGET}
        LIBRARY DESTINATION "${OPENSWMM_GPU_PLUGIN_INSTALL_DESTINATION}"
        RUNTIME DESTINATION "${OPENSWMM_GPU_PLUGIN_INSTALL_DESTINATION}")

    # macOS: the plugin links Homebrew libomp by ABSOLUTE path while the engine
    # references the bundled @rpath/libomp.dylib — loading both aborts every 2D
    # run with "OMP: Error #15" (two OpenMP runtimes). The BundleRuntimeDeps
    # rewrite can miss this plugin (subdirectory install ordering + the
    # opt-vs-Cellar symlink split), so fix it here, immediately after our own
    # install rule: rewrite ANY absolute libomp reference to @rpath, make sure
    # the loader can find the bundled copy next to the CLI (bin/) or the engine
    # (lib/), and re-sign — install_name_tool invalidates the ad-hoc signature
    # on arm64, which otherwise SIGKILLs at dlopen.
    if(APPLE)
        install(CODE "
            set(_plugin \"\${CMAKE_INSTALL_PREFIX}/${OPENSWMM_GPU_PLUGIN_INSTALL_DESTINATION}/$<TARGET_FILE_NAME:${_OPENSWMM_GPU_TARGET}>\")
            if(EXISTS \"\${_plugin}\")
                execute_process(COMMAND bash -c \"
                    set -e
                    plugin='\${_plugin}'
                    otool -L \\\"\$plugin\\\" | awk 'NR>1 {print \$1}' | grep '/libomp\\.dylib\$' | grep -v '^@' | while read -r old; do
                        install_name_tool -change \\\"\$old\\\" @rpath/libomp.dylib \\\"\$plugin\\\"
                    done
                    for rp in @loader_path @loader_path/../bin @loader_path/../lib; do
                        install_name_tool -add_rpath \\\"\$rp\\\" \\\"\$plugin\\\" 2>/dev/null || true
                    done
                    codesign --force --sign - \\\"\$plugin\\\"
                \")
            endif()
        ")
    endif()
endif()
