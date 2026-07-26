"""Snowpacks collection coverage (gap-review 2026-07-06).

Exercises ``solver.snowpacks`` — previously untouched by the test suite.
Uses ``site_drainage_snow.inp`` which carries a ``[SNOWPACKS]`` section.
"""
from __future__ import annotations

import os
import unittest

try:
    import openswmm.engine._subcatchments  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import Snowpacks, Solver

from tests._paths import artifact_dir

_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                      "data", "solver", "site_drainage_snow.inp")

_SURFACE_KEYS = {"cmin", "cmax", "tbase", "fwfrac", "sd0", "fw0", "last"}


class SnowpacksCase(unittest.TestCase):
    """Base class providing the opened-solver helper (former fixture)."""

    def snow_solver(self):
        """An opened Solver on a model with a [SNOWPACKS] section."""
        d = artifact_dir(self)
        s = Solver(_MODEL, os.path.join(d, "sn.rpt"), os.path.join(d, "sn.out"))
        s.open()
        self.addCleanup(self._close_destroy, s)
        return s

    @staticmethod
    def _close_destroy(s):
        try:
            s.close()
        except Exception:
            pass
        s.destroy()


class TestSnowpacksContainer(SnowpacksCase):
    def test_property_type(self):
        snow_solver = self.snow_solver()
        self.assertIsInstance(snow_solver.snowpacks, Snowpacks)

    def test_len_and_iter(self):
        snow_solver = self.snow_solver()
        ids = list(snow_solver.snowpacks)
        self.assertEqual(len(ids), len(snow_solver.snowpacks))
        self.assertGreater(len(ids), 0)
        self.assertTrue(all(isinstance(i, str) for i in ids))


class TestSnowpackSurfaces(SnowpacksCase):
    def test_surface_dicts(self):
        snow_solver = self.snow_solver()
        for getter in ["get_plowable", "get_impervious", "get_pervious"]:
            with self.subTest(getter=getter):
                sp = next(iter(snow_solver.snowpacks))
                d = getattr(snow_solver.snowpacks, getter)(sp)
                self.assertTrue(_SURFACE_KEYS.issubset(d))

    def test_set_plowable_roundtrip(self):
        snow_solver = self.snow_solver()
        packs = snow_solver.snowpacks
        sp = next(iter(packs))
        packs.set_plowable(sp, cmin=0.1, cmax=0.5, tbase=0.0,
                           fwfrac=0.1, sd0=0.0, fw0=0.0, last=0.0)
        d = packs.get_plowable(sp)
        self.assertAlmostEqual(d["cmin"], 0.1, places=6)
        self.assertAlmostEqual(d["cmax"], 0.5, places=6)
        self.assertAlmostEqual(d["fwfrac"], 0.1, places=6)

    def test_removal_dict_non_empty(self):
        snow_solver = self.snow_solver()
        sp = next(iter(snow_solver.snowpacks))
        d = snow_solver.snowpacks.get_removal(sp)
        self.assertIsInstance(d, dict)
        self.assertTrue(d)

    def test_removal_subcatch_is_str(self):
        snow_solver = self.snow_solver()
        sp = next(iter(snow_solver.snowpacks))
        self.assertIsInstance(snow_solver.snowpacks.get_removal_subcatch(sp), str)
