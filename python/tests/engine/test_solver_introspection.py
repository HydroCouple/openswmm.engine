"""P2.5 — Solver introspection accessors + progress callback.

Surfaces the previously-unwrapped engine lifecycle functions
(``swmm_get_start_time`` / ``swmm_get_end_time`` / ``swmm_get_event_count`` /
``swmm_set_progress_callback``). All tests drive the real ``Solver`` over
``site_drainage_example.inp`` (no mocks).
"""

from __future__ import annotations

from datetime import datetime

import pytest

pytest.importorskip("openswmm.engine._solver")

from openswmm.engine import EngineState


class TestTimeIntrospection:
    def test_sim_window(self, opened_solver):
        start = opened_solver.sim_start_time
        end = opened_solver.sim_end_time
        assert isinstance(start, datetime)
        assert isinstance(end, datetime)
        assert end > start

    def test_event_count(self, opened_solver):
        n = opened_solver.event_count
        assert isinstance(n, int)
        assert n >= 0


class TestProgressCallback:
    def test_progress_fires_monotonic(self, initialized_solver):
        fractions: list[float] = []
        initialized_solver.set_progress_callback(fractions.append)
        initialized_solver.start()
        while initialized_solver.state == EngineState.RUNNING:
            initialized_solver.step()
        # Callback fired, fractions are valid and non-decreasing.
        assert fractions, "progress callback never fired"
        assert all(0.0 <= f <= 1.0 + 1e-9 for f in fractions)
        assert fractions == sorted(fractions)

    def test_unregister(self, initialized_solver):
        initialized_solver.set_progress_callback(lambda f: None)
        initialized_solver.set_progress_callback(None)  # must not raise
