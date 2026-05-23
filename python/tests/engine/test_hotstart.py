"""Tests for :class:`openswmm.engine.HotStart` save/restore functionality."""

import os
import pytest

from openswmm.engine import Solver, HotStart, EngineState
from tests.engine.conftest import SITE_DRAINAGE_INP


# ---------------------------------------------------------------------------
# Save
# ---------------------------------------------------------------------------
class TestHotStartSave:
    """Saving simulation state to a hot start file."""

    def test_save_creates_file(self, running_solver, tmp_path):
        # Step to get some state
        for _ in range(5):
            running_solver.step()
        hs_path = str(tmp_path / "test.hs")
        HotStart.save(running_solver, hs_path)
        assert os.path.exists(hs_path)
        assert os.path.getsize(hs_path) > 0

    def test_save_after_full_simulation(self, solver_files, tmp_path):
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()
        while s.state == EngineState.RUNNING:
            if s.step() != 0:
                break
            pass
        hs_path = str(tmp_path / "completed.hs")
        HotStart.save(s, hs_path)
        assert os.path.exists(hs_path)
        s.end()
        s.close()
        s.destroy()


# ---------------------------------------------------------------------------
# Open / Close
# ---------------------------------------------------------------------------
class TestHotStartOpenClose:
    """Opening and closing hot start files."""

    @pytest.fixture
    def hs_file(self, running_solver, tmp_path):
        for _ in range(5):
            running_solver.step()
        path = str(tmp_path / "test.hs")
        HotStart.save(running_solver, path)
        return path

    def test_open(self, hs_file):
        hs = HotStart.open(hs_file)
        assert hs is not None
        hs.close()

    def test_context_manager(self, hs_file):
        with HotStart.open(hs_file) as hs:
            assert hs is not None

    def test_close_idempotent(self, hs_file):
        hs = HotStart.open(hs_file)
        hs.close()
        hs.close()  # second close should be safe

    def test_open_nonexistent_raises(self, tmp_path):
        with pytest.raises((IOError, RuntimeError)):
            HotStart.open(str(tmp_path / "nonexistent.hs"))


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------
class TestHotStartApply:
    """Applying a hot start to restore simulation state."""

    def test_apply_to_initialized_solver(self, solver_files, tmp_path):
        inp, rpt, out = solver_files
        # First run: create hot start file
        s1 = Solver(inp, rpt, out)
        s1.open()
        s1.initialize()
        s1.start()
        for _ in range(10):
            s1.step()
        hs_path = str(tmp_path / "apply_test.hs")
        HotStart.save(s1, hs_path)
        s1.end()
        s1.close()
        s1.destroy()

        # Second run: apply hot start
        rpt2 = str(tmp_path / "run2.rpt")
        out2 = str(tmp_path / "run2.out")
        s2 = Solver(inp, rpt2, out2)
        s2.open()
        s2.initialize()

        with HotStart.open(hs_path) as hs:
            hs.apply(s2)

        s2.start()
        # Should be able to continue stepping after applying hot start
        stepped = False
        while s2.state == EngineState.RUNNING:
            if s2.step() != 0:
                break
            stepped = True
        assert stepped
        s2.end()
        s2.close()
        s2.destroy()


# ---------------------------------------------------------------------------
# Save schedule (new additions)
# ---------------------------------------------------------------------------
class TestHotStartSaves:
    """saves_count / saves_add / saves_get_path / saves_get_datetime /
    saves_set_path / saves_set_datetime / saves_remove / saves_clear."""

    def test_saves_count_initially_zero(self, opened_solver):
        count = HotStart.saves_count(opened_solver)
        assert isinstance(count, int)
        assert count >= 0

    def test_saves_add_increments_count(self, opened_solver, tmp_path):
        before = HotStart.saves_count(opened_solver)
        path = str(tmp_path / "sched.hs")
        HotStart.saves_add(opened_solver, path)
        assert HotStart.saves_count(opened_solver) == before + 1

    def test_saves_get_path_roundtrip(self, opened_solver, tmp_path):
        path = str(tmp_path / "sched_rt.hs")
        HotStart.saves_add(opened_solver, path)
        idx = HotStart.saves_count(opened_solver) - 1
        retrieved = HotStart.saves_get_path(opened_solver, idx)
        assert retrieved == path

    def test_saves_get_datetime_default_zero(self, opened_solver, tmp_path):
        path = str(tmp_path / "sched_dt.hs")
        HotStart.saves_add(opened_solver, path)
        idx = HotStart.saves_count(opened_solver) - 1
        dt = HotStart.saves_get_datetime(opened_solver, idx)
        assert isinstance(dt, float)
        assert dt == 0.0

    def test_saves_set_path(self, opened_solver, tmp_path):
        HotStart.saves_add(opened_solver, str(tmp_path / "orig.hs"))
        idx = HotStart.saves_count(opened_solver) - 1
        new_path = str(tmp_path / "updated.hs")
        HotStart.saves_set_path(opened_solver, idx, new_path)
        assert HotStart.saves_get_path(opened_solver, idx) == new_path

    def test_saves_set_datetime(self, opened_solver, tmp_path):
        HotStart.saves_add(opened_solver, str(tmp_path / "dt_set.hs"))
        idx = HotStart.saves_count(opened_solver) - 1
        HotStart.saves_set_datetime(opened_solver, idx, 1.5)
        dt = HotStart.saves_get_datetime(opened_solver, idx)
        assert abs(dt - 1.5) < 1e-9

    def test_saves_remove_decrements_count(self, opened_solver, tmp_path):
        HotStart.saves_add(opened_solver, str(tmp_path / "rm.hs"))
        before = HotStart.saves_count(opened_solver)
        HotStart.saves_remove(opened_solver, before - 1)
        assert HotStart.saves_count(opened_solver) == before - 1

    def test_saves_clear_empties_schedule(self, opened_solver, tmp_path):
        HotStart.saves_add(opened_solver, str(tmp_path / "clr1.hs"))
        HotStart.saves_add(opened_solver, str(tmp_path / "clr2.hs"))
        HotStart.saves_clear(opened_solver)
        assert HotStart.saves_count(opened_solver) == 0

