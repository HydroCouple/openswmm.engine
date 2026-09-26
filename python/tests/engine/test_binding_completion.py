"""Regression coverage for bulk editing, live output, staged writes and diagnostics."""
from pathlib import Path
import struct
import numpy as np
import pytest
from openswmm.engine import (
    ModelEditor, BadIndexError, BadHandleError, StaleObjectError,
    LifecycleError, OutputReader, OutNodeVar, Surface2D, HeatElemKind,
    ForcingType, ForcingMode,
)
from tests.engine._groundwater_cases import open_model, cleanup
from tests.engine._solver_cases import EngineSolverCase


class TestBulkEditAndLiveOutput(EngineSolverCase):
    def test_batch_invalid_atomic_and_duplicates(self):
        s = self.opened_solver()
        editor = ModelEditor(s)
        names = [n.id for n in s.nodes]
        old = s.nodes[0]
        with pytest.raises(BadIndexError):
            editor.delete_nodes([0, 999999])
        assert [n.id for n in s.nodes] == names
        assert old.id == names[0]
        impacts = editor.delete_nodes([names[0], 0])
        assert isinstance(impacts, list)
        assert editor.node_count == len(names) - 1
        with pytest.raises(StaleObjectError):
            _ = old.id
        for method in (editor.delete_nodes, editor.delete_links, editor.delete_subcatchments, editor.delete_gages):
            assert method([]) == []
        s.destroy()
        with pytest.raises(BadHandleError):
            _ = editor.node_count

    def test_report_snapshot_uses_live_diagnostics_and_percentages(self):
        from openswmm.engine import get_report_snapshot, RunoffTotal, RoutingTotal
        s = self.completed_solver()
        report = get_report_snapshot(s)
        mb = s.mass_balance
        assert report.routing_diagnostics == mb.routing_diagnostics
        assert report.routing_diagnostics.n_steps > 0
        assert report.runoff_continuity.continuity_error_pct == pytest.approx(100 * mb.runoff_continuity_error)
        assert report.routing_continuity.continuity_error_pct == pytest.approx(100 * mb.routing_continuity_error)
        assert report.runoff_continuity.initial_snow == mb.runoff_total(RunoffTotal.INITSNOW)
        assert report.runoff_continuity.final_snow == mb.runoff_total(RunoffTotal.FINALSNOW)
        assert report.routing_continuity.forcing_inflow == mb.routing_total(RoutingTotal.FORCING_INFLOW)
        assert report.routing_continuity.coupling_outflow == mb.routing_total(RoutingTotal.COUPLING_OUT)
        assert report.routing_continuity.link_groundwater_inflow == mb.routing_total(RoutingTotal.LINK_GW_INFLOW)
        for i, quality in enumerate(report.quality_continuity):
            assert quality.continuity_error_pct == pytest.approx(100 * mb.quality_continuity_error(i))

    def test_other_batch_deletions_validate_and_deduplicate(self):
        for collection, method in [('links', 'delete_links'), ('subcatchments', 'delete_subcatchments'), ('gages', 'delete_gages')]:
            s = self.opened_solver()
            editor = ModelEditor(s)
            objects = getattr(s, collection)
            count = len(objects)
            assert count > 0
            name = objects[0].id
            delete = getattr(editor, method)
            with pytest.raises(BadIndexError):
                delete([name, count + 10])
            assert len(objects) == count
            delete([name, 0])
            assert len(getattr(s, collection)) == count - 1

    def test_live_partial_period_refresh_and_footer(self):
        self.completed_solver()
        source = Path(self.solver_files()[2])
        data = source.read_bytes()
        _, _, start, periods, _, _ = struct.unpack('<6i', data[-24:])
        size = (len(data) - 24 - start) // periods
        live_path = source.with_name('binding_live.out')
        live_path.write_bytes(data[:start])
        with OutputReader(live_path, live=True) as live:
            assert live.is_live and live.period_count == 0
            assert len(live.period_times) == 0
            with live_path.open('ab') as stream:
                stream.write(data[start:start + size + size // 2])
            assert live.refresh() == 1
            assert len(live.period_times) == 1
            with live_path.open('ab') as stream:
                stream.write(data[start + size + size // 2:])
            assert live.refresh() == periods
            assert not live.is_live
            assert len(live.period_times) == periods
            with OutputReader(source) as finished:
                np.testing.assert_array_equal(live.node_result(0, OutNodeVar.DEPTH), finished.node_result(0, OutNodeVar.DEPTH))
        for read in (live.refresh, lambda: live.period_count, lambda: live.is_live):
            with pytest.raises(BadHandleError):
                read()


def test_staged_writer_callback_contract(tmp_path):
    solver = open_model(tmp_path)
    try:
        final = tmp_path / 'final.inp'
        stage = tmp_path / 'stage.inp'
        calls = []
        def mapper(path, kind):
            calls.append((path, kind))
            return stage if kind == 0 else tmp_path / ('stage-' + Path(path).name)
        solver.write_staged(final, mapper)
        assert stage.exists() and not final.exists()
        assert calls[0] == (str(final), 0)
        sentinel = ValueError('mapper failed')
        def fail(path, kind):
            raise sentinel
        with pytest.raises(ValueError) as error:
            solver.write_staged(final, fail)
        assert error.value is sentinel
        with pytest.raises(LifecycleError):
            solver.write_staged(final, lambda path, kind: solver.destroy())
        assert solver.handle
    finally:
        cleanup(solver)


def test_2d_diagnostics_and_forcing(tmp_path):
    solver = open_model(tmp_path, "[OPTIONS]\nHEAT_TRANSPORT ON\nWATER_AGE ON\n")
    try:
        surface = solver.surface2d
        assert Surface2D.output_variables()
        for preset in ('ALL', 'DEFAULT', 'MINIMAL'):
            mask = Surface2D.output_variable_mask(preset)
            assert Surface2D.output_variable_mask(Surface2D.output_variable_text(mask)) == mask
        with pytest.raises(ValueError):
            Surface2D.output_variable_mask('NO_SUCH_VARIABLE')
        solver.initialize()
        solver.start(False)
        forcing = solver.forcing
        forcing.node_temperature('J1', 20)
        forcing.node_age('J1', 1)
        forcing.link_seepage('C1', 0)
        forcing.element_climate(HeatElemKind.NODE, 'J1', ForcingType.ELEM_AIR_TEMPERATURE, 21, persist=True)
        value, mode = forcing.element_climate_get(HeatElemKind.NODE, 'J1', ForcingType.ELEM_AIR_TEMPERATURE)
        assert value == 21 and mode == ForcingMode.REPLACE
        solver.step()
        for read in (surface.get_rainfall_bulk, surface.get_rain_volume_bulk, surface.get_coupling_volume_bulk):
            result = read()
            assert result.shape == (2,) and np.isfinite(result).all()
        method, indices, weights = surface.rainfall_weights(1)
        assert method == -1 and indices.size == weights.size == 0
        solver.end()
    finally:
        cleanup(solver)


@pytest.mark.parametrize('units,rain', [('CMS', 36.0), ('CFS', 36.0 / 25.4)])
def test_nonzero_rainfall_weights_and_volume_in_si(tmp_path, units, rain):
    from openswmm.engine import Solver
    from tests.engine._groundwater_cases import MODEL
    path = tmp_path / 'rain.inp'
    model = MODEL.replace('FLOW_UNITS CMS', 'FLOW_UNITS ' + units)
    model = model.replace('WET_STEP 00:01:00', 'WET_STEP 00:00:01\nDRY_STEP 00:00:01').replace('ROUTING_STEP 5', 'ROUTING_STEP 1')
    model = model.replace('RAINFALL_MODE SYSTEM', 'RAINFALL_MODE NEAREST_NEIGHBOUR')
    path.write_text(model + f'''\n[RAINGAGES]
RG INTENSITY 1:00 1 TIMESERIES RAIN
[TIMESERIES]
RAIN 01/01/2026 00:00 {rain}
RAIN 01/01/2026 01:00 {rain}
[SYMBOLS]
RG 5 5
''')
    solver = Solver(path, tmp_path / 'rain.rpt')
    try:
        solver.open()
        surface = solver.surface2d
        solver.initialize()
        solver.start(False)
        for _ in range(10):
            solver.step()
        initial_volume = surface.get_rain_volume_bulk()
        initial_time = solver.elapsed.total_seconds()
        elapsed = solver.step().total_seconds() - initial_time
        np.testing.assert_allclose(surface.get_rainfall_bulk(), [1e-5, 1e-5], rtol=1e-6)
        for cell in range(2):
            method, indices, weights = surface.rainfall_weights(cell)
            assert method == 2
            np.testing.assert_array_equal(indices, [0])
            np.testing.assert_allclose(weights, [1])
            assert surface.get_rainfall_bulk()[cell] == pytest.approx(surface.get_rainfall(cell))
        area_scale = 0.3048 ** 2 if units == 'CFS' else 1
        np.testing.assert_allclose(surface.get_rain_volume_bulk() - initial_volume,
                                   np.array([50, 100]) * area_scale * 1e-5 * elapsed,
                                   rtol=1e-5)
        assert surface.get_coupling_volume_bulk().shape == (2,)
        solver.end()
    finally:
        cleanup(solver)


def test_geopackage_bulk_shapes_and_closed_handle(tmp_path):
    from openswmm.engine import GeoPackage
    with GeoPackage(str(tmp_path / 'observed.gpkg')) as database:
        series = database.create_observed_series('rain', 'rainfall')
        times = ['2026-01-01T00:00:00Z', '2026-01-01T00:01:00Z', '2026-01-01T00:02:00Z']
        with pytest.raises(ValueError):
            database.write_observed_values(series, times, [1])
        with pytest.raises(ValueError):
            database.write_observed_values(series, times, [1, 2, 3], ['A'])
        database.write_observed_values(series, [], [])
        values = np.arange(6, dtype=float)[::2]  # noncontiguous input must be copied
        database.write_observed_values(series, times, values)
        actual_times, actual = database.read_observed_values(series)
        assert actual_times == times
        np.testing.assert_array_equal(actual, values)
    with pytest.raises(BadHandleError):
        database.read_observed_values(series)
