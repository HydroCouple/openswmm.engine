#
# CMakeLists.txt - CMake configuration file for OpenSWMM Engine
#
# Project:     openswmm.engine
# Description: Open Storm Water Management Model Engine
# Version:     6.0.0-alpha.1 (refactored engine) / 6.0.0-beta.1 (legacy engine wrapper)
#
# This is the root CMake file for the openswmm.engine project which contains:
#   - openswmm_legacy_engine : Original EPA SWMM 5.x C solver (preserved unmodified)
#   - openswmm_legacy_output : Original EPA SWMM 5.x binary output reader
#   - openswmm_engine        : New refactored C++20 engine with plugin I/O
#   - openswmm_plugin_sdk    : Header-only plugin development kit
#
# Build system: CMake 3.21+ with vcpkg for dependency management
# Test framework: Google Test + Google Benchmark
#
# See docs/MASTER_IMPLEMENTATION_PLAN.md for the full refactoring roadmap.
#

cmake_minimum_required(VERSION 3.21...4.2)

# Prevent in-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there.")
endif()

# Modern project declaration with metadata
project(
    openswmm
    VERSION 6.0.0
    DESCRIPTION "Open Storm Water Management Model Engine"
    HOMEPAGE_URL "https://hydrocouple.org/projects/openswmm"
    LANGUAGES C CXX
)

# Pre-release label for the refactored engine (e.g. "alpha.1", "beta.1", "rc.1"). Leave empty for a final release.
set(OPENSWMM_PRERELEASE "alpha.2" CACHE STRING "Pre-release tag for the refactored engine (e.g. alpha.1, beta.1, rc.1). Empty for a final release.")

# Pre-release label for the legacy engine wrapper. Advances to beta ahead of the refactored engine.
set(OPENSWMM_LEGACY_PRERELEASE "beta.1" CACHE STRING "Pre-release tag for the legacy engine wrapper (e.g. beta.1, rc.1). Empty for a final release.")

if(OPENSWMM_PRERELEASE)
    set(OPENSWMM_FULL_VERSION "${PROJECT_VERSION}-${OPENSWMM_PRERELEASE}")
else()
    set(OPENSWMM_FULL_VERSION "${PROJECT_VERSION}")
endif()

if(OPENSWMM_LEGACY_PRERELEASE)
    set(OPENSWMM_LEGACY_FULL_VERSION "${PROJECT_VERSION}-${OPENSWMM_LEGACY_PRERELEASE}")
else()
    set(OPENSWMM_LEGACY_FULL_VERSION "${PROJECT_VERSION}")
endif()

message(STATUS "OpenSWMM Engine version (refactored): ${OPENSWMM_FULL_VERSION}")
message(STATUS "OpenSWMM Engine version (legacy):     ${OPENSWMM_LEGACY_FULL_VERSION}")


# Modern build type handling
# Only default to Release when no build type was supplied (via preset, -D, or
# parent project). Using FORCE here unconditionally would clobber an explicit
# CMAKE_BUILD_TYPE=Debug from a preset and silently mix Debug CMAKE_C_FLAGS
# with the default Release -O3 -DNDEBUG flag set.
get_property(isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT isMultiConfig)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Essential global settings
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Modern feature options with namespacing
option(OPENSWMM_BUILD_TESTS       "Build all tests (unit + regression). Convenience flag." OFF)
option(OPENSWMM_BUILD_UNIT_TESTS  "Build unit tests only" OFF)
option(OPENSWMM_BUILD_REGRESSION_TESTS "Build regression tests only" OFF)
option(OPENSWMM_BUILD_BENCHMARKS  "Build Google Benchmark performance tests" OFF)
option(OPENSWMM_INSTALL           "Install openswmm libraries" ON)
option(OPENSWMM_BUILD_PYTHON      "Build Python bindings" OFF)
option(OPENSWMM_WITH_GEOPACKAGE   "Build GeoPackage I/O library (requires sqlite3)" ON)
option(OPENSWMM_BUILD_2D          "Build optional 2D surface routing module (requires SUNDIALS)" ON)
option(OPENSWMM_BUILD_GPU_PLUGIN  "Build the optional GPU/Kokkos surface-solver plugin (standalone, dlopen()ed at runtime — never linked into the core). OFF by default so the base build stays Kokkos-free and portable. Enable with the `gpu`/`gpu-cuda`/`gpu-hip`/`gpu-sycl` vcpkg feature and select the backend via OPENSWMM_GPU_BACKEND. When the omp plugin is installed, runtime `auto` selects it (OpenMP becomes the default acceleration where present)." OFF)

