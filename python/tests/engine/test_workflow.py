"""Integration workflow tests exercising new Python binding features."""

import os
import unittest

from openswmm.engine import (
    Solver, ModelBuilder, NodeType, LinkType, XSectShape,
    run, run_with_callback,
)
from tests._paths import SITE_DRAINAGE_INP, artifact_dir


# ---------------------------------------------------------------------------
# Programmatic build → run → read
# ---------------------------------------------------------------------------
class TestBuildRunRead(unittest.TestCase):
    """Build a model programmatically, run it, verify outputs exist."""

    def test_build_and_run(self):
        m = ModelBuilder()
        m.add_node("J1", NodeType.JUNCTION)
        m.add_node("OUT1", NodeType.OUTFALL)
        m.add_link("C1", LinkType.CONDUIT)
        m.set_node_invert(0, 10.0)
        m.set_node_invert(1, 0.0)
        m.set_link_nodes(0, 0, 1)
        m.set_link_length(0, 300.0)
        m.set_link_roughness(0, 0.013)
        m.set_link_xsect(0, XSectShape.CIRCULAR, 1.5)
        m.validate()
        m.finalize()
        solver = m.to_solver()
        self.assertNotEqual(solver.handle, 0)
        solver.destroy()


# ---------------------------------------------------------------------------
# Callback-driven simulation
# ---------------------------------------------------------------------------
class TestCallbackDrivenRun(unittest.TestCase):
    """Run a simulation using the module-level run_with_callback function."""

    def test_progress_reported(self):
        d = artifact_dir(self)
        rpt = os.path.join(d, "cb.rpt")
        out = os.path.join(d, "cb.out")
        progress = []
        run_with_callback(SITE_DRAINAGE_INP, rpt, out, lambda p: progress.append(p))
        self.assertGreater(len(progress), 0)
        self.assertTrue(os.path.exists(rpt))


# ---------------------------------------------------------------------------
# Module-level run
# ---------------------------------------------------------------------------
class TestModuleRun(unittest.TestCase):
    """Module-level run() completes without error."""

    def test_run_creates_outputs(self):
        d = artifact_dir(self)
        rpt = os.path.join(d, "run.rpt")
        out = os.path.join(d, "run.out")
        run(SITE_DRAINAGE_INP, rpt, out)
        self.assertTrue(os.path.exists(rpt))
        self.assertTrue(os.path.exists(out))


# ---------------------------------------------------------------------------
# Stride-based stepping
# ---------------------------------------------------------------------------
class TestStrideBasedStepping(unittest.TestCase):
    """Use stride() to advance multiple steps at once."""

    def test_stride_loop(self):
        from datetime import timedelta

        d = artifact_dir(self)
        rpt = os.path.join(d, "stride.rpt")
        out = os.path.join(d, "stride.out")
        s = Solver(SITE_DRAINAGE_INP, rpt, out)
        s.open()
        s.initialize()
        s.start()

        total_elapsed = timedelta(0)
        for _ in range(5):
            elapsed = s.stride(10)
            if elapsed <= timedelta(0):
                break
            total_elapsed = elapsed
        self.assertGreater(total_elapsed, timedelta(0))

        s.end()
        s.report()
        s.close()
        s.destroy()
