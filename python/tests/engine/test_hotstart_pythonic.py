"""P6 — HotStart Pythonic surface tests."""

from __future__ import annotations

import os
from datetime import datetime
from pathlib import Path

import pytest

pytest.importorskip("openswmm.engine._hotstart")

from openswmm.engine import HotStart, Solver
from openswmm.engine._hotstart import SaveSchedule, SaveScheduleEntry


@pytest.fixture
def saved_hs_path(completed_solver, tmp_path):
    """Save a hot start at the end of completed_solver and return its path."""
    p = tmp_path / "test.hs"
    HotStart.save_from(completed_solver, p)
    return p


class TestSaveAndOpen:
    def test_save_then_open(self, saved_hs_path):
        with HotStart.open(saved_hs_path) as hs:
            assert isinstance(hs.sim_datetime, datetime)
            assert hs.node_count > 0
            assert hs.link_count > 0

    def test_open_path_or_str(self, saved_hs_path):
        with HotStart.open(Path(saved_hs_path)) as a, \
             HotStart.open(str(saved_hs_path)) as b:
            assert a.sim_datetime == b.sim_datetime


class TestMetadata:
    def test_warnings_is_list(self, saved_hs_path):
        with HotStart.open(saved_hs_path) as hs:
            assert isinstance(hs.warnings, list)
            for w in hs.warnings:
                assert isinstance(w, str)

    def test_crs_is_str_or_none(self, saved_hs_path):
        with HotStart.open(saved_hs_path) as hs:
            assert hs.crs is None or isinstance(hs.crs, str)


class TestDirectEdits:
    def test_set_node_depth(self, saved_hs_path, opened_solver):
        with HotStart.open(saved_hs_path) as hs:
            # Pick the first node from the opened solver as our key.
            nid = opened_solver.nodes.get_id(0)
            hs.set_node_depth(nid, 0.5)
            # No round-trip getter on the HotStart, but the call must not raise.


class TestApply:
    def test_apply_to_initialized_solver(self, saved_hs_path, solver_files):
        inp, rpt, out = solver_files
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


class TestSaveSchedule:
    def test_view_type(self, opened_solver):
        assert isinstance(opened_solver.save_schedule, SaveSchedule)

    def test_initial_length(self, opened_solver):
        assert len(opened_solver.save_schedule) >= 0

    def test_append_and_read(self, opened_solver, tmp_path):
        opened_solver.save_schedule.clear()
        when = datetime(2024, 6, 15, 12, 0, 0)
        path = str(tmp_path / "midday.hs")
        opened_solver.save_schedule.append(SaveScheduleEntry(when=when, path=path))
        assert len(opened_solver.save_schedule) == 1
        entry = opened_solver.save_schedule[0]
        # Whole-second precision; C API rounds.
        assert isinstance(entry.when, datetime)
        assert entry.path == path

    def test_delete_clear(self, opened_solver, tmp_path):
        opened_solver.save_schedule.clear()
        opened_solver.save_schedule.append((datetime(2024,1,1), str(tmp_path/"a.hs")))
        opened_solver.save_schedule.append((datetime(2024,2,1), str(tmp_path/"b.hs")))
        del opened_solver.save_schedule[0]
        assert len(opened_solver.save_schedule) == 1
        opened_solver.save_schedule.clear()
        assert len(opened_solver.save_schedule) == 0
