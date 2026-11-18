"""Physical cell velocities come from the native API, in independent SI arrays."""
import ctypes as ct

import numpy as np
import pytest

pytest.importorskip("openswmm.engine._2d")

from openswmm.engine import LifecycleError, StaleObjectError
from openswmm.engine import _2d as extension
from tests._capi import load_engine_api
from tests.engine.test_runtime_coupling_r0 import SURFACE_MODEL, _close, _open


@pytest.mark.parametrize("units", ["CMS", "CFS"])
def test_physical_velocities_preserve_state_and_array_ownership(units):
    text = SURFACE_MODEL.replace("FLOW_UNITS CMS", "FLOW_UNITS " + units)
    text = text.replace("MAX_TIMESTEP 5", "MOMENTUM_EQUATION FULL_SWE\nBACKEND CPU\nLTS_TIERS 1\nMAX_TIMESTEP 5")
    text = text.replace("0 1 2 0.03\n0 2 3 0.03", "0 1 2 0.03 0\n0 2 3 0.03 0.01")
    text += "[2D_INITIAL_VELOCITY]\n1 0.3 -0.4\n"
    solver = _open("native_velocities_" + units, text)
    try:
        surface = solver.surface2d
        with pytest.raises(LifecycleError):
            surface.get_velocities()
        solver.initialize()
        vx, vy = surface.get_velocities()
        for values in (vx, vy):
            assert values.shape == (surface.n_cells,)
            assert values.dtype == np.float64
            assert values.flags.c_contiguous and values.flags.owndata
        assert not np.shares_memory(vx, vy)
        np.testing.assert_allclose(vx, [0.0, 0.3], atol=1e-14)
        np.testing.assert_allclose(vy, [0.0, -0.4], atol=1e-14)
        vx[:] = 999.0
        np.testing.assert_allclose(surface.get_velocities()[0], [0.0, 0.3], atol=1e-14)

        solver.start()
        solver.step()
        elapsed = solver.elapsed
        depth, flux = surface.get_depths(), surface.get_edge_flux_bulk()
        vx, vy = surface.get_velocities()
        assert solver.elapsed == elapsed
        np.testing.assert_array_equal(surface.get_depths(), depth)
        np.testing.assert_array_equal(surface.get_edge_flux_bulk(), flux)

        # Compare the Python arrays to a separate native read, including NaNs.
        api = load_engine_api(extension)
        native = api.swmm_2d_get_velocities_bulk
        pointer = ct.POINTER(ct.c_double)
        native.argtypes = [ct.c_void_p, pointer, pointer]
        native.restype = ct.c_int
        expected_x, expected_y = np.empty_like(vx), np.empty_like(vy)
        assert native(solver.handle, expected_x.ctypes.data_as(pointer),
                      expected_y.ctypes.data_as(pointer)) == 0
        np.testing.assert_array_equal(vx, expected_x)
        np.testing.assert_array_equal(vy, expected_y)
        solver.end()
    finally:
        _close(solver)
    with pytest.raises(StaleObjectError):
        surface.get_velocities()


@pytest.mark.parametrize("momentum", ["LOCAL_INERTIAL", "DIFFUSIVE_WAVE", "FULL_SWE"])
def test_dry_cell_velocities_are_zero(momentum):
    text = SURFACE_MODEL.replace("MAX_TIMESTEP 5", "BACKEND CPU\nMOMENTUM_EQUATION " + momentum + "\nMAX_TIMESTEP 5")
    solver = _open("dry_velocities_" + momentum, text)
    try:
        solver.initialize()
        vx, vy = solver.surface2d.get_velocities()
        np.testing.assert_array_equal(vx, np.zeros(solver.surface2d.n_cells))
        np.testing.assert_array_equal(vy, np.zeros(solver.surface2d.n_cells))
    finally:
        _close(solver)


def test_indeterminate_wet_velocity_remains_nan():
    # At eta=0.02 the raised triangle has only one usable hydraulic section.
    eta, height = 0.02, 3.0
    mean_depth = eta**2 / height - eta**3 / (3 * height**2)
    text = SURFACE_MODEL.split("[2D_OPTIONS]")[0] + f"""[2D_OPTIONS]
BACKEND CPU
MOMENTUM_EQUATION LOCAL_INERTIAL
LTS_TIERS 1
CELL_CLOSURE VFR
FACE_RECONSTRUCTION VFR_FACE
VFR_MIN_WET_FRAC 1e-8
DRY_DEPTH 0.0001
[2D_VERTICES]
0 0 0
1 0 0
1 1 3
[2D_TRIANGLES]
0 1 2 0.03 {mean_depth:.17g}
"""
    solver = _open("indeterminate_native_velocity", text)
    try:
        solver.initialize()
        surface = solver.surface2d
        assert surface.get_depths()[0] > surface.dry_depth
        vx, vy = surface.get_velocities()
        assert np.isnan(vx[0]) and np.isnan(vy[0])
    finally:
        _close(solver)
