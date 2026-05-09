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
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---- Link against common interface ----
target_link_libraries(openswmm_engine
    PUBLIC
        openswmm_common
    PRIVATE
        $<$<NOT:$<BOOL:$<CXX_COMPILER_ID:MSVC>>>:m>
)

# ---- Threads (for IOThread) ----
find_package(Threads REQUIRED)
target_link_libraries(openswmm_engine PRIVATE Threads::Threads)

# ---- OpenMP (for parallel solver loops) ----
# Uses custom FindOpenMP.cmake for Apple Silicon Homebrew libomp support.
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(${PROJECT_SOURCE_DIR}/cmake/FindOpenMP.cmake)

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

# ---- Link-time optimization (IPO/LTO) ----
# Enables cross-translation-unit inlining and dead-code elimination. Critical
# for performance when hot-path functions (fastmath::pow4_3, SIMD helpers,
# per-conduit dispatch) cross TU boundaries. Guarded by compiler support check
# to avoid failures on toolchains that lack bitcode linkers.
if(NOT DEFINED OPENSWMM_ENABLE_LTO)
    set(OPENSWMM_ENABLE_LTO ON CACHE BOOL "Enable interprocedural / link-time optimization for Release builds")
endif()
if(OPENSWMM_ENABLE_LTO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
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
# The engine dylib is installed to openswmm/engine/ in the wheel.
# External bundled deps (e.g. libomp, placed by delocate/auditwheel) go into
# openswmm/.dylibs/ — one level above the engine directory.
# @loader_path/../.dylibs  ==  openswmm/engine/../.dylibs  ==  openswmm/.dylibs/
if(APPLE)
    set(LIB_ROOT "@loader_path/../.dylibs")
elseif(UNIX)
    set(LIB_ROOT "$ORIGIN/../.dylibs")
else()
    # Windows: DLL loading uses PATH / LoadLibraryEx; no RPATH concept.
    set(LIB_ROOT "")
endif()

set_target_properties(openswmm_engine PROPERTIES
    MACOSX_RPATH         TRUE
    INSTALL_RPATH        "${LIB_ROOT}"
    BUILD_RPATH          "${LIB_ROOT}"
    BUILD_WITH_INSTALL_RPATH TRUE
)

# ---- Install ----
install(
    TARGETS openswmm_engine
    EXPORT  OpenSWMMEngineTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
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

    message(STATUS "OpenSWMM Engine: internal STATIC library enabled for MSVC unit tests")
endif()

# ---- Optional 2D Surface Routing Module ----
if(OPENSWMM_BUILD_2D)
    # Collect 2D source files (non-CVODE sources are always compiled)
    file(GLOB_RECURSE OPENSWMM_2D_SOURCES
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        2d/data/*.cpp
        2d/mesh/*.cpp
        2d/solver/SurfaceFluxCalculator.cpp
        2d/coupling/*.cpp
        2d/input/*.cpp
        2d/SurfaceRouter2D.cpp
        2d/api/*.cpp
    )

    # CVODE wrapper requires SUNDIALS
    list(APPEND OPENSWMM_2D_SOURCES 2d/solver/CvodeSurfaceSolver.cpp)

    target_sources(openswmm_engine PRIVATE ${OPENSWMM_2D_SOURCES})

    # Find SUNDIALS (CVODE) via vcpkg or system install
    find_package(SUNDIALS REQUIRED COMPONENTS cvode)
    target_link_libraries(openswmm_engine PRIVATE SUNDIALS::cvode)

    target_compile_definitions(openswmm_engine PUBLIC OPENSWMM_HAS_2D=1)

    # Also add to the internal static library for MSVC unit tests
    if(TARGET openswmm_engine_internal)
        target_sources(openswmm_engine_internal PRIVATE ${OPENSWMM_2D_SOURCES})
        target_link_libraries(openswmm_engine_internal PRIVATE SUNDIALS::cvode)
        target_compile_definitions(openswmm_engine_internal PUBLIC OPENSWMM_HAS_2D=1)
    endif()

    message(STATUS "OpenSWMM Engine: 2D surface routing module enabled (SUNDIALS/CVODE)")
else()
    message(STATUS "OpenSWMM Engine: 2D surface routing disabled (set -DOPENSWMM_BUILD_2D=ON to enable)")
endif()

# ---- Optional GeoPackage I/O library ----
if(OPENSWMM_WITH_GEOPACKAGE)
    add_subdirectory(input/geopackage)
    message(STATUS "OpenSWMM Engine: GeoPackage I/O library enabled")
else()
    message(STATUS "OpenSWMM Engine: GeoPackage I/O disabled (set -DOPENSWMM_WITH_GEOPACKAGE=ON to enable)")
endif()
