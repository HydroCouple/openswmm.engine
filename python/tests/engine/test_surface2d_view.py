"""``Solver.surface2d`` — the lazy 2D overland-flow view.

Exercises the new property against the real engine using the complete 2D
example model (``examples/2d_complete_example.inp``: 9 vertices, 8
triangles, every BC type, one ``[2D_EDGE_CONVEYANCE]`` berm row), per
``docs/API_GAP_CLOSURE_PLAN_2026-06-10.md`` Phase A.

No mocks; skips cleanly when the build lacks 2D support.
"""

from __future__ import annotations

import os

import pytest

pytest.importorskip("openswmm.engine._2d")

from openswmm.engine import Solver, Surface2D

_TESTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_REPO_ROOT = os.path.dirname(os.path.dirname(_TESTS_DIR))
TWOD_EXAMPLE_INP = os.path.join(_REPO_ROOT, "examples", "2d_complete_example.inp")
SITE_DRAINAGE_INP = os.path.join(
    _TESTS_DIR, "data", "solver", "site_drainage_example.inp"
)


@pytest.fixture
def twod_solver(tmp_path):
    s = Solver(
        TWOD_EXAMPLE_INP,
        str(tmp_path / "twod.rpt"),
        str(tmp_path / "twod.out"),
    )
    s.open()
    # The 2D mesh is built during initialize(), so the surface only becomes
    # active after this call — open() alone leaves it inactive.
    s.initialize()
    yield s
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


class TestSurface2dProperty:
    def test_returns_active_surface_for_2d_model(self, twod_solver):
        surface = twod_solver.surface2d
        assert isinstance(surface, Surface2D)
        assert surface.is_active
        assert surface.n_vertices == 9
        assert surface.n_triangles == 8

    def test_view_is_cached(self, twod_solver):
        assert twod_solver.surface2d is twod_solver.surface2d

    def test_inactive_for_1d_model(self, tmp_path):
        s = Solver(
            SITE_DRAINAGE_INP,
            str(tmp_path / "oned.rpt"),
            str(tmp_path / "oned.out"),
        )
        s.open()
        try:
            assert s.surface2d.is_active is False
        finally:
            s.close()
            s.destroy()


class TestVertexCouplingParams:
    """CD / AREA columns of ``[2D_VERTEX_NODE_MAP]``.

    The example model couples vertex 4 to node ``J1`` with ``CD 0.65`` and
    ``AREA 2.0``; every other vertex carries the engine defaults
    (CD 0.65, AREA 1.0 m²) per ``openswmm_2d.h``.
    """

    def test_inp_values_loaded(self, twod_solver):
        surface = twod_solver.surface2d
        assert surface.get_vertex_coupling_cd(4) == pytest.approx(0.65)
        assert surface.get_vertex_coupling_area(4) == pytest.approx(2.0)

    def test_defaults_for_uncoupled_vertex(self, twod_solver):
        surface = twod_solver.surface2d
        assert surface.get_vertex_coupling_cd(0) == pytest.approx(0.65)
        assert surface.get_vertex_coupling_area(0) == pytest.approx(1.0)

    def test_set_round_trip(self, twod_solver):
        surface = twod_solver.surface2d
        surface.set_vertex_coupling_cd(4, 0.8)
        surface.set_vertex_coupling_area(4, 3.5)
        assert surface.get_vertex_coupling_cd(4) == pytest.approx(0.8)
        assert surface.get_vertex_coupling_area(4) == pytest.approx(3.5)

    def test_non_positive_values_rejected(self, twod_solver):
        surface = twod_solver.surface2d
        with pytest.raises(RuntimeError):
            surface.set_vertex_coupling_cd(4, 0.0)
        with pytest.raises(RuntimeError):
            surface.set_vertex_coupling_area(4, -1.0)

    def test_bad_vertex_index_rejected(self, twod_solver):
        surface = twod_solver.surface2d
        with pytest.raises(RuntimeError):
            surface.get_vertex_coupling_cd(surface.n_vertices)
