"""Round-trip all new groundwater and surface-quality authoring tables."""
from dataclasses import replace
import numpy as np
import pytest
from openswmm.engine import (
    Solver, CellScope, GroundwaterParameters, GroundwaterSorption,
    GroundwaterInitialQuality, GroundwaterBoundary, GroundwaterSource,
    GroundwaterSourceTerm, GroundwaterZone, GroundwaterSpeciesLedger,
    BadIndexError, BadParamError, LifecycleError, StaleObjectError,
)
from tests.engine._groundwater_cases import open_model, cleanup


def test_groundwater_transport_tables(tmp_path):
    solver = open_model(tmp_path)
    try:
        t = solver.surface2d.groundwater.transport
        t.options['TRANSPORT_POLLUTANTS'] = 'YES'
        assert t.options['TRANSPORT_POLLUTANTS'] == 'YES'
        t.set_parameters(GroundwaterParameters())
        t.set_parameters(replace(t.parameters[0], rho_s=2700))
        assert len(t.parameters) == 1 and t.parameters[0].rho_s == 2700
        t.set_sorption(GroundwaterSorption(species='TSS', kd=0.25))
        t.set_initial_quality(GroundwaterInitialQuality(species='TSS', value=5))
        t.set_boundary(GroundwaterBoundary(cell=1, edge=3, species='TSS', value=2))
        t.set_source(GroundwaterSource(name='well', cell=1, flow=1e-5))
        t.set_source_species(0, GroundwaterSourceTerm(species='TSS', value=3))
        assert t.source_species(0)[0].value == 3
        assert t.authored
        saved = tmp_path / 'transport.inp'
        solver.write(saved)
        reopened = Solver(saved, tmp_path / 'transport.rpt')
        try:
            reopened.open()
            actual = reopened.surface2d.groundwater.transport
            for field in ('parameters', 'sorption', 'initial_quality', 'boundaries', 'sources'):
                assert getattr(actual, field) == getattr(t, field)
            assert actual.source_species(0) == t.source_species(0)
        finally:
            cleanup(reopened)
        t.initial_quality_file = tmp_path / ('quality-' + 'x' * 180 + '.csv')
        assert t.initial_quality_file.endswith('.csv')
        t.initial_quality_file = None
        assert t.initial_quality_file == ''
        with pytest.raises(BadIndexError):
            t.remove_source(999)
        t.remove_source_species(0, 0)
        assert t.source_species(0) == ()
        for field, remove in [('parameters', t.remove_parameters), ('sorption', t.remove_sorption),
                              ('initial_quality', t.remove_initial_quality), ('boundaries', t.remove_boundary),
                              ('sources', t.remove_source)]:
            remove(0)
            assert getattr(t, field) == ()
        solver.close()
        with pytest.raises(StaleObjectError):
            _ = t.parameters
    finally:
        cleanup(solver)


def test_groundwater_species_snapshots(tmp_path):
    solver = open_model(tmp_path)
    try:
        gw = solver.surface2d.groundwater
        gw.options['NODE_ENROLMENT'] = 'ROWS'
        gw.add_row(CellScope.GLOBAL, 36, 0.5, 0.45, 0.1, 2)
        gw.set_row_property(0, 'HG0', 0.2)
        gw.transport.options['TRANSPORT_POLLUTANTS'] = 'YES'
        gw.transport.set_initial_quality(GroundwaterInitialQuality(species='TSS', value=5))
        solver.initialize()
        solver.start(False)
        solver.step()
        index = gw.species.index('TSS')
        for zone in GroundwaterZone:
            values = gw.concentrations(zone, index)
            assert values.shape == (2,) and np.isfinite(values).all()
            values[:] = -999
            assert (gw.concentrations(zone, index) >= 0).all()
        for term in GroundwaterSpeciesLedger:
            assert np.isfinite(gw.species_ledger(index, term))
        solver.end()
    finally:
        cleanup(solver)


def test_surface_quality_tables_and_buildup(tmp_path):
    solver = open_model(tmp_path, '''[LANDUSES]
URBAN 0 0 0
[BUILDUP]
URBAN TSS POW 100 0 1 AREA
''')
    try:
        q = solver.surface2d.quality
        q.set_coverage(CellScope.GLOBAL, {'URBAN': 100})
        q.set_loading(CellScope.GLOBAL, 'TSS', 10)
        q.set_loading(CellScope.TAG, 'TSS', 20, tag='PAN')
        q.set_loading(CellScope.CELL, 'TSS', 30, cell=0)
        q.set_curb_length(CellScope.GLOBAL, 25)
        assert q.coverages[0].landuses == (('URBAN', 100),)
        assert len(q.loadings) == 3
        assert q.curb_lengths[0].length == 25
        with pytest.raises(ValueError):
            q.set_coverage(CellScope.GLOBAL, {})
        with pytest.raises(BadParamError):
            q.set_coverage(CellScope.GLOBAL, {'URBAN': 101})
        saved = tmp_path / 'quality.inp'
        solver.write(saved)
        reopened = Solver(saved, tmp_path / 'quality.rpt')
        try:
            reopened.open()
            actual = reopened.surface2d.quality
            assert actual.coverages == q.coverages
            assert actual.loadings == q.loadings
            assert actual.curb_lengths == q.curb_lengths
            actual.remove_coverage(0)
            actual.remove_loading(2)
            actual.remove_curb_length(0)
            assert actual.coverages == () and actual.curb_lengths == ()
            assert len(actual.loadings) == 2
        finally:
            cleanup(reopened)
        solver.initialize()
        solver.start(False)
        values = q.buildup('TSS')
        np.testing.assert_allclose(values, [30, 20])
        values[:] = -1
        assert (q.buildup('TSS') >= 0).all()
        with pytest.raises(LifecycleError):
            q.remove_loading(0)
        solver.end()
    finally:
        cleanup(solver)
