"""P6 — HotStart Pythonic surface tests."""

from __future__ import annotations

import os
import unittest
from datetime import datetime
from pathlib import Path

try:
    import openswmm.engine._hotstart  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import HotStart, Solver
from openswmm.engine._hotstart import SaveSchedule, SaveScheduleEntry

from tests._paths import artifact_dir
from tests.engine._solver_cases import EngineSolverCase


class _HotStartCase(EngineSolverCase):
    """Base case providing the ``saved_hs_path`` helper (formerly a fixture)."""

    def saved_hs_path(self):
        """Save a hot start at the end of a completed solver and return its path."""
        completed = self.completed_solver()
        p = os.path.join(artifact_dir(self), "test.hs")
        HotStart.save_from(completed, p)
        return p


class TestSaveAndOpen(_HotStartCase):
    def test_save_then_open(self):
        saved_hs_path = self.saved_hs_path()
        with HotStart.open(saved_hs_path) as hs:
            self.assertIsInstance(hs.sim_datetime, datetime)
            self.assertGreater(hs.node_count, 0)
            self.assertGreater(hs.link_count, 0)

    def test_open_path_or_str(self):
        saved_hs_path = self.saved_hs_path()
        with HotStart.open(Path(saved_hs_path)) as a, \
             HotStart.open(str(saved_hs_path)) as b:
            self.assertEqual(a.sim_datetime, b.sim_datetime)


class TestMetadata(_HotStartCase):
    def test_warnings_is_list(self):
        saved_hs_path = self.saved_hs_path()
        with HotStart.open(saved_hs_path) as hs:
            self.assertIsInstance(hs.warnings, list)
            for w in hs.warnings:
                self.assertIsInstance(w, str)

    def test_crs_is_str_or_none(self):
        saved_hs_path = self.saved_hs_path()
        with HotStart.open(saved_hs_path) as hs:
            self.assertTrue(hs.crs is None or isinstance(hs.crs, str))


class TestDirectEdits(_HotStartCase):
    def test_set_node_depth(self):
        saved_hs_path = self.saved_hs_path()
        opened_solver = self.opened_solver()
        with HotStart.open(saved_hs_path) as hs:
            # Pick the first node from the opened solver as our key.
            nid = opened_solver.nodes.get_id(0)
            hs.set_node_depth(nid, 0.5)
            # No round-trip getter on the HotStart, but the call must not raise.


class TestApply(_HotStartCase):
    def test_apply_to_initialized_solver(self):
        saved_hs_path = self.saved_hs_path()
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        try:
            s.open()
            s.initialize()
            with HotStart.open(saved_hs_path) as hs:
                hs.apply(s)
            s.start()
            for _ in s.steps():
                pass
            s.end()
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()


class TestSaveSchedule(EngineSolverCase):
    def test_view_type(self):
        opened_solver = self.opened_solver()
        self.assertIsInstance(opened_solver.save_schedule, SaveSchedule)

    def test_initial_length(self):
        opened_solver = self.opened_solver()
        self.assertGreaterEqual(len(opened_solver.save_schedule), 0)

    def test_append_and_read(self):
        opened_solver = self.opened_solver()
        opened_solver.save_schedule.clear()
        when = datetime(2024, 6, 15, 12, 0, 0)
        path = os.path.join(artifact_dir(self), "midday.hs")
        opened_solver.save_schedule.append(SaveScheduleEntry(when=when, path=path))
        self.assertEqual(len(opened_solver.save_schedule), 1)
        entry = opened_solver.save_schedule[0]
        # Whole-second precision; C API rounds.
        self.assertIsInstance(entry.when, datetime)
        self.assertEqual(entry.path, path)

    def test_delete_clear(self):
        opened_solver = self.opened_solver()
        d = artifact_dir(self)
        opened_solver.save_schedule.clear()
        opened_solver.save_schedule.append((datetime(2024, 1, 1), os.path.join(d, "a.hs")))
        opened_solver.save_schedule.append((datetime(2024, 2, 1), os.path.join(d, "b.hs")))
        del opened_solver.save_schedule[0]
        self.assertEqual(len(opened_solver.save_schedule), 1)
        opened_solver.save_schedule.clear()
        self.assertEqual(len(opened_solver.save_schedule), 0)
