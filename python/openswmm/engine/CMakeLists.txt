# ============================================================================
# openswmm.engine — Cython bindings for the OpenSWMM 6.x C API.
#
# Every extension is built through add_cython_extension() (see
# python/cmake/CythonHelpers.cmake) so directives, RPATH, visibility,
# symbol export, and install rules stay uniform across modules.
#
# CMake target names carry an "engine_" prefix to avoid collisions with the
# identically-named legacy modules (legacy/engine/_solver.pyx, etc.).  The
# helper sets OUTPUT_NAME so the on-disk filename remains the bare module
# name and Python imports resolve correctly.
# ============================================================================

set(_engine_modules
    _solver
    _model
    _edit
    _nodes
    _links
    _subcatchments
    _gages
    _hotstart
    _massbalance
    _pollutants
    _tables
    _inflows
    _controls
    _infrastructure
    _quality
    _statistics
    _output_reader
    _spatial
    _forcing
)

foreach(_mod IN LISTS _engine_modules)
    add_cython_extension(
        NAME        ${_mod}
        TARGET      engine_${_mod}
        SOURCE      ${_mod}.pyx
        INSTALL_DIR openswmm/engine
        LIBS        openswmm::engine
    )
endforeach()

# Optional 2-D module (requires SUNDIALS/CVODE; off by default).
if(OPENSWMM_BUILD_2D)
    add_cython_extension(
        NAME        _2d
        TARGET      engine__2d
        SOURCE      _2d.pyx
        INSTALL_DIR openswmm/engine
        LIBS        openswmm::engine
    )
endif()

# GeoPackage support is optional — built only when the engine target was
# configured with OPENSWMM_WITH_GEOPACKAGE.  When omitted, openswmm.engine
# imports HAS_GEOPACKAGE = False at runtime (see __init__.py).
if(OPENSWMM_WITH_GEOPACKAGE)
    add_cython_extension(
        NAME        _geopackage
        TARGET      engine__geopackage
        SOURCE      _geopackage.pyx
        INSTALL_DIR openswmm/engine
        LIBS        openswmm::engine
    )
endif()
