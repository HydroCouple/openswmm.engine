"""P6 — MassBalance Pythonic surface tests."""

from __future__ import annotations

import pytest

pytest.importorskip("openswmm.engine._massbalance")

from openswmm.engine import RoutingTotal, RunoffTotal
from openswmm.engine._massbalance import MassBalance
from openswmm.engine._report import RoutingDiagnostics


class TestContinuityErrors:
    def test_properties_are_floats(self, completed_solver):
        mb = completed_solver.mass_balance
        assert isinstance(mb, MassBalance)
        assert isinstance(mb.runoff_continuity_error, float)
        assert isinstance(mb.routing_continuity_error, float)

    def test_quality_continuity_error_by_int(self, completed_solver):
        mb = completed_solver.mass_balance
        if completed_solver.pollutants and len(completed_solver.pollutants) > 0:
            v = mb.quality_continuity_error(0)
            assert isinstance(v, float)


class TestTotals:
    def test_runoff_total_with_enum(self, completed_solver):
        mb = completed_solver.mass_balance
        v = mb.runoff_total(RunoffTotal.RAINFALL)
        assert isinstance(v, float)

    def test_routing_total_with_enum(self, completed_solver):
        mb = completed_solver.mass_balance
        v = mb.routing_total(RoutingTotal.OUTFLOW)
        assert isinstance(v, float)


class TestRoutingDiagnostics:
    def test_returns_dataclass(self, completed_solver):
        d = completed_solver.mass_balance.routing_diagnostics
        assert isinstance(d, RoutingDiagnostics)
        assert isinstance(d.avg_time_step, float)
        assert isinstance(d.n_steps, int)

    def test_max_courant_property(self, completed_solver):
        v = completed_solver.mass_balance.max_courant
        assert isinstance(v, float)