# OPENSWMM_BUILD_TESTS is a convenience flag that enables both unit and regression
if(OPENSWMM_BUILD_TESTS)
    set(OPENSWMM_BUILD_UNIT_TESTS ON CACHE BOOL "" FORCE)
    set(OPENSWMM_BUILD_REGRESSION_TESTS ON CACHE BOOL "" FORCE)
endif()

# Include essential modules
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(GenerateExportHeader)

# ---- Runtime-dependency bundling ----------------------------------------
# Regexes shared by every install(RUNTIME_DEPENDENCY_SET …) call to keep the
# OS-provided libraries out of the package (bundling libc / libGL.so.1 /
# opengl32.dll would break dispatch on the target machine and is forbidden
# by Apple's bundle rules). Anything NOT matched here — SUNDIALS, HDF5,
# libomp, sqlite3, and any vcpkg-provided OpenGL-stack wrappers (GLEW,
# GLFW, ANGLE, glbinding) — will be copied next to the executable.
set(OPENSWMM_RUNTIME_DEP_PRE_EXCLUDES
    # Windows OS contract DLLs (MSVC redists not in this list — we bundle them)
    "api-ms-.*" "ext-ms-.*"
    # Windows system DLLs by basename. These get pulled in transitively
    # by Win32 API surface (shell, ACL UI, networking, theming, etc.) and
    # must NOT be bundled — Windows always provides them. Critically,
    # excluding them via POST is too late: one bundle pass copies them
    # into the staging bin/ dir, the next pass then finds them in TWO
    # places (system32 + staging/bin) and file(GET_RUNTIME_DEPENDENCIES)
    # errors with "Multiple conflicting paths" before POST filters run.
    # Add to this list when a new system DLL surfaces in CI warnings.
    # NB1: must be ONE quoted string — CMake parses each "" as a separate
    #      list element, so splitting across lines breaks the alternation.
    # NB2: char-class form is intentional. CMake regex is case-SENSITIVE,
    #      and PE import tables typically store system DLL names UPPERCASE
    #      (e.g. ACLUI.dll, KERNEL32.dll). A plain lowercase alternation
    #      silently fails to match — verified by cmake -P testing.
    "^([Aa][Cc][Ll][Uu][Ii]|[Aa][Dd][Vv][Aa][Pp][Ii]32|[Bb][Cc][Rr][Yy][Pp][Tt]|[Bb][Cc][Rr][Yy][Pp][Tt][Pp][Rr][Ii][Mm][Ii][Tt][Ii][Vv][Ee][Ss]|[Cc][Oo][Mm][Bb][Aa][Ss][Ee]|[Cc][Oo][Mm][Cc][Tt][Ll]32|[Cc][Oo][Mm][Dd][Ll][Gg]32|[Cc][Rr][Yy][Pp][Tt]32|[Cc][Rr][Yy][Pp][Tt][Bb][Aa][Ss][Ee]|[Cc][Rr][Yy][Pp][Tt][Ss][Pp]|[Dd][Bb][Gg][Cc][Oo][Rr][Ee]|[Dd][Bb][Gg][Hh][Ee][Ll][Pp]|[Dd][Nn][Ss][Aa][Pp][Ii]|[Dd][Ww][Mm][Aa][Pp][Ii]|[Dd][Ww][Rr][Ii][Tt][Ee]|[Ff][Ww][Pp][Uu][Cc][Ll][Nn][Tt]|[Gg][Dd][Ii]32|[Gg][Dd][Ii]32[Ff][Uu][Ll][Ll]|[Ii][Mm][Aa][Gg][Ee][Hh][Ll][Pp]|[Ii][Mm][Mm]32|[Ii][Pp][Hh][Ll][Pp][Aa][Pp][Ii]|[Kk][Ee][Rr][Nn][Ee][Ll]32|[Kk][Ee][Rr][Nn][Ee][Ll][Bb][Aa][Ss][Ee]|[Mm][Pp][Rr]|[Mm][Ss][Aa][Ss][Nn]1|[Mm][Ss][Cc][Oo][Rr][Ee][Ee]|[Mm][Ss][Ii]|[Mm][Ss][Vv][Cc][Pp]_[Ww][Ii][Nn]|[Mm][Ss][Vv][Cc][Rr][Tt]|[Mm][Ss][Ww][Ss][Oo][Cc][Kk]|[Nn][Ee][Tt][Aa][Pp][Ii]32|[Nn][Ee][Tt][Uu][Tt][Ii][Ll][Ss]|[Nn][Oo][Rr][Mm][Aa][Ll][Ii][Zz]|[Nn][Tt][Dd][Ll][Ll]|[Nn][Tt][Mm][Aa][Rr][Tt][Aa]|[Oo][Ll][Ee]32|[Oo][Ll][Ee][Aa][Cc][Cc]|[Oo][Ll][Ee][Aa][Uu][Tt]32|[Pp][Oo][Ww][Rr][Pp][Rr][Oo][Ff]|[Pp][Rr][Oo][Ff][Aa][Pp][Ii]|[Pp][Rr][Oo][Pp][Ss][Yy][Ss]|[Pp][Ss][Aa][Pp][Ii]|[Rr][Pp][Cc][Rr][Tt]4|[Ss][Aa][Mm][Cc][Ll][Ii]|[Ss][Ee][Cc][Hh][Oo][Ss][Tt]|[Ss][Ee][Cc][Uu][Rr]32|[Ss][Ee][Tt][Uu][Pp][Aa][Pp][Ii]|[Ss][Ff][Cc]|[Ss][Ff][Cc]_[Oo][Ss]|[Ss][Hh][Cc][Oo][Rr][Ee]|[Ss][Hh][Ee][Ll][Ll]32|[Ss][Hh][Ll][Ww][Aa][Pp][Ii]|[Ss][Rr][Vv][Cc][Ll][Ii]|[Uu][Ss][Ee][Rr]32|[Uu][Ss][Ee][Rr][Ee][Nn][Vv]|[Uu][Ss][Pp]10|[Uu][Xx][Tt][Hh][Ee][Mm][Ee]|[Vv][Ee][Rr][Ss][Ii][Oo][Nn]|[Ww][Ee][Rr]|[Ww][Ii][Nn]32[Uu]|[Ww][Ii][Nn][Ii][Nn][Ee][Tt]|[Ww][Ii][Nn][Mm][Mm]|[Ww][Ii][Nn][Nn][Ss][Ii]|[Ww][Ii][Nn][Tt][Rr][Uu][Ss][Tt]|[Ww][Kk][Ss][Cc][Ll][Ii]|[Ww][Ll][Dd][Aa][Pp]32|[Ww][Ss]2_32|[Ww][Ss][Oo][Cc][Kk]32|[Ww][Tt][Ss][Aa][Pp][Ii]32|[Zz][Ll][Ii][Bb][Ww][Aa][Pp][Ii])\\.[Dd][Ll][Ll]"
    # macOS / Linux system search paths
    "^/usr/lib/.*" "^/lib/.*" "^/lib64/.*"
    "^/System/Library/.*" "^/usr/lib/system/.*"
    # Unix system libs by basename
    "^libc\\..*" "^libm\\..*" "^libdl\\..*"
    "^libpthread\\..*" "^librt\\..*" "^libutil\\..*"
    "^libresolv\\..*" "^libnsl\\..*"
    "^libstdc\\+\\+\\..*" "^libgcc_s\\..*"
    # OpenGL loader / driver dispatch — must come from the OS, never bundled.
    # vcpkg's GLEW/GLFW/ANGLE wrappers do NOT match these (different names).
    # `$` end-anchor is dropped on purpose: CMake's variable-reference parser
    # (CMP0010) treats stray `$` characters as bad references when these
    # regex strings get interpolated into install(CODE "...") bodies.
    "^libGL\\..*" "^libGLX\\..*" "^libEGL\\..*" "^libGLdispatch\\..*"
    "^opengl32\\.dll" "^glu32\\.dll"
)

