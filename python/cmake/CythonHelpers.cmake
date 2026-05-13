# ============================================================================
# CythonHelpers.cmake
#
# Single source of truth for how every openswmm Cython extension is built:
#   - Cython directives are declared once and applied to every module.
#   - Python_add_library(... WITH_SOABI ...) handles the .so/.pyd suffix.
#   - RPATH, visibility, LTO, and PyInit symbol export are uniform.
#   - install() places the artefact into the wheel's package tree.
#
# Required (set by python/CMakeLists.txt before include):
#   OPENSWMM_CYTHON_EXECUTABLE  — path to the cython binary
#   NUMPY_INCLUDE_DIR           — numpy.get_include()
#   Python_EXECUTABLE           — active Python interpreter
# ============================================================================

# ----------------------------------------------------------------------------
# Cython compiler directives applied to every openswmm extension.
#
# Two classes of directives:
#
#   1. SAFE / SEMANTIC-NEUTRAL — applied universally.  These do not change
#      observable behaviour, only Cython output (metadata, language-level
#      string handling).
#
#   2. PERFORMANCE / SEMANTIC-ALTERING — left to per-file `# cython: …`
#      headers.  Examples that we DO NOT set globally because they change
#      runtime behaviour and have caused regressions in this codebase:
#
#        - boundscheck / wraparound: hide IndexError at the C layer.
#        - cdivision:              integer / on negatives rounds toward 0
#                                  instead of −∞ (Python semantics).
#        - initializedcheck:       hides "used-before-assigned" cdef errors.
#        - c_string_type / c_string_encoding: changes whether `char*`
#                                  returns are auto-decoded to str.  Our
#                                  .pyx files do explicit `b.decode("utf-8")`
#                                  on a known-bytes return; flipping the
#                                  default would double-decode and crash.
#
# A `.pyx` file that benefits from one of the perf flags should opt in at
# the file level — that change is local, reviewable, and survives a
# refactor of this helper.
#
# Override at configure time with:
#   -DOPENSWMM_CYTHON_DIRECTIVES="--directive=embedsignature=True;..."
# ----------------------------------------------------------------------------
set(OPENSWMM_CYTHON_DIRECTIVES_DEFAULT
    "--directive=language_level=3str"
    "--directive=embedsignature=True"
)
set(OPENSWMM_CYTHON_DIRECTIVES "${OPENSWMM_CYTHON_DIRECTIVES_DEFAULT}"
    CACHE STRING "Cython compiler directives applied to all openswmm extensions")
mark_as_advanced(OPENSWMM_CYTHON_DIRECTIVES)

