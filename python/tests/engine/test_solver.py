"""Tests for :class:`openswmm.engine.Solver` lifecycle management."""

import os
import pytest

from openswmm.engine import Solver, EngineError, EngineState
from tests.engine.conftest import SITE_DRAINAGE_INP, NON_EXISTENT_INP


# ---------------------------------------------------------------------------
# Construction
# ---------------------------------------------------------------------------
class TestSolverConstruction:
    """Solver instantiation and default state."""

    def test_create_default(self):
        s = Solver()
        assert s.handle == 0  # NULL before create()

    def test_create_with_paths(self):
        s = Solver("a.inp", "a.rpt", "a.out")
        assert s.handle == 0


# ---------------------------------------------------------------------------
# Manual lifecycle
# ---------------------------------------------------------------------------
class TestSolverManualLifecycle:
    """Explicit open → initialize → start → step → end → close → destroy."""

    def test_open_sets_handle(self, solver_files):
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        assert s.handle != 0
        s.close()
        s.destroy()

    def test_full_lifecycle(self, solver_files):
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()

        stepped = False
        while s.step():
            stepped = True
        assert stepped, "Simulation should advance at least one timestep"

        s.end()
        s.report()
        s.close()
        s.destroy()
        assert os.path.exists(rpt)
        assert os.path.exists(out)

    def test_step_returns_false_at_end(self, running_solver):
        last = True
        while last:
            last = running_solver.step()
        assert last is False


# ---------------------------------------------------------------------------
# Context manager
# ---------------------------------------------------------------------------
class TestSolverContextManager:
    """with Solver(...) as s: opens, initializes, starts on enter."""

    def test_context_manager_runs(self, solver_files):
        inp, rpt, out = solver_files
        with Solver(inp, rpt, out) as s:
            count = 0
            while s.step():
                count += 1
            assert count > 0

    def test_context_manager_cleanup(self, solver_files):
        inp, rpt, out = solver_files
        with Solver(inp, rpt, out) as s:
            while s.step():
                pass
        # After exiting, handle should be NULL (destroyed)
        assert s.handle == 0


# ---------------------------------------------------------------------------
# Properties
# ---------------------------------------------------------------------------
class TestSolverProperties:
    """Test solver properties: elapsed, state, handle."""

    def test_elapsed_after_step(self, running_solver):
        running_solver.step()
        assert running_solver.elapsed > 0.0

    def test_elapsed_zero_before_step(self, solver_files):
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()
        assert s.elapsed == 0.0
        s.end()
        s.close()
        s.destroy()

    def test_handle_is_integer(self, opened_solver):
        assert isinstance(opened_solver.handle, int)
        assert opened_solver.handle > 0


# ---------------------------------------------------------------------------
# Timing methods
# ---------------------------------------------------------------------------
class TestSolverTiming:
    """Simulation time query methods."""

    def test_start_time(self, running_solver):
        t = running_solver.get_start_time()
        assert isinstance(t, float)
        assert t > 0  # should be a valid Julian day

    def test_end_time_after_start(self, running_solver):
        assert running_solver.get_end_time() > running_solver.get_start_time()

    def test_current_time_advances(self, running_solver):
        t0 = running_solver.get_current_time()
        running_solver.step()
        t1 = running_solver.get_current_time()
        assert t1 > t0

    def test_routing_step_positive(self, running_solver):
        dt = running_solver.get_routing_step()
        assert dt > 0


# ---------------------------------------------------------------------------
# Model write
# ---------------------------------------------------------------------------
class TestSolverModelWrite:
    """Writing the model back to disk."""

    def test_model_write(self, opened_solver, tmp_path):
        out_path = str(tmp_path / "written.inp")
        opened_solver.model_write(out_path)
        assert os.path.exists(out_path)
        assert os.path.getsize(out_path) > 0


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------
class TestSolverErrors:
    """Error paths and invalid usage."""

    def test_open_nonexistent_raises(self, tmp_path):
        s = Solver(NON_EXISTENT_INP, str(tmp_path / "x.rpt"), str(tmp_path / "x.out"))
        with pytest.raises(RuntimeError):
            s.open()

    def test_double_destroy_safe(self, solver_files):
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        s.close()
        s.destroy()
        # Second destroy should be a no-op, not a crash
        s.destroy()

    def test_close_without_open_safe(self):
        s = Solver()
        # close on a NULL handle should be safe
        s.close()