set(OPENSWMM_RUNTIME_DEP_POST_EXCLUDES
    ".*[/\\\\][Ss]ystem32[/\\\\].*\\.dll"
    ".*[/\\\\][Ss]ys[Ww][Oo][Ww]64[/\\\\].*\\.dll"
)

# Install runtime files for a list of imported targets into the given
# destination. Skips targets that are not defined in the current
# configuration, which lets callers list optional deps unconditionally.
#
# `IMPORTED_RUNTIME_ARTIFACTS` requires SHARED_LIBRARY / MODULE_LIBRARY /
# EXECUTABLE targets. We allowlist those explicitly and skip everything
# else — covers STATIC_LIBRARY (vcpkg static triplets), INTERFACE_LIBRARY
# (header-only deps), OBJECT_LIBRARY, and notably UNKNOWN_LIBRARY (the
# type reported by `FindSQLite3.cmake` and other classic Find modules
# that don't classify the artifact). Without this filter, configure
# aborts with "given target X which is not an executable, library, or
# module."
function(openswmm_install_runtime_deps DESTINATION)
    foreach(_target IN LISTS ARGN)
        if(NOT TARGET ${_target})
            continue()
        endif()
        get_target_property(_type ${_target} TYPE)
        if(NOT (_type STREQUAL "SHARED_LIBRARY" OR
                _type STREQUAL "MODULE_LIBRARY" OR
                _type STREQUAL "EXECUTABLE"))
            continue()
        endif()
        install(IMPORTED_RUNTIME_ARTIFACTS ${_target}
            RUNTIME DESTINATION ${DESTINATION}
            LIBRARY DESTINATION ${DESTINATION}
        )
    endforeach()
