"""Transport configuration, capability diagnostics and thread introspection."""
import math
import pytest
from openswmm.engine import (
    Solver, TransportDispersionMode, TransportDomain, TransportClass,
    TransportState, BadParamError, BadHandleError,
    transport_domain_name, transport_class_name,
)
from tests.engine._solver_cases import EngineSolverCase


class TestTransport(EngineSolverCase):
    def test_configuration_and_validation(self):
        s = self.opened_solver()
        t = s.transport
        t.dispersion_mode = TransportDispersionMode.VALUE
        t.dispersion_value = 1.25
        t.target_dx = 12.5
        assert t.configured
        assert t.dispersion_mode is TransportDispersionMode.VALUE
        assert t.dispersion_value == 1.25
        assert t.target_dx == 12.5
        assert isinstance(t.boundaries, tuple)
        assert isinstance(t.sources, tuple)
        assert isinstance(t.conduit_dispersion, tuple)
        for invalid in (-1.0, math.nan, math.inf):
            with pytest.raises(BadParamError):
                t.dispersion_value = invalid
            with pytest.raises(BadParamError):
                t.target_dx = invalid
        with pytest.raises(ValueError):
            t.dispersion_mode = 99
        s.destroy()
        with pytest.raises(BadHandleError):
            _ = t.target_dx

    def test_matrix_axes_and_diagnostics(self):
        s = self.opened_solver()
        matrix = s.transport_matrix
        assert set(matrix) == set(TransportDomain)
        for domain, cells in matrix.items():
            assert transport_domain_name(domain)
            assert set(cells) == set(TransportClass)
            for species, cell in cells.items():
                assert transport_class_name(species)
                assert isinstance(cell.state, TransportState)
                assert cell.count >= 0
                if cell.state is not TransportState.ENABLED:
                    assert cell.reason
        with pytest.raises(ValueError):
            transport_domain_name(100)

    def test_thread_limits(self):
        s = self.opened_solver()
        info = s.thread_info
        assert info.logical_cpus >= 0
        assert info.omp_max_threads >= 1
        assert isinstance(info.omp_available, bool)
        explicit = s.effective_threads(1)
        assert explicit.global_threads == 1
        assert explicit.dynamic_wave in (0, 1)
        assert explicit.surface2d == 0
        assert s.effective_threads().global_threads >= 1


@pytest.mark.parametrize('units', ['CMS', 'CFS'])
def test_authored_transport_units_and_roundtrip(tmp_path, units):
    from tests.engine._groundwater_cases import MODEL, cleanup
    path = tmp_path / 'transport.inp'
    path.write_text(MODEL.split('[2D_OPTIONS]')[0].replace('FLOW_UNITS CMS', 'FLOW_UNITS ' + units) + '[REPORT]\nINPUT NO\n')
    solver = Solver(path, tmp_path / 'transport.rpt')
    try:
        solver.open()
        solver.process_components.register("org.hydrocouple.openswmm.transport.ard", "transport.ard")
        solver.options["QUALITY_SOLVER"] = "EULERIAN_ARD"
        solver.transport.dispersion_mode = TransportDispersionMode.VALUE
        solver.transport.dispersion_value = 1.25
        solver.transport.target_dx = 12.5
        saved = tmp_path / 'saved.inp'
        solver.write(saved)
        reopened = Solver(saved, tmp_path / 'saved.rpt')
        try:
            reopened.open()
            assert reopened.transport.dispersion_value == 1.25
            assert reopened.transport.target_dx == 12.5
            assert reopened.transport.dispersion_mode == TransportDispersionMode.VALUE
        finally:
            cleanup(reopened)
    finally:
        cleanup(solver)
