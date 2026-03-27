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

    add_cython_target(${target} ${pyx_file} CXX PY3)
    add_library(${target} MODULE ${${target}})

    # The Python module file must be named after the original module, not
    # the prefixed CMake target.
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ${name})

    target_link_libraries(${target}
        openswmm::engine
    )

    target_include_directories(${target}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE ${NUMPY_INCLUDE_DIR}
    )

    python_extension_module(${target})

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
