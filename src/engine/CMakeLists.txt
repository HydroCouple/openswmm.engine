#
# CMakeLists.txt - CMake configuration for the new openswmm_engine library
#
# Library name:    openswmm_engine
# CMake target:    openswmm::engine
# Output name:     openswmm.engine (libopenwmm.engine.so / openswmm.engine.dll)
#
# This is the new data-oriented, plugin-extensible, reentrant SWMM engine.
# It is built alongside the legacy engine (openswmm_legacy_engine) to allow
# side-by-side comparison during the transition period.
#
# Architecture:
#   - core/          : SWMMEngine lifecycle, SimulationContext (SoA), HotStartManager
#   - data/          : SoA data stores (NodeData, LinkData, SubcatchData, etc.)
#   - input/         : Tokenizer, SectionRegistry, CsvTimeSeriesReader
#   - hydrology/     : Runoff, Gage, Climate, Snow, Infiltration, LID
#   - hydraulics/    : Routing, DynamicWave, KinematicWave, TimestepController
#   - quality/       : QualityRouting, Treatment
#   - controls/      : Controls, RuleEngine
#   - output/        : IOThread, SimulationSnapshot, WriteTask
#   - plugins/       : PluginFactory, DefaultOutputPlugin, DefaultReportPlugin
#   - math/          : SIMD abstraction, OdeSolver, FindRoot
#
# See: docs/MASTER_IMPLEMENTATION_PLAN.md  Phase 2–7
#

cmake_minimum_required(VERSION 3.21)

