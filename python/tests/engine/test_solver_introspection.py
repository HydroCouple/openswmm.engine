"""P2.5 — Solver introspection accessors + progress callback.

Surfaces the previously-unwrapped engine lifecycle functions
(``swmm_get_start_time`` / ``swmm_get_end_time`` / ``swmm_get_event_count`` /
``swmm_set_progress_callback``). All tests drive the real ``Solver`` over
``site_drainage_example.inp`` (no mocks).
"""

from __future__ import annotations

import unittest

from datetime import datetime

try:
    import openswmm.engine._solver  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import EngineState

from tests.engine._solver_cases import EngineSolverCase


class TestTimeIntrospection(EngineSolverCase):
    def test_sim_window(self):
        solver = self.opened_solver()
        start = solver.sim_start_time
        end = solver.sim_end_time
        self.assertIsInstance(start, datetime)
        self.assertIsInstance(end, datetime)
        self.assertGreater(end, start)

    def test_event_count(self):
        solver = self.opened_solver()
        n = solver.event_count
        self.assertIsInstance(n, int)
        self.assertGreaterEqual(n, 0)


class TestProgressCallback(EngineSolverCase):
    def test_progress_fires_monotonic(self):
        solver = self.initialized_solver()
        fractions: list[float] = []
        solver.set_progress_callback(fractions.append)
        solver.start()
        while solver.state == EngineState.RUNNING:
            solver.step()
        # Callback fired, fractions are valid and non-decreasing.
        self.assertTrue(fractions, "progress callback never fired")
        self.assertTrue(all(0.0 <= f <= 1.0 + 1e-9 for f in fractions))
        self.assertEqual(fractions, sorted(fractions))

    def test_unregister(self):
        solver = self.initialized_solver()
        solver.set_progress_callback(lambda f: None)
        solver.set_progress_callback(None)  # must not raise
