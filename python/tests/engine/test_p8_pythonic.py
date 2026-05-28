"""P8 — Spatial / Quality / Infrastructure Pythonic surface tests.

Editing (delete/convert) and ModelBuilder are deliberately not exercised
here — the existing test_edit.py / test_model_builder.py already cover
them and the API surface from this phase only touches the runtime
sub-views.
"""

from __future__ import annotations

import numpy as np
import pytest

pytest.importorskip("openswmm.engine._spatial")

from openswmm.engine import BuildupFunc, LidType, WashoffFunc
from openswmm.engine._infrastructure import (
    Inlets, Infrastructure, LIDs, Streets, Transects,
)
from openswmm.engine._quality import Landuse, Landuses, Quality
from openswmm.engine._spatial import Spatial


# ---------------------------------------------------------------------------
# Spatial
# ---------------------------------------------------------------------------


class TestSpatial:
    def test_view_type(self, opened_solver):
        assert isinstance(opened_solver.spatial, Spatial)

    def test_node_coord_str_or_int(self, opened_solver):
        if len(opened_solver.nodes) == 0:
            pytest.skip("no nodes")
        nid = opened_solver.nodes.get_id(0)
        by_str = opened_solver.spatial.node_coord(nid)
        by_int = opened_solver.spatial.node_coord(0)
        assert by_str == by_int

    def test_node_coords_array(self, opened_solver):
        arr = opened_solver.spatial.node_coords()
        assert isinstance(arr, np.ndarray)
        assert arr.shape == (len(opened_solver.nodes), 2)

    def test_link_vertices_shape(self, opened_solver):
        if len(opened_solver.links) == 0:
            pytest.skip("no links")
        verts = opened_solver.spatial.link_vertices(0)
        assert verts.ndim == 2
        assert verts.shape[1] == 2

    def test_subcatchment_polygon_shape(self, opened_solver):
        if len(opened_solver.subcatchments) == 0:
            pytest.skip("no subcatchments")
        poly = opened_solver.spatial.subcatchment_polygon(0)
        assert poly.ndim == 2
        assert poly.shape[1] == 2


# ---------------------------------------------------------------------------
# Quality
# ---------------------------------------------------------------------------


class TestQuality:
    def test_view_type(self, opened_solver):
        assert isinstance(opened_solver.quality, Quality)
        assert isinstance(opened_solver.quality.landuses, Landuses)

    def test_landuse_add_and_get(self, opened_solver):
        opened_solver.quality.landuses.add("TEST_LU_PYTHONIC")
        lu = opened_solver.quality.landuses["TEST_LU_PYTHONIC"]
        assert isinstance(lu, Landuse)
        assert lu.id == "TEST_LU_PYTHONIC"

    def test_buildup_round_trip(self, opened_solver):
        if len(opened_solver.pollutants) == 0:
            pytest.skip("need a pollutant for buildup")
        opened_solver.quality.landuses.add("LU_BU")
        pol = opened_solver.pollutants.get_id(0)
        opened_solver.quality.set_buildup(
            "LU_BU", pol,
            func=BuildupFunc.POW, c1=10.0, c2=0.1, c3=2.0)
        info = opened_solver.quality.get_buildup("LU_BU", pol)
        assert info["func"] == BuildupFunc.POW
        assert info["c1"] == pytest.approx(10.0)

    def test_washoff_round_trip(self, opened_solver):
        if len(opened_solver.pollutants) == 0:
            pytest.skip("need a pollutant for washoff")
        opened_solver.quality.landuses.add("LU_WO")
        pol = opened_solver.pollutants.get_id(0)
        opened_solver.quality.set_washoff(
            "LU_WO", pol,
            func=WashoffFunc.EXP, coeff=0.5, expon=2.0)
        info = opened_solver.quality.get_washoff("LU_WO", pol)
        assert info["func"] == WashoffFunc.EXP


# ---------------------------------------------------------------------------
# Infrastructure
# ---------------------------------------------------------------------------


class TestInfrastructure:
    def test_view_types(self, opened_solver):
        infra = opened_solver.infrastructure
        assert isinstance(infra, Infrastructure)
        assert isinstance(infra.transects, Transects)
        assert isinstance(infra.streets, Streets)
        assert isinstance(infra.inlets, Inlets)
        assert isinstance(infra.lids, LIDs)

    def test_transect_add(self, opened_solver):
        n0 = len(opened_solver.infrastructure.transects)
        idx = opened_solver.infrastructure.transects.add("T_PY1")
        assert idx == n0
        opened_solver.infrastructure.transects.set_roughness(idx, 0.05, 0.05, 0.03)
        opened_solver.infrastructure.transects.add_station(idx, 0.0, 100.0)

    def test_lid_add_and_usage(self, opened_solver):
        if len(opened_solver.subcatchments) == 0:
            pytest.skip("need a subcatchment for LID usage")
        idx = opened_solver.infrastructure.lids.add("BC_PY1", LidType.BIO_CELL)
        opened_solver.infrastructure.lids.set_surface(
            idx, storage=0.0, roughness=0.0, slope=0.5)
        sid = opened_solver.subcatchments.get_id(0)
        opened_solver.infrastructure.lids.usage_add(
            sid, idx, number=1, area=100.0, width=10.0)

    def test_lid_usage_by_id_rejects(self, opened_solver):
        with pytest.raises(TypeError):
            opened_solver.infrastructure.lids.usage_add(
                0, "NO_SUCH_LID", number=1, area=10.0, width=1.0)
