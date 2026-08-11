"""P8 — Spatial / Quality / Infrastructure Pythonic surface tests.

Editing (delete/convert) and ModelBuilder are deliberately not exercised
here — the existing test_edit.py / test_model_builder.py already cover
them and the API surface from this phase only touches the runtime
sub-views.
"""

from __future__ import annotations

import unittest

import numpy as np

try:
    import openswmm.engine._spatial  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import BuildupFunc, LidType, WashoffFunc  # noqa: E402
from openswmm.engine._infrastructure import (  # noqa: E402
    Inlets, Infrastructure, LIDs, Streets, Transects,
)
from openswmm.engine._quality import Landuse, Landuses, Quality  # noqa: E402
from openswmm.engine._spatial import Spatial  # noqa: E402

from tests.engine._solver_cases import EngineSolverCase  # noqa: E402


# ---------------------------------------------------------------------------
# Spatial
# ---------------------------------------------------------------------------


class TestSpatial(EngineSolverCase):
    def test_view_type(self):
        solver = self.opened_solver()
        self.assertIsInstance(solver.spatial, Spatial)

    def test_node_coord_str_or_int(self):
        solver = self.opened_solver()
        if len(solver.nodes) == 0:
            self.skipTest("no nodes")
        nid = solver.nodes.get_id(0)
        by_str = solver.spatial.node_coord(nid)
        by_int = solver.spatial.node_coord(0)
        self.assertEqual(by_str, by_int)

    def test_node_coords_array(self):
        solver = self.opened_solver()
        arr = solver.spatial.node_coords()
        self.assertIsInstance(arr, np.ndarray)
        self.assertEqual(arr.shape, (len(solver.nodes), 2))

    def test_link_vertices_shape(self):
        solver = self.opened_solver()
        if len(solver.links) == 0:
            self.skipTest("no links")
        verts = solver.spatial.link_vertices(0)
        self.assertEqual(verts.ndim, 2)
        self.assertEqual(verts.shape[1], 2)

    def test_subcatchment_polygon_shape(self):
        solver = self.opened_solver()
        if len(solver.subcatchments) == 0:
            self.skipTest("no subcatchments")
        poly = solver.spatial.subcatchment_polygon(0)
        self.assertEqual(poly.ndim, 2)
        self.assertEqual(poly.shape[1], 2)


# ---------------------------------------------------------------------------
# Quality
# ---------------------------------------------------------------------------


class TestQuality(EngineSolverCase):
    def test_view_type(self):
        solver = self.opened_solver()
        self.assertIsInstance(solver.quality, Quality)
        self.assertIsInstance(solver.quality.landuses, Landuses)

    def test_landuse_add_and_get(self):
        solver = self.opened_solver()
        solver.quality.landuses.add("TEST_LU_PYTHONIC")
        lu = solver.quality.landuses["TEST_LU_PYTHONIC"]
        self.assertIsInstance(lu, Landuse)
        self.assertEqual(lu.id, "TEST_LU_PYTHONIC")

    def test_buildup_round_trip(self):
        solver = self.opened_solver()
        if len(solver.pollutants) == 0:
            self.skipTest("need a pollutant for buildup")
        solver.quality.landuses.add("LU_BU")
        pol = solver.pollutants.get_id(0)
        solver.quality.set_buildup(
            "LU_BU", pol,
            func=BuildupFunc.POW, c1=10.0, c2=0.1, c3=2.0)
        info = solver.quality.get_buildup("LU_BU", pol)
        self.assertEqual(info["func"], BuildupFunc.POW)
        self.assertAlmostEqual(info["c1"], 10.0, places=6)

    def test_washoff_round_trip(self):
        solver = self.opened_solver()
        if len(solver.pollutants) == 0:
            self.skipTest("need a pollutant for washoff")
        solver.quality.landuses.add("LU_WO")
        pol = solver.pollutants.get_id(0)
        solver.quality.set_washoff(
            "LU_WO", pol,
            func=WashoffFunc.EXP, coeff=0.5, expon=2.0)
        info = solver.quality.get_washoff("LU_WO", pol)
        self.assertEqual(info["func"], WashoffFunc.EXP)


# ---------------------------------------------------------------------------
# Infrastructure
# ---------------------------------------------------------------------------


class TestInfrastructure(EngineSolverCase):
    def test_view_types(self):
        solver = self.opened_solver()
        infra = solver.infrastructure
        self.assertIsInstance(infra, Infrastructure)
        self.assertIsInstance(infra.transects, Transects)
        self.assertIsInstance(infra.streets, Streets)
        self.assertIsInstance(infra.inlets, Inlets)
        self.assertIsInstance(infra.lids, LIDs)

    def test_transect_add(self):
        solver = self.opened_solver()
        n0 = len(solver.infrastructure.transects)
        idx = solver.infrastructure.transects.add("T_PY1")
        self.assertEqual(idx, n0)
        solver.infrastructure.transects.set_roughness(idx, 0.05, 0.05, 0.03)
        solver.infrastructure.transects.add_station(idx, 0.0, 100.0)

    def test_lid_add_and_usage(self):
        solver = self.opened_solver()
        if len(solver.subcatchments) == 0:
            self.skipTest("need a subcatchment for LID usage")
        idx = solver.infrastructure.lids.add("BC_PY1", LidType.BIO_CELL)
        solver.infrastructure.lids.set_surface(
            idx, storage=0.0, roughness=0.0, slope=0.5)
        sid = solver.subcatchments.get_id(0)
        solver.infrastructure.lids.usage_add(
            sid, idx, number=1, area=100.0, width=10.0)

    def test_lid_usage_by_id_rejects(self):
        solver = self.opened_solver()
        with self.assertRaises(TypeError):
            solver.infrastructure.lids.usage_add(
                0, "NO_SUCH_LID", number=1, area=10.0, width=1.0)
