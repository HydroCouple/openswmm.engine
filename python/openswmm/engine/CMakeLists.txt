# CMake configuration for openswmm.engine Cython bindings
#
# Builds one Cython extension per .pyx file that wraps the new C API
# (openswmm_engine).  Each extension is a Python MODULE library linked
# against openswmm::engine.

# ---- helper: add one Cython extension ----
function(add_engine_cython_module name pyx_file)
    add_cython_target(${name} ${pyx_file} CXX PY3)
    add_library(${name} MODULE ${${name}})

    target_link_libraries(${name}
        openswmm::engine
    )

    target_include_directories(${name}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )

    python_extension_module(${name})

    # RPATH so the extension can find the shared engine library at runtime
    if(APPLE)
        set_target_properties(${name} PROPERTIES
            BUILD_RPATH   "@loader_path;@rpath;@loader_path/../../../../.."
            INSTALL_RPATH "@loader_path;@rpath;@loader_path/../../../../.."
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    elseif(UNIX)
        set_target_properties(${name} PROPERTIES
            BUILD_RPATH   "$ORIGIN;$ORIGIN/../../../../.."
            INSTALL_RPATH "$ORIGIN;$ORIGIN/../../../../.."
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endif()

    install(TARGETS ${name} LIBRARY DESTINATION openswmm/engine)
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
add_engine_cython_module(_forcing       _forcing.pyx)
