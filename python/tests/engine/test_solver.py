"""Tests for :class:`openswmm.engine.Solver` lifecycle management."""

import os
from datetime import datetime, timedelta

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
        for _ in s.steps():
            stepped = True
        assert stepped, "Simulation should advance at least one timestep"

        s.end()
        s.report()
        s.close()
        s.destroy()
        assert os.path.exists(rpt)
        assert os.path.exists(out)

    def test_step_signals_completion_via_state(self, running_solver):
        # v1: step() returns timedelta, simulation completion is reported via
        # both the state property transitioning to ENDED and the
        # steps() iterator terminating.
        for _ in running_solver.steps():
            pass
        assert running_solver.state == EngineState.ENDED


# ---------------------------------------------------------------------------
# Context manager
# ---------------------------------------------------------------------------
class TestSolverContextManager:
    """with Solver(...) as s: opens, initializes, starts on enter."""

    def test_context_manager_runs(self, solver_files):
        inp, rpt, out = solver_files
        with Solver(inp, rpt, out) as s:
            count = 0
            for _ in s.steps():
                count += 1
            assert count > 0

    def test_context_manager_cleanup(self, solver_files):
        inp, rpt, out = solver_files
        with Solver(inp, rpt, out) as s:
            for _ in s.steps():
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
        # v1: elapsed is a datetime.timedelta, not a float.
        assert running_solver.elapsed > timedelta(0)

    def test_elapsed_zero_before_step(self, solver_files):
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()
        assert s.elapsed == timedelta(0)
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
        # v1: simulation start is exposed as a datetime property, not a
        # float Julian day returned from get_start_time().
        t = running_solver.sim_start_time
        assert isinstance(t, datetime)

    def test_end_time_after_start(self, running_solver):
        assert running_solver.sim_end_time > running_solver.sim_start_time

    def test_current_time_advances(self, running_solver):
        t0 = running_solver.current_datetime
        running_solver.step()
        t1 = running_solver.current_datetime
        assert t1 > t0

    def test_routing_step_positive(self, running_solver):
        # v1: routing_step is a timedelta property.
        dt = running_solver.routing_step
        assert dt > timedelta(0)


# ---------------------------------------------------------------------------
# Model write
# ---------------------------------------------------------------------------
class TestSolverModelWrite:
    """Writing the model back to disk."""

    def test_model_write(self, opened_solver, tmp_path):
        out_path = str(tmp_path / "written.inp")
        opened_solver.write(out_path)
        assert os.path.exists(out_path)
        assert os.path.getsize(out_path) > 0


# ---------------------------------------------------------------------------
# Stride
# ---------------------------------------------------------------------------
class TestSolverStride:
    """Test stride() multi-step advancement."""

    def test_stride_advances(self, running_solver):
        # v1: stride() returns the elapsed advancement as a timedelta; the
        # .elapsed property reflects the same advancement as a side effect.
        assert running_solver.stride(5) > timedelta(0)
        assert running_solver.elapsed > timedelta(0)

    def test_stride_single(self, running_solver):
        assert running_solver.stride(1) > timedelta(0)
        assert running_solver.elapsed > timedelta(0)


# ---------------------------------------------------------------------------
# Open with plugin_lib
# ---------------------------------------------------------------------------
class TestSolverOpenPluginLib:
    """Test open() with optional plugin_lib parameter."""

    def test_open_with_none_plugin(self, solver_files):
        inp, rpt, out = solver_files
        s = Solver(inp, rpt, out)
        s.open(plugin_lib=None)
        assert s.handle != 0
        s.close()
        s.destroy()


# ---------------------------------------------------------------------------
# Step callbacks
# ---------------------------------------------------------------------------
class TestSolverStepCallbacks:
    """Test step begin/end callback registration."""

    def test_set_step_begin_callback(self, running_solver):
        called = []
        running_solver.set_step_begin_callback(lambda t, dt: called.append("begin"))
        running_solver.step()
        assert len(called) > 0

    def test_set_step_end_callback(self, running_solver):
        called = []
        running_solver.set_step_end_callback(lambda t, dt: called.append("end"))
        running_solver.step()
        assert len(called) > 0

    def test_callback_none_clears(self, running_solver):
        running_solver.set_step_begin_callback(lambda t, dt: None)
        running_solver.set_step_begin_callback(None)
        running_solver.step()  # Should not raise


# ---------------------------------------------------------------------------
# Module-level run functions
# ---------------------------------------------------------------------------
class TestModuleLevelRun:
    """Test the module-level run() and run_with_callback() functions."""

    def test_run(self, solver_files):
        from openswmm.engine import run
        inp, rpt, out = solver_files
        run(inp, rpt, out)
        assert os.path.exists(rpt)

    def test_run_with_callback(self, solver_files):
        from openswmm.engine import run_with_callback
        inp, rpt, out = solver_files
        progress = []
        run_with_callback(inp, rpt, out, lambda p: progress.append(p))
        assert len(progress) > 0


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------
class TestSolverErrors:
    """Error paths and invalid usage."""

    def test_open_nonexistent_raises(self, tmp_path):
        # v1: open() returns None and raises EngineError on failure (e.g. a
        # missing input file) rather than returning a non-zero rc.
        s = Solver(NON_EXISTENT_INP, str(tmp_path / "x.rpt"), str(tmp_path / "x.out"))
        with pytest.raises(EngineError):
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
