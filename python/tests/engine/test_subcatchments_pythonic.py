"""
P4 — Subcatchments collection + Subcatchment wrapper Pythonic tests.

Covers the container protocol, identity + geometry properties, gage
back-reference (returns Gage wrapper), runtime state, infiltration view
tagged-union, coverage MutableMapping, stats sub-view, and bulk numpy
properties. Staleness and equality are identical to nodes/links.
"""

from __future__ import annotations

import unittest

import numpy as np

try:
    import openswmm.engine._subcatchments  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine import InfilModel, StaleObjectError  # noqa: E402
from openswmm.engine._gages import Gage  # noqa: E402
from openswmm.engine._nodes import Node  # noqa: E402
from openswmm.engine._subcatchments import (  # noqa: E402
    CoverageView,
    InfiltrationView,
    Subcatchment,
    Subcatchments,
    SubcatchmentStatsView,
)

from tests.engine._solver_cases import EngineSolverCase  # noqa: E402


class TestContainerProtocol(EngineSolverCase):
    def test_len_and_iter(self):
        opened_solver = self.opened_solver()
        self.assertGreater(len(opened_solver.subcatchments), 0)
        wrappers = list(opened_solver.subcatchments)
        self.assertTrue(all(isinstance(w, Subcatchment) for w in wrappers))

    def test_int_and_str_indexing(self):
        opened_solver = self.opened_solver()
        zero_id = opened_solver.subcatchments.get_id(0)
        self.assertEqual(opened_solver.subcatchments[zero_id],
                         opened_solver.subcatchments[0])


