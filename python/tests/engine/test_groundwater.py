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


def test_authored_edges_and_layer_inventory(tmp_path):
    from openswmm.engine import GroundwaterBoundary, GroundwaterInitialQuality, GroundwaterTransportZone
    solver = open_model(tmp_path)
    try:
        gw = solver.surface2d.groundwater
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        gw.options['CLOSURE'] = 'SIGMA'
        gw.options['M_LAYERS'] = '4'
        gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        gw.transport.options['TRANSPORT_POLLUTANTS'] = 'YES'
        gw.transport.options['UNSAT_TRANSPORT'] = 'LAYERED'
        gw.set_authored_boundary(0, 0, 'FLOW', 1e-6)
        gw.transport.set_boundary(GroundwaterBoundary(cell=0, edge=0, species='TSS', value=3))
        gw.transport.set_boundary(GroundwaterBoundary(cell=0, edge=0, species='TSS', kind='MASSFLUX', value=2))
        assert len(gw.transport.boundaries) == 2
        assert gw.authored_boundaries[0]['kind'] == 'FLOW'
        gw.transport.set_initial_quality(GroundwaterInitialQuality(
            zone=GroundwaterTransportZone.LAYER, layer=2, species='TSS', value=7))
        with pytest.raises(LifecycleError):
            gw.layer_quality(0, 1, 0)
        solver.initialize()
        assert gw.layer_quality(0, 1, 0)['dissolved_concentration'] == pytest.approx(7)
        assert gw.layer_quality(0, 0, 0)['dissolved_concentration'] == 0
        solver.start(False)
        for _ in range(100):
            solver.step()
            if gw.authored_boundary_receipt(0)['accepted_in'] > 0:
                break
        assert gw.authored_boundary_receipt(0)['accepted_in'] > 0
        assert gw.authored_boundary_receipt(0, 0)['flux_in'] > 0
        with pytest.raises(LifecycleError):
            gw.remove_authored_boundary(0)
        solver.end()
    finally:
        cleanup(solver)


def test_subsurface_reaction_text_authoring_is_atomic(tmp_path):
    from openswmm.engine import ReactionScope, GroundwaterInitialQuality
    solver = open_model(tmp_path)
    try:
        gw = solver.surface2d.groundwater
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        rx = solver.reactions
        rx.apply_text('[REACTION_SPECIES]\nBULK A MG\nWALL W MG/FT2\n'
                      '[REACTION_TERMS]\nsoil GW_THETA/GW_POROSITY\n'
                      '[REACTION_SUBSURFACE]\n* SAT A RATE -0.001*A*soil\n')
        saved = rx.serialize()
        assert '[REACTION_SUBSURFACE]' in saved
        assert rx.validate('-A*soil', ReactionScope.SUBSURFACE).valid
        assert not rx.validate('-A*HRT', ReactionScope.SUBSURFACE).valid
        with pytest.raises(BadParamError):
            rx.apply_text(saved.replace('-0.001*A*soil', '-A*HRT'))
        assert rx.serialize() == saved
        gw = solver.surface2d.groundwater  # reaction-text replacement invalidates old views
        gw.transport.set_initial_quality(GroundwaterInitialQuality(species='A', value=7))
        solver.initialize()
        assert 'A' in gw.species
    finally:
        cleanup(solver)
