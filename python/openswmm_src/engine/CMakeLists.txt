# CMake configuration for openswmm.engine Cython bindings
#
# Builds one Cython extension per .pyx file that wraps the new C API
# (openswmm_engine).  Each extension is a Python MODULE library linked
# against openswmm::engine.
#
# CMake target names are prefixed with "engine_" to avoid collisions with
# identically-named modules in openswmm.legacy.engine (e.g. both have
# _solver.pyx).  The OUTPUT_NAME property ensures the compiled .so/.pyd
# file is still named correctly for Python import.

# ---- helper: add one Cython extension ----
function(add_engine_cython_module name pyx_file)
    set(target "engine_${name}")

    # Build the Cython-generated .cxx in a unique subdirectory so its
    # filename matches the module name (Cython derives PyInit_<name> from
    # the output filename stem).  This avoids collisions with identically-
    # named legacy modules (e.g. both packages have _solver.pyx) while
    # ensuring the correct PyInit_ symbol is emitted.
    set(_pyx_abs "${CMAKE_CURRENT_SOURCE_DIR}/${pyx_file}")
    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/engine_cython")
    file(MAKE_DIRECTORY ${_gen_dir})
    set(_cxx_out "${_gen_dir}/${name}.cxx")

    add_custom_command(
        OUTPUT  ${_cxx_out}
        COMMAND ${CYTHON_EXECUTABLE}
                --cplus
                --include-dir ${CMAKE_CURRENT_SOURCE_DIR}
                -3
                ${_pyx_abs}
                --output-file ${_cxx_out}
        DEPENDS ${_pyx_abs}
        COMMENT "Cythonizing ${pyx_file} (module ${name})"
        VERBATIM
    )

    add_library(${target} MODULE ${_cxx_out})

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${name}
        CXX_VISIBILITY_PRESET default
        C_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF
    )

    # Python extension entry points (PyInit_*) must remain exported even when
    # the global preset enables -fvisibility=hidden.
    # -fno-lto: GCC/Clang LTO treats PyInit_* as dead code because nothing
    # inside the module calls it (Python loads it via dlsym).  Disable LTO
    # for Cython wrappers — it provides no measurable benefit here.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -fvisibility=default -fno-lto)
        target_link_options(${target} PRIVATE -fno-lto)
    endif()

    target_link_libraries(${target}
        openswmm::engine
    )

    target_include_directories(${target}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE ${NUMPY_INCLUDE_DIR}
    )

    python_extension_module(${target})

    # scikit-build's python_extension_module() generates an export list based
    # on the TARGET name (engine_<name>), but Cython emits PyInit_<name>
    # (from the output filename).  The mismatch causes PyInit_<name> to be
    # localized on Linux (version script) and missing on Windows (/EXPORT).
    # Override the link flags to export the correct symbol on each platform.
    if(MSVC)
        set_target_properties(${target} PROPERTIES LINK_FLAGS
            "/EXPORT:PyInit_${name}"
        )
    elseif(UNIX AND NOT APPLE)
        set(_export_map "${CMAKE_CURRENT_BINARY_DIR}/${name}_exports.map")
        file(WRITE ${_export_map} "{ global: PyInit_${name}; local: *; };\n")
        set_target_properties(${target} PROPERTIES LINK_FLAGS
            "-Wl,--version-script=${_export_map}"
        )
    endif()

    # RPATH so the extension can find the shared engine library at runtime
    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH   "@loader_path;@rpath;@loader_path/../../../../.."
            INSTALL_RPATH "@loader_path;@rpath;@loader_path/../../../../.."
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    elseif(UNIX)
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH   "$ORIGIN;$ORIGIN/../../../../.."
            INSTALL_RPATH "$ORIGIN;$ORIGIN/../../../../.."
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endif()

    install(TARGETS ${target} LIBRARY DESTINATION openswmm/engine)
endfunction()

# ---- extensions ----
add_engine_cython_module(_solver         _solver.pyx)
add_engine_cython_module(_model          _model.pyx)
add_engine_cython_module(_nodes          _nodes.pyx)
add_engine_cython_module(_links          _links.pyx)
add_engine_cython_module(_subcatchments  _subcatchments.pyx)
add_engine_cython_module(_gages          _gages.pyx)
add_engine_cython_module(_hotstart       _hotstart.pyx)
add_engine_cython_module(_massbalance    _massbalance.pyx)
add_engine_cython_module(_pollutants     _pollutants.pyx)
add_engine_cython_module(_tables         _tables.pyx)
add_engine_cython_module(_inflows        _inflows.pyx)
add_engine_cython_module(_controls       _controls.pyx)
add_engine_cython_module(_infrastructure _infrastructure.pyx)
add_engine_cython_module(_quality        _quality.pyx)
add_engine_cython_module(_statistics     _statistics.pyx)
add_engine_cython_module(_output_reader  _output_reader.pyx)
add_engine_cython_module(_spatial        _spatial.pyx)
add_engine_cython_module(_forcing        _forcing.pyx)
