# ============================================================================
# openswmm.legacy.output — Cython bindings for the SWMM 5.x output reader.
# ============================================================================

add_cython_extension(
    NAME        _output
    TARGET      legacy_output__output
    SOURCE      _output.pyx
    INSTALL_DIR openswmm/legacy/output
    LIBS        openswmm::common openswmm::legacy::output
)
