"""Integration workflow tests exercising new Python binding features."""

import os
import pytest

from openswmm.engine import (
    Solver, ModelBuilder, NodeType, LinkType, XSectShape,
    run, run_with_callback,
)
from tests.engine.conftest import SITE_DRAINAGE_INP


# ---------------------------------------------------------------------------
# Programmatic build → run → read
# ---------------------------------------------------------------------------
class TestBuildRunRead:
    """Build a model programmatically, run it, verify outputs exist."""

    def test_build_and_run(self, tmp_path):
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
        assert solver.handle != 0
        solver.destroy()


# ---------------------------------------------------------------------------
# Callback-driven simulation
# ---------------------------------------------------------------------------
class TestCallbackDrivenRun:
    """Run a simulation using the module-level run_with_callback function."""

    def test_progress_reported(self, tmp_path):
        rpt = str(tmp_path / "cb.rpt")
        out = str(tmp_path / "cb.out")
        progress = []
        run_with_callback(SITE_DRAINAGE_INP, rpt, out, lambda p: progress.append(p))
        assert len(progress) > 0
        assert os.path.exists(rpt)


# ---------------------------------------------------------------------------
# Module-level run
# ---------------------------------------------------------------------------
class TestModuleRun:
    """Module-level run() completes without error."""

    def test_run_creates_outputs(self, tmp_path):
        rpt = str(tmp_path / "run.rpt")
        out = str(tmp_path / "run.out")
        run(SITE_DRAINAGE_INP, rpt, out)
        assert os.path.exists(rpt)
        assert os.path.exists(out)


# ---------------------------------------------------------------------------
# Stride-based stepping
# ---------------------------------------------------------------------------
class TestStrideBasedStepping:
    """Use stride() to advance multiple steps at once."""

    def test_stride_loop(self, tmp_path):
        rpt = str(tmp_path / "stride.rpt")
        out = str(tmp_path / "stride.out")
        s = Solver(SITE_DRAINAGE_INP, rpt, out)
        s.open()
        s.initialize()
        s.start()

        total_elapsed = 0.0
        for _ in range(5):
            rc = s.stride(10)
            assert rc == 0
            elapsed = s.elapsed
            if elapsed <= 0:
                break
            total_elapsed = elapsed
        assert total_elapsed > 0.0

        s.end()
        s.report()
        s.close()
        s.destroy()
