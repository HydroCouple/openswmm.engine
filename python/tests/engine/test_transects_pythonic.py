"""Transects collection coverage (gap-review 2026-07-06).

Exercises ``solver.infrastructure.transects`` — thin method-level test
coverage identified in the gap review. Uses the ``b8_aq_probe`` model,
which carries a ``[TRANSECTS]`` section. Geometry-validated setters are
round-tripped read-then-write to stay independent of the model's exact
station ranges.
"""
from __future__ import annotations

import os

import pytest

pytest.importorskip("openswmm.engine._infrastructure")

from openswmm.engine import Solver
from openswmm.engine._infrastructure import Transects

_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                      "legacy", "output", "b8_aq_probe.inp")


@pytest.fixture
def tr_solver(tmp_path):
    s = Solver(_MODEL, str(tmp_path / "tr.rpt"), str(tmp_path / "tr.out"))
    try:
        s.open()
    except Exception as exc:  # pragma: no cover - environment guard
        try:
            s.destroy()
        except Exception:
            pass
        pytest.skip(f"transect fixture model not openable by new engine: {exc}")
    yield s
    try:
        s.close()
    except Exception:
        pass
    s.destroy()


@pytest.fixture
def transects(tr_solver):
    t = tr_solver.infrastructure.transects
    if len(t) == 0:
        pytest.skip("model exposes no transects")
    return t


class TestTransectsContainer:
    def test_type(self, transects):
        assert isinstance(transects, Transects)

    def test_len_and_iter(self, transects):
        ids = list(transects)
        assert len(ids) == len(transects)
        assert len(ids) > 0
        assert all(isinstance(i, str) for i in ids)

    def test_id_index_roundtrip(self, transects):
        name = transects.get_id(0)
        assert transects.get_index(name) == 0


class TestTransectGeometry:
    def test_station_count_and_get(self, transects):
        assert transects.station_count(0) > 0
        st = transects.get_station(0, 0)
        assert isinstance(st, tuple) and len(st) == 2

    def test_stations_list_matches_count(self, transects):
        pts = transects.stations(0)
        assert isinstance(pts, list)
        assert len(pts) == transects.station_count(0)

    def test_roughness_is_triple(self, transects):
        r = transects.get_roughness(0)
        assert isinstance(r, tuple) and len(r) == 3

    def test_roughness_roundtrip(self, transects):
        n_left, n_right, n_chan = transects.get_roughness(0)
        transects.set_roughness(0, n_left, n_right, n_chan)
        assert transects.get_roughness(0) == pytest.approx((n_left, n_right, n_chan))

    def test_bank_stations_roundtrip(self, transects):
        left, right = transects.get_bank_stations(0)
        transects.set_bank_stations(0, left, right)
        assert transects.get_bank_stations(0) == pytest.approx((left, right))
