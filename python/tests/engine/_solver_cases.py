"""unittest base class for openswmm.engine tests.

Replaces the pytest fixtures formerly defined in
``tests/engine/conftest.py``.  Each factory method returns a Solver at a
given lifecycle stage and registers ``addCleanup`` teardowns that mirror
the original fixture finalizers exactly, so individual test methods can
request whichever stage they need (as they previously did via fixture
arguments).
"""

import os
import unittest

from openswmm.engine import Solver

from tests._paths import (  # noqa: F401  (re-exported for test modules)
    NON_EXISTENT_INP,
    SITE_DRAINAGE_INP,
    artifact_dir,
)


class EngineSolverCase(unittest.TestCase):
    """Base TestCase providing Solver lifecycle factories."""

    # -- paths --------------------------------------------------------------
    def solver_files(self):
        """Return (inp, rpt, out) with reviewable per-test output paths."""
        d = artifact_dir(self)
        rpt = os.path.join(d, "site_drainage.rpt")
        out = os.path.join(d, "site_drainage.out")
        return SITE_DRAINAGE_INP, rpt, out

    # -- lifecycle factories --------------------------------------------------
    def opened_solver(self):
        """A Solver that has been opened but not yet started."""
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        s.open()
        self.addCleanup(self._close_destroy, s)
        return s

    def initialized_solver(self):
        """A Solver that has been opened and initialized (ready for start)."""
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        self.addCleanup(self._close_destroy, s)
        return s

    def running_solver(self):
        """A Solver in STARTED/RUNNING state (ready for step())."""
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()
        self.addCleanup(self._end_close_destroy, s)
        return s

    def stepped_solver(self, steps=12):
        """A running solver that has been advanced *steps* timesteps."""
        s = self.running_solver()
        for _ in range(steps):
            s.step()
        return s

    def completed_solver(self):
        """A Solver that has finished the entire simulation (ENDED state)."""
        inp, rpt, out = self.solver_files()
        s = Solver(inp, rpt, out)
        s.open()
        s.initialize()
        s.start()
        for _ in s.steps():
            pass
        s.end()
        self.addCleanup(self._report_close_destroy, s)
        return s

    # -- teardown helpers (mirror the former fixture finalizers) -------------
    @staticmethod
    def _close_destroy(s):
        try:
            s.close()
        except Exception:
            pass
        s.destroy()

    @staticmethod
    def _end_close_destroy(s):
        try:
            s.end()
            s.report()
        except Exception:
            pass
        try:
            s.close()
        except Exception:
            pass
        s.destroy()

    @staticmethod
    def _report_close_destroy(s):
        try:
            s.report()
        except Exception:
            pass
        try:
            s.close()
        except Exception:
            pass
        s.destroy()
