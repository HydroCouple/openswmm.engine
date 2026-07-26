"""Tests for :class:`openswmm.engine.Solver` lifecycle management."""

import os
import unittest
from datetime import datetime, timedelta

from openswmm.engine import Solver, EngineError, EngineState
from tests._paths import NON_EXISTENT_INP, artifact_dir
from tests.engine._solver_cases import EngineSolverCase


# ---------------------------------------------------------------------------
# Construction
# ---------------------------------------------------------------------------
class TestSolverConstruction(unittest.TestCase):
    """Solver instantiation and default state."""

    def test_create_default(self):
        s = Solver()
        self.assertEqual(s.handle, 0)  # NULL before create()

    def test_create_with_paths(self):
        s = Solver("a.inp", "a.rpt", "a.out")
        self.assertEqual(s.handle, 0)


# ---------------------------------------------------------------------------
# Manual lifecycle
# ---------------------------------------------------------------------------
class TestSolverManualLifecycle(EngineSolverCase):
    """Explicit open → initialize → start → step → end → close → destroy."""

    def test_open_sets_handle(self):
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        s.open()
        self.assertNotEqual(s.handle, 0)
        s.close()
        s.destroy()

    def test_full_lifecycle(self):
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()

        stepped = False
        for _ in s.steps():
            stepped = True
        self.assertTrue(stepped, "Simulation should advance at least one timestep")

        s.end()
        s.report()
        s.close()
        s.destroy()
        self.assertTrue(os.path.exists(rpt))
        self.assertTrue(os.path.exists(out))

    def test_step_signals_completion_via_state(self):
        running_solver = self.running_solver()
        # v1: step() returns timedelta, simulation completion is reported via
        # both the state property transitioning to ENDED and the
        # steps() iterator terminating.
        for _ in running_solver.steps():
            pass
        self.assertEqual(running_solver.state, EngineState.ENDED)


# ---------------------------------------------------------------------------
# Context manager
# ---------------------------------------------------------------------------
class TestSolverContextManager(EngineSolverCase):
    """with Solver(...) as s: opens, initializes, starts on enter."""

    def test_context_manager_runs(self):
        inp, rpt, out = self.solver_files()
        with Solver(inp, rpt, out) as s:
            count = 0
            for _ in s.steps():
                count += 1
            self.assertGreater(count, 0)

    def test_context_manager_cleanup(self):
        inp, rpt, out = self.solver_files()
        with Solver(inp, rpt, out) as s:
            for _ in s.steps():
                pass

        # After exiting, handle should be NULL (destroyed)
        self.assertEqual(s.handle, 0)


# ---------------------------------------------------------------------------
# Properties
# ---------------------------------------------------------------------------
class TestSolverProperties(EngineSolverCase):
    """Test solver properties: elapsed, state, handle."""

    def test_elapsed_after_step(self):
        running_solver = self.running_solver()
        running_solver.step()
        # v1: elapsed is a datetime.timedelta, not a float.
        self.assertGreater(running_solver.elapsed, timedelta(0))

    def test_elapsed_zero_before_step(self):
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()
        self.assertEqual(s.elapsed, timedelta(0))
        s.end()
        s.close()
        s.destroy()

    def test_handle_is_integer(self):
        opened_solver = self.opened_solver()
        self.assertIsInstance(opened_solver.handle, int)
        self.assertGreater(opened_solver.handle, 0)


# ---------------------------------------------------------------------------
# Timing methods
# ---------------------------------------------------------------------------
class TestSolverTiming(EngineSolverCase):
    """Simulation time query methods."""

    def test_start_time(self):
        running_solver = self.running_solver()
        # v1: simulation start is exposed as a datetime property, not a
        # float Julian day returned from get_start_time().
        t = running_solver.sim_start_time
        self.assertIsInstance(t, datetime)

    def test_end_time_after_start(self):
        running_solver = self.running_solver()
        self.assertGreater(running_solver.sim_end_time, running_solver.sim_start_time)

    def test_current_time_advances(self):
        running_solver = self.running_solver()
        t0 = running_solver.current_datetime
        running_solver.step()
        t1 = running_solver.current_datetime
        self.assertGreater(t1, t0)

    def test_routing_step_positive(self):
        running_solver = self.running_solver()
        # v1: routing_step is a timedelta property.
        dt = running_solver.routing_step
        self.assertGreater(dt, timedelta(0))


