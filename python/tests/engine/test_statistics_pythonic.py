"""P6 — Statistics Pythonic surface tests."""

from __future__ import annotations

import unittest

import numpy as np

try:
    import openswmm.engine._statistics  # noqa: F401
except ImportError as _exc:  # pragma: no cover - environment dependent
    raise unittest.SkipTest(f"requires compiled engine: {_exc}")

from openswmm.engine._statistics import Statistics

from tests.engine._solver_cases import EngineSolverCase


class TestStatisticsBulk(EngineSolverCase):
    def test_view_returned(self):
        solver = self.completed_solver()
        self.assertIsInstance(solver.statistics, Statistics)

    def test_bulk_property(self):
        solver = self.completed_solver()
        for prop, count_attr in [
            ("node_max_depth",        "nodes"),
            ("node_max_overflow",     "nodes"),
            ("node_vol_flooded",      "nodes"),
            ("node_time_flooded",     "nodes"),
            ("link_max_flow",         "links"),
            ("link_max_velocity",     "links"),
            ("link_max_filling",      "links"),
            ("link_vol_flow",         "links"),
            ("link_surcharge_time",   "links"),
            ("subcatchment_runoff_vol", "subcatchments"),
            ("subcatchment_max_runoff", "subcatchments"),
            ("subcatchment_precip", "subcatchments"),
        ]:
            with self.subTest(prop=prop, count_attr=count_attr):
                arr = getattr(solver.statistics, prop)
                self.assertIsInstance(arr, np.ndarray)
                self.assertEqual(arr.dtype, np.float64)
                self.assertEqual(arr.shape[0], len(getattr(solver, count_attr)))


class TestScalarGetters(EngineSolverCase):
    """P2.6 scalar getters must equal the bulk array at the same index."""

    def test_scalar_matches_bulk(self):
        solver = self.completed_solver()
        for scalar, bulk in [
            ("node_max_depth_at", "node_max_depth"),
            ("node_max_overflow_at", "node_max_overflow"),
            ("node_vol_flooded_at", "node_vol_flooded"),
            ("node_time_flooded_at", "node_time_flooded"),
            ("link_max_flow_at", "link_max_flow"),
            ("link_max_velocity_at", "link_max_velocity"),
            ("link_max_filling_at", "link_max_filling"),
            ("link_surcharge_time_at", "link_surcharge_time"),
            ("link_vol_flow_at", "link_vol_flow"),
            ("subcatchment_max_runoff_at", "subcatchment_max_runoff"),
            ("subcatchment_runoff_vol_at", "subcatchment_runoff_vol"),
        ]:
            with self.subTest(scalar=scalar, bulk=bulk):
                stats = solver.statistics
                arr = getattr(stats, bulk)
                if arr.shape[0] == 0:
                    self.skipTest("no elements")
                getter = getattr(stats, scalar)
                for i in range(arr.shape[0]):
                    np.testing.assert_allclose(getter(i), arr[i], rtol=1e-6)


class TestSubcatchmentPrecip(EngineSolverCase):
    """``subcatchment_precip`` is the only statistic with no C ``_bulk``
    companion; it is gathered scalar-wise. Verify it is reachable and that
    cumulative precipitation depths are physically sane (non-negative)."""

    def test_precip_non_negative(self):
        solver = self.completed_solver()
        precip = solver.statistics.subcatchment_precip
        self.assertEqual(precip.shape[0], len(solver.subcatchments))
        self.assertTrue(np.all(precip >= 0.0))