# ----------------------------------------------------------------------------
# add_cython_extension(
#     NAME         <python_module_name>      # e.g. _solver, _nodes  (required)
#     SOURCE       <path/to/file.pyx>        # relative to caller    (required)
#     INSTALL_DIR  <wheel/relative/dir>      # e.g. openswmm/engine  (required)
#     TARGET       <cmake_target_name>       # default: NAME         (optional)
#     LIBS         <lib1> [<lib2> ...]       # link dependencies     (optional)
#     INCLUDE_DIRS <dir1> [<dir2> ...]       # extra include dirs    (optional)
# )
#
# TARGET is needed when two modules share a NAME (we have engine/_solver.pyx
# and legacy/engine/_solver.pyx — same Python name, different packages).
# Pass a unique CMake target name, e.g. TARGET engine__solver.  Cython derives
# PyInit_<name> from the .cxx filename, which is generated from NAME, so the
# correct symbol is emitted regardless of the CMake target name.
# ----------------------------------------------------------------------------
function(add_cython_extension)
    set(_oneValueArgs NAME SOURCE INSTALL_DIR TARGET)
    set(_multiValueArgs LIBS INCLUDE_DIRS)
    cmake_parse_arguments(ACE "" "${_oneValueArgs}" "${_multiValueArgs}" ${ARGN})

    if(NOT ACE_NAME OR NOT ACE_SOURCE OR NOT ACE_INSTALL_DIR)
        message(FATAL_ERROR "add_cython_extension: NAME, SOURCE, INSTALL_DIR are required")
    endif()
    if(NOT ACE_TARGET)
        set(ACE_TARGET "${ACE_NAME}")
    endif()

    set(_pyx_abs "${CMAKE_CURRENT_SOURCE_DIR}/${ACE_SOURCE}")
    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/cython_gen/${ACE_TARGET}")
    file(MAKE_DIRECTORY "${_gen_dir}")

    # Cythonized output filename = ACE_NAME (NOT ACE_TARGET).  Cython derives
    # the PyInit_ symbol from the file stem, so this MUST match the runtime
    # module name to keep import working when CMake target names collide
    # with Python module names across subpackages.
    set(_cxx_out "${_gen_dir}/${ACE_NAME}.cxx")

    # ── Cythonize ─────────────────────────────────────────────────────────
    # OPENSWMM_CYTHON_EXECUTABLE is a CMake list — typically
    # "<python>;-m;cython" — so it expands as `python -m cython ...` here.
    # Quoting the whole variable would break -m argument splitting.
    #
    # Include-dir order matters:
    #   1. OPENSWMM_NUMPY_PATCHED_INCLUDE_DIR — overrides numpy's broken
    #      cython-30.pxd (see cmake/PatchNumpyPxd.cmake).  MUST come first
    #      so its numpy/__init__.cython-30.pxd shadows the install copy.
    #   2. CMAKE_CURRENT_SOURCE_DIR — local .pxd files in the same package.
    set(_cython_inc_args "")
    if(OPENSWMM_NUMPY_PATCHED_INCLUDE_DIR)
        list(APPEND _cython_inc_args
            --include-dir "${OPENSWMM_NUMPY_PATCHED_INCLUDE_DIR}")
    endif()
    list(APPEND _cython_inc_args
        --include-dir "${CMAKE_CURRENT_SOURCE_DIR}")

    add_custom_command(
        OUTPUT  "${_cxx_out}"
        COMMAND ${OPENSWMM_CYTHON_EXECUTABLE}
                --cplus
                ${_cython_inc_args}
                ${OPENSWMM_CYTHON_DIRECTIVES}
                "${_pyx_abs}"
                --output-file "${_cxx_out}"
        DEPENDS "${_pyx_abs}"
        COMMENT "Cythonizing ${ACE_SOURCE} → ${ACE_NAME}.cxx"
        VERBATIM
    )

    # ── Build the extension module ────────────────────────────────────────
    # Python_add_library(... WITH_SOABI) sets the correct suffix
    # (.cpython-313-darwin.so on macOS cp313, .pyd on Windows, etc.).
    Python_add_library("${ACE_TARGET}" MODULE WITH_SOABI "${_cxx_out}")

    set_target_properties("${ACE_TARGET}" PROPERTIES
        OUTPUT_NAME              "${ACE_NAME}"
        CXX_VISIBILITY_PRESET    default
        C_VISIBILITY_PRESET      default
        VISIBILITY_INLINES_HIDDEN OFF
    )

    # PyInit_* must remain exported even when the build globally enables
    # -fvisibility=hidden / LTO.  -fno-lto avoids GCC/Clang stripping the
    # symbol because no in-module call edge reaches it (Python loads it
    # via dlsym).
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options("${ACE_TARGET}" PRIVATE -fvisibility=default -fno-lto)
        target_link_options(   "${ACE_TARGET}" PRIVATE -fno-lto)
    endif()

    if(ACE_LIBS)
        target_link_libraries("${ACE_TARGET}" PRIVATE ${ACE_LIBS})
    endif()

    # ── OpenMP linkage ───────────────────────────────────────────────────
    # The parent project's src/engine/CMakeLists.txt links OpenMP PRIVATE
    # to openswmm_engine, so the dependency does NOT transitively propagate
    # through openswmm::engine to consumers.  Each Cython extension must
    # therefore link OpenMP itself, using the same target the engine uses
    # (created by ../cmake/FindOpenMP.cmake — Apple Silicon and Intel
    # Homebrew prefixes already detected there).
    #
    # This:
    #   - Adds `-Xpreprocessor -fopenmp` to the .cxx compile (Apple clang
    #     requires this form to interpret `#pragma omp` directives).
    #   - Pulls libomp's include directory.
    #   - Adds an explicit DT_NEEDED entry on libomp.dylib so delocate-wheel
    #     bundles it into the wheel even for extensions whose .pyx never
    #     uses prange but whose generated code may reference OMP symbols
    #     pulled in from cimported headers.
    if(TARGET OpenMP::OpenMP_CXX)
        target_link_libraries("${ACE_TARGET}" PRIVATE OpenMP::OpenMP_CXX)
    endif()

    target_include_directories("${ACE_TARGET}" PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${NUMPY_INCLUDE_DIR}"
        ${ACE_INCLUDE_DIRS}
    )

    # ── RPATH + symbol export, per platform ───────────────────────────────
    if(APPLE)
        # Bundled dylibs sit next to the .so; @loader_path is enough.
        set_target_properties("${ACE_TARGET}" PROPERTIES
            BUILD_RPATH              "@loader_path"
            INSTALL_RPATH            "@loader_path"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    elseif(UNIX)
        # Hidden visibility + version script keeps PyInit_<name> exported
        # while everything else is local.
        set(_map "${_gen_dir}/${ACE_NAME}_exports.map")
        file(WRITE "${_map}" "{ global: PyInit_${ACE_NAME}; local: *; };\n")
        target_link_options("${ACE_TARGET}" PRIVATE "-Wl,--version-script=${_map}")
        set_target_properties("${ACE_TARGET}" PROPERTIES
            BUILD_RPATH              "$ORIGIN"
            INSTALL_RPATH            "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    elseif(WIN32)
        # MSVC: explicit /EXPORT for the PyInit symbol.  delvewheel (in CI)
        # bundles transitive DLLs alongside the .pyd; in dev builds the DLL
        # is installed next to the .pyd via install(TARGETS ... RUNTIME).
        set_target_properties("${ACE_TARGET}" PROPERTIES
            LINK_FLAGS "/EXPORT:PyInit_${ACE_NAME}"
        )
    endif()

    # ── Install ──────────────────────────────────────────────────────────
    install(TARGETS "${ACE_TARGET}"
            LIBRARY DESTINATION "${ACE_INSTALL_DIR}"
            RUNTIME DESTINATION "${ACE_INSTALL_DIR}"
    )
endfunction()