endfunction()

# Bundle the full runtime-dependency closure of an installed executable into
# the given subdir of CMAKE_INSTALL_PREFIX. Runs at install time as an
# install(SCRIPT …) — configure_file produces a per-target script from
# cmake/BundleRuntimeDeps.cmake.in. The template approach avoids escape-
# hell with backslashes in the regex strings inside install(CODE "…").
#
# Why a custom script instead of install(TARGETS … RUNTIME_DEPENDENCY_SET …):
# when openswmm_engine is itself installed by another install(TARGETS …)
# rule, CMake auto-adds the build-tree engine dylib to the set's
# POST_EXCLUDE_FILES_STRICT. That strict exclusion prunes the dep walk
# *through* the engine, so libomp / SUNDIALS / HDF5 — which are deps of
# the engine, not of the CLI — never appear in the resolved set. Probing
# the installed exe via the template sidesteps that auto-exclusion entirely.
function(openswmm_bundle_runtime_deps TARGET_NAME SUBDIR)
    # Resolve the executable's installed file name at configure time so the
    # template substitution baked into the script is a plain path, not a
    # genex (configure_file doesn't evaluate $<…>).
    get_target_property(_out_name ${TARGET_NAME} OUTPUT_NAME)
    if(NOT _out_name)
        set(_out_name "${TARGET_NAME}")
    endif()
    if(WIN32)
        set(_file_name "${_out_name}.exe")
    else()
        set(_file_name "${_out_name}")
    endif()

    set(OPENSWMM_BUNDLE_TARGET_NAME      "${TARGET_NAME}")
    set(OPENSWMM_BUNDLE_TARGET_FILE_NAME "${_file_name}")
    set(OPENSWMM_BUNDLE_SUBDIR           "${SUBDIR}")
    set(OPENSWMM_BUNDLE_PRE_EXCLUDES     "[==[${OPENSWMM_RUNTIME_DEP_PRE_EXCLUDES}]==]")
    set(OPENSWMM_BUNDLE_POST_EXCLUDES    "[==[${OPENSWMM_RUNTIME_DEP_POST_EXCLUDES}]==]")

    # Extra DIRECTORIES that file(GET_RUNTIME_DEPENDENCIES) should search
    # when resolving transitive dependencies of the executable. Critically
    # this is where we point at vcpkg's installed/<triplet>/bin so SUNDIALS,
    # HDF5, sqlite3, etc. (linked by openswmm_engine) get found and copied
    # into the package. Without this, those DLLs show up in the bundle's
    # unresolved-deps warnings on Windows and never get included in the
    # cpack zip. Linux/macOS usually resolve them via rpath, but the extra
    # dir is harmless on those platforms.
    set(OPENSWMM_BUNDLE_EXTRA_DIRS "")
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND OPENSWMM_BUNDLE_EXTRA_DIRS
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    elseif(DEFINED ENV{VCPKG_ROOT})
        # Fallback: use the env-var-rooted classic-mode layout.
        set(_triplet "${VCPKG_TARGET_TRIPLET}")
        if(NOT _triplet AND WIN32)
            set(_triplet "x64-windows")
        endif()
        if(_triplet)
            list(APPEND OPENSWMM_BUNDLE_EXTRA_DIRS
                "$ENV{VCPKG_ROOT}/installed/${_triplet}/bin")
        endif()
    endif()

    # Use PROJECT_SOURCE_DIR (not CMAKE_SOURCE_DIR): the engine has its own
    # project() at line 28, so this resolves to the engine root even when the
    # engine is pulled in via add_subdirectory(..) from python/CMakeLists.txt.
    # With CMAKE_SOURCE_DIR, the lookup would (incorrectly) start at the
    # top-level CMakeLists — which under scikit-build-core is python/.
    set(_script_in  "${PROJECT_SOURCE_DIR}/cmake/BundleRuntimeDeps.cmake.in")
    set(_script_out "${CMAKE_CURRENT_BINARY_DIR}/BundleRuntimeDeps-${TARGET_NAME}.cmake")
    configure_file("${_script_in}" "${_script_out}" @ONLY)
    install(SCRIPT "${_script_out}")