# Collect all source files (excluding geopackage — built as a separate library)
file(GLOB_RECURSE OPENSWMM_ENGINE_SOURCES
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    core/*.cpp
    data/*.cpp
    edit/*.cpp
    input/*.cpp
    hydrology/*.cpp
    hydraulics/*.cpp
    quality/*.cpp
    controls/*.cpp
    output/*.cpp
    plugins/*.cpp
    math/*.cpp
)
list(FILTER OPENSWMM_ENGINE_SOURCES EXCLUDE REGEX "input/geopackage/.*")

# PORT v2 (2026-07-31): the uncertainty sidecar is being re-homed onto the
# explicit-marcher base incrementally (PORT_V2_PREFLIGHT.md). Its 1D sources
# (uncertainty/*.cpp) and 2D sources (2d/uncertainty/*) are NOT swept by these
# globs, so they sit inert until wired — each is listed explicitly as it lands.
# The one exception is hydraulics/SpectralCoarse.cpp (caught by hydraulics/*.cpp):
# it depends on the DW spectral_accel scaffolding that has not been re-applied to
# this base (and is flagged droppable), so exclude it until/unless that lands.
list(FILTER OPENSWMM_ENGINE_SOURCES EXCLUDE REGEX "hydraulics/SpectralCoarse.*")

# Shared graph-Laplacian eigensolver (reorthogonalized Lanczos + tridiagonal QL)
# backing both the 1D network ROM basis and the 2D MeshEigenBasis. Dimension-
# agnostic: it takes a CSR Laplacian and returns the k smallest nontrivial
# eigenpairs.
list(APPEND OPENSWMM_ENGINE_SOURCES uncertainty/GraphEigenBasis.cpp)

# 1D network spectral ROM. Built unconditionally: the 2D surface ROM links it
# for per-member 1D heads at coupling points (SpectralROM::applyCouplingFlux →
# SpectralROM1D::reconstructHead), and the 1D ROM stands alone without 2D.
list(APPEND OPENSWMM_ENGINE_SOURCES uncertainty/SpectralROM1D.cpp)

# Shared parameter-registry ensemble (PARAMETER_REGISTRY.md §6): generic LHS
# columns for any registered [UNCERTAINTY] source beyond the ROMs' built-in
# Manning/rainfall pair (e.g. a 1D INFLOW FORCING_VECTOR spec). Consumed by
# SWMMEngine::buildROM1D().
list(APPEND OPENSWMM_ENGINE_SOURCES uncertainty/UncertaintyEnsemble.cpp)

file(GLOB_RECURSE OPENSWMM_ENGINE_HEADERS
    RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    core/*.hpp
    data/*.hpp
    edit/*.hpp
    input/*.hpp
    hydrology/*.hpp
    hydraulics/*.hpp
    quality/*.hpp
    controls/*.hpp
    output/*.hpp
    plugins/*.hpp
    math/*.hpp
)
list(FILTER OPENSWMM_ENGINE_HEADERS EXCLUDE REGEX "input/geopackage/.*")

# ---- Library target ----
# NOTE: If there are no .cpp files yet (skeleton phase), use a placeholder.
# Remove this condition when real sources exist.
if(OPENSWMM_ENGINE_SOURCES)
    add_library(openswmm_engine SHARED ${OPENSWMM_ENGINE_SOURCES})
else()
    # Placeholder object library until sources are written
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/placeholder.cpp
         "// OpenSWMM Engine — placeholder (Phase 2+ sources not yet written)\n"
         "// See docs/MASTER_IMPLEMENTATION_PLAN.md\n")
    add_library(openswmm_engine SHARED ${CMAKE_CURRENT_BINARY_DIR}/placeholder.cpp)
endif()

# ---- Alias targets ----
add_library(openswmm::engine ALIAS openswmm_engine)

# ---- Output name ----
set_target_properties(openswmm_engine PROPERTIES
    OUTPUT_NAME   "openswmm.engine"
    VERSION       ${PROJECT_VERSION}
    SOVERSION     ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET      default
    C_VISIBILITY_PRESET        default
    VISIBILITY_INLINES_HIDDEN  OFF
    # Use explicit export macros instead of CMake auto-export scanning.
    # Auto-export invokes __create_def on object files and is brittle on
    # Windows CI toolchains.
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
)

# ---- C++ standard ----
target_compile_features(openswmm_engine PUBLIC cxx_std_20)

# ---- Include directories ----
target_include_directories(openswmm_engine
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/openswmm/engine>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/openswmm/plugin_sdk>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
        $<INSTALL_INTERFACE:include/openswmm/engine>
        $<INSTALL_INTERFACE:include/openswmm/plugin_sdk>
        # Lets installed-engine consumers resolve the quoted
        # `#include "legacy_version.h"` (generated header installed by
        # src/legacy/engine) without referencing the legacy target's BINARY_DIR.
        $<INSTALL_INTERFACE:include/openswmm/legacy/engine>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---- Link against common interface ----
target_link_libraries(openswmm_engine
    PUBLIC
        openswmm_common
    PRIVATE
        # On macOS, libm is part of libSystem and HDF5 already pulls in
        # -lm transitively — adding it here produces an "ignoring duplicate
        # libraries: '-lm'" ld warning. Restrict to non-APPLE UNIX.
        $<$<AND:$<NOT:$<BOOL:$<CXX_COMPILER_ID:MSVC>>>,$<NOT:$<PLATFORM_ID:Darwin>>>:m>
)

# ---- Threads (for IOThread) ----
find_package(Threads REQUIRED)
target_link_libraries(openswmm_engine PRIVATE Threads::Threads)

# ---- OpenMP (for parallel solver loops) ----
# macOS/AppleClang is the only platform where the stock FindOpenMP cannot
# locate OpenMP on its own (Homebrew libomp): there we add the project cmake/
# dir to the module path and use the custom finder. On Linux/Windows the stock
# FindOpenMP discovers the compiler's OpenMP — and is also what Kokkos'
# find_dependency(OpenMP) resolves to at find_package(Kokkos) time — so we
# must NOT put cmake/FindOpenMP.cmake on the module path there (it is Apple-only
# and would shadow stock, breaking that transitive find).
if(APPLE)
    list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
    include(${PROJECT_SOURCE_DIR}/cmake/FindOpenMP.cmake)
else()
    find_package(OpenMP)
endif()

if(OpenMP_FOUND OR TARGET OpenMP::OpenMP_CXX)
    target_link_libraries(openswmm_engine PRIVATE OpenMP::OpenMP_CXX)
    target_compile_definitions(openswmm_engine PRIVATE SWMM_USE_OPENMP)
    message(STATUS "OpenSWMM Engine: OpenMP enabled for parallel solver loops")
else()
    message(STATUS "OpenSWMM Engine: OpenMP not found — single-threaded solver")
endif()

# ---- Platform-specific DLL loading ----
if(UNIX AND NOT APPLE)
    target_link_libraries(openswmm_engine PRIVATE dl)
elseif(APPLE)
    # dl is part of libSystem on macOS
endif()

# ---- CBLAS (PR H4 — quantile-reconstruction GEMM) ----
# Apple's Accelerate framework ships a full CBLAS implementation as part of
# the base OS (zero new deps, per the H4 spec) and is the only platform this
# PR wires up directly. Elsewhere, SpectralROM1D/SpectralROM fall back to a
# portable triple-loop GEMM (OPENSWMM_HAVE_CBLAS undefined) -- correct
# everywhere, just not BLAS-accelerated off Apple. Adding a system BLAS
# (OpenBLAS/MKL) dependency for Linux/Windows is explicitly out of scope here.
if(APPLE)
    find_library(ACCELERATE_FRAMEWORK Accelerate)
    if(ACCELERATE_FRAMEWORK)
        target_link_libraries(openswmm_engine PRIVATE ${ACCELERATE_FRAMEWORK})
        target_compile_definitions(openswmm_engine PRIVATE OPENSWMM_HAVE_CBLAS)
        message(STATUS "OpenSWMM Engine: CBLAS via Accelerate framework enabled (quantile-reconstruction GEMM)")
    else()
        message(STATUS "OpenSWMM Engine: Accelerate framework not found — quantile reconstruction uses the portable fallback")
    endif()
else()
    message(STATUS "OpenSWMM Engine: no CBLAS wired on this platform — quantile reconstruction uses the portable fallback")
endif()

# ---- SIMD compiler flags ----
option(OPENSWMM_FORCE_SCALAR "Disable SIMD intrinsics; use scalar fallback only" OFF)

if(OPENSWMM_FORCE_SCALAR)
    target_compile_definitions(openswmm_engine PRIVATE OPENSWMM_SIMD_SCALAR=1)
    message(STATUS "OpenSWMM Engine: SIMD disabled — using scalar fallback")
else()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            # Enable AVX2 + FMA for SIMD acceleration (x86_64)
            target_compile_options(openswmm_engine PRIVATE -mavx2 -mfma)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            # ARM64 (Apple Silicon / Graviton / Cortex-A): NEON is always
            # available, but an explicit CPU target lets the compiler emit
            # more aggressive auto-vectorised code for the inner hot loops
            # (XSectBatch, DynamicWave momentum kernels). -mcpu=native when
            # available, else a safe default baseline.
            include(CheckCXXCompilerFlag)
            check_cxx_compiler_flag("-mcpu=native" OPENSWMM_HAVE_MCPU_NATIVE)
            if(OPENSWMM_HAVE_MCPU_NATIVE)
                target_compile_options(openswmm_engine PRIVATE -mcpu=native)
                message(STATUS "OpenSWMM Engine: -mcpu=native enabled for arm64")
            endif()
        endif()
    elseif(MSVC)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|x86")
            target_compile_options(openswmm_engine PRIVATE /arch:AVX2)
        endif()
        # MSVC ARM64: NEON always available, no flag needed
    endif()
endif()

# ---- Fast geometry / Manning kernels (opt-OUT; default ON) ----
# The refactored engine ships with the fast cross-section table lookup
# (reciprocal-multiply + `*inv_delta`, no per-element IEEE divide) and the fast
# Manning friction (x*cbrt(x) instead of libm pow). Together ~10% faster DW
# routing. These are NOT bit-exact vs legacy (ULP-level; ~1e-4 flow accumulated
# over long runs — well inside the abs 1e-8 / rel 1e-7 gate per lookup). Turn
# BOTH off for bit-exact legacy parity (the QA parity harness / regression
# baselines): -DOPENSWMM_FAST_XSECT_LOOKUP=OFF -DOPENSWMM_FAST_MANNING_POW=OFF
option(OPENSWMM_FAST_XSECT_LOOKUP
    "Fast cross-section table lookup (reciprocal-multiply; not bit-exact vs legacy)" ON)
option(OPENSWMM_FAST_MANNING_POW
    "Fast Manning friction via cbrt instead of libm pow (not bit-exact vs legacy)" ON)
if(OPENSWMM_FAST_XSECT_LOOKUP)
    target_compile_definitions(openswmm_engine PRIVATE SWMM_XSECT_FAST_LOOKUP)
endif()
if(OPENSWMM_FAST_MANNING_POW)
    target_compile_definitions(openswmm_engine PRIVATE SWMM_FAST_MANNING_POW)
endif()
message(STATUS "OpenSWMM Engine: fast kernels — xsect_lookup=${OPENSWMM_FAST_XSECT_LOOKUP}"
        " manning_pow=${OPENSWMM_FAST_MANNING_POW}"
        " (ON ⇒ ~10% faster DW routing, NOT bit-exact vs legacy)")

# ---- Force-optimized override (for Debug parent builds) ----
# When the engine is consumed as a subdirectory by a parent project that's
# in Debug mode (e.g. a GUI under active development), CMAKE_BUILD_TYPE=Debug
# normally collapses -O3 to -O0 and disables LTO. The engine is a stable
# dependency in those scenarios — runtime perf, not engine debuggability,
# is what matters — so this option lets a parent force the engine targets
# to compile with the Darwin/Linux preset Release flags as PRIVATE target
# options, overriding the parent's Debug flags for engine TUs only.
option(OPENSWMM_ENGINE_FORCE_OPTIMIZED
    "Always compile engine targets with optimized flags, even in Debug builds" OFF)
if(OPENSWMM_ENGINE_FORCE_OPTIMIZED AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(openswmm_engine PRIVATE
        -O2 -fdata-sections -ffunction-sections
        -fno-fast-math -ffp-contract=off -fno-math-errno)
    message(STATUS "OpenSWMM Engine: OPENSWMM_ENGINE_FORCE_OPTIMIZED ON → "
            "compiling engine with -O2 + section-gc regardless of CMAKE_BUILD_TYPE")
endif()

# ---- Link-time optimization (IPO/LTO) ----
# Enables cross-translation-unit inlining and dead-code elimination. Critical
# for performance when hot-path functions (fastmath::pow4_3, SIMD helpers,
# per-conduit dispatch) cross TU boundaries. Guarded by compiler support check
# to avoid failures on toolchains that lack bitcode linkers.
if(NOT DEFINED OPENSWMM_ENABLE_LTO)
    set(OPENSWMM_ENABLE_LTO ON CACHE BOOL "Enable interprocedural / link-time optimization for Release builds")
endif()
# musl libc (Alpine / musllinux wheels): the `fortify-headers` package (pulled in
# by `build-base`) wraps stdio in `always_inline` gnu_inline functions that GCC
# cannot inline during the `-flto` link ("inlining failed in call to
# 'always_inline' 'vsnprintf': function body can be overwritten at link time"),
# which aborts the engine shared-library link. LTO + fortify-headers is a
# known-broken combination on musl; disable IPO there. glibc (manylinux),
# macOS, and Windows keep LTO. check_ipo_supported() does NOT catch this — its
# probe program never includes the fortify stdio wrappers.
if(OPENSWMM_ENABLE_LTO AND CMAKE_CXX_COMPILER)
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
                    OUTPUT_VARIABLE OPENSWMM_TARGET_TRIPLE
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(OPENSWMM_TARGET_TRIPLE MATCHES "musl")
        set(OPENSWMM_ENABLE_LTO OFF)
        message(STATUS "OpenSWMM Engine: IPO/LTO disabled on musl libc "
                       "(${OPENSWMM_TARGET_TRIPLE}) — fortify-headers + LTO incompatible")
    endif()
endif()
if(OPENSWMM_ENABLE_LTO AND
   (OPENSWMM_ENGINE_FORCE_OPTIMIZED OR NOT CMAKE_BUILD_TYPE STREQUAL "Debug"))
    include(CheckIPOSupported)
    check_ipo_supported(RESULT OPENSWMM_IPO_SUPPORTED OUTPUT OPENSWMM_IPO_ERROR)
    if(OPENSWMM_IPO_SUPPORTED)
        set_property(TARGET openswmm_engine PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "OpenSWMM Engine: IPO/LTO enabled")
    else()
        message(STATUS "OpenSWMM Engine: IPO/LTO not supported by toolchain (${OPENSWMM_IPO_ERROR}); skipping")
    endif()
endif()

# ---- Export macros ----
include(GenerateExportHeader)
generate_export_header(openswmm_engine
    BASE_NAME            openswmm_engine
    EXPORT_MACRO_NAME    SWMM_ENGINE_API
    EXPORT_FILE_NAME     openswmm_engine_export.h
    STATIC_DEFINE        OPENSWMM_ENGINE_STATIC
)

file(COPY ${CMAKE_CURRENT_BINARY_DIR}/openswmm_engine_export.h
    DESTINATION ${PROJECT_SOURCE_DIR}/include/openswmm/engine
)

# ---- Preprocessor definitions ----
if(WIN32)
    target_compile_definitions(openswmm_engine PRIVATE SWMM_OS_WINDOWS)
elseif(APPLE)
    target_compile_definitions(openswmm_engine PRIVATE SWMM_OS_MACOS)
elseif(UNIX)
    target_compile_definitions(openswmm_engine PRIVATE SWMM_OS_LINUX)
endif()

# ---- RPATH ----
# INSTALL_RPATH: wheel layout — external bundled deps (libomp etc.) placed by
# delocate/auditwheel land in openswmm/.dylibs/, one level above openswmm/engine/.
# For standalone cmake --install the system finds libomp via its own search path.
# CMAKE_INSTALL_RPATH_USE_LINK_PATH (set globally) appends the linked-library
# directory automatically, so consumers can always locate this dylib.
#
# BUILD_WITH_INSTALL_RPATH is left FALSE (the global default) so that the
# build-tree binary uses CMake's automatically-computed build rpath (link dirs)
# rather than the wheel-layout path that does not exist in the build tree.
if(APPLE)
    # @loader_path/../.dylibs : wheel layout (delocate places bundled deps there)
    # @loader_path             : sibling-co-located deps when consumer puts the
    #                            engine + its deps in the same dir
    # @loader_path/../bin      : standalone install (cmake --install) — deps are
    #                            bundled into bin/, engine .dylib lives in lib/
    # @loader_path/../lib      : sibling lib dir (engine.dylib's lib/)
    set(LIB_ROOT "@loader_path/../.dylibs;@loader_path;@loader_path/../bin;@loader_path/../lib")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN/../.dylibs;$ORIGIN;$ORIGIN/../bin;$ORIGIN/../lib")
else()
    set(LIB_ROOT "")
endif()

if(NOT WIN32)
    set_target_properties(openswmm_engine PROPERTIES
        MACOSX_RPATH  TRUE
        INSTALL_RPATH "${LIB_ROOT}"
    )
endif()

# Keep vendored static dependencies (HDF5, SQLite) PRIVATE at the dynamic-symbol
# level, not just the link interface. Linking them PRIVATE keeps them out of the
# CMake usage requirements, but the static archives' symbols still land in this
# dylib's export table with default visibility (e.g. ~3.5k _H5* symbols). A
# consumer that ALSO links its own copy — the GUI links HDF5 for its 2D mesh
# reader and SQLite transitively via GDAL — then cross-binds some calls to this
# dylib's copy and crashes in HDF5's atexit teardown (H5_term_library →
# H5I_clear_type operating on a foreign HDF5's global state). Hiding these from
# the export table makes each binary use its own copy end-to-end, regardless of
# version. (Kokkos is not hidden here: no current consumer links its
# own copy, and the GPU plugin carries its own statically.)
if(APPLE)
    target_link_options(openswmm_engine PRIVATE
        "LINKER:-unexported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/macos_unexported_symbols.txt")
endif()

# ---- Install ----
install(
    TARGETS openswmm_engine
    EXPORT  OpenSWMMEngineTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Bundle imported third-party runtime libraries next to the executables that
# consume openswmm_engine. The CLI install (src/cli/CMakeLists.txt) already
# captures the full closure via RUNTIME_DEPENDENCIES; this block exists for
# downstream consumers who install openswmm_engine without an executable
# (e.g. a Python wheel built standalone outside cibuildwheel, or a sibling
# C++ project that links the engine). Co-locating in bin/ matches the RPATH
# already configured on the engine ("@loader_path/../.dylibs" stays for the
# wheel path; "@loader_path" + "$ORIGIN" cover the standalone-install path).
openswmm_install_runtime_deps(${CMAKE_INSTALL_BINDIR}
    hdf5::hdf5-shared
    unofficial::sqlite3::sqlite3
    SQLite::SQLite3
)

install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/include/openswmm/engine
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/openswmm
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# ---- Copy to build tree for testing ----
add_custom_command(TARGET openswmm_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:openswmm_engine>
        ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:openswmm_engine>
)

# ---- Internal static library for MSVC unit tests ----
# On Windows, unit tests that exercise internal (non-exported) C++ symbols
# cannot link against the shared DLL since WINDOWS_EXPORT_ALL_SYMBOLS is OFF.
# We build a matching STATIC library with OPENSWMM_ENGINE_STATIC defined so
# SWMM_ENGINE_API resolves to nothing, making all symbols directly available.
# This avoids fragile per-test target_sources tracking of transitive deps.
if(MSVC AND OPENSWMM_BUILD_UNIT_TESTS)
    add_library(openswmm_engine_internal STATIC ${OPENSWMM_ENGINE_SOURCES})
    add_library(openswmm::engine_internal ALIAS openswmm_engine_internal)

    target_compile_features(openswmm_engine_internal PUBLIC cxx_std_20)
    target_compile_definitions(openswmm_engine_internal
        PUBLIC  OPENSWMM_ENGINE_STATIC
        PRIVATE SWMM_OS_WINDOWS
    )

    target_include_directories(openswmm_engine_internal
        PUBLIC
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/openswmm/engine>
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/openswmm/plugin_sdk>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(openswmm_engine_internal
        PUBLIC  openswmm_common
        PRIVATE Threads::Threads
    )

    if(OpenMP_FOUND OR TARGET OpenMP::OpenMP_CXX)
        target_link_libraries(openswmm_engine_internal PRIVATE OpenMP::OpenMP_CXX)
        target_compile_definitions(openswmm_engine_internal PRIVATE SWMM_USE_OPENMP)
    endif()

    if(OPENSWMM_FORCE_SCALAR)
        target_compile_definitions(openswmm_engine_internal PRIVATE OPENSWMM_SIMD_SCALAR=1)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|x86")
        target_compile_options(openswmm_engine_internal PRIVATE /arch:AVX2)
    endif()

    # Fast kernels (default ON) — must match the shared engine so MSVC unit
    # tests exercise the same build variant.
    if(OPENSWMM_FAST_XSECT_LOOKUP)
        target_compile_definitions(openswmm_engine_internal PRIVATE SWMM_XSECT_FAST_LOOKUP)
    endif()
    if(OPENSWMM_FAST_MANNING_POW)
        target_compile_definitions(openswmm_engine_internal PRIVATE SWMM_FAST_MANNING_POW)
    endif()

    message(STATUS "OpenSWMM Engine: internal STATIC library enabled for MSVC unit tests")
endif()

# ---- Optional 2D Surface Routing Module ----
if(OPENSWMM_BUILD_2D)
    # Collect 2D source files. The explicit local-inertial LTS marcher is the
    # only integrator (D2 retirement, 2026-07-29): no SUNDIALS, no hypre, no
    # global linear solve anywhere in the 2D module.
    file(GLOB_RECURSE OPENSWMM_2D_SOURCES
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        2d/data/*.cpp
        2d/mesh/*.cpp
        2d/solver/SurfaceFluxCalculator.cpp
        2d/coupling/*.cpp
        2d/input/*.cpp
        2d/output/*.cpp
        2d/SurfaceRouter2D.cpp
        2d/api/*.cpp
    )

    # Explicit local-inertial marcher (the 2D integrator) + its unique-edge
    # CSR layout.
    list(APPEND OPENSWMM_2D_SOURCES 2d/solver/InertialEdges.cpp)
    list(APPEND OPENSWMM_2D_SOURCES 2d/solver/ExplicitInertialSolver.cpp)

    # Runtime backend selector (dlopen of the optional GPU plugin; serial CPU
    # fallback). Lives in the core — it owns the CPU↔plugin decision — but
    # pulls in no Kokkos/GPU dependency itself.
    list(APPEND OPENSWMM_2D_SOURCES 2d/solver/SurfaceSolverFactory.cpp)

    # Surface uncertainty ROM: the mesh eigenbasis it projects onto, the
    # deviation-form ensemble integrator itself, and the correlated-field
    # generator for spatial parameter perturbation. All read mesh + state only
    # (never solver internals), so they are integrator-independent and owned by
    # SurfaceRouter2D rather than by any backend.
    list(APPEND OPENSWMM_2D_SOURCES 2d/uncertainty/MeshEigenBasis.cpp)
    list(APPEND OPENSWMM_2D_SOURCES 2d/uncertainty/DeviationOperator2D.cpp)
    list(APPEND OPENSWMM_2D_SOURCES 2d/uncertainty/SpectralROM.cpp)
    list(APPEND OPENSWMM_2D_SOURCES 2d/uncertainty/CorrelatedFieldGenerator.cpp)

    # SR-2c deterministic gridded rainfall: the HDF5 grid-plane reader
    # (already required by Default2DOutputPlugin below, so no new dependency)
    # consumed by SurfaceRouter2D::initGridRainfall(), and the [SOFT_RAINFALL_GRID]
    # parser (registerSoftRainfallGridSection) that produces the grid-source specs
    # it's called with. NOT registerQualityUncertaintySection from the same file
    # (superseded by register2DSections' own general LAYER-aware [UNCERTAINTY]
    # handler) — the file compiles as a unit regardless, only the one function
    # is ever called.
    list(APPEND OPENSWMM_2D_SOURCES uncertainty/GridFileReader.cpp)
    list(APPEND OPENSWMM_2D_SOURCES uncertainty/QualityUncertaintyHandler.cpp)

    target_sources(openswmm_engine PRIVATE ${OPENSWMM_2D_SOURCES})

    # Default2DOutputPlugin emits CF-1.11/UGRID-1.0 HDF5 — required dep.
    # Prefer vcpkg's hdf5-config.cmake (CONFIG mode) since CMake's bundled
    # FindHDF5.cmake hunts for h5cc/well-known layouts and misses vcpkg installs.
    # Fall back to FindHDF5 for system installs (Linux distros, Homebrew) that
    # don't ship hdf5-config.cmake.
    find_package(hdf5 CONFIG QUIET)
    if(hdf5_FOUND)
        if(TARGET hdf5::hdf5-shared)
            set(_OPENSWMM_HDF5_TARGET hdf5::hdf5-shared)
        elseif(TARGET hdf5::hdf5-static)
            set(_OPENSWMM_HDF5_TARGET hdf5::hdf5-static)
        else()
            message(FATAL_ERROR "hdf5 CONFIG package found but neither hdf5::hdf5-shared nor hdf5::hdf5-static is defined")
        endif()
        target_link_libraries(openswmm_engine PRIVATE ${_OPENSWMM_HDF5_TARGET})
    else()
        find_package(HDF5 REQUIRED COMPONENTS C)
        target_link_libraries(openswmm_engine PRIVATE ${HDF5_C_LIBRARIES})
        target_include_directories(openswmm_engine PRIVATE ${HDF5_INCLUDE_DIRS})
    endif()

    target_compile_definitions(openswmm_engine PUBLIC OPENSWMM_HAS_2D=1)

    # Also add to the internal static library for MSVC unit tests
    if(TARGET openswmm_engine_internal)
        target_sources(openswmm_engine_internal PRIVATE ${OPENSWMM_2D_SOURCES})
        if(hdf5_FOUND)
            target_link_libraries(openswmm_engine_internal PRIVATE ${_OPENSWMM_HDF5_TARGET})
        else()
            target_link_libraries(openswmm_engine_internal PRIVATE ${HDF5_C_LIBRARIES})
            target_include_directories(openswmm_engine_internal PRIVATE ${HDF5_INCLUDE_DIRS})
        endif()
        target_compile_definitions(openswmm_engine_internal PUBLIC OPENSWMM_HAS_2D=1)
    endif()

    message(STATUS "OpenSWMM Engine: 2D surface routing module enabled (explicit LTS marcher + HDF5)")

    # Optional GPU surface-solver plugin (standalone shared lib, dlopen()ed
    # at runtime — NOT linked into openswmm_engine). Phase 0 stub for now.
    # See docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md.
    if(OPENSWMM_BUILD_GPU_PLUGIN)
        add_subdirectory(2d/gpu)
    endif()
else()
    message(STATUS "OpenSWMM Engine: 2D surface routing disabled (set -DOPENSWMM_BUILD_2D=ON to enable)")
endif()

# ---- Optional GeoPackage I/O library ----
if(OPENSWMM_WITH_GEOPACKAGE)
    add_subdirectory(input/geopackage)
    # Slice RC.1 (APPROVED 2026-05-25, see §R.3 of openswmm.gui's
    # GUI_IMPLEMENTATION_PLAN.md) — link the static GeoPackage lib into
    # the engine SHARED so its IPluginComponentInfo singleton is
    # resolvable from PluginFactory::register_builtin_infos AND so the
    # PUBLIC OPENSWMM_HAS_GEOPACKAGE define propagates to the engine's
    # own TUs (gating the explicit register_one call).
    # PRIVATE because the cycle (engine ←→ geopackage PRIVATE both
    # directions) collapses cleanly when geopackage's STATIC archive is
    # absorbed into the engine SHARED at link time.
    target_link_libraries(openswmm_engine PRIVATE openswmm_geopackage)
    if(TARGET openswmm_engine_internal)
        target_link_libraries(openswmm_engine_internal PRIVATE openswmm_geopackage)
    endif()

    # Linkers on every platform we target drop .obj/.o files from a static
    # archive when no source in the consumer references their symbols. The
    # swmm_gpkg_* C API exists solely for external consumers (Cython
    # _geopackage, host apps) — nothing inside the engine itself calls it —
    # so the openswmm_geopackage_impl translation unit is pruned and the
    # C API never reaches the engine shared library's export table.
    #
    # On Windows the failure is loud: 27 LNK2019 errors at the Cython
    # _geopackage link step (openswmm.engine.lib has no swmm_gpkg_* in it).
    # On Linux/macOS the failure is silent: the wheel builds fine, but
    # `from openswmm.engine._geopackage import GeoPackage` raises
    # ImportError at runtime (undefined symbol), and test_geopackage.py
    # wraps that import in try/except and skips its tests.
    #
    # Force whole-archive linking so every translation unit in the
    # geopackage static archive is retained in the engine shared library:
    #   - MSVC:    /WHOLEARCHIVE:<archive-name>
    #   - Apple:   -Wl,-force_load,<archive-path>
    #   - GNU ld:  -Wl,--whole-archive <archive-path> -Wl,--no-whole-archive
    # The visibility attributes on swmm_gpkg_* (SWMM_ENGINE_API → either
    # __declspec(dllexport) on MSVC or __attribute__((visibility("default")))
    # elsewhere — see input/geopackage/CMakeLists.txt) then ensure the
    # retained symbols are also exported.
    if(MSVC)
        target_link_options(openswmm_engine PRIVATE
            "/WHOLEARCHIVE:$<TARGET_FILE_NAME:openswmm_geopackage>"
        )
    elseif(APPLE)
        target_link_options(openswmm_engine PRIVATE
            "LINKER:-force_load,$<TARGET_FILE:openswmm_geopackage>"
        )
    else()
        # GNU ld / lld accept bracketed --whole-archive; the archive path
        # must appear between the two flags.
        target_link_options(openswmm_engine PRIVATE
            "LINKER:--whole-archive"
            "$<TARGET_FILE:openswmm_geopackage>"
            "LINKER:--no-whole-archive"
        )
    endif()
    # Re-export OPENSWMM_HAS_GEOPACKAGE on the engine target so consumers
    # (tests, host apps) can compile-time gate on engine capabilities.
    # The PRIVATE link above hides geopackage's own PUBLIC defines from
    # engine consumers, but the capability is an engine-level fact once
    # geopackage is absorbed into the engine SHARED.
    target_compile_definitions(openswmm_engine PUBLIC OPENSWMM_HAS_GEOPACKAGE=1)
    if(TARGET openswmm_engine_internal)
        target_compile_definitions(openswmm_engine_internal PUBLIC OPENSWMM_HAS_GEOPACKAGE=1)
    endif()
    message(STATUS "OpenSWMM Engine: GeoPackage I/O library enabled (built-in plugin)")
else()
    message(STATUS "OpenSWMM Engine: GeoPackage I/O disabled (set -DOPENSWMM_WITH_GEOPACKAGE=ON to enable)")
endif()
