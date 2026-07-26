"""Smoke tests for solver.controls and solver.inflows counts."""

import unittest  # noqa: F401  (kept for parity with the unittest suite)

from tests.engine._solver_cases import EngineSolverCase


class TestControlsBasic(EngineSolverCase):
    def test_count_initial(self):
        solver = self.running_solver()
        self.assertIsInstance(len(solver.controls), int)
        self.assertGreaterEqual(len(solver.controls), 0)


class TestInflowsCounts(EngineSolverCase):
    def test_ext_inflow_count(self):
        solver = self.running_solver()
        v = solver.inflows.external_count
        self.assertIsInstance(v, int)
        self.assertGreaterEqual(v, 0)

    def test_dwf_count(self):
        solver = self.running_solver()
        v = solver.inflows.dwf_count
        self.assertIsInstance(v, int)
        self.assertGreaterEqual(v, 0)

    def test_rdii_count(self):
        solver = self.running_solver()
        v = solver.inflows.rdii_count
        self.assertIsInstance(v, int)
        self.assertGreaterEqual(v, 0)
