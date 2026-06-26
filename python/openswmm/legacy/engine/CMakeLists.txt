# ============================================================================
# openswmm.legacy.engine — Cython bindings for the SWMM 5.x solver.
#
# CMake target name disambiguates from openswmm.engine._solver (different
# package, same Python module name).
# ============================================================================

add_cython_extension(
    NAME        _solver
    TARGET      legacy_engine__solver
    SOURCE      _solver.pyx
    INSTALL_DIR openswmm/legacy/engine
    LIBS        openswmm::common openswmm::legacy::engine
)
