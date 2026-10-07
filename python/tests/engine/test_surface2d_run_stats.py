"""``Surface2D.run_stats`` — the backend label, momentum closure, configured
LTS_TIERS and the cumulative marcher counters (``swmm_2d_get_run_stats``)
read during a run of ``examples/2d_complete_example.inp``.
"""

from __future__ import annotations

import os
import unittest

try:  # pragma: no cover - import guard mirrors test_surface2d_view.py
    import openswmm.engine._2d  # noqa: F401
except ImportError as exc:  # pragma: no cover
    raise unittest.SkipTest(f"2D bindings not built: {exc}")

from openswmm.engine import Solver  # noqa: E402

from tests._paths import artifact_dir  # noqa: E402

_TESTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_REPO_ROOT = os.path.dirname(os.path.dirname(_TESTS_DIR))
TWOD_EXAMPLE_INP = os.path.join(_REPO_ROOT, "examples", "2d_complete_example.inp")


class TestSurface2dRunStats(unittest.TestCase):
    def setUp(self):
        # The AUTO backend is size-gated to the CPU marcher on the example's
        # 8 cells; an inherited OPENSWMM_2D_BACKEND would override that.
        self._old_backend = os.environ.get("OPENSWMM_2D_BACKEND")
        os.environ["OPENSWMM_2D_BACKEND"] = "cpu"

    def tearDown(self):
        if self._old_backend is None:
            os.environ.pop("OPENSWMM_2D_BACKEND", None)
        else:
            os.environ["OPENSWMM_2D_BACKEND"] = self._old_backend

    def test_stats_populate_during_the_run(self):
        d = artifact_dir(self)
        s = Solver(
            TWOD_EXAMPLE_INP,
            os.path.join(d, "twod.rpt"),
            os.path.join(d, "twod.out"),
        )
        s.open()
        s.initialize()
        try:
            s.start()
            surface = s.surface2d
            self.assertIsNotNone(surface)

            st0 = surface.run_stats
            self.assertTrue(st0["backend"].startswith("cpu"), st0["backend"])
            self.assertEqual(st0["momentum"], "LOCAL_INERTIAL")
            self.assertGreaterEqual(st0["lts_tiers"], 1)
            self.assertEqual(st0["steps"], 0)

            for _ in range(10):
                # step() returns the elapsed time as a timedelta; zero = done.
                if s.step().total_seconds() <= 0.0:
                    break

            st1 = surface.run_stats
            self.assertGreater(st1["steps"], 0)
            self.assertGreater(st1["face_evals"], 0)
            self.assertGreater(st1["last_step"], 0.0)
            self.assertEqual(len(st1["tier_cells"]), st1["lts_tiers"])
            self.assertGreater(sum(st1["tier_cells"]), 0)
            lo, mean, hi = st1["active_frac"]
            self.assertLessEqual(lo, hi)
            self.assertLessEqual(hi, 1.0 + 1e-12)
            self.assertGreaterEqual(mean, 0.0)
        finally:
            try:
                s.close()
            except Exception:
                pass
            s.destroy()


if __name__ == "__main__":
    unittest.main()
