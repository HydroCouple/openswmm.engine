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
