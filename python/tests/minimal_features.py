"""Explicitly selected tests for a build with native optional features off."""
import pytest
import openswmm.engine as engine


def test_minimal_package_exports():
    assert not engine.HAS_2D
    assert not engine.HAS_GEOPACKAGE
    namespace = {}
    exec('from openswmm.engine import *', namespace)
    assert 'Surface2D' not in namespace
    assert 'Groundwater' not in namespace
    assert 'SurfaceQuality' not in namespace
    assert 'GeoPackage' not in namespace
    solver = engine.Solver()
    try:
        solver.create()
        assert solver.handle
        with pytest.raises(ImportError):
            _ = solver.surface2d
        assert solver.thread_info.logical_cpus >= 1
    finally:
        solver.destroy()