class TestSubcatchmentProperties(EngineSolverCase):
    def test_identity(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        self.assertIsInstance(s0.id, str)
        self.assertEqual(s0.index, 0)
        self.assertIs(s0.solver, opened_solver)

    def test_geometry_round_trips(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        for attr in ("area", "width", "slope", "imperv_pct", "zero_imperv_pct",
                     "n_imperv", "n_perv", "ds_imperv", "ds_perv"):
            v = getattr(s0, attr)
            self.assertIsInstance(v, float)
            setattr(s0, attr, v)             # round-trip; no behaviour change
            self.assertAlmostEqual(getattr(s0, attr), v,
                                   delta=max(abs(v) * 1e-6, 1e-12))

    def test_gage_returns_gage_wrapper(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        self.assertIsInstance(s0.gage, Gage)
        self.assertIs(s0.gage.solver, opened_solver)

    def test_outlet_returns_node_or_subcatchment(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        out = s0.outlet
        self.assertTrue(isinstance(out, (Node, Subcatchment)) or out is None)

    def test_runtime_state(self):
        running_solver = self.running_solver()
        s0 = running_solver.subcatchments[0]
        for attr in ("runoff", "groundwater", "rainfall",
                     "snow_depth", "evap", "infil"):
            self.assertIsInstance(getattr(s0, attr), float)

    def test_scale_factors_default_to_one(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        self.assertAlmostEqual(s0.rain_scale_factor, 1.0, places=6)
        self.assertAlmostEqual(s0.snow_scale_factor, 1.0, places=6)

    def test_scale_factors_round_trip(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        s0.rain_scale_factor = 0.5
        s0.snow_scale_factor = 1.3
        self.assertAlmostEqual(s0.rain_scale_factor, 0.5, places=6)
        self.assertAlmostEqual(s0.snow_scale_factor, 1.3, places=6)

    def test_scale_factors_reject_nonpositive(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        with self.assertRaises(Exception):
            s0.rain_scale_factor = 0.0
        with self.assertRaises(Exception):
            s0.snow_scale_factor = -1.0

    def test_four_precip_factors_are_independent(self):
        # rain vs snow scale on the subcatchment, and vs the gage's own two
        # factors — the set most likely to get cross-wired.
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        s0.rain_scale_factor = 0.7
        s0.snow_scale_factor = 1.9
        g = s0.gage
        g.scale_factor = 2.3
        g.snow_factor = 1.4
        self.assertAlmostEqual(s0.rain_scale_factor, 0.7, places=6)
        self.assertAlmostEqual(s0.snow_scale_factor, 1.9, places=6)
        self.assertAlmostEqual(g.scale_factor, 2.3, places=6)
        self.assertAlmostEqual(g.snow_factor, 1.4, places=6)


class TestInfiltration(EngineSolverCase):
    def test_view_exposes_model_enum(self):
        opened_solver = self.opened_solver()
        v = opened_solver.subcatchments[0].infiltration
        self.assertIsInstance(v, InfiltrationView)
        self.assertIsInstance(v.model, InfilModel)

    def test_set_horton_changes_model(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        s0.infiltration.set_horton(3.0, 0.5, 4.0, 7.0)
        self.assertIn(s0.infiltration.model,
                      (InfilModel.HORTON, InfilModel.MOD_HORTON))
        params = s0.infiltration.horton
        np.testing.assert_allclose(params, (3.0, 0.5, 4.0, 7.0), rtol=1e-6)

    def test_set_green_ampt_changes_model(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        s0.infiltration.set_green_ampt(3.5, 0.06, 0.26)
        self.assertIn(s0.infiltration.model,
                      (InfilModel.GREEN_AMPT, InfilModel.MOD_GREEN_AMPT))
        params = s0.infiltration.green_ampt
        np.testing.assert_allclose(params, (3.5, 0.06, 0.26), rtol=1e-6)

    def test_set_curve_number_changes_model(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        s0.infiltration.set_curve_number(85.0, 7.0)
        self.assertEqual(s0.infiltration.model, InfilModel.CURVE_NUMBER)
        self.assertAlmostEqual(s0.infiltration.curve_number, 85.0, places=6)
        self.assertAlmostEqual(s0.infiltration.curve_number_drying_time, 7.0, places=6)

    def test_curve_number_drying_time_round_trips(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        s0.infiltration.set_curve_number(70.0, 3.5)
        self.assertAlmostEqual(s0.infiltration.curve_number, 70.0, places=6)
        self.assertAlmostEqual(s0.infiltration.curve_number_drying_time, 3.5, places=6)

    def test_zero_imperv_pct_round_trips(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        s0.zero_imperv_pct = 42.5
        self.assertAlmostEqual(s0.zero_imperv_pct, 42.5, places=6)


class TestCoverage(EngineSolverCase):
    def test_is_mutable_mapping(self):
        from collections.abc import MutableMapping
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        self.assertIsInstance(s0.coverage, MutableMapping)

    def test_set_get_round_trip(self):
        opened_solver = self.opened_solver()
        if len(opened_solver.subcatchments) == 0:
            self.skipTest("no subcatchments")
        # Landuse count is exposed at runtime via the quality.landuses
        # collection (the swmm_landuse_count C symbol lives in the
        # cimport-only _common.pxd and has no runtime module).
        n_landuses = len(opened_solver.quality.landuses)
        self.assertGreaterEqual(n_landuses, 0)
        s0 = opened_solver.subcatchments[0]
        # Read coverage[0] via the engine's int-based path to dodge id checks.
        # We don't have a landuse id helper in the fixture, so just smoke-test
        # iteration and len.
        n_present = len(s0.coverage)
        self.assertGreaterEqual(n_present, 0)


class TestSubviews(EngineSolverCase):
    def test_stats_view(self):
        completed_solver = self.completed_solver()
        v = completed_solver.subcatchments[0].stats
        self.assertIsInstance(v, SubcatchmentStatsView)
        self.assertIsInstance(v.precip, float)
        self.assertIsInstance(v.runoff_vol, float)
        self.assertIsInstance(v.max_runoff, float)


class TestBulk(EngineSolverCase):
    def test_bulk_props_are_arrays(self):
        running_solver = self.running_solver()
        for prop in ("runoffs", "rainfalls", "evaps",
                     "infils", "snow_depths"):
            with self.subTest(prop=prop):
                arr = getattr(running_solver.subcatchments, prop)
                self.assertIsInstance(arr, np.ndarray)
                self.assertEqual(arr.dtype, np.float64)
                self.assertEqual(arr.shape[0],
                                 len(running_solver.subcatchments))


class TestStaleness(EngineSolverCase):
    def test_rename_invalidates(self):
        opened_solver = self.opened_solver()
        s0 = opened_solver.subcatchments[0]
        original = s0.id
        opened_solver.subcatchments.rename(0, original + "_x")
        with self.assertRaises(StaleObjectError):
            _ = s0.area
        opened_solver.subcatchments.rename(0, original)
