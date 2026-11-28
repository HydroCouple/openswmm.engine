"""Explicitly selected tests for a build with native optional features off."""
import pytest
import openswmm.engine as engine


def test_minimal_package_exports():
    assert not engine.HAS_2D
    assert not engine.HAS_GEOPACKAGE
    namespace = {}
    exec('from openswmm.engine import *', namespace)
    assert 'MeshSnow' not in namespace
    assert 'Snow' in namespace
    assert 'Checkpoints' in namespace
    assert 'Surface2D' not in namespace
    assert 'Groundwater' not in namespace
    assert 'SurfaceQuality' not in namespace
    assert 'GeoPackage' not in namespace
    assert 'Results' in namespace
    assert 'Output2DReader' in namespace
    assert not engine.Output2DReader.available()
    with pytest.raises(engine.PluginError):
        engine.Output2DReader('unused.h5')
    solver = engine.Solver()
    try:
        solver.create()
        assert solver.handle
        with pytest.raises(ImportError):
            _ = solver.surface2d
        assert solver.thread_info.logical_cpus >= 1
    finally:
        solver.destroy()


def test_named_binary_results_without_optional_features():
    from tests._paths import EXAMPLE_OUTPUT_FILE
    with engine.Results.open(EXAMPLE_OUTPUT_FILE) as result:
        assert result.surface is None
        assert len(result.nodes) > 0
        assert result.nodes[0].depth.read().shape == (result.output.period_count,)
