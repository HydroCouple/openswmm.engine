"""``Solver.surface2d`` — the lazy 2D overland-flow view.

Exercises the new property against the real engine using the complete 2D
example model (``examples/2d_complete_example.inp``: 9 vertices, 8
triangles, every BC type, one ``[2D_EDGE_CONVEYANCE]`` berm row), per
``docs/API_GAP_CLOSURE_PLAN_2026-06-10.md`` Phase A.

No mocks; skips cleanly when the build lacks 2D support.
"""

from __future__ import annotations

import os
import unittest

try:
    import openswmm.engine._2d  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import Solver, Surface2D  # noqa: E402

from tests._paths import artifact_dir  # noqa: E402

_TESTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_REPO_ROOT = os.path.dirname(os.path.dirname(_TESTS_DIR))
TWOD_EXAMPLE_INP = os.path.join(_REPO_ROOT, "examples", "2d_complete_example.inp")
SITE_DRAINAGE_INP = os.path.join(
    _TESTS_DIR, "data", "solver", "site_drainage_example.inp"
)


class _Surface2dCase(unittest.TestCase):
    """Base case providing the ``twod_solver`` helper (formerly a fixture)."""

    def twod_solver(self):
        d = artifact_dir(self)
        s = Solver(
            TWOD_EXAMPLE_INP,
            os.path.join(d, "twod.rpt"),
            os.path.join(d, "twod.out"),
        )
        s.open()
        # The 2D mesh is built during initialize(), so the surface only becomes
        # active after this call — open() alone leaves it inactive.
        s.initialize()
        self.addCleanup(self._close_destroy, s)
        return s

    @staticmethod
    def _close_destroy(s):
        try:
            s.close()
        except Exception:
            pass
        s.destroy()


class TestSurface2dProperty(_Surface2dCase):
    def test_returns_active_surface_for_2d_model(self):
        twod_solver = self.twod_solver()
        surface = twod_solver.surface2d
        self.assertIsInstance(surface, Surface2D)
        self.assertTrue(surface.is_active)
        self.assertEqual(surface.n_vertices, 9)
        self.assertEqual(surface.n_triangles, 8)

    def test_view_is_cached(self):
        twod_solver = self.twod_solver()
        self.assertIs(twod_solver.surface2d, twod_solver.surface2d)

    def test_vertex_render_depths_shape_and_dtype(self):
        import numpy as np

        surface = self.twod_solver().surface2d
        depths = surface.get_vertex_render_depths()
        self.assertEqual(depths.shape, (surface.n_vertices,))
        self.assertEqual(depths.dtype, np.float64)

    def test_render_depths_distinct_from_heads(self):
        # Render depths are eta_v - z_v (0 where no incident cell is wet),
        # so before any routing they should be non-positive everywhere,
        # unlike vertex heads which carry bed elevation on dry cells.
        surface = self.twod_solver().surface2d
        depths = surface.get_vertex_render_depths()
        self.assertTrue((depths <= 1e-9).all())

    def test_inactive_for_1d_model(self):
        d = artifact_dir(self)
        s = Solver(
            SITE_DRAINAGE_INP,
            os.path.join(d, "oned.rpt"),
            os.path.join(d, "oned.out"),
        )
        s.open()
        try:
            self.assertIs(s.surface2d.is_active, False)
        finally:
            s.close()
            s.destroy()


class TestVertexCouplingParams(_Surface2dCase):
    """CD / AREA columns of ``[2D_VERTEX_NODE_MAP]``.

    The example model couples vertex 4 to node ``J1`` with ``CD 0.65`` and
    ``AREA 2.0``; every other vertex carries the engine defaults
    (CD 0.65, AREA 1.0 m²) per ``openswmm_2d.h``.
    """

    def test_inp_values_loaded(self):
        surface = self.twod_solver().surface2d
        self.assertAlmostEqual(surface.get_vertex_coupling_cd(4), 0.65, places=6)
        self.assertAlmostEqual(surface.get_vertex_coupling_area(4), 2.0, places=6)

    def test_defaults_for_uncoupled_vertex(self):
        surface = self.twod_solver().surface2d
        self.assertAlmostEqual(surface.get_vertex_coupling_cd(0), 0.65, places=6)
        self.assertAlmostEqual(surface.get_vertex_coupling_area(0), 1.0, places=6)

    def test_set_round_trip(self):
        surface = self.twod_solver().surface2d
        surface.set_vertex_coupling_cd(4, 0.8)
        surface.set_vertex_coupling_area(4, 3.5)
        self.assertAlmostEqual(surface.get_vertex_coupling_cd(4), 0.8, places=6)
        self.assertAlmostEqual(surface.get_vertex_coupling_area(4), 3.5, places=6)

    def test_non_positive_values_rejected(self):
        surface = self.twod_solver().surface2d
        with self.assertRaises(RuntimeError):
            surface.set_vertex_coupling_cd(4, 0.0)
        with self.assertRaises(RuntimeError):
            surface.set_vertex_coupling_area(4, -1.0)

    def test_bad_vertex_index_rejected(self):
        surface = self.twod_solver().surface2d
        with self.assertRaises(RuntimeError):
            surface.get_vertex_coupling_cd(surface.n_vertices)