# ---------------------------------------------------------------------------
# Model write
# ---------------------------------------------------------------------------
class TestSolverModelWrite(EngineSolverCase):
    """Writing the model back to disk."""

    def test_model_write(self):
        opened_solver = self.opened_solver()
        out_path = os.path.join(artifact_dir(self), "written.inp")
        opened_solver.write(out_path)
        self.assertTrue(os.path.exists(out_path))
        self.assertGreater(os.path.getsize(out_path), 0)


# ---------------------------------------------------------------------------
# Stride
# ---------------------------------------------------------------------------
class TestSolverStride(EngineSolverCase):
    """Test stride() multi-step advancement."""

    def test_stride_advances(self):
        running_solver = self.running_solver()
        # v1: stride() returns the elapsed advancement as a timedelta; the
        # .elapsed property reflects the same advancement as a side effect.
        self.assertGreater(running_solver.stride(5), timedelta(0))
        self.assertGreater(running_solver.elapsed, timedelta(0))

    def test_stride_single(self):
        running_solver = self.running_solver()
        self.assertGreater(running_solver.stride(1), timedelta(0))
        self.assertGreater(running_solver.elapsed, timedelta(0))


# ---------------------------------------------------------------------------
# Open with plugin_lib
# ---------------------------------------------------------------------------
class TestSolverOpenPluginLib(EngineSolverCase):
    """Test open() with optional plugin_lib parameter."""

    def test_open_with_none_plugin(self):
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        s.open(plugin_lib=None)
        self.assertNotEqual(s.handle, 0)
        s.close()
        s.destroy()


# ---------------------------------------------------------------------------
# Step callbacks
# ---------------------------------------------------------------------------
class TestSolverStepCallbacks(EngineSolverCase):
    """Test step begin/end callback registration."""

    def test_set_step_begin_callback(self):
        running_solver = self.running_solver()
        called = []
        running_solver.set_step_begin_callback(lambda t, dt: called.append("begin"))
        running_solver.step()
        self.assertGreater(len(called), 0)

    def test_set_step_end_callback(self):
        running_solver = self.running_solver()
        called = []
        running_solver.set_step_end_callback(lambda t, dt: called.append("end"))
        running_solver.step()
        self.assertGreater(len(called), 0)

    def test_callback_none_clears(self):
        running_solver = self.running_solver()
        running_solver.set_step_begin_callback(lambda t, dt: None)
        running_solver.set_step_begin_callback(None)
        running_solver.step()  # Should not raise


# ---------------------------------------------------------------------------
# Module-level run functions
# ---------------------------------------------------------------------------
class TestModuleLevelRun(EngineSolverCase):
    """Test the module-level run() and run_with_callback() functions."""

    def test_run(self):
        from openswmm.engine import run
        inp, rpt, out = self.solver_files()
        run(inp, rpt, out)
        self.assertTrue(os.path.exists(rpt))

    def test_run_with_callback(self):
        from openswmm.engine import run_with_callback
        inp, rpt, out = self.solver_files()
        progress = []
        run_with_callback(inp, rpt, out, lambda p: progress.append(p))
        self.assertGreater(len(progress), 0)


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------
class TestSolverErrors(EngineSolverCase):
    """Error paths and invalid usage."""

    def test_open_nonexistent_raises(self):
        d = artifact_dir(self)
        # v1: open() returns None and raises EngineError on failure (e.g. a
        # missing input file) rather than returning a non-zero rc.
        s = Solver(NON_EXISTENT_INP, os.path.join(d, "x.rpt"), os.path.join(d, "x.out"))
        with self.assertRaises(EngineError):
            s.open()

    def test_double_destroy_safe(self):
        inp, rpt, out = self.solver_files()
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
