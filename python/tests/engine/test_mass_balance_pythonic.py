"""P6 — MassBalance Pythonic surface tests."""

from __future__ import annotations

import unittest

try:
    import openswmm.engine._massbalance  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import RoutingTotal, RunoffTotal
from openswmm.engine._massbalance import MassBalance
from openswmm.engine._report import RoutingDiagnostics

from tests.engine._solver_cases import EngineSolverCase


class TestContinuityErrors(EngineSolverCase):
    def test_properties_are_floats(self):
        solver = self.completed_solver()
        mb = solver.mass_balance
        self.assertIsInstance(mb, MassBalance)
        self.assertIsInstance(mb.runoff_continuity_error, float)
        self.assertIsInstance(mb.routing_continuity_error, float)

    def test_quality_continuity_error_by_int(self):
        solver = self.completed_solver()
        mb = solver.mass_balance
        if solver.pollutants and len(solver.pollutants) > 0:
            v = mb.quality_continuity_error(0)
            self.assertIsInstance(v, float)


class TestTotals(EngineSolverCase):
    def test_runoff_total_with_enum(self):
        solver = self.completed_solver()
        mb = solver.mass_balance
        v = mb.runoff_total(RunoffTotal.RAINFALL)
        self.assertIsInstance(v, float)

    def test_routing_total_with_enum(self):
        solver = self.completed_solver()
        mb = solver.mass_balance
        v = mb.routing_total(RoutingTotal.OUTFLOW)
        self.assertIsInstance(v, float)


class TestRoutingDiagnostics(EngineSolverCase):
    def test_returns_dataclass(self):
        solver = self.completed_solver()
        d = solver.mass_balance.routing_diagnostics
        self.assertIsInstance(d, RoutingDiagnostics)
        self.assertIsInstance(d.avg_time_step, float)
        self.assertIsInstance(d.n_steps, int)

    def test_max_courant_property(self):
        solver = self.completed_solver()
        v = solver.mass_balance.max_courant
        self.assertIsInstance(v, float)
