"""Round trips for model metadata added to the public native API."""
from pathlib import Path
import tempfile

import pytest
from openswmm.engine import Solver, InpProfile
from openswmm.legacy.engine import _solver as legacy
from tests.engine._solver_cases import EngineSolverCase


class TestMetadataBindings(EngineSolverCase):
    def test_initial_quality_sidecar_provenance(self):
        solver = self.opened_solver()
        quality = solver.initial_quality
        node = solver.nodes.get_id(0)
        pollutant = solver.pollutants.get_id(0)
        quality.set(pollutant, 2.0, node=node)
        assert not quality.is_file(-1)
        with pytest.raises(IndexError):
            quality.is_file(100000)
        with tempfile.TemporaryDirectory() as directory:
            sidecar = Path(directory) / 'quality.csv'
            sidecar_node = solver.nodes.get_id(1)
            sidecar.write_text(f'NODE,{sidecar_node},{pollutant},7.0\n')
            quality.file_path = str(sidecar)
            assert quality.file_path == str(sidecar)
            assert quality[-1].value == 2.0  # setting the reference does not reload it
            model = Path(directory) / 'model.inp'
            solver.write(model)
            loaded = Solver(model, Path(directory) / 'model.rpt')
            try:
                loaded.open()
                rows = loaded.initial_quality
                assert any(rows.is_file(i) and rows[i].value == 7.0 for i in range(len(rows)))
            finally:
                loaded.close()
                loaded.destroy()
        quality.file_path = None
        assert quality.file_path == ''

    def test_heat_timeseries_readback(self):
        solver = self.opened_solver()
        series = solver.tables.add_timeseries('PY_CLIMATE_SERIES')
        series.add(solver.start_datetime, 0.5)
        heat = solver.heat
        assert heat.shortwave_timeseries == ''
        assert heat.cloud.timeseries == ''
        heat.set_shortwave_timeseries(series.id)
        heat.cloud.set_timeseries(series.id)
        assert heat.shortwave_timeseries == series.id
        assert heat.cloud.timeseries == series.id
        heat.cloud.clear()
        assert heat.cloud.timeseries == ''

    def test_registry_is_distinct_from_model_membership(self):
        solver = self.opened_solver()
        registry = solver.process_components.known()
        assert registry
        assert len({row.id for row in registry}) == len(registry)
        assert all(row.id and row.description and isinstance(row.implemented, bool) for row in registry)
        assert len(solver.process_components) == 0

    def test_timeseries_relative_metadata(self):
        solver = self.opened_solver()
        series = solver.tables.add_timeseries('PY_RELATIVE')
        series.add(45000.0, 1.0)
        series.add(45000.5, 2.0)
        before = series.points.copy()
        series.set_relative_info(2, 45000.0)
        assert series.relative_info == (2, 45000.0)
        assert (series.points == before).all()
        series.set_relative_info(0, 0.0)
        assert series.relative_info[0] == 0

    def test_restore_orientation_is_idempotent(self):
        links = self.opened_solver().links
        assert links.restore_authored_orientation() >= 0
        assert links.restore_authored_orientation() == 0

    def test_compatibility_profiles_can_be_loaded(self):
        solver = self.opened_solver()
        counts = (len(solver.nodes), len(solver.links))
        with tempfile.TemporaryDirectory() as directory:
            for profile in InpProfile:
                target = Path(directory) / (profile.name + '.inp')
                solver.write_compat(target, profile)
                assert target.stat().st_size > 0
                assert (len(solver.nodes), len(solver.links)) == counts
                if profile is InpProfile.FULL:
                    loaded = Solver(target, target.with_suffix('.rpt'))
                    try:
                        loaded.open()
                        assert (len(loaded.nodes), len(loaded.links)) == counts
                    finally:
                        loaded.close()
                        loaded.destroy()
                else:
                    loaded = legacy.Solver(inp_file=str(target), rpt_file=str(target.with_suffix('.rpt')),
                                           out_file=str(target.with_suffix('.out')))
                    try:
                        loaded.initialize()
                    finally:
                        loaded.end()
                        loaded.finalize()
        with pytest.raises(ValueError):
            solver.write_compat('unused.inp', 999)