endfunction()

# ---- RPATH / runtime-library search-path policy -------------------------
# Separate build-tree and install rpaths so developers can run binaries
# directly from the build dir without extra environment variables, while
# installed binaries locate their dylibs via relative paths.
#
# CMAKE_INSTALL_RPATH_USE_LINK_PATH appends every linked-library directory
# that is not an implicit system path to each target's install rpath
# automatically — this ensures editable Python installs and standalone
# cmake --install both find the engine dylibs without manual DYLD/LD setup.
#
# Per-target INSTALL_RPATH settings (wheel layout etc.) override the global
# CMAKE_INSTALL_RPATH value but are still AUGMENTED by USE_LINK_PATH.
#
# Windows has no rpath; DLL resolution uses PATH / the executable directory.
if(NOT WIN32)
    set(CMAKE_SKIP_BUILD_RPATH            FALSE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH    FALSE)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    if(APPLE)
        set(CMAKE_MACOSX_RPATH ON)
        # Executables/libs that do NOT set their own INSTALL_RPATH use this.
        # @loader_path      — same directory as the binary (co-located dylibs)
        # @loader_path/../lib — standard prefix layout (bin/ → ../lib/)
        set(CMAKE_INSTALL_RPATH "@loader_path;@loader_path/../lib")
    else()
        set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
    endif()
endif()

