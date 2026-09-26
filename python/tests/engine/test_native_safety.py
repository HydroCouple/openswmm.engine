"""Regression tests for ownership and Python/native call boundaries."""
from concurrent.futures import ThreadPoolExecutor
from datetime import timedelta
import gc
from threading import Event
import weakref

import pytest

from openswmm.engine import Solver, ModelBuilder
from openswmm.engine._2d import Surface2D
from openswmm.engine._exceptions import BadHandleError, LifecycleError, StaleObjectError
from tests.engine._solver_cases import EngineSolverCase


class TestNativeSafety(EngineSolverCase):
    def test_2d_views_are_invalid_after_close(self):
        solver = self.opened_solver()
        surface = solver.surface2d
        infiltration = surface.infiltration
        defaults = infiltration.defaults
        solver.close()
        with pytest.raises(StaleObjectError):
            _ = surface.is_active
        with pytest.raises(StaleObjectError):
            _ = infiltration.infil_step
        with pytest.raises(StaleObjectError):
            _ = len(defaults)

    def test_2d_view_pins_its_owner(self):
        solver = Solver()
        solver.create()
        owner = weakref.ref(solver)
        surface = Surface2D(solver)
        del solver
        gc.collect()
        assert owner() is not None
        assert surface.is_active is False
        owner().destroy()
        with pytest.raises(StaleObjectError):
            _ = surface.is_active

    def test_raw_pointer_requires_live_registered_owner(self):
        with pytest.warns(DeprecationWarning), pytest.raises(BadHandleError):
            Surface2D(123)
        solver = self.opened_solver()
        address = solver.handle
        with pytest.warns(DeprecationWarning):
            surface = Surface2D(address)
        assert surface.is_active is False
        solver.destroy()
        with pytest.warns(DeprecationWarning), pytest.raises(BadHandleError):
            Surface2D(address)

    def test_builder_transfer_initializes_solver_and_invalidates_views(self):
        builder = ModelBuilder()
        view = Surface2D(builder)
        solver = builder.to_solver()
        try:
            assert solver.handle != 0
            assert solver.surface2d.is_active is False
            with pytest.raises(StaleObjectError):
                _ = view.is_active
            with pytest.raises(BadHandleError):
                builder.to_solver()
        finally:
            solver.destroy()

    def test_callback_failure_is_raised_at_each_advance_entrypoint(self):
        solver = self.running_solver()
        error = ValueError("callback failed")
        def fail(*args):
            raise error
        for advance in (solver.step, lambda: solver.stride(1),
                        lambda: next(solver.steps()),
                        lambda: solver.until(timedelta(days=1))):
            solver.set_step_begin_callback(fail)
            with pytest.raises(ValueError) as caught:
                advance()
            assert caught.value is error
        solver.set_step_begin_callback(None)
        assert solver.step() > timedelta(0)

    def test_callback_can_read_but_cannot_destroy_owner(self):
        solver = self.running_solver()
        seen = []
        def callback(*args):
            seen.append(solver.nodes.depths.copy())
            solver.destroy()
        solver.set_step_begin_callback(callback)
        with pytest.raises(LifecycleError):
            solver.step()
        assert len(seen) == 1
        assert solver.handle != 0
        solver.set_step_begin_callback(None)
        solver.step()

    def test_foreign_thread_access_fails_without_deadlock(self):
        solver = self.running_solver()
        entered, release = Event(), Event()
        def hold(*args):
            entered.set()
            assert release.wait(10), "test did not release callback"
        solver.set_step_begin_callback(hold)
        with ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(solver.step)
            try:
                assert entered.wait(10), "step did not enter callback"
                for access in (solver.destroy, lambda: solver.nodes.depths,
                               lambda: solver.state):
                    with pytest.raises(LifecycleError):
                        access()
            finally:
                release.set()
            future.result(timeout=10)
        solver.set_step_begin_callback(None)
        solver.step()

    def test_iterator_rechecks_handle_after_yield(self):
        solver = self.running_solver()
        steps = solver.steps()
        next(steps)
        solver.destroy()
        with pytest.raises(BadHandleError):
            next(steps)


def test_destroyed_view_regression_in_subprocess():
    # A regression must fail this subprocess rather than taking down pytest.
    import subprocess
    import sys
    from pathlib import Path
    import openswmm
    stage = str(Path(openswmm.__file__).parents[1])
    script = f"""
import sys
sys.meta_path[:] = [f for f in sys.meta_path if type(f).__module__ != '_openswmm_editable']
sys.path.insert(0, {stage!r})
from openswmm.engine import Solver, ModelEditor, StaleObjectError, BadHandleError
s = Solver()
s.create()
v = s.surface2d
editor = ModelEditor(s)
s.destroy()
for operation, error in [(lambda: v.is_active, StaleObjectError), (lambda: editor.node_count, BadHandleError)]:
    try:
        operation()
    except error:
        pass
    else:
        raise AssertionError('destroyed native handle was accepted')
"""
    result = subprocess.run([sys.executable, '-c', script], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
