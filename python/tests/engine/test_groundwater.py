"""Mixed-cell aquifer authoring, lifecycle, units and snapshot regressions."""
import numpy as np
import pytest
from openswmm.engine import (
    CellScope, GroundwaterClosure, GroundwaterVariable, GroundwaterLedger,
    BadParamError, LifecycleError, StaleObjectError,
)
from tests.engine._groundwater_cases import open_model, cleanup


def test_groundwater_authoring_and_roundtrip(tmp_path):
    solver = open_model(tmp_path)
    try:
        gw = solver.surface2d.groundwater
        assert not gw.active
        assert gw.rows == ()
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        assert gw.options['NODE_ENROLMENT'] == 'ROWS'
        gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        gw.add_row(CellScope.TAG, 18, 0.6, 0.4, 0.1, 1, tag='PAN')
        assert gw.rows[1].tag == 'PAN'
        gw.remove_row(1)
        assert gw.rows[0].ks == 36  # authored mm/hr, not converted to m/s
        assert gw.row_property(0, 'HG0') == 0.2
        gw.add_node('J1', -1, 36, 0.1, 0.5)
        assert gw.nodes[0].locate
        assert not gw.nodes[0].automatic
        gw.set_node_exchange(0, False)
        assert not gw.nodes[0].exchange
        gw.remove_node(0)
        assert gw.nodes == ()
        with pytest.raises(BadParamError):
            gw.set_row_property(0, 'NO_SUCH_PROPERTY', 1)
        saved = tmp_path / 'saved.inp'
        solver.write(saved)
        from openswmm.engine import Solver
        reopened = Solver(saved, tmp_path / 'saved.rpt')
        try:
            reopened.open()
            assert reopened.surface2d.groundwater.rows == gw.rows
            assert reopened.surface2d.groundwater.row_property(0, 'HG0') == 0.2
        finally:
            cleanup(reopened)
        solver.close()
        with pytest.raises(StaleObjectError):
            _ = gw.rows
    finally:
        cleanup(solver)


@pytest.mark.parametrize('sigma', [False, True])
def test_groundwater_runtime_mixed_cells(tmp_path, sigma):
    solver = open_model(tmp_path)
    try:
        gw = solver.surface2d.groundwater
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        if sigma:
            gw.options['M_LAYERS'] = '4'
            gw.set_row_property(0, 'CLOSURE', GroundwaterClosure.SIGMA)
            gw.set_row_property(0, 'M_LAYERS', 4)
        with pytest.raises(LifecycleError):
            gw.cells(GroundwaterVariable.HG)
        solver.initialize()
        solver.start(False)
        solver.step()
        assert gw.active
        assert gw.dimensions[0] == 2  # one triangle plus one quad
        values = gw.cells(GroundwaterVariable.HG)
        assert values.shape == (2,)
        assert values.dtype == np.float64
        assert np.isfinite(values).all()
        assert values[0] == pytest.approx(gw.cell(0, GroundwaterVariable.HG))
        values[:] = -999
        assert gw.cell(0, GroundwaterVariable.HG) >= 0  # independent snapshot
        assert gw.ledger(GroundwaterLedger.STORAGE) >= 0
        assert abs(gw.continuity_error) < 1e-7
        assert gw.tier_histogram.dtype == np.dtype('l')
        assert gw.tier_histogram.ndim == 1
        assert isinstance(gw.species, tuple)
        if sigma:
            assert gw.column(0).shape == (4,)
        else:
            with pytest.raises(BadParamError):
                gw.column(0)
        with pytest.raises(LifecycleError):
            gw.add_row(CellScope.GLOBAL, 1, 1, 0.4, 0.1, 1)
        solver.end()
    finally:
        cleanup(solver)
