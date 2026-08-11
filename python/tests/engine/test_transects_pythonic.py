"""Transects collection coverage (gap-review 2026-07-06).

Exercises ``solver.infrastructure.transects`` — thin method-level test
coverage identified in the gap review. Uses the ``b8_aq_probe`` model,
which carries a ``[TRANSECTS]`` section. Geometry-validated setters are
round-tripped read-then-write to stay independent of the model's exact
station ranges.
"""
from __future__ import annotations

import os
import unittest

try:
    import openswmm.engine._infrastructure  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import Solver
from openswmm.engine._infrastructure import Transects

from tests._paths import artifact_dir

_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                      "legacy", "output", "b8_aq_probe.inp")


class TransectsCase(unittest.TestCase):
    """Base class providing the solver / transects helpers (former fixtures)."""

    def tr_solver(self):
        d = artifact_dir(self)
        s = Solver(_MODEL, os.path.join(d, "tr.rpt"), os.path.join(d, "tr.out"))
        try:
            s.open()
        except Exception as exc:  # pragma: no cover - environment guard
            try:
                s.destroy()
            except Exception:
                pass
            self.skipTest(f"transect fixture model not openable by new engine: {exc}")
        self.addCleanup(self._close_destroy, s)
        return s

    def transects(self):
        t = self.tr_solver().infrastructure.transects
        if len(t) == 0:
            self.skipTest("model exposes no transects")
        return t

    @staticmethod
    def _close_destroy(s):
        try:
            s.close()
        except Exception:
            pass
        s.destroy()


class TestTransectsContainer(TransectsCase):
    def test_type(self):
        transects = self.transects()
        self.assertIsInstance(transects, Transects)

    def test_len_and_iter(self):
        transects = self.transects()
        ids = list(transects)
        self.assertEqual(len(ids), len(transects))
        self.assertGreater(len(ids), 0)
        self.assertTrue(all(isinstance(i, str) for i in ids))

    def test_id_index_roundtrip(self):
        transects = self.transects()
        name = transects.get_id(0)
        self.assertEqual(transects.get_index(name), 0)


class TestTransectGeometry(TransectsCase):
    def test_station_count_and_get(self):
        transects = self.transects()
        self.assertGreater(transects.station_count(0), 0)
        st = transects.get_station(0, 0)
        self.assertIsInstance(st, tuple)
        self.assertEqual(len(st), 2)

    def test_stations_list_matches_count(self):
        transects = self.transects()
        pts = transects.stations(0)
        self.assertIsInstance(pts, list)
        self.assertEqual(len(pts), transects.station_count(0))

    def test_roughness_is_triple(self):
        transects = self.transects()
        r = transects.get_roughness(0)
        self.assertIsInstance(r, tuple)
        self.assertEqual(len(r), 3)

    def test_roughness_roundtrip(self):
        transects = self.transects()
        n_left, n_right, n_chan = transects.get_roughness(0)
        transects.set_roughness(0, n_left, n_right, n_chan)
        got = transects.get_roughness(0)
        for g, expected in zip(got, (n_left, n_right, n_chan)):
            self.assertAlmostEqual(g, expected, places=6)

    def test_bank_stations_roundtrip(self):
        transects = self.transects()
        left, right = transects.get_bank_stations(0)
        transects.set_bank_stations(0, left, right)
        got = transects.get_bank_stations(0)
        for g, expected in zip(got, (left, right)):
            self.assertAlmostEqual(g, expected, places=6)
