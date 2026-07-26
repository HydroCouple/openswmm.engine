"""
P4 — Gages collection + Gage wrapper Pythonic surface tests.

Smaller scope than nodes/links: no sub-views, just identity + a few
typed properties + bulk rainfall + the same int|str / staleness /
equality contracts.
"""

from __future__ import annotations

import unittest

import numpy as np

try:
    import openswmm.engine._gages  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import GageDataSource, GageRainType, StaleObjectError  # noqa: E402
from openswmm.engine._gages import Gage, Gages  # noqa: E402, F401

from tests.engine._solver_cases import EngineSolverCase  # noqa: E402


class TestContainerProtocol(EngineSolverCase):
    def test_len_and_iter(self):
        solver = self.opened_solver()
        n = len(solver.gages)
        self.assertGreater(n, 0)
        wrappers = list(solver.gages)
        self.assertTrue(all(isinstance(w, Gage) for w in wrappers))
        self.assertEqual(len(wrappers), n)

    def test_int_and_str_indexing(self):
        solver = self.opened_solver()
        zero_id = solver.gages.get_id(0)
        self.assertEqual(solver.gages[zero_id], solver.gages[0])

    def test_unknown_id_raises_keyerror(self):
        solver = self.opened_solver()
        with self.assertRaises(KeyError):
            _ = solver.gages["NO_SUCH_GAGE_xyz"]


class TestGageProperties(EngineSolverCase):
    def test_identity(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        self.assertIsInstance(g0.id, str)
        self.assertEqual(g0.index, 0)
        self.assertIs(g0.solver, solver)

    def test_typed_enums(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        self.assertIsInstance(g0.rain_type, GageRainType)
        self.assertIsInstance(g0.data_source, GageDataSource)

    def test_rainfall_setter(self):
        solver = self.running_solver()
        g0 = solver.gages[0]
        g0.rainfall = 12.5
        self.assertAlmostEqual(g0.rainfall, 12.5, places=6)

    def test_scale_factor_default_is_one(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        self.assertAlmostEqual(g0.scale_factor, 1.0, places=6)

    def test_scale_factor_round_trip(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        g0.scale_factor = 2.5
        self.assertAlmostEqual(g0.scale_factor, 2.5, places=6)

    def test_scale_factor_rejects_nonpositive(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        with self.assertRaises(Exception):
            g0.scale_factor = 0.0
        with self.assertRaises(Exception):
            g0.scale_factor = -1.0
        # Value must remain at its prior default.
        self.assertAlmostEqual(g0.scale_factor, 1.0, places=6)

    def test_scale_factor_settable_while_running(self):
        solver = self.running_solver()
        g0 = solver.gages[0]
        g0.scale_factor = 3.0
        self.assertAlmostEqual(g0.scale_factor, 3.0, places=6)

    # ---- DA.2 parity — interval / SCF / series / station / units ----

    def test_rain_interval_parsed_value(self):
        # Fixture: "RainGage VOLUME 0:05 1.0 TIMESERIES 2-yr" -> 300 s.
        solver = self.opened_solver()
        g0 = solver.gages[0]
        self.assertAlmostEqual(g0.rain_interval, 300.0, places=6)

    def test_rain_interval_round_trip(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        g0.rain_interval = 900.0
        self.assertAlmostEqual(g0.rain_interval, 900.0, places=6)

    def test_rain_interval_accepts_timedelta(self):
        from datetime import timedelta
        solver = self.opened_solver()
        g0 = solver.gages[0]
        g0.rain_interval = timedelta(minutes=15)
        self.assertAlmostEqual(g0.rain_interval, 900.0, places=6)

    def test_snow_factor_parsed_default(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        self.assertAlmostEqual(g0.snow_factor, 1.0, places=6)

    def test_snow_factor_round_trip(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        g0.snow_factor = 1.4
        self.assertAlmostEqual(g0.snow_factor, 1.4, places=6)

    def test_snow_factor_rejects_nonpositive(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        with self.assertRaises(Exception):
            g0.snow_factor = 0.0
        self.assertAlmostEqual(g0.snow_factor, 1.0, places=6)

    def test_snow_factor_distinct_from_scale_factor(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        g0.snow_factor = 1.7
        g0.scale_factor = 2.3
        self.assertAlmostEqual(g0.snow_factor, 1.7, places=6)
        self.assertAlmostEqual(g0.scale_factor, 2.3, places=6)

    def test_timeseries_parsed_value(self):
        # Fixture source is "TIMESERIES 2-yr".
        solver = self.opened_solver()
        g0 = solver.gages[0]
        self.assertEqual(g0.timeseries, "2-yr")

    def test_rain_units_default_is_inches(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        self.assertEqual(g0.rain_units, 0)

    def test_rain_units_round_trip(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        g0.rain_units = 1  # MM
        self.assertEqual(g0.rain_units, 1)

    def test_station_id_round_trip(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        g0.station_id = "STA_07"
        self.assertEqual(g0.station_id, "STA_07")


class TestBulk(EngineSolverCase):
    def test_rainfalls_array(self):
        solver = self.running_solver()
        arr = solver.gages.rainfalls
        self.assertIsInstance(arr, np.ndarray)
        self.assertEqual(arr.dtype, np.float64)
        self.assertEqual(arr.shape[0], len(solver.gages))

    def test_ids_array(self):
        solver = self.opened_solver()
        ids = solver.gages.ids
        self.assertEqual(ids.dtype, object)
        self.assertEqual(list(ids), [g.id for g in solver.gages])


class TestStaleness(EngineSolverCase):
    def test_rename_invalidates(self):
        solver = self.opened_solver()
        g0 = solver.gages[0]
        original = g0.id
        solver.gages.rename(0, original + "_x")
        with self.assertRaises(StaleObjectError):
            _ = g0.rainfall
        solver.gages.rename(0, original)