# Build information (improved)
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OPENSWMM_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RESULT
    )
    if(NOT GIT_RESULT EQUAL 0)
        set(OPENSWMM_GIT_HASH "unknown")
    endif()
else()
    set(OPENSWMM_GIT_HASH "unknown")
endif()

# Create interface library for global headers
add_library(openswmm_common INTERFACE)
add_library(openswmm::common ALIAS openswmm_common)

# Configure global version header
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/version.h
)

# Set include directories for the common interface
target_include_directories(openswmm_common
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Install generated headers
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/include/version.h
    DESTINATION
        ${CMAKE_INSTALL_INCLUDEDIR}/openswmm
)

# Install all public headers
install(
    DIRECTORY include/openswmm
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)

# Add testing subdirectories conditionally.
# Note: testing/benchmarks subdirs are added AFTER add_subdirectory(src) below
# so that imported third-party targets (OpenMP::OpenMP_CXX, SUNDIALS::cvode,
# hdf5::hdf5-shared, sqlite3) created during the engine configure are visible
# to the test-tree dependency-staging helper (openswmm_stage_test_runtime_deps).
# CMake permits forward target references in target_link_libraries, so this
# reorder is safe.
if(OPENSWMM_BUILD_UNIT_TESTS OR OPENSWMM_BUILD_REGRESSION_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
endif()

# Modern package configuration
if(OPENSWMM_INSTALL OR OPENSWMMCORE_INSTALL)
    configure_package_config_file(
        cmake/OpenSWMMEngineConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/OpenSWMMEngineConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OpenSWMMEngine
        PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/OpenSWMMEngineConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion  # ✅ More appropriate than AnyNewerVersion
    )

    install(
        FILES
            ${CMAKE_CURRENT_BINARY_DIR}/OpenSWMMEngineConfig.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/OpenSWMMEngineConfigVersion.cmake
        DESTINATION
            ${CMAKE_INSTALL_LIBDIR}/cmake/OpenSWMMEngine
    )

    install(
        EXPORT OpenSWMMEngineTargets
        FILE OpenSWMMEngineTargets.cmake
        NAMESPACE OpenSWMMEngine::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/OpenSWMMEngine
    )

    install(
        TARGETS openswmm_common
        EXPORT OpenSWMMEngineTargets
    )
endif()

# Add subdirectories
add_subdirectory(src)

# Test / benchmark subdirs are added here (after src/) so the staging helper
# sees the imported runtime targets created during the engine configure.
if(OPENSWMM_BUILD_UNIT_TESTS OR OPENSWMM_BUILD_REGRESSION_TESTS)
    add_subdirectory(tests)
endif()

if(OPENSWMM_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
    add_subdirectory(tests/benchmarks)
endif()

# Optional Python bindings (Cython extensions compiled via scikit-build).
# Typically invoked indirectly by `pip install ./python`, but can also be
# enabled as part of a top-level CMake build:
#   cmake -DOPENSWMM_BUILD_PYTHON=ON ..
if(OPENSWMM_BUILD_PYTHON)
    # Discover Python and extension-building dependencies without making them
    # hard requirements for a plain C/C++ build. If any of these packages are
    # missing, the Python bindings will be skipped with a warning instead of
    # causing CMake configuration to fail.
    find_package(Python3 QUIET COMPONENTS Interpreter Development.Module)
    find_package(PythonExtensions QUIET)
    find_package(Cython QUIET)

    if(Python3_FOUND AND PythonExtensions_FOUND AND Cython_FOUND)
        add_subdirectory(python/openswmm)
    else()
        message(WARNING
            "OPENSWMM_BUILD_PYTHON is enabled, but required Python build "
            "dependencies (Python3, PythonExtensions, Cython) were not found. "
            "Skipping build of Python bindings."
        )
    endif()
endif()

# Simplified CPack
set(CPACK_PACKAGE_NAME "openswmm-engine")
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "openswmm-engine-${OPENSWMM_FULL_VERSION}")
set(CPACK_PACKAGE_VENDOR "https://hydrocouple.org")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
include(CPack)
